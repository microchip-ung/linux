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
#include <linux/uio_driver.h>

#include "lan969x_pci_regs.h"

#define LAN_OFFSET_(id, tinst, tcnt,			\
		    gbase, ginst, gcnt, gwidth,		\
		    raddr, rinst, rcnt, rwidth)		\
	gbase + ((ginst) * gwidth) + raddr + ((rinst * rwidth))
#define LAN_OFFSET(...) LAN_OFFSET_(__VA_ARGS__)

#define PCI_VENDOR_ID_MCHP		0x1055
#define PCI_DEVICE_ID_MCHP_LAN969X	0x9690

#define LAN969X_CSR_BAR		0
#define CSR_TARGET_OFFSET	(0x10000)
#define CSR_TARGET_LENGTH	(0x10000)

#define LAN969X_CPU_BAR		1
#define CPU_TARGET_OFFSET	(0xc0000)
#define CPU_TARGET_LENGTH	(0x10000)

#define LAN969X_NR_IRQ		121

#define IRQ_XTR_RDY		10
#define IRQ_GPIO		15
#define IRQ_SGPIO		16
#define IRQ_FLEXCOM3		49
#define IRQ_FDMA_LEGACY		88

#define LAN969X_UIO_VERSION	"1.0.0"

static struct pci_device_id lan969x_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MCHP, PCI_DEVICE_ID_MCHP_LAN969X) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, lan969x_ids);

struct lan969x_priv {
	struct uio_info switch_uio;
	struct uio_info cpu_uio;
	struct pci_dev *pdev;
};

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

/*
 * LAN969x level encoding: this is a 2 bit value with
 * - LSB in CPU:INTR:INTR_TRIGGER*[0].INTR_TRIGGER
 * - MSB in CPU:INTR:INTR_TRIGGER*[1].INTR_TRIGGER
 * 0: Interrupt is level-activated
 * 1: Interrupt is edge-triggered
 * 2: Interrupt is falling-edge-triggered
 * 3: Interrupt is rising-edge-triggered
 */
static int lan969x_irq_set_type(struct irq_data *data, unsigned int flow_type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	int chip_hwirq = data->hwirq % 32;
	u32 new_lsb = 0, new_msb = 0;
	u32 lsb, msb;

	switch (flow_type) {
	case IRQ_TYPE_NONE:
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
	case IRQ_TYPE_LEVEL_LOW:
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

	irqd_set_trigger_type(data, flow_type);
	if (flow_type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		irq_set_handler_locked(data, handle_level_irq);
	else
		irq_set_handler_locked(data, handle_edge_irq);
	return IRQ_SET_MASK_OK;
}

static void lan969x_irq_handler_domain(struct irq_domain *dom,
				       struct irq_chip *chip,
				       struct irq_desc *desc,
				       u32 first_irq)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(dom, first_irq);
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

		generic_handle_irq(irq_find_mapping(dom, hwirq + first_irq));
		reg &= ~(BIT(hwirq));
	}

	val = irq_reg_readl(gc, gc->chip_types[0].regs.enable);
	irq_reg_writel(gc, 0, gc->chip_types[0].regs.enable);
	irq_reg_writel(gc, val, gc->chip_types[0].regs.enable);

	chained_irq_exit(chip, desc);
}

static void lan969x_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *dom = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	lan969x_irq_handler_domain(dom, chip, desc, 0);
	lan969x_irq_handler_domain(dom, chip, desc, 32);
	lan969x_irq_handler_domain(dom, chip, desc, 64);
	lan969x_irq_handler_domain(dom, chip, desc, 96);
}

static void lan969x_config_irqchip(struct irq_chip_generic *gc,
				   void __iomem *regs,
				   dma_addr_t acknowledge,
				   dma_addr_t atomic_disable,
				   dma_addr_t atomic_enable,
				   dma_addr_t identity,
				   dma_addr_t trigger,
				   dma_addr_t map,
				   u32 mask)
{
	gc->reg_base = regs;
	gc->chip_types[0].regs.ack = acknowledge;
	gc->chip_types[0].regs.mask = atomic_disable;
	gc->chip_types[0].regs.enable = atomic_enable;
	gc->chip_types[0].regs.type = identity;
	gc->chip_types[0].regs.polarity = trigger;
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_unmask = lan969x_irq_unmask;
	gc->chip_types[0].chip.irq_set_type = lan969x_irq_set_type;
	gc->chip_types[0].mask_cache = &gc->mask_cache;
	gc->mask_cache = mask;

	/* Mask and ack interrupts */
	irq_reg_writel(gc, 0, atomic_enable);
	irq_reg_writel(gc, mask, acknowledge);
	irq_reg_writel(gc, mask, map);
}

static int lan969x_irq_common_init(struct pci_dev *pdev, void __iomem *regs,
				   int size)
{
	struct device_node *node = pdev->dev.of_node;
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


	/* Configure first domain (irq 0-31) */
	lan969x_config_irqchip(irq_get_domain_generic_chip(domain, 0),
			       regs,
			       LAN_OFFSET(CPU_INTR_STICKY),
			       LAN_OFFSET(CPU_INTR_ENA_CLR),
			       LAN_OFFSET(CPU_INTR_ENA_SET),
			       LAN_OFFSET(CPU_INTR_IDENT),
			       LAN_OFFSET(CPU_INTR_TRIGGER(0)),
			       LAN_OFFSET(CPU_DST_INTR_MAP(0)),
			       BIT(IRQ_SGPIO) | BIT(IRQ_GPIO) |
			       BIT(IRQ_XTR_RDY));

	/* Configure second domain (irq 32-63) */
	lan969x_config_irqchip(irq_get_domain_generic_chip(domain, 32),
			       regs,
			       LAN_OFFSET(CPU_INTR_STICKY1),
			       LAN_OFFSET(CPU_INTR_ENA_CLR1),
			       LAN_OFFSET(CPU_INTR_ENA_SET1),
			       LAN_OFFSET(CPU_INTR_IDENT1),
			       LAN_OFFSET(CPU_INTR_TRIGGER1(0)),
			       LAN_OFFSET(CPU_DST_INTR_MAP1(0)),
			       BIT(IRQ_FLEXCOM3 - 32));

	/* Configure third domain (irq 64-95) */
	lan969x_config_irqchip(irq_get_domain_generic_chip(domain, 64),
			       regs,
			       LAN_OFFSET(CPU_INTR_STICKY2),
			       LAN_OFFSET(CPU_INTR_ENA_CLR2),
			       LAN_OFFSET(CPU_INTR_ENA_SET2),
			       LAN_OFFSET(CPU_INTR_IDENT2),
			       LAN_OFFSET(CPU_INTR_TRIGGER2(0)),
			       LAN_OFFSET(CPU_DST_INTR_MAP2(0)),
			       BIT(IRQ_FDMA_LEGACY - 64));

	/* Configure fourth domain (irq 96-127) */
	lan969x_config_irqchip(irq_get_domain_generic_chip(domain, 96),
			       regs,
			       LAN_OFFSET(CPU_INTR_STICKY3),
			       LAN_OFFSET(CPU_INTR_ENA_CLR3),
			       LAN_OFFSET(CPU_INTR_ENA_SET3),
			       LAN_OFFSET(CPU_INTR_IDENT3),
			       LAN_OFFSET(CPU_INTR_TRIGGER3(0)),
			       LAN_OFFSET(CPU_DST_INTR_MAP3(0)),
			       0);

	irq_set_chained_handler_and_data(pdev->irq, lan969x_irq_handler, domain);

	return 0;

err_domain_remove:
	irq_domain_remove(domain);

	return ret;
}

static int lan969x_uio_bar(struct pci_dev *pdev, struct uio_info *uio,
			   const char *name, int bar)
{
	int err;

	uio->mem[0].name = name;
	uio->mem[0].addr = pci_resource_start(pdev, bar);
	uio->mem[0].size = pci_resource_len(pdev, bar);
	uio->mem[0].memtype = UIO_MEM_PHYS;
	uio->mem[0].internal_addr = devm_ioremap(&pdev->dev, uio->mem[0].addr,
						 uio->mem[0].size);
	uio->name = name;
	uio->version = LAN969X_UIO_VERSION;
	err = devm_uio_register_device(&pdev->dev, uio);
	if (err)
		if (err != -EPROBE_DEFER)
			dev_warn(&pdev->dev,
				 "Could not register UIO driver for %s: %d\n",
				 name, err);
	return err;
}

static int lan969x_uio_init(struct pci_dev *pdev)
{
	struct lan969x_priv *priv;
	struct uio_info *uio;
	int err;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = pdev;
	uio = &priv->switch_uio;
	uio->priv = priv;

	err = lan969x_uio_bar(pdev, uio, "mscc_switch", LAN969X_CSR_BAR);
	if (err)
		return err;

	uio = &priv->cpu_uio;
	uio->priv = priv;
	return lan969x_uio_bar(pdev, uio, "mscc_cpu", LAN969X_CPU_BAR);
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

	ret = lan969x_uio_init(pdev);
	if (ret)
		return ret;

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
