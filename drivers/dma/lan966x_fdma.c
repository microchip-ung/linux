// SPDX-License-Identifier: (GPL-2.0 OR MIT)

/* Copyright (C) 2020 Microchip Technology Inc. */

#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <asm/cacheflush.h>

#include "dmaengine.h"
#include "lan966x_fdma_regs_sr.h"

#ifdef CONFIG_DEBUG_KERNEL
#define LAN_RD_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 readl(lan966x->regs[id + (tinst)] +		\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)))

#define LAN_WR_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 writel(val, lan966x->regs[id + (tinst)] +	\
	        gbase + ((ginst) * gwidth) +		\
	        raddr + ((rinst) * rwidth)))

#define LAN_RMW_(val, mask, lan966x, id, tinst, tcnt,	\
		 gbase, ginst, gcnt, gwidth,		\
		 raddr, rinst, rcnt, rwidth) do {	\
	u32 _v_;					\
	WARN_ON((tinst) >= tcnt);			\
	WARN_ON((ginst) >= gcnt);			\
	WARN_ON((rinst) >= rcnt);			\
	_v_ = readl(lan966x->regs[id + (tinst)] +	\
		    gbase + ((ginst) * gwidth) +	\
		    raddr + ((rinst) * rwidth));	\
	_v_ = ((_v_ & ~(mask)) | ((val) & (mask)));	\
	writel(_v_, lan966x->regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)); } while (0)
#else
#define LAN_RD_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	readl(lan966x->regs[id + (tinst)] +		\
	      gbase + ((ginst) * gwidth) +		\
	      raddr + ((rinst) * rwidth))

#define LAN_WR_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	writel(val, lan966x->regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth))

#define LAN_RMW_(val, mask, lan966x, id, tinst, tcnt,	\
		 gbase, ginst, gcnt, gwidth,		\
		 raddr, rinst, rcnt, rwidth) do {	\
	u32 _v_;					\
	_v_ = readl(lan966x->regs[id + (tinst)] +	\
		    gbase + ((ginst) * gwidth) +	\
		    raddr + ((rinst) * rwidth));	\
	_v_ = ((_v_ & ~(mask)) | ((val) & (mask)));	\
	writel(_v_, lan966x->regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)); } while (0)
#endif

#define LAN_WR(...) LAN_WR_(__VA_ARGS__)
#define LAN_RD(...) LAN_RD_(__VA_ARGS__)
#define LAN_RMW(...) LAN_RMW_(__VA_ARGS__)

#define FDMA_DCB_MAX_DBS			3
#define FDMA_DCB_INFO_DATAL(x)			((x) & GENMASK(15, 0))
#define FDMA_DCB_INFO_TOKEN			BIT(17)
#define FDMA_DCB_INFO_INTR			BIT(18)
#define FDMA_DCB_INFO_SW(x)			(((x) << 24) & GENMASK(31, 24))

#define FDMA_DCB_STATUS_BLOCKL(x)		((x) & GENMASK(15, 0))
#define FDMA_DCB_STATUS_SOF			BIT(16)
#define FDMA_DCB_STATUS_EOF			BIT(17)
#define FDMA_DCB_STATUS_INTR			BIT(18)
#define FDMA_DCB_STATUS_DONE			BIT(19)
#define FDMA_DCB_STATUS_BLOCKO(x)		(((x) << 20) & GENMASK(31, 20))
#define FDMA_DCB_INVALID_DATA			0x1

#define FDMA_BUFFER_ALIGN			128
#define FDMA_BUFFER_MASK			127
#define XTR_BUFFER_SIZE				(XTR_CHUNK_SIZE*12)
#define FDMA_XTR_CHANNEL			6
#define FDMA_DCB_MAX				21
#define VCORE_ACCESS_TIMEOUT_MS			5
#define FDMA_DISABLE_TIMEOUT_MS			5

struct lan966x_fdma;

enum lan966x_fdma_channel_state {
	DCS_IDLE = 0,
	DCS_ACTIVE,
	DCS_RUNNING,
	DCS_STOPPING,
	DCS_ERROR
};

enum lan966x_fdma_dcb_state {
	DCBS_IDLE = 0,    /* Not yet used or ready to be reused */
	DCBS_QUEUED,      /* In queue for transfer */
	DCBS_ISSUED,      /* In transfer in progress */
	DCBS_ERROR,       /* Transfer failed */
	DCBS_COMPLETE,    /* Transfer successful */
};

struct lan966x_fdma_data {
	u64 dataptr;
	u64 status;
};

struct lan966x_fdma_dcb_hw {
	u64 nextptr;
	u64 info;
	struct lan966x_fdma_data block[FDMA_DCB_MAX_DBS];
};

struct lan966x_fdma_block_info {
	int size;
};

struct lan966x_fdma_dcb {
	struct lan966x_fdma_dcb_hw hw;
	struct dma_async_tx_descriptor txd;
	dma_addr_t phys;
	enum lan966x_fdma_dcb_state state;
	int valid_blocks;
	struct lan966x_fdma_dcb *first_dcb;
	int is_last_dcb;
	struct lan966x_fdma_block_info binfo[FDMA_DCB_MAX_DBS];
	u32 residue;
	struct list_head node;
};

struct lan966x_stats {
	int free_dcbs;
	int free_dcbs_low_mark;
};

struct lan966x_fdma_channel {
	struct dma_chan chan;
	enum lan966x_fdma_channel_state state;
	struct dma_tx_state tx_state;
	struct list_head free_dcbs;
	struct list_head queued_dcbs;
	u64 dbirq_pattern;
	struct tasklet_struct tasklet;
	struct lan966x_fdma *drv;
	struct lan966x_fdma_dcb *next_dcb;
	int next_idx;
	struct lan966x_stats stats;
	spinlock_t lock;
};

struct lan966x_fdma {
	struct dma_device dma; /* Must be first member due to xlate function */
	struct dma_pool *dcb_pool;
	int irq;

	void __iomem *regs[NUM_TARGETS];

	/* contains the number of physical channels */
	int nr_pchans;
	struct lan966x_fdma_channel chans[0];
};

static inline struct lan966x_fdma *to_lan966x_fdma(struct dma_device *dd)
{
	return container_of(dd, struct lan966x_fdma, dma);
}

static inline struct lan966x_fdma_channel
*to_lan966x_fdma_channel(struct dma_chan *c)
{
	return container_of(c, struct lan966x_fdma_channel, chan);
}

static void lan966x_fdma_xtr_eof(struct lan966x_fdma *lan966x,
				 struct lan966x_fdma_channel *fdma_chan,
				 struct lan966x_fdma_dcb *first,
				 struct lan966x_fdma_dcb *iter, int idx,
				 int packet_size, u64 status)
{
	struct dma_async_tx_descriptor *txd = &first->txd;
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_NOERROR,
		.residue = 0
	};

	packet_size += FDMA_DCB_STATUS_BLOCKL(status);
	if (first != iter)
		first->residue = 0;

	iter->residue -= FDMA_DCB_STATUS_BLOCKL(status);
	fdma_chan->tx_state.residue = iter->residue;

	pr_debug("%s:%d %s: Channel: %d, notify client:"
		 " txd: 0x%px, [C%u,I%u], packet size: %u\n",
		 __FILE__, __LINE__, __func__,
		 fdma_chan->chan.chan_id,
		 txd,
		 txd->cookie,
		 idx,
		 packet_size);

	dma_result.residue = packet_size;

	dmaengine_desc_get_callback_invoke(txd, &dma_result);

	if (first != iter) {
		do {
			/* Last block in this DCB has been transferred  */
			fdma_chan->tx_state.last = txd->cookie;
			dma_cookie_complete(txd);

			pr_debug("%s:%d %s: Channel: %d,"
				 " completed DCB: 0x%llx"
				 " [C%u,I%u]\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 (u64)first->phys,
				 fdma_chan->tx_state.last, idx);

			iter->state = DCBS_IDLE;
			spin_lock(&fdma_chan->lock);

			list_move_tail(&first->node, &fdma_chan->free_dcbs);
			fdma_chan->stats.free_dcbs++;

			spin_unlock(&fdma_chan->lock);
			first = list_first_entry(&fdma_chan->queued_dcbs,
						 struct lan966x_fdma_dcb, node);
		} while (first != iter);
	}

	idx++;
	if (idx == iter->valid_blocks) {
		/* Last block in this DCB has been transferred  */
		fdma_chan->tx_state.last = txd->cookie;
		dma_cookie_complete(txd);

		pr_debug("%s:%d %s: Channel: %d,"
			 " completed DCB: 0x%llx"
			 " [C%u,I%u]\n",
			 __FILE__, __LINE__, __func__,
			 fdma_chan->chan.chan_id,
			 (u64)iter->phys,
			 fdma_chan->tx_state.last, idx);

		iter->state = DCBS_IDLE;
		spin_lock(&fdma_chan->lock);

		list_move_tail(&iter->node, &fdma_chan->free_dcbs);
		fdma_chan->stats.free_dcbs++;

		spin_unlock(&fdma_chan->lock);
		iter = list_first_entry(&fdma_chan->queued_dcbs,
					struct lan966x_fdma_dcb, node);

		idx = 0;
	}

	fdma_chan->next_dcb = iter;
	fdma_chan->next_idx = idx;
}

static void lan966x_fdma_xtr_tasklet(unsigned long data)
{
	struct lan966x_fdma_channel *fdma_chan =
		(struct lan966x_fdma_channel *)data;
	struct lan966x_fdma *lan966x = fdma_chan->drv;
	struct lan966x_fdma_dcb *first = NULL;
	struct lan966x_fdma_dcb *iter;
	u32 packet_size = 0;
	bool more = true;
	u64 pktstatus;
	u64 status;
	int budget;
	int idx;

	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	iter = fdma_chan->next_dcb;
	idx = fdma_chan->next_idx;
	status = iter->hw.block[idx].status;
	more = !!(status & FDMA_DCB_STATUS_DONE);

	pktstatus = status & (FDMA_DCB_STATUS_SOF | FDMA_DCB_STATUS_EOF |
			      FDMA_DCB_STATUS_DONE);
	budget = 10;
	while (more && budget--) {
		pr_debug("%s:%d %s: Channel: %d, [C%u,I%u], status: 0x%llx\n",
			 __FILE__, __LINE__, __func__,
			 fdma_chan->chan.chan_id,
			 iter->txd.cookie,
			 idx,
			 status);
		if (pktstatus ==
		    (FDMA_DCB_STATUS_SOF | FDMA_DCB_STATUS_EOF |
		     FDMA_DCB_STATUS_DONE)) {
			lan966x_fdma_xtr_eof(lan966x, fdma_chan, iter, iter,
					     idx, 0, status);

			iter = fdma_chan->next_dcb;
			idx = fdma_chan->next_idx;

		} else if (pktstatus & FDMA_DCB_STATUS_SOF) {
			packet_size = FDMA_DCB_STATUS_BLOCKL(status);
			first = iter;

			pr_debug("%s:%d %s: Channel: %d, SOF:"
				 " txd: 0x%px, [C%u,I%u], packet size: %u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 &iter->txd,
				 iter->txd.cookie,
				 idx,
				 packet_size);
			idx++;
			if (idx == iter->valid_blocks) {
				iter = list_next_entry(iter, node);
				idx = 0;
			}

		} else if ((pktstatus & FDMA_DCB_STATUS_EOF) && first) {
			lan966x_fdma_xtr_eof(lan966x, fdma_chan, first, iter,
					     idx, packet_size, status);

			iter = fdma_chan->next_dcb;
			idx = fdma_chan->next_idx;
		} else {
			packet_size += FDMA_DCB_STATUS_BLOCKL(status);
			pr_debug("%s:%d %s: Channel: %d, middle block: packet size: %u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 packet_size);
			idx++;

			if (idx == iter->valid_blocks) {
				iter = list_next_entry(iter, node);
				idx = 0;
			}
		}
		status = iter->hw.block[idx].status;
		more = !!(status & FDMA_DCB_STATUS_DONE);
		pktstatus = status & (FDMA_DCB_STATUS_SOF | FDMA_DCB_STATUS_EOF
				      | FDMA_DCB_STATUS_DONE);
	}

	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
}

static void lan966x_fdma_inj_tasklet(unsigned long data)
{
	struct lan966x_fdma_dcb *iter, *first = 0, *prev = 0, *tmp;
	struct lan966x_fdma_channel *fdma_chan =
		(struct lan966x_fdma_channel *)data;
	struct lan966x_fdma_dcb *request = 0;
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_ABORTED,
		.residue = 0
	};
	u32 packet_size = 0;
	int idx;

	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	spin_lock(&fdma_chan->lock);
	list_for_each_entry_safe(iter, tmp, &fdma_chan->queued_dcbs, node) {
		if (prev && prev->state == DCBS_COMPLETE &&
		    prev->hw.nextptr != FDMA_DCB_INVALID_DATA) {
			/* The previous completed DCB can be freed */
			pr_debug("%s:%d %s: Channel: %d, previous completed DCB:"
				 " 0x%llx move to free list\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id, (u64)prev->phys);
			prev->state = DCBS_IDLE;
			list_move_tail(&prev->node, &fdma_chan->free_dcbs);
			fdma_chan->stats.free_dcbs++;
		}
		prev = iter;
		if (iter->state != DCBS_ISSUED)
			continue;

		for (idx = 0; idx < iter->valid_blocks; ++idx) {
			struct lan966x_fdma_block_info *blk = &iter->binfo[idx];
			u64 status = iter->hw.block[idx].status;

			pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx,"
				 " Block[%02d], dataptr: 0x%09llx, status:"
				 " 0x%09llx: bytes: %u, C%u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 (u64)iter->phys,
				 idx,
				 iter->hw.block[idx].dataptr,
				 status,
				 blk->size,
				 iter->txd.cookie);

			if (iter->txd.cookie > DMA_MIN_COOKIE) {
				/* Requests have valid cookies */
				if (!request) {
					/* First in queue is the used TXD */
					fdma_chan->tx_state.used = iter->txd.cookie;
				}
				request = iter;
			}

			if (!request)
				continue;

			if (blk->size == 0)
				continue;

			if (!(status & FDMA_DCB_STATUS_DONE))
				break;

			if (status & FDMA_DCB_STATUS_SOF)
				first = iter;

			if (!first)
				continue;

			/* Update packet size and request residue with this
			 * block
			 */
			packet_size += FDMA_DCB_STATUS_BLOCKL(status);
			request->residue -= FDMA_DCB_STATUS_BLOCKL(status);
			if (!(status & FDMA_DCB_STATUS_EOF))
				continue;

			if (idx == iter->valid_blocks - 1) {
				/* Last block in this DCB has been transferred  */
				pr_debug("%s:%d %s: Channel: %d, completed DCB:"
					 " 0x%llx\n",
					 __FILE__, __LINE__, __func__,
					 fdma_chan->chan.chan_id,
					 (u64)iter->phys);
				iter->state = DCBS_COMPLETE;
				/* Last DCB in this request has been transferred  */
				if (iter->is_last_dcb) {
					fdma_chan->tx_state.last =
						iter->first_dcb->txd.cookie;
					dma_cookie_complete(&iter->first_dcb->txd);
					pr_debug("%s:%d %s: Channel: %d,"
						 " completed cookie: %u\n",
						 __FILE__, __LINE__, __func__,
						 fdma_chan->chan.chan_id,
						 fdma_chan->tx_state.last);
				}
			}

			fdma_chan->tx_state.residue = request->residue;
			dma_result.residue = request->residue;
			dma_result.result = DMA_TRANS_NOERROR;
			pr_debug("%s:%d %s: Channel: %d, notify client:"
				 " txd: 0x%px, residue: %u, packet size: %u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 &request->txd,
				 request->residue,
				 packet_size);
			if (fdma_chan->chan.chan_id >= FDMA_XTR_CHANNEL) {
				dma_result.residue = packet_size;
			}

			spin_unlock(&fdma_chan->lock);
			dmaengine_desc_get_callback_invoke(
				&request->txd, &dma_result);
			spin_lock(&fdma_chan->lock);

			packet_size = 0;
			/* Mark data block as transferred */
			blk->size = 0;
		}
	}
	spin_unlock(&fdma_chan->lock);
	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
}

static int lan966x_fdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct lan966x_fdma *lan966x = to_lan966x_fdma(chan->device);
	u32 mask;

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);

	LAN_WR(FDMA_CH_CFG_CH_DCB_DB_CNT(FDMA_DCB_MAX_DBS) |
	       FDMA_CH_CFG_CH_INTR_DB_EOF_ONLY(1) |
	       FDMA_CH_CFG_CH_INJ_PORT(0) |
	       FDMA_CH_CFG_CH_MEM(1),
	       lan966x, FDMA_CH_CFG(chan->chan_id));

	/* Start fdma */
	lan966x->chans[chan->chan_id].state = DCS_ACTIVE;
	if (chan->chan_id >= FDMA_XTR_CHANNEL) {
		/* Start extraction */
		LAN_RMW(FDMA_PORT_CTRL_XTR_STOP(0),
			FDMA_PORT_CTRL_XTR_STOP_M,
			lan966x, FDMA_PORT_CTRL(0));
	} else {
		/* Start injection */
		LAN_RMW(FDMA_PORT_CTRL_INJ_STOP(0),
			FDMA_PORT_CTRL_INJ_STOP_M,
			lan966x, FDMA_PORT_CTRL(0));
	}

	dma_cookie_init(chan);

	/* Enable interrupts */
	mask = LAN_RD(lan966x, FDMA_INTR_DB_ENA);
	mask = FDMA_INTR_DB_ENA_INTR_DB_ENA_X(mask);
	mask |= BIT(chan->chan_id);
	LAN_RMW(FDMA_INTR_DB_ENA_INTR_DB_ENA(mask),
		FDMA_INTR_DB_ENA_INTR_DB_ENA_M,
		lan966x, FDMA_INTR_DB_ENA);

	return 0;
}

static int lan966x_fdma_wait_for_xtr_buffer_empty(struct lan966x_fdma *lan966x,
						  int channel)
{
	unsigned long deadline;
	int empty;

	/* Wait here until the extraction buffer is empty */
	deadline = jiffies + msecs_to_jiffies(FDMA_DISABLE_TIMEOUT_MS);
	do {
		empty = LAN_RD(lan966x, FDMA_PORT_CTRL(0));
		empty &= FDMA_PORT_CTRL_XTR_BUF_IS_EMPTY_M;
	} while (time_before(jiffies, deadline) && (empty == 0)) ;

	return empty;
}

static void lan966x_fdma_free_chan_resources(struct dma_chan *chan)
{
	struct lan966x_fdma *lan966x = to_lan966x_fdma(chan->device);

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);

	/* Stop fdma */
	lan966x->chans[chan->chan_id].state = DCS_STOPPING;
	if (chan->chan_id >= FDMA_XTR_CHANNEL) {
		lan966x_fdma_wait_for_xtr_buffer_empty(lan966x, chan->chan_id);

		/* Stop extraction */
		LAN_RMW(FDMA_PORT_CTRL_XTR_STOP(1),
			FDMA_PORT_CTRL_XTR_STOP_M,
			lan966x, FDMA_PORT_CTRL(0));

		lan966x_fdma_wait_for_xtr_buffer_empty(lan966x, chan->chan_id);
	} else {
		/* Stop injection */
		LAN_RMW(FDMA_PORT_CTRL_INJ_STOP(1),
			FDMA_PORT_CTRL_INJ_STOP_M,
			lan966x, FDMA_PORT_CTRL(0));
	}

	/* disable channel */
	LAN_RMW(FDMA_CH_DISABLE_CH_DISABLE(BIT(chan->chan_id)),
		FDMA_CH_DISABLE_CH_DISABLE_M,
		lan966x, FDMA_CH_DISABLE);

	/* disable the channels DB interrupt */
	LAN_RMW(FDMA_INTR_DB_ENA_INTR_DB_ENA(~BIT(chan->chan_id)),
		FDMA_INTR_DB_ENA_INTR_DB_ENA_M,
		lan966x, FDMA_INTR_DB_ENA);
}

static dma_cookie_t lan966x_fdma_tx_submit(struct dma_async_tx_descriptor *txd)
{
	if (!txd) {
		pr_debug("%s:%d %s: Channel: %u, reuse cookie: %u",
			 __FILE__, __LINE__, __func__,
			 txd->chan->chan_id, txd->cookie);
		return 0;
	}

	if (txd->cookie >= DMA_MIN_COOKIE) {
		pr_debug("%s:%d %s: Channel: %u, reuse cookie: %u",
			 __FILE__, __LINE__, __func__,
			 txd->chan->chan_id, txd->cookie);
		return txd->cookie;
	}

	dma_cookie_assign(txd);

	pr_debug("%s:%d %s: Channel: %u, new cookie: %u",
		 __FILE__, __LINE__, __func__,
		 txd->chan->chan_id, txd->cookie);
	return txd->cookie;
}

static bool lan966x_fdma_get_dcb(struct lan966x_fdma *lan966x,
				 struct dma_chan *chan, int sg_len,
				 struct lan966x_fdma_dcb **res, int *residx)
{
	struct lan966x_fdma_channel *fdma_chan = to_lan966x_fdma_channel(chan);
	struct lan966x_fdma_dcb *dcb;
	int idx, jdx;

	if (list_empty(&fdma_chan->free_dcbs))
		return false;

	dcb = list_first_entry(&fdma_chan->free_dcbs, struct lan966x_fdma_dcb,
			       node);

	/* Initialize the new DCB */
	memset(dcb, 0, sizeof(struct  lan966x_fdma_dcb_hw));
	memset(dcb->binfo, 0, sizeof(struct  lan966x_fdma_block_info));
	dcb->state = DCBS_QUEUED;
	dcb->valid_blocks = 0;
	dcb->residue = 0;
	dcb->is_last_dcb = 0;

	/* No next DCB */
	dcb->hw.nextptr = FDMA_DCB_INVALID_DATA;
	for (jdx = 0; jdx < FDMA_DCB_MAX_DBS; ++jdx) {
		dcb->hw.block[jdx].dataptr =
			FDMA_DCB_INVALID_DATA;
	}

	dma_async_tx_descriptor_init(&dcb->txd, chan);
	dcb->txd.tx_submit = lan966x_fdma_tx_submit;

	dcb->txd.phys = dcb->phys;

	/* Move item into the channel */
	list_move_tail(&dcb->node, &fdma_chan->queued_dcbs);

	/* Update free dcb statistics */
	fdma_chan->stats.free_dcbs--;
	if (fdma_chan->stats.free_dcbs < fdma_chan->stats.free_dcbs_low_mark) {
		fdma_chan->stats.free_dcbs_low_mark =
			fdma_chan->stats.free_dcbs;
	}
	idx = 0;

	pr_debug("%s:%d %s: Channel: %u, new DCB: 0x%llx\n",
		 __FILE__, __LINE__, __func__,
		 chan->chan_id, (u64)dcb->phys);

	*res = dcb;
	*residx = idx;
	return true;
}

static void lan966x_fdma_add_datablock(struct lan966x_fdma *lan966x,
				       struct lan966x_fdma_channel *fdma_chan,
				       struct lan966x_fdma_dcb *dcb,
				       enum dma_transfer_direction direction,
				       struct scatterlist *sg, int sg_len,
				       int sidx, int idx)
{
	dma_addr_t db_phys = 0;
	/* Interrupt on DB status update */
	u32 status_flags;
	u64 len, off;

	len = dcb->binfo[idx].size = sg_dma_len(sg);
	db_phys = sg_dma_address(sg);

	/* Why? */
	off = db_phys & 0x7;
	db_phys &= ~0x7;

	/* Adapt the DB Interrupt to the current load */
	status_flags = (fdma_chan->dbirq_pattern >> idx) & 0x1 ?  FDMA_DCB_STATUS_INTR : 0;

	if (direction == DMA_MEM_TO_DEV) {
		if (sidx == 0) {
			status_flags |= FDMA_DCB_STATUS_SOF;
		}
		if (sg_is_last(sg)) {
			status_flags |= FDMA_DCB_STATUS_EOF;
		}
		dcb->hw.block[idx].dataptr = db_phys;
		dcb->hw.block[idx].status =
			FDMA_DCB_STATUS_BLOCKL(len) |
			status_flags |
			FDMA_DCB_STATUS_BLOCKO(off);
	} else {
		/* Length is a multipla of 128 */
		dcb->hw.info = FDMA_DCB_INFO_DATAL(
			len & ~FDMA_BUFFER_MASK);
		dcb->hw.block[idx].dataptr = db_phys;
		dcb->hw.block[idx].status = status_flags;
	}
	dcb->valid_blocks++;

	pr_debug("%s:%d %s: DCB: 0x%llx, Block[%02d], "
		 "dataptr: 0x%09llx, offset: 0x%llu, bytes: %llu\n",
		 __FILE__, __LINE__, __func__,
		 (u64)dcb->phys,
		 idx, dcb->hw.block[idx].dataptr,
		 off, len);
}

static struct dma_async_tx_descriptor *
lan966x_fdma_prep_slave_sg(struct dma_chan *chan, struct scatterlist *sgl,
			   unsigned int sg_len,
			   enum dma_transfer_direction direction,
			   unsigned long flags, void *context)
{
	struct lan966x_fdma_channel *fdma_chan = to_lan966x_fdma_channel(chan);
	struct lan966x_fdma *lan966x = to_lan966x_fdma(chan->device);
	struct lan966x_fdma_dcb *first;
	struct lan966x_fdma_dcb *dcb;
	struct scatterlist *sg;
	u32 residue;
	int sidx; /* Segment index */
	int idx;  /* Block index */

	pr_debug("%s:%d %s begin\n", __FILE__, __LINE__, __func__);

	if (!sgl)
		return NULL;

	if (!is_slave_direction(direction)) {
		dev_err(&chan->dev->device, "Invalid DMA direction\n");
		return NULL;
	}

	if (fdma_chan->state == DCS_STOPPING) {
		pr_debug("%s:%d %s, Stopping channel %d\n",
			__FILE__, __LINE__, __func__, chan->chan_id);
		return NULL;
	}

	spin_lock(&fdma_chan->lock);
	first = 0;
	dcb = 0;
	idx = 0;
	residue = 0;
	for_each_sg(sgl, sg, sg_len, sidx) {
		/* One DCB has room for FDMA_DCB_MAX_DBS blocks */
		if (idx == 0) {
			if (!lan966x_fdma_get_dcb(lan966x, chan, sg_len,
						  &dcb, &idx)) {
				pr_err("%s:%d %s: no more DCBs\n",
				       __FILE__, __LINE__, __func__);
				goto unlock;
			}
		}

		lan966x_fdma_add_datablock(lan966x, fdma_chan, dcb, direction,
					   sg, sg_len, sidx, idx);

		residue += dcb->binfo[idx].size;

		pr_debug("%s:%d %s, Channel %d, residue: %u, block[%02u]: %u\n",
			__FILE__, __LINE__, __func__,
			chan->chan_id, residue, idx, dcb->binfo[idx].size);
		if (!first) {
			first = dcb;

			pr_debug("%s:%d %s, Channel %d, dcb: 0x%llx, txd:"
				 " 0x%px, block: %02u\n",
				 __FILE__, __LINE__, __func__,
				 chan->chan_id, (u64)dcb->phys, &dcb->txd, idx);
		}

		++idx;
		idx = idx % FDMA_DCB_MAX_DBS;
	}

	dcb->is_last_dcb = 1;
	first->residue += residue;
	dcb->first_dcb = first;

	spin_unlock(&fdma_chan->lock);

	pr_debug("%s:%d %s, Channel %d, len: %d, dir: %s: txd: 0x%px\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id,
		sg_len,
		direction == DMA_MEM_TO_DEV ? "to device" : "from device",
		&first->txd);

	return &first->txd;
unlock:
	spin_unlock(&fdma_chan->lock);
	return NULL;
}

static enum dma_status lan966x_fdma_tx_status(struct dma_chan *chan,
					      dma_cookie_t cookie,
					      struct dma_tx_state *txstate)
{
	struct lan966x_fdma *lan966x = to_lan966x_fdma(chan->device);
	struct lan966x_fdma_channel *fdma_chan;
	struct lan966x_fdma_dcb *iter;
	unsigned int residue = 0;
	enum dma_status status;
	int found = 0;
	int idx;

	pr_debug("%s:%d %s, cookie: %u\n", __FILE__, __LINE__, __func__, cookie);

	status = dma_cookie_status(chan, cookie, txstate);

	fdma_chan = &lan966x->chans[chan->chan_id];

	if (fdma_chan->state == DCS_ERROR)
		return DMA_ERROR;

	if (status != DMA_IN_PROGRESS)
		return status;

	list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
		pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx:"
			" state: %u, cookie: %u\n",
			__FILE__, __LINE__, __func__,
			fdma_chan->chan.chan_id,
			(u64)iter->txd.phys,
			iter->state,
			iter->txd.cookie);

		if (iter->txd.cookie == cookie) {
			found = 1;
			for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
				residue += iter->binfo[idx].size;
			}
		} else if (iter->txd.cookie > DMA_MIN_COOKIE) {
			found = 0;
		} else if (found) {
			for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
				residue += iter->binfo[idx].size;
			}
		}
	}

	txstate->residue = residue;
	return status;
}

static void lan966x_fdma_issue_pending(struct dma_chan *chan)
{
	struct lan966x_fdma *lan966x= to_lan966x_fdma(chan->device);
	struct lan966x_fdma_dcb *dcb, *first = 0;
	struct lan966x_fdma_channel *fdma_chan;
	u32 channel_bit = BIT(chan->chan_id);
	int queued = 0;
	int idx = 0;

	pr_debug("%s:%d %s, Channel: %d\n", __FILE__, __LINE__, __func__,
		 chan->chan_id);

	fdma_chan = &lan966x->chans[chan->chan_id];

	spin_lock(&fdma_chan->lock);
	list_for_each_entry(dcb, &fdma_chan->queued_dcbs, node) {
		if (dcb->state == DCBS_QUEUED) {
			if (first == 0) {
				first = dcb;
			}
			++queued;
		}
		++idx;
	}

	spin_unlock(&fdma_chan->lock);
	if (!first) {
		pr_err("%s:%d %s, Channel: %d, nothing queued\n",
		       __FILE__, __LINE__, __func__, chan->chan_id);
		return;
	}

	pr_debug("%s:%d %s, Channel: %d, state: %u, len: %d, queued: %d\n",
		 __FILE__, __LINE__, __func__, chan->chan_id, fdma_chan->state,
		 idx, queued);

	switch (fdma_chan->state) {
	case DCS_ACTIVE: {
		struct lan966x_fdma_dcb *prev = 0;
		int idx = 0;

		pr_debug("%s:%d %s, Activate channel %d, DCB: 0x%llx\n",
			 __FILE__, __LINE__, __func__, chan->chan_id,
			 (u64)first->phys);

		spin_lock(&fdma_chan->lock);
		list_for_each_entry(dcb, &fdma_chan->queued_dcbs, node) {
			if (dcb->state == DCBS_QUEUED) {
				dcb->state = DCBS_ISSUED;
				pr_debug("%s:%d %s, Channel: %d: Issued: [%02d]:"
					 " DCB: 0x%llx\n",
					 __FILE__, __LINE__, __func__,
					 chan->chan_id,
					 idx,
					 (u64)dcb->phys);

				if (prev) {
					pr_debug("%s:%d %s, Channel: %d:"
						 " chain[%02d]:"
						 " DCB: 0x%llx -> 0x%llx\n",
						 __FILE__, __LINE__, __func__,
						 chan->chan_id,
						 idx,
						 (u64)prev->phys,
						 (u64)dcb->phys);

					prev->hw.nextptr = dcb->phys; /* Valid */
				}
			}
			prev = dcb;
			++idx;
		}
		spin_unlock(&fdma_chan->lock);

		fdma_chan->next_dcb = first;
		fdma_chan->next_idx = 0;

		/* Write the DCB address */
		LAN_WR(((u64)first->phys) & GENMASK(31, 0), lan966x,
		       FDMA_DCB_LLP(chan->chan_id));
		LAN_WR(((u64)first->phys) >> 32, lan966x,
		       FDMA_DCB_LLP1(chan->chan_id));

		/* Activate the channel */
		LAN_RMW(channel_bit, FDMA_CH_ACTIVATE_CH_ACTIVATE_M,
			lan966x, FDMA_CH_ACTIVATE);
		fdma_chan->state = DCS_RUNNING;
		break;
	}
	case DCS_RUNNING: {
		struct lan966x_fdma_dcb *prev = 0;
		int idx = 0;

		spin_lock(&fdma_chan->lock);
		list_for_each_entry(dcb, &fdma_chan->queued_dcbs, node) {
			if (dcb->state == DCBS_QUEUED) {
				dcb->state = DCBS_ISSUED;
				if (prev) {
					prev->hw.nextptr = dcb->phys; /* Valid */
					pr_debug("%s:%d %s, Channel: %d:"
						 " chain[%02d]:"
						 " DCB: 0x%llx -> 0x%llx\n",
						 __FILE__, __LINE__, __func__,
						 chan->chan_id,
						 idx,
						 (u64)prev->phys,
						 (u64)dcb->phys);
				}
			}
			prev = dcb;
			++idx;
		}
		spin_unlock(&fdma_chan->lock);
		pr_debug("%s:%d %s, Reload channel %d\n",
			 __FILE__, __LINE__, __func__, chan->chan_id);

		LAN_RMW(channel_bit, FDMA_CH_RELOAD_CH_RELOAD_M,
			lan966x, FDMA_CH_RELOAD);
		break;
	}
	case DCS_STOPPING:
		pr_debug("%s:%d %s, Stopping channel %d\n",
			 __FILE__, __LINE__, __func__, chan->chan_id);
		break;
	case DCS_IDLE:
		/* When is a reload needed ? */
		pr_debug("%s:%d %s, Queue channel %d, DCB: 0x%llx\n",
			 __FILE__, __LINE__, __func__, chan->chan_id,
			 (u64)dcb->phys);
		break;
	case DCS_ERROR:
		pr_err("%s:%d %s, Errored channel %d,\n",
		       __FILE__, __LINE__, __func__, chan->chan_id);
		break;
	}
}

static int lan966x_fdma_terminate(struct dma_chan *chan)
{
	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);
	return 0;
}

static void lan966x_fdma_notify_clients_abort(struct dma_chan *chan)
{
	struct lan966x_fdma_channel *fdma_chan = to_lan966x_fdma_channel(chan);
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_ABORTED,
		.residue = 0
	};
	struct lan966x_fdma_dcb *iter;
	int idx;

	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
		pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx:"
			" state: %u, cookie: %u\n",
			__FILE__, __LINE__, __func__,
			fdma_chan->chan.chan_id,
			(u64)iter->phys,
			iter->state,
			iter->txd.cookie);

		for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
			iter->binfo[idx].size = 0;
		}
		if (iter->txd.cookie > DMA_MIN_COOKIE) {
			/* Requests have valid cookies */
			fdma_chan->tx_state.used = iter->txd.cookie;
			fdma_chan->tx_state.last = iter->txd.cookie;
			fdma_chan->tx_state.residue = 0;
			iter->state = DCBS_COMPLETE;
			dma_cookie_complete(&iter->txd);

			pr_debug("%s:%d %s: Channel: %d, notify client abort:"
				" txd: 0x%px\n",
				__FILE__, __LINE__, __func__,
				fdma_chan->chan.chan_id,
				&iter->txd);

			dmaengine_desc_get_callback_invoke(&iter->txd,
							   &dma_result);
		}
	}

	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
}

static void lan966x_fdma_sync(struct dma_chan *chan)
{
	struct lan966x_fdma *lan966x = to_lan966x_fdma(chan->device);
	u32 channel_mask = BIT(chan->chan_id);
	unsigned long deadline;
	u32 status;

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);

	/* Wait here until the FDMA has stopped */
	deadline = jiffies + msecs_to_jiffies(FDMA_DISABLE_TIMEOUT_MS);
	do {
		status = LAN_RD(lan966x, FDMA_CH_ACTIVE);
		status |= LAN_RD(lan966x, FDMA_CH_PENDING);

		pr_debug("%s:%d %s: Channel: %u, status: %u\n",
			__FILE__, __LINE__, __func__,
			chan->chan_id, status);

	} while (time_before(jiffies, deadline) && (status & channel_mask)) ;

	/* Notify the client that the queued transfers have been aborted */
	lan966x_fdma_notify_clients_abort(chan);
}

static void
lan966x_fdma_notify_clients_error(struct lan966x_fdma_channel *fdma_chan)
{
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_ABORTED,
		.residue = 0
	};
	struct lan966x_fdma_dcb *iter;
	int idx;

	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	dma_result.result = DMA_TRANS_WRITE_FAILED;
	if (fdma_chan->chan.chan_id >= FDMA_XTR_CHANNEL)
		dma_result.result = DMA_TRANS_READ_FAILED;

	list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
		pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx:"
			" state: %u, cookie: %u\n",
			__FILE__, __LINE__, __func__,
			fdma_chan->chan.chan_id,
			(u64)iter->phys,
			iter->state,
			iter->txd.cookie);

		for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx)
			iter->binfo[idx].size = 0;

		if (iter->txd.cookie > DMA_MIN_COOKIE) {
			/* Requests have valid cookies */
			fdma_chan->tx_state.used = iter->txd.cookie;
			fdma_chan->tx_state.last = iter->txd.cookie;
			fdma_chan->tx_state.residue = 0;
			iter->state = DCBS_COMPLETE;
			dma_cookie_complete(&iter->txd);

			pr_debug("%s:%d %s: Channel: %d, notify client abort:"
				" txd: 0x%px\n",
				__FILE__, __LINE__, __func__,
				fdma_chan->chan.chan_id,
				&iter->txd);
			dmaengine_desc_get_callback_invoke(&iter->txd,
							   &dma_result);
		}
	}

	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
}

static irqreturn_t lan966x_fdma_interrupt(int irq, void *args)
{
	struct lan966x_fdma *lan966x = args;
	u32 dcb = 0, db = 0, err = 0;

	pr_debug("%s:%d %s: begin\n",
		__FILE__, __LINE__, __func__);

	dcb = LAN_RD(lan966x, FDMA_INTR_DCB);
	db = LAN_RD(lan966x, FDMA_INTR_DB);
	err = LAN_RD(lan966x, FDMA_INTR_ERR);

	/* Clear interrupt */
	if (dcb) {
		LAN_WR(dcb, lan966x, FDMA_INTR_DCB);
		pr_debug("%s:%d %s: DCB int: 0x%x\n",
			__FILE__, __LINE__, __func__, dcb);
	}
	if (db) {
		LAN_WR(db, lan966x, FDMA_INTR_DB);
		pr_debug("%s:%d %s: DB int: 0x%x\n",
			__FILE__, __LINE__, __func__, db);
		while (db) {
			u32 chan = __fls(db);
			struct lan966x_fdma_channel *fdma_chan =
				&lan966x->chans[chan];

			tasklet_schedule(&fdma_chan->tasklet);
			db &= ~(BIT(chan));
		}
	}
	if (err) {
		u32 err_type = LAN_RD(lan966x, FDMA_ERRORS);

		pr_err("%s:%d %s: ERR int: 0x%x\n",
		       __FILE__, __LINE__, __func__, err);
		pr_err("%s:%d %s: errtype: 0x%x\n",
		       __FILE__, __LINE__, __func__, err_type);

		LAN_WR(err, lan966x, FDMA_INTR_ERR);
		LAN_WR(err_type, lan966x, FDMA_ERRORS);

		err = FDMA_INTR_ERR_INTR_CH_ERR_X(err);

		while (err) {
			u32 chan = __fls(err);
			struct lan966x_fdma_channel *fdma_chan =
				&lan966x->chans[chan];

			lan966x_fdma_notify_clients_error(fdma_chan);
			err &= ~(BIT(chan));
		}
	}

	pr_debug("%s:%d %s: end\n",
		__FILE__, __LINE__, __func__);

	return IRQ_HANDLED;
}

struct {
	enum lan966x_target id;
	char *name;
} res[] = {
	{ TARGET_FDMA, "fdma" },
};

static int lan966x_fdma_probe(struct platform_device *pdev)
{
	struct device_node *devnode = pdev->dev.of_node;
	struct lan966x_fdma *lan966x;
	int ret, nr_channels;
	int i, j;

	if (!devnode) {
		dev_err(&pdev->dev, "Did not find frame dma device tree node\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(devnode, "dma-channels", &nr_channels);
	if (ret) {
		dev_err(&pdev->dev, "Cannot get dma-channels\n");
		ret = -ENODEV;
		goto out_freetreenode;
	}

	lan966x = devm_kzalloc(&pdev->dev, sizeof(*lan966x) + nr_channels *
			       sizeof(struct lan966x_fdma_channel), GFP_KERNEL);
	if (!lan966x) {
		ret = -ENOMEM;
		goto out_freetreenode;
	}

	lan966x->nr_pchans = nr_channels;
	platform_set_drvdata(pdev, lan966x);

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);

	/* Use slave mode DMA */
	dma_cap_set(DMA_SLAVE, lan966x->dma.cap_mask);
	lan966x->dma.dev = &pdev->dev;

	/* Create a pool of consistent memory blocks for hardware descriptors */
	lan966x->dcb_pool = dmam_pool_create("lan966x-fdma-dcb",
					     lan966x->dma.dev,
					     sizeof(struct lan966x_fdma_dcb),
					     FDMA_BUFFER_ALIGN, 0);
	if (!lan966x->dcb_pool) {
		dev_err(&pdev->dev, "Unable to allocate DMA descriptor pool\n");
		ret = -ENOMEM;
		goto out_free;
	}

	ret = of_reserved_mem_device_init_by_idx(lan966x->dma.dev,
						 devnode, 0);
	INIT_LIST_HEAD(&lan966x->dma.channels);
	for (i = 0; i < nr_channels; i++) {
		struct lan966x_fdma_channel *fdma_chan = &lan966x->chans[i];

		fdma_chan->chan.device = &lan966x->dma;
		fdma_chan->state = DCS_IDLE;
		INIT_LIST_HEAD(&fdma_chan->queued_dcbs);
		fdma_chan->dbirq_pattern = 0x7;
		fdma_chan->drv = lan966x;

		if (i >= FDMA_XTR_CHANNEL) {
			tasklet_init(&fdma_chan->tasklet,
				     lan966x_fdma_xtr_tasklet,
				     (unsigned long)fdma_chan);
		} else {
			tasklet_init(&fdma_chan->tasklet,
				     lan966x_fdma_inj_tasklet,
				     (unsigned long)fdma_chan);
		}

		spin_lock_init(&fdma_chan->lock);
		list_add_tail(&fdma_chan->chan.device_node,
			      &lan966x->dma.channels);

		INIT_LIST_HEAD(&fdma_chan->free_dcbs);
		for (j = 0; j < FDMA_DCB_MAX; ++j) {
			struct lan966x_fdma_dcb *dcb;
			dma_addr_t dcb_phys;

			dcb = dma_pool_zalloc(lan966x->dcb_pool, GFP_KERNEL,
					      &dcb_phys);
			if (dcb) {
				dcb->phys = dcb_phys;
				list_add(&dcb->node, &fdma_chan->free_dcbs);
			}
		}

		fdma_chan->stats.free_dcbs = FDMA_DCB_MAX;
		fdma_chan->stats.free_dcbs_low_mark = FDMA_DCB_MAX;
	}

	/* Provide DMA Engine device interface */
	lan966x->dma.dev = &pdev->dev;
	lan966x->dma.device_alloc_chan_resources = lan966x_fdma_alloc_chan_resources;
	lan966x->dma.device_free_chan_resources = lan966x_fdma_free_chan_resources;
	lan966x->dma.device_prep_slave_sg = lan966x_fdma_prep_slave_sg;
	lan966x->dma.device_tx_status = lan966x_fdma_tx_status;
	lan966x->dma.device_issue_pending = lan966x_fdma_issue_pending;
	lan966x->dma.device_terminate_all = lan966x_fdma_terminate;
	lan966x->dma.device_synchronize = lan966x_fdma_sync;
	lan966x->dma.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
	lan966x->dma.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
	lan966x->dma.directions = BIT(DMA_MEM_TO_MEM);
	lan966x->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;

	/* Register DMA Engine device */
	ret = dma_async_device_register(&lan966x->dma);
	if (ret) {
		dev_err(&pdev->dev, "Could not register DMA engine device\n");
		goto out_free;
	}

	/* Register DMA controller (uses "dmas" and "dma-names" in DT) */
	ret = of_dma_controller_register(devnode, of_dma_xlate_by_chan_id,
					 lan966x);
	if (ret) {
		dev_err(&pdev->dev, "Could not register DMA controller\n");
		goto out_unregister;
	}

	/* Get resources */
	for (i = 0; i < ARRAY_SIZE(res); i++) {
		struct resource *resource;

		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							res[i].name);
		if (!resource) {
			ret = -ENODEV;
			goto out_unregister;
		}

		lan966x->regs[res[i].id] = devm_ioremap_resource(&pdev->dev,
								 resource);
		if (IS_ERR(lan966x->regs[res[i].id])) {
			dev_err(&pdev->dev, "Unable to map fdma registers\n");
			ret = PTR_ERR(lan966x->regs[res[i].id]);
			goto out_unregister;
		}
	}

	lan966x->irq = platform_get_irq_byname(pdev, "fdma");
	ret = devm_request_threaded_irq(&pdev->dev, lan966x->irq, NULL,
					lan966x_fdma_interrupt, IRQF_ONESHOT,
					"fdma interrupt", lan966x);
	if (ret) {
		dev_err(&pdev->dev, "Could not request IRQ\n");
		goto out_unregister;
	}

	return 0;

out_unregister:
	dma_async_device_unregister(&lan966x->dma);
out_free:
	kfree(lan966x);
out_freetreenode:
	of_node_put(devnode);
	return ret;
}

static int lan966x_fdma_remove(struct platform_device *pdev)
{
	struct lan966x_fdma *lan966x = platform_get_drvdata(pdev);
	struct lan966x_fdma_channel *fdma_chan;
	struct lan966x_fdma_dcb *iter;
	int i, chan;

	for (chan = 0; chan < lan966x->nr_pchans; ++chan) {
		fdma_chan = &lan966x->chans[chan];

		lan966x_fdma_free_chan_resources(&fdma_chan->chan);
		lan966x_fdma_sync(&fdma_chan->chan);

		/* Free DCB lists */
		list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
			dma_pool_free(lan966x->dcb_pool, iter, iter->phys);
		}

		list_for_each_entry(iter, &fdma_chan->free_dcbs, node) {
			dma_pool_free(lan966x->dcb_pool, iter, iter->phys);
		}
	}

	devm_free_irq(lan966x->dma.dev, lan966x->irq, lan966x);

	for (i = 0; i < ARRAY_SIZE(res); i++)
		iounmap(lan966x->regs[res[i].id]);


	kfree(lan966x);

	return 0;
}

static const struct of_device_id lan966x_fdma_match[] = {
	{ .compatible = "lan966x-fdma", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lan966x_fdma_match);

static struct platform_driver lan966x_fdma_driver = {
	.probe = lan966x_fdma_probe,
	.remove = lan966x_fdma_remove,
	.driver = {
		.name = "lan966x-fdma",
		.of_match_table = of_match_ptr(lan966x_fdma_match),
	},
};

static int __init lan966x_fdma_init(void)
{
	return platform_driver_register(&lan966x_fdma_driver);
}

static void __exit lan966x_fdma_exit(void)
{
	platform_driver_unregister(&lan966x_fdma_driver);
}

subsys_initcall(lan966x_fdma_init);
module_exit(lan966x_fdma_exit);

MODULE_DESCRIPTION("Microchip LAN966X FDMA driver");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
MODULE_LICENSE("Dual MIT/GPL");
