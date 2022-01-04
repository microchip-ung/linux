// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi SoCs parallel NAND driver
 *
 * Author: <lars.povlsen@microchip.com>
 *
 * Copyright (c) 2018 Microsemi Corporation
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/resource.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <linux/io.h>

#define setmasked(_addr, _m, _v) writel((readl(_addr) & ~(_m)) | (_v), (_addr))
#define NAND_MANUFACTURER_MICRON     0x2c
#define MT29F2G08                    0xda
#define MT29F2G08ABAGAWP             0x90

struct mscc_nand_prop {
	const char *syscon_name;
	u32 general_ctrl_off;
	u32 pi_enable_bit_off;
	u32 ale_mask;
	u32 cle_mask;
};

static const struct mscc_nand_prop luton_props = {
	.syscon_name		= "mscc,ocelot-cpu-syscon",
	.general_ctrl_off	= 0x24,
	.pi_enable_bit_off	= 1,
	.ale_mask = BIT(2),
	.cle_mask = BIT(3),
};

static const struct mscc_nand_prop jaguar2_props = {
	.syscon_name		= "mscc,jaguar2-cpu-syscon",
	.general_ctrl_off	= 0x24,
	.pi_enable_bit_off	= 9,
	.ale_mask = BIT(2),
	.cle_mask = BIT(3),
};

struct mscc_nand_data {
	struct nand_chip		nand;
	struct regmap       		*syscon;
	void __iomem			*pi_region;
	void __iomem			*pi_mst_ctrl;
	const struct mscc_nand_prop	*props;
};

static inline struct mscc_nand_data *mtd_to_mscc(struct mtd_info *mtd)
{
	return container_of(mtd_to_nand(mtd), struct mscc_nand_data, nand);
}

/* hardware specific access to control-lines */
static void vcoreiii_nand_cmd_ctrl(struct nand_chip *this, int cmd,
				   unsigned int ctrl)
{
	struct mtd_info *mtd = nand_to_mtd(this);
	if (ctrl & NAND_CTRL_CHANGE) {
		struct mscc_nand_data *host = mtd_to_mscc(mtd);
		u32 ioaddr = (u32) this->legacy.IO_ADDR_R;
		if(ctrl & NAND_CLE) {
			ioaddr |= host->props->cle_mask;
		} else if(ctrl & NAND_ALE) {
			ioaddr |= host->props->ale_mask;
		}
		this->legacy.IO_ADDR_W = (void __iomem *)ioaddr;
	}
	if (cmd != NAND_CMD_NONE) {
		__raw_writeb(cmd, this->legacy.IO_ADDR_W);
		wmb();
	}
}

/* Hook into the NAND detection and override the default Device Tree
 * configuration for the Micron NAND device on Luton26
 * model: MT29F2G08ABAGAWP id: 2c da 90 95 86
 * model: MT29F2G08AAD     id: 2c da 80 95 50
 */
static int vcoreiii_nand_attach_chip(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct device *dev = mtd->dev.parent;

	dev_info(dev, "NAND id: %*ph\n", chip->id.len, chip->id.data);
	if (chip->id.len > 3 && (chip->id.data[0] == NAND_MANUFACTURER_MICRON
				 && chip->id.data[1] == MT29F2G08
				 && chip->id.data[2] == MT29F2G08ABAGAWP)) {
		dev_info(dev, "on-die ECC chosen\n");
		chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_DIE;
	}
	return 0;
}

static const struct nand_controller_ops vcoreiii_nand_controller_ops = {
	.attach_chip = vcoreiii_nand_attach_chip,
};

/*
 * mscc_nand_probe - Probe function
 * @pdev:       platform device structure
 */
static int __init mscc_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mscc_nand_data *host;
	struct mtd_info *mtd;
	struct nand_chip *nand;
	struct resource *regs;
	const struct mscc_nand_prop *props;
	u32 val;
	int ret = 0;

	/* Allocate memory for the device structure (and zero it) */
	host = devm_kzalloc(&pdev->dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	nand = &host->nand;

	/* Chip PI region */
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host->pi_region = devm_ioremap_resource(dev, regs);
	if (IS_ERR(host->pi_region))
		return PTR_ERR(host->pi_region);
	/* Chip PI_MST registers */
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	host->pi_mst_ctrl = devm_ioremap_resource(dev, regs);
	if (IS_ERR(host->pi_mst_ctrl))
		return PTR_ERR(host->pi_mst_ctrl);

	host->props = props = of_device_get_match_data(dev);
	host->syscon = syscon_regmap_lookup_by_compatible(props->syscon_name);
	if (IS_ERR(host->syscon))
		return PTR_ERR(host->syscon);

	/* Enable PI */
	regmap_update_bits(host->syscon, props->general_ctrl_off,
			   BIT(props->pi_enable_bit_off),
			   BIT(props->pi_enable_bit_off));
	if (!of_property_read_u32(pdev->dev.of_node,
				  "waitcc", &val)) {
		/* Set waitcc = bits [14;7] */
		setmasked(host->pi_mst_ctrl, GENMASK(14, 7), val << 7);
		dev_dbg(dev, "Waitcc = %d\n", val);
	}

	/* Link all private pointers */
	mtd = nand_to_mtd(&host->nand);
	nand_set_controller_data(nand, host);
	nand_set_flash_node(nand, pdev->dev.of_node);
	mtd->dev.parent = &pdev->dev;

	nand->legacy.IO_ADDR_R = nand->legacy.IO_ADDR_W = host->pi_region;
	nand->legacy.cmd_ctrl = vcoreiii_nand_cmd_ctrl;
	nand->ecc.engine_type = NAND_ECC_ENGINE_TYPE_SOFT;
	nand->ecc.algo = NAND_ECC_ALGO_HAMMING;
	nand->parameters.supports_set_get_features = true;

	if (!of_property_read_u32(pdev->dev.of_node,
				  "chip-delay", &val)) {
		nand->legacy.chip_delay = val;
		dev_dbg(dev, "Chip Delay = %d\n", nand->legacy.chip_delay);
	}

	/*
	 * Scan to find existence of the device
	 */
	nand->legacy.dummy_controller.ops = &vcoreiii_nand_controller_ops;
	ret = nand_scan(nand, 1);
	if (ret)
		goto cleanup_nand;

	mtd->name = "pi_nand";
	ret = mtd_device_register(mtd, NULL, 0);
	if (ret)
		goto cleanup_nand;

	platform_set_drvdata(pdev, host);
	dev_info(dev, "MSCC NAND driver registration successful\n");

	return 0;

cleanup_nand:
	nand_cleanup(nand);

	return ret;
}

/*
 * Clean up routine
 */
static int mscc_nand_remove(struct platform_device *pdev)
{
	struct mscc_nand_data *host = platform_get_drvdata(pdev);
	if (host)
		nand_cleanup(&host->nand);
	return 0;
}

static const struct of_device_id mscc_nand_id_table[] = {
	{
		.compatible = "mscc,luton-nand",
		.data = &luton_props,
	},
	{
		.compatible = "mscc,jaguar2-nand",
		.data = &jaguar2_props,
	},
	{}
};
MODULE_DEVICE_TABLE(of, mscc_nand_id_table);

static struct platform_driver mscc_nand_driver = {
	.remove = mscc_nand_remove,
	.driver = {
		.name = "mscc-nand",
		.of_match_table = mscc_nand_id_table,
	},
};

module_platform_driver_probe(mscc_nand_driver, mscc_nand_probe);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microchip.com>");
MODULE_DESCRIPTION("MSCC SoC NAND driver");
