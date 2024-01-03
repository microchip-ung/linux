/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2022 Microchip Technology Inc. and its subsidiaries.
 * Microchip VCAP API
 */

#ifndef __VCAP_API_DEBUGFS__
#define __VCAP_API_DEBUGFS__

#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/netdevice.h>

#include "vcap_api.h"

#define MSB_32_MASK 0x80000000

#if defined(CONFIG_DEBUG_FS)

void vcap_port_debugfs(struct device *dev, struct dentry *parent,
		       struct vcap_control *vctrl,
		       struct net_device *ndev);

/* Create a debugFS entry for a vcap instance */
struct dentry *vcap_debugfs(struct device *dev, struct dentry *parent,
			    struct vcap_control *vctrl);

#else

static inline void vcap_port_debugfs(struct device *dev, struct dentry *parent,
				     struct vcap_control *vctrl,
				     struct net_device *ndev)
{
}

static inline struct dentry *vcap_debugfs(struct device *dev,
					  struct dentry *parent,
					  struct vcap_control *vctrl)
{
	return NULL;
}

#endif

void vcap_show_admin_info(struct vcap_control *vctrl,
			  struct vcap_admin *admin,
			  struct vcap_output_print *out);

#endif /* __VCAP_API_DEBUGFS__ */
