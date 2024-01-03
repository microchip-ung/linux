// SPDX-License-Identifier: (GPL-2.0 OR MIT)
//
// Microsemi SoC Sparx5 Multifunction/PCIe driver
//
// Copyright (c) 2019 Microsemi

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/mfd/core.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip.h>
#include <linux/irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/uio_driver.h>

#define DEVICE_NAME "microchip-sparx5-pci"
#define UIO_NAME    "mscc_switch" /* Used by MESA */
#define UIO_VERSION "1.0.0"

#define PCI_VENDOR_ID_MSCC              0x101b
#define PCI_DEVICE_ID_MSCC_SPARX5      0xb006

#define SPARX5_SWITCH_BAR      0     /* Default instance in CML model */
#define SPARX5_CPU_BAR         2     /* amba_top instance in CML model */
#define SPARX5_SUBCPU_BAR      4     /* subcpu_sys instance in CML model */

/* CPU @amba_top, offset 0x10000000, size: 724 bytes */
/* CPU:INTR:INTR_STICKY */
#define CPU_INTR_STICKY_OFF                            0x100001c4
/* CPU:INTR:INTR_STICKY1 */
#define CPU_INTR_STICKY1_OFF                           0x100001c8
/* CPU:INTR:INTR_IDENT */
#define CPU_INTR_IDENT_OFF                             0x100001ec
/* CPU:INTR:INTR_IDENT1 */
#define CPU_INTR_IDENT1_OFF                            0x100001f0
/* CPU:INTR:INTR_ENA_CLR */
#define CPU_INTR_ENA_CLR_OFF                           0x100001dc
/* CPU:INTR:INTR_ENA_CLR1 */
#define CPU_INTR_ENA_CLR1_OFF                          0x100001e0
/* CPU:INTR:INTR_ENA_SET */
#define CPU_INTR_ENA_SET_OFF                           0x100001e4
/* CPU:INTR:INTR_ENA_SET1 */
#define CPU_INTR_ENA_SET1_OFF                          0x100001e8
/* CPU:INTR:DST_INTR_MAP[2] */
#define CPU_INTR_DST_MAP_R_OFF(ridx)                   (0x100001f4 + (ridx*4))
/* CPU:INTR:DST_INTR_MAP1[2] */
#define CPU_INTR_DST_MAP1_R_OFF(ridx)                  (0x100001fc + (ridx*4))
/* CPU:INTR:INTR_TRIGGER[2] */
#define CPU_INTR_TRIGGER_R_OFF(ridx)                   (0x100001ac + (ridx*4))
/* CPU:INTR:INTR_TRIGGER1[2] */
#define CPU_INTR_TRIGGER1_R_OFF(ridx)                  (0x100001b4 + (ridx*4))
/* CPU:PCIE:PCIE_INTR_COMMON_CFG[2] */
#define CPU_PCIE_INTR_COMMON_CFG_R_OFF(ridx)           (0x1000018c + (ridx*4))
/* CPU:PCIE:PCIE_CFG */
#define CPU_PCIE_CFG_OFF                               0x10000110
/* CPU:PCIE:PCIEMST_PF0_BAR2_OFFSET_LOW */
#define CPU_PCIE_PCIEMST_PF0_BAR2_OFFSET_LOW_OFF       0x10000134
/* CPU:PCIE:PCIEMST_PF0_BAR2_OFFSET_HIGH */
#define CPU_PCIE_PCIEMST_PF0_BAR2_OFFSET_HIGH_OFF      0x10000138
/* CPU:PCIE:PCIEMST_PF0_BAR2_MASK_LOW */
#define CPU_PCIE_PCIEMST_PF0_BAR2_MASK_LOW_OFF         0x1000013c
/* CPU:PCIE:PCIEMST_PF0_BAR2_MASK_HIGH */
#define CPU_PCIE_PCIEMST_PF0_BAR2_MASK_HIGH_OFF        0x10000140


/* DEVCPU_GCB:CHIP_REGS:CHIP_ID */
#define DEVCPU_GCB_CHIP_REGS_ID_OFF                    0x01010000

/* CPU:PCIE:PCIE_INTR_COMMON_CFG[2] */
#define CPU_PCIE_INTR_COMMON_CFG_ENA                   BIT(1)
/* CPU:PCIE:PCIE_CFG */
#define CPU_PCIE_CFG_DBI_ACCESS_ENA(x)                    (((x) << 7) & GENMASK(8, 7))
#define CPU_PCIE_CFG_DBI_ACCESS_ENA_M                     GENMASK(8, 7)
#define CPU_PCIE_CFG_DBI_ACCESS_ENA_X(x)                  (((x) & GENMASK(8, 7)) >> 7)

#define GET_REGION(off)                                ((off) & GENMASK(31, 28))
#define GET_REGION_INDEX(off)                          ((off) >> 28)
#define GET_ADDRESS(off)                               ((off) & GENMASK(27, 0))

/* Debugging flags */
/* #define SPARX5_ACCESS_LOG */

static struct pci_device_id microchip_sparx5_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MSCC, PCI_DEVICE_ID_MSCC_SPARX5) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, microchip_sparx5_ids);

struct microchip_sparx5_uio {
	struct uio_info uio;
	spinlock_t lock;
	unsigned long flags;
	struct pci_dev *pdev;
};

static u32 microchip_sparx5_mfd_readl(struct microchip_sparx5_uio *priv, u32 reg)
{
	void __iomem *addr =
		priv->uio.mem[GET_REGION_INDEX(reg)].internal_addr +
		GET_ADDRESS(reg);
	u32 data = readl(addr);
#ifdef SPARX5_ACCESS_LOG
	pr_debug("%s:%d %s: addr 0x%llx, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		priv->uio.mem[GET_REGION_INDEX(reg)].addr +
		GET_ADDRESS(reg),
		data);
#endif
	return data;
}

static void microchip_sparx5_mfd_writel(struct microchip_sparx5_uio *priv, u32 reg, u32 data)
{
	void __iomem *addr =
		priv->uio.mem[GET_REGION_INDEX(reg)].internal_addr +
		GET_ADDRESS(reg);
#ifdef SPARX5_ACCESS_LOG
	pr_debug("%s:%d %s: addr 0x%llx, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		priv->uio.mem[GET_REGION_INDEX(reg)].addr +
			GET_ADDRESS(reg),
		data);
#endif
	writel(data, addr);
}

#ifdef SPARX5_ACCESS_LOG
static u32 sparx5_readl(void __iomem *addr)
{
	u32 data = readl(addr);
	pr_debug("%s:%d %s: addr %px, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		 addr, data);
	return data;
}

static void sparx5_writel(u32 data, void __iomem *addr)
{
	pr_debug("%s:%d %s: addr %px, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		addr, data);
	writel(data, addr);
}
#endif

static int microchip_sparx5_mfd_irqcontrol(struct uio_info *info, s32 irq_on)
{
	struct microchip_sparx5_uio *priv = info->priv;
	unsigned long flags;

	pr_debug("%s:%d %s: irq_on %d\n",
		__FILE__, __LINE__, __func__, irq_on);
	spin_lock_irqsave(&priv->lock, flags);
	if (irq_on) {
		if (test_and_clear_bit(0, &priv->flags)) {
			pci_intx(priv->pdev, 1);
		}
	} else {
		if (!test_and_set_bit(0, &priv->flags)) {
			pci_intx(priv->pdev, 0);
		}
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static void microchip_sparx5_mfd_irq_unmask(struct irq_data *data)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	unsigned int mask = data->mask;

	pr_debug("%s:%d %s: hwirq %lu\n",
		__FILE__, __LINE__, __func__, data->hwirq);
	irq_gc_lock(gc);
	irq_reg_writel(gc, mask, ct->regs.ack);
	*ct->mask_cache &= ~mask;
	irq_reg_writel(gc, mask, ct->regs.enable);
	irq_gc_unlock(gc);
}


/*
 * Sparx5 level encoding: this is a 2 bit value
 * with
 * - LSB in CPU:INTR:INTR_TRIGGER*[0].INTR_TRIGGER
 * - MSB in CPU:INTR:INTR_TRIGGER*[1].INTR_TRIGGER
 * 0: Interrupt is level-activated
 * 1: Interrupt is edge-triggered
 * 2: Interrupt is falling-edge-triggered
 * 3: Interrupt is rising-edge-triggered
 */
static int microchip_sparx5_mfd_irq_set_type(struct irq_data *data,
					 unsigned int flow_type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	int chip_hwirq = data->hwirq % 32;
	u32 lsb, msb;
	u32 new_lsb = 0, new_msb = 0;

	pr_debug("%s:%d %s: irq: %d, hwirq %lu, chip_hwirq: %d, flow type: %u\n",
		__FILE__, __LINE__, __func__,
		data->irq, data->hwirq, chip_hwirq, flow_type);

	switch (flow_type) {
	case IRQ_TYPE_NONE:
	case IRQ_TYPE_LEVEL_LOW:
		return -1;
	case IRQ_TYPE_EDGE_RISING:
		new_lsb |= BIT(chip_hwirq);
		new_msb |= BIT(chip_hwirq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		new_msb |= BIT(chip_hwirq);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		new_lsb |= BIT(chip_hwirq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		break;
	}
	irq_gc_lock(gc);
	/* Read the trigger register values */
	lsb = irq_reg_readl(gc, ct->regs.polarity);
	msb = irq_reg_readl(gc, ct->regs.polarity + 4);
	/* Mask out the current trigger values */
	lsb &= ~BIT(chip_hwirq);
	msb &= ~BIT(chip_hwirq);
	/* Add the new trigger values */
	lsb |= new_lsb;
	msb |= new_msb;
	/* Write the new trigger register values */
	irq_reg_writel(gc, lsb, ct->regs.polarity);
	irq_reg_writel(gc, msb, ct->regs.polarity + 4);
	irq_gc_unlock(gc);

	return IRQ_SET_MASK_OK;
}

static int microchip_sparx5_mfd_find_irq(struct irq_domain *d, int first_irq)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, first_irq);
	struct irq_chip_type *ct = &gc->chip_types[0];
	u32 mask = *ct->mask_cache;
	u32 identity;
	u32 uio;

	identity = irq_reg_readl(gc, ct->regs.type);
	uio = identity & mask;
	identity &= ~mask;

	pr_debug("%s:%d %s: index: %d, identity: 0x%x\n",
		 __FILE__, __LINE__, __func__, first_irq, identity);
	while (identity) {
		u32 hwirq = __fls(identity);

		pr_debug("%s:%d %s: hwirq: %u maps to IRQ: %d\n",
			 __FILE__, __LINE__, __func__,
			 hwirq + first_irq,
			 irq_find_mapping(d, hwirq + first_irq));
		generic_handle_irq(irq_find_mapping(d, hwirq + first_irq));
		identity &= ~(BIT(hwirq));
	}
	return uio;

}

static void microchip_sparx5_mfd_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	u32 uio;

	chained_irq_enter(irq_desc_get_chip(desc), desc);
	uio = microchip_sparx5_mfd_find_irq(d, 0);
	uio |= microchip_sparx5_mfd_find_irq(d, 32);
	chained_irq_exit(irq_desc_get_chip(desc), desc);

	if (uio) {
		struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, 0);

		if (gc) {
			struct microchip_sparx5_uio *priv = gc->private;

			if (!test_and_set_bit(0, &priv->flags)) {
				pci_intx(priv->pdev, 0);
			}
			uio_event_notify(&priv->uio);
		}
	}
}

static void microchip_sparx5_mfd_config_irqchip(struct microchip_sparx5_uio *priv,
	struct irq_chip_generic *gc,
	dma_addr_t acknowledge,
	dma_addr_t atomic_disable,
	dma_addr_t atomic_enable,
	dma_addr_t identity,
	dma_addr_t trigger)
{
	gc->reg_base = priv->uio.mem[1].internal_addr;
#ifdef SPARX5_ACCESS_LOG
	gc->reg_writel = sparx5_writel;
	gc->reg_readl = sparx5_readl;
#endif
	gc->chip_types[0].regs.ack = acknowledge;
	gc->chip_types[0].regs.mask = atomic_disable;
	gc->chip_types[0].regs.enable = atomic_enable;
	gc->chip_types[0].regs.type = identity;
	gc->chip_types[0].regs.polarity = trigger;
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_unmask = microchip_sparx5_mfd_irq_unmask;
	gc->chip_types[0].chip.irq_set_type = microchip_sparx5_mfd_irq_set_type;
	gc->chip_types[0].mask_cache = &gc->mask_cache;
	gc->mask_cache = 0xffffffff;
	gc->private = priv;
	/* Mask and ack all interrupts */
	irq_reg_writel(gc, 0, atomic_enable);
	irq_reg_writel(gc, ~0, acknowledge);
}

static int __init microchip_sparx5_mfd_irq_common_init(struct microchip_sparx5_uio *priv,
	struct device_node *node)
{
	struct irq_domain *domain;
	int ret;

	pr_debug("%s:%d %s: Using IRQ: %d\n",
		 __FILE__, __LINE__, __func__, priv->pdev->irq);
	if (!priv->pdev->irq || !node) {
		pr_err("no node or IRQ: %d, 0x%px\n", priv->pdev->irq, node);
		return -EINVAL;
	}

	/*
	 * There are 50 interrupts from Sparx5 in all
	 */
	/* Map interrupts to destination EXT_DST0 (0) */
	microchip_sparx5_mfd_writel(priv, CPU_INTR_DST_MAP_R_OFF(0), ~0);
	microchip_sparx5_mfd_writel(priv, CPU_INTR_DST_MAP1_R_OFF(0), 0x3ffff);
	/* Set Level activated interrupts */
	microchip_sparx5_mfd_writel(priv, CPU_INTR_TRIGGER_R_OFF(0), 0);
	microchip_sparx5_mfd_writel(priv, CPU_INTR_TRIGGER_R_OFF(1), 0);
	microchip_sparx5_mfd_writel(priv, CPU_INTR_TRIGGER1_R_OFF(0), 0);
	microchip_sparx5_mfd_writel(priv, CPU_INTR_TRIGGER1_R_OFF(1), 0);
	/* Enable PCIe Legacy interrupt on Function 0 using EXT_DST0 */
	microchip_sparx5_mfd_writel(priv, CPU_PCIE_INTR_COMMON_CFG_R_OFF(0),
				CPU_PCIE_INTR_COMMON_CFG_ENA);

	domain = irq_domain_add_linear(node, 50, &irq_generic_chip_ops, NULL);
	if (!domain) {
		pr_err("%s: unable to add irq domain\n", node->name);
		return -ENOMEM;
	}

	/* Create 2 generic chips with 32 interrupts each */
	ret = irq_alloc_domain_generic_chips(domain, 32, 2, "sparx5",
					     handle_level_irq, 0, 0, 0);
	if (ret) {
		pr_err("%s: unable to alloc irq domain gc\n", node->name);
		goto err_domain_remove;
	}

	/* Setup the first chip (handles irq 0-31) */
	microchip_sparx5_mfd_config_irqchip(priv,
					irq_get_domain_generic_chip(domain, 0),
					GET_ADDRESS(CPU_INTR_STICKY_OFF),
					GET_ADDRESS(CPU_INTR_ENA_CLR_OFF),
					GET_ADDRESS(CPU_INTR_ENA_SET_OFF),
					GET_ADDRESS(CPU_INTR_IDENT_OFF),
					GET_ADDRESS(CPU_INTR_TRIGGER_R_OFF(0)));

	/* Setup the second chip (handles irq 32-49) */
	microchip_sparx5_mfd_config_irqchip(priv,
					irq_get_domain_generic_chip(domain, 32),
					GET_ADDRESS(CPU_INTR_STICKY1_OFF),
					GET_ADDRESS(CPU_INTR_ENA_CLR1_OFF),
					GET_ADDRESS(CPU_INTR_ENA_SET1_OFF),
					GET_ADDRESS(CPU_INTR_IDENT1_OFF),
					GET_ADDRESS(CPU_INTR_TRIGGER1_R_OFF(0)));

	pr_debug("%s:%d %s: Chaining IRQ: %d\n",
		 __FILE__, __LINE__, __func__, priv->pdev->irq);
	irq_set_chained_handler_and_data(priv->pdev->irq,
					 microchip_sparx5_mfd_irq_handler,
					 domain);

	return 0;

err_domain_remove:
	irq_domain_remove(domain);

	return ret;
}

static int microchip_sparx5_mfd_probe(struct pci_dev *dev,
				  const struct pci_device_id *id)
{
	struct microchip_sparx5_uio *priv;
	struct uio_info *info;
	u32 chip_id;
	int ret = -ENODEV;

	if (!dev->dev.of_node) {
		dev_warn(&dev->dev, "No platform device nodes in PCIe device\n");
		return -ENODEV;
	}
	if (dev->devfn > 0) {
		dev_warn(&dev->dev, "Not accepting function 1 or higher\n");
		return -ENOENT;
	}
	priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(&dev->dev, "No memory\n");
		return -ENOMEM;
	}
	spin_lock_init(&priv->lock);
	priv->flags = 0; /* interrupt is enabled to begin with */
	priv->pdev = dev;

	ret = pcim_enable_device(dev);
	if (ret) {
		dev_err(&dev->dev, "Could not enable PCI device\n");
		return ret;
	}
	pci_set_master(dev);
	dev_info(&dev->dev, "Device is master\n");

	info = &priv->uio;
	info->priv = priv;

	info->mem[0].name = "switch_regs";  /* Used by MESA */
	info->mem[0].addr = pci_resource_start(dev, SPARX5_SWITCH_BAR);
	info->mem[0].size = pci_resource_len(dev, SPARX5_SWITCH_BAR);
	info->mem[0].memtype = UIO_MEM_PHYS;
	if (!info->mem[0].addr) {
		dev_err(&dev->dev, "Could not map region: %d\n",
			SPARX5_SWITCH_BAR);
		ret = -ENXIO;
		goto out_disable;
	}
	info->mem[0].internal_addr = ioremap(info->mem[0].addr,
					     info->mem[0].size);

	info->mem[1].name = "cpu_regs";  /* Used by MESA */
	info->mem[1].addr = pci_resource_start(dev, SPARX5_CPU_BAR);
	info->mem[1].size = pci_resource_len(dev, SPARX5_CPU_BAR);
	info->mem[1].memtype = UIO_MEM_PHYS;
	if (!info->mem[1].addr) {
		dev_err(&dev->dev, "Could not map region: %d\n",
			SPARX5_CPU_BAR);
		ret = -ENXIO;
		goto out_disable;
	}
	info->mem[1].internal_addr = ioremap(info->mem[1].addr,
					     info->mem[1].size);

	pci_set_drvdata(dev, info);
	info->name = UIO_NAME;
	info->version = UIO_VERSION;
	/* info->irq = dev->irq; */
	info->irq = UIO_IRQ_CUSTOM;
	info->irqcontrol = microchip_sparx5_mfd_irqcontrol;
	ret = uio_register_device(&dev->dev, info);
	if (ret) {
		if (ret == -EPROBE_DEFER) {
			dev_info(&dev->dev, "Defer UIO registration\n");
		} else {
			dev_warn(&dev->dev, "Could not register UIO driver: %d\n", ret);
		}
		goto out_disable;
	}

	ret = microchip_sparx5_mfd_irq_common_init(priv, dev->dev.of_node);
	if (ret) {
		dev_err(&dev->dev, "Could not configure irqs: %d\n", ret);
		goto out_unregister;
	}
	chip_id = microchip_sparx5_mfd_readl(priv, DEVCPU_GCB_CHIP_REGS_ID_OFF);
	dev_info(&dev->dev, "Found %s, UIO device, IRQ %ld, chip id 0x%08x\n",
		 info->name, info->irq, chip_id);
	pr_debug("%s:%d %s: Region: %d, size: %lluMB, %llx-%llx => %px\n",
		__FILE__, __LINE__, __func__,
		0,
		info->mem[0].size >> 20,
		info->mem[0].addr,
		info->mem[0].addr + info->mem[0].size - 1,
		info->mem[0].internal_addr);
	pr_debug("%s:%d %s: Region: %d, size: %lluMB, %llx-%llx => %px\n",
		__FILE__, __LINE__, __func__,
		1,
		info->mem[1].size >> 20,
		info->mem[1].addr,
		info->mem[1].addr + info->mem[1].size - 1,
		info->mem[1].internal_addr);

	/* Update PCIe PF0 BAR2 mask */
	{
		u32 dbi_access, orig;
		dbi_access = orig = microchip_sparx5_mfd_readl(priv, CPU_PCIE_CFG_OFF);
		dbi_access &= ~CPU_PCIE_CFG_DBI_ACCESS_ENA_M;
		dbi_access |= CPU_PCIE_CFG_DBI_ACCESS_ENA(3);
		microchip_sparx5_mfd_writel(priv, CPU_PCIE_PCIEMST_PF0_BAR2_MASK_LOW_OFF, 0xff000000);
		microchip_sparx5_mfd_writel(priv, CPU_PCIE_PCIEMST_PF0_BAR2_MASK_HIGH_OFF, 0xf);
		microchip_sparx5_mfd_writel(priv, CPU_PCIE_CFG_OFF, orig);
		pr_debug("%s:%d %s: Update PF0 BAR2 mask\n",
		__FILE__, __LINE__, __func__);
	}
	return of_platform_default_populate(dev->dev.of_node, NULL, &dev->dev);

out_unregister:
	uio_unregister_device(info);
out_disable:
	pci_disable_device(dev);
	return ret;
}

static void microchip_sparx5_mfd_remove(struct pci_dev *dev)
{
	struct uio_info *info = pci_get_drvdata(dev);

	pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);
	uio_unregister_device(info);
	iounmap(info->mem[0].internal_addr);
	iounmap(info->mem[1].internal_addr);
	pci_disable_device(dev);
	pci_set_drvdata(dev, NULL);
}

static struct pci_driver microchip_sparx5_pci_driver = {
	.name = DEVICE_NAME,
	.id_table = microchip_sparx5_ids,
	.probe = microchip_sparx5_mfd_probe,
	.remove = microchip_sparx5_mfd_remove,
};


module_pci_driver(microchip_sparx5_pci_driver);

MODULE_AUTHOR("Steen Hegelund <steen.hegelund@microchip.com>");
MODULE_DESCRIPTION("Microchip Sparx5 PCI/MFD driver");
MODULE_LICENSE("Dual MIT/GPL");
