/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qc_pci.c - PCI discovery, BAR0 mapping, MMIO access, the interrupt handler and the context hook.
 *
 * PCI configuration space is reached with configuration mechanism #1 (ports 0CF8h/0CFCh, PCI Local Bus
 * Specification 2.x section 3.2.2.3.2). That is a PC hardware interface, not an OS/2 one. The OS/2 way is
 * the OEMHLP$ PCI IOCtl (category 80h, function 0Bh); its packet layout could not be sourced while this was
 * written (EDM2 was unreachable), so it is not used yet - see docs/ARCHITECTURE.md, "Open questions".
 *
 * BAR0 of the QCA6174 is 2 MB of registers (ath10k pci.c maps the whole of BAR 0). It is mapped through 32
 * GDT selectors of 64 KB each (PhysToGDTSelector); every register access is one 32-bit bus cycle (qc_rd32/
 * qc_wr32 in qc_lib.asm).
 */
#include "qca.h"

#define PCI_ADDR_PORT   0x0CF8
#define PCI_DATA_PORT   0x0CFC

#define PCI_CMD_MEM     0x0002
#define PCI_CMD_MASTER  0x0004
#define PCI_CMD_INTXDIS 0x0400

USHORT g_pci_bdf = 0xFFFF;
USHORT g_pci_ven, g_pci_dev, g_pci_subven, g_pci_subdev;
UCHAR  g_pci_rev, g_pci_irq;
ULONG  g_bar0;
ULONG  g_bar_len = 0x00200000UL;          /* QCA6174 BAR0 is 2 MB                                        */

#define BARSEL_MAX 32
USHORT g_barsel[BARSEL_MAX];               /* allocated in mem_init()                                      */
USHORT g_nbarsel;                          /* windows mapped                                               */

USHORT g_irq_hooked, g_irq_count, g_irq_spurious;

/* ---- configuration mechanism #1 -------------------------------------------------------------------- */
static ULONG cfg_addr(USHORT bdf, UCHAR reg)
{
   U32 a;

   a.w[1] = (USHORT)(0x8000 | (bdf >> 8));                 /* enable bit 31, bus number in bits 23..16     */
   a.w[0] = (USHORT)(((bdf & 0xFF) << 8) | (reg & 0xFC));   /* device 15..11, function 10..8, register     */
   return a.l;
}

static ULONG cfg_rd(USHORT bdf, UCHAR reg)
{
   USHORT fl;
   ULONG  v;

   fl = rt_irq_save();
   qc_outpd(PCI_ADDR_PORT, cfg_addr(bdf, reg));
   v = qc_inpd(PCI_DATA_PORT);
   rt_irq_restore(fl);
   return v;
}

static void cfg_wr(USHORT bdf, UCHAR reg, ULONG v)
{
   USHORT fl;

   fl = rt_irq_save();
   qc_outpd(PCI_ADDR_PORT, cfg_addr(bdf, reg));
   qc_outpd(PCI_DATA_PORT, v);
   rt_irq_restore(fl);
}

ULONG pci_cfg_rd32(UCHAR reg)            { return cfg_rd(g_pci_bdf, reg); }
void  pci_cfg_wr32(UCHAR reg, ULONG v)   { cfg_wr(g_pci_bdf, reg, v); }

USHORT pci_cfg_rd16(UCHAR reg)
{
   U32 v;

   v.l = cfg_rd(g_pci_bdf, reg);
   return (reg & 2) ? v.w[1] : v.w[0];
}

void pci_cfg_wr16(UCHAR reg, USHORT val)
{
   U32 v;

   v.l = cfg_rd(g_pci_bdf, reg);
   if (reg & 2) v.w[1] = val; else v.w[0] = val;
   cfg_wr(g_pci_bdf, reg, v.l);
}

/* Find the first 168C:003E (QCA6174) or 168C:0042 (QCA9377) function. Buses 0..255, devices 0..31, functions 0..7 (functions 1..7 only when
 * function 0 is a multi-function device). */
int pci_find(void)
{
   USHORT bus, dev, fn, bdf;
   U32    id, r;

   for (bus = 0; bus < 256; bus++)
      for (dev = 0; dev < 32; dev++)
         for (fn = 0; fn < 8; fn++) {
            bdf = (USHORT)((bus << 8) | (dev << 3) | fn);
            id.l = cfg_rd(bdf, 0x00);
            if (id.w[0] == 0xFFFF) {
               if (fn == 0) break;               /* no device here                                         */
               continue;
            }
            if (id.w[0] == QCA_VENDOR_ID && (id.w[1] == QCA6174_DEV_ID || id.w[1] == QCA9377_DEV_ID)) {
               g_pci_bdf = bdf;
               g_pci_ven = id.w[0];
               g_pci_dev = id.w[1];
               r.l = cfg_rd(bdf, 0x08);
               g_pci_rev = r.b[0];
               r.l = cfg_rd(bdf, 0x2C);
               g_pci_subven = r.w[0];
               g_pci_subdev = r.w[1];
               r.l = cfg_rd(bdf, 0x3C);
               g_pci_irq = r.b[0];
               r.l = cfg_rd(bdf, 0x10);
               g_bar0 = r.l & 0xFFFFFFF0UL;
               if ((r.b[0] & 0x06) == 0x04 && cfg_rd(bdf, 0x14) != 0UL) {
                  qc_err("BAR0 is above 4 GB, not reachable", cfg_rd(bdf, 0x14));
                  g_bar0 = 0;                    /* 64-bit BAR placed above 4 GB by the firmware           */
               }
               return 0;
            }
            if (fn == 0) {
               r.l = cfg_rd(bdf, 0x0C);
               if (!(r.b[2] & 0x80)) break;      /* header type bit 7 clear: single-function device         */
            }
         }
   return -1;
}

/* Memory decode and bus mastering on, INTx allowed; ASPM (PCIe Link Control bits 1..0) off, as ath10k does
 * for the whole time the device is up (hif_power_up clears PCI_EXP_LNKCTL_ASPMC). */
int pci_enable(void)
{
   USHORT cmd, st;
   UCHAR  cap;
   USHORT guard = 0;

   cmd = pci_cfg_rd16(0x04);
   cmd |= PCI_CMD_MEM | PCI_CMD_MASTER;
   cmd &= ~PCI_CMD_INTXDIS;
   pci_cfg_wr16(0x04, cmd);

   st = pci_cfg_rd16(0x06);
   if (!(st & 0x0010))                       /* no capability list                                         */
      return 0;
   cap = (UCHAR)(pci_cfg_rd16(0x34) & 0xFC);
   while (cap && guard++ < 48) {
      U32 c;

      c.l = pci_cfg_rd32(cap);
      if (c.b[0] == 0x10) {                  /* PCI Express capability: Link Control at +10h               */
         USHORT lc = pci_cfg_rd16((UCHAR)(cap + 0x10));

         pci_cfg_wr16((UCHAR)(cap + 0x10), (USHORT)(lc & ~0x0003));
         break;
      }
      cap = (UCHAR)(c.b[1] & 0xFC);
   }
   return 0;
}

/* ---- BAR0 windows ---------------------------------------------------------------------------------- */
int bar_map(void)
{
   USHORT i, n;
   ULONG  ph;

   if (g_nbarsel)
      return 0;
   if (!g_bar0)
      return -1;
   n = (USHORT)(HI16(g_bar_len));            /* number of 64 KB windows                                    */
   if (n > BARSEL_MAX) n = BARSEL_MAX;
   ph = g_bar0;
   for (i = 0; i < n; i++) {
      if (rt_dh_phys_to_gdt(ph, 0xFFFF, g_barsel[i]))
         return -2;
      ph += 0x10000UL;
   }
   g_nbarsel = n;
   return 0;
}

ULONG ath_rd(ULONG off)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return 0xFFFFFFFFUL;
   return qc_rd32(g_barsel[a.w[1]], a.w[0]);
}

void ath_wr(ULONG off, ULONG v)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return;
   qc_wr32(g_barsel[a.w[1]], a.w[0], v);
}

/* ---- legacy interrupt (ath10k pci.c: ath10k_pci_disable_and_clear_intx_irq / enable_intx_irq) ------ */
void intx_disable_clear(void)
{
   ath_wr(SOC_CORE_BASE + PCIE_INTR_ENABLE_ADDRESS, 0UL);
   ath_wr(SOC_CORE_BASE + PCIE_INTR_CLR_ADDRESS, PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL);
   (void)ath_rd(SOC_CORE_BASE + PCIE_INTR_ENABLE_ADDRESS);   /* flush the posted writes                     */
}

void intx_enable(void)
{
   ath_wr(SOC_CORE_BASE + PCIE_INTR_ENABLE_ADDRESS, PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL);
   (void)ath_rd(SOC_CORE_BASE + PCIE_INTR_ENABLE_ADDRESS);
}

/* ---- context hook: task-time servicing armed by the interrupt handler ---------------------------- */
static ULONG  g_hook;
static USHORT g_hook_ok;
USHORT        g_hook_runs, g_hook_armfail, g_lat_max, g_lat_n;
ULONG         g_lat_sum;
static ULONG  g_irq_t;
extern void   ndis_hook_deliver(void);    /* qc_ndis.c: received frames queued for the protocol          */

int hook_init(void)
{
   if (g_hook_ok)
      return 0;
   if (rt_dh_alloc_ctxhook(rt_hook_offset(), &g_hook) != 0)
      return -1;
   g_hook_ok = 1;
   return 0;
}

void hook_arm(void)
{
   if (g_hook_ok && rt_dh_arm_ctxhook(0UL, g_hook) != 0)
      g_hook_armfail++;                      /* already armed: it will run anyway                          */
}

void __cdecl qc_hook_c(void)
{
   USHORT d = (USHORT)(qc_ms() - g_irq_t);          /* interrupt -> task-time servicing, in ms              */

   if (d < 1000) {
      if (d > g_lat_max) g_lat_max = d;
      g_lat_sum += d;
      g_lat_n++;
   }
   g_hook_runs++;
   if (g_hw_ready) {
      ath_service();
      intx_enable();
   }
   ndis_hook_deliver();
}

/* Interrupt time. Returns 1 when the interrupt was ours (EOI issued), 0 otherwise (shared line). */
USHORT __cdecl qc_irq_c(void)
{
   ULONG cause;

   if (!g_nbarsel)
      return 0;
   cause = ath_rd(SOC_CORE_BASE + PCIE_INTR_CAUSE_ADDRESS);
   if (cause == 0xFFFFFFFFUL || !(cause & (PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL))) {
      g_irq_spurious++;
      return 0;
   }
   intx_disable_clear();
   g_irq_count++;
   g_irq_t = qc_ms();
   qc_dh_eoi(g_pci_irq);
   hook_arm();
   return 1;
}

int irq_hook(void)
{
   if (g_irq_hooked)
      return 0;
   if (g_pci_irq == 0 || g_pci_irq == 0xFF)
      return -1;
   if (qc_dh_setirq(qc_irq_offset(), g_pci_irq, 1))
      return -2;
   g_irq_hooked = 1;
   return 0;
}

void irq_unhook(void)
{
   if (g_irq_hooked) {
      qc_dh_unsetirq(g_pci_irq);
      g_irq_hooked = 0;
   }
}
