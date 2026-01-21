// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Microchip Sparx5 Switch Symreg support
 *
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/debugfs.h>

#define IORES_MAX 3
#define REGS_CELLS_MAX 9

struct sparx5_symreg_io {
	phys_addr_t start;
	phys_addr_t size;
	phys_addr_t end;
	const char *name;
};

struct sparx5_symreg {
	struct platform_device *pdev;
	struct device *dev;
	struct dentry *debugfs_root;
	int num_ranges;
	struct sparx5_symreg_io symreg_io[IORES_MAX];
	void __iomem *iomem[IORES_MAX];
};

#define REG_VALUE_SIZE 4

static ssize_t sparx5_symreg_debugfs_write(struct file *filp,
					   const char __user *buf, size_t count,
					   loff_t *pos)
{
	struct sparx5_symreg *sreg = filp->private_data;
	int iomax = sreg->num_ranges;
	phys_addr_t reg = *pos;
	int err = -EINVAL, idx;
	u8 *buf_dup;
	u32 val;

	if (count < REG_VALUE_SIZE)
		return err;

	buf_dup = memdup_user(buf, count);
	if (IS_ERR(buf_dup))
		return PTR_ERR(buf_dup);

	/* LE encoded output */
	val = (u32)buf_dup[0] << 0 |
	      (u32)buf_dup[1] << 8 |
	      (u32)buf_dup[2] << 16 |
	      (u32)buf_dup[3] << 24;

	kfree(buf_dup);

	pr_debug("%s:%d: %pa: 0x%08x\n", __func__, __LINE__, &reg, val);
	for (idx = 0; idx < iomax; idx++) {
		pr_debug("%s:%d: [%d]: [%pa, %pa] -> %p\n", __func__,
			__LINE__, idx,
			&sreg->symreg_io[idx].start,
			&sreg->symreg_io[idx].end,
			sreg->iomem[idx]);
		if (reg >= sreg->symreg_io[idx].start &&
		    reg < sreg->symreg_io[idx].end) {
			void __iomem *ioreg = sreg->iomem[idx] +
				(reg - sreg->symreg_io[idx].start);
			pr_debug("%s:%d: %pa -> 0x%p\n", __func__, __LINE__,
				&reg, ioreg);
			writel(val, ioreg);
			return REG_VALUE_SIZE;
		}
	}
	return err;
}

static ssize_t sparx5_symreg_debugfs_read(struct file *filp, char __user *buf,
					size_t count, loff_t *pos)
{
	struct sparx5_symreg *sreg = filp->private_data;
	int iomax = sreg->num_ranges;
	u8 tmp[REG_VALUE_SIZE] = { 0 };
	int err = -EINVAL, idx;
	phys_addr_t reg = *pos;
	u32 val;

	if (count < REG_VALUE_SIZE)
		return err;

	pr_debug("%s:%d: %pa\n", __func__, __LINE__, &reg);
	for (idx = 0; idx < iomax; idx++) {
		pr_debug("%s:%d: [%d]: [%pa, %pa] -> %p\n", __func__,
			__LINE__, idx,
			&sreg->symreg_io[idx].start,
			&sreg->symreg_io[idx].end,
			sreg->iomem[idx]);
		if (reg >= sreg->symreg_io[idx].start &&
		    reg < sreg->symreg_io[idx].end) {
			void __iomem *ioreg = sreg->iomem[idx] +
				(reg - sreg->symreg_io[idx].start);
			pr_debug("%s:%d: %pa -> 0x%p\n", __func__, __LINE__,
				&reg, ioreg);
			val = readl(ioreg);
			/* LE encoded output */
			tmp[3] = val >> 24;
			tmp[2] = val >> 16;
			tmp[1] = val >> 8;
			tmp[0] = val >> 0;
			err = copy_to_user(buf, tmp, sizeof(tmp));
			if (err) {
				err = -ENOMEM;
				break;
			}
			return sizeof(tmp);
		}
	}
	return err;
}

static int sparx5_symreg_read_chip_ranges(struct sparx5_symreg *sreg)
{
	int kdx, ret, addr_cells, size_cells, total_cells, num_ranges;
	struct device_node *dn = sreg->pdev->dev.of_node;
	u32 sr[REGS_CELLS_MAX] = { 0 };
	u64 addr, size;

	/* Get cell counts */
	addr_cells = of_n_addr_cells(dn);
	size_cells = of_n_size_cells(dn);

	/* Get total number of cells in the property */
	total_cells = of_property_count_elems_of_size(dn, "microchip,symreg", sizeof(u32));
	if (total_cells < 0) {
		dev_err(sreg->dev, "Failed to get total_cells\n");
		return total_cells;
	}
	if (total_cells > REGS_CELLS_MAX) {
		dev_err(sreg->dev, "Array too large\n");
		return -ERANGE;
	}

	/* Calculate number of ranges */
	num_ranges = total_cells / (addr_cells + size_cells);

	ret = of_property_read_u32_array(dn, "microchip,symreg", sr, total_cells);
	if (ret) {
		dev_err(sreg->dev, "Failed to get array elements\n");
		return ret;
	}

	kdx = 0;
	for (int idx = 0; idx < num_ranges; idx++) {
		addr = 0;
		for (int jdx = 0; jdx < addr_cells; jdx++)
			addr = (addr << 32) | sr[kdx++];
		size = 0;
		for (int jdx = 0; jdx < size_cells; jdx++)
			size = (size << 32) | sr[kdx++];
		pr_debug("%s:%d: Symreg Range %d: Address: %pa, Size: 0x%llx\n",
			__func__, __LINE__,
			idx, &addr,
			size);
		sreg->symreg_io[idx].start = addr;
		sreg->symreg_io[idx].size = size;
		sreg->symreg_io[idx].end = addr + size - 1;

	}
	sreg->num_ranges = num_ranges;
	return 0;
}

static int sparx5_symreg_add_ranges(struct sparx5_symreg *sreg)
{
	unsigned int rescount = sreg->pdev->num_resources;
	struct resource *res;
	int err;

	err = sparx5_symreg_read_chip_ranges(sreg);
	if (err < 0)
		return err;

	if (sreg->num_ranges != rescount) {
		dev_err(sreg->dev, "Mismatch in ranges\n");
		return -EINVAL;
	}

	for (int idx = 0; idx < rescount; idx++) {
		res = platform_get_resource(sreg->pdev, IORESOURCE_MEM, idx);
		if (!res) {
			dev_err(sreg->dev, "Invalid resource\n");
			return -EINVAL;
		}
		sreg->iomem[idx] =
			devm_ioremap(sreg->dev, res->start, resource_size(res));
		if (!sreg->iomem[idx]) {
			dev_err(sreg->dev, "Unable to get registers: %s\n",
				sreg->symreg_io[idx].name);
			return -ENOMEM;
		}
		sreg->symreg_io[idx].name = res->name;
		pr_debug("%s:%d: Symreg Range %d: %s: [%pa,%pa]: %pa -> 0x%p\n",
			__func__, __LINE__,
			idx,
			sreg->symreg_io[idx].name,
			&sreg->symreg_io[idx].start,
			&sreg->symreg_io[idx].end,
			&sreg->symreg_io[idx].size,
			sreg->iomem[idx]);
	}

	return 0;
}

static const struct file_operations memfops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= sparx5_symreg_debugfs_write,
	.read	= sparx5_symreg_debugfs_read,
};

static int sparx5_symreg_probe(struct platform_device *pdev)
{
	struct sparx5_symreg *sreg;

	sreg = devm_kzalloc(&pdev->dev, sizeof(*sreg), GFP_KERNEL);
	if (!sreg)
		return -ENOMEM;

	platform_set_drvdata(pdev, sreg);
	sreg->pdev = pdev;
	sreg->dev = &pdev->dev;

	sreg->debugfs_root = debugfs_create_dir("symreg", NULL);
	debugfs_create_file("mem", 0644, sreg->debugfs_root, sreg, &memfops);

	return sparx5_symreg_add_ranges(sreg);
}

static void sparx5_symreg_remove(struct platform_device *pdev)
{
	struct sparx5_symreg *sreg = platform_get_drvdata(pdev);

	debugfs_remove_recursive(sreg->debugfs_root);
}

static const struct of_device_id sparx5_symreg_match[] = {
	{ .compatible = "microchip,sparx5-symreg" },
	{ }
};
MODULE_DEVICE_TABLE(of, sparx5_symreg_match);

static struct platform_driver sparx5_symreg_driver = {
	.probe = sparx5_symreg_probe,
	.remove = sparx5_symreg_remove,
	.driver = {
		.name = "sparx5-symreg",
		.of_match_table = sparx5_symreg_match,
	},
};

module_platform_driver(sparx5_symreg_driver);

MODULE_DESCRIPTION("Microchip Sparx5 symreg driver");
MODULE_AUTHOR("Steen Hegelund <steen.hegelund@microchip.com>");
MODULE_LICENSE("Dual MIT/GPL");
