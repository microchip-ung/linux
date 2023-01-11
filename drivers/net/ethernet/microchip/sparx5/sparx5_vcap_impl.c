// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver VCAP Library
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 * The Sparx5 Chip Register Model can be browsed at this location:
 * https://github.com/microchip-ung/sparx-5_reginfo
 */

#include <linux/types.h>
#include <linux/list.h>

#include "vcap_api.h"
#include "vcap_api_client.h"
#include "vcap_api_debugfs.h"
#include "vcap_netlink.h"
#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include "sparx5_vcap_impl.h"
#include "sparx5_debugfs.h"
#include "vcap_api_debugfs.h"

extern const struct vcap_info sparx5_vcaps[];
extern const struct vcap_statistics sparx5_vcap_stats;

#define SUPER_VCAP_BLK_SIZE 3072 /* addresses per Super VCAP block */

#define STREAMSIZE (64 * 4)

#define SPARX5_ES0_LOOKUPS 1
#define SPARX5_ES2_LOOKUPS 2
#define SPARX5_IS0_LOOKUPS 6
#define SPARX5_IS2_LOOKUPS 4

#define SPARX5_STAT_ESDX_GRN_PKTS  0x300
#define SPARX5_STAT_ESDX_YEL_PKTS  0x301

/* IS0 Ingress port traffic type classification */
enum vcap_is0_port_traffic_class {
	VCAP_IS0_PTC_ETYPE,
	VCAP_IS0_PTC_IPV4,
	VCAP_IS0_PTC_IPV6,
	VCAP_IS0_PTC_MPLS_UC,
	VCAP_IS0_PTC_MPLS_MC,
	VCAP_IS0_PTC_MPLS_LS,
	VCAP_IS0_PTC_MAX,
};

enum vcap_is0_port_sel_etype {
	VCAP_IS0_PS_ETYPE_DEFAULT,
	VCAP_IS0_PS_ETYPE_MLL,
	VCAP_IS0_PS_ETYPE_SGL_MLBS,
	VCAP_IS0_PS_ETYPE_DBL_MLBS,
	VCAP_IS0_PS_ETYPE_TRI_MLBS,
	VCAP_IS0_PS_ETYPE_TRI_VID,
	VCAP_IS0_PS_ETYPE_LL_FULL,
	VCAP_IS0_PS_ETYPE_NORMAL_SRC,
	VCAP_IS0_PS_ETYPE_NORMAL_DST,
	VCAP_IS0_PS_ETYPE_NORMAL_7TUPLE,
	VCAP_IS0_PS_ETYPE_NORMAL_5TUPLE_IP4,
	VCAP_IS0_PS_ETYPE_PURE_5TUPLE_IP4,
	VCAP_IS0_PS_ETYPE_DBL_VID_IDX,
	VCAP_IS0_PS_ETYPE_ETAG,
	VCAP_IS0_PS_ETYPE_NO_LOOKUP,
};

enum vcap_is0_port_sel_mpls_uc_mc {
	VCAP_IS0_PS_MPLS_UC_MC_FOLLOW_ETYPE,
	VCAP_IS0_PS_MPLS_UC_MC_MLL,
	VCAP_IS0_PS_MPLS_UC_MC_SGL_MLBS,
	VCAP_IS0_PS_MPLS_UC_MC_DBL_MLBS,
	VCAP_IS0_PS_MPLS_UC_MC_TRI_MLBS,
	VCAP_IS0_PS_MPLS_UC_MC_TRI_VID,
	VCAP_IS0_PS_MPLS_UC_MC_LL_FULL,
	VCAP_IS0_PS_MPLS_UC_MC_NORMAL_SRC,
	VCAP_IS0_PS_MPLS_UC_MC_NORMAL_DST,
	VCAP_IS0_PS_MPLS_UC_MC_NORMAL_7TUPLE,
	VCAP_IS0_PS_MPLS_UC_MC_NORMAL_5TUPLE_IP4,
	VCAP_IS0_PS_MPLS_UC_MC_PURE_5TUPLE_IP4,
	VCAP_IS0_PS_MPLS_UC_MC_DBL_VID_IDX,
	VCAP_IS0_PS_MPLS_UC_MC_ETAG,
	VCAP_IS0_PS_MPLS_UC_MC_NO_LOOKUP,
};

enum vcap_is0_port_sel_mpls_ls {
	VCAP_IS0_PS_MPLS_LS_FOLLOW_ETYPE,
	VCAP_IS0_PS_MPLS_LS_SGL_MLBS,
	VCAP_IS0_PS_MPLS_LS_DBL_MLBS,
	VCAP_IS0_PS_MPLS_LS_TRI_MLBS,
	VCAP_IS0_PS_MPLS_LS_NO_LOOKUP = 17,
};

union vcap_is0_port_sel {
	enum vcap_is0_port_sel_etype etype;
	enum vcap_is0_port_sel_mpls_uc_mc mpls;
	enum vcap_is0_port_sel_mpls_ls label;
};

/* IS2 Ingress port traffic type classification */
enum vcap_is2_port_traffic_class {
	VCAP_IS2_PTC_NONETH, /* Also used in place of non-IP traffic */
	VCAP_IS2_PTC_IPV4_UC,
	VCAP_IS2_PTC_IPV4_MC,
	VCAP_IS2_PTC_IPV6_UC,
	VCAP_IS2_PTC_IPV6_MC,
	VCAP_IS2_PTC_ARP,
	VCAP_IS2_PTC_MAX,
};

enum vcap_is2_port_sel_igr {
	VCAP_IS2_PS_L2_INFO_IN_IGR_PORT_MASK,
	VCAP_IS2_PS_L3_INFO_IN_IGR_PORT_MASK,
};

enum vcap_is2_port_sel_noneth {
	VCAP_IS2_PS_NONETH_MAC_ETYPE,
	VCAP_IS2_PS_NONETH_CUSTOM_1,
	VCAP_IS2_PS_NONETH_CUSTOM_2,
	VCAP_IS2_PS_NONETH_NO_LOOKUP
};

enum vcap_is2_port_sel_ipv4_uc {
	VCAP_IS2_PS_IPV4_UC_MAC_ETYPE,
	VCAP_IS2_PS_IPV4_UC_IP4_TCP_UDP_OTHER,
	VCAP_IS2_PS_IPV4_UC_IP_7TUPLE,
};

enum vcap_is2_port_sel_ipv4_mc {
	VCAP_IS2_PS_IPV4_MC_MAC_ETYPE,
	VCAP_IS2_PS_IPV4_MC_IP4_TCP_UDP_OTHER,
	VCAP_IS2_PS_IPV4_MC_IP_7TUPLE,
	VCAP_IS2_PS_IPV4_MC_IP4_VID,
};

enum vcap_is2_port_sel_ipv6_uc {
	VCAP_IS2_PS_IPV6_UC_MAC_ETYPE,
	VCAP_IS2_PS_IPV6_UC_IP_7TUPLE,
	VCAP_IS2_PS_IPV6_UC_IP6_STD,
	VCAP_IS2_PS_IPV6_UC_IP4_TCP_UDP_OTHER,
};

enum vcap_is2_port_sel_ipv6_mc {
	VCAP_IS2_PS_IPV6_MC_MAC_ETYPE,
	VCAP_IS2_PS_IPV6_MC_IP_7TUPLE,
	VCAP_IS2_PS_IPV6_MC_IP6_VID,
	VCAP_IS2_PS_IPV6_MC_IP6_STD,
	VCAP_IS2_PS_IPV6_MC_IP4_TCP_UDP_OTHER,
};

enum vcap_is2_port_sel_arp {
	VCAP_IS2_PS_ARP_MAC_ETYPE,
	VCAP_IS2_PS_ARP_ARP,
};

/* ES0 Egress port traffic type classification */
enum vcap_es0_port_sel {
	VCAP_ES0_PS_NORMAL_SELECTION,
	VCAP_ES0_PS_FORCE_ISDX_LOOKUPS,
	VCAP_ES0_PS_FORCE_VID_LOOKUPS,
	VCAP_ES0_PS_RESERVED,
};

/* ES2 Egress port traffic type classification */
enum vcap_es2_port_traffic_class {
	VCAP_ES2_PTC_IPV4,
	VCAP_ES2_PTC_IPV6,
	VCAP_ES2_PTC_ARP,
	VCAP_ES2_PTC_MAX,
};

enum vcap_es2_port_sel_ipv4 {
	VCAP_ES2_PS_IPV4_MAC_ETYPE,
	VCAP_ES2_PS_IPV4_IP_7TUPLE,
	VCAP_ES2_PS_IPV4_IP4_TCP_UDP_VID,
	VCAP_ES2_PS_IPV4_IP4_TCP_UDP_OTHER,
	VCAP_ES2_PS_IPV4_IP4_VID,
	VCAP_ES2_PS_IPV4_IP4_OTHER,
};

enum vcap_es2_port_sel_ipv6 {
	VCAP_ES2_PS_IPV6_MAC_ETYPE,
	VCAP_ES2_PS_IPV6_IP_7TUPLE,
	VCAP_ES2_PS_IPV6_IP_7TUPLE_VID,
	VCAP_ES2_PS_IPV6_IP_7TUPLE_STD,
	VCAP_ES2_PS_IPV6_IP6_VID,
	VCAP_ES2_PS_IPV6_IP6_STD,
	VCAP_ES2_PS_IPV6_IP4_DOWNGRADE,
};

enum vcap_es2_port_sel_arp {
	VCAP_ES2_PS_ARP_MAC_ETYPE,
	VCAP_ES2_PS_ARP_ARP,
};

static struct sparx5_vcap_inst {
	enum vcap_type vtype; /* type of vcap */
	int vinst; /* instance number within the same type */
	int lookups; /* number of lookups in this vcap type */
	int lookups_per_instance; /* number of lookups in this instance */
	int first_cid; /* first chain id in this vcap */
	int last_cid; /* last chain id in this vcap */
	int count; /* number of available addresses, if not mapped in super vcap */
	int map_id; /* id in the super vcap block mapping (if applicable) */
	int blockno; /* starting block in super vcap (if applicable) */
	int blocks; /* number of blocks in super vcap (if applicable) */
} sparx5_vcap_inst_cfg[] = {
	{
		.vtype = VCAP_TYPE_ES0,
		.lookups = SPARX5_ES0_LOOKUPS,
		.lookups_per_instance = SPARX5_ES0_LOOKUPS,
		.first_cid = SPARX5_VCAP_CID_ES0_L0,
		.last_cid = SPARX5_VCAP_CID_ES0_MAX,
		.count = 4096,
	},
	{
		.vtype = VCAP_TYPE_ES2,
		.lookups = SPARX5_ES2_LOOKUPS,
		.lookups_per_instance = SPARX5_ES2_LOOKUPS,
		.first_cid = SPARX5_VCAP_CID_ES2_L0,
		.last_cid = SPARX5_VCAP_CID_ES2_MAX,
		.count = 12288,
	},
	{
		.vtype = VCAP_TYPE_IS2, /* IS2-0 */
		.vinst = 0,
		.map_id = 4,
		.lookups = SPARX5_IS2_LOOKUPS,
		.lookups_per_instance = SPARX5_IS2_LOOKUPS / 2,
		.first_cid = SPARX5_VCAP_CID_IS2_L0,
		.last_cid = SPARX5_VCAP_CID_IS2_L2 - 1,
		.blockno = 0, /* Maps block 0-1 */
		.blocks = 2,
	},
	{
		.vtype = VCAP_TYPE_IS2, /* IS2-1 */
		.vinst = 1,
		.map_id = 5,
		.lookups = SPARX5_IS2_LOOKUPS,
		.lookups_per_instance = SPARX5_IS2_LOOKUPS / 2,
		.first_cid = SPARX5_VCAP_CID_IS2_L2,
		.last_cid = SPARX5_VCAP_CID_IS2_MAX,
		.blockno = 2, /* Maps block 2-3 */
		.blocks = 2,
	},
	{
		.vtype = VCAP_TYPE_IS0, /* CLM-0 */
		.vinst = 0,
		.map_id = 1,
		.lookups = SPARX5_IS0_LOOKUPS,
		.lookups_per_instance = SPARX5_IS0_LOOKUPS / 3,
		.first_cid = SPARX5_VCAP_CID_IS0_L0,
		.last_cid = SPARX5_VCAP_CID_IS0_L2 - 1,
		.blockno = 8, /* Maps block 8-9 */
		.blocks = 2,
	},
	{
		.vtype = VCAP_TYPE_IS0, /* CLM-1 */
		.vinst = 1,
		.map_id = 2,
		.lookups = SPARX5_IS0_LOOKUPS,
		.lookups_per_instance = SPARX5_IS0_LOOKUPS / 3,
		.first_cid = SPARX5_VCAP_CID_IS0_L2,
		.last_cid = SPARX5_VCAP_CID_IS0_L4 - 1,
		.blockno = 6, /* Maps block 6-7 */
		.blocks = 2,
	},
	{
		.vtype = VCAP_TYPE_IS0, /* CLM-2 */
		.vinst = 2,
		.map_id = 3,
		.lookups = SPARX5_IS0_LOOKUPS,
		.lookups_per_instance = SPARX5_IS0_LOOKUPS / 3,
		.first_cid = SPARX5_VCAP_CID_IS0_L4,
		.last_cid = SPARX5_VCAP_CID_IS0_MAX,
		.blockno = 4, /* Maps block 4-5 */
		.blocks = 2,
	},
};

/* In the following mapping tables the first lookup typically has the most
 * L2-friendly keysets where the following lookups provides the L3/L4 and the
 * smaller rules (IPv4) before the larger rules (IPv6).
 */
static enum vcap_keyfield_set
sparx5_vcap_is0_port_cfg[SPARX5_IS0_LOOKUPS][VCAP_IS0_PTC_MAX] = {
	{
		[VCAP_IS0_PTC_ETYPE] = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV4]  = VCAP_KFS_NORMAL_5TUPLE_IP4,
		[VCAP_IS0_PTC_IPV6]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_UC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_MC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_LS]  = VCAP_KFS_NORMAL_7TUPLE,
	},{
		[VCAP_IS0_PTC_ETYPE] = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV4]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV6]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_UC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_MC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_LS]  = VCAP_KFS_NORMAL_7TUPLE,
	},{
		[VCAP_IS0_PTC_ETYPE] = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV4]  = VCAP_KFS_NORMAL_5TUPLE_IP4,
		[VCAP_IS0_PTC_IPV6]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_UC]  = VCAP_KFS_NO_VALUE,
		[VCAP_IS0_PTC_MPLS_MC]  = VCAP_KFS_NO_VALUE,
		[VCAP_IS0_PTC_MPLS_LS]  = VCAP_KFS_NO_VALUE,
	},{
		[VCAP_IS0_PTC_ETYPE] = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV4]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV6]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_UC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_MC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_LS]  = VCAP_KFS_NORMAL_7TUPLE,
	},{
		[VCAP_IS0_PTC_ETYPE] = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV4]  = VCAP_KFS_NORMAL_5TUPLE_IP4,
		[VCAP_IS0_PTC_IPV6]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_UC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_MC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_LS]  = VCAP_KFS_NORMAL_7TUPLE,
	},{
		[VCAP_IS0_PTC_ETYPE] = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV4]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_IPV6]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_UC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_MC]  = VCAP_KFS_NORMAL_7TUPLE,
		[VCAP_IS0_PTC_MPLS_LS]  = VCAP_KFS_NORMAL_7TUPLE,
	}
};

static enum vcap_keyfield_set
sparx5_vcap_is2_port_cfg[SPARX5_IS2_LOOKUPS][VCAP_IS2_PTC_MAX] = {
	{
		[VCAP_IS2_PTC_NONETH] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_ARP] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_IPV4_UC] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_IPV4_MC] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_IPV6_UC] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_IPV6_MC] = VCAP_KFS_MAC_ETYPE,
	},
	{
		[VCAP_IS2_PTC_NONETH] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_ARP] = VCAP_KFS_ARP,
		[VCAP_IS2_PTC_IPV4_UC] = VCAP_KFS_IP4_TCP_UDP,
		[VCAP_IS2_PTC_IPV4_MC] = VCAP_KFS_IP4_TCP_UDP,
		[VCAP_IS2_PTC_IPV6_UC] = VCAP_KFS_IP6_STD,
		[VCAP_IS2_PTC_IPV6_MC] = VCAP_KFS_IP6_STD,
	},
	{
		[VCAP_IS2_PTC_NONETH] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_ARP] = VCAP_KFS_ARP,
		[VCAP_IS2_PTC_IPV4_UC] = VCAP_KFS_IP_7TUPLE,
		[VCAP_IS2_PTC_IPV4_MC] = VCAP_KFS_IP_7TUPLE,
		[VCAP_IS2_PTC_IPV6_UC] = VCAP_KFS_IP_7TUPLE,
		[VCAP_IS2_PTC_IPV6_MC] = VCAP_KFS_IP_7TUPLE,
	},
	{
		[VCAP_IS2_PTC_NONETH] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_ARP] = VCAP_KFS_MAC_ETYPE,
		[VCAP_IS2_PTC_IPV4_UC] = VCAP_KFS_IP_7TUPLE,
		[VCAP_IS2_PTC_IPV4_MC] = VCAP_KFS_IP_7TUPLE,
		[VCAP_IS2_PTC_IPV6_UC] = VCAP_KFS_IP_7TUPLE,
		[VCAP_IS2_PTC_IPV6_MC] = VCAP_KFS_IP_7TUPLE,
	},
};

static enum vcap_keyfield_set
sparx5_vcap_es2_port_cfg[SPARX5_ES2_LOOKUPS][VCAP_ES2_PTC_MAX] = {
	{
		[VCAP_ES2_PTC_IPV4] = VCAP_KFS_MAC_ETYPE,
		[VCAP_ES2_PTC_IPV6] = VCAP_KFS_MAC_ETYPE,
		[VCAP_ES2_PTC_ARP] = VCAP_KFS_MAC_ETYPE,
	},
	{
		[VCAP_ES2_PTC_IPV4] = VCAP_KFS_IP4_TCP_UDP,
		[VCAP_ES2_PTC_IPV6] = VCAP_KFS_IP_7TUPLE,
		[VCAP_ES2_PTC_ARP] = VCAP_KFS_ARP,
	},
};

/* Get the keyset name from the Sparx5 VCAP model */
const char *sparx5_vcap_keyset_name(struct net_device *ndev,
				    enum vcap_keyfield_set keyset)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;

	return vctrl->stats->keyfield_set_names[keyset];
}

/* Get the key name from the Sparx5 VCAP model */
const char *sparx5_vcap_key_name(struct net_device *ndev,
				 enum vcap_key_field key)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;

	return vctrl->stats->keyfield_names[key];
}

static int sparx5_vcap_is0_keyset_to_portsel(enum vcap_is0_port_traffic_class ptc,
					     enum vcap_keyfield_set keyset)
{
	int sel;

	switch (ptc) {
	case VCAP_IS0_PTC_ETYPE:
	case VCAP_IS0_PTC_IPV4:
	case VCAP_IS0_PTC_IPV6:
		switch (keyset) {
		case VCAP_KFS_NORMAL_7TUPLE:
			sel = VCAP_IS0_PS_ETYPE_NORMAL_7TUPLE;
			break;
		case VCAP_KFS_NORMAL_5TUPLE_IP4:
			sel = VCAP_IS0_PS_ETYPE_NORMAL_5TUPLE_IP4;
			break;
		default:
			sel = VCAP_IS0_PS_ETYPE_NO_LOOKUP;
			break;
		}
		break;
	case VCAP_IS0_PTC_MPLS_UC:
	case VCAP_IS0_PTC_MPLS_MC:
		sel = VCAP_IS0_PS_MPLS_UC_MC_FOLLOW_ETYPE;
		break;
	case VCAP_IS0_PTC_MPLS_LS:
		sel = VCAP_IS0_PS_MPLS_LS_FOLLOW_ETYPE;
		break;
	default:
		sel = -EINVAL;
		break;
	}
	return sel;
}

static int sparx5_vcap_is2_keyset_to_portsel(enum vcap_is2_port_traffic_class ptc,
					     enum vcap_keyfield_set keyset)
{
	int sel;

	switch (ptc) {
	case VCAP_IS2_PTC_NONETH:
		switch (keyset) {
		case VCAP_KFS_MAC_ETYPE:
			sel = VCAP_IS2_PS_NONETH_MAC_ETYPE;
			break;
		default:
			sel = VCAP_IS2_PS_NONETH_NO_LOOKUP;
			break;
		}
		break;
	case VCAP_IS2_PTC_ARP:
		switch (keyset) {
		case VCAP_KFS_MAC_ETYPE:
			sel = VCAP_IS2_PS_ARP_MAC_ETYPE;
			break;
		case VCAP_KFS_ARP:
			sel = VCAP_IS2_PS_ARP_ARP;
			break;
		default:
			sel = -EINVAL;
			break;
		}
		break;
	case VCAP_IS2_PTC_IPV4_UC:
		switch (keyset) {
		case VCAP_KFS_MAC_ETYPE:
			sel = VCAP_IS2_PS_IPV4_UC_MAC_ETYPE;
			break;
		case VCAP_KFS_IP4_OTHER:
		case VCAP_KFS_IP4_TCP_UDP:
			sel = VCAP_IS2_PS_IPV4_UC_IP4_TCP_UDP_OTHER;
			break;
		case VCAP_KFS_IP_7TUPLE:
			sel = VCAP_IS2_PS_IPV4_UC_IP_7TUPLE;
			break;
		default:
			sel = -EINVAL;
			break;
		}
		break;
	case VCAP_IS2_PTC_IPV4_MC:
		switch (keyset) {
		case VCAP_KFS_MAC_ETYPE:
			sel = VCAP_IS2_PS_IPV4_MC_MAC_ETYPE;
			break;
		case VCAP_KFS_IP4_OTHER:
		case VCAP_KFS_IP4_TCP_UDP:
			sel = VCAP_IS2_PS_IPV4_MC_IP4_TCP_UDP_OTHER;
			break;
		case VCAP_KFS_IP_7TUPLE:
			sel = VCAP_IS2_PS_IPV4_MC_IP_7TUPLE;
			break;
		default:
			sel = -EINVAL;
			break;
		}
		break;
	case VCAP_IS2_PTC_IPV6_UC:
		switch (keyset) {
		case VCAP_KFS_MAC_ETYPE:
			sel = VCAP_IS2_PS_IPV6_UC_MAC_ETYPE;
			break;
		case VCAP_KFS_IP_7TUPLE:
			sel = VCAP_IS2_PS_IPV6_UC_IP_7TUPLE;
			break;
		case VCAP_KFS_IP6_STD:
			sel = VCAP_IS2_PS_IPV6_UC_IP6_STD;
			break;
		case VCAP_KFS_IP4_OTHER:
		case VCAP_KFS_IP4_TCP_UDP:
			sel = VCAP_IS2_PS_IPV6_UC_IP4_TCP_UDP_OTHER;
			break;
		default:
			sel = -EINVAL;
			break;
		}
		break;
	case VCAP_IS2_PTC_IPV6_MC:
		switch (keyset) {
		case VCAP_KFS_MAC_ETYPE:
			sel = VCAP_IS2_PS_IPV6_MC_MAC_ETYPE;
			break;
		case VCAP_KFS_IP_7TUPLE:
			sel = VCAP_IS2_PS_IPV6_MC_IP_7TUPLE;
			break;
		case VCAP_KFS_IP6_STD:
			sel = VCAP_IS2_PS_IPV6_MC_IP6_STD;
			break;
		case VCAP_KFS_IP4_OTHER:
		case VCAP_KFS_IP4_TCP_UDP:
			sel = VCAP_IS2_PS_IPV6_MC_IP4_TCP_UDP_OTHER;
			break;
		default:
			sel = -EINVAL;
			break;
		}
		break;
	default:
		sel = -EINVAL;
		break;
	}
	return sel;
}

static int sparx5_vcap_es2_keyset_to_portsel(enum vcap_es2_port_traffic_class ptc,
					     enum vcap_keyfield_set keyset)
{
	int sel;

	switch (ptc) {
	case VCAP_ES2_PTC_ARP:
		switch (keyset) {
		case VCAP_KFS_ARP:
			sel = VCAP_ES2_PS_ARP_ARP;
			break;
		default:
			sel = VCAP_ES2_PS_ARP_MAC_ETYPE;
			break;
		}
		break;
	case VCAP_ES2_PTC_IPV4:
		switch (keyset) {
		case VCAP_KFS_IP_7TUPLE:
			sel = VCAP_ES2_PS_IPV4_IP_7TUPLE;
			break;
		case VCAP_KFS_IP4_OTHER:
		case VCAP_KFS_IP4_TCP_UDP:
			sel = VCAP_ES2_PS_IPV4_IP4_TCP_UDP_OTHER;
			break;
		default:
			sel = VCAP_ES2_PS_IPV4_MAC_ETYPE;
			break;
		}
		break;
	case VCAP_ES2_PTC_IPV6:
		switch (keyset) {
		case VCAP_KFS_IP_7TUPLE:
			sel = VCAP_ES2_PS_IPV6_IP_7TUPLE;
			break;
		case VCAP_KFS_IP6_STD:
			sel = VCAP_ES2_PS_IPV6_IP_7TUPLE_STD;
			break;
		default:
			sel = VCAP_ES2_PS_IPV6_MAC_ETYPE;
			break;
		}
		break;
	default:
		sel = -EINVAL;
		break;
	}
	return sel;
}

static const char *sparx5_vcap_is0_etype_port_cfg_to_str(u32 value)
{
	switch (value) {
	case VCAP_IS0_PS_ETYPE_DEFAULT:
		return "default";
	case VCAP_IS0_PS_ETYPE_NORMAL_7TUPLE:
		return "normal_7tuple";
	case VCAP_IS0_PS_ETYPE_NORMAL_5TUPLE_IP4:
		return "normal_5tuple_ip4";
	default:
		return "no lookup";
	}
}

static const char *sparx5_ifname(struct sparx5 *sparx5, int portno)
{
	const char *ifname = "-";
	struct sparx5_port *port = sparx5->ports[portno];

	if (port)
		ifname = netdev_name(port->ndev);
	return ifname;
}

static void sparx5_vcap_port_keys(int (*pf)(void *out, int arg, const char *f, ...),
				  void *out,
				  int arg,
				  struct sparx5 *sparx5,
				  struct vcap_admin *admin)
{
	u32 value, last_value = 0;
	int lookup, portno;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
			value = spx5_rd(sparx5, REW_ES0_CTRL);
			pf(out, arg, "\n  lookup: ");
			if (REW_ES0_CTRL_ES0_LU_ENA_GET(value))
				pf(out, arg, "enabled");
			else
				pf(out, arg, "disabled");
		for (portno = 0; portno < SPX5_PORTS; ++portno) {
			if (!sparx5->ports[portno])
				continue;
			value = spx5_rd(sparx5, REW_RTAG_ETAG_CTRL(portno));
			if (value == last_value)
				continue;
			pf(out, arg, "\n  port[%02d] (%s): ", portno,
			   sparx5_ifname(sparx5, portno));
			switch (REW_RTAG_ETAG_CTRL_ES0_ISDX_KEY_ENA_GET(value)) {
			case VCAP_ES0_PS_NORMAL_SELECTION: pf(out, arg, "normal"); break;
			case VCAP_ES0_PS_FORCE_ISDX_LOOKUPS: pf(out, arg, "force isdx"); break;
			case VCAP_ES0_PS_FORCE_VID_LOOKUPS: pf(out, arg, "force vid"); break;
			case VCAP_ES0_PS_RESERVED: pf(out, arg, "reserved"); break;
			}
			last_value = value;
		}
		pf(out, arg, "\n");
		break;
	case VCAP_TYPE_ES2:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			for (portno = 0; portno < SPX5_PORTS; ++portno) {
				if (!sparx5->ports[portno])
					continue;
				value = spx5_rd(sparx5, EACL_VCAP_ES2_KEY_SEL(portno, lookup));
				if (value == last_value)
					continue;
				pf(out, arg, "\n  port[%02d][%d] (%s): ", portno, lookup,
				   sparx5_ifname(sparx5, portno));
				pf(out, arg, "\n    state: ");
				if (EACL_VCAP_ES2_KEY_SEL_KEY_ENA_GET(value))
					pf(out, arg, "on");
				else
					pf(out, arg, "off");
				pf(out, arg, "\n    arp: ");
				switch (EACL_VCAP_ES2_KEY_SEL_ARP_KEY_SEL_GET(value)) {
				case VCAP_ES2_PS_ARP_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_ES2_PS_ARP_ARP: pf(out, arg, "arp"); break;
				}
				pf(out, arg, "\n    ipv4: ");
				switch (EACL_VCAP_ES2_KEY_SEL_IP4_KEY_SEL_GET(value)) {
				case VCAP_ES2_PS_IPV4_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_ES2_PS_IPV4_IP_7TUPLE: pf(out, arg, "ip_7tuple"); break;
				case VCAP_ES2_PS_IPV4_IP4_TCP_UDP_VID: pf(out, arg, "ip4_tcp_udp or ip4_vid"); break;
				case VCAP_ES2_PS_IPV4_IP4_TCP_UDP_OTHER: pf(out, arg, "ip4_tcp_udp or ip4_other"); break;
				case VCAP_ES2_PS_IPV4_IP4_VID: pf(out, arg, "ip4_vid"); break;
				case VCAP_ES2_PS_IPV4_IP4_OTHER: pf(out, arg, "ip4_other"); break;
				}
				pf(out, arg, "\n    ipv6: ");
				switch (EACL_VCAP_ES2_KEY_SEL_IP6_KEY_SEL_GET(value)) {
				case VCAP_ES2_PS_IPV6_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_ES2_PS_IPV6_IP_7TUPLE: pf(out, arg, "ip_7tuple"); break;
				case VCAP_ES2_PS_IPV6_IP_7TUPLE_VID: pf(out, arg, "ip_7tuple or ip6_vid"); break;
				case VCAP_ES2_PS_IPV6_IP_7TUPLE_STD: pf(out, arg, "ip_7tuple or ip6_std"); break;
				case VCAP_ES2_PS_IPV6_IP6_VID: pf(out, arg, "ip6_vid"); break;
				case VCAP_ES2_PS_IPV6_IP6_STD: pf(out, arg, "ip6_std"); break;
				case VCAP_ES2_PS_IPV6_IP4_DOWNGRADE: pf(out, arg, "ip4_downgrade"); break;
				}
				last_value = value;
			}
		}
		pf(out, arg, "\n");
		break;
	case VCAP_TYPE_IS0:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			for (portno = 0; portno < SPX5_PORTS; ++portno) {
				if (!sparx5->ports[portno])
					continue;
				value = spx5_rd(sparx5, ANA_CL_ADV_CL_CFG(portno, lookup));
				if (value == last_value)
					continue;
				pf(out, arg, "\n  port[%02d][%d] (%s): ", portno, lookup,
				   sparx5_ifname(sparx5, portno));
				pf(out, arg, "\n    state: ");
				if (ANA_CL_ADV_CL_CFG_LOOKUP_ENA_GET(value))
					pf(out, arg, "on");
				else
					pf(out, arg, "off");
				pf(out, arg, "\n    etype: %s",
				   sparx5_vcap_is0_etype_port_cfg_to_str(
					   ANA_CL_ADV_CL_CFG_ETYPE_CLM_KEY_SEL_GET(value)));
				pf(out, arg, "\n    ipv4: %s",
				   sparx5_vcap_is0_etype_port_cfg_to_str(
					   ANA_CL_ADV_CL_CFG_IP4_CLM_KEY_SEL_GET(value)));
				pf(out, arg, "\n    ipv6: %s",
				   sparx5_vcap_is0_etype_port_cfg_to_str(
					   ANA_CL_ADV_CL_CFG_IP6_CLM_KEY_SEL_GET(value)));
				pf(out, arg, "\n    mpls_uc: follow_etype");
				pf(out, arg, "\n    mpls_mc: follow_etype");
				pf(out, arg, "\n    mpls_ls: follow_etype");
				last_value = value;
			}
		}
		pf(out, arg, "\n");
		break;
	case VCAP_TYPE_IS2:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			for (portno = 0; portno < SPX5_PORTS; ++portno) {
				if (!sparx5->ports[portno])
					continue;
				value = spx5_rd(sparx5, ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
				if (value == last_value)
					continue;
				pf(out, arg, "\n  port[%02d][%d] (%s): ", portno, lookup,
				   sparx5_ifname(sparx5, portno));
				pf(out, arg, "\n    state: ");
				if (ANA_ACL_VCAP_S2_KEY_SEL_KEY_SEL_ENA_GET(value))
					pf(out, arg, "on");
				else
					pf(out, arg, "off");
				pf(out, arg, "\n    igr_port: ");
				if (ANA_ACL_VCAP_S2_KEY_SEL_IGR_PORT_MASK_SEL_GET(value))
					pf(out, arg, "l3 info");
				else
					pf(out, arg, "l2 info");
				pf(out, arg, "\n    noneth: ");
				switch (ANA_ACL_VCAP_S2_KEY_SEL_NON_ETH_KEY_SEL_GET(value)) {
				case VCAP_IS2_PS_NONETH_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_IS2_PS_NONETH_CUSTOM_1: pf(out, arg, "custom1"); break;
				case VCAP_IS2_PS_NONETH_CUSTOM_2: pf(out, arg, "custom2"); break;
				case VCAP_IS2_PS_NONETH_NO_LOOKUP: pf(out, arg, "none"); break;
				}
				pf(out, arg, "\n    ipv4_mc: ");
				switch (ANA_ACL_VCAP_S2_KEY_SEL_IP4_MC_KEY_SEL_GET(value)) {
				case VCAP_IS2_PS_IPV4_MC_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_IS2_PS_IPV4_MC_IP4_TCP_UDP_OTHER: pf(out, arg, "ip4_tcp_udp ip4_other"); break;
				case VCAP_IS2_PS_IPV4_MC_IP_7TUPLE: pf(out, arg, "ip_7tuple"); break;
				case VCAP_IS2_PS_IPV4_MC_IP4_VID: pf(out, arg, "ip4_vid"); break;
				}
				pf(out, arg, "\n    ipv4_uc: ");
				switch (ANA_ACL_VCAP_S2_KEY_SEL_IP4_UC_KEY_SEL_GET(value)) {
				case VCAP_IS2_PS_IPV4_UC_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_IS2_PS_IPV4_UC_IP4_TCP_UDP_OTHER: pf(out, arg, "ip4_tcp_udp ip4_other"); break;
				case VCAP_IS2_PS_IPV4_UC_IP_7TUPLE: pf(out, arg, "ip_7tuple"); break;
				}
				pf(out, arg, "\n    ipv6_mc: ");
				switch (ANA_ACL_VCAP_S2_KEY_SEL_IP6_MC_KEY_SEL_GET(value)) {
				case VCAP_IS2_PS_IPV6_MC_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_IS2_PS_IPV6_MC_IP_7TUPLE: pf(out, arg, "ip_7tuple"); break;
				case VCAP_IS2_PS_IPV6_MC_IP6_VID: pf(out, arg, "ip6_vid"); break;
				case VCAP_IS2_PS_IPV6_MC_IP6_STD: pf(out, arg, "ip6_std"); break;
				case VCAP_IS2_PS_IPV6_MC_IP4_TCP_UDP_OTHER: pf(out, arg, "ip4_tcp_udp ipv4_other"); break;
				}
				pf(out, arg, "\n    ipv6_uc: ");
				switch (ANA_ACL_VCAP_S2_KEY_SEL_IP6_UC_KEY_SEL_GET(value)) {
				case VCAP_IS2_PS_IPV6_UC_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_IS2_PS_IPV6_UC_IP_7TUPLE: pf(out, arg, "ip_7tuple"); break;
				case VCAP_IS2_PS_IPV6_UC_IP6_STD: pf(out, arg, "ip6_std"); break;
				case VCAP_IS2_PS_IPV6_UC_IP4_TCP_UDP_OTHER: pf(out, arg, "ip4_tcp_udp ip4_other"); break;
				}
				pf(out, arg, "\n    arp: ");
				switch (ANA_ACL_VCAP_S2_KEY_SEL_ARP_KEY_SEL_GET(value)) {
				case VCAP_IS2_PS_ARP_MAC_ETYPE: pf(out, arg, "mac_etype"); break;
				case VCAP_IS2_PS_ARP_ARP: pf(out, arg, "arp"); break;
				}
				last_value = value;
			}
		}
		pf(out, arg, "\n");
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

static void sparx5_vcap_port_stickies(int (*pf)(void *out, int arg, const char *f, ...),
				      void *out,
				      int arg,
				      struct sparx5 *sparx5,
				      struct vcap_admin *admin)
{
	int lookup;
	u32 value;

	switch (admin->vtype) {
	case VCAP_TYPE_ES2:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			value = spx5_rd(sparx5, EACL_SEC_LOOKUP_STICKY(lookup));
			pf(out, arg, "  lookup[%d]: sticky: 0x%08x",
				   lookup, value);
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP_7TUPLE_STICKY_GET(value))
				pf(out, arg, " IP_7TUPLE");
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP6_VID_STICKY_GET(value))
				pf(out, arg, " IP6_VID");
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP6_STD_STICKY_GET(value))
				pf(out, arg, " IP6_STD");
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP4_TCPUDP_STICKY_GET(value))
				pf(out, arg, " IP4_TCP_UDP");
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP4_VID_STICKY_GET(value))
				pf(out, arg, " IP4_VID");
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP4_OTHER_STICKY_GET(value))
				pf(out, arg, " IP4_OTHER");
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_ARP_STICKY_GET(value))
				pf(out, arg, " ARP");
			if (EACL_SEC_LOOKUP_STICKY_SEC_TYPE_MAC_ETYPE_STICKY_GET(value))
				pf(out, arg, " MAC_ETYPE");
			pf(out, arg, "\n");
			/* Clear stickies */
			spx5_wr(value, sparx5, EACL_SEC_LOOKUP_STICKY(lookup));
		}
		break;
	case VCAP_TYPE_ES0:
	case VCAP_TYPE_IS0:
		/* No key selection stickies are available */
		break;
	case VCAP_TYPE_IS2:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			value = spx5_rd(sparx5, ANA_ACL_SEC_LOOKUP_STICKY(lookup));
			pf(out, arg, "  lookup[%d]: sticky: 0x%08x",
				   lookup, value);
			if (ANA_ACL_SEC_LOOKUP_STICKY_KEY_SEL_CLM_STICKY_GET(value))
				pf(out, arg, " SEL_CLM");
			if (ANA_ACL_SEC_LOOKUP_STICKY_KEY_SEL_IRLEG_STICKY_GET(value))
				pf(out, arg, " SEL_IRLEG");
			if (ANA_ACL_SEC_LOOKUP_STICKY_KEY_SEL_ERLEG_STICKY_GET(value))
				pf(out, arg, " SEL_ERLEG");
			if (ANA_ACL_SEC_LOOKUP_STICKY_KEY_SEL_PORT_STICKY_GET(value))
				pf(out, arg, " SEL_PORT");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_CUSTOM2_STICKY_GET(value))
				pf(out, arg, " CUSTOM2");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_CUSTOM1_STICKY_GET(value))
				pf(out, arg, " CUSTOM1");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_OAM_STICKY_GET(value))
				pf(out, arg, " OAM");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP6_VID_STICKY_GET(value))
				pf(out, arg, " IP6_VID");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP6_STD_STICKY_GET(value))
				pf(out, arg, " IP6_STD");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP6_TCPUDP_STICKY_GET(value))
				pf(out, arg, " IP6_TCPUDP");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP_7TUPLE_STICKY_GET(value))
				pf(out, arg, " IP_7TUPLE");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP4_VID_STICKY_GET(value))
				pf(out, arg, " IP4_VID");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP4_TCPUDP_STICKY_GET(value))
				pf(out, arg, " IP4_TCPUDP");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_IP4_OTHER_STICKY_GET(value))
				pf(out, arg, " IP4_OTHER");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_ARP_STICKY_GET(value))
				pf(out, arg, " ARP");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_MAC_SNAP_STICKY_GET(value))
				pf(out, arg, " MAC_SNAP");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_MAC_LLC_STICKY_GET(value))
				pf(out, arg, " MAC_LLC");
			if (ANA_ACL_SEC_LOOKUP_STICKY_SEC_TYPE_MAC_ETYPE_STICKY_GET(value))
				pf(out, arg, " MAC_ETYPE");
			pf(out, arg, "\n");
			/* Clear stickies */
			spx5_wr(value, sparx5, ANA_ACL_SEC_LOOKUP_STICKY(lookup));
		}
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

int sparx5_vcap_port_info(struct sparx5 *sparx5,
				 struct vcap_admin *admin,
				 int (*pf)(void *out, int arg, const char *f, ...),
				 void *out, int arg)
{
	struct vcap_control *vctrl = sparx5->vcap_ctrl;
	const struct vcap_info *vcap = &vctrl->vcaps[admin->vtype];

	pf(out, arg, "%s:\n", vcap->name);
	sparx5_vcap_port_stickies(pf, out, arg, sparx5, admin);
	sparx5_vcap_port_keys(pf, out, arg, sparx5, admin);
	return 0;
}

static void sparx5_es0_read_esdx_counter(struct sparx5 *sparx5,
					 struct vcap_admin *admin, u32 id)
{
	u32 counter;

	mutex_lock(&sparx5->queue_stats_lock);
	spx5_wr(XQS_STAT_CFG_STAT_VIEW_SET(id), sparx5, XQS_STAT_CFG);
	counter = spx5_rd(sparx5, XQS_CNT(SPARX5_STAT_ESDX_GRN_PKTS)) +
		spx5_rd(sparx5, XQS_CNT(SPARX5_STAT_ESDX_YEL_PKTS));
	mutex_unlock(&sparx5->queue_stats_lock);
	if (counter)
		admin->cache.counter = counter;
}

static void sparx5_es0_write_esdx_counter(struct sparx5 *sparx5,
					   struct vcap_admin *admin, u32 id)
{
	mutex_lock(&sparx5->queue_stats_lock);
	spx5_wr(XQS_STAT_CFG_STAT_VIEW_SET(id), sparx5, XQS_STAT_CFG);
	spx5_wr(admin->cache.counter, sparx5, XQS_CNT(SPARX5_STAT_ESDX_GRN_PKTS));
	spx5_wr(0, sparx5, XQS_CNT(SPARX5_STAT_ESDX_YEL_PKTS));
	mutex_unlock(&sparx5->queue_stats_lock);
}

static void sparx5_vcap_wait_es0_update(struct sparx5 *sparx5)
{
	u32 value;

	read_poll_timeout(spx5_rd, value,
			  !VCAP_ES0_CTRL_UPDATE_SHOT_GET(value), 500, 10000,
			  false, sparx5, VCAP_ES0_CTRL);
}

static void sparx5_vcap_wait_es2_update(struct sparx5 *sparx5)
{
	u32 value;

	read_poll_timeout(spx5_rd, value,
			  !VCAP_ES2_CTRL_UPDATE_SHOT_GET(value), 500, 10000,
			  false, sparx5, VCAP_ES2_CTRL);
}

static void sparx5_vcap_wait_super_update(struct sparx5 *sparx5)
{
	u32 value;

	read_poll_timeout(spx5_rd, value,
			  !VCAP_SUPER_CTRL_UPDATE_SHOT_GET(value), 500, 10000,
			  false, sparx5, VCAP_SUPER_CTRL);
}

/* Convert chain id to vcap lookup id */
static int sparx5_vcap_cid_to_lookup(struct vcap_admin *admin, int cid)
{
	int lookup = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		break;
	case VCAP_TYPE_ES2:
		if (cid >= SPARX5_VCAP_CID_ES2_L1 &&
		    cid < SPARX5_VCAP_CID_ES2_MAX)
			lookup = 1;
		break;
	case VCAP_TYPE_IS0:
		if (cid >= SPARX5_VCAP_CID_IS0_L1 &&
		    cid < SPARX5_VCAP_CID_IS0_L2)
			lookup = 1;
		else if (cid >= SPARX5_VCAP_CID_IS0_L2 &&
			 cid < SPARX5_VCAP_CID_IS0_L3)
			lookup = 2;
		else if (cid >= SPARX5_VCAP_CID_IS0_L3 &&
			 cid < SPARX5_VCAP_CID_IS0_L4)
			lookup = 3;
		else if (cid >= SPARX5_VCAP_CID_IS0_L4 &&
			 cid < SPARX5_VCAP_CID_IS0_L5)
			lookup = 4;
		else if (cid >= SPARX5_VCAP_CID_IS0_L5 &&
			 cid < SPARX5_VCAP_CID_IS0_MAX)
			lookup = 5;
		break;
	case VCAP_TYPE_IS2:
		if (cid >= SPARX5_VCAP_CID_IS2_L1 &&
		    cid < SPARX5_VCAP_CID_IS2_L2)
			lookup = 1;
		else if (cid >= SPARX5_VCAP_CID_IS2_L2 &&
			 cid < SPARX5_VCAP_CID_IS2_L3)
			lookup = 2;
		else if (cid >= SPARX5_VCAP_CID_IS2_L3 &&
			 cid < SPARX5_VCAP_CID_IS2_MAX)
			lookup = 3;
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
	return lookup;
}

static void
sparx5_vcap_is0_get_port_etype_keysets(struct vcap_keyset_list *keysetlist,
					 u32 value)
{
	switch (ANA_CL_ADV_CL_CFG_ETYPE_CLM_KEY_SEL_GET(value)) {
	case VCAP_IS0_PS_ETYPE_DEFAULT:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
		break;
	case VCAP_IS0_PS_ETYPE_NORMAL_7TUPLE:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_7TUPLE);
		break;
	case VCAP_IS0_PS_ETYPE_NORMAL_5TUPLE_IP4:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_5TUPLE_IP4);
		break;
	}
}

/* Return the list of keysets for the vcap port configuration */
static int sparx5_vcap_is0_get_port_keysets(struct net_device *ndev,
					    int lookup,
					    struct vcap_keyset_list *keysetlist,
					    u16 l3_proto)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	int portno = port->portno;
	u32 value;

	/* Check if the port keyset selection is enabled */
	value = spx5_rd(sparx5, ANA_CL_ADV_CL_CFG(portno, lookup));
	if (!ANA_CL_ADV_CL_CFG_LOOKUP_ENA_GET(value))
		return -ENOENT;

	/* Collect all keysets for the port in a list */
	if (l3_proto == ETH_P_ALL)
		sparx5_vcap_is0_get_port_etype_keysets(keysetlist, value);
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IP) {
		switch (ANA_CL_ADV_CL_CFG_IP4_CLM_KEY_SEL_GET(value)) {
		case VCAP_IS0_PS_ETYPE_DEFAULT:
			sparx5_vcap_is0_get_port_etype_keysets(keysetlist, value);
			break;
		case VCAP_IS0_PS_ETYPE_NORMAL_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_7TUPLE);
			break;
		case VCAP_IS0_PS_ETYPE_NORMAL_5TUPLE_IP4:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_5TUPLE_IP4);
			break;
		}
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IPV6) {
		switch (ANA_CL_ADV_CL_CFG_IP6_CLM_KEY_SEL_GET(value)) {
		case VCAP_IS0_PS_ETYPE_DEFAULT:
			sparx5_vcap_is0_get_port_etype_keysets(keysetlist, value);
			break;
		case VCAP_IS0_PS_ETYPE_NORMAL_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_7TUPLE);
			break;
		case VCAP_IS0_PS_ETYPE_NORMAL_5TUPLE_IP4:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_5TUPLE_IP4);
			break;
		}
	}
	if (l3_proto != ETH_P_IP && l3_proto != ETH_P_IPV6) {
		sparx5_vcap_is0_get_port_etype_keysets(keysetlist, value);
	}
	return 0;
}

/* Return the list of keysets for the vcap port configuration */
static int sparx5_vcap_is2_get_port_keysets(struct net_device *ndev,
					    int lookup,
					    struct vcap_keyset_list *keysetlist,
					    u16 l3_proto)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	int portno = port->portno;
	u32 value;

	/* Check if the port keyset selection is enabled */
	value = spx5_rd(sparx5, ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
	if (!ANA_ACL_VCAP_S2_KEY_SEL_KEY_SEL_ENA_GET(value))
		return -ENOENT;

	/* Collect all keysets for the port in a list */
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_ARP) {
		switch (ANA_ACL_VCAP_S2_KEY_SEL_ARP_KEY_SEL_GET(value)) {
		case VCAP_IS2_PS_ARP_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		case VCAP_IS2_PS_ARP_ARP:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_ARP);
			break;
		}
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IP) {
		switch (ANA_ACL_VCAP_S2_KEY_SEL_IP4_UC_KEY_SEL_GET(value)) {
		case VCAP_IS2_PS_IPV4_UC_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		case VCAP_IS2_PS_IPV4_UC_IP4_TCP_UDP_OTHER:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
			break;
		case VCAP_IS2_PS_IPV4_UC_IP_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
			break;
		}
		switch (ANA_ACL_VCAP_S2_KEY_SEL_IP4_MC_KEY_SEL_GET(value)) {
		case VCAP_IS2_PS_IPV4_MC_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		case VCAP_IS2_PS_IPV4_MC_IP4_TCP_UDP_OTHER:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
			break;
		case VCAP_IS2_PS_IPV4_MC_IP_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
			break;
		}
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IPV6) {
		switch (ANA_ACL_VCAP_S2_KEY_SEL_IP6_UC_KEY_SEL_GET(value)) {
		case VCAP_IS2_PS_IPV6_UC_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		case VCAP_IS2_PS_IPV6_UC_IP_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
			break;
		case VCAP_IS2_PS_IPV6_UC_IP6_STD:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP6_STD);
			break;
		case VCAP_IS2_PS_IPV6_UC_IP4_TCP_UDP_OTHER:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
			break;
		}
		switch (ANA_ACL_VCAP_S2_KEY_SEL_IP6_MC_KEY_SEL_GET(value)) {
		case VCAP_IS2_PS_IPV6_MC_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		case VCAP_IS2_PS_IPV6_MC_IP_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
			break;
		case VCAP_IS2_PS_IPV6_MC_IP6_STD:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP6_STD);
			break;
		case VCAP_IS2_PS_IPV6_MC_IP4_TCP_UDP_OTHER:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
			break;
		case VCAP_IS2_PS_IPV6_MC_IP6_VID:
			/* Not used */
			break;
		}
	}
	if (l3_proto != ETH_P_ARP && l3_proto != ETH_P_IP &&
	    l3_proto != ETH_P_IPV6) {
		switch (ANA_ACL_VCAP_S2_KEY_SEL_NON_ETH_KEY_SEL_GET(value)) {
		case VCAP_IS2_PS_NONETH_MAC_ETYPE:
			/* IS2 non-classified frames generate MAC_ETYPE */
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		}
	}
	return 0;
}

static int sparx5_vcap_es0_get_port_keysets(struct net_device *ndev,
					    struct vcap_keyset_list *keysetlist)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	int portno = port->portno;
	u32 value;

	/* Check if the port keyset selection is enabled */
	value = spx5_rd(sparx5, REW_ES0_CTRL);
	if (!REW_ES0_CTRL_ES0_LU_ENA_GET(value))
		return -ENOENT;

	value = spx5_rd(sparx5, REW_RTAG_ETAG_CTRL(portno));
	/* Collect all keysets for the port in a list */
	switch (REW_RTAG_ETAG_CTRL_ES0_ISDX_KEY_ENA_GET(value)) {
	case VCAP_ES0_PS_NORMAL_SELECTION:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_VID);
		vcap_keyset_list_add(keysetlist, VCAP_KFS_ISDX);
		break;
	case VCAP_ES0_PS_FORCE_ISDX_LOOKUPS:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_ISDX);
		break;
	case VCAP_ES0_PS_FORCE_VID_LOOKUPS:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_VID);
		break;
	}
	return 0;
}

static void sparx5_vcap_es2_get_port_ipv4_keysets(struct vcap_keyset_list *keysetlist,
						 u32 value)
{
	switch (EACL_VCAP_ES2_KEY_SEL_IP4_KEY_SEL_GET(value)) {
	case VCAP_ES2_PS_IPV4_MAC_ETYPE:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
		break;
	case VCAP_ES2_PS_IPV4_IP_7TUPLE:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
		break;
	case VCAP_ES2_PS_IPV4_IP4_TCP_UDP_VID:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
		break;
	case VCAP_ES2_PS_IPV4_IP4_TCP_UDP_OTHER:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
		vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
		break;
	case VCAP_ES2_PS_IPV4_IP4_VID:
		/* Not used */
		break;
	case VCAP_ES2_PS_IPV4_IP4_OTHER:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
		break;
	}
}

static int sparx5_vcap_es2_get_port_keysets(struct net_device *ndev,
					    int lookup,
					    struct vcap_keyset_list *keysetlist,
					    u16 l3_proto)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	int portno = port->portno;
	u32 value;

	/* Check if the port keyset selection is enabled */
	value = spx5_rd(sparx5, EACL_VCAP_ES2_KEY_SEL(portno, lookup));
	if (!EACL_VCAP_ES2_KEY_SEL_KEY_ENA_GET(value))
		return -ENOENT;

	/* Collect all keysets for the port in a list */
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_ARP) {
		switch (EACL_VCAP_ES2_KEY_SEL_ARP_KEY_SEL_GET(value)) {
		case VCAP_ES2_PS_ARP_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		case VCAP_ES2_PS_ARP_ARP:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_ARP);
			break;
		}
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IP)
		sparx5_vcap_es2_get_port_ipv4_keysets(keysetlist, value);
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IPV6) {
		switch (EACL_VCAP_ES2_KEY_SEL_IP6_KEY_SEL_GET(value)) {
		case VCAP_ES2_PS_IPV6_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		case VCAP_ES2_PS_IPV6_IP_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
			break;
		case VCAP_ES2_PS_IPV6_IP_7TUPLE_VID:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
			break;
		case VCAP_ES2_PS_IPV6_IP_7TUPLE_STD:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP_7TUPLE);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP6_STD);
			break;
		case VCAP_ES2_PS_IPV6_IP6_VID:
			/* Not used */
			break;
		case VCAP_ES2_PS_IPV6_IP6_STD:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP6_STD);
			break;
		case VCAP_ES2_PS_IPV6_IP4_DOWNGRADE:
			sparx5_vcap_es2_get_port_ipv4_keysets(keysetlist, value);
			break;
		}
	}
	if (l3_proto != ETH_P_ARP && l3_proto != ETH_P_IP &&
	    l3_proto != ETH_P_IPV6) {
		vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
	}
	return 0;
}

static bool sparx5_vcap_is0_is_first_chain(struct vcap_rule *rule)
{
	return (rule->vcap_chain_id >= SPARX5_VCAP_CID_IS0_L0 &&
		rule->vcap_chain_id < SPARX5_VCAP_CID_IS0_L1) ||
		((rule->vcap_chain_id >= SPARX5_VCAP_CID_IS0_L2 &&
		  rule->vcap_chain_id < SPARX5_VCAP_CID_IS0_L3)) ||
		((rule->vcap_chain_id >= SPARX5_VCAP_CID_IS0_L4 &&
		  rule->vcap_chain_id < SPARX5_VCAP_CID_IS0_L5));
}

static bool sparx5_vcap_is2_is_first_chain(struct vcap_rule *rule)
{
	return (rule->vcap_chain_id >= SPARX5_VCAP_CID_IS2_L0 &&
		rule->vcap_chain_id < SPARX5_VCAP_CID_IS2_L1) ||
		((rule->vcap_chain_id >= SPARX5_VCAP_CID_IS2_L2 &&
		  rule->vcap_chain_id < SPARX5_VCAP_CID_IS2_L3));
}

static bool sparx5_vcap_es2_is_first_chain(struct vcap_rule *rule)
{
	return (rule->vcap_chain_id >= SPARX5_VCAP_CID_ES2_L0 &&
		rule->vcap_chain_id < SPARX5_VCAP_CID_ES2_L1);
}

/* Set the wide range (narrow) port mask on a rule */
static void sparx5_vcap_add_range_port_mask(struct vcap_rule *rule,
					    struct net_device *ndev)
{
	struct sparx5_port *port = netdev_priv(ndev);
	int width = 32;
	u32 port_mask;
	u32 range;

	range = port->portno / width;
	/* Port bit set to match-any */
	port_mask = ~BIT(port->portno % width);
	vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK_SEL, 0, 0xf);
	vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK_RNG, range, 0xf);
	vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, 0, port_mask);
}

/* Set the wide ingress port mask on a rule */
static void sparx5_vcap_add_wide_port_mask(struct vcap_rule *rule,
					   struct net_device *ndev)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct vcap_u72_key port_mask;
	u32 range;

	/* Port bit set to match-any */
	memset(port_mask.value, 0, sizeof(port_mask.value));
	memset(port_mask.mask, 0xff, sizeof(port_mask.mask));
	range = port->portno / 8;
	port_mask.mask[range] = ~BIT(port->portno % 8);
	vcap_rule_add_key_u72(rule, VCAP_KF_IF_IGR_PORT_MASK, &port_mask);
}

static void sparx5_vcap_add_is0_default_fields(struct sparx5 *sparx5,
					       struct vcap_admin *admin,
					       struct vcap_rule *rule,
					       struct net_device *ndev)
{
	const struct vcap_field *field;

	field = vcap_lookup_keyfield(rule, VCAP_KF_IF_IGR_PORT_MASK);
	if (field && field->width == 65)
		sparx5_vcap_add_wide_port_mask(rule, ndev);
	else if (field && field->width == 32)
		sparx5_vcap_add_range_port_mask(rule, ndev);
	else
		pr_err("%s:%d: %s: could not add an ingress port mask for: %s\n",
		       __func__, __LINE__, netdev_name(ndev),
		       sparx5_vcap_keyset_name(ndev, rule->keyset));
	/* The supported keysets below must match the configuration
	 * in the sparx5_vcap_is0_port_cfg table
	 */
	switch (rule->keyset) {
	case VCAP_KFS_NORMAL_7TUPLE:
	case VCAP_KFS_NORMAL_5TUPLE_IP4:
		if (sparx5_vcap_is0_is_first_chain(rule))
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_1);
		else
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_0);
		/* Add any default actions */
		break;
	default:
		pr_err("%s:%d: %s - missing default handling\n",
		       __func__, __LINE__,
		       sparx5_vcap_keyset_name(ndev, rule->keyset));
		break;
	}
}

static void sparx5_vcap_add_is2_default_fields(struct sparx5 *sparx5,
					       struct vcap_admin *admin,
					       struct vcap_rule *rule,
					       struct net_device *ndev)
{
	struct vcap_client_actionfield *af;
	const struct vcap_field *field;

	field = vcap_lookup_keyfield(rule, VCAP_KF_IF_IGR_PORT_MASK);
	if (field && field->width == 65)
		sparx5_vcap_add_wide_port_mask(rule, ndev);
	else if (field && field->width == 32)
		sparx5_vcap_add_range_port_mask(rule, ndev);
	else
		pr_err("%s:%d: %s: could not add an ingress port mask for: %s\n",
		       __func__, __LINE__, netdev_name(ndev),
		       sparx5_vcap_keyset_name(ndev, rule->keyset));
	/* The supported keysets below must match the configuration
	 * in the sparx5_vcap_is2_port_cfg table
	 */
	switch (rule->keyset) {
	case VCAP_KFS_MAC_ETYPE:
	case VCAP_KFS_IP4_TCP_UDP:
	case VCAP_KFS_IP4_OTHER:
	case VCAP_KFS_ARP:
	case VCAP_KFS_IP_7TUPLE:
	case VCAP_KFS_IP6_STD:
		if (sparx5_vcap_is2_is_first_chain(rule))
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_1);
		else
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_0);
		/* Add any default actions */
		break;
	default:
		pr_err("%s:%d: %s - missing default handling\n",
		       __func__, __LINE__,
		       sparx5_vcap_keyset_name(ndev, rule->keyset));
		break;
	}
	/* Find any rule counter id and store it in the rule information */
	af = vcap_find_actionfield(rule, VCAP_AF_CNT_ID);
	field = vcap_lookup_actionfield(rule, VCAP_AF_CNT_ID);
	if (af && field && field->type == VCAP_FIELD_U32)
		vcap_rule_set_counter_id(rule, af->data.u32.value);
}

static void sparx5_vcap_add_es2_default_fields(struct sparx5 *sparx5,
					       struct vcap_admin *admin,
					       struct vcap_rule *rule,
					       struct net_device *ndev)
{
	struct vcap_client_actionfield *af;
	const struct vcap_field *field;

	/* The supported keysets below must match the configuration
	 * in the sparx5_vcap_es2_port_cfg table
	 */
	switch (rule->keyset) {
	case VCAP_KFS_MAC_ETYPE:
	case VCAP_KFS_IP4_TCP_UDP:
	case VCAP_KFS_IP6_STD:
	case VCAP_KFS_ARP:
	case VCAP_KFS_IP_7TUPLE:
		if (sparx5_vcap_es2_is_first_chain(rule))
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_1);
		else
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_0);
		/* Add any default actions */
		break;
	default:
		pr_err("%s:%d: %s - missing default handling\n",
		       __func__, __LINE__,
		       sparx5_vcap_keyset_name(ndev, rule->keyset));
		break;
	}
	/* Find any rule counter id and store it in the rule information */
	af = vcap_find_actionfield(rule, VCAP_AF_CNT_ID);
	field = vcap_lookup_actionfield(rule, VCAP_AF_CNT_ID);
	if (af && field && field->type == VCAP_FIELD_U32)
		vcap_rule_set_counter_id(rule, af->data.u32.value);
}

/* Initializing a VCAP address range */
static void _sparx5_vcap_range_init(struct sparx5 *sparx5,
				   struct vcap_admin *admin, u32 addr, u32 count)
{
	u32 size = count - 1;
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		pr_debug("%s:%d: type: %d, addr: %u, count: %d, size: %u\n",
			 __func__, __LINE__,
			 admin->vtype, addr, count, size);
		spx5_wr(VCAP_ES0_CFG_MV_NUM_POS_SET(0) |
				VCAP_ES0_CFG_MV_SIZE_SET(size),
			sparx5, VCAP_ES0_CFG);
		spx5_wr(VCAP_ES0_CTRL_UPDATE_CMD_SET(VCAP_CMD_INITIALIZE) |
				VCAP_ES0_CTRL_UPDATE_ENTRY_DIS_SET(0) |
				VCAP_ES0_CTRL_UPDATE_ACTION_DIS_SET(0) |
				VCAP_ES0_CTRL_UPDATE_CNT_DIS_SET(0) |
				VCAP_ES0_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_ES0_CTRL_CLEAR_CACHE_SET(true) |
				VCAP_ES0_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_ES0_CTRL);
		sparx5_vcap_wait_es0_update(sparx5);
		break;
	case VCAP_TYPE_ES2:
		pr_debug("%s:%d: type: %d, addr: %u, count: %d, size: %u\n",
			 __func__, __LINE__,
			 admin->vtype, addr, count, size);
		spx5_wr(VCAP_ES2_CFG_MV_NUM_POS_SET(0) |
				VCAP_ES2_CFG_MV_SIZE_SET(size),
			sparx5, VCAP_ES2_CFG);
		spx5_wr(VCAP_ES2_CTRL_UPDATE_CMD_SET(VCAP_CMD_INITIALIZE) |
				VCAP_ES2_CTRL_UPDATE_ENTRY_DIS_SET(0) |
				VCAP_ES2_CTRL_UPDATE_ACTION_DIS_SET(0) |
				VCAP_ES2_CTRL_UPDATE_CNT_DIS_SET(0) |
				VCAP_ES2_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_ES2_CTRL_CLEAR_CACHE_SET(true) |
				VCAP_ES2_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_ES2_CTRL);
		sparx5_vcap_wait_es2_update(sparx5);
		break;
	case VCAP_TYPE_IS0:
	case VCAP_TYPE_IS2:
		pr_debug("%s:%d: type: %d, addr: %u, count: %d, size: %u\n",
			 __func__, __LINE__,
			 admin->vtype, addr, count, size);
		spx5_wr(VCAP_SUPER_CFG_MV_NUM_POS_SET(0) |
				VCAP_SUPER_CFG_MV_SIZE_SET(size),
			sparx5, VCAP_SUPER_CFG);
		spx5_wr(VCAP_SUPER_CTRL_UPDATE_CMD_SET(VCAP_CMD_INITIALIZE) |
				VCAP_SUPER_CTRL_UPDATE_ENTRY_DIS_SET(0) |
				VCAP_SUPER_CTRL_UPDATE_ACTION_DIS_SET(0) |
				VCAP_SUPER_CTRL_UPDATE_CNT_DIS_SET(0) |
				VCAP_SUPER_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_SUPER_CTRL_CLEAR_CACHE_SET(true) |
				VCAP_SUPER_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_SUPER_CTRL);
		sparx5_vcap_wait_super_update(sparx5);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

static void sparx5_vcap_block_init(struct sparx5 *sparx5, struct vcap_admin *admin)
{
	_sparx5_vcap_range_init(sparx5, admin, admin->first_valid_addr,
				admin->last_valid_addr - admin->first_valid_addr);
}

/* API callback used for validating a field keyset (check the port keysets )*/
static enum vcap_keyfield_set
sparx5_vcap_validate_keyset(struct net_device *ndev,
			    struct vcap_admin *admin,
			    struct vcap_rule *rule,
			    struct vcap_keyset_list *kslist,
			    u16 l3_proto)
{
	struct vcap_keyset_list keysetlist = {0};
	enum vcap_keyfield_set keysets[12] = {0};
	int idx, jdx, lookup;

	/* Get the key selection for the (vcap, port, lookup) and compare with
	 * the suggested set, return an error of there is no match
	 * - IS0: 0-2: ANA_CL:PORT[0-69]:ADV_CL_CFG[0-5] (3 instances with first
	 *   and second)
	 * - IS2: 0-1: ANA_ACL:KEY_SEL[0-133]:VCAP_S2_KEY_SEL[0-3] (2 instances
	 *   with first and second)
	 * - ES0: REW:COMMON:RTAG_ETAG_CTRL[0-69].ES0_ISDX_KEY_ENA
	 * - ES2: EACL:ES2_KEY_SELECT_PROFILE[0-137]:VCAP_ES2_KEY_SEL[0-1]
	 * - LPM: no port keys but ANA_L3:COMMON:ROUTING_CFG and
	 *   ANA_L3:COMMON:ROUTING_CFG2 controls generation of keys in general
	 * - IP6PFX: no port keys
	 * - ES0: no port keys
	 */
	pr_debug("%s:%d: %d sets\n", __func__, __LINE__, kslist->cnt);
	lookup = sparx5_vcap_cid_to_lookup(admin, rule->vcap_chain_id);

	keysetlist.max = ARRAY_SIZE(keysets);
	keysetlist.keysets = keysets;

	switch (admin->vtype) {
	case VCAP_TYPE_IS0:
		sparx5_vcap_is0_get_port_keysets(ndev, lookup, &keysetlist, l3_proto);
		break;
	case VCAP_TYPE_IS2:
		sparx5_vcap_is2_get_port_keysets(ndev, lookup, &keysetlist, l3_proto);
		break;
	case VCAP_TYPE_ES0:
		sparx5_vcap_es0_get_port_keysets(ndev, &keysetlist);
		break;
	case VCAP_TYPE_ES2:
		sparx5_vcap_es2_get_port_keysets(ndev, lookup, &keysetlist, l3_proto);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
	/* Check if there is a match and return the match */
	for (idx = 0; idx < kslist->cnt; ++idx)
		for (jdx = 0; jdx < keysetlist.cnt; ++jdx)
			if (kslist->keysets[idx] == keysets[jdx]) {
				pr_debug("%s:%d: keyset [%d]: %s\n",
					 __func__, __LINE__,
					 kslist->keysets[idx],
					 sparx5_vcap_keyset_name(ndev, kslist->keysets[idx]));
				return kslist->keysets[idx];
			}
	pr_err("%s:%d: %s not supported in port key selection\n",
	       __func__, __LINE__,
	       sparx5_vcap_keyset_name(ndev, kslist->keysets[0]));
	return -ENOENT;
}

/* API callback used for adding default fields to a rule */
static void sparx5_vcap_add_default_fields(struct net_device *ndev,
					   struct vcap_admin *admin,
					   struct vcap_rule *rule)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	struct vcap_client_actionfield *af;
	const struct vcap_field *field;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		/* Find any ESDX rule counter id and store it in the rule information */
		af = vcap_find_actionfield(rule, VCAP_AF_ESDX);
		field = vcap_lookup_actionfield(rule, VCAP_AF_ESDX);
		if (af && field && field->type == VCAP_FIELD_U32)
			vcap_rule_set_counter_id(rule, af->data.u32.value);
		break;
	case VCAP_TYPE_ES2:
		sparx5_vcap_add_es2_default_fields(sparx5, admin, rule, ndev);
		break;
	case VCAP_TYPE_IS0:
		sparx5_vcap_add_is0_default_fields(sparx5, admin, rule, ndev);
		break;
	case VCAP_TYPE_IS2:
		sparx5_vcap_add_is2_default_fields(sparx5, admin, rule, ndev);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

/* API callback used for erasing the vcap cache area (not the register area) */
static void sparx5_vcap_cache_erase(struct vcap_admin *admin)
{
	memset(admin->cache.keystream, 0, STREAMSIZE);
	memset(admin->cache.maskstream, 0, STREAMSIZE);
	memset(admin->cache.actionstream, 0, STREAMSIZE);
	memset(&admin->cache.counter, 0, sizeof(admin->cache.counter));
}

/* API callback used for writing to the VCAP cache */
static void sparx5_vcap_cache_write(struct net_device *ndev, struct vcap_admin *admin,
				    enum vcap_selection sel, u32 start,
				    u32 count)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	u32 *keystr, *mskstr, *actstr;
	int idx;

	keystr = &admin->cache.keystream[start];
	mskstr = &admin->cache.maskstream[start];
	actstr = &admin->cache.actionstream[start];
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		switch (sel) {
		case VCAP_SEL_ENTRY:
			for (idx = 0; idx < count; ++idx) {
				/* Avoid 'match-off' by setting value & mask */
				spx5_wr(keystr[idx] & mskstr[idx], sparx5,
					VCAP_ES0_VCAP_ENTRY_DAT(idx));
				spx5_wr(~mskstr[idx], sparx5,
					VCAP_ES0_VCAP_MASK_DAT(idx));
			}
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
					 __func__, __LINE__,
					 start + idx, keystr[idx], ~mskstr[idx]);
			}
			break;
		case VCAP_SEL_ACTION:
			for (idx = 0; idx < count; ++idx)
				spx5_wr(actstr[idx], sparx5,
					VCAP_ES0_VCAP_ACTION_DAT(idx));
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: actdata[%02d]: 0x%08x\n",
					 __func__, __LINE__,
					 start + idx, actstr[idx]);
			}
			break;
		case VCAP_SEL_ALL:
			pr_err("%s:%d: cannot write all streams at once\n",
			       __func__, __LINE__);
			break;
		default:
			break;
		}
		break;
	case VCAP_TYPE_ES2:
		switch (sel) {
		case VCAP_SEL_ENTRY:
			for (idx = 0; idx < count; ++idx) {
				/* Avoid 'match-off' by setting value & mask */
				spx5_wr(keystr[idx] & mskstr[idx], sparx5,
					VCAP_ES2_VCAP_ENTRY_DAT(idx));
				spx5_wr(~mskstr[idx], sparx5,
					VCAP_ES2_VCAP_MASK_DAT(idx));
			}
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
					 __func__, __LINE__,
					 start + idx, keystr[idx], ~mskstr[idx]);
			}
			break;
		case VCAP_SEL_ACTION:
			for (idx = 0; idx < count; ++idx)
				spx5_wr(actstr[idx], sparx5,
					VCAP_ES2_VCAP_ACTION_DAT(idx));
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: actdata[%02d]: 0x%08x\n",
					 __func__, __LINE__,
					 start + idx, actstr[idx]);
			}
			break;
		case VCAP_SEL_ALL:
			pr_err("%s:%d: cannot write all streams at once\n",
			       __func__, __LINE__);
			break;
		default:
			break;
		}
		break;
	case VCAP_TYPE_IS0:
	case VCAP_TYPE_IS2:
		switch (sel) {
		case VCAP_SEL_ENTRY:
			for (idx = 0; idx < count; ++idx) {
				/* Avoid 'match-off' by setting value & mask */
				spx5_wr(keystr[idx] & mskstr[idx], sparx5,
					VCAP_SUPER_VCAP_ENTRY_DAT(idx));
				spx5_wr(~mskstr[idx], sparx5,
					VCAP_SUPER_VCAP_MASK_DAT(idx));
			}
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
					 __func__, __LINE__,
					 start + idx, keystr[idx], ~mskstr[idx]);
			}
			break;
		case VCAP_SEL_ACTION:
			for (idx = 0; idx < count; ++idx)
				spx5_wr(actstr[idx], sparx5,
					VCAP_SUPER_VCAP_ACTION_DAT(idx));
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: actdata[%02d]: 0x%08x\n",
					 __func__, __LINE__,
					 start + idx, actstr[idx]);
			}
			break;
		case VCAP_SEL_ALL:
			pr_err("%s:%d: cannot write all streams at once\n",
			       __func__, __LINE__);
			break;
		default:
			break;
		}
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		return;
	}
	if (sel & VCAP_SEL_COUNTER) {
		switch (admin->vtype) {
		case VCAP_TYPE_ES0:
			pr_debug("%s:%d: cnt[%d] = %d, sticky = %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			spx5_wr(admin->cache.counter, sparx5, VCAP_ES0_VCAP_CNT_DAT(0));
			/* use ESDX counters located in the XQS */
			sparx5_es0_write_esdx_counter(sparx5, admin, start);
			break;
		case VCAP_TYPE_ES2:
			start = start & 0x7ff; /* counter limit */
			pr_debug("%s:%d: cnt[%d] = %d, sticky = %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			spx5_wr(admin->cache.counter, sparx5, EACL_ES2_CNT(start));
			spx5_wr(admin->cache.sticky, sparx5, VCAP_ES2_VCAP_CNT_DAT(0));
			break;
		case VCAP_TYPE_IS0:
			pr_debug("%s:%d: cnt[%d] = %d, sticky = %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			spx5_wr(admin->cache.counter, sparx5, VCAP_SUPER_VCAP_CNT_DAT(0));
			break;
		case VCAP_TYPE_IS2:
			start = start & 0xfff; /* counter limit */
			pr_debug("%s:%d: cnt[%d] = %d, sticky = %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			if (admin->vinst == 0)
				spx5_wr(admin->cache.counter, sparx5, ANA_ACL_CNT_A(start));
			else
				spx5_wr(admin->cache.counter, sparx5, ANA_ACL_CNT_B(start));
			spx5_wr(admin->cache.sticky, sparx5, VCAP_SUPER_VCAP_CNT_DAT(0));
			break;
		default:
			pr_err("%s:%d: vcap type: %d not supported\n",
			       __func__, __LINE__, admin->vtype);
			break;
		}
	}
}

/* API callback used for reading from the VCAP into the VCAP cache */
static void sparx5_vcap_cache_read(struct net_device *ndev, struct vcap_admin *admin,
				   enum vcap_selection sel, u32 start,
				   u32 count)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	u32 *keystr, *mskstr, *actstr;
	int idx;

	keystr = &admin->cache.keystream[start];
	mskstr = &admin->cache.maskstream[start];
	actstr = &admin->cache.actionstream[start];
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		if (sel & VCAP_SEL_ENTRY) {
			for (idx = 0; idx < count; ++idx) {
				keystr[idx] = spx5_rd(
					sparx5, VCAP_ES0_VCAP_ENTRY_DAT(idx));
				mskstr[idx] = ~spx5_rd(
					sparx5, VCAP_ES0_VCAP_MASK_DAT(idx));
			}
			for (idx = 0; idx < count; ++idx)
				pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
					 __func__, __LINE__,
					 start + idx, keystr[idx], ~mskstr[idx]);
		}
		if (sel & VCAP_SEL_ACTION) {
			for (idx = 0; idx < count; ++idx)
				actstr[idx] = spx5_rd(
					sparx5, VCAP_ES0_VCAP_ACTION_DAT(idx));
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: actdata[%02d]: 0x%08x\n",
					 __func__, __LINE__,
					 start + idx, actstr[idx]);
			}
		}
		break;
	case VCAP_TYPE_ES2:
		if (sel & VCAP_SEL_ENTRY) {
			for (idx = 0; idx < count; ++idx) {
				keystr[idx] = spx5_rd(
					sparx5, VCAP_ES2_VCAP_ENTRY_DAT(idx));
				mskstr[idx] = ~spx5_rd(
					sparx5, VCAP_ES2_VCAP_MASK_DAT(idx));
			}
			for (idx = 0; idx < count; ++idx)
				pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
					 __func__, __LINE__,
					 start + idx, keystr[idx], ~mskstr[idx]);
		}
		if (sel & VCAP_SEL_ACTION) {
			for (idx = 0; idx < count; ++idx)
				actstr[idx] = spx5_rd(
					sparx5, VCAP_ES2_VCAP_ACTION_DAT(idx));
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: actdata[%02d]: 0x%08x\n",
					 __func__, __LINE__,
					 start + idx, actstr[idx]);
			}
		}
		break;
	case VCAP_TYPE_IS0:
	case VCAP_TYPE_IS2:
		if (sel & VCAP_SEL_ENTRY) {
			for (idx = 0; idx < count; ++idx) {
				keystr[idx] = spx5_rd(
					sparx5, VCAP_SUPER_VCAP_ENTRY_DAT(idx));
				mskstr[idx] = ~spx5_rd(
					sparx5, VCAP_SUPER_VCAP_MASK_DAT(idx));
			}
			for (idx = 0; idx < count; ++idx)
				pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
					 __func__, __LINE__,
					 start + idx, keystr[idx], ~mskstr[idx]);
		}
		if (sel & VCAP_SEL_ACTION) {
			for (idx = 0; idx < count; ++idx)
				actstr[idx] = spx5_rd(
					sparx5, VCAP_SUPER_VCAP_ACTION_DAT(idx));
			for (idx = 0; idx < count; ++idx) {
				pr_debug("%s:%d: actdata[%02d]: 0x%08x\n",
					 __func__, __LINE__,
					 start + idx, actstr[idx]);
			}
		}
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		return;
	}
	if (sel & VCAP_SEL_COUNTER) {
		switch (admin->vtype) {
		case VCAP_TYPE_ES0:
			admin->cache.counter =
				spx5_rd(sparx5, VCAP_ES0_VCAP_CNT_DAT(0));
			admin->cache.sticky = admin->cache.counter;
			pr_debug("%s:%d: cnt[%d]: %u, sticky: %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			/* use ESDX counters located in the XQS */
			sparx5_es0_read_esdx_counter(sparx5, admin, start);
			break;
		case VCAP_TYPE_ES2:
			start = start & 0x7ff; /* counter limit */
			admin->cache.counter =
				spx5_rd(sparx5, EACL_ES2_CNT(start));
			admin->cache.sticky =
				spx5_rd(sparx5, VCAP_ES2_VCAP_CNT_DAT(0));
			pr_debug("%s:%d: cnt[%d]: %u, sticky: %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			break;
		case VCAP_TYPE_IS0:
			admin->cache.counter =
				spx5_rd(sparx5, VCAP_SUPER_VCAP_CNT_DAT(0));
			admin->cache.sticky =
				spx5_rd(sparx5, VCAP_SUPER_VCAP_CNT_DAT(0));
			pr_debug("%s:%d: cnt[%d]: %u, sticky: %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			break;
		case VCAP_TYPE_IS2:
			start = start & 0xfff; /* counter limit */
			if (admin->vinst == 0)
				admin->cache.counter =
					spx5_rd(sparx5, ANA_ACL_CNT_A(start));
			else
				admin->cache.counter =
					spx5_rd(sparx5, ANA_ACL_CNT_B(start));
			admin->cache.sticky =
				spx5_rd(sparx5, VCAP_SUPER_VCAP_CNT_DAT(0));
			pr_debug("%s:%d: cnt[%d]: %u, sticky: %d\n",
				 __func__, __LINE__,
				 start, admin->cache.counter, admin->cache.sticky);
			break;
		default:
			break;
		}
	}
}

/* API callback used for initializing a VCAP address range */
static void sparx5_vcap_range_init(struct net_device *ndev, struct vcap_admin *admin,
				   u32 addr, u32 count)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;

	_sparx5_vcap_range_init(sparx5, admin, addr, count);
}

/* API callback used for updating the VCAP cache */
static void sparx5_vcap_update(struct net_device *ndev, struct vcap_admin *admin,
			       enum vcap_command cmd, enum vcap_selection sel,
			       u32 addr)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	char *cmdstr, *selstr;
	bool clear;

	switch (cmd) {
	case VCAP_CMD_WRITE: cmdstr = "write"; break;
	case VCAP_CMD_READ: cmdstr = "read"; break;
	case VCAP_CMD_MOVE_DOWN: cmdstr = "move_down"; break;
	case VCAP_CMD_MOVE_UP: cmdstr = "move_up"; break;
	case VCAP_CMD_INITIALIZE: cmdstr = "init"; break;
	}
	switch (sel) {
	case VCAP_SEL_ENTRY: selstr = "entry"; break;
	case VCAP_SEL_ACTION: selstr = "action"; break;
	case VCAP_SEL_COUNTER: selstr = "counter"; break;
	case VCAP_SEL_ALL: selstr = "all"; break;
	}
	pr_debug("%s:%d: %s %s: addr: %d\n", __func__, __LINE__, cmdstr, selstr,
		addr);
	clear = (cmd == VCAP_CMD_INITIALIZE);
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		spx5_wr(VCAP_ES0_CFG_MV_NUM_POS_SET(0) |
			VCAP_ES0_CFG_MV_SIZE_SET(0), sparx5, VCAP_ES0_CFG);
		spx5_wr(VCAP_ES0_CTRL_UPDATE_CMD_SET(cmd) |
				VCAP_ES0_CTRL_UPDATE_ENTRY_DIS_SET(
					!(VCAP_SEL_ENTRY & sel)) |
				VCAP_ES0_CTRL_UPDATE_ACTION_DIS_SET(
					!(VCAP_SEL_ACTION & sel)) |
				VCAP_ES0_CTRL_UPDATE_CNT_DIS_SET(
					!(VCAP_SEL_COUNTER & sel)) |
				VCAP_ES0_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_ES0_CTRL_CLEAR_CACHE_SET(clear) |
				VCAP_ES0_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_ES0_CTRL);
		sparx5_vcap_wait_es0_update(sparx5);
		break;
	case VCAP_TYPE_ES2:
		spx5_wr(VCAP_ES2_CFG_MV_NUM_POS_SET(0) |
			VCAP_ES2_CFG_MV_SIZE_SET(0), sparx5, VCAP_ES2_CFG);
		spx5_wr(VCAP_ES2_CTRL_UPDATE_CMD_SET(cmd) |
				VCAP_ES2_CTRL_UPDATE_ENTRY_DIS_SET(
					!(VCAP_SEL_ENTRY & sel)) |
				VCAP_ES2_CTRL_UPDATE_ACTION_DIS_SET(
					!(VCAP_SEL_ACTION & sel)) |
				VCAP_ES2_CTRL_UPDATE_CNT_DIS_SET(
					!(VCAP_SEL_COUNTER & sel)) |
				VCAP_ES2_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_ES2_CTRL_CLEAR_CACHE_SET(clear) |
				VCAP_ES2_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_ES2_CTRL);
		sparx5_vcap_wait_es2_update(sparx5);
		break;
	case VCAP_TYPE_IS0:
	case VCAP_TYPE_IS2:
		spx5_wr(VCAP_SUPER_CFG_MV_NUM_POS_SET(0) |
			VCAP_SUPER_CFG_MV_SIZE_SET(0), sparx5, VCAP_SUPER_CFG);
		spx5_wr(VCAP_SUPER_CTRL_UPDATE_CMD_SET(cmd) |
				VCAP_SUPER_CTRL_UPDATE_ENTRY_DIS_SET(
					!(VCAP_SEL_ENTRY & sel)) |
				VCAP_SUPER_CTRL_UPDATE_ACTION_DIS_SET(
					!(VCAP_SEL_ACTION & sel)) |
				VCAP_SUPER_CTRL_UPDATE_CNT_DIS_SET(
					!(VCAP_SEL_COUNTER & sel)) |
				VCAP_SUPER_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_SUPER_CTRL_CLEAR_CACHE_SET(clear) |
				VCAP_SUPER_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_SUPER_CTRL);
		sparx5_vcap_wait_super_update(sparx5);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

/* API callback used for moving a block of rules in the VCAP */
static void sparx5_vcap_move(struct net_device *ndev, struct vcap_admin *admin,
			     u32 addr, int offset, int count)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	const char *dir;
	enum vcap_command cmd;
	u16 mv_num_pos;
	u16 mv_size;

	mv_size = count - 1;
	if (offset > 0) {
		mv_num_pos = offset - 1;
		cmd = VCAP_CMD_MOVE_DOWN;
		dir = "down";
	} else {
		mv_num_pos = -offset - 1;
		cmd = VCAP_CMD_MOVE_UP;
		dir = "up";
	}
	pr_debug("%s:%d: move: addr: %u, offset: %d, count: %d, mv_num_pos: %u, mv_size: %u, dir: %s\n",
		 __func__, __LINE__, addr, offset, count, mv_num_pos, mv_size, dir);
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		spx5_wr(VCAP_ES0_CFG_MV_NUM_POS_SET(mv_num_pos) |
				VCAP_ES0_CFG_MV_SIZE_SET(mv_size),
			sparx5, VCAP_ES0_CFG);
		spx5_wr(VCAP_ES0_CTRL_UPDATE_CMD_SET(cmd) |
				VCAP_ES0_CTRL_UPDATE_ENTRY_DIS_SET(0) |
				VCAP_ES0_CTRL_UPDATE_ACTION_DIS_SET(0) |
				VCAP_ES0_CTRL_UPDATE_CNT_DIS_SET(0) |
				VCAP_ES0_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_ES0_CTRL_CLEAR_CACHE_SET(false) |
				VCAP_ES0_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_ES0_CTRL);
		sparx5_vcap_wait_es0_update(sparx5);
		break;
	case VCAP_TYPE_ES2:
		spx5_wr(VCAP_ES2_CFG_MV_NUM_POS_SET(mv_num_pos) |
				VCAP_ES2_CFG_MV_SIZE_SET(mv_size),
			sparx5, VCAP_ES2_CFG);
		spx5_wr(VCAP_ES2_CTRL_UPDATE_CMD_SET(cmd) |
				VCAP_ES2_CTRL_UPDATE_ENTRY_DIS_SET(0) |
				VCAP_ES2_CTRL_UPDATE_ACTION_DIS_SET(0) |
				VCAP_ES2_CTRL_UPDATE_CNT_DIS_SET(0) |
				VCAP_ES2_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_ES2_CTRL_CLEAR_CACHE_SET(false) |
				VCAP_ES2_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_ES2_CTRL);
		sparx5_vcap_wait_es2_update(sparx5);
		break;
	case VCAP_TYPE_IS0:
	case VCAP_TYPE_IS2:
		spx5_wr(VCAP_SUPER_CFG_MV_NUM_POS_SET(mv_num_pos) |
				VCAP_SUPER_CFG_MV_SIZE_SET(mv_size),
			sparx5, VCAP_SUPER_CFG);
		spx5_wr(VCAP_SUPER_CTRL_UPDATE_CMD_SET(cmd) |
				VCAP_SUPER_CTRL_UPDATE_ENTRY_DIS_SET(0) |
				VCAP_SUPER_CTRL_UPDATE_ACTION_DIS_SET(0) |
				VCAP_SUPER_CTRL_UPDATE_CNT_DIS_SET(0) |
				VCAP_SUPER_CTRL_UPDATE_ADDR_SET(addr) |
				VCAP_SUPER_CTRL_CLEAR_CACHE_SET(false) |
				VCAP_SUPER_CTRL_UPDATE_SHOT_SET(true),
			sparx5, VCAP_SUPER_CTRL);
		sparx5_vcap_wait_super_update(sparx5);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

/* Provide port information via a callback interface */
static int sparx5_port_info(struct net_device *ndev, enum vcap_type vtype,
			    int (*pf)(void *out, int arg, const char *fmt, ...),
			    void *out, int arg)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	struct vcap_control *ctrl;
	struct vcap_admin *admin;

	ctrl = sparx5->vcap_ctrl;
	list_for_each_entry(admin, &ctrl->list, list) {
		if (admin->vinst)
			continue;
		if (admin->vtype == vtype)
			return sparx5_vcap_port_info(sparx5, admin, pf, out, arg);
	}
	pf(out, arg, "VCAP not supported\n");
	return 0;
}

/* API callback operations */
static struct vcap_operations sparx5_vcap_ops = {
	.validate_keyset = sparx5_vcap_validate_keyset,
	.add_default_fields = sparx5_vcap_add_default_fields,
	.cache_erase = sparx5_vcap_cache_erase,
	.cache_write = sparx5_vcap_cache_write,
	.cache_read = sparx5_vcap_cache_read,
	.init = sparx5_vcap_range_init,
	.update = sparx5_vcap_update,
	.move = sparx5_vcap_move,
	.port_info = sparx5_port_info,
};

static u32 sparx5_vcap_is0_port_key_selection(int lookup)
{
	return ANA_CL_ADV_CL_CFG_LOOKUP_ENA_SET(1) |
		ANA_CL_ADV_CL_CFG_ETYPE_CLM_KEY_SEL_SET(
			sparx5_vcap_is0_keyset_to_portsel(
				VCAP_IS0_PTC_ETYPE,
				sparx5_vcap_is0_port_cfg[lookup][VCAP_IS0_PTC_ETYPE])) |
		ANA_CL_ADV_CL_CFG_IP4_CLM_KEY_SEL_SET(
			sparx5_vcap_is0_keyset_to_portsel(
				VCAP_IS0_PTC_IPV4,
				sparx5_vcap_is0_port_cfg[lookup][VCAP_IS0_PTC_IPV4])) |
		ANA_CL_ADV_CL_CFG_IP6_CLM_KEY_SEL_SET(
			sparx5_vcap_is0_keyset_to_portsel(
				VCAP_IS0_PTC_IPV6,
				sparx5_vcap_is0_port_cfg[lookup][VCAP_IS0_PTC_IPV6])) |
		ANA_CL_ADV_CL_CFG_MPLS_UC_CLM_KEY_SEL_SET(
			sparx5_vcap_is0_keyset_to_portsel(
				VCAP_IS0_PTC_MPLS_UC,
				sparx5_vcap_is0_port_cfg[lookup][VCAP_IS0_PTC_MPLS_UC])) |
		ANA_CL_ADV_CL_CFG_MPLS_MC_CLM_KEY_SEL_SET(
			sparx5_vcap_is0_keyset_to_portsel(
				VCAP_IS0_PTC_MPLS_MC,
				sparx5_vcap_is0_port_cfg[lookup][VCAP_IS0_PTC_MPLS_MC])) |
		ANA_CL_ADV_CL_CFG_MLBS_CLM_KEY_SEL_SET(
			sparx5_vcap_is0_keyset_to_portsel(
				VCAP_IS0_PTC_MPLS_LS,
				sparx5_vcap_is0_port_cfg[lookup][VCAP_IS0_PTC_MPLS_LS]));
}

static u32 sparx5_vcap_is2_port_key_selection(int lookup)
{
	/* Disable unused lookups */
	if (sparx5_vcap_is2_keyset_to_portsel(
				VCAP_IS2_PTC_NONETH,
				sparx5_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_NONETH])
				== VCAP_IS2_PS_NONETH_NO_LOOKUP)
		return 0;
	return ANA_ACL_VCAP_S2_KEY_SEL_KEY_SEL_ENA_SET(true) |
		ANA_ACL_VCAP_S2_KEY_SEL_IGR_PORT_MASK_SEL_SET(
			VCAP_IS2_PS_L2_INFO_IN_IGR_PORT_MASK) |
		ANA_ACL_VCAP_S2_KEY_SEL_NON_ETH_KEY_SEL_SET(
			sparx5_vcap_is2_keyset_to_portsel(
				VCAP_IS2_PTC_NONETH,
				sparx5_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_NONETH])) |
		ANA_ACL_VCAP_S2_KEY_SEL_IP4_MC_KEY_SEL_SET(
			sparx5_vcap_is2_keyset_to_portsel(
				VCAP_IS2_PTC_IPV4_MC,
				sparx5_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_IPV4_MC])) |
		ANA_ACL_VCAP_S2_KEY_SEL_IP4_UC_KEY_SEL_SET(
			sparx5_vcap_is2_keyset_to_portsel(
				VCAP_IS2_PTC_IPV4_UC,
				sparx5_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_IPV4_UC])) |
		ANA_ACL_VCAP_S2_KEY_SEL_IP6_MC_KEY_SEL_SET(
			sparx5_vcap_is2_keyset_to_portsel(
				VCAP_IS2_PTC_IPV6_MC,
				sparx5_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_IPV6_MC])) |
		ANA_ACL_VCAP_S2_KEY_SEL_IP6_UC_KEY_SEL_SET(
			sparx5_vcap_is2_keyset_to_portsel(
				VCAP_IS2_PTC_IPV6_UC,
				sparx5_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_IPV6_UC])) |
		ANA_ACL_VCAP_S2_KEY_SEL_ARP_KEY_SEL_SET(
			sparx5_vcap_is2_keyset_to_portsel(
			VCAP_IS2_PTC_ARP,
			sparx5_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_ARP]));
}

static u32 sparx5_vcap_es2_port_key_selection(int lookup)
{
	return EACL_VCAP_ES2_KEY_SEL_KEY_ENA_SET(1) |
		EACL_VCAP_ES2_KEY_SEL_ARP_KEY_SEL_SET(
			sparx5_vcap_es2_keyset_to_portsel(
				VCAP_ES2_PTC_ARP,
				sparx5_vcap_es2_port_cfg[lookup][VCAP_ES2_PTC_ARP])) |
		EACL_VCAP_ES2_KEY_SEL_IP4_KEY_SEL_SET(
			sparx5_vcap_es2_keyset_to_portsel(
				VCAP_ES2_PTC_IPV4,
				sparx5_vcap_es2_port_cfg[lookup][VCAP_ES2_PTC_IPV4])) |
		EACL_VCAP_ES2_KEY_SEL_IP6_KEY_SEL_SET(
			sparx5_vcap_es2_keyset_to_portsel(
				VCAP_ES2_PTC_IPV6,
				sparx5_vcap_es2_port_cfg[lookup][VCAP_ES2_PTC_IPV6]));
}

/* Enable lookups per port and set the keyset generation */
static void sparx5_vcap_port_key_selection(struct sparx5 *sparx5, struct vcap_admin *admin)
{
	int portno;
	int lookup;
	u32 value;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		spx5_rmw(REW_ES0_CTRL_ES0_LU_ENA_SET(1), REW_ES0_CTRL_ES0_LU_ENA,
			 sparx5, REW_ES0_CTRL);
		for (portno = 0; portno < SPX5_PORTS; ++portno) {
			spx5_rmw(REW_RTAG_ETAG_CTRL_ES0_ISDX_KEY_ENA_SET(VCAP_ES0_PS_FORCE_ISDX_LOOKUPS),
				 REW_RTAG_ETAG_CTRL_ES0_ISDX_KEY_ENA,
				 sparx5, REW_RTAG_ETAG_CTRL(portno));
		}
		break;
	case VCAP_TYPE_ES2:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			value = sparx5_vcap_es2_port_key_selection(lookup);
			pr_debug("%s:%d: ES2 portsel: %#08x\n", __func__, __LINE__, value);
			for (portno = 0; portno < SPX5_PORTS; ++portno) {
				spx5_wr(value, sparx5,
					EACL_VCAP_ES2_KEY_SEL(portno, lookup));
			}
		}
		break;
	case VCAP_TYPE_IS0:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			value = sparx5_vcap_is0_port_key_selection(lookup);
			pr_debug("%s:%d: IS0 portsel: %#08x\n", __func__, __LINE__, value);
			for (portno = 0; portno < SPX5_PORTS; ++portno) {
				spx5_wr(value, sparx5,
					ANA_CL_ADV_CL_CFG(portno, lookup));
			}
		}
		break;
	case VCAP_TYPE_IS2:
		for (portno = 0; portno < SPX5_PORTS; ++portno)
			spx5_wr(ANA_ACL_VCAP_S2_CFG_SEC_ENA_SET(0xf), sparx5,
				ANA_ACL_VCAP_S2_CFG(portno));
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			value = sparx5_vcap_is2_port_key_selection(lookup);
			pr_debug("%s:%d: IS2 portsel: %#08x\n", __func__, __LINE__, value);
			for (portno = 0; portno < SPX5_PORTS; ++portno) {
				spx5_wr(value, sparx5,
					ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
			}
		}
		/* Statistics: Use ESDX from ES0 if hit, otherwise no counting */
		spx5_rmw(REW_CNT_CTRL_STAT_MODE_SET(1),
			 REW_CNT_CTRL_STAT_MODE,
			 sparx5,
			 REW_CNT_CTRL);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

static void sparx5_vcap_port_key_deselection(struct sparx5 *sparx5, struct vcap_admin *admin)
{
	int portno, lookup;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		spx5_rmw(REW_ES0_CTRL_ES0_LU_ENA_SET(0), REW_ES0_CTRL_ES0_LU_ENA,
			 sparx5, REW_ES0_CTRL);
		for (portno = 0; portno < SPX5_PORTS; ++portno) {
			spx5_rmw(REW_RTAG_ETAG_CTRL_ES0_ISDX_KEY_ENA_SET(VCAP_ES0_PS_NORMAL_SELECTION),
				 REW_RTAG_ETAG_CTRL_ES0_ISDX_KEY_ENA,
				 sparx5, REW_RTAG_ETAG_CTRL(portno));
		}
		break;
	case VCAP_TYPE_ES2:
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			for (portno = 0; portno < SPX5_PORTS; ++portno) {
				spx5_rmw(EACL_VCAP_ES2_KEY_SEL_KEY_ENA_SET(0),
					 EACL_VCAP_ES2_KEY_SEL_KEY_ENA,
					 sparx5,
					 EACL_VCAP_ES2_KEY_SEL(portno, lookup));
			}
		}
		break;
	case VCAP_TYPE_IS0:
		for (lookup = 0; lookup < admin->lookups; ++lookup)
			for (portno = 0; portno < SPX5_PORTS; ++portno)
				spx5_rmw(ANA_CL_ADV_CL_CFG_LOOKUP_ENA_SET(0),
					 ANA_CL_ADV_CL_CFG_LOOKUP_ENA,
					 sparx5,
					 ANA_CL_ADV_CL_CFG(portno, lookup));
		break;
	case VCAP_TYPE_IS2:
		for (portno = 0; portno < SPX5_PORTS; ++portno)
			spx5_rmw(ANA_ACL_VCAP_S2_CFG_SEC_ENA_SET(0),
				 ANA_ACL_VCAP_S2_CFG_SEC_ENA,
				 sparx5,
				 ANA_ACL_VCAP_S2_CFG(portno));
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

/* Get the port keyset for the vcap lookup */
int sparx5_vcap_get_port_keyset(struct net_device *ndev,
				struct vcap_admin *admin,
				int cid,
				u16 l3_proto,
				struct vcap_keyset_list *keysetlist)
{
	int err = 0;
	int lookup;

	lookup = sparx5_vcap_cid_to_lookup(admin, cid);
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		err = sparx5_vcap_es0_get_port_keysets(ndev, keysetlist);
		break;
	case VCAP_TYPE_ES2:
		err = sparx5_vcap_es2_get_port_keysets(ndev, lookup, keysetlist, l3_proto);
		break;
	case VCAP_TYPE_IS0:
		err = sparx5_vcap_is0_get_port_keysets(ndev, lookup, keysetlist, l3_proto);
		break;
	case VCAP_TYPE_IS2:
		err = sparx5_vcap_is2_get_port_keysets(ndev, lookup, keysetlist, l3_proto);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
	return err;
}

/* Set the port keyset for the vcap lookup */
void sparx5_vcap_set_port_keyset(struct net_device *ndev,
				 struct vcap_admin *admin,
				 int cid,
				 u16 l3_proto,
				 u8 l4_proto,
				 enum vcap_keyfield_set keyset)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	int portno = port->portno;
	int lookup;
	u32 value;

	lookup = sparx5_vcap_cid_to_lookup(admin, cid);
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		/* No selection */
		break;
	case VCAP_TYPE_ES2:
		switch (l3_proto) {
		case ETH_P_IP:
			value = sparx5_vcap_es2_keyset_to_portsel(VCAP_ES2_PTC_IPV4,
								  keyset);
			spx5_rmw(EACL_VCAP_ES2_KEY_SEL_IP4_KEY_SEL_SET(value),
				 EACL_VCAP_ES2_KEY_SEL_IP4_KEY_SEL,
				 sparx5,
				 EACL_VCAP_ES2_KEY_SEL(portno, lookup));
			break;
		case ETH_P_IPV6:
			value = sparx5_vcap_es2_keyset_to_portsel(VCAP_ES2_PTC_IPV6,
								  keyset);
			spx5_rmw(EACL_VCAP_ES2_KEY_SEL_IP6_KEY_SEL_SET(value),
				 EACL_VCAP_ES2_KEY_SEL_IP6_KEY_SEL,
				 sparx5,
				 EACL_VCAP_ES2_KEY_SEL(portno, lookup));
			break;
		case ETH_P_ARP:
			value = sparx5_vcap_es2_keyset_to_portsel(VCAP_ES2_PTC_ARP,
								  keyset);
			spx5_rmw(EACL_VCAP_ES2_KEY_SEL_ARP_KEY_SEL_SET(value),
				 EACL_VCAP_ES2_KEY_SEL_ARP_KEY_SEL,
				 sparx5,
				 EACL_VCAP_ES2_KEY_SEL(portno, lookup));
			break;
		}
		break;
	case VCAP_TYPE_IS0:
		switch (l3_proto) {
		case ETH_P_IP:
			value = sparx5_vcap_is0_keyset_to_portsel(VCAP_IS0_PTC_IPV4,
								  keyset);
			spx5_rmw(ANA_CL_ADV_CL_CFG_IP4_CLM_KEY_SEL_SET(value),
				 ANA_CL_ADV_CL_CFG_IP4_CLM_KEY_SEL,
				 sparx5,
				 ANA_CL_ADV_CL_CFG(portno, lookup));
			break;
		case ETH_P_IPV6:
			value = sparx5_vcap_is0_keyset_to_portsel(VCAP_IS0_PTC_IPV6,
								  keyset);
			spx5_rmw(ANA_CL_ADV_CL_CFG_IP6_CLM_KEY_SEL_SET(value),
				 ANA_CL_ADV_CL_CFG_IP6_CLM_KEY_SEL,
				 sparx5,
				 ANA_CL_ADV_CL_CFG(portno, lookup));
			break;
		default:
			value = sparx5_vcap_is0_keyset_to_portsel(VCAP_IS0_PTC_ETYPE,
								  keyset);
			spx5_rmw(ANA_CL_ADV_CL_CFG_ETYPE_CLM_KEY_SEL_SET(value),
				 ANA_CL_ADV_CL_CFG_ETYPE_CLM_KEY_SEL,
				 sparx5,
				 ANA_CL_ADV_CL_CFG(portno, lookup));
			break;
		}
		break;
	case VCAP_TYPE_IS2:
		switch (l3_proto) {
		case ETH_P_ARP:
			value = sparx5_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_ARP,
								  keyset);
			spx5_rmw(ANA_ACL_VCAP_S2_KEY_SEL_ARP_KEY_SEL_SET(value),
				 ANA_ACL_VCAP_S2_KEY_SEL_ARP_KEY_SEL,
				 sparx5,
				 ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
			break;
		case ETH_P_IP:
			value = sparx5_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV4_UC,
								  keyset);
			spx5_rmw(ANA_ACL_VCAP_S2_KEY_SEL_IP4_UC_KEY_SEL_SET(value),
				 ANA_ACL_VCAP_S2_KEY_SEL_IP4_UC_KEY_SEL,
				 sparx5,
				 ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
			value = sparx5_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV4_MC,
								  keyset);
			spx5_rmw(ANA_ACL_VCAP_S2_KEY_SEL_IP4_MC_KEY_SEL_SET(value),
				 ANA_ACL_VCAP_S2_KEY_SEL_IP4_MC_KEY_SEL,
				 sparx5,
				 ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
			break;
		case ETH_P_IPV6:
			value = sparx5_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV6_UC,
								  keyset);
			spx5_rmw(ANA_ACL_VCAP_S2_KEY_SEL_IP6_UC_KEY_SEL_SET(value),
				 ANA_ACL_VCAP_S2_KEY_SEL_IP6_UC_KEY_SEL,
				 sparx5,
				 ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
			value = sparx5_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV6_MC,
								  keyset);
			spx5_rmw(ANA_ACL_VCAP_S2_KEY_SEL_IP6_MC_KEY_SEL_SET(value),
				 ANA_ACL_VCAP_S2_KEY_SEL_IP6_MC_KEY_SEL,
				 sparx5,
				 ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
			break;
		default:
			value = sparx5_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_NONETH,
								  keyset);
			spx5_rmw(ANA_ACL_VCAP_S2_KEY_SEL_NON_ETH_KEY_SEL_SET(value),
				 ANA_ACL_VCAP_S2_KEY_SEL_NON_ETH_KEY_SEL,
				 sparx5,
				 ANA_ACL_VCAP_S2_KEY_SEL(portno, lookup));
			break;
		}
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
}

/* Do block allocations and provide addresses for VCAP instances */
static void sparx5_vcap_block_alloc(struct sparx5 *sparx5,
				   struct vcap_admin *admin,
				   const struct sparx5_vcap_inst *cfg)
{
	int idx, cores;
	const char *vname = sparx5_vcaps[admin->vtype].name;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		admin->first_valid_addr = 0;
		admin->last_used_addr = cfg->count;
		admin->last_valid_addr = cfg->count - 1;
		cores = spx5_rd(sparx5, VCAP_ES0_CORE_CNT);
		for (idx = 0; idx < cores; ++idx) {
			spx5_wr(VCAP_ES0_IDX_CORE_IDX_SET(idx), sparx5,
				VCAP_ES0_IDX);
			spx5_wr(VCAP_ES0_MAP_CORE_MAP_SET(1), sparx5,
				VCAP_ES0_MAP);
		}
		break;
	case VCAP_TYPE_ES2:
		admin->first_valid_addr = 0;
		admin->last_used_addr = cfg->count;
		admin->last_valid_addr = cfg->count - 1;
		cores = spx5_rd(sparx5, VCAP_ES2_CORE_CNT);
		for (idx = 0; idx < cores; ++idx) {
			spx5_wr(VCAP_ES2_IDX_CORE_IDX_SET(idx), sparx5,
				VCAP_ES2_IDX);
			spx5_wr(VCAP_ES2_MAP_CORE_MAP_SET(1), sparx5,
				VCAP_ES2_MAP);
		}
		break;
	case VCAP_TYPE_IS0:
	case VCAP_TYPE_IS2:
		/* Super VCAP block mapping and address configuration. Block 0
		 * is assigned addresses 0 through 3071, block 1 is assigned
		 * addresses 3072 though 6143, and so on.
		 */
		cores = cfg->blocks;
		for (idx = cfg->blockno; idx < cfg->blockno + cfg->blocks; ++idx) {
			spx5_wr(VCAP_SUPER_IDX_CORE_IDX_SET(idx), sparx5,
				VCAP_SUPER_IDX);
			spx5_wr(VCAP_SUPER_MAP_CORE_MAP_SET(cfg->map_id), sparx5,
				VCAP_SUPER_MAP);
		}
		admin->first_valid_addr = cfg->blockno * SUPER_VCAP_BLK_SIZE;
		admin->last_used_addr = admin->first_valid_addr +
			cfg->blocks * SUPER_VCAP_BLK_SIZE;
		admin->last_valid_addr = admin->last_used_addr - 1;
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n",
		       __func__, __LINE__, admin->vtype);
		break;
	}
	pr_debug("%s:%d: enabled %d %s cores\n", __func__, __LINE__, cores, vname);
}

/* Allocate a vcap instance with a rule list and a cache area */
static struct vcap_admin *
sparx5_vcap_admin_alloc(struct sparx5 *sparx5, struct vcap_control *ctrl,
			const struct sparx5_vcap_inst *cfg)
{
	struct vcap_admin *admin;

	admin = devm_kzalloc(sparx5->dev, sizeof(*admin), GFP_KERNEL);
	if (admin) {
		INIT_LIST_HEAD(&admin->list);
		INIT_LIST_HEAD(&admin->rules);
		admin->vtype = cfg->vtype;
		admin->vinst = cfg->vinst;
		mutex_init(&admin->lock);
		admin->lookups = cfg->lookups;
		admin->lookups_per_instance = cfg->lookups_per_instance;
		admin->first_cid = cfg->first_cid;
		admin->last_cid = cfg->last_cid;
		admin->cache.keystream =
			devm_kzalloc(sparx5->dev, STREAMSIZE, GFP_KERNEL);
		admin->cache.maskstream =
			devm_kzalloc(sparx5->dev, STREAMSIZE, GFP_KERNEL);
		admin->cache.actionstream =
			devm_kzalloc(sparx5->dev, STREAMSIZE, GFP_KERNEL);
		if (!admin->cache.keystream || !admin->cache.maskstream ||
		    !admin->cache.actionstream)
			goto memerr;
		return admin;
	}
memerr:
	kfree(admin->cache.keystream);
	kfree(admin->cache.maskstream);
	kfree(admin->cache.actionstream);
	return ERR_PTR(-ENOMEM);
}

static void
sparx5_vcap_admin_free(struct sparx5 *sparx5, struct vcap_admin *admin)
{
	if (!admin)
		return;
	devm_kfree(sparx5->dev, admin->cache.keystream);
	devm_kfree(sparx5->dev, admin->cache.maskstream);
	devm_kfree(sparx5->dev, admin->cache.actionstream);
	devm_kfree(sparx5->dev, admin);
}

/* Allocate a vcap control and vcap instances and configure the system */
int sparx5_vcap_init(struct sparx5 *sparx5)
{
	struct vcap_control *ctrl =
		devm_kzalloc(sparx5->dev, sizeof(*ctrl), GFP_KERNEL);
	const struct sparx5_vcap_inst *cfg;
	struct vcap_admin *admin;
	int idx, err = 0;

	/* - Setup key selection for packet types per port and lookup
	 * - Create administrative state for each available VCAP
	 *   - Lists of rules
	 *   - Address information
	 *   - Key selection information
	 */
	if (ctrl) {
		sparx5->vcap_ctrl = ctrl;
		/* Setup callbacks to allow the API to use the VCAP HW */
		ctrl->ops = &sparx5_vcap_ops;
		INIT_LIST_HEAD(&ctrl->list);
		/* Do VCAP instance initialization */
		for (idx = 0; idx < ARRAY_SIZE(sparx5_vcap_inst_cfg); ++idx) {
			cfg = &sparx5_vcap_inst_cfg[idx];
			admin = sparx5_vcap_admin_alloc(sparx5, ctrl, cfg);
			if (IS_ERR(admin)) {
				err = PTR_ERR(admin);
				pr_err("%s:%d: vcap allocation failed: %d\n",
				       __func__, __LINE__, err);
				return err;
			}
			sparx5_vcap_block_alloc(sparx5, admin, cfg);
			sparx5_vcap_block_init(sparx5, admin);
			if (cfg->vinst == 0)
				sparx5_vcap_port_key_selection(sparx5, admin);
			pr_info("%s:%d: vcap: {%d,%d}, cid: [%d,%d]: blocks: [%d,%d], addr: [%d,%d]\n",
				__func__, __LINE__, admin->vtype, admin->vinst,
				admin->first_cid, admin->last_cid, cfg->blockno,
				(cfg->blockno + cfg->blocks) - 1,
				admin->first_valid_addr,
				admin->last_valid_addr);
			list_add_tail(&admin->list, &ctrl->list);
		}
		/* Start the netlink service with any available port */
		for (idx = 0; idx < SPX5_PORTS; idx++) {
			if (sparx5->ports[idx] && sparx5->ports[idx]->ndev) {
				vcap_netlink_init(ctrl, sparx5->ports[idx]->ndev);
				break;
			}
		}
		/* let the api know the vcap model and client */
		ctrl->vcaps = sparx5_vcaps;
		ctrl->stats = &sparx5_vcap_stats;
		vcap_api_set_client(ctrl);
		sparx5_create_vcap_debugfs(sparx5, ctrl);
	}
	return err;
}

void sparx5_vcap_destroy(struct sparx5 *sparx5)
{
	struct vcap_control *ctrl = sparx5->vcap_ctrl;
	struct vcap_admin *admin, *admin_next;

	vcap_netlink_uninit();
	/*
	 * - For each VCAP instance
	 *   - Remove key selection on ports
	 *   - Delete rules in VCAP (init)
	 *   - Deallocate rules
	 *   - Remove VCAP instance
	 * - Remove VCAP control instance
	 */
	if (ctrl) {
		list_for_each_entry_safe(admin, admin_next, &ctrl->list, list) {
			sparx5_vcap_port_key_deselection(sparx5, admin);
			vcap_del_rules(admin);
			mutex_destroy(&admin->lock);
			list_del(&admin->list);
			sparx5_vcap_admin_free(sparx5, admin);
		}
		devm_kfree(sparx5->dev, ctrl);
		vcap_api_set_client(NULL);
	}
	sparx5->vcap_ctrl = NULL;
}
