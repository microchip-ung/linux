// SPDX-License-Identifier: GPL-2.0
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

#include "lan969x_pci_regs.h"

#define LAN_OFFSET_(id, tinst, tcnt,			\
		    gbase, ginst, gcnt, gwidth,		\
		    raddr, rinst, rcnt, rwidth)		\
	gbase + ((ginst) * gwidth) + raddr + ((rinst * rwidth))
#define LAN_OFFSET(...) LAN_OFFSET_(__VA_ARGS__)

#define PCI_VENDOR_ID_MCHP		0x1055
#define PCI_DEVICE_ID_MCHP_LAN969X	0x9690

#define LAN969X_CPU_BAR		1
#define LAN969X_NR_IRQ		121
#define CPU_TARGET_OFFSET	(0xc0000)
#define CPU_TARGET_LENGTH	(0x10000)

static struct pci_device_id lan969x_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MCHP, PCI_DEVICE_ID_MCHP_LAN969X) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, lan969x_ids);

static void lan969x_irq_unmask(struct irq_data *data)
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

static void lan969x_irq_handler_domain(struct irq_domain *d,
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

static void lan969x_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	lan969x_irq_handler_domain(d, chip, desc, 0);
	lan969x_irq_handler_domain(d, chip, desc, 32);
}

static int lan969x_irq_common_init(struct pci_dev *pdev, void __iomem *regs,
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
	gc->chip_types[0].chip.irq_unmask = lan969x_irq_unmask;
	/* Enable interrupts ANA, PTP-SYNC, PTP, XTR, INJ, GPIO, SGPIO */
	gc->mask_cache = 0x18f80;

	irq_reg_writel(gc, 0x0, LAN_OFFSET(CPU_INTR_ENA));
	irq_reg_writel(gc, 0x18f80, LAN_OFFSET(CPU_INTR_STICKY));
	irq_reg_writel(gc, 0x18f80, LAN_OFFSET(CPU_DST_INTR_MAP(0)));

	/* Get second domain(32-63) */
	gc = irq_get_domain_generic_chip(domain, 32);
	gc->reg_base = regs;
	gc->chip_types[0].regs.enable = LAN_OFFSET(CPU_INTR_ENA_SET1);
	gc->chip_types[0].regs.type = LAN_OFFSET(CPU_DST_INTR_IDENT1(0));
	gc->chip_types[0].regs.ack = LAN_OFFSET(CPU_INTR_STICKY1);
	gc->chip_types[0].regs.mask = LAN_OFFSET(CPU_INTR_ENA_CLR1);
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_unmask = lan969x_irq_unmask;
	/* Enable interrupts FLX0, FLX1, FLX2, FLX3, FLX4 */
	gc->mask_cache = 0x7c000;

	irq_reg_writel(gc, 0x0, LAN_OFFSET(CPU_INTR_ENA1));
	irq_reg_writel(gc, 0x7c000, LAN_OFFSET(CPU_INTR_STICKY1));
	irq_reg_writel(gc, 0x7c000, LAN_OFFSET(CPU_DST_INTR_MAP1(0)));

	irq_set_chained_handler_and_data(pdev->irq, lan969x_irq_handler,
					 domain);

	return 0;

err_domain_remove:
	irq_domain_remove(domain);

	return ret;
}

static int lan969x_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	void __iomem *regs;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev,
				"DMA configuration failed: 0x%x\n", ret);
			return ret;
		}
	}

	regs = pci_iomap_range(pdev, LAN969X_CPU_BAR,
			       CPU_TARGET_OFFSET, CPU_TARGET_LENGTH);
	pci_set_master(pdev);

	ret = lan969x_irq_common_init(pdev, regs, LAN969X_NR_IRQ);
	if (ret) {
		dev_err(&pdev->dev, "Interrupt config failed: 0x%x\n", ret);
		return ret;
	}

	return of_platform_default_populate(pdev->dev.of_node, NULL, &pdev->dev);
}

static void lan969x_pci_remove(struct pci_dev *pdev)
{
	pci_set_drvdata(pdev, NULL);
}

static struct pci_driver lan969x_pci_driver = {
	.name = "microchip_lan969x_pci",
	.id_table = lan969x_ids,
	.probe = lan969x_pci_probe,
	.remove = lan969x_pci_remove,
};

module_pci_driver(lan969x_pci_driver);

MODULE_DESCRIPTION("Microchip LAN969X driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
