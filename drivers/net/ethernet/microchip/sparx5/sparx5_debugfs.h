// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver debug filesystem support
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#ifndef __SPARX5_DEBUGFS_H__
#define __SPARX5_DEBUGFS_H__

#include <linux/types.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

struct sparx5;
struct vcap_control;

#if defined(CONFIG_DEBUG_FS)
void sparx5_debugfs(struct sparx5 *sparx5);
#else
static inline void sparx5_debugfs(struct sparx5 *sparx5) {}
#endif


#endif /* __SPARX5_DEBUGFS_H__ */
