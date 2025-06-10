// SPDX-License-Identifier: GPL-2.0+
/* Microchip lan969x Switch driver
 *
 * Copyright (c) 2024 Microchip Technology Inc. and its subsidiaries.
 *
 * The lan969x Chip Register Model can be browsed at this location:
 * https://github.com/microchip-ung/lan969x-industrial_reginfo
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <net/page_pool/helpers.h>

#include "lan969x.h"

static int lan969x_fdma_pci_dataptr_cb(struct fdma *fdma, int dcb, int db,
				       u64 *dataptr)
{
	*dataptr = fdma_pci_atu_get_mapped_addr(fdma->atu_region,
						fdma_dataptr_get_contiguous(fdma, dcb, db));

	return 0;
}

static int lan969x_fdma_pci_nextptr_cb(struct fdma *fdma, int dcb, u64 *nextptr)
{
	u64 addr;

	fdma_nextptr_cb(fdma, dcb, &addr);

	*nextptr = fdma_pci_atu_get_mapped_addr(fdma->atu_region, addr);

	return 0;
}

static struct sk_buff *lan969x_fdma_pci_rx_get_frame(struct sparx5 *sparx5,
						     struct sparx5_rx *rx)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	struct fdma *fdma = rx->fdma;
	struct sparx5_port *port;
	struct frame_info fi;
	struct sk_buff *skb;
	struct fdma_db *db;

	db = fdma_db_next_get(fdma);
	skb = __netdev_alloc_skb(rx->ndev, fdma->db_size, GFP_ATOMIC);
	if (unlikely(!skb))
		return NULL;

	skb_put(skb, fdma_db_len_get(db));
	memcpy(skb->data,
	       fdma_dataptr_virt_get_contiguous(fdma, fdma->dcb_index, fdma->db_index),
	       fdma_db_len_get(db));

	sparx5_ifh_parse(sparx5, (u32 *)skb->data, &fi);

	port = fi.src_port < consts->chip_ports ?
		sparx5->ports[fi.src_port] : NULL;

	if (WARN_ON(fi.src_port >= consts->chip_ports)) {
		dev_kfree_skb_any(skb);
		return NULL;
	}

	skb->dev = port->ndev;

	skb_pull(skb, IFH_LEN * sizeof(u32));

	if (likely(!(skb->dev->features & NETIF_F_RXFCS)))
		skb_trim(skb, skb->len - ETH_FCS_LEN);

	sparx5_ptp_rxtstamp(sparx5, skb, fi.src_port, fi.timestamp);
	skb->protocol = eth_type_trans(skb, skb->dev);

	if (test_bit(port->portno, sparx5->bridge_mask)) {
		skb->offload_fwd_mark = 1;

		if (!sparx5_skb_offloaded(sparx5, fi.src_port, skb))
			skb->offload_fwd_mark = 0;
	}

	skb->dev->stats.rx_bytes += skb->len;
	skb->dev->stats.rx_packets++;

	return skb;
}

int lan969x_fdma_pci_napi_poll(struct napi_struct *napi, int weight)
{
	struct sparx5_rx *rx = container_of(napi, struct sparx5_rx, napi);
	struct sparx5 *sparx5 = container_of(rx, struct sparx5, rx);
	int old_dcb, dcb_reload, counter = 0;
	struct fdma *fdma = rx->fdma;
	struct sk_buff *skb;

	dcb_reload = fdma->dcb_index;

	/* Get all received skb */
	while (counter < weight) {
		if (!fdma_has_frames(fdma))
			break;

		skb = lan969x_fdma_pci_rx_get_frame(sparx5, rx);
		if (!skb)
			break;

		napi_gro_receive(&rx->napi, skb);

		fdma_db_advance(fdma);
		counter++;

		if (fdma_dcb_is_reusable(fdma))
			continue;

		fdma_db_reset(fdma);
		fdma_dcb_advance(fdma);
	}

	while (dcb_reload != fdma->dcb_index) {
		old_dcb = dcb_reload;
		dcb_reload++;
		dcb_reload &= fdma->n_dcbs - 1;

		fdma_dcb_add(fdma,
			     old_dcb,
			     FDMA_DCB_INFO_DATAL(fdma->db_size),
			     FDMA_DCB_STATUS_INTR);
	}

	sparx5_fdma_reload(sparx5, fdma);

	/* Re-enable interrupts */
	if (counter < weight && napi_complete_done(napi, counter))
		spx5_rmw(BIT(fdma->channel_id),
			 BIT(fdma->channel_id) & FDMA_INTR_DB_ENA_INTR_DB_ENA,
			 sparx5, FDMA_INTR_DB_ENA);

	return counter;
}

static int lan969x_fdma_pci_rx_alloc(struct sparx5 *sparx5)
{
	struct sparx5_rx *rx = &sparx5->rx;
	struct fdma *fdma = rx->fdma;
	int err;

	err = fdma_alloc_coherent_and_map(sparx5->dev, fdma, &sparx5->atu);
	if (err)
		return err;

	fdma_dcbs_init(fdma,
		       FDMA_DCB_INFO_DATAL(fdma->db_size),
		       FDMA_DCB_STATUS_INTR);

	return 0;
}

static int lan969x_fdma_pci_tx_alloc(struct sparx5 *sparx5)
{
	struct sparx5_tx *tx = &sparx5->tx;
	struct fdma *fdma = tx->fdma;
	int err;

	err = fdma_alloc_coherent_and_map(sparx5->dev, fdma, &sparx5->atu);
	if (err)
		return err;

	fdma_dcbs_init(fdma,
		       FDMA_DCB_INFO_DATAL(fdma->db_size),
		       FDMA_DCB_STATUS_DONE);

	return 0;
}

int lan969x_fdma_pci_stop(struct sparx5 *sparx5)
{
	fdma_free_coherent_and_unmap(sparx5->dev, sparx5->rx.fdma);
	fdma_free_coherent_and_unmap(sparx5->dev, sparx5->tx.fdma);

	return 0;
}

static struct fdma lan969x_fdma_pci_tx = {
	.channel_id = FDMA_INJ_CHANNEL,
	.n_dcbs = 64,
	.n_dbs = 1,
	.ops = {
		.dataptr_cb = &lan969x_fdma_pci_dataptr_cb,
		.nextptr_cb = &lan969x_fdma_pci_nextptr_cb,
	},
};

static struct fdma lan969x_fdma_pci_rx = {
	.channel_id = FDMA_XTR_CHANNEL,
	.n_dcbs = 64,
	.n_dbs = 3,
	.ops = {
		.dataptr_cb = &lan969x_fdma_pci_dataptr_cb,
		.nextptr_cb = &lan969x_fdma_pci_nextptr_cb,
	},
};

int lan969x_fdma_pci_start(struct sparx5 *sparx5)
{
	struct fdma_pci_atu *atu = &sparx5->atu;
	int err;

	/* Must be initialized before FDMA buffers are allocated */
	fdma_pci_atu_init(atu, sparx5->regs[TARGET_PCIE_DBI]);

	sparx5->tx.max_mtu = sparx5_fdma_get_mtu(sparx5) + XDP_PACKET_HEADROOM;
	sparx5->rx.ndev = sparx5_fdma_get_ndev(sparx5);

	sparx5->rx.fdma = &lan969x_fdma_pci_rx;
	sparx5->rx.fdma->priv = sparx5;
	sparx5->rx.fdma->db_size = FDMA_PCI_DB_SIZE(sparx5->tx.max_mtu);
	sparx5->rx.fdma->size = fdma_get_size_contiguous(sparx5->rx.fdma);

	sparx5->tx.fdma = &lan969x_fdma_pci_tx;
	sparx5->tx.fdma->priv = sparx5;
	sparx5->tx.fdma->db_size = FDMA_PCI_DB_SIZE(sparx5->tx.max_mtu);
	sparx5->tx.fdma->size = fdma_get_size_contiguous(sparx5->tx.fdma);

	/* Reset FDMA state */
	spx5_wr(FDMA_CTRL_NRESET_SET(0), sparx5, FDMA_CTRL);
	spx5_wr(FDMA_CTRL_NRESET_SET(1), sparx5, FDMA_CTRL);

	err = dma_set_mask_and_coherent(sparx5->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(sparx5->dev, "Failed to set 64-bit FDMA mask");
		return err;
	}

	sparx5_fdma_injection_mode(sparx5);

	err = lan969x_fdma_pci_rx_alloc(sparx5);
	if (err) {
		dev_err(sparx5->dev, "Could not allocate RX buffers: %d\n", err);
		return err;
	}

	err = lan969x_fdma_pci_tx_alloc(sparx5);
	if (err) {
		fdma_free_coherent_and_unmap(sparx5->dev, sparx5->rx.fdma);
		dev_err(sparx5->dev, "Could not allocate TX buffers: %d\n", err);
		return err;
	}

	return 0;
}

int lan969x_fdma_pci_xmit(struct sparx5 *sparx5, u32 *ifh, struct sk_buff *skb)
{
	int needed_headroom, needed_tailroom, err = NETDEV_TX_OK;
	struct sparx5_tx *tx = &sparx5->tx;
	static bool first_time = true;
	struct fdma *fdma = tx->fdma;
	struct fdma_db *db;
	bool ptp = false;
	void *virt_addr;

	fdma_dcb_advance(fdma);

	db = fdma_db_next_get(fdma);

	if (unlikely(!fdma_db_is_done(db))) {
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

	virt_addr = fdma_dataptr_virt_get_contiguous(fdma, fdma->dcb_index, 0);
	memcpy(virt_addr, ifh, IFH_LEN * 4);
	memcpy((u8 *)virt_addr + IFH_LEN * 4, skb->data, skb->len);

	fdma_dcb_add(fdma, fdma->dcb_index, 0,
		     FDMA_DCB_STATUS_SOF |
		     FDMA_DCB_STATUS_EOF |
		     FDMA_DCB_STATUS_BLOCKO(0) |
		     FDMA_DCB_STATUS_BLOCKL(skb->len + IFH_LEN * 4 + ETH_FCS_LEN));

	if (first_time) {
		sparx5_fdma_tx_activate(sparx5, tx);
		first_time = false;
	} else {
		sparx5_fdma_reload(sparx5, fdma);
	}

	if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP &&
	    SPARX5_SKB_CB(skb)->rew_op == IFH_REW_OP_TWO_STEP_PTP)
		ptp = true;

	if (!ptp)
		dev_consume_skb_any(skb);

	return NETDEV_TX_OK;
}

int lan969x_fdma_pci_resize(struct sparx5 *sparx5)
{
	return -EOPNOTSUPP;
}
