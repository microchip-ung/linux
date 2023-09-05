// SPDX-License-Identifier: GPL-2.0-or-later
/* Sparx5 SoC temperature sensor driver
 *
 * Copyright (C) 2020 Lars Povlsen <lars.povlsen@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define TEMP_CTRL		0
#define TEMP_CFG		4
#define  TEMP_CFG_CYCLES	GENMASK(24, 15)
#define  TEMP_CFG_ENA		BIT(0)
#define TEMP_STAT		8
#define  TEMP_STAT_VALID	BIT(12)
#define  TEMP_STAT_TEMP		GENMASK(11, 0)

#define FAN_CFG			0x0
#define   FAN_CFG_DUTY_CYCLE		GENMASK(23, 16)
#define   INV_POL			BIT(3)
#define   GATE_ENA			BIT(2)
#define   PWM_OPEN_COL_ENA		BIT(1)
#define   FAN_STAT_CFG			BIT(0)
#define FAN_PWM_FREQ		0x4
#define   FAN_PWM_CYC_10US		GENMASK(27, 16)
#define   FAN_PWM_FREQ_FREQ		GENMASK(15, 0)
#define FAN_CNT			0xc
#define   FAN_CNT_DATA			GENMASK(15, 0)

struct s5_hwmon {
	void __iomem *base;
	void __iomem *fan;
	struct clk *clk;
};

static void s5_temp_enable(struct s5_hwmon *hwmon)
{
	u32 val = readl(hwmon->base + TEMP_CFG);
	u32 clk = clk_get_rate(hwmon->clk) / USEC_PER_SEC;

	val &= ~TEMP_CFG_CYCLES;
	val |= FIELD_PREP(TEMP_CFG_CYCLES, clk);
	val |= TEMP_CFG_ENA;

	writel(val, hwmon->base + TEMP_CFG);
}

static int s5_read_temp(struct device *dev, long *temp)
{
	struct s5_hwmon *hwmon = dev_get_drvdata(dev);
	int value;
	u32 stat;

	stat = readl_relaxed(hwmon->base + TEMP_STAT);
	if (!(stat & TEMP_STAT_VALID))
		return -EAGAIN;
	value = stat & TEMP_STAT_TEMP;
	/*
	 * From register documentation:
	 * Temp(C) = TEMP_SENSOR_STAT.TEMP / 4096 * 352.2 - 109.4
	 */
	value = DIV_ROUND_CLOSEST(value * 3522, 4096) - 1094;
	/*
	 * Scale down by 10 from above and multiply by 1000 to
	 * have millidegrees as specified by the hwmon sysfs
	 * interface.
	 */
	value *= 100;
	*temp = value;

	return 0;
}

static int s5_read_pwm(struct device *dev, long *val)
{
	struct s5_hwmon *hwmon = dev_get_drvdata(dev);
	u32 data;

	data = readl_relaxed(hwmon->fan + FAN_CFG);
	*val = FIELD_GET(FAN_CFG_DUTY_CYCLE, data);

	return 0;
}

static int s5_read(struct device *dev, enum hwmon_sensor_types type,
		   u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_temp:
		return s5_read_temp(dev, val);
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return s5_read_pwm(dev, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int s5_write_pwm(struct device *dev, long val)
{
	struct s5_hwmon *hwmon = dev_get_drvdata(dev);
	u32 tmp;

	if (val < 0 || val > 255)
		return -EINVAL;

	tmp = readl_relaxed(hwmon->fan + FAN_CFG);
	tmp &= ~FAN_CFG_DUTY_CYCLE;
	tmp |= FIELD_PREP(FAN_CFG_DUTY_CYCLE, val);
	writel_relaxed(tmp, hwmon->fan + FAN_CFG);

	return 0;
}

static int s5_write(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return s5_write_pwm(dev, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t s5_is_visible(const void *_data, enum hwmon_sensor_types type,
			     u32 attr, int channel)
{
	const struct s5_hwmon *hwmon = _data;
	umode_t mode = 0;

	if (type != hwmon_temp && !hwmon->fan)
		return 0;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			mode = 0444;
			break;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			mode = 0644;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return mode;
}

static const struct hwmon_channel_info *s5_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_ops s5_hwmon_ops = {
	.is_visible = s5_is_visible,
	.read = s5_read,
	.write = s5_write,
};

static const struct hwmon_chip_info s5_chip_info = {
	.ops = &s5_hwmon_ops,
	.info = s5_info,
};

void __iomem *s5_get_opt_io_res(struct platform_device *pdev,
				     unsigned int index)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!res)
		return NULL;

	return devm_ioremap_resource(&pdev->dev, res);
}

static int s5_temp_probe(struct platform_device *pdev)
{
	struct device *hwmon_dev;
	struct s5_hwmon *hwmon;

	hwmon = devm_kzalloc(&pdev->dev, sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;

	hwmon->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hwmon->base))
		return PTR_ERR(hwmon->base);

	/* Get optional fan resource */
	hwmon->fan = s5_get_opt_io_res(pdev, 1);

	hwmon->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(hwmon->clk))
		return PTR_ERR(hwmon->clk);

	s5_temp_enable(hwmon);

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
							 "s5_temp",
							 hwmon,
							 &s5_chip_info,
							 NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id s5_temp_match[] = {
	{ .compatible = "microchip,sparx5-temp" },
	{},
};
MODULE_DEVICE_TABLE(of, s5_temp_match);

static struct platform_driver s5_temp_driver = {
	.probe = s5_temp_probe,
	.driver = {
		.name = "sparx5-temp",
		.of_match_table = s5_temp_match,
	},
};

module_platform_driver(s5_temp_driver);

MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microchip.com>");
MODULE_DESCRIPTION("Sparx5 SoC temperature sensor driver");
MODULE_LICENSE("GPL");
