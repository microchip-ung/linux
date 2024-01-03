/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Microchip LAN966x PCI irqchip driver
 */

#ifndef MASERATI_PCI_IRQ_H
#define MASERATI_PCI_IRQ_H

#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/platform_device.h>

struct lan966x_irq_data {
	struct irq_domain *domain;
	void __iomem *regs;
};

int lan966x_pci_irq_setup(struct pci_dev *pdev, struct device_node *node,
			  struct lan966x_irq_data *data);
void lan966x_pci_irq_remove(struct pci_dev *pdev, struct lan966x_irq_data *data);

#endif
