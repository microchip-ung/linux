// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Microchip Ocelot
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2019 Microchip Corporation
 */
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

#define PCI_VENDOR_ID_MSCC		0x101b
#define PCI_DEVICE_ID_MSCC_OCELOT	0xb005

static struct pci_device_id ocelot_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MSCC, PCI_DEVICE_ID_MSCC_OCELOT) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, ocelot_ids);

static int ocelot_pci_probe(struct pci_dev *pdev,
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

	/* Enable interrupts */
	pci_set_master(pdev);

	return of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
}

static void ocelot_pci_remove(struct pci_dev *pdev)
{
}

static struct pci_driver ocelot_pci_driver = {
	.name = "microsemi_ocelot_pci",
	.id_table = ocelot_ids,
	.probe = ocelot_pci_probe,
	.remove = ocelot_pci_remove,
};

module_pci_driver(ocelot_pci_driver);

MODULE_DESCRIPTION("Microsemi Ocelot driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
