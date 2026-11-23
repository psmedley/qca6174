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
static USHORT g_opt_v, g_opt_q, g_opt_w, g_opt_bad, g_opt_b, g_opt_stop;

/* Boot diagnostics for a machine without a serial port. With /B the driver beeps the number of the step it has
 * reached (n short beeps, then a pause), so a hang can be located by counting the last group. /Sn finishes INIT
 * successfully after step n without doing the rest, to find the step that makes the boot hang.
 *   1 arguments read          4 BAR0 windows mapped            8 INIT COMPLETE entered
 *   2 DMA memory and selectors 5 NDIS MAC registered           9 INIT COMPLETE done
 *   3 PCI scan                6 PROTOCOL.INI read; 7 INIT done */
void qc_step(USHORT n)
{
   USHORT i;

   tcom_always('['); tcom_always('S'); tcom_always((char)('0' + n / 10)); tcom_always((char)('0' + n % 10));
   tcom_always(']');
   if (!g_opt_b)
      return;
   if (n >= 10) {                                    /* sub-steps: one LONG beep, then (n - 10) short ones      */
      qc_speaker(1);
      for (i = 0; i < 6; i++)
         qc_iodelay(60000);
      qc_speaker(0);
      qc_iodelay(60000);
      n = (USHORT)(n - 10);
   }
   for (i = 0; i < n; i++) {
      qc_speaker(1);
      qc_iodelay(50000);
      qc_speaker(0);
      qc_iodelay(50000);
   }
   for (i = 0; i < 6; i++)                          /* pause between groups                                    */
      qc_iodelay(60000);
}

#define STOP_HERE(n)  (g_opt_stop == (n))

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
         else if (c == 'B') g_opt_b = 1;
         else if (c == 'S' && p[2] >= '1' && p[2] <= '9') g_opt_stop = (USHORT)(p[2] - '0');
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
   qc_step(1);
   if (STOP_HERE(1)) {
      rp->rph.Status |= STDON;
      return;
   }

   if (mem_init()) {
      say("QCA6174: could not be loaded: no memory for DMA buffers or selectors.");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   tcom_always('I'); tcom_always('1');
   qc_step(2);
   if (STOP_HERE(2)) {
      rp->rph.Status |= STDON;
      return;
   }
   if (pci_find()) {
      say("QCA6174: could not be loaded: no Qualcomm Atheros QCA6174 (168C:003E) or QCA9377 (168C:0042) found.");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   if (!g_bar0) {
      say("QCA6174: could not be loaded: the adapter's registers are above 4 GB (BAR0).");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   qc_step(3);
   if (STOP_HERE(3)) {
      rp->rph.Status |= STDON;
      return;
   }
   /* BAR0 is mapped at bring-up (task time), not here */
   tcom_always('I'); tcom_always('2');
   qc_step(4);
   if (STOP_HERE(4)) {
      rp->rph.Status |= STDON;
      return;
   }
   nrc = ndis_init();
   qc_step(5);
   if (STOP_HERE(5)) {
      rp->rph.Status |= STDON;
      return;
   }
   ndis_find_lan();
   tcom_always('I'); tcom_always('3');
   qc_step(6);
   if (STOP_HERE(6)) {
      rp->rph.Status |= STDON;
      return;
   }

   if (g_opt_bad)
      say("QCA6174: warning - unknown option ignored (valid: /V /Q /W /B /S1../S9 /AA).");
   if (g_opt_v) {
      say("QCA6174: Qualcomm Atheros QCA6174 802.11ac PCIe WLAN driver for OS/2.");
      say("QCA6174: Version " DRV_VERSION "  Build " DRV_BUILD "  Date " __DATE__ " " __TIME__ ".");
      say(nrc == 0 ? (PSZ)"QCA6174: NDIS MAC registered with the Protocol Manager."
                   : (PSZ)"QCA6174: NDIS MAC not registered (no [QCA6174_NIF] in PROTOCOL.INI?).");
      say("QCA6174: GenMAC device WRND32$. Copy firmware-6.bin and board-2.bin to it at boot.");
   }
   if (g_opt_w)
      say("QCA6174: the boot pauses 30 seconds after all drivers are loaded.");
   qc_step(7);
   rp->rph.Status |= STDON;
}

static void rt_initdone(RPH FAR *rp)
{
   USHORT i;
   qc_step(8);

   if (g_opt_w && !g_opt_q)
      for (i = 0; i < 30 * 20; i++)
         qc_msleep(50);
   qc_step(9);
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
