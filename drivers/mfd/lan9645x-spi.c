// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/math.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/mfd/lan9645x.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/units.h>
#include <linux/mfd/core.h>

#define DEVCPU_GCB_IF_CTRL (0x0007 << 2)
#define DEVCPU_GCB_IF_CFGSTAT (0x0008 << 2)

#define DEVCPU_GCB_BUILDID (0x0006 << 2)

#define DEVCPU_GCB_SOFT_RST (0x0004 << 2)
#define SOFT_CHIP_RST BIT(0)
#define SOFT_SWC_RST BIT(1)

#define LAN9645X_GCB_RST_SLEEP_US 100
#define LAN9645X_GCB_RST_TIMEOUT_US 100000

#define REG_ADDR_SIZE 3
#define REG_VALUE_SIZE 4
#define MAX_BUF_SIZE \
	(REG_ADDR_SIZE + LAN9645X_SPI_MAX_PADDING_BYTES + REG_VALUE_SIZE)

/* The static latency is approximately 1 us, however there are some
 * targets which utilize an internal calendar and they can introduce over
 * 200 cycles additional latency.
 *
 * Therefore, design suggests using a more modest value such as 2us.
 */
#define LAN9645X_REG_ACCESS_TIME_US 2

static const struct resource lan9645x_pinctrl_resources[] = {
	DEFINE_RES_REG_NAMED(0x4028, 0x6c, "gcb_gpio"),
};

static const struct resource lan9645x_miim0_resources[] = {
	DEFINE_RES_REG_NAMED(0x4098, 0x28, "gcb_miim0"),
};

static const struct resource lan9645x_miim1_resources[] = {
	DEFINE_RES_REG_NAMED(0x40bc, 0x28, "gcb_miim1"),
	/* CHIP_TOP:CUPHY_CFG:CUPHY_COMMON_CFG */
	DEFINE_RES_REG_NAMED(0x10048, 0x1, "phy"),
};

static const struct resource lan9645x_spi_resources[] = {
	DEFINE_RES_REG_NAMED(0x4000, 0x244, "gcb"),
#if defined(CONFIG_DEBUG_FS)
	DEFINE_RES_REG_NAMED(0x0, 0x540305, "all"),
#endif
};

static const struct mfd_cell lan9645x_devs[] = {
	{
		.name = "lan9645x-pinctrl",
		.of_compatible = "microchip,lan9645x-pinctrl",
		.num_resources = ARRAY_SIZE(lan9645x_pinctrl_resources),
		.resources = lan9645x_pinctrl_resources,
	},
	{
		.name = "lan9645x-miim0",
		.of_compatible = "microchip,lan966x-miim",
		.of_reg = 0x4098,
		.use_of_reg = true,
		.num_resources = ARRAY_SIZE(lan9645x_miim0_resources),
		.resources = lan9645x_miim0_resources,
	},
	{
		.name = "lan9645x-miim1",
		.of_compatible = "microchip,lan966x-miim",
		.of_reg = 0x40bc,
		.use_of_reg = true,
		.num_resources = ARRAY_SIZE(lan9645x_miim1_resources),
		.resources = lan9645x_miim1_resources,
	},
};

/* Assume byte-addressed register addresses.
 *
 * NOTE: default regmap locking is a mutex (can sleep). If regmap address spaces
 * are disjoint, there is no deadlock threat.
 *
 * The regmap share the spi-device, which has a mutex, so there is no
 * concurrency danger vs the controller with the driver userspace access via
 * the debugfs.
 */
static const struct regmap_config lan9645x_spi_regmap_config = {
	.reg_bits = 24,
	.reg_stride = 4,
	.reg_shift = REGMAP_DOWNSHIFT(2),
	.val_bits = 32,
	.write_flag_mask = 0x80,
	.use_single_read = true,
	.use_single_write = true,
	.can_multi_write = false,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_NATIVE,
};

static int lan9645x_spi_regmap_bus_read(void *context, const void *reg,
					size_t reg_size, void *val,
					size_t val_size)
{
	struct spi_transfer t = { 0 };
	struct device *dev = context;
	struct lan9645x_ddata *ddata;
	const u8 *regu8 = reg;
	u8 tx[MAX_BUF_SIZE];
	u8 rx[MAX_BUF_SIZE];
	u8 *valu8 = val;
	int err;

	ddata = dev_get_drvdata(dev);

	tx[0] = regu8[0];
	tx[1] = regu8[1];
	tx[2] = regu8[2];

	t.tx_buf = tx;
	t.rx_buf = rx;
	t.len = reg_size + ddata->spi_padding_bytes + val_size;

	err = spi_sync_transfer(to_spi_device(dev), &t, 1);
	if (err)
		return err;

	valu8[0] = rx[reg_size + ddata->spi_padding_bytes + 0];
	valu8[1] = rx[reg_size + ddata->spi_padding_bytes + 1];
	valu8[2] = rx[reg_size + ddata->spi_padding_bytes + 2];
	valu8[3] = rx[reg_size + ddata->spi_padding_bytes + 3];

	return 0;
}

static int lan9645x_spi_regmap_bus_write(void *context, const void *data,
					 size_t count)
{
	struct device *dev = context;
	struct spi_device *spi = to_spi_device(dev);

	return spi_write(spi, data, count);
}

static const struct regmap_bus lan9645x_spi_regmap_bus = {
	.write = lan9645x_spi_regmap_bus_write,
	.read = lan9645x_spi_regmap_bus_read,
};

static struct regmap *lan9645x_spi_init_regmap(struct device *dev,
					       const struct resource *res)
{
	struct regmap_config regmap_config;

	memcpy(&regmap_config, &lan9645x_spi_regmap_config,
	       sizeof(regmap_config));

	regmap_config.name = res->name;
	regmap_config.max_register = resource_size(res) - 1;
	regmap_config.reg_base = res->start;

	return devm_regmap_init(dev, &lan9645x_spi_regmap_bus, dev,
				&regmap_config);
}

static int lan9645x_add_regmap(struct device *dev, const struct resource *res)
{
	struct regmap *r;

	if (dev_get_regmap(dev, res->name))
		return 0;

	r = lan9645x_spi_init_regmap(dev, res);
	if (IS_ERR(r))
		return PTR_ERR(r);

	return 0;
}

static int lan9645x_add_regmaps(struct device *dev, const char *name,
				const struct resource *resources,
				int num_resources)
{
	int err;

	for (int i = 0; i < num_resources; i++) {
		err = lan9645x_add_regmap(dev, &resources[i]);
		if (err)
			return dev_err_probe(dev, err,
					     "Error initializing device %s regmap index %d\n",
					     name, i);
	}

	return 0;
}

static int lan9645x_mfd_add_devs(struct device *dev)
{
	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, lan9645x_devs,
				    ARRAY_SIZE(lan9645x_devs), NULL, 0, NULL);
}

int lan9645x_spi_chip_reset(struct device *dev)
{
	struct lan9645x_ddata *ddata = dev_get_drvdata(dev);
	int ret, val;

	ret = regmap_write(ddata->gcb, DEVCPU_GCB_SOFT_RST, SOFT_SWC_RST);
	if (ret)
		return ret;

	/* If the chip is reset, will also reset the spi-controller. Therefore we
	 * are no longer able to read unless we use the correct padding bytes.
	 *
	 * Reading with padding byte less than the chip configuration results in
	 * all zero reads, which makes it challenging to determine if reset bit
	 * is cleared.
	 *
	 * Note the byte and bitorder is also reset to their default values:
	 * Big-Endian and MSB-first.
	 *
	 * But with correct padding bytes, we should be able to detect != 0 for
	 * the reset bit.
	 *
	 * Callers are expected to reconfigure the SPI controller afterwards.
	 */
	ddata->spi_padding_bytes = LAN9645X_SPI_DEFAULT_PADDING_BYTES;

	return regmap_read_poll_timeout(ddata->gcb, DEVCPU_GCB_SOFT_RST, val,
					!val, LAN9645X_GCB_RST_SLEEP_US,
					LAN9645X_GCB_RST_TIMEOUT_US);
}

static u32 lan9645x_spi_padding_bytes(struct spi_device *spi)
{
	u32 khz = spi->max_speed_hz / HZ_PER_KHZ;

	/* Datasheet has this formula when using the padding bytes
	 * strategy. It gives you the maximum clock a given amount
	 * of padding bytes will sustain. Access times are stated in the
	 * datasheet.
	 *
	 * (IF_CFGSTAT.IF_CFG * 8 - 1.5) / access_time = Max clock
	 */
	return DIV_ROUND_UP(khz * LAN9645X_REG_ACCESS_TIME_US + 1500, 8000);
}

static int lan9645x_spi_initialize(struct device *dev)
{
	struct lan9645x_ddata *ddata = dev_get_drvdata(dev);
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int err;

	ddata->spi_padding_bytes = lan9645x_spi_padding_bytes(spi);

	err = regmap_write(ddata->gcb, DEVCPU_GCB_IF_CTRL,
			   LAN9645X_SPI_BYTE_ORDER);
	if (err)
		return err;

	err = regmap_write(ddata->gcb, DEVCPU_GCB_IF_CFGSTAT,
			   ddata->spi_padding_bytes);
	if (err)
		return err;

	mdelay(5);

	err = regmap_read(ddata->gcb, DEVCPU_GCB_IF_CFGSTAT, &val);
	if (err)
		return err;

	if (ddata->spi_padding_bytes != val) {
		dev_err(dev,
			"Unpexected padding bytes value read: %u expected=%d\n",
			val, ddata->spi_padding_bytes);
		return -ENODEV;
	}

	dev_info(dev, "SPI frequency: %u, padding bytes required: %d",
		 spi->max_speed_hz,
		 ddata->spi_padding_bytes);

	return 0;
}

static ssize_t lan9645x_debugfs_write(struct file *filp, const char __user *buf,
				      size_t count, loff_t *pos)
{
	struct lan9645x_ddata *ddata = filp->private_data;
	u32 reg = *pos;
	u8 *buf_dup;
	u32 val;
	int err;

	if (count < REG_VALUE_SIZE)
		return -EINVAL;

	buf_dup = memdup_user(buf, count);
	if (IS_ERR(buf_dup))
		return PTR_ERR(buf_dup);

	/* NOTE: assume LE encoding */
	val = (u32)buf_dup[0] << 0 |
	      (u32)buf_dup[1] << 8 |
	      (u32)buf_dup[2] << 16 |
	      (u32)buf_dup[3] << 24;

	kfree(buf_dup);

	err = regmap_write(ddata->regs, reg, val);
	if (err)
		return err;

	return REG_VALUE_SIZE;
}

static ssize_t lan9645x_debugfs_read(struct file *filp, char __user *buf,
				     size_t count, loff_t *pos)
{
	struct lan9645x_ddata *ddata = filp->private_data;
	u8 tmp[REG_VALUE_SIZE] = { 0 };
	u32 spi_addr = *pos;
	u32 val;
	int err;

	if (count < REG_VALUE_SIZE)
		return -EINVAL;

	err = regmap_read(ddata->regs, spi_addr, &val);
	if (err)
		return err;

	/* NOTE: output LE encoding */
	tmp[3] = val >> 24;
	tmp[2] = val >> 16;
	tmp[1] = val >> 8;
	tmp[0] = val >> 0;

	err = copy_to_user(buf, tmp, sizeof(tmp));
	if (err)
		return err;

	return sizeof(tmp);
}

static const struct file_operations fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= lan9645x_debugfs_write,
	.read	= lan9645x_debugfs_read,
};

static int lan9645x_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct lan9645x_ddata *ddata;
	int err;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	spi_set_drvdata(spi, ddata);

	/* Make sure reads succeed when chip is reset. */
	ddata->spi_padding_bytes = LAN9645X_SPI_DEFAULT_PADDING_BYTES;
	spi->bits_per_word = 8;

	err = spi_setup(spi);
	if (err)
		return dev_err_probe(&spi->dev, err,
				     "Error performing SPI setup\n");

	for (int d = 0; d < ARRAY_SIZE(lan9645x_devs); d++) {
		const struct mfd_cell *child = &lan9645x_devs[d];

		err = lan9645x_add_regmaps(dev, child->name, child->resources,
					   child->num_resources);
		if (err)
			return err;
	}

	err = lan9645x_add_regmaps(dev, dev_name(dev), lan9645x_spi_resources,
				   ARRAY_SIZE(lan9645x_spi_resources));
	if (err)
		return err;

	ddata->gcb = dev_get_regmap(dev, "gcb");
	if (!ddata->gcb)
		return dev_err_probe(dev, -EINVAL,
				     "Error could not find 'gcb' regmap\n");

#if defined(CONFIG_DEBUG_FS)
	ddata->regs = dev_get_regmap(dev, "all");
	if (!ddata->regs)
		return dev_err_probe(dev, -EINVAL,
				     "Error could not find 'all' regmap\n");

	ddata->debugfs_root = debugfs_create_dir("lan9645x", NULL);
	debugfs_create_file("mem", 0666, ddata->debugfs_root, ddata, &fops);
#endif
	/* The chip must be set up for SPI before it gets initialized and reset.
	 * This must be done before calling init, and after a chip reset is
	 * performed.
	 */
	err = lan9645x_spi_initialize(dev);
	if (err)
		return dev_err_probe(dev, err, "Error initializing SPI bus\n");


	err = lan9645x_spi_chip_reset(dev);
	if (err)
		return dev_err_probe(dev, err, "Error resetting device\n");

	/* A chip reset clears SPI controller config, so we init again. */
	err = lan9645x_spi_initialize(dev);
	if (err)
		return dev_err_probe(dev, err,
				     "Error initializing SPI bus after reset\n");

	/* Add child devices and register their regmaps */
	err = lan9645x_mfd_add_devs(dev);
	if (err)
		return dev_err_probe(dev, err,
				     "Error initializing MFD Lan9645x child devices\n");

	return 0;
}

static const struct spi_device_id lan9645x_spi_ids[] = {
	{ "lan9645x-spi", },
	{}
};
MODULE_DEVICE_TABLE(spi, lan9645x_spi_ids);

static const struct of_device_id lan9645x_spi_of_match[] = {
	{ .compatible = "microchip,lan9645x-spi" },
	{}
};
MODULE_DEVICE_TABLE(of, lan9645x_spi_of_match);

static struct spi_driver lan9645x_spi_driver = {
	.driver = {
		.name = "microchip-lan9645x-spi",
		.of_match_table = lan9645x_spi_of_match,
	},
	.id_table = lan9645x_spi_ids,
	.probe = lan9645x_spi_probe,
};
module_spi_driver(lan9645x_spi_driver);

MODULE_DESCRIPTION("SPI Controlled Lan9645x Chip Driver");
MODULE_AUTHOR("Jens Emil Schulz Østergaard <jensemil.schulzostergaard@microchip.com>");
MODULE_LICENSE("GPL");
