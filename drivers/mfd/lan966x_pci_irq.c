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
#include <linux/pci.h>
#include <linux/delay.h>

#include "lan966x_pci_irq.h"
#include "lan966x_pci_regs.h"

#define CPU_TARGET_OFFSET		0xc0000
#define CPU_TARGET_LENGTH		0x10000

#define LAN966X_CPU_BAR		1
#define LAN966X_NR_IRQ		63

#define LAN_OFFSET_(id, tinst, tcnt,			\
		    gbase, ginst, gcnt, gwidth,		\
		    raddr, rinst, rcnt, rwidth)		\
	gbase + ((ginst) * gwidth) + raddr + ((rinst * rwidth))
#define LAN_OFFSET(...) LAN_OFFSET_(__VA_ARGS__)

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

int lan966x_pci_irq_setup(struct pci_dev *pdev, struct device_node *node,
			  struct lan966x_irq_data *data)
{
	struct irq_chip_generic *gc;
	struct irq_domain *domain;
	void __iomem *regs;
	int ret;

	regs = pci_iomap_range(pdev, LAN966X_CPU_BAR, CPU_TARGET_OFFSET,
			       CPU_TARGET_LENGTH);
	if (!regs)
		return -EIO;

	domain = irq_domain_add_linear(node, LAN966X_NR_IRQ,
				       &irq_generic_chip_ops, NULL);
	if (!domain) {
		dev_err(&pdev->dev, "unable to add irq domain\n");
		ret = -EIO;
		goto err_unmap;
	}

	ret = irq_alloc_domain_generic_chips(domain, 32, LAN966X_NR_IRQ / 32,
					     "icpu", handle_level_irq, 0, 0, 0);
	if (ret) {
		dev_err(&pdev->dev, "unable to alloc irq domain gc\n");
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
	/* Enable interrupts ANA, PTP-SYNC, PTP, XTR, INJ, FDMA, GPIO */
	gc->mask_cache = 0x17e00;

	irq_reg_writel(gc, 0x0, LAN_OFFSET(CPU_INTR_ENA));
	irq_reg_writel(gc, 0x17e00, LAN_OFFSET(CPU_INTR_STICKY));
	irq_reg_writel(gc, 0x17e00, LAN_OFFSET(CPU_DST_INTR_MAP(0)));

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
err_unmap:
	pci_iounmap(pdev, regs);

	return ret;
}

void lan966x_pci_irq_remove(struct pci_dev *pdev, struct lan966x_irq_data *data)
{
	irq_free_generic_chip(irq_get_domain_generic_chip(data->domain, 0));
	irq_free_generic_chip(irq_get_domain_generic_chip(data->domain, 32));
	irq_domain_remove(data->domain);
	pci_iounmap(pdev, data->regs);
}
