/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * QCA6174 - Qualcomm Atheros QCA6174 PCIe WiFi driver for OS/2 and ArcaOS (GenMAC + NDIS 2.0.1)
 *
 * qca.h - definitions shared by every module of the driver.
 *
 * The chip side is a port of the Linux ath10k driver (drivers/net/wireless/ath/ath10k, ISC licence):
 * register addresses and sequences come from pci.c, ce.c, hw.c/hw.h, bmi.c, htc.c, core.c, wmi-tlv.c.
 * The OS/2 side (strategy routine, NDIS MAC, GenMAC OID device) is adapted from the OS2World RTL88EU
 * driver (GPL-2.0-only). The combination is distributed under GPL-2.0-only.
 *
 * Compiler constraints (16-bit Open Watcom, no C runtime): no 32-bit multiply, divide or variable shift;
 * SS != DS (never pass the address of a stack variable to code that dereferences it DS-relative).
 */
#ifndef QCA_H
#define QCA_H

#include "qc_os2dd.h"

#define DRV_NAME        "QCA6174"
#define DRV_VERSION     "0.0.1"
#define DRV_BUILD       "phase1"

#ifndef TRACE
#define TRACE 1                         /* 1: progress markers and errors on COM1                         */
#endif

/* ---- little helpers ---------------------------------------------------------------------------- */
typedef union { ULONG l; USHORT w[2]; UCHAR b[4]; } U32;
#define LO16(x)         ((USHORT)(x))
#define HI16(x)         (((U32 FAR *)&(x))->w[1])          /* x must be an lvalue */

/* ---- trace (qc_dbg.c) ---------------------------------------------------------------------------- */
#if TRACE
#define tcom(c) tcom_always(c)
#else
#define tcom(c) ((void)(c))
#endif
void   tstr(const char *s);             /* near string in DGROUP                                          */
void   th8(UCHAR b);
void   th16(USHORT w);
void   th32(ULONG v);
void   tnl(void);
void   qc_err(const char *what, ULONG v);    /* "QCA6174: <what> <v>" on COM1, remembered for the diagnostics */
extern ULONG g_last_err;
extern char *g_last_err_txt;

void   qc_step(USHORT n);                  /* boot diagnostics: n beeps with /B (qc_strat.c)                 */

/* ---- time (qc_strat.c) --------------------------------------------------------------------------- */
ULONG  qc_ms(void);                     /* ms since boot (global info segment)                            */
void   qc_msleep(USHORT ms);            /* task time only: ProcBlock with a timeout                        */
#define qc_udelay(us) qc_iodelay(us)

/* ---- PCI and MMIO (qc_pci.c) ---------------------------------------------------------------------- */
#define QCA_VENDOR_ID   0x168C
#define QCA6174_DEV_ID  0x003E
extern USHORT g_pci_bdf;                 /* bus << 8 | dev << 3 | fn, 0xFFFF = not found                   */
extern USHORT g_pci_ven, g_pci_dev, g_pci_subven, g_pci_subdev;
extern UCHAR  g_pci_rev, g_pci_irq;
extern ULONG  g_bar0;                    /* physical address of BAR0                                       */
extern ULONG  g_bar_len;
int    pci_find(void);                   /* 0 = found the QCA6174                                          */
ULONG  pci_cfg_rd32(UCHAR reg);
void   pci_cfg_wr32(UCHAR reg, ULONG v);
USHORT pci_cfg_rd16(UCHAR reg);
void   pci_cfg_wr16(UCHAR reg, USHORT v);
int    pci_enable(void);                 /* memory space + bus master, ASPM off                             */
int    bar_map(void);                    /* INIT time: GDT selectors over BAR0                              */
ULONG  ath_rd(ULONG off);                /* one 32-bit MMIO read of BAR0 + off                              */
void   ath_wr(ULONG off, ULONG v);
int    irq_hook(void);
void   intx_enable(void);
void   intx_disable_clear(void);
void   hook_arm(void);                   /* SetIRQ (shared) on the PCI interrupt line                       */
void   irq_unhook(void);
extern USHORT g_irq_hooked, g_irq_count, g_irq_spurious;

/* ---- DMA memory (qc_mem.c) ------------------------------------------------------------------------ *
 * Physically contiguous 64 KB chunks from AllocPhys, each with a permanent GDT selector (allocated at
 * INIT). Small buffers are carved out of them and never freed. A DMA buffer never crosses a chunk.    */
typedef struct {
   USHORT sel;                           /* GDT selector of the chunk                                      */
   USHORT off;                           /* offset of the buffer in the chunk                              */
   ULONG  phys;                          /* physical (= device) address of the buffer                      */
} DMA;
#define DPTR(d)         ((UCHAR FAR *)MAKEP((d).sel, (d).off))
int    mem_init(void);                   /* INIT: selectors + chunks                                       */
int    dma_get(USHORT size, DMA *d);     /* 8-byte aligned, zeroed; 0 = ok                                 */
USHORT dma_free_bytes(void);
ULONG  dma_big(ULONG size);              /* a larger contiguous block for the firmware (no mapping); 0 = fail */
USHORT scr_map(USHORT slot, ULONG phys, USHORT len);   /* scratch selector over phys, returns the selector */
#define SCR_A 0                          /* request path (nic_lock): firmware blobs, status text            */
#define SCR_B 1
#define SCR_C 2
#define SCR_RX 3                         /* HTT receive buffers (service path, one CPU at a time)            */
#define SCR_TX 4                         /* NDIS TransmitChain physical blocks (under g_txsel_lock)          */

/* Firmware files, written to the device at run time ("copy /b file \DEV\WRND32$"). Kept as a list of
 * 64 KB physical blocks (they need not be contiguous: only the driver reads them). */
#define BLOB_MAXBLK  32                  /* 2 MB                                                           */
typedef struct {
   ULONG  len;
   USHORT nblk;
   USHORT done;                          /* complete (the writer closed the device)                        */
   ULONG  blk[BLOB_MAXBLK];
} BLOB;
extern BLOB g_fwblob, g_bdblob;
int    blob_write(BLOB *b, ULONG src_phys, USHORT n);   /* append n bytes from physical src; 0 = ok          */
int    blob_read(BLOB *b, ULONG off, UCHAR FAR *dst, USHORT n);   /* 0 = ok                                */
ULONG  blob_rd32(BLOB *b, ULONG off);
void   blob_reset(BLOB *b);

/* ---- firmware container parsing (qc_fw.c) --------------------------------------------------------- */
typedef struct {
   ULONG  fw_off, fw_len;                /* ATH10K_FW_IE_FW_IMAGE                                          */
   ULONG  otp_off, otp_len;              /* ATH10K_FW_IE_OTP_IMAGE                                         */
   ULONG  swap_off, swap_len;            /* ATH10K_FW_IE_FW_CODE_SWAP_IMAGE                                */
   ULONG  wmi_op, htt_op;
   UCHAR  features[16];
   char   version[40];
   ULONG  bd_off, bd_len;                /* the matching board data in board-2.bin                         */
} FWINFO;
extern FWINFO g_fw;
int    fw_parse(void);                   /* firmware-N.bin                                                 */
int    fw_find_board(char *name);        /* board-2.bin: 0 = found                                         */
int    fw_feature(USHORT bit);

/* ---- QCA6174 register map (ath10k hw.c qca6174_regs, hw.h) --------------------------------------- */
#define RTC_SOC_BASE            0x00000800UL
#define RTC_WMAC_BASE           0x00001000UL
#define SOC_CORE_BASE           0x0003A000UL
#define CE_WRAPPER_BASE         0x00034000UL
#define CE0_BASE                0x00034400UL         /* CEn = CE0_BASE + n * 0x400                     */
#define PCIE_LOCAL_BASE         0x00080000UL
#define FW_INDICATOR_ADDRESS    0x0003A028UL
#define SOC_CHIP_ID_ADDRESS     0x000000F0UL         /* + RTC_SOC_BASE                                   */
#define RTC_STATE_ADDRESS       0x0000UL             /* + PCIE_LOCAL_BASE                                */
#define RTC_STATE_V_MASK        0x7UL
#define RTC_STATE_V_ON          3UL                  /* qca6174_values.rtc_state_val_on                  */
#define PCIE_SOC_WAKE_ADDRESS   0x0004UL
#define PCIE_SOC_WAKE_V_MASK    0x1UL
#define SOC_GLOBAL_RESET_ADDRESS 0x0008UL
#define SOC_RESET_CONTROL_ADDRESS 0x0000UL           /* + RTC_SOC_BASE                                   */
#define SOC_RESET_CONTROL_SI0_RST_MASK 0x0UL
#define SOC_RESET_CONTROL_CE_RST_MASK 0x1UL
#define SOC_RESET_CONTROL_CPU_WARM_RST_MASK 0x40UL
#define SOC_LF_TIMER_CONTROL0_ADDRESS 0x0050UL
#define SOC_LF_TIMER_CONTROL0_ENABLE_MASK 0x4UL
#define CORE_CTRL_ADDRESS       0x0000UL             /* + SOC_CORE_BASE                                  */
#define CORE_CTRL_CPU_INTR_MASK 0x2000UL
#define CORE_CTRL_PCIE_REG_31_MASK 0x0800UL
#define PCIE_INTR_ENABLE_ADDRESS 0x0008UL            /* + SOC_CORE_BASE                                  */
#define PCIE_INTR_CAUSE_ADDRESS 0x000CUL
#define PCIE_INTR_CLR_ADDRESS   0x0014UL
#define PCIE_INTR_FIRMWARE_MASK 0x00000400UL
#define PCIE_INTR_CE_MASK_ALL   0x0007F800UL
#define FW_IND_EVENT_PENDING    1UL
#define FW_IND_INITIALIZED      2UL
#define CE_WRAP_INTR_SUM_LSB    8
#define CE_WRAP_INTR_SUM_MASK   0x0000FF00UL

/* host interest area (targaddrs.h): QCA988X_HOST_INTEREST_ADDRESS is used for every PCI chip */
#define HOST_INTEREST           0x00400800UL
#define HI_APP_HOST_INTEREST    0x00
#define HI_OPTION_FLAG          0x10
#define HI_SERIAL_ENABLE        0x14
#define HI_BE                   0x44
#define HI_BOARD_DATA           0x54
#define HI_BOARD_DATA_INITIALIZED 0x58
#define HI_BOARD_EXT_DATA       0xAC
#define HI_HCI_UART_PWR_MGMT_PARAMS_EXT 0xBC
#define HI_OPTION_FLAG2         0xCC
#define HI_INTERCONNECT_STATE   0xF8
#define HI_EARLY_ALLOC          0x100
#define HI_FW_SWAP              0x104
#define HI_OPTION_EARLY_CFG_DONE 0x10UL
#define HI_EARLY_ALLOC_MAGIC    0x6D8AUL
#define QCA6174_PATCH_LOAD_ADDR 0x1234UL
#define QCA6174_BOARD_DATA_SZ   8192

/* ---- copy engines (ath_ce.c) ---------------------------------------------------------------------- */
#define CE_COUNT        8
#define CE_DIAG         7
#define CE_RX_MAX       32                    /* largest host receive ring we use                         */
#define CE_ATTR_DIS_INTR 0x08
#define CE_SEND_GATHER  0x0001
typedef struct {
   ULONG  ctrl;                               /* register base of this engine                             */
   USHORT flags;
   USHORT src_n, src_mask, src_sw, src_wr, src_hw, src_max;
   DMA    src_ring;
   USHORT dst_n, dst_mask, dst_sw, dst_wr;
   DMA    dst_ring;
   /* host buffers */
   USHORT buf_sz;                             /* receive buffer size (dest) or slot size (source)         */
   USHORT nbuf;                               /* number of host buffers                                   */
   DMA    bufs;                               /* nbuf * buf_sz, one chunk                                 */
   UCHAR  dctx[CE_RX_MAX];                    /* dest ring index -> buffer number                         */
   UCHAR  bfree[CE_RX_MAX];                   /* receive buffer is not posted                             */
} CE;
extern CE g_ce[CE_COUNT];
int    ce_alloc(void);                        /* rings + buffers (INIT or first bring-up)                 */
void   ce_init_pipes(void);                   /* program every ring (after each reset)                    */
void   ce_irq_enable(void);
void   ce_irq_disable(void);
int    ce_send(USHORT id, ULONG phys, USHORT len, USHORT xfer_id, USHORT flags);   /* 0 = queued          */
int    ce_send_done(USHORT id);               /* 1 = one completed send consumed                          */
int    ce_rx_post(USHORT id, ULONG phys);     /* one raw dest buffer (BMI/diag)                           */
int    ce_rx_next(USHORT id, USHORT *nbytes);  /* 1 = a dest entry completed                               */
void   ce_rx_fill(USHORT id);                 /* post every free host buffer                              */
void   ce_rx_revoke(USHORT id);
ULONG  ce_summary(void);
void   ce_clear(USHORT id);
UCHAR FAR *ce_slot(USHORT id, USHORT n, ULONG *phys);   /* source slot n (n < nbuf)                         */
int    diag_read(ULONG addr, UCHAR FAR *dst, USHORT n);
int    diag_write(ULONG addr, UCHAR FAR *src, USHORT n);
ULONG  diag_rd32(ULONG addr);                /* g_diag_rc = 0 when read                  */
extern int g_diag_rc;
int    diag_wr32(ULONG addr, ULONG v);

/* ---- chip bring-up, BMI (ath_boot.c) -------------------------------------------------------------- */
extern USHORT g_hw_ready, g_hw_failed, g_hw_state;
extern ULONG  g_chip_id, g_target_ver;
extern UCHAR  g_mymac[6];
extern UCHAR  g_cfgmac;
extern UCHAR  g_wmi_mac[6];
int    ath_up(void);                          /* full bring-up; 0 = ready                                 */
void   ath_down(void);
ULONG  bmi_rd32(USHORT hi_item);             /* g_bmi_rc = 0 when read                   */
extern int g_bmi_rc;
int    bmi_wr32(USHORT hi_item, ULONG v);

/* ---- HTC (ath_htc.c) ------------------------------------------------------------------------------ */
#define HTC_SVC_RSVD_CTRL       0x0001
#define HTC_SVC_WMI_CONTROL     0x0100
#define HTC_SVC_HTT_DATA_MSG    0x0300
#define HTC_HDR_LEN     8
extern USHORT g_wmi_ep, g_htt_ep;
void   htc_reset(void);
int    htc_wait_target(void);
int    htc_connect(USHORT svc, USHORT *ep);
int    htc_start(void);
void   htc_rx(USHORT pipe, UCHAR FAR *p, USHORT len);    /* a completed CE receive                         */
UCHAR FAR *htc_tx_begin(USHORT ep, USHORT *slot, ULONG *phys);   /* payload area after the HTC header    */
int    htc_tx_end(USHORT ep, USHORT slot, USHORT paylen);
USHORT htc_credits(USHORT ep);
void   ath_service(void);                    /* poll every copy engine (task time / context hook)         */
int    ath_wait(USHORT *flag, USHORT ms);     /* service until *flag != 0 or timeout; 0 = flag set         */
int    htc_map_service(USHORT svc, UCHAR *ul, UCHAR *dl);

/* ---- WMI-TLV (ath_wmi.c) -------------------------------------------------------------------------- */
extern USHORT g_wmi_svc_ready, g_wmi_ready, g_scan_done, g_scan_running;
extern USHORT g_svc_full_reorder, g_svc_mgmt_tx_wmi;
int    wmi_init_target(void);                 /* service ready -> INIT -> ready, regdomain, channels, vdev */
int    wmi_scan(void);                        /* full active scan, results in g_net                       */
void   wmi_rx(UCHAR FAR *p, USHORT len);
/* station commands (all pointer arguments are far: callers may pass stack addresses) */
int    wmi_vdev_start(UCHAR ch, USHORT bcn_intval, USHORT dtim);
int    wmi_vdev_stop(void);
int    wmi_vdev_up(USHORT aid, UCHAR FAR *bssid);
int    wmi_vdev_down(void);
int    wmi_peer_create(UCHAR FAR *addr);
int    wmi_peer_delete(UCHAR FAR *addr);
int    wmi_peer_assoc(UCHAR FAR *addr, USHORT aid, USHORT caps, USHORT listen, ULONG flags,
                      UCHAR FAR *rates, USHORT nrates, USHORT phymode);
int    wmi_peer_authorize(UCHAR FAR *addr);
int    wmi_install_key(UCHAR FAR *addr, UCHAR idx, USHORT group, UCHAR FAR *key, USHORT klen);
int    wmi_mgmt_tx(UCHAR FAR *frame, USHORT len);
/* event results the join code waits for */
extern USHORT g_vdev_start_done, g_vdev_start_status, g_vdev_stopped, g_peer_map, g_sec_ind;
extern UCHAR  g_peer_map_addr[6];
extern USHORT g_auth_rx, g_auth_status, g_assoc_rx, g_assoc_status, g_assoc_aid, g_link_lost;
extern USHORT g_assoc_caps, g_assoc_reslen;
extern UCHAR  g_assoc_res[128];

/* ---- HTT (ath_htt.c) ----------------------------------------------------------------------------- */
extern USHORT g_htt_ready;
extern USHORT g_htt_txok, g_htt_txfail, g_htt_txfull, g_htt_rxfrm, g_htt_rxdrop, g_htt_rxind, g_htt_txcompl;
int    htt_setup(void);                       /* version, rx ring, aggregation limits                      */
void   htt_reset(void);
void   htt_rx(UCHAR FAR *p, USHORT len);      /* every message of the HTT endpoint                          */
int    htt_tx_mgmt(UCHAR FAR *frame, USHORT len);   /* a complete management frame, 0 = queued                */
int    htt_tx_eth(UCHAR FAR *eth, USHORT len, USHORT noenc);  /* 0 = queued, -1 = not connected, -2 = busy */

/* ---- station state and scan table (ath_sta.c) ----------------------------------------------------- */
#define NET_MAX         20
#define NET_IE_MAX      320
typedef struct {
   UCHAR  bssid[6];
   UCHAR  len;                                /* SSID length                                              */
   char   ssid[32];
   UCHAR  ch;
   signed char rssi;
   UCHAR  sec;                                /* privacy bit                                              */
   USHORT ielen;                              /* bytes in ie[] (12 fixed beacon bytes + elements)          */
   UCHAR  ie[NET_IE_MAX];
   ULONG  seen;
} net_t;
extern net_t  g_net[NET_MAX];
extern USHORT g_nnet;
extern USHORT g_joined, g_authorized, g_ptk_on, g_open_net;
extern UCHAR  g_jbssid[6], g_jssid[32], g_jslen, g_jch;
extern USHORT g_jfail;                        /* reason of the last failed join (see ARCHITECTURE.md)        */
extern UCHAR  g_jreq[160], g_jres[128];
extern USHORT g_jreqlen, g_jreslen, g_jcaps, g_jaid;
void   sta_beacon(UCHAR FAR *frame, USHORT len, USHORT chan, short rssi);
void   sta_scan_clear(void);
void   sta_age(void);                         /* drop scan entries not heard for 30 s                        */
void   sta_set_want(UCHAR FAR *ssid, UCHAR len, USHORT open);
int    sta_join(UCHAR FAR *want_bssid, USHORT any_bssid);   /* 0 = associated                              */
void   sta_leave(void);
void   sta_poll(void);                        /* request path: notice a lost link                          */
short  sta_rssi(void);
int    sta_key_add(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx);
int    sta_key_remove(USHORT pairwise, UCHAR idx);
void   sta_mgmt_rx(UCHAR FAR *frame, USHORT len);          /* auth / assoc response / deauth / disassoc    */
int    sta_tx(UCHAR FAR *eth, USHORT len);    /* NDIS data: 0 = queued, -1 = no link, -2 = busy              */
int    sta_eapol_tx(UCHAR FAR *eth, USHORT len);
void   sta_rx_eth(UCHAR FAR *eth, USHORT len);            /* HTT: a received frame as Ethernet             */
USHORT sta_eapol_count(void);
USHORT sta_eapol_get(UCHAR FAR *dst, USHORT max);

/* ---- NDIS and GenMAC (qc_ndis.c, qc_nic.c) -------------------------------------------------------- */
extern USHORT g_lannum;
void   ndis_find_lan(void);
int    ndis_set_mac(UCHAR *m);
void   qc_genmac_ioctl(RP_GENIOCTL FAR *rp);
void   qc_shutdown(void);
void   nic_lock(void);
void   nic_unlock(void);

/* ---- device writes: firmware upload and debug commands (qc_dbg.c) -------------------------------- */
void   qc_dev_open(USHORT sfn);
void   qc_dev_close(USHORT sfn);
void   qc_dev_write(USHORT sfn, ULONG phys, USHORT n);
USHORT qc_dev_read(USHORT sfn, ULONG phys, USHORT n);

/* ---- interrupt / context hook glue (qc_pci.c) ------------------------------------------------------ */
USHORT __cdecl qc_irq_c(void);
void   __cdecl qc_hook_c(void);
int    hook_init(void);

#endif
