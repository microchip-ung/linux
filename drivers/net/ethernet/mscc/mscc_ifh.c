// SPDX-License-Identifier: (GPL-2.0 OR MIT)
//
// Microsemi SoC FDMA IFH driver
//
// Copyright (c) 2019 Microsemi

#include <linux/dmaengine.h>
#include <linux/etherdevice.h>
#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/dmapool.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

#define DEVNAME             "vtss.ifh"
#define ETH_VLAN_TAGSZ     4    /* Size of a 802.1Q VLAN tag */
#define TX_MTU_MIN         64
#define TX_MTU_MAX         2000
#define MTU_DEFAULT        (ETH_FRAME_LEN + ETH_FCS_LEN + (2 * ETH_VLAN_TAGSZ))
#define MTU_MIN            (TX_MTU_MIN + ETH_FCS_LEN + (2 * ETH_VLAN_TAGSZ))
#define MTU_MAX            (TX_MTU_MAX + ETH_FCS_LEN + (2 * ETH_VLAN_TAGSZ))
#define IF_BUFSIZE_JUMBO   10400
#define SGL_MAX            15

#define FDMA_TX_REQUEST_MAX        10
#define FDMA_RX_REQUEST_MAX        10
#define FDMA_XTR_BUFFER_COUNT      SGL_MAX
#define FDMA_XTR_BUFFER_SIZE       2048
#define FDMA_BUFFER_ALIGN          128

#define VITESSE_ENCAP_SIZE     16
#define VITESSE_ETHTYPE_OFFSET 12
#define VITESSE_IFH_ID_OFFSET  15
#define VITESSE_ETHTYPE_MSB    0x88
#define VITESSE_ETHTYPE_LSB    0x80

/* ASM:CFG:PORT_CFG[67] */
#define ASM_CFG_PORT_R_OFF(ridx)                   (0x0060841c + (ridx*4))

/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_R_OFF(ridx)          (0x01030000 + (ridx*4))
/* DEVCPU_QS:XTR:XTR_RD[2] */
#define DEVCPU_QS_XTR_RD_R_OFF(ridx)               (0x01030008 + (ridx*4))
/* DEVCPU_QS:XTR:XTR_FRM_PRUNING[2] */
#define DEVCPU_QS_XTR_FRM_PRUNING_R_OFF(ridx)      (0x01030010 + (ridx*4))
/* DEVCPU_QS:XTR:XTR_FLUSH */
#define DEVCPU_QS_XTR_FLUSH_OFF                    0x01030018
/* DEVCPU_QS:XTR:XTR_DATA_PRESENT */
#define DEVCPU_QS_XTR_DATA_PRESENT_OFF             0x0103001c
/* DEVCPU_QS:XTR:XTR_CFG */
#define DEVCPU_QS_XTR_CFG_OFF                      0x01030020
/* DEVCPU_QS:INJ:INJ_GRP_CFG[2] */
#define DEVCPU_QS_INJ_GRP_CFG_R_OFF(ridx)          (0x01030024 + (ridx*4))
/* DEVCPU_QS:INJ:INJ_WR[2] */
#define DEVCPU_QS_INJ_WR_R_OFF(ridx)               (0x0103002c + (ridx*4))
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_R_OFF(ridx)             (0x01030034 + (ridx*4))
/* DEVCPU_QS:INJ:INJ_STATUS */
#define DEVCPU_QS_INJ_STATUS_OFF                   0x0103003c
/* DEVCPU_QS:INJ:INJ_ERR[2] */
#define DEVCPU_QS_INJ_ERR_R_OFF(ridx)              (0x01030040 + (ridx*4))
/* DEVCPU_QS:INJ:VTSS_DBG */
#define DEVCPU_QS_INJ_VTSS_DBG_OFF                 0x01030048

/* DSM:CFG:DEV_TX_STOP_WM_CFG[67] */
#define DSM_CFG_DEV_TX_STOP_WM_R_OFF(ridx)         (0x00504564 + (ridx*4))
/* DSM:CFG:BUF_CFG[67] */
#define DSM_CFG_BUF_R_OFF(ridx)                    (0x00504014 + (ridx*4))

/* QSYS:SYSTEM:FRM_AGING */
#define QSYS_SYSTEM_FRM_AGING_OFF                  0x010a0108

/* REW:COMMON:IFH_CTRL_CPUVD */
#define REW_COMMON_IFH_CTRL_CPUVD_OFF              0x0165e9d4

/* QFWD:SYSTEM:FRAME_COPY_CFG[12] */
#define QFWD_SYSTEM_FRAME_COPY_CFG_R_OFF(ridx)     (0x010b011c + (ridx*4))
#define QFWD_SYSTEM_FRAME_COPY_CFG_MAX             12


/* ASM:CFG:PORT_CFG[67] */
#define ASM_CFG_PORT_NO_PREAMBLE_ENA             BIT(9)
#define ASM_CFG_PORT_FRM_AGING_DIS               BIT(7)
#define ASM_CFG_PORT_INJ_FORMAT(x)               (((x) << 2) & GENMASK(3, 2))
#define ASM_CFG_PORT_INJ_FORMAT_M                GENMASK(3, 2)
#define ASM_CFG_PORT_INJ_FORMAT_X(x)             (((x) & GENMASK(3, 2)) >> 2)
#define ASM_CFG_PORT_VSTAX2_AWR_ENA              BIT(1)

/* DSM:CFG:DEV_TX_STOP_WM_CFG[67] */
#define DSM_CFG_DEV_TX_STOP_WM(x)                (((x) << 1) & GENMASK(7, 1))
#define DSM_CFG_DEV_TX_STOP_WM_M                 GENMASK(7, 1)
#define DSM_CFG_DEV_TX_STOP_WM_X(x)              (((x) & GENMASK(7, 1)) >> 1)
/* DSM:CFG:BUF_CFG[67] */
#define DSM_CFG_BUF_UNDERFLOW_WATCHDOG_DIS       BIT(11)

/* QFWD:SYSTEM:FRAME_COPY_CFG[12] */
#define QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL(x)   (((x) << 6) & GENMASK(12, 6))
#define QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL_M    GENMASK(12, 6)
#define QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL_X(x) (((x) & GENMASK(12, 6)) >> 6)

/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_MODE(x)            (((x) << 2) & GENMASK(3, 2))
#define DEVCPU_QS_XTR_GRP_CFG_MODE_M             GENMASK(3, 2)
#define DEVCPU_QS_XTR_GRP_CFG_MODE_X(x)          (((x) & GENMASK(3, 2)) >> 2)
/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_STATUS_WORD_POS    BIT(1)
/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_BYTE_SWAP          BIT(0)
/* DEVCPU_QS:XTR:XTR_RD[2] */
#define DEVCPU_QS_XTR_RD_DATA(x)                 ((x) & GENMASK(31, 0))
/* DEVCPU_QS:XTR:XTR_FRM_PRUNING[2] */
#define DEVCPU_QS_XTR_FRM_PRUNING_PRUNE_SIZE(x)  ((x) & GENMASK(7, 0))
/* DEVCPU_QS:XTR:XTR_FLUSH */
#define DEVCPU_QS_XTR_FLUSH(x)                   ((x) & GENMASK(1, 0))
/* DEVCPU_QS:XTR:XTR_DATA_PRESENT */
#define DEVCPU_QS_XTR_DATA_PRESENT(x)            ((x) & GENMASK(1, 0))
/* DEVCPU_QS:XTR:XTR_CFG */
#define DEVCPU_QS_XTR_CFG_DP_WM(x)               (((x) << 7) & GENMASK(11, 7))
#define DEVCPU_QS_XTR_CFG_DP_WM_M                GENMASK(11, 7)
#define DEVCPU_QS_XTR_CFG_DP_WM_X(x)             (((x) & GENMASK(11, 7)) >> 7)
/* DEVCPU_QS:XTR:XTR_CFG */
#define DEVCPU_QS_XTR_CFG_SCH_WM(x)              (((x) << 2) & GENMASK(6, 2))
#define DEVCPU_QS_XTR_CFG_SCH_WM_M               GENMASK(6, 2)
#define DEVCPU_QS_XTR_CFG_SCH_WM_X(x)            (((x) & GENMASK(6, 2)) >> 2)
/* DEVCPU_QS:XTR:XTR_CFG */
#define DEVCPU_QS_XTR_CFG_OFLW_ERR_STICKY(x)     ((x) & GENMASK(1, 0))
#define DEVCPU_QS_XTR_CFG_OFLW_ERR_STICKY_M      GENMASK(1, 0)
/* DEVCPU_QS:INJ:INJ_GRP_CFG[2] */
#define DEVCPU_QS_INJ_GRP_CFG_MODE(x)            (((x) << 2) & GENMASK(3, 2))
#define DEVCPU_QS_INJ_GRP_CFG_MODE_M             GENMASK(3, 2)
#define DEVCPU_QS_INJ_GRP_CFG_MODE_X(x)          (((x) & GENMASK(3, 2)) >> 2)
/* DEVCPU_QS:INJ:INJ_GRP_CFG[2] */
#define DEVCPU_QS_INJ_GRP_CFG_BYTE_SWAP          BIT(0)
/* DEVCPU_QS:INJ:INJ_WR[2] */
#define DEVCPU_QS_INJ_WR_DATA(x)                 ((x) & GENMASK(31, 0))
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_GAP_SIZE(x)           (((x) << 21) & GENMASK(24, 21))
#define DEVCPU_QS_INJ_CTRL_GAP_SIZE_M            GENMASK(24, 21)
#define DEVCPU_QS_INJ_CTRL_GAP_SIZE_X(x)         (((x) & GENMASK(24, 21)) >> 21)
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_ABORT                 BIT(20)
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_EOF                   BIT(19)
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_SOF                   BIT(18)
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_VLD_BYTES(x)          (((x) << 16) & GENMASK(17, 16))
#define DEVCPU_QS_INJ_CTRL_VLD_BYTES_M           GENMASK(17, 16)
#define DEVCPU_QS_INJ_CTRL_VLD_BYTES_X(x)        (((x) & GENMASK(17, 16)) >> 16)
/* DEVCPU_QS:INJ:INJ_STATUS */
#define DEVCPU_QS_INJ_STATUS_WMARK_REACHED_M     GENMASK(5, 4)
#define DEVCPU_QS_INJ_STATUS_WMARK_REACHED_X(x)  (((x) & GENMASK(5, 4)) >> 4)
/* DEVCPU_QS:INJ:INJ_STATUS */
#define DEVCPU_QS_INJ_STATUS_FIFO_RDY_M          GENMASK(3, 2)
#define DEVCPU_QS_INJ_STATUS_FIFO_RDY_X(x)       (((x) & GENMASK(3, 2)) >> 2)
/* DEVCPU_QS:INJ:INJ_STATUS */
#define DEVCPU_QS_INJ_STATUS_IN_PROGRESS(x)      ((x) & GENMASK(1, 0))
#define DEVCPU_QS_INJ_STATUS_IN_PROGRESS_M       GENMASK(1, 0)

#define XTR_EOF_0          0x00000080U
#define XTR_EOF_1          0x01000080U
#define XTR_EOF_2          0x02000080U
#define XTR_EOF_3          0x03000080U
#define XTR_PRUNED         0x04000080U
#define XTR_ABORT          0x05000080U
#define XTR_ESCAPE         0x06000080U
#define XTR_NOT_READY      0x07000080U
#define XTR_VALID_BYTES(x) (4 - (((x) >> 24) & 3))

#define use_fdma(p)                (p->rxdma && p->txdma)

/* Logging options */

/* Testing options */
// #define DEBUG_RX
// #define DEBUG_TX
// #define DEBUG_VCORE_ACCESS

/* Structures */

struct fdma_config {
	u16 first_cpu_port;
	u16 last_cpu_port;
	u8 ifh_id;
	u16 ifh_len;
	u16 ifh_encap_len;
};

struct mscc_ifh_priv;

struct mscc_ifh_tx_request {
	struct list_head node;
	struct mscc_ifh_priv *priv;
	dma_cookie_t cookie;
	unsigned int size;
	unsigned int blocks;
	struct scatterlist sgl[SGL_MAX];
	void *buffer[SGL_MAX];
};

struct mscc_ifh_rx_request {
	struct list_head node;
	struct mscc_ifh_priv *priv;
	dma_cookie_t cookie;
	int idx;
	struct scatterlist sgl[SGL_MAX];
	void *buffer[SGL_MAX];
	int fill_level;
};

struct request_iterator {
	int idx;
	struct mscc_ifh_rx_request *req;
};

struct mscc_ifh_stats_sample {
	u64 min;
	u64 max;
	u64 avg;
};

struct mscc_ifh_stats {
	int coherent_allocs;
	int coherent_allocs_high_mark;
};

struct mscc_ifh_priv {
	struct device *dev;
	struct net_device *netdev;
	const struct fdma_config *config;
	struct dma_chan *rxdma;
	struct dma_chan *txdma;
	struct dma_pool *rx_pool;
	struct list_head free_tx_reqs;
	struct list_head free_rx_reqs;
	struct list_head tx_reqs;
	struct list_head rx_reqs;
	int inj_port;
	void __iomem *io_addr;
	struct tasklet_struct tasklet;
#if defined(FDMA_INJECT_MULTIPLE)
	struct timer_list injection_timer;
	spinlock_t rx_lock;
	u32 tx_req_fill_level;
#endif
	u32 rx_req_fill_level;
	u32 injects;
	u32 tx_req_interval;
	struct dentry *rootfs;
	struct mscc_ifh_stats stats;
	spinlock_t tx_lock;
};

/* Module globals */
static int mscc_ifh_module_retry = 0;

/* Forward Declarations */
static bool mscc_ifh_prepare_rx_request(struct mscc_ifh_priv *priv);

/* Implementation */

struct prof_sample {
	u64 min;
	u64 max;
	u64 sum;
};

struct prof_stat {
	int samples;
	char *name;
	int count;
	u64 last;
	struct prof_sample sample;
	struct prof_sample result;
};


static struct prof_stat profstats[8];

static void prof_sample_init(struct prof_stat *stat, int samples, char *name)
{
	stat->samples = samples;
	stat->count = stat->samples;
	stat->name = name;
	stat->sample.min = ~0;
	stat->sample.max = 0;
	stat->sample.sum = 0;
	stat->result.min = 0;
	stat->result.max = 0;
	stat->result.sum = 0;
}

static void prof_sample_begin(struct prof_stat *stat)
{
	stat->last = ktime_get_ns();
}

static void prof_sample_reset(struct prof_stat *stat)
{
	stat->count = stat->samples;
	stat->sample.min = ~0;
	stat->sample.max = 0;
	stat->sample.sum = 0;
}

static void prof_sample_end(struct prof_stat *stat)
{
	u64 diff = ktime_get_ns() - stat->last;
	stat->sample.sum += diff;
	if (diff > stat->sample.max) {
		stat->sample.max = diff;
	}
	if (diff < stat->sample.min) {
		stat->sample.min = diff;
	}
	if (--stat->count == 0) {
		memcpy(&stat->result, &stat->sample, sizeof(struct prof_sample));
		prof_sample_reset(stat);
	}
}

#if defined(DEBUG) || defined(DEBUG_TX) || defined(DEBUG_RX) || defined(DEBUG_VCORE_ACCESS)
static void mscc_ifh_dump_byte_array(const char *name, const u8 *buf, size_t len)
{
	char prefix[64];
	if (!buf) {
		return;
	}
	snprintf(prefix, sizeof(prefix), "%.50s[%zu]: ", name, len);
	print_hex_dump(KERN_NOTICE, prefix, DUMP_PREFIX_OFFSET, 16, 1, buf, len,
		       false);
}
#endif

static void mscc_ifh_writel(struct mscc_ifh_priv *priv, u32 reg, u32 data)
{
	void __iomem *addr = priv->io_addr + reg;
#ifdef DEBUG_VCORE_ACCESS
	pr_debug("%s:%d %s: addr 0x%px, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		addr,
		data);
#endif
	writel(data, addr);
}

static u32 mscc_ifh_readl(struct mscc_ifh_priv *priv, u32 reg)
{
	void __iomem *addr = priv->io_addr + reg;
	u32 data = readl(addr);
#ifdef DEBUG_VCORE_ACCESS
	pr_debug("%s:%d %s: addr 0x%px, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		addr,
		data);
#endif
	return data;
}

static int mscc_ifh_xtr_word(struct mscc_ifh_priv *priv, bool first_word,
			     u32 *rval, u32 *bytes_valid)
{
	u32 val;

	*bytes_valid = 0;
	val = mscc_ifh_readl(priv, DEVCPU_QS_XTR_RD_R_OFF(priv->inj_port));
	if (val == XTR_NOT_READY) {
		/*
		 * XTR_NOT_READY means two different things depending on whether
		 * this is the first word read of a frame or after at least one
		 * word has been read. When the first word, the group is empty,
		 * and we return an error. Otherwise we have to wait for the
		 * FIFO to have received some more data.
		 */
		if (first_word) {
			return 2; /* Error */
		}
		do {
			val = mscc_ifh_readl(priv,
				DEVCPU_QS_XTR_RD_R_OFF(priv->inj_port));
		} while (val == XTR_NOT_READY);
	}

	switch (val) {
	case XTR_ABORT:
		return 2; /* Error */
	case XTR_EOF_0:
	case XTR_EOF_1:
	case XTR_EOF_2:
	case XTR_EOF_3:
	case XTR_PRUNED:
		*bytes_valid = XTR_VALID_BYTES(val);
		val = mscc_ifh_readl(priv, DEVCPU_QS_XTR_RD_R_OFF(priv->inj_port));
		if (val == XTR_ESCAPE) {
			*rval = mscc_ifh_readl(priv,
					DEVCPU_QS_XTR_RD_R_OFF(priv->inj_port));
		} else {
			*rval = val;
		}
		return 1; /* EOF */
	case XTR_ESCAPE:
		*rval = mscc_ifh_readl(priv,
				DEVCPU_QS_XTR_RD_R_OFF(priv->inj_port));
		*bytes_valid = 4;
		return 0;
	default:
		*rval = val;
		*bytes_valid = 4;
		return 0;
	}
}

static void mscc_ifh_xtr_tasklet(unsigned long arg)
{
	struct mscc_ifh_priv *priv = (struct mscc_ifh_priv *)arg;
	struct sk_buff *skb;
	u32 *wbuf;
	int buf_len = 4*1024;
	int header_word_size;
	bool done = 0;
	u32 val;
	u32 bytes_valid;
	u32 size = 0;
	int idx;
	int mask = BIT(priv->inj_port);

	pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);
	skb = netdev_alloc_skb(priv->netdev, buf_len);
	if (unlikely(!skb)) {
		netdev_err(priv->netdev, "Unable to allocate sk_buff\n");
		goto error;
	}

	wbuf = (u32 *)skb->data;

	size = priv->config->ifh_encap_len + priv->config->ifh_len;
	header_word_size = size / 4;

	for (idx = 0; idx < header_word_size; ++idx) {
		if (mscc_ifh_xtr_word(priv, 1, &val, &bytes_valid) != 0) {
			goto error;
		}
		*wbuf++ = val;
	}
#ifdef DEBUG
	mscc_ifh_dump_byte_array("RxHeader", skb->data, header_word_size * 4);
#endif
	/* Read the rest of the frame */
	while (!done && buf_len >= 4) {
		u32 result = mscc_ifh_xtr_word(priv, 0, &val, &bytes_valid);
		if (result == 2) {
			pr_debug("%s:%d %s: abort\n", __FILE__, __LINE__, __func__);
			goto error;
		}
		*wbuf++ = val;
		buf_len -= bytes_valid;
		size += bytes_valid;
		done = result == 1;
	}
	if (!done) {
		goto error;
	}
	/* Skip to the IFH encap IFH type */
	skb_put(skb, size);
#ifdef DEBUG
	mscc_ifh_dump_byte_array("RxSkb", skb->data, skb->len);
#endif
	skb->protocol = eth_type_trans(skb, priv->netdev);
	netif_rx(skb);
	priv->netdev->stats.rx_bytes += size;
	priv->netdev->stats.rx_packets++;
	return;

error:
	pr_debug("%s:%d %s: start error reading\n", __FILE__, __LINE__, __func__);
	while (mscc_ifh_readl(priv, DEVCPU_QS_XTR_DATA_PRESENT_OFF) & mask) {
		mscc_ifh_readl(priv, DEVCPU_QS_XTR_RD_R_OFF(priv->inj_port));
	}
	mscc_ifh_writel(priv, DEVCPU_QS_XTR_FLUSH_OFF, mask);
	mscc_ifh_writel(priv, DEVCPU_QS_XTR_FLUSH_OFF, 0);
	pr_debug("%s:%d %s: end error reading\n", __FILE__, __LINE__, __func__);
}


static irqreturn_t mscc_ifh_xtr_irq_handler(int irq, void *arg)
{
	struct mscc_ifh_priv *priv = arg;
	int mask = BIT(priv->inj_port);

	pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);
	if (!(mscc_ifh_readl(priv, DEVCPU_QS_XTR_DATA_PRESENT_OFF) & mask)) {
		return IRQ_NONE;
	}

	tasklet_schedule(&priv->tasklet);

	return IRQ_HANDLED;
}

static int mscc_ifh_transmit_register_based(struct mscc_ifh_priv *priv,
					  struct sk_buff *skb)
{
	u32 status;
	u32 mask = BIT(priv->inj_port);
	int idx;
	int count, last;
	u32 *buffer;

	pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);
#ifdef DEBUG
	mscc_ifh_dump_byte_array("TxSkb", skb->data, skb->len);
#endif
	status = mscc_ifh_readl(priv, DEVCPU_QS_INJ_STATUS_OFF);
	if (!(DEVCPU_QS_INJ_STATUS_FIFO_RDY_X(status) & mask)) {
		pr_debug("%s:%d %s: FIFO not ready\n",
			 __FILE__, __LINE__, __func__);
		return -1;
	}
	if (DEVCPU_QS_INJ_STATUS_WMARK_REACHED_X(status) & mask) {
		pr_debug("%s:%d %s: Watermark reached\n",
			 __FILE__, __LINE__, __func__);
		return -1;
	}
	/* Indicate SOF */
	mscc_ifh_writel(priv, DEVCPU_QS_INJ_CTRL_R_OFF(priv->inj_port), \
			DEVCPU_QS_INJ_CTRL_GAP_SIZE(1) | DEVCPU_QS_INJ_CTRL_SOF);

	/* Write frame words and round up to whole words (32 bit) */
	count = ((skb->len + 3) / 4);
	last = skb->len % 4;
	buffer = (u32 *)skb->data;
	for (idx = 0; idx < count; idx++, skb->data += 4) {
		pr_debug("%s:%d %s: Data: %08x\n",
			 __FILE__, __LINE__, __func__, *buffer);
		mscc_ifh_writel(priv, DEVCPU_QS_INJ_WR_R_OFF(priv->inj_port),
				*buffer++);
	}

	/* Add padding to reach the minimum packet size for the VCore */
	while (idx < (60 / 4)) {
		mscc_ifh_writel(priv, DEVCPU_QS_INJ_WR_R_OFF(priv->inj_port), 0);
		idx++;
	}
	/* Indicate EOF and valid bytes in last word */
	mscc_ifh_writel(priv, DEVCPU_QS_INJ_CTRL_R_OFF(priv->inj_port), \
			DEVCPU_QS_INJ_CTRL_GAP_SIZE(1) |
			DEVCPU_QS_INJ_CTRL_VLD_BYTES(skb->len < 60 ? 0 : last) |
			DEVCPU_QS_INJ_CTRL_EOF);
	/* Add dummy CRC */
	mscc_ifh_writel(priv, DEVCPU_QS_INJ_WR_R_OFF(priv->inj_port), 0);
	idx++;
	pr_debug("%s:%d %s: written: %d bytes\n",
		 __FILE__, __LINE__, __func__, idx * 4);

	return 0;
}

static void mscc_ifh_register_based_injection(struct mscc_ifh_priv *priv)
{
	unsigned int chipport = priv->inj_port + priv->config->first_cpu_port;
	u32 value, mask;
	int idx;

	/* Allow manual injection via DEVCPU_QS registers, and byte swap
	 * these registes endianness.
	 */
	mscc_ifh_writel(priv, DEVCPU_QS_INJ_GRP_CFG_R_OFF(priv->inj_port),
			DEVCPU_QS_INJ_GRP_CFG_BYTE_SWAP |
			DEVCPU_QS_INJ_GRP_CFG_MODE(1));
	mscc_ifh_writel(priv, DEVCPU_QS_XTR_GRP_CFG_R_OFF(priv->inj_port),
			DEVCPU_QS_XTR_GRP_CFG_BYTE_SWAP |
			DEVCPU_QS_XTR_GRP_CFG_MODE(1));

	/* INJ CPU Port Format: IFH without prefix, no preamble, no VSTAX2 aware */
	value = mscc_ifh_readl(priv, ASM_CFG_PORT_R_OFF(chipport));
	value &= ~ASM_CFG_PORT_VSTAX2_AWR_ENA;
	value |= ASM_CFG_PORT_INJ_FORMAT(1);
#ifdef DEBUG_VCORE_ACCESS
	value |= ASM_CFG_PORT_FRM_AGING_DIS;
#endif
	value |= ASM_CFG_PORT_NO_PREAMBLE_ENA;
	mscc_ifh_writel(priv, ASM_CFG_PORT_R_OFF(chipport), value);

	/* XTR CPU Port Format: IFH with prefix */
	value = mscc_ifh_readl(priv, REW_COMMON_IFH_CTRL_CPUVD_OFF);
	mask = ~(0x3 << (priv->inj_port << 1));
	value &= mask;
	value |= 2 << (priv->inj_port << 1);
	mscc_ifh_writel(priv, REW_COMMON_IFH_CTRL_CPUVD_OFF, value);

	/* XTR: Set Disassembler Stop Watermark level */
	value = mscc_ifh_readl(priv, DSM_CFG_DEV_TX_STOP_WM_R_OFF(chipport));
	value &= ~DSM_CFG_DEV_TX_STOP_WM_M;
	value |= DSM_CFG_DEV_TX_STOP_WM(10);  /* Watermark at 10 packets */
	mscc_ifh_writel(priv, DSM_CFG_DEV_TX_STOP_WM_R_OFF(chipport), value);

	/* Disable Disassembler buffer underrun watchdog (to avoid truncated
	 * packets in XTR)
	 */
	value = mscc_ifh_readl(priv, DSM_CFG_BUF_R_OFF(chipport));
	value |= DSM_CFG_BUF_UNDERFLOW_WATCHDOG_DIS;
	mscc_ifh_writel(priv, DSM_CFG_BUF_R_OFF(chipport), value);

	/* Copy frames in all fwd queues to the regbased chipport */
	for (idx = 0; idx < QFWD_SYSTEM_FRAME_COPY_CFG_MAX; ++idx) {
		value = mscc_ifh_readl(priv, QFWD_SYSTEM_FRAME_COPY_CFG_R_OFF(idx));
		value &= ~QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL_M;
		value |= QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL(chipport);
		mscc_ifh_writel(priv, QFWD_SYSTEM_FRAME_COPY_CFG_R_OFF(idx), value);
	}
}

static struct mscc_ifh_tx_request *mscc_ifh_prepare_tx_request(
	struct mscc_ifh_priv *priv,
	struct sk_buff *skb)
{
	struct mscc_ifh_tx_request *req;
	struct scatterlist *sg;
	int idx = 0;
	int fidx;
	int blocks;
	unsigned int bufsize;
	unsigned int size;
	dma_addr_t phys;
	void *buffer;

	blocks = skb_shinfo(skb)->nr_frags + 1;
	pr_debug("%s:%d %s: skb: frags: %d, size: %u, headsize: %u\n",
		__FILE__, __LINE__, __func__,
		skb_shinfo(skb)->nr_frags,
		skb->len,
		skb_headlen(skb));
	if (blocks > SGL_MAX) {
		pr_err("%s:%d %s: too many blocks\n",
		       __FILE__, __LINE__, __func__);
		return NULL;
	}
	req = list_first_entry_or_null(&priv->free_tx_reqs, struct mscc_ifh_tx_request,
				       node);
	if (!req) {
		return NULL;
	}
	memset(req->sgl, 0, sizeof(req->sgl));
	memset(req->buffer, 0, sizeof(req->buffer));
	req->priv = priv;
	req->cookie = 0;
	req->size = skb->len;
	req->blocks = blocks;
	sg_init_table(req->sgl, blocks);
	sg = req->sgl;

	size = skb_headlen(skb); /* Includes the IFH and FCS */
	bufsize = round_up(size, 64);
	buffer = dma_alloc_coherent(priv->txdma->device->dev, bufsize,
					      &phys, GFP_ATOMIC);
	memset(buffer, 0, bufsize);
	if (++priv->stats.coherent_allocs >
	    priv->stats.coherent_allocs_high_mark) {
		priv->stats.coherent_allocs_high_mark =
			priv->stats.coherent_allocs;
	}
	prof_sample_begin(&profstats[6]);
	memcpy(buffer, skb->data, size - ETH_FCS_LEN);
	prof_sample_end(&profstats[6]);
	sg_dma_address(sg) = phys;
	sg_dma_len(sg) = size;
	req->buffer[idx] = buffer;
	sg = sg_next(sg);
	/* Add SKB fragments if available */
	idx++;
	for (fidx = 0; fidx < skb_shinfo(skb)->nr_frags; fidx++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[fidx];

		size = skb_frag_size(frag);
		if (!size) {
			sg_dma_address(sg) = 0;
			sg_dma_len(sg) = 0;
			continue;
		}
		bufsize = round_up(size, 64);
		buffer = dma_alloc_coherent(priv->txdma->device->dev, bufsize,
					    &phys, GFP_ATOMIC);
		memset(buffer, 0, bufsize);
		if (++priv->stats.coherent_allocs >
		    priv->stats.coherent_allocs_high_mark) {
			priv->stats.coherent_allocs_high_mark =
				priv->stats.coherent_allocs;
		}
		memcpy(buffer, frag, size);
		sg_dma_address(sg) = (dma_addr_t)phys;
		sg_dma_len(sg) = size;
		req->buffer[idx] = buffer;
		idx++;
		sg = sg_next(sg);
	}
	list_move_tail(&req->node, &priv->tx_reqs);
	return req;
}

static void mscc_ifh_close_tx_request(struct mscc_ifh_priv *priv,
				   struct mscc_ifh_tx_request *req)
{
	struct scatterlist *sg;
	int idx;

	pr_debug("%s:%d %s: [C%u]\n",
		__FILE__, __LINE__, __func__, req->cookie);

	for_each_sg(req->sgl, sg, req->blocks, idx) {
		pr_debug("%s:%d %s: %u [C%u] %u 0x%px 0x%llx\n",
			__FILE__, __LINE__, __func__,
			idx,
			req->cookie,
			sg_dma_len(sg),
			req->buffer[idx],
			sg_dma_address(sg));
		dma_free_coherent(priv->txdma->device->dev,
				  sg_dma_len(sg),
				  req->buffer[idx],
				  sg_dma_address(sg));
		--priv->stats.coherent_allocs;
	}
	list_move_tail(&req->node, &priv->free_tx_reqs);
}

static void mscc_ifh_init_iterator(struct request_iterator *iter,
	int idx,
	struct mscc_ifh_rx_request *req)
{
	iter->idx = idx;
	iter->req = req;
	if (idx >= req->fill_level) {
		iter->idx = idx % req->fill_level;
		iter->req = list_next_entry(iter->req, node);
	}
	pr_debug("%s:%d %s: [C%u,I%u]\n",
		__FILE__, __LINE__, __func__, iter->req->cookie, iter->idx);
}

static struct mscc_ifh_rx_request *next_block(struct request_iterator *iter)
{
	struct mscc_ifh_rx_request *req = NULL;

	iter->idx++;
	if (iter->idx == iter->req->fill_level) {
		req = iter->req;
		iter->idx = 0;
		iter->req = list_next_entry(iter->req, node);
	}
	pr_debug("%s:%d %s: [C%u,I%u], req: %u\n",
		__FILE__, __LINE__, __func__, iter->req->cookie, iter->idx,
		req != NULL);
	return req;
}

static bool mscc_ifh_reached(struct request_iterator *iter,
	struct request_iterator *max)
{
	pr_debug("%s:%d %s: %u\n", __FILE__, __LINE__, __func__,
		iter->req == max->req && iter->idx == max->idx);
	return iter->req == max->req && iter->idx == max->idx;
}

static void *mscc_ifh_get_block_data(struct mscc_ifh_priv *priv,
				     struct request_iterator *iter)
{
	pr_debug("%s:%d %s: [C%u,I%u]: 0x%px\n",
		__FILE__, __LINE__, __func__,
		iter->req->cookie, iter->idx,
		iter->req->buffer[iter->idx]);
	return iter->req->buffer[iter->idx];
}

static struct sk_buff *mscc_ifh_create_receive_skb(
	struct mscc_ifh_priv *priv,
	struct request_iterator *iter,
	struct request_iterator *max,
	int size,
	int block_bytes)
{
	struct sk_buff *skb = 0;
	struct mscc_ifh_rx_request *done_req;
	u8 *skbdata;
	void *data = NULL;
	int block_size;

	prof_sample_begin(&profstats[4]);
	skb = netdev_alloc_skb(priv->netdev, block_bytes);
	prof_sample_end(&profstats[4]);
	if (!skb) {
		pr_err("%s:%d %s: no skb: %u bytes\n",
		       __FILE__, __LINE__, __func__, block_bytes);
		return NULL;
	}
	skbdata = skb->data;
	skb_put(skb, size);
	pr_debug("%s:%d %s: skb: len: %d, data: 0x%px\n",
		 __FILE__, __LINE__, __func__, skb->len, skb->data);
	while (!mscc_ifh_reached(iter, max)) {
		data = mscc_ifh_get_block_data(priv, iter);
		block_size = min(size, FDMA_XTR_BUFFER_SIZE);
#ifdef DEBUG
		mscc_ifh_dump_byte_array("RxData", data, block_size);
#endif
		pr_debug("%s:%d %s: copy: len: %d, data: 0x%px\n",
			 __FILE__, __LINE__, __func__,
			 block_size, data);
		prof_sample_begin(&profstats[5]);
		memcpy(skbdata, data, block_size);
		prof_sample_end(&profstats[5]);
		done_req = next_block(iter);
		if (done_req) {
			pr_debug("%s:%d %s: done: [C:%u]\n",
				 __FILE__, __LINE__, __func__,
				 done_req->cookie);
			list_move_tail(&done_req->node, &priv->free_rx_reqs);
		}
		skbdata += FDMA_XTR_BUFFER_SIZE;
		size -= block_size;
	}
	if (!data) {
		pr_err("%s:%d %s: did not copy: [C:%u,I%u]\n",
			 __FILE__, __LINE__, __func__,
			 iter->req->cookie, iter->idx);
	}
	return skb;
}

#define IFH_OFF  2  /* We have IFH ID first (2 bytes) */

static bool mscc_ifh_drop_received_skb(struct mscc_ifh_priv *priv,
				       struct sk_buff *skb)
{
	/*
	 * We discard all frames that are transmitted by the CPU itself.
	 * In reality, we should check whether the frame was sFlow
	 * marked and only drop those that were, but the sflow_marking
	 * bit of the IFH will not be set in case the CPU originally
	 * transmitted the frame directly to a port (rather than onto
	 * a VLAN, a.k.a. switched).
	 */
	u16 src_chip_port = ((skb->data[IFH_OFF + 29] & 0x1f) << 2) |
			     ((skb->data[IFH_OFF + 30] & 0xc0) >> 6);

#ifdef DEBUG
	pr_debug("%s:%d %s: src chip port: %d\n", __FILE__, __LINE__, __func__,
		src_chip_port);
	mscc_ifh_dump_byte_array("IFH", skb->data + IFH_OFF, priv->config->ifh_len);
#endif
	return (src_chip_port >= priv->config->first_cpu_port &&
		src_chip_port <= priv->config->last_cpu_port);
}

static void mscc_ifh_receive_cb(void *data,
	const struct dmaengine_result *result)
{
	struct mscc_ifh_rx_request *req = data;
	struct mscc_ifh_priv *priv;
	int used_blocks;
	struct sk_buff *skb;
	struct request_iterator cur;
	struct request_iterator next;
	int next_sof;
	int packet_size;

	prof_sample_begin(&profstats[0]);
	pr_debug("%s:%d %s: result: %u, residue: %u\n",
		 __FILE__, __LINE__, __func__,
		 result->result, result->residue);
	if (!req) {
		pr_err("%s:%d %s: no request\n",
		       __FILE__, __LINE__, __func__);
		return;
	}
	priv = req->priv;
	/* Get the packet size (includes IFH and FCS) */
	packet_size = result->residue;
	used_blocks = DIV_ROUND_UP(packet_size, FDMA_XTR_BUFFER_SIZE);
	next_sof = req->idx + used_blocks;
	mscc_ifh_init_iterator(&cur, req->idx, req);
	mscc_ifh_init_iterator(&next, next_sof, req);
	pr_debug("%s:%d %s: from: [C%u,I%u] to: [C%u,I%u]  size: %u, blocks: %u\n",
		 __FILE__, __LINE__, __func__,
		 cur.req->cookie,
		 cur.idx,
		 next.req->cookie,
		 next.idx,
		 packet_size,
		 used_blocks);
	if (req->idx == 0) {
		struct mscc_ifh_rx_request *prev = list_prev_entry(req, node);
		static int cookie = 0;

		if (prev->idx != req->fill_level) {
			if (cookie != prev->cookie) {
				pr_info("%s:%d %s: going from: [C%u,I%u] to: [C%u,I%u]\n",
				       __FILE__, __LINE__, __func__,
				       prev->cookie,
				       prev->idx,
				       req->cookie,
				       req->idx);
				cookie = prev->cookie;
			}
		}
	}

	if (result->result != DMA_TRANS_NOERROR) {
		goto err;
	}
	if (used_blocks == 0) {
		goto err;
	}
	prof_sample_begin(&profstats[1]);
	skb = mscc_ifh_create_receive_skb(priv, &cur,
					  &next, packet_size,
					  used_blocks * FDMA_XTR_BUFFER_SIZE);
	prof_sample_end(&profstats[1]);
	if (skb) {
		pr_debug("%s:%d %s: skb: len: %d, data: 0x%px\n",
			 __FILE__, __LINE__, __func__, skb->len, skb->data);
#ifdef DEBUG_RX
		mscc_ifh_dump_byte_array("IFH RxData", data,
					 min(size, iter->req->size));
#endif
		skb->protocol = eth_type_trans(skb, priv->netdev);
		pr_debug("%s:%d %s: skb: len: %d, data: 0x%px, used_blocks: %u\n",
			 __FILE__, __LINE__, __func__,
			 skb->len, skb->data, used_blocks);
		/* Save state for next packet */
		next.req->idx = next.idx;
	} else {
		pr_err("%s:%d %s: could not create skb: [C%u,I%u]"
		       "result: %u, size: %u\n",
		       __FILE__, __LINE__, __func__,
		       cur.req->cookie,
		       cur.idx,
		       result->result, result->residue);
		goto err;
	}
	/* Prepare more buffers */
	prof_sample_begin(&profstats[2]);
	mscc_ifh_prepare_rx_request(priv);
	prof_sample_end(&profstats[2]);

	if (mscc_ifh_drop_received_skb(priv, skb)) {
		priv->netdev->stats.rx_dropped++;
		pr_debug("%s:%d %s: RX Dropped\n",
		 __FILE__, __LINE__, __func__);
		dev_kfree_skb_any(skb);
		return;
	}

	priv->netdev->stats.rx_packets++;
	priv->netdev->stats.rx_bytes += skb->len;

	pr_debug("%s:%d %s: RX Done: packets: %lu, bytes: %lu\n",
		 __FILE__, __LINE__, __func__,
		 priv->netdev->stats.rx_packets,
		 priv->netdev->stats.rx_bytes);

#ifdef DEBUG
	mscc_ifh_dump_byte_array("RxSkb", skb->data, skb->len);
#endif
	prof_sample_begin(&profstats[3]);
	netif_rx(skb);
	prof_sample_end(&profstats[3]);
	prof_sample_end(&profstats[0]);
	return;
err:
	pr_err("%s:%d %s: error: %u, [C%u,I%u]\n",
	       __FILE__, __LINE__, __func__,
	       result->result, next.req->cookie, next.idx);
	/* Save state for next packet */
	req->idx = next.idx;
	/* TODO: count unreceived bytes/packet */
}

static void mscc_ifh_init_rx_request(struct mscc_ifh_priv *priv,
				 struct mscc_ifh_rx_request *req,
				 int size)
{
	struct scatterlist *sg;
	int idx;
	dma_addr_t phys;

	pr_debug("%s:%d %s: rx request: 0x%px\n",
		 __FILE__, __LINE__, __func__,
		 req);
	req->priv = priv;
	req->cookie = 0;
	sg_init_table(req->sgl, FDMA_XTR_BUFFER_COUNT);
	for_each_sg(req->sgl, sg, FDMA_XTR_BUFFER_COUNT, idx) {
		req->buffer[idx] = dma_pool_zalloc(priv->rx_pool, GFP_KERNEL, &phys);
		sg_dma_address(sg) = phys;
		sg_dma_len(sg) = size;
		pr_debug("%s:%d %s: buffer[%02u]: 0x%llx\n",
			 __FILE__, __LINE__, __func__,
			 idx, phys);
	}
}

static bool mscc_ifh_prepare_rx_request(struct mscc_ifh_priv *priv)
{
	struct dma_async_tx_descriptor *txd;
	struct mscc_ifh_rx_request *req;

	while (true) {
		req = list_first_entry_or_null(&priv->free_rx_reqs,
					       struct mscc_ifh_rx_request, node);
		if (!req) {
			return false;
		}
		pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);
		req->cookie = 0;
		req->idx = 0;
		req->fill_level = priv->rx_req_fill_level;
		txd = dmaengine_prep_slave_sg(priv->rxdma, req->sgl, req->fill_level,
					      DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);

		if (!txd) {
			dev_err(priv->dev, "Could not get RX Descriptor\n");
			goto error;
		}

		txd->callback_param = req;
		txd->callback_result = mscc_ifh_receive_cb;
		req->cookie = dmaengine_submit(txd);
		if (req->cookie < DMA_MIN_COOKIE) {
			dev_err(priv->dev, "Submit failed\n");
			goto error;
		}
		pr_debug("%s:%d %s: Issue: txd: 0x%px, C%u, Submitted: %d\n",
			 __FILE__, __LINE__, __func__,
			 txd,
			 txd->cookie,
			 req->cookie);
		dma_async_issue_pending(priv->rxdma);
		list_move_tail(&req->node, &priv->rx_reqs);
	}
	return true;

error:
	pr_err("%s:%d %s: error\n", __FILE__, __LINE__, __func__);
	return false;
}


static void mscc_ifh_transmit_cb(void *data,
	const struct dmaengine_result *result)
{
	struct mscc_ifh_tx_request *req = data;
	enum dma_status status;
	struct dma_tx_state state;
	struct mscc_ifh_priv *priv;

	pr_debug("%s:%d %s: result: %u, residue: %u\n",
		 __FILE__, __LINE__, __func__,
		 result->result, result->residue);
	if (!req) {
		pr_err("%s:%d %s: no request\n",
		       __FILE__, __LINE__, __func__);
		return;
	}
	priv = req->priv;
	spin_lock(&priv->tx_lock);
	if (result->result != DMA_TRANS_NOERROR) {
		pr_err("%s:%d %s: error: %u, [C%u]\n",
			__FILE__, __LINE__, __func__,
			result->result, req->cookie);
	} else {
		status = dmaengine_tx_status(priv->txdma, req->cookie, &state);
		pr_debug("%s:%d %s: status %d, state: last: %u, used: %u, "
			 "residue: %u\n",
			__FILE__, __LINE__, __func__,
			status, state.last, state.used, state.residue);
		/* TODO: req->size should cover the whole thing */
		priv->netdev->stats.tx_packets++;
		priv->netdev->stats.tx_bytes += req->blocks * req->size;
	}
	mscc_ifh_close_tx_request(priv, req);
	spin_unlock(&priv->tx_lock);

	pr_debug("%s:%d %s: TX Done: packets: %lu, bytes: %lu\n",
		__FILE__, __LINE__, __func__,
		priv->netdev->stats.tx_packets,
		priv->netdev->stats.tx_bytes);
}

#if defined(FDMA_INJECT_MULTIPLE)
static void mscc_ifh_fdma_issue_tx(struct timer_list *tmr)
{
	struct mscc_ifh_priv *priv = from_timer(priv, tmr, injection_timer);

	pr_debug("%s:%d %s: issuing\n",
		 __FILE__, __LINE__, __func__);
	spin_lock(&priv->rx_lock);
	dma_async_issue_pending(priv->txdma);
	priv->injects = 0;
	spin_unlock(&priv->rx_lock);
}
#endif

static int mscc_ifh_transmit_fdma(struct mscc_ifh_priv *priv, struct sk_buff *skb)
{
	struct dma_async_tx_descriptor *txd;
	struct mscc_ifh_tx_request *req;
#if defined(FDMA_INJECT_MULTIPLE)
	unsigned long expires;
#endif

	prof_sample_end(&profstats[7]); /* Sample from last time */
	prof_sample_begin(&profstats[7]);
	req = mscc_ifh_prepare_tx_request(priv, skb);
	if (!req) {
		pr_debug("%s:%d %s: request prepare error\n",
			__FILE__, __LINE__, __func__);
		return -1;
	}
	txd = dmaengine_prep_slave_sg(priv->txdma, req->sgl, req->blocks,
				      DMA_MEM_TO_DEV, 0);

	if (!txd) {
		dev_err(priv->dev, "Could not get TX Descriptor\n");
		goto error;
	}

	txd->callback_param = req;
	txd->callback_result = mscc_ifh_transmit_cb;
	req->cookie = dmaengine_submit(txd);
	if (req->cookie < DMA_MIN_COOKIE) {
		dev_err(priv->dev, "Submit failed\n");
		goto error;
	}

#if defined(FDMA_INJECT_MULTIPLE)
	spin_lock(&priv->rx_lock);
	if (++priv->injects >= priv->tx_req_fill_level) {
		pr_debug("%s:%d %s: Direct Issue: txd: 0x%px, Submitted: %d\n",
			__FILE__, __LINE__, __func__,
			txd,
			req->cookie);
		dma_async_issue_pending(priv->txdma);
		priv->injects = 0;
	} else {
		pr_debug("%s:%d %s: Timed Issue: txd: 0x%px, Submitted: %d\n",
			__FILE__, __LINE__, __func__,
			txd,
			req->cookie);

		expires = jiffies + usecs_to_jiffies(priv->tx_req_interval);
		mod_timer(&priv->injection_timer, expires);
	}
	spin_unlock(&priv->rx_lock);
#else
	dma_async_issue_pending(priv->txdma);
#endif
	return 0;

error:
	pr_err("%s:%d %s: error, close request\n", __FILE__, __LINE__, __func__);
	mscc_ifh_close_tx_request(priv, req);
	return -1;
}

static int mscc_ifh_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}


static int mscc_ifh_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static int mscc_ifh_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mscc_ifh_priv *priv = netdev_priv(dev);
	int ret;

	spin_lock(&priv->tx_lock);
	pr_debug("%s:%d %s: %s: Transmit %d bytes: 0x%px\n",
		 __FILE__, __LINE__, __func__,
		 dev->name, skb->len, skb->data);

	/* Check encapsulation header (MAC + VLAN Tag) */
	if (!(skb->data[VITESSE_ETHTYPE_OFFSET] == VITESSE_ETHTYPE_MSB &&
		skb->data[VITESSE_ETHTYPE_OFFSET+1] == VITESSE_ETHTYPE_LSB &&
		skb->data[VITESSE_IFH_ID_OFFSET] == priv->config->ifh_id)) {
		pr_debug("%s:%d %s: encapsulation check failed: %d, %d, %d\n",
		       __FILE__, __LINE__, __func__,
		       skb->data[VITESSE_ETHTYPE_OFFSET] == VITESSE_ETHTYPE_MSB,
		       skb->data[VITESSE_ETHTYPE_OFFSET+1] == VITESSE_ETHTYPE_LSB,
		       skb->data[VITESSE_IFH_ID_OFFSET] == priv->config->ifh_id);
		/* Count as dropped */
		priv->netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		spin_unlock(&priv->tx_lock);
		return NETDEV_TX_OK;
	}
	/* Skip past the encapsulation header */
	skb_pull_inline(skb, VITESSE_ENCAP_SIZE);

#ifdef DEBUG_TX
	mscc_ifh_dump_byte_array("TxSkb", skb->data, skb->len);
#endif
	/* Include the FCS */
	skb->len += ETH_FCS_LEN;

	if (use_fdma(priv)) {
		ret = mscc_ifh_transmit_fdma(priv, skb);
	} else {
		ret = mscc_ifh_transmit_register_based(priv, skb);
	}
	/* Exclude the FCS */
	skb->len -= ETH_FCS_LEN;
	if (ret) {
		goto out;
	}

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;
	skb_tx_timestamp(skb);
	dev_kfree_skb_any(skb);
	spin_unlock(&priv->tx_lock);
	return NETDEV_TX_OK;
out:
	spin_unlock(&priv->tx_lock);
	return NETDEV_TX_BUSY;

}

static int mscc_ifh_change_mtu(struct net_device *dev, int new_mtu)
{
	struct mscc_ifh_priv *priv = netdev_priv(dev);

	if (new_mtu < (TX_MTU_MIN + priv->config->ifh_len) ||
	    new_mtu > (TX_MTU_MAX + priv->config->ifh_len)) {
		return -EINVAL;
	}
	dev->mtu = new_mtu;
	return 0;
}

static const struct net_device_ops mscc_ifh_netdev_ops = {
	.ndo_open            = mscc_ifh_open,
	.ndo_stop            = mscc_ifh_close,
	.ndo_start_xmit      = mscc_ifh_start_xmit,
	.ndo_change_mtu      = mscc_ifh_change_mtu,
	.ndo_validate_addr   = eth_validate_addr,
	.ndo_set_mac_address = eth_mac_addr,
};

static void proc_mscc_ifh_stats(struct seq_file *s, struct prof_stat *stat)
{
	seq_printf(s, "%s min ns: %llu\n", stat->name, stat->result.min);
	seq_printf(s, "%s max ns: %llu\n", stat->name, stat->result.max);
	seq_printf(s, "%s avg ns: %llu\n", stat->name, stat->result.sum / stat->samples);
}

static int mscc_ifh_seqfile_stats(struct seq_file *s, void *offset)
{
	struct mscc_ifh_priv *priv = dev_get_drvdata(s->private);
	int rx_free_count = 0;
	int rx_used_count = 0;
	struct list_head *pos;
	struct mscc_ifh_rx_request *req;
	int idx;

	list_for_each(pos, &priv->free_rx_reqs) {
		++rx_free_count;
	}
	list_for_each(pos, &priv->rx_reqs) {
		++rx_used_count;
	}

	seq_printf(s, "Max extraction requests: %d\n", FDMA_RX_REQUEST_MAX);
	seq_printf(s, "Used extraction requests: %d\n", rx_used_count);
	seq_printf(s, "Free extraction requests: %d\n", rx_free_count);
	seq_printf(s, "Max injection requests: %d\n", FDMA_TX_REQUEST_MAX);
	seq_printf(s, "Coherent allocs: %d\n", priv->stats.coherent_allocs);
	seq_printf(s, "Coherent allocs high mark: %d\n", priv->stats.coherent_allocs_high_mark);

	proc_mscc_ifh_stats(s, &profstats[0]);
	proc_mscc_ifh_stats(s, &profstats[1]);
	proc_mscc_ifh_stats(s, &profstats[2]);
	proc_mscc_ifh_stats(s, &profstats[3]);
	proc_mscc_ifh_stats(s, &profstats[4]);
	proc_mscc_ifh_stats(s, &profstats[5]);
	proc_mscc_ifh_stats(s, &profstats[6]);
	proc_mscc_ifh_stats(s, &profstats[7]);

	idx = 0;
	seq_printf(s, "Queued rx requests\n");
	list_for_each_entry(req, &priv->rx_reqs, node) {
		seq_printf(s, "  %02d: [C%u,I%u]\n", idx, req->cookie,
			   req->idx);
		++idx;
	}
	return 0;
}

static void mscc_ifh_debugfs(struct mscc_ifh_priv *priv)
{
	prof_sample_init(&profstats[0], 100000, "xtr callback");
	prof_sample_init(&profstats[1], 100000, "xtr create skb");
	prof_sample_init(&profstats[2], 100000, "xtr prepare req");
	prof_sample_init(&profstats[3], 100000, "xtr netif rx");
	prof_sample_init(&profstats[4], 100000, "xtr alloc skb");
	prof_sample_init(&profstats[5], 100000, "xtr memcpy");
	prof_sample_init(&profstats[6], 100000, "inj memcpy");
	prof_sample_init(&profstats[7], 100000, "inj rate");

	/* Provide debugfs access to data */
	priv->rootfs = debugfs_create_dir("mscc_ifh", NULL);
	debugfs_create_devm_seqfile(priv->dev, "stats", priv->rootfs,
				    mscc_ifh_seqfile_stats);
	debugfs_create_u32("rx_fill_level", 0666, priv->rootfs,
			   &priv->rx_req_fill_level);
#if defined(FDMA_INJECT_MULTIPLE)
	debugfs_create_u32("tx_fill_level", 0666, priv->rootfs,
			   &priv->tx_req_fill_level);
#endif
	debugfs_create_u32("tx_req_interval", 0666, priv->rootfs,
			   &priv->tx_req_interval);
}

static struct net_device *ifhdev_create(struct platform_device *pdev)
{
	struct mscc_ifh_priv *priv;
	struct net_device *dev;

	if ((dev = devm_alloc_etherdev(&pdev->dev, sizeof(*priv))) == NULL) {
		return NULL;
	}

	dev->netdev_ops = &mscc_ifh_netdev_ops;
	priv = netdev_priv(dev);
	memset(priv, 0, sizeof(*priv));
	priv->netdev = dev; /* Backlink */
	priv->dev = &pdev->dev;
	priv->config = device_get_match_data(&pdev->dev);

	eth_hw_addr_random(dev);
	memset(&dev->broadcast[0], 0xff, 6);

	/* Set MTU for injection (not rx) */
	dev->mtu = MTU_DEFAULT + priv->config->ifh_len;
	dev->min_mtu = MTU_MIN + priv->config->ifh_len;
	dev->max_mtu = MTU_MAX + priv->config->ifh_len;

	return dev;
}

static int mscc_ifh_probe(struct platform_device *pdev)
{
	struct mscc_ifh_priv *priv;
	struct net_device *dev;
	int idx;
	int ret;

	if ((dev = ifhdev_create(pdev)) == NULL) {
		dev_info(&pdev->dev, "Cannot create netdevice\n");
		return -ENOMEM;
	}
	strcpy(dev->name, DEVNAME);

	SET_NETDEV_DEV(dev, &pdev->dev);
	priv = netdev_priv(dev);
	platform_set_drvdata(pdev, priv);

	spin_lock_init(&priv->tx_lock);

	/* Request TX and RX DMA Channels */
	priv->txdma = dma_request_chan(priv->dev, "tx");
	priv->rxdma = dma_request_chan(priv->dev, "rx");
	if (IS_ERR(priv->txdma) || IS_ERR(priv->rxdma)) {
		if (!IS_ERR(priv->rxdma))
			dma_release_channel(priv->rxdma);
		if (!IS_ERR(priv->txdma))
			dma_release_channel(priv->txdma);
		priv->rxdma = NULL;
		priv->txdma = NULL;
		/* TODO: Use register access */
		mscc_ifh_module_retry++;
		if (mscc_ifh_module_retry == 3) {
			struct resource *res;
			int irq;

			priv->inj_port = 0; /* chipport 65 on Sparx5 */
			res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
			priv->io_addr =
				ioremap(res->start, res->end - res->start + 1);

			mscc_ifh_register_based_injection(priv);

			tasklet_init(&priv->tasklet, mscc_ifh_xtr_tasklet,
				     (unsigned long)priv);

			irq = platform_get_irq_byname(pdev, "xtr");
			if (irq < 0) {
				dev_warn(priv->dev, "No IRQ\n");
				return -ENODEV;
			}

			ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				mscc_ifh_xtr_irq_handler, IRQF_ONESHOT,
				"frame extraction", priv);
			if (ret) {
				dev_warn(priv->dev, "No IRQ thread\n");
				return ret;
			}
			dev_warn(priv->dev,
				 "No FDMA support: Going register based\n");
		} else {
			dev_warn(priv->dev,
				 "No FDMA support: retry later: %d\n",
				 mscc_ifh_module_retry);
			return -EPROBE_DEFER;
		}
	} else {
		dev_info(priv->dev, "Requested TX & RX DMA channels\n");
	}

	INIT_LIST_HEAD(&priv->free_tx_reqs);
	INIT_LIST_HEAD(&priv->free_rx_reqs);
	INIT_LIST_HEAD(&priv->rx_reqs);
	INIT_LIST_HEAD(&priv->tx_reqs);
	if (use_fdma(priv)) {
		priv->rx_req_fill_level = FDMA_XTR_BUFFER_COUNT;
#if defined(FDMA_INJECT_MULTIPLE)
		priv->tx_req_fill_level = 5;
#endif
		priv->tx_req_interval = 20;
		for (idx = 0; idx < FDMA_TX_REQUEST_MAX; ++idx) {
			struct mscc_ifh_tx_request *req =
				devm_kzalloc(priv->dev, sizeof(*req), GFP_KERNEL);
			if (!req) {
				goto err_list;
			}
			list_add(&req->node, &priv->free_tx_reqs);
		}
		/* Create a pool of coherent memory blocks for extraction */
		priv->rx_pool = dmam_pool_create("mscc-ifh-rx", priv->rxdma->device->dev,
						 FDMA_XTR_BUFFER_SIZE,
						 FDMA_BUFFER_ALIGN,
						 0);
		if (!priv->rx_pool) {
			dev_err(&pdev->dev, "Unable to allocate DMA RX pool\n");
			return -ENOMEM;
		}
		for (idx = 0; idx < FDMA_RX_REQUEST_MAX; ++idx) {
			struct mscc_ifh_rx_request *req =
				devm_kzalloc(priv->dev, sizeof(*req), GFP_KERNEL);
			if (!req) {
				goto err_list;
			}
			mscc_ifh_init_rx_request(priv, req, FDMA_XTR_BUFFER_SIZE);
			list_add(&req->node, &priv->free_rx_reqs);
			mscc_ifh_prepare_rx_request(priv);
		}
	}

	dev->needed_tailroom = ETH_FCS_LEN;

	ret = register_netdev(dev);
	dev_info(priv->dev, "Starting IFH driver: %s", DEVNAME);
	if (ret < 0) {
		dev_err(priv->dev, "Cannot register netdevice: %d\n", ret);
		free_netdev(dev);
		return ret;
	}

#if defined(FDMA_INJECT_MULTIPLE)
	spin_lock_init(&priv->rx_lock);
	/* Managing multiple injections in the same request */
	timer_setup(&priv->injection_timer, mscc_ifh_fdma_issue_tx, 0);
#endif

	/* Provide debugfs access to data */
	mscc_ifh_debugfs(priv);

	return 0;

err_list:
	dev_err(priv->dev, "Request allocation error\n");
	return -ENOMEM;
}

static int mscc_ifh_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct mscc_ifh_priv *priv = netdev_priv(dev);

	dev_info(priv->dev, "removing\n");

	debugfs_remove_recursive(priv->rootfs);
	if (use_fdma(priv)) {
		dma_release_channel(priv->rxdma);
		dma_release_channel(priv->txdma);
	} else {
		iounmap(priv->io_addr);
	}

	unregister_netdev(dev);
	free_netdev(dev);
	return 0;
}

static const struct fdma_config sparx5_data = {
	.first_cpu_port = 65,
	.last_cpu_port = 66,
	.ifh_id = 0xb,
	.ifh_len = 36,
	.ifh_encap_len = 16,
};

static const struct of_device_id mscc_ifh_match[] = {
	{ .compatible = "microchip,vsc7558-fdma-ifh", .data = &sparx5_data },
	{},
};
MODULE_DEVICE_TABLE(of, mscc_ifh_match);

static struct platform_driver mscc_ifh_driver = {
	.probe = mscc_ifh_probe,
	.remove = mscc_ifh_remove,
	.driver = {
		.name = "microchip-fdma-ifh",
		.of_match_table = mscc_ifh_match,
	},
};

module_platform_driver(mscc_ifh_driver);

MODULE_AUTHOR("Steen Hegelund <steen.hegelund@microchip.com>");
MODULE_DESCRIPTION("Microsemi FDMA IFH driver");
MODULE_LICENSE("Dual MIT/GPL");
