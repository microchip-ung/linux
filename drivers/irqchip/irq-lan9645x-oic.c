// SPDX-License-Identifier: GPL-2.0
/*
 * Regmap based driver for the Microchip Lan9645x outbound interrupt controller
 *
 * Copyright (c) 2025 Technology Inc. and its subsidiaries.
 */

#include <linux/of.h>
#include <linux/err.h>
#include <linux/mfd/ocelot.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define LAN9645X_NUM_IRQ		39

/* Lan9645x interrupt sources */
enum lan9645x_hwirq_regmap {
	LIRQ_PORT_MODULE = 0, /* Has bypass set by default, do not use */
	LIRQ_EXT0 = 1,
	LIRQ_EXT1 = 2,
	LIRQ_EXT2 = 3,
	LIRQ_EXT3 = 4,
	LIRQ_TIMER0 = 5,
	LIRQ_TIMER1 = 6,
	LIRQ_TIMER2 = 7,
	LIRQ_UART = 8,
	LIRQ_I2C0 = 9,
	LIRQ_I2C1 = 10,
	LIRQ_OTP = 11,
	LIRQ_CUPHY0 = 12,
	LIRQ_CUPHY1 = 13,
	LIRQ_CUPHY2 = 14,
	LIRQ_CUPHY3 = 15,
	LIRQ_CUPHY4 = 16,
	LIRQ_ANA = 17,
	LIRQ_GPIO = 18,
	LIRQ_SW0 = 19,
	LIRQ_SW1 = 20,
	LIRQ_MIIM0 = 21,
	LIRQ_MIIM1 = 22,
	LIRQ_XTR = 23, /* Extraction data ready (manual injection) */
	LIRQ_INJ = 24, /* Injection data ready */
	LIRQ_SGPIO = 25,
	LIRQ_PTP_SYNC = 26,
	LIRQ_PTP_BLK = 27,
	LIRQ_ITGR = 28, /* Memory integrity */
	LIRQ_SEMA0 = 29,
	LIRQ_SEMA1 = 30,
	LIRQ_SEMA2 = 31,
	LIRQ_SEMA3 = 32,
	LIRQ_SEMA4 = 33,
	LIRQ_SEMA5 = 34,
	LIRQ_SEMA6 = 35,
	LIRQ_SEMA7 = 36,
	LIRQ_UVOV = 37, /* Under voltage over voltage ctrl */
	LIRQ_WDT = 38, /* Watchdog timer */
};

/* Lan9645x interrupt destinations */
enum lan9645x_irq_dest {
	LIRQ_DEST_CPU0 = 0,
	LIRQ_DEST_CPU1 = 1,
	LIRQ_DEST_EXT0 = 2, /* GPIO IRQ0 alt mode */
	LIRQ_DEST_EXT1 = 3, /* GPIO IRQ1 alt mode */
	LIRQ_DEST_EXT2 = 4, /* GPIO IRQ2 alt mode */
	LIRQ_DEST_EXT3 = 5, /* GPIO IRQ3 alt mode */
};

#define LAN9645X_IRQ(hwirq) \
	REGMAP_IRQ_REG(hwirq, hwirq / 32, BIT(hwirq % 32))

static const struct regmap_irq lan9645x_irqs[] = {
	LAN9645X_IRQ(LIRQ_EXT0),
	LAN9645X_IRQ(LIRQ_EXT1),
	LAN9645X_IRQ(LIRQ_EXT2),
	LAN9645X_IRQ(LIRQ_EXT3),
	LAN9645X_IRQ(LIRQ_TIMER0),
	LAN9645X_IRQ(LIRQ_TIMER1),
	LAN9645X_IRQ(LIRQ_TIMER2),
	LAN9645X_IRQ(LIRQ_UART),
	LAN9645X_IRQ(LIRQ_I2C0),
	LAN9645X_IRQ(LIRQ_I2C1),
	LAN9645X_IRQ(LIRQ_OTP),
	LAN9645X_IRQ(LIRQ_CUPHY0),
	LAN9645X_IRQ(LIRQ_CUPHY1),
	LAN9645X_IRQ(LIRQ_CUPHY2),
	LAN9645X_IRQ(LIRQ_CUPHY3),
	LAN9645X_IRQ(LIRQ_CUPHY4),
	LAN9645X_IRQ(LIRQ_ANA),
	LAN9645X_IRQ(LIRQ_GPIO),
	LAN9645X_IRQ(LIRQ_SW0),
	LAN9645X_IRQ(LIRQ_SW1),
	LAN9645X_IRQ(LIRQ_MIIM0),
	LAN9645X_IRQ(LIRQ_MIIM1),
	LAN9645X_IRQ(LIRQ_XTR),
	LAN9645X_IRQ(LIRQ_INJ),
	LAN9645X_IRQ(LIRQ_SGPIO),
	LAN9645X_IRQ(LIRQ_PTP_SYNC),
	LAN9645X_IRQ(LIRQ_PTP_BLK),
	LAN9645X_IRQ(LIRQ_ITGR),
	LAN9645X_IRQ(LIRQ_SEMA0),
	LAN9645X_IRQ(LIRQ_SEMA1),
	LAN9645X_IRQ(LIRQ_SEMA2),
	LAN9645X_IRQ(LIRQ_SEMA3),
	LAN9645X_IRQ(LIRQ_SEMA4),
	LAN9645X_IRQ(LIRQ_SEMA5),
	LAN9645X_IRQ(LIRQ_SEMA6),
	LAN9645X_IRQ(LIRQ_SEMA7),
	LAN9645X_IRQ(LIRQ_UVOV),
	LAN9645X_IRQ(LIRQ_WDT),
	[LAN9645X_NUM_IRQ] = { 0 },
};

struct lan9645x_oic_match_data {
	const struct regmap_irq *irqs;
	int num_irqs;

	u32 num_regs;
	u32 ena_base;
	u32 clr_base;
	u32 sticky_base;

	u32 num_dst;
	u32 status_base;
	u32 dst_map_base;

	u32 ext_dst_intr_drv;
};

struct lan9645x_oic_data {
	struct device *dev;
	struct mutex lock; /* bus sync lock */
	struct regmap *map;
	struct irq_domain *domain;
	int irq;

	unsigned int *sbuf; /* status buffer */
	unsigned int *mbuf; /* mask buffer */
	unsigned int *ins;  /* installed irqs */

	const struct irq_chip *chip;
	const struct lan9645x_oic_match_data *mdata;

	u32 dst_target;
	u32 drv_mode;
};

static u32 lan_addr(struct lan9645x_oic_data *oic, u32 base, int idx)
{
	const struct lan9645x_oic_match_data *d = oic->mdata;
	int stride = regmap_get_reg_stride(oic->map);

	if (!(base == d->status_base || base == d->dst_map_base))
		return base + idx * stride;

	/* We want the status qualified by destination, not the global status
	 * register. Therefore, we need to handle the replication per
	 * destination.
	 */
	return base + stride * (d->num_dst * idx + oic->dst_target);
}

static void lan_rd(struct lan9645x_oic_data *oic, u32 base, int idx, u32 *val)
{
	WARN_ON_ONCE(regmap_read(oic->map, lan_addr(oic, base, idx), val));
}

static void lan_wr(struct lan9645x_oic_data *oic, u32 base, int idx, u32 val)
{
	WARN_ON_ONCE(regmap_write(oic->map, lan_addr(oic, base, idx), val));
}

static void lan_rmw(struct lan9645x_oic_data *oic, u32 base, int idx, u32 mask,
		    u32 val)
{
	WARN_ON_ONCE(regmap_update_bits(oic->map, lan_addr(oic, base, idx),
					mask, val));
}

/* Can be called in atomic context. */
static void lan9645x_oic_enable(struct irq_data *data)
{
	struct lan9645x_oic_data *oic = irq_data_get_irq_chip_data(data);
	const struct lan9645x_oic_match_data *d = oic->mdata;
	const struct regmap_irq *irq = &d->irqs[data->hwirq];

	dev_dbg(oic->dev, "irq enable mask=0x%x hwirq=%lu idx=%u",
		irq->mask, data->hwirq, irq->reg_offset);

	oic->mbuf[irq->reg_offset] &= ~irq->mask;
}

/* Can be called in atomic context. */
static void lan9645x_oic_disable(struct irq_data *data)
{
	struct lan9645x_oic_data *oic = irq_data_get_irq_chip_data(data);
	const struct lan9645x_oic_match_data *d = oic->mdata;
	const struct regmap_irq *irq = &d->irqs[data->hwirq];

	dev_dbg(oic->dev, "irq disable mask=0x%x hwirq=%lu idx=%u",
		irq->mask, data->hwirq, irq->reg_offset);

	oic->mbuf[irq->reg_offset] |= irq->mask;
}

static void lan9645x_oic_irq_bus_lock(struct irq_data *data)
{
	struct lan9645x_oic_data *oic = irq_data_get_irq_chip_data(data);

	mutex_lock(&oic->lock);
}

static void lan9645x_oic_irq_bus_sync_unlock(struct irq_data *data)
{
	struct lan9645x_oic_data *oic = irq_data_get_irq_chip_data(data);
	const struct lan9645x_oic_match_data *d = oic->mdata;
	int i;

	for (i = 0; i < d->num_regs; i++) {
		/* Map enabled IRQs to configured destination */
		if (~oic->mbuf[i])
			lan_wr(oic, d->dst_map_base, i, ~oic->mbuf[i]);
		/* Disable masked interrupts, clear sticky and enable unmasked */
		lan_wr(oic, d->clr_base, i, oic->ins[i] & oic->mbuf[i]);
		lan_wr(oic, d->sticky_base, i, oic->ins[i] & ~oic->mbuf[i]);
		lan_wr(oic, d->ena_base, i, oic->ins[i] & ~oic->mbuf[i]);
	}

	mutex_unlock(&oic->lock);
}

static const struct irq_chip lan9645x_irq_chip = {
	.name = "irq-lan9645x-oic",
	.irq_enable = lan9645x_oic_enable,
	.irq_disable = lan9645x_oic_disable,
	.irq_bus_lock = lan9645x_oic_irq_bus_lock,
	.irq_bus_sync_unlock = lan9645x_oic_irq_bus_sync_unlock,
};

static int lan9645x_oic_domain_map(struct irq_domain *h, unsigned int virq,
				   irq_hw_number_t hwirq)
{
	struct lan9645x_oic_data *oic = h->host_data;

	dev_dbg(oic->dev, "domain_map virq=%u hwirq=%lu", virq, hwirq);

	irq_set_chip_data(virq, oic);
	irq_set_chip(virq, oic->chip);
	irq_set_nested_thread(virq, 1);
	irq_set_parent(virq, oic->irq);
	irq_set_noprobe(virq);

	return 0;
}

static const struct irq_domain_ops lan9645x_domain_ops = {
	.map = lan9645x_oic_domain_map,
	.xlate = irq_domain_xlate_onetwocell,
};

static irqreturn_t lan9645x_oic_thread_fn(int virq, void *dev_id)
{
	struct lan9645x_oic_data *oic = dev_id;
	const struct lan9645x_oic_match_data *d = oic->mdata;
	const struct regmap_irq *irq;
	int i, nhandled = 0;

	/* Read status registers */
	for (i = 0; i < d->num_regs; i++) {
		oic->sbuf[i] = 0;
		lan_rd(oic, d->status_base, i, &oic->sbuf[i]);
		oic->sbuf[i] &= ~oic->mbuf[i];
	}

	/* Ack/disable active interrupts */
	for (i = 0; i < d->num_regs; i++) {
		if (!oic->sbuf[i])
			continue;

		lan_wr(oic, d->clr_base, i, oic->sbuf[i]);
	}

	/* Handle interrupts */
	for (i = 0; i < d->num_irqs; i++) {
		irq = &d->irqs[i];
		if (!irq->mask)
			continue;

		if (oic->sbuf[irq->reg_offset] & irq->mask) {
			handle_nested_irq(irq_find_mapping(oic->domain, i));
			nhandled++;
		}
	}

	/* Clear sticky and reenable */
	for (i = 0; i < d->num_regs; i++) {
		if (!oic->sbuf[i])
			continue;

		lan_wr(oic, d->sticky_base, i, oic->sbuf[i]);
		lan_wr(oic, d->ena_base, i, oic->sbuf[i]);
	}

	return IRQ_RETVAL(nhandled > 0);
}

static int lan9645x_oic_domain_init(struct irq_domain *d)
{
	struct lan9645x_oic_data *oic = d->host_data;

	return devm_request_threaded_irq(oic->dev, oic->irq, NULL,
					 lan9645x_oic_thread_fn, IRQF_ONESHOT,
					 "lan9645x-oic-irq", oic);
}

static int lan9645x_oic_probe(struct platform_device *pdev)
{
	const struct lan9645x_oic_match_data *d =
		of_device_get_match_data(&pdev->dev);
	struct irq_domain_info d_info = {
		.fwnode = of_node_to_fwnode(pdev->dev.of_node),
		.size = d->num_irqs,
		.hwirq_max = d->num_irqs,
		.ops = &lan9645x_domain_ops,
		.init = lan9645x_oic_domain_init,
	};
	const struct regmap_config rmap_cfg = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
	};
	struct device *dev = &pdev->dev;
	struct lan9645x_oic_data *oic;
	struct irq_domain *domain;
	int irq, err = -ENOMEM;
	struct regmap *map;
	int i;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get the IRQ\n");

	map = ocelot_regmap_from_resource(pdev, 0, &rmap_cfg);
	if (IS_ERR_OR_NULL(map))
		return dev_err_probe(dev, PTR_ERR_OR_ZERO(map),
				     "Failed to create regmap\n");

	oic = devm_kzalloc(dev, sizeof(*oic), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	oic->irq = irq;
	oic->map = map;

	oic->mbuf =
		devm_kcalloc(dev, d->num_regs, sizeof(*oic->mbuf), GFP_KERNEL);
	if (!oic->mbuf)
		return -ENOMEM;

	oic->sbuf =
		devm_kcalloc(dev, d->num_regs, sizeof(*oic->sbuf), GFP_KERNEL);
	if (!oic->sbuf)
		return -ENOMEM;

	oic->ins =
		devm_kcalloc(dev, d->num_regs, sizeof(*oic->ins), GFP_KERNEL);
	if (!oic->ins)
		return -ENOMEM;

	oic->dev = dev;
	oic->mdata = d;
	oic->chip = &lan9645x_irq_chip;
	d_info.host_data = oic;
	mutex_init(&oic->lock);

	for (i = 0; i < d->num_irqs; i++) {
		if (!d->irqs[i].mask)
			continue;

		oic->ins[d->irqs[i].reg_offset] |= d->irqs[i].mask;
	}

	for (i = 0; i < d->num_regs; i++) {
		/* Mask/disable everything */
		oic->mbuf[i] = 0xffffffff;
		lan_wr(oic, d->clr_base, i, oic->ins[i]);
		lan_wr(oic, d->sticky_base, i, oic->ins[i]);
	}

	oic->dst_target = 2;
	if (of_property_present(dev->of_node, "microchip,dst_target")) {
		err = of_property_read_u32(dev->of_node, "microchip,dst_target",
					   &oic->dst_target);
		if (err) {
			dev_err(dev,
				"could not read property microchip,dst_target err:%d\n",
				err);
			return err;
		}
	}

	oic->drv_mode = 0;
	if (of_property_present(dev->of_node, "microchip,drv_mode")) {
		err = of_property_read_u32(dev->of_node, "microchip,drv_mode",
					   &oic->drv_mode);
		if (err) {
			dev_err(dev,
				"could not read property microchip,drv_mode err:%d\n",
				err);
			return err;
		}
	}

	if (oic->drv_mode)
		regmap_write(oic->map, d->ext_dst_intr_drv, oic->drv_mode);

	domain = devm_irq_domain_instantiate(dev, &d_info);
	if (IS_ERR(domain)) {
		err = PTR_ERR(domain);
		return dev_err_probe(dev, err,
				     "failed to instantiate the IRQ domain\n");
	}

	oic->domain = domain;

	dev_info(dev, "Driver registered.\n");

	return 0;
}

static const struct lan9645x_oic_match_data lan9645x_mdata = {
	.irqs = lan9645x_irqs,
	.num_irqs = ARRAY_SIZE(lan9645x_irqs),

	.num_regs = 2,
	.ena_base = 0x40,    /* CPU:INTR:INTR_ENA_SET */
	.clr_base = 0x38,    /* CPU:INTR:INTR_ENA_CLR */
	.sticky_base = 0x20, /* CPU:INTR:INTR_STICKY */

	.num_dst = 6,
	.status_base = 0x80,  /* CPU:INTR:DST_INTR_IDENT[0-5] */
	.dst_map_base = 0x50, /* CPU:INTR:DST_INTR_MAP[0-5] */

	.ext_dst_intr_drv = 0xb8, /* CPU:INTR:EXT_DST_INTR_DRV */
};

static const struct of_device_id lan9645x_oic_of_match[] = {
	{ .compatible = "microchip,lan9645x-oic", .data = &lan9645x_mdata },
	{},
};
MODULE_DEVICE_TABLE(of, lan9645x_oic_of_match);

static struct platform_driver lan9645x_oic_driver = {
	.probe = lan9645x_oic_probe,
	.driver = {
		.name = "lan9645x-oic",
		.of_match_table = lan9645x_oic_of_match,
	},
};
module_platform_driver(lan9645x_oic_driver);

MODULE_AUTHOR("Jens Emil Schulz Østergaard <jensemil.schulzostergaard@microchip.com>");
MODULE_DESCRIPTION("Microchip Lan9645x OIC driver");
MODULE_LICENSE("Dual MIT/GPL");
