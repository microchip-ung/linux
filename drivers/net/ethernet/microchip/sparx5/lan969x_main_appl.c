// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#define __REG(...)    __VA_ARGS__

/*      DEVCPU_QS:XTR:XTR_GRP_CFG */
#define QS_XTR_GRP_CFG(r)         __REG(TARGET_QS, 0, 1, 0, 0, 1, 36, 0, r, 2, 4)

#define QS_XTR_GRP_CFG_MODE                      GENMASK(3, 2)
#define QS_XTR_GRP_CFG_MODE_SET(x)\
	FIELD_PREP(QS_XTR_GRP_CFG_MODE, x)
#define QS_XTR_GRP_CFG_MODE_GET(x)\
	FIELD_GET(QS_XTR_GRP_CFG_MODE, x)

#define QS_XTR_GRP_CFG_STATUS_WORD_POS           BIT(1)
#define QS_XTR_GRP_CFG_STATUS_WORD_POS_SET(x)\
	FIELD_PREP(QS_XTR_GRP_CFG_STATUS_WORD_POS, x)
#define QS_XTR_GRP_CFG_STATUS_WORD_POS_GET(x)\
	FIELD_GET(QS_XTR_GRP_CFG_STATUS_WORD_POS, x)

#define QS_XTR_GRP_CFG_BYTE_SWAP                 BIT(0)
#define QS_XTR_GRP_CFG_BYTE_SWAP_SET(x)\
	FIELD_PREP(QS_XTR_GRP_CFG_BYTE_SWAP, x)
#define QS_XTR_GRP_CFG_BYTE_SWAP_GET(x)\
	FIELD_GET(QS_XTR_GRP_CFG_BYTE_SWAP, x)

/*      DEVCPU_QS:XTR:XTR_RD */
#define QS_XTR_RD(r)              __REG(TARGET_QS, 0, 1, 0, 0, 1, 36, 8, r, 2, 4)

/*      DEVCPU_QS:XTR:XTR_FLUSH */
#define QS_XTR_FLUSH              __REG(TARGET_QS, 0, 1, 0, 0, 1, 36, 24, 0, 1, 4)

#define QS_XTR_FLUSH_FLUSH                       GENMASK(1, 0)
#define QS_XTR_FLUSH_FLUSH_SET(x)\
	FIELD_PREP(QS_XTR_FLUSH_FLUSH, x)
#define QS_XTR_FLUSH_FLUSH_GET(x)\
	FIELD_GET(QS_XTR_FLUSH_FLUSH, x)

/*      DEVCPU_QS:XTR:XTR_DATA_PRESENT */
#define QS_XTR_DATA_PRESENT       __REG(TARGET_QS, 0, 1, 0, 0, 1, 36, 28, 0, 1, 4)

#define QS_XTR_DATA_PRESENT_DATA_PRESENT         GENMASK(1, 0)
#define QS_XTR_DATA_PRESENT_DATA_PRESENT_SET(x)\
	FIELD_PREP(QS_XTR_DATA_PRESENT_DATA_PRESENT, x)
#define QS_XTR_DATA_PRESENT_DATA_PRESENT_GET(x)\
	FIELD_GET(QS_XTR_DATA_PRESENT_DATA_PRESENT, x)

/*      DEVCPU_QS:INJ:INJ_GRP_CFG */
#define QS_INJ_GRP_CFG(r)         __REG(TARGET_QS, 0, 1, 36, 0, 1, 40, 0, r, 2, 4)

#define QS_INJ_GRP_CFG_MODE                      GENMASK(3, 2)
#define QS_INJ_GRP_CFG_MODE_SET(x)\
	FIELD_PREP(QS_INJ_GRP_CFG_MODE, x)
#define QS_INJ_GRP_CFG_MODE_GET(x)\
	FIELD_GET(QS_INJ_GRP_CFG_MODE, x)

#define QS_INJ_GRP_CFG_BYTE_SWAP                 BIT(0)
#define QS_INJ_GRP_CFG_BYTE_SWAP_SET(x)\
	FIELD_PREP(QS_INJ_GRP_CFG_BYTE_SWAP, x)
#define QS_INJ_GRP_CFG_BYTE_SWAP_GET(x)\
	FIELD_GET(QS_INJ_GRP_CFG_BYTE_SWAP, x)

/*      DEVCPU_QS:INJ:INJ_WR */
#define QS_INJ_WR(r)              __REG(TARGET_QS, 0, 1, 36, 0, 1, 40, 8, r, 2, 4)

/*      DEVCPU_QS:INJ:INJ_CTRL */
#define QS_INJ_CTRL(r)            __REG(TARGET_QS, 0, 1, 36, 0, 1, 40, 16, r, 2, 4)

#define QS_INJ_CTRL_GAP_SIZE                     GENMASK(24, 21)
#define QS_INJ_CTRL_GAP_SIZE_SET(x)\
	FIELD_PREP(QS_INJ_CTRL_GAP_SIZE, x)
#define QS_INJ_CTRL_GAP_SIZE_GET(x)\
	FIELD_GET(QS_INJ_CTRL_GAP_SIZE, x)

#define QS_INJ_CTRL_ABORT                        BIT(20)
#define QS_INJ_CTRL_ABORT_SET(x)\
	FIELD_PREP(QS_INJ_CTRL_ABORT, x)
#define QS_INJ_CTRL_ABORT_GET(x)\
	FIELD_GET(QS_INJ_CTRL_ABORT, x)

#define QS_INJ_CTRL_EOF                          BIT(19)
#define QS_INJ_CTRL_EOF_SET(x)\
	FIELD_PREP(QS_INJ_CTRL_EOF, x)
#define QS_INJ_CTRL_EOF_GET(x)\
	FIELD_GET(QS_INJ_CTRL_EOF, x)

#define QS_INJ_CTRL_SOF                          BIT(18)
#define QS_INJ_CTRL_SOF_SET(x)\
	FIELD_PREP(QS_INJ_CTRL_SOF, x)
#define QS_INJ_CTRL_SOF_GET(x)\
	FIELD_GET(QS_INJ_CTRL_SOF, x)

#define QS_INJ_CTRL_VLD_BYTES                    GENMASK(17, 16)
#define QS_INJ_CTRL_VLD_BYTES_SET(x)\
	FIELD_PREP(QS_INJ_CTRL_VLD_BYTES, x)
#define QS_INJ_CTRL_VLD_BYTES_GET(x)\
	FIELD_GET(QS_INJ_CTRL_VLD_BYTES, x)

/*      DEVCPU_QS:INJ:INJ_STATUS */
#define QS_INJ_STATUS             __REG(TARGET_QS, 0, 1, 36, 0, 1, 40, 24, 0, 1, 4)

#define QS_INJ_STATUS_WMARK_REACHED              GENMASK(5, 4)
#define QS_INJ_STATUS_WMARK_REACHED_SET(x)\
	FIELD_PREP(QS_INJ_STATUS_WMARK_REACHED, x)
#define QS_INJ_STATUS_WMARK_REACHED_GET(x)\
	FIELD_GET(QS_INJ_STATUS_WMARK_REACHED, x)

#define QS_INJ_STATUS_FIFO_RDY                   GENMASK(3, 2)
#define QS_INJ_STATUS_FIFO_RDY_SET(x)\
	FIELD_PREP(QS_INJ_STATUS_FIFO_RDY, x)
#define QS_INJ_STATUS_FIFO_RDY_GET(x)\
	FIELD_GET(QS_INJ_STATUS_FIFO_RDY, x)

#define QS_INJ_STATUS_INJ_IN_PROGRESS            GENMASK(1, 0)
#define QS_INJ_STATUS_INJ_IN_PROGRESS_SET(x)\
	FIELD_PREP(QS_INJ_STATUS_INJ_IN_PROGRESS, x)
#define QS_INJ_STATUS_INJ_IN_PROGRESS_GET(x)\
	FIELD_GET(QS_INJ_STATUS_INJ_IN_PROGRESS, x)

/* IFH ENCAP LEN is form of DMAC, SMAC, ETH_TYPE and ID */
#define IFH_ENCAP_LEN	16
static const u8 ifh_dmac[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const u8 ifh_smac[] = { 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff };
#define IFH_ETH_TYPE	0x8880
#define IFH_ID		0x000e
#define IF_BUFSIZE_JUMBO	10400
#define IFH_LEN		36
#define IFH_LEN_WORDS	9

#define XTR_EOF_0			0x00000080U
#define XTR_EOF_1			0x01000080U
#define XTR_EOF_2			0x02000080U
#define XTR_EOF_3			0x03000080U
#define XTR_PRUNED			0x04000080U
#define XTR_ABORT			0x05000080U
#define XTR_ESCAPE			0x06000080U
#define XTR_NOT_READY			0x07000080U
#define XTR_VALID_BYTES(x)		(((x) >> 24) & 3)

#define IFH_POS_SRCPORT			124
#define IFH_WID_SRCPORT			12
#define IFH_POS_TCI			108
#define IFH_WID_TCI			16

#define LAN969X_BUFFER_MIN_SZ		60
#define LAN969X_BUFFER_CELL_SZ		64

enum lan969x_target {
	TARGET_QS = 0,
	NUM_TARGETS = 1,
};

struct lan969x;
struct lan969x_port;

struct lan969x {
	struct device *dev;
	struct lan969x_port **ports;

	void __iomem *regs[NUM_TARGETS];

	int xtr_irq;
};

struct lan969x_port {
	struct net_device *dev;
	struct lan969x *lan969x;
};

static const struct of_device_id mchp_lan969x_match[] = {
	{ .compatible = "mchp,lan969x-switch-appl" },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_lan969x_match);

static inline void __iomem *lan_addr(void __iomem *base[],
				     int id, int tinst, int tcnt,
				     int gbase, int ginst,
				     int gcnt, int gwidth,
				     int raddr, int rinst,
				     int rcnt, int rwidth)
{
	WARN_ON((tinst) >= tcnt);
	WARN_ON((ginst) >= gcnt);
	WARN_ON((rinst) >= rcnt);
	return base[id + (tinst)] +
		gbase + ((ginst) * gwidth) +
		raddr + ((rinst) * rwidth);
}

static inline u32 lan_rd(struct lan969x *lan969x, int id, int tinst, int tcnt,
			 int gbase, int ginst, int gcnt, int gwidth,
			 int raddr, int rinst, int rcnt, int rwidth)
{
	return readl(lan_addr(lan969x->regs, id, tinst, tcnt, gbase, ginst,
			      gcnt, gwidth, raddr, rinst, rcnt, rwidth));
}

static inline void lan_wr(u32 val, struct lan969x *lan969x,
			  int id, int tinst, int tcnt,
			  int gbase, int ginst, int gcnt, int gwidth,
			  int raddr, int rinst, int rcnt, int rwidth)
{
	writel(val, lan_addr(lan969x->regs, id, tinst, tcnt,
			     gbase, ginst, gcnt, gwidth,
			     raddr, rinst, rcnt, rwidth));
}

static int lan969x_rx_frame_word(struct lan969x *lan969x, u8 grp,
				 u32 *rval, bool *eof)
{
	u32 bytes_valid;
	u32 val;

	val = lan_rd(lan969x, QS_XTR_RD(grp));
	if (val == XTR_NOT_READY) {
		do {
			val = lan_rd(lan969x, QS_XTR_RD(grp));
		} while (val == XTR_NOT_READY);
	}

	switch (val) {
	case XTR_ABORT:
		*eof = true;
		return -EIO;
	case XTR_EOF_0:
	case XTR_EOF_1:
	case XTR_EOF_2:
	case XTR_EOF_3:
	case XTR_PRUNED:
		bytes_valid = XTR_VALID_BYTES(val);
		val = lan_rd(lan969x, QS_XTR_RD(grp));
		if (val == XTR_ESCAPE)
			*rval = lan_rd(lan969x, QS_XTR_RD(grp));
		else
			*rval = val;

		*eof = true;

		return bytes_valid;
	case XTR_ESCAPE:
		*rval = lan_rd(lan969x, QS_XTR_RD(grp));

		return 4;
	default:
		*rval = val;

		return 4;
	}
}

static irqreturn_t lan969x_xtr_irq_handler(int irq, void *args)
{
	struct lan969x *lan969x = args;
	int i = 0, grp = 0, err = 0;

	if (!(lan_rd(lan969x, QS_XTR_DATA_PRESENT) & BIT(grp)))
		return IRQ_NONE;

	do {
		u32 ifh[IFH_LEN_WORDS] = { 0 };
		struct net_device *dev;
		struct sk_buff *skb;
		bool eof = false;
		int sz, len;
		u32 *buf;
		u32 val;

		for (i = 0; i < IFH_LEN_WORDS; i++) {
			err = lan969x_rx_frame_word(lan969x, grp, &ifh[i], &eof);
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

		dev = lan969x->ports[0]->dev;
		skb = netdev_alloc_skb(dev,
				       dev->mtu + IFH_LEN  +
				       IFH_ENCAP_LEN + ETH_FCS_LEN + ETH_HLEN);
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
		 * the function lan969x_parse_ifh, is changing the endianness to
		 * be able to calculate the length of the frame
		 */
		buf = (u32 *)skb_put(skb, IFH_LEN);
		for (i = 0; i < IFH_LEN_WORDS; ++i)
			*buf++ = htonl(ifh[i]);

		buf = (u32*)skb_tail_pointer(skb);

		len = 0;
		eof = false;
		do {
			sz = lan969x_rx_frame_word(lan969x, grp, &val, &eof);
			if (sz < 0) {
				kfree_skb(skb);
				goto recover;
			}

			if (sz > 0) {
				*buf++ = val;
				len += sz;
			}
		} while (!eof);

		if (sz < 0) {
			kfree_skb(skb);
			goto recover;
		}

		skb_put(skb, len);
		skb->protocol = eth_type_trans(skb, skb->dev);

		netif_rx(skb);

recover:
		if (sz < 0 || err)
			lan_rd(lan969x, QS_XTR_RD(grp));

	} while (lan_rd(lan969x, QS_XTR_DATA_PRESENT) & BIT(grp));

	return IRQ_HANDLED;
}

static int lan969x_port_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct lan969x_port *port = netdev_priv(dev);
	struct lan969x *lan969x = port->lan969x;
	u32 val;
	u8 grp = 0;
	u32 i, count, last;

	val = lan_rd(lan969x, QS_INJ_STATUS);
	if (!(val & QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))) ||
	    (val & QS_INJ_STATUS_WMARK_REACHED_SET(BIT(grp))))
		return NETDEV_TX_BUSY;

	skb_pull(skb, IFH_ENCAP_LEN);

	/* Write start of frame */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_SOF_SET(1),
	       lan969x, QS_INJ_CTRL(grp));

	/* Write frame */
	count = (skb->len + 3) / 4;
	last = skb->len % 4;
	for (i = 0; i < count; ++i) {
		/* Wait until the fifo is ready */
		while (!(lan_rd(lan969x, QS_INJ_STATUS) &
			 QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))))
				;

		lan_wr(((u32 *)skb->data)[i], lan969x, QS_INJ_WR(grp));
	}

	/* Add padding */
	while (i < ((LAN969X_BUFFER_MIN_SZ + IFH_LEN) / 4)) {
		/* Wait until the fifo is ready */
		while (!(lan_rd(lan969x, QS_INJ_STATUS) &
			 QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))))
				;

		lan_wr(0, lan969x, QS_INJ_WR(grp));
		++i;
	}

	/* Inidcate EOF and valid bytes in the last word */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_VLD_BYTES_SET(skb->len < LAN969X_BUFFER_CELL_SZ ?  0 : last) |
	       QS_INJ_CTRL_EOF_SET(1),
	       lan969x, QS_INJ_CTRL(grp));

	/* Add dummy CRC */
	lan_wr(0, lan969x, QS_INJ_WR(grp));

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static int lan969x_port_open(struct net_device *dev)
{
	return 0;
}

static int lan969x_port_stop(struct net_device *dev)
{
	return 0;
}

static int lan969x_change_mtu(struct net_device *dev, int new_mtu)
{
	dev->mtu = new_mtu;
	return 0;
}

static const struct net_device_ops lan969x_port_netdev_ops = {
	.ndo_open			= lan969x_port_open,
	.ndo_stop			= lan969x_port_stop,
	.ndo_start_xmit			= lan969x_port_xmit,
	.ndo_change_mtu			= lan969x_change_mtu,
};

static int lan969x_appl_ifh(struct platform_device *pdev,
			    struct lan969x *lan969x)
{
	struct lan969x_port *lan969x_port;
	struct net_device *dev;
	int err;

	lan969x->xtr_irq = platform_get_irq_byname(pdev, "xtr");
	err = devm_request_threaded_irq(&pdev->dev, lan969x->xtr_irq, NULL,
					lan969x_xtr_irq_handler, IRQF_ONESHOT,
					"frame extraction", lan969x);
	if (err) {
		pr_info("Unable to use xtr irq\n");
		return -ENODEV;
	}

	/* Create the network inteface */
	dev = alloc_etherdev_mqs(sizeof(struct lan969x_port), 8, 1);
	if (!dev)
		return -ENOMEM;

	lan969x->ports = devm_kcalloc(&pdev->dev, 1,
				      sizeof(struct lan969x_port *),
				      GFP_KERNEL);

	SET_NETDEV_DEV(dev, lan969x->dev);
	lan969x_port = netdev_priv(dev);
	lan969x_port->dev = dev;
	lan969x_port->lan969x = lan969x;

	lan969x->ports[0] = lan969x_port;

	dev->netdev_ops = &lan969x_port_netdev_ops;
	strcpy(dev->name, "vtss.ifh");

	dev->mtu = IF_BUFSIZE_JUMBO;

	err = register_netdev(dev);
	if (err) {
		dev_err(lan969x->dev, "register_netdev failed\n");
		return -1;
	}

	return 0;
}

static int mchp_lan969x_probe(struct platform_device *pdev)
{
	struct lan969x *lan969x;

	struct {
		enum lan969x_target id;
		char *name;
	} res[] = {
		{ TARGET_QS, "qs" },
	};

	lan969x = devm_kzalloc(&pdev->dev, sizeof(*lan969x), GFP_KERNEL);
	if (!lan969x)
		return -ENOMEM;

	platform_set_drvdata(pdev, lan969x);
	lan969x->dev = &pdev->dev;

	for (int i = 0; i < ARRAY_SIZE(res); i++) {
		struct resource *resource;

		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							res[i].name);
		if (!resource)
			return -ENODEV;

		lan969x->regs[res[i].id] = ioremap(resource->start,
						   resource_size(resource));
		if (IS_ERR(lan969x->regs[res[i].id])) {
			dev_info(&pdev->dev,
				"Unable to map Switch registers: %x\n", i);
		}
	}

	lan969x_appl_ifh(pdev, lan969x);

	return 0;
}

static int mchp_lan969x_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mchp_lan969x_driver = {
	.probe = mchp_lan969x_probe,
	.remove = mchp_lan969x_remove,
	.driver = {
		.name = "lan969x-switch-appl",
		.of_match_table = mchp_lan969x_match,
	},
};
module_platform_driver(mchp_lan969x_driver);
