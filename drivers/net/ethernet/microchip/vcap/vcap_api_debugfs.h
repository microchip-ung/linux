// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver VCAP Library
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 * The Sparx5 Chip Register Model can be browsed at this location:
 * https://github.com/microchip-ung/sparx-5_reginfo
 */

#ifndef __VCAP_API_DEBUGFS_H__
#define __VCAP_API_DEBUGFS_H__

#include <linux/types.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

#include "vcap_api.h"
#include "vcap_api_client.h"

#if defined(CONFIG_DEBUG_FS)
struct dentry *vcap_debugfs(struct dentry *parent, struct vcap_control *vctrl);
#else
struct dentry *vcap_debugfs(struct dentry *parent, struct vcap_control *vctrl) { return NULL; }
#endif

#endif /* __VCAP_API_DEBUGFS_H__ */
