/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include "lan966x_tc_dbg.h"
#include <net/tc_act/tc_gate.h>

static const char * const tc_setup_type_strings[] = {
	[TC_SETUP_QDISC_MQPRIO] = "QDISC_MQPRIO",
	[TC_SETUP_CLSU32]       = "CLSU32",
	[TC_SETUP_CLSFLOWER]    = "CLSFLOWER",
	[TC_SETUP_CLSMATCHALL]  = "CLSMATCHALL",
	[TC_SETUP_CLSBPF]       = "CLSBPF",
	[TC_SETUP_BLOCK]        = "BLOCK",
	[TC_SETUP_QDISC_CBS]    = "QDISC_CBS",
	[TC_SETUP_QDISC_RED]    = "QDISC_RED",
	[TC_SETUP_QDISC_PRIO]   = "QDISC_PRIO",
	[TC_SETUP_QDISC_MQ]     = "QDISC_MQ",
	[TC_SETUP_QDISC_ETF]    = "QDISC_ETF",
	[TC_SETUP_ROOT_QDISC]   = "ROOT_QDISC",
	[TC_SETUP_QDISC_GRED]   = "QDISC_GRED",
	[TC_SETUP_QDISC_TAPRIO] = "QDISC_TAPRIO",
	[TC_SETUP_FT]           = "FT",
	[TC_SETUP_QDISC_ETS]    = "QDISC_ETS",
	[TC_SETUP_QDISC_TBF]    = "QDISC_TBF",
	[TC_SETUP_QDISC_FIFO]   = "QDISC_FIFO",
};

const char *tc_dbg_tc_setup_type(enum tc_setup_type type)
{
	if (type > TC_SETUP_QDISC_FIFO)
		return "INVALID TC_SETUP_TYPE!";
	return tc_setup_type_strings[type];
}

static const char * const tc_root_disc_command_strings[] = {
	[TC_ROOT_GRAFT] = "ROOT_GRAFT",
};

const char *tc_dbg_root_command(enum tc_root_command command)
{
	if (command > TC_ROOT_GRAFT)
		return "UNKNOWN TC_ROOT_COMMAND!";
	return tc_root_disc_command_strings[command];
}

static const char * const flow_block_binder_type_strings[] = {
	[FLOW_BLOCK_BINDER_TYPE_UNSPEC]         = "FBBT_UNSPEC",
	[FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS] = "FBBT_INGRESS",
	[FLOW_BLOCK_BINDER_TYPE_CLSACT_EGRESS]  = "FBBT_EGRESS",
};

const char *tc_dbg_flow_block_binder_type(enum flow_block_binder_type type)
{
	if (type > FLOW_BLOCK_BINDER_TYPE_CLSACT_EGRESS)
		return "INVALID FLOW_BLOCK_BINDER_TYPE!";
	return flow_block_binder_type_strings[type];
}

static const char * const flow_block_command_strings[] = {
	[FLOW_BLOCK_BIND]   = "FBC_BIND",
	[FLOW_BLOCK_UNBIND] = "FBC_UNBIND",
};

const char *tc_dbg_flow_block_command(enum flow_block_command command)
{
	if (command > FLOW_BLOCK_UNBIND)
		return "INVALID FLOW_BLOCK_COMMAND!";
	return flow_block_command_strings[command];
}

static const char * const flow_cls_command_strings[] = {
	[FLOW_CLS_REPLACE]       = "FCC_REPLACE",
	[FLOW_CLS_DESTROY]       = "FCC_DESTROY",
	[FLOW_CLS_STATS]         = "FCC_STATS",
	[FLOW_CLS_TMPLT_CREATE]  = "FCC_TMPLT_CREATE",
	[FLOW_CLS_TMPLT_DESTROY] = "FCC_TMPLT_DESTROY",
};

const char *tc_dbg_flow_cls_command(enum flow_cls_command command)
{
	if (command > FLOW_CLS_TMPLT_DESTROY)
		return "INVALID FLOW_CLS_COMMAND!";
	return flow_cls_command_strings[command];
}

static const char * const flow_action_id_strings[] = {
	[FLOW_ACTION_ACCEPT]           = "FA_ACCEPT",
	[FLOW_ACTION_DROP]             = "FA_DROP",
	[FLOW_ACTION_TRAP]             = "FA_TRAP",
	[FLOW_ACTION_GOTO]             = "FA_GOTO",
	[FLOW_ACTION_REDIRECT]         = "FA_REDIRECT",
	[FLOW_ACTION_MIRRED]           = "FA_MIRRED",
	[FLOW_ACTION_REDIRECT_INGRESS] = "FA_REDIRECT_INGRESS",
	[FLOW_ACTION_MIRRED_INGRESS]   = "FA_MIRRED_INGRESS",
	[FLOW_ACTION_VLAN_PUSH]        = "FA_VLAN_PUSH",
	[FLOW_ACTION_VLAN_POP]         = "FA_VLAN_POP",
	[FLOW_ACTION_VLAN_MANGLE]      = "FA_VLAN_MANGLE",
	[FLOW_ACTION_TUNNEL_ENCAP]     = "FA_TUNNEL_ENCAP",
	[FLOW_ACTION_TUNNEL_DECAP]     = "FA_TUNNEL_DECAP",
	[FLOW_ACTION_MANGLE]           = "FA_MANGLE",
	[FLOW_ACTION_ADD]              = "FA_ADD",
	[FLOW_ACTION_CSUM]             = "FA_CSUM",
	[FLOW_ACTION_MARK]             = "FA_MARK",
	[FLOW_ACTION_PTYPE]            = "FA_PTYPE",
	[FLOW_ACTION_PRIORITY]         = "FA_PRIORITY",
	[FLOW_ACTION_WAKE]             = "FA_WAKE",
	[FLOW_ACTION_QUEUE]            = "FA_QUEUE",
	[FLOW_ACTION_SAMPLE]           = "FA_SAMPLE",
	[FLOW_ACTION_POLICE]           = "FA_POLICE",
	[FLOW_ACTION_CT]               = "FA_CT",
	[FLOW_ACTION_CT_METADATA]      = "FA_CT_METADATA",
	[FLOW_ACTION_MPLS_PUSH]        = "FA_MPLS_PUSH",
	[FLOW_ACTION_MPLS_POP]         = "FA_MPLS_POP",
	[FLOW_ACTION_MPLS_MANGLE]      = "FA_MPLS_MANGLE",
	[FLOW_ACTION_GATE]             = "FA_GATE",
	[NUM_FLOW_ACTIONS]             = "NUM_FA",
};

const char *tc_dbg_flow_action_id(enum flow_action_id id)
{
	if (id >= NUM_FLOW_ACTIONS)
		return "INVALID FLOW_ACTION_ID!";
	return flow_action_id_strings[id];
}

static const char * const flow_dissector_key_id_strings[] = {
	[FLOW_DISSECTOR_KEY_CONTROL] = "FDK_CONTROL",
	[FLOW_DISSECTOR_KEY_BASIC] = "FDK_BASIC",
	[FLOW_DISSECTOR_KEY_IPV4_ADDRS] = "FDK_IPV4_ADDRS",
	[FLOW_DISSECTOR_KEY_IPV6_ADDRS] = "FDK_IPV6_ADDRS",
	[FLOW_DISSECTOR_KEY_PORTS] = "FDK_PORTS",
	[FLOW_DISSECTOR_KEY_PORTS_RANGE] = "FDK_PORTS_RANGE",
	[FLOW_DISSECTOR_KEY_ICMP] = "FDK_ICMP",
	[FLOW_DISSECTOR_KEY_ETH_ADDRS] = "FDK_ETH_ADDRS",
	[FLOW_DISSECTOR_KEY_TIPC] = "FDK_TIPC",
	[FLOW_DISSECTOR_KEY_ARP] = "FDK_ARP",
	[FLOW_DISSECTOR_KEY_VLAN] = "FDK_VLAN",
	[FLOW_DISSECTOR_KEY_FLOW_LABEL] = "FDK_FLOW_LABEL",
	[FLOW_DISSECTOR_KEY_GRE_KEYID] = "FDK_GRE_KEYID",
	[FLOW_DISSECTOR_KEY_MPLS_ENTROPY] = "FDK_MPLS_ENTROPY",
	[FLOW_DISSECTOR_KEY_ENC_KEYID] = "FDK_ENC_KEYID",
	[FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS] = "FDK_ENC_IPV4_ADDRS",
	[FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS] = "FDK_ENC_IPV6_ADDRS",
	[FLOW_DISSECTOR_KEY_ENC_CONTROL] = "FDK_ENC_CONTROL",
	[FLOW_DISSECTOR_KEY_ENC_PORTS] = "FDK_ENC_PORTS",
	[FLOW_DISSECTOR_KEY_MPLS] = "FDK_MPLS",
	[FLOW_DISSECTOR_KEY_TCP] = "FDK_TCP",
	[FLOW_DISSECTOR_KEY_IP] = "FDK_IP",
	[FLOW_DISSECTOR_KEY_CVLAN] = "FDK_CVLAN",
	[FLOW_DISSECTOR_KEY_ENC_IP] = "FDK_ENC_IP",
	[FLOW_DISSECTOR_KEY_ENC_OPTS] = "FDK_ENC_OPTS",
	[FLOW_DISSECTOR_KEY_META] = "FDK_META",
	[FLOW_DISSECTOR_KEY_CT] = "FDK_CT",
	[FLOW_DISSECTOR_KEY_MAX] = "FDK_MAX",
};

const char *tc_dbg_flow_dissector_key_id(enum flow_dissector_key_id id)
{
	if (id >= FLOW_DISSECTOR_KEY_MAX)
		return "INVALID FLOW_DISSECTOR_KEY_ID!";
	return flow_dissector_key_id_strings[id];
}

static const char * const tc_matchall_command_strings[] = {
	[TC_CLSMATCHALL_REPLACE] = "MATCHALL_REPLACE",
	[TC_CLSMATCHALL_DESTROY] = "MATCHALL_DESTROY",
	[TC_CLSMATCHALL_STATS]   = "MATCHALL_STATS",
};

const char *tc_dbg_tc_matchall_command(enum tc_matchall_command command)
{
	if (command > TC_CLSMATCHALL_STATS)
		return "INVALID TC_MATCHALL_COMMAND!";
	return tc_matchall_command_strings[command];
}

void tc_dbg_match_dump(const struct net_device *dev,
		       const struct flow_rule *r)
{
	unsigned int uk = 0;

	if (!r || !r->match.dissector)
		return;

	uk = r->match.dissector->used_keys;
	netdev_dbg(dev, "used keys 0x%08x\n", uk);

	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;
		flow_rule_match_control(r, &match);
		netdev_dbg(dev, "CONTROL thoff %04x/%04x addr_type %04x/%04x flags %08x/%08x\n",
			   match.key->thoff, match.mask->thoff,
			   match.key->addr_type, match.mask->addr_type,
			   match.key->flags, match.mask->flags);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_CONTROL);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;
		flow_rule_match_basic(r, &match);
		netdev_dbg(dev, "BASIC n_proto %04x/%04x ip_proto %02x/%02x\n",
			   be16_to_cpu(match.key->n_proto),
			   be16_to_cpu(match.mask->n_proto),
			   match.key->ip_proto, match.mask->ip_proto);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_BASIC);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;
		flow_rule_match_eth_addrs(r, &match);
		netdev_dbg(dev, "ETH_ADDRS dst %pM/%pM src %pM/%pM\n",
			   match.key->dst, match.mask->dst,
			   match.key->src, match.mask->src);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_ETH_ADDRS);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		flow_rule_match_vlan(r, &match);
		netdev_dbg(dev, "VLAN tpid %04x/%04x id %03x/%03x pcp %u/%u dei %u/%u\n",
			   be16_to_cpu(match.key->vlan_tpid),
			   be16_to_cpu(match.mask->vlan_tpid),
			   match.key->vlan_id, match.mask->vlan_id,
			   match.key->vlan_priority, match.mask->vlan_priority,
			   match.key->vlan_dei, match.mask->vlan_dei);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_VLAN);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;
		flow_rule_match_cvlan(r, &match);
		netdev_dbg(dev, "CVLAN tpid %04x/%04x id %03x/%03x pcp %u/%u dei %u/%u\n",
			   be16_to_cpu(match.key->vlan_tpid),
			   be16_to_cpu(match.mask->vlan_tpid),
			   match.key->vlan_id, match.mask->vlan_id,
			   match.key->vlan_priority, match.mask->vlan_priority,
			   match.key->vlan_dei, match.mask->vlan_dei);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_CVLAN);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_ARP)) {
		struct flow_match_arp match;
		flow_rule_match_arp(r, &match);
		netdev_dbg(dev, "ARP sip %pI4/%pI4 dip %pI4/%pI4 op %u/%u sha %pM/%pM tpa %pM/%pM\n",
			   &match.key->sip,
			   &match.mask->sip,
			   &match.key->tip,
			   &match.mask->tip,
			   match.key->op,
			   match.mask->op,
			   match.key->sha,
			   match.mask->sha,
			   match.key->tha,
			   match.mask->tha);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_ARP);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs match;
		flow_rule_match_ipv4_addrs(r, &match);
		netdev_dbg(dev, "IPV4_ADDRS src %pI4/%pI4 dst %pI4/%pI4\n",
			   &match.key->src, &match.mask->src,
			   &match.key->dst, &match.mask->dst);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_IPV4_ADDRS);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
		struct flow_match_ipv6_addrs match;
		flow_rule_match_ipv6_addrs(r, &match);
		netdev_dbg(dev, "IPV6_ADDRS src %pI6/%pI6 dst %pI6/%pI6\n",
			   &match.key->src, &match.mask->src,
			   &match.key->dst, &match.mask->dst);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_IPV6_ADDRS);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;
		flow_rule_match_ports(r, &match);
		netdev_dbg(dev, "PORTS src %04x/%04x dst %04x/%04x\n",
			   be16_to_cpu(match.key->src),
			   be16_to_cpu(match.mask->src),
			   be16_to_cpu(match.key->dst),
			   be16_to_cpu(match.mask->dst));
		uk &= ~(1 << FLOW_DISSECTOR_KEY_PORTS);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_TCP)) {
		struct flow_match_tcp match;
		flow_rule_match_tcp(r, &match);
		netdev_dbg(dev, "TCP flags %04x/%04x\n",
			   be16_to_cpu(match.key->flags),
			   be16_to_cpu(match.mask->flags));
		uk &= ~(1 << FLOW_DISSECTOR_KEY_TCP);
	}
	if (flow_rule_match_key(r, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;
		flow_rule_match_ip(r, &match);
		netdev_dbg(dev, "IP tos %02x/%02x ttl %02x/%02x\n",
			   match.key->tos, match.mask->tos,
			   match.key->ttl, match.mask->ttl);
		uk &= ~(1 << FLOW_DISSECTOR_KEY_IP);
	}
	/* Add more here */
	if (uk)
		netdev_dbg(dev, "UNHANDLED KEYS 0x%08x\n", uk);
}

void tc_dbg_actions_dump(const struct net_device *dev,
			 const struct flow_rule *r)
{
	const struct flow_action_entry *e;
	int i, g;

	if (!r)
		return;

	netdev_dbg(dev, "num_entries %d\n", r->action.num_entries);
	flow_action_for_each(i, e, &r->action) {
		const char *idn = tc_dbg_flow_action_id(e->id);
		switch (e->id) {
		case FLOW_ACTION_ACCEPT:
		case FLOW_ACTION_DROP:
		case FLOW_ACTION_TRAP:
			netdev_dbg(dev, "[%d]%s\n", i, idn);
			break;
		case FLOW_ACTION_GOTO:
			netdev_dbg(dev, "[%d]%s chain %u\n", i, idn,
				   e->chain_index);
			break;
		case FLOW_ACTION_REDIRECT:
		case FLOW_ACTION_REDIRECT_INGRESS:
		case FLOW_ACTION_MIRRED:
		case FLOW_ACTION_MIRRED_INGRESS:
			netdev_dbg(dev, "[%d]%s dev %s\n", i, idn, e->dev->name);
			break;
		case FLOW_ACTION_VLAN_PUSH:
		case FLOW_ACTION_VLAN_POP:
		case FLOW_ACTION_VLAN_MANGLE:
			netdev_dbg(dev, "[%d]%s proto 0x%04x vid %u pcp %u\n",
				   i, idn, be16_to_cpu(e->vlan.proto),
				   e->vlan.vid, e->vlan.prio);
			break;
		case FLOW_ACTION_MANGLE:
		case FLOW_ACTION_ADD:
			netdev_dbg(dev, "[%d]%s htype %d offset %u mask 0x%08x val 0x%08x\n",
				   i, idn, e->mangle.htype, e->mangle.offset,
				   e->mangle.mask, e->mangle.val);
			break;
		case FLOW_ACTION_MARK:
			netdev_dbg(dev, "[%d]%s mark %u\n", i, idn, e->mark);
			break;
		case FLOW_ACTION_PTYPE:
			netdev_dbg(dev, "[%d]%s ptype %u\n", i, idn, e->ptype);
			break;
		case FLOW_ACTION_PRIORITY:
			netdev_dbg(dev, "[%d]%s prio %u\n", i, idn, e->priority);
			break;
		case FLOW_ACTION_POLICE:
			netdev_dbg(dev, "[%d]%s index %u burst %u rate_bytes_ps %llu mtu %u\n",
				   i, idn, e->hw_index, e->police.burst,
				   e->police.rate_bytes_ps, e->police.mtu);
			break;
		case FLOW_ACTION_GATE:
			netdev_dbg(dev, "[%d]%s index %u prio %d basetime %llu cycletime %llu cycletimeext %llu num_entries %u\n",
				   i, idn, e->hw_index, e->gate.prio,
				   e->gate.basetime, e->gate.cycletime,
				   e->gate.cycletimeext, e->gate.num_entries);
			for (g = 0; g < e->gate.num_entries; g++) {
				netdev_dbg(dev, "[%d,%d] gate_state %u interval %u ipv %d maxoctets %d\n",
					   i, g, e->gate.entries[g].gate_state,
					   e->gate.entries[g].interval,
					   e->gate.entries[g].ipv,
					   e->gate.entries[g].maxoctets);
			}
			break;
		default:
			netdev_dbg(dev, "[%d]%s NOT IMPLEMENTED!\n", i, idn);
		}
	}
}
