/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2022 Microchip Technology Inc.
 * Microchip Sparx5 Switch driver
 */

#ifndef _SPARX5_QOS_DEBUGFS_H_
#define _SPARX5_QOS_DEBUGFS_H_

#include <linux/types.h>

struct sparx5;

#if defined(CONFIG_DEBUG_FS)
void sparx5_qos_debugfs(struct sparx5 *sparx5);
#else
static inline void sparx5_qos_debugfs(struct sparx5 *sparx5) {}
#endif

#endif /* _SPARX5_QOS_DEBUG_FS_H_ */
