/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ath_htc.c - Host Target Communication on top of the copy engines (port of ath10k htc.c), and the
 * copy-engine service loop (ath10k_ce_per_engine_service_any + the pci.c send/receive callbacks).
 *
 * Every message starts with an 8-byte HTC header { u8 eid; u8 flags; le16 len; u8 trailer_len/ctrl0;
 * u8 seq/ctrl1; le16 pad }. Endpoint 0 is the HTC control endpoint (READY, CONNECT_SERVICE, SETUP_COMPLETE).
 * Only the WMI control endpoint uses credit flow control; it gets all of the target's transmit credits
 * (ath10k_htc_get_credit_allocation), and credit reports come back in message trailers.
 */
#include "qca.h"

#define HTC_FLAG_NEED_CREDIT_UPDATE 0x01
#define HTC_FLAG_TRAILER_PRESENT    0x02
#define HTC_MSG_READY_ID            1
#define HTC_MSG_CONNECT_SERVICE_ID  2
#define HTC_MSG_CONNECT_RESP_ID     3
#define HTC_MSG_SETUP_COMPLETE_EX_ID 5
#define HTC_RECORD_CREDITS          1
#define HTC_CONN_DISABLE_CREDIT_FLOW 0x0008
#define HTC_EP_COUNT                9
#define HTC_CTRL_MAX                256          /* ATH10K_HTC_MAX_CTRL_MSG_LEN                              */

typedef struct {
   USHORT svc;
   UCHAR  ul, dl;
   USHORT credits;
   UCHAR  seq;
   UCHAR  flow;
   USHORT maxlen;
} EP;

static EP     g_ep[HTC_EP_COUNT];
USHORT        g_wmi_ep = 0xFF, g_htt_ep = 0xFF;
static USHORT g_credit_size, g_credit_total;
static UCHAR  g_ctrl[32];
static USHORT g_ctrl_len, g_ctrl_got;
USHORT        g_htt_rx_msgs, g_htc_drops;
extern int    htc_map_service(USHORT svc, UCHAR *ul, UCHAR *dl);
extern void   htt_rx(UCHAR FAR *p, USHORT len);

void htc_reset(void)
{
   USHORT i;
   USHORT n;

   for (i = 0; i < HTC_EP_COUNT; i++)
      rt_setmem((PVOID)&g_ep[i], 0, sizeof(EP));
   g_wmi_ep = g_htt_ep = 0xFF;
   g_ctrl_got = 0;
   /* endpoint 0 = HTC control, pipes from the service map (RSVD_CTRL: out 0, in 1) */
   g_ep[0].svc = HTC_SVC_RSVD_CTRL;
   htc_map_service(HTC_SVC_RSVD_CTRL, &g_ep[0].ul, &g_ep[0].dl);
   g_ep[0].maxlen = HTC_CTRL_MAX;
   /* transmit slots of the source engines are free again */
   for (i = 0; i < CE_COUNT; i++)
      if (g_ce[i].src_n)
         for (n = 0; n < CE_RX_MAX; n++)
            g_ce[i].bfree[n] = 1;
}

USHORT htc_credits(USHORT ep) { return (ep < HTC_EP_COUNT) ? g_ep[ep].credits : 0; }

/* ---- transmit --------------------------------------------------------------------------------------- */
UCHAR FAR *htc_tx_begin(USHORT ep, USHORT *slot, ULONG *phys)
{
   CE    *c;
   USHORT b;
   UCHAR FAR *p;

   if (ep >= HTC_EP_COUNT || (ep && !g_ep[ep].svc))
      return 0;
   c = &g_ce[g_ep[ep].ul];
   /* Senders are serialised by the caller (nic_lock); the service loop only ever frees slots. */
   for (b = 0; b < c->nbuf; b++)
      if (c->bfree[b]) {
         c->bfree[b] = 0;
         *slot = b;
         p = ce_slot(g_ep[ep].ul, b, phys);
         return p + HTC_HDR_LEN;
      }
   return 0;
}

static USHORT credits_for(USHORT len)
{
   USHORT n = 0, acc = 0;

   if (!g_credit_size)
      return 1;
   while (acc < len) { acc = (USHORT)(acc + g_credit_size); n++; }
   return n;
}

/* Returns 0 when queued, -1 without credits (slot kept: retry), -2 when the ring was full (slot kept). */
int htc_tx_end(USHORT ep, USHORT slot, USHORT paylen)
{
   EP    *e = &g_ep[ep];
   CE    *c = &g_ce[e->ul];
   static ULONG ph;
   UCHAR FAR *h = ce_slot(e->ul, slot, &ph);
   USHORT need = 0;
   int    wi;

   if (e->flow) {
      need = credits_for((USHORT)(paylen + HTC_HDR_LEN));
      if (e->credits < need)
         return -1;
   }
   h[0] = (UCHAR)ep;
   h[1] = (UCHAR)(e->flow ? HTC_FLAG_NEED_CREDIT_UPDATE : 0);
   h[2] = (UCHAR)paylen;
   h[3] = (UCHAR)(paylen >> 8);
   h[4] = 0;
   h[5] = e->seq++;
   h[6] = h[7] = 0;
   wi = ce_send(e->ul, ph, (USHORT)(paylen + HTC_HDR_LEN), ep, 0);
   if (wi < 0)
      return -2;
   c->dctx[wi] = (UCHAR)slot;
   e->credits = (USHORT)(e->credits - need);
   return 0;
}

static void htc_tx_cancel(USHORT ep, USHORT slot)
{
   g_ce[g_ep[ep].ul].bfree[slot] = 1;
}

/* ---- receive -------------------------------------------------------------------------------------- */
/* ath10k_htc_process_trailer: records { u8 id; u8 len; u8 pad[2]; payload[len] } */
static void trailer(UCHAR FAR *t, USHORT n)
{
   while (n >= 4) {
      UCHAR  id = t[0], rl = t[1];
      USHORT k;

      if ((USHORT)(rl + 4) > n)
         break;
      if (id == HTC_RECORD_CREDITS)
         for (k = 0; k + 4 <= rl; k = (USHORT)(k + 4)) {     /* { u8 eid; u8 credits; u8 pad[2] }        */
            UCHAR eid = t[4 + k], cr = t[5 + k];

            if (eid >= HTC_EP_COUNT)
               break;
            g_ep[eid].credits = (USHORT)(g_ep[eid].credits + cr);
         }
      t += 4 + rl;
      n = (USHORT)(n - 4 - rl);
   }
}

void htc_rx(USHORT pipe, UCHAR FAR *p, USHORT len)
{
   UCHAR  eid, flags, tl;
   USHORT plen;

   if (len < HTC_HDR_LEN) { g_htc_drops++; return; }
   eid   = p[0];
   flags = p[1];
   plen  = (USHORT)(p[2] | (p[3] << 8));
   tl    = p[4];
   if ((USHORT)(plen + HTC_HDR_LEN) > len || eid >= HTC_EP_COUNT) { g_htc_drops++; return; }
   if ((flags & HTC_FLAG_TRAILER_PRESENT) && tl && tl <= plen) {
      trailer(p + HTC_HDR_LEN + plen - tl, tl);
      plen = (USHORT)(plen - tl);
   }
   if (eid == 0) {
      USHORT n = (plen < sizeof(g_ctrl)) ? plen : (USHORT)sizeof(g_ctrl);

      rt_movmem((PVOID)g_ctrl, p + HTC_HDR_LEN, n);
      g_ctrl_len = n;
      g_ctrl_got = 1;
   } else if (eid == g_wmi_ep) {
      wmi_rx(p + HTC_HDR_LEN, plen);
   } else if (eid == g_htt_ep) {
      g_htt_rx_msgs++;
      htt_rx(p + HTC_HDR_LEN, plen);
   } else {
      g_htc_drops++;
   }
   (void)pipe;
}

/* ---- the service loop ----------------------------------------------------------------------------- */
static USHORT g_svc_lock, g_svc_again;
USHORT        g_svc_runs;

static void service_once(void)
{
   USHORT ce;
   static USHORT nb;
   int    idx;

   for (ce = 0; ce < 5; ce++)
      if (g_ce[ce].src_n || g_ce[ce].dst_n)
         ce_clear(ce);
   /* completed sends free their slots */
   for (ce = 0; ce < 5; ce++) {
      CE *c = &g_ce[ce];

      if (!c->src_n || !c->nbuf)
         continue;
      while ((idx = ce_send_done(ce)) != 0)
         c->bfree[c->dctx[idx - 1]] = 1;
   }
   /* received messages */
   for (ce = 1; ce <= 2; ce++) {
      CE *c = &g_ce[ce];

      while ((idx = ce_rx_next(ce, &nb)) != 0) {
         UCHAR b = c->dctx[idx - 1];

         htc_rx(ce, ce_slot(ce, b, 0), nb);
         c->bfree[b] = 1;
      }
      ce_rx_fill(ce);
   }
}

/* Task time only (request path or context hook): never blocks. A caller that finds the loop running on
 * another CPU leaves a note, and the owner runs it once more. */
void ath_service(void)
{
   USHORT id = (USHORT)(rt_cpu_id() + 1);

   if (rt_lock_try(&g_svc_lock, id)) {
      g_svc_again = 1;
      return;
   }
   do {
      g_svc_again = 0;
      g_svc_runs++;
      service_once();
   } while (g_svc_again);
   rt_lock_release(&g_svc_lock);
}

int ath_wait(USHORT *flag, USHORT ms)
{
   ULONG t0 = qc_ms();

   for (;;) {
      ath_service();
      if (*flag)
         return 0;
      if ((long)(qc_ms() - t0) > (long)ms)
         return -1;
      qc_msleep(4);
   }
}

/* ---- HTC setup ------------------------------------------------------------------------------------ */
static USHORT get16(UCHAR *p) { return (USHORT)(p[0] | (p[1] << 8)); }

int htc_wait_target(void)
{
   if (ath_wait(&g_ctrl_got, 3000)) {
      qc_err("no HTC READY from the target", 0UL);
      return -1;
   }
   g_ctrl_got = 0;
   if (g_ctrl_len < 8 || get16(g_ctrl) != HTC_MSG_READY_ID) {
      qc_err("unexpected HTC control message", (ULONG)get16(g_ctrl));
      return -2;
   }
   g_credit_total = get16(g_ctrl + 2);
   g_credit_size  = get16(g_ctrl + 4);
   if (!g_credit_total || !g_credit_size) {
      qc_err("target offers no HTC credits", 0UL);
      return -3;
   }
   return 0;
}

static UCHAR FAR *g_ctrl_p;
static USHORT     g_ctrl_slot;

static UCHAR FAR *ctrl_begin(void)
{
   static USHORT slot;
   static ULONG  ph;

   g_ctrl_p = htc_tx_begin(0, &slot, &ph);
   g_ctrl_slot = slot;
   return g_ctrl_p;
}

static int ctrl_end(USHORT len)
{
   int rc;
   ULONG t0 = qc_ms();

   while ((rc = htc_tx_end(0, g_ctrl_slot, len)) != 0) {
      if ((long)(qc_ms() - t0) > 1000L) {
         htc_tx_cancel(0, g_ctrl_slot);
         return rc;
      }
      ath_service();
      qc_msleep(2);
   }
   return 0;
}

int htc_connect(USHORT svc, USHORT *pep)
{
   UCHAR FAR *m;
   USHORT flags, eid, status;
   int    wmi = (svc == HTC_SVC_WMI_CONTROL);

   m = ctrl_begin();
   if (!m)
      return -1;
   flags = wmi ? (USHORT)((g_credit_total & 0xFF) << 8) : HTC_CONN_DISABLE_CREDIT_FLOW;
   m[0] = HTC_MSG_CONNECT_SERVICE_ID; m[1] = 0;
   m[2] = (UCHAR)svc;   m[3] = (UCHAR)(svc >> 8);
   m[4] = (UCHAR)flags; m[5] = (UCHAR)(flags >> 8);
   m[6] = m[7] = 0;
   g_ctrl_got = 0;
   if (ctrl_end(8))
      return -2;
   if (ath_wait(&g_ctrl_got, 1000)) {
      qc_err("no HTC connect response for service", (ULONG)svc);
      return -3;
   }
   g_ctrl_got = 0;
   if (g_ctrl_len < 8 || get16(g_ctrl) != HTC_MSG_CONNECT_RESP_ID || get16(g_ctrl + 2) != svc)
      return -4;
   status = g_ctrl[4];
   eid    = g_ctrl[5];
   if (status != 0 || eid == 0 || eid >= HTC_EP_COUNT) {
      qc_err("HTC service refused (status)", (ULONG)status);
      return -5;
   }
   g_ep[eid].svc     = svc;
   g_ep[eid].maxlen  = get16(g_ctrl + 6);
   g_ep[eid].flow    = (UCHAR)wmi;
   g_ep[eid].credits = wmi ? g_credit_total : 0;
   if (htc_map_service(svc, &g_ep[eid].ul, &g_ep[eid].dl))
      return -6;
   *pep = eid;
   return 0;
}

int htc_start(void)
{
   UCHAR FAR *m = ctrl_begin();

   if (!m)
      return -1;
   rt_setmem(m, 0, 12);
   m[0] = HTC_MSG_SETUP_COMPLETE_EX_ID;              /* flags 0, no receive bundling over PCI       */
   return ctrl_end(12);
}
