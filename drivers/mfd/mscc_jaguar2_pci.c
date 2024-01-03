/*
 * Driver for Microsemi Jaguar
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2018 Microsemi Corporation
 */
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip.h>
#include <linux/irq.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/uio_driver.h>

#define JAGUAR2_SWITCH_BAR	0
#define JAGUAR2_CONFIG_BAR	1
#define JAGUAR2_DDR_BAR		2

#define PCI_VENDOR_ID_MSCC		0x101b
#define PCI_DEVICE_ID_MSCC_JAGUAR	0xb003

static struct pci_device_id jaguar2_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MSCC, PCI_DEVICE_ID_MSCC_JAGUAR) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, jaguar2_ids);

#define ICPU_INTR_DST_INTR_MAP0				0x98
#define ICPU_INTR_DST_INTR_MAP1				0x9c
#define ICPU_INTR_DST_INTR_MAP2				0xa0
#define ICPU_INTR_DST_INTR_MAP3				0xa4

#define ICPU_PCIE_INTR_COMMON_CFG_WAKEUP_ON_INTR_DIS	BIT(2)
#define ICPU_PCIE_INTR_COMMON_CFG_LEGACY_MODE_INTR_SEL	BIT(1)
#define ICPU_PCIE_INTR_COMMON_CFG_PCIE_INTR_ENA		BIT(0)

#define ICPU_PCIE_INTR_COMMON_CFG			0x3c8
#define ICPU_PCIE_INTR_CFG0				0x3cc
#define ICPU_PCIE_INTR_CFG1				0x3d0

#define ICPU_PCIE_INTR_CFG_INTR_FALLING_ENA		BIT(1)
#define ICPU_PCIE_INTR_CFG_INTR_RISING_ENA		BIT(0)

static void __iomem *iomap;

#define ICPU_CFG_INTR_INTR_STICKY	0x80
#define ICPU_CFG_INTR_INTR_ENA		0x88
#define ICPU_CFG_INTR_INTR_ENA_CLR	0x8c
#define ICPU_CFG_INTR_INTR_ENA_SET	0x90
#define ICPU_CFG_INTR_DST_INTR_IDENT(x)	(0xa8 + 0x4 * (x))
#define ICPU_CFG_INTR_INTR_TRIGGER(x)	(0xcc + 0x4 * (x))

#define JAGUAR2_NR_IRQ 29

struct uio_jaguar2 {
    struct uio_info uio;
    spinlock_t lock;
    unsigned long flags;
    struct pci_dev *pdev;
};

static void jaguar2_irq_unmask(struct irq_data *data)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	unsigned int mask = data->mask;
	u32 val;

	irq_gc_lock(gc);
	val = irq_reg_readl(gc, ICPU_CFG_INTR_INTR_TRIGGER(0)) |
	      irq_reg_readl(gc, ICPU_CFG_INTR_INTR_TRIGGER(1));
	if (!(val & mask))
		irq_reg_writel(gc, mask, ICPU_CFG_INTR_INTR_STICKY);

	*ct->mask_cache &= ~mask;
	irq_reg_writel(gc, mask, ICPU_CFG_INTR_INTR_ENA_SET);
	irq_gc_unlock(gc);
}

static void jaguar2_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, 0);
	u32 uio, reg = irq_reg_readl(gc, ICPU_CFG_INTR_DST_INTR_IDENT(0));
	u32 mask = *gc->chip_types[0].mask_cache;
	struct uio_jaguar2 *priv = gc->private;

	uio = reg & mask;
	reg &= ~mask;

	chained_irq_enter(chip, desc);

	while (reg) {
		u32 hwirq = __fls(reg);

		generic_handle_irq(irq_find_mapping(d, hwirq));
		reg &= ~(BIT(hwirq));
	}

	chained_irq_exit(chip, desc);

	if (uio) {
		if (!test_and_set_bit(0, &priv->flags))
			pci_intx(priv->pdev, 0);
		uio_event_notify(&priv->uio);
	}

}

static int __init jaguar2_irq_common_init(struct pci_dev *pdev, int size,
					  struct uio_jaguar2 *priv)
{
	struct irq_domain *domain;
	struct irq_chip_generic *gc;
	int ret;
	struct device_node *node = pdev->dev.of_node;

	if (!pdev->irq || !node)
		return -EINVAL;

	domain = irq_domain_add_linear(node, size, &irq_generic_chip_ops, NULL);
	if (!domain) {
		pr_err("%s: unable to add irq domain\n", node->name);
		return -ENOMEM;
	}

	ret = irq_alloc_domain_generic_chips(domain, size, 1, "icpu",
					     handle_level_irq, 0, 0, 0);
	if (ret) {
		pr_err("%s: unable to alloc irq domain gc\n", node->name);
		goto err_domain_remove;
	}

	gc = irq_get_domain_generic_chip(domain, 0);
	gc->reg_base = iomap;
	if (!gc->reg_base) {
		pr_err("%s: unable to map resource\n", node->name);
		ret = -ENOMEM;
		goto err_gc_free;
	}

	gc->chip_types[0].regs.ack = ICPU_CFG_INTR_INTR_STICKY;
	gc->chip_types[0].regs.mask = ICPU_CFG_INTR_INTR_ENA_CLR;
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_unmask = jaguar2_irq_unmask;
	gc->mask_cache = 0xffffffff;

	gc->private = priv;

	/* Mask and ack all interrupts */
	irq_reg_writel(gc, 0, ICPU_CFG_INTR_INTR_ENA);
	irq_reg_writel(gc, 0xffffffff, ICPU_CFG_INTR_INTR_STICKY);

	irq_set_chained_handler_and_data(pdev->irq, jaguar2_irq_handler,
					 domain);

	return 0;

err_gc_free:
	irq_free_generic_chip(gc);

err_domain_remove:
	irq_domain_remove(domain);

	return ret;
}

static int jaguar2_irqcontrol(struct uio_info *dev_info, s32 irq_on)
{
	struct uio_jaguar2 *priv = dev_info->priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	if (irq_on) {
		if (test_and_clear_bit(0, &priv->flags))
			pci_intx(priv->pdev, 1);
	} else {
		if (!test_and_set_bit(0, &priv->flags))
			pci_intx(priv->pdev, 0);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int jaguar2_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct uio_jaguar2 *priv;
	struct uio_info *info;
	int ret;
	resource_size_t offset;

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

	offset = pci_resource_start(pdev, JAGUAR2_SWITCH_BAR);

	pci_set_master(pdev);
	pci_enable_msi(pdev);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	info = &priv->uio;
	info->priv = priv;

	iomap = pcim_iomap(pdev, JAGUAR2_SWITCH_BAR, 0);
	if (!iomap)
		return -ENOMEM;

	info->mem[0].addr = pci_resource_start(pdev, JAGUAR2_SWITCH_BAR);
	info->mem[0].size = pci_resource_len(pdev, JAGUAR2_SWITCH_BAR);
	info->mem[0].memtype = UIO_MEM_PHYS;
	info->mem[0].internal_addr = iomap;

	info->mem[1].addr = pci_resource_start(pdev, JAGUAR2_CONFIG_BAR);
	info->mem[1].size = pci_resource_len(pdev, JAGUAR2_CONFIG_BAR);
	info->mem[1].memtype = UIO_MEM_PHYS;

	info->name = "vcoreiii_switch";
	info->version = "0";
	info->irq = UIO_IRQ_CUSTOM;
	info->irqcontrol = jaguar2_irqcontrol;

	spin_lock_init(&priv->lock);
	priv->flags = 0; /* interrupt is enabled to begin with */
	priv->pdev = pdev;

	ret = uio_register_device(&pdev->dev, info);
	if (ret)
		return ret;

	pci_set_drvdata(pdev, info);

	dev_info(&pdev->dev, "JAGUAR2: %x\n",
		 (readl(iomap + 0x1010000) >> 12) & 0xffff);

	/* Route IRQs to PCI IRQs (both legacy and MSI) */
	writel(0xffffffff, ICPU_INTR_DST_INTR_MAP0 + iomap);
	writel(0xffffffff, ICPU_INTR_DST_INTR_MAP1 + iomap);
	writel(0xffffffff, ICPU_INTR_DST_INTR_MAP2 + iomap);
	writel(0xffffffff, ICPU_INTR_DST_INTR_MAP3 + iomap);
	writel(ICPU_PCIE_INTR_CFG_INTR_RISING_ENA |
	       ICPU_PCIE_INTR_CFG_INTR_FALLING_ENA, ICPU_PCIE_INTR_CFG0 +
	       iomap);
	writel(ICPU_PCIE_INTR_CFG_INTR_RISING_ENA |
	       ICPU_PCIE_INTR_CFG_INTR_FALLING_ENA, ICPU_PCIE_INTR_CFG1 +
	       iomap);
	writel(ICPU_PCIE_INTR_COMMON_CFG_PCIE_INTR_ENA,
	       ICPU_PCIE_INTR_COMMON_CFG + iomap);
	jaguar2_irq_common_init(pdev, JAGUAR2_NR_IRQ, priv);

	return of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
}

static void jaguar2_pci_remove(struct pci_dev *pdev)
{
	struct uio_info *info = pci_get_drvdata(pdev);

	uio_unregister_device(info);
}

static struct pci_driver jaguar2_pci_driver = {
	.name = "mscc_jaguar2",
	.id_table = jaguar2_ids,
	.probe = jaguar2_pci_probe,
	.remove = jaguar2_pci_remove,
};

module_pci_driver(jaguar2_pci_driver);

MODULE_DESCRIPTION("Microsemi Jaguar2 driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Alexandre Belloni <alexandre.belloni@bootlin.com>");
