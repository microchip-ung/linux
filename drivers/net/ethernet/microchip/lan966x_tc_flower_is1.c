/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include "lan966x_main.h"
#include <net/tc_act/tc_gate.h>


/*******************************************************************************
 * tc chain templates IS1 functional overview
 ******************************************************************************/
/*
  In VCAP IS1 the user can select which key to generate for IPv4, IPv6 and 'other'
  frames in each of the three lookups.

  Selecting the smallest possible key provides the best utilization of the VCAP.

  The X4 S1_7TUPLE key is selected by default and is used for all kind of frames.

  If the user creates a chain template for IPv4, IPv6 or 'other', the
  corresponding list of matches are searched from the beginning.
  Each list is sorted with the smallest key at the beginning and the first match
  returns the smallest possible key including the settings for 'smac', 'dmac_dip'
  and 'inner_tag'.

  If the user creates a template for protocol ALL or RTAG, the list for 'other'
  is searched because this list contains keys that are common and suitable for
  all kind of frames.
  In this case all three kind of key selectors, 'IPv4', 'IPv6' and 'other', are
  set to generate the same key.
*/

/**
 * lan966x_tc_flower_is1_proto_to_frame_type - Convert proto to frame_type
 * @proto: protocol
 *
 * Returns:
 * frame_type
 */
static enum lan966x_vcap_is1_frame_type
	lan966x_tc_flower_is1_proto_to_frame_type(u16 proto)
{
	switch (proto) {
	case ETH_P_IP:
		return LAN966X_VCAP_IS1_FRAME_TYPE_IPV4;
	case ETH_P_IPV6:
		return LAN966X_VCAP_IS1_FRAME_TYPE_IPV6;
	case ETH_P_ALL:
	case ETH_P_RTAG:
		return LAN966X_VCAP_IS1_FRAME_TYPE_ALL;
	default:
		return LAN966X_VCAP_IS1_FRAME_TYPE_OTHER;
	}
}

/**
 * struct lan966x_tc_flower_is1_match:
 * @match_ids: mask of supported match ids
 * @key: key that supports match ids
 * @smac: value of smac for supported match ids
 * @dmac_dip: value of dmac_dip for supported match ids
 */
struct lan966x_tc_flower_is1_match {
	unsigned int match_ids;
	enum lan966x_vcap_is1_key key;
	bool smac;
	bool dmac_dip;
};

/* Superset of supported dissectors for IS1: */
static const unsigned int lan966x_vcap_is1_dissector_all =
	(BIT(FLOW_DISSECTOR_KEY_CONTROL) |
	 BIT(FLOW_DISSECTOR_KEY_BASIC) |
	 BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
	 BIT(FLOW_DISSECTOR_KEY_VLAN) |
	 BIT(FLOW_DISSECTOR_KEY_CVLAN) |
	 BIT(FLOW_DISSECTOR_KEY_IP) |
	 BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
	 BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
	 BIT(FLOW_DISSECTOR_KEY_PORTS));

/* Superset of supported match ids for IS1: */
static const unsigned int lan966x_vcap_is1_match_id_all =
	(BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP4) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP4) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP6) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT));

/* Array of is1 ipv4 matches with smallest key first */
static const struct lan966x_tc_flower_is1_match lan966x_tc_flower_is1_match_ipv4[] = {
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DBL_VID, /* X1 */
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DMAC_VID, /* X1 */
	  .smac = false,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DMAC_VID, /* X1 */
	  .smac = true,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP4) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP4)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4, /* X2 */
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP4) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL, /* X2 */
	  .dmac_dip = false,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP4) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL, /* X2 */
	  .dmac_dip = true,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP4) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP4) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_7TUPLE, /* X4 */
	},
};

/* Find match in 'ipv4' with smallest key */
static const struct lan966x_tc_flower_is1_match *
	lan966x_tc_flower_is1_match_ipv4_get(unsigned int match)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lan966x_tc_flower_is1_match_ipv4); i++) {
		if (match & ~lan966x_tc_flower_is1_match_ipv4[i].match_ids)
			continue;
		return &lan966x_tc_flower_is1_match_ipv4[i];
	}
	return NULL;
}

/* Array of is1 ipv6 matches with smallest key first */
static const struct lan966x_tc_flower_is1_match lan966x_tc_flower_is1_match_ipv6[] = {
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DBL_VID, /* X1 */
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DMAC_VID, /* X1 */
	  .smac = false,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DMAC_VID, /* X1 */
	  .smac = true,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4, /* X2 */
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL, /* X2 */
	  .dmac_dip = false,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL, /* X2 */
	  .dmac_dip = true,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6, /* X4 */
	  .dmac_dip = false,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP6)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6, /* X4 */
	  .dmac_dip = true,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP6)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP6, /* X4 */
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_7TUPLE, /* X4 */
	},
};

/* Find match in 'ipv6' with smallest key */
static const struct lan966x_tc_flower_is1_match *
	lan966x_tc_flower_is1_match_ipv6_get(unsigned int match)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lan966x_tc_flower_is1_match_ipv6); i++) {
		if (match & ~lan966x_tc_flower_is1_match_ipv6[i].match_ids)
			continue;
		return &lan966x_tc_flower_is1_match_ipv6[i];
	}
	return NULL;
}

/* Array of is1 other matches with smallest key first */
static const struct lan966x_tc_flower_is1_match lan966x_tc_flower_is1_match_other[] = {
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN)  |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_ETYPE)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DBL_VID, /* X1 */
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DMAC_VID, /* X1 */
	  .smac = false,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_DMAC_VID, /* X1 */
	  .smac = true,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_ETYPE)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL, /* X2 */
	  .dmac_dip = false,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_ETYPE)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_NORMAL, /* X2 */
	  .dmac_dip = true,
	},
	{ .match_ids = (BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
			BIT(LAN966X_TC_FLOWER_MATCH_ID_ETYPE)),
	  .key = LAN966X_VCAP_IS1_KEY_S1_7TUPLE, /* X4 */
	},
};

/* Find match in 'other' with smallest key */
static const struct lan966x_tc_flower_is1_match *
	lan966x_tc_flower_is1_match_other_get(unsigned int match)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lan966x_tc_flower_is1_match_other); i++) {
		if (match & ~lan966x_tc_flower_is1_match_other[i].match_ids)
			continue;
		return &lan966x_tc_flower_is1_match_other[i];
	}
	return NULL;
}

/* Find match with smallest key */
static const struct lan966x_tc_flower_is1_match *
	lan966x_tc_flower_is1_match_get(
		enum lan966x_vcap_is1_frame_type frame_type,
		unsigned int match)
{
	switch (frame_type) {
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV4:
		return lan966x_tc_flower_is1_match_ipv4_get(match);
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV6:
		return lan966x_tc_flower_is1_match_ipv6_get(match);
	default:
		if (frame_type == LAN966X_VCAP_IS1_FRAME_TYPE_OTHER)
			/* Restrict to keys that can match ETYPE */
			match |= BIT(LAN966X_TC_FLOWER_MATCH_ID_ETYPE);

		return lan966x_tc_flower_is1_match_other_get(match);
	}
}

/* Supported dissectors for IS1 key S1_7TUPLE: */
static const unsigned int lan966x_vcap_is1_dissectors_s1_7tuple =
	(BIT(FLOW_DISSECTOR_KEY_CONTROL) |
	 BIT(FLOW_DISSECTOR_KEY_BASIC) |
	 BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
	 BIT(FLOW_DISSECTOR_KEY_VLAN) |
	 BIT(FLOW_DISSECTOR_KEY_CVLAN) |
	 BIT(FLOW_DISSECTOR_KEY_IP) |
	 BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
	 BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
	 BIT(FLOW_DISSECTOR_KEY_PORTS));

/* Supported match ids for IS1 key S1_7TUPLE with ipv4 frames: */
static const unsigned int lan966x_vcap_is1_match_id_s1_7tuple_ipv4 =
	(BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FRAGMENT) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_IS_FIRST_FRAGMENT) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP4) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP4) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT));

/* Supported match ids for IS1 key S1_7TUPLE with ipv6 frames:
 * Note that only the X marked part of SIP6 and DIP6 can be matched:
 * XXXX:0000:0000:0000:XXXX:XXXX:XXXX:XXXX
 * Matching full IPv6 addresses requires a chain template.
 */
static const unsigned int lan966x_vcap_is1_match_id_s1_7tuple_ipv6 =
	(BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_TOS) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_IP_PROTO) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SIP6) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DIP6) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SPORT) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DPORT));

/* Supported match ids for IS1 key S1_7TUPLE with 'other' frames: */
static const unsigned int lan966x_vcap_is1_match_id_s1_7tuple_other =
	(BIT(LAN966X_TC_FLOWER_MATCH_ID_VLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_CVLAN) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_SMAC) |
	 BIT(LAN966X_TC_FLOWER_MATCH_ID_DMAC));

/**
 * Check dissectors and match ids for S1_7TUPLE/frame type
 * @frame_type: Frame type to check against.
 * @f: Offload info.
 *
 * Returns:
 * 0 if ok.
 * Negative error code on failure.
 */
static int lan966x_tc_flower_is1_match_id_s1_7tuple_check(
	enum lan966x_vcap_is1_frame_type frame_type,
	struct flow_cls_offload *f)
{
	unsigned int match_ids;

	switch (frame_type) {
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV4:
		match_ids = lan966x_vcap_is1_match_id_s1_7tuple_ipv4;
		break;
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV6:
		match_ids = lan966x_vcap_is1_match_id_s1_7tuple_ipv6;
		break;
	default:
		match_ids = lan966x_vcap_is1_match_id_s1_7tuple_other;
		break;
	}

	return lan966x_tc_flower_match_info_get(f,
						lan966x_vcap_is1_dissectors_s1_7tuple,
						match_ids,
						NULL);
}

int lan966x_tc_flower_is1_tmplt_create(struct lan966x_port *port,
				       const struct lan966x_tc_ci *ci,
				       const struct lan966x_tc_flower_proto *p,
				       struct flow_cls_offload *f)
{
	enum lan966x_vcap_is1_frame_type frame_type;
	const struct lan966x_tc_flower_is1_match *match;
	unsigned int match_ids;
	int err;

	netdev_dbg(port->dev, "vcap %d\n", ci->vcap);

	err = lan966x_tc_flower_match_info_get(f,
					       lan966x_vcap_is1_dissector_all,
					       lan966x_vcap_is1_match_id_all,
					       &match_ids);
	if (err)
		return err;

	frame_type = lan966x_tc_flower_is1_proto_to_frame_type(p->l3);
	netdev_dbg(port->dev, "proto 0x%04x frame_type %d\n", p->l3, frame_type);

	match = lan966x_tc_flower_is1_match_get(frame_type, match_ids);
	if (!match)
		return -EINVAL;

	err = lan966x_vcap_is1_port_key_set(port,
					    ci->lookup,
					    frame_type,
					    match->key);
	if (err)
		return err;

	err = lan966x_vcap_is1_port_smac_set(port,
					     ci->lookup,
					     match->smac);
	if (err)
		return err;

	return lan966x_vcap_is1_port_dmac_dip_set(port,
						  ci->lookup,
						  match->dmac_dip);
}

int lan966x_tc_flower_is1_tmplt_destroy(struct lan966x_port *port,
					const struct lan966x_tc_ci *ci)
{
	int err;

	netdev_dbg(port->dev, "vcap %d\n", ci->vcap);

	err = lan966x_vcap_is1_port_key_set(port,
					    ci->lookup,
					    LAN966X_VCAP_IS1_FRAME_TYPE_ALL,
					    LAN966X_VCAP_IS1_KEY_S1_7TUPLE);
	if (err)
		return err;

	err = lan966x_vcap_is1_port_smac_set(port,
					     ci->lookup,
					     false);
	if (err)
		return err;

	return lan966x_vcap_is1_port_dmac_dip_set(port,
						  ci->lookup,
						  false);
}

/**
 * lan966x_tc_flower_is1_action - Check and parse TC IS1 action S1
 * @ci: Chain info
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if action is invalid.
 * -EOPNOTSUPP if action is unsupported.
 */
static int lan966x_tc_flower_is1_action(const struct lan966x_port *port,
					const struct lan966x_tc_ci *ci,
					struct flow_cls_offload *f,
					struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_rule *is1 = &r->is1;
	struct flow_action *action = &rule->action;
	struct lan966x_tc_policer pol = { 0 };
	struct lan966x_vcap_is1_action_s1 *s1;
	struct lan966x_psfp_sg_cfg sg = { 0 };
	struct flow_action_entry *act;
	u32 pol_ix, sgi_ix;
	u32 max_sdu = 0;
	int err, a, i;
	u64 rate;

	err = lan966x_tc_flower_action_check(ci, f, NULL);
	if (err)
		return err;

	is1->action.action = LAN966X_VCAP_IS1_ACTION_S1;
	s1 = &is1->action.s1;

	flow_action_for_each(a, act, action) {
		switch (act->id) {
		case FLOW_ACTION_ACCEPT:
			break;
		case FLOW_ACTION_VLAN_MANGLE:
			if (be16_to_cpu(act->vlan.proto) != ETH_P_8021Q) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid vlan proto");
				return -EINVAL;
			}
			s1->vid_replace_ena = 1;
			s1->vid_add_val = act->vlan.vid;
			s1->pcp_ena = 1;
			s1->pcp_val = act->vlan.prio;
			break;
		case FLOW_ACTION_PRIORITY:
			if (act->priority > 7) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Invalid skbedit priority");
				return -EINVAL;
			}
			s1->qos_ena = 1;
			s1->qos_val = act->priority;
			break;
		case FLOW_ACTION_POLICE:
			if (!r->sfi) {
				err = lan966x_sfi_ix_reserve(port->lan966x,
							     &r->sfi_ix);
				if (err < 0) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Cannot reserve stream filter");
					return err;
				}
				r->sfi = true;/* Mark stream filter as in-use */
			}

			err = lan966x_pol_ix_reserve(port->lan966x,
						     LAN966X_RES_POOL_USER_IS1,
						     act->hw_index,
						     &pol_ix);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot reserve policer");
				return err;
			}

			/* Save reserved policer in rule. This is used to
			 * release the policer when the rule is deleted */
			r->pol_user = LAN966X_RES_POOL_USER_IS1;
			r->pol_id = act->hw_index;

			s1->police_ena = 1;
			s1->police_idx = pol_ix;

			rate = act->police.rate_bytes_ps;
			pol.rate = div_u64(rate, 1000) * 8;
			pol.burst = act->police.burst;
			max_sdu = act->police.mtu; /* Use mtu for stream filter max_sdu */
			err = lan966x_tc_policer_set(port->lan966x, pol_ix, &pol);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set policer");
				return err;
			}
			break;
		case FLOW_ACTION_GATE:
			if (act->hw_index == LAN966X_TC_AOSG) {
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

			for (i = 0; i < act->gate.num_entries; i++) {
				if ((act->gate.entries[i].interval < LAN966X_PSFP_SG_MIN_CYCLE_TIME_NS) ||
				    (act->gate.entries[i].interval > LAN966X_PSFP_SG_MAX_CYCLE_TIME_NS)) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Invalid interval");
					return -EINVAL;
				}
				if ((act->gate.entries[i].ipv < -1) ||
				    (act->gate.entries[i].ipv > LAN966X_PSFP_SG_MAX_IPV)) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Invalid internal priority");
					return -EINVAL;
				}
				if (act->gate.entries[i].maxoctets < -1) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Invalid max octets");
					return -EINVAL;
				}

				sg.gce[i].gate_state = (act->gate.entries[i].gate_state != 0);
				sg.gce[i].interval = act->gate.entries[i].interval;
				sg.gce[i].ipv = act->gate.entries[i].ipv;
				sg.gce[i].maxoctets = act->gate.entries[i].maxoctets;
			}

			if (!r->sfi) {
				err = lan966x_sfi_ix_reserve(port->lan966x,
							     &r->sfi_ix);
				if (err < 0) {
					NL_SET_ERR_MSG_MOD(f->common.extack,
							   "Cannot reserve stream filter");
					return err;
				}
				r->sfi = true;/* Mark stream filter as in-use */
			}

			err = lan966x_sgi_ix_reserve(port->lan966x,
						     LAN966X_RES_POOL_USER_IS1,
						     act->hw_index,
						     &sgi_ix);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot reserve stream gate");
				return err;
			}

			/* Save reserved stream gate in rule. This is used to
			 * release the stream gate when the rule is deleted */
			r->sgi_user = LAN966X_RES_POOL_USER_IS1;
			r->sgi_id = act->hw_index;

			s1->sgid_ena = 1;
			s1->sgid_val = sgi_ix;

			err = lan966x_psfp_sg_set(port->lan966x, sgi_ix, &sg);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set stream gate");
				return err;
			}
			break;
		case FLOW_ACTION_GOTO:
			if (ci->pag_offset) { /* Set PAG value */
				s1->pag_override_mask = ~0;
				s1->pag_val = act->chain_index - ci->pag_offset;
			}
			break;
		default:
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TC action");
			return -EOPNOTSUPP;
		}
	}

	if (r->sfi) {
		struct lan966x_psfp_sf_cfg sf = { 0 };
		sf.max_sdu = max_sdu;
		err = lan966x_psfp_sf_set(port->lan966x, r->sfi_ix, &sf);
		if (err < 0) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Cannot set stream filter");
			return err;
		}

		s1->sfid_ena = 1;
		s1->sfid_val = r->sfi_ix;

		/* A stream filter must always have a stream gate. Create an
		 * always open stream gate in case user hasn't specified one */
		if (r->sgi_user == LAN966X_RES_POOL_FREE) {
			err = lan966x_sgi_ix_reserve(port->lan966x,
						     LAN966X_RES_POOL_USER_IS1,
						     LAN966X_TC_AOSG,
						     &sgi_ix);
			if (err < 0) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot reserve stream gate");
				return err;
			}

			/* Save reserved stream gate in rule. This is used to
			 * release the stream gate when the rule is deleted */
			r->sgi_user = LAN966X_RES_POOL_USER_IS1;
			r->sgi_id = LAN966X_TC_AOSG;

			s1->sgid_ena = 1;
			s1->sgid_val = sgi_ix;

			/* Setup the always open stream gate */
			sg.gate_state = true;
			sg.ipv = -1;
			sg.cycletime = 1000000000; // 1 sec
			sg.num_entries = 1;
			sg.gce[0].gate_state = true;
			sg.gce[0].interval = 1000000000;
			sg.gce[0].ipv = -1;
			sg.gce[0].maxoctets = -1;
			err = lan966x_psfp_sg_set(port->lan966x, sgi_ix, &sg);
			if (err) {
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot set always open stream gate");
				return err;
			}
		}
	}
	return 0;
}

/**
 * lan966x_tc_flower_is1_key_s1_normal - Check and parse key S1_NORMAL
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is1_key_s1_normal(const struct lan966x_port *port,
					       const struct lan966x_tc_ci *ci,
					       const struct lan966x_tc_flower_proto *p,
					       struct flow_cls_offload *f,
					       struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_key_s1_normal *key;
	struct lan966x_vcap_is1_rule *is1 = &r->is1;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is1->key.key = LAN966X_VCAP_IS1_KEY_S1_NORMAL;

	key = &is1->key.s1_normal;
	key->lookup.value = ci->lookup;
	key->lookup.mask = ~0;
	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);;

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

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		/* The template ensures that either SMAC or DMAC is present */
		if (!is_zero_ether_addr(match.mask->dst)) {
			ether_addr_copy(key->l2_smac.value,
					match.key->dst);
			ether_addr_copy(key->l2_smac.mask,
					match.mask->dst);
		} else if (!is_zero_ether_addr(match.mask->src)) {
			ether_addr_copy(key->l2_smac.value,
					match.key->src);
			ether_addr_copy(key->l2_smac.mask,
					match.mask->src);
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_vlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}
		key->vlan_tagged = LAN966X_VCAP_BIT_1;
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		key->l3_dscp.value = match.key->tos;
		key->l3_dscp.mask = match.mask->tos;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		/* The template ensures that either SIP or DIP is present */
		if (match.mask->src) {
			key->l3_ip4_sip.value = be32_to_cpu(match.key->src);
			key->l3_ip4_sip.mask = be32_to_cpu(match.mask->src);
		} else if (match.mask->dst) {
			key->l3_ip4_sip.value = be32_to_cpu(match.key->dst);
			key->l3_ip4_sip.mask = be32_to_cpu(match.mask->dst);
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* The template ensures that either SIP6 or DIP6 is present */
		if (!ipv6_addr_any(&match.key->src)) {
			/* Match the 32 least significant bits in the IPv6 SIP:
			 * 0000:0000:0000:0000:0000:0000:XXXX:XXXX */
			key->l3_ip4_sip.value =
				(match.key->src.s6_addr[12] << 24) +
				(match.key->src.s6_addr[13] << 16) +
				(match.key->src.s6_addr[14] << 8) +
				match.key->src.s6_addr[15];

			key->l3_ip4_sip.mask =
				(match.mask->src.s6_addr[12] << 24) +
				(match.mask->src.s6_addr[13] << 16) +
				(match.mask->src.s6_addr[14] << 8) +
				match.mask->src.s6_addr[15];
		} else if (!ipv6_addr_any(&match.key->dst)) {
			/* Match the 32 least significant bits in the IPv6 DIP:
			 * 0000:0000:0000:0000:0000:0000:XXXX:XXXX */
			key->l3_ip4_sip.value =
				(match.key->dst.s6_addr[12] << 24) +
				(match.key->dst.s6_addr[13] << 16) +
				(match.key->dst.s6_addr[14] << 8) +
				match.key->dst.s6_addr[15];

			key->l3_ip4_sip.mask =
				(match.mask->dst.s6_addr[12] << 24) +
				(match.mask->dst.s6_addr[13] << 16) +
				(match.mask->dst.s6_addr[14] << 8) +
				match.mask->dst.s6_addr[15];
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		key->l4_sport.value = be16_to_cpu(match.key->src);
		key->l4_sport.mask = be16_to_cpu(match.mask->src);
		key->etype.value = be16_to_cpu(match.key->dst);
		key->etype.mask = be16_to_cpu(match.mask->dst);
	}

	if (p->l4) {
		if (p->l4 == IPPROTO_TCP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_1;
		} else if (p->l4 == IPPROTO_UDP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_0;
		} else {
			key->tcp_udp = LAN966X_VCAP_BIT_0;
			key->tcp = LAN966X_VCAP_BIT_0;
			key->etype.value = p->l4;
			key->etype.mask = ~0;
		}
	}

	switch (p->l3) {
	case ETH_P_ALL:
		break;
	case ETH_P_IP:
		key->ip4 = LAN966X_VCAP_BIT_1;
		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	case ETH_P_IPV6:
		key->ip4 = LAN966X_VCAP_BIT_0;
		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	case ETH_P_802_2:
		key->etype_len = LAN966X_VCAP_BIT_0;
		key->ip_snap = LAN966X_VCAP_BIT_0;
		break;
	case ETH_P_SNAP:
		key->etype_len = LAN966X_VCAP_BIT_0;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	default:
		if (p->l3 < ETH_P_802_3_MIN) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported protocol");
			return -EOPNOTSUPP;
		}

		if (p->l3 == ETH_P_RTAG)
			key->r_tagged = LAN966X_VCAP_BIT_1;

		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_0;
		key->etype.value = p->l3;
		key->etype.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is1_key_s1_5tuple_ip4 - Check and parse key S1_5TUPLE_IP4
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is1_key_s1_5tuple_ip4(const struct lan966x_port *port,
						   const struct lan966x_tc_ci *ci,
						   const struct lan966x_tc_flower_proto *p,
						   struct flow_cls_offload *f,
						   struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_key_s1_5tuple_ip4 *key;
	struct lan966x_vcap_is1_rule *is1 = &r->is1;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is1->key.key = LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4;

	key = &is1->key.s1_5tuple_ip4;
	key->lookup.value = ci->lookup;
	key->lookup.mask = ~0;
	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);;

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

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_vlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}
		key->vlan_tagged = LAN966X_VCAP_BIT_1;
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_cvlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->inner_tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->inner_tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}

		key->vlan_dbl_tagged = LAN966X_VCAP_BIT_1;
		key->inner_vid.value = match.key->vlan_id;
		key->inner_vid.mask = match.mask->vlan_id;
		key->inner_pcp.value = match.key->vlan_priority;
		key->inner_pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		key->l3_dscp.value = match.key->tos;
		key->l3_dscp.mask = match.mask->tos;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		key->l3_ip4_sip.value = be32_to_cpu(match.key->src);
		key->l3_ip4_sip.mask = be32_to_cpu(match.mask->src);
		key->l3_ip4_dip.value = be32_to_cpu(match.key->dst);
		key->l3_ip4_dip.mask = be32_to_cpu(match.mask->dst);
	}

	if (p->l4) {
		if (p->l4 == IPPROTO_TCP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_1;
		} else if (p->l4 == IPPROTO_UDP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_0;
		} else {
			key->tcp_udp = LAN966X_VCAP_BIT_0;
			key->tcp = LAN966X_VCAP_BIT_0;
		}
		key->l3_ip_proto.value = p->l4;
		key->l3_ip_proto.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is1_key_s1_normal_ip6 - Check and parse key S1_NORMAL_IP6
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is1_key_s1_normal_ip6(const struct lan966x_port *port,
						   const struct lan966x_tc_ci *ci,
						   const struct lan966x_tc_flower_proto *p,
						   struct flow_cls_offload *f,
						   struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_key_s1_normal_ip6 *key;
	struct lan966x_vcap_is1_rule *is1 = &r->is1;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is1->key.key = LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6;

	key = &is1->key.s1_normal_ip6;
	key->lookup.value = ci->lookup;
	key->lookup.mask = ~0;
	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		/* The template ensures that either SMAC or DMAC is present */
		if (!is_zero_ether_addr(match.mask->dst)) {
			ether_addr_copy(key->l2_smac.value,
					match.key->dst);
			ether_addr_copy(key->l2_smac.mask,
					match.mask->dst);
		} else if (!is_zero_ether_addr(match.mask->src)) {
			ether_addr_copy(key->l2_smac.value,
					match.key->src);
			ether_addr_copy(key->l2_smac.mask,
					match.mask->src);
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_vlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}
		key->vlan_tagged = LAN966X_VCAP_BIT_1;
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_cvlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->inner_tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->inner_tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}

		key->vlan_dbl_tagged = LAN966X_VCAP_BIT_1;
		key->inner_vid.value = match.key->vlan_id;
		key->inner_vid.mask = match.mask->vlan_id;
		key->inner_pcp.value = match.key->vlan_priority;
		key->inner_pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		key->l3_dscp.value = match.key->tos;
		key->l3_dscp.mask = match.mask->tos;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		int i;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* The template ensures that either SIP6 or DIP6 is present */
		if (!ipv6_addr_any(&match.key->src)) {
			for (i = 0; i < 16; i++) {
				key->l3_ip6_sip.value[i] = match.key->src.s6_addr[i];
				key->l3_ip6_sip.mask[i] = match.mask->src.s6_addr[i];
			}
		} else if (!ipv6_addr_any(&match.key->dst)) {
			for (i = 0; i < 16; i++) {
				key->l3_ip6_sip.value[i] = match.key->dst.s6_addr[i];
				key->l3_ip6_sip.mask[i] = match.mask->dst.s6_addr[i];
			}
		}
	}

	if (p->l4) {
		if (p->tcp_udp) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
		} else {
			key->tcp_udp = LAN966X_VCAP_BIT_0;
		}
		key->l3_ip_proto.value = p->l4;
		key->l3_ip_proto.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is1_key_s1_7tuple - Check and parse key S1_7TUPLE
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is1_key_s1_7tuple(const struct lan966x_port *port,
					       const struct lan966x_tc_ci *ci,
					       const struct lan966x_tc_flower_proto *p,
					       struct flow_cls_offload *f,
					       struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_key_s1_7tuple *key;
	struct lan966x_vcap_is1_rule *is1 = &r->is1;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is1->key.key = LAN966X_VCAP_IS1_KEY_S1_7TUPLE;

	key = &is1->key.s1_7tuple;
	key->lookup.value = ci->lookup;
	key->lookup.mask = ~0;
	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);;

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

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);
		ether_addr_copy(key->l2_dmac.value,
				match.key->dst);
		ether_addr_copy(key->l2_dmac.mask,
				match.mask->dst);
		ether_addr_copy(key->l2_smac.value,
				match.key->src);
		ether_addr_copy(key->l2_smac.mask,
				match.mask->src);
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_vlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}

		key->vlan_tagged = LAN966X_VCAP_BIT_1;
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_cvlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->inner_tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->inner_tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}

		key->vlan_dbl_tagged = LAN966X_VCAP_BIT_1;
		key->inner_vid.value = match.key->vlan_id;
		key->inner_vid.mask = match.mask->vlan_id;
		key->inner_pcp.value = match.key->vlan_priority;
		key->inner_pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		key->l3_dscp.value = match.key->tos;
		key->l3_dscp.mask = match.mask->tos;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;
		u32 val;

		flow_rule_match_ipv4_addrs(rule, &match);

		val = be32_to_cpu(match.key->src);
		key->l3_ip6_sip.value[4] = (val >> 24) & 0xff;
		key->l3_ip6_sip.value[5] = (val >> 16) & 0xff;
		key->l3_ip6_sip.value[6] = (val >> 8) & 0xff;
		key->l3_ip6_sip.value[7] = val & 0xff;

		val = be32_to_cpu(match.mask->src);
		key->l3_ip6_sip.mask[4] = (val >> 24) & 0xff;
		key->l3_ip6_sip.mask[5] = (val >> 16) & 0xff;
		key->l3_ip6_sip.mask[6] = (val >> 8) & 0xff;
		key->l3_ip6_sip.mask[7] = val & 0xff;

		val = be32_to_cpu(match.key->dst);
		key->l3_ip6_dip.value[4] = (val >> 24) & 0xff;
		key->l3_ip6_dip.value[5] = (val >> 16) & 0xff;
		key->l3_ip6_dip.value[6] = (val >> 8) & 0xff;
		key->l3_ip6_dip.value[7] = val & 0xff;

		val = be32_to_cpu(match.mask->dst);
		key->l3_ip6_dip.mask[4] = (val >> 24) & 0xff;
		key->l3_ip6_dip.mask[5] = (val >> 16) & 0xff;
		key->l3_ip6_dip.mask[6] = (val >> 8) & 0xff;
		key->l3_ip6_dip.mask[7] = val & 0xff;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;
		int i;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* Match the 16 most significant bits in the IPv6 addresses:
		 * XXXX:0000:0000:0000:0000:0000:0000:0000 */
		key->l3_ip6_sip_msb.value = (match.key->src.s6_addr[0] << 8) +
			match.key->src.s6_addr[1];
		key->l3_ip6_sip_msb.mask = (match.mask->src.s6_addr[0] << 8) +
			match.mask->src.s6_addr[1];
		key->l3_ip6_dip_msb.value = (match.key->dst.s6_addr[0] << 8) +
			match.key->dst.s6_addr[1];
		key->l3_ip6_dip_msb.mask = (match.mask->dst.s6_addr[0] << 8) +
			match.mask->dst.s6_addr[1];

		/* Match the 64 least significant bits in the IPv6 addresses:
		 * 0000:0000:0000:0000:XXXX:XXXX:XXXX:XXXX */
		for (i = 0; i < 8; i++) {
			key->l3_ip6_sip.value[i] = match.key->src.s6_addr[i + 8];
			key->l3_ip6_sip.mask[i] = match.mask->src.s6_addr[i + 8];
			key->l3_ip6_dip.value[i] = match.key->dst.s6_addr[i + 8];
			key->l3_ip6_dip.mask[i] = match.mask->dst.s6_addr[i + 8];
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		key->l4_sport.value = be16_to_cpu(match.key->src);
		key->l4_sport.mask = be16_to_cpu(match.mask->src);
		key->etype.value = be16_to_cpu(match.key->dst);
		key->etype.mask = be16_to_cpu(match.mask->dst);
	}

	if (p->l4) {
		if (p->l4 == IPPROTO_TCP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_1;
		} else if (p->l4 == IPPROTO_UDP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_0;
		} else {
			key->tcp_udp = LAN966X_VCAP_BIT_0;
			key->tcp = LAN966X_VCAP_BIT_0;
			key->etype.value = p->l4;
			key->etype.mask = ~0;
		}
	}

	switch (p->l3) {
	case ETH_P_ALL:
		break;
	case ETH_P_IP:
		key->ip4 = LAN966X_VCAP_BIT_1;
		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	case ETH_P_IPV6:
		key->ip4 = LAN966X_VCAP_BIT_0;
		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	case ETH_P_802_2:
		key->etype_len = LAN966X_VCAP_BIT_0;
		key->ip_snap = LAN966X_VCAP_BIT_0;
		break;
	case ETH_P_SNAP:
		key->etype_len = LAN966X_VCAP_BIT_0;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	default:
		if (p->l3 < ETH_P_802_3_MIN) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported protocol");
			return -EOPNOTSUPP;
		}

		if (p->l3 == ETH_P_RTAG)
			key->r_tagged = LAN966X_VCAP_BIT_1;

		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_0;
		key->etype.value = p->l3;
		key->etype.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is1_key_s1_5tuple_ip6 - Check and parse key S1_5TUPLE_IP6
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is1_key_s1_5tuple_ip6(const struct lan966x_port *port,
						   const struct lan966x_tc_ci *ci,
						   const struct lan966x_tc_flower_proto *p,
						   struct flow_cls_offload *f,
						   struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_key_s1_5tuple_ip6 *key;
	struct lan966x_vcap_is1_rule *is1 = &r->is1;
	u16 addr_type = 0;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is1->key.key = LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP6;

	key = &is1->key.s1_5tuple_ip6;
	key->lookup.value = ci->lookup;
	key->lookup.mask = ~0;
	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_vlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}
		key->vlan_tagged = LAN966X_VCAP_BIT_1;
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_cvlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->inner_tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->inner_tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}

		key->vlan_dbl_tagged = LAN966X_VCAP_BIT_1;
		key->inner_vid.value = match.key->vlan_id;
		key->inner_vid.mask = match.mask->vlan_id;
		key->inner_pcp.value = match.key->vlan_priority;
		key->inner_pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		key->l3_dscp.value = match.key->tos;
		key->l3_dscp.mask = match.mask->tos;
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

	if (p->l4) {
		if (p->tcp_udp) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
		} else {
			key->tcp_udp = LAN966X_VCAP_BIT_0;
		}
		key->l3_ip_proto.value = p->l4;
		key->l3_ip_proto.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is1_key_s1_dbl_vid - Check and parse key S1_DBL_VID
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is1_key_s1_dbl_vid(const struct lan966x_port *port,
						const struct lan966x_tc_ci *ci,
						const struct lan966x_tc_flower_proto *p,
						struct flow_cls_offload *f,
						struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_key_s1_dbl_vid *key;
	struct lan966x_vcap_is1_rule *is1 = &r->is1;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is1->key.key = LAN966X_VCAP_IS1_KEY_S1_DBL_VID;

	key = &is1->key.s1_dbl_vid;
	key->lookup.value = ci->lookup;
	key->lookup.mask = ~0;
	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);

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

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_vlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}
		key->vlan_tagged = LAN966X_VCAP_BIT_1;
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_cvlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->inner_tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->inner_tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}

		key->vlan_dbl_tagged = LAN966X_VCAP_BIT_1;
		key->inner_vid.value = match.key->vlan_id;
		key->inner_vid.mask = match.mask->vlan_id;
		key->inner_pcp.value = match.key->vlan_priority;
		key->inner_pcp.mask = match.mask->vlan_priority;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);

		key->l3_dscp.value = match.key->tos;
		key->l3_dscp.mask = match.mask->tos;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		key->etype.value = be16_to_cpu(match.key->dst);
		key->etype.mask = be16_to_cpu(match.mask->dst);
	}

	if (p->l4) {
		if (p->l4 == IPPROTO_TCP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_1;
		} else if (p->l4 == IPPROTO_UDP) {
			key->tcp_udp = LAN966X_VCAP_BIT_1;
			key->tcp = LAN966X_VCAP_BIT_0;
		} else {
			key->tcp_udp = LAN966X_VCAP_BIT_0;
			key->tcp = LAN966X_VCAP_BIT_0;
			key->etype.value = p->l4;
			key->etype.mask = ~0;
		}
	}

	switch (p->l3) {
	case ETH_P_ALL:
		break;
	case ETH_P_IP:
		key->ip4 = LAN966X_VCAP_BIT_1;
		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	case ETH_P_IPV6:
		key->ip4 = LAN966X_VCAP_BIT_0;
		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	case ETH_P_802_2:
		key->etype_len = LAN966X_VCAP_BIT_0;
		key->ip_snap = LAN966X_VCAP_BIT_0;
		break;
	case ETH_P_SNAP:
		key->etype_len = LAN966X_VCAP_BIT_0;
		key->ip_snap = LAN966X_VCAP_BIT_1;
		break;
	default:
		if (p->l3 < ETH_P_802_3_MIN) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported protocol");
			return -EOPNOTSUPP;
		}

		if (p->l3 == ETH_P_RTAG)
			key->r_tagged = LAN966X_VCAP_BIT_1;

		key->etype_len = LAN966X_VCAP_BIT_1;
		key->ip_snap = LAN966X_VCAP_BIT_0;
		key->etype.value = p->l3;
		key->etype.mask = ~0;
	}

	return 0;
}

/**
 * lan966x_tc_flower_is1_key_s1_dmac_vid - Check and parse key S1_DMAC_VID
 * @port: The interface
 * @ci: Chain info
 * @p: Protocol
 * @f: Offload info
 * @r: IS1 rule
 *
 * Returns:
 * 0 if ok
 * -EINVAL if rule is invalid.
 * -EOPNOTSUPP if rule is unsupported.
 */
static int lan966x_tc_flower_is1_key_s1_dmac_vid(const struct lan966x_port *port,
						 const struct lan966x_tc_ci *ci,
						 const struct lan966x_tc_flower_proto *p,
						 struct flow_cls_offload *f,
						 struct lan966x_vcap_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct lan966x_vcap_is1_key_s1_dmac_vid *key;
	struct lan966x_vcap_is1_rule *is1 = &r->is1;

	netdev_dbg(port->dev, "proto 0x%04x\n", p->l3);
	is1->key.key = LAN966X_VCAP_IS1_KEY_S1_DMAC_VID;

	key = &is1->key.s1_dmac_vid;
	key->lookup.value = ci->lookup;
	key->lookup.mask = ~0;
	/* wild-card the port by setting the bit in mask to zero */
	key->igr_port_mask.mask = ~BIT(port->chip_port);;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		/* The template ensures that either SMAC or DMAC is present */
		if (!is_zero_ether_addr(match.mask->dst)) {
			ether_addr_copy(key->l2_dmac.value,
					match.key->dst);
			ether_addr_copy(key->l2_dmac.mask,
					match.mask->dst);
		} else if (!is_zero_ether_addr(match.mask->src)) {
			ether_addr_copy(key->l2_dmac.value,
					match.key->src);
			ether_addr_copy(key->l2_dmac.mask,
					match.mask->src);
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;
		u16 tpid;

		flow_rule_match_vlan(rule, &match);
		tpid = be16_to_cpu(match.key->vlan_tpid);
		if (tpid == ETH_P_8021Q) {
			key->tpid = LAN966X_VCAP_BIT_0;
		} else if (tpid == ETH_P_8021AD) {
			key->tpid = LAN966X_VCAP_BIT_1;
		} else {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported TPID");
			return -EOPNOTSUPP;
		}
		key->vlan_tagged = LAN966X_VCAP_BIT_1;
		key->vid.value = match.key->vlan_id;
		key->vid.mask = match.mask->vlan_id;
		key->pcp.value = match.key->vlan_priority;
		key->pcp.mask = match.mask->vlan_priority;
	}

	if (p->l3 == ETH_P_RTAG) {
		key->r_tagged = LAN966X_VCAP_BIT_1;
	}

	return 0;
}

int lan966x_tc_flower_is1_parse(const struct lan966x_port *port,
				const struct lan966x_tc_ci *ci,
				const struct lan966x_tc_flower_proto *p,
				struct flow_cls_offload *f,
				struct lan966x_vcap_rule *r)
{
	enum lan966x_vcap_is1_frame_type frame_type;
	enum lan966x_vcap_is1_key key;
	int err;

	frame_type = lan966x_tc_flower_is1_proto_to_frame_type(p->l3);
	err = lan966x_vcap_is1_port_key_get(port, ci->lookup, frame_type, &key);
	if (err)
		return err;

	/* Explicitly check S1_7TUPLE key. This is the default key and the only
	 * one that can be used without a template.
	 * All other keys are checked when creating the template. */
	if (key == LAN966X_VCAP_IS1_KEY_S1_7TUPLE) {
		err = lan966x_tc_flower_is1_match_id_s1_7tuple_check(frame_type,
								     f);
		if (err) {
			netdev_err(port->dev,
				   "Unsupported matches in flower rule\n");
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Unsupported matches in flower rule");
			return err;
		}
	}

	switch (key) {
	case LAN966X_VCAP_IS1_KEY_S1_NORMAL:
		err = lan966x_tc_flower_is1_key_s1_normal(port, ci, p, f, r);
		break;
	case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4:
		err = lan966x_tc_flower_is1_key_s1_5tuple_ip4(port, ci, p, f, r);
		break;
	case LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6:
		err = lan966x_tc_flower_is1_key_s1_normal_ip6(port, ci, p, f, r);
		break;
	case LAN966X_VCAP_IS1_KEY_S1_7TUPLE:
		err = lan966x_tc_flower_is1_key_s1_7tuple(port, ci, p, f, r);
		break;
	case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP6:
		err = lan966x_tc_flower_is1_key_s1_5tuple_ip6(port, ci, p, f, r);
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DBL_VID:
		err = lan966x_tc_flower_is1_key_s1_dbl_vid(port, ci, p, f, r);
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DMAC_VID:
		err = lan966x_tc_flower_is1_key_s1_dmac_vid(port, ci, p, f, r);
		break;
	default:
		netdev_err(port->dev,  "Unsupported IS1 key %d\n", key);
		NL_SET_ERR_MSG_MOD(f->common.extack, "Unsupported IS1 key");
		return -EOPNOTSUPP;
	}

	if (err)
		return err;

	return lan966x_tc_flower_is1_action(port, ci, f, r);
}
