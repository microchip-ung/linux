// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi VCoreIII bitbang SPI driver
 *
 * Copyright (c) 2019 Microsemi Corporation
 */

#include <linux/platform_device.h>
#include <linux/mtd/spi-nor.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>

//#define DEBUG

#if defined(DEBUG)
#define bb_dbg(dev, ...)	dev_info(dev, ##__VA_ARGS__)
#else
#define bb_dbg(dev, ...)	/* Go away */
#endif

#define ICPU_SW_MODE_SW_PIN_CTRL_MODE                     BIT(13)
#define ICPU_SW_MODE_SW_SPI_SCK                           BIT(12)
#define ICPU_SW_MODE_SW_SPI_SCK_OE                        BIT(11)
#define ICPU_SW_MODE_SW_SPI_SDO                           BIT(10)
#define ICPU_SW_MODE_SW_SPI_SDO_OE                        BIT(9)
#define ICPU_SW_MODE_SW_SPI_CS(x)                         (((x) << 5) & GENMASK(8, 5))
#define ICPU_SW_MODE_SW_SPI_CS_M                          GENMASK(8, 5)
#define ICPU_SW_MODE_SW_SPI_CS_X(x)                       (((x) & GENMASK(8, 5)) >> 5)
#define ICPU_SW_MODE_SW_SPI_CS_OE(x)                      (((x) << 1) & GENMASK(4, 1))
#define ICPU_SW_MODE_SW_SPI_CS_OE_M                       GENMASK(4, 1)
#define ICPU_SW_MODE_SW_SPI_CS_OE_X(x)                    (((x) & GENMASK(4, 1)) >> 1)
#define ICPU_SW_MODE_SW_SPI_SDI                           BIT(0)

#define MAX_CS		4
#define BOOTMASTER_CS	0

struct spi_vcoreiii {
	void __iomem *regs;	/* Bitbang register */
	void __iomem *read_map;	/* Memory mapped read window */
        u32 deactivate_delay;
	u8 cs_num;
	u32 svalue;                     /* Value to start transfer with */
	u32 clk1;                       /* Clock value start */
	u32 clk2;                       /* Clock value 2nd phase */
	void (*hold_time_delay)(void);	/* Hold time pause */
};

#define DELAY_8_NOPS() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")

static void hold_time_delay_nop_1_4khz(void)
{
	int i;
	for (i = 0; i < 8; i++)
		DELAY_8_NOPS();
}

static void hold_time_delay_1us(void)
{
	udelay(1);
}

static void inline vcoreiii_bb_writel_hold(struct spi_vcoreiii *priv, u32 value)
{
	__raw_writel(value, priv->regs);
	wmb();
        /* Hold time delay, if set */
	if (unlikely(priv->hold_time_delay))
		priv->hold_time_delay();
}

static int vcoreiii_bb_exec_mem_op(struct spi_mem *mem,
				   const struct spi_mem_op *op)
{
	int ret = -ENOTSUPP;

	/* Only reads, addrsize 1..4 */
	if (!op->data.nbytes || !op->addr.nbytes || op->addr.nbytes > 4 ||
	    op->data.dir != SPI_MEM_DATA_IN)
		return ret;

	/* Only handle (normal+fast) 3/4 bytes read */
	if (op->cmd.opcode != SPINOR_OP_READ &&
	    op->cmd.opcode != SPINOR_OP_READ_FAST &&
	    op->cmd.opcode != SPINOR_OP_READ_4B &&
	    op->cmd.opcode != SPINOR_OP_READ_FAST_4B)
		return ret;

	/* Only 16M reach */
	if ((op->addr.val + op->data.nbytes) < SZ_16M) {
		struct spi_device *spi = mem->spi;
		struct spi_vcoreiii *p = spi_master_get_devdata(spi->master);
		u8 __iomem *src = p->read_map + (spi->chip_select * SZ_16M) + op->addr.val;

		if (spi->chip_select != BOOTMASTER_CS)
			return ret;

		memcpy(op->data.buf.in, src, op->data.nbytes);
		ret = op->data.nbytes;
	}

	return ret;
}

static const struct spi_controller_mem_ops vcoreiii_bb_mem_ops = {
	.exec_op = vcoreiii_bb_exec_mem_op,
};

static void vcoreiii_bb_cs_gpio(struct spi_device *spi, bool start)
{
	/* Activate/deactivate observing polarity */
	bool cs_value = (spi->mode & SPI_CS_HIGH) ? start : !start;
	if (spi->cs_gpiod) {
		gpiod_direction_output(spi->cs_gpiod, cs_value);
	}
}

static void vcoreiii_bb_cs_activate(struct spi_vcoreiii *priv, struct spi_device *spi)
{
	u32 cpha = spi->mode & SPI_CPHA;

	priv->cs_num = spi->chip_select;

	if (cpha) {
		/* Initial clock starts SCK=1 */
		priv->clk1 = ICPU_SW_MODE_SW_SPI_SCK;
		priv->clk2 = 0;
	} else {
		/* Initial clock starts SCK=0 */
		priv->clk1 = 0;
		priv->clk2 = ICPU_SW_MODE_SW_SPI_SCK;
	}

	/* Enable bitbang, SCK_OE, SDO_OE */
	priv->svalue = (ICPU_SW_MODE_SW_PIN_CTRL_MODE | /* Bitbang */
			ICPU_SW_MODE_SW_SPI_SCK_OE    | /* SCK_OE */
			ICPU_SW_MODE_SW_SPI_SDO_OE);   /* SDO OE */

	/* Add CS */
	if (spi->cs_gpiod )
		vcoreiii_bb_cs_gpio(spi, true);
	else
		priv->svalue |=
			ICPU_SW_MODE_SW_SPI_CS_OE(BIT(spi->chip_select)) |
			ICPU_SW_MODE_SW_SPI_CS(BIT(spi->chip_select));

	/* Crude speed setup */
	if (spi->max_speed_hz > 3500000) {
		priv->hold_time_delay = NULL;
	} else if (spi->max_speed_hz > 1400000) {
		priv->hold_time_delay = hold_time_delay_nop_1_4khz;
	} else {
		/* Appx. 422KHz */
		priv->hold_time_delay = hold_time_delay_1us;
	}

	/* Enable the CS in HW, Initial clock value */
	vcoreiii_bb_writel_hold(priv, priv->svalue | priv->clk1);
}

static void vcoreiii_bb_cs_deactivate(struct spi_vcoreiii *priv, struct spi_device *spi)
{
	/* Keep driving the CLK to its current value while
	 * actively deselecting CS.
	 */
	u32 value = readl(priv->regs);

	/* Drop CS */
	if (spi->cs_gpiod)
		vcoreiii_bb_cs_gpio(spi, false);
	value &= ~ICPU_SW_MODE_SW_SPI_CS_M;
	vcoreiii_bb_writel_hold(priv, value);

	/* Deselect hold time delay */
	if (unlikely(priv->deactivate_delay))
		udelay(priv->deactivate_delay);

	/* Drop everything */
	vcoreiii_bb_writel_hold(priv, 0);

	bb_dbg(&spi->dev, "Deactivated CS%d\n", priv->cs_num);
}

static void vcoreiii_bb_do_transfer(struct spi_vcoreiii *priv,
				    struct spi_message *msg,
				    struct spi_transfer *xfer,
				    bool last_xfer)
{
        u32             i, count = xfer->len;
	const u8        *txd = xfer->tx_buf;
	u8              *rxd = xfer->rx_buf;
	unsigned long   __flags;

	local_irq_save(__flags);
	for (i = 0; i < count; i++) {
		u32 rx = 0, mask = 0x80, value;
		while (mask) {
			/* Initial condition: CLK is low/hi per mode setting. */
			value = priv->svalue | ICPU_SW_MODE_SW_SPI_SDO_OE;
			if (txd && txd[i] & mask)
				value |= ICPU_SW_MODE_SW_SPI_SDO;

			/* Drive data while taking CLK low. The device
                         * we're accessing will sample on the
                         * following rising edge and will output data
                         * on this edge for us to be sampled at the
                         * end of this loop.
                         */
			vcoreiii_bb_writel_hold(priv, value | priv->clk1);

			/* Drive the clock high. */
			vcoreiii_bb_writel_hold(priv, value | priv->clk2);

			/* We sample as close to the next falling edge
			 * as possible.
			 */
			value = __raw_readl(priv->regs);
			if (value & ICPU_SW_MODE_SW_SPI_SDI)
				rx |= mask;

			/* Next bit */
			mask >>= 1;
		}
		if (rxd) {
			bb_dbg(&msg->spi->dev, "Read 0x%02x\n", rx);
			rxd[i] = (u8)rx;
		}
		bb_dbg(&msg->spi->dev, "spi_xfer: byte %d/%d\n", i + 1, count);
	}
	local_irq_restore(__flags);
	bb_dbg(&msg->spi->dev, "spi_xfer: done\n");
}

int vcoreiii_bb_transfer_one_message(struct spi_master *master,
				     struct spi_message *msg)
{
	struct spi_vcoreiii *p = spi_master_get_devdata(master);
	struct spi_device *spi = msg->spi;
	unsigned int total_len = 0;
	struct spi_transfer *xfer;

	vcoreiii_bb_cs_activate(p, spi);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		bool last_xfer = list_is_last(&xfer->transfer_list,
					      &msg->transfers);
		vcoreiii_bb_do_transfer(p, msg, xfer, last_xfer);
		total_len += xfer->len;
	}

	vcoreiii_bb_cs_deactivate(p, spi);

	msg->status = 0;
	msg->actual_length = total_len;
	spi_finalize_current_message(master);
	return msg->status;
}

static int vcoreiii_bb_probe(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *ptr;
	struct spi_master *master;
	struct spi_vcoreiii *p;
	int err = -ENOENT;

	master = spi_alloc_master(&pdev->dev, sizeof(struct spi_vcoreiii));
	if (!master)
		return -ENOMEM;
	p = spi_master_get_devdata(master);
	platform_set_drvdata(pdev, master);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ptr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ptr)) {
		err = PTR_ERR(ptr);
		goto fail;
	}

	p->regs = ptr;
	if (device_property_read_u32(&pdev->dev, "spi-deactivate-delay", &p->deactivate_delay))
		p->deactivate_delay = 0;

	master->mode_bits = SPI_CPHA | SPI_CPOL | SPI_CS_HIGH;
	master->use_gpio_descriptors = true;
	master->num_chipselect = MAX_CS;

	master->transfer_one_message = vcoreiii_bb_transfer_one_message;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res && resource_size(res) >= (SZ_16M*MAX_CS)) {
		ptr = devm_ioremap_resource(&pdev->dev, res);
		if (!IS_ERR(ptr)) {
			p->read_map = ptr;
			master->mem_ops = &vcoreiii_bb_mem_ops;
			dev_info(&pdev->dev, "Enabling fast memory operations\n");
		}
	}

	dev_info(&pdev->dev, "vcoreiii bitbang SPI bus driver\n");

	master->dev.of_node = pdev->dev.of_node;
	err = devm_spi_register_master(&pdev->dev, master);
	if (err) {
		dev_err(&pdev->dev, "register master failed: %d\n", err);
		goto fail;
	}

	return 0;
fail:
	spi_master_put(master);
	return err;
}

static int vcoreiii_bb_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);
	struct spi_vcoreiii *priv = spi_master_get_devdata(master);

	/* Clear everything in a known state. */
	vcoreiii_bb_writel_hold(priv, 0);

	return 0;
}

static const struct of_device_id vcoreiii_bb_match[] = {
	{ .compatible = "mscc,luton-bb-spi", },
	{},
};
MODULE_DEVICE_TABLE(of, vcoreiii_bb_match);

static struct platform_driver vcoreiii_bb_driver = {
	.driver = {
		.name		= "spi-vcoreiii",
		.of_match_table = vcoreiii_bb_match,
	},
	.probe		= vcoreiii_bb_probe,
	.remove		= vcoreiii_bb_remove,
};

module_platform_driver(vcoreiii_bb_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microchip.com>");
MODULE_DESCRIPTION("Microsemi VCore-III bitbang SPI bus driver");
