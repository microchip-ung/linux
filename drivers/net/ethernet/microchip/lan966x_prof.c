// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include "lan966x_main.h"

static void lan966x_prof_sample_init(struct lan966x *lan966x,
				     struct lan966x_prof_stat *stat,
				     int samples, char *name)
{
	stat->count = 0;
	stat->name = name;
	stat->min = ~0;
	stat->max = 0;
	stat->samples_size = samples;
	stat->samples = devm_kzalloc(lan966x->dev, samples * sizeof(u64),
				     GFP_KERNEL);
}

void lan966x_prof_sample_begin(struct lan966x_prof_stat *stat)
{
	stat->last = ktime_get_ns();
}

void lan966x_prof_sample_end(struct lan966x_prof_stat *stat)
{
	u64 diff = ktime_get_ns() - stat->last;
	stat->samples[stat->count] = diff;
	stat->count = (stat->count + 1) % stat->samples_size;

	if (diff > stat->max) {
		stat->max = diff;
	}
	if (diff < stat->min) {
		stat->min = diff;
	}
}

static u64 lan966x_prof_sample_avg(struct lan966x_prof_stat *stat)
{
	u64 sum = 0, res = 0;
	int cnt = 0;
	int i = 0;

	for (i = 0; i < stat->samples_size; ++i){
		if (stat->samples[i] != 0) {
			cnt++;
			sum += stat->samples[i];
		}
	}

	res = sum;
	do_div(res, cnt);
	return res;
}

static int lan966x_prof_sample_dbgfs(struct seq_file *file, void *offset)
{
	struct device *dev = file->private;
	struct lan966x *lan966x = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < LAN966X_PROFILE_MAX; ++i) {
		struct lan966x_prof_stat *stat = &lan966x->prof_stat[i];

		seq_printf(file, "%s min ns: %llu, max ns: %lld, avg ns: %lld\n",
			   stat->name, stat->min, stat->max,
			   lan966x_prof_sample_avg(stat));
	}

	return 0;
}

void lan966x_prof_init_dbgfs(struct lan966x *lan966x)
{
	debugfs_create_devm_seqfile(lan966x->dev, "samples",
				    lan966x->debugfs_root,
				    lan966x_prof_sample_dbgfs);

	lan966x_prof_sample_init(lan966x,
				 &lan966x->prof_stat[LAN966X_PROFILE_MAC_IRQ],
				 20000, "mac irq");
}

void lan966x_prof_remove_dbgfs(struct lan966x *lan966x)
{
	int i;

	for (i = 0; i < LAN966X_PROFILE_MAX; ++i) {
		struct lan966x_prof_stat *stat = &lan966x->prof_stat[i];
		devm_kfree(lan966x->dev, stat->samples);
	}
}
