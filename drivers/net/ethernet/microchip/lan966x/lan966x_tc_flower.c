/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <net/tcp.h> /* TCP flags */
#include <net/tc_act/tc_gate.h>

#include "vcap_api_client.h"
#include "lan966x_vcap_impl.h"
#include "lan966x_main.h"

#define LAN966X_VCAP_KEYS_MAX 50
#define LAN966X_MAX_RULE_SIZE 5 /* allows X1, X2 and X4 rules */

#define ETH_P_RTAG	0xF1C1          /* Redundancy Tag (IEEE 802.1CB) */
#define ETH_P_ELMI	0x88EE          /* MEF 16 E-LMI */

/* Collect keysets and type ids for multiple rules per size */
struct lan966x_wildcard_rule {
	bool selected;
	uint8_t value;
	uint8_t mask;
	enum vcap_keyfield_set keyset;
};
struct lan966x_multiple_rules {
	struct lan966x_wildcard_rule rule[LAN966X_MAX_RULE_SIZE];
};

struct lan966x_tc_flower_parse_keylist {
	struct flow_cls_offload *fco;
	struct flow_rule *frule;
	struct vcap_admin *admin;
	struct vcap_key_list *keylist;
	u16 l3_proto;
	u8 l4_proto;
};

struct lan966x_tc_flower_parse_usage {
	struct flow_cls_offload *fco;
	struct flow_rule *frule;
	struct vcap_admin *admin;
	struct vcap_rule *vrule;
	u16 l3_proto;
	u8 l4_proto;
	unsigned int used_keys;
	struct lan966x_port *port;
};

struct lan966x_tc_rule_pkt_cnt {
	u64 cookie;
	u32 pkts;
};

struct lan966x_tc_flower_template {
	struct list_head list; /* for insertion in the list of templates */
	int vcap_chain_id; /* used by tc */
	struct vcap_key_list keylist; /* keys used by the template */
	enum vcap_key_field vkeys[LAN966X_VCAP_KEYS_MAX];
	enum vcap_keyfield_set original; /* port keyset used before the template */
	enum vcap_keyfield_set keyset; /* template derived keyset */
	u16 l3_proto; /* ethertype for keyset */
	u8 l4_proto; /* ip protocol for keyset */
};

static u16 lan966x_tc_known_etypes[] = {
	ETH_P_ALL,
	ETH_P_IP,
	ETH_P_ARP,
	ETH_P_IPV6,
	ETH_P_RTAG,
	ETH_P_SNAP,    /* IS2 */
	ETH_P_802_2,   /* IS2 */
	ETH_P_SLOW,    /* IS2 */
	ETH_P_CFM,     /* IS2 */
	ETH_P_ELMI,    /* IS2 */
};

static bool lan966x_tc_is_known_etype(u16 etype)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(lan966x_tc_known_etypes); ++idx)
		if (lan966x_tc_known_etypes[idx] == etype)
			return true;
	return false;
}

/* Copy to host byte order */
static void lan966x_netbytes_copy(u8 *dst, u8 *src, int count)
{
	int idx;

	for (idx = 0; idx < count; ++idx, ++dst)
		*dst = src[count - idx - 1];
}

int lan966x_tc_flower_handler_control_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_control match;

	flow_rule_match_control(st->frule, &match);
	if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
		vcap_key_list_add(st->keylist, VCAP_KF_L3_FRAGMENT);
		vcap_key_list_add(st->keylist, VCAP_KF_L3_FRAG_OFS_GT0);
	}
	return 0;
}

int lan966x_tc_flower_handler_control_usage(struct lan966x_tc_flower_parse_usage *st)
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
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_CONTROL);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_frag parse error");
	return err;
}

int lan966x_tc_flower_handler_basic_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_basic match;

	flow_rule_match_basic(st->frule, &match);
	if (match.mask->n_proto)
		st->l3_proto = be16_to_cpu(match.key->n_proto);

	if (match.mask->ip_proto)
		st->l4_proto = match.key->ip_proto;

	return 0;
}

int lan966x_tc_flower_handler_basic_usage(struct lan966x_tc_flower_parse_usage *st)
{
	struct flow_match_basic match;
	int err = 0;

	flow_rule_match_basic(st->frule, &match);
	if (match.mask->n_proto) {
		st->l3_proto = be16_to_cpu(match.key->n_proto);
		if (!lan966x_tc_is_known_etype(st->l3_proto)) {
			err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_ETYPE,
						    st->l3_proto, ~0);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IP) {
			err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_IP4_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IPV6) {
			/* Not available in IP6 type keysets */
			/* err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_IP4_IS, */
			/* 			    VCAP_BIT_0); */
			/* if (err) */
			/* 	goto out; */
		} else if (st->l3_proto == ETH_P_ALL) {
			/* Nothing to do */
		} else if (st->l3_proto == ETH_P_SNAP) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_ETYPE_LEN_IS,
						      VCAP_BIT_0);
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_IP_SNAP_IS,
						      VCAP_BIT_1);
			}
		} else if (st->l3_proto == ETH_P_RTAG) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_8021CB_R_TAGGED_IS,
						      VCAP_BIT_1);
			}
		} else {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_ETYPE_LEN_IS,
						      VCAP_BIT_1);
				vcap_rule_add_key_u32(st->vrule, VCAP_KF_ETYPE,
						      st->l3_proto, ~0);
			}
		}
	}
	if (match.mask->ip_proto) {
		st->l4_proto = match.key->ip_proto;

		if (st->l4_proto == IPPROTO_TCP) {
			if (st->admin->vtype == VCAP_TYPE_IS1)
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_TCP_UDP_IS,
						      VCAP_BIT_1);

			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_TCP_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l4_proto == IPPROTO_UDP) {
			/* Only in IS1 */
			if (st->admin->vtype == VCAP_TYPE_IS1)
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_TCP_UDP_IS,
						      VCAP_BIT_1);

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

	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_BASIC);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_proto parse error");
	return err;
}

int lan966x_tc_flower_handler_basic_usage_normal_ipv6(struct lan966x_tc_flower_parse_usage *st)
{
	struct flow_match_basic match;
	int err = 0;

	flow_rule_match_basic(st->frule, &match);
	if (match.mask->n_proto) {
		st->l3_proto = be16_to_cpu(match.key->n_proto);
		if (!lan966x_tc_is_known_etype(st->l3_proto)) {
			err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_ETYPE,
						    st->l3_proto, ~0);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IP) {
			err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_IP4_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IPV6) {
			/* Not available in IP6 type keysets */
			/* err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_IP4_IS, */
			/* 			    VCAP_BIT_0); */
			/* if (err) */
			/* 	goto out; */
		} else if (st->l3_proto == ETH_P_ALL) {
			/* Nothing to do */
		} else {
			vcap_rule_add_key_bit(st->vrule, VCAP_KF_ETYPE_LEN_IS,
					      VCAP_BIT_1);
			vcap_rule_add_key_u32(st->vrule, VCAP_KF_ETYPE,
					      st->l3_proto, ~0);
		}
	}
	if (match.mask->ip_proto) {
		st->l4_proto = match.key->ip_proto;

		if (st->l4_proto == IPPROTO_TCP ||
		    st->l4_proto == IPPROTO_UDP)
			vcap_rule_add_key_bit(st->vrule, VCAP_KF_TCP_UDP_IS,
					      VCAP_BIT_1);
		else
			vcap_rule_add_key_bit(st->vrule, VCAP_KF_TCP_UDP_IS,
					      VCAP_BIT_0);

		vcap_rule_add_key_u32(st->vrule, VCAP_KF_L3_IP_PROTO,
				      st->l4_proto, ~0);
	}

	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_BASIC);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_proto parse error");
	return err;
}

int lan966x_tc_flower_handler_ipv4_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	if (st->l3_proto == ETH_P_IP) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(st->frule, &match);
		if (match.mask->src)
			vcap_key_list_add(st->keylist, VCAP_KF_L3_IP4_SIP);
		if (match.mask->dst)
			vcap_key_list_add(st->keylist, VCAP_KF_L3_IP4_DIP);
	}
	return 0;
}

int lan966x_tc_flower_handler_ipv4_usage(struct lan966x_tc_flower_parse_usage *st)
{
	int err = 0;

	if (st->l3_proto == ETH_P_IP) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(st->frule, &match);
		if (match.mask->src) {
			err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L3_IP4_SIP,
						    be32_to_cpu(match.key->src),
						    be32_to_cpu(match.mask->src));
			if (err)
				goto out;
		}
		if (match.mask->dst) {
			err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L3_IP4_DIP,
						    be32_to_cpu(match.key->dst),
						    be32_to_cpu(match.mask->dst));
			if (err)
				goto out;
		}
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ipv4_addr parse error");
	return err;
}

int lan966x_tc_flower_handler_ipv6_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	if (st->l3_proto == ETH_P_IPV6) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(st->frule, &match);
		if (!ipv6_addr_any(&match.mask->src))
			vcap_key_list_add(st->keylist, VCAP_KF_L3_IP6_SIP);
		if (!ipv6_addr_any(&match.mask->dst))
			vcap_key_list_add(st->keylist, VCAP_KF_L3_IP6_DIP);
	}
	return 0;
}

int lan966x_tc_flower_handler_ipv6_usage(struct lan966x_tc_flower_parse_usage *st)
{
	int err = 0;

	if (st->l3_proto == ETH_P_IPV6) {
		struct flow_match_ipv6_addrs match;
		struct vcap_u128_key sip;
		struct vcap_u128_key dip;

		flow_rule_match_ipv6_addrs(st->frule, &match);
		/* Check if address masks are non-zero */
		if (!ipv6_addr_any(&match.mask->src)) {
			lan966x_netbytes_copy(sip.value, match.key->src.s6_addr, 16);
			lan966x_netbytes_copy(sip.mask, match.mask->src.s6_addr, 16);
			err = vcap_rule_add_key_u128(st->vrule, VCAP_KF_L3_IP6_SIP, &sip);
			if (err)
				goto out;
		}
		if (!ipv6_addr_any(&match.mask->dst)) {
			lan966x_netbytes_copy(dip.value, match.key->dst.s6_addr, 16);
			lan966x_netbytes_copy(dip.mask, match.mask->dst.s6_addr, 16);
			err = vcap_rule_add_key_u128(st->vrule, VCAP_KF_L3_IP6_DIP, &dip);
			if (err)
				goto out;
		}
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ipv6_addr parse error");
	return err;
}

int lan966x_tc_flower_handler_portnum_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_ports match;
	enum vcap_key_field key;

	if (st->admin->vtype == VCAP_TYPE_IS1)
		key = VCAP_KF_ETYPE;
	else
		key = VCAP_KF_L4_DPORT;

	flow_rule_match_ports(st->frule, &match);
	if (match.mask->src)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_SPORT);
	if (match.mask->dst)
		vcap_key_list_add(st->keylist, key);
	return 0;
}

int lan966x_tc_flower_handler_portnum_usage(struct lan966x_tc_flower_parse_usage *st)
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

int lan966x_tc_flower_handler_ethaddr_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_eth_addrs match;

	flow_rule_match_eth_addrs(st->frule, &match);
	if (!is_zero_ether_addr(match.mask->src))
		vcap_key_list_add(st->keylist, VCAP_KF_L2_SMAC);
	if (!is_zero_ether_addr(match.mask->dst))
		vcap_key_list_add(st->keylist, VCAP_KF_L2_DMAC);
	return 0;
}

int lan966x_tc_flower_handler_ethaddr_usage(struct lan966x_tc_flower_parse_usage *st)
{
	struct flow_match_eth_addrs match;
	enum vcap_key_field smac_key = VCAP_KF_L2_SMAC;
	enum vcap_key_field dmac_key = VCAP_KF_L2_DMAC;
	struct vcap_u48_key smac, dmac;
	int err = 0;

	flow_rule_match_eth_addrs(st->frule, &match);
	if (!is_zero_ether_addr(match.mask->src)) {
		lan966x_netbytes_copy(smac.value, match.key->src, ETH_ALEN);
		lan966x_netbytes_copy(smac.mask, match.mask->src, ETH_ALEN);
		err = vcap_rule_add_key_u48(st->vrule, smac_key, &smac);
		if (err)
			goto out;
	}
	if (!is_zero_ether_addr(match.mask->dst)) {
		lan966x_netbytes_copy(dmac.value, match.key->dst, ETH_ALEN);
		lan966x_netbytes_copy(dmac.mask, match.mask->dst, ETH_ALEN);
		err = vcap_rule_add_key_u48(st->vrule, dmac_key, &dmac);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "eth_addr parse error");
	return err;
}

int lan966x_tc_flower_handler_arp_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_arp match;

	flow_rule_match_arp(st->frule, &match);
	if (match.mask->op)
		vcap_key_list_add(st->keylist, VCAP_KF_ARP_OPCODE);
	if (match.mask->sip)
		vcap_key_list_add(st->keylist, VCAP_KF_L3_IP4_SIP);
	if (match.mask->tip)
		vcap_key_list_add(st->keylist, VCAP_KF_L3_IP4_DIP);
	return 0;
}

int lan966x_tc_flower_handler_arp_usage(struct lan966x_tc_flower_parse_usage *st)
{
	struct flow_match_arp match;
	u16 value, mask;
	int err;

	flow_rule_match_arp(st->frule, &match);
	if (match.mask->op) {
		mask = 0x3;
		if (st->l3_proto == ETH_P_ARP) {
			value = match.key->op == 1 ? 0 : 1;
		} else { /* RARP */
			value = match.key->op == 1 ? 2 : 3;
		}
		err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_ARP_OPCODE, value, mask);
		if (err)
			goto out;
	}
	if (match.mask->sip) {
		err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L3_IP4_SIP,
					    be32_to_cpu(match.key->sip),
					    be32_to_cpu(match.mask->sip));
		if (err)
			goto out;
	}
	if (match.mask->tip) {
		err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L3_IP4_DIP,
					    be32_to_cpu(match.key->tip),
					    be32_to_cpu(match.mask->tip));
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_ARP);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "arp parse error");
	return err;
}

int lan966x_tc_flower_handler_vlan_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_vlan match;
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID_CLS;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP_CLS;

	flow_rule_match_vlan(st->frule, &match);
	if (st->admin->vtype == VCAP_TYPE_IS1) {
		vid_key = VCAP_KF_8021Q_VID0;
		pcp_key = VCAP_KF_8021Q_PCP0;
	}
	if (match.mask->vlan_id)
		vcap_key_list_add(st->keylist, vid_key);
	if (match.mask->vlan_priority)
		vcap_key_list_add(st->keylist, pcp_key);
	return 0;
}

int lan966x_tc_flower_handler_vlan_usage(struct lan966x_tc_flower_parse_usage *st)
{
	struct flow_match_vlan match;
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID_CLS;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP_CLS;
	int err;

	flow_rule_match_vlan(st->frule, &match);
	if (st->admin->vtype == VCAP_TYPE_IS1) {
		vid_key = VCAP_KF_8021Q_VID0;
		pcp_key = VCAP_KF_8021Q_PCP0;
	}
	if (match.mask->vlan_id) {
		err = vcap_rule_add_key_u32(st->vrule, vid_key,
					    match.key->vlan_id,
					    match.mask->vlan_id);
		if (err)
			goto out;
	}
	if (match.mask->vlan_priority) {
		err = vcap_rule_add_key_u32(st->vrule, pcp_key,
					    match.key->vlan_priority,
					    match.mask->vlan_priority);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_VLAN);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "vlan parse error");
	return err;
}

int lan966x_tc_flower_handler_tcp_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_tcp tcp;
	u16 tcp_flags_mask;

	flow_rule_match_tcp(st->frule, &tcp);
	tcp_flags_mask = be16_to_cpu(tcp.mask->flags);

	if (tcp_flags_mask & TCPHDR_FIN)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_FIN);
	if (tcp_flags_mask & TCPHDR_SYN)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_SYN);
	if (tcp_flags_mask & TCPHDR_RST)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_RST);
	if (tcp_flags_mask & TCPHDR_PSH)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_PSH);
	if (tcp_flags_mask & TCPHDR_ACK)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_ACK);
	if (tcp_flags_mask & TCPHDR_URG)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_URG);
	return 0;
}

int lan966x_tc_flower_handler_tcp_usage(struct lan966x_tc_flower_parse_usage *st)
{
	struct flow_match_tcp tcp;
	u16 tcp_flags_key;
	u16 tcp_flags_mask;
	enum vcap_bit val;
	int err = 0;

	flow_rule_match_tcp(st->frule, &tcp);
	tcp_flags_key = be16_to_cpu(tcp.key->flags);
	tcp_flags_mask = be16_to_cpu(tcp.mask->flags);

	if (tcp_flags_mask & TCPHDR_FIN) {
		val = VCAP_BIT_0;
		if (tcp_flags_key & TCPHDR_FIN)
			val = VCAP_BIT_1;
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L4_FIN, val);
		if (err)
			goto out;
	}
	if (tcp_flags_mask & TCPHDR_SYN) {
		val = VCAP_BIT_0;
		if (tcp_flags_key & TCPHDR_SYN)
			val = VCAP_BIT_1;
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L4_SYN, val);
		if (err)
			goto out;
	}
	if (tcp_flags_mask & TCPHDR_RST) {
		val = VCAP_BIT_0;
		if (tcp_flags_key & TCPHDR_RST)
			val = VCAP_BIT_1;
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L4_RST, val);
		if (err)
			goto out;
	}
	if (tcp_flags_mask & TCPHDR_PSH) {
		val = VCAP_BIT_0;
		if (tcp_flags_key & TCPHDR_PSH)
			val = VCAP_BIT_1;
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L4_PSH, val);
		if (err)
			goto out;
	}
	if (tcp_flags_mask & TCPHDR_ACK) {
		val = VCAP_BIT_0;
		if (tcp_flags_key & TCPHDR_ACK)
			val = VCAP_BIT_1;
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L4_ACK, val);
		if (err)
			goto out;
	}
	if (tcp_flags_mask & TCPHDR_URG) {
		val = VCAP_BIT_0;
		if (tcp_flags_key & TCPHDR_URG)
			val = VCAP_BIT_1;
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L4_URG, val);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_TCP);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "tcp_flags parse error");
	return err;
}

int lan966x_tc_flower_handler_ip_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	struct flow_match_ip match;
	enum vcap_key_field key;

	flow_rule_match_ip(st->frule, &match);

	if (st->admin->vtype == VCAP_TYPE_IS1)
		key = VCAP_KF_L3_DSCP;
	else
		key = VCAP_KF_L3_TOS;


	if (match.mask->tos)
		vcap_key_list_add(st->keylist, key);
	return 0;
}

int lan966x_tc_flower_handler_ip_usage(struct lan966x_tc_flower_parse_usage *st)
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

int lan966x_tc_flower_handler_cvlan_keylist(struct lan966x_tc_flower_parse_keylist *st)
{
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID0;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP0;
	struct flow_match_vlan match;
	u16 tpid;

	if (st->admin->vtype != VCAP_TYPE_IS1)
		return -EINVAL;
	flow_rule_match_cvlan(st->frule, &match);
	tpid = be16_to_cpu(match.key->vlan_tpid);
	if (tpid == ETH_P_8021Q) {
		vid_key = VCAP_KF_8021Q_VID1;
		pcp_key = VCAP_KF_8021Q_PCP1;
	}
	if (match.mask->vlan_id)
		vcap_key_list_add(st->keylist, vid_key);
	if (match.mask->vlan_priority)
		vcap_key_list_add(st->keylist, pcp_key);
	return 0;
}

int lan966x_tc_flower_handler_cvlan_usage(struct lan966x_tc_flower_parse_usage *st)

{
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID0;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP0;
	struct flow_match_vlan match;
	u16 tpid;
	int err;

	if (st->admin->vtype != VCAP_TYPE_IS1)
		return -EINVAL;
	flow_rule_match_cvlan(st->frule, &match);
	tpid = be16_to_cpu(match.key->vlan_tpid);
	if (tpid == ETH_P_8021Q) {
		vid_key = VCAP_KF_8021Q_VID1;
		pcp_key = VCAP_KF_8021Q_PCP1;
	}
	if (match.mask->vlan_id) {
		err = vcap_rule_add_key_u32(st->vrule, vid_key,
					    match.key->vlan_id,
					    match.mask->vlan_id);
		if (err)
			goto out;
	}
	if (match.mask->vlan_priority) {
		err = vcap_rule_add_key_u32(st->vrule, pcp_key,
					    match.key->vlan_priority,
					    match.mask->vlan_priority);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_CVLAN);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "cvlan parse error");
	return err;
}

int (*lan966x_tc_flower_keylist_handlers[])(struct lan966x_tc_flower_parse_keylist *st) = {
	[FLOW_DISSECTOR_KEY_CONTROL] = lan966x_tc_flower_handler_control_keylist,
	[FLOW_DISSECTOR_KEY_BASIC] = lan966x_tc_flower_handler_basic_keylist,
	[FLOW_DISSECTOR_KEY_IPV4_ADDRS] = lan966x_tc_flower_handler_ipv4_keylist,
	[FLOW_DISSECTOR_KEY_IPV6_ADDRS] = lan966x_tc_flower_handler_ipv6_keylist,
	[FLOW_DISSECTOR_KEY_PORTS] = lan966x_tc_flower_handler_portnum_keylist,
	[FLOW_DISSECTOR_KEY_ETH_ADDRS] = lan966x_tc_flower_handler_ethaddr_keylist,
	[FLOW_DISSECTOR_KEY_ARP] = lan966x_tc_flower_handler_arp_keylist,
	[FLOW_DISSECTOR_KEY_VLAN] = lan966x_tc_flower_handler_vlan_keylist,
	[FLOW_DISSECTOR_KEY_TCP] = lan966x_tc_flower_handler_tcp_keylist,
	[FLOW_DISSECTOR_KEY_IP] = lan966x_tc_flower_handler_ip_keylist,
	[FLOW_DISSECTOR_KEY_CVLAN] = lan966x_tc_flower_handler_cvlan_keylist,
};

int (*lan966x_tc_flower_usage_handlers[])(struct lan966x_tc_flower_parse_usage *st) = {
	[FLOW_DISSECTOR_KEY_CONTROL] = lan966x_tc_flower_handler_control_usage,
	[FLOW_DISSECTOR_KEY_BASIC] = lan966x_tc_flower_handler_basic_usage,
	[FLOW_DISSECTOR_KEY_IPV4_ADDRS] = lan966x_tc_flower_handler_ipv4_usage,
	[FLOW_DISSECTOR_KEY_IPV6_ADDRS] = lan966x_tc_flower_handler_ipv6_usage,
	[FLOW_DISSECTOR_KEY_PORTS] = lan966x_tc_flower_handler_portnum_usage,
	[FLOW_DISSECTOR_KEY_ETH_ADDRS] = lan966x_tc_flower_handler_ethaddr_usage,
	[FLOW_DISSECTOR_KEY_ARP] = lan966x_tc_flower_handler_arp_usage,
	[FLOW_DISSECTOR_KEY_VLAN] = lan966x_tc_flower_handler_vlan_usage,
	[FLOW_DISSECTOR_KEY_TCP] = lan966x_tc_flower_handler_tcp_usage,
	[FLOW_DISSECTOR_KEY_IP] = lan966x_tc_flower_handler_ip_usage,
	[FLOW_DISSECTOR_KEY_CVLAN] = lan966x_tc_flower_handler_cvlan_usage,
};

static int lan966x_tc_match_dissectors(struct flow_cls_offload *fco,
				      struct vcap_admin *admin,
				      struct vcap_key_list *keylist,
				      u16 *l3,
				      u8 *l4)
{
	struct lan966x_tc_flower_parse_keylist state = {
		.fco = fco,
		.admin = admin,
		.keylist = keylist,
		.l3_proto = ETH_P_ALL,
	};
	int idx, err = 0;

	state.frule = flow_cls_offload_flow_rule(fco);
	for (idx = 0; idx < ARRAY_SIZE(lan966x_tc_flower_keylist_handlers); ++idx)
		if (lan966x_tc_flower_keylist_handlers[idx] &&
			flow_rule_match_key(state.frule, idx))
				lan966x_tc_flower_keylist_handlers[idx](&state);
	*l3 = state.l3_proto;
	*l4 = state.l4_proto;
	return err;
}

static enum vcap_keyfield_set lan966x_tc_get_temp_keyfield_set(struct lan966x_port *port)
{
	struct lan966x_tc_flower_template *tmpl;

	if (list_empty(&port->tc.templates))
		return VCAP_KFS_NO_VALUE;

	tmpl = list_first_entry(&port->tc.templates,
				struct lan966x_tc_flower_template,
				list);
	return tmpl->keyset;
}

static int lan966x_tc_use_dissectors(struct flow_cls_offload *fco,
				     struct lan966x_port *port,
				     struct vcap_admin *admin,
				     struct vcap_rule *vrule,
				     u16 *l3)
{
	struct lan966x_tc_flower_parse_usage state = {
		.fco = fco,
		.admin = admin,
		.vrule = vrule,
		.l3_proto = ETH_P_ALL,
		.port = port,
	};
	int idx, err = 0;

	switch (lan966x_tc_get_temp_keyfield_set(port)) {
	case VCAP_KFS_5TUPLE_IP6:
	case VCAP_KFS_NORMAL_IP6:
	case VCAP_KFS_NORMAL_IP6_DMAC:
		lan966x_tc_flower_usage_handlers[FLOW_DISSECTOR_KEY_BASIC] = lan966x_tc_flower_handler_basic_usage_normal_ipv6;
		break;
	default:
		lan966x_tc_flower_usage_handlers[FLOW_DISSECTOR_KEY_BASIC] = lan966x_tc_flower_handler_basic_usage;
		break;
	}

	state.frule = flow_cls_offload_flow_rule(fco);
	for (idx = 0; idx < ARRAY_SIZE(lan966x_tc_flower_usage_handlers); ++idx) {
		if (flow_rule_match_key(state.frule, idx)) {
			if (lan966x_tc_flower_usage_handlers[idx]) {
				err = lan966x_tc_flower_usage_handlers[idx](&state);
				if (err)
					return err;
			}
		}
	}
	pr_debug("%s:%d: used_keys: %#x - %#x\n", __func__, __LINE__,
		 state.frule->match.dissector->used_keys, state.used_keys);
	if (state.frule->match.dissector->used_keys ^ state.used_keys) {
		pr_err("%s:%d: unused dissectors: 0x%x\n", __func__, __LINE__,
		       state.frule->match.dissector->used_keys ^ state.used_keys);
		NL_SET_ERR_MSG_MOD(fco->common.extack,
				   "Unsupported match item");
		return -ENOENT;
	}
	*l3 = state.l3_proto;
	return err;
}

/* Collect all port keysets and apply the first of them, possibly wildcarded */
static int lan966x_tc_select_protocol_keyset(struct lan966x_port *port,
					     struct vcap_rule *vrule,
					     struct vcap_admin *admin,
					     u16 l3_proto,
					     struct lan966x_multiple_rules *multi)
{
	struct vcap_keyset_list portkeysetlist = {0};
	enum vcap_keyfield_set portkeysets[10] = {0};
	struct vcap_keyset_match match = {0};
	enum vcap_keyfield_set keysets[10];
	const struct vcap_set *kinfo;
	enum vcap_key_field keys[10];
	int idx, jdx, err, count = 0;

	/* ES0 has only one keyset, so no keyset wildcarding */
	if (admin->vtype == VCAP_TYPE_ES0)
		return 0;

	match.matches.keysets = keysets;
	match.matches.max = ARRAY_SIZE(keysets);
	match.unmatched_keys.keys = keys;
	match.unmatched_keys.max = ARRAY_SIZE(keys);
	if (vcap_rule_find_keysets(vrule, &match) == 0)
		return -EINVAL;
	portkeysetlist.max = ARRAY_SIZE(portkeysets);
	portkeysetlist.keysets = portkeysets;
	err = lan966x_vcap_get_port_keyset(port->dev,
					   admin, vrule->vcap_chain_id,
					   l3_proto,
					   &portkeysetlist);
	if (err)
		return err;
	for (idx = 0; idx < portkeysetlist.cnt; ++idx) {
		kinfo = vcap_keyfieldset(admin->vtype, portkeysetlist.keysets[idx]);
		if (kinfo == NULL) {
			pr_debug("%s:%d: no keyset info: portkeyset[%d] = %s\n",
				 __func__, __LINE__,
				 idx,
				 lan966x_vcap_keyset_name(port->dev, portkeysetlist.keysets[idx]));
			continue;
		}
		pr_debug("%s:%d: found: portkeyset[%d] = %s, X%d, type_id: %d\n",
			 __func__, __LINE__,
			 idx,
			 lan966x_vcap_keyset_name(port->dev, portkeysetlist.keysets[idx]),
			 kinfo->sw_per_item,
			 kinfo->type_id);
		/* Find a port keyset that matches the required keys
		 * If there are multiple keysets then compose a type id mask
		 */
		for (jdx = 0; jdx < match.matches.cnt; ++jdx) {
			if (portkeysetlist.keysets[idx] == match.matches.keysets[jdx]) {
				if (!multi->rule[kinfo->sw_per_item].selected) {
					multi->rule[kinfo->sw_per_item].selected = true;
					multi->rule[kinfo->sw_per_item].keyset = portkeysetlist.keysets[idx];
					multi->rule[kinfo->sw_per_item].value = kinfo->type_id;
				}
				multi->rule[kinfo->sw_per_item].value &= kinfo->type_id;
				multi->rule[kinfo->sw_per_item].mask |= kinfo->type_id;
				++count;
			}
		}
	}
	if (count == 0) {
		pr_debug("%s:%d: no portkeysets had the requested keys\n",
			 __func__, __LINE__);
		return -ENOENT;
	}
	for (idx = 0; idx < LAN966X_MAX_RULE_SIZE; ++idx) {
		if (!multi->rule[idx].selected)
			continue;
		/* Align the mask to the combined value */
		multi->rule[idx].mask ^= multi->rule[idx].value;
	}
	for (idx = 0; idx < LAN966X_MAX_RULE_SIZE; ++idx) {
		if (!multi->rule[idx].selected)
			continue;
		vcap_set_rule_set_keyset(vrule, multi->rule[idx].keyset);
		pr_debug("%s:%d: selected: X%d, keyset: %s\n",
			 __func__, __LINE__,
			 idx,
			 lan966x_vcap_keyset_name(port->dev, multi->rule[idx].keyset));
		if (count > 1) {
			err = vcap_rule_mod_key_u32(vrule, VCAP_KF_TYPE,
						    multi->rule[idx].value, ~multi->rule[idx].mask);
			pr_debug("%s:%d: modified: X%d, keyset: %s, value: %#x, mask: %#x\n",
				 __func__, __LINE__,
				 idx,
				 lan966x_vcap_keyset_name(port->dev, multi->rule[idx].keyset),
				 multi->rule[idx].value,
				 ~multi->rule[idx].mask);
		}
		multi->rule[idx].selected = false; /* mark as done */
		break; /* Stop here and add more rules later */
	}
	return err;
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

static int lan966x_tc_add_rule_link(struct vcap_admin *admin,
				   struct vcap_rule *vrule,
				   int from_cid, int to_cid)
{
	struct vcap_admin *to_admin = vcap_find_admin(to_cid);
	int diff = to_cid - from_cid;
	int err = 0;

	if (to_admin && diff > 0) {
		diff %= VCAP_CID_LOOKUP_SIZE;
		pr_debug("%s:%d: from: %d, to: %d, diff %d\n",
			 __func__, __LINE__, from_cid, to_cid, diff);
		/* Between IS1 and IS2 the PAG value is used */
		/* Between IS1 and ES0 the ISDX value is used */
		if (admin->vtype == VCAP_TYPE_IS1 && to_admin->vtype == VCAP_TYPE_IS2) {
			/* This works for IS1->IS2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_VAL, diff);
			if (err)
				goto out;
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_OVERRIDE_MASK, 0xff);
			if (err)
				goto out;
		} else if (admin->vtype == VCAP_TYPE_IS1 && to_admin->vtype == VCAP_TYPE_ES0) {
			/* This works for IS1->ES0 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_ISDX_ADD_VAL, diff);
			if (err)
				goto out;
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_ISDX_REPLACE_ENA, VCAP_BIT_1);
			if (err)
				goto out;
		} else {
			pr_err("%s:%d: unsupported chain destination: %d\n",
			       __func__, __LINE__, to_cid);
			err = -EOPNOTSUPP;
		}
	} else {
		pr_err("%s:%d: unsupported chain direction: %d\n",
		       __func__, __LINE__, to_cid);
		err = -EINVAL;
	}
out:
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
		err = vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_GEN_IDX_SEL, 1, ~0);
		if (err)
			return err;
		return vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_GEN_IDX, link_val, ~0);
	case VCAP_TYPE_IS2:
		/* Add IS2 specific PAG key (for chaining rules from IS1) */
		return vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_PAG, link_val, ~0);
	case VCAP_TYPE_ES0:
		/* Add ES0 specific ISDX key (for chaining rules from IS1) */
		return vcap_rule_add_key_u32(vrule, VCAP_KF_ISDX_CLS, link_val, ~0);
	default:
		break;
	}
	return 0;
}

static int lan966x_tc_add_rule_counter(struct vcap_admin *admin,
				      struct vcap_rule *vrule)
{
	int err = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_ESDX, vrule->id);
		break;
	default:
		break;
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

static void lan966x_tc_flower_use_template(struct net_device *ndev,
					  struct flow_cls_offload *fco,
					  struct vcap_rule *vrule)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x_tc_flower_template *ftmp;
	int idx = 0;

	list_for_each_entry(ftmp, &port->tc.templates, list) {
		if (fco->common.chain_index == ftmp->vcap_chain_id) {
			pr_debug("%s:%d: [%02d]: chain: %d, keyset: %s \n",
				 __func__, __LINE__,
				 idx, ftmp->vcap_chain_id,
				 lan966x_vcap_keyset_name(ndev, ftmp->keyset));
			vcap_set_rule_set_keyset(vrule, ftmp->keyset);
			break;
		}
		++idx;
	}
}

/* Use the ethertype to choose a keyset from the port configuration */
static int lan966x_tc_flower_port_keyset(struct net_device *ndev,
					 struct vcap_admin *admin,
					 struct vcap_rule *vrule,
					 u16 l3_proto)
{
	struct vcap_keyset_list portkeysetlist = {0};
	enum vcap_keyfield_set portkeysets[12] = {0};
	int err;

	if (lan966x_tc_is_known_etype(l3_proto)) {
		portkeysetlist.max = ARRAY_SIZE(portkeysets);
		portkeysetlist.keysets = portkeysets;
		err = lan966x_vcap_get_port_keyset(ndev, admin,
						  vrule->vcap_chain_id,
						  l3_proto,
						  &portkeysetlist);
		if (err)
			return err;
		/* Set the port keyset */
		if (portkeysetlist.cnt == 1)
			vcap_set_rule_set_keyset(vrule, portkeysetlist.keysets[0]);
	}
	return 0;
}

static int lan966x_tc_flower_reduce_rule(struct net_device *ndev,
					struct vcap_rule *vrule)
{
	struct vcap_keyset_match match = {0};
	enum vcap_keyfield_set keysets[10];
	enum vcap_key_field keys[10];
	int idx, err = -EINVAL;

	match.matches.keysets = keysets;
	match.matches.max = ARRAY_SIZE(keysets);
	match.unmatched_keys.keys = keys;
	match.unmatched_keys.max = ARRAY_SIZE(keys);
	if (vcap_rule_find_keysets(vrule, &match))
		return -EINVAL;
	/* Get the missing keys and reduce the rule if possible */
	switch (match.best_match) {
	case VCAP_KFS_IP4_TCP_UDP:
		/* TCP_UDP key is not needed in this keyset */
		if (match.unmatched_keys.cnt == 0)
			break;
		for (idx = 0; idx < match.unmatched_keys.cnt; ++idx)
			if (match.unmatched_keys.keys[idx] == VCAP_KF_TCP_UDP_IS) {
				pr_debug("%s:%d: remove key: %s\n",
					 __func__, __LINE__,
					 lan966x_vcap_key_name(ndev,
							      match.unmatched_keys.keys[idx]));
				vcap_rule_rem_key(vrule, VCAP_KF_TCP_UDP_IS);
				err = 0;
			}
		if (err == 0) {
			vcap_set_rule_set_keyset(vrule, match.best_match);
			err = vcap_val_rule(vrule, ETH_P_ALL);
		}
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
	admin = vcap_rule_get_admin(vrule);
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
	admin = vcap_rule_get_admin(vrule);
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

/**
 * lan966x_tc_flower_replace - Replace (actually add) a flower rule
 * @port: The interface
 * @fco: Rule info
 * @admin: VCAP instance
 *
 * Note that TC never modifies a rule if user uses "tc filter change" or
 * tc filter replace". The updated rule is always added first with a new cookie
 * and then the existing rule is deleted.
 *
 * TC will not call us if the rule does not match the template.
 *
 * When using shared blocks, TC will call us multiple times with the same rule
 * on multiple ports.
 * When IS1 and IS2 are used with shared blocks, a single VCAP rule is used and
 * IGR_PORT_MASK is updated when ports are added and deleted.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if invalid parameters
 * -EEXIST if rule already exists
 * -ENOSPC if there is no more space in VCAP
 * -ENOMEM if cannot allocate memory for rule
 */
static int lan966x_tc_flower_replace(struct lan966x_port *port,
				     struct flow_cls_offload *fco,
				     struct vcap_admin *admin)
{
	struct lan966x_multiple_rules multi = {0};
	struct lan966x_tc_policer pol = {0};
	struct net_device *ndev = port->dev;
	struct lan966x_psfp_sg_cfg sg = {0};
	struct lan966x_psfp_sf_cfg sf = {0};
	struct flow_action_entry *act;
	struct flow_rule *frule;
	struct vcap_rule *vrule;
	u32 ports = 0;
	u16 l3_proto;
	int err, idx;
	u32 polidx;
	u32 sfi_ix;
	u32 sgi_ix;

	vrule = vcap_alloc_rule(ndev, fco->common.chain_index, VCAP_USER_TC,
				fco->common.prio, 0);
	if (IS_ERR(vrule)) {
		pr_err("%s:%d: could not allocate rule: %u\n", __func__, __LINE__, vrule->id);
		return PTR_ERR(vrule);
	}
	vrule->cookie = fco->cookie;
	frule = flow_cls_offload_flow_rule(fco);
	err = lan966x_tc_use_dissectors(fco, port, admin, vrule, &l3_proto);
	if (err)
		goto out;
	lan966x_tc_flower_use_template(ndev, fco, vrule);
	err = lan966x_tc_add_rule_link_target(admin, vrule, fco->common.chain_index);
	if (err)
		goto out;
	err = lan966x_tc_add_rule_counter(admin, vrule);
	if (err)
		goto out;
	if (!flow_action_has_entries(&frule->action)) {
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No actions");
		err = -EINVAL;
		goto out;
	}
	if (!flow_action_basic_hw_stats_check(&frule->action, fco->common.extack)) {
		err = -EOPNOTSUPP;
		goto out;
	}
	flow_action_for_each(idx, act, &frule->action) {
		switch (act->id) {
		case FLOW_ACTION_TRAP:
			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Trap action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			/* VCAP_AF_CPU_COPY_ENA: W1, lan966x: is2 */
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_COPY_ENA, VCAP_BIT_1);
			if (err)
				goto out;
			/* VCAP_AF_CPU_QUEUE_NUM: W3, lan966x: is2/es2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_CPU_QUEUE_NUM, 0);
			if (err)
				goto out;
			/* VCAP_AF_MASK_MODE: lan966x is2 W2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, LAN966X_PMM_REPLACE);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_DROP:
			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Drop action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			/* VCAP_AF_MASK_MODE: lan966x is2 W2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, LAN966X_PMM_REPLACE);
			if (err)
				goto out;
			/* VCAP_AF_POLICE_ENA: lan966x s1 W1, lan966x s2 W1 */
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_POLICE_ENA, VCAP_BIT_1);
			if (err)
				goto out;
			/* VCAP_AF_POLICE_IDX: (lan966x s1 W9), (lan966x s2 W9) */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_POLICE_IDX, LAN966X_POL_IX_DISCARD);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_MIRRED:
			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Mirror action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			err = lan966x_mirror_vcap_add(port,
						      netdev_priv(act->dev));
			if (err) {
				switch (err) {
				case -EBUSY:
					NL_SET_ERR_MSG_MOD(fco->common.extack,
							   "Cannot change the mirror monitor port while in use");
					break;
				case -EINVAL:
					NL_SET_ERR_MSG_MOD(fco->common.extack,
							   "Cannot mirror the mirror monitor port");
					break;
				default:
					NL_SET_ERR_MSG_MOD(fco->common.extack,
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
				NL_SET_ERR_MSG_MOD(fco->common.extack,
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
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Police action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			if (lan966x_vcap_cid_to_lookup(admin, fco->common.chain_index) != 0) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Police action is only supported in first IS2 lookup");
				err = -EOPNOTSUPP;
				goto out;
			}
			err = lan966x_tc_flower_reserve_policer(port,
								fco,
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
				NL_SET_ERR_MSG_MOD(fco->common.extack,
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
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot set vlan mangle");
				goto out;
			}

			break;
		case FLOW_ACTION_VLAN_POP:
			if (admin->vtype != VCAP_TYPE_ES0) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot use vlan pop on non es0");
				err = -EOPNOTSUPP;
				goto out;
			}

			err = vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_OUTER_TAG, 3); /* Force untag */
			if (err) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot push tag");
				err = -EOPNOTSUPP;
				goto out;
			}

			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (admin->vtype != VCAP_TYPE_ES0) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
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
				NL_SET_ERR_MSG_MOD(fco->common.extack,
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
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot set vlan push");

				err = -EINVAL;
				goto out;
			}
			break;
		case FLOW_ACTION_PRIORITY:
			if (act->priority > 7) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Invalid skbedit priority");
				err = -EINVAL;
				goto out;
			}

			err = vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, act->priority);
			if (err) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot set skkedit priority");
				err = -EINVAL;
				goto out;
			}

			break;
		case FLOW_ACTION_GATE:
			if (admin->vtype != VCAP_TYPE_IS1) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot use gate on non is1");
				err = -EOPNOTSUPP;
				goto out;
			}

			if (act->hw_index == U32_MAX) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot use reserved stream gate");
				return -EINVAL;
			}
			if ((act->gate.prio < -1) ||
			    (act->gate.prio > LAN966X_PSFP_SG_MAX_IPV)) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Invalid initial priority");
				return -EINVAL;
			}
			if ((act->gate.cycletime < LAN966X_PSFP_SG_MIN_CYCLE_TIME_NS) ||
			    (act->gate.cycletime > LAN966X_PSFP_SG_MAX_CYCLE_TIME_NS)) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Invalid cycle time");
				return -EINVAL;
			}
			if (act->gate.cycletimeext > LAN966X_PSFP_SG_MAX_CYCLE_TIME_NS) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Invalid cycle time ext");
				return -EINVAL;
			}
			if (act->gate.num_entries >= LAN966X_PSFP_NUM_GCE) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
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
					NL_SET_ERR_MSG_MOD(fco->common.extack,
							   "Invalid interval");
					err = -EINVAL;
					goto out;
				}
				if ((act->gate.entries[i].ipv < -1) ||
				    (act->gate.entries[i].ipv > LAN966X_PSFP_SG_MAX_IPV)) {
					NL_SET_ERR_MSG_MOD(fco->common.extack,
							   "Invalid internal priority");
					err = -EINVAL;
					goto out;
				}
				if (act->gate.entries[i].maxoctets < -1) {
					NL_SET_ERR_MSG_MOD(fco->common.extack,
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
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot reserve stream filter");
				goto out;
			}

			err = lan966x_sgi_ix_reserve(port->lan966x,
						     LAN966X_RES_POOL_USER_IS1,
						     act->hw_index,
						     &sgi_ix);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot reserve stream gate");
				goto out;
			}

			err = lan966x_psfp_sg_set(port->lan966x, sgi_ix, &sg);
			if (err) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot set stream gate");
				goto out;
			}

			err = lan966x_psfp_sf_set(port->lan966x, sfi_ix, &sf);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot set stream filter");
				goto out;
			}

			err = vcap_rule_add_action_bit(vrule, VCAP_AF_SGID_ENA, VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_SGID_VAL, sgi_ix);
			err |= vcap_rule_add_action_bit(vrule, VCAP_AF_SFID_ENA, VCAP_BIT_1);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_SFID_VAL, sfi_ix);
			if (err) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Cannot set sgid and sfid");

				err = -EINVAL;
				goto out;
			}

			break;
		case FLOW_ACTION_ACCEPT:
			lan966x_tc_set_default_actionset(admin, vrule,
							 fco->common.chain_index);
			break;
		case FLOW_ACTION_GOTO:
			lan966x_tc_add_rule_link(admin, vrule,
						fco->common.chain_index, act->chain_index);
			break;
		default:
			NL_SET_ERR_MSG_MOD(fco->common.extack,
					   "Unsupported TC action");
			err = -EOPNOTSUPP;
			goto out;
		}
	}
	err = lan966x_tc_select_protocol_keyset(port, vrule, admin, l3_proto, &multi);
	if (err) {
		pr_err("%s:%d: Could not find usable keyset: %u\n", __func__, __LINE__, vrule->id);
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No matching port keyset for filter protocol and keys");
		goto out;
	}
	err = vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		err = lan966x_tc_flower_port_keyset(ndev, admin, vrule, l3_proto);
		if (err) {
			pr_err("%s:%d: Could not find port keyset: %u\n", __func__, __LINE__, vrule->id);
			NL_SET_ERR_MSG_MOD(fco->common.extack, "Could not validate the filter");
			goto out;
		}
		err = lan966x_tc_flower_reduce_rule(ndev, vrule);
		if (err) {
			pr_err("%s:%d: Could not validate rule: %u\n", __func__, __LINE__, vrule->id);
			lan966x_tc_flower_set_exterr(ndev, fco, vrule);
			goto out;
		}
	}
	pr_debug("%s:%d: chain: %d, keyset: %s \n",
		 __func__, __LINE__,
		 fco->common.chain_index,
		 lan966x_vcap_keyset_name(ndev, vrule->keyset));
	err = vcap_add_rule(vrule);
	if (err) {
		pr_err("%s:%d: Could not add rule: %u\n", __func__, __LINE__, vrule->id);
		NL_SET_ERR_MSG_MOD(fco->common.extack, "Could not add the filter");
		goto out;
	}
	pr_debug("%s:%d: created rule: %u\n", __func__, __LINE__, vrule->id);
	if (l3_proto == ETH_P_ALL)
		err = lan966x_tc_add_remaining_rules(port, fco, vrule, admin, &multi);
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

	vrule = vcap_get_rule(ndev, rule_id);
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

/**
 * lan966x_tc_flower_destroy - Destroy (delete) a flower rule
 * @port: The interface
 * @f: Rule info
 * @ci: Chain info
 *
 * If port is part of a shared block:
 *
 * we must get the rule and remove us from the
 *
 *
 * Returns:
 * 0 if ok
 * -EINVAL if invalid parameters
 * -ENOENT if rule not found
 */
static int lan966x_tc_flower_destroy(struct lan966x_port *port,
				     struct flow_cls_offload *fco,
				     struct vcap_admin *admin)
{
	struct net_device *ndev = port->dev;
	int err = -ENOENT, rule_id;
	int count = 0;

	while (true) {
		rule_id = vcap_lookup_rule_by_cookie(fco->cookie);
		if (rule_id > 0) {
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
			err = vcap_del_rule(ndev, rule_id);
			if (err) {
				pr_err("%s:%d: could not delete rule %d\n",
				       __func__, __LINE__, rule_id);
				break;
			}
		} else {
			break;
		}
		++count;
	}
	return err;
}

/* Collect packet counts from all rules with the same cookie */
static int lan966x_tc_rule_counter_cb(void *arg, struct vcap_rule *rule)
{
	struct lan966x_tc_rule_pkt_cnt *rinfo = arg;
	struct vcap_counter counter;
	int err = 0;

	if (rule->cookie == rinfo->cookie) {
		err = vcap_rule_get_counter(rule->id, &counter);
		if (err)
			return err;
		rinfo->pkts += counter.value;
		counter.value = 0;
		vcap_rule_set_counter(rule->id, &counter);
	}
	return err;
}

/**
 * lan966x_tc_flower_stats - Get packet statistics for a rule
 * @port: The interface
 * @f: Rule info
 * @ci: Chain info
 *
 * Returns:
 * 0 if ok
 * -ENOENT if rule does not exists
 */
static int lan966x_tc_flower_stats(struct lan966x_port *port,
				   struct flow_cls_offload *fco,
				   struct vcap_admin *admin)
{
	struct lan966x_tc_rule_pkt_cnt rinfo = {0};
	int err = -ENOENT;
	ulong lastused = 0;
	u64 drops = 0;
	u32 pkts = 0;

	/* TODO: Calculate drops from stream filter counters */
	rinfo.cookie = fco->cookie;
	err = vcap_rule_iter(lan966x_tc_rule_counter_cb, &rinfo);
	if (err)
		return err;
	pkts = rinfo.pkts;
	flow_stats_update(&fco->stats, 0x0, pkts, drops, lastused,
			  FLOW_ACTION_HW_STATS_IMMEDIATE);
	return err;
}

enum vcap_keyfield_set lan966x_all_keysets[] = {
	VCAP_KFS_MAC_ETYPE,
};

enum vcap_keyfield_set lan966x_ipv4_keysets[] = {
	VCAP_KFS_IP4_TCP_UDP,
	VCAP_KFS_IP4_OTHER,
};

enum vcap_keyfield_set lan966x_ipv6_keysets[] = {
	VCAP_KFS_DMAC_VID,
	VCAP_KFS_NORMAL_IP6,
	VCAP_KFS_NORMAL_IP6_DMAC,
	VCAP_KFS_5TUPLE_IP6,
	VCAP_KFS_7TUPLE,
	VCAP_KFS_NORMAL_7TUPLE,
	VCAP_KFS_IP6_STD,
};

enum vcap_keyfield_set lan966x_arp_keysets[] = {
	VCAP_KFS_ARP,
};

enum vcap_keyfield_set lan966x_8021q_keysets[] = {
	VCAP_KFS_7TUPLE,
	VCAP_KFS_MAC_ETYPE,
};

enum vcap_keyfield_set lan966x_8021ad_keysets[] = {
	VCAP_KFS_7TUPLE,
	VCAP_KFS_MAC_ETYPE,
};

enum vcap_keyfield_set lan966x_snap_keysets[] = {
	VCAP_KFS_NORMAL,
	VCAP_KFS_NORMAL_DMAC,
	VCAP_KFS_7TUPLE,
};

/* Return the index of the best matching keyset according to L3 protocol */
static int lan966x_tc_flower_select_keyset(struct vcap_keyset_match *match,
					   u16 l3_proto)
{
	int idx, jdx, max = 0;
	enum vcap_keyfield_set *keysets;

	switch (l3_proto) {
	case ETH_P_ALL:
		keysets = lan966x_all_keysets;
		max = ARRAY_SIZE(lan966x_all_keysets);
		break;
	case ETH_P_IP:
		keysets = lan966x_ipv4_keysets;
		max = ARRAY_SIZE(lan966x_ipv4_keysets);
		break;
	case ETH_P_IPV6:
		keysets = lan966x_ipv6_keysets;
		max = ARRAY_SIZE(lan966x_ipv6_keysets);
		break;
	case ETH_P_ARP:
		keysets = lan966x_arp_keysets;
		max = ARRAY_SIZE(lan966x_arp_keysets);
		break;
	case ETH_P_8021Q:
		keysets = lan966x_8021q_keysets;
		max = ARRAY_SIZE(lan966x_8021q_keysets);
		break;
	case ETH_P_8021AD:
		keysets = lan966x_8021ad_keysets;
		max = ARRAY_SIZE(lan966x_8021ad_keysets);
		break;
	case ETH_P_SNAP:
		keysets = lan966x_snap_keysets;
		max = ARRAY_SIZE(lan966x_snap_keysets);
		break;
	}
	for (idx = 0; idx < max; ++idx) /* highest priority */
		for (jdx = 0; jdx < match->matches.cnt; ++jdx)
			if (keysets[idx] == match->matches.keysets[jdx])
				return jdx;
	return 0;
}

/**
 * lan966x_tc_flower_template_create - Create a template for a chain
 * @port: The interface
 * @f: Rule info
 * @ci: Chain info
 *
 * Returns:
 * 0 if ok
 * -EEXIST if template already exists
 * -EINVAL if invalid parameters
 */
static int lan966x_tc_flower_template_create(struct lan966x_port *port,
					    struct flow_cls_offload *fco,
					    struct vcap_admin *admin)
{
	enum vcap_key_field unmatched_keys[LAN966X_VCAP_KEYS_MAX];
	struct vcap_keyset_list portkeysetlist = {0};
	enum vcap_keyfield_set portkeysets[12] = {0};
	struct lan966x_tc_flower_template *ftmp;
	struct vcap_keyset_match match = {0};
	struct net_device *ndev = port->dev;
	enum vcap_keyfield_set keysets[10];
	int err = -ENOENT;
	u16 l3_proto;
	u8 l4_proto;
	int count;
	int idx;

	count = vcap_admin_rule_count(admin, fco->common.chain_index);
	if (count > 0) {
		pr_err("%s:%d: Cannot create template when rules are present\n",
		       __func__, __LINE__);
		return -EBUSY;
	}
	ftmp = kzalloc(sizeof(*ftmp), GFP_KERNEL);
	if (!ftmp)
		return -ENOMEM;
	ftmp->vcap_chain_id = fco->common.chain_index;
	ftmp->original = VCAP_KFS_NO_VALUE;
	ftmp->keyset = VCAP_KFS_NO_VALUE;
	/* Verify the template, and possibly change the port keyset config */
	ftmp->keylist.keys = ftmp->vkeys;
	ftmp->keylist.max = LAN966X_VCAP_KEYS_MAX;
	match.matches.keysets = keysets;
	match.matches.max = ARRAY_SIZE(keysets);
	match.unmatched_keys.keys = unmatched_keys;
	match.unmatched_keys.max = ARRAY_SIZE(unmatched_keys);
	lan966x_tc_match_dissectors(fco, admin, &ftmp->keylist, &l3_proto, &l4_proto);
	ftmp->l3_proto = l3_proto;
	ftmp->l4_proto = l4_proto;
	/* Check if a keyset that fits exists */
	if (vcap_rule_match_keysets(admin->vtype, &ftmp->keylist, &match)) {
		idx = lan966x_tc_flower_select_keyset(&match, l3_proto);
		ftmp->keyset = match.matches.keysets[idx];
		pr_debug("%s:%d: chosen via L3 proto: %s\n", __func__, __LINE__,
			 lan966x_vcap_keyset_name(ndev, match.matches.keysets[idx]));
	} else {
		ftmp->keyset = match.best_match;
		pr_debug("%s:%d: best match: %s missing: %d\n", __func__, __LINE__,
			 lan966x_vcap_keyset_name(ndev, match.best_match),
			 match.unmatched_keys.cnt);
	}
	portkeysetlist.max = ARRAY_SIZE(portkeysets);
	portkeysetlist.keysets = portkeysets;
	/* Update the port configuration if needed */
	err = lan966x_vcap_get_port_keyset(ndev, admin, fco->common.chain_index,
					  l3_proto,
					  &portkeysetlist);
	/* Pick the first keyset from the port config */
	if (err == 0 && portkeysetlist.cnt > 0) {
		ftmp->original = portkeysets[0];
		if (ftmp->original != ftmp->keyset)
			lan966x_vcap_set_port_keyset(ndev, admin,
						    fco->common.chain_index,
						    l3_proto, l4_proto,
						    ftmp->keyset);
	} else {
		pr_err("%s:%d: Could not get port keyset\n", __func__, __LINE__);
		ftmp->original = ftmp->keyset;
	}

	/* Store new template */
	list_add_tail(&ftmp->list, &port->tc.templates);
	return err;
}

/**
 * lan966x_tc_flower_tmplt_destroy - Destroy a template for a chain
 * @port: The interface
 * @f: Rule info
 * @ci: Chain info
 *
 * Refuses to destroy the template if rules are present in the chain
 *
 * Returns:
 * 0 if ok
 * -ENOENT if template does not exists
 * -EBUSY if rules are present
 */
static int lan966x_tc_flower_template_destroy(struct lan966x_port *port,
					     struct flow_cls_offload *fco,
					     struct vcap_admin *admin)
{
	struct lan966x_tc_flower_template *ftmp, *tmp;
	struct net_device *ndev = port->dev;
	int err = -ENOENT;

	/* The TC framework automatically removes the rules using the template */
	list_for_each_entry_safe(ftmp, tmp, &port->tc.templates, list) {
		if (ftmp->vcap_chain_id == fco->common.chain_index) {
			/* Restore port config */
			if (ftmp->original != ftmp->keyset)
				lan966x_vcap_set_port_keyset(ndev, admin,
							    fco->common.chain_index,
							    ftmp->l3_proto,
							    ftmp->l4_proto,
							    ftmp->original);
			list_del(&ftmp->list);
			kfree(ftmp);
			break;
		}
	}
	return err;
}

int lan966x_tc_flower(struct lan966x_port *port,
		      struct flow_cls_offload *fco,
		      bool ingress)
{
	struct vcap_admin *admin;
	struct flow_rule *frule;
	int err = -EINVAL;

	frule = flow_cls_offload_flow_rule(fco);
	/* Get vcap info */
	admin = vcap_find_admin(fco->common.chain_index);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(fco->common.extack, "Invalid chain");
		return err;
	}
	switch (fco->command) {
	case FLOW_CLS_REPLACE:
		return lan966x_tc_flower_replace(port, fco, admin);
	case FLOW_CLS_DESTROY:
		return lan966x_tc_flower_destroy(port, fco, admin);
	case FLOW_CLS_STATS:
		return lan966x_tc_flower_stats(port, fco, admin);
	case FLOW_CLS_TMPLT_CREATE:
		return lan966x_tc_flower_template_create(port, fco, admin);
	case FLOW_CLS_TMPLT_DESTROY:
		return lan966x_tc_flower_template_destroy(port, fco, admin);
	default:
		return -EOPNOTSUPP;
	}
}
