// SPDX-License-Identifier: GPL-2.0+
/* Microchip VCAP API debug file system support
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/debugfs.h>

#include "vcap_api.h"
#include "vcap_api_client.h"

/* API callback used for debugfs purposes */
static int vcap_debugfs_printf(void *client, const char *fmt, ...)
{
	struct seq_file *m = client;
	va_list args;

	va_start(args, fmt);
	seq_vprintf(m, fmt, args);
	va_end(args);
	return 0;
}

static int vcap_debugfs_show(struct seq_file *m, void *unused)
{
	struct vcap_admin *admin = m->private;

	return vcap_show_admin(vcap_debugfs_printf, m, admin);
}
DEFINE_SHOW_ATTRIBUTE(vcap_debugfs);

static int vcap_raw_debugfs_show(struct seq_file *m, void *unused)
{
	struct vcap_admin *admin = m->private;

	return vcap_show_admin_raw(vcap_debugfs_printf, m, admin);
}
DEFINE_SHOW_ATTRIBUTE(vcap_raw_debugfs);

struct dentry *vcap_debugfs(struct dentry *parent, struct vcap_control *vctrl)
{
	struct vcap_admin *admin;
	struct dentry *dir;
	char name[50];

	dir = debugfs_create_dir("vcaps", parent);
	if (PTR_ERR_OR_ZERO(dir))
		return NULL;
	list_for_each_entry(admin, &vctrl->list, list) {
		sprintf(name, "%s_%d", vctrl->vcaps[admin->vtype].name, admin->vinst);
		debugfs_create_file(name, 0444, dir, admin, &vcap_debugfs_fops);
		sprintf(name, "raw_%s_%d", vctrl->vcaps[admin->vtype].name, admin->vinst);
		debugfs_create_file(name, 0444, dir, admin, &vcap_raw_debugfs_fops);
	}
	return dir;
}
