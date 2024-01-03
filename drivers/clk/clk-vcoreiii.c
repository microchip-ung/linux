// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Microchip VCOREIII SoC Clock driver.
 *
 * Copyright (c) 2023 Microchip Inc.
 *
 * Author: Lars Povlsen <lars.povlsen@microchip.com>
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/clk-provider.h>
#include <linux/bitfield.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define CPU_CLK_DIV		GENMASK(11, 6)

struct vc3_hw_clk {
	struct clk_hw hw;
	void __iomem *base;
};

#define to_vc3_pll(hw) container_of(hw, struct vc3_hw_clk, hw)

static int vc3_pll_enable(struct clk_hw *hw)
{
	return 0;
}

static void vc3_pll_disable(struct clk_hw *hw)
{
}

static unsigned long vc3_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct vc3_hw_clk *pll = to_vc3_pll(hw);
	u32 val, w, div;

	w = readl(pll->base);
	div = FIELD_GET(CPU_CLK_DIV, w);
#define MHZ(n)          ((n) * 1000 * 1000)
	switch (div) {
	case 2:
		val = MHZ(500);
		break;
	case 5:
		val = MHZ(250);
		break;
	case 14:
		val = MHZ(312.5);
		break;
	case 15:
		val = MHZ(166.666667);
		break;
	default:
		pr_warn("%s: Invalid CPU_CLK_DIV: %d\n", clk_hw_get_name(hw), div);
		fallthrough;
	case 6:
		/* Default value */
		val = MHZ(416.666667);
		break;
	}
#undef MHZ

	pr_info("%s: Running @ %d MHz\n", clk_hw_get_name(hw), val / 1000000);

	return val;
}

static const struct clk_ops vc3_pll_ops = {
	.enable		= vc3_pll_enable,
	.disable	= vc3_pll_disable,
	.recalc_rate	= vc3_pll_recalc_rate,
};

static int vc3_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vc3_hw_clk *vc3_clk;
	struct clk_init_data init = {
		.name = "clk-vcoreiii",
		.ops = &vc3_pll_ops,
		.num_parents = 0,
	};
	int ret;

	vc3_clk = devm_kzalloc(dev, sizeof(*vc3_clk), GFP_KERNEL);
	if (!vc3_clk)
		return -ENOMEM;

	vc3_clk->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vc3_clk->base)) {
		dev_err(dev, "%s: failed to get register base\n", init.name);
		return PTR_ERR(vc3_clk->base);
	}

	vc3_clk->hw.init = &init;
	ret = devm_clk_hw_register(dev, &vc3_clk->hw);
	if (ret) {
		dev_err(dev, "%s: failed to register clock\n", init.name);
		return ret;
	}

	dev_info(dev, "Registered %s clock\n", init.name);

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get, &vc3_clk->hw);
}

static const struct of_device_id vc3_clk_dt_ids[] = {
	{ .compatible = "microchip,vcoreiii-servalt-pll", },
	{ }
};
MODULE_DEVICE_TABLE(of, vc3_clk_dt_ids);

static struct platform_driver vc3_clk_driver = {
	.probe  = vc3_clk_probe,
	.driver = {
		.name = "vcoreiii-clk",
		.of_match_table = vc3_clk_dt_ids,
	},
};
builtin_platform_driver(vc3_clk_driver);
