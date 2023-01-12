/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip VCAP API
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#ifndef __SPARX5_TC_H__
#define __SPARX5_TC_H__

#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "sparx5_tc_dbg.h"

struct sparx5;

struct sparx5_tc_flower_proto {
	u16 addr_type;
	u16 l3;
	u8 l4;
};

int sparx5_port_setup_tc(struct net_device *ndev, enum tc_setup_type type,
			 void *type_data);

int sparx5_tc_matchall(struct net_device *ndev,
		       struct tc_cls_matchall_offload *tmo,
		       bool ingress);

int sparx5_tc_flower(struct net_device *ndev, struct flow_cls_offload *fco,
		     bool ingress);

#if defined(CONFIG_DEBUG_FS)
void sparx5_mirror_probe_debugfs(struct sparx5 *sparx5);
#else
static inline void sparx5_mirror_probe_debugfs(struct sparx5 *sparx5) {}
#endif

#endif	/* __SPARX5_TC_H__ */
