// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Atmel Flexcom
 *
 * Copyright (C) 2015 Atmel Corporation
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@atmel.com>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <dt-bindings/mfd/atmel-flexcom.h>

/* I/O register offsets */
#define FLEX_MR		0x0	/* Mode Register */
#define FLEX_VERSION	0xfc	/* Version Register */

/* Mode Register bit fields */
#define FLEX_MR_OPMODE_OFFSET	(0)  /* Operating Mode */
#define FLEX_MR_OPMODE_MASK	(0x3 << FLEX_MR_OPMODE_OFFSET)
#define FLEX_MR_OPMODE(opmode)	(((opmode) << FLEX_MR_OPMODE_OFFSET) &	\
				 FLEX_MR_OPMODE_MASK)

/* LAN966x flexcom shared register offsets */
#include <linux/reset.h>
#define FLEX_SHRD_SS_MASK_0	0x0
#define FLEX_SHRD_SS_MASK_1	0x4
#define FLEX_SHRD_PIN_MAX	20
#define FLEX_CS_MAX		1
#define FLEX_SHRD_MASK		GENMASK(20, 0)

struct atmel_flex_caps {
	bool has_flx_cs;
};

struct atmel_flexcom {
	void __iomem *base;
	void __iomem *flexcom_shared_base;
	u32 opmode;
	struct clk *clk;
};

static int atmel_flexcom_lan966x_cs_config(struct platform_device *pdev)
{
	struct atmel_flexcom *ddata = dev_get_drvdata(&pdev->dev);
	struct device_node *np = pdev->dev.of_node;
	struct reset_control *reset;
	u32 flx_shrd_pins[2], flx_cs[2], val;
	int err, i, count;

	reset = devm_reset_control_get_shared(&pdev->dev, "switch");
	if (!IS_ERR(reset))
		reset_control_reset(reset);

	count = of_property_count_u32_elems(np, "microchip,flx-shrd-pins");
	if (count <= 0 || count > 2) {
		dev_err(&pdev->dev, "Invalid %s property (%d)\n", "flx-shrd-pins",
				count);
		return -EINVAL;
	}

	err = of_property_read_u32_array(np, "microchip,flx-shrd-pins", flx_shrd_pins, count);
	if (err)
		return err;

	err = of_property_read_u32_array(np, "microchip,flx-cs", flx_cs, count);
	if (err)
		return err;

	for (i = 0; i < count; i++) {
		if (flx_shrd_pins[i] > FLEX_SHRD_PIN_MAX)
			return -EINVAL;

		if (flx_cs[i] > FLEX_CS_MAX)
			return -EINVAL;

		val = ~(1 << flx_shrd_pins[i]) & FLEX_SHRD_MASK;

		if (flx_cs[i] == 0)
			writel(val, ddata->flexcom_shared_base + FLEX_SHRD_SS_MASK_0);
		else
			writel(val, ddata->flexcom_shared_base + FLEX_SHRD_SS_MASK_1);
	}

	return 0;
}

static int atmel_flexcom_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct atmel_flex_caps *caps;
	struct atmel_flexcom *ddata;
	int err;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);

	err = of_property_read_u32(np, "atmel,flexcom-mode", &ddata->opmode);
	if (err)
		return err;

	if (ddata->opmode < ATMEL_FLEXCOM_MODE_USART ||
	    ddata->opmode > ATMEL_FLEXCOM_MODE_TWI)
		return -EINVAL;

	ddata->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(ddata->base))
		return PTR_ERR(ddata->base);

	ddata->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(ddata->clk))
		return PTR_ERR(ddata->clk);

	err = clk_prepare_enable(ddata->clk);
	if (err)
		return err;

	/*
	 * Set the Operating Mode in the Mode Register: only the selected device
	 * is clocked. Hence, registers of the other serial devices remain
	 * inaccessible and are read as zero. Also the external I/O lines of the
	 * Flexcom are muxed to reach the selected device.
	 */
	writel(FLEX_MR_OPMODE(ddata->opmode), ddata->base + FLEX_MR);

	caps = of_device_get_match_data(&pdev->dev);
	if (!caps) {
		dev_err(&pdev->dev, "Could not retrieve flexcom caps\n");
		err = -EINVAL;
		goto clk_disable;
	}

	if (caps->has_flx_cs) {
		ddata->flexcom_shared_base = devm_platform_get_and_ioremap_resource(pdev, 1, NULL);
		if (IS_ERR(ddata->flexcom_shared_base)) {
			err = dev_err_probe(&pdev->dev,
					PTR_ERR(ddata->flexcom_shared_base),
					"failed to get flexcom shared base address\n");
			goto clk_disable;
		}

		err = atmel_flexcom_lan966x_cs_config(pdev);
		if (err)
			goto clk_disable;
	}

clk_disable:
	clk_disable_unprepare(ddata->clk);
	if (err)
		return err;

	return devm_of_platform_populate(&pdev->dev);
}

static const struct atmel_flex_caps atmel_flexcom_caps = {};

static const struct atmel_flex_caps lan966x_flexcom_caps = {
	.has_flx_cs = true,
};

static const struct of_device_id atmel_flexcom_of_match[] = {
	{
		.compatible = "atmel,sama5d2-flexcom",
		.data = &atmel_flexcom_caps,
	},

	{
		.compatible = "microchip,lan966x-flexcom",
		.data = &lan966x_flexcom_caps,
	},

	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, atmel_flexcom_of_match);

static int __maybe_unused atmel_flexcom_resume_noirq(struct device *dev)
{
	struct atmel_flexcom *ddata = dev_get_drvdata(dev);
	int err;
	u32 val;

	err = clk_prepare_enable(ddata->clk);
	if (err)
		return err;

	val = FLEX_MR_OPMODE(ddata->opmode),
	writel(val, ddata->base + FLEX_MR);

	clk_disable_unprepare(ddata->clk);

	return 0;
}

static const struct dev_pm_ops __maybe_unused atmel_flexcom_pm_ops = {
	.resume_noirq = atmel_flexcom_resume_noirq,
};

static struct platform_driver atmel_flexcom_driver = {
	.probe	= atmel_flexcom_probe,
	.driver	= {
		.name		= "atmel_flexcom",
		.pm		= pm_ptr(&atmel_flexcom_pm_ops),
		.of_match_table	= atmel_flexcom_of_match,
	},
};

module_platform_driver(atmel_flexcom_driver);

MODULE_AUTHOR("Cyrille Pitchen <cyrille.pitchen@atmel.com>");
MODULE_DESCRIPTION("Atmel Flexcom MFD driver");
MODULE_LICENSE("GPL v2");
