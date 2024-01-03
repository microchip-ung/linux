/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Microchip lan966x PCI card
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2022 Microchip Corporation
 */

#ifndef MASERATI_PCI_H
#define MASERATI_PCI_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/irqchip.h>
#include <linux/irq.h>
#include <linux/pci.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "lan966x_pci_irq.h"

#define LAN966X_BAR_CPU		0
#define LAN966X_BAR_AMBA	1
#define LAN966X_BAR_COUNT	2

/**
 * struct lan966x_pci - Driver private structure
 * @lan966x: child lan966x platform_device
 * @pci: our link to PCI bus
 */
struct lan966x_pci {
	struct device *dev;
	struct pci_dev *pci_dev;
	struct lan966x_irq_data irq_data;
	int ovcs_id;
};

#endif
