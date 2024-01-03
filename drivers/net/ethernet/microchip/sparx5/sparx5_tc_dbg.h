/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip VCAP API
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

/*
 * The following utilities are only meant to used during tc development and will
 * not be upstreamed.
 */

#ifndef __SPARX5_TC_DBG_H__
#define __SPARX5_TC_DBG_H__

#include <net/pkt_cls.h>
#include <net/tc_act/tc_gate.h>

#if defined(CONFIG_SPARX5_DEBUG)

/* TC enums to string */
const char *tc_dbg_tc_setup_type(enum tc_setup_type type);
const char *tc_dbg_root_command(enum tc_root_command command);
const char *tc_dbg_flow_block_binder_type(enum flow_block_binder_type type);
const char *tc_dbg_flow_block_command(enum flow_block_command command);
const char *tc_dbg_flow_cls_command(enum flow_cls_command command);
const char *tc_dbg_flow_action_id(enum flow_action_id id);
const char *tc_dbg_flow_dissector_key_id(enum flow_dissector_key_id id);
const char *tc_dbg_tc_matchall_command(enum tc_matchall_command command);

/* Dump info */
void tc_dbg_match_dump(const struct net_device *dev,
		       const struct flow_rule *rule);
void tc_dbg_actions_dump(const struct net_device *dev,
			 const struct flow_rule *rule);

#else

static inline const char *tc_dbg_tc_setup_type(enum tc_setup_type type)
{
	return NULL;
}

static inline const char *tc_dbg_root_command(enum tc_root_command command)
{
	return NULL;
}

static inline const char *tc_dbg_flow_block_binder_type(enum flow_block_binder_type type)
{
	return NULL;
}

static inline const char *tc_dbg_flow_block_command(enum flow_block_command command)
{
	return NULL;
}

static inline const char *tc_dbg_flow_cls_command(enum flow_cls_command command)
{
	return NULL;
}

static inline const char *tc_dbg_flow_action_id(enum flow_action_id id)
{
	return NULL;
}

static inline const char *tc_dbg_flow_dissector_key_id(enum flow_dissector_key_id id)
{
	return NULL;
}

static inline const char *tc_dbg_tc_matchall_command(enum tc_matchall_command command)
{
	return NULL;
}

static inline void tc_dbg_match_dump(const struct net_device *dev,
				     const struct flow_rule *r)
{
}

static inline void tc_dbg_actions_dump(const struct net_device *dev,
				       const struct flow_rule *r)
{
}

#endif /* CONFIG_SPARX5_DEBUG */

#endif /* __SPARX5_TC_DBG_H__ */
