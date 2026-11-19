/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qc_fw.c - the ath10k firmware and board file containers.
 *
 * Formats from ath10k core.c (ath10k_core_fetch_firmware_api_n, ath10k_core_fetch_board_data_api_n,
 * ath10k_core_search_bd, ath10k_core_parse_bd_ie_board) and hw.h (ATH10K_FW_IE_*, ATH10K_BD_IE_*):
 *
 *   firmware-N.bin: "QCA-ATH10K\0" padded to 4, then elements { le32 id; le32 len; data[len] pad to 4 }
 *   board-2.bin:    "QCA-ATH10K-BOARD\0" padded to 4, then elements of type BOARD (0) / BOARD_EXT (1), each
 *                   a list of sub-elements NAME (0) and DATA (1); the DATA following a NAME equal to the
 *                   board name is the board data.
 */
#include "qca.h"

#define FW_IE_FW_VERSION        0
#define FW_IE_TIMESTAMP         1
#define FW_IE_FEATURES          2
#define FW_IE_FW_IMAGE          3
#define FW_IE_OTP_IMAGE         4
#define FW_IE_WMI_OP_VERSION    5
#define FW_IE_HTT_OP_VERSION    6
#define FW_IE_FW_CODE_SWAP      7

#define BD_IE_BOARD             0
#define BD_IE_BOARD_EXT         1
#define BD_IE_BOARD_NAME        0
#define BD_IE_BOARD_DATA        1

FWINFO g_fw;

static UCHAR g_tmp[128];

static int magic_ok(BLOB *b, const char *m, USHORT n)
{
   USHORT i;

   if (b->len < n)
      return 0;
   if (blob_read(b, 0UL, (UCHAR FAR *)g_tmp, n))
      return 0;
   for (i = 0; i < n; i++)
      if (g_tmp[i] != (UCHAR)m[i])
         return 0;
   return 1;
}

#define ALIGN4(x) (((x) + 3UL) & ~3UL)

int fw_feature(USHORT bit)
{
   if (bit >= 128)
      return 0;
   return (g_fw.features[bit >> 3] >> (bit & 7)) & 1;
}

int fw_parse(void)
{
   static const char magic[] = "QCA-ATH10K";         /* sizeof includes the terminating zero (11)          */
   ULONG pos, id, len;
   USHORT k;

   rt_setmem((UCHAR FAR *)&g_fw, 0, sizeof(g_fw));
   if (!g_fwblob.done || !magic_ok(&g_fwblob, magic, sizeof(magic)))
      return -1;
   pos = ALIGN4((ULONG)sizeof(magic));
   while (pos + 8 <= g_fwblob.len) {
      id  = blob_rd32(&g_fwblob, pos);
      len = blob_rd32(&g_fwblob, pos + 4);
      pos += 8;
      if (len > g_fwblob.len - pos)
         return -2;                                    /* truncated element                                  */
      switch ((USHORT)id) {
      case FW_IE_FW_VERSION:
         k = (len < sizeof(g_fw.version) - 1) ? (USHORT)len : (USHORT)(sizeof(g_fw.version) - 1);
         blob_read(&g_fwblob, pos, (UCHAR FAR *)g_fw.version, k);
         g_fw.version[k] = 0;
         break;
      case FW_IE_FEATURES:
         k = (len < sizeof(g_fw.features)) ? (USHORT)len : (USHORT)sizeof(g_fw.features);
         blob_read(&g_fwblob, pos, (UCHAR FAR *)g_fw.features, k);
         break;
      case FW_IE_FW_IMAGE:
         g_fw.fw_off = pos;  g_fw.fw_len = len;
         break;
      case FW_IE_OTP_IMAGE:
         g_fw.otp_off = pos; g_fw.otp_len = len;
         break;
      case FW_IE_WMI_OP_VERSION:
         if (len == 4) g_fw.wmi_op = blob_rd32(&g_fwblob, pos);
         break;
      case FW_IE_HTT_OP_VERSION:
         if (len == 4) g_fw.htt_op = blob_rd32(&g_fwblob, pos);
         break;
      case FW_IE_FW_CODE_SWAP:
         g_fw.swap_off = pos; g_fw.swap_len = len;
         break;
      default:
         break;                                        /* timestamp and unknown elements are skipped        */
      }
      pos += ALIGN4(len);
   }
   if (!g_fw.fw_len)
      return -3;
   return 0;
}

/* Search one BOARD element (sub-elements from pos, len bytes) for name; 0 = found (g_fw.bd_*). */
static int parse_board(ULONG pos, ULONG len, char *name, USHORT nlen)
{
   ULONG end = pos + len, id, l;
   int   match = 0;
   USHORT i;

   while (pos + 8 <= end) {
      id = blob_rd32(&g_bdblob, pos);
      l  = blob_rd32(&g_bdblob, pos + 4);
      pos += 8;
      if (l > end - pos)
         return -2;
      if (id == BD_IE_BOARD_NAME) {
         match = 0;
         if (l == nlen && l <= sizeof(g_tmp)) {
            blob_read(&g_bdblob, pos, (UCHAR FAR *)g_tmp, (USHORT)l);
            match = 1;
            for (i = 0; i < nlen; i++)
               if (g_tmp[i] != (UCHAR)name[i])
                  match = 0;
         }
      } else if (id == BD_IE_BOARD_DATA && match) {
         g_fw.bd_off = pos;
         g_fw.bd_len = l;
         return 0;
      }
      pos += ALIGN4(l);
   }
   return -1;
}

int fw_find_board(char *name)
{
   static const char magic[] = "QCA-ATH10K-BOARD";
   ULONG  pos, id, len;
   USHORT nlen;

   for (nlen = 0; name[nlen]; nlen++)
      ;
   g_fw.bd_off = g_fw.bd_len = 0;
   if (!g_bdblob.done || !magic_ok(&g_bdblob, magic, sizeof(magic)))
      return -1;
   pos = ALIGN4((ULONG)sizeof(magic));
   while (pos + 8 <= g_bdblob.len) {
      id  = blob_rd32(&g_bdblob, pos);
      len = blob_rd32(&g_bdblob, pos + 4);
      pos += 8;
      if (len > g_bdblob.len - pos)
         return -2;
      if (id == BD_IE_BOARD && parse_board(pos, len, name, nlen) == 0)
         return 0;
      pos += ALIGN4(len);
   }
   return -1;
}
