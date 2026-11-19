/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ath_sta.c - station state: the scan table (beacons and probe responses from WMI_MGMT_RX events) and,
 * in phase 2, the join (vdev start, peer, association), keys and the HTT data path.
 *
 * The scan table keeps the frame body of each BSS (12 fixed bytes: timestamp, beacon interval, capability;
 * then the information elements) because NDIS_802_11_BSSID_LIST_EX hands exactly that to the supplicant.
 */
#include "qca.h"

net_t  g_net[NET_MAX];
USHORT g_nnet;
USHORT g_joined;
UCHAR  g_jbssid[6], g_jssid[32], g_jslen, g_jch;

void sta_scan_clear(void)
{
   g_nnet = 0;
}

static int mac_eq(UCHAR FAR *a, UCHAR *b)
{
   USHORT i;

   for (i = 0; i < 6; i++)
      if (a[i] != b[i])
         return 0;
   return 1;
}

/* frame: 802.11 management frame (24-byte header), chan: channel number from the event */
void sta_beacon(UCHAR FAR *f, USHORT len, USHORT chan, short rssi)
{
   UCHAR FAR *bssid = f + 16;                    /* address 3                                              */
   UCHAR FAR *body = f + 24;
   USHORT blen, k, i, pos, slot;
   net_t *n;

   if (len < 24 + 12)
      return;
   blen = (USHORT)(len - 24);
   /* the DS parameter set (element 3) is more reliable than the event's channel (off-channel reception) */
   for (pos = 12; pos + 2 <= blen; pos = (USHORT)(pos + 2 + body[pos + 1])) {
      if ((USHORT)(pos + 2 + body[pos + 1]) > blen)
         break;
      if (body[pos] == 3 && body[pos + 1] == 1)
         chan = body[pos + 2];
   }
   slot = 0xFFFF;
   for (k = 0; k < g_nnet; k++)
      if (mac_eq(bssid, g_net[k].bssid)) { slot = k; break; }
   if (slot == 0xFFFF) {
      if (g_nnet < NET_MAX)
         slot = g_nnet++;
      else {                                     /* table full: replace the weakest                         */
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
   n->sec = (UCHAR)((body[10] & 0x10) ? 1 : 0);  /* capability: privacy                                    */
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

/* HTT messages (target -> host): phase 2. Counted only. */
USHORT g_htt_msgs;

void htt_rx(UCHAR FAR *p, USHORT len)
{
   (void)p; (void)len;
   g_htt_msgs++;
}

/* Data transmit (phase 2: HTT TX). Without an association there is nowhere to send the frame. */
int sta_tx(UCHAR FAR *eth, USHORT len)
{
   (void)eth; (void)len;
   return -1;
}
