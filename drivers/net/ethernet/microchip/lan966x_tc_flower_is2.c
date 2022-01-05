/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <net/tcp.h> /* TCP flags */
#include "lan966x_main.h"

/* Supported dissectors for "protocol all" that is common for all keys: */
static const unsigned int lan966x_vcap_is2_dissectors_protocol_all =
	(BIT(FLOW_DISSECTOR_KEY_CONTROL) |
	 BIT(FLOW_DISSECTOR_KEY_BASIC) |
	 BIT(FLOW_DISSECTOR_KEY_VLAN));

/* Supported dissectors for each IS2 key: */
static const unsigned int lan966x_vcap_is2_dissectors[LAN966X_VCAP_IS2_KEY_LAST] = {
	[LAN966X_VCAP_IS2_KEY_MAC_ETYPE] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					    BIT(FLOW_DISSECTOR_KEY_BASIC) |
					    BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
					    BIT(FLOW_DISSECTOR_KEY_VLAN)),

	[LAN966X_VCAP_IS2_KEY_MAC_LLC] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					  BIT(FLOW_DISSECTOR_KEY_BASIC) |
					  BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
					  BIT(FLOW_DISSECTOR_KEY_VLAN)),

	[LAN966X_VCAP_IS2_KEY_MAC_SNAP] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					   BIT(FLOW_DISSECTOR_KEY_BASIC) |
					   BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
					   BIT(FLOW_DISSECTOR_KEY_VLAN)),

	[LAN966X_VCAP_IS2_KEY_ARP] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
				      BIT(FLOW_DISSECTOR_KEY_BASIC) |
				      BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
				      BIT(FLOW_DISSECTOR_KEY_VLAN) |
				      BIT(FLOW_DISSECTOR_KEY_ARP)),

	[LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					      BIT(FLOW_DISSECTOR_KEY_BASIC) |
					      BIT(FLOW_DISSECTOR_KEY_VLAN) |
					      BIT(FLOW_DISSECTOR_KEY_IP) |
					      BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
					      BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
					      BIT(FLOW_DISSECTOR_KEY_TCP) |
					      BIT(FLOW_DISSECTOR_KEY_PORTS)),

	[LAN966X_VCAP_IS2_KEY_IP4_OTHER] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					    BIT(FLOW_DISSECTOR_KEY_BASIC) |
					    BIT(FLOW_DISSECTOR_KEY_VLAN) |
					    BIT(FLOW_DISSECTOR_KEY_IP) |
					    BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
					    BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS)),

	[LAN966X_VCAP_IS2_KEY_IP6_STD] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					  BIT(FLOW_DISSECTOR_KEY_BASIC) |
					  BIT(FLOW_DISSECTOR_KEY_VLAN) |
					  BIT(FLOW_DISSECTOR_KEY_IP) |
					  BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
					  BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS)),

	[LAN966X_VCAP_IS2_KEY_OAM] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
				      BIT(FLOW_DISSECTOR_KEY_BASIC) |
				      BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
				      BIT(FLOW_DISSECTOR_KEY_VLAN)),

	[LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					      BIT(FLOW_DISSECTOR_KEY_BASIC) |
					      BIT(FLOW_DISSECTOR_KEY_VLAN) |
					      BIT(FLOW_DISSECTOR_KEY_IP) |
					      BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
					      BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
					      BIT(FLOW_DISSECTOR_KEY_TCP) |
					      BIT(FLOW_DISSECTOR_KEY_PORTS)),

	[LAN966X_VCAP_IS2_KEY_IP6_OTHER] = (BIT(FLOW_DISSECTOR_KEY_CONTROL) |
					    BIT(FLOW_DISSECTOR_KEY_BASIC) |
					    BIT(FLOW_DISSECTOR_KEY_VLAN) |
					    BIT(FLOW_DISSECTOR_KEY_IP) |
					    BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
					    BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS)),

	[LAN966X_VCAP_IS2_KEY_CUSTOM] = 0,

	[LAN966X_VCAP_IS2_KEY_SMAC_SIP4] = 0,

	[LAN966X_VCAP_IS2_KEY_SMAC_SIP6] = 0,
};

/* Supported match ids for "protocol all" that is common for all keys: */
static const unsigned int lan966x_vcap_is2_match_ids_protocol_all =
	BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN);

/* Supported match ids for each IS2 key: */
static const unsigned int lan966x_vcap_is2_match_ids[LAN966X_VCAP_IS2_KEY_LAST] = {
	[LAN966X_VCAP_IS2_KEY_MAC_ETYPE] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_ETYPE)),

	[LAN966X_VCAP_IS2_KEY_MAC_LLC] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					  BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
					  BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC)),

	[LAN966X_VCAP_IS2_KEY_MAC_SNAP] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					   BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
					   BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC)),

	[LAN966X_VCAP_IS2_KEY_ARP] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
				      BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
				      BIT(LAN966X_TC_FLOWER_MATCH_ID_ARP_SIP) |
				      BIT(LAN966X_TC_FLOWER_MATCH_ID_ARP_TIP) |
				      BIT(LAN966X_TC_FLOWER_MATCH_ID_ARP_OP)),

	[LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP4) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP4) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TTL) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_FIN) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_SYN) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_RST) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_PSH) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_ACK) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_URG)),

	[LAN966X_VCAP_IS2_KEY_IP4_OTHER] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP4) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP4) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TTL) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO)),

	[LAN966X_VCAP_IS2_KEY_IP6_STD] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					  BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
					  BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TTL) |
					  BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO)),

	[LAN966X_VCAP_IS2_KEY_OAM] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
				      BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
				      BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC)),

	[LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP6) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TTL) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_FIN) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_SYN) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_RST) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_PSH) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_ACK) |
					      BIT(LAN966X_TC_FLOWER_MATCH_ID_TCP_URG)),

	[LAN966X_VCAP_IS2_KEY_IP6_OTHER] = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP6) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TTL) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
					    BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO)),

	[LAN966X_VCAP_IS2_KEY_CUSTOM] = 0,

	[LAN966X_VCAP_IS2_KEY_SMAC_SIP4] = 0,

	[LAN966X_VCAP_IS2_KEY_SMAC_SIP6] = 0,
};

void lan966x_tc_flower_is2_x4_all(const struct lan966x_port *port,
				  const struct lan966x_vcap_rule *x2_rule,
				  struct lan966x_vcap_rule *x4_rule)
{
	const struct lan966x_vcap_is2_rule *x2 = &x2_rule->is2;
	const struct lan966x_vcap_is2_key_mac_etype *x2_key;
	struct lan966x_vcap_is2_rule *x4 = &x4_rule->is2;
	struct lan966x_vcap_is2_key_ip6_other *x4_key;

	if (x2->key.key != LAN966X_VCAP_IS2_KEY_MAC_ETYPE) {
		netdev_dbg(port->dev, "Wrong X2 key\n");
		return;
	}

	netdev_dbg(port->dev, "convert key %s to IP6_OTHER\n",
		   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS2,
					      x2->key.key)->name);
	x2_key = &x2->key.mac_etype;

	x4->key.key = LAN966X_VCAP_IS2_KEY_IP6_OTHER;
	x4_key = &x4->key.ip6_other;

	x4_key->type.value = LAN966X_VCAP_IS2_KEY_IP6_OTHER_TYPE_ID;
	x4_key->type.mask = 0x0e; /* Also match on IP6_TCP_UDP */
	x4_key->first = x2_key->first;
	x4_key->pag = x2_key->pag;
	x4_key->igr_port_mask = x2_key->igr_port_mask;
	x4_key->isdx_gt0 = x2_key->isdx_gt0;
	x4_key->host_match = x2_key->host_match;
	x4_key->l2_mc = x2_key->l2_mc;
	x4_key->l2_bc = x2_key->l2_bc;
	x4_key->vlan_tagged = x2_key->vlan_tagged;
	x4_key->vid = x2_key->vid;
	x4_key->dei = x2_key->dei;
	x4_key->pcp = x2_key->pcp;

	x4->action = x2->action;
}

/**
 * lan966x_tc_flower_is2_action - Check and parse X2 action BASE_TYPE
 * @port: The interface
 * @ci: Chain info
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if action is invalid.
 * -EOPNOTSUPP if action is unsupported.
 */
static int lan966x_tc_flower_is2_action(const struct lan966x_port *port,
					const struct lan966x_tc_ci *ci,
					struct flow_cls_offload *f,
					struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_action_base_type *base_type;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;
	struct flow_action *action = &rule->action;
	struct lan966x_tc_policer pol = { 0 };
	struct flow_action_entry *act;
	u64 action_mask = 0;
	int err, i;
	u32 pol_ix;
	u64 rate;

	err = lan966x_tc_flower_action_check(ci, f, &action_mask);
	if (err)
		return err;

	is2->action.action = LAN966X_VCAP_IS2_ACTION_BASE_TYPE;
	base_type = &is2->action.base_type;

	flow_action_for_each(i, act, action) {
		switch (act->id) {
		case FLOW_ACTION_POLICE:
			if (ci->lookup != 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Police action is only supported in first IS2 lookup");
				return -EOPNOTSUPP;
			}
			if (action_mask & BIT(FLOW_ACTION_DROP)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine police and drop action");
				return -EOPNOTSUPP;
			}
			if (action_mask & BIT(FLOW_ACTION_TRAP)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine police and trap action");
				return -EOPNOTSUPP;
			}
			err = lan966x_pol_ix_reserve(port->lan966x,
						     LAN966X_RES_POOL_USER_IS2,
						     act->hw_index,
						     &pol_ix);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot reserve policer");
				return err;
			}

			/* Save reserved policer in rule. This is used to
			 * release the policer when the rule is deleted */
			r->pol_user = LAN966X_RES_POOL_USER_IS2;
			r->pol_id = act->hw_index;

			base_type->police_ena = 1;
			base_type->police_idx = pol_ix;

			rate = act->police.rate_bytes_ps;
			pol.rate = div_u64(rate, 1000) * 8;
			pol.burst = act->police.burst;
			err = lan966x_tc_policer_set(port->lan966x, pol_ix,
						     &pol);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set policer");
				return err;
			}
			break;
		case FLOW_ACTION_MIRRED:
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

			/* Mark mirroring in use in rule */
			r->mirroring = true;

			base_type->mirror_ena = 1;
			break;
		case FLOW_ACTION_DROP:
			base_type->mask_mode = 1;
			base_type->police_ena = 1;
			base_type->police_idx = LAN966X_POL_IX_DISCARD;
			break;
		case FLOW_ACTION_TRAP:
			if (action_mask & BIT(FLOW_ACTION_DROP)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine trap and drop action");
				return -EOPNOTSUPP;
			}
			base_type->cpu_copy_ena = 1;
			base_type->cpu_qu_num = 0;
			base_type->mask_mode = 1;
			break;
		case FLOW_ACTION_ACCEPT:
			if (action_mask & BIT(FLOW_ACTION_DROP)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine pass and drop action");
				return -EOPNOTSUPP;
			}
			if (action_mask & BIT(FLOW_ACTION_TRAP)) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot combine pass and trap action");
				return -EOPNOTSUPP;
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
 * lan966x_tc_flower_is2_key_mac_etype - Check and parse X2 key MAC_ETYPE
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_mac_etype(const struct lan966x_port *port,
					       const struct lan966x_tc_ci *ci,
					       const struct lan966x_tc_flower_proto *p,
					       struct flow_cls_offload *f,
					       struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_mac_etype *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is2->key.key = LAN966X_VCAP_IS2_KEY_MAC_ETYPE;
	key = &is2->key.mac_etype;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		ether_addr_copy(key->l2_dmac.value, match.key->dst);
		ether_addr_copy(key->l2_dmac.mask, match.mask->dst);
		ether_addr_copy(key->l2_smac.value, match.key->src);
		ether_addr_copy(key->l2_smac.mask, match.mask->src);
	}

	if (p->l3 == ETH_P_ALL) {
		/* Wildcard the type to match any frame type */
		key->type.value = 0;
		key->type.mask = ~0x7;
	} else {
		key->etype.value = p->l3;
		key->etype.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_mac_llc - Check and parse X2 key MAC_LLC
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_mac_llc(const struct lan966x_port *port,
					     const struct lan966x_tc_ci *ci,
					     const struct lan966x_tc_flower_proto *p,
					     struct flow_cls_offload *f,
					     struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_mac_llc *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is2->key.key = LAN966X_VCAP_IS2_KEY_MAC_LLC;
	key = &is2->key.mac_llc;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		ether_addr_copy(key->l2_dmac.value, match.key->dst);
		ether_addr_copy(key->l2_dmac.mask, match.mask->dst);
		ether_addr_copy(key->l2_smac.value, match.key->src);
		ether_addr_copy(key->l2_smac.mask, match.mask->src);
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_mac_snap - Check and parse X2 key MAC_SNAP
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_mac_snap(const struct lan966x_port *port,
					      const struct lan966x_tc_ci *ci,
					      const struct lan966x_tc_flower_proto *p,
					      struct flow_cls_offload *f,
					      struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_mac_snap *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is2->key.key = LAN966X_VCAP_IS2_KEY_MAC_SNAP;
	key = &is2->key.mac_snap;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		ether_addr_copy(key->l2_dmac.value, match.key->dst);
		ether_addr_copy(key->l2_dmac.mask, match.mask->dst);
		ether_addr_copy(key->l2_smac.value, match.key->src);
		ether_addr_copy(key->l2_smac.mask, match.mask->src);
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_arp - Check and parse X2 key ARP
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_arp(const struct lan966x_port *port,
					 const struct lan966x_tc_ci *ci,
					 const struct lan966x_tc_flower_proto *p,
					 struct flow_cls_offload *f,
					 struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_arp *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is2->key.key = LAN966X_VCAP_IS2_KEY_ARP;
	key = &is2->key.arp;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		ether_addr_copy(key->l2_smac.value, match.key->src);
		ether_addr_copy(key->l2_smac.mask, match.mask->src);
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ARP)) {
		struct flow_match_arp match;

		flow_rule_match_arp(rule, &match);

		if (match.mask->op) {
			key->arp_opcode.mask = 3;
			if (p->l3 == ETH_P_ARP) {
				key->arp_opcode.value = match.key->op == 1 ? 0 : 1;
			} else { /* RARP */
				key->arp_opcode.value = match.key->op == 1 ? 2 : 3;
			}
		}

		key->l3_ip4_sip.value = be32_to_cpu(match.key->sip);
		key->l3_ip4_sip.mask = be32_to_cpu(match.mask->sip);
		key->l3_ip4_dip.value = be32_to_cpu(match.key->tip);
		key->l3_ip4_dip.mask = be32_to_cpu(match.mask->tip);
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_ip4_tcp_udp - Check and parse X2 key IP4_TCP_UDP
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_ip4_tcp_udp(const struct lan966x_port *port,
						 const struct lan966x_tc_ci *ci,
						 const struct lan966x_tc_flower_proto *p,
						 struct flow_cls_offload *f,
						 struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_ip4_tcp_udp *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;
	bool ipv4 = (p->l3 == ETH_P_IP);
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x/%d\n", p->l3, p->l4);
	is2->key.key = LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP;
	key = &is2->key.ip4_tcp_udp;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	/* Layer 3 */
	key->ip4 = ipv4 ? LAN966X_VCAP_BIT_1 : LAN966X_VCAP_BIT_0;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;

		if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
			if (match.key->flags & FLOW_DIS_IS_FRAGMENT)
				key->l3_fragment = LAN966X_VCAP_BIT_1;
			else
				key->l3_fragment = LAN966X_VCAP_BIT_0;
		}
		if (match.mask->flags & FLOW_DIS_FIRST_FRAG) {
			if (match.key->flags & FLOW_DIS_FIRST_FRAG)
				key->l3_frag_ofs_gt0 = LAN966X_VCAP_BIT_0;
			else
				key->l3_frag_ofs_gt0 = LAN966X_VCAP_BIT_1;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		if (match.mask->ttl) {
			if (match.key->ttl)
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_1;
			else
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_0;
		}

		key->l3_tos.value = match.key->tos;
		key->l3_tos.mask = match.mask->tos;
	}

	if (ipv4 && addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		key->l3_ip4_sip.value = be32_to_cpu(match.key->src);
		key->l3_ip4_sip.mask = be32_to_cpu(match.mask->src);
		key->l3_ip4_dip.value = be32_to_cpu(match.key->dst);
		key->l3_ip4_dip.mask = be32_to_cpu(match.mask->dst);
	}

	if (!ipv4 && addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		u8 *k, *m;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* Match on DIP6 is not possible in this key */
		k = &match.key->src.s6_addr[0];
		m = &match.mask->src.s6_addr[0];

		/* bits 63:32 are encoded in l3_ip4_dip
		 * 0000:0000:0000:0000:XXXX:XXXX:0000:0000 */
		key->l3_ip4_dip.value = (k[8] << 24) + (k[9] << 16) +
			(k[10] << 8) + k[11];
		key->l3_ip4_dip.mask = (m[8] << 24) + (m[9] << 16) +
			(m[10] << 8) + m[11];

		/* bits 31:0 are encoded in l3_ip4_sip
		 * 0000:0000:0000:0000:0000:0000:XXXX:XXXX */
		key->l3_ip4_sip.value = (k[12] << 24) + (k[13] << 16) +
			(k[14] << 8) + k[15];
		key->l3_ip4_sip.mask = (m[12] << 24) + (m[13] << 16) +
			(m[14] << 8) + m[15];
	}

	/* Layer 4 */
	if (p->l4 == IPPROTO_TCP) {
		key->tcp = LAN966X_VCAP_BIT_1;

		if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_TCP)) {
			struct flow_match_tcp tcp;
			u16 tcp_flags_mask;
			u16 tcp_flags_key;

			flow_rule_match_tcp(rule, &tcp);

			tcp_flags_key = be16_to_cpu(tcp.key->flags);
			tcp_flags_mask = be16_to_cpu(tcp.mask->flags);

			if (tcp_flags_mask & TCPHDR_FIN) {
				if (tcp_flags_key & TCPHDR_FIN)
					key->l4_fin = LAN966X_VCAP_BIT_1;
				else
					key->l4_fin = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_SYN) {
				if (tcp_flags_key & TCPHDR_SYN)
					key->l4_syn = LAN966X_VCAP_BIT_1;
				else
					key->l4_syn = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_RST) {
				if (tcp_flags_key & TCPHDR_RST)
					key->l4_rst = LAN966X_VCAP_BIT_1;
				else
					key->l4_rst = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_PSH) {
				if (tcp_flags_key & TCPHDR_PSH)
					key->l4_psh = LAN966X_VCAP_BIT_1;
				else
					key->l4_psh = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_ACK) {
				if (tcp_flags_key & TCPHDR_ACK)
					key->l4_ack = LAN966X_VCAP_BIT_1;
				else
					key->l4_ack = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_URG) {
				if (tcp_flags_key & TCPHDR_URG)
					key->l4_urg = LAN966X_VCAP_BIT_1;
				else
					key->l4_urg = LAN966X_VCAP_BIT_0;
			}
		}
	} else {
		key->tcp = LAN966X_VCAP_BIT_0;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		key->l4_sport.value = be16_to_cpu(match.key->src);
		key->l4_sport.mask = be16_to_cpu(match.mask->src);
		key->l4_dport.value = be16_to_cpu(match.key->dst);
		key->l4_dport.mask = be16_to_cpu(match.mask->dst);
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_ip4_other - Check and parse X2 key IP4_OTHER
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_ip4_other(const struct lan966x_port *port,
					       const struct lan966x_tc_ci *ci,
					       const struct lan966x_tc_flower_proto *p,
					       struct flow_cls_offload *f,
					       struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_ip4_other *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;
	bool ipv4 = (p->l3 == ETH_P_IP);
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x/%d\n", p->l3, p->l4);
	is2->key.key = LAN966X_VCAP_IS2_KEY_IP4_OTHER;
	key = &is2->key.ip4_other;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	/* Layer 3 */
	key->ip4 = ipv4 ? LAN966X_VCAP_BIT_1 : LAN966X_VCAP_BIT_0;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;

		if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
			if (match.key->flags & FLOW_DIS_IS_FRAGMENT)
				key->l3_fragment = LAN966X_VCAP_BIT_1;
			else
				key->l3_fragment = LAN966X_VCAP_BIT_0;
		}
		if (match.mask->flags & FLOW_DIS_FIRST_FRAG) {
			if (match.key->flags & FLOW_DIS_FIRST_FRAG)
				key->l3_frag_ofs_gt0 = LAN966X_VCAP_BIT_0;
			else
				key->l3_frag_ofs_gt0 = LAN966X_VCAP_BIT_1;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		if (match.mask->ttl) {
			if (match.key->ttl)
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_1;
			else
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_0;
		}

		key->l3_tos.value = match.key->tos;
		key->l3_tos.mask = match.mask->tos;
	}

	if (ipv4 && addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		key->l3_ip4_sip.value = be32_to_cpu(match.key->src);
		key->l3_ip4_sip.mask = be32_to_cpu(match.mask->src);
		key->l3_ip4_dip.value = be32_to_cpu(match.key->dst);
		key->l3_ip4_dip.mask = be32_to_cpu(match.mask->dst);
	}

	if (!ipv4 && addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		u8 *k, *m;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* Match on DIP6 is not possible in this key */
		k = &match.key->src.s6_addr[0];
		m = &match.mask->src.s6_addr[0];

		/* bits 63:32 are encoded in l3_ip4_dip
		 * 0000:0000:0000:0000:XXXX:XXXX:0000:0000 */
		key->l3_ip4_dip.value = (k[8] << 24) + (k[9] << 16) +
			(k[10] << 8) + k[11];
		key->l3_ip4_dip.mask = (m[8] << 24) + (m[9] << 16) +
			(m[10] << 8) + m[11];

		/* bits 31:0 are encoded in l3_ip4_sip
		 * 0000:0000:0000:0000:0000:0000:XXXX:XXXX */
		key->l3_ip4_sip.value = (k[12] << 24) + (k[13] << 16) +
			(k[14] << 8) + k[15];
		key->l3_ip4_sip.mask = (m[12] << 24) + (m[13] << 16) +
			(m[14] << 8) + m[15];
	}

	/* Layer 4 */
	if (p->l4 == 0) {
		/* Match all L4 protocols by setting the type
		 * to match on both tcp_udp and other */
		key->type.value = LAN966X_VCAP_IS2_KEY_IP4_OTHER_TYPE_ID;
		key->type.mask = 0x0e;
	} else {
		key->l3_proto.value = p->l4;
		key->l3_proto.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_ip6_std - Check and parse X2 key IP6_STD
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_ip6_std(const struct lan966x_port *port,
					     const struct lan966x_tc_ci *ci,
					     const struct lan966x_tc_flower_proto *p,
					     struct flow_cls_offload *f,
					     struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_ip6_std *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x/%d\n", p->l3, p->l4);
	is2->key.key = LAN966X_VCAP_IS2_KEY_IP6_STD;
	key = &is2->key.ip6_std;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	/* Layer 3 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		if (match.mask->ttl) {
			if (match.key->ttl)
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_1;
			else
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_0;
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		int i;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* Match on DIP6 is not possible in this key */
		for (i = 0; i < 16; i++) {
			key->l3_ip6_sip.value[i] = match.key->src.s6_addr[i];
			key->l3_ip6_sip.mask[i] = match.mask->src.s6_addr[i];
		}
	}

	/* Layer 4 */
	if (p->l4) {
		key->l3_proto.value = p->l4;
		key->l3_proto.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_oam - Check and parse X2 key OAM
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_oam(const struct lan966x_port *port,
					 const struct lan966x_tc_ci *ci,
					 const struct lan966x_tc_flower_proto *p,
					 struct flow_cls_offload *f,
					 struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_oam *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is2->key.key = LAN966X_VCAP_IS2_KEY_OAM;
	key = &is2->key.oam;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		ether_addr_copy(key->l2_dmac.value, match.key->dst);
		ether_addr_copy(key->l2_dmac.mask, match.mask->dst);
		ether_addr_copy(key->l2_smac.value, match.key->src);
		ether_addr_copy(key->l2_smac.mask, match.mask->src);
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_ip6_tcp_udp - Check and parse X4 key IP6_TCP_UDP
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_ip6_tcp_udp(const struct lan966x_port *port,
						 const struct lan966x_tc_ci *ci,
						 const struct lan966x_tc_flower_proto *p,
						 struct flow_cls_offload *f,
						 struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_ip6_tcp_udp *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x/%d\n", p->l3, p->l4);
	is2->key.key = LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP;
	key = &is2->key.ip6_tcp_udp;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	/* Layer 3 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		if (match.mask->ttl) {
			if (match.key->ttl)
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_1;
			else
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_0;
		}

		key->l3_tos.value = match.key->tos;
		key->l3_tos.mask = match.mask->tos;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		int i;

		flow_rule_match_ipv6_addrs(rule, &match);

		for (i = 0; i < 16; i++) {
			key->l3_ip6_sip.value[i] = match.key->src.s6_addr[i];
			key->l3_ip6_sip.mask[i] = match.mask->src.s6_addr[i];
			key->l3_ip6_dip.value[i] = match.key->dst.s6_addr[i];
			key->l3_ip6_dip.mask[i] = match.mask->dst.s6_addr[i];
		}
	}

	/* Layer 4 */
	if (p->l4 == IPPROTO_TCP) {
		key->tcp = LAN966X_VCAP_BIT_1;

		if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_TCP)) {
			struct flow_match_tcp tcp;
			u16 tcp_flags_mask;
			u16 tcp_flags_key;

			flow_rule_match_tcp(rule, &tcp);

			tcp_flags_key = be16_to_cpu(tcp.key->flags);
			tcp_flags_mask = be16_to_cpu(tcp.mask->flags);

			if (tcp_flags_mask & TCPHDR_FIN) {
				if (tcp_flags_key & TCPHDR_FIN)
					key->l4_fin = LAN966X_VCAP_BIT_1;
				else
					key->l4_fin = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_SYN) {
				if (tcp_flags_key & TCPHDR_SYN)
					key->l4_syn = LAN966X_VCAP_BIT_1;
				else
					key->l4_syn = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_RST) {
				if (tcp_flags_key & TCPHDR_RST)
					key->l4_rst = LAN966X_VCAP_BIT_1;
				else
					key->l4_rst = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_PSH) {
				if (tcp_flags_key & TCPHDR_PSH)
					key->l4_psh = LAN966X_VCAP_BIT_1;
				else
					key->l4_psh = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_ACK) {
				if (tcp_flags_key & TCPHDR_ACK)
					key->l4_ack = LAN966X_VCAP_BIT_1;
				else
					key->l4_ack = LAN966X_VCAP_BIT_0;
			}
			if (tcp_flags_mask & TCPHDR_URG) {
				if (tcp_flags_key & TCPHDR_URG)
					key->l4_urg = LAN966X_VCAP_BIT_1;
				else
					key->l4_urg = LAN966X_VCAP_BIT_0;
			}
		}
	} else {
		key->tcp = LAN966X_VCAP_BIT_0;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		key->l4_sport.value = be16_to_cpu(match.key->src);
		key->l4_sport.mask = be16_to_cpu(match.mask->src);
		key->l4_dport.value = be16_to_cpu(match.key->dst);
		key->l4_dport.mask = be16_to_cpu(match.mask->dst);
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key_ip6_other - Check and parse X4 key IP6_OTHER
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key_ip6_other(const struct lan966x_port *port,
					       const struct lan966x_tc_ci *ci,
					       const struct lan966x_tc_flower_proto *p,
					       struct flow_cls_offload *f,
					       struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is2_key_ip6_other *key;
	struct lan966x_vcap_is2_rule *is2 = &r->is2;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x/%d\n", p->l3, p->l4);
	is2->key.key = LAN966X_VCAP_IS2_KEY_IP6_OTHER;
	key = &is2->key.ip6_other;

	if (ci->lookup == 0) { /* First lookup */
		key->first = LAN966X_VCAP_BIT_1;
		key->pag.value = ci->pag_value;
		key->pag.mask = ~0;
	} else { /* Second lookup */
		key->first = LAN966X_VCAP_BIT_0;
	}

	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	/* Layer 3 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		if (match.mask->ttl) {
			if (match.key->ttl)
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_1;
			else
				key->l3_ttl_gt0 = LAN966X_VCAP_BIT_0;
		}

		key->l3_tos.value = match.key->tos;
		key->l3_tos.mask = match.mask->tos;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		int i;

		flow_rule_match_ipv6_addrs(rule, &match);

		for (i = 0; i < 16; i++) {
			key->l3_ip6_sip.value[i] = match.key->src.s6_addr[i];
			key->l3_ip6_sip.mask[i] = match.mask->src.s6_addr[i];
			key->l3_ip6_dip.value[i] = match.key->dst.s6_addr[i];
			key->l3_ip6_dip.mask[i] = match.mask->dst.s6_addr[i];
		}
	}

	/* Layer 4 */
	if (p->l4 == 0) {
		/* Match all L4 protocols by setting the type
		 * to match on both tcp_udp and other */
		key->type.value = LAN966X_VCAP_IS2_KEY_IP6_OTHER_TYPE_ID;
		key->type.mask = 0x0e;
	} else {
		key->l3_proto.value = p->l4;
		key->l3_proto.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is2_key - Check and parse TC IS2 key
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS2 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is2_key(const struct lan966x_port *port,
				     const struct lan966x_tc_ci *ci,
				     const struct lan966x_tc_flower_proto *p,
				     struct flow_cls_offload *f,
				     struct lan966x_vcap_rule *r)
{
	enum lan966x_vcap_is2_key ipv6_key, key;
	unsigned int dissectors, match_ids;
	bool x4 = false;
	int err;

	err = lan966x_vcap_is2_port_key_ipv6_get(port, ci->lookup, &ipv6_key);
	if (err)
		return err;

	if (ipv6_key == LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP)
		x4 = true; /* X4 keys are possible in this lookup */

	/* Get key from protocol */
	switch (p->l3) {
	case ETH_P_IPV6:
		key = ipv6_key;
		if (!p->tcp_udp) {
			if (key == LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP)
				key = LAN966X_VCAP_IS2_KEY_IP4_OTHER;
			else if (key == LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP)
				key = LAN966X_VCAP_IS2_KEY_IP6_OTHER;
		}
		break;
	case ETH_P_IP:
		if (p->tcp_udp)
			key = LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP;
		else
			key = LAN966X_VCAP_IS2_KEY_IP4_OTHER;
		break;
	case 0x8809: /* Ethernet slow protocols */
	case 0x88ee: /* MEF 16 E-LMI */
	case 0x8902: /* IEEE 802.1ag Connectivity Fault Management */
		key = LAN966X_VCAP_IS2_KEY_OAM;
		break;
	case ETH_P_ARP:
	case ETH_P_RARP:
		key = LAN966X_VCAP_IS2_KEY_ARP;
		break;
	case ETH_P_SNAP:
		key = LAN966X_VCAP_IS2_KEY_MAC_SNAP;
		break;
	case ETH_P_802_2:
		key = LAN966X_VCAP_IS2_KEY_MAC_LLC;
		break;
	default:
		if (p->l3 != ETH_P_ALL && p->l3 < ETH_P_802_3_MIN) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported protocol");
			return -EOPNOTSUPP;
		}

		key = LAN966X_VCAP_IS2_KEY_MAC_ETYPE;
		break;
	}

	if (x4 && p->l3 == ETH_P_ALL)
		r->is2_x4_all = true;

	netdev_dbg(port->dev, "Protocol 0x%04x/%d matches key %s\n",
		   p->l3, p->l4,
		   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS2, key)->name);

	/* Check supported dissectors and match ids */
	if (p->l3 == ETH_P_ALL) {
	    dissectors = lan966x_vcap_is2_dissectors_protocol_all;
	    match_ids = lan966x_vcap_is2_match_ids_protocol_all;
	} else {
	    dissectors = lan966x_vcap_is2_dissectors[key];
	    match_ids = lan966x_vcap_is2_match_ids[key];
	}

	err = lan966x_tc_flower_match_info_get(f, dissectors, match_ids, NULL);
	if (err)
		return err;

	switch (key) {
	case LAN966X_VCAP_IS2_KEY_MAC_ETYPE:
		return lan966x_tc_flower_is2_key_mac_etype(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_MAC_LLC:
		return lan966x_tc_flower_is2_key_mac_llc(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_MAC_SNAP:
		return lan966x_tc_flower_is2_key_mac_snap(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_ARP:
		return lan966x_tc_flower_is2_key_arp(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP:
		return lan966x_tc_flower_is2_key_ip4_tcp_udp(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_IP4_OTHER:
		return lan966x_tc_flower_is2_key_ip4_other(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_IP6_STD:
		return lan966x_tc_flower_is2_key_ip6_std(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_OAM:
		return lan966x_tc_flower_is2_key_oam(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP:
		return lan966x_tc_flower_is2_key_ip6_tcp_udp(port, ci, p, f, r);
	case LAN966X_VCAP_IS2_KEY_IP6_OTHER:
		return lan966x_tc_flower_is2_key_ip6_other(port, ci, p, f, r);
	default:
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Unsupported key");
		return -EOPNOTSUPP;
	}
}

int lan966x_tc_flower_is2_parse(const struct lan966x_port *port,
				const struct lan966x_tc_ci *ci,
				const struct lan966x_tc_flower_proto *p,
				struct flow_cls_offload *f,
				struct lan966x_vcap_rule *r)
{
	int err = lan966x_tc_flower_is2_key(port, ci, p, f, r);
	if (err)
		return err;

	return lan966x_tc_flower_is2_action(port, ci, f, r);
}
