// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi MIPS SoC reset driver
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2017 Microsemi Corporation
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/of.h>

struct reset_props {
	u32 protect_reg;
	u32 vcore_protect;
	u32 if_si_owner_bit;
};

struct ocelot_reset_context {
	void __iomem *base;
	void __iomem *icpu_base;
	void __iomem *hsio_base;
	const struct reset_props *props;
	struct notifier_block restart_handler;
	bool cpu_reset_only;
};

#define BIT_OFF_INVALID				32

#define SOFT_SWC_RST  BIT(1)
#define SOFT_CHIP_RST BIT(0)

#define ICPU_CFG_CPU_SYSTEM_CTRL_RESET		0x20
#define ICPU_RESET_CORE_RST_CPU_ONLY		BIT(3)
#define ICPU_RESET_CORE_RST_PROTECT		BIT(2)
#define ICPU_RESET_CORE_RST_FORCE		BIT(1)
#define ICPU_RESET_MEM_RST_FORCE		BIT(0)

#define ICPU_CFG_CPU_SYSTEM_CTRL_GENERAL_CTRL	0x24
#define IF_SI_OWNER_MASK			GENMASK(1, 0)
#define IF_SI_OWNER_SISL			0
#define IF_SI_OWNER_SIBM			1
#define IF_SI_OWNER_SIMC			2
#define ICPU_GENERAL_CTRL_BOOT_MODE_ENA         BIT(0)

#define ICPU_CFG_SPI_MST_SW_MODE		0x50

/* HSIO PLL5G reset registers (serval-t) */
#define HSIO_PLL5G_CFG0_PLL5G_CFG2		0x08
#define HSIO_PLL5G_CFG0_PLL5G_CFG3		0x0c
#define HSIO_PLL5G_CFG0_PLL5G_CFG6		0x18
#define HSIO_HW_CFGSTAT_CLK_CFG			0x018c

static inline void set_bits(void __iomem *reg, u32 set_mask)
{
	u32 data = readl(reg);

	data |= set_mask;
	writel(data, reg);
}

static inline void clr_bits(void __iomem *reg, u32 clear_mask)
{
	u32 data = readl(reg);

	data &= ~clear_mask;
	writel(data, reg);
}

static inline void update_bits(void __iomem *reg, u32 clear_mask, u32 set_mask)
{
	u32 data = readl(reg);

	data &= ~clear_mask;
	data |= set_mask;
	writel(data, reg);
}

static int ocelot_switch_core_reset(void __iomem *base,
				    void __iomem *icpu_base,
				    const struct reset_props *props)
{
	const char *driver = "ocelot-reset";
	int timeout;

	pr_notice("%s: Resetting Switch Core\n", driver);

	/* Make sure the core is PROTECTED from reset */
	set_bits(icpu_base + props->protect_reg, props->vcore_protect);

	writel(SOFT_SWC_RST, base);
	for (timeout = 0; timeout < 100; timeout++) {
		if ((readl(base) & SOFT_SWC_RST) == 0) {
			pr_debug("%s: Switch Core Reset complete.\n", driver);
			return 0;
		}
		udelay(1);
	}

	pr_warn("%s: Switch Core Reset timeout!\n", driver);
	return -ENXIO;
}

#if defined(CONFIG_MIPS)
#define MIPS32_CACHE_OP(which, op)             (which | (op << 2))

#define MIPS32_WHICH_ICACHE                    0x0
#define MIPS32_WHICH_DCACHE                    0x1

#define MIPS32_INDEX_INVALIDATE                0x0
#define MIPS32_INDEX_LOAD_TAG                  0x1
#define MIPS32_INDEX_STORE_TAG                 0x2
#define MIPS32_HIT_INVALIDATE                  0x4
#define MIPS32_ICACHE_FILL                     0x5
#define MIPS32_DCACHE_HIT_INVALIDATE           0x5
#define MIPS32_DCACHE_HIT_WRITEBACK            0x6
#define MIPS32_FETCH_AND_LOCK                  0x7

#define ICACHE_LOAD_LOCK (MIPS32_CACHE_OP(MIPS32_WHICH_ICACHE,		\
					  MIPS32_FETCH_AND_LOCK))

#define CACHE_LINE_LEN 32

/* Prefetch and lock instructions into cache */
static void icache_lock(void *func, size_t len)
{
	int i, lines = ((len - 1) / CACHE_LINE_LEN) + 1;

	for (i = 0; i < lines; i++) {
		asm volatile (" cache %0, %1(%2)"
			      : /* No Output */
			      : "I" ICACHE_LOAD_LOCK,
				"n" (i*CACHE_LINE_LEN),
				"r" (func)
			      : /* No Clobbers */);
	}
}
#endif

static noinline void _cpu_reset_doit(struct ocelot_reset_context *ctx,
				     u32 reg_ctl, u32 reg_rst)
{
	writel(reg_ctl, ctx->icpu_base + ICPU_CFG_CPU_SYSTEM_CTRL_GENERAL_CTRL);
	/* Read back to make setting effective */
	reg_ctl = readl(ctx->icpu_base + ICPU_CFG_CPU_SYSTEM_CTRL_GENERAL_CTRL);
	/* Now, do the reset */
	writel(reg_rst, ctx->icpu_base + ICPU_CFG_CPU_SYSTEM_CTRL_RESET);
}

static void cpu_reset(struct ocelot_reset_context *ctx)
{
	u32 reg_ctl, reg_rst;

	if (ctx->hsio_base) {
		// Selected registers of Serval-T's 5G PLL need to have their
		// values restored to defaults prior to the boot, or the system
		// will hang (see Bugzilla#20926).
		writel(0x00106114, ctx->hsio_base + HSIO_PLL5G_CFG0_PLL5G_CFG2);
		writel(0x00224028, ctx->hsio_base + HSIO_PLL5G_CFG0_PLL5G_CFG3);
		writel(0x000014ce, ctx->hsio_base + HSIO_PLL5G_CFG0_PLL5G_CFG6);
		writel(0x00000000, ctx->hsio_base + HSIO_HW_CFGSTAT_CLK_CFG);
	}

	/*
	 * Note: Reset is done by first resetting switch-core only, and
	 * then the CPU core only to avoid resetting DDR controlller.
	 * This is due to the lack of DDR RAM reset out, which means we
	 * can't reset the DDR RAM during controller reset. This again
	 * can cause a potential DDR RAM lockup if the DDR controller is
	 * reset at a "bad" time. Thus, we avoid the DDR controller reset
	 * by resetting only the switchcore and then the CPU core, and
	 * not the entire CPU system. While doing this we must ensure the
	 * "boot mode" bit points at ROM.
	 *
	 * As we are changing the translation "in mid air", we must
	 * ensure the instructions until the cpu reset are in the CPU
	 * cache.
	 */
	ocelot_switch_core_reset(ctx->base, ctx->icpu_base, ctx->props);

	/* Avoid bad things happening */
	local_irq_disable();

	/* Reset SW_MODE (be sure bitbang mode is off) */
	writel(0, ctx->icpu_base + ICPU_CFG_SPI_MST_SW_MODE);

	/* Set BOOT_MODE (only activates at write, read) */
	reg_ctl = readl(ctx->icpu_base + ICPU_CFG_CPU_SYSTEM_CTRL_GENERAL_CTRL)
		| ICPU_GENERAL_CTRL_BOOT_MODE_ENA;
	reg_rst = (ICPU_RESET_CORE_RST_CPU_ONLY|ICPU_RESET_CORE_RST_FORCE);

	/* Reset CPU core */
#if defined(CONFIG_MIPS)
	icache_lock(_cpu_reset_doit, 128);
#endif
	_cpu_reset_doit(ctx, reg_ctl, reg_rst);
}

static int ocelot_restart_handle(struct notifier_block *this,
				 unsigned long mode, void *cmd)
{
	struct ocelot_reset_context *ctx = container_of(this, struct
							ocelot_reset_context,
							restart_handler);
	u32 if_si_owner_bit = ctx->props->if_si_owner_bit;

	/* Change SI owner for boot mode to work */
	if (if_si_owner_bit != BIT_OFF_INVALID)
		update_bits(ctx->icpu_base + ICPU_CFG_CPU_SYSTEM_CTRL_GENERAL_CTRL,
			    IF_SI_OWNER_MASK << if_si_owner_bit,
			    IF_SI_OWNER_SIBM << if_si_owner_bit);

	if (ctx->cpu_reset_only) {
		pr_emerg("Resetting CPU\n");
		cpu_reset(ctx);
		pr_emerg("Resetting CPU failed\n");
	}

	/* Make sure the core is not protected from reset */
	clr_bits(ctx->icpu_base + ctx->props->protect_reg,
		 ctx->props->vcore_protect);

	pr_emerg("Resetting SoC\n");

	writel(SOFT_CHIP_RST, ctx->base);

	pr_emerg("Unable to restart system\n");
	return NOTIFY_DONE;
}

static int ocelot_reset_probe(struct platform_device *pdev)
{
	struct ocelot_reset_context *ctx;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct device_node *np = pdev->dev.of_node;
	int err;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->base))
		return PTR_ERR(ctx->base);

	ctx->cpu_reset_only = of_property_read_bool(np, "microchip,cpu-reset-only");

	ctx->props = device_get_match_data(dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -ENOENT;

	ctx->icpu_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!ctx->icpu_base)
		return -ENOMEM;

	/* Optional PLL5G workaround for CPU reset (serval-t) */
	if (ctx->cpu_reset_only) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
		if (res)
			ctx->hsio_base = devm_ioremap(dev, res->start,
						      resource_size(res));
	}

	/* Optionally, call switch reset function */
	if (of_property_read_bool(np, "microchip,reset-switch-core"))
		ocelot_switch_core_reset(ctx->base, ctx->icpu_base, ctx->props);


	ctx->restart_handler.notifier_call = ocelot_restart_handle;
	ctx->restart_handler.priority = 192;
	err = register_restart_handler(&ctx->restart_handler);
	if (err)
		dev_err(dev, "can't register restart notifier (err=%d)\n", err);

	return err;
}

static const struct reset_props reset_props_jaguar2 = {
	.protect_reg     = 0x20,
	.vcore_protect   = BIT(2),
	.if_si_owner_bit = 6,
};

static const struct reset_props reset_props_luton = {
	.protect_reg     = 0x20,
	.vcore_protect   = BIT(2),
	.if_si_owner_bit = BIT_OFF_INVALID, /* n/a */
};

static const struct reset_props reset_props_ocelot = {
	.protect_reg     = 0x20,
	.vcore_protect   = BIT(2),
	.if_si_owner_bit = 4,
};

static const struct reset_props reset_props_sparx5 = {
	.protect_reg     = 0x84,
	.vcore_protect   = BIT(10),
	.if_si_owner_bit = 6,
};

static const struct of_device_id ocelot_reset_of_match[] = {
	{
		.compatible = "mscc,luton-chip-reset",
		.data = &reset_props_luton },
	{
		.compatible = "mscc,jaguar2-chip-reset",
		.data = &reset_props_jaguar2 },
	{
		.compatible = "mscc,ocelot-chip-reset",
		.data = &reset_props_ocelot
	}, {
		.compatible = "microchip,sparx5-chip-reset",
		.data = &reset_props_sparx5
	},
	{ /*sentinel*/ }
};

static struct platform_driver ocelot_reset_driver = {
	.probe = ocelot_reset_probe,
	.driver = {
		.name = "ocelot-chip-reset",
		.of_match_table = ocelot_reset_of_match,
	},
};

static int __init reset_init(void)
{
	return platform_driver_register(&ocelot_reset_driver);
}
postcore_initcall(reset_init);
