// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <net/rtnetlink.h>
#include <net/netevent.h>
#include <net/switchdev.h>
#include <asm/memory.h>
#include <linux/dma-direct.h>

#include "lan966x_main.h"
#include "lan966x_ifh.h"

/* IFH ENCAP LEN is form of DMAC, SMAC, ETH_TYPE and ID */
#define IFH_ENCAP_LEN	16
static const u8 ifh_dmac[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const u8 ifh_smac[] = { 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff };
#define IFH_ETH_TYPE	0x8880
#define IFH_ID		0x000d
#define IF_BUFSIZE_JUMBO	10400

#define XTR_EOF_0			0x00000080U
#define XTR_EOF_1			0x01000080U
#define XTR_EOF_2			0x02000080U
#define XTR_EOF_3			0x03000080U
#define XTR_PRUNED			0x04000080U
#define XTR_ABORT			0x05000080U
#define XTR_ESCAPE			0x06000080U
#define XTR_NOT_READY			0x07000080U
#define XTR_VALID_BYTES(x)		(4 - (((x) >> 24) & 3))

struct frame_info {
	u32 len;
	u16 port; /* Bit mask */
	u16 vid;
	u32 timestamp;
	u32 ptp_seq_idx;
	u32 rew_op;
	u8 qos_class;
	u8 ipv;
	bool afi;
	bool rew_oam;
	u8 oam_type;
};

static const struct of_device_id mchp_lan966x_match[] = {
	{ .compatible = "mchp,lan966x-switch-appl" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_lan966x_match);

static int lan966x_ifh_extract(u32 *ifh, size_t pos, size_t length)
{
	int i;
	int val = 0;

	for (i = pos; i < pos + length; ++i)
		val |= ((ifh[IFH_LEN - i / 32 - 1] & BIT(i % 32)) >>
			(i % 32)) << (i - pos);

	return val;
}

static inline int lan966x_parse_ifh(u32 *ifh, struct frame_info *info)
{
	int i;

	/* The IFH is in network order, switch to CPU order */
	for (i = 0; i < IFH_LEN; i++)
		ifh[i] = ntohl((__force __be32)ifh[i]);

	info->len = lan966x_ifh_extract(ifh, IFH_POS_LEN, IFH_WID_LEN);
	info->port = lan966x_ifh_extract(ifh, IFH_POS_SRCPORT, IFH_WID_SRCPORT);
	info->vid = lan966x_ifh_extract(ifh, IFH_POS_TCI, IFH_WID_TCI) & 0xfff;
	return 0;
}

static int lan966x_rx_frame_word(struct lan966x *lan966x, u8 grp, u32 *rval)
{
	u32 bytes_valid;
	u32 val;

	val = lan_rd(lan966x, QS_XTR_RD(grp));
	if (val == XTR_NOT_READY) {
		do {
			val = lan_rd(lan966x, QS_XTR_RD(grp));
		} while (val == XTR_NOT_READY);
	}

	switch (val) {
	case XTR_ABORT:
		return -EIO;
	case XTR_EOF_0:
	case XTR_EOF_1:
	case XTR_EOF_2:
	case XTR_EOF_3:
	case XTR_PRUNED:
		bytes_valid = XTR_VALID_BYTES(val);
		val = lan_rd(lan966x, QS_XTR_RD(grp));
		if (val == XTR_ESCAPE)
			*rval = lan_rd(lan966x, QS_XTR_RD(grp));
		else
			*rval = val;

		return bytes_valid;
	case XTR_ESCAPE:
		*rval = lan_rd(lan966x, QS_XTR_RD(grp));

		return 4;
	default:
		*rval = val;

		return 4;
	}
}

static irqreturn_t lan966x_xtr_irq_handler(int irq, void *args)
{
	struct lan966x *lan966x = args;
	int i = 0, grp = 0, err = 0;

	if (!(lan_rd(lan966x, QS_XTR_DATA_PRESENT) & BIT(grp)))
		return IRQ_NONE;

	do {
		u32 ifh[IFH_LEN] = { 0 };
		struct net_device *dev;
		struct frame_info info;
		int sz, len, buf_len;
		struct sk_buff *skb;
		u32 *buf;
		u32 val;

		for (i = 0; i < IFH_LEN; i++) {
			err = lan966x_rx_frame_word(lan966x, grp, &ifh[i]);
			if (err != 4)
				goto recover;
		}

		/* The error needs to be reseted.
		 * In case there is only 1 frame in the queue, then after the
		 * extraction of ifh and of the frame then the while condition
		 * will failed. Then it would check if it is an err but the err
		 * is 4, as set previously. In this case will try to read the
		 * rest of the frames from the queue. And in case only a part of
		 * the frame is in the queue, it would read only that. So next
		 * time when this function is called it would presume would read
		 * initially the ifh but actually will read the rest of the
		 * previous frame. Therfore reset here the error code, meaning
		 * that there is no error with reading the ifh. Then if there is
		 * an error reading the frame the error will be set and then the
		 * check is partially correct.
		 */
		err = 0;

		lan966x_parse_ifh(ifh, &info);

		dev = lan966x->ports[0]->dev;
		skb = netdev_alloc_skb(dev, info.len + sizeof(ifh[0]) * IFH_LEN  + IFH_ENCAP_LEN + ETH_FCS_LEN);
		if (unlikely(!skb)) {
			netdev_err(dev, "Unable to allocate sk_buff\n");
			err = -ENOMEM;
			break;
		}

		ether_addr_copy((u8 *)skb_put(skb, ETH_ALEN), ifh_dmac);
		ether_addr_copy((u8 *)skb_put(skb, ETH_ALEN), ifh_smac);
		*(u16 *)skb_put(skb, sizeof(u16)) = htons(IFH_ETH_TYPE);
		*(u16 *)skb_put(skb, sizeof(u16)) = htons(IFH_ID);

		/* Add the IFH to skb and it is required to be in big endiane,
		 * the function lan966x_parse_ifh, is changing the endianness to
		 * be able to calculate the length of the frame
		 */
		buf = (u32 *)skb_put(skb, sizeof(ifh[0]) * IFH_LEN);
		for (i = 0; i < IFH_LEN; ++i)
			*buf++ = htonl(ifh[i]);

		buf_len = info.len;
		buf = (u32 *)skb_put(skb, buf_len);

		len = 0;
		do {
			sz = lan966x_rx_frame_word(lan966x, grp, &val);
			if (sz < 0) {
				kfree_skb(skb);
				goto recover;
			}

			*buf++ = val;
			len += sz;
		} while (len < buf_len);

		if (sz < 0) {
			kfree_skb(skb);
			goto recover;
		}

		skb->protocol = eth_type_trans(skb, skb->dev);

		netif_rx(skb);

recover:
		if (sz < 0 || err)
			lan_rd(lan966x, QS_XTR_RD(grp));

	} while (lan_rd(lan966x, QS_XTR_DATA_PRESENT) & BIT(grp));

	return IRQ_HANDLED;
}

static int lan966x_port_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	u32 val;
	u8 grp = 0;
	u32 i, count, last;

	val = lan_rd(lan966x, QS_INJ_STATUS);
	if (!(val & QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))) ||
	    (val & QS_INJ_STATUS_WMARK_REACHED_SET(BIT(grp))))
		return NETDEV_TX_BUSY;

	skb_pull(skb, IFH_ENCAP_LEN);

	/* Write start of frame */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_SOF_SET(1),
	       lan966x, QS_INJ_CTRL(grp));

	/* Write frame */
	count = (skb->len + 3) / 4;
	last = skb->len % 4;
	for (i = 0; i < count; ++i) {
		/* Wait until the fifo is ready */
		while (!(lan_rd(lan966x, QS_INJ_STATUS) &
			 QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))))
				;

		lan_wr(((u32 *)skb->data)[i], lan966x, QS_INJ_WR(grp));
	}

	/* Add padding */
	while (i < ((LAN966X_BUFFER_MIN_SZ + sizeof(u32) * IFH_LEN) / 4)) {
		/* Wait until the fifo is ready */
		while (!(lan_rd(lan966x, QS_INJ_STATUS) &
			 QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))))
				;

		lan_wr(0, lan966x, QS_INJ_WR(grp));
		++i;
	}

	/* Inidcate EOF and valid bytes in the last word */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_VLD_BYTES_SET(skb->len < LAN966X_BUFFER_CELL_SZ ?  0 : last) |
	       QS_INJ_CTRL_EOF_SET(1),
	       lan966x, QS_INJ_CTRL(grp));

	/* Add dummy CRC */
	lan_wr(0, lan966x, QS_INJ_WR(grp));

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static int lan966x_port_open(struct net_device *dev)
{
	return 0;
}

static int lan966x_port_stop(struct net_device *dev)
{
	return 0;
}

static int lan966x_change_mtu(struct net_device *dev, int new_mtu)
{
	dev->mtu = new_mtu;
	return 0;
}

static const struct net_device_ops lan966x_port_netdev_ops = {
	.ndo_open			= lan966x_port_open,
	.ndo_stop			= lan966x_port_stop,
	.ndo_start_xmit			= lan966x_port_xmit,
	.ndo_change_mtu			= lan966x_change_mtu,
};

static int lan966x_appl_ifh(struct platform_device *pdev,
			     struct lan966x *lan966x)
{
	struct lan966x_port *lan966x_port;
	struct net_device *dev;
	int err;

	lan966x->xtr_irq = platform_get_irq_byname(pdev, "xtr");
	err = devm_request_threaded_irq(&pdev->dev, lan966x->xtr_irq, NULL,
					lan966x_xtr_irq_handler, IRQF_ONESHOT,
					"frame extraction", lan966x);
	if (err) {
		pr_info("Unable to use xtr irq\n");
		return -ENODEV;
	}

	/* Create the network inteface */
	dev = alloc_etherdev_mqs(sizeof(struct lan966x_port), 8, 1); /* TODO: Use devicetree? */
	if (!dev)
		return -ENOMEM;

	lan966x->ports = devm_kcalloc(&pdev->dev, 1,
				      sizeof(struct lan966x_port *),
				      GFP_KERNEL);

	SET_NETDEV_DEV(dev, lan966x->dev);
	lan966x_port = netdev_priv(dev);
	lan966x_port->dev = dev;
	lan966x_port->lan966x = lan966x;

	lan966x->ports[0] = lan966x_port;

	dev->netdev_ops = &lan966x_port_netdev_ops;
	strcpy(dev->name, "vtss.ifh");

	eth_hw_addr_gen(dev, lan966x->base_mac, 1);
	dev->mtu = IF_BUFSIZE_JUMBO;

	err = register_netdev(dev);
	if (err) {
		dev_err(lan966x->dev, "register_netdev failed\n");
		return -1;
	}

	return 0;
}

static int mchp_lan966x_probe(struct platform_device *pdev)
{
	struct lan966x *lan966x;
	u8 mac_addr[ETH_ALEN];

	struct {
		enum lan966x_target id;
		char *name;
	} res[] = {
		{ TARGET_QS, "qs" },
	};

	lan966x = devm_kzalloc(&pdev->dev, sizeof(*lan966x), GFP_KERNEL);
	if (!lan966x)
		return -ENOMEM;

	platform_set_drvdata(pdev, lan966x);
	lan966x->dev = &pdev->dev;

	for (int i = 0; i < ARRAY_SIZE(res); i++) {
		struct resource *resource;

		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							res[i].name);
		if (!resource)
			return -ENODEV;

		lan966x->regs[res[i].id] = ioremap(resource->start,
						   resource_size(resource));
		if (IS_ERR(lan966x->regs[res[i].id])) {
			dev_info(&pdev->dev,
				"Unable to map Switch registers: %x\n", i);
		}
	}

	if (device_get_mac_address(&pdev->dev, mac_addr)) {
		ether_addr_copy(lan966x->base_mac, mac_addr);
	} else {
		pr_info("MAC addr was not set, use random MAC\n");
		eth_random_addr(lan966x->base_mac);
		lan966x->base_mac[5] &= 0xf0;
	}

	lan966x_appl_ifh(pdev, lan966x);

	return 0;
}

static int mchp_lan966x_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mchp_lan966x_driver = {
	.probe = mchp_lan966x_probe,
	.remove = mchp_lan966x_remove,
	.driver = {
		.name = "lan966x-switch-appl",
		.of_match_table = mchp_lan966x_match,
	},
};
module_platform_driver(mchp_lan966x_driver);

MODULE_DESCRIPTION("Microchip LAN966X switch driver");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
MODULE_LICENSE("Dual MIT/GPL");
