// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2024 Microchip Technology Inc. and its subsidiaries.
 */

#include "sparx5_main.h"

#define SPX5_HW_MTU(mtu) ((mtu) + ETH_ALEN + IFH_LEN * 4 + ETH_FCS_LEN)

/* Get the MTU for this port */
static u32 sparx5_mtu_get(struct sparx5_port *port)
{
	struct sparx5 *sparx5 = port->sparx5;
	const struct sparx5_ops *ops = sparx5->data->ops;
	u32 val;

	/* We always set 2G5 or RGMII MTU, so there is no point in trying to
	 * read high-speed MAC MTU here
	 */
	if (ops->is_port_rgmii(port->portno)) {
		u32 idx = ops->get_port_dev_index(sparx5, port->portno);
		val = spx5_rd(sparx5, DEVRGMII_MAC_MAXLEN_CFG(idx));
		return DEVRGMII_MAC_MAXLEN_CFG_MAX_LEN_GET(val);
	} else {
		val = spx5_rd(sparx5, DEV2G5_MAC_MAXLEN_CFG(port->portno));
		return DEV2G5_MAC_MAXLEN_CFG_MAX_LEN_GET(val);
	}
}

/* Set the MTU for this port. */
static void sparx5_mtu_set(struct sparx5_port *port, u32 new_mtu)
{
	struct sparx5 *sparx5 = port->sparx5;
	const struct sparx5_ops *ops;
	u32 idx, hw_mtu;

	ops = sparx5->data->ops;
	idx = ops->get_port_dev_index(sparx5, port->portno);
	hw_mtu = SPX5_HW_MTU(new_mtu);

	pr_debug("Setting MTU to: %u for portno (idx: %u): %u\n", new_mtu,
		 port->portno, idx);

	/* All port modules have a 2g5 shadow device (and DEV2G5 is indexed
	 * by port number). Except the RGMII ports
	 */
	if (!ops->is_port_rgmii(port->portno))
		spx5_rmw(DEV2G5_MAC_MAXLEN_CFG_MAX_LEN_SET(hw_mtu),
			 DEV2G5_MAC_MAXLEN_CFG_MAX_LEN, sparx5,
			 DEV2G5_MAC_MAXLEN_CFG(port->portno));

	if (ops->is_port_rgmii(port->portno))
		spx5_rmw(DEVRGMII_MAC_MAXLEN_CFG_MAX_LEN_SET(hw_mtu),
			DEVRGMII_MAC_MAXLEN_CFG_MAX_LEN, sparx5,
			DEVRGMII_MAC_MAXLEN_CFG(idx));
	else if (ops->is_port_2g5(port->portno))
		return; /* Already configured. */
	else if (ops->is_port_5g(port->portno))
		spx5_rmw(DEV5G_MAC_MAXLEN_CFG_MAX_LEN_SET(hw_mtu),
			 DEV5G_MAC_MAXLEN_CFG_MAX_LEN, sparx5,
			 DEV5G_MAC_MAXLEN_CFG(idx));
	else if (ops->is_port_10g(port->portno))
		spx5_rmw(DEV10G_MAC_MAXLEN_CFG_MAX_LEN_SET(hw_mtu),
			 DEV10G_MAC_MAXLEN_CFG_MAX_LEN, sparx5,
			 DEV10G_MAC_MAXLEN_CFG(idx));
	else
		spx5_rmw(DEV25G_MAC_MAXLEN_CFG_MAX_LEN_SET(hw_mtu),
			 DEV25G_MAC_MAXLEN_CFG_MAX_LEN, sparx5,
			 DEV25G_MAC_MAXLEN_CFG(idx));
}

u32 sparx5_mtu_max(struct sparx5 *sparx5)
{
	u32 mtu_max = 0;

	for (int i = 0; i < sparx5->data->consts->n_ports; i++) {
		struct sparx5_port *port = sparx5->ports[i];
		u32 mtu;

		if (!port)
			continue;

		mtu = sparx5_mtu_get(port) + IFH_LEN_BYTES +
		      SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) +
		      VLAN_HLEN * 2 + XDP_PACKET_HEADROOM;

		if (mtu > mtu_max)
			mtu_max = mtu;
	}

	return mtu_max;
}

static int sparx5_mtu_fdma_set(struct sparx5 *sparx5)
{
	const struct sparx5_ops *ops = sparx5->data->ops;
	u32 mtu_max = sparx5_mtu_max(sparx5);

	/* We might not be using FDMA */
	if (sparx5->fdma_irq <= 0)
		return 0;

	/* No need to restart FDMA if the MTU reflects the buffer size */
	if (mtu_max == sparx5->tx.max_mtu)
		return 0;

	return ops->fdma_resize(sparx5);
}

int sparx5_mtu_change(struct net_device *dev, int new_mtu)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *sparx5 = port->sparx5;
	int err, orig_mtu = dev->mtu;

	/* Write MTU to hardware */
	sparx5_mtu_set(port, new_mtu);

	err = sparx5_mtu_fdma_set(sparx5);
	if (err) {
		sparx5_mtu_set(port, orig_mtu);
		return err;
	}

	/* Let the stack know */
	WRITE_ONCE(dev->mtu, new_mtu);

	return 0;
}
