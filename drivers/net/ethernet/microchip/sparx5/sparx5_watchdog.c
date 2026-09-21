// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Handle Watchdog pretimeout by resetting the switch core as the
 * hardware watchdog does not do this.
 *
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 */

#include "sparx5_main.h"

static void sparx5_watchdog_reset_switchcore(struct sparx5 *sparx5)
{
	/* SOFT_CHIP_RST resets the whole SoC except for areas selected by
	 * protection bits in CPU:CPU_REGS:RESET_PROT_STAT
	 * The VCore CPU is already protected from reset by the sparx5 reset
	 * driver and this is what the HW watchdog will reset later.
	 * So here we ensure that the SERDES and PLL settings needed for the
	 * bootloader to get network up and running are reset.
	 * The reset bit and protection bits are cleared again at power on.
	 * The reset runs to completion in hardware, software need not wait
	 * The switchdev driver can still access registers during this reset,
	 * but since the system is in a fragile state (a watchdog timeout) we
	 * cannot expect to handle this from a hard irq context.
	 */
	spx5_wr(GCB_SOFT_RST_SOFT_CHIP_RST_SET(1), sparx5, GCB_SOFT_RST);
	dev_info(sparx5->dev, "watchdog pretimeout, switch core reset issued\n");
}

/* Reset in hard interrupt context, this does not depend on any pretimeout governor */
static irqreturn_t sparx5_watchdog_handler(int irq, void *args)
{
	struct sparx5 *sparx5 = args;

	/* CONFIG_DEBUG_SHIRQ calls shared handlers once more while freeing them */
	if (!READ_ONCE(sparx5->wdt_armed))
		return IRQ_NONE;

	sparx5_watchdog_reset_switchcore(sparx5);

	return IRQ_HANDLED;
}

void sparx5_watchdog_init(struct sparx5 *sparx5)
{
	int err;

	/* without the interrupt only the CPU is reset, the switch core is not */
	if (sparx5->wdt_irq < 0) {
		dev_info(sparx5->dev, "no watchdog pretimeout support\n");
		return;
	}

	/* armed before the handler is installed, since the shared line can
	 * deliver a pretimeout as soon as the handler has been registered
	 */
	WRITE_ONCE(sparx5->wdt_armed, true);

	err = devm_request_irq(sparx5->dev, sparx5->wdt_irq,
			       sparx5_watchdog_handler,
			       IRQF_SHARED | IRQF_TRIGGER_RISING,
			       "sparx5-watchdog", sparx5);

	if (err) {
		dev_err(sparx5->dev, "watchdog irq %d not available: %d\n",
			sparx5->wdt_irq, err);
		WRITE_ONCE(sparx5->wdt_armed, false);
		sparx5->wdt_irq = -ENXIO;
	}
}

void sparx5_watchdog_deinit(struct sparx5 *sparx5)
{
	if (sparx5->wdt_irq < 0)
		return;

	/* disarm first, freeing a shared irq can call the handler one last time */
	WRITE_ONCE(sparx5->wdt_armed, false);

	/* free our own action only, the line is shared with the watchdog driver */
	devm_free_irq(sparx5->dev, sparx5->wdt_irq, sparx5);
	sparx5->wdt_irq = -ENXIO;
}
