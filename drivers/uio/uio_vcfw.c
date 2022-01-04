/*
 * VCoreIII firmware buffer interface
 *
 * Copyright (C) 2016 Microsemi Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>

#define DRV_NAME "vcfw_uio"
#define DRV_VERSION "1.0"

static const struct of_device_id vcfw_match[] = {
	{ .compatible = "mscc,vcfw_uio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, microchip_sparx5_fdma_match);

struct uio_vcfw_dev {
	struct uio_info *info;
	void __iomem *vcfwio_vaddr;
};

static void vcfw_cleanup(struct platform_device *dev,
                         struct uio_info *info)
{
	iounmap(info->mem[0].internal_addr);
	kfree(info);
}

static int vcfw_probe(struct platform_device *dev)
{
	struct uio_info *info;
	struct resource *regs_vcfwio;
	int ret = -ENODEV;

	info = kzalloc(sizeof(struct uio_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	regs_vcfwio = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (!regs_vcfwio) {
                dev_info(&dev->dev, "No firmware I/O resource specified, not registering\n");
		goto out_free;
	}

	if (!regs_vcfwio->start) {
		dev_info(&dev->dev, "Null memory resource, not registering\n");
		goto out_free;
	}

        info->mem[0].internal_addr = (void *)regs_vcfwio->start;
        info->mem[0].addr = virt_to_phys((void *)regs_vcfwio->start);
        info->mem[0].size = resource_size(regs_vcfwio);
        info->mem[0].memtype = UIO_MEM_PHYS;

        info->name = DRV_NAME;
        info->version = DRV_VERSION;

        ret = uio_register_device(&dev->dev, info);
        if (ret < 0)
                goto out_free;

	platform_set_drvdata(dev, info);
        dev_info(&dev->dev, "UIO map: %pR\n", regs_vcfwio);
	return 0;

out_free:
	vcfw_cleanup(dev, info);
	return ret;
}

static int vcfw_remove(struct platform_device *dev)
{
	struct uio_info *info = platform_get_drvdata(dev);

	vcfw_cleanup(dev, info);
	return 0;
}

static struct platform_driver vcfw_driver = {
	.probe = vcfw_probe,
	.remove = vcfw_remove,
	.driver = {
		   .name = DRV_NAME,
		   .owner = THIS_MODULE,
		    .of_match_table = of_match_ptr(vcfw_match),
		   },
};

module_platform_driver(vcfw_driver);

MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microsemi.com>");
