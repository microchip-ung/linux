// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * License: Dual MIT/GPL
 * Copyright (c) 2020 Microchip Corporation
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

static const char *cpu_syscon = "microchip,lan966x-cpu-syscon";
static const char *gcb_syscon = "microchip,lan966x-switch-syscon";

struct lan966x_reset_context {
	struct regmap *gcb_ctrl;
	struct regmap *cpu_ctrl;
	struct notifier_block restart_handler;
};

#define PROTECT_REG    0x88
#define PROTECT_BIT    BIT(5)
#define SOFT_RESET_REG 0x00
#define SOFT_RESET_BIT BIT(1)

static int lan966x_restart_handle(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	struct lan966x_reset_context *ctx = container_of(this, struct lan966x_reset_context,
							restart_handler);

	/* Make sure the core is not protected from reset */
	regmap_update_bits(ctx->cpu_ctrl, PROTECT_REG, PROTECT_BIT, 0);

	pr_emerg("Resetting SoC\n");

	regmap_write(ctx->gcb_ctrl, SOFT_RESET_REG, SOFT_RESET_BIT);

	pr_emerg("Unable to restart system\n");
	return NOTIFY_DONE;
}

static int lan966x_reset_probe(struct platform_device *pdev)
{
	struct lan966x_reset_context *ctx;
	struct device *dev = &pdev->dev;
	int err;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->gcb_ctrl = syscon_regmap_lookup_by_compatible(gcb_syscon);
	if (IS_ERR(ctx->gcb_ctrl)) {
		dev_err(dev, "No gcb_syscon map: %s\n", gcb_syscon);
		return PTR_ERR(ctx->gcb_ctrl);
	}

	ctx->cpu_ctrl = syscon_regmap_lookup_by_compatible(cpu_syscon);
	if (IS_ERR(ctx->cpu_ctrl)) {
		dev_err(dev, "No cpu_syscon map: %s\n", cpu_syscon);
		return PTR_ERR(ctx->cpu_ctrl);
	}

	ctx->restart_handler.notifier_call = lan966x_restart_handle;
	ctx->restart_handler.priority = 192;
	err = register_restart_handler(&ctx->restart_handler);
	if (err)
		dev_err(dev, "can't register restart notifier (err=%d)\n", err);

	return err;
}

static const struct of_device_id lan966x_reset_of_match[] = {
	{ .compatible = "microchip,lan966x-chip-reset", },
	{ /*sentinel*/ }
};

static struct platform_driver lan966x_reset_driver = {
	.probe = lan966x_reset_probe,
	.driver = {
		.name = "lan966x-chip-reset",
		.of_match_table = lan966x_reset_of_match,
	},
};
builtin_platform_driver(lan966x_reset_driver);
