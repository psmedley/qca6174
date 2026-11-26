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
static USHORT g_opt_v, g_opt_q, g_opt_w, g_opt_bad, g_opt_b, g_opt_stop, g_opt_f, g_opt_early;

/* Boot diagnostics for a machine without a serial port. With /B the driver beeps the number of the step it has
 * reached (n short beeps, then a pause), so a hang can be located by counting the last group. /Sn finishes INIT
 * successfully after step n without doing the rest, to find the step that makes the boot hang.
 *   1 arguments read          4 BAR0 windows mapped            8 INIT COMPLETE entered
 *   2 DMA memory and selectors 5 NDIS MAC registered           9 INIT COMPLETE done
 *   3 PCI scan                6 PROTOCOL.INI read; 7 INIT done
 * With /F: 21-24 DMA chunk mapped and written at INIT, 25-27 firmware files read, 31-36 bring-up progress. */
void qc_step(USHORT n)
{
   USHORT i, longs = 0;

   tcom_always('['); tcom_always('S'); tcom_always((char)('0' + n / 10)); tcom_always((char)('0' + n % 10));
   tcom_always(']');
   if (!g_opt_b)
      return;
   if (n >= 30)      { longs = 3; n = (USHORT)(n - 30); }   /* 30+k: three LONG beeps, then k short ones          */
   else if (n >= 20) { longs = 2; n = (USHORT)(n - 20); }   /* 20+k: two LONG beeps, then k short ones            */
   else if (n >= 10) { longs = 1; n = (USHORT)(n - 10); }   /* 10+k: one LONG beep, then k short ones             */
   for (i = 0; i < longs; i++) {
      qc_speaker(1);
      {
         USHORT j;

         for (j = 0; j < 6; j++)
            qc_iodelay(60000);
      }
      qc_speaker(0);
      qc_iodelay(60000);
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
   if (g_initphase) {                                /* INIT: no process to block; spin on the millisecond counter */
      ULONG  t0 = qc_ms();
      USHORT guard;

      for (guard = 0; guard < 30000 && (long)(qc_ms() - t0) < (long)ms; guard++)
         qc_iodelay(20);
      return;
   }
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
         else if (c == 'F') g_opt_f = (USHORT)((p[2] >= '1' && p[2] <= '3') ? p[2] - '0' : 3);          /* at INITCOMPLETE */
         else if (c == 'G') { g_opt_f = (USHORT)((p[2] >= '1' && p[2] <= '3') ? p[2] - '0' : 3); g_opt_early = 1; }   /* at INIT */
         else if (c == 'A' && (p[2] & 0xDF) == 'A') ;
         else               g_opt_bad = 1;
      }
      p++;
   }
}

/* ---- loading the firmware at boot (CONFIG.SYS /F1../F3) ---------------------------------------------- */
/* Messages after INIT go to the COM1 trace only (SaveMessage belongs to INIT). */
static void bsay(char *s)
{
   while (*s)
      tcom_always(*s++);
   tcom_always(13); tcom_always(10);
}

/* Mode 1: only test that DMA memory can be mapped and written at INIT. Mode 2: also read the firmware and board
 * files. Mode 3: also bring the adapter up (polled) so that its MAC address is known before the protocol binds. */
static void boot_load(USHORT mode)
{
   static char fw[] = "\\IBMCOM\\MACS\\QCA6174\\firmware-6.bin";
   static char bd[] = "\\IBMCOM\\MACS\\QCA6174\\board-2.bin";
   USHORT i;

   if (mem_probe()) {
      bsay("QCA6174: /F: DMA memory could not be mapped at boot; the firmware must be copied to WRND32$.");
      return;
   }
   if (mode < 2)
      return;
   if (g_pci_dev == QCA9377_DEV_ID)
      for (i = 0; i < 4; i++) {
         fw[16 + i] = "9377"[i];
         bd[16 + i] = "9377"[i];
      }
   qc_step(25);
   if (boot_read_file(fw, &g_fwblob)) {
      bsay("QCA6174: /F: cannot read firmware-6.bin from \\IBMCOM\\MACS\\QCA6174\\.");
      return;
   }
   qc_step(26);
   if (boot_read_file(bd, &g_bdblob)) {
      bsay("QCA6174: /F: cannot read board-2.bin from \\IBMCOM\\MACS\\QCA6174\\.");
      return;
   }
   qc_step(27);
   bsay("QCA6174: firmware and board data loaded at boot.");
   if (mode < 3)
      return;
   g_initphase = 1;
   i = (USHORT)ath_up();
   g_initphase = 0;
   bsay(i == 0 ? "QCA6174: adapter started at boot."
              : "QCA6174: adapter could not be started at boot (see STATE= in the status text).");
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
   if (g_opt_f && g_opt_early)                        /* /G: the same steps as /F, but already here, at INIT      */
      boot_load(g_opt_f);

   if (g_opt_bad)
      say("QCA6174: warning - unknown option ignored (valid: /V /Q /W /B /S1../S9 /F1../F3 /G1../G3 /AA).");
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
   if (g_opt_f && !g_opt_early)                       /* all drivers are loaded: the memory manager is up (writing DMA memory at INIT hung) */
      boot_load(g_opt_f);

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
