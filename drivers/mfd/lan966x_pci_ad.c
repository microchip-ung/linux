// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Microchip lan966x
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2019 Microchip Corporation
 */
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip.h>
#include <linux/irq.h>
#include <linux/iopoll.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/delay.h>

#include "lan966x_pci_regs_ad.h"

#ifdef CONFIG_DEBUG_KERNEL
#define LAN_RD_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 readl((lan966x).regs[id + (tinst)] +		\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)))

#define LAN_WR_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 writel(val, (lan966x).regs[id + (tinst)] +	\
	        gbase + ((ginst) * gwidth) +		\
	        raddr + ((rinst) * rwidth)))
#else
#define LAN_RD_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	readl((lan966x).regs[id + (tinst)] +		\
	      gbase + ((ginst) * gwidth) +		\
	      raddr + ((rinst) * rwidth))

#define LAN_WR_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	writel(val, (lan966x).regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth))
#endif

#define LAN_WR(...) LAN_WR_(__VA_ARGS__)
#define LAN_RD(...) LAN_RD_(__VA_ARGS__)

#define LAN966X_SWITCH_BAR	0

#define PCI_VENDOR_ID_MCHP		0x101b
#define PCI_DEVICE_ID_MCHP_LAN966X	0x9956

#define SYS_TARGET_OFFSET	(0x1 << 16)
#define SYS_TARGET_LENGTH	(0x10000)

#define GCB_TARGET_OFFSET	(0x7 << 16)
#define GCB_TARGET_LENGTH	(0x10000)

#define ORG_TARGET_OFFSET	(0x0 << 16)
#define ORG_TARGET_LENGTH	(0x10000)

#define READL_SLEEP_US		10
#define READL_TIMEOUT_US	100000

struct lan966x {
	void __iomem *regs[NUM_TARGETS];
};

static struct pci_device_id lan966x_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MCHP, PCI_DEVICE_ID_MCHP_LAN966X) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, lan966x_ids);

static inline int lan966x_ram_init(struct lan966x *lan966x)
{
	return LAN_RD(*lan966x, SYS_RAM_INIT);
}

static inline int lan966x_soft_reset(struct lan966x *lan966x)
{
	return LAN_RD(*lan966x, GCB_SOFT_RST);
}

static int lan966x_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct lan966x lan966x;
	u32 val;
	int ret;
	int i;

	if (!pdev->dev.of_node)
		return -ENODEV;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev,
				"DMA configuration failed: 0x%x\n", ret);
			return ret;
		}
	}

	/* Needs to come from DT */
	lan966x.regs[TARGET_SYS] = pci_iomap_range(pdev, LAN966X_SWITCH_BAR,
						   SYS_TARGET_OFFSET,
						   SYS_TARGET_LENGTH);
	lan966x.regs[TARGET_GCB] = pci_iomap_range(pdev, LAN966X_SWITCH_BAR,
						   GCB_TARGET_OFFSET,
						   GCB_TARGET_LENGTH);
	lan966x.regs[TARGET_ORG] = pci_iomap_range(pdev, LAN966X_SWITCH_BAR,
						   ORG_TARGET_OFFSET,
						   ORG_TARGET_LENGTH);

	/* Change endianness, this will be fix in the hardware,
	 * when getting a new drop, this should be removed
	 */
	LAN_WR(0x81818181, lan966x, ORG_IF_CTRL);

	/* Reset the switch */
	LAN_WR(0x1, lan966x, GCB_SOFT_RST);
	ret = readx_poll_timeout(lan966x_soft_reset, &lan966x,
				 val, val == 0, READL_SLEEP_US,
				 READL_TIMEOUT_US);
	if (ret)
		return ret;

	/* Change endianness, this will be fix in the hardware,
	 * when getting a new drop, this should be removed
	 */
	LAN_WR(0x81818181, lan966x, ORG_IF_CTRL);

	LAN_WR(0x0, lan966x, SYS_RESET_CFG);
	LAN_WR(0x2, lan966x, SYS_RAM_INIT);

	ret = readx_poll_timeout(lan966x_ram_init, &lan966x, val,
				 (val & BIT(1)) == 0, READL_SLEEP_US,
				 READL_TIMEOUT_US);
	if (ret)
		return ret;
	LAN_WR(0x1, lan966x, SYS_RESET_CFG);

	/* Enable interrupts */
	pci_set_master(pdev);

	/* Release the resets of the phys */
	for (i = 0; i < 5; ++i)
		LAN_WR(0x1, lan966x, GCB_GPIO_OUT(i));

	return of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
}

static void lan966x_pci_remove(struct pci_dev *pdev)
{
}

static struct pci_driver lan966x_pci_driver = {
	.name = "microchip_lan966x_pci_ad",
	.id_table = lan966x_ids,
	.probe = lan966x_pci_probe,
	.remove = lan966x_pci_remove,
};

module_pci_driver(lan966x_pci_driver);

MODULE_DESCRIPTION("Microchip LAN966X driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
