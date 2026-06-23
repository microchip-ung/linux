// SPDX-License-Identifier: GPL-2.0+
/* Microchip lan969x Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */
#include <net/page_pool/helpers.h>

#include "../sparx5_main.h"
#include "../sparx5_main_regs.h"
#include "../sparx5_port.h"

#include "fdma_api.h"
#include "lan969x.h"

#define FDMA_PRIV(fdma) ((struct sparx5 *)((fdma)->priv))

static int lan969x_fdma_tx_dataptr_cb(struct fdma *fdma, int dcb, int db,
				      u64 *dataptr)
{
	struct sparx5_tx_buf *db_buf = &FDMA_PRIV(fdma)->tx.dbs[dcb];

	*dataptr = db_buf->dma_addr + db_buf->offset;

	return 0;
}

static int lan969x_fdma_rx_dataptr_cb(struct fdma *fdma, int dcb, int db,
				      u64 *dataptr)
{
	struct sparx5_rx *rx = &FDMA_PRIV(fdma)->rx;
	struct page *page;

	page = page_pool_dev_alloc_pages(rx->page_pool);
	if (unlikely(!page))
		return -ENOMEM;

	rx->page[dcb][db] = page;

	*dataptr = page_pool_get_dma_addr(page) + XDP_PACKET_HEADROOM;

	return 0;
}

static int lan969x_fdma_get_next_dcb(struct sparx5_tx *tx)
{
	struct fdma *fdma = &tx->fdma;

	for (int i = 0; i < fdma->n_dcbs; ++i)
		if (!tx->dbs[i].used && !fdma_is_last(fdma, &fdma->dcbs[i]))
			return i;

	return -ENOSPC;
}

static void lan969x_fdma_tx_clear_buf(struct sparx5 *sparx5, int weight)
{
	struct net_device *ndev = sparx5->rx.ndev;
	struct fdma *fdma = &sparx5->tx.fdma;
	struct xdp_frame_bulk bq;
	struct sparx5_tx_buf *db;
	bool clear = false;
	int i;

	xdp_frame_bulk_init(&bq);

	spin_lock(&sparx5->tx_lock);

	for (i = 0; i < fdma->n_dcbs; ++i) {
		db = &sparx5->tx.dbs[i];

		if (!db->used)
			continue;

		if (!fdma_db_is_done(fdma_db_get(fdma, i, 0)))
			continue;

		db->dev->stats.tx_bytes += db->skb->len;
		db->dev->stats.tx_packets++;
		sparx5->tx.packets++;

		/* Free the resource based on the data type. */
		switch (db->data_type) {
		case SPX5_DB_DATA_TYPE_SKB:
			/* Normal SKB */

		dma_unmap_single(sparx5->dev,
				 db->dma_addr,
				 db->skb->len,
				 DMA_TO_DEVICE);

		if (!db->ptp)
			napi_consume_skb(db->skb, weight);
break;
		case SPX5_DB_DATA_TYPE_XDPF:
			/* XDP_REDIRECT or AF_XDP */

			dma_unmap_single(sparx5->dev,
					 db->dma_addr,
					 db->len,
					 DMA_TO_DEVICE);

			xdp_return_frame_bulk(db->data.xdpf, &bq);
			break;
		case SPX5_DB_DATA_TYPE_PAGE:
			/* XDP_TX */

			page_pool_recycle_direct(sparx5->rx.page_pool,
						 db->data.page);
			break;
		default:
			/* Should not happen */
			break;
		}

		db->used = false;
		clear = true;
	}

	xdp_flush_frame_bulk(&bq);

	if (clear && netif_queue_stopped(ndev))
		netif_wake_queue(ndev);

	spin_unlock(&sparx5->tx_lock);
}

static void lan969x_fdma_free_page(struct sparx5_rx *rx)
{
	struct fdma *fdma = &rx->fdma;
	struct page *page;

	page = rx->page[fdma->dcb_index][fdma->db_index];
	if (unlikely(!page))
		return;

	page_pool_recycle_direct(rx->page_pool, page);
}

static void lan969x_fdma_free_pages(struct sparx5_rx *rx)
{
	struct fdma *fdma = &rx->fdma;

	for (int i = 0; i < fdma->n_dcbs; ++i) {
		for (int j = 0; j < fdma->n_dbs; ++j)
			page_pool_put_full_page(rx->page_pool,
						rx->page[i][j], false);
	}
}

static int lan969x_fdma_rx_process_frame(struct sparx5 *sparx5, int *src_port,
					 u64 *rx_timestamp)
{
	const struct sparx5_consts *consts = sparx5->data->consts;
	struct sparx5_rx *rx = &sparx5->rx;
	struct fdma *fdma = &rx->fdma;
	struct sparx5_port *port;
	struct frame_info fi;
	struct fdma_db *db;
	struct page *page;

	db = &fdma->dcbs[fdma->dcb_index].db[fdma->db_index];
	page = rx->page[fdma->dcb_index][fdma->db_index];

	sparx5_ifh_parse(sparx5,
			 (u32 *) ((char *)page_address(page) + XDP_PACKET_HEADROOM),
			 &fi);

	*src_port = fi.src_port;
	*rx_timestamp = fi.timestamp;

#ifdef CONFIG_SPARX5_SWITCH_APPL
	*src_port = 0;
	port = sparx5->ports[0];
#else
	port = fi.src_port < consts->n_ports ? sparx5->ports[fi.src_port] :
						  NULL;
#endif
	if (WARN_ON(fi.src_port >= consts->n_ports))
		return FDMA_ERROR;

	if (!sparx5_port_has_xdp(port))
		return FDMA_PASS;

	return sparx5_xdp_run(port, page, fdma_db_len_get(db));
}

static void lan969x_set_redundancy_info(struct sparx5 *sparx5,
					struct sk_buff *skb)
{
	/* HSR tag removal is offloaded to hardware.
	 * Extract sequence number, path ID, and ingress leg from IFH metadata
	 * so userspace (e.g. ptp4l) can still correlate redundancy paths.
	 */

	/* IFH was already stripped; restore it. */
	u8 *ifh = skb->data - IFH_LEN * sizeof(u32);
	struct skb_redundancy_info *sred;
	u16 seqno, pathid;
	u8 src;

	/* We only care about frames that were HSR tagged. */
	if (!sparx5_get_ifh_field(sparx5, ifh, IFH_RB_TAG)) {
		pr_debug("%s: frame was not HSR tagged", __func__);
		return;
	}

	src = sparx5_get_ifh_field(sparx5, ifh, IFH_RB_SRC);
	seqno = sparx5_get_ifh_field(sparx5, ifh, IFH_TAGGING_SEQ_NO);

	/* Path ID encoding:
	 *  - bits [2:0] from MPLS SBIT
	 *  - bit  [3]   from MPLS TC
	 */
	pathid = (sparx5_get_ifh_field(sparx5, ifh, IFH_MPLS_SBIT) & 0x7) |
		 ((sparx5_get_ifh_field(sparx5, ifh, IFH_MPLS_TC) & 0x1) << 3);

	sred = skb_redinfo(skb);
	sred->io_port = (PTP_MSG_IN | BIT(src));
	sred->seqnr = seqno;
	sred->pathid = pathid;

	pr_debug("%s: src=%u leg=%c msg: %s io_port=0x%02x seq=%u path=%u\n",
		__func__,
		src,
		src ? 'B' : 'A',
		sparx5_ptp_msg_type_str(skb),
		sred->io_port,
		sred->seqnr,
		sred->pathid);
}

static struct sk_buff *lan969x_fdma_rx_get_frame(struct sparx5 *sparx5,
						 struct sparx5_rx *rx,
						 int src_port,
						 u64 rx_timestamp)
{
	struct fdma *fdma = &rx->fdma;
	struct sparx5_port *port;
	struct sk_buff *skb;
	struct fdma_db *db;
	struct page *page;

	db = &fdma->dcbs[fdma->dcb_index].db[fdma->db_index];
	page = rx->page[fdma->dcb_index][fdma->db_index];

	skb = build_skb(page_address(page), fdma->db_size);
	if (unlikely(!skb))
		goto free_page;

	skb_mark_for_recycle(skb);
	skb_reserve(skb, XDP_PACKET_HEADROOM);
	skb_put(skb, fdma_db_len_get(db));

	port = sparx5->ports[src_port];
	skb->dev = port->ndev;
#ifdef CONFIG_SPARX5_SWITCH_APPL
	if (pskb_expand_head(skb, IFH_ENCAP_LEN, 0, GFP_ATOMIC)) {
		kfree_skb(skb);
		goto free_page;
	}

	*(u16 *)skb_push(skb, sizeof(u16)) = is_sparx5(sparx5) ? htons(11) :
								 htons(14);
	*(u16 *)skb_push(skb, sizeof(u16)) = htons(IFH_ETH_TYPE);
	ether_addr_copy((u8 *)skb_push(skb, ETH_ALEN), ifh_smac);
	ether_addr_copy((u8 *)skb_push(skb, ETH_ALEN), ifh_dmac);
#else
	skb_pull(skb, IFH_LEN * sizeof(u32));

	skb->dev = port->ndev;

	if (likely(!(skb->dev->features & NETIF_F_RXFCS)))
		skb_trim(skb, skb->len - ETH_FCS_LEN);

	lan969x_set_redundancy_info(sparx5, skb);
#endif
	sparx5_ptp_rxtstamp(sparx5, skb, src_port, rx_timestamp);
	skb->protocol = eth_type_trans(skb, skb->dev);

	if (test_bit(port->portno, sparx5->bridge_mask)) {
		skb->offload_fwd_mark = 1;
		skb_reset_network_header(skb);

		if (!sparx5_skb_offloaded(sparx5, src_port, skb))
			skb->offload_fwd_mark = 0;
	}

	skb->dev->stats.rx_bytes += skb->len;
	skb->dev->stats.rx_packets++;

	return skb;

free_page:
	page_pool_recycle_direct(rx->page_pool, page);

	return NULL;
}

static int lan969x_fdma_rx_alloc(struct sparx5 *sparx5)
{
	bool has_xdp = sparx5_has_xdp(sparx5);
	struct sparx5_rx *rx = &sparx5->rx;
	struct fdma *fdma = &rx->fdma;
	int err;

	struct page_pool_params pp_params = {
		.order = rx->page_order,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = fdma->n_dcbs * fdma->n_dbs,
		.nid = NUMA_NO_NODE,
		.dev = sparx5->dev,
		.dma_dir = has_xdp ? DMA_BIDIRECTIONAL : DMA_FROM_DEVICE,
		.offset = XDP_PACKET_HEADROOM,
		.max_len = fdma->db_size -
			   SKB_DATA_ALIGN(sizeof(struct skb_shared_info)),
	};

	rx->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(rx->page_pool))
		return PTR_ERR(rx->page_pool);

	sparx5_xdp_mem_type_set(sparx5, MEM_TYPE_PAGE_POOL, rx->page_pool);

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
	struct fdma *fdma = &tx->fdma;
	int err;

	tx->dbs = kcalloc(fdma->n_dcbs,
			  sizeof(struct sparx5_tx_buf),
			  GFP_KERNEL);
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

static void lan969x_fdma_rx_init(struct sparx5 *sparx5)
{
	struct fdma *fdma = &sparx5->rx.fdma;

	sparx5->rx.page_order =
		round_up(sparx5->tx.max_mtu, PAGE_SIZE) / PAGE_SIZE - 1;
	fdma->channel_id = FDMA_XTR_CHANNEL;
	fdma->n_dcbs = FDMA_DCB_MAX;
	fdma->n_dbs = 1;
	fdma->priv = sparx5;
	fdma->size = fdma_get_size(fdma);
	fdma->db_size = PAGE_SIZE << sparx5->rx.page_order;
	fdma->ops.dataptr_cb = &lan969x_fdma_rx_dataptr_cb;
	fdma->ops.nextptr_cb = &fdma_nextptr_cb;

	/* Fetch a netdev for SKB and NAPI use, any will do */
	for (int idx = 0; idx < sparx5->data->consts->n_ports; ++idx) {
		struct sparx5_port *port = sparx5->ports[idx];

		if (port && port->ndev) {
			sparx5->rx.ndev = port->ndev;
			break;
		}
	}
}

static void lan969x_fdma_tx_init(struct sparx5 *sparx5)
{
	struct fdma *fdma = &sparx5->tx.fdma;

	fdma->channel_id = FDMA_INJ_CHANNEL;
	fdma->n_dcbs = FDMA_DCB_MAX;
	fdma->n_dbs = 1;
	fdma->priv = sparx5;
	fdma->size = fdma_get_size(fdma);
	fdma->db_size = PAGE_SIZE;
	fdma->ops.dataptr_cb = &lan969x_fdma_tx_dataptr_cb;
	fdma->ops.nextptr_cb = &fdma_nextptr_cb;
}

int lan969x_fdma_napi_poll(struct napi_struct *napi, int weight)
{
	struct sparx5_rx *rx = container_of(napi, struct sparx5_rx, napi);
	struct sparx5 *sparx5 = container_of(rx, struct sparx5, rx);
	int old_dcb, dcb_reload, counter = 0;
	struct fdma *fdma = &rx->fdma;
	bool redirect = false;
	struct sk_buff *skb;
	u64 rx_timestamp;
	int src_port;

	dcb_reload = fdma->dcb_index;

	lan969x_fdma_tx_clear_buf(sparx5, weight);

	dcb_reload = fdma->dcb_index;

	/* Get all received skb */
	while (counter < weight) {
		if (!fdma_has_frames(fdma))
			break;

		counter++;

		switch (lan969x_fdma_rx_process_frame(sparx5, &src_port,
						      &rx_timestamp)) {
		case FDMA_PASS:
			break;
		case FDMA_ERROR:
			lan969x_fdma_free_page(rx);
			fdma_dcb_advance(fdma);
			goto allocate_new;
		case FDMA_REDIRECT:
			redirect = true;
			fallthrough;
		case FDMA_TX:
			fdma_dcb_advance(fdma);
			continue;
		case FDMA_DROP:
			lan969x_fdma_free_page(rx);
			fdma_dcb_advance(fdma);
			continue;
		}

		skb = lan969x_fdma_rx_get_frame(sparx5, rx, src_port,
						rx_timestamp);
		if (!skb)
			break;

		napi_gro_receive(&rx->napi, skb);

		fdma_db_advance(fdma);

		/* Check if the DCB can be reused */
		if (fdma_dcb_is_reusable(fdma))
			continue;

		fdma_db_reset(fdma);
		fdma_dcb_advance(fdma);
	}

allocate_new:
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

	if (redirect)
		/* Flush bulk queues to complete the redirect. */
		xdp_do_flush();

	if (counter < weight && napi_complete_done(napi, counter))
		spx5_wr(0xff, sparx5, FDMA_INTR_DB_ENA);

	return counter;
}

int lan969x_fdma_xmit(struct sparx5 *sparx5, u32 *ifh, struct sk_buff *skb,
		      struct net_device *dev)
{
	int next_dcb, needed_headroom, needed_tailroom, err;
	struct sparx5_tx *tx = &sparx5->tx;
	struct fdma *fdma = &tx->fdma;
	struct sparx5_tx_buf *db_buf;
	u64 status;

	next_dcb = lan969x_fdma_get_next_dcb(tx);
	if (next_dcb < 0) {
		netif_stop_queue(sparx5->rx.ndev);
		return NETDEV_TX_BUSY;
	}

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
	skb_put(skb, ETH_FCS_LEN);

	db_buf = &tx->dbs[next_dcb];
	db_buf->dma_addr = dma_map_single(sparx5->dev,
					  skb->data,
					  skb->len,
					  DMA_TO_DEVICE);
	if (dma_mapping_error(sparx5->dev, db_buf->dma_addr))
		return -ENOMEM;

	db_buf->len = skb->len;
	db_buf->dev = dev;
	db_buf->skb = skb;
	db_buf->ptp = false;
	db_buf->used = true;
	db_buf->offset = 0;
	db_buf->data_type = SPX5_DB_DATA_TYPE_SKB;

	if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP &&
	    SPARX5_SKB_CB(skb)->rew_op == IFH_REW_OP_TWO_STEP_PTP)
		db_buf->ptp = true;

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

int lan969x_fdma_xmit_xdp(struct sparx5_port *port, void *data, u32 len)
{
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_tx *tx = &sparx5->tx;
	struct fdma *fdma = &tx->fdma;
	struct sparx5_tx_buf *db_buf;
	dma_addr_t dma_addr;
	struct page *page;
	int next_dcb;
	__be32 *ifh;
	u64 status;

	next_dcb = lan969x_fdma_get_next_dcb(tx);

	if (next_dcb < 0) {
		netif_stop_queue(sparx5->rx.ndev);
		return NETDEV_TX_BUSY;
	}

	db_buf = &tx->dbs[next_dcb];

	if (!len) {
		/* XDP_REDIRECT or AF_XDP */
		struct xdp_frame *xdpf = data;

		if (xdpf->headroom < IFH_LEN_BYTES)
			return NETDEV_TX_OK;

		ifh = xdpf->data - IFH_LEN_BYTES;
		memset(ifh, 0, IFH_LEN_BYTES);

		sparx5_set_port_ifh(sparx5,
				    ifh,
				    port->portno);

		dma_addr = dma_map_single(sparx5->dev,
					  xdpf->data - IFH_LEN_BYTES,
					  xdpf->len + IFH_LEN_BYTES,
					  DMA_TO_DEVICE);

		if (dma_mapping_error(sparx5->dev, dma_addr))
			return NETDEV_TX_OK;

		db_buf->data.xdpf = xdpf;
		db_buf->len = xdpf->len + IFH_LEN_BYTES;
		db_buf->data_type = SPX5_DB_DATA_TYPE_XDPF;
		db_buf->offset = 0;

	} else {
		/* XDP_TX */
		page = data;

		ifh = page_address(page) + XDP_PACKET_HEADROOM;
		memset(ifh, 0, IFH_LEN_BYTES);

		sparx5_set_port_ifh(sparx5,
				    ifh,
				    port->portno);

		dma_addr = page_pool_get_dma_addr(page);

		dma_sync_single_for_device(sparx5->dev,
					   dma_addr + XDP_PACKET_HEADROOM,
					   len + IFH_LEN_BYTES,
					   DMA_TO_DEVICE);

		db_buf->data.page = page;
		db_buf->len = len + IFH_LEN_BYTES;
		db_buf->data_type = SPX5_DB_DATA_TYPE_PAGE;
		db_buf->offset = XDP_PACKET_HEADROOM;
	}

	db_buf->dma_addr = dma_addr;
	db_buf->used = true;
	db_buf->ptp = false;

	status = FDMA_DCB_STATUS_SOF |
		 FDMA_DCB_STATUS_EOF |
		 FDMA_DCB_STATUS_BLOCKO(0) |
		 FDMA_DCB_STATUS_BLOCKL(db_buf->len);

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
	struct sparx5_rx *rx = &sparx5->rx;
	int err;

	sparx5->tx.max_mtu = sparx5->data->ops->get_mtu(sparx5);

	lan969x_fdma_rx_init(sparx5);
	lan969x_fdma_tx_init(sparx5);
	sparx5_fdma_injection_mode(sparx5);

	err = dma_set_mask_and_coherent(sparx5->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(sparx5->dev, "Failed to set 64-bit FDMA mask");
		return err;
	}

	err = lan969x_fdma_rx_alloc(sparx5);
	if (err) {
		dev_err(sparx5->dev, "Failed to allocate RX buffers: %d\n",
			err);
		return err;
	}

	err = lan969x_fdma_tx_alloc(sparx5);
	if (err) {
		fdma_free_coherent(sparx5->dev, &rx->fdma);
		dev_err(sparx5->dev, "Failed to allocate TX buffers: %d\n",
			err);
		return err;
	}

	/* Reset FDMA state */
	spx5_wr(FDMA_CTRL_NRESET_SET(0), sparx5, FDMA_CTRL);
	spx5_wr(FDMA_CTRL_NRESET_SET(1), sparx5, FDMA_CTRL);

	return err;
}

int lan969x_fdma_deinit(struct sparx5 *sparx5)
{
	struct sparx5_rx *rx = &sparx5->rx;
	struct sparx5_tx *tx = &sparx5->tx;

	sparx5_fdma_stop(sparx5);
	fdma_free_coherent(sparx5->dev, &tx->fdma);
	fdma_free_coherent(sparx5->dev, &rx->fdma);
	lan969x_fdma_free_pages(rx);
	page_pool_destroy(rx->page_pool);

	return 0;
}

int lan969x_fdma_resize(struct sparx5 *sparx5)
{
	struct page_pool *page_pool_old = sparx5->rx.page_pool;
	u32 old_mtu = sparx5->tx.max_mtu;
	struct fdma tx_fdma_old = {0};
	struct fdma rx_fdma_old = {0};
	int err;

	memcpy(&tx_fdma_old, &sparx5->tx.fdma, sizeof(struct fdma));
	memcpy(&rx_fdma_old, &sparx5->rx.fdma, sizeof(struct fdma));

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
	memcpy(&sparx5->tx.fdma, &tx_fdma_old, sizeof(struct fdma));
	memcpy(&sparx5->rx.fdma, &rx_fdma_old, sizeof(struct fdma));

	/* Old buffers have to be re-initialized. */

	fdma_dcbs_init(&sparx5->rx.fdma,
		       FDMA_DCB_INFO_DATAL(sparx5->rx.fdma.db_size),
		       FDMA_DCB_STATUS_INTR);

	fdma_dcbs_init(&sparx5->tx.fdma,
		       FDMA_DCB_INFO_DATAL(sparx5->tx.fdma.db_size),
		       FDMA_DCB_STATUS_DONE);

start:
	sparx5_fdma_start(sparx5);

	return err;
}
