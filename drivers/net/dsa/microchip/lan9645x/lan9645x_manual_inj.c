// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include "lan9645x_main.h"

#define LAN9645X_BUFFER_MIN_SZ		60

static int lan9645x_port_inj_ready(struct lan9645x *lan9645x, u8 cpum)
{
	u32 val = 0;

	return lan9645x_rd_poll_timeout(lan9645x, QS_INJ_STATUS, val,
					QS_INJ_STATUS_FIFO_RDY_GET(val) &
					BIT(cpum));
}

static netdev_tx_t lan9645x_port_ifh_xmit(struct sk_buff *skb,
					  __be32 *ifh,
					  struct lan9645x_port *port)
{
	struct lan9645x *lan9645x = port->lan9645x;
	struct net_device *dev;
	u32 i, count, last;
	u8 cpum = 1; /* use second cpu module at chip_port 10 */
	u32 val;

	dev = lan9645x_port_to_ndev(port);

	val = lan_rd(lan9645x, QS_INJ_STATUS);
	if (!(QS_INJ_STATUS_FIFO_RDY_GET(val) & BIT(cpum)) ||
	    (QS_INJ_STATUS_WMARK_REACHED_GET(val) & BIT(cpum)))
		goto err;

	/* Write start of frame */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_SOF_SET(1),
	       lan9645x, QS_INJ_CTRL(cpum));

	/* Write IFH header */
	for (i = 0; i < LAN9645X_IFH_LEN_U32; ++i) {
		/* Wait until the fifo is ready */
		if (lan9645x_port_inj_ready(lan9645x, cpum))
			goto err;

		lan_wr((__force u32)ifh[i], lan9645x, QS_INJ_WR(cpum));
	}

	/* Write frame */
	count = DIV_ROUND_UP(skb->len, 4);
	last = skb->len % 4;
	for (i = 0; i < count; ++i) {
		/* Wait until the fifo is ready */
		if (lan9645x_port_inj_ready(lan9645x, cpum))
			goto err;

		lan_wr(((u32 *)skb->data)[i], lan9645x, QS_INJ_WR(cpum));
	}

	/* Add padding */
	while (i < (LAN9645X_BUFFER_MIN_SZ / 4)) {
		/* Wait until the fifo is ready */
		if (lan9645x_port_inj_ready(lan9645x, cpum))
			goto err;

		lan_wr(0, lan9645x, QS_INJ_WR(cpum));
		++i;
	}

	/* Indicate EOF and valid bytes in the last word */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_VLD_BYTES_SET(skb->len < LAN9645X_BUFFER_MIN_SZ ?
				     0 : last) |
	       QS_INJ_CTRL_EOF_SET(1),
	       lan9645x, QS_INJ_CTRL(cpum));

	/* Add dummy CRC */
	lan_wr(0, lan9645x, QS_INJ_WR(cpum));

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;

err:
	return NETDEV_TX_BUSY;
}

netdev_tx_t lan9645x_inj_xmit(struct lan9645x_port *port,
			      struct sk_buff *skb,
			      __be32 ifh[LAN9645X_IFH_LEN_U32])
{
	struct lan9645x *lan9645x = port->lan9645x;
	int ret;

	mutex_lock(&lan9645x->tx_lock);
	ret = lan9645x_port_ifh_xmit(skb, ifh, port);
	mutex_unlock(&lan9645x->tx_lock);

	return ret;
}
