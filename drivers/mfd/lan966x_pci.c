// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Microchip lan966x
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2019 Microchip Corporation
 */
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip.h>
#include <linux/irq.h>
#include <linux/iopoll.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/delay.h>

#include "lan966x_pci_regs.h"

#define LAN_OFFSET_(id, tinst, tcnt,			\
		    gbase, ginst, gcnt, gwidth,		\
		    raddr, rinst, rcnt, rwidth)		\
	gbase + ((ginst) * gwidth) + raddr + ((rinst * rwidth))
#define LAN_OFFSET(...) LAN_OFFSET_(__VA_ARGS__)

#define PCI_VENDOR_ID_MCHP		0x1055
#define PCI_DEVICE_ID_MCHP_LAN966X	0x9660

#define LAN966X_CPU_BAR		1
#define LAN966X_NR_IRQ		63
#define CPU_TARGET_OFFSET	(0xc0000)
#define CPU_TARGET_LENGTH	(0x10000)

static struct pci_device_id lan966x_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MCHP, PCI_DEVICE_ID_MCHP_LAN966X) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, lan966x_ids);

static void lan966x_irq_unmask(struct irq_data *data)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	unsigned int mask = data->mask;

	irq_gc_lock(gc);
	irq_reg_writel(gc, mask, gc->chip_types[0].regs.ack);
	*ct->mask_cache &= ~mask;
	irq_reg_writel(gc, mask, gc->chip_types[0].regs.enable);
	irq_gc_unlock(gc);
}

static void lan966x_irq_handler_domain(struct irq_domain *d,
				       struct irq_chip *chip,
				       struct irq_desc *desc,
				       u32 first_irq)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, first_irq);
	u32 reg = irq_reg_readl(gc, gc->chip_types[0].regs.type);
	u32 mask;
	u32 val;

	if (!gc->chip_types[0].mask_cache)
		return;

	mask = *gc->chip_types[0].mask_cache;
	reg &= ~mask;

	chained_irq_enter(chip, desc);
	while (reg) {
		u32 hwirq = __fls(reg);

		generic_handle_irq(irq_find_mapping(d, hwirq + first_irq));
		reg &= ~(BIT(hwirq));
	}

	val = irq_reg_readl(gc, gc->chip_types[0].regs.enable);
	irq_reg_writel(gc, 0, gc->chip_types[0].regs.enable);
	irq_reg_writel(gc, val, gc->chip_types[0].regs.enable);

	chained_irq_exit(chip, desc);
}

static void lan966x_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	lan966x_irq_handler_domain(d, chip, desc, 0);
	lan966x_irq_handler_domain(d, chip, desc, 32);
}

static int lan966x_irq_common_init(struct pci_dev *pdev, void __iomem *regs,
				   int size)
{
	struct device_node *node = pdev->dev.of_node;
	struct irq_chip_generic *gc;
	struct irq_domain *domain;
	int ret;

	domain = irq_domain_add_linear(node, size, &irq_generic_chip_ops, NULL);
	if (!domain) {
		pr_err("%s unable to add irq domain\n", node->name);
		return -ENOMEM;
	}

	ret = irq_alloc_domain_generic_chips(domain, 32, size / 32, "icpu",
					     handle_level_irq, 0, 0, 0);
	if (ret) {
		pr_err("%s unable to alloc irq domain gc\n", node->name);
		goto err_domain_remove;
	}

	/* Get first domain(0-31) */
	gc = irq_get_domain_generic_chip(domain, 0);
	gc->reg_base = regs;
	gc->chip_types[0].regs.enable = LAN_OFFSET(CPU_INTR_ENA_SET);
	gc->chip_types[0].regs.type = LAN_OFFSET(CPU_DST_INTR_IDENT(0));
	gc->chip_types[0].regs.ack = LAN_OFFSET(CPU_INTR_STICKY);
	gc->chip_types[0].regs.mask = LAN_OFFSET(CPU_INTR_ENA_CLR);
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_unmask = lan966x_irq_unmask;
	/* Enable interrupts ANA, PTP-SYNC, PTP, XTR, INJ, FDMA, GPIO, SGPIO */
	gc->mask_cache = 0x57e00;

	irq_reg_writel(gc, 0x0, LAN_OFFSET(CPU_INTR_ENA));
	irq_reg_writel(gc, 0x57e00, LAN_OFFSET(CPU_INTR_STICKY));
	irq_reg_writel(gc, 0x57e00, LAN_OFFSET(CPU_DST_INTR_MAP(0)));

	/* Get second domain(32-63) */
	gc = irq_get_domain_generic_chip(domain, 32);
	gc->reg_base = regs;
	gc->chip_types[0].regs.enable = LAN_OFFSET(CPU_INTR_ENA_SET1);
	gc->chip_types[0].regs.type = LAN_OFFSET(CPU_DST_INTR_IDENT1(0));
	gc->chip_types[0].regs.ack = LAN_OFFSET(CPU_INTR_STICKY1);
	gc->chip_types[0].regs.mask = LAN_OFFSET(CPU_INTR_ENA_CLR1);
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_unmask = lan966x_irq_unmask;
	/* Enable interrupts FLX0, FLX1, FLX2, FLX3, FLX4 */
	gc->mask_cache = 0x1f0000;

	irq_reg_writel(gc, 0x0, LAN_OFFSET(CPU_INTR_ENA1));
	irq_reg_writel(gc, 0x1f0000, LAN_OFFSET(CPU_INTR_STICKY1));
	irq_reg_writel(gc, 0x1f0000, LAN_OFFSET(CPU_DST_INTR_MAP1(0)));

	irq_set_chained_handler_and_data(pdev->irq, lan966x_irq_handler,
					 domain);

	return 0;

err_domain_remove:
	irq_domain_remove(domain);

	return ret;
}

struct lan966x_ctx {
	struct work_struct rescan_work;
	struct completion rescan_comp;
	struct pci_dev *pdev;
};

static void lan966x_rescan_cb(struct work_struct *work)
{
	struct lan966x_ctx *ctx = container_of(work, struct lan966x_ctx, rescan_work);
	struct pci_dev *pdev = ctx->pdev;
	struct pci_bus *bus;

	complete(&ctx->rescan_comp);

	pci_lock_rescan_remove();
	bus = pdev->bus;
	pci_stop_and_remove_bus_device(pdev);
	pci_rescan_bus(bus);
	pci_unlock_rescan_remove();
	put_device(&pdev->dev);
}

static void lan966x_pci_rescan(struct pci_dev *pdev)
{
	struct lan966x_ctx *ctx;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return;

	pci_set_drvdata(pdev, ctx);
	ctx->pdev = pdev;
	get_device(&pdev->dev);

	init_completion(&ctx->rescan_comp);
	INIT_WORK(&ctx->rescan_work, lan966x_rescan_cb);
	schedule_work(&ctx->rescan_work);
}

static int lan966x_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	static bool try_rescan = 0;
	void __iomem *regs;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	ret = pcim_enable_device(pdev);
	if (ret) {
		/* The following issue was observed, by the time the PCI host
		 * does the enumaration then end node doesn't have enough time
		 * to configure the PCI. So we will be in a situation where
		 * BARs are not mapped and device ID is wrong. In that case,
		 * this pcim_enable_device will fail. So in case of failure
		 * try to remove the device and start again the pci rescan
		 * because by this time the end node managed to configure the
		 * PCI. It takes ~100ms from power on until the the PCI is
		 * configured. So, we will get probe again and hopefully the
		 * pcim_enable_device will succeed and everything will work.
		 * Otherwise just return the error code.
		 * This code is similar to the one in 'ath9k_pci_owl_loader.c'
		 * where they need to load a different firmware.
		 */
		if (try_rescan)
			return ret;

		try_rescan = 1;
		lan966x_pci_rescan(pdev);
		return 0;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev,
				"DMA configuration failed: 0x%x\n", ret);
			return ret;
		}
	}

	regs = pci_iomap_range(pdev, LAN966X_CPU_BAR,
			       CPU_TARGET_OFFSET, CPU_TARGET_LENGTH);
	pci_set_master(pdev);

	ret = lan966x_irq_common_init(pdev, regs, LAN966X_NR_IRQ);
	if (ret) {
		dev_err(&pdev->dev, "Interrupt config failed: 0x%x\n", ret);
		return ret;
	}

	return of_platform_default_populate(pdev->dev.of_node, NULL, &pdev->dev);
}

static void lan966x_pci_remove(struct pci_dev *pdev)
{
	struct lan966x_ctx *ctx = pci_get_drvdata(pdev);

	if (ctx) {
		wait_for_completion(&ctx->rescan_comp);
		pci_set_drvdata(pdev, NULL);
	}
}

static struct pci_driver lan966x_pci_driver = {
	.name = "microchip_lan966x_pci",
	.id_table = lan966x_ids,
	.probe = lan966x_pci_probe,
	.remove = lan966x_pci_remove,
};

module_pci_driver(lan966x_pci_driver);

MODULE_DESCRIPTION("Microchip LAN966X driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
