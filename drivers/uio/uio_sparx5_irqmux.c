// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi Sparx5 UIO driver
 *
 * Author: <lars.povlsen@microchip.com>
 * License: Dual MIT/GPL
 * Copyright (c) 2019 Microchip Corporation
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/sysfs.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/reset.h>

#define DRIVER_NAME	"uio_sparx5_irqmux"
#define MAXNAMELEN	16

struct slave_irq_data {
	int index;
	int irq;
	bool active;
	bool disabled;
	char name[MAXNAMELEN];
	struct irqmux_platdata *priv;
};

struct irqmux_platdata {
	struct uio_info info;
	spinlock_t lock;
	unsigned long flags;
	struct platform_device *pdev;
#define MASTER_IRQ 0		/* 0 is the master */
	struct slave_irq_data *sirq;
	int n_sirq;
	int n_active;
	bool io_enabled;
};

/* Bits in irqmux_platdata.flags */
enum {
	UIO_IRQ_DISABLED = 0,
};

static void uio_irq_trigger_master(const struct irqmux_platdata *priv)
{
	int err, master_irq = priv->sirq[MASTER_IRQ].irq;

	dev_dbg(&priv->pdev->dev, "Trigger master IRQ - %d\n", master_irq);
	err = irq_set_irqchip_state(master_irq,
				    IRQCHIP_STATE_PENDING, true);
	if (err)
		dev_err(&priv->pdev->dev, "Unable to trigger master IRQ %d\n",
			master_irq);
}

static ssize_t irqctl_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct irqmux_platdata *priv = platform_get_drvdata(pdev);
	int i;
	ssize_t n, nwritten = 0;

	for (i = 0; i < priv->n_sirq; i++) {
		if (priv->sirq[i].active && !priv->sirq[i].disabled) {
			n = sprintf(buf, "%d|%s\n", i, priv->sirq[i].name);
			nwritten += n;
			buf += n;
		}
	}

#if defined(DEBUG)
	nwritten += sprintf(buf, "Active: %d\n", priv->n_active);
#endif

	return nwritten;
}

static bool valid_index(struct irqmux_platdata *priv, int index)
{
	return (index > MASTER_IRQ && index < priv->n_sirq);
}

static ssize_t irqctl_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct irqmux_platdata *priv = platform_get_drvdata(pdev);
	int index;
	unsigned long flags;

	if (kstrtoint(buf, 0, &index))
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	/* Allow [1; max-1] - 0 is master (reserved) */
	if (valid_index(priv, index)) {
		if (priv->sirq[index].active) {
			dev_dbg(dev, "Enable IRQ %d = %s, #active = %d\n",
				index, priv->sirq[index].name,
				priv->n_active);
			priv->sirq[index].active = false;
			priv->n_active--;
			if (priv->n_active > 0)
				/* Keep triggering */
				uio_irq_trigger_master(priv);
			enable_irq(priv->sirq[index].irq);
		} else
			dev_warn(dev, "Interrupt %d already inactive\n", index);
	} else if (index < 0 && valid_index(priv, abs(index))) {
		index = abs(index);
		dev_warn(dev, "Disabling index: %d\n", index);
		priv->sirq[index].disabled = true;
		if (priv->sirq[index].active) {
			priv->sirq[index].active = false;
			priv->n_active--;
		}
		/* Disable to be sure its muted */
		disable_irq(priv->sirq[index].irq);
	} else {
		dev_warn(dev, "Illegal interrupt index: %d\n", index);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return count;
}

static DEVICE_ATTR_RW(irqctl);

static struct attribute *irqmux_attrs[] = {
	&dev_attr_irqctl.attr,
	NULL
};

static struct attribute_group attr_group = {
	.attrs = irqmux_attrs,
};

static int uio_sparx5_irqmux_open(struct uio_info *info, struct inode *inode)
{
	return 0;
}

static int uio_sparx5_irqmux_release(struct uio_info *info,
				      struct inode *inode)
{
	//struct irqmux_platdata *priv = info->priv;
	return 0;
}

static irqreturn_t uio_sparx5_irqmux_handler(int irq,
					      struct uio_info *dev_info)
{
	struct irqmux_platdata *priv = dev_info->priv;

	/* Just disable the interrupt in the interrupt controller, and
	 * remember the state so we can allow user space to enable it later.
	 */

	spin_lock(&priv->lock);
	if (!__test_and_set_bit(UIO_IRQ_DISABLED, &priv->flags))
		disable_irq_nosync(irq);
	spin_unlock(&priv->lock);

	return IRQ_HANDLED;
}

static int uio_sparx5_irqmux_irqcontrol(struct uio_info *dev_info, s32 irq_on)
{
	struct irqmux_platdata *priv = dev_info->priv;
	unsigned long flags;

	/* Allow user space to enable and disable the interrupt
	 * in the interrupt controller, but keep track of the
	 * state to prevent per-irq depth damage.
	 *
	 * Serialize this operation to support multiple tasks and concurrency
	 * with irq handler on SMP systems.
	 */

	spin_lock_irqsave(&priv->lock, flags);
	if (irq_on) {
		if (__test_and_clear_bit(UIO_IRQ_DISABLED, &priv->flags))
			enable_irq(dev_info->irq);
	} else {
		if (!__test_and_set_bit(UIO_IRQ_DISABLED, &priv->flags))
			disable_irq_nosync(dev_info->irq);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static irqreturn_t slave_irq(int irq, void *ident)
{
	struct slave_irq_data *sirq = ident;
	struct irqmux_platdata *priv = sirq->priv;
	unsigned long flags;

	dev_dbg(&priv->pdev->dev, "IRQ %d: %s fired\n", irq, sirq->name);

	spin_lock_irqsave(&priv->lock, flags);
	if (!sirq->active) {
		sirq->active = true;
		priv->n_active++;
		disable_irq_nosync(irq);
		uio_irq_trigger_master(priv);
	} else {
		dev_err(&priv->pdev->dev, "Got IRQ %d while already active\n",
			sirq->index);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return IRQ_HANDLED;
}

static int uio_sparx5_irqmux_request_irqs(struct irqmux_platdata *priv)
{
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	int ret, irq, num, max;
	struct resource irq_res;
	const char *irq_name;

	priv->n_sirq = platform_irq_count(pdev);
	priv->sirq = devm_kzalloc(dev,
				  sizeof(struct slave_irq_data)*priv->n_sirq,
				  GFP_KERNEL);
	if (!priv->sirq)
		return -ENOMEM;

	for (num = 0, max = priv->n_sirq; num < max; num++) {
		ret = of_irq_to_resource(dev->of_node, num, &irq_res);
		if (ret <= 0) {
			dev_err(dev, "failed to get IRQ %d resource: %d\n", num, ret);
			return ret;
		}
		irq = irq_res.start;
		irq_name = irq_res.name;
		priv->sirq[num].index = num;
		priv->sirq[num].irq = irq;
		priv->sirq[num].priv = priv;
		strncpy(priv->sirq[num].name, irq_name, MAXNAMELEN);
		if (num == 0) {
			priv->info.irq = irq;
			if (strcmp(irq_name, "master")) {
				dev_err(dev, "First irq must be 'master'\n");
				return -EINVAL;
			}
		} else {
			ret = devm_request_irq(dev, irq, slave_irq, 0, irq_name,
					       (void *) &priv->sirq[num]);
			if (ret < 0) {
				dev_err(dev, "%s: can not get IRQ %d\n", irq_name, irq);
				return ret;
			}
		}
	}

	dev_info(dev, "Mapped %d irqs\n", priv->n_sirq);
	return 0;
}

static int uio_sparx5_irqmux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct irqmux_platdata *priv;
	struct reset_control *reset;
	int ret = -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	reset = devm_reset_control_get_optional_shared(&pdev->dev, NULL);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "Failed to get reset\n");
	reset_control_reset(reset);

	spin_lock_init(&priv->lock);
	priv->flags = 0; /* interrupt is enabled to begin with */
	priv->pdev = pdev;

	priv->io_enabled = !of_property_read_bool(pdev->dev.of_node,
						  "external-cpu");
	if (priv->io_enabled)
		dev_info(dev, "IO is enabled\n");
	else
		dev_info(dev, "IO is disabled, using external CPU\n");

	ret = uio_sparx5_irqmux_request_irqs(priv);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get IRQs: %d\n", ret);
		return ret;
	}

	priv->info.name = pdev->dev.of_node->name;
	priv->info.version = "devicetree";
	if (priv->io_enabled) {
		int i;
		const struct resource *res;

		for (i = 0, res = &pdev->resource[0];
		     resource_type(res) == IORESOURCE_MEM; i++, res++) {
			size_t sz = resource_size(res);

			priv->info.mem[i].memtype = UIO_MEM_PHYS;
			priv->info.mem[i].addr = res->start;
			priv->info.mem[i].size = sz;
			priv->info.mem[i].name = res->name;
			priv->info.mem[i].internal_addr =
				devm_ioremap(dev, priv->info.mem[i].addr,
					     priv->info.mem[i].size);
			if (!priv->info.mem[i].internal_addr) {
				dev_err(dev,
					"failed to map chip region %d sz %zd\n",
					i, sz);
				return -ENODEV;
			}
		}
	}

	priv->info.handler = uio_sparx5_irqmux_handler;
	priv->info.irqcontrol = uio_sparx5_irqmux_irqcontrol;
	priv->info.open = uio_sparx5_irqmux_open;
	priv->info.release = uio_sparx5_irqmux_release;
	priv->info.priv = priv;

	ret = uio_register_device(dev, &priv->info);
	if (ret) {
		dev_err(dev, "unable to register uio device\n");
		return ret;
	}

	if (sysfs_create_group(&dev->kobj, &attr_group))
		dev_err(dev, "sysfs register error\n");

	platform_set_drvdata(pdev, priv);
	dev_info(dev, "Mapping %pR\n", &pdev->resource[0]);
	return 0;
}

static int uio_sparx5_irqmux_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct irqmux_platdata *priv = platform_get_drvdata(pdev);

	uio_unregister_device(&priv->info);

	priv->info.handler = NULL;
	priv->info.irqcontrol = NULL;

	sysfs_remove_group(&dev->kobj, &attr_group);

	return 0;
}

static const struct of_device_id uio_of_sparx5_irqmux_match[] = {
	{ .compatible = "microchip,uio_sparx5_irqmux" },
	{ .compatible = "mscc,uio_jaguar2_irqmux" },
	{ .compatible = "mscc,uio_ocelot_irqmux" },
	{ .compatible = "mscc,uio_luton_irqmux" },
	{ .compatible = "microchip,uio_lan966x_irqmux" },
	{ .compatible = "microchip,uio_lan969x_irqmux" },
	{},
};

static struct platform_driver uio_sparx5_irqmux = {
	.probe = uio_sparx5_irqmux_probe,
	.remove = uio_sparx5_irqmux_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = of_match_ptr(uio_of_sparx5_irqmux_match),
	},
};

module_platform_driver(uio_sparx5_irqmux);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microchip.com>");
