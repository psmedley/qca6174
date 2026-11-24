/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ath_sta.c - the station: scan table, join (vdev start, peer, authentication, association, peer association,
 * vdev up), leave, keys, the EAPOL queue and the glue to the data path.
 *
 * ath10k leaves the 802.11 state machine to mac80211. This driver has no mac80211, so the open-system
 * authentication and the association request are built here and sent as management frames over WMI; the answers
 * arrive as WMI management-RX events (sta_mgmt_rx). The order of the firmware commands is the one ath10k uses for
 * a station: vdev start on the AP's channel, peer create, [auth, assoc], peer assoc, vdev up; keys with
 * WMI_VDEV_INSTALL_KEY, then the peer is authorized.
 *
 * All of it runs in the GenMAC request path (task time, under nic_lock) and may block in ath_wait().
 */
#include "qca.h"
#include "qc_ndis.h"

net_t  g_net[NET_MAX];
USHORT g_nnet;
USHORT g_joined, g_authorized, g_ptk_on, g_open_net;
UCHAR  g_jbssid[6], g_jssid[32], g_jslen, g_jch;
USHORT g_jfail;
UCHAR  g_jreq[160], g_jres[128];
USHORT g_jreqlen, g_jreslen, g_jcaps, g_jaid;
USHORT g_auth_rx, g_auth_status, g_assoc_rx, g_assoc_status, g_assoc_aid, g_assoc_caps, g_assoc_reslen;
UCHAR  g_assoc_res[192];

static UCHAR  g_want_ssid[33];
static UCHAR  g_want_len;
static USHORT g_want_open;
static UCHAR  g_target[6];
static USHORT g_joining, g_vdev_started, g_peer_made, g_seq, g_pm_miss;
static UCHAR  g_frame[280];
UCHAR  g_use_ht, g_use_vht, g_qos;                                /* HT20 negotiated with this AP; QoS data frames          */
UCHAR  g_forcech;                                      /* PROTOCOL.INI CHANNEL: join only this primary channel (0 = any) */
UCHAR  g_cw, g_sco, g_cc0, g_maxw = 2;                  /* width in use (0 20, 1 40, 2 80 MHz), secondary offset, 80 MHz centre channel index, limit (PROTOCOL.INI MAXWIDTH) */
UCHAR  g_ht_mcs[2], g_ht_nss;                          /* status: AP rx MCS bytes 0/1, streams used             */

/* The element with this id in a block of information elements, or NULL. */
static UCHAR FAR *find_ie(UCHAR FAR *b, USHORT len, UCHAR id)
{
   USHORT pos = 0;

   while ((USHORT)(pos + 2) <= len) {
      if ((USHORT)(pos + 2 + b[pos + 1]) > len)
         break;
      if (b[pos] == id)
         return b + pos;
      pos = (USHORT)(pos + 2 + b[pos + 1]);
   }
   return 0;
}

/* Our HT capabilities element (26 bytes of data): HT20 only, short GI 20, SM power save off, 2 streams of MCS 0-7. */
static const UCHAR g_htcap[28] = { 45, 26, 0x2C, 0x00, 0x03,
                                   0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0 };


void sta_scan_clear(void)
{
   g_nnet = 0;
}

/* ---- scan table ------------------------------------------------------------------------------------- */
static int mac_eq(UCHAR FAR *a, UCHAR FAR *b)
{
   USHORT i;

   for (i = 0; i < 6; i++)
      if (a[i] != b[i])
         return 0;
   return 1;
}

/* Drop entries not heard for 30 s (the table is not cleared at a scan: the joined AP must stay in it). */
void sta_age(void)
{
   USHORT k = 0, i;
   ULONG  now = qc_ms();

   for (i = 0; i < g_nnet; i++) {
      if ((long)(now - g_net[i].seen) > 30000L && !(g_joined && mac_eq(g_net[i].bssid, g_jbssid)))
         continue;
      if (k != i)
         g_net[k] = g_net[i];
      k++;
   }
   g_nnet = k;
}

/* frame: 802.11 management frame (24-byte header), chan: channel number from the event */
void sta_beacon(UCHAR FAR *f, USHORT len, USHORT chan, short rssi)
{
   UCHAR FAR *bssid = f + 16;
   UCHAR FAR *body = f + 24;
   USHORT blen, k, i, pos, slot;
   net_t *n;

   if (len < 24 + 12)
      return;
   blen = (USHORT)(len - 24);
   for (pos = 12; pos + 2 <= blen; pos = (USHORT)(pos + 2 + body[pos + 1])) {
      if ((USHORT)(pos + 2 + body[pos + 1]) > blen)
         break;
      if (body[pos] == 3 && body[pos + 1] == 1)       /* DS parameter set: more reliable than the event channel */
         chan = body[pos + 2];
   }
   slot = 0xFFFF;
   for (k = 0; k < g_nnet; k++)
      if (mac_eq(bssid, g_net[k].bssid)) { slot = k; break; }
   if (slot == 0xFFFF) {
      if (g_nnet < NET_MAX)
         slot = g_nnet++;
      else {
         slot = 0;
         for (k = 1; k < NET_MAX; k++)
            if (g_net[k].rssi < g_net[slot].rssi)
               slot = k;
         if (g_net[slot].rssi > rssi)
            return;
      }
   }
   n = &g_net[slot];
   for (i = 0; i < 6; i++)
      n->bssid[i] = bssid[i];
   n->ch = (UCHAR)chan;
   n->rssi = (signed char)(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi));
   n->sec = (UCHAR)((body[10] & 0x10) ? 1 : 0);
   n->htc = n->ht40 = n->sco = n->htw = n->vht = n->vwid = n->cc0 = n->cc1 = 0;
   for (pos = 12; pos + 2 <= blen; pos = (USHORT)(pos + 2 + body[pos + 1])) {          /* channel width elements       */
      UCHAR FAR *d = body + pos + 2;
      UCHAR l = body[pos + 1];

      if ((USHORT)(pos + 2 + l) > blen)
         break;
      if (body[pos] == 45 && l >= 26) { n->htc = 1; n->ht40 = (UCHAR)((d[0] >> 1) & 1); }
      else if (body[pos] == 61 && l >= 22) { n->sco = (UCHAR)(d[1] & 3); n->htw = (UCHAR)((d[1] >> 2) & 1); }
      else if (body[pos] == 191 && l >= 12) n->vht = 1;
      else if (body[pos] == 192 && l >= 3) { n->vwid = d[0]; n->cc0 = d[1]; n->cc1 = d[2]; }
   }
   n->len = 0;
   for (pos = 12; pos + 2 <= blen; pos = (USHORT)(pos + 2 + body[pos + 1])) {
      if ((USHORT)(pos + 2 + body[pos + 1]) > blen)
         break;
      if (body[pos] == 0 && body[pos + 1] <= 32) {
         n->len = body[pos + 1];
         for (i = 0; i < n->len; i++)
            n->ssid[i] = (char)body[pos + 2 + i];
         break;
      }
   }
   n->ielen = (blen > NET_IE_MAX) ? NET_IE_MAX : blen;
   rt_movmem((PVOID)n->ie, body, n->ielen);
   n->seen = qc_ms();
}

short sta_rssi(void)
{
   USHORT k;

   for (k = 0; k < g_nnet; k++)
      if (mac_eq(g_net[k].bssid, g_jbssid))
         return g_net[k].rssi;
   return -70;
}

/* ---- what to join ---------------------------------------------------------------------------------- */
void sta_set_want(UCHAR FAR *ssid, UCHAR len, USHORT open)
{
   USHORT i;

   if (len > 32)
      len = 32;
   for (i = 0; i < len; i++)
      g_want_ssid[i] = ssid[i];
   g_want_ssid[len] = 0;
   g_want_len = len;
   g_want_open = open;
}

static int ssid_match(net_t *n)
{
   USHORT i;

   if (!g_want_len || n->len != g_want_len)
      return 0;
   for (i = 0; i < g_want_len; i++)
      if ((UCHAR)n->ssid[i] != g_want_ssid[i])
         return 0;
   return 1;
}

/* ---- management frames ------------------------------------------------------------------------------ */
static const UCHAR g_rates24[12] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
static const UCHAR g_rates5[8]   = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
/* RSN element: version 1, group CCMP, 1 pairwise CCMP, 1 AKM PSK, no capabilities (WPA2-PSK / CCMP only) */
static const UCHAR g_rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F,
                                 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };

static void mgmt_hdr(UCHAR fc0)
{
   USHORT i;

   rt_setmem((PVOID)g_frame, 0, 24);
   g_frame[0] = fc0;
   for (i = 0; i < 6; i++) {
      g_frame[4 + i]  = g_target[i];                   /* DA = AP                                           */
      g_frame[10 + i] = g_mymac[i];                    /* SA = us                                           */
      g_frame[16 + i] = g_target[i];                   /* BSSID                                             */
   }
   g_seq++;
   g_frame[22] = (UCHAR)(g_seq << 4);                  /* sequence control: number << 4                     */
   g_frame[23] = (UCHAR)(g_seq >> 4);
}

static int tx_auth(void)
{
   mgmt_hdr(0xB0);
   g_frame[24] = g_frame[25] = 0;                      /* algorithm: open system                            */
   g_frame[26] = 1; g_frame[27] = 0;                   /* transaction sequence 1                            */
   g_frame[28] = g_frame[29] = 0;                      /* status                                            */
   return wmi_mgmt_tx((UCHAR FAR *)g_frame, 30);
}

static int tx_deauth(void)
{
   mgmt_hdr(0xC0);
   g_frame[24] = 3;                                    /* reason 3: leaving                                 */
   g_frame[25] = 0;
   return wmi_mgmt_tx((UCHAR FAR *)g_frame, 26);
}

static int tx_assoc(net_t *n)
{
   USHORT i, k = 24;
   UCHAR  hi = (UCHAR)(n->ch > 14);

   mgmt_hdr(0x00);
   g_frame[k++] = (UCHAR)(g_want_open ? 0x21 : 0x31);  /* capability: ESS, [privacy], short preamble, short slot */
   g_frame[k++] = 0x04;
   g_frame[k++] = 10; g_frame[k++] = 0;                /* listen interval                                   */
   g_frame[k++] = 0;  g_frame[k++] = n->len;           /* SSID                                              */
   for (i = 0; i < n->len; i++)
      g_frame[k++] = (UCHAR)n->ssid[i];
   g_frame[k++] = 1;  g_frame[k++] = (UCHAR)(hi ? 8 : 8);
   for (i = 0; i < 8; i++)
      g_frame[k++] = hi ? g_rates5[i] : g_rates24[i];
   if (!hi) {
      g_frame[k++] = 50; g_frame[k++] = 4;             /* extended supported rates                          */
      for (i = 8; i < 12; i++)
         g_frame[k++] = g_rates24[i];
   }
   if (!g_want_open)
      for (i = 0; i < 22; i++)
         g_frame[k++] = g_rsn[i];
   g_jreqlen = (USHORT)(k - 28);                       /* what the supplicant reads back as request IEs     */
   if (g_jreqlen > sizeof(g_jreq))
      g_jreqlen = sizeof(g_jreq);
   for (i = 0; i < g_jreqlen; i++)
      g_jreq[i] = g_frame[28 + i];
   if (g_use_ht) {                                     /* after the RSN element, as mac80211 orders them     */
      for (i = 0; i < 28; i++)
         g_frame[k++] = g_htcap[i];
      if (g_num_rf < 2)
         g_frame[k - 28 + 6] = 0;                      /* 1x1 radio (QCA9377): no MCS 8-15 (rx mask byte 1)   */
      if (g_cw)
         g_frame[k - 28 + 2] |= 0x42;                  /* supports 40 MHz, short GI 40                        */
      if (g_cw == 2) {                                 /* VHT capabilities: 80 MHz, short GI 80, A-MPDU exponent 3 */
         USHORT mp = (USHORT)(g_num_rf >= 2 ? 0xFFF0U : 0xFFFCU);

         g_frame[k++] = 191; g_frame[k++] = 12;
         g_frame[k++] = 0x20; g_frame[k++] = 0; g_frame[k++] = 0x80; g_frame[k++] = 0x01;
         g_frame[k++] = (UCHAR)mp; g_frame[k++] = (UCHAR)(mp >> 8);       /* rx MCS map: MCS 0-9 per stream  */
         g_frame[k++] = 0; g_frame[k++] = 0;
         g_frame[k++] = (UCHAR)mp; g_frame[k++] = (UCHAR)(mp >> 8);       /* tx MCS map                      */
         g_frame[k++] = 0; g_frame[k++] = 0;
      }
   }
   return wmi_mgmt_tx((UCHAR FAR *)g_frame, k);
}

/* A management frame that is not a beacon or probe response (from the WMI management-RX event). */
void sta_mgmt_rx(UCHAR FAR *f, USHORT len)
{
   UCHAR fc0 = f[0];
   USHORT i, n;

   if (!g_joining && !g_joined)
      return;
   if (!mac_eq(f + 10, (UCHAR FAR *)g_target))          /* sender must be the AP we talk to                  */
      return;
   if (fc0 == 0xB0 && len >= 30) {                       /* authentication                                    */
      if (f[26] == 2 && f[27] == 0) {                    /* transaction sequence 2                            */
         g_auth_status = (USHORT)(f[28] | (f[29] << 8));
         g_auth_rx = 1;
      }
   } else if ((fc0 == 0x10 || fc0 == 0x30) && len >= 30) {   /* association / reassociation response          */
      g_assoc_caps   = (USHORT)(f[24] | (f[25] << 8));
      g_assoc_status = (USHORT)(f[26] | (f[27] << 8));
      g_assoc_aid    = (USHORT)((f[28] | (f[29] << 8)) & 0x3FFF);
      n = (USHORT)(len - 30);
      if (n > sizeof(g_assoc_res))
         n = sizeof(g_assoc_res);
      for (i = 0; i < n; i++)
         g_assoc_res[i] = f[30 + i];
      g_assoc_reslen = n;
      g_assoc_rx = 1;
   } else if (fc0 == 0xC0 || fc0 == 0xA0) {              /* deauthentication / disassociation                 */
      g_link_lost = 1;
   }
}

/* ---- join --------------------------------------------------------------------------------------------- */
static void abort_join(void)
{
   if (g_peer_made) {
      wmi_peer_delete((UCHAR FAR *)g_target);
      g_peer_made = 0;
   }
   if (g_vdev_started) {
      wmi_vdev_stop();
      ath_wait(&g_vdev_stopped, 1000);
      g_vdev_started = 0;
   }
   g_joining = 0;
}

/* The rates of the AP's association response (supported rates, extended rates), at most 12. */
static USHORT ap_rates(UCHAR FAR *out)
{
   USHORT pos = 0, n = 0, i;

   while (pos + 2 <= g_assoc_reslen) {
      UCHAR id = g_assoc_res[pos], l = g_assoc_res[pos + 1];

      if ((USHORT)(pos + 2 + l) > g_assoc_reslen)
         break;
      if (id == 1 || id == 50)
         for (i = 0; i < l && n < 12; i++)
            out[n++] = g_assoc_res[pos + 2 + i];
      pos = (USHORT)(pos + 2 + l);
   }
   return n;
}

int sta_join(UCHAR FAR *want_bssid, USHORT any)
{
   static UCHAR rates[12];
   USHORT k, best = 0xFFFF, i, att, bcn, dtim, nr, pos;
   int    bestr = -1000, rv;
   net_t *n;
   UCHAR FAR *htp, FAR *vhp;

   g_jfail = 0;
   if (!g_hw_ready) { g_jfail = 8; return -1; }
   if (g_joined)
      sta_leave();
   g_link_lost = 0;
   for (k = 0; k < g_nnet; k++) {
      if (!ssid_match(&g_net[k]))
         continue;
      if (!any && !mac_eq(g_net[k].bssid, want_bssid))
         continue;
      if (g_forcech && g_net[k].ch != g_forcech)
         continue;
      rv = g_net[k].rssi ? (int)g_net[k].rssi : -120;
      if (best == 0xFFFF || rv > bestr) {
         best = k;
         bestr = rv;
      }
   }
   if (best == 0xFFFF) {
      g_jfail = 1;                                      /* no network with that SSID in the scan table       */
      return -1;
   }
   n = &g_net[best];
   for (i = 0; i < 6; i++)
      g_target[i] = n->bssid[i];
   g_joining = 1;
   g_auth_status = g_assoc_status = 0xFFFF;

   bcn = (USHORT)(n->ie[8] | (n->ie[9] << 8));
   if (bcn < 10 || bcn > 2000)
      bcn = 100;
   dtim = 1;
   for (pos = 12; pos + 2 <= n->ielen; pos = (USHORT)(pos + 2 + n->ie[pos + 1])) {
      if ((USHORT)(pos + 2 + n->ie[pos + 1]) > n->ielen)
         break;
      if (n->ie[pos] == 5 && n->ie[pos + 1] >= 2 && n->ie[pos + 3] >= 1)
         dtim = n->ie[pos + 3];                          /* TIM element: DTIM period                         */
   }

   {
      UCHAR FAR *h = (n->ielen > 12) ? find_ie((UCHAR FAR *)n->ie + 12, (USHORT)(n->ielen - 12), 45) : 0;

      g_use_ht = (UCHAR)(h && h[1] >= 26);            /* the AP's beacon carries an HT capabilities element */
   }
   g_cw = 0;
   g_sco = n->sco;
   g_cc0 = 0;
   if (g_use_ht && g_maxw >= 1 && n->htw && (n->sco == 1 || n->sco == 3) && (n->ch > 14 || n->ht40)) {
      g_cw = 1;                                        /* the AP runs a 40 MHz channel: so do we              */
      if (n->ch > 14 && g_maxw >= 2 && n->vht && n->vwid >= 1 && n->cc0 >= 36 && n->cc0 <= 165) {
         USHORT pm = (USHORT)(5000 + 5 * n->ch), cm = (USHORT)(5000 + 5 * n->cc0);
         USHORT dd = (pm > cm) ? (USHORT)(pm - cm) : (USHORT)(cm - pm);

         if (dd == 10 || dd == 30) {                   /* the primary channel is inside the 80 MHz block      */
            g_cw = 2;
            g_cc0 = n->cc0;
         }
      }
   }
   g_use_vht = (UCHAR)(g_cw == 2);
   if (wmi_vdev_start(n->ch, bcn, dtim)) { g_jfail = 9; abort_join(); return -1; }
   g_vdev_started = 1;
   if (ath_wait(&g_vdev_start_done, 2000) || g_vdev_start_status != 0) {
      g_jfail = 9;                                       /* the firmware did not start the vdev on that channel */
      qc_err("vdev start failed, status", (ULONG)g_vdev_start_status);
      abort_join();
      return -1;
   }
   if (wmi_peer_create((UCHAR FAR *)g_target)) { g_jfail = 10; abort_join(); return -1; }
   g_peer_made = 1;
   if (ath_wait(&g_peer_map, 1500)) {
      g_pm_miss++;                                       /* no PEER_MAP event seen: go on, the command was sent */
      tstr("[no peer map]");
   }

   for (att = 1; att <= 3; att++) {                       /* open-system authentication                        */
      g_auth_rx = 0;
      if (tx_auth()) { g_jfail = 10; abort_join(); return -1; }
      if (!ath_wait(&g_auth_rx, 400))
         break;
   }
   if (!g_auth_rx)               { g_jfail = 2; abort_join(); return -1; }   /* no reply                            */
   if (g_auth_status != 0)       { g_jfail = 3; abort_join(); return -1; }   /* refused                             */

   for (att = 1; att <= 3; att++) {                       /* association                                       */
      g_assoc_rx = 0;
      if (tx_assoc(n)) { g_jfail = 10; abort_join(); return -1; }
      if (!ath_wait(&g_assoc_rx, 600))
         break;
   }
   if (!g_assoc_rx)              { g_jfail = 4; abort_join(); return -1; }   /* no reply                            */
   if (g_assoc_status != 0)      { g_jfail = 5; abort_join(); return -1; }   /* refused (status in JASSOC)          */

   nr = ap_rates((UCHAR FAR *)rates);
   {
      UCHAR FAR *h = g_use_ht ? find_ie((UCHAR FAR *)g_assoc_res, g_assoc_reslen, 45) : 0;

      if (h && h[1] < 26)
         h = 0;
      if (!h)
         g_use_ht = 0;                                 /* the AP did not confirm HT in its response          */
      g_qos = g_use_ht;
      if (h) { g_ht_mcs[0] = h[2 + 3]; g_ht_mcs[1] = h[2 + 4]; }
      htp = h;
      vhp = (g_cw == 2) ? find_ie((UCHAR FAR *)g_assoc_res, g_assoc_reslen, 191) : 0;
      if (vhp && vhp[1] < 12)
         vhp = 0;
      if (g_cw == 2 && !vhp)
         tstr("[AP gave no VHT caps]");
   }
   if (wmi_peer_assoc((UCHAR FAR *)g_target, g_assoc_aid, g_assoc_caps, 10,
                      g_want_open ? 0UL : 0x4UL,          /* WMI_TLV_PEER_NEED_PTK_4_WAY for an RSN network     */
                      (UCHAR FAR *)rates, nr, htp, vhp,
                      (USHORT)(g_cw == 2 ? 10 : (g_cw == 1 ? (n->ch > 14 ? 6 : 7) : (g_use_ht ? (n->ch > 14 ? 4 : 5) : (n->ch > 14 ? 0 : 1))))) ||
       wmi_vdev_up(g_assoc_aid, (UCHAR FAR *)g_target)) {
      g_jfail = 10;
      abort_join();
      return -1;
   }
   for (i = 0; i < 6; i++)
      g_jbssid[i] = g_target[i];
   for (i = 0; i < 32; i++)
      g_jssid[i] = (UCHAR)n->ssid[i];
   g_jslen = n->len;
   g_jch = n->ch;
   g_jaid = g_assoc_aid;
   g_jcaps = g_assoc_caps;
   g_jreslen = (g_assoc_reslen > sizeof(g_jres)) ? (USHORT)sizeof(g_jres) : g_assoc_reslen;
   for (i = 0; i < g_jreslen; i++)
      g_jres[i] = g_assoc_res[i];
   g_open_net = g_want_open;
   g_ptk_on = 0;
   g_authorized = 0;
   g_joined = 1;
   g_joining = 0;
   if (g_want_open) {
      wmi_peer_authorize((UCHAR FAR *)g_jbssid);          /* nothing to wait for on an open network            */
      g_authorized = 1;
   }
   tstr("[joined aid "); th16(g_jaid); tstr("]"); tnl();
   return 0;
}

void sta_leave(void)
{
   if (!g_joined && !g_joining && !g_vdev_started)
      return;
   if (g_joined) {
      tx_deauth();
      wmi_vdev_down();
   }
   g_joined = g_authorized = g_ptk_on = 0;
   g_use_ht = g_qos = 0;
   g_cw = 0;
   abort_join();                                          /* peer delete, vdev stop                            */
   g_link_lost = 0;
}

/* Request path: notice that the AP is gone (deauthentication, disassociation, firmware kick-out). */
void sta_poll(void)
{
   if (g_link_lost && g_joined) {
      tstr("[link lost]"); tnl();
      sta_leave();
   }
   g_link_lost = 0;
}

/* ---- keys ------------------------------------------------------------------------------------------- */
int sta_key_add(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx)
{
   if (!g_joined)
      return -1;
   if (klen != 16)
      return -2;                                          /* CCMP only                                         */
   g_sec_ind = 0;
   if (wmi_install_key((UCHAR FAR *)g_jbssid, pairwise ? 0 : idx, (USHORT)(pairwise ? 0 : 1), key, 16))
      return -3;
   if (pairwise) {
      ath_wait(&g_sec_ind, 300);                          /* ath10k waits for the security indication          */
      g_ptk_on = 1;
      wmi_peer_authorize((UCHAR FAR *)g_jbssid);
      g_authorized = 1;
   }
   return 0;
}

int sta_key_remove(USHORT pairwise, UCHAR idx)
{
   (void)idx;
   if (!g_joined || !pairwise)
      return 0;                                           /* group keys are replaced, not removed              */
   g_ptk_on = 0;
   return wmi_install_key((UCHAR FAR *)g_jbssid, 0, 0, 0, 0);
}

/* ---- EAPOL queue and data -------------------------------------------------------------------------- */
#define EQ_N   4
#define EQ_SZ  256
static UCHAR  g_eq[EQ_N][EQ_SZ];
static USHORT g_eql[EQ_N], g_eqh, g_eqt, g_eqn, g_eq_drop;
USHORT        g_rx_eapol, g_rx_ndis, g_rx_nobuf;

USHORT sta_eapol_count(void)
{
   return g_eqn;
}

USHORT sta_eapol_get(UCHAR FAR *dst, USHORT max)
{
   USHORT n, i;

   if (!g_eqn)
      return 0;
   n = g_eql[g_eqt];
   if (n > max)
      n = max;
   for (i = 0; i < n; i++)
      dst[i] = g_eq[g_eqt][i];
   g_eqt = (USHORT)((g_eqt + 1) % EQ_N);
   g_eqn--;
   return n;
}

/* A received frame as Ethernet. Runs in the service path (task time). */
void sta_rx_eth(UCHAR FAR *eth, USHORT len)
{
   USHORT i;
   static USHORT h;                                      /* address passed on: static (SS != DS); one caller at a time */
   UCHAR FAR *d;

   if (eth[12] == 0x88 && eth[13] == 0x8E) {              /* EAPOL: to the supplicant, never to the stack     */
      g_rx_eapol++;
      if (g_eqn >= EQ_N || len > EQ_SZ) {
         g_eq_drop++;
         return;
      }
      for (i = 0; i < len; i++)
         g_eq[g_eqh][i] = eth[i];
      g_eql[g_eqh] = len;
      g_eqh = (USHORT)((g_eqh + 1) % EQ_N);
      g_eqn++;
      return;
   }
   if (!g_open_net && !g_ptk_on)                          /* secure network, no keys yet: nothing else passes  */
      return;
   d = ndis_rx_get(&h);
   if (!d) {
      g_rx_nobuf++;
      return;
   }
   rt_movmem(d, eth, len);
   g_rx_ndis++;
   ndis_rx_put(h, len);
}

int sta_tx(UCHAR FAR *eth, USHORT len)
{
   if (!g_joined)
      return -1;
   if (!g_open_net && !g_ptk_on)
      return -1;                                          /* never send data in the clear on a secure network */
   return htt_tx_eth(eth, len, 0);
}

int sta_eapol_tx(UCHAR FAR *eth, USHORT len)
{
   if (!g_joined)
      return -1;
   /* Unprotected while the pairwise key is not installed (4-way handshake messages 2 and 4: wpa_supplicant installs the
    * key after sending message 4). Once it is, the group-key handshake reply and any re-keying must be encrypted, or
    * the access point ignores them. */
   return htt_tx_eth(eth, len, (USHORT)(g_ptk_on ? 0 : 1));
}
