// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/debugfs.h>

#include "lan9645x_main.h"

struct lan9645x_bum_bool_dbfs_priv {
	struct list_head list;
	struct lan9645x_bum_pol *bpol;
	bool *val;
};

static const char *lan9645x_bum_type_name(enum lan9645x_bum_type type)
{
	switch (type) {
	case LAN9645X_BUM_UC:
		return "uc";
	case LAN9645X_BUM_BC:
		return "bc";
	case LAN9645X_BUM_MC:
		return "mc";
	default:
		return "unknown";
	}
}

static void lan9645x_bum_lock(struct lan9645x_bum_pol *bpol)
{
	mutex_lock(&bpol->lan9645x->bum->bum_lock);
}

static void lan9645x_bum_unlock(struct lan9645x_bum_pol *bpol)
{
	mutex_unlock(&bpol->lan9645x->bum->bum_lock);
}

static int lan9645x_bum_get_fps(int rate, int unit)
{
	return (1 << rate) * (unit ? 1 : 1000);
}

static int lan9645x_bum_get_burst_fps(int burst)
{
	return 1 << burst;
}

static int lan9645x_bum_get_closest_burst(struct lan9645x_bum_ctrl *bctrl,
					  int frames_per_second)
{
	int best_burst, dist;
	int best_fps_dist = INT_MAX;

	/* Max burst is 1<<12 */
	for (int burst = 0; burst < 13; burst++) {
		dist = abs(lan9645x_bum_get_burst_fps(burst) -
			   frames_per_second);
		if (dist < best_fps_dist) {
			best_burst = burst;
			best_fps_dist = dist;
		}
	}

	bctrl->burst = best_burst;
	return lan9645x_bum_get_burst_fps(bctrl->burst);
}

static int lan9645x_bum_get_closest_rate(struct lan9645x_bum_pol *bpol,
					 int frames_per_second)
{
	int best_unit, best_rate, dist;
	int best_fps_dist = INT_MAX;

	for (int rate = 0; rate < 11; rate++) {
		for (int unit = 0; unit < 2; unit++) {
			dist = abs(lan9645x_bum_get_fps(rate, unit) -
				   frames_per_second);
			if (dist < best_fps_dist) {
				best_unit = unit;
				best_rate = rate;
				best_fps_dist = dist;
			}
		}
	}

	bpol->unit = best_unit;
	bpol->rate = best_rate;
	return lan9645x_bum_get_fps(bpol->rate, bpol->unit);
}

static void lan9645x_bum_pol_apply(struct lan9645x_bum_pol *bpol)
{
	struct lan9645x *lan9645x = bpol->lan9645x;
	u32 mask, ipmc_mask;

	mask = ((u32)bpol->known_ena << 1) | bpol->unknown_ena;
	ipmc_mask = ((u32)bpol->ipmc_known_ena << 1) | bpol->ipmc_unknown_ena;

	switch (bpol->type) {
	case LAN9645X_BUM_UC:
		lan_rmw(ANA_STORM_CFG_STORM_UC_MASK_SET(mask),
			ANA_STORM_CFG_STORM_UC_MASK, lan9645x,
			ANA_STORM_CFG);
		break;
	case LAN9645X_BUM_BC:
		lan_rmw(ANA_STORM_CFG_STORM_BC_MASK_SET(mask),
			ANA_STORM_CFG_STORM_BC_MASK, lan9645x,
			ANA_STORM_CFG);
		break;
	case LAN9645X_BUM_MC:
		lan_rmw(ANA_STORM_CFG_STORM_MC_MASK_SET(mask) |
			ANA_STORM_CFG_STORM_IPMC_MASK_SET(ipmc_mask),
			ANA_STORM_CFG_STORM_MC_MASK |
			ANA_STORM_CFG_STORM_IPMC_MASK,
			lan9645x,
			ANA_STORM_CFG);
		break;
	default:
		break;
	}

	lan_wr(QSYS_STORMLIM_CFG_STORM_CPU_REDIR_SET(bpol->cpu_redir_ena) |
	       QSYS_STORMLIM_CFG_STORM_RATE_SET(bpol->rate) |
	       QSYS_STORMLIM_CFG_STORM_UNIT_SET(bpol->unit) |
	       QSYS_STORMLIM_CFG_STORM_MODE_SET(bpol->mode),
	       lan9645x, QSYS_STORMLIM_CFG(bpol->type));
}

static int debugfs_fps_set(void *data, u64 val)
{
	struct lan9645x_bum_pol *bpol = data;

	lan9645x_bum_lock(bpol);
	lan9645x_bum_get_closest_rate(bpol, val);
	lan9645x_bum_pol_apply(bpol);
	lan9645x_bum_unlock(bpol);
	return 0;
}
static int debugfs_fps_get(void *data, u64 *val)
{
	struct lan9645x_bum_pol *bpol = data;

	lan9645x_bum_lock(bpol);
	*val = lan9645x_bum_get_fps(bpol->rate, bpol->unit);
	lan9645x_bum_unlock(bpol);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_fps, debugfs_fps_get, debugfs_fps_set, "%llu\n");

static int debugfs_mode_set(void *data, u64 val)
{
	struct lan9645x_bum_pol *bpol = data;

	lan9645x_bum_lock(bpol);
	bpol->mode = val & 0x3;
	lan9645x_bum_pol_apply(bpol);
	lan9645x_bum_unlock(bpol);
	return 0;
}
static int debugfs_mode_get(void *data, u64 *val)
{
	struct lan9645x_bum_pol *bpol = data;

	lan9645x_bum_lock(bpol);
	*val = bpol->mode;
	lan9645x_bum_unlock(bpol);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_mode, debugfs_mode_get, debugfs_mode_set, "%llu\n");

static int debugfs_burst_set(void *data, u64 val)
{
	struct lan9645x_bum_ctrl *bctrl = data;

	mutex_lock(&bctrl->bum_lock);
	lan9645x_bum_get_closest_burst(bctrl, val);
	lan_wr(QSYS_STORMLIM_BURST_STORM_BURST_SET(bctrl->burst),
	       bctrl->lan9645x, QSYS_STORMLIM_BURST);
	mutex_unlock(&bctrl->bum_lock);
	return 0;
}
static int debugfs_burst_get(void *data, u64 *val)
{
	struct lan9645x_bum_ctrl *bctrl = data;

	mutex_lock(&bctrl->bum_lock);
	*val = lan9645x_bum_get_burst_fps(bctrl->burst);
	mutex_unlock(&bctrl->bum_lock);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_burst, debugfs_burst_get, debugfs_burst_set, "%llu\n");

static ssize_t debugfs_read_file_bum_bool(struct file *file,
					  char __user *user_buf, size_t count,
					  loff_t *ppos)
{
	struct dentry *dentry = file->f_path.dentry;
	struct lan9645x_bum_bool_dbfs_priv *data;
	char buf[2];
	bool val;
	int r;

	data = file->private_data;

	r = debugfs_file_get(dentry);
	if (unlikely(r))
		return r;
	lan9645x_bum_lock(data->bpol);
	val = *data->val;
	debugfs_file_put(dentry);
	lan9645x_bum_unlock(data->bpol);

	if (val)
		buf[0] = 'Y';
	else
		buf[0] = 'N';
	buf[1] = '\n';
	return simple_read_from_buffer(user_buf, count, ppos, buf, 2);
}

static ssize_t debugfs_write_file_bum_bool(struct file *file,
					   const char __user *user_buf,
					   size_t count, loff_t *ppos)
{
	struct dentry *dentry = file->f_path.dentry;
	struct lan9645x_bum_bool_dbfs_priv *data;
	bool bv;
	int r;

	data = file->private_data;

	r = kstrtobool_from_user(user_buf, count, &bv);
	if (!r) {
		r = debugfs_file_get(dentry);
		if (unlikely(r))
			return r;
		lan9645x_bum_lock(data->bpol);
		*data->val = bv;
		lan9645x_bum_pol_apply(data->bpol);
		debugfs_file_put(dentry);
		lan9645x_bum_unlock(data->bpol);
	}

	return count;
}

static const struct file_operations fops_bool = {
	.read = debugfs_read_file_bum_bool,
	.write = debugfs_write_file_bum_bool,
	.open = simple_open,
	.llseek = default_llseek,
};

static int lan9645x_stats_show(struct seq_file *s, void *unused)
{
	struct lan9645x_bum_ctrl *bctrl = s->private;
	struct lan9645x *lan9645x = bctrl->lan9645x;

	mutex_lock(&bctrl->bum_lock);
	seq_printf(s, "UC: %10d\n",
		   lan_rd(lan9645x, QSYS_STORMLIM_STAT(LAN9645X_BUM_UC)));
	seq_printf(s, "BC: %10d\n",
		   lan_rd(lan9645x, QSYS_STORMLIM_STAT(LAN9645X_BUM_BC)));
	seq_printf(s, "MC: %10d\n",
		   lan_rd(lan9645x, QSYS_STORMLIM_STAT(LAN9645X_BUM_MC)));
	mutex_unlock(&bctrl->bum_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan9645x_stats);

static int lan9645x_bum_bool_dbfs_create(struct lan9645x_bum_pol *bpol,
					 struct dentry *bum_pol_root,
					 char *name, bool *field)
{
	struct lan9645x_bum_bool_dbfs_priv *d;
	struct lan9645x_bum_ctrl *bctrl;

	bctrl = bpol->lan9645x->bum;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->bpol = bpol;
	d->val = field;
	list_add_tail(&d->list, &bctrl->debugfs_list);
	debugfs_create_file(name, 0666, bum_pol_root, d, &fops_bool);

	return 0;
}

static int lan9645x_bpol_dbfs_add(struct lan9645x *lan9645x,
				  struct dentry *bum_root,
				  struct lan9645x_bum_ctrl *bctrl,
				  enum lan9645x_bum_type type)
{
	struct lan9645x_bum_pol *bpol;
	struct dentry *bpol_root;
	int err;

	bpol = &bctrl->policers[type];

	bpol_root = debugfs_create_dir(lan9645x_bum_type_name(type), bum_root);
	if (IS_ERR(bpol_root))
		return PTR_ERR(bpol_root);

	debugfs_create_file("frames_per_second", 0666, bpol_root, bpol,
			    &fops_fps);
	debugfs_create_file("mode", 0666, bpol_root, bpol, &fops_mode);

	err = lan9645x_bum_bool_dbfs_create(bpol, bpol_root, "cpu_redir_ena",
					    &bpol->cpu_redir_ena);
	if (err)
		return err;

	err = lan9645x_bum_bool_dbfs_create(bpol, bpol_root, "known_ena",
					    &bpol->known_ena);
	if (err)
		return err;

	err = lan9645x_bum_bool_dbfs_create(bpol, bpol_root, "unknown_ena",
					    &bpol->unknown_ena);
	if (err)
		return err;

	if (type == LAN9645X_BUM_MC) {
		err = lan9645x_bum_bool_dbfs_create(bpol, bpol_root,
						    "ipmc_unknown_ena",
						    &bpol->ipmc_unknown_ena);
		if (err)
			return err;

		err = lan9645x_bum_bool_dbfs_create(bpol, bpol_root,
						    "ipmc_known_ena",
						    &bpol->ipmc_known_ena);
		if (err)
			return err;
	}

	return 0;
}

static int lan9645x_bum_dbfs_add(struct lan9645x *lan9645x,
				 struct lan9645x_bum_ctrl *bctrl)
{
	struct dentry *bum_dir;
	int err;

	bum_dir = debugfs_create_dir("bum", lan9645x->debugfs_root);
	if (IS_ERR(bum_dir))
		return PTR_ERR(bum_dir);

	debugfs_create_file("burst", 0666, bum_dir, bctrl, &fops_burst);
	debugfs_create_file("stats", 0444, bum_dir, bctrl, &lan9645x_stats_fops);

	for (int btype = 0; btype < __LAN9645X_BUM_NUM; btype++) {
		err = lan9645x_bpol_dbfs_add(lan9645x, bum_dir, bctrl, btype);
		if (err)
			return err;
	}

	return 0;
}

int lan9645x_bum_init(struct lan9645x *lan9645x)
{
	struct lan9645x_bum_ctrl *bctrl;

	bctrl = kzalloc(sizeof(*bctrl), GFP_KERNEL);
	if (!bctrl)
		return -ENOMEM;

	mutex_init(&bctrl->bum_lock);
	bctrl->lan9645x = lan9645x;
	/* Default burst is 1<<7 frames */
	bctrl->burst = 7;

	INIT_LIST_HEAD(&bctrl->debugfs_list);

	for (int btype = 0; btype < __LAN9645X_BUM_NUM; btype++) {
		bctrl->policers[btype].lan9645x = lan9645x;
		bctrl->policers[btype].type = btype;
		bctrl->policers[btype].cpu_redir_ena = 0;
		/* Default rate is 16k frames per second */
		bctrl->policers[btype].rate = 4;
		bctrl->policers[btype].unit = 0;
		bctrl->policers[btype].mode = LAN9645X_BUM_MODE_CPU_AND_FPORTS;
		bctrl->policers[btype].known_ena = false;
		bctrl->policers[btype].unknown_ena = false;
		bctrl->policers[btype].ipmc_known_ena = false;
		bctrl->policers[btype].ipmc_unknown_ena = false;
	}

	lan9645x->bum = bctrl;

	return lan9645x_bum_dbfs_add(lan9645x, bctrl);
}


void lan9645x_bum_deinit(struct lan9645x *lan9645x)
{
	struct lan9645x_bum_bool_dbfs_priv *pos, *tmp;
	struct lan9645x_bum_ctrl *bctrl;

	bctrl = lan9645x->bum;
	lan9645x->bum = NULL;

	list_for_each_entry_safe(pos, tmp, &bctrl->debugfs_list, list) {
		list_del(&pos->list);
		kfree(pos);
	}

	mutex_destroy(&bctrl->bum_lock);
	kfree(bctrl);
}
