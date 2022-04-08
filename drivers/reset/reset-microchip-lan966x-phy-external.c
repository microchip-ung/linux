// SPDX-License-Identifier: GPL-2.0+
#include <linux/gpio/consumer.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>

struct lan966x_phy_external_reset_context {
	struct gpio_desc *external_phy_ctrl;
	struct reset_controller_dev rcdev;
};

static int lan966x_phy_external_reset(struct reset_controller_dev *rcdev,
				      unsigned long id)
{
	struct lan966x_phy_external_reset_context *ctx =
		container_of(rcdev, struct lan966x_phy_external_reset_context, rcdev);

	gpiod_direction_output(ctx->external_phy_ctrl, 1);
	gpiod_set_value(ctx->external_phy_ctrl, 0);
	gpiod_set_value(ctx->external_phy_ctrl, 1);
	gpiod_set_value(ctx->external_phy_ctrl, 0);

	return 0;
}

static const struct reset_control_ops lan966x_phy_external_reset_ops = {
	.reset = lan966x_phy_external_reset,
};

static int lan966x_phy_external_reset_probe(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node;
	struct lan966x_phy_external_reset_context *ctx;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->external_phy_ctrl = devm_gpiod_get(&pdev->dev,
						"phy-external-reset",
						GPIOD_OUT_LOW);
	if (IS_ERR(ctx->external_phy_ctrl))
		return dev_err_probe(&pdev->dev, PTR_ERR(ctx->external_phy_ctrl),
				     "Could not get reset GPIO\n");

	ctx->rcdev.owner = THIS_MODULE;
	ctx->rcdev.nr_resets = 1;
	ctx->rcdev.ops = &lan966x_phy_external_reset_ops;
	ctx->rcdev.of_node = dn;

	return devm_reset_controller_register(&pdev->dev, &ctx->rcdev);
}

static const struct of_device_id lan966x_phy_external_reset_of_match[] = {
	{ .compatible = "microchip,lan966x-phy-reset-external", },
	{ }
};

static struct platform_driver lan966x_phy_external_reset_driver = {
	.probe = lan966x_phy_external_reset_probe,
	.driver = {
		.name = "lan966x-phy-reset-external",
		.of_match_table = lan966x_phy_external_reset_of_match,
	},
};

static int __init lan966x_phy_external_reset_init(void)
{
	return platform_driver_register(&lan966x_phy_external_reset_driver);
}
postcore_initcall(lan966x_phy_external_reset_init);
