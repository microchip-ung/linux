// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi Sparx5 UIO driver
 *
 * Author: <lars.povlsen@microchip.com>
 * License: Dual MIT/GPL
 * Copyright (c) 2019 Microchip Corporation
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>
#include <linux/delay.h>
#include <linux/of_platform.h>

#define DEVICE_NAME "sparx5"

#define PCIE_VENDOR_ID		0x101B
#define PCIE_DEVICE_SPARX5_ID	0xB006

struct uio_sparx5 {
    struct uio_info uio;
    /* Private data */
    spinlock_t lock;
    unsigned long flags;
    struct pci_dev *pdev;
};

static irqreturn_t sparx5_handler(int irq, struct uio_info *dev_info)
{
    struct uio_sparx5 *priv = dev_info->priv;

    //printk("Sparx5: Got IRQ, disabling\n");

    if (!test_and_set_bit(0, &priv->flags))
        disable_irq_nosync(irq);

    return IRQ_HANDLED;
}

static int sparx5_irqcontrol(struct uio_info *dev_info, s32 irq_on)
{
    struct uio_sparx5 *priv = dev_info->priv;
    unsigned long flags;

    spin_lock_irqsave(&priv->lock, flags);
    if (irq_on) {
        if (test_and_clear_bit(0, &priv->flags)) {
            //printk("Sparx5: Enable IRQ\n");
            enable_irq(dev_info->irq);
        }
    } else {
        if (!test_and_set_bit(0, &priv->flags)) {
            //printk("Sparx5: Disable IRQ\n");
            disable_irq(dev_info->irq);
        }
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    return 0;
}

static int sparx5_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct uio_sparx5 *priv;
	struct uio_info *info;
	void __iomem *chipid_reg;

	priv = kzalloc(sizeof(struct uio_sparx5), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	info = &priv->uio;
	info->priv = priv;

	if (pci_enable_device(dev))
		goto out_free;

	if (pci_request_regions(dev, DEVICE_NAME))
		goto out_disable;

	if (pci_resource_len(dev, 2) != SZ_1M) {
		dev_warn(&dev->dev, "Skipping non-sparx5 PCIe device\n");
		goto out_release;
	}

	/* BAR0 = registers, BAR1 = CONFIG, BAR2 = DDR (unused) */
	info->mem[0].addr = pci_resource_start(dev, 0);
	if (!info->mem[0].addr)
		goto out_release;
	info->mem[0].size = pci_resource_len(dev, 0);
	info->mem[0].memtype = UIO_MEM_PHYS;
	info->mem[0].internal_addr = ioremap(info->mem[0].addr, info->mem[0].size);
	info->mem[0].name = "switch_regs";

	info->mem[1].addr = pci_resource_start(dev, 2); /* BAR2! */
	if (!info->mem[1].addr)
		goto out_release;
	info->mem[1].size = pci_resource_len(dev, 2); /* BAR2! */
	info->mem[1].memtype = UIO_MEM_PHYS;
	info->mem[1].name = "cpu_regs";

	info->name = "mscc_switch";
	info->version = "1.0.0";
	info->irq = dev->irq;
	info->handler = sparx5_handler;
	info->irqcontrol = sparx5_irqcontrol;

	spin_lock_init(&priv->lock);
	priv->flags = 0; /* interrupt is enabled to begin with */
	priv->pdev = dev;

	if (uio_register_device(&dev->dev, info))
		goto out_unmap;

	pci_set_drvdata(dev, info);
	chipid_reg = info->mem[0].internal_addr + (0x01010000);
    dev_info(&dev->dev, "Found %s, UIO device - IRQ %ld, id 0x%08x.\n", info->name, info->irq, ioread32(chipid_reg));

    return 0;

out_unmap:
	dev_err(&dev->dev, "UIO register failed\n");
	iounmap(info->mem[0].internal_addr);
out_release:
	pci_release_regions(dev);
out_disable:
	pci_disable_device(dev);
out_free:
	kfree(info);
	return -ENODEV;
}


static void sparx5_pci_remove(struct pci_dev *dev)
{
    struct uio_info *info = pci_get_drvdata(dev);

    uio_unregister_device(info);
    iounmap(info->mem[0].internal_addr);
    pci_release_regions(dev);
    pci_disable_device(dev);
    pci_set_drvdata(dev, NULL);

    kfree(info->priv);
}

static struct pci_device_id sparx5_pci_ids[] = {
    {
        .vendor =    PCIE_VENDOR_ID,
        .device =    PCIE_DEVICE_SPARX5_ID,
        .subvendor = PCI_ANY_ID,
        .subdevice = PCI_ANY_ID,
    },
    { 0, }
};

static struct pci_driver sparx5_pci_driver = {
    .name = DEVICE_NAME,
    .id_table = sparx5_pci_ids,
    .probe = sparx5_pci_probe,
    .remove = sparx5_pci_remove,
};

static int __init sparx5_init_module(void)
{
    return pci_register_driver(&sparx5_pci_driver);
}

static void __exit sparx5_exit_module(void)
{
    pci_unregister_driver(&sparx5_pci_driver);
}

module_init(sparx5_init_module);
module_exit(sparx5_exit_module);

MODULE_DEVICE_TABLE(pci, sparx5_pci_ids);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microchip.com>");
