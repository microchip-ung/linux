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

#include "lan966x_pci_regs_sr.h"

#ifdef CONFIG_DEBUG_KERNEL
#define LAN_RD_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 readl(lan966x->regs[id + (tinst)] +		\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)))

#define LAN_WR_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 writel(val, lan966x->regs[id + (tinst)] +	\
	        gbase + ((ginst) * gwidth) +		\
	        raddr + ((rinst) * rwidth)))
#else
#define LAN_RD_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	readl(lan966x->regs[id + (tinst)] +		\
	      gbase + ((ginst) * gwidth) +		\
	      raddr + ((rinst) * rwidth))

#define LAN_WR_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	writel(val, lan966x->regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth))
#endif

#define LAN_OFFSET_(id, tinst, tcnt,			\
		    gbase, ginst, gcnt, gwidth,		\
		    raddr, rinst, rcnt, rwidth)		\
	gbase + ((ginst) * gwidth) + raddr + ((rinst * rwidth))


#define LAN_WR(...) LAN_WR_(__VA_ARGS__)
#define LAN_RD(...) LAN_RD_(__VA_ARGS__)
#define LAN_OFFSET(...) LAN_OFFSET_(__VA_ARGS__)

#define LAN966X_SWITCH_BAR	0
#define LAN966X_CPU_BAR		1

#define PCI_VENDOR_ID_MCHP		0x101b
#define PCI_DEVICE_ID_MCHP_LAN966X	0x9662

#define CPU_TARGET_OFFSET	(0xc0000)
#define CPU_TARGET_LENGTH	(0x10000)

#define LAN966X_NR_IRQ		31

#define READL_SLEEP_US		10
#define READL_TIMEOUT_US	100000

static struct pci_device_id lan966x_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MCHP, PCI_DEVICE_ID_MCHP_LAN966X) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, lan966x_ids);

struct lan966x {
	void __iomem *regs[NUM_TARGETS];
};

struct lan966x *lan966x;

static void lan966x_irq_unmask(struct irq_data *data)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	unsigned int mask = data->mask;
	u32 val;

	irq_gc_lock(gc);
	val = irq_reg_readl(gc, LAN_OFFSET(CPU_INTR_TRIGGER(0))) |
	      irq_reg_readl(gc, LAN_OFFSET(CPU_INTR_TRIGGER(1)));

	if (!(val & mask))
		irq_reg_writel(gc, mask, LAN_OFFSET(CPU_INTR_STICKY));

	*ct->mask_cache &= ~mask;
	irq_reg_writel(gc, mask, LAN_OFFSET(CPU_INTR_ENA_SET));

	irq_gc_unlock(gc);
}

static void lan966x_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, 0);
	u32 reg = irq_reg_readl(gc, LAN_OFFSET(CPU_DST_INTR_IDENT(0)));
	u32 mask = *gc->chip_types[0].mask_cache;
	u32 val;

	reg &= ~mask;

	chained_irq_enter(chip, desc);
	while (reg) {
		u32 hwirq = __fls(reg);

		generic_handle_irq(irq_find_mapping(d, hwirq));
		reg &= ~(BIT(hwirq));
	}

	val = irq_reg_readl(gc, LAN_OFFSET(CPU_INTR_ENA));
	irq_reg_writel(gc, 0, LAN_OFFSET(CPU_INTR_ENA));
	irq_reg_writel(gc, val, LAN_OFFSET(CPU_INTR_ENA));

	chained_irq_exit(chip, desc);
}

static int lan966x_irq_common_init(struct pci_dev *pdev, int size)
{
	struct device_node *node = pdev->dev.of_node;
	struct irq_chip_generic *gc;
	struct irq_domain *domain;
	int ret;

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (!ret) {
		dev_err(&pdev->dev, "Couldn't allocate MSI IRQ vectors\n");
		return ret;
	}

	domain = irq_domain_add_linear(node, size, &irq_generic_chip_ops, NULL);
	if (!domain) {
		pr_err("%s unable to add irq domain\n", node->name);
		return -ENOMEM;
	}

	ret = irq_alloc_domain_generic_chips(domain, size, 1, "icpu",
					     handle_level_irq, 0, 0, 0);
	if (ret) {
		pr_err("%s unable to alloc irq domain gc\n", node->name);
		goto err_domain_remove;
	}

	gc = irq_get_domain_generic_chip(domain, 0);
	gc->reg_base = lan966x->regs[TARGET_CPU];
	if (!gc->reg_base) {
		pr_err("%s unable to map resource\n", node->name);
		ret = -ENOMEM;
		goto err_gc_free;
	}

	gc->chip_types[0].regs.ack = LAN_OFFSET(CPU_INTR_STICKY);
	gc->chip_types[0].regs.mask = LAN_OFFSET(CPU_INTR_ENA_CLR);
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_unmask = lan966x_irq_unmask;
	gc->mask_cache = 0x7e00;

	irq_reg_writel(gc, 0x0, LAN_OFFSET(CPU_INTR_ENA));
	irq_reg_writel(gc, 0x7e00, LAN_OFFSET(CPU_INTR_STICKY));

	irq_set_chained_handler_and_data(pdev->irq, lan966x_irq_handler,
					 domain);

	return 0;

err_gc_free:
	irq_free_generic_chip(gc);

err_domain_remove:
	irq_domain_remove(domain);

	return ret;
}

static int lan966x_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
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

	lan966x = devm_kmalloc(&pdev->dev, sizeof(struct lan966x), GFP_KERNEL);
	if (!lan966x)
		return -ENOMEM;

	lan966x->regs[TARGET_CPU] = pci_iomap_range(pdev, LAN966X_CPU_BAR,
						    CPU_TARGET_OFFSET,
						    CPU_TARGET_LENGTH);

	/* Enable interrupts */
	pci_set_master(pdev);

	/* Enable irq */
	LAN_WR(0x7e00, lan966x, CPU_DST_INTR_MAP(0));
	ret = lan966x_irq_common_init(pdev, LAN966X_NR_IRQ);
	if (ret) {
		dev_err(&pdev->dev, "Interrupt config failed: 0x%x\n", ret);
		return ret;
	}

	return of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
}

static void lan966x_pci_remove(struct pci_dev *pdev)
{
}

static struct pci_driver lan966x_pci_driver = {
	.name = "microchip_lan966x_pci_sr",
	.id_table = lan966x_ids,
	.probe = lan966x_pci_probe,
	.remove = lan966x_pci_remove,
};

module_pci_driver(lan966x_pci_driver);

MODULE_DESCRIPTION("Microchip LAN966X driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
