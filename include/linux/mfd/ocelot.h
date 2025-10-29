/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright 2022 Innovative Advantage Inc. */

#ifndef _LINUX_MFD_OCELOT_H
#define _LINUX_MFD_OCELOT_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/of.h>

struct resource;

static inline struct regmap *
ocelot_regmap_from_parent_optional(struct platform_device *pdev,
				   unsigned int index)
{
	struct device *dev = &pdev->dev;
	const char *name = NULL;
	struct resource *res;

	if (!dev->parent)
		return NULL;

	/* Use REG and getting the resource from the parent device, which is
	 * possible in an MFD configuration
	 */
	res = platform_get_resource(pdev, IORESOURCE_REG, index);
	if (res)
		return dev_get_regmap(dev->parent, res->name);

	/* Request regmap from parent based on reg-names property */
	if (!of_property_read_string_index(pdev->dev.of_node, "reg-names",
					   index, &name))
		return dev_get_regmap(dev->parent, name);

	return NULL;
}

static inline struct regmap *
ocelot_regmap_from_resource_optional(struct platform_device *pdev,
				     unsigned int index,
				     const struct regmap_config *config)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *regs;

	/*
	 * Don't use _get_and_ioremap_resource() here, since that will invoke
	 * prints of "invalid resource" which will simply add confusion.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (res) {
		regs = devm_ioremap_resource(dev, res);
		if (IS_ERR(regs))
			return ERR_CAST(regs);
		return devm_regmap_init_mmio(dev, regs, config);
	}

	return ocelot_regmap_from_parent_optional(pdev, index);
}

static inline struct regmap *
ocelot_regmap_from_resource(struct platform_device *pdev, unsigned int index,
			    const struct regmap_config *config)
{
	struct regmap *map;

	map = ocelot_regmap_from_resource_optional(pdev, index, config);
	return map ?: ERR_PTR(-ENOENT);
}

#endif
