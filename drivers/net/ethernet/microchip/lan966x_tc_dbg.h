/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

/*
 * The following utilities are only meant to used during tc development and will
 * not be upstreamed.
 */

#ifndef _LAN966X_TC_DBG_H_
#define _LAN966X_TC_DBG_H_

#include <net/pkt_cls.h>

/* TC enums to string */
const char *tc_dbg_tc_setup_type(enum tc_setup_type type);
const char *tc_dbg_root_command(enum tc_root_command command);
const char *tc_dbg_flow_block_binder_type(enum flow_block_binder_type type);
const char *tc_dbg_flow_block_command(enum flow_block_command command);
const char *tc_dbg_flow_cls_command(enum flow_cls_command command);
const char *tc_dbg_flow_action_id(enum flow_action_id id);
const char *tc_dbg_flow_dissector_key_id(enum flow_dissector_key_id id);
const char *tc_dbg_tc_matchall_command(enum tc_matchall_command command);

/* Dump info via netdev_dbg(dev, ...) */
void tc_dbg_match_dump(const struct net_device *dev,
		       const struct flow_rule *rule);
void tc_dbg_actions_dump(const struct net_device *dev,
			 const struct flow_rule *rule);

#endif /* _LAN966X_TC_DBG_H_ */
