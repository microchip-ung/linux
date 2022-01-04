// SPDX-License-Identifier: GPL-2.0
/*
 * Sparx5 SPI MUX driver
 *
 * Copyright (c) 2019 Microsemi Corporation
 *
 * Author: Lars Povlsen <lars.povlsen@microchip.com>
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/mux/driver.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/bitfield.h>

#define MSCC_IF_SI_OWNER_SISL			0
#define MSCC_IF_SI_OWNER_SIBM			1
#define MSCC_IF_SI_OWNER_SIMC			2

#define SPARX5_CPU_SYSTEM_CTRL_GENERAL_CTRL	0x88
#define SPARX5_IF_SI_OWNER			GENMASK(7, 6)
#define SPARX5_IF_SI2_OWNER			GENMASK(5, 4)

#define SPARX5_MAX_CS	16

struct mux_sparx5 {
	struct regmap *syscon;
	u8 bus[SPARX5_MAX_CS];
	int cur_bus;
};

/*
 * Set the owner of the SPI interfaces
 */
static void mux_sparx5_set_owner(struct regmap *syscon,
				 u8 owner, u8 owner2)
{
	u32 val, msk;

	val = FIELD_PREP(SPARX5_IF_SI_OWNER, owner) |
		FIELD_PREP(SPARX5_IF_SI2_OWNER, owner2);
	msk = SPARX5_IF_SI_OWNER | SPARX5_IF_SI2_OWNER;
	regmap_update_bits(syscon,
			   SPARX5_CPU_SYSTEM_CTRL_GENERAL_CTRL,
			   msk, val);
}

static void mux_sparx5_set_cs_owner(struct mux_sparx5 *mux_sparx5,
				    u8 cs, u8 owner)
{
	u8 other = (owner == MSCC_IF_SI_OWNER_SIBM ?
		    MSCC_IF_SI_OWNER_SIMC : MSCC_IF_SI_OWNER_SIBM);
	if (mux_sparx5->bus[cs])
		/* SPI2 */
		mux_sparx5_set_owner(mux_sparx5->syscon, other, owner);
	else
		/* SPI1 */
		mux_sparx5_set_owner(mux_sparx5->syscon, owner, other);
}

static int mux_sparx5_set(struct mux_control *mux, int state)
{
	struct mux_sparx5 *mux_sparx5 = mux_chip_priv(mux->chip);

	mux_sparx5_set_cs_owner(mux_sparx5, state, MSCC_IF_SI_OWNER_SIMC);

	return 0;
}

static const struct mux_control_ops mux_sparx5_ops = {
	.set = mux_sparx5_set,
};

static const struct of_device_id mux_sparx5_dt_ids[] = {
	{ .compatible = "microchip,sparx5-spi-mux", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mux_sparx5_dt_ids);

static int mux_sparx5_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mux_chip *mux_chip;
	struct mux_sparx5 *mux_sparx5;
	struct device_node *nc;
	const char *syscon_name = "microchip,sparx5-cpu-syscon";
	int ret;

	mux_chip = devm_mux_chip_alloc(dev, 1, sizeof(*mux_sparx5));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	mux_sparx5 = mux_chip_priv(mux_chip);
	mux_chip->ops = &mux_sparx5_ops;

	mux_sparx5->syscon =
		syscon_regmap_lookup_by_compatible(syscon_name);
	if (IS_ERR(mux_sparx5->syscon)) {
		dev_err(dev, "No syscon map %s\n", syscon_name);
		return PTR_ERR(mux_sparx5->syscon);
	}

	/* Get bus interface mapping */
	for_each_available_child_of_node(dev->of_node, nc) {
		u32 cs, bus;

		if (of_property_read_u32(nc, "reg", &cs) == 0 &&
		    cs < SPARX5_MAX_CS &&
		    of_property_read_u32(nc, "microchip,bus-interface",
					 &bus) == 0)
			mux_sparx5->bus[cs] = bus;
	}

	mux_chip->mux->states = SPARX5_MAX_CS;

	ret = devm_mux_chip_register(dev, mux_chip);
	if (ret < 0)
		return ret;

	dev_info(dev, "%u-way mux-controller registered\n",
		 mux_chip->mux->states);

	return 0;
}

static struct platform_driver mux_sparx5_driver = {
	.driver = {
		.name = "sparx5-mux",
		.of_match_table	= of_match_ptr(mux_sparx5_dt_ids),
	},
	.probe = mux_sparx5_probe,
};
module_platform_driver(mux_sparx5_driver);
