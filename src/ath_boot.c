/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ath_boot.c - QCA6174 reset, target configuration, BMI firmware download and the bring-up sequence.
 *
 * Port of ath10k pci.c (ath10k_pci_qca6174_chip_reset, _cold_reset, _warm_reset, _wait_for_target_init,
 * _init_config, _wake_target_cpu, _hif_exchange_bmi_msg), bmi.c and the BMI half of core.c
 * (ath10k_core_start up to ath10k_bmi_done: init_configure_target, download_and_run_otp, download_fw,
 * init_uart). The PLL setup and the diagnostic-window firmware download that ath10k uses on hw3.2 are
 * speed-ups with a BMI fallback in Linux; this driver always uses BMI (slower, the same result).
 */
#include "qca.h"

USHORT g_hw_ready, g_hw_failed, g_hw_state;
ULONG  g_chip_id, g_target_ver;
USHORT g_num_rf;
UCHAR  g_mymac[6];
int    g_bmi_rc;

#define BMI_DONE            1
#define BMI_READ_MEMORY     2
#define BMI_WRITE_MEMORY    3
#define BMI_EXECUTE         4
#define BMI_READ_SOC_REG    6
#define BMI_WRITE_SOC_REG   7
#define BMI_GET_TARGET_INFO 8
#define BMI_LZ_STREAM_START 13
#define BMI_LZ_DATA         14
#define BMI_MAX_DATA        256
#define BMI_PARAM_GET_EEPROM_BOARD_ID 0x10UL

#define BMI_TX_CE   0
#define BMI_RX_CE   1

static DMA    g_bmi_req, g_bmi_resp;
static USHORT g_bmi_done_sent;
static char   g_bdname[100];
static UCHAR  g_otp_board, g_otp_chip, g_otp_valid, g_ext_bid;

/* ---- little-endian composition helpers --------------------------------------------------------- */
static void put32(UCHAR FAR *p, ULONG v)
{
   U32 u;

   u.l = v;
   p[0] = u.b[0]; p[1] = u.b[1]; p[2] = u.b[2]; p[3] = u.b[3];
}

static ULONG get32(UCHAR FAR *p)
{
   U32 u;

   u.b[0] = p[0]; u.b[1] = p[1]; u.b[2] = p[2]; u.b[3] = p[3];
   return u.l;
}

/* ---- BMI message exchange (ath10k_pci_hif_exchange_bmi_msg + _bmi_wait, 3 s timeout) ------------ */
static int bmi_xchg(USHORT req_len, USHORT *resp_len)
{
   ULONG  t0;
   USHORT tx_done = 0, rx_done = 0;
   static USHORT nb;
   int    want = (resp_len != 0);

   if (want && ce_rx_post(BMI_RX_CE, g_bmi_resp.phys) < 0)
      return -1;
   if (ce_send(BMI_TX_CE, g_bmi_req.phys, req_len, 0x3FFF, 0) < 0) {
      if (want) ce_rx_revoke(BMI_RX_CE);
      return -2;
   }
   nb = 0;
   t0 = qc_ms();
   for (;;) {
      if (!tx_done && ce_send_done(BMI_TX_CE))
         tx_done = 1;
      if (want && !rx_done && ce_rx_next(BMI_RX_CE, &nb))
         rx_done = 1;
      if (tx_done && (!want || rx_done))
         break;
      if ((long)(qc_ms() - t0) > 3000L) {
         if (want) ce_rx_revoke(BMI_RX_CE);
         return -3;
      }
      qc_udelay(20);
   }
   if (want) {
      if (nb < *resp_len)
         *resp_len = nb;
   }
   return 0;
}

static int bmi_cmd2(ULONG id, ULONG a, ULONG b, USHORT n, USHORT rlen)
{
   UCHAR FAR *q = DPTR(g_bmi_req);
   static USHORT rl;

   put32(q, id);
   put32(q + 4, a);
   put32(q + 8, b);
   rl = rlen;
   return bmi_xchg(n, rlen ? &rl : 0);
}

static int bmi_read_memory(ULONG addr, UCHAR FAR *dst, USHORT len)
{
   USHORT k;
   int    rc;

   while (len) {
      k = (len > BMI_MAX_DATA) ? BMI_MAX_DATA : len;
      rc = bmi_cmd2(BMI_READ_MEMORY, addr, (ULONG)k, 12, k);
      if (rc)
         return rc;
      rt_movmem(dst, DPTR(g_bmi_resp), k);
      addr += k;
      dst += k;
      len = (USHORT)(len - k);
   }
   return 0;
}

/* Source is a far pointer (DGROUP or DMA memory). */
static int bmi_write_memory(ULONG addr, UCHAR FAR *src, USHORT len)
{
   UCHAR FAR *q = DPTR(g_bmi_req);
   USHORT k, kr;
   int    rc;

   while (len) {
      k = (len > BMI_MAX_DATA - 12) ? (USHORT)(BMI_MAX_DATA - 12) : len;
      rt_setmem(q + 12, 0, (USHORT)((k + 3) & ~3));
      rt_movmem(q + 12, src, k);
      kr = (USHORT)((k + 3) & ~3);
      put32(q, BMI_WRITE_MEMORY);
      put32(q + 4, addr);
      put32(q + 8, (ULONG)kr);
      rc = bmi_xchg((USHORT)(12 + kr), 0);
      if (rc)
         return rc;
      addr += k;
      src += k;
      len = (USHORT)(len - k);
   }
   return 0;
}

ULONG bmi_rd32(USHORT item)
{
   static ULONG v;

   v = 0;
   g_bmi_rc = bmi_read_memory(HOST_INTEREST + item, (UCHAR FAR *)&v, 4);
   return v;
}

int bmi_wr32(USHORT item, ULONG val)
{
   static ULONG v;

   v = val;
   return bmi_write_memory(HOST_INTEREST + item, (UCHAR FAR *)&v, 4);
}

static int bmi_execute(ULONG addr, ULONG param, ULONG *result)
{
   static USHORT rl;
   UCHAR FAR *q = DPTR(g_bmi_req);
   int rc;

   put32(q, BMI_EXECUTE);
   put32(q + 4, addr);
   put32(q + 8, param);
   rl = 4;
   rc = bmi_xchg(12, &rl);
   if (rc)
      return rc;
   if (rl < 4)
      return -10;
   *result = get32(DPTR(g_bmi_resp));
   return 0;
}

static int bmi_lz_start(ULONG addr)
{
   UCHAR FAR *q = DPTR(g_bmi_req);

   put32(q, BMI_LZ_STREAM_START);
   put32(q + 4, addr);
   return bmi_xchg(8, 0);
}

/* LZ data straight out of a blob (len a multiple of 4, except a zero-padded tail word). */
static int bmi_lz_blob(BLOB *b, ULONG off, ULONG len)
{
   UCHAR FAR *q = DPTR(g_bmi_req);
   USHORT k;
   int    rc;

   while (len) {
      k = (len > (ULONG)(BMI_MAX_DATA - 8)) ? (USHORT)(BMI_MAX_DATA - 8) : (USHORT)len;
      rt_setmem(q + 8, 0, 4);
      if (blob_read(b, off, q + 8, k))
         return -20;
      put32(q, BMI_LZ_DATA);
      put32(q + 4, (ULONG)((k + 3) & ~3));
      rc = bmi_xchg((USHORT)(8 + ((k + 3) & ~3)), 0);
      if (rc)
         return rc;
      off += k;
      len -= k;
   }
   return 0;
}

/* ath10k_bmi_fast_download: LZ stream to addr, then a fake stream at 0 to flush the target caches. */
static int bmi_fast_download(ULONG addr, BLOB *b, ULONG off, ULONG len)
{
   int rc;

   rc = bmi_lz_start(addr);
   if (!rc) rc = bmi_lz_blob(b, off, len);
   if (!rc) rc = bmi_lz_start(0UL);
   return rc;
}

static int bmi_get_target_info(void)
{
   static USHORT rl;
   UCHAR FAR *q = DPTR(g_bmi_req);
   int rc;

   put32(q, BMI_GET_TARGET_INFO);
   rl = 12;
   rc = bmi_xchg(4, &rl);
   if (rc)
      return rc;
   if (rl < 12)
      return -10;
   g_target_ver = get32(DPTR(g_bmi_resp) + 4);
   return 0;
}

static int bmi_done(void)
{
   if (g_bmi_done_sent)
      return 0;
   g_bmi_done_sent = 1;
   put32(DPTR(g_bmi_req), BMI_DONE);
   return bmi_xchg(4, 0);
}

/* ---- wake / reset (ath10k pci.c) ---------------------------------------------------------------- */
static int force_wake(void)
{
   USHORT i;

   ath_wr(PCIE_LOCAL_BASE + PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);
   for (i = 0; i < 600; i++) {                          /* PCIE_WAKE_TIMEOUT: 30 ms                      */
      if ((ath_rd(PCIE_LOCAL_BASE + RTC_STATE_ADDRESS) & RTC_STATE_V_MASK) == RTC_STATE_V_ON)
         return 0;
      qc_udelay(50);
   }
   return -1;
}

static void msi_fw_mask(int on)
{
   ULONG v = ath_rd(SOC_CORE_BASE + CORE_CTRL_ADDRESS);

   if (on) v &= ~CORE_CTRL_PCIE_REG_31_MASK;
   else    v |= CORE_CTRL_PCIE_REG_31_MASK;
   ath_wr(SOC_CORE_BASE + CORE_CTRL_ADDRESS, v);
}

static void irq_disable_all(void)
{
   ce_irq_disable();
   intx_disable_clear();
   msi_fw_mask(1);
}

static int wait_target_init(void)
{
   ULONG t0 = qc_ms(), v;

   do {
      v = ath_rd(FW_INDICATOR_ADDRESS);
      if (v != 0xFFFFFFFFUL) {
         if (v & FW_IND_EVENT_PENDING) break;
         if (v & FW_IND_INITIALIZED) break;
      }
      intx_enable();                                    /* "fix potential race by repeating CORE_BASE writes" */
      qc_msleep(10);
   } while ((long)(qc_ms() - t0) < 3000L);
   intx_disable_clear();
   msi_fw_mask(1);
   if (v == 0xFFFFFFFFUL) { qc_err("device is gone", v); return -1; }
   if (v & FW_IND_EVENT_PENDING) { qc_err("target crashed during init", v); return -2; }
   if (!(v & FW_IND_INITIALIZED)) { qc_err("target did not initialise", v); return -3; }
   return 0;
}

static ULONG soc_rd(ULONG a)          { return ath_rd(RTC_SOC_BASE + a); }
static void  soc_wr(ULONG a, ULONG v) { ath_wr(RTC_SOC_BASE + a, v); }

static void cold_reset(void)
{
   ULONG v = ath_rd(PCIE_LOCAL_BASE + SOC_GLOBAL_RESET_ADDRESS);

   ath_wr(PCIE_LOCAL_BASE + SOC_GLOBAL_RESET_ADDRESS, v | 1UL);
   qc_msleep(20);
   ath_wr(PCIE_LOCAL_BASE + SOC_GLOBAL_RESET_ADDRESS, v & ~1UL);
   qc_msleep(20);
}

static void warm_reset_cpu(void)
{
   ath_wr(FW_INDICATOR_ADDRESS, 0UL);
   soc_wr(SOC_RESET_CONTROL_ADDRESS, soc_rd(SOC_RESET_CONTROL_ADDRESS) | SOC_RESET_CONTROL_CPU_WARM_RST_MASK);
}

static int warm_reset(void)
{
   ULONG v;

   irq_disable_all();
   /* warm_reset_si0: the SI0 mask is 0 on QCA6174, the two writes change nothing but are kept */
   v = soc_rd(SOC_RESET_CONTROL_ADDRESS);
   soc_wr(SOC_RESET_CONTROL_ADDRESS, v | SOC_RESET_CONTROL_SI0_RST_MASK);
   qc_msleep(10);
   v = soc_rd(SOC_RESET_CONTROL_ADDRESS);
   soc_wr(SOC_RESET_CONTROL_ADDRESS, v & ~SOC_RESET_CONTROL_SI0_RST_MASK);
   qc_msleep(10);
   warm_reset_cpu();
   ce_init_pipes();
   wait_target_init();                                  /* the result of the first wait is ignored (ath10k) */
   /* clear the LF timer, reset the copy engines, reset the CPU again */
   soc_wr(SOC_LF_TIMER_CONTROL0_ADDRESS, soc_rd(SOC_LF_TIMER_CONTROL0_ADDRESS) & ~SOC_LF_TIMER_CONTROL0_ENABLE_MASK);
   v = soc_rd(SOC_RESET_CONTROL_ADDRESS);
   soc_wr(SOC_RESET_CONTROL_ADDRESS, v | SOC_RESET_CONTROL_CE_RST_MASK);
   qc_msleep(10);
   soc_wr(SOC_RESET_CONTROL_ADDRESS, v & ~SOC_RESET_CONTROL_CE_RST_MASK);
   warm_reset_cpu();
   ce_init_pipes();
   return wait_target_init();
}

/* ath10k_pci_qca6174_chip_reset: cold reset, wait, warm reset. */
static int chip_reset(void)
{
   int rc;

   cold_reset();
   rc = wait_target_init();
   if (rc)
      return rc;
   return warm_reset();
}

/* ---- ath10k_pci_init_config: CE configuration and service map into the target, early alloc, go ---- */
typedef struct { ULONG pipenum, pipedir, nentries, nbytes_max, flags, reserved; } PIPECFG;
typedef struct { ULONG svc, dir, pipe; } SVCMAP;

#define PIPEDIR_IN    1
#define PIPEDIR_OUT   2
#define PIPEDIR_INOUT 3

/* pci_target_ce_config_wlan with the QCA6174 override of CE5 (out, 2048) */
static PIPECFG g_tgt_pipes[9] = {
   { 0, PIPEDIR_OUT,   32,  256, 0, 0 },
   { 1, PIPEDIR_IN,    32, 2048, 0, 0 },
   { 2, PIPEDIR_IN,    64, 2048, 0, 0 },
   { 3, PIPEDIR_OUT,   32, 2048, 0, 0 },
   { 4, PIPEDIR_OUT,  256,  256, 0, 0 },
   { 5, PIPEDIR_OUT,   32, 2048, 0, 0 },
   { 6, PIPEDIR_INOUT, 32, 4096, 0, 0 },
   { 7, PIPEDIR_INOUT,  0,    0, 0, 0 },
   { 8, PIPEDIR_IN,    64, 2048, CE_ATTR_DIS_INTR, 0 }
};

/* pci_target_service_to_ce_map_wlan, entry 15 (HTT data in) moved to pipe 1 for QCA6174 */
static SVCMAP g_svcmap[19] = {
   { 0x0104, PIPEDIR_OUT, 3 }, { 0x0104, PIPEDIR_IN, 2 },   /* WMI_DATA_VO   */
   { 0x0102, PIPEDIR_OUT, 3 }, { 0x0102, PIPEDIR_IN, 2 },   /* WMI_DATA_BK   */
   { 0x0101, PIPEDIR_OUT, 3 }, { 0x0101, PIPEDIR_IN, 2 },   /* WMI_DATA_BE   */
   { 0x0103, PIPEDIR_OUT, 3 }, { 0x0103, PIPEDIR_IN, 2 },   /* WMI_DATA_VI   */
   { 0x0100, PIPEDIR_OUT, 3 }, { 0x0100, PIPEDIR_IN, 2 },   /* WMI_CONTROL   */
   { 0x0001, PIPEDIR_OUT, 0 }, { 0x0001, PIPEDIR_IN, 1 },   /* RSVD_CTRL     */
   { 0xFE00, PIPEDIR_OUT, 0 }, { 0xFE00, PIPEDIR_IN, 1 },   /* TEST_RAW      */
   { 0x0300, PIPEDIR_OUT, 4 }, { 0x0300, PIPEDIR_IN, 1 },   /* HTT_DATA_MSG  */
   { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }
};

int htc_map_service(USHORT svc, UCHAR *ul, UCHAR *dl)
{
   USHORT i, got = 0;

   for (i = 0; i < 16; i++) {
      if (g_svcmap[i].svc != (ULONG)svc)
         continue;
      if (g_svcmap[i].dir == PIPEDIR_OUT) { *ul = (UCHAR)g_svcmap[i].pipe; got |= 1; }
      if (g_svcmap[i].dir == PIPEDIR_IN)  { *dl = (UCHAR)g_svcmap[i].pipe; got |= 2; }
   }
   return (got == 3) ? 0 : -1;
}

static int init_config(void)
{
   ULONG pcie_state, pipe_cfg, svc_map, flags, ealloc, flag2;

   pcie_state = diag_rd32(HOST_INTEREST + HI_INTERCONNECT_STATE);
   if (g_diag_rc || !pcie_state) { qc_err("no pcie state address", pcie_state); return -1; }
   pipe_cfg = diag_rd32(pcie_state + 0);                 /* struct pcie_state.pipe_cfg_addr             */
   if (g_diag_rc || !pipe_cfg) { qc_err("no pipe config address", pipe_cfg); return -2; }
   if (diag_write(pipe_cfg, (UCHAR FAR *)g_tgt_pipes, 7 * sizeof(PIPECFG))) {  /* NUM_TARGET_CE_CONFIG_WLAN = 7 */
      qc_err("pipe config write failed", 0UL); return -3;
   }
   svc_map = diag_rd32(pcie_state + 4);                  /* .svc_to_pipe_map                            */
   if (g_diag_rc || !svc_map) { qc_err("no service map address", svc_map); return -4; }
   if (diag_write(svc_map, (UCHAR FAR *)g_svcmap, sizeof(g_svcmap))) {
      qc_err("service map write failed", 0UL); return -5;
   }
   flags = diag_rd32(pcie_state + 32);                   /* .config_flags (9th word, offset 32)         */
   if (g_diag_rc) return -6;
   if (diag_wr32(pcie_state + 32, flags & ~1UL))         /* PCIE_CONFIG_FLAG_ENABLE_L1 off              */
      return -7;
   ealloc = diag_rd32(HOST_INTEREST + HI_EARLY_ALLOC);
   if (g_diag_rc) return -8;
   {
      U32 e;

      e.l = ealloc;
      e.w[1] |= (USHORT)HI_EARLY_ALLOC_MAGIC;            /* magic in bits 31..16                       */
      e.w[0] |= 9;                                       /* 9 IRAM banks on hw3.0 / 3.1 / 3.2          */
      if (diag_wr32(HOST_INTEREST + HI_EARLY_ALLOC, e.l))
         return -9;
   }
   flag2 = diag_rd32(HOST_INTEREST + HI_OPTION_FLAG2);
   if (g_diag_rc) return -10;
   if (diag_wr32(HOST_INTEREST + HI_OPTION_FLAG2, flag2 | HI_OPTION_EARLY_CFG_DONE))
      return -11;
   return 0;
}

static void wake_target_cpu(void)
{
   ath_wr(SOC_CORE_BASE + CORE_CTRL_ADDRESS, ath_rd(SOC_CORE_BASE + CORE_CTRL_ADDRESS) | CORE_CTRL_CPU_INTR_MASK);
}

/* ---- board data: OTP board id, board name, download (core.c) ------------------------------------ */
static char *hex4(char *p, USHORT v)
{
   static char hx[] = "0123456789abcdef";

   *p++ = hx[(v >> 12) & 15]; *p++ = hx[(v >> 8) & 15]; *p++ = hx[(v >> 4) & 15]; *p++ = hx[v & 15];
   return p;
}

static char *dec(char *p, USHORT v)
{
   char t[6];
   int  n = 0;

   do { t[n++] = (char)('0' + v % 10); v = (USHORT)(v / 10); } while (v);
   while (n) *p++ = t[--n];
   return p;
}

static char *cpy(char *p, const char *s)
{
   while (*s) *p++ = *s++;
   return p;
}

/* "bus=pci,bmi-chip-id=%d,bmi-board-id=%d" or "bus=pci,vendor=%04x,device=%04x,subsystem-vendor=%04x,
 * subsystem-device=%04x" (ath10k_core_create_board_name, no variant). */
static void board_name(int bmi)
{
   char *p = g_bdname;

   p = cpy(p, "bus=pci,");
   if (bmi) {
      p = cpy(p, "bmi-chip-id="); p = dec(p, g_otp_chip);
      p = cpy(p, ",bmi-board-id="); p = dec(p, g_otp_board);
   } else {
      p = cpy(p, "vendor="); p = hex4(p, g_pci_ven);
      p = cpy(p, ",device="); p = hex4(p, g_pci_dev);
      p = cpy(p, ",subsystem-vendor="); p = hex4(p, g_pci_subven);
      p = cpy(p, ",subsystem-device="); p = hex4(p, g_pci_subdev);
   }
   *p = 0;
}

char *ath_board_name(void) { return g_bdname; }

static int otp_board_id(void)
{
   static ULONG result;

   if (!g_fw.otp_len)
      return -1;
   if (bmi_fast_download(QCA6174_PATCH_LOAD_ADDR, &g_fwblob, g_fw.otp_off, g_fw.otp_len))
      return -2;
   if (bmi_execute(QCA6174_PATCH_LOAD_ADDR, BMI_PARAM_GET_EEPROM_BOARD_ID, &result))
      return -3;
   g_ext_bid = (UCHAR)((result & 0x40000UL) ? 1 : 0);
   if ((result & 0xFFUL) != 0)
      return -4;                                       /* status: no board id in OTP                    */
   {
      U32 r;

      r.l = result;
      g_otp_board = (UCHAR)((r.w[0] >> 10) & 0x1F);                      /* mask 0x7c00, shift 10   */
      g_otp_chip  = (UCHAR)(((r.w[0] >> 15) & 1) | ((r.w[1] & 1) << 1)); /* mask 0x18000, shift 15  */
   }
   if (g_otp_board == 0)
      return -5;
   g_otp_valid = 1;
   return 0;
}

static int find_board(void)
{
   if (otp_board_id() == 0) {
      board_name(1);
      if (fw_find_board(g_bdname) == 0)
         return 0;
   }
   board_name(0);
   if (fw_find_board(g_bdname) == 0)
      return 0;
   qc_err("no board data in board-2.bin for this board", 0UL);
   return -1;
}

/* BMI write of blob bytes, straight from the blob into the request buffer. */
static int bmi_write_blob(ULONG addr, BLOB *b, ULONG off, USHORT len)
{
   UCHAR FAR *q = DPTR(g_bmi_req);
   USHORT k, kr;
   int    rc;

   while (len) {
      k = (len > BMI_MAX_DATA - 12) ? (USHORT)(BMI_MAX_DATA - 12) : len;
      kr = (USHORT)((k + 3) & ~3);
      rt_setmem(q + 12, 0, kr);
      if (blob_read(b, off, q + 12, k))
         return -20;
      put32(q, BMI_WRITE_MEMORY);
      put32(q + 4, addr);
      put32(q + 8, (ULONG)kr);
      rc = bmi_xchg((USHORT)(12 + kr), 0);
      if (rc)
         return rc;
      addr += k;
      off += k;
      len = (USHORT)(len - k);
   }
   return 0;
}

/* ath10k_download_board_data (QCA6174: 8 KB of board data, no extended board data) */
static int download_board_data(void)
{
   ULONG  addr, ext;
   USHORT n;

   n = (g_fw.bd_len < QCA6174_BOARD_DATA_SZ) ? (USHORT)g_fw.bd_len : QCA6174_BOARD_DATA_SZ;
   /* ath10k_push_board_ext_data: with an extended-data address the data must be exactly 8 KB + 0 */
   ext = bmi_rd32(HI_BOARD_EXT_DATA);
   if (g_bmi_rc) return -2;
   if (ext != 0UL) {
      if (g_fw.bd_len != QCA6174_BOARD_DATA_SZ) { qc_err("board data size", g_fw.bd_len); return -3; }
      if (bmi_wr32(0xB0, 1UL))                    /* hi_board_ext_data_config = (0 << 16) | 1          */
         return -3;
   }
   addr = bmi_rd32(HI_BOARD_DATA);
   if (g_bmi_rc) return -4;
   if (bmi_write_blob(addr, &g_bdblob, g_fw.bd_off, n))
      return -5;
   if (bmi_wr32(HI_BOARD_DATA_INITIALIZED, 1UL))
      return -6;
   return 0;
}

static int run_otp(void)
{
   static ULONG result;

   if (!g_fw.otp_len) {
      tstr("[no OTP image: calibration will be incorrect]");
      return 0;
   }
   if (bmi_fast_download(QCA6174_PATCH_LOAD_ADDR, &g_fwblob, g_fw.otp_off, g_fw.otp_len))
      return -1;
   if (bmi_execute(QCA6174_PATCH_LOAD_ADDR, 0UL, &result))   /* otp_exe_param = 0                        */
      return -2;
   if (result != 0UL && !fw_feature(7)) {                    /* ATH10K_FW_FEATURE_IGNORE_OTP_RESULT        */
      qc_err("OTP calibration failed", result);
      return -3;
   }
   return 0;
}

/* ath10k swap.c: the code swap segment goes into host memory, its description into the target. */
static int code_swap(void)
{
   static UCHAR hwinfo[208];                      /* struct ath10k_swap_code_seg_hw_info                */
   ULONG pos, end, total = 0, size, phys, len, tgt = 0xFFFFFFFFUL;
   USHORT n;
   UCHAR  log2 = 0;

   if (!g_fw.swap_len)
      return 0;
   size = (g_fw.swap_len + 1) & ~1UL;
   if (size > 512UL * 1024UL) { qc_err("code swap image too large", size); return -1; }
   phys = dma_big(size);
   if (!phys) { qc_err("no memory for the code swap segment", size); return -2; }
   pos = g_fw.swap_off;
   end = pos + g_fw.swap_len;
   while (pos + 8 <= end) {
      len = blob_rd32(&g_fwblob, pos + 4);
      if (len == 0) {                              /* tail: 12 magic bytes, then the BMI write address    */
         tgt = blob_rd32(&g_fwblob, pos + 12);
         break;
      }
      if (len > end - pos - 8) return -3;
      pos += 8;
      while (len) {                                /* copy payload to host memory, 4 KB at a time         */
         static UCHAR t[512];

         n = (len > 512UL) ? 512 : (USHORT)len;
         if (blob_read(&g_fwblob, pos, (UCHAR FAR *)t, n)) return -4;
         {
            USHORT s = scr_map(SCR_B, phys + total, n);

            if (!s) return -5;
            rt_movmem(MAKEP(s, 0), (PVOID)t, n);
         }
         pos += n; total += n; len -= n;
      }
   }
   if (tgt == 0xFFFFFFFFUL) { qc_err("invalid code swap image", 0UL); return -6; }
   for (len = size; len > 1; len >>= 1)            /* ilog2 (constant shift by 1)                         */
      log2++;
   rt_setmem((PVOID)hwinfo, 0, sizeof(hwinfo));
   put32((UCHAR FAR *)hwinfo + 0, total);         /* swap_size                                           */
   put32((UCHAR FAR *)hwinfo + 4, 1UL);           /* num_segs                                            */
   put32((UCHAR FAR *)hwinfo + 8, size);          /* size                                                */
   put32((UCHAR FAR *)hwinfo + 12, (ULONG)log2);  /* size_log2                                           */
   put32((UCHAR FAR *)hwinfo + 16, phys);         /* bus_addr[0]                                         */
   return bmi_write_memory(tgt, (UCHAR FAR *)hwinfo, sizeof(hwinfo));
}

/* ath10k_init_configure_target */
static int configure_target(void)
{
   ULONG v;

   if (bmi_wr32(HI_APP_HOST_INTEREST, 2UL))                  /* HTC_PROTOCOL_VERSION                    */
      return -1;
   v = bmi_rd32(HI_OPTION_FLAG);
   if (g_bmi_rc) return -2;
   v |= 0x00000200UL;                                        /* 1 device       << HI_OPTION_NUM_DEV_SHIFT (9)        */
   v |= 0x00002000UL;                                        /* FW_MODE_AP (2) << HI_OPTION_FW_MODE_SHIFT (12)       */
   v |= 0x00000008UL;                                        /* mac addr method 1 << HI_OPTION_MAC_ADDR_METHOD_SHIFT */
   if (bmi_wr32(HI_OPTION_FLAG, v)) return -3;
   if (bmi_wr32(HI_BE, 0UL)) return -4;
   if (bmi_wr32(HI_FW_SWAP, 0UL)) return -5;
   if (bmi_wr32(HI_HCI_UART_PWR_MGMT_PARAMS_EXT, (ULONG)g_pci_dev)) return -6;
   return 0;
}

/* ---- the whole bring-up -------------------------------------------------------------------------- */
#define STEP(n, call) do { g_hw_state = (n); rc = (call); if (rc) { qc_err(#call, (ULONG)(long)rc); goto fail; } } while (0)

int ath_up(void)
{
   int rc;

   if (g_hw_ready)
      return 0;
   g_hw_failed = 0;
   g_bmi_done_sent = 0;
   g_wmi_svc_ready = g_wmi_ready = 0;
   g_svc_full_reorder = g_svc_mgmt_tx_wmi = 0;
   tstr("[up]");
   STEP(1, (g_fwblob.done && g_bdblob.done) ? 0 : -1);   /* both files must have been copied to WRND32$  */
   STEP(2, fw_parse());
   STEP(3, pci_enable());
   STEP(4, bar_map());
   STEP(5, ce_alloc());
   if (!g_bmi_req.phys) {
      STEP(6, dma_get(BMI_MAX_DATA, &g_bmi_req));
      STEP(7, dma_get(BMI_MAX_DATA, &g_bmi_resp));
   }
   hook_init();
   if (irq_hook())
      tstr("[no irq: polled]");
   STEP(8, force_wake());
   irq_disable_all();
   STEP(9, chip_reset());
   g_chip_id = soc_rd(SOC_CHIP_ID_ADDRESS);
   {
      U32 c;
      USHORT rev;

      c.l = g_chip_id;
      rev = (USHORT)((c.w[0] >> 8) & 0x0F);
      STEP(10, (g_pci_dev == QCA9377_DEV_ID ? (rev == 0 || rev == 1)           /* QCA9377 hw1.0 / 1.1                 */
                                           : (rev == 8 || rev == 9 || rev == 10)) ? 0 : -1);   /* QCA6174 hw3.0 / 3.1 / 3.2 */
   }
   ce_init_pipes();
   STEP(11, init_config());
   wake_target_cpu();
   STEP(12, bmi_get_target_info());
   STEP(13, find_board());
   STEP(14, configure_target());
   STEP(15, download_board_data());
   STEP(16, run_otp());
   STEP(17, code_swap());
   STEP(18, bmi_fast_download(QCA6174_PATCH_LOAD_ADDR, &g_fwblob, g_fw.fw_off, g_fw.fw_len));
   STEP(19, bmi_wr32(HI_SERIAL_ENABLE, 0UL));            /* ath10k_init_uart                              */
   htc_reset();
   STEP(20, bmi_done());
   /* hif start: interrupts on, receive buffers posted */
   ce_irq_enable();
   intx_enable();
   msi_fw_mask(0);
   ce_rx_fill(1);
   ce_rx_fill(2);
   STEP(21, htc_wait_target());
   STEP(22, htc_connect(HTC_SVC_HTT_DATA_MSG, &g_htt_ep));
   STEP(23, htc_connect(HTC_SVC_WMI_CONTROL, &g_wmi_ep));
   STEP(24, htc_start());
   STEP(25, wmi_init_target());
   STEP(26, htt_setup());                                /* version, rx ring, aggregation                  */
   g_hw_state = 100;
   g_hw_ready = 1;
   intx_enable();                                       /* an interrupt during start-up left it masked   */
   tstr("[ready]");
   return 0;

fail:
   g_hw_failed = 1;
   irq_disable_all();
   return rc;
}

extern USHORT g_nbarsel;

void ath_down(void)
{
   if (g_hw_ready)
      sta_leave();
   g_hw_ready = 0;
   g_htt_ready = 0;
   if (g_nbarsel)
      irq_disable_all();
}
