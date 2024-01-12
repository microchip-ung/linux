// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include "sparx5_main.h"
#include "sparx5_regs.h"
#include "lan969x/lan969x.h"

#define ETH_VLAN_TAGSZ		4    /* Size of a 802.1Q VLAN tag */
#define TX_MTU_MIN		64
#define TX_MTU_MAX		2000
#define MTU_DEFAULT		(ETH_FRAME_LEN + ETH_FCS_LEN + (2 * ETH_VLAN_TAGSZ))
#define MTU_MIN			(TX_MTU_MIN + ETH_FCS_LEN + (2 * ETH_VLAN_TAGSZ))
#define MTU_MAX			(TX_MTU_MAX + ETH_FCS_LEN + (2 * ETH_VLAN_TAGSZ))
#define IF_BUFSIZE_JUMBO	10400

#define XTR_EOF_0     ntohl((__force __be32)0x80000000u)
#define XTR_EOF_1     ntohl((__force __be32)0x80000001u)
#define XTR_EOF_2     ntohl((__force __be32)0x80000002u)
#define XTR_EOF_3     ntohl((__force __be32)0x80000003u)
#define XTR_PRUNED    ntohl((__force __be32)0x80000004u)
#define XTR_ABORT     ntohl((__force __be32)0x80000005u)
#define XTR_ESCAPE    ntohl((__force __be32)0x80000006u)
#define XTR_NOT_READY ntohl((__force __be32)0x80000007u)

#define XTR_VALID_BYTES(x)      (4 - ((ntohl(x)) & 3))

const u8 ifh_dmac[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
const u8 ifh_smac[] = { 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff };

static const struct sparx5_main_io_resource sparx5_appl_main_iomap[] =  {
	{ TARGET_CPU,                         0, 0 }, /* 0x600000000 */
	{ TARGET_FDMA,                  0x80000, 0 }, /* 0x600080000 */
	{ TARGET_DSM,                0x10504000, 1 }, /* 0x610504000 */
	{ TARGET_ASM,                0x10600000, 1 }, /* 0x610600000 */
	{ TARGET_GCB,                0x11010000, 2 }, /* 0x611010000 */
	{ TARGET_QS,                 0x11030000, 2 }, /* 0x611030000 */
	{ TARGET_QFWD,               0x110b0000, 2 }, /* 0x6110b0000 */
	{ TARGET_HSCH,               0x11580000, 2 }, /* 0x611580000 */
};

static const struct sparx5_main_io_resource lan969x_appl_main_iomap[] =  {
	{ TARGET_CPU,                   0xc0000, 0 }, /* 0xe00c0000 */
	{ TARGET_FDMA,                  0xc0400, 0 }, /* 0xe00c0400 */
	{ TARGET_GCB,                 0x2010000, 1 }, /* 0xe2010000 */
	{ TARGET_QS,                  0x2030000, 1 }, /* 0xe2030000 */
	{ TARGET_QFWD,                0x20b0000, 1 }, /* 0xe20b0000 */
	{ TARGET_HSCH,                0x2580000, 1 }, /* 0xe2580000 */
	{ TARGET_DSM,                 0x30ec000, 1 }, /* 0xe30ec000 */
	{ TARGET_ASM,                 0x3200000, 1 }, /* 0xe3200000 */
};

static int sparx5_appl_create_targets(struct sparx5 *sparx5)
{
	const struct sparx5_main_io_resource *iomap;
	int iomap_size = sparx5->data->iomap_size;
	int ioranges = sparx5->data->ioranges;
	struct resource *iores[3];
	void __iomem *iomem[3];
	void __iomem *begin[3];
	int range_id[3];
	int idx, jdx;

	iomap = sparx5->data->iomap;

	for (idx = 0, jdx = 0; jdx < iomap_size; jdx++) {
		const struct sparx5_main_io_resource *io = &iomap[jdx];

		if (idx == io->range) {
			range_id[idx] = jdx;
			idx++;
		}
	}
	for (idx = 0; idx < ioranges; idx++) {
		iores[idx] = platform_get_resource(sparx5->pdev, IORESOURCE_MEM,
						   idx);
		if (!iores[idx]) {
			dev_err(sparx5->dev, "Invalid resource\n");
			return -EINVAL;
		}
		iomem[idx] = devm_ioremap(sparx5->dev,
					  iores[idx]->start,
					  resource_size(iores[idx]));
		if (!iomem[idx]) {
			dev_err(sparx5->dev, "Unable to get switch registers: %s\n",
				iores[idx]->name);
			return -ENOMEM;
		}
		begin[idx] = iomem[idx] - iomap[range_id[idx]].offset;
	}

	for (jdx = 0; jdx < iomap_size; jdx++) {
		const struct sparx5_main_io_resource *io = &iomap[jdx];

		sparx5->regs[io->id] = begin[io->range] + io->offset;
	}
	return 0;
}

static const struct sparx5_match_data sparx5_appl_desc = {
	.iomap = sparx5_appl_main_iomap,
	.iomap_size = ARRAY_SIZE(sparx5_appl_main_iomap),
	.ioranges = 3,
	.regs = {
		.tsize = sparx5_tsize,
		.gaddr = sparx5_gaddr,
		.gcnt = sparx5_gcnt,
		.gsize = sparx5_gsize,
		.raddr = sparx5_raddr,
		.rcnt = sparx5_rcnt,
		.fpos = sparx5_fpos,
		.fsize = sparx5_fsize,
	},
	.ops = {
		.get_pipeline_pt = &sparx5_get_packet_pipeline_pt,
		.get_ifh_field_pos = &sparx5_get_ifh_field_pos,
		.get_ifh_field_width = &sparx5_get_ifh_field_width,
		.fdma_stop = &sparx5_fdma_stop,
		.fdma_start = &sparx5_fdma_start,
		.fdma_xmit = &sparx5_fdma_xmit,
	},
	.consts = {
		.chip_ports = 65,
		.fdma_db_cnt = 15,
		.ifh_id = 11,
	}
};

static const struct sparx5_match_data lan969x_appl_desc = {
	.iomap = lan969x_appl_main_iomap,
	.iomap_size = ARRAY_SIZE(lan969x_appl_main_iomap),
	.ioranges = 2,
	.regs = {
		.tsize = lan969x_tsize,
		.gaddr = lan969x_gaddr,
		.gcnt = lan969x_gcnt,
		.gsize = lan969x_gsize,
		.raddr = lan969x_raddr,
		.rcnt = lan969x_rcnt,
		.fpos = lan969x_fpos,
		.fsize = lan969x_fsize,
	},
	.ops = {
		.get_pipeline_pt = &lan969x_get_packet_pipeline_pt,
		.get_ifh_field_pos = &lan969x_get_ifh_field_pos,
		.get_ifh_field_width = &lan969x_get_ifh_field_width,
		.fdma_stop = lan969x_fdma_stop,
		.fdma_start = lan969x_fdma_start,
		.fdma_xmit = lan969x_fdma_xmit,
	},
	.consts = {
		.chip_ports = 30,
		.fdma_db_cnt = 3,
		.ifh_id = 14,
	}
};

static const struct of_device_id mchp_sparx5_appl_match[] = {
	{ .compatible = "microchip,sparx5-switch-appl",
	  .data = &sparx5_appl_desc },
	{ .compatible = "mchp,lan969x-switch-appl",
	  .data = &lan969x_appl_desc },
	{}
};
MODULE_DEVICE_TABLE(of, mchp_sparx5_appl_match);

static int sparx5_appl_rx_frame_word(struct sparx5 *sparx5, u8 grp, u32 *rval,
				     bool *eof)
{
	u32 bytes_valid;
	u32 val;

	val = spx5_rd(sparx5, QS_XTR_RD(grp));
	if (val == XTR_NOT_READY) {
		do {
			val = spx5_rd(sparx5, QS_XTR_RD(grp));
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
		val = spx5_rd(sparx5, QS_XTR_RD(grp));
		if (val == XTR_ESCAPE)
			*rval = spx5_rd(sparx5, QS_XTR_RD(grp));
		else
			*rval = val;

		*eof = true;

		return bytes_valid;
	case XTR_ESCAPE:
		*rval = spx5_rd(sparx5, QS_XTR_RD(grp));

		return 4;
	default:
		*rval = val;

		return 4;
	}
}

static irqreturn_t sparx5_appl_xtr_irq_handler(int irq, void *args)
{
	const struct sparx5_consts *consts;
	struct sparx5 *sparx5 = args;
	int i = 0, grp = 0, err = 0;

	consts = &sparx5->data->consts;

	if (!(spx5_rd(sparx5, QS_XTR_DATA_PRESENT) & BIT(grp)))
		return IRQ_NONE;

	do {
		bool eof_flag = false, pruned_flag = false, abort_flag = false;
		u32 ifh[IFH_LEN] = {0};
		struct net_device *dev;
		struct sk_buff *skb;
		u32 byte_cnt = 0;
		bool eof = false;
		u32 *buf;

		for (i = 0; i < IFH_LEN; i++) {
			err = sparx5_appl_rx_frame_word(sparx5, grp, &ifh[i],
							&eof);
			if (err != 4)
				goto recover;
		}

		err = 0;

		dev = sparx5->ports[0]->ndev;
		skb = netdev_alloc_skb(dev,
				       dev->mtu + IFH_LEN * 4  +
				       IFH_ENCAP_LEN + ETH_FCS_LEN + ETH_HLEN);
		if (unlikely(!skb)) {
			netdev_err(dev, "Unable to allocate sk_buff\n");
			err = -ENOMEM;
			break;
		}

		ether_addr_copy((u8 *)skb_put(skb, ETH_ALEN), ifh_dmac);
		ether_addr_copy((u8 *)skb_put(skb, ETH_ALEN), ifh_smac);
		*(u16 *)skb_put(skb, sizeof(u16)) = htons(IFH_ETH_TYPE);
		*(u16 *)skb_put(skb, sizeof(u16)) = htons(consts->ifh_id);

		buf = (u32 *)skb_put(skb, IFH_LEN * 4);
		for (i = 0; i < IFH_LEN; ++i)
			*buf++ = ifh[i];

		buf = (u32 *)skb_tail_pointer(skb);

		/* Now, pull frame data */
		while (!eof_flag) {
			u32 val = spx5_rd(sparx5, QS_XTR_RD(grp));
			u32 cmp = val;

			switch (cmp) {
			case XTR_NOT_READY:
				break;
			case XTR_ABORT:
				/* No accompanying data */
				abort_flag = true;
				eof_flag = true;
				break;
			case XTR_EOF_0:
			case XTR_EOF_1:
			case XTR_EOF_2:
			case XTR_EOF_3:
				/* This assumes STATUS_WORD_POS == 1, Status
				 * just after last data
				 */
				byte_cnt -= (4 - XTR_VALID_BYTES(val));
				eof_flag = true;
				break;
			case XTR_PRUNED:
				/* But get the last 4 bytes as well */
				eof_flag = true;
				pruned_flag = true;
				fallthrough;
			case XTR_ESCAPE:
				*buf = spx5_rd(sparx5, QS_XTR_RD(grp));
				byte_cnt += 4;
				buf++;
				break;
			default:
				*buf = val;
				byte_cnt += 4;
				buf++;
			}
		}

		if (abort_flag || pruned_flag || !eof_flag) {
			kfree_skb(skb);
			goto recover;
		}

		skb_put(skb, byte_cnt);
		skb->protocol = eth_type_trans(skb, skb->dev);

		netif_rx(skb);

recover:
		if (err)
			spx5_rd(sparx5, QS_XTR_RD(grp));

	} while (spx5_rd(sparx5, QS_XTR_DATA_PRESENT) & BIT(grp));

	return IRQ_HANDLED;
}

static int sparx5_appl_xtr(struct sparx5 *sparx5)
{
	int err;

	if (!sparx5->xtr_irq)
		return -EINVAL;

	err = devm_request_threaded_irq(sparx5->dev, sparx5->xtr_irq, NULL,
					sparx5_appl_xtr_irq_handler, IRQF_ONESHOT,
					"sparx5-appl-xtr", sparx5);
	if (err)
		return err;

	return sparx5_manual_injection_mode(sparx5);
}

static int sparx5_appl_fdma(struct sparx5 *sparx5)
{
	int err;

	if (!sparx5->fdma_irq)
		return -EINVAL;

	if (GCB_CHIP_ID_REV_ID_GET(sparx5->chip_id) <= 0) {
		sparx5->fdma_irq = 0;
		return -EINVAL;
	}

	err = devm_request_threaded_irq(sparx5->dev, sparx5->fdma_irq,
					sparx5_fdma_handler, NULL, IRQF_SHARED,
					"sparx5-appl-fdma", sparx5);
	if (err)
		return err;

	return sparx5->data->ops.fdma_start(sparx5);
}

static const struct net_device_ops netdev_ops = {
	.ndo_start_xmit = sparx5_port_xmit_impl,
};

static int mchp_sparx5_appl_probe(struct platform_device *pdev)
{
	const struct sparx5_match_data *data;
	struct sparx5_port *sparx5_port;
	const struct sparx5_ops *ops;
	struct net_device *dev;
	struct sparx5 *sparx5;
	int err;

	sparx5 = devm_kzalloc(&pdev->dev, sizeof(*sparx5), GFP_KERNEL);
	if (!sparx5)
		return -ENOMEM;

	platform_set_drvdata(pdev, sparx5);
	sparx5->pdev = pdev;
	sparx5->dev = &pdev->dev;

	data = device_get_match_data(sparx5->dev);
	if (!data)
		return -EINVAL;

	sparx5->data = data;
	regs = &data->regs;
	ops = &sparx5->data->ops;

	err = sparx5_appl_create_targets(sparx5);
	if (err)
		return err;

	sparx5->chip_id = spx5_rd(sparx5, GCB_CHIP_ID);

	dev = alloc_etherdev_mqs(sizeof(struct sparx5_port), 8, 1);
	if (!dev)
		return -ENOMEM;

	dev->netdev_ops = &netdev_ops;
	strcpy(dev->name, "vtss.ifh");
	dev->mtu = IF_BUFSIZE_JUMBO;

	sparx5->ports[0] = devm_kcalloc(&pdev->dev, 1,
					sizeof(struct sparx5_port *),
					GFP_KERNEL);

	SET_NETDEV_DEV(dev, sparx5->dev);
	sparx5_port = netdev_priv(dev);
	sparx5_port->ndev = dev;
	sparx5_port->sparx5 = sparx5;
	sparx5->ports[0] = sparx5_port;

	err = register_netdev(dev);
	if (err) {
		dev_err(sparx5->dev, "Failed to register netdevice\n");
		return -1;
	}

	sparx5->xtr_irq = platform_get_irq_byname(sparx5->pdev, "xtr");
	sparx5->fdma_irq = platform_get_irq_byname(sparx5->pdev, "fdma");

	err = sparx5_appl_fdma(sparx5);
	if (err) {
		dev_info(sparx5->dev, "Failed to start FDMA. Falling back to register-based INJ/XTR\n");
		err = sparx5_appl_xtr(sparx5);
		if (err) {
			dev_err(sparx5->dev, "Failed to start register-based INJ/XTR\n");
			return err;
		}
	}

	return 0;
}

static int mchp_sparx5_appl_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mchp_sparx5_appl_driver = {
	.probe = mchp_sparx5_appl_probe,
	.remove = mchp_sparx5_appl_remove,
	.driver = {
		.name = "sparx5-switch-appl",
		.of_match_table = mchp_sparx5_appl_match,
	},
};
module_platform_driver(mchp_sparx5_appl_driver);

MODULE_DESCRIPTION("Microchip sparx5/lan969x appl switch driver");
MODULE_AUTHOR("Daniel Machon <daniel.machon@microchip.com>");
MODULE_LICENSE("Dual MIT/GPL");
