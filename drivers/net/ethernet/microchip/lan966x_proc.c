// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "lan966x_main.h"

unsigned long rx_counters;

static int proc_counters_(struct seq_file *f, void *v)
{
	seq_printf(f, "rx_counters: %lx\n", rx_counters);

	return 0;
}

static int proc_counters(struct inode *inode, struct file *f)
{
	return single_open(f, proc_counters_, NULL);
}

static ssize_t lan966x_proc_write(struct file *f, const char __user *buff,
				  size_t sz, loff_t *loff)
{
	rx_counters = 0;
	return sz;
}

static struct proc_dir_entry *proc_ent;
static const struct proc_ops proc_ops = {
	.proc_open = proc_counters,
	.proc_write = lan966x_proc_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

void lan966x_proc_register_dbg(struct lan966x *lan966x)
{
	rx_counters = 0;
	proc_ent = proc_create_data("lan966x_count", 0444, NULL, &proc_ops,
				    lan966x);
}

void lan966x_proc_unregister_dbg(void)
{
	remove_proc_entry("lan966x_count", NULL);
}
