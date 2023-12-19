// SPDX-License-Identifier: GPL-2.0+

#include <net/tc_act/tc_gate.h>

#include "lan966x_main.h"
#include "lan966x_vcap_utils.h"
#include "vcap_api.h"
#include "vcap_api_client.h"
#include "vcap_tc.h"

#define LAN966X_FORCE_UNTAGED	3
#define LAN966X_MAX_RULE_SIZE 5 /* allows X1, X2 and X4 rules */

#define ETH_P_RTAG	0xF1C1          /* Redundancy Tag (IEEE 802.1CB) */
#define ETH_P_ELMI	0x88EE          /* MEF 16 E-LMI */

/* Collect keysets and type ids for multiple rules per size */
struct lan966x_wildcard_rule {
	bool selected;
	u8 value;
	u8 mask;
	enum vcap_keyfield_set keyset;
};

struct lan966x_multiple_rules {
	struct lan966x_wildcard_rule rule[LAN966X_MAX_RULE_SIZE];
};

struct lan966x_tc_flower_template {
	struct list_head list; /* for insertion in the list of templates */
	int cid; /* chain id */
	enum vcap_keyfield_set orig; /* keyset used before the template */
	enum vcap_keyfield_set keyset; /* new keyset used by template */
	u16 l3_proto; /* protocol specified in the template */
};

static bool lan966x_tc_is_known_etype(struct vcap_tc_flower_parse_usage *st,
				      u16 etype)
{
	switch (st->admin->vtype) {
	case VCAP_TYPE_IS1:
		switch (etype) {
		case ETH_P_ALL:
		case ETH_P_ARP:
		case ETH_P_IP:
		case ETH_P_IPV6:
		case ETH_P_RTAG:
		case ETH_P_SNAP:
			return true;
		}
		break;
	case VCAP_TYPE_IS2:
		switch (etype) {
		case ETH_P_ALL:
		case ETH_P_ARP:
		case ETH_P_IP:
		case ETH_P_IPV6:
		case ETH_P_RTAG:
		case ETH_P_SNAP:
		case ETH_P_802_2:
		case ETH_P_SLOW:
		case ETH_P_CFM:
		case ETH_P_ELMI:
			return true;
		}
		break;
	case VCAP_TYPE_ES0:
		return true;
	default:
		NL_SET_ERR_MSG_MOD(st->fco->common.extack,
				   "VCAP type not supported");
		return false;
	}

	return false;
}

static int
lan966x_tc_flower_handler_control_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct flow_match_control match;
	int err = 0;

	flow_rule_match_control(st->frule, &match);
	if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
		if (match.key->flags & FLOW_DIS_IS_FRAGMENT)
			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_L3_FRAGMENT,
						    VCAP_BIT_1);
		else
			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_L3_FRAGMENT,
						    VCAP_BIT_0);
		if (err)
			goto out;
	}

	if (match.mask->flags & FLOW_DIS_FIRST_FRAG) {
		if (match.key->flags & FLOW_DIS_FIRST_FRAG)
			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_L3_FRAG_OFS_GT0,
						    VCAP_BIT_0);
		else
			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_L3_FRAG_OFS_GT0,
						    VCAP_BIT_1);
		if (err)
			goto out;
	}

	st->used_keys |= BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL);

	return err;

out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_frag parse error");
	return err;
}

int lan966x_tc_flower_handler_ipv6_usage(struct vcap_tc_flower_parse_usage *st)
{
	int err = 0;

	if (st->l3_proto == ETH_P_IPV6) {
		struct flow_match_ipv6_addrs mt;
		struct vcap_u128_key sip;
		struct vcap_u128_key dip;

		flow_rule_match_ipv6_addrs(st->frule, &mt);
		/* Check if address masks are non-zero */
		if (!ipv6_addr_any(&mt.mask->src)) {

			vcap_netbytes_copy(sip.value, mt.key->src.s6_addr, 16);
			vcap_netbytes_copy(sip.mask, mt.mask->src.s6_addr, 16);
			err = vcap_rule_add_key_u128(st->vrule,
						     VCAP_KF_L3_IP6_SIP, &sip);
			if (err)
				goto out;

			/* IS1: With ipv6 addresses, we have to hit: NORMAL_IPV6
			 * or 5TUPLE_IPV6. These keysets do not support TCP_IS
			 * key, which might have been added earlier by the
			 * basic_usage() dissector. We remove it, and add the
			 * l4 proto in the L3_IP_PROTO key instead.
			 */
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				if (vcap_contains_key(st->vrule, VCAP_KF_TCP_IS))
					vcap_rule_rem_key(st->vrule, VCAP_KF_TCP_IS);

				err = vcap_rule_add_key_u32(st->vrule,
							    VCAP_KF_L3_IP_PROTO,
							    st->l4_proto, ~0);
				if (err)
					goto out;
			}
		}
		if (!ipv6_addr_any(&mt.mask->dst)) {

			vcap_netbytes_copy(dip.value, mt.key->dst.s6_addr, 16);
			vcap_netbytes_copy(dip.mask, mt.mask->dst.s6_addr, 16);
			err = vcap_rule_add_key_u128(st->vrule,
						     VCAP_KF_L3_IP6_DIP, &dip);
			if (err)
				goto out;

			/* IS1: With ipv6 addresses, we have to hit: NORMAL_IPV6
			 * or 5TUPLE_IPV6. These keysets do not support TCP_IS
			 * key, which might have been added earlier by the
			 * basic_usage() dissector. We remove it, and add the
			 * l4 proto in the L3_IP_PROTO key instead.
			 */
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				if (vcap_contains_key(st->vrule, VCAP_KF_TCP_IS))
					vcap_rule_rem_key(st->vrule, VCAP_KF_TCP_IS);

				if (!vcap_contains_key(st->vrule, VCAP_KF_L3_IP_PROTO)) {
					err = vcap_rule_add_key_u32(st->vrule,
								    VCAP_KF_L3_IP_PROTO,
								    st->l4_proto, ~0);
					if (err)
						goto out;
				}
			}
		}
	}
	st->used_keys |= BIT_ULL(FLOW_DISSECTOR_KEY_IPV6_ADDRS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ipv6_addr parse error");
	return err;
}

static int
lan966x_tc_flower_handler_basic_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct flow_match_basic match;
	int err = 0;

	flow_rule_match_basic(st->frule, &match);
	if (match.mask->n_proto) {
		st->l3_proto = be16_to_cpu(match.key->n_proto);
		if (!lan966x_tc_is_known_etype(st, st->l3_proto)) {
			err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_ETYPE,
						    st->l3_proto, ~0);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IP) {
			err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_IP4_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IPV6 &&
			   st->admin->vtype == VCAP_TYPE_IS1) {
			/* Don't set any keys in this case */
		} else if (st->l3_proto == ETH_P_ALL) {
			/* Nothing to do */
		} else if (st->l3_proto == ETH_P_SNAP &&
			   st->admin->vtype == VCAP_TYPE_IS1) {
			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_ETYPE_LEN_IS,
						    VCAP_BIT_0);
			if (err)
				goto out;

			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_IP_SNAP_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_RTAG) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_8021CB_R_TAGGED_IS,
						      VCAP_BIT_1);
			}
		} else {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				err = vcap_rule_add_key_bit(st->vrule,
							    VCAP_KF_ETYPE_LEN_IS,
							    VCAP_BIT_1);
				if (err)
					goto out;

				err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_ETYPE,
							    st->l3_proto, ~0);
				if (err)
					goto out;
			}
		}
	}
	if (match.mask->ip_proto) {
		st->l4_proto = match.key->ip_proto;

		if (st->l4_proto == IPPROTO_TCP) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				err = vcap_rule_add_key_bit(st->vrule,
							    VCAP_KF_TCP_UDP_IS,
							    VCAP_BIT_1);
				if (err)
					goto out;
			}

			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_TCP_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l4_proto == IPPROTO_UDP) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				err = vcap_rule_add_key_bit(st->vrule,
							    VCAP_KF_TCP_UDP_IS,
							    VCAP_BIT_1);
				if (err)
					goto out;
			}

			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_TCP_IS,
						    VCAP_BIT_0);
			if (err)
				goto out;
		} else {
			err = vcap_rule_add_key_u32(st->vrule,
						    VCAP_KF_L3_IP_PROTO,
						    st->l4_proto, ~0);
			if (err)
				goto out;
		}
	}

	st->used_keys |= BIT_ULL(FLOW_DISSECTOR_KEY_BASIC);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_proto parse error");
	return err;
}

static int
lan966x_tc_flower_handler_cvlan_usage(struct vcap_tc_flower_parse_usage *st)
{
	if (st->admin->vtype != VCAP_TYPE_IS1) {
		NL_SET_ERR_MSG_MOD(st->fco->common.extack,
				   "cvlan not supported in this VCAP");
		return -EINVAL;
	}

	return vcap_tc_flower_handler_cvlan_usage(st);
}

static int
lan966x_tc_flower_handler_vlan_usage(struct vcap_tc_flower_parse_usage *st)
{
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID_CLS;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP_CLS;

	if (st->admin->vtype == VCAP_TYPE_IS1) {
		vid_key = VCAP_KF_8021Q_VID0;
		pcp_key = VCAP_KF_8021Q_PCP0;
	}

	return vcap_tc_flower_handler_vlan_usage(st, vid_key, pcp_key);
}

static int lan966x_tc_flower_handler_portnum_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct flow_match_ports match;
	enum vcap_key_field key;
	u16 value, mask;
	int err = 0;

	if (st->admin->vtype == VCAP_TYPE_IS1)
		key = VCAP_KF_ETYPE;
	else
		key = VCAP_KF_L4_DPORT;

	flow_rule_match_ports(st->frule, &match);
	if (match.mask->src) {
		value = be16_to_cpu(match.key->src);
		mask = be16_to_cpu(match.mask->src);
		err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L4_SPORT, value, mask);
		if (err)
			goto out;
	}
	if (match.mask->dst) {
		value = be16_to_cpu(match.key->dst);
		mask = be16_to_cpu(match.mask->dst);
		err = vcap_rule_add_key_u32(st->vrule, key, value, mask);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_PORTS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "port parse error");
	return err;
}

int lan966x_tc_flower_handler_ip_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct flow_match_ip match;
	enum vcap_key_field key;
	int err;

	flow_rule_match_ip(st->frule, &match);

	if (st->admin->vtype == VCAP_TYPE_IS1)
		key = VCAP_KF_L3_DSCP;
	else
		key = VCAP_KF_L3_TOS;

	if (match.mask->tos) {
		err = vcap_rule_add_key_u32(st->vrule, key,
					    match.key->tos,
					    match.mask->tos);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_IP);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_tos parse error");
	return err;
}

static int
(*lan966x_tc_flower_handlers_usage[])(struct vcap_tc_flower_parse_usage *st) = {
	[FLOW_DISSECTOR_KEY_ETH_ADDRS] = vcap_tc_flower_handler_ethaddr_usage,
	[FLOW_DISSECTOR_KEY_IPV4_ADDRS] = vcap_tc_flower_handler_ipv4_usage,
	[FLOW_DISSECTOR_KEY_IPV6_ADDRS] = lan966x_tc_flower_handler_ipv6_usage,
	[FLOW_DISSECTOR_KEY_CONTROL] = lan966x_tc_flower_handler_control_usage,
	[FLOW_DISSECTOR_KEY_PORTS] = lan966x_tc_flower_handler_portnum_usage,
	[FLOW_DISSECTOR_KEY_BASIC] = lan966x_tc_flower_handler_basic_usage,
	[FLOW_DISSECTOR_KEY_CVLAN] = lan966x_tc_flower_handler_cvlan_usage,
	[FLOW_DISSECTOR_KEY_VLAN] = lan966x_tc_flower_handler_vlan_usage,
	[FLOW_DISSECTOR_KEY_TCP] = vcap_tc_flower_handler_tcp_usage,
	[FLOW_DISSECTOR_KEY_ARP] = vcap_tc_flower_handler_arp_usage,
	[FLOW_DISSECTOR_KEY_IP] = lan966x_tc_flower_handler_ip_usage,
};

static int
lan966x_tc_flower_use_dissectors(struct vcap_tc_flower_parse_usage *st,
				 struct vcap_admin *admin,
				 struct vcap_rule *vrule)
{
	int idx, err = 0;

	for (idx = 0; idx < ARRAY_SIZE(lan966x_tc_flower_handlers_usage); ++idx) {
		if (!flow_rule_match_key(st->frule, idx))
			continue;
		if (!lan966x_tc_flower_handlers_usage[idx])
			continue;
		err = lan966x_tc_flower_handlers_usage[idx](st);
		if (err)
			return err;
	}

	if (st->frule->match.dissector->used_keys ^ st->used_keys) {
		NL_SET_ERR_MSG_MOD(st->fco->common.extack,
				   "Unsupported match item");
		return -ENOENT;
	}

	return err;
}

static int lan966x_tc_flower_action_check(struct vcap_control *vctrl,
					  struct net_device *dev,
					  struct flow_cls_offload *fco,
					  bool ingress)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(fco);
	struct flow_action_entry *actent, *last_actent = NULL;
	struct flow_action *act = &rule->action;
	u64 action_mask = 0;
	int idx;

	if (!flow_action_has_entries(act)) {
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No actions");
		return -EINVAL;
	}

	if (!flow_action_basic_hw_stats_check(act, fco->common.extack))
		return -EOPNOTSUPP;

	flow_action_for_each(idx, actent, act) {
		if (action_mask & BIT(actent->id)) {
			NL_SET_ERR_MSG_MOD(fco->common.extack,
					   "More actions of the same type");
			return -EINVAL;
		}
		action_mask |= BIT(actent->id);
		last_actent = actent; /* Save last action for later check */
	}

	/* Check that last action is a goto
	 * The last chain/lookup does not need to have goto action
	 */
	if (last_actent->id == FLOW_ACTION_GOTO) {
		/* Check if the destination chain is in one of the VCAPs */
		if (!vcap_is_next_lookup(vctrl, fco->common.chain_index,
					 last_actent->chain_index)) {
			NL_SET_ERR_MSG_MOD(fco->common.extack,
					   "Invalid goto chain");
			return -EINVAL;
		}
	} else if (!vcap_is_last_chain(vctrl, fco->common.chain_index,
				       ingress)) {
		NL_SET_ERR_MSG_MOD(fco->common.extack,
				   "Last action must be 'goto'");
		return -EINVAL;
	}

	/* Catch unsupported combinations of actions */
	if (action_mask & BIT(FLOW_ACTION_TRAP) &&
	    action_mask & BIT(FLOW_ACTION_ACCEPT)) {
		NL_SET_ERR_MSG_MOD(fco->common.extack,
				   "Cannot combine pass and trap action");
		return -EOPNOTSUPP;
	}

	return 0;
}

/* Add the actionset that is the default for the VCAP type */
static int lan966x_tc_set_actionset(struct vcap_admin *admin,
				    struct vcap_rule *vrule)
{
	enum vcap_actionfield_set aset;
	int err = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		aset = VCAP_AFS_S1;
		break;
	case VCAP_TYPE_IS2:
		aset = VCAP_AFS_BASE_TYPE;
		break;
	case VCAP_TYPE_ES0:
		aset = VCAP_AFS_VID;
		break;
	default:
		return -EINVAL;
	}

	/* Do not overwrite any current actionset */
	if (vrule->actionset == VCAP_AFS_NO_VALUE)
		err = vcap_set_rule_set_actionset(vrule, aset);

	return err;
}

static int lan966x_tc_add_rule_link_target(struct vcap_admin *admin,
					   struct vcap_rule *vrule,
					   int target_cid)
{
	int link_val = target_cid % VCAP_CID_LOOKUP_SIZE;
	int err;

	if (!link_val)
		return 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		/* Choose IS1 specific NXT_IDX key (for chaining rules from IS1) */
		err = vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_GEN_IDX_SEL,
					    1, ~0);
		if (err)
			return err;

		return vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_GEN_IDX,
					     link_val, ~0);
	case VCAP_TYPE_IS2:
		/* Add IS2 specific PAG key (for chaining rules from IS1) */
		return vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_PAG,
					     link_val, ~0);
	case VCAP_TYPE_ES0:
		/* Add ES0 specific ISDX key (for chaining rules from IS1) */
		return vcap_rule_add_key_u32(vrule, VCAP_KF_ISDX_CLS,
					     link_val, ~0);
	default:
		break;
	}
	return 0;
}

static void lan966x_tc_flower_set_exterr(struct net_device *ndev,
					struct flow_cls_offload *fco,
					struct vcap_rule *vrule)
{
	switch (vrule->exterr) {
	case VCAP_ERR_NONE:
		break;
	case VCAP_ERR_NO_ADMIN:
		NL_SET_ERR_MSG_MOD(fco->common.extack, "Missing VCAP instance");
		break;
	case VCAP_ERR_NO_NETDEV:
		NL_SET_ERR_MSG_MOD(fco->common.extack, "Missing network interface");
		break;
	case VCAP_ERR_NO_KEYSET_MATCH:
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No keyset matched the filter keys");
		break;
	case VCAP_ERR_NO_ACTIONSET_MATCH:
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No actionset matched the filter actions");
		break;
	case VCAP_ERR_NO_PORT_KEYSET_MATCH:
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No port keyset matched the filter keys");
		break;
	}
}

static int lan966x_tc_add_rule_copy(struct lan966x_port *port,
				    struct flow_cls_offload *fco,
				    struct vcap_rule *erule,
				    struct lan966x_wildcard_rule *rule)
{
	enum vcap_key_field keylist[] = {
		VCAP_KF_IF_IGR_PORT_MASK,
		VCAP_KF_IF_IGR_PORT_MASK_SEL,
		VCAP_KF_IF_IGR_PORT_MASK_RNG,
		VCAP_KF_LOOKUP_FIRST_IS,
		VCAP_KF_TYPE,
	};
	struct vcap_rule *vrule;
	int err;

	/* Add an extra rule with a special user and the new keyset */
	erule->user = VCAP_USER_TC_EXTRA;
	pr_debug("%s:%d: modified: keyset: %s, value: %#x, mask: %#x\n",
		 __func__, __LINE__,
		 lan966x_vcap_keyset_name(port->dev, rule->keyset),
		 rule->value,
		 ~rule->mask);
	vrule = vcap_copy_rule(erule);
	if (IS_ERR(vrule))
		return PTR_ERR(vrule);
	/* Link the new rule to the existing rule with the cookie */
	vrule->cookie = erule->cookie;
	vcap_filter_rule_keys(vrule, keylist, ARRAY_SIZE(keylist), true);
	err = vcap_set_rule_set_keyset(vrule, rule->keyset);
	if (err) {
		pr_err("%s:%d: could not set keyset %s in rule: %u\n",
		       __func__, __LINE__,
		       lan966x_vcap_keyset_name(port->dev, rule->keyset),
		       vrule->id);
		goto out;
	}
	err = vcap_rule_mod_key_u32(vrule, VCAP_KF_TYPE, rule->value, ~rule->mask);
	if (err) {
		pr_err("%s:%d: could wildcard rule type id in rule: %u\n",
		       __func__, __LINE__, vrule->id);
		goto out;
	}
	err = vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		pr_err("%s:%d: could not validate rule: %u\n",
		       __func__, __LINE__, vrule->id);
		lan966x_tc_flower_set_exterr(port->dev, fco, vrule);
		goto out;
	}
	err = vcap_add_rule(vrule);
	if (err) {
		pr_err("%s:%d: could not add rule: %u\n",
		       __func__, __LINE__, vrule->id);
		goto out;
	}
	pr_debug("%s:%d: created rule: %u\n", __func__, __LINE__, vrule->id);
out:
	vcap_free_rule(vrule);
	return err;
}

static int lan966x_tc_add_remaining_rules(struct lan966x_port *port,
					  struct flow_cls_offload *fco,
					  struct vcap_rule *erule,
					  struct vcap_admin *admin,
					  struct lan966x_multiple_rules *multi)
{
	int idx, err = 0;

	/* ES0 only has one keyset, so no keyset wildcarding */
	if (admin->vtype == VCAP_TYPE_ES0)
		return err;

	for (idx = 0; idx < LAN966X_MAX_RULE_SIZE; ++idx) {
		if (!multi->rule[idx].selected)
			continue;
		err = lan966x_tc_add_rule_copy(port, fco, erule, &multi->rule[idx]);
		if (err)
			break;
	}
	return err;
}

static int lan966x_tc_add_rule_link(struct vcap_control *vctrl,
				    struct vcap_admin *admin,
				    struct vcap_rule *vrule,
				    struct flow_cls_offload *f,
				    int to_cid)
{
	struct vcap_admin *to_admin = vcap_find_admin(vctrl, to_cid);
	int diff, err = 0;

	if (!to_admin) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Unknown destination chain");
		return -EINVAL;
	}

	diff = vcap_chain_offset(vctrl, f->common.chain_index, to_cid);
	if (!diff)
		return 0;

	/* Between IS1 and IS2 the PAG value is used */
	if (admin->vtype == VCAP_TYPE_IS1 && to_admin->vtype == VCAP_TYPE_IS2) {
		/* This works for IS1->IS2 */
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_VAL, diff);
		if (err)
			return err;

		err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_OVERRIDE_MASK,
					       0xff);
		if (err)
			return err;
	} else if (admin->vtype == VCAP_TYPE_IS1 &&
		   to_admin->vtype == VCAP_TYPE_ES0) {
		/* This works for IS1->ES0 */
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_ISDX_ADD_VAL,
					       diff);
		if (err)
			return err;

		err = vcap_rule_add_action_bit(vrule, VCAP_AF_ISDX_REPLACE_ENA,
					       VCAP_BIT_1);
		if (err)
			return err;
	} else {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Unsupported chain destination");
		return -EOPNOTSUPP;
	}

	return err;
}

static int lan966x_tc_add_rule_counter(struct vcap_admin *admin,
				       struct vcap_rule *vrule)
{
	int err = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		err = vcap_rule_mod_action_u32(vrule, VCAP_AF_ESDX,
					       vrule->id);
		break;
	default:
		break;
	}

	return err;
}

/* Collect all port keysets and apply the first of them, possibly wildcarded */
static int
lan966x_tc_select_protocol_keyset(struct net_device *ndev,
				  struct vcap_rule *vrule,
				  struct vcap_admin *admin, u16 l3_proto,
				  struct lan966x_multiple_rules *multi)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct vcap_keyset_list portkeysetlist = {};
	enum vcap_keyfield_set portkeysets[10] = {};
	struct vcap_keyset_list matches = {};
	enum vcap_keyfield_set keysets[10];
	struct lan966x_wildcard_rule *mru;
	int idx, jdx, err = 0, count = 0;
	const struct vcap_set *kinfo;
	struct vcap_control *vctrl;

	vctrl = port->lan966x->vcap_ctrl;

	/* Find the keysets that the rule can use */
	matches.keysets = keysets;
	matches.max = ARRAY_SIZE(keysets);
	if (!vcap_rule_find_keysets(vrule, &matches))
		return -EINVAL;

	/* Find the keysets that the port configuration supports */
	portkeysetlist.max = ARRAY_SIZE(portkeysets);
	portkeysetlist.keysets = portkeysets;
	err = lan966x_vcap_get_port_keyset(ndev, admin, vrule->vcap_chain_id,
					   l3_proto, &portkeysetlist);
	if (err)
		return err;

	/* Find the intersection of the two sets of keyset */
	for (idx = 0; idx < portkeysetlist.cnt; ++idx) {
		kinfo = vcap_keyfieldset(vctrl, admin->vtype,
					 portkeysetlist.keysets[idx]);
		if (!kinfo)
			continue;

		/* Find a port keyset that matches the required keys
		 * If there are multiple keysets then compose a type id mask
		 */
		for (jdx = 0; jdx < matches.cnt; ++jdx) {
			if (portkeysetlist.keysets[idx] != matches.keysets[jdx])
				continue;

			mru = &multi->rule[kinfo->sw_per_item];
			if (!mru->selected) {
				mru->selected = true;
				mru->keyset = portkeysetlist.keysets[idx];
				mru->value = kinfo->type_id;
			}
			mru->value &= kinfo->type_id;
			mru->mask |= kinfo->type_id;
			++count;
		}
	}
	/* Fail if the VCAP has port keysets and no keyset matched the rule
	 * keys.
	 */
	if (portkeysetlist.cnt > 0 && count == 0)
		return -EPROTO;

	for (idx = 0; idx < LAN966X_MAX_RULE_SIZE; ++idx) {
		mru = &multi->rule[idx];
		if (!mru->selected)
			continue;

		/* Align the mask to the combined value */
		mru->mask ^= mru->value;
	}

	/* Set the chosen keyset on the rule and set a wildcarded type if there
	 * are more than one keyset
	 */
	for (idx = 0; idx < LAN966X_MAX_RULE_SIZE; ++idx) {
		mru = &multi->rule[idx];
		if (!mru->selected)
			continue;

		vcap_set_rule_set_keyset(vrule, mru->keyset);
		if (count > 1)
			/* Some keysets do not have a type field */
			vcap_rule_mod_key_u32(vrule, VCAP_KF_TYPE,
					      mru->value,
					      ~mru->mask);
		mru->selected = false; /* mark as done */
		break; /* Stop here and add more rules later */
	}

	return err;
}


static int lan966x_tc_set_default_actionset(struct vcap_admin *admin,
					    struct vcap_rule *vrule,
					    int cid)
{
	int err = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_S1);
		break;
	case VCAP_TYPE_IS2:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
		break;
	case VCAP_TYPE_ES0:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_VID);
		break;
	default:
		break;
	}
	return err;

}

static int lan966x_tc_flower_reserve_policer(struct lan966x_port *port,
					     struct flow_cls_offload *fco,
					     struct vcap_rule *vrule,
					     u32 tc_policer_index)
{
	enum lan966x_res_pool_user user;
	struct vcap_admin *admin;
	int err, polidx;

	/* Find the policer pool user */
	admin = vcap_find_admin(port->lan966x->vcap_ctrl, vrule->vcap_chain_id);
	user = LAN966X_RES_POOL_USER_IS1;
	if (admin->vtype == VCAP_TYPE_IS2)
		user = LAN966X_RES_POOL_USER_IS2;

	err = lan966x_pol_ix_reserve(port->lan966x,
				     user,
				     tc_policer_index,
				     &polidx);
	if (err < 0) {
		NL_SET_ERR_MSG_MOD(fco->common.extack,
				   "Cannot reserve policer");
		err = -EOPNOTSUPP;
	}
	vrule->client = tc_policer_index;
	pr_debug("%s:%d: rule %d: reserve policer: %d\n",
		 __func__, __LINE__, vrule->id, tc_policer_index);
	return polidx;
}

static int lan966x_tc_flower_release_policer(struct lan966x_port *port,
					     struct vcap_rule *vrule)
{
	enum lan966x_res_pool_user user;
	struct vcap_admin *admin;
	int tc_policer_index;
	int err = 0;

	/* Find the policer pool user */
	admin = vcap_find_admin(port->lan966x->vcap_ctrl, vrule->vcap_chain_id);
	user = LAN966X_RES_POOL_USER_IS1;
	if (admin->vtype == VCAP_TYPE_IS2)
		user = LAN966X_RES_POOL_USER_IS2;

	tc_policer_index = vrule->client;
	pr_debug("%s:%d: rule %d: release policer: %d\n",
		 __func__, __LINE__, vrule->id, tc_policer_index);
	err = lan966x_pol_ix_release(port->lan966x,
				     user,
				     tc_policer_index);
	vrule->client = 0;
	return err;
}

static int lan966x_tc_flower_parse_act_es0(struct vcap_rule *vrule,
					   struct flow_action_entry *act)
{
	int err;

	switch (be16_to_cpu(act->vlan.proto)) {
	case ETH_P_8021Q:
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_TPID_SEL, 0); /* 0x8100 */
		break;
	case ETH_P_8021AD:
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_TPID_SEL, 1); /* 0x88a8 */
		break;
	default:
		return -EINVAL;
	}

	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_OUTER_TAG, 1); /* Push ES0 tag A */
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_TAG_A_VID_SEL, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_VID_A_VAL, act->vlan.vid);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_PCP_SEL, 1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PCP_A_VAL, act->vlan.prio);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_DEI_SEL, 0);

	return err;
}

static int lan966x_tc_flower_parse_act_is1(struct vcap_rule *vrule,
					   struct flow_action_entry *act)
{
	int err;

	if (be16_to_cpu(act->vlan.proto) != ETH_P_8021Q)
		return -EINVAL;

	err = vcap_rule_add_action_bit(vrule, VCAP_AF_VID_REPLACE_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_VID_VAL, act->vlan.vid);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_PCP_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PCP_VAL, act->vlan.prio);

	return err;
}

/* Remove rule keys that may prevent templates from matching a keyset */
static void lan966x_tc_flower_simplify_rule(struct vcap_admin *admin,
					    struct vcap_rule *vrule,
					    u16 l3_proto)
{
	switch (admin->vtype) {
	case VCAP_TYPE_IS0:
		vcap_rule_rem_key(vrule, VCAP_KF_ETYPE);
		switch (l3_proto) {
		case ETH_P_IP:
			break;
		case ETH_P_IPV6:
			vcap_rule_rem_key(vrule, VCAP_KF_IP_SNAP_IS);
			break;
		default:
			break;
		}
		break;
	case VCAP_TYPE_ES2:
		switch (l3_proto) {
		case ETH_P_IP:
			if (vrule->keyset == VCAP_KFS_IP4_OTHER)
				vcap_rule_rem_key(vrule, VCAP_KF_TCP_IS);
			break;
		case ETH_P_IPV6:
			if (vrule->keyset == VCAP_KFS_IP6_STD)
				vcap_rule_rem_key(vrule, VCAP_KF_TCP_IS);
			vcap_rule_rem_key(vrule, VCAP_KF_IP4_IS);
			break;
		default:
			break;
		}
		break;
	case VCAP_TYPE_IS2:
		switch (l3_proto) {
		case ETH_P_IP:
		case ETH_P_IPV6:
			vcap_rule_rem_key(vrule, VCAP_KF_IP4_IS);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static bool lan966x_tc_flower_use_template(struct net_device *ndev,
					   struct flow_cls_offload *fco,
					   struct vcap_admin *admin,
					   struct vcap_rule *vrule)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x_tc_flower_template *ftp;

	list_for_each_entry(ftp, &port->tc.templates, list) {
		if (ftp->cid != fco->common.chain_index)
			continue;

		vcap_set_rule_set_keyset(vrule, ftp->keyset);
		lan966x_tc_flower_simplify_rule(admin, vrule, ftp->l3_proto);
		return true;
	}
	return false;
}

static int lan966x_tc_flower_add(struct lan966x_port *port,
				 struct flow_cls_offload *f,
				 struct vcap_admin *admin,
				 bool ingress)
{
	struct vcap_tc_flower_parse_usage state = {
		.fco = f,
		.l3_proto = ETH_P_ALL,
		.admin = admin,
	};
	struct lan966x_multiple_rules multi = {};
	struct lan966x_tc_policer pol = {0};
	struct lan966x_psfp_sg_cfg sg = {0};
	struct lan966x_psfp_sf_cfg sf = {0};
	struct flow_action_entry *act;
	u16 l3_proto = ETH_P_ALL;
	struct flow_rule *frule;
	struct vcap_rule *vrule;
	int err, idx, lookup;
	u32 ports = 0;
	u32 polidx;
	u32 sfi_ix;
	u32 sgi_ix;

	err = lan966x_tc_flower_action_check(port->lan966x->vcap_ctrl,
					     port->dev, f, ingress);
	if (err)
		return err;

	vrule = vcap_alloc_rule(port->lan966x->vcap_ctrl, port->dev,
				f->common.chain_index, VCAP_USER_TC,
				f->common.prio, 0);
	if (IS_ERR(vrule))
		return PTR_ERR(vrule);

	vrule->cookie = f->cookie;

	state.vrule = vrule;
	state.frule = flow_cls_offload_flow_rule(f);

	err = lan966x_tc_flower_use_dissectors(&state, admin, vrule);
	if (err)
		goto out;

	err = lan966x_tc_add_rule_link_target(admin, vrule,
					      f->common.chain_index);
	if (err)
		goto out;

	frule = flow_cls_offload_flow_rule(f);

	flow_action_for_each(idx, act, &frule->action) {
		switch (act->id) {
		case FLOW_ACTION_TRAP:

			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Trap action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			err = vcap_rule_add_action_bit(vrule,
						       VCAP_AF_CPU_COPY_ENA,
						       VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule,
							VCAP_AF_CPU_QUEUE_NUM,
							0);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE,
							LAN966X_PMM_REPLACE);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_DROP:
			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Drop action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			/* VCAP_AF_MASK_MODE: lan966x is2 W2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE,
						       LAN966X_PMM_REPLACE);
			if (err)
				goto out;
			/* VCAP_AF_POLICE_ENA: lan966x s1 W1, lan966x s2 W1 */
			err = vcap_rule_add_action_bit(
				vrule, VCAP_AF_POLICE_ENA, VCAP_BIT_1);
			if (err)
				goto out;
			/* VCAP_AF_POLICE_IDX: (lan966x s1 W9), (lan966x s2 W9) */
			err = vcap_rule_add_action_u32(vrule,
						       VCAP_AF_POLICE_IDX,
						       LAN966X_POL_IX_DISCARD);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_MIRRED:
			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Mirror action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			err = lan966x_mirror_vcap_add(port,
						      netdev_priv(act->dev));
			if (err) {
				switch (err) {
				case -EBUSY:
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Cannot change the mirror monitor port while in use");
					break;
				case -EINVAL:
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Cannot mirror the mirror monitor port");
					break;
				default:
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Unknown error");
					break;
				}
				return err;
			}
			/* VCAP_AF_MIRROR_ENA: W1, lan966x: is2 */
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_MIRROR_ENA, VCAP_BIT_1);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_REDIRECT:
			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Redirect action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			/* VCAP_AF_MASK_MODE: lan966x is2 W2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, LAN966X_PMM_REDIRECT);
			if (err)
				goto out;
			/* VCAP_AF_PORT_MASK: (lan966x s2 W8 */
			ports |= BIT(port->chip_port);
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_PORT_MASK, ports);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_POLICE:
			if (admin->vtype != VCAP_TYPE_IS1 && admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Police action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			if (vcap_chain_id_to_lookup(admin, f->common.chain_index) != 0) {
			//if (lan966x_vcap_cid_to_lookup(admin, f->common.chain_index) != 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Police action is only supported in first IS2 lookup");
				err = -EOPNOTSUPP;
				goto out;
			}
			err = lan966x_tc_flower_reserve_policer(port,
								f,
								vrule,
								act->hw_index);
			if (err < 0)
				goto out;
			polidx = err;

			/* VCAP_AF_POLICE_ENA: lan966x s1 W1, lan966x s2 W1 */
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_POLICE_ENA, VCAP_BIT_1);
			if (err)
				goto out;
			/* VCAP_AF_POLICE_IDX: (lan966x s1 W9), (lan966x s2 W9) */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_POLICE_IDX, polidx);
			if (err)
				goto out;

			pol.rate = div_u64(act->police.rate_bytes_ps, 1000) * 8;
			pol.burst = act->police.burst;
			err = lan966x_police_add(port, &pol, polidx);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set policer");
				err = -EOPNOTSUPP;
				goto out;
			}
			break;
		case FLOW_ACTION_VLAN_MANGLE:
			if (admin->vtype == VCAP_TYPE_ES0)
				err = lan966x_tc_flower_parse_act_es0(vrule, act);
			else if (admin->vtype == VCAP_TYPE_IS1)
				err = lan966x_tc_flower_parse_act_is1(vrule, act);
			else
				err = -EINVAL;

			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set vlan mangle");
				goto out;
			}

			break;
		case FLOW_ACTION_VLAN_POP:
			if (admin->vtype != VCAP_TYPE_ES0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot use vlan pop on non es0");
				err = -EOPNOTSUPP;
				goto out;
			}

			/* Force untag */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_OUTER_TAG,
						       LAN966X_FORCE_UNTAGED);
			if (err)
				goto out;

			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (admin->vtype != VCAP_TYPE_ES0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot use vlan pop on non es0");
				err = -EOPNOTSUPP;
				goto out;
			}

			switch (be16_to_cpu(act->vlan.proto)) {
			case ETH_P_8021Q:
				err = vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_TPID_SEL, 0); /* 0x8100 */
				break;
			case ETH_P_8021AD:
				err = vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_TPID_SEL, 1); /* 0x88a8 */
				break;
			default:
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid vlan proto");
				err = -EINVAL;
				goto out;
			}

			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_OUTER_TAG, 1); /* Push ES0 tag A */
			err |= vcap_rule_add_action_bit(vrule, VCAP_AF_TAG_A_VID_SEL, VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_VID_A_VAL, act->vlan.vid);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_PCP_SEL, 1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PCP_A_VAL, act->vlan.prio);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_DEI_SEL, 0);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_INNER_TAG, 1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_B_TPID_SEL, 3);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set vlan push");

				err = -EINVAL;
				goto out;
			}
			break;
		case FLOW_ACTION_PRIORITY:
			if (act->priority > 7) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid skbedit priority");
				err = -EINVAL;
				goto out;
			}

			err = vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, act->priority);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set skkedit priority");
				err = -EINVAL;
				goto out;
			}

			break;
		case FLOW_ACTION_GATE:
			if (admin->vtype != VCAP_TYPE_IS1) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot use gate on non is1");
				err = -EOPNOTSUPP;
				goto out;
			}

			if (act->hw_index == U32_MAX) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot use reserved stream gate");
				return -EINVAL;
			}
			if ((act->gate.prio < -1) ||
			    (act->gate.prio > LAN966X_PSFP_SG_MAX_IPV)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid initial priority");
				return -EINVAL;
			}
			if ((act->gate.cycletime < LAN966X_PSFP_SG_MIN_CYCLE_TIME_NS) ||
			    (act->gate.cycletime > LAN966X_PSFP_SG_MAX_CYCLE_TIME_NS)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid cycle time");
				return -EINVAL;
			}
			if (act->gate.cycletimeext > LAN966X_PSFP_SG_MAX_CYCLE_TIME_NS) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid cycle time ext");
				return -EINVAL;
			}
			if (act->gate.num_entries >= LAN966X_PSFP_NUM_GCE) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid number of entries");
				return -EINVAL;
			}

			sg.gate_state = true;
			sg.ipv = act->gate.prio;
			sg.basetime = act->gate.basetime;
			sg.cycletime = act->gate.cycletime;
			sg.cycletimeext = act->gate.cycletimeext;
			sg.num_entries = act->gate.num_entries;

			for (int i = 0; i < act->gate.num_entries; i++) {
				if ((act->gate.entries[i].interval < LAN966X_PSFP_SG_MIN_CYCLE_TIME_NS) ||
				    (act->gate.entries[i].interval > LAN966X_PSFP_SG_MAX_CYCLE_TIME_NS)) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Invalid interval");
					err = -EINVAL;
					goto out;
				}
				if ((act->gate.entries[i].ipv < -1) ||
				    (act->gate.entries[i].ipv > LAN966X_PSFP_SG_MAX_IPV)) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Invalid internal priority");
					err = -EINVAL;
					goto out;
				}
				if (act->gate.entries[i].maxoctets < -1) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Invalid max octets");
					err = -EINVAL;
					goto out;
				}

				sg.gce[i].gate_state = (act->gate.entries[i].gate_state != 0);
				sg.gce[i].interval = act->gate.entries[i].interval;
				sg.gce[i].ipv = act->gate.entries[i].ipv;
				sg.gce[i].maxoctets = act->gate.entries[i].maxoctets;
			}

			err = lan966x_sfi_ix_reserve(port->lan966x,
						     &sfi_ix);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot reserve stream filter");
				goto out;
			}

			err = lan966x_sgi_ix_reserve(port->lan966x,
						     LAN966X_RES_POOL_USER_IS1,
						     act->hw_index,
						     &sgi_ix);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot reserve stream gate");
				goto out;
			}

			err = lan966x_psfp_sg_set(port->lan966x, sgi_ix, &sg);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set stream gate");
				goto out;
			}

			err = lan966x_psfp_sf_set(port->lan966x, sfi_ix, &sf);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set stream filter");
				goto out;
			}

			err = vcap_rule_add_action_bit(vrule, VCAP_AF_SGID_ENA, VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_SGID_VAL, sgi_ix);
			err |= vcap_rule_add_action_bit(vrule, VCAP_AF_SFID_ENA, VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_SFID_VAL, sfi_ix);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set sgid and sfid");

				err = -EINVAL;
				goto out;
			}

			break;
		case FLOW_ACTION_ACCEPT:
			lan966x_tc_set_default_actionset(admin, vrule,
							 f->common.chain_index);
			break;
		case FLOW_ACTION_GOTO:
			err = lan966x_tc_set_actionset(admin, vrule);
			if (err)
				goto out;

			err = lan966x_tc_add_rule_link(port->lan966x->vcap_ctrl,
						       admin, vrule,
						       f, act->chain_index);
			if (err)
				goto out;

			break;
		default:
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TC action");
			err = -EOPNOTSUPP;
			goto out;
		}
	}
	if (!lan966x_tc_flower_use_template(port->dev, f, admin, vrule)) {
		err = lan966x_tc_select_protocol_keyset(port->dev, vrule, admin,
							state.l3_proto, &multi);
		if (err) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "No matching port keyset for filter protocol and keys");
			goto out;
		}

		lookup = vcap_chain_id_to_lookup(admin, f->common.chain_index);
		if (vrule->keyset == VCAP_KFS_NORMAL_DMAC)
			lan966x_dmac_enable(port, lookup, true);
		else
			lan966x_dmac_enable(port, lookup, false);
	}

	err = lan966x_tc_add_rule_counter(admin, vrule);
	if (err) {
		vcap_set_tc_exterr(f, vrule);
		goto out;
	}

	err = vcap_val_rule(vrule, l3_proto);
	if (err) {
		vcap_set_tc_exterr(f, vrule);
		goto out;
	}

	err = vcap_add_rule(vrule);
	if (err)
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Could not add the filter");
	if (state.l3_proto == ETH_P_ALL)
		err = lan966x_tc_add_remaining_rules(port, f, vrule, admin,
						     &multi);

out:
	vcap_free_rule(vrule);
	return err;
}

static int lan966x_tc_free_rule_resources(struct net_device *ndev, int rule_id)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct vcap_client_actionfield *afield;
	struct lan966x *lan966x = port->lan966x;
	struct vcap_rule *vrule;
	int ret = 0;

	vrule = vcap_get_rule(port->lan966x->vcap_ctrl, rule_id);
	if (vrule == NULL || IS_ERR(vrule))
		return -EINVAL;

	/* Check for enabled mirroring in this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_MIRROR_ENA);
	if (afield && afield->ctrl.type == VCAP_FIELD_BIT && afield->data.u1.value) {
		pr_debug("%s:%d: rule %d: remove mirroring\n",
			 __func__, __LINE__, vrule->id);
		lan966x_mirror_vcap_del(lan966x);
	}

	/* Check for an enabled policer for this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_POLICE_ENA);
	if (afield && afield->ctrl.type == VCAP_FIELD_BIT && afield->data.u1.value) {
		/* Release policer reserved by this rule */
		ret = lan966x_tc_flower_release_policer(port, vrule);
	}
	vcap_free_rule(vrule);
	return ret;
}

static int lan966x_tc_flower_del(struct lan966x_port *port,
				 struct flow_cls_offload *f,
				 struct vcap_admin *admin)
{
	struct net_device *ndev = port->dev;
	struct vcap_control *vctrl;
	int err = -ENOENT, rule_id;
	int count = 0;

	vctrl = port->lan966x->vcap_ctrl;
	while (true) {
		rule_id = vcap_lookup_rule_by_cookie(vctrl, f->cookie);
		if (rule_id <= 0)
			break;
		if (count == 0) {
			/* Resources are attached to the first rule of
			 * a set of rules. Only works if the rules are
			 * in the correct order.
			 */
			err = lan966x_tc_free_rule_resources(ndev, rule_id);
			if (err)
				pr_err("%s:%d: could not get rule %d\n",
				       __func__, __LINE__, rule_id);
		}

		err = vcap_del_rule(vctrl, port->dev, rule_id);
		if (err) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Cannot delete rule");
			break;
		}
	}

	return err;
}

static int lan966x_tc_flower_stats(struct lan966x_port *port,
				   struct flow_cls_offload *f,
				   struct vcap_admin *admin)
{
	struct vcap_counter count = {};
	int err;

	err = vcap_get_rule_count_by_cookie(port->lan966x->vcap_ctrl,
					    &count, f->cookie);
	if (err)
		return err;

	flow_stats_update(&f->stats, 0x0, count.value, 0, 0,
			  FLOW_ACTION_HW_STATS_IMMEDIATE);

	return err;
}

static int lan966x_tc_flower_template_create(struct lan966x_port *port,
					     struct flow_cls_offload *fco,
					     struct vcap_admin *admin)
{
	struct vcap_tc_flower_parse_usage state = {
		.fco = fco,
		.l3_proto = ETH_P_ALL,
		.admin = admin,
	};
	struct lan966x_tc_flower_template *ftp;
	struct net_device *ndev = port->dev;
	struct vcap_keyset_list kslist = {};
	enum vcap_keyfield_set keysets[10];
	struct vcap_control *vctrl;
	struct vcap_rule *vrule;
	int count, err;

	if (admin->vtype == VCAP_TYPE_ES0) {
		pr_err("%s:%d: %s\n", __func__, __LINE__,
		       "VCAP does not support templates");
		return -EINVAL;
	}

	count = vcap_admin_rule_count(admin, fco->common.chain_index);
	if (count > 0) {
		pr_err("%s:%d: %s\n", __func__, __LINE__,
		       "Filters are already present");
		return -EBUSY;
	}

	ftp = kzalloc(sizeof(*ftp), GFP_KERNEL);
	if (!ftp)
		return -ENOMEM;

	ftp->cid = fco->common.chain_index;
	ftp->orig = VCAP_KFS_NO_VALUE;
	ftp->keyset = VCAP_KFS_NO_VALUE;

	vctrl = port->lan966x->vcap_ctrl;
	vrule = vcap_alloc_rule(vctrl, ndev, fco->common.chain_index,
				VCAP_USER_TC, fco->common.prio, 0);
	if (IS_ERR(vrule)) {
		err = PTR_ERR(vrule);
		goto err_rule;
	}

	state.vrule = vrule;
	state.frule = flow_cls_offload_flow_rule(fco);
	err = lan966x_tc_flower_use_dissectors(&state, admin, vrule);
	if (err) {
		pr_err("%s:%d: key error: %d\n", __func__, __LINE__, err);
		goto out;
	}

	ftp->l3_proto = state.l3_proto;

	lan966x_tc_flower_simplify_rule(admin, vrule, state.l3_proto);

	/* Find the keysets that the rule can use */
	kslist.keysets = keysets;
	kslist.max = ARRAY_SIZE(keysets);
	if (!vcap_rule_find_keysets(vrule, &kslist)) {
		pr_err("%s:%d: %s\n", __func__, __LINE__,
		       "Could not find a suitable keyset");
		err = -ENOENT;
		goto out;
	}

	ftp->keyset = vcap_select_min_rule_keyset(vctrl, admin->vtype, &kslist);
	kslist.cnt = 0;
	lan966x_vcap_set_port_keyset(ndev, admin, fco->common.chain_index,
				    state.l3_proto,
				    ftp->keyset,
				    &kslist);

	if (kslist.cnt > 0)
		ftp->orig = kslist.keysets[0];

	/* Store new template */
	list_add_tail(&ftp->list, &port->tc.templates);
	vcap_free_rule(vrule);
	return 0;

out:
	vcap_free_rule(vrule);
err_rule:
	kfree(ftp);
	return err;
}

static int lan966x_tc_flower_template_destroy(struct lan966x_port *port,
					      struct flow_cls_offload *fco,
					      struct vcap_admin *admin)
{
	struct lan966x_tc_flower_template *ftp, *tmp;
	struct net_device *ndev = port->dev;
	int err = -ENOENT;

	/* Rules using the template are removed by the tc framework */
	list_for_each_entry_safe(ftp, tmp, &port->tc.templates, list) {
		if (ftp->cid != fco->common.chain_index)
			continue;

		lan966x_vcap_set_port_keyset(ndev, admin,
					    fco->common.chain_index,
					    ftp->l3_proto, ftp->orig,
					    NULL);
		list_del(&ftp->list);
		kfree(ftp);
		break;
	}
	return err;
}

int lan966x_tc_flower(struct lan966x_port *port,
		      struct flow_cls_offload *f,
		      bool ingress)
{
	struct vcap_admin *admin;

	admin = vcap_find_admin(port->lan966x->vcap_ctrl,
				f->common.chain_index);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(f->common.extack, "Invalid chain");
		return -EINVAL;
	}

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return lan966x_tc_flower_add(port, f, admin, ingress);
	case FLOW_CLS_DESTROY:
		return lan966x_tc_flower_del(port, f, admin);
	case FLOW_CLS_STATS:
		return lan966x_tc_flower_stats(port, f, admin);
	case FLOW_CLS_TMPLT_CREATE:
		return lan966x_tc_flower_template_create(port, f, admin);
	case FLOW_CLS_TMPLT_DESTROY:
		return lan966x_tc_flower_template_destroy(port, f, admin);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}
