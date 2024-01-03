// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Microchip UNG
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/mfd/core.h>
#include <linux/pci.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/libfdt.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/machine.h>
#include <linux/property.h>
#include <linux/pci_ids.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>

#include "lan966x_pci_of.h"
#include "lan966x_pci_irq.h"

#define DTB_EXTRA_SPACE			200

#define PCI_DEVICE_ID_MCHP		0x1055
#define PCI_DEVICE_ID_MCHP_LAN966X	0x9660

#define CPU_RESET_PROT_STAT_OFFSET	0x88
#define CPU_TARGET_OFFSET		0xc0000
#define CPU_TARGET_LENGTH		0x10000

#define LAN966X_BAR_CPU_OFFSET		0xe2000000
#define LAN966X_BAR_CPU_SIZE		SZ_32M
#define LAN966X_BAR_AMBA_OFFSET		0xe0000000
#define LAN966X_BAR_AMBA_SIZE		SZ_16M

#define LAN966X_DEV_ADDR(__res)	(__res ## _ADDR)
#define LAN966X_DEV_END(__res)	(LAN966X_DEV_ADDR(__res) + (__res ## _SIZE) - 1)
#define LAN966X_DEV_LEN(__res)	(__res ## _SIZE)

extern char __dtb_lan966x_pci_begin[];
extern char __dtb_lan966x_pci_end[];

static int lan966x_of_add_property(struct of_changeset *ocs,
				   struct device_node *np, const char *name,
				   const void *value, int length)
{
	int ret;
	struct property *prop;

	prop = of_property_alloc(name, value, length, GFP_KERNEL);
	if (!prop)
		return -ENOMEM;

	ret = of_changeset_add_property(ocs, np, prop);
	if (ret)
		of_property_free(prop);

	return ret;
}

static int of_bar_remap(struct pci_dev *pci_dev, int bar, u64 offset, int val[7])
{
	u64 start, size;
	u32 flags;

	start = pci_resource_start(pci_dev, bar);
	if (!start)
		return -EINVAL;

	flags = pci_resource_flags(pci_dev, bar);
	if (!(flags & IORESOURCE_MEM))
		return -EINVAL;

	/* Bus address */
	val[0] = __cpu_to_be32(offset);

	/* PCI bus address */
	val[1] = __cpu_to_be32(0x2 << 24);
	val[2] = __cpu_to_be32(start << 32);
	val[3] = __cpu_to_be32(start & 0xFFFFFFFF);

	/* Size */
	size = pci_resource_len(pci_dev, bar);
	val[4] = __cpu_to_be32(size & 0xFFFFFFFF);

	return 0;
}

static int lan966x_pci_load_overlay(struct lan966x_pci *data)
{
	u32 dtbo_size = __dtb_lan966x_pci_end - __dtb_lan966x_pci_begin;
	void *dtbo_start = __dtb_lan966x_pci_begin;
	__be32 val[LAN966X_BAR_COUNT][5];
	static struct of_changeset cs;
	int ret;

	ret = of_overlay_fdt_apply(dtbo_start, dtbo_size, &data->ovcs_id,
				   data->dev->of_node);
	if (ret)
		return ret;

	of_bar_remap(data->pci_dev, LAN966X_BAR_CPU, LAN966X_BAR_CPU_OFFSET,
		     val[0]);
	of_bar_remap(data->pci_dev, LAN966X_BAR_AMBA, LAN966X_BAR_AMBA_OFFSET,
		     val[1]);

	of_changeset_init(&cs);

	lan966x_of_add_property(&cs, data->dev->of_node, "ranges", val,
				sizeof(val));
	of_changeset_apply(&cs);

	return ret;
}

static int lan966x_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct device_node *itc_node;
	struct lan966x_pci *data;
	int ret;

	if (!dev->of_node) {
		dev_err(dev, "Missing of_node for device\n");
		return -EINVAL;
	}

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(dev, data);
	data->dev = dev;
	data->pci_dev = pdev;

	ret = lan966x_pci_load_overlay(data);
	if (ret)
		return ret;

	itc_node = of_get_child_by_name(dev->of_node, "itc");
	if (!itc_node) {
		ret = -EINVAL;
		goto overlay_remove;
	}

	ret = lan966x_pci_irq_setup(pdev, itc_node, &data->irq_data);
	if (ret)
		goto overlay_remove;

	return of_platform_default_populate(dev->of_node, NULL, dev);

overlay_remove:
	of_overlay_remove(&data->ovcs_id);

	return ret;
}

static void lan966x_pci_remove(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;
	struct lan966x_pci *data = dev_get_drvdata(dev);

	if (data->ovcs_id)
		of_overlay_remove(&data->ovcs_id);
}

/* PCI device */
static struct pci_device_id lan966x_ids[] = {
	{ PCI_DEVICE(PCI_DEVICE_ID_MCHP, PCI_DEVICE_ID_MCHP_LAN966X) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, lan966x_ids);

static struct pci_driver lan966x_pci_driver = {
	.name = "mchp_lan966x",
	.id_table = lan966x_ids,
	.probe = lan966x_pci_probe,
	.remove = lan966x_pci_remove,
};

module_pci_driver(lan966x_pci_driver);

MODULE_DESCRIPTION("Maserati PCI driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Clément Léger <clement.leger@bootlin.com>");
