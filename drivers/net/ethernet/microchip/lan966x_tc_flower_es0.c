/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include "lan966x_main.h"

/*******************************************************************************
 * tc flower ES0
 ******************************************************************************/

/**
 * lan966x_tc_flower_es0_action - Check and parse TC ES0 action vid
 * @port: The interface
 * @ci: Chain info
 * @f: Offload info
 * @r: ES0 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if action is invalid.
 * -EOPNOTSUPP if action is unsupported.
 */
static int lan966x_tc_flower_es0_action(const struct lan966x_port *port,
					const struct lan966x_tc_ci *ci,
					struct flow_cls_offload *f,
					struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_es0_rule *es0 = &r->es0;
	struct flow_action *action = &rule->action;
	struct lan966x_vcap_es0_action_vid *vid;
	struct flow_action_entry *act;
	u64 action_mask = 0;
	int err, i;

	err = lan966x_tc_flower_action_check(ci, f, &action_mask);
	if (err)
		return err;

	es0->action.action = LAN966X_VCAP_ES0_ACTION_VID;
	vid = &es0->action.vid;

	flow_action_for_each(i, act, action) {
		switch (act->id) {
		case FLOW_ACTION_ACCEPT:
			break;
		case FLOW_ACTION_VLAN_POP:
			if (action_mask & BIT(FLOW_ACTION_VLAN_PUSH)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine pop and push action");
				return -EOPNOTSUPP;
			}
			if (action_mask & BIT(FLOW_ACTION_VLAN_MANGLE)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine pop and modify action");
				return -EOPNOTSUPP;
			}
			vid->push_outer_tag = 3; /* Force untag */
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (action_mask & BIT(FLOW_ACTION_VLAN_MANGLE)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine push and modify action");
				return -EOPNOTSUPP;
			}
			fallthrough;
		case FLOW_ACTION_VLAN_MANGLE:
			vid->push_outer_tag = 1; /* Push ES0 tag A */

			switch (be16_to_cpu(act->vlan.proto)) {
			case ETH_P_8021Q:
				vid->tag_a_tpid_sel = 0; /* 0x8100 */
				break;
			case ETH_P_8021AD:
				vid->tag_a_tpid_sel = 1; /* 0x88a8 */
				break;
			default:
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid vlan proto");
				return -EINVAL;
			}

			vid->tag_a_vid_sel = true; /* Use vid_a_val */
			vid->vid_a_val = act->vlan.vid;

			vid->tag_a_pcp_sel = 1; /* Use pcp_a_val */
			vid->pcp_a_val = act->vlan.prio;

			vid->tag_a_dei_sel = 0; /* Use classified dei */

			if (act->id == FLOW_ACTION_VLAN_PUSH) {
				/* Push classified tag as inner tag */
				vid->push_inner_tag = 1; /* Push ES0 tag B */
				vid->tag_b_tpid_sel = 3;
			}
			break;
		case FLOW_ACTION_GOTO:
			break;
		default:
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TC action");
			return -EOPNOTSUPP;
		}
	}
	return 0;
}

/**
 * lan966x_tc_flower_es0_key - Check and parse TC ES0 key
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: ES0 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_es0_key(const struct lan966x_port *port,
				     const struct lan966x_tc_ci *ci,
				     const struct lan966x_tc_flower_proto *p,
				     struct flow_cls_offload *f,
				     struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_es0_rule *es0 = &r->es0;
	struct lan966x_vcap_es0_key_vid *vid;

	/* Check supported dissectors */
	if (rule->match.dissector->used_keys &
	    ~(BIT(FLOW_DISSECTOR_KEY_CONTROL) |
	      BIT(FLOW_DISSECTOR_KEY_BASIC) |
	      BIT(FLOW_DISSECTOR_KEY_VLAN))) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Unsupported flower match");
		return -EOPNOTSUPP;
	}

	/* We cannot match specific protocols */
	if (p->l3 != ETH_P_ALL) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Unsupported protocol. Use all, 802.1q or 802.1ad");
		return -EOPNOTSUPP;
	}

	es0->key.key = LAN966X_VCAP_ES0_KEY_VID;
	vid = &es0->key.vid;

	vid->egr_port.value = port->chip_port;
	vid->egr_port.mask = ~0;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		vid->vid.value = match.key->vlan_id;
		vid->vid.mask = match.mask->vlan_id;
		vid->pcp.value = match.key->vlan_priority;
		vid->pcp.mask = match.mask->vlan_priority;
	}

        return 0;
}

int lan966x_tc_flower_es0_parse(const struct lan966x_port *port,
				const struct lan966x_tc_ci *ci,
				const struct lan966x_tc_flower_proto *p,
				struct flow_cls_offload *f,
				struct lan966x_vcap_rule *r)
{
	int err;

	err = lan966x_tc_flower_es0_key(port, ci, p, f, r);
	if (err)
		return err;

	return lan966x_tc_flower_es0_action(port, ci, f, r);
}
