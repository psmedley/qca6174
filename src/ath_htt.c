/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ath_htt.c - HTT, the data path between the host and the firmware (port of ath10k htt.c, htt_tx.c, htt_rx.c for
 * the QCA6174 "TLV" firmware: low-latency PCIe, 32-bit addresses, full RX reorder, native-WiFi decapsulation).
 *
 * Transmit: an Ethernet frame is turned into an 802.11 data frame (24-byte header, LLC/SNAP) and sent as an HTT
 * TX_FRM message: copy engine 4 gets two chained descriptors, [HTC header + HTT header + TX descriptor] and the
 * first bytes of the frame ("prefetch"); the TX descriptor's fragment list points at the whole frame.
 * The firmware encrypts (CCMP) with the key installed through WMI when the frame's Protected bit is set.
 *
 * Receive: the host gives the firmware a ring of empty 2 KB buffers; the firmware fills them and reports them,
 * in order, in RX_IN_ORD_PADDR_IND messages (physical address + length per MSDU). Each buffer starts with the
 * 300-byte rx descriptor (htt_rx_desc_v1: layout computed from the ath10k structs with gcc) followed by the MSDU
 * as an 802.11 "native wifi" frame, which is converted back to Ethernet.
 *
 * Message type numbers are those of the TLV firmware (htt_tlv_t2h_msg_types).
 */
#include "qca.h"

#define H2T_VERSION_REQ     0
#define H2T_TX_FRM          1
#define H2T_RX_RING_CFG     2
#define H2T_AGGR_CFG        5

#define T2H_VERSION_CONF    0x00
#define T2H_PEER_MAP        0x03
#define T2H_PEER_UNMAP      0x04
#define T2H_TX_COMPL_IND    0x07
#define T2H_SEC_IND         0x0B
#define T2H_MGMT_TX_COMPL   0x0E
#define T2H_RX_IN_ORD_PADDR_IND 0x12

#define RXRING_N            128                  /* entries (HTT_RX_RING_SIZE_MIN); fill stays below half       */
#define RX_NBUF             62
#define RX_PER_CHUNK        31                   /* 31 x 2048 = 63488 bytes fit one 64 KB chunk                 */
#define RX_BUF_SZ           2048
#define RXD_ATTENTION       4                    /* offsets inside the 300-byte descriptor (struct htt_rx_desc_v1) */
#define RXD_MSDU_START      24
#define RXD_PAYLOAD         300
#define ATT_MSDU_DONE       0x80000000UL
#define ATT_FCS_ERR         0x40000000UL
#define ATT_DECRYPT_ERR     0x20000000UL
#define ATT_TKIP_MIC_ERR    0x10000000UL

#define TX_N                24
#define TX_SLOT             1584                 /* 40-byte header block + frame (<= 1544), multiple of 8        */
#define TX_HDR              40
#define RX_CHUNK_BYTES      63488U               /* 31 x 2048 (a plain int product would overflow 16 bits)   */
#define TX_POOL_BYTES       38016U               /* 24 x 1584                                                */
#define TX_PREFETCH         50                   /* 36 + 4 + 8 + 2 (ath10k htt->prefetch_len)                    */
#define FW_FEAT_NO_NWIFI_4ADDR_PAD 8

USHORT g_htt_ready;
/* why received frames were dropped (status text RXWHY): 0 unknown buffer, 1 not done, 2 error flags, 3 decap format,
 * 4 not data / null, 5 too short, 6 too long, 7 not joined */
USHORT g_rx_why[8];
UCHAR  g_rx_decap_last, g_rx_fc_last;
ULONG  g_rx_attn_last;
USHORT g_htt_txok, g_htt_txfail, g_htt_txfull, g_htt_rxfrm, g_htt_rxdrop, g_htt_rxind, g_htt_txcompl;

static DMA     g_rxring, g_rxidx, g_rb[RX_NBUF], g_txpool;
static UCHAR   g_rb_free[RX_NBUF];
static USHORT  g_rx_wr, g_rx_fill, g_alloc_done;
static UCHAR   g_tx_busy[TX_N];
static ULONG   g_tx_t[TX_N];
static USHORT  g_tx_lock;
static USHORT  g_htt_ver_done, g_htt_major, g_htt_minor;
static UCHAR   g_eth[1536];                       /* the Ethernet frame built from a received MSDU            */

static ULONG g32(UCHAR FAR *p)
{
   U32 u;

   u.b[0] = p[0]; u.b[1] = p[1]; u.b[2] = p[2]; u.b[3] = p[3];
   return u.l;
}

static void p32(UCHAR FAR *p, ULONG v)
{
   U32 u;

   u.l = v;
   p[0] = u.b[0]; p[1] = u.b[1]; p[2] = u.b[2]; p[3] = u.b[3];
}

static void p16(UCHAR FAR *p, USHORT v)
{
   p[0] = (UCHAR)v;
   p[1] = (UCHAR)(v >> 8);
}

/* ---- receive ring ------------------------------------------------------------------------------- */
static int rx_alloc(void)
{
   static DMA a, b;                              /* addresses are passed on: static, not on the stack       */
   USHORT i, k;
   ULONG  off;

   if (g_alloc_done)
      return 0;
   if (dma_get(RXRING_N * 4, &g_rxring) || dma_get(8, &g_rxidx) ||
       dma_get(RX_CHUNK_BYTES, &a) || dma_get(RX_CHUNK_BYTES, &b) ||
       dma_get(TX_POOL_BYTES, &g_txpool))
      return -1;
   for (i = 0; i < RX_NBUF; i++) {
      DMA *src = (i < RX_PER_CHUNK) ? &a : &b;

      k = (USHORT)((i < RX_PER_CHUNK) ? i : i - RX_PER_CHUNK);
      off = (ULONG)k << 11;                      /* k * 2048                                                   */
      g_rb[i].sel  = src->sel;
      g_rb[i].off  = (USHORT)(src->off + (USHORT)off);
      g_rb[i].phys = src->phys + off;
   }
   g_alloc_done = 1;
   return 0;
}

/* ath10k_htt_rx_ring_fill_n: deposit empty buffers, then tell the firmware how far with the alloc index. */
static void rx_refill(void)
{
   USHORT i;

   while (g_rx_fill < RX_NBUF) {
      for (i = 0; i < RX_NBUF && !g_rb_free[i]; i++)
         ;
      if (i == RX_NBUF)
         break;
      {
         UCHAR FAR *d = DPTR(g_rb[i]);

         d[RXD_ATTENTION] = d[RXD_ATTENTION + 1] = d[RXD_ATTENTION + 2] = d[RXD_ATTENTION + 3] = 0;
         p32(DPTR(g_rxring) + ((USHORT)g_rx_wr << 2), g_rb[i].phys);
      }
      g_rb_free[i] = 0;
      g_rx_wr = (USHORT)((g_rx_wr + 1) & (RXRING_N - 1));
      g_rx_fill++;
   }
   p32(DPTR(g_rxidx), (ULONG)g_rx_wr);          /* the firmware reads this word                               */
}

static int find_buf(ULONG paddr)
{
   USHORT i;

   for (i = 0; i < RX_NBUF; i++)
      if (g_rb[i].phys == paddr)
         return (int)i;
   return -1;
}

/* ---- control messages ----------------------------------------------------------------------------- */
static int ctrl_send(USHORT slot, USHORT len)
{
   ULONG t0 = qc_ms();
   int   rc;

   while ((rc = htc_tx_end(g_htt_ep, slot, len)) != 0) {
      if ((long)(qc_ms() - t0) > 1000L) {
         g_ce[4].bfree[slot] = 1;
         return rc;
      }
      ath_service();
      qc_msleep(2);
   }
   return 0;
}

static UCHAR FAR *ctrl_begin(USHORT *slot)
{
   static ULONG ph;
   ULONG t0 = qc_ms();
   UCHAR FAR *p;

   while ((p = htc_tx_begin(g_htt_ep, slot, &ph)) == 0) {
      if ((long)(qc_ms() - t0) > 1000L)
         return 0;
      ath_service();
      qc_msleep(2);
   }
   return p;
}

void htt_reset(void)
{
   USHORT i;

   g_htt_ready = 0;
   g_rx_wr = g_rx_fill = 0;
   for (i = 0; i < RX_NBUF; i++)
      g_rb_free[i] = 1;
   for (i = 0; i < TX_N; i++)
      g_tx_busy[i] = 0;
}

/* ath10k_htt_setup: version request, rx ring configuration, aggregation limits. */
int htt_setup(void)
{
   static USHORT slot;
   UCHAR FAR *m;

   if (!g_svc_full_reorder) {
      qc_err("firmware lacks WMI_SERVICE_RX_FULL_REORDER (legacy HTT receive is not implemented)", 0UL);
      return -1;
   }
   if (rx_alloc()) { qc_err("no DMA memory for the HTT rings", 0UL); return -2; }
   htt_reset();
   rt_setmem(DPTR(g_rxring), 0, RXRING_N * 4);
   rx_refill();

   m = ctrl_begin(&slot);                                  /* version request: 1 byte + padding to 4           */
   if (!m) return -3;
   m[0] = H2T_VERSION_REQ; m[1] = m[2] = m[3] = 0;
   g_htt_ver_done = 0;
   if (ctrl_send(slot, 4)) return -4;
   if (ath_wait(&g_htt_ver_done, 2000)) { qc_err("no HTT version confirmation", 0UL); return -5; }
   if (g_htt_major != 2 && g_htt_major != 3) { qc_err("unsupported HTT version", (ULONG)g_htt_major); return -6; }

   m = ctrl_begin(&slot);                                  /* rx ring setup: 4-byte header + one 36-byte ring   */
   if (!m) return -7;
   m[0] = H2T_RX_RING_CFG;
   m[1] = 1;                                               /* num_rings                                        */
   m[2] = m[3] = 0;
   m += 4;
   p32(m + 0, g_rxidx.phys);                               /* fw_idx_shadow_reg_paddr                          */
   p32(m + 4, g_rxring.phys);                              /* rx_ring_base_paddr                               */
   p16(m + 8, RXRING_N);                                   /* rx_ring_len (4-byte words)                       */
   p16(m + 10, RX_BUF_SZ);                                 /* rx_ring_bufsize                                  */
   p16(m + 12, 0xFFFF);                                    /* flags: every HTT_RX_RING_FLAGS_*                 */
   p16(m + 14, g_rx_wr);                                   /* fw_idx_init_val = the alloc index                */
   p16(m + 16, 236 >> 2);                                  /* mac80211_hdr_offset  (rx_hdr_status)             */
   p16(m + 18, RXD_PAYLOAD >> 2);                          /* msdu_payload_offset                              */
   p16(m + 20, 84 >> 2);                                   /* ppdu_start_offset                                */
   p16(m + 22, 124 >> 2);                                  /* ppdu_end_offset                                  */
   p16(m + 24, 12 >> 2);                                   /* mpdu_start_offset                                */
   p16(m + 26, 80 >> 2);                                   /* mpdu_end_offset                                  */
   p16(m + 28, 24 >> 2);                                   /* msdu_start_offset                                */
   p16(m + 30, 40 >> 2);                                   /* msdu_end_offset                                  */
   p16(m + 32, 4 >> 2);                                    /* rx_attention_offset                              */
   p16(m + 34, 8 >> 2);                                    /* frag_info_offset                                 */
   if (ctrl_send(slot, 40)) return -8;

   m = ctrl_begin(&slot);                                  /* aggregation limits: A-MPDU 64, A-MSDU 3          */
   if (!m) return -9;
   m[0] = H2T_AGGR_CFG; m[1] = 64; m[2] = 3;
   if (ctrl_send(slot, 3)) return -10;
   g_htt_ready = 1;
   return 0;
}

/* ---- receive path -------------------------------------------------------------------------------- */
/* Throughput diagnostics: the busiest one-second window of received / transmitted frames. */
USHORT g_pps_rx_max, g_pps_tx_max;
static ULONG  g_w_rx, g_w_tx;
static USHORT g_c_rx, g_c_tx;

static void stat_rx(void)
{
   ULONG t = qc_ms();

   if ((long)(t - g_w_rx) >= 1000L) {
      if (g_c_rx > g_pps_rx_max) g_pps_rx_max = g_c_rx;
      g_c_rx = 0;
      g_w_rx = t;
   }
   g_c_rx++;
}

static void stat_tx(void)
{
   ULONG t = qc_ms();

   if ((long)(t - g_w_tx) >= 1000L) {
      if (g_c_tx > g_pps_tx_max) g_pps_tx_max = g_c_tx;
      g_c_tx = 0;
      g_w_tx = t;
   }
   g_c_tx++;
}

static void rx_msdu(USHORT b, USHORT mlen)
{
   UCHAR FAR *d = DPTR(g_rb[b]);
   UCHAR FAR *pl = d + RXD_PAYLOAD;
   ULONG  attn = g32(d + RXD_ATTENTION);
   USHORT hl, blen, n, i;
   UCHAR  fc0, fc1, decap;
   UCHAR FAR *da, FAR *sa, FAR *body;

   g_rx_attn_last = attn;
   if (!(attn & ATT_MSDU_DONE)) {
      g_htt_rxdrop++; g_rx_why[1]++;
      return;
   }
   if (attn & (ATT_FCS_ERR | ATT_DECRYPT_ERR | ATT_TKIP_MIC_ERR)) {
      g_htt_rxdrop++; g_rx_why[2]++;
      return;
   }
   if (mlen > RX_BUF_SZ - RXD_PAYLOAD)
      mlen = RX_BUF_SZ - RXD_PAYLOAD;
   /* struct rx_msdu_start_common is { info0; flow_id_crc; info1 }: info1 is at +8, its bits 9:8 are the decap
    * format (0 raw, 1 native wifi, 2 Ethernet II, 3 802.3 with SNAP). (An earlier version read byte +5.) */
   decap = (UCHAR)(d[RXD_MSDU_START + 9] & 3);
   g_rx_decap_last = decap;
   g_rx_fc_last = pl[0];

   if (decap == 2) {                                      /* Ethernet II: nothing to convert                    */
      if (mlen < 14 || mlen > 1514) { g_htt_rxdrop++; g_rx_why[6]++; return; }
      rt_movmem((PVOID)g_eth, pl, mlen);
      g_htt_rxfrm++;
      stat_rx();
      sta_rx_eth((UCHAR FAR *)g_eth, mlen);
      return;
   }
   if (decap != 1) {                                      /* raw and 802.3 SNAP are not produced by this setup  */
      g_htt_rxdrop++; g_rx_why[3]++;
      return;
   }
   if (mlen < 24 + 8) {
      g_htt_rxdrop++; g_rx_why[5]++;
      return;
   }
   fc0 = pl[0];
   fc1 = pl[1];
   if ((fc0 & 0x0C) != 0x08 || (fc0 & 0x40)) {           /* not data, or a null-data frame                     */
      g_htt_rxdrop++; g_rx_why[4]++;
      return;
   }
   hl = 24;
   if ((fc1 & 3) == 3) hl = 30;
   if (fc0 & 0x80) hl += 2;                               /* QoS control                                        */
   if (!fw_feature(FW_FEAT_NO_NWIFI_4ADDR_PAD))
      hl = (USHORT)((hl + 3) & ~3);                       /* ath10k_htt_rx_nwifi_hdrlen                          */
   if (mlen < hl + 8) { g_htt_rxdrop++; g_rx_why[5]++; return; }
   da = ((fc1 & 3) == 3 || (fc1 & 1)) ? pl + 16 : pl + 4;
   sa = ((fc1 & 3) == 3) ? pl + 24 : ((fc1 & 2) ? pl + 16 : pl + 10);
   body = pl + hl;
   blen = (USHORT)(mlen - hl);
   for (i = 0; i < 6; i++) {
      g_eth[i] = da[i];
      g_eth[6 + i] = sa[i];
   }
   if (body[0] == 0xAA && body[1] == 0xAA && body[2] == 0x03 &&
       ((body[3] == 0 && body[4] == 0 && body[5] == 0 &&
         !((body[6] == 0x80 && body[7] == 0xF3) || (body[6] == 0x81 && body[7] == 0x37))) ||
        (body[3] == 0 && body[4] == 0 && body[5] == 0xF8))) {
      g_eth[12] = body[6];                                /* RFC 1042 / bridge tunnel: the SNAP type is the      */
      g_eth[13] = body[7];                                /* Ethernet type                                       */
      body += 8;
      blen = (USHORT)(blen - 8);
   } else {
      g_eth[12] = (UCHAR)(blen >> 8);                     /* IEEE 802.3: length field, LLC header kept           */
      g_eth[13] = (UCHAR)blen;
   }
   n = (USHORT)(14 + blen);
   if (n > 1514) { g_htt_rxdrop++; g_rx_why[6]++; return; }
   rt_movmem((PVOID)(g_eth + 14), body, blen);
   g_htt_rxfrm++;
   stat_rx();
   sta_rx_eth((UCHAR FAR *)g_eth, n);
}

/* RX_IN_ORD_PADDR_IND: u8 type; u8 info; le16 peer_id; u8 vdev_id; u8 reserved; le16 msdu_count; then per MSDU
 * { le32 paddr; le16 len; u8 fw_desc; u8 reserved } (32-bit target). */
static void rx_in_ord(UCHAR FAR *p, USHORT len)
{
   USHORT n, i, avail;
   UCHAR  offload;

   if (len < 8)
      return;
   g_htt_rxind++;
   offload = (UCHAR)((p[1] & 0x20) ? 1 : 0);
   n = (USHORT)(p[6] | (p[7] << 8));
   avail = (USHORT)((len - 8) >> 3);
   if (n > avail)
      n = avail;
   for (i = 0; i < n; i++) {
      UCHAR FAR *d = p + 8 + (i << 3);
      ULONG paddr = g32(d);
      USHORT mlen = (USHORT)(d[4] | (d[5] << 8));
      int    b = find_buf(paddr);

      if (b < 0 || g_rb_free[b]) {
         g_htt_rxdrop++; g_rx_why[0]++;                   /* a buffer we do not know: ring state is suspect   */
         continue;
      }
      if (!offload && g_joined)
         rx_msdu((USHORT)b, mlen);
      else
         g_rx_why[7]++;                                   /* offloaded, or not connected (not counted as a drop) */
      g_rb_free[b] = 1;
      if (g_rx_fill)
         g_rx_fill--;
   }
   rx_refill();
}

/* TX_COMPL_IND: u8 type; u8 flags (status in bits 2:0); u8 num_msdus; u8 flags2; le16 msdu_ids[]. */
static void tx_compl(UCHAR FAR *p, USHORT len)
{
   USHORT n, i, id;

   if (len < 4)
      return;
   n = p[2];
   if (n > (USHORT)((len - 4) >> 1))
      n = (USHORT)((len - 4) >> 1);
   for (i = 0; i < n; i++) {
      id = (USHORT)(p[4 + (i << 1)] | (p[5 + (i << 1)] << 8));
      if (id < TX_N) {
         g_tx_busy[id] = 0;
         g_htt_txcompl++;
      }
   }
   if ((p[1] & 7) != 0)
      g_htt_txfail++;                                     /* discarded / no ack                               */
}

void htt_rx(UCHAR FAR *p, USHORT len)
{
   if (len < 1)
      return;
   switch (p[0]) {
   case T2H_VERSION_CONF:
      if (len >= 3) {
         g_htt_minor = p[1];
         g_htt_major = p[2];
         g_htt_ver_done = 1;
      }
      break;
   case T2H_RX_IN_ORD_PADDR_IND:
      rx_in_ord(p, len);
      break;
   case T2H_TX_COMPL_IND:
      tx_compl(p, len);
      break;
   case T2H_MGMT_TX_COMPL:                                /* u8 type; u8 rsvd[2]; u8 flags; le32 desc_id; le32 status */
      if (len >= 12) {
         ULONG id = g32(p + 4);

         if (id < TX_N) {
            g_tx_busy[id] = 0;
            g_htt_txcompl++;
         }
         if (g32(p + 8) != 0UL)
            g_htt_txfail++;
      }
      break;
   case T2H_PEER_MAP:                                     /* u8 type; u8 vdev_id; le16 peer_id; u8 addr[6]    */
      if (len >= 10) {
         USHORT i;

         for (i = 0; i < 6; i++)
            g_peer_map_addr[i] = p[4 + i];
         g_peer_map = 1;
      }
      break;
   case T2H_SEC_IND:
      g_sec_ind = 1;
      break;
   default:                                               /* PEER_UNMAP, RX_ADDBA/DELBA, credit updates, ...   */
      break;
   }
}

/* ---- transmit path -------------------------------------------------------------------------------- */
/* mgmt = 0: an Ethernet frame (eth, len) becomes a native-wifi data frame. mgmt = 1: eth is a complete 802.11
 * management frame (HTT >= 3 TX_FRM with packet type MGMT: the frag list is replaced by the frame address). */
static int tx_common(UCHAR FAR *eth, USHORT len, USHORT noenc, USHORT mgmt)
{
   USHORT cid = (USHORT)(rt_cpu_id() + 1), fl, i, slot, flen, pre, so;
   UCHAR FAR *s, FAR *f;
   ULONG  ph, t;
   CE    *c = &g_ce[4];
   int    r;

   if (!g_htt_ready || (!mgmt && !g_joined))
      return -1;
   if (mgmt ? (len < 24 || len > 400) : (len < 14 || len > 1514))
      return -1;
   fl = rt_irq_save();
   if (rt_lock_try(&g_tx_lock, cid)) {
      rt_irq_restore(fl);
      return -2;
   }
   t = qc_ms();
   for (slot = 0; slot < TX_N && g_tx_busy[slot]; slot++)
      ;
   if (slot == TX_N) {                                    /* all busy: reclaim slots whose completion never came */
      for (i = 0; i < TX_N; i++)
         if ((long)(t - g_tx_t[i]) > 3000L)
            g_tx_busy[i] = 0;
      for (slot = 0; slot < TX_N && g_tx_busy[slot]; slot++)
         ;
   }
   if (slot == TX_N || ((c->src_sw - 1 - c->src_wr) & c->src_mask) < 2) {
      g_htt_txfull++;
      rt_lock_release(&g_tx_lock);
      rt_irq_restore(fl);
      return -2;
   }
   g_tx_busy[slot] = 1;
   g_tx_t[slot] = t;

   so = 0;
   for (i = 0; i < slot; i++)
      so += TX_SLOT;                                      /* slot * 1584 without a multiply helper             */
   s = DPTR(g_txpool) + so;
   ph = g_txpool.phys + so;
   f = s + TX_HDR;

   {
      UCHAR  prot = 0, flags0;
      USHORT flags1;
      ULONG  frags;

      if (mgmt) {
         rt_movmem((PVOID)f, eth, len);
         flen = len;
         flags0 = (UCHAR)(0x01 | 0x60 | 0x04);          /* MAC_HDR_PRESENT | pkt type MGMT (3) | NO_ENCRYPT     */
         flags1 = 0x0C40;                                /* vdev 0, ext tid 17 (management), POSTPONED           */
         frags = ph + TX_HDR;                            /* the frame itself                                     */
      } else {
         /* 802.11 native-wifi frame: data, To-DS, [Protected], addr1 = BSSID, addr2 = SA, addr3 = DA, LLC/SNAP */
         prot = (UCHAR)((!noenc && g_ptk_on) ? 0x40 : 0);
         f[0] = 0x08;                                    /* native wifi: no QoS control field; the firmware adds it from the TID */
         f[1] = (UCHAR)(0x01 | prot);
         f[2] = f[3] = 0;
         for (i = 0; i < 6; i++) {
            f[4 + i]  = g_jbssid[i];
            f[10 + i] = eth[6 + i];
            f[16 + i] = eth[i];
         }
         f[22] = f[23] = 0;                              /* sequence control: the firmware numbers frames        */
         so = 24;
         f[so] = 0xAA; f[so + 1] = 0xAA; f[so + 2] = 0x03; f[so + 3] = f[so + 4] = f[so + 5] = 0;
         f[so + 6] = eth[12]; f[so + 7] = eth[13];
         rt_movmem((PVOID)(f + so + 8), eth + 14, (USHORT)(len - 14));
         flen = (USHORT)(so + 8 + len - 14);
         flags0 = (UCHAR)(0x01 | 0x20 | (prot ? 0 : 0x04));   /* MAC_HDR_PRESENT | NATIVE_WIFI | NO_ENCRYPT    */
         flags1 = (USHORT)(g_qos ? 0x0800 : 0x0C00);     /* vdev 0, ext tid 0 (QoS) or 16 (non-QoS), POSTPONED   */
         frags = ph;                                     /* the fragment list at the start of the slot           */
      }
      pre = (flen < TX_PREFETCH) ? flen : TX_PREFETCH;
      pre = (USHORT)((pre + 3) & ~3);
      /* frag list (data frames) */
      p32(s + 0, ph + TX_HDR);
      p32(s + 4, (ULONG)flen);
      p32(s + 8, 0UL);
      p32(s + 12, 0UL);
      /* HTC header */
      s[16] = (UCHAR)g_htt_ep; s[17] = 0;
      p16(s + 18, (USHORT)(1 + 15 + pre));
      s[20] = s[21] = 0; s[22] = s[23] = 0;
      /* HTT command header and TX descriptor */
      s[24] = H2T_TX_FRM;
      s[25] = flags0;
      p16(s + 26, flags1);
      p16(s + 28, flen);
      p16(s + 30, slot);                                 /* msdu id                                              */
      p32(s + 32, frags);                                /* frags_paddr                                          */
      p32(s + 36, 0x0000FFFFUL);                         /* peer id: invalid (HTT_INVALID_PEERID)                */
   }
   r = ce_send(4, ph + 16, 24, 0, CE_SEND_GATHER);
   if (r >= 0) {
      c->dctx[r] = 0xFF;
      r = ce_send(4, ph + TX_HDR, pre, 0, 0);
      if (r >= 0)
         c->dctx[r] = 0xFF;
   }
   if (r < 0) {
      g_tx_busy[slot] = 0;
      g_htt_txfull++;
      rt_lock_release(&g_tx_lock);
      rt_irq_restore(fl);
      return -2;
   }
   g_htt_txok++;
   stat_tx();
   rt_lock_release(&g_tx_lock);
   rt_irq_restore(fl);
   return 0;
}

int htt_tx_eth(UCHAR FAR *eth, USHORT len, USHORT noenc)
{
   return tx_common(eth, len, noenc, 0);
}

int htt_tx_mgmt(UCHAR FAR *frame, USHORT len)
{
   return tx_common(frame, len, 1, 1);
}
