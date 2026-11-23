/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ath_wmi.c - WMI-TLV, the firmware's control interface (port of the parts of ath10k wmi-tlv.c / wmi.c
 * needed to start the firmware and scan).
 *
 * A WMI message is { le32 cmd_id (bits 23..0) } followed by TLVs { le16 len; le16 tag; value[len] }.
 * Numeric tag, command and event values were computed from the enums in ath10k wmi-tlv.h / wmi.h
 * (enum wmi_tlv_tag, WMI_TLV_CMD(grp)/WMI_TLV_EV(grp) = grp << 12 | 1, then sequential).
 */
#include "qca.h"

/* commands */
#define WMI_TLV_INIT_CMDID              0x00001UL
#define WMI_TLV_START_SCAN_CMDID        0x03001UL
#define WMI_TLV_STOP_SCAN_CMDID         0x03002UL
#define WMI_TLV_SCAN_CHAN_LIST_CMDID    0x03003UL
#define WMI_TLV_PDEV_SET_REGDOMAIN_CMDID 0x04001UL
#define WMI_TLV_VDEV_CREATE_CMDID       0x05001UL
/* events */
#define WMI_TLV_SERVICE_READY_EVENTID   0x00001UL
#define WMI_TLV_READY_EVENTID           0x00002UL
#define WMI_TLV_SCAN_EVENTID            0x03001UL
#define WMI_TLV_MGMT_RX_EVENTID         0x07001UL
#define WMI_TLV_VDEV_START_RESP_EVENTID 0x05001UL
#define WMI_TLV_VDEV_STOPPED_EVENTID    0x05002UL
#define WMI_TLV_PEER_STA_KICKOUT_EVENTID 0x06001UL
#define WMI_TLV_MGMT_TX_COMPL_EVENTID   0x07006UL
/* station commands */
#define WMI_TLV_VDEV_START_REQUEST_CMDID 0x05003UL
#define WMI_TLV_VDEV_UP_CMDID           0x05005UL
#define WMI_TLV_VDEV_STOP_CMDID         0x05006UL
#define WMI_TLV_VDEV_DOWN_CMDID         0x05007UL
#define WMI_TLV_VDEV_INSTALL_KEY_CMDID  0x05009UL
#define WMI_TLV_PEER_CREATE_CMDID       0x06001UL
#define WMI_TLV_PEER_DELETE_CMDID       0x06002UL
#define WMI_TLV_PEER_SET_PARAM_CMDID    0x06004UL
#define WMI_TLV_PEER_ASSOC_CMDID        0x06005UL
#define WMI_TLV_MGMT_TX_CMDID           0x07006UL
/* tags */
#define TAG_ARRAY_UINT32                0x10
#define TAG_ARRAY_BYTE                  0x11
#define TAG_ARRAY_STRUCT                0x12
#define TAG_ARRAY_FIXED_STRUCT          0x13
#define TAG_SERVICE_READY_EVENT         0x20
#define TAG_HAL_REG_CAPABILITIES        0x21
#define TAG_WLAN_HOST_MEM_REQ           0x22
#define TAG_READY_EVENT                 0x23
#define TAG_SCAN_EVENT                  0x24
#define TAG_MGMT_RX_HDR                 0x2C
#define TAG_INIT_CMD                    0x4A
#define TAG_RESOURCE_CONFIG             0x4B
#define TAG_WLAN_HOST_MEMORY_CHUNK      0x4C
#define TAG_START_SCAN_CMD              0x4D
#define TAG_SCAN_CHAN_LIST_CMD          0x4F
#define TAG_CHANNEL                     0x50
#define TAG_PDEV_SET_REGDOMAIN_CMD      0x51
#define TAG_VDEV_CREATE_CMD             0x56
#define TAG_VDEV_START_REQUEST_CMD      0x58
#define TAG_VDEV_UP_CMD                 0x5C
#define TAG_VDEV_STOP_CMD               0x5D
#define TAG_VDEV_DOWN_CMD               0x5E
#define TAG_VDEV_INSTALL_KEY_CMD        0x60
#define TAG_PEER_CREATE_CMD             0x61
#define TAG_PEER_DELETE_CMD             0x62
#define TAG_PEER_SET_PARAM_CMD          0x64
#define TAG_PEER_ASSOC_COMPLETE_CMD     0x65
#define TAG_VHT_RATE_SET                0x66
#define TAG_VDEV_START_RESPONSE_EVENT   0x28
#define TAG_VDEV_STOPPED_EVENT          0x29
#define TAG_MGMT_TX_CMD                 0x1A6
#define WMI_TLV_CIPHER_NONE             0
#define WMI_TLV_CIPHER_AES_CCM          4
#define WMI_KEY_PAIRWISE                0
#define WMI_KEY_GROUP                   1
#define WMI_PEER_AUTHORIZE              3
#define SVC_RX_FULL_REORDER             65     /* WMI_TLV_SERVICE_RX_FULL_REORDER                           */
#define SVC_MGMT_TX_WMI                 76     /* WMI_TLV_SERVICE_MGMT_TX_WMI                               */

#define WMI_SCAN_EVENT_STARTED          0x001UL
#define WMI_SCAN_EVENT_COMPLETED        0x002UL
#define WMI_SCAN_EVENT_BSS_CHANNEL      0x004UL
#define WMI_SCAN_EVENT_FOREIGN_CHANNEL  0x008UL
#define WMI_SCAN_EVENT_DEQUEUED         0x010UL
#define WMI_SCAN_EVENT_START_FAILED     0x040UL
#define WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT 0x100UL
#define WMI_SCAN_CHAN_STAT_EVENT        0x10UL
#define WMI_SCAN_FILTER_PROBE_REQ       0x20UL
#define WMI_CHAN_FLAG_PASSIVE           0x0080UL
#define WMI_CHAN_FLAG_DFS               0x0400UL
#define WMI_CHAN_FLAG_ALLOW_HT          0x0800UL
#define MODE_11A                        0
#define MODE_11G                        1
#define WMI_VDEV_TYPE_STA               2

#define NUM_UNITS_IS_NUM_VDEVS          0x1UL
#define NUM_UNITS_IS_NUM_PEERS          0x2UL
#define NUM_UNITS_IS_NUM_ACTIVE_PEERS   0x4UL
#define TLV_NUM_VDEVS                   4
#define TLV_NUM_PEERS                   33
#define MAX_MEM_REQS                    16

USHORT g_wmi_svc_ready, g_wmi_ready, g_scan_done, g_scan_running;
ULONG  g_fw_build, g_eeprom_rd;
USHORT g_wmi_events, g_mgmt_rx;
USHORT g_svc_full_reorder, g_svc_mgmt_tx_wmi;
USHORT g_vdev_start_done, g_vdev_start_status, g_vdev_stopped, g_peer_map, g_sec_ind;
UCHAR  g_peer_map_addr[6];
USHORT g_link_lost;
static UCHAR g_wmi_mac[6];

static struct { ULONG req_id, unit_size, info, units; } g_memreq[MAX_MEM_REQS];
static USHORT g_nmemreq;
static struct { ULONG req_id, phys, len; } g_chunk[MAX_MEM_REQS];
static USHORT g_nchunk_tgt;

/* ---- byte helpers ---------------------------------------------------------------------------------- */
static void p32(UCHAR FAR *p, ULONG v)
{
   U32 u;

   u.l = v;
   p[0] = u.b[0]; p[1] = u.b[1]; p[2] = u.b[2]; p[3] = u.b[3];
}

static ULONG g32(UCHAR FAR *p)
{
   U32 u;

   u.b[0] = p[0]; u.b[1] = p[1]; u.b[2] = p[2]; u.b[3] = p[3];
   return u.l;
}

static UCHAR FAR *tlv(UCHAR FAR *p, USHORT tag, USHORT len)
{
   p[0] = (UCHAR)len; p[1] = (UCHAR)(len >> 8);
   p[2] = (UCHAR)tag; p[3] = (UCHAR)(tag >> 8);
   return p + 4;
}

/* ---- sending -------------------------------------------------------------------------------------- */
static USHORT     g_wslot;
static UCHAR FAR *g_wp;

/* The command payload area (after the 4-byte WMI header); NULL when no slot is free. */
static UCHAR FAR *wmi_begin(void)
{
   static USHORT slot;
   static ULONG  ph;
   ULONG t0 = qc_ms();

   while ((g_wp = htc_tx_begin(g_wmi_ep, &slot, &ph)) == 0) {
      if ((long)(qc_ms() - t0) > 1000L)
         return 0;
      ath_service();
      qc_msleep(2);
   }
   g_wslot = slot;
   rt_setmem(g_wp, 0, 2040);
   return g_wp + 4;
}

static int wmi_send(ULONG cmd, USHORT len)
{
   ULONG t0 = qc_ms();
   int   rc;

   p32(g_wp, cmd & 0x00FFFFFFUL);
   while ((rc = htc_tx_end(g_wmi_ep, g_wslot, (USHORT)(len + 4))) != 0) {
      if ((long)(qc_ms() - t0) > 2000L) {
         g_ce[3].bfree[g_wslot] = 1;
         qc_err("WMI command not sent (no credits)", cmd);
         return rc;
      }
      ath_service();                               /* credit reports arrive with received messages      */
      qc_msleep(2);
   }
   return 0;
}

/* ---- events ----------------------------------------------------------------------------------------- */
static void ev_service_ready(UCHAR FAR *p, USHORT len)
{
   USHORT pos = 0, tl, tag, got_bmap = 0;

   while (pos + 4 <= len) {
      tl  = (USHORT)(p[pos] | (p[pos + 1] << 8));
      tag = (USHORT)(p[pos + 2] | (p[pos + 3] << 8));
      pos += 4;
      if ((USHORT)(pos + tl) > len)
         break;
      if (tag == TAG_SERVICE_READY_EVENT && tl >= 76)
         g_fw_build = g32(p + pos);
      else if (tag == TAG_HAL_REG_CAPABILITIES && tl >= 4)
         g_eeprom_rd = g32(p + pos);
      else if (tag == TAG_ARRAY_UINT32 && !got_bmap) {      /* the first u32 array is the service bitmap     */
         got_bmap = 1;
         if (tl >= 12) {                                    /* service 65 is in word 2, bit 1; 76 in bit 12   */
            ULONG w2 = g32(p + pos + 8);

            g_svc_full_reorder = (USHORT)((w2 >> 1) & 1);
            g_svc_mgmt_tx_wmi  = (USHORT)((w2 >> 12) & 1);
         }
      }
      else if (tag == TAG_ARRAY_STRUCT) {                   /* the struct array of memory requests            */
         USHORT q = pos, end = (USHORT)(pos + tl);
         USHORT it0 = (q + 4 <= end) ? (USHORT)(p[q + 2] | (p[q + 3] << 8)) : 0;

         if (it0 == TAG_WLAN_HOST_MEM_REQ) {
            g_nmemreq = 0;
            while (q + 4 + 16 <= end && g_nmemreq < MAX_MEM_REQS) {
               USHORT il = (USHORT)(p[q] | (p[q + 1] << 8));
               USHORT it = (USHORT)(p[q + 2] | (p[q + 3] << 8));

               if (it == TAG_WLAN_HOST_MEM_REQ && il >= 16) {
                  g_memreq[g_nmemreq].req_id    = g32(p + q + 4);
                  g_memreq[g_nmemreq].unit_size = g32(p + q + 8);
                  g_memreq[g_nmemreq].info      = g32(p + q + 12);
                  g_memreq[g_nmemreq].units     = g32(p + q + 16);
                  g_nmemreq++;
               }
               q = (USHORT)(q + 4 + il);
            }
         }
      }
      pos = (USHORT)(pos + tl);
   }
   g_wmi_svc_ready = 1;
}

static void ev_ready(UCHAR FAR *p, USHORT len)
{
   USHORT i;

   /* TLV { struct wmi_tlv_rdy_ev: abi (24 bytes), mac_addr (8), status } */
   if (len >= 4 + 36 && (p[2] | (p[3] << 8)) == TAG_READY_EVENT) {
      for (i = 0; i < 6; i++)
         g_wmi_mac[i] = p[4 + 24 + i];
      g_wmi_ready = 1;
   }
}

static void ev_scan(UCHAR FAR *p, USHORT len)
{
   ULONG type;

   if (len < 4 + 24 || (p[2] | (p[3] << 8)) != TAG_SCAN_EVENT)
      return;
   type = g32(p + 4);
   if (type & (WMI_SCAN_EVENT_COMPLETED | WMI_SCAN_EVENT_START_FAILED)) {
      g_scan_done = 1;
      g_scan_running = 0;
   }
}

static void ev_mgmt_rx(UCHAR FAR *p, USHORT len)
{
   USHORT pos, tl, tag;
   ULONG  chan = 0, snr = 0, status = 1, blen = 0;
   UCHAR FAR *frame = 0;
   USHORT flen = 0;

   g_mgmt_rx++;
   pos = 0;
   while (pos + 4 <= len) {
      tl  = (USHORT)(p[pos] | (p[pos + 1] << 8));
      tag = (USHORT)(p[pos + 2] | (p[pos + 3] << 8));
      pos += 4;
      if ((USHORT)(pos + tl) > len)
         break;
      if (tag == TAG_MGMT_RX_HDR && tl >= 24) {
         chan   = g32(p + pos);
         snr    = g32(p + pos + 4);
         blen   = g32(p + pos + 16);
         status = g32(p + pos + 20);
      } else if (tag == TAG_ARRAY_BYTE) {
         frame = p + pos;
         flen  = tl;
      }
      pos = (USHORT)(pos + tl);
   }
   if (!frame || status != 0UL)
      return;
   if (blen < (ULONG)flen)
      flen = (USHORT)blen;
   if (flen < 24)
      return;
   if (frame[0] == 0x80 || frame[0] == 0x50)                      /* beacon / probe response           */
      sta_beacon(frame, flen, (USHORT)chan, (short)((short)(USHORT)snr - 95));  /* ATH10K_DEFAULT_NOISE_FLOOR */
   else
      sta_mgmt_rx(frame, flen);                                   /* auth, assoc response, deauth, ... */
}

void wmi_rx(UCHAR FAR *p, USHORT len)
{
   ULONG id;

   if (len < 4)
      return;
   g_wmi_events++;
   id = g32(p) & 0x00FFFFFFUL;
   p += 4;
   len = (USHORT)(len - 4);
   if (id == WMI_TLV_SERVICE_READY_EVENTID)      ev_service_ready(p, len);
   else if (id == WMI_TLV_READY_EVENTID)         ev_ready(p, len);
   else if (id == WMI_TLV_SCAN_EVENTID)          ev_scan(p, len);
   else if (id == WMI_TLV_MGMT_RX_EVENTID)       ev_mgmt_rx(p, len);
   else if (id == WMI_TLV_VDEV_START_RESP_EVENTID) {
      if (len >= 4 + 16 && (p[2] | (p[3] << 8)) == TAG_VDEV_START_RESPONSE_EVENT) {
         g_vdev_start_status = (USHORT)g32(p + 4 + 12);        /* wmi_vdev_start_response_event.status      */
         g_vdev_start_done = 1;
      }
   }
   else if (id == WMI_TLV_VDEV_STOPPED_EVENTID)  g_vdev_stopped = 1;
   else if (id == WMI_TLV_PEER_STA_KICKOUT_EVENTID) g_link_lost = 1;   /* the firmware gave up on the AP    */
}

/* ---- host memory for the firmware (ath10k_wmi_event_service_ready_work, _alloc_host_mem) ---------- */
static ULONG mul_small(ULONG a, USHORT n)            /* a * n without a 32-bit multiply helper          */
{
   ULONG r = 0;

   while (n--) r += a;
   return r;
}

static void zero_phys(ULONG ph, ULONG len)
{
   while (len) {
      USHORT k = (len > 0x8000UL) ? 0x8000 : (USHORT)len;
      USHORT s = scr_map(SCR_B, ph, k);

      if (!s) return;
      rt_setmem(MAKEP(s, 0), 0, k);
      ph += k;
      len -= k;
   }
}

static int alloc_host_mem(void)
{
   USHORT i;

   g_nchunk_tgt = 0;
   for (i = 0; i < g_nmemreq; i++) {
      ULONG  units = g_memreq[i].units, len, ph;

      if (g_memreq[i].info & NUM_UNITS_IS_NUM_ACTIVE_PEERS)  units = TLV_NUM_PEERS + 1;
      else if (g_memreq[i].info & NUM_UNITS_IS_NUM_PEERS)    units = TLV_NUM_PEERS + 1;
      else if (g_memreq[i].info & NUM_UNITS_IS_NUM_VDEVS)    units = TLV_NUM_VDEVS + 1;
      if (units > 4096UL)
         return -1;
      for (;;) {
         len = mul_small(g_memreq[i].unit_size, (USHORT)units);
         ph = len ? dma_big(len) : 0;
         if (ph || units <= 1)
            break;
         units >>= 1;                                /* halve the pool until it fits (as ath10k does)   */
      }
      if (!ph) {
         qc_err("no memory for firmware host memory request", g_memreq[i].req_id);
         return -2;
      }
      zero_phys(ph, len);
      g_chunk[g_nchunk_tgt].req_id = g_memreq[i].req_id;
      g_chunk[g_nchunk_tgt].phys   = ph;
      g_chunk[g_nchunk_tgt].len    = len;
      g_nchunk_tgt++;
   }
   return 0;
}

/* ---- commands --------------------------------------------------------------------------------------- */
static int cmd_init(void)
{
   UCHAR FAR *p = wmi_begin(), FAR *q, FAR *c;
   USHORT i, chunks_len = (USHORT)(g_nchunk_tgt * 20);

   if (!p)
      return -1;
   q = tlv(p, TAG_INIT_CMD, 28);                     /* wmi_tlv_init_cmd: abi (6 words) + num chunks   */
   p32(q + 0,  0x01000000UL);                        /* WMI_TLV_ABI_VER0 (major 1, minor 0)            */
   p32(q + 4,  53UL);                                /* WMI_TLV_ABI_VER1                               */
   p32(q + 8,  0x5F414351UL);                        /* WMI_TLV_ABI_VER_NS0 "QCA_"                     */
   p32(q + 12, 0x00004C4DUL);                        /* WMI_TLV_ABI_VER_NS1 "ML"                       */
   p32(q + 16, 0UL);
   p32(q + 20, 0UL);
   p32(q + 24, (ULONG)g_nchunk_tgt);
   q += 28;
   c = tlv(q, TAG_RESOURCE_CONFIG, 44 * 4);          /* struct wmi_tlv_resource_config: 44 words       */
   p32(c + 0 * 4,  TLV_NUM_VDEVS);                   /* num_vdevs                                      */
   p32(c + 1 * 4,  TLV_NUM_PEERS);                   /* num_peers                                      */
   p32(c + 2 * 4,  g_svc_full_reorder ? (ULONG)TLV_NUM_VDEVS : 0UL);   /* num_offload_peers            */
   p32(c + 3 * 4,  g_svc_full_reorder ? (ULONG)TLV_NUM_VDEVS : 0UL);   /* num_offload_reorder_bufs     */
   p32(c + 4 * 4,  2UL);                             /* num_peer_keys                                   */
   p32(c + 5 * 4,  TLV_NUM_PEERS * 2);               /* num_tids                                        */
   p32(c + 6 * 4,  0x10UL);                          /* ast_skid_limit                                  */
   p32(c + 7 * 4,  0x7UL);                           /* tx_chain_mask                                   */
   p32(c + 8 * 4,  0x7UL);                           /* rx_chain_mask                                   */
   p32(c + 9 * 4,  0x64UL);                          /* rx_timeout_pri[0..3]                            */
   p32(c + 10 * 4, 0x64UL);
   p32(c + 11 * 4, 0x64UL);
   p32(c + 12 * 4, 0x28UL);
   p32(c + 13 * 4, 1UL);                             /* rx_decap_mode: ATH10K_HW_TXRX_NATIVE_WIFI       */
   p32(c + 14 * 4, 4UL);                             /* scan_max_pending_reqs                           */
   p32(c + 15 * 4, TLV_NUM_VDEVS);                   /* bmiss_offload_max_vdev                          */
   p32(c + 16 * 4, TLV_NUM_VDEVS);                   /* roam_offload_max_vdev                           */
   p32(c + 17 * 4, 8UL);                             /* roam_offload_max_ap_profiles                    */
   /* 18 num_mcast_groups 0, 19 num_mcast_table_elems 0, 20 mcast2ucast_mode 0, 21 tx_dbg_log_size 0 */
   p32(c + 22 * 4, 0x20UL);                          /* num_wds_entries                                 */
   /* 23 dma_burst_size 0, 24 mac_aggr_delim 0, 25 rx_skip_defrag... 0, 26 vow_config 0 */
   p32(c + 27 * 4, 2UL);                             /* gtk_offload_max_vdev                            */
   p32(c + 28 * 4, 1024UL + 32UL);                   /* num_msdu_desc = TARGET_TLV_NUM_MSDU_DESC        */
   p32(c + 29 * 4, 2UL);                             /* max_frag_entries                                */
   p32(c + 30 * 4, 1UL);                             /* num_tdls_vdevs                                  */
   p32(c + 31 * 4, 0x20UL);                          /* num_tdls_conn_table_entries                     */
   p32(c + 32 * 4, 2UL);                             /* beacon_tx_offload_max_vdev                      */
   p32(c + 33 * 4, 5UL);                             /* num_multicast_filter_entries                    */
   p32(c + 34 * 4, 22UL);                            /* num_wow_filters = TARGET_TLV_NUM_WOW_PATTERNS   */
   p32(c + 35 * 4, 6UL);                             /* num_keep_alive_pattern                          */
   /* 36 keep_alive_pattern_size 0 */
   p32(c + 37 * 4, 1UL);                             /* max_tdls_concurrent_sleep_sta                   */
   p32(c + 38 * 4, 1UL);                             /* max_tdls_concurrent_buffer_sta                  */
   /* 39 wmi_send_separate 0, 40..42 ocb 0 */
   p32(c + 43 * 4, 0x200UL);                         /* host_capab = WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL   */
   q = c + 44 * 4;
   q = tlv(q, TAG_ARRAY_STRUCT, chunks_len);
   for (i = 0; i < g_nchunk_tgt; i++) {
      UCHAR FAR *k = tlv(q, TAG_WLAN_HOST_MEMORY_CHUNK, 16);

      p32(k + 0, g_chunk[i].req_id);
      p32(k + 4, g_chunk[i].phys);
      p32(k + 8, g_chunk[i].len);
      p32(k + 12, 0UL);                              /* ptr_high                                        */
      q = k + 16;
   }
   return wmi_send(WMI_TLV_INIT_CMDID, (USHORT)(q - p));
}

static int cmd_set_regdomain(void)
{
   UCHAR FAR *p = wmi_begin(), FAR *q;
   ULONG rd = g_eeprom_rd & 0xFFFFUL;

   if (!p)
      return -1;
   q = tlv(p, TAG_PDEV_SET_REGDOMAIN_CMD, 24);
   p32(q + 0, 0UL);                                   /* pdev_id                                         */
   p32(q + 4, rd);                                    /* regd                                            */
   p32(q + 8, rd);                                    /* regd_2ghz                                       */
   p32(q + 12, rd);                                   /* regd_5ghz                                       */
   p32(q + 16, 0x10UL);                               /* conform_limit_2ghz: CTL_FCC (see ARCHITECTURE)  */
   p32(q + 20, 0x10UL);                               /* conform_limit_5ghz                              */
   return wmi_send(WMI_TLV_PDEV_SET_REGDOMAIN_CMDID, 28);
}

/* The channels the driver scans: 2.4 GHz 1-13 active; 5 GHz UNII-1 and UNII-3 active, DFS passive. */
static UCHAR g_chans[] = {
   1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
   36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
   149, 153, 157, 161, 165
};
#define NCHANS (sizeof(g_chans))

static USHORT ch_mhz(UCHAR c)
{
   if (c == 14) return 2484;
   if (c <= 13) return (USHORT)(2407 + 5 * c);
   return (USHORT)(5000 + 5 * c);
}

static int ch_dfs(UCHAR c) { return c >= 52 && c <= 140; }

static int cmd_scan_chan_list(void)
{
   UCHAR FAR *p = wmi_begin(), FAR *q;
   USHORT i;

   if (!p)
      return -1;
   q = tlv(p, TAG_SCAN_CHAN_LIST_CMD, 4);
   p32(q, (ULONG)NCHANS);
   q += 4;
   q = tlv(q, TAG_ARRAY_STRUCT, (USHORT)(NCHANS * (4 + 24)));
   for (i = 0; i < NCHANS; i++) {
      UCHAR  c = g_chans[i];
      USHORT mhz = ch_mhz(c);
      ULONG  fl = (c <= 14) ? MODE_11G : MODE_11A;
      UCHAR FAR *w = tlv(q, TAG_CHANNEL, 24);         /* struct wmi_channel                              */

      fl |= WMI_CHAN_FLAG_ALLOW_HT;
      if (ch_dfs(c))
         fl |= WMI_CHAN_FLAG_PASSIVE | WMI_CHAN_FLAG_DFS;
      p32(w + 0, (ULONG)mhz);                         /* mhz                                             */
      p32(w + 4, (ULONG)mhz);                         /* band_center_freq1 (20 MHz)                       */
      p32(w + 8, 0UL);                                /* band_center_freq2                                */
      p32(w + 12, fl);                                /* mode (6 LSBs) | flags                            */
      w[16] = 0;                                      /* min_power                                       */
      w[17] = 40;                                     /* max_power   (0.5 dBm units: 20 dBm)             */
      w[18] = 40;                                     /* reg_power                                       */
      w[19] = 0;                                      /* reg_classid                                     */
      w[20] = 0;                                      /* antenna_max                                     */
      w[21] = 40;                                     /* max_tx_power                                    */
      q = w + 24;
   }
   return wmi_send(WMI_TLV_SCAN_CHAN_LIST_CMDID, (USHORT)(q - p));
}

static int cmd_vdev_create(void)
{
   UCHAR FAR *p = wmi_begin(), FAR *q;
   USHORT i;

   if (!p)
      return -1;
   q = tlv(p, TAG_VDEV_CREATE_CMD, 20);
   p32(q + 0, 0UL);                                   /* vdev_id                                         */
   p32(q + 4, WMI_VDEV_TYPE_STA);
   p32(q + 8, 0UL);                                   /* subtype                                         */
   for (i = 0; i < 6; i++)
      q[12 + i] = g_mymac[i];
   return wmi_send(WMI_TLV_VDEV_CREATE_CMDID, 24);
}

/* ---- start-up after HTC is connected ------------------------------------------------------------- */
int wmi_init_target(void)
{
   USHORT i;
   int    rc;

   if (ath_wait(&g_wmi_svc_ready, 5000)) { qc_err("no WMI service ready event", 0UL); return -1; }
   rc = alloc_host_mem();
   if (rc) return -2;
   g_wmi_ready = 0;
   if (cmd_init()) return -3;
   if (ath_wait(&g_wmi_ready, 5000)) { qc_err("no WMI ready event", 0UL); return -4; }
   for (i = 0; i < 6; i++)
      g_mymac[i] = g_wmi_mac[i];
   ndis_set_mac(g_mymac);
   if (cmd_set_regdomain()) return -5;
   if (cmd_scan_chan_list()) return -6;
   if (cmd_vdev_create()) return -7;
   return 0;
}

int wmi_scan(void)
{
   UCHAR FAR *p, FAR *q, FAR *c;
   USHORT i;

   p = wmi_begin();
   if (!p)
      return -1;
   c = tlv(p, TAG_START_SCAN_CMD, 100);              /* struct wmi_tlv_start_scan_cmd                  */
   p32(c + 0,  0xA000UL);                            /* scan_id = WMI_HOST_SCAN_REQ_ID_PREFIX | 0      */
   p32(c + 4,  0xA001UL);                            /* scan_req_id = REQUESTOR_ID_PREFIX | 1          */
   p32(c + 8,  0UL);                                 /* vdev_id                                         */
   p32(c + 12, 1UL);                                 /* scan_priority LOW                               */
   p32(c + 16, WMI_SCAN_EVENT_STARTED | WMI_SCAN_EVENT_COMPLETED | WMI_SCAN_EVENT_BSS_CHANNEL |
               WMI_SCAN_EVENT_FOREIGN_CHANNEL | WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT | WMI_SCAN_EVENT_DEQUEUED);
   p32(c + 20, 50UL);                                /* dwell_time_active                               */
   p32(c + 24, 150UL);                               /* dwell_time_passive                              */
   p32(c + 28, 50UL);                                /* min_rest_time                                   */
   p32(c + 32, 500UL);                               /* max_rest_time                                   */
   /* 36 repeat_probe_time 0, 40 probe_spacing_time 0, 44 idle_time 0 */
   p32(c + 48, 20000UL);                             /* max_scan_time                                   */
   p32(c + 52, 5UL);                                 /* probe_delay                                     */
   p32(c + 56, WMI_SCAN_CHAN_STAT_EVENT ^ WMI_SCAN_FILTER_PROBE_REQ);   /* WMI-TLV inverts FILTER_PROBE_REQ */
   /* 60 burst_duration_ms 0 */
   p32(c + 64, (ULONG)NCHANS);                       /* num_channels                                    */
   p32(c + 68, 1UL);                                 /* num_bssids                                      */
   p32(c + 72, 1UL);                                 /* num_ssids (one wildcard)                        */
   p32(c + 76, 0UL);                                 /* ie_len                                          */
   p32(c + 80, 3UL);                                 /* num_probes                                      */
   /* 84 mac_addr, 92 mac_mask: zero */
   q = tlv(c + 100, TAG_ARRAY_UINT32, (USHORT)(NCHANS * 4));
   for (i = 0; i < NCHANS; i++)
      p32(q + i * 4, (ULONG)ch_mhz(g_chans[i]));
   q += NCHANS * 4;
   q = tlv(q, TAG_ARRAY_FIXED_STRUCT, 36);           /* struct wmi_ssid: len 0 = wildcard               */
   q += 36;
   q = tlv(q, TAG_ARRAY_FIXED_STRUCT, 8);            /* struct wmi_mac_addr: broadcast BSSID            */
   for (i = 0; i < 6; i++)
      q[i] = 0xFF;
   q += 8;
   q = tlv(q, TAG_ARRAY_BYTE, 0);
   g_scan_done = 0;
   g_scan_running = 1;
   if (wmi_send(WMI_TLV_START_SCAN_CMDID, (USHORT)(q - p))) {
      g_scan_running = 0;
      return -2;
   }
   if (ath_wait(&g_scan_done, 15000)) {
      g_scan_running = 0;
      qc_err("scan did not complete", 0UL);
      return -3;
   }
   return 0;
}

/* =====================================================================================================
 * Station commands (phase 2). Formats from ath10k wmi-tlv.c (ath10k_wmi_tlv_op_gen_vdev_start, _vdev_up,
 * _vdev_stop, _vdev_down, _peer_create, _peer_delete, _peer_set_param, _vdev_install_key, _peer_assoc,
 * _mgmt_tx_send); the numeric tags and command ids were computed from the headers (see PROVENANCE.md).
 * vdev 0 is the only vdev (created in wmi_init_target).
 * ===================================================================================================== */

static int put_channel(UCHAR FAR *w, UCHAR c)
{
   USHORT mhz = ch_mhz(c);
   ULONG  fl = (c <= 14) ? MODE_11G : MODE_11A;

   p32(w + 0, (ULONG)mhz);
   p32(w + 4, (ULONG)mhz);
   p32(w + 8, 0UL);
   p32(w + 12, fl);                                   /* legacy (non-HT) 20 MHz mode                       */
   w[16] = 0;  w[17] = 40;  w[18] = 40;  w[19] = 0;
   w[20] = 0;  w[21] = 40;
   return 0;
}

static void put_mac(UCHAR FAR *d, UCHAR FAR *a)
{
   USHORT i;

   for (i = 0; i < 6; i++)
      d[i] = a[i];
}

int wmi_vdev_start(UCHAR ch, USHORT bcn, USHORT dtim)
{
   UCHAR FAR *p = wmi_begin(), FAR *c, FAR *q;

   if (!p)
      return -1;
   c = tlv(p, TAG_VDEV_START_REQUEST_CMD, 72);        /* struct wmi_tlv_vdev_start_cmd                      */
   p32(c + 0, 0UL);                                   /* vdev_id                                            */
   p32(c + 8, (ULONG)bcn);                            /* bcn_intval                                         */
   p32(c + 12, (ULONG)dtim);                          /* dtim_period                                        */
   /* flags, ssid (len 0), bcn_tx_rate, bcn_tx_power, num_noa_descr, disable_hw_ack: all zero              */
   q = tlv(c + 72, TAG_CHANNEL, 24);
   put_channel(q, ch);
   q = tlv(q + 24, TAG_ARRAY_STRUCT, 0);
   g_vdev_start_done = 0;
   return wmi_send(WMI_TLV_VDEV_START_REQUEST_CMDID, (USHORT)(q - p));
}

int wmi_vdev_stop(void)
{
   UCHAR FAR *p = wmi_begin(), FAR *c;

   if (!p)
      return -1;
   c = tlv(p, TAG_VDEV_STOP_CMD, 4);
   p32(c, 0UL);
   g_vdev_stopped = 0;
   return wmi_send(WMI_TLV_VDEV_STOP_CMDID, 8);
}

int wmi_vdev_down(void)
{
   UCHAR FAR *p = wmi_begin(), FAR *c;

   if (!p)
      return -1;
   c = tlv(p, TAG_VDEV_DOWN_CMD, 4);
   p32(c, 0UL);
   return wmi_send(WMI_TLV_VDEV_DOWN_CMDID, 8);
}

int wmi_vdev_up(USHORT aid, UCHAR FAR *bssid)
{
   UCHAR FAR *p = wmi_begin(), FAR *c;

   if (!p)
      return -1;
   c = tlv(p, TAG_VDEV_UP_CMD, 16);
   p32(c + 0, 0UL);
   p32(c + 4, (ULONG)aid);
   put_mac(c + 8, bssid);
   return wmi_send(WMI_TLV_VDEV_UP_CMDID, 20);
}

int wmi_peer_create(UCHAR FAR *addr)
{
   UCHAR FAR *p = wmi_begin(), FAR *c;

   if (!p)
      return -1;
   c = tlv(p, TAG_PEER_CREATE_CMD, 16);
   p32(c + 0, 0UL);                                   /* vdev_id                                            */
   put_mac(c + 4, addr);
   p32(c + 12, 0UL);                                  /* WMI_PEER_TYPE_DEFAULT                              */
   g_peer_map = 0;
   return wmi_send(WMI_TLV_PEER_CREATE_CMDID, 20);
}

int wmi_peer_delete(UCHAR FAR *addr)
{
   UCHAR FAR *p = wmi_begin(), FAR *c;

   if (!p)
      return -1;
   c = tlv(p, TAG_PEER_DELETE_CMD, 12);
   p32(c + 0, 0UL);
   put_mac(c + 4, addr);
   return wmi_send(WMI_TLV_PEER_DELETE_CMDID, 16);
}

int wmi_peer_authorize(UCHAR FAR *addr)
{
   UCHAR FAR *p = wmi_begin(), FAR *c;

   if (!p)
      return -1;
   c = tlv(p, TAG_PEER_SET_PARAM_CMD, 20);
   p32(c + 0, 0UL);
   put_mac(c + 4, addr);
   p32(c + 12, (ULONG)WMI_PEER_AUTHORIZE);
   p32(c + 16, 1UL);
   return wmi_send(WMI_TLV_PEER_SET_PARAM_CMDID, 24);
}

/* rates: 802.11 supported-rate octets (500 kbit/s units, bit 7 = basic); nrates <= 12 */
int wmi_peer_assoc(UCHAR FAR *addr, USHORT aid, USHORT caps, USHORT listen, ULONG flags,
                   UCHAR FAR *rates, USHORT nrates, USHORT phymode)
{
   UCHAR FAR *p = wmi_begin(), FAR *c, FAR *q;
   USHORT i, rl;

   if (!p)
      return -1;
   if (nrates > 12)
      nrates = 12;
   rl = (USHORT)((nrates + 3) & ~3);
   c = tlv(p, TAG_PEER_ASSOC_COMPLETE_CMD, 76);       /* struct wmi_tlv_peer_assoc_cmd                      */
   put_mac(c + 0, addr);
   p32(c + 8, 0UL);                                   /* vdev_id                                            */
   p32(c + 12, 1UL);                                  /* new_assoc                                          */
   p32(c + 16, (ULONG)aid);
   p32(c + 20, flags);
   p32(c + 24, (ULONG)caps);
   p32(c + 28, (ULONG)listen);
   /* ht_caps, max_mpdu, mpdu_density, rate_caps: zero (no HT) */
   p32(c + 48, 1UL);                                  /* nss                                                */
   /* vht_caps zero */
   p32(c + 56, (ULONG)phymode);
   p32(c + 68, (ULONG)nrates);                        /* num_legacy_rates                                   */
   p32(c + 72, 0UL);                                  /* num_ht_rates                                       */
   q = tlv(c + 76, TAG_ARRAY_BYTE, rl);               /* legacy rates, in ath10k rate codes                 */
   for (i = 0; i < nrates; i++) {
      UCHAR v = (UCHAR)(rates[i] & 0x7F);

      q[i] = (UCHAR)((v & 1) ? (v | 0x80) : v);       /* 5.5 Mbit/s (11) -> 0x8B, the others unchanged     */
   }
   q = tlv(q + rl, TAG_ARRAY_BYTE, 0);                /* HT rates: none                                     */
   q = tlv(q, TAG_VHT_RATE_SET, 16);                  /* VHT rate set: zeros                                */
   q += 16;
   return wmi_send(WMI_TLV_PEER_ASSOC_CMDID, (USHORT)(q - p));
}

/* key == NULL: delete (cipher none). CCMP: 16-byte key, no separate MIC. */
int wmi_install_key(UCHAR FAR *addr, UCHAR idx, USHORT group, UCHAR FAR *key, USHORT klen)
{
   UCHAR FAR *p = wmi_begin(), FAR *c, FAR *q;
   USHORT i, kl4 = (USHORT)((klen + 3) & ~3);

   if (!p)
      return -1;
   c = tlv(p, TAG_VDEV_INSTALL_KEY_CMD, 92);
   p32(c + 0, 0UL);                                   /* vdev_id                                            */
   put_mac(c + 4, addr);
   p32(c + 12, (ULONG)idx);
   p32(c + 16, group ? (ULONG)WMI_KEY_GROUP : (ULONG)WMI_KEY_PAIRWISE);
   p32(c + 20, key ? (ULONG)WMI_TLV_CIPHER_AES_CCM : (ULONG)WMI_TLV_CIPHER_NONE);
   /* rsc / global rsc / tsc / wpi counters: zero */
   p32(c + 80, key ? (ULONG)klen : 0UL);
   /* txmic / rxmic lengths zero for CCMP */
   q = tlv(c + 92, TAG_ARRAY_BYTE, key ? kl4 : 0);
   if (key) {
      for (i = 0; i < klen; i++)
         q[i] = key[i];
      q += kl4;
   }
   return wmi_send(WMI_TLV_VDEV_INSTALL_KEY_CMDID, (USHORT)(q - p));
}

/* Management frame through WMI (the firmware advertises WMI_SERVICE_MGMT_TX_WMI): the frame goes both inline and by
 * DMA address, as ath10k does. */
static DMA    g_mgmt_buf;
static ULONG  g_mgmt_desc;

int wmi_mgmt_tx(UCHAR FAR *frame, USHORT len)
{
   UCHAR FAR *p, FAR *c, FAR *q;
   USHORT bl = (USHORT)((len + 3) & ~3);

   if (len > 400)
      return -3;
   /* ath10k_mac_tx_h_get_txpath: WMI if the firmware has the feature "has-wmi-mgmt-tx" (bit 2) or the service
    * WMI_SERVICE_MGMT_TX_WMI, else the HTT data path (HTT >= 3) with the management packet type. */
   if (!g_svc_mgmt_tx_wmi && !fw_feature(2))
      return htt_tx_mgmt(frame, len);
   if (!g_mgmt_buf.phys && dma_get(512, &g_mgmt_buf))
      return -4;
   rt_movmem(DPTR(g_mgmt_buf), frame, len);
   p = wmi_begin();
   if (!p)
      return -1;
   c = tlv(p, TAG_MGMT_TX_CMD, 28);
   p32(c + 0, 0UL);                                   /* vdev_id                                            */
   p32(c + 4, ++g_mgmt_desc);                         /* desc_id                                            */
   p32(c + 8, 0UL);                                   /* chanfreq: current channel                          */
   p32(c + 12, g_mgmt_buf.phys);                      /* paddr (64 bit)                                     */
   p32(c + 16, 0UL);
   p32(c + 20, (ULONG)len);                           /* frame_len                                          */
   p32(c + 24, (ULONG)bl);                            /* buf_len                                            */
   q = tlv(c + 28, TAG_ARRAY_BYTE, bl);
   rt_movmem(q, frame, len);
   q += bl;
   return wmi_send(WMI_TLV_MGMT_TX_CMDID, (USHORT)(q - p));
}
