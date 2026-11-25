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
   ath_late_irq();                               /* adapter started at boot (CONFIG.SYS /F3): hook the interrupt now */
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
      /* Both files are in: start the chip now, so the real MAC is known before the protocol binds. */
      if (!g_wr_err && !g_hw_ready && !g_hw_failed && g_fwblob.done && g_bdblob.done)
         ath_up();
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
static char   g_txt[7000];
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

extern USHORT g_t2h_seen[16], g_t2h_other, g_rx_nbuf, g_txl_max;
extern ULONG  g_txl_n, g_txl_h[6], g_txst[4];
extern ULONG g_txl_sum;
extern ULONG  g_ind_frames, g_ind_batches;
extern USHORT g_tr_sc[64], g_tr_id[64], g_tr_ts[64], g_tr_len[64], g_tr_ttl[64], g_tr_n;
extern ULONG  g_rx_retry, g_rx_seqsame, g_rx_seqback, g_rx_retry_same;
extern ULONG  g_rr_pre[3], g_rr_vmcs[10], g_rr_hmcs[16], g_rr_bw[4], g_rr_nss[3], g_rr_sgi, g_rr_n;
extern UCHAR g_rr_rssi;
extern USHORT g_pps_rx_max, g_pps_tx_max, g_lat_max;
extern ULONG  g_lat_n, g_hook_ms;
extern ULONG g_lat_sum;
extern USHORT g_wmi_events, g_mgmt_rx, g_htc_drops;
extern ULONG  g_svc_runs, g_hook_runs;
extern USHORT g_rx_why[8], g_tx_calls, g_tx_why[8], g_tx_last_len, g_tx_last_et;
extern int    g_tx_last_rc;
USHORT ndis_state(USHORT what);
extern UCHAR  g_rx_decap_last, g_rx_fc_last;
extern ULONG  g_rx_attn_last;
extern ULONG  g_oidlog[], g_oidlog_st[];
extern UCHAR  g_oidlog_set[], g_mg_ch[15];
extern USHORT g_oidlog_n, g_ioctl_n, g_bad_n, g_bad_cf;
extern USHORT g_mg_5g, g_mg_drop, g_mg_laststatus, g_scan_visit[3], g_scan_events, g_scan_last_type;
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
   ps("RFCHAINS="); ph(g_num_rf, 2); nl();
   ps("STATE="); ph(g_hw_state, 4); ps(g_hw_ready ? " READY" : (g_hw_failed ? " FAILED" : " DOWN"));
   ps(" CHIP="); ph(g_chip_id, 8); ps(" TARGET="); ph(g_target_ver, 8); ps(" RD="); ph(g_eeprom_rd, 4); nl();
   ps("MAC=");
   for (i = 0; i < 6; i++) { ph(g_mymac[i], 2); if (i < 5) ps(":"); }
   ps(g_cfgmac == 1 ? " (PROTOCOL.INI NETADDRESS)" : (g_cfgmac == 2 ? " (saved LASTMAC.TXT)" : " (firmware; save the status text in LASTMAC.TXT or set NETADDRESS)")); nl();
   ps("FWMAC=");
   for (i = 0; i < 6; i++) { ph(g_wmi_mac[i], 2); if (i < 5) ps(":"); }
   if (g_cfgmac && g_hw_ready && (g_mymac[0] != g_wmi_mac[0] || g_mymac[1] != g_wmi_mac[1] || g_mymac[2] != g_wmi_mac[2] ||
                                  g_mymac[3] != g_wmi_mac[3] || g_mymac[4] != g_wmi_mac[4] || g_mymac[5] != g_wmi_mac[5]))
      ps(" <- differs from the chip's address (FWMAC); the saved one is in use");
   nl();
   ps("NETS="); ph(g_nnet, 4); ps(" IRQS="); ph(g_irq_count, 8); ps(" SPUR="); ph(g_irq_spurious, 4);
   ps(" HOOK="); ph(g_hook_runs, 8); ps(" SVC="); ph(g_svc_runs, 8); ps(" WMIEV="); ph(g_wmi_events, 4);
   ps(" MGMTRX="); ph(g_mgmt_rx, 4); ps(" HTCDROP="); ph(g_htc_drops, 4); nl();
   ps("SCANCH="); ph(g_scan_visit[2], 4); ph(g_scan_visit[1], 4); ph(g_scan_visit[0], 4);
   ps(" SCEV="); ph(g_scan_events, 4); ps(" LASTTYPE="); ph(g_scan_last_type, 4); nl();
   ps("MG2G=");
   for (i = 1; i <= 14; i++)
      ph(g_mg_ch[i], 2);
   ps(" MG5G="); ph(g_mg_5g, 4); ps(" MGDROP="); ph(g_mg_drop, 4); ps(" ST="); ph(g_mg_laststatus, 4); nl();
   ps("IOCTLS="); ph(g_ioctl_n, 4); ps(" OTHER="); ph(g_bad_n, 4); ps(" LASTOTHER(cat/func)="); ph(g_bad_cf, 4);
   ps(" LANNUM="); ph(g_lannum, 2); nl();
   {
      USHORT n = g_oidlog_n, first = (n > 16) ? (USHORT)(n - 16) : 0, k;

      for (k = first; k < n; k++) {                       /* oldest first                                     */
         ps(g_oidlog_set[k % 16] ? "SET " : "GET "); ph(g_oidlog[k % 16], 8);
         ps(" -> "); ph(g_oidlog_st[k % 16], 8); nl();
      }
   }
   ps("LASTERR="); ps(g_last_err_txt); ps(" "); ph(g_last_err, 8); nl();
   ps("DBG=");  ph(g_dbg_n, 4); ps(" ADDR="); ph(g_dbg_addr, 8); ps(" VAL="); ph(g_dbg_val, 8);
   ps(" BARWIN="); ph(g_nbarsel, 2); nl();
   ps("LINK="); ps(g_joined ? "JOINED " : "none "); ph(g_jch, 2); ps(" AID="); ph(g_jaid, 4);
   ps(" JFAIL="); ph(g_jfail, 2); ps(" AUTH="); ph(g_auth_status, 4); ps(" ASSOC="); ph(g_assoc_status, 4);
   ps(g_open_net ? " OPEN" : " RSN"); ps(g_ptk_on ? " PTK" : " nokey"); ps(g_authorized ? " AUTHORIZED" : ""); nl();
   ps("HTT="); ps(g_htt_ready ? "ready" : "down"); ps(" RXBUF="); ph(g_rx_nbuf, 2); ps(" RXIND="); ph(g_htt_rxind, 8); ps(" RXFRM="); ph(g_htt_rxfrm, 8);
   ps(" RXDROP="); ph(g_htt_rxdrop, 8); ps(" TXOK="); ph(g_htt_txok, 8); ps(" TXCOMPL="); ph(g_htt_txcompl, 8);
   ps(" TXFAIL="); ph(g_htt_txfail, 8); ps(" TXFULL="); ph(g_htt_txfull, 8); nl();
   ps("RXWHY=");
   for (i = 0; i < 8; i++) { ph(g_rx_why[i], 4); ps(i < 7 ? "," : ""); }
   ps(" LAST decap="); ph(g_rx_decap_last, 2); ps(" fc0="); ph(g_rx_fc_last, 2); ps(" attn="); ph(g_rx_attn_last, 8); nl();
   ps("NDIS reg="); ph(ndis_state(8), 1); ps(" status="); ph(ndis_state(0), 4); ps(" filter="); ph(ndis_state(1), 4);
   ps(" rxbufs="); ph(ndis_state(6), 2); ps(" txbuf="); ph(ndis_state(7), 1); ps(" tx="); ph(ndis_state(2), 4);
   ps(" txhw="); ph(ndis_state(3), 4); ps(" rx="); ph(ndis_state(4), 4); ps(" nobuf="); ph(ndis_state(5), 4); nl();
   ps("NDISQ hi="); ph(ndis_state(9), 4); ps(" hooks="); ph(ndis_state(10), 4); ps(" dropbusy="); ph(ndis_state(11), 4);
   ps(" dropoff="); ph(ndis_state(12), 4); ps(" frames="); ph(g_ind_frames, 8); ps(" batches="); ph(g_ind_batches, 8); nl();
   ps("RATE rxpps="); ph(g_pps_rx_max, 4); ps(" txpps="); ph(g_pps_tx_max, 4);
   ps(" HOOKMS="); ph(g_hook_ms, 8); ps(" irq2hook max="); ph(g_lat_max, 4); ps("ms n="); ph(g_lat_n, 8); ps(" sumlo="); ph(g_lat_sum, 8); nl();
   ps("T2H="); for (i = 0; i < 16; i++) { ph(g_t2h_seen[i], 4); ps(i < 15 ? "," : ""); } ps(" other="); ph(g_t2h_other, 4); nl();
   ps("RXDUP retry="); ph(g_rx_retry, 8); ps(" seqsame="); ph(g_rx_seqsame, 8); ps(" seqback="); ph(g_rx_seqback, 8);
   ps(" retry&same="); ph(g_rx_retry_same, 8); ps(" DEDUP="); ph(g_dedup, 1); ps(" dropped="); ph(g_rx_dedup, 8); nl();
   ps("RXTRACE (80211seq,ipid,tcpseq16,len,ttl+tos; oldest first)"); nl();
   {
      USHORT k, j;

      for (k = 0; k < 64; k++) {
         j = (USHORT)((g_tr_n + k) & 63);
         if (!g_tr_len[j]) continue;
         ph(g_tr_sc[j], 3); ps(","); ph(g_tr_id[j], 4); ps(","); ph(g_tr_ts[j], 4); ps(","); ph(g_tr_len[j], 3); ps(","); ph(g_tr_ttl[j], 4);
         ps((k & 3) == 3 ? "\r\n" : " ");
      }
      nl();
   }
   ps("RXRATE n="); ph(g_rr_n, 8); ps(" pre(leg,ht,vht)="); for (i = 0; i < 3; i++) { ph(g_rr_pre[i], 8); ps(i < 2 ? "," : ""); }
   ps(" bw(20,40,80,160)="); for (i = 0; i < 4; i++) { ph(g_rr_bw[i], 8); ps(i < 3 ? "," : ""); }
   ps(" nss1,2="); ph(g_rr_nss[1], 8); ps(","); ph(g_rr_nss[2], 8); ps(" sgi="); ph(g_rr_sgi, 8); ps(" rssi="); ph(g_rr_rssi, 2); nl();
   ps("RXVHTMCS0-9="); for (i = 0; i < 10; i++) { ph(g_rr_vmcs[i], 6); ps(i < 9 ? "," : ""); } nl();
   ps("RXHTMCS0-15="); for (i = 0; i < 16; i++) { ph(g_rr_hmcs[i], 6); ps(i < 15 ? "," : ""); } nl();
   ps("TXLAT max="); ph(g_txl_max, 4); ps("ms n="); ph(g_txl_n, 8); ps(" sum="); ph((USHORT)g_txl_sum, 4); ps(" hist(<2,4,8,16,32,>=32)=");
   for (i = 0; i < 6; i++) { ph(g_txl_h[i], 6); ps(i < 5 ? "," : ""); }
   ps(" STAT(ok,disc,noack,postp)="); for (i = 0; i < 4; i++) { ph(g_txst[i], 6); ps(i < 3 ? "," : ""); } nl();
   ps("TXCALLS="); ph(g_tx_calls, 4); ps(" WHY(open,link,buf,big,map,refused,busy,ok)=");
   for (i = 0; i < 8; i++) { ph(g_tx_why[i], 4); ps(i < 7 ? "," : ""); }
   ps(" LAST len="); ph(g_tx_last_len, 4); ps(" type="); ph(g_tx_last_et, 4); ps(" rc="); ph((USHORT)g_tx_last_rc, 4); nl();
   ps("WIDTH="); ps(g_cw == 2 ? "80" : (g_cw == 1 ? "40" : "20")); ps(" MAX="); ph(g_maxw, 1); ps(" SCO="); ph(g_sco, 1);
   ps(" CC0="); ph(g_cc0, 2); ps(" FORCECH="); ph(g_forcech, 2); nl();
   ps("HT="); ps(g_use_ht ? "on" : "off"); ps(" QOS="); ph(g_qos, 2); ps(" APMCS="); ph(g_ht_mcs[0], 2); ph(g_ht_mcs[1], 2);
   ps(" NSS="); ph(g_ht_nss, 2); nl();
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
