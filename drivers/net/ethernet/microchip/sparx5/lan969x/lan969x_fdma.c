// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2023 Microchip Technology Inc. and its subsidiaries.
 *
 * The Sparx5 Chip Register Model can be browsed at this location:
 * https://github.com/microchip-ung/sparx-5_reginfo
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <net/page_pool/helpers.h>

#include "lan969x.h"

static int lan969x_fdma_tx_dataptr_cb(struct fdma *fdma, int dcb, int db,
				      u64 *dataptr)
{
	struct sparx5 *sparx5 = (struct sparx5 *)fdma->priv;

	*dataptr = sparx5->tx.dbs[dcb].dma_addr;

	return 0;
}

static int lan969x_fdma_rx_dataptr_cb(struct fdma *fdma, int dcb, int db,
				      u64 *dataptr)
{
	struct sparx5 *sparx5 = (struct sparx5 *)fdma->priv;
	struct sparx5_rx *rx = &sparx5->rx;
	struct page *page;

	page = page_pool_dev_alloc_pages(rx->page_pool);
	if (unlikely(!page))
		return -ENOMEM;

	rx->page[dcb][db] = page;
	*dataptr = page_pool_get_dma_addr(page);

	return 0;
}

static void lan969x_fdma_tx_clear_buf(struct sparx5 *sparx5, int weight)
{
	struct net_device *ndev = sparx5->rx.ndev;
	struct fdma *fdma = sparx5->tx.fdma;
	struct sparx5_db *db;
	unsigned long flags;
	bool clear = false;
	int i;

	spin_lock_irqsave(&sparx5->tx_lock, flags);

	for (i = 0; i < fdma->n_dcbs; ++i) {
		db = &sparx5->tx.dbs[i];

		if (!db->used)
			continue;
		if (!fdma_db_is_done(fdma_db_get(fdma, i, 0)))
			continue;

		dma_unmap_single(sparx5->dev,
				 db->dma_addr,
				 db->len,
				 DMA_TO_DEVICE);

		if (!db->ptp)
			napi_consume_skb(db->skb, weight);

		db->used = false;
		clear = true;
	}

	if (clear && netif_queue_stopped(ndev))
		netif_wake_queue(ndev);

	spin_unlock_irqrestore(&sparx5->tx_lock, flags);
}

static void lan969x_fdma_free_pages(struct sparx5_rx *rx)
{
	struct fdma *fdma = rx->fdma;
	int i, j;

	for (i = 0; i < fdma->n_dcbs; ++i) {
		for (j = 0; j < fdma->n_dbs; ++j)
			page_pool_put_full_page(rx->page_pool,
						rx->page[i][j], false);
	}
}

static struct sk_buff *lan969x_fdma_rx_get_frame(struct sparx5 *sparx5,
						 struct sparx5_rx *rx)
{
	struct fdma *fdma = rx->fdma;
	struct sparx5_port *port;
	struct frame_info fi;
	struct sk_buff *skb;
	struct fdma_db *db;
	struct page *page;

	/* Get the received frame and unmap it */
	db = &fdma->dcbs[fdma->dcb_index].db[fdma->db_index];
	page = rx->page[fdma->dcb_index][fdma->db_index];

	skb = build_skb(page_address(page), fdma->db_size);
	if (unlikely(!skb))
		goto free;

	skb_mark_for_recycle(skb);
	skb_put(skb, fdma_db_len_get(db));

	sparx5_ifh_parse(sparx5, (u32 *)skb->data, &fi);
#ifdef CONFIG_SPARX5_SWITCH_APPL
	port = sparx5->ports[0];
#else
	port = fi.src_port < sparx5->data->consts.chip_ports ? sparx5->ports[fi.src_port] :
						  NULL;
#endif

	if (WARN_ON(fi.src_port >= sparx5->data->consts.chip_ports))
		goto free_skb;

	skb->dev = port->ndev;
#ifdef CONFIG_SPARX5_SWITCH_APPL
	if (pskb_expand_head(skb, IFH_ENCAP_LEN, 0, GFP_ATOMIC))
		goto free_skb;

	*(u16 *)skb_push(skb, sizeof(u16)) = htons(sparx5->data->consts.ifh_id);
	*(u16 *)skb_push(skb, sizeof(u16)) = htons(IFH_ETH_TYPE);
	ether_addr_copy((u8 *)skb_push(skb, ETH_ALEN), ifh_smac);
	ether_addr_copy((u8 *)skb_push(skb, ETH_ALEN), ifh_dmac);
#else
	skb_pull(skb, IFH_LEN * sizeof(u32));

	if (likely(!(skb->dev->features & NETIF_F_RXFCS)))
		skb_trim(skb, skb->len - ETH_FCS_LEN);
#endif

	sparx5_ptp_rxtstamp(sparx5, skb, fi.src_port, fi.timestamp);
	skb->protocol = eth_type_trans(skb, skb->dev);

	if (test_bit(port->portno, sparx5->bridge_mask)) {
		skb->offload_fwd_mark = 1;
		skb_reset_network_header(skb);
	}

	skb->dev->stats.rx_bytes += skb->len;
	skb->dev->stats.rx_packets++;

	return skb;

free_skb:
	kfree_skb(skb);
free:

	page_pool_recycle_direct(rx->page_pool, page);

	return NULL;
}

int lan969x_fdma_napi_poll(struct napi_struct *napi, int weight)
{
	struct sparx5_rx *rx = container_of(napi, struct sparx5_rx, napi);
	struct sparx5 *sparx5 = container_of(rx, struct sparx5, rx);
	int old_dcb, dcb_reload, counter = 0;
	struct fdma *fdma = rx->fdma;
	struct sk_buff *skb;

	dcb_reload = fdma->dcb_index;

	lan969x_fdma_tx_clear_buf(sparx5, weight);

	dcb_reload = fdma->dcb_index;

	/* Get all received skb */
	while (counter < weight) {
		if (!fdma_has_frames(fdma))
			break;

		skb = lan969x_fdma_rx_get_frame(sparx5, rx);
		if (!skb)
			break;

		napi_gro_receive(&rx->napi, skb);

		fdma_db_advance(fdma);
		counter++;
		/* Check if the DCB can be reused */
		if (fdma_dcb_is_reusable(fdma))
			continue;

		fdma_db_reset(fdma);
		fdma_dcb_advance(fdma);
	}

	/* Allocate new pages and map them */
	while (dcb_reload != fdma->dcb_index) {
		old_dcb = dcb_reload;
		dcb_reload++;
		dcb_reload &= fdma->n_dcbs - 1;

		fdma_dcb_add(fdma,
			     old_dcb,
			     FDMA_DCB_INFO_DATAL(fdma->db_size),
			     FDMA_DCB_STATUS_INTR);

		sparx5_fdma_reload(sparx5, fdma);
	}

	if (counter < weight && napi_complete_done(napi, counter))
		spx5_wr(0xff, sparx5, FDMA_INTR_DB_ENA);

	return counter;
}

static int lan969x_fdma_rx_alloc(struct sparx5 *sparx5, struct sparx5_rx *rx)
{
	struct fdma *fdma = rx->fdma;
	int err;

	struct page_pool_params pp_params = {
		.order = rx->page_order,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = fdma->n_dcbs * fdma->n_dbs,
		.nid = NUMA_NO_NODE,
		.dev = sparx5->dev,
		.dma_dir = DMA_FROM_DEVICE,
		.offset = 0,
		.max_len = fdma->db_size -
			   SKB_DATA_ALIGN(sizeof(struct skb_shared_info)),
	};

	rx->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(rx->page_pool))
		return PTR_ERR(rx->page_pool);

	err = fdma_alloc_coherent(sparx5->dev, fdma);
	if (err)
		return err;

	fdma_dcbs_init(fdma,
		       FDMA_DCB_INFO_DATAL(fdma->db_size),
		       FDMA_DCB_STATUS_INTR);

	return 0;
}

static int lan969x_fdma_tx_alloc(struct sparx5 *sparx5)
{
	struct sparx5_tx *tx = &sparx5->tx;
	struct fdma *fdma = tx->fdma;
	int err;

	tx->dbs = kcalloc(fdma->n_dcbs, sizeof(struct sparx5_db), GFP_KERNEL);
	if (!tx->dbs)
		return -ENOMEM;

	err = fdma_alloc_coherent(sparx5->dev, fdma);
	if (err) {
		kfree(tx->dbs);
		return err;
	}

	fdma_dcbs_init(fdma,
		       FDMA_DCB_INFO_DATAL(fdma->db_size),
		       FDMA_DCB_STATUS_DONE);

	return 0;
}

static struct fdma lan969x_fdma_tx = {
	.channel_id = FDMA_INJ_CHANNEL,
	.n_dcbs = 64,
	.n_dbs = 1,
	.ops = {
		.dataptr_cb = &lan969x_fdma_tx_dataptr_cb,
		.nextptr_cb = &fdma_nextptr_cb,
	},
};

static struct fdma lan969x_fdma_rx = {
	.channel_id = FDMA_XTR_CHANNEL,
	.n_dcbs = 64,
	.n_dbs = 1,
	.ops = {
		.dataptr_cb = &lan969x_fdma_rx_dataptr_cb,
		.nextptr_cb = &fdma_nextptr_cb,
	},
};

static int lan969x_fdma_get_next_dcb(struct sparx5_tx *tx)
{
	for (int i = 0; i < tx->fdma->n_dcbs; ++i)
		if (!tx->dbs[i].used &&
		    !fdma_is_last(tx->fdma, &tx->fdma->dcbs[i]))
			return i;

	return -1;
}

int lan969x_fdma_xmit(struct sparx5 *sparx5, u32 *ifh, struct sk_buff *skb)
{
	int next_dcb, needed_headroom, needed_tailroom, err;
	struct sparx5_tx *tx = &sparx5->tx;
	struct fdma *fdma = tx->fdma;
	struct sparx5_db *db_buf;
	u64 status;

	next_dcb = lan969x_fdma_get_next_dcb(tx);

	if (next_dcb < 0) {
		netif_stop_queue(sparx5->rx.ndev);
		return NETDEV_TX_BUSY;
	}

	db_buf = &tx->dbs[next_dcb];

	needed_headroom = max_t(int, IFH_LEN * 4 - skb_headroom(skb), 0);
	needed_tailroom = max_t(int, ETH_FCS_LEN - skb_tailroom(skb), 0);
	if (needed_headroom || needed_tailroom || skb_header_cloned(skb)) {
		err = pskb_expand_head(skb, needed_headroom, needed_tailroom,
				       GFP_ATOMIC);
		if (unlikely(err))
			return err;
	}

	skb_push(skb, IFH_LEN * 4);
	memcpy(skb->data, ifh, IFH_LEN * 4);
	skb_put(skb, 4);

	db_buf->dma_addr = dma_map_single(sparx5->dev, skb->data, skb->len,
					  DMA_TO_DEVICE);
	db_buf->len = skb->len;
	db_buf->used = true;
	db_buf->skb = skb;
	db_buf->ptp = false;

	if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP &&
	    SPARX5_SKB_CB(skb)->rew_op == IFH_REW_OP_TWO_STEP_PTP)
		db_buf->ptp = true;

	if (dma_mapping_error(sparx5->dev, db_buf->dma_addr))
		return -1;

	status = FDMA_DCB_STATUS_SOF |
		 FDMA_DCB_STATUS_EOF |
		 FDMA_DCB_STATUS_BLOCKO(0) |
		 FDMA_DCB_STATUS_BLOCKL(skb->len);

	/* Only require an interrupt for every other tx DCB */
	fdma_dcb_advance(fdma);
	if (fdma->dcb_index % 2)
		status |= FDMA_DCB_STATUS_INTR;

	fdma_dcb_add(fdma, next_dcb, 0, status);

	sparx5_fdma_reload(sparx5, fdma);

	return NETDEV_TX_OK;
}

int lan969x_fdma_init(struct sparx5 *sparx5)
{
	int err;

	sparx5->tx.max_mtu = sparx5->data->ops.get_mtu(sparx5);
	sparx5->rx.ndev = sparx5_fdma_get_ndev(sparx5);

	sparx5->rx.page_order =
		round_up(sparx5->tx.max_mtu, PAGE_SIZE) / PAGE_SIZE - 1;

	sparx5->tx.fdma = &lan969x_fdma_tx;
	sparx5->tx.fdma->priv = sparx5;
	sparx5->tx.fdma->size = fdma_get_size(sparx5->tx.fdma);
	sparx5->tx.fdma->db_size = PAGE_SIZE << sparx5->rx.page_order;

	sparx5->rx.fdma = &lan969x_fdma_rx;
	sparx5->rx.fdma->priv = sparx5;
	sparx5->rx.fdma->size = fdma_get_size(sparx5->rx.fdma);
	sparx5->rx.fdma->db_size = PAGE_SIZE << sparx5->rx.page_order;

	/* Reset FDMA state */
	spx5_wr(FDMA_CTRL_NRESET_SET(0), sparx5, FDMA_CTRL);
	spx5_wr(FDMA_CTRL_NRESET_SET(1), sparx5, FDMA_CTRL);

	err = dma_set_mask_and_coherent(sparx5->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(sparx5->dev, "Failed to set 64-bit FDMA mask");
		return err;
	}

	sparx5_fdma_injection_mode(sparx5);
	err = lan969x_fdma_rx_alloc(sparx5, &sparx5->rx);
	if (err) {
		fdma_free_coherent(sparx5->dev, sparx5->rx.fdma);
		dev_err(sparx5->dev, "Could not allocate RX buffers: %d\n",
			err);
		return err;
	}

	err = lan969x_fdma_tx_alloc(sparx5);
	if (err) {
		dev_err(sparx5->dev, "Could not allocate TX buffers: %d\n",
			err);
		return err;
	}

	return err;
}

int lan969x_fdma_deinit(struct sparx5 *sparx5)
{
	sparx5_fdma_stop(sparx5);
	fdma_free_coherent(sparx5->dev, sparx5->tx.fdma);
	fdma_free_coherent(sparx5->dev, sparx5->rx.fdma);
	lan969x_fdma_free_pages(&sparx5->rx);
	page_pool_destroy(sparx5->rx.page_pool);

	return 0;
}

int lan969x_fdma_resize(struct sparx5 *sparx5)
{
	struct page_pool *page_pool_old = sparx5->rx.page_pool;
	struct fdma tx_fdma_old = *sparx5->tx.fdma;
	struct fdma rx_fdma_old = *sparx5->rx.fdma;
	u32 old_mtu = sparx5->tx.max_mtu;
	int err;

	sparx5_fdma_stop(sparx5);
	lan969x_fdma_free_pages(&sparx5->rx);

	err = lan969x_fdma_init(sparx5);
	if (err)
		goto restore;

	fdma_free_coherent(sparx5->dev, &rx_fdma_old);
	fdma_free_coherent(sparx5->dev, &tx_fdma_old);
	page_pool_destroy(page_pool_old);

	goto start;

restore:

	/* At this point, the FDMA engine is stopped and the stack is not
	 * calling us for xmit. Restore the old MTU and rx,tx buffers and
	 * restart the engine.
	 */

	sparx5->tx.max_mtu = old_mtu;
	sparx5->rx.page_pool = page_pool_old;
	memcpy(sparx5->tx.fdma, &tx_fdma_old, sizeof(struct fdma));
	memcpy(sparx5->rx.fdma, &rx_fdma_old, sizeof(struct fdma));

	/* Old buffers have to be re-initialized. */

	fdma_dcbs_init(sparx5->rx.fdma,
		       FDMA_DCB_INFO_DATAL(sparx5->rx.fdma->db_size),
		       FDMA_DCB_STATUS_INTR);

	fdma_dcbs_init(sparx5->tx.fdma,
		       FDMA_DCB_INFO_DATAL(sparx5->tx.fdma->db_size),
		       FDMA_DCB_STATUS_DONE);

start:
	sparx5_fdma_start(sparx5);

	return err;
}
