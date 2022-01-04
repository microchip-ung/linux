// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 Microchip Technology Inc.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <linux/spi/spi.h>

#include "lan966x_spi_regs_sr.h"

/* This needs to be the same with DT */
#define QSPI_TARGET			1

#ifdef CONFIG_DEBUG_KERNEL
#define LAN_RD_(regs, id, tinst, tcnt,			\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 readl(regs[id + (tinst)] +			\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)))

#define LAN_WR_(val, regs, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 writel(val, regs[id + (tinst)] +		\
	        gbase + ((ginst) * gwidth) +		\
	        raddr + ((rinst) * rwidth)))

#define LAN_RMW_(val, mask, regs, id, tinst, tcnt,	\
		 gbase, ginst, gcnt, gwidth,		\
		 raddr, rinst, rcnt, rwidth) do {	\
	u32 _v_;					\
	WARN_ON((tinst) >= tcnt);			\
	WARN_ON((ginst) >= gcnt);			\
	WARN_ON((rinst) >= rcnt);			\
	_v_ = readl(regs[id + (tinst)] +		\
		    gbase + ((ginst) * gwidth) +	\
		    raddr + ((rinst) * rwidth));	\
	_v_ = ((_v_ & ~(mask)) | ((val) & (mask)));	\
	writel(_v_, regs[id + (tinst)] +		\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)); } while (0)
#else
#define LAN_RD_(regs, id, tinst, tcnt,			\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	readl(regs[id + (tinst)] +			\
	      gbase + ((ginst) * gwidth) +		\
	      raddr + ((rinst) * rwidth))

#define LAN_WR_(val, regs, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	writel(val, egs[id + (tinst)] +			\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth))

#define LAN_RMW_(val, mask, regs, id, tinst, tcnt,	\
		 gbase, ginst, gcnt, gwidth,		\
		 raddr, rinst, rcnt, rwidth) do {	\
	u32 _v_;					\
	_v_ = readl(regs[id + (tinst)] +		\
		    gbase + ((ginst) * gwidth) +	\
		    raddr + ((rinst) * rwidth));	\
	_v_ = ((_v_ & ~(mask)) | ((val) & (mask)));	\
	writel(_v_, regs[id + (tinst)] +		\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)); } while (0)
#endif

#define LAN_WR(...) LAN_WR_(__VA_ARGS__)
#define LAN_RD(...) LAN_RD_(__VA_ARGS__)
#define LAN_RMW(...) LAN_RMW_(__VA_ARGS__)

static int lan966x_spi_rte_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	void __iomem *regs[NUM_TARGETS];
	unsigned long timeout;
	struct clk *pclk;
	int i;

	struct {
		enum lan966x_target id;
		char *name;
	} res[] = {
		{ TARGET_QSPI, "qspi0" },
		{ TARGET_QSPI + 1, "qspi1" },
		{ TARGET_QSPI + 2, "qspi2" },
	};

	if (!np && !pdev->dev.platform_data)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(res); i++) {
		struct resource *resource;

		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							res[i].name);
		if (!resource)
			return -ENODEV;

		regs[res[i].id] = ioremap(resource->start,
					  resource_size(resource));
		if (IS_ERR(regs[res[i].id])) {
			dev_err(&pdev->dev,
				"Unable to map Switch registers: %x\n", i);
			return PTR_ERR(regs[res[i].id]);
		}
	}

	pclk = devm_clk_get(&pdev->dev, "gclk");
	if (IS_ERR(pclk))
		return PTR_ERR(pclk);

	clk_prepare_enable(pclk);

	/* Set QSPI regisers */
	LAN_RMW(QSPI_QSPI_CR_DLLON(1), QSPI_QSPI_CR_DLLON_M,
		regs, QSPI_QSPI_CR(QSPI_TARGET));

	timeout = jiffies + msecs_to_jiffies(100);
	while (!(LAN_RD(regs, QSPI_QSPI_SR(QSPI_TARGET)) & QSPI_QSPI_SR_DLOCK_M) &&
	        time_before_eq(timeout, jiffies))
		;

	if (!(LAN_RD(regs, QSPI_QSPI_SR(QSPI_TARGET)) & QSPI_QSPI_SR_DLOCK_M))
		dev_warn(&pdev->dev, "DLOCK was not set\n");

	LAN_RMW(QSPI_QSPI_MR_SMM(1), QSPI_QSPI_MR_SMM_M,
		regs, QSPI_QSPI_MR(QSPI_TARGET));

	LAN_RMW(QSPI_QSPI_CR_UPDCFG(1), QSPI_QSPI_CR_UPDCFG_M,
		regs, QSPI_QSPI_CR(QSPI_TARGET));

	while (LAN_RD(regs, QSPI_QSPI_SR(QSPI_TARGET)) & QSPI_QSPI_SR_SYNCBSY_M)
		;

	LAN_RMW(QSPI_QSPI_CR_QSPIEN(1), QSPI_QSPI_CR_QSPIEN_M,
		regs, QSPI_QSPI_CR(QSPI_TARGET));

	while (!(LAN_RD(regs, QSPI_QSPI_SR(QSPI_TARGET)) & QSPI_QSPI_SR_QSPIENS_M))
		;

	LAN_RMW(QSPI_QSPI_RICR_RDINST(0xeb), QSPI_QSPI_RICR_RDINST_M,
		regs, QSPI_QSPI_RICR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_WICR_WRINST(0x38), QSPI_QSPI_WICR_WRINST_M,
		regs, QSPI_QSPI_WICR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_APBTFRTYP(1), QSPI_QSPI_IFR_APBTFRTYP_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_TFRTYP(1), QSPI_QSPI_IFR_TFRTYP_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_NBDUM(6), QSPI_QSPI_IFR_NBDUM_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_ADDRL(2), QSPI_QSPI_IFR_ADDRL_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_DATAEN(1), QSPI_QSPI_IFR_DATAEN_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_ADDREN(1), QSPI_QSPI_IFR_ADDREN_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_INSTEN(1), QSPI_QSPI_IFR_INSTEN_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_IFR_WIDTH(4), QSPI_QSPI_IFR_WIDTH_M,
		regs, QSPI_QSPI_IFR(QSPI_TARGET));
	LAN_RMW(QSPI_QSPI_CR_LASTXFER(1), QSPI_QSPI_CR_LASTXFER_M,
		regs, QSPI_QSPI_CR(QSPI_TARGET));

	while (LAN_RD(regs, QSPI_QSPI_SR(QSPI_TARGET)) & QSPI_QSPI_SR_SYNCBSY_M)
		;

	LAN_RMW(QSPI_QSPI_CR_UPDCFG(1), QSPI_QSPI_CR_UPDCFG_M,
		regs, QSPI_QSPI_CR(QSPI_TARGET));

	while (LAN_RD(regs, QSPI_QSPI_SR(QSPI_TARGET)) & QSPI_QSPI_SR_SYNCBSY_M)
		;

	return 0;
}

static int lan966x_spi_rte_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id lan966x_spi_rte_match[] = {
	{ .compatible = "mchp,lan966x_spi_rte", },
	{}
};
MODULE_DEVICE_TABLE(of, lan966x_spi_rte_match);

static struct platform_driver lan966x_spi_rte_driver = {
	.probe	= lan966x_spi_rte_probe,
	.remove	= lan966x_spi_rte_remove,
	.driver	= {
		.name		= "lan966x-spi-rte",
		.of_match_table	= lan966x_spi_rte_match,
	},
};
module_platform_driver(lan966x_spi_rte_driver);
