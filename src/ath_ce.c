/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ath_ce.c - QCA6174 copy engines (port of ath10k ce.c + the CE parts of pci.c).
 *
 * Every copy engine has a source ring (host -> target) and/or a destination ring (target -> host) of 8-byte
 * descriptors { le32 addr; le16 nbytes; le16 flags } in host memory. Register offsets are qcax_ce_regs
 * (ath10k hw.c). Host-side ring sizes are smaller than Linux uses (memory below 64 KB per chunk):
 *
 *   CE  dir    Linux entries / size     here
 *   0   out    16 x 256   HTC control   16 x 256
 *   1   in     512 x 2048 HTC/HTT ctrl  32 x 2048 (31 buffers posted)
 *   2   in     128 x 2048 WMI events    32 x 2048 (31 buffers posted)
 *   3   out    32 x 2048  WMI commands  32 ring entries, 16 x 2048 slots
 *   4   out    8192 x 256 HTT messages  32 x 256 (two entries per data frame) + 4 control message slots
 *   5   -      overridden to nothing for QCA6174 (ath10k_pci_override_ce_config)
 *   7   both   2 x 2048   diagnostic window (polled)
 */
#include "qca.h"

/* qcax_ce_regs */
#define SR_BASE_ADDR    0x00
#define SR_SIZE         0x04
#define DR_BASE_ADDR    0x08
#define DR_SIZE         0x0C
#define CE_CTRL1        0x10
#define HOST_IE         0x2C
#define HOST_IS         0x30
#define MISC_IE         0x34
#define SR_WR_INDEX     0x3C
#define DST_WR_INDEX    0x40
#define CURRENT_SRRI    0x44
#define CURRENT_DRRI    0x48
#define SRC_WATERMARK   0x4C
#define DST_WATERMARK   0x50

#define CTRL1_DMAX_MASK 0x0000FFFFUL
#define CTRL1_SRC_SWAP  0x00010000UL
#define CTRL1_DST_SWAP  0x00020000UL
#define HOST_IE_CC      0x00000001UL
#define HOST_IE_WM      0x0000001EUL
#define MISC_IE_ERR     0x000007E0UL
#define HOST_IS_CLEAR   0x0000001FUL          /* cc_mask | wm_mask                                          */

#define DESC_GATHER     0x0001
#define META_LSB        2                     /* qca6174_values.ce_desc_meta_data_lsb / mask 0xFFFC        */

#define DIAG_LIMIT      2048                  /* DIAG_TRANSFER_LIMIT                                        */

CE g_ce[CE_COUNT];

typedef struct { USHORT src_n, src_sz, dst_n, nbuf, flags; } CEATTR;
static CEATTR g_attr[CE_COUNT] = {
   { 16,  256,  0, 16, 0 },
   {  0, 2048, 32, 31, 0 },
   {  0, 2048, 32, 31, 0 },
   { 32, 2048,  0, 16, 0 },
   { 32,  256,  0,  4, CE_ATTR_DIS_INTR },          /* 4 slots for HTT control messages; data frames use ath_htt.c buffers */
   {  0,    0,  0,  0, 0 },
   {  0,    0,  0,  0, 0 },
   {  2, DIAG_LIMIT, 2, 0, 0x10 }             /* CE_ATTR_POLL                                               */
};

static DMA g_diag_buf;
static USHORT g_ce_allocated;

static ULONG ce_rd(CE *c, USHORT reg)          { return ath_rd(c->ctrl + reg); }
static void  ce_wr(CE *c, USHORT reg, ULONG v) { ath_wr(c->ctrl + reg, v); }

typedef struct { ULONG addr; USHORT nbytes; USHORT flags; } CEDESC;
#define DESC(ring, i) ((CEDESC FAR *)(DPTR(ring) + ((i) << 3)))

int ce_alloc(void)
{
   USHORT i;

   if (g_ce_allocated)
      return 0;
   for (i = 0; i < CE_COUNT; i++) {
      CE *c = &g_ce[i];
      CEATTR *a = &g_attr[i];

      c->ctrl  = CE0_BASE + ((ULONG)i << 10);
      c->flags = a->flags;
      c->buf_sz = a->src_sz;
      c->src_max = a->src_sz;
      if (a->src_n) {
         c->src_n = a->src_n;
         c->src_mask = (USHORT)(a->src_n - 1);
         if (dma_get((USHORT)(a->src_n << 3), &c->src_ring))
            return -1;
      }
      if (a->dst_n) {
         c->dst_n = a->dst_n;
         c->dst_mask = (USHORT)(a->dst_n - 1);
         if (dma_get((USHORT)(a->dst_n << 3), &c->dst_ring))
            return -1;
      }
      c->nbuf = a->nbuf;
      if (c->nbuf) {
         USHORT k;
         ULONG  bytes = 0;

         for (k = 0; k < c->nbuf; k++)
            bytes += c->buf_sz;
         if (bytes > 0xFFF0UL || dma_get((USHORT)bytes, &c->bufs))
            return -2;
         for (k = 0; k < CE_RX_MAX; k++)
            c->bfree[k] = 1;
      }
   }
   if (dma_get(DIAG_LIMIT, &g_diag_buf))
      return -3;
   g_ce_allocated = 1;
   return 0;
}

/* Buffer n of an engine: far pointer and physical address. */
UCHAR FAR *ce_slot(USHORT id, USHORT n, ULONG *phys)
{
   CE    *c = &g_ce[id];
   USHORT off = 0, k;

   for (k = 0; k < n; k++)
      off = (USHORT)(off + c->buf_sz);
   if (phys)
      *phys = c->bufs.phys + off;
   return DPTR(c->bufs) + off;
}

/* ath10k_ce_init_src_ring / ath10k_ce_init_dest_ring for every engine; after each chip reset. */
void ce_init_pipes(void)
{
   USHORT i, k;

   for (i = 0; i < CE_COUNT; i++) {
      CE *c = &g_ce[i];
      ULONG v;

      if (c->src_n) {
         rt_setmem(DPTR(c->src_ring), 0, (USHORT)(c->src_n << 3));
         c->src_sw = (USHORT)(ce_rd(c, CURRENT_SRRI) & c->src_mask);
         c->src_hw = c->src_sw;
         c->src_wr = (USHORT)(ce_rd(c, SR_WR_INDEX) & c->src_mask);
         ce_wr(c, SR_BASE_ADDR, c->src_ring.phys);
         ce_wr(c, SR_SIZE, (ULONG)c->src_n);
         v = ce_rd(c, CE_CTRL1);
         v = (v & ~CTRL1_DMAX_MASK) | (ULONG)c->src_max;
         ce_wr(c, CE_CTRL1, v);
         v = ce_rd(c, CE_CTRL1);
         ce_wr(c, CE_CTRL1, v & ~CTRL1_SRC_SWAP);
         v = ce_rd(c, SRC_WATERMARK);
         v = (v & 0x0000FFFFUL);                              /* low mark (31..16) = 0                    */
         ce_wr(c, SRC_WATERMARK, v);
         v = ce_rd(c, SRC_WATERMARK);
         v = (v & 0xFFFF0000UL) | (ULONG)c->src_n;            /* high mark (15..0) = entries              */
         ce_wr(c, SRC_WATERMARK, v);
      }
      if (c->dst_n) {
         rt_setmem(DPTR(c->dst_ring), 0, (USHORT)(c->dst_n << 3));
         c->dst_sw = (USHORT)(ce_rd(c, CURRENT_DRRI) & c->dst_mask);
         c->dst_wr = (USHORT)(ce_rd(c, DST_WR_INDEX) & c->dst_mask);
         ce_wr(c, DR_BASE_ADDR, c->dst_ring.phys);
         ce_wr(c, DR_SIZE, (ULONG)c->dst_n);
         v = ce_rd(c, CE_CTRL1);
         ce_wr(c, CE_CTRL1, v & ~CTRL1_DST_SWAP);
         v = ce_rd(c, DST_WATERMARK);
         ce_wr(c, DST_WATERMARK, v & 0x0000FFFFUL);
         v = ce_rd(c, DST_WATERMARK);
         ce_wr(c, DST_WATERMARK, (v & 0xFFFF0000UL) | (ULONG)c->dst_n);
         for (k = 0; k < CE_RX_MAX; k++)
            c->bfree[k] = 1;
      }
   }
}

void ce_irq_enable(void)
{
   USHORT i;

   for (i = 0; i < CE_COUNT; i++) {
      CE *c = &g_ce[i];
      ULONG v;

      if (c->flags & 0x10)                                   /* polled engine                            */
         continue;
      v = ce_rd(c, HOST_IE);
      if (!(c->flags & CE_ATTR_DIS_INTR) && (c->src_n || c->dst_n))
         v |= HOST_IE_CC;
      else
         v &= ~HOST_IE_CC;
      ce_wr(c, HOST_IE, v);
      v = ce_rd(c, HOST_IE);
      ce_wr(c, HOST_IE, v & ~HOST_IE_WM);
   }
}

void ce_irq_disable(void)
{
   USHORT i;

   for (i = 0; i < CE_COUNT; i++) {
      CE *c = &g_ce[i];
      ULONG v;

      if (c->flags & 0x10)
         continue;
      v = ce_rd(c, HOST_IE);
      ce_wr(c, HOST_IE, v & ~HOST_IE_CC);
      v = ce_rd(c, MISC_IE);
      ce_wr(c, MISC_IE, v & ~MISC_IE_ERR);
      v = ce_rd(c, HOST_IE);
      ce_wr(c, HOST_IE, v & ~HOST_IE_WM);
   }
}

ULONG ce_summary(void)
{
   U32 v;

   v.l = ath_rd(CE_WRAPPER_BASE + 0x0000UL);
   return (ULONG)(v.b[1]);                                   /* bits 15..8 = one bit per engine           */
}

void ce_clear(USHORT id)
{
   ce_wr(&g_ce[id], HOST_IS, HOST_IS_CLEAR);
}

/* ---- source ring --------------------------------------------------------------------------------- */
/* Returns the ring index used (>= 0), -1 when the ring is full. */
int ce_send(USHORT id, ULONG phys, USHORT len, USHORT xfer_id, USHORT flags)
{
   CE *c = &g_ce[id];
   CEDESC FAR *d;
   USHORT wi = c->src_wr;

   if (!c->src_n)
      return -1;
   if (((c->src_sw - 1 - wi) & c->src_mask) == 0)
      return -1;                                              /* CE_RING_DELTA(...) <= 0                   */
   d = DESC(c->src_ring, wi);
   d->addr   = phys;
   d->nbytes = len;
   d->flags  = (USHORT)((xfer_id << META_LSB) | ((flags & CE_SEND_GATHER) ? DESC_GATHER : 0));
   c->src_wr = (USHORT)((wi + 1) & c->src_mask);
   if (!(flags & CE_SEND_GATHER))
      ce_wr(c, SR_WR_INDEX, (ULONG)c->src_wr);
   return (int)wi;
}

/* One completed send: returns its ring index + 1, 0 when none. */
int ce_send_done(USHORT id)
{
   CE *c = &g_ce[id];
   USHORT sw = c->src_sw;

   if (!c->src_n)
      return 0;
   if (c->src_hw == sw) {
      ULONG ri = ce_rd(c, CURRENT_SRRI);

      if (ri == 0xFFFFFFFFUL)
         return 0;
      c->src_hw = (USHORT)(ri & c->src_mask);
   }
   if (c->src_hw == sw)
      return 0;
   DESC(c->src_ring, sw)->nbytes = 0;
   c->src_sw = (USHORT)((sw + 1) & c->src_mask);
   return (int)sw + 1;
}

/* ---- destination ring --------------------------------------------------------------------------- */
int ce_rx_post(USHORT id, ULONG phys)
{
   CE *c = &g_ce[id];
   CEDESC FAR *d;
   USHORT wi = c->dst_wr;

   if (!c->dst_n || ((c->dst_sw - 1 - wi) & c->dst_mask) == 0)
      return -1;
   d = DESC(c->dst_ring, wi);
   d->addr = phys;
   d->nbytes = 0;
   d->flags = 0;
   c->dst_wr = (USHORT)((wi + 1) & c->dst_mask);
   ce_wr(c, DST_WR_INDEX, (ULONG)c->dst_wr);
   return (int)wi;
}

/* One completed receive: returns its ring index + 1 (nbytes filled in), 0 when none. */
int ce_rx_next(USHORT id, USHORT *nbytes)
{
   CE *c = &g_ce[id];
   CEDESC FAR *d;
   USHORT sw = c->dst_sw, n;

   if (!c->dst_n || sw == c->dst_wr)
      return 0;
   d = DESC(c->dst_ring, sw);
   n = d->nbytes;
   if (n == 0)
      return 0;
   d->nbytes = 0;
   *nbytes = n;
   c->dst_sw = (USHORT)((sw + 1) & c->dst_mask);
   return (int)sw + 1;
}

/* Post every free host buffer of a receive engine. */
void ce_rx_fill(USHORT id)
{
   CE    *c = &g_ce[id];
   USHORT b;
   static ULONG ph;                         /* address passed on: static (SS != DS)                     */
   int    wi;

   for (b = 0; b < c->nbuf; b++) {
      if (!c->bfree[b])
         continue;
      ce_slot(id, b, &ph);
      wi = ce_rx_post(id, ph);
      if (wi < 0)
         break;
      c->dctx[wi] = (UCHAR)b;
      c->bfree[b] = 0;
   }
}

/* Take back the posted (not completed) buffers: the ring is reprogrammed anyway after a reset. */
void ce_rx_revoke(USHORT id)
{
   CE *c = &g_ce[id];
   USHORT k;

   for (k = 0; k < CE_RX_MAX; k++)
      c->bfree[k] = 1;
   c->dst_wr = c->dst_sw;
}

/* ---- diagnostic window (ath10k_pci_diag_read_mem / _write_mem) ---------------------------------- */
static ULONG targ_to_ce(ULONG addr)
{
   U32 v, a;

   v.l = ath_rd(SOC_CORE_BASE + CORE_CTRL_ADDRESS);
   a.l = addr;
   v.w[1] = (USHORT)((v.w[0] & 0x7FF) << 5);               /* (CORE_CTRL & 0x7ff) << 21                 */
   v.w[0] = a.w[0];
   v.w[1] |= (USHORT)(a.w[1] & 0x000F);                     /* region = addr & 0xfffff                   */
   if (addr >= 0x100000UL)
      v.w[1] |= 0x0010;                                     /* bit 20                                    */
   return v.l;
}

static int diag_xfer(ULONG src, ULONG dst, USHORT n)
{
   USHORT i;
   static USHORT nb;

   if (ce_rx_post(CE_DIAG, dst) < 0)
      return -1;
   if (ce_send(CE_DIAG, src, n, 0, 0) < 0)
      return -2;
   for (i = 0; ce_send_done(CE_DIAG) == 0; i++) {
      if (i > 200) return -3;                                /* 10 ms in 50 us steps                      */
      qc_udelay(50);
   }
   for (i = 0; ce_rx_next(CE_DIAG, &nb) == 0; i++) {
      if (i > 200) return -4;
      qc_udelay(50);
   }
   return (nb == n) ? 0 : -5;
}

int diag_read(ULONG addr, UCHAR FAR *dst, USHORT n)
{
   USHORT k;
   int    rc;

   addr = targ_to_ce(addr);
   while (n) {
      k = (n > DIAG_LIMIT) ? DIAG_LIMIT : n;
      rc = diag_xfer(addr, g_diag_buf.phys, k);
      if (rc)
         return rc;
      rt_movmem(dst, DPTR(g_diag_buf), k);
      addr += k;
      dst += k;
      n = (USHORT)(n - k);
   }
   return 0;
}

int diag_write(ULONG addr, UCHAR FAR *src, USHORT n)
{
   USHORT k;
   int    rc;

   addr = targ_to_ce(addr);
   while (n) {
      k = (n > DIAG_LIMIT) ? DIAG_LIMIT : n;
      rt_movmem(DPTR(g_diag_buf), src, k);
      rc = diag_xfer(g_diag_buf.phys, addr, k);
      if (rc)
         return rc;
      addr += k;
      src += k;
      n = (USHORT)(n - k);
   }
   return 0;
}

int g_diag_rc;

/* One 32-bit word of target memory; g_diag_rc = 0 when it was read. */
ULONG diag_rd32(ULONG addr)
{
   static ULONG t;

   t = 0xFFFFFFFFUL;
   g_diag_rc = diag_read(addr, (UCHAR FAR *)&t, 4);
   return t;
}

int diag_wr32(ULONG addr, ULONG v)
{
   static ULONG t;

   t = v;
   return diag_write(addr, (UCHAR FAR *)&t, 4);
}
