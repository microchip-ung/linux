/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip VCAP API
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#include <net/tcp.h>
#include "sparx5_tc.h"
#include "sparx5_tc_dbg.h"
#include "vcap_api_client.h"
#include "sparx5_vcap_impl.h"
#include "sparx5_main_regs.h"
#include "sparx5_main.h"

#define SPX5_VCAP_KEYS_MAX 50
#define SPX5_MAX_RULE_SIZE 13 /* allows X1, X2, X4, X6 and X12 rules */

/* Collect keysets and type ids for multiple rules per size */
struct sparx5_wildcard_rule {
	bool selected;
	uint8_t value;
	uint8_t mask;
	enum vcap_keyfield_set keyset;
};
struct sparx5_multiple_rules {
	struct sparx5_wildcard_rule rule[SPX5_MAX_RULE_SIZE];
};

struct sparx5_tc_flower_parse_keylist {
	struct flow_cls_offload *fco;
	struct flow_rule *frule;
	struct vcap_admin *admin;
	struct vcap_key_list *keylist;
	u16 l3_proto;
	u8 l4_proto;
};

struct sparx5_tc_flower_parse_usage {
	struct flow_cls_offload *fco;
	struct flow_rule *frule;
	struct vcap_admin *admin;
	struct vcap_rule *vrule;
	u16 l3_proto;
	u8 l4_proto;
	unsigned int used_keys;
};

struct sparx5_tc_flower_template {
	struct list_head list; /* for insertion in the list of templates */
	int vcap_chain_id; /* used by tc */
	struct vcap_key_list keylist; /* keys used by the template */
	enum vcap_key_field vkeys[SPX5_VCAP_KEYS_MAX];
	enum vcap_keyfield_set original; /* port keyset used before the template */
	enum vcap_keyfield_set keyset; /* template derived keyset */
	u16 l3_proto; /* ethertype for keyset */
	u8 l4_proto; /* ip protocol for keyset */
};

static u16 sparx5_tc_known_etypes[] = {
	ETH_P_ALL,
	ETH_P_IP,
	ETH_P_ARP,
	ETH_P_IPV6,
};

struct sparx5_tc_rule_pkt_cnt {
	u64 cookie;
	u32 pkts;
};

static bool sparx5_tc_is_known_etype(u16 etype)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(sparx5_tc_known_etypes); ++idx)
		if (sparx5_tc_known_etypes[idx] == etype)
			return true;
	return false;
}

/* Copy to host byte order */
static void sparx5_netbytes_copy(u8 *dst, u8 *src, int count)
{
	int idx;

	for (idx = 0; idx < count; ++idx, ++dst)
		*dst = src[count - idx - 1];
}

int sparx5_tc_flower_handler_control_keylist(struct sparx5_tc_flower_parse_keylist *st)
{
	struct flow_match_control match;

	flow_rule_match_control(st->frule, &match);
	if (match.mask->flags)
		vcap_key_list_add(st->keylist, VCAP_KF_L3_FRAGMENT_TYPE);
	return 0;
}

int sparx5_tc_flower_handler_control_usage(struct sparx5_tc_flower_parse_usage *st)
{
	struct flow_match_control match;
	u32 value, mask;
	int err = 0;

	flow_rule_match_control(st->frule, &match);
	if (match.mask->flags) {
		if (match.mask->flags & FLOW_DIS_FIRST_FRAG) {
			if (match.key->flags & FLOW_DIS_FIRST_FRAG) {
				value = 1; /* initial fragment */
				mask = 0x3;
			} else {
				if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
					value = 3; /* valid follow up fragment */
					mask = 0x3;
				} else {
					value = 0; /* no fragment */
					mask = 0x3;
				}
			}
		} else {
			if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
				value = 3; /* valid follow up fragment */
				mask = 0x3;
			} else {
				value = 0; /* no fragment */
				mask = 0x3;
			}
		}
		err = vcap_rule_add_key_u32(st->vrule,
					    VCAP_KF_L3_FRAGMENT_TYPE,
					    value, mask);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_CONTROL);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_frag parse error");
	return err;
}

int sparx5_tc_flower_handler_basic_keylist(struct sparx5_tc_flower_parse_keylist *st)
{
	struct flow_match_basic match;

	flow_rule_match_basic(st->frule, &match);
	if (match.mask->n_proto) {
		st->l3_proto = be16_to_cpu(match.key->n_proto);
		if (!sparx5_tc_is_known_etype(st->l3_proto))
			vcap_key_list_add(st->keylist, VCAP_KF_ETYPE);
		else if (st->l3_proto == ETH_P_IP)
			vcap_key_list_add(st->keylist, VCAP_KF_IP4_IS);
		/* else if (st->l3_proto == ETH_P_IPV6) */
		/* 	vcap_key_list_add(st->keylist, VCAP_KF_IP4_IS); */
	}
	if (match.mask->ip_proto) {
		st->l4_proto = match.key->ip_proto;
		if (st->l4_proto == IPPROTO_TCP) {
			vcap_key_list_add(st->keylist, VCAP_KF_TCP_IS);
		} else if (st->l4_proto == IPPROTO_UDP) {
			/* Only in 7tuple keysets */
			/* vcap_key_list_add(st->keylist, VCAP_KF_TCP_UDP_IS); */
			vcap_key_list_add(st->keylist, VCAP_KF_TCP_IS);
		} else {
			vcap_key_list_add(st->keylist, VCAP_KF_L3_IP_PROTO);
		}
	}
	return 0;
}

int sparx5_tc_flower_handler_basic_usage(struct sparx5_tc_flower_parse_usage *st)
{
	struct flow_match_basic match;
	int err = 0;

	flow_rule_match_basic(st->frule, &match);
	if (match.mask->n_proto) {
		st->l3_proto = be16_to_cpu(match.key->n_proto);
		if (!sparx5_tc_is_known_etype(st->l3_proto)) {
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
			err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_IP4_IS,
						    VCAP_BIT_0);
			if (err)
				goto out;
		}
	}
	if (match.mask->ip_proto) {
		st->l4_proto = match.key->ip_proto;
		if (st->l4_proto == IPPROTO_TCP) {
			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_TCP_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l4_proto == IPPROTO_UDP) {
			/* Only in 7tuple keysets */
			/* err = vcap_rule_add_key_bit(st->vrule, */
			/* 			    VCAP_KF_TCP_UDP_IS, */
			/* 			    VCAP_BIT_1); */
			/* if (err) */
			/* 	goto out; */
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

int sparx5_tc_flower_handler_ipv4_keylist(struct sparx5_tc_flower_parse_keylist *st)
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

int sparx5_tc_flower_handler_ipv4_usage(struct sparx5_tc_flower_parse_usage *st)
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

int sparx5_tc_flower_handler_ipv6_keylist(struct sparx5_tc_flower_parse_keylist *st)
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

int sparx5_tc_flower_handler_ipv6_usage(struct sparx5_tc_flower_parse_usage *st)
{
	int err = 0;

	if (st->l3_proto == ETH_P_IPV6) {
		struct flow_match_ipv6_addrs match;
		struct vcap_u128_key sip;
		struct vcap_u128_key dip;

		flow_rule_match_ipv6_addrs(st->frule, &match);
		/* Check if address masks are non-zero */
		if (!ipv6_addr_any(&match.mask->src)) {
			sparx5_netbytes_copy(sip.value, match.key->src.s6_addr, 16);
			sparx5_netbytes_copy(sip.mask, match.mask->src.s6_addr, 16);
			err = vcap_rule_add_key_u128(st->vrule, VCAP_KF_L3_IP6_SIP, &sip);
			if (err)
				goto out;
		}
		if (!ipv6_addr_any(&match.mask->dst)) {
			sparx5_netbytes_copy(dip.value, match.key->dst.s6_addr, 16);
			sparx5_netbytes_copy(dip.mask, match.mask->dst.s6_addr, 16);
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

int sparx5_tc_flower_handler_portnum_keylist(struct sparx5_tc_flower_parse_keylist *st)
{
	struct flow_match_ports match;

	flow_rule_match_ports(st->frule, &match);
	if (match.mask->src)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_SPORT);
	if (match.mask->dst)
		vcap_key_list_add(st->keylist, VCAP_KF_L4_DPORT);
	return 0;
}

int sparx5_tc_flower_handler_portnum_usage(struct sparx5_tc_flower_parse_usage *st)
{
	struct flow_match_ports match;
	u16 value, mask;
	int err = 0;

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
		err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L4_DPORT, value, mask);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_PORTS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "port parse error");
	return err;
}

int sparx5_tc_flower_handler_ethaddr_keylist(struct sparx5_tc_flower_parse_keylist *st)
{
	struct flow_match_eth_addrs match;

	flow_rule_match_eth_addrs(st->frule, &match);
	if (!is_zero_ether_addr(match.mask->src))
		vcap_key_list_add(st->keylist, VCAP_KF_L2_SMAC);
	if (!is_zero_ether_addr(match.mask->dst))
		vcap_key_list_add(st->keylist, VCAP_KF_L2_DMAC);
	return 0;
}

int sparx5_tc_flower_handler_ethaddr_usage(struct sparx5_tc_flower_parse_usage *st)
{
	struct flow_match_eth_addrs match;
	enum vcap_key_field smac_key = VCAP_KF_L2_SMAC;
	enum vcap_key_field dmac_key = VCAP_KF_L2_DMAC;
	struct vcap_u48_key smac, dmac;
	int err = 0;

	flow_rule_match_eth_addrs(st->frule, &match);
	if (!is_zero_ether_addr(match.mask->src)) {
		sparx5_netbytes_copy(smac.value, match.key->src, ETH_ALEN);
		sparx5_netbytes_copy(smac.mask, match.mask->src, ETH_ALEN);
		err = vcap_rule_add_key_u48(st->vrule, smac_key, &smac);
		if (err)
			goto out;
	}
	if (!is_zero_ether_addr(match.mask->dst)) {
		sparx5_netbytes_copy(dmac.value, match.key->dst, ETH_ALEN);
		sparx5_netbytes_copy(dmac.mask, match.mask->dst, ETH_ALEN);
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

int sparx5_tc_flower_handler_arp_keylist(struct sparx5_tc_flower_parse_keylist *st)
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

int sparx5_tc_flower_handler_arp_usage(struct sparx5_tc_flower_parse_usage *st)
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

int sparx5_tc_flower_handler_vlan_keylist(struct sparx5_tc_flower_parse_keylist *st)
{
	struct flow_match_vlan match;
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID_CLS;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP_CLS;

	flow_rule_match_vlan(st->frule, &match);
	if (st->admin->vtype == VCAP_TYPE_IS0) {
		vid_key = VCAP_KF_8021Q_VID0;
		pcp_key = VCAP_KF_8021Q_PCP0;
	}
	if (st->admin->vtype == VCAP_TYPE_ES0) {
		vid_key = VCAP_KF_8021Q_VID_CLS;
	}
	if (match.mask->vlan_id)
		vcap_key_list_add(st->keylist, vid_key);
	if (match.mask->vlan_priority) {
		if (st->admin->vtype == VCAP_TYPE_ES0)
			return -EINVAL;
		else
			vcap_key_list_add(st->keylist, pcp_key);
	}
	return 0;
}

int sparx5_tc_flower_handler_vlan_usage(struct sparx5_tc_flower_parse_usage *st)
{
	struct flow_match_vlan match;
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID_CLS;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP_CLS;
	int err;

	flow_rule_match_vlan(st->frule, &match);
	if (st->admin->vtype == VCAP_TYPE_IS0) {
		vid_key = VCAP_KF_8021Q_VID0;
		pcp_key = VCAP_KF_8021Q_PCP0;
	}
	if (st->admin->vtype == VCAP_TYPE_ES0)
		vid_key = VCAP_KF_8021Q_VID_CLS;
	if (match.mask->vlan_id) {
		err = vcap_rule_add_key_u32(st->vrule, vid_key,
					    match.key->vlan_id,
					    match.mask->vlan_id);
		if (err)
			goto out;
	}
	if (match.mask->vlan_priority) {
		if (st->admin->vtype == VCAP_TYPE_ES0)
			return -EINVAL;
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

int sparx5_tc_flower_handler_tcp_keylist(struct sparx5_tc_flower_parse_keylist *st)
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

int sparx5_tc_flower_handler_tcp_usage(struct sparx5_tc_flower_parse_usage *st)
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

int sparx5_tc_flower_handler_ip_keylist(struct sparx5_tc_flower_parse_keylist *st)
{
	struct flow_match_ip match;

	flow_rule_match_ip(st->frule, &match);

	if (match.mask->tos)
		vcap_key_list_add(st->keylist, VCAP_KF_L3_TOS);
	return 0;
}

int sparx5_tc_flower_handler_ip_usage(struct sparx5_tc_flower_parse_usage *st)
{
	struct flow_match_ip match;
	int err = 0;

	flow_rule_match_ip(st->frule, &match);

	if (match.mask->tos) {
		err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L3_TOS,
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

int sparx5_tc_flower_handler_cvlan_keylist(struct sparx5_tc_flower_parse_keylist *st)
{
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID0;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP0;
	struct flow_match_vlan match;
	u16 tpid;

	if (st->admin->vtype != VCAP_TYPE_IS0)
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

int sparx5_tc_flower_handler_cvlan_usage(struct sparx5_tc_flower_parse_usage *st)

{
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID0;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP0;
	struct flow_match_vlan match;
	u16 tpid;
	int err;

	if (st->admin->vtype != VCAP_TYPE_IS0)
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

int (*sparx5_tc_flower_keylist_handlers[])(struct sparx5_tc_flower_parse_keylist *st) = {
	[FLOW_DISSECTOR_KEY_CONTROL] = sparx5_tc_flower_handler_control_keylist,
	[FLOW_DISSECTOR_KEY_BASIC] = sparx5_tc_flower_handler_basic_keylist,
	[FLOW_DISSECTOR_KEY_IPV4_ADDRS] = sparx5_tc_flower_handler_ipv4_keylist,
	[FLOW_DISSECTOR_KEY_IPV6_ADDRS] = sparx5_tc_flower_handler_ipv6_keylist,
	[FLOW_DISSECTOR_KEY_PORTS] = sparx5_tc_flower_handler_portnum_keylist,
	[FLOW_DISSECTOR_KEY_ETH_ADDRS] = sparx5_tc_flower_handler_ethaddr_keylist,
	[FLOW_DISSECTOR_KEY_ARP] = sparx5_tc_flower_handler_arp_keylist,
	[FLOW_DISSECTOR_KEY_VLAN] = sparx5_tc_flower_handler_vlan_keylist,
	[FLOW_DISSECTOR_KEY_TCP] = sparx5_tc_flower_handler_tcp_keylist,
	[FLOW_DISSECTOR_KEY_IP] = sparx5_tc_flower_handler_ip_keylist,
	[FLOW_DISSECTOR_KEY_CVLAN] = sparx5_tc_flower_handler_cvlan_keylist,
};

int (*sparx5_tc_flower_usage_handlers[])(struct sparx5_tc_flower_parse_usage *st) = {
	[FLOW_DISSECTOR_KEY_CONTROL] = sparx5_tc_flower_handler_control_usage,
	[FLOW_DISSECTOR_KEY_BASIC] = sparx5_tc_flower_handler_basic_usage,
	[FLOW_DISSECTOR_KEY_IPV4_ADDRS] = sparx5_tc_flower_handler_ipv4_usage,
	[FLOW_DISSECTOR_KEY_IPV6_ADDRS] = sparx5_tc_flower_handler_ipv6_usage,
	[FLOW_DISSECTOR_KEY_PORTS] = sparx5_tc_flower_handler_portnum_usage,
	[FLOW_DISSECTOR_KEY_ETH_ADDRS] = sparx5_tc_flower_handler_ethaddr_usage,
	[FLOW_DISSECTOR_KEY_ARP] = sparx5_tc_flower_handler_arp_usage,
	[FLOW_DISSECTOR_KEY_VLAN] = sparx5_tc_flower_handler_vlan_usage,
	[FLOW_DISSECTOR_KEY_TCP] = sparx5_tc_flower_handler_tcp_usage,
	[FLOW_DISSECTOR_KEY_IP] = sparx5_tc_flower_handler_ip_usage,
	[FLOW_DISSECTOR_KEY_CVLAN] = sparx5_tc_flower_handler_cvlan_usage,
};

static int sparx5_tc_match_dissectors(struct flow_cls_offload *fco,
				      struct vcap_admin *admin,
				      struct vcap_key_list *keylist,
				      u16 *l3,
				      u8 *l4)
{
	struct sparx5_tc_flower_parse_keylist state = {
		.fco = fco,
		.admin = admin,
		.keylist = keylist,
		.l3_proto = ETH_P_ALL,
	};
	int idx, err = 0;

	state.frule = flow_cls_offload_flow_rule(fco);
	for (idx = 0; idx < ARRAY_SIZE(sparx5_tc_flower_keylist_handlers); ++idx)
		if (sparx5_tc_flower_keylist_handlers[idx] &&
			flow_rule_match_key(state.frule, idx))
				sparx5_tc_flower_keylist_handlers[idx](&state);
	*l3 = state.l3_proto;
	*l4 = state.l4_proto;
	return err;
}

static int sparx5_tc_use_dissectors(struct flow_cls_offload *fco,
				    struct vcap_admin *admin,
				    struct vcap_rule *vrule,
				    u16 *l3)
{
	struct sparx5_tc_flower_parse_usage state = {
		.fco = fco,
		.admin = admin,
		.vrule = vrule,
		.l3_proto = ETH_P_ALL,
	};
	int idx, err = 0;

	state.frule = flow_cls_offload_flow_rule(fco);
	for (idx = 0; idx < ARRAY_SIZE(sparx5_tc_flower_usage_handlers); ++idx) {
		if (flow_rule_match_key(state.frule, idx)) {
			if (sparx5_tc_flower_usage_handlers[idx]) {
				err = sparx5_tc_flower_usage_handlers[idx](&state);
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
static int sparx5_tc_select_protocol_keyset(struct net_device *ndev,
					    struct vcap_rule *vrule,
					    struct vcap_admin *admin,
					    u16 l3_proto,
					    struct sparx5_multiple_rules *multi)
{
	struct vcap_keyset_list portkeysetlist = {0};
	enum vcap_keyfield_set portkeysets[10] = {0};
	struct vcap_keyset_match match = {0};
	enum vcap_keyfield_set keysets[10];
	const struct vcap_set *kinfo;
	enum vcap_key_field keys[10];
	int idx, jdx, err = 0, count = 0;

	/* ES0 has only one keyset, so no keyset wildcarding */
	if (admin->vtype == VCAP_TYPE_ES0)
		return err;

	match.matches.keysets = keysets;
	match.matches.max = ARRAY_SIZE(keysets);
	match.unmatched_keys.keys = keys;
	match.unmatched_keys.max = ARRAY_SIZE(keys);
	if (vcap_rule_find_keysets(vrule, &match) == 0)
		return -EINVAL;
	portkeysetlist.max = ARRAY_SIZE(portkeysets);
	portkeysetlist.keysets = portkeysets;
	err = sparx5_vcap_get_port_keyset(ndev,
					  admin, vrule->vcap_chain_id,
					  l3_proto,
					  &portkeysetlist);
	if (err)
		return err;
	pr_info("%s:%d: count: %d\n", __func__, __LINE__, portkeysetlist.cnt);
	for (idx = 0; idx < portkeysetlist.cnt; ++idx) {
		kinfo = vcap_keyfieldset(admin->vtype, portkeysetlist.keysets[idx]);
		if (kinfo == NULL) {
			pr_debug("%s:%d: no keyset info: portkeyset[%d] = %s\n",
				 __func__, __LINE__,
				 idx,
				 sparx5_vcap_keyset_name(ndev, portkeysetlist.keysets[idx]));
			continue;
		}
		pr_debug("%s:%d: found: portkeyset[%d] = %s, X%d, type_id: %d\n",
			 __func__, __LINE__,
			 idx,
			 sparx5_vcap_keyset_name(ndev, portkeysetlist.keysets[idx]),
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
		return -EPROTO;
	}
	if (l3_proto == ETH_P_ALL && count < portkeysetlist.cnt) {
		pr_debug("%s:%d: not all portkeysets had the requested keys\n",
			 __func__, __LINE__);
		return -ENOENT;
	}
	for (idx = 0; idx < SPX5_MAX_RULE_SIZE; ++idx) {
		if (!multi->rule[idx].selected)
			continue;
		/* Align the mask to the combined value */
		multi->rule[idx].mask ^= multi->rule[idx].value;
		pr_debug("%s:%d: available: X%d, keyset: %s, value: %#x, mask: %#x\n",
			 __func__, __LINE__,
			 idx,
			 sparx5_vcap_keyset_name(ndev, multi->rule[idx].keyset),
				 multi->rule[idx].value,
				 ~multi->rule[idx].mask);
	}
	for (idx = 0; idx < SPX5_MAX_RULE_SIZE; ++idx) {
		if (!multi->rule[idx].selected)
			continue;
		vcap_set_rule_set_keyset(vrule, multi->rule[idx].keyset);
		pr_debug("%s:%d: selected: X%d, keyset: %s\n",
			 __func__, __LINE__,
			 idx,
			 sparx5_vcap_keyset_name(ndev, multi->rule[idx].keyset));
		if (count > 1) {
			/* Some keysets do not have a type field */
			vcap_rule_mod_key_u32(vrule, VCAP_KF_TYPE,
					      multi->rule[idx].value,
					      ~multi->rule[idx].mask);
			pr_debug("%s:%d: modified: X%d, keyset: %s, value: %#x, mask: %#x\n",
				 __func__, __LINE__,
				 idx,
				 sparx5_vcap_keyset_name(ndev, multi->rule[idx].keyset),
				 multi->rule[idx].value,
				 ~multi->rule[idx].mask);
		}
		multi->rule[idx].selected = false; /* mark as done */
		break; /* Stop here and add more rules later */
	}
	return err;
}

static void sparx5_tc_flower_set_exterr(struct net_device *ndev,
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

static int sparx5_tc_add_rule_copy(struct net_device *ndev,
				   struct flow_cls_offload *fco,
				   struct vcap_rule *erule,
				   struct sparx5_wildcard_rule *rule)
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
		 sparx5_vcap_keyset_name(ndev, rule->keyset),
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
		       sparx5_vcap_keyset_name(ndev, rule->keyset),
		       vrule->id);
		goto out;
	}
	/* Some keysets do not have a type field */
	vcap_rule_mod_key_u32(vrule, VCAP_KF_TYPE, rule->value, ~rule->mask);
	err = vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		pr_err("%s:%d: could not validate rule: %u\n",
		       __func__, __LINE__, vrule->id);
		sparx5_tc_flower_set_exterr(ndev, fco, vrule);
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

static int sparx5_tc_add_remaining_rules(struct net_device *ndev,
					 struct flow_cls_offload *fco,
					 struct vcap_rule *erule,
					 struct vcap_admin *admin,
					 struct sparx5_multiple_rules *multi)
{
	int idx, err = 0;

	/* ES0 only has one keyset, so no keyset wildcarding */
	if (admin->vtype == VCAP_TYPE_ES0)
		return err;

	for (idx = 0; idx < SPX5_MAX_RULE_SIZE; ++idx) {
		if (!multi->rule[idx].selected)
			continue;
		err = sparx5_tc_add_rule_copy(ndev, fco, erule, &multi->rule[idx]);
		if (err)
			break;
	}
	return err;
}

static int sparx5_tc_add_rule_link(struct vcap_admin *admin,
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
		/* Between IS0 instances the G_IDX value is used */
		/* Between IS0 and IS2 the PAG value is used */
		/* Between IS0 and ES0/ES2 the ISDX value is used */
		if (admin->vtype == VCAP_TYPE_IS0 && to_admin->vtype == VCAP_TYPE_IS0) {
			/* This works for IS0->IS0 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_NXT_IDX, diff);
			if (err)
				goto out;
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_NXT_IDX_CTRL, 1); /* Replace */
			if (err)
				goto out;
		} else if (admin->vtype == VCAP_TYPE_IS0 && to_admin->vtype == VCAP_TYPE_IS2) {
			/* This works for IS0->IS2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_VAL, diff);
			if (err)
				goto out;
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_OVERRIDE_MASK, 0xff);
			if (err)
				goto out;
		} else if (admin->vtype == VCAP_TYPE_IS0 && (to_admin->vtype == VCAP_TYPE_ES0 ||
							     to_admin->vtype == VCAP_TYPE_ES2)) {
			/* This works for IS0->ESx */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_ISDX_VAL, diff);
			if (err)
				goto out;
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_ISDX_ADD_REPLACE_SEL, VCAP_BIT_1);
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

static int sparx5_tc_add_rule_link_target(struct vcap_admin *admin,
					  struct vcap_rule *vrule,
					  int target_cid)
{
	int link_val = target_cid % VCAP_CID_LOOKUP_SIZE;
	int err;

	if (!link_val)
		return 0;
	switch (admin->vtype) {
	case VCAP_TYPE_IS0:
		/* Choose IS0 specific NXT_IDX key (for chaining rules from IS0) */
		err = vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_GEN_IDX_SEL, 1, ~0);
		if (err)
			return err;
		return vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_GEN_IDX, link_val, ~0);
	case VCAP_TYPE_IS2:
		/* Add IS2 specific PAG key (for chaining rules from IS0) */
		return vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_PAG, link_val, ~0);
	case VCAP_TYPE_ES0:
	case VCAP_TYPE_ES2:
		/* Add ES0 specific ISDX key (for chaining rules from IS0) */
		return vcap_rule_add_key_u32(vrule, VCAP_KF_ISDX_CLS, link_val, ~0);
	default:
		break;
	}
	return 0;
}

static int sparx5_tc_add_rule_counter(struct vcap_admin *admin,
				      struct vcap_rule *vrule)
{
	int err = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS2:
	case VCAP_TYPE_ES2:
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_CNT_ID, vrule->id);
		break;
	case VCAP_TYPE_ES0:
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_ESDX, vrule->id);
		break;
	default:
		break;
	}
	return err;
}

static int sparx5_tc_set_default_actionset(struct vcap_admin *admin,
					   struct vcap_rule *vrule,
					   int cid)
{
	int err = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS0:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_CLASSIFICATION);
		break;
	case VCAP_TYPE_IS2:
	case VCAP_TYPE_ES2:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
		break;
	case VCAP_TYPE_ES0:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_ES0);
		break;
	default:
		break;
	}
	return err;

}

static int sparx5_tc_flower_filter_rule(struct net_device *ndev,
					struct vcap_rule *vrule)
{
	enum vcap_key_field key;
	int res = 0;

	/* Select a key that is not needed in a keyset */
	switch (vrule->keyset) {
	case VCAP_KFS_IP4_TCP_UDP:
	case VCAP_KFS_IP6_TCP_UDP:
		key = VCAP_KF_TCP_UDP_IS;
		pr_debug("%s:%d: remove key: %s\n",
			 __func__, __LINE__, sparx5_vcap_key_name(ndev, key));
		res = vcap_rule_rem_key(vrule, key);
		break;
	default:
		break;
	}
	switch (vrule->keyset) {
	case VCAP_KFS_IP6_STD:
	case VCAP_KFS_IP6_OTHER:
	case VCAP_KFS_IP6_TCP_UDP:
		key = VCAP_KF_IP4_IS;
		pr_debug("%s:%d: remove key: %s\n",
			 __func__, __LINE__, sparx5_vcap_key_name(ndev, key));
		return vcap_rule_rem_key(vrule, key);
		break;
	default:
		break;
	}
	return res;
}

static void sparx5_tc_flower_use_template(struct net_device *ndev,
					  struct flow_cls_offload *fco,
					  struct vcap_rule *vrule)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5_tc_flower_template *ftmp;
	int idx = 0;

	list_for_each_entry(ftmp, &port->tc.templates, list) {
		if (fco->common.chain_index == ftmp->vcap_chain_id) {
			pr_debug("%s:%d: [%02d]: chain: %d, keyset: %s \n",
				 __func__, __LINE__,
				 idx, ftmp->vcap_chain_id,
				 sparx5_vcap_keyset_name(ndev, ftmp->keyset));
			vcap_set_rule_set_keyset(vrule, ftmp->keyset);
			sparx5_tc_flower_filter_rule(ndev, vrule);
			break;
		}
		++idx;
	}
}

/* Use the ethertype to choose a keyset from the port configuration */
static int sparx5_tc_flower_port_keyset(struct net_device *ndev,
					 struct vcap_admin *admin,
					 struct vcap_rule *vrule,
					 u16 l3_proto)
{
	struct vcap_keyset_list portkeysetlist = {0};
	enum vcap_keyfield_set portkeysets[12] = {0};
	int err;

	if (sparx5_tc_is_known_etype(l3_proto)) {
		portkeysetlist.max = ARRAY_SIZE(portkeysets);
		portkeysetlist.keysets = portkeysets;
		err = sparx5_vcap_get_port_keyset(ndev, admin,
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

static int sparx5_tc_flower_reduce_rule(struct net_device *ndev,
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
					 sparx5_vcap_key_name(ndev,
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

static void sparx5_tc_flower_set_port_mask(struct vcap_u72_action *ports,
					   struct net_device *ndev)
{
	struct sparx5_port *port = netdev_priv(ndev);
	int byidx = port->portno / BITS_PER_BYTE;
	int biidx = port->portno % BITS_PER_BYTE;

	ports->value[byidx] |= BIT(biidx);
}

static int sparx5_tc_flower_parse_act_gate(struct sparx5_psfp_sg *sg,
					   struct flow_action_entry *act)
{
	int i;

	if (act->gate.prio < -1 || act->gate.prio > SPARX5_PSFP_SG_MAX_IPV)
		return -EINVAL;

	if (act->gate.cycletime < SPARX5_PSFP_SG_MIN_CYCLE_TIME_NS ||
	    act->gate.cycletime > SPARX5_PSFP_SG_MAX_CYCLE_TIME_NS)
		return -EINVAL;

	if (act->gate.cycletimeext > SPARX5_PSFP_SG_MAX_CYCLE_TIME_NS)
		return -EINVAL;

	if (act->gate.num_entries >= SPARX5_PSFP_GCE_NUM)
		return -EINVAL;

	sg->gate_state = true;
	sg->ipv = act->gate.prio;
	sg->num_entries = act->gate.num_entries;
	sg->cycletime = act->gate.cycletime;
	sg->cycletimeext = act->gate.cycletimeext;

	for (i = 0; i < sg->num_entries; i++) {
		sg->gce[i].gate_state = !!act->gate.entries[i].gate_state;
		sg->gce[i].interval = act->gate.entries[i].interval;
		sg->gce[i].ipv = act->gate.entries[i].ipv;
		sg->gce[i].maxoctets = act->gate.entries[i].maxoctets;
	}

	return 0;
}

static int sparx5_tc_flower_parse_act_police(struct sparx5_policer *pol,
					     struct flow_action_entry *act)
{
	pol->type = SPX5_POL_SERVICE;
	pol->rate = div_u64(act->police.rate_bytes_ps, 1000) * 8;
	pol->burst = act->police.burst;
	pol->idx = act->police.index;

	return 0;
}

static int sparx5_tc_flower_psfp_setup(struct sparx5 *sparx5,
				       struct vcap_rule *vrule, int sg_idx,
				       int pol_idx, struct sparx5_psfp_sg *sg,
				       struct sparx5_psfp_fm *fm,
				       struct sparx5_psfp_sf *sf)
{
	u32 psfp_sfid = 0, psfp_fmid = 0, psfp_sgid = 0;
	int ret;

	/* Must always have a stream gate - max sdu is evaluated after
	 * frames have passed the gate, so in case of only a policer, we
	 * allocate a stream gate that is always open.
	 */
	if (sg_idx < 0) {
		sg_idx = sparx5_pool_idx_to_id(SPARX5_PSFP_SG_OPEN);
		memcpy(sg, &sparx5_sg_open, sizeof(*sg));
	}

	ret = sparx5_psfp_sg_add(sparx5, sg_idx, sg, &psfp_sgid);
	if (ret < 0)
		return ret;

	if (pol_idx >= 0) {
		/* Add new flow-meter */
		ret = sparx5_psfp_fm_add(sparx5, pol_idx, fm, &psfp_fmid);
		if (ret < 0)
			return ret;
	}

	/* Map stream filter to stream gate */
	sf->sgid = psfp_sgid;

	/* Add new stream-filter and map it to a steam gate */
	ret = sparx5_psfp_sf_add(sparx5, sf, &psfp_sfid);
	if (ret < 0)
		return ret;

	/* Streams are classified by ISDX.
	 * Map ISDX 1:1 to sfid for now.
	 */
	sparx5_isdx_conf_set(sparx5, psfp_sfid, psfp_sfid, psfp_fmid);

	ret = vcap_rule_add_action_bit(vrule, VCAP_AF_ISDX_ADD_REPLACE_SEL,
				       VCAP_BIT_1);
	if (ret)
		return ret;

	ret = vcap_rule_add_action_u32(vrule, VCAP_AF_ISDX_VAL, psfp_sfid);
	if (ret)
		return ret;

	return 0;
}

static int sparx5_tc_flower_replace(struct net_device *ndev,
				    struct flow_cls_offload *fco,
				    struct vcap_admin *admin)
{
	struct sparx5_psfp_sf sf = { .max_sdu = GENMASK(14, 0) }; /* All ones */
	int err, idx, tc_sg_idx = -1, tc_pol_idx = -1;
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5_multiple_rules multi = {0};
	struct sparx5 *sparx5 = port->sparx5;
	struct vcap_u72_action ports = {0};
	struct sparx5_psfp_sg sg = {0};
	struct sparx5_psfp_fm fm = {0};
	struct flow_action_entry *act;
	struct flow_rule *frule;
	struct vcap_rule *vrule;
	u16 l3_proto;

	vrule = vcap_alloc_rule(ndev, fco->common.chain_index, VCAP_USER_TC,
				fco->common.prio, 0);
	if (IS_ERR(vrule)) {
		pr_err("%s:%d: could not allocate rule: %u\n", __func__, __LINE__, vrule->id);
		return PTR_ERR(vrule);
	}
	vrule->cookie = fco->cookie;
	frule = flow_cls_offload_flow_rule(fco);
	err = sparx5_tc_use_dissectors(fco, admin, vrule, &l3_proto);
	if (err)
		goto out;
	sparx5_tc_flower_use_template(ndev, fco, vrule);
	err = sparx5_tc_add_rule_link_target(admin, vrule, fco->common.chain_index);
	if (err)
		goto out;
	err = sparx5_tc_add_rule_counter(admin, vrule);
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
		case FLOW_ACTION_GATE: {
			err = sparx5_tc_flower_parse_act_gate(&sg, act);
			if (err)
				goto out;

			tc_sg_idx = act->gate.index;

			break;
		}

		case FLOW_ACTION_POLICE: {
			err = sparx5_tc_flower_parse_act_police(&fm.pol, act);
			if (err)
				goto out;

			tc_pol_idx = fm.pol.idx;
			sf.max_sdu = act->police.mtu;

			break;
		}
		case FLOW_ACTION_TRAP:
			if (admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Trap action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			/* VCAP_AF_CPU_COPY_ENA: W1, sparx5: is2/es2 */
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_COPY_ENA, VCAP_BIT_1);
			if (err)
				goto out;
			/* VCAP_AF_CPU_QUEUE_NUM: W3, sparx5: is2/es2 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_CPU_QUEUE_NUM, 0);
			if (err)
				goto out;
			/* VCAP_AF_MASK_MODE: sparx5 is0 W3, sparx5 is2 W3 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, SPX5_PMM_REPLACE_ALL);
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
			/* VCAP_AF_MASK_MODE: sparx5 is0 W3, sparx5 is2 W3 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, SPX5_PMM_REPLACE_ALL);
			if (err)
				goto out;
			/* VCAP_AF_POLICE_ENA: W1, sparx5: is2/es2 */
			err = vcap_rule_add_action_bit(vrule, VCAP_AF_POLICE_ENA, VCAP_BIT_1);
			if (err)
				goto out;
			/* VCAP_AF_POLICE_IDX: sparx5 is2 W6, sparx5 es2 W6 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_POLICE_IDX, SPX5_POL_ACL_DISCARD);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_MIRRED:
			if (admin->vtype != VCAP_TYPE_IS0 && admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "Mirror action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			/* VCAP_AF_MASK_MODE: sparx5 is0 W3, sparx5 is2 W3 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, SPX5_PMM_OR_DSTMASK);
			if (err)
				goto out;
			/* VCAP_AF_PORT_MASK: sparx5 is0 W65, sparx5 is2 W68 */
			sparx5_tc_flower_set_port_mask(&ports, act->dev);
			err = vcap_rule_add_action_u72(vrule, VCAP_AF_PORT_MASK, &ports);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_REDIRECT:
			if (admin->vtype != VCAP_TYPE_IS0 && admin->vtype != VCAP_TYPE_IS2) {
				NL_SET_ERR_MSG_MOD(fco->common.extack,
						   "redirect action not supported in this VCAP");
				err = -EOPNOTSUPP;
				goto out;
			}
			/* VCAP_AF_MASK_MODE: sparx5 is0 W3, sparx5 is2 W3 */
			err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, SPX5_PMM_REPLACE_ALL);
			if (err)
				goto out;
			/* VCAP_AF_PORT_MASK: sparx5 is0 W65, sparx5 is2 W68 */
			sparx5_tc_flower_set_port_mask(&ports, act->dev);
			err = vcap_rule_add_action_u72(vrule, VCAP_AF_PORT_MASK, &ports);
			if (err)
				goto out;
			break;
		case FLOW_ACTION_ACCEPT:
			sparx5_tc_set_default_actionset(admin, vrule,
							 fco->common.chain_index);
			break;
		case FLOW_ACTION_GOTO:
			sparx5_tc_add_rule_link(admin, vrule,
						fco->common.chain_index, act->chain_index);
			break;
		default:
			NL_SET_ERR_MSG_MOD(fco->common.extack,
					   "Unsupported TC action");
			err = -EOPNOTSUPP;
			goto out;
		}
	}

	/* Setup PSFP */
	if (tc_sg_idx >= 0 || tc_pol_idx >= 0) {
		err = sparx5_tc_flower_psfp_setup(sparx5, vrule, tc_sg_idx,
						  tc_pol_idx, &sg, &fm, &sf);
		if (err)
			goto out;
	}

	err = sparx5_tc_select_protocol_keyset(ndev, vrule, admin, l3_proto, &multi);
	if (err) {
		pr_err("%s:%d: Could not find usable keyset for rule: %u\n", __func__, __LINE__, vrule->id);
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No matching port keyset for filter protocol and keys");
		goto out;
	}
	err = vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		err = sparx5_tc_flower_port_keyset(ndev, admin, vrule, l3_proto);
		if (err) {
			pr_err("%s:%d: Could not find port keyset: %u\n", __func__, __LINE__, vrule->id);
			NL_SET_ERR_MSG_MOD(fco->common.extack, "Could not validate the filter");
			goto out;
		}
		err = sparx5_tc_flower_reduce_rule(ndev, vrule);
		if (err) {
			pr_err("%s:%d: Could not validate rule: %u\n", __func__, __LINE__, vrule->id);
			sparx5_tc_flower_set_exterr(ndev, fco, vrule);
			goto out;
		}
	}
	pr_debug("%s:%d: chain: %d, keyset: %s \n",
		 __func__, __LINE__,
		 fco->common.chain_index,
		 sparx5_vcap_keyset_name(ndev, vrule->keyset));
	err = vcap_add_rule(vrule);
	if (err) {
		pr_err("%s:%d: Could not add rule: %u\n", __func__, __LINE__, vrule->id);
		NL_SET_ERR_MSG_MOD(fco->common.extack, "Could not add the filter");
		goto out;
	}
	pr_debug("%s:%d: created rule: %u\n", __func__, __LINE__, vrule->id);
	if (l3_proto == ETH_P_ALL)
		err = sparx5_tc_add_remaining_rules(ndev, fco, vrule, admin, &multi);
out:
	vcap_free_rule(vrule);
	return err;
}

static int sparx5_tc_free_rule_resources(struct net_device *ndev, int rule_id)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct vcap_client_actionfield *afield;
	struct sparx5 *sparx5 = port->sparx5;
	u32 isdx, sfid, sgid, fmid;
	struct vcap_rule *vrule;
	int ret = 0, err;

	vrule = vcap_get_rule(ndev, rule_id);
	if (vrule == NULL || IS_ERR(vrule))
		return -EINVAL;

	/* Check for enabled mirroring in this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_MIRROR_ENA);
	if (afield && afield->ctrl.type == VCAP_FIELD_BIT && afield->data.u1.value) {
		pr_debug("%s:%d: rule %d: remove mirroring\n",
			 __func__, __LINE__, vrule->id);
	}

	/* Check for an enabled policer for this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_POLICE_ENA);
	if (afield && afield->ctrl.type == VCAP_FIELD_BIT && afield->data.u1.value) {
		/* Release policer reserved by this rule */
		pr_debug("%s:%d: rule %d: remove policer\n",
			 __func__, __LINE__, vrule->id);
	}

	/* Check if VCAP_AF_ISDX_VAL action is set for this rule - and if
	 * it is used for stream and/or flow-meter classification.
	 */
	afield = vcap_find_actionfield(vrule, VCAP_AF_ISDX_VAL);
	if (afield) {
		isdx = afield->data.u32.value;
		sfid = sparx5_psfp_isdx_get_sf(sparx5, isdx);

		if (sfid) {
			fmid = sparx5_psfp_isdx_get_fm(sparx5, isdx);
			sgid = sparx5_psfp_sf_get_sg(sparx5, sfid);

			pr_info("Deleting stream: isdx: %d sfid: %d, fmid: %d sgid: %d", isdx, sfid, fmid, sgid);

			if (fmid) {
				err = sparx5_psfp_fm_del(sparx5, fmid);
				if (err) {
					pr_err("%s:%d Could not delete invalid fmid: %d",
					       __func__, __LINE__, fmid);
				}
			}

			if (sgid) {
				err = sparx5_psfp_sg_del(sparx5, sgid);
				if (err) {
					pr_err("%s:%d Could not delete invalid sgid: %d",
					       __func__, __LINE__, sgid);
				}
			}

			err = sparx5_psfp_sf_del(sparx5, sfid);
			if (err) {
				pr_err("%s:%d Could not delete invalid sfid: %d",
				       __func__, __LINE__, sfid);
			}
			sparx5_isdx_conf_set(sparx5, isdx, 0, 0);
		}
	}

	vcap_free_rule(vrule);
	return ret;
}

static int sparx5_tc_flower_destroy(struct net_device *ndev,
				    struct flow_cls_offload *fco,
				    struct vcap_admin *admin)
{
	int err = -ENOENT, rule_id;
	int count = 0;

	while (true) {
		rule_id = vcap_lookup_rule_by_cookie(fco->cookie);

		if (rule_id <= 0)
			break;

		if (count == 0) {
			/* Resources are attached to the first rule of
				 * a set of rules. Only works if the rules are
				 * in the correct order.
				 */
			err = sparx5_tc_free_rule_resources(ndev, rule_id);
			if (err)
				pr_err("%s:%d: could not get rule %d\n",
				       __func__, __LINE__, rule_id);
		}
		err = vcap_del_rule(ndev, rule_id);
		if (err) {
			pr_err("%s:%d: could not delete rule %d\n", __func__,
			       __LINE__, rule_id);
			break;
		}

		++count;
	}
	return err;
}

/* Collect packet counts from all rules with the same cookie */
static int sparx5_tc_rule_counter_cb(void *arg, struct vcap_rule *rule)
{
	struct sparx5_tc_rule_pkt_cnt *rinfo = arg;
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

static int sparx5_tc_flower_stats(struct net_device *ndev,
				  struct flow_cls_offload *fco,
				  struct vcap_admin *admin)
{
	struct sparx5_tc_rule_pkt_cnt rinfo = {0};
	int err = -ENOENT;
	ulong lastused = 0;
	u64 drops = 0;
	u32 pkts = 0;

	rinfo.cookie = fco->cookie;
	err = vcap_rule_iter(sparx5_tc_rule_counter_cb, &rinfo);
	if (err)
		return err;
	pkts = rinfo.pkts;
	flow_stats_update(&fco->stats, 0x0, pkts, drops, lastused,
			  FLOW_ACTION_HW_STATS_IMMEDIATE);
	return err;
}

enum vcap_keyfield_set sparx5_all_keysets[] = {
	VCAP_KFS_MAC_ETYPE,
};

enum vcap_keyfield_set sparx5_ipv4_keysets[] = {
	VCAP_KFS_IP4_TCP_UDP,
	VCAP_KFS_IP4_OTHER,
};

enum vcap_keyfield_set sparx5_ipv6_keysets[] = {
	VCAP_KFS_IP_7TUPLE,
	VCAP_KFS_NORMAL_7TUPLE,
	VCAP_KFS_IP6_STD,
};

enum vcap_keyfield_set sparx5_arp_keysets[] = {
	VCAP_KFS_ARP,
};

enum vcap_keyfield_set sparx5_8021q_keysets[] = {
	VCAP_KFS_IP_7TUPLE,
	VCAP_KFS_MAC_ETYPE,
};

enum vcap_keyfield_set sparx5_8021ad_keysets[] = {
	VCAP_KFS_IP_7TUPLE,
	VCAP_KFS_MAC_ETYPE,
};


/* Return the index of the best matching keyset according to L3 protocol */
static int sparx5_tc_flower_select_keyset(struct vcap_keyset_match *match,
					  u16 l3_proto)
{
	int idx, jdx, max = 0;
	enum vcap_keyfield_set *keysets;

	switch (l3_proto) {
	case ETH_P_ALL:
		keysets = sparx5_all_keysets;
		max = ARRAY_SIZE(sparx5_all_keysets);
		break;
	case ETH_P_IP:
		keysets = sparx5_ipv4_keysets;
		max = ARRAY_SIZE(sparx5_ipv4_keysets);
		break;
	case ETH_P_IPV6:
		keysets = sparx5_ipv6_keysets;
		max = ARRAY_SIZE(sparx5_ipv6_keysets);
		break;
	case ETH_P_ARP:
		keysets = sparx5_arp_keysets;
		max = ARRAY_SIZE(sparx5_arp_keysets);
		break;
	case ETH_P_8021Q:
		keysets = sparx5_8021q_keysets;
		max = ARRAY_SIZE(sparx5_8021q_keysets);
		break;
	case ETH_P_8021AD:
		keysets = sparx5_8021ad_keysets;
		max = ARRAY_SIZE(sparx5_8021ad_keysets);
		break;
	}
	for (idx = 0; idx < max; ++idx) /* highest priority */
		for (jdx = 0; jdx < match->matches.cnt; ++jdx)
			if (keysets[idx] == match->matches.keysets[jdx])
				return jdx;
	return 0;
}

static int sparx5_tc_flower_template_create(struct net_device *ndev,
					    struct flow_cls_offload *fco,
					    struct vcap_admin *admin)
{
	enum vcap_key_field unmatched_keys[SPX5_VCAP_KEYS_MAX];
	struct sparx5_port *port = netdev_priv(ndev);
	struct vcap_keyset_list portkeysetlist = {0};
	enum vcap_keyfield_set portkeysets[12] = {0};
	struct sparx5_tc_flower_template *ftmp;
	struct vcap_keyset_match match = {0};
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
	ftmp->keylist.max = SPX5_VCAP_KEYS_MAX;
	match.matches.keysets = keysets;
	match.matches.max = ARRAY_SIZE(keysets);
	match.unmatched_keys.keys = unmatched_keys;
	match.unmatched_keys.max = ARRAY_SIZE(unmatched_keys);
	sparx5_tc_match_dissectors(fco, admin, &ftmp->keylist, &l3_proto, &l4_proto);
	ftmp->l3_proto = l3_proto;
	ftmp->l4_proto = l4_proto;
	/* Check if a keyset that fits exists */
	if (vcap_rule_match_keysets(admin->vtype, &ftmp->keylist, &match)) {
		idx = sparx5_tc_flower_select_keyset(&match, l3_proto);
		ftmp->keyset = match.matches.keysets[idx];
		pr_debug("%s:%d: chosen via L3 proto: %s\n", __func__, __LINE__,
			 sparx5_vcap_keyset_name(ndev, match.matches.keysets[idx]));
	} else {
		ftmp->keyset = match.best_match;
		pr_debug("%s:%d: best match: %s missing: %d\n", __func__, __LINE__,
			 sparx5_vcap_keyset_name(ndev, match.best_match),
			 match.unmatched_keys.cnt);
	}
	portkeysetlist.max = ARRAY_SIZE(portkeysets);
	portkeysetlist.keysets = portkeysets;
	/* Update the port configuration if needed */
	err = sparx5_vcap_get_port_keyset(ndev, admin, fco->common.chain_index,
					  l3_proto,
					  &portkeysetlist);
	/* Pick the first keyset from the port config */
	if (err == 0 && portkeysetlist.cnt > 0) {
		ftmp->original = portkeysets[0];
		if (ftmp->original != ftmp->keyset)
			sparx5_vcap_set_port_keyset(ndev, admin,
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

static int sparx5_tc_flower_template_destroy(struct net_device *ndev,
					     struct flow_cls_offload *fco,
					     struct vcap_admin *admin)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5_tc_flower_template *ftmp, *tmp;
	int err = -ENOENT;

	/* The TC framework automatically removes the rules using the template */
	list_for_each_entry_safe(ftmp, tmp, &port->tc.templates, list) {
		if (ftmp->vcap_chain_id == fco->common.chain_index) {
			/* Restore port config */
			if (ftmp->original != ftmp->keyset)
				sparx5_vcap_set_port_keyset(ndev, admin,
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

int sparx5_tc_flower(struct net_device *ndev, struct flow_cls_offload *fco,
		     bool ingress)
{
	struct vcap_admin *admin;
	struct flow_rule *frule;
	int err = -EINVAL;

	pr_debug("%s:%d: %s: command: %s, chain: %u, proto: 0x%04x, prio: %u, classid: %u, cookie: %lx\n",
		 __func__, __LINE__,
		 netdev_name(ndev),
		 tc_dbg_flow_cls_command(fco->command), fco->common.chain_index,
		 be16_to_cpu(fco->common.protocol), fco->common.prio, fco->classid,
		 fco->cookie);
	frule = flow_cls_offload_flow_rule(fco);
	if (frule) {
		tc_dbg_match_dump(ndev, frule);
		tc_dbg_actions_dump(ndev, frule);
	}
	/* Get vcap info */
	admin = vcap_find_admin(fco->common.chain_index);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(fco->common.extack, "Invalid chain");
		return err;
	}
	switch (fco->command) {
	case FLOW_CLS_REPLACE:
		return sparx5_tc_flower_replace(ndev, fco, admin);
	case FLOW_CLS_DESTROY:
		return sparx5_tc_flower_destroy(ndev, fco, admin);
	case FLOW_CLS_STATS:
		return sparx5_tc_flower_stats(ndev, fco, admin);
	case FLOW_CLS_TMPLT_CREATE:
		return sparx5_tc_flower_template_create(ndev, fco, admin);
	case FLOW_CLS_TMPLT_DESTROY:
		return sparx5_tc_flower_template_destroy(ndev, fco, admin);
	default:
		return -EOPNOTSUPP;
	}
}
