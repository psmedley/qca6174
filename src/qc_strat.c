/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qc_strat.c - strategy routine, INIT and the clock of the QCA6174 driver.
 *
 * Adapted from rt_strat.c of the OS2World RTL88EU driver (GPL-2.0-only), written from the IBM Physical
 * Device Driver Reference (strategy commands, request packets, INIT, INIT COMPLETE, Save_Message).
 *
 *   INIT          - DevHelp entry, switches, resident end, DMA memory and selectors, find the PCI function,
 *                   register the NDIS MAC with the Protocol Manager.
 *   OPEN / CLOSE  - start / finish a firmware upload or a debug session (qc_dbg.c).
 *   WRITE         - firmware upload ("copy /b firmware-6.bin \DEV\WRND32$") or a debug command line.
 *   READ          - the driver status as KEY=VALUE text ("copy \DEV\WRND32$ con").
 *   GENIOCTL      - the GenMAC OID interface (qc_nic.c); the chip is brought up on the first request.
 */
#include "qca.h"
#include "qc_ndis.h"

#define MSG_REPLACEMENT_STRING  1178
static struct {
   USHORT id;
   USHORT count;
   PSZ    text;
} gInitMsg = { MSG_REPLACEMENT_STRING, 1, 0 };
static USHORT g_opt_v, g_opt_q, g_opt_w, g_opt_bad;

static void say(PSZ s)
{
   PSZ p;

   if (g_opt_q)
      return;
   gInitMsg.text = s;
   rt_save_message((void *)&gInitMsg);
   for (p = s; *p; p++)
      tcom_always(*p);
   tcom_always(13); tcom_always(10);
}

/* ---- clock --------------------------------------------------------------------------------------- */
static ULONG FAR *g_msp;

ULONG qc_ms(void)
{
   PVOID pv;
   union { PVOID p; USHORT w[2]; } u;

   if (!g_msp) {
      static PVOID spv;

      if (rt_dh_getdosvar(DOSVAR_SYSINFOSEG, 0, (PPVOID)&spv))
         return 0;
      pv = spv;
      u.p = pv;
      u.w[0] = 4;                                 /* msecs in the global information segment             */
      u.w[1] = *(USHORT FAR *)pv;                 /* GetDOSVar returns a pointer to the selector         */
      g_msp = (ULONG FAR *)u.p;
   }
   return *g_msp;
}

#define EVT_SLEEP 0x51434101UL

void qc_msleep(USHORT ms)
{
   rt_dh_procblock(EVT_SLEEP, (ULONG)ms, WAIT_UNINTERRUPTIBLE);
}

/* ---- switches ------------------------------------------------------------------------------------ */
static void parse_args(PSZ p)
{
   char c;

   if (!p)
      return;
   while (*p) {
      if (*p == '/') {
         c = (char)(p[1] & 0xDF);
         if (c == 'V')      g_opt_v = 1;
         else if (c == 'Q') g_opt_q = 1;
         else if (c == 'W') g_opt_w = 1;
         else if (c == 'A' && (p[2] & 0xDF) == 'A') ;
         else               g_opt_bad = 1;
      }
      p++;
   }
}

/* ---- INIT ---------------------------------------------------------------------------------------- */
static void rt_init(RP_INIT FAR *rp)
{
   USHORT nrc;

   rt_devhelp = rp->p1.DevHlp;
   tcom_always('I'); tcom_always('0');
   parse_args(rp->Args);
   rp->Data1 = 0;
   rp->p1.End.CodeEnd = rt_code_end();
   rp->p1.End.DataEnd = rt_data_end();

   if (mem_init()) {
      say("QCA6174: could not be loaded: no memory for DMA buffers or selectors.");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   tcom_always('I'); tcom_always('1');
   if (pci_find()) {
      say("QCA6174: could not be loaded: no Qualcomm Atheros QCA6174 (168C:003E) found.");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   if (!g_bar0) {
      say("QCA6174: could not be loaded: the adapter's registers are above 4 GB (BAR0).");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   bar_map();                                      /* retried at bring-up if PhysToGDTSelector refuses here */
   tcom_always('I'); tcom_always('2');
   nrc = ndis_init();
   ndis_find_lan();
   tcom_always('I'); tcom_always('3');

   if (g_opt_bad)
      say("QCA6174: warning - unknown option ignored (valid: /V /Q /W /AA).");
   if (g_opt_v) {
      say("QCA6174: Qualcomm Atheros QCA6174 802.11ac PCIe WLAN driver for OS/2.");
      say("QCA6174: Version " DRV_VERSION "  Build " DRV_BUILD "  Date " __DATE__ " " __TIME__ ".");
      say(nrc == 0 ? (PSZ)"QCA6174: NDIS MAC registered with the Protocol Manager."
                   : (PSZ)"QCA6174: NDIS MAC not registered (no [QCA6174_NIF] in PROTOCOL.INI?).");
      say("QCA6174: GenMAC device WRND32$. Copy firmware-6.bin and board-2.bin to it at boot.");
   }
   if (g_opt_w)
      say("QCA6174: the boot pauses 30 seconds after all drivers are loaded.");
   rp->rph.Status |= STDON;
}

static void rt_initdone(RPH FAR *rp)
{
   USHORT i;

   if (g_opt_w && !g_opt_q)
      for (i = 0; i < 30 * 20; i++)
         qc_msleep(50);
   rp->Status |= STDON;
}

/* ---- the strategy routine ------------------------------------------------------------------------- */
void __cdecl rt_strategy(RPH FAR *rp)
{
   rp->Status = 0;

   switch (rp->Cmd) {
   case CMD_INIT:
      rt_init((RP_INIT FAR *)rp);
      break;
   case CMD_INITCOMPLETE:
      rt_initdone(rp);
      break;
   case CMD_OPEN:
      qc_dev_open(((RP_OPENCLOSE FAR *)rp)->sfn);
      rp->Status |= STDON;
      break;
   case CMD_CLOSE:
      qc_dev_close(((RP_OPENCLOSE FAR *)rp)->sfn);
      rp->Status |= STDON;
      break;
   case CMD_WRITE: {
      RP_RW FAR *rw = (RP_RW FAR *)rp;

      qc_dev_write(rw->sfn, rw->Xfer, rw->Count);
      rp->Status |= STDON;
      break;
   }
   case CMD_READ: {
      RP_RW FAR *rw = (RP_RW FAR *)rp;

      rw->Count = qc_dev_read(rw->sfn, rw->Xfer, rw->Count);
      rp->Status |= STDON;
      break;
   }
   case CMD_GENIOCTL:
      qc_genmac_ioctl((RP_GENIOCTL FAR *)rp);
      rp->Status |= STDON;
      break;
   case CMD_SHUTDOWN:
      qc_shutdown();
      rp->Status |= STDON;
      break;
   default:
      rp->Status |= STDON;
      break;
   }
}
