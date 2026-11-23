/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qc_dbg.c - COM1 trace, the device's read/write channel: firmware upload, status text, debug commands.
 *
 * Writing to \DEV\WRND32$:
 *   - a stream that starts with "QCA-ATH10K-BOARD" is board-2.bin, one that starts with "QCA-ATH10K\0" is
 *     firmware-N.bin; it is collected until the writer closes the device;
 *   - anything else is a debug command line: up, scan, r <hex>, d <hex> (results on COM1).
 * Reading \DEV\WRND32$ returns the driver state as KEY=VALUE lines, then end of file.
 */
#include "qca.h"

ULONG g_last_err;
char *g_last_err_txt = "";

/* ---- trace ------------------------------------------------------------------------------------------ */
void tstr(const char *s)
{
   while (*s)
      tcom(*s++);
}

void th8(UCHAR b)
{
   static char hx[] = "0123456789ABCDEF";

   tcom(hx[(b >> 4) & 0x0F]);
   tcom(hx[b & 0x0F]);
}

void th16(USHORT w) { th8((UCHAR)(w >> 8)); th8((UCHAR)w); }

void th32(ULONG v)
{
   U32 u;

   u.l = v;
   th16(u.w[1]);
   th16(u.w[0]);
}

void tnl(void) { tcom(13); tcom(10); }

void qc_err(const char *what, ULONG v)
{
   g_last_err_txt = (char *)what;
   g_last_err = v;
   tstr("[QCA6174: ");
   tstr(what);
   tcom(' ');
   th32(v);
   tcom(']');
   tnl();
}

/* ---- upload / command stream ------------------------------------------------------------------------ */
#define K_NONE  0
#define K_FW    1
#define K_BD    2
#define K_TEXT  3

static USHORT g_wr_sfn = 0xFFFF, g_wr_kind, g_wr_err;
static char   g_cmd[82];
static USHORT g_cmdn;
static USHORT g_rd_sfn = 0xFFFF, g_rd_done;
static ULONG  g_dbg_val, g_dbg_addr;
static USHORT g_dbg_n;                           /* debug reads done                                           */

static ULONG hexval(char *p)
{
   ULONG v = 0;

   while (*p == ' ') p++;
   for (;; p++) {
      char c = *p;
      USHORT d;

      if (c >= '0' && c <= '9') d = (USHORT)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (USHORT)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (USHORT)(c - 'A' + 10);
      else break;
      v = (v << 4) | d;
   }
   return v;
}

static void run_cmd(void)
{
   char *c = g_cmd;

   while (*c == ' ') c++;
   if (c[0] == 'u' && c[1] == 'p') {
      if (!g_hw_ready) { g_hw_failed = 0; ath_up(); }
   } else if (c[0] == 's' && c[1] == 'c') {
      if (g_hw_ready) wmi_scan();
   } else if (c[0] == 'j' && c[1] == 'o') {                  /* "join <ssid>": an UNENCRYPTED network, from the scan table */
      if (g_hw_ready) {
         char *q = c + 2;
         UCHAR n = 0;

         while (*q == ' ') q++;
         while (q[n] && n < 32) n++;
         sta_set_want((UCHAR FAR *)q, n, 1);
         tstr("[join rc "); th16((USHORT)(sta_join(0, 1) & 0xFFFF)); tstr(" fail "); th16(g_jfail); tcom(']'); tnl();
      }
   } else if (c[0] == 'l' && c[1] == 'e') {
      if (g_hw_ready) sta_leave();
   } else if (c[0] == 'r' && c[1] == ' ') {
      if (bar_map() == 0) {
         ULONG a = hexval(c + 2);

         g_dbg_addr = a;
         g_dbg_val = ath_rd(a);
         g_dbg_n++;
         tstr("[r "); th32(a); tcom('='); th32(g_dbg_val); tcom(']'); tnl();
      }
   } else if (c[0] == 'd' && c[1] == ' ') {
      if (g_hw_ready) {
         ULONG a = hexval(c + 2);

         g_dbg_addr = a;
         g_dbg_val = diag_rd32(a);
         g_dbg_n++;
         tstr("[d "); th32(a); tcom('='); th32(g_dbg_val); tcom(' '); th16((USHORT)g_diag_rc); tcom(']'); tnl();
      }
   }
}

void qc_dev_open(USHORT sfn)
{
   (void)sfn;
}

void qc_dev_close(USHORT sfn)
{
   if (sfn == g_rd_sfn)
      g_rd_sfn = 0xFFFF;
   if (sfn != g_wr_sfn)
      return;
   nic_lock();
   if (g_wr_kind == K_FW || g_wr_kind == K_BD) {
      BLOB *b = (g_wr_kind == K_FW) ? &g_fwblob : &g_bdblob;

      b->done = (USHORT)(g_wr_err ? 0 : 1);
      tstr(g_wr_kind == K_FW ? "[fw " : "[board ");
      th32(b->len);
      tstr(g_wr_err ? " FAILED]" : " loaded]");
      tnl();
   } else if (g_wr_kind == K_TEXT && g_cmdn) {
      g_cmd[g_cmdn] = 0;
      run_cmd();
   }
   g_wr_sfn = 0xFFFF;
   g_wr_kind = K_NONE;
   g_cmdn = 0;
   nic_unlock();
}

void qc_dev_write(USHORT sfn, ULONG phys, USHORT n)
{
   static UCHAR head[17];
   USHORT s, i;

   if (!n)
      return;
   nic_lock();
   if (sfn != g_wr_sfn) {                        /* first write of this handle: what is it?                */
      USHORT k = (n < 17) ? n : 17;

      g_wr_sfn = sfn;
      g_wr_err = 0;
      g_cmdn = 0;
      rt_setmem((PVOID)head, 0, sizeof(head));
      s = scr_map(SCR_B, phys, k);
      if (s)
         rt_movmem((PVOID)head, MAKEP(s, 0), k);
      if (k == 17 && head[10] == '-' && head[0] == 'Q' && head[16] == 0)
         g_wr_kind = K_BD;                       /* "QCA-ATH10K-BOARD\0"                                    */
      else if (k >= 11 && head[0] == 'Q' && head[3] == '-' && head[10] == 0)
         g_wr_kind = K_FW;                       /* "QCA-ATH10K\0"                                          */
      else
         g_wr_kind = K_TEXT;
      if (g_wr_kind != K_TEXT && g_hw_ready) {
         tstr("[upload ignored: the adapter is running]");
         g_wr_err = 1;
      } else if (g_wr_kind == K_FW)
         blob_reset(&g_fwblob);
      else if (g_wr_kind == K_BD)
         blob_reset(&g_bdblob);
   }
   if (g_wr_kind == K_FW || g_wr_kind == K_BD) {
      if (!g_wr_err && blob_write(g_wr_kind == K_FW ? &g_fwblob : &g_bdblob, phys, n))
         g_wr_err = 1;
   } else {
      s = scr_map(SCR_B, phys, n);
      for (i = 0; s && i < n; i++) {
         char c = ((char FAR *)MAKEP(s, 0))[i];

         if (c == '\r' || c == '\n') {
            if (g_cmdn) {
               g_cmd[g_cmdn] = 0;
               run_cmd();
               g_cmdn = 0;
               s = scr_map(SCR_B, phys, n);      /* run_cmd may have used the scratch selector             */
            }
         } else if (g_cmdn < sizeof(g_cmd) - 1)
            g_cmd[g_cmdn++] = c;
      }
   }
   nic_unlock();
}

/* ---- status text ----------------------------------------------------------------------------------- */
static char   g_txt[1500];
static USHORT g_tn;

static void ps(const char *s) { while (*s && g_tn < sizeof(g_txt) - 1) g_txt[g_tn++] = *s++; }
static void pfs(char FAR *s, USHORT max) { while (max-- && *s && g_tn < sizeof(g_txt) - 1) g_txt[g_tn++] = *s++; }
static void ph(ULONG v, USHORT digits)
{
   static char hx[] = "0123456789ABCDEF";
   U32 u;
   USHORT i;

   u.l = v;
   for (i = digits; i > 0; i--) {
      USHORT sh = (USHORT)((i - 1) * 4);
      USHORT d  = (USHORT)((sh >= 16 ? (u.w[1] >> (sh - 16)) : (u.w[0] >> sh)) & 15);

      if (g_tn < sizeof(g_txt) - 1) g_txt[g_tn++] = hx[d];
   }
}
static void nl(void) { ps("\r\n"); }

extern USHORT g_wmi_events, g_mgmt_rx, g_htc_drops, g_svc_runs, g_hook_runs;
extern ULONG  g_fw_build, g_eeprom_rd;
extern USHORT g_nbarsel;
extern char  *ath_board_name(void);

static void build_status(void)
{
   USHORT i;

   g_tn = 0;
   ps("DRIVER=" DRV_NAME " " DRV_VERSION " " DRV_BUILD); nl();
   ps("PCI="); ph(g_pci_bdf, 4); ps(" ID="); ph(g_pci_ven, 4); ps(":"); ph(g_pci_dev, 4);
   ps(" SUB="); ph(g_pci_subven, 4); ps(":"); ph(g_pci_subdev, 4);
   ps(" REV="); ph(g_pci_rev, 2); ps(" IRQ="); ph(g_pci_irq, 2); ps(" BAR0="); ph(g_bar0, 8); nl();
   ps("FW="); ph(g_fwblob.len, 8); ps(g_fwblob.done ? " loaded" : " missing");
   ps(" BOARD="); ph(g_bdblob.len, 8); ps(g_bdblob.done ? " loaded" : " missing"); nl();
   ps("FWVER="); pfs((char FAR *)g_fw.version, sizeof(g_fw.version)); ps(" BUILD="); ph(g_fw_build, 8); nl();
   ps("BOARDNAME="); ps(ath_board_name()); nl();
   ps("STATE="); ph(g_hw_state, 4); ps(g_hw_ready ? " READY" : (g_hw_failed ? " FAILED" : " DOWN"));
   ps(" CHIP="); ph(g_chip_id, 8); ps(" TARGET="); ph(g_target_ver, 8); ps(" RD="); ph(g_eeprom_rd, 4); nl();
   ps("MAC=");
   for (i = 0; i < 6; i++) { ph(g_mymac[i], 2); if (i < 5) ps(":"); }
   nl();
   ps("NETS="); ph(g_nnet, 4); ps(" IRQS="); ph(g_irq_count, 4); ps(" SPUR="); ph(g_irq_spurious, 4);
   ps(" HOOK="); ph(g_hook_runs, 4); ps(" SVC="); ph(g_svc_runs, 4); ps(" WMIEV="); ph(g_wmi_events, 4);
   ps(" MGMTRX="); ph(g_mgmt_rx, 4); ps(" HTCDROP="); ph(g_htc_drops, 4); nl();
   ps("LASTERR="); ps(g_last_err_txt); ps(" "); ph(g_last_err, 8); nl();
   ps("DBG=");  ph(g_dbg_n, 4); ps(" ADDR="); ph(g_dbg_addr, 8); ps(" VAL="); ph(g_dbg_val, 8);
   ps(" BARWIN="); ph(g_nbarsel, 2); nl();
   ps("LINK="); ps(g_joined ? "JOINED " : "none "); ph(g_jch, 2); ps(" AID="); ph(g_jaid, 4);
   ps(" JFAIL="); ph(g_jfail, 2); ps(" AUTH="); ph(g_auth_status, 4); ps(" ASSOC="); ph(g_assoc_status, 4);
   ps(g_open_net ? " OPEN" : " RSN"); ps(g_ptk_on ? " PTK" : " nokey"); ps(g_authorized ? " AUTHORIZED" : ""); nl();
   ps("HTT="); ps(g_htt_ready ? "ready" : "down"); ps(" RXIND="); ph(g_htt_rxind, 4); ps(" RXFRM="); ph(g_htt_rxfrm, 4);
   ps(" RXDROP="); ph(g_htt_rxdrop, 4); ps(" TXOK="); ph(g_htt_txok, 4); ps(" TXCOMPL="); ph(g_htt_txcompl, 4);
   ps(" TXFAIL="); ph(g_htt_txfail, 4); ps(" TXFULL="); ph(g_htt_txfull, 4); nl();
   ps("EAPOL="); ph(sta_eapol_count(), 2); ps(" FWREORDER="); ph(g_svc_full_reorder, 1);
   ps(" MGMTWMI="); ph(g_svc_mgmt_tx_wmi, 1); ps(" MGMT="); ps((g_svc_mgmt_tx_wmi || fw_feature(2)) ? "wmi" : "htt");
   ps(" FEAT=");
   for (i = 0; i < 8; i++)
      ph(g_fw.features[i], 2);
   nl();
   for (i = 0; i < g_nnet; i++) {
      ps("NET="); ph(g_net[i].ch, 2); ps(" ");
      ph((ULONG)(UCHAR)g_net[i].rssi, 2); ps(" ");
      pfs((char FAR *)g_net[i].ssid, g_net[i].len); nl();
   }
}

USHORT qc_dev_read(USHORT sfn, ULONG phys, USHORT n)
{
   USHORT s, k;

   if (sfn == g_rd_sfn && g_rd_done)
      return 0;                                   /* second read of the same open: end of file             */
   g_rd_sfn = sfn;
   g_rd_done = 1;
   nic_lock();
   build_status();
   k = (g_tn < n) ? g_tn : n;
   s = scr_map(SCR_B, phys, k);
   if (s)
      rt_movmem(MAKEP(s, 0), (PVOID)g_txt, k);
   nic_unlock();
   return s ? k : 0;
}
