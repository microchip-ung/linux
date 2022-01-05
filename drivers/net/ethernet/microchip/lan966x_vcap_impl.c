// SPDX-License-Identifier: GPL-2.0+
/* Microchip LAN966x Switch driver VCAP Library
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 */

#include <linux/types.h>
#include <linux/list.h>

#include "vcap_api.h"
#include "vcap_api_client.h"
#include "vcap_api_debugfs.h"
#include "vcap_netlink.h"

#include "lan966x_main.h"
#include "lan966x_regs.h"
#include "lan966x_vcap_impl.h"

extern const struct vcap_info lan966x_vcaps[];
extern const struct vcap_statistics lan966x_vcap_stats;

#define STREAMSIZE (64 * 4)

#define LAN966X_IS1_LOOKUPS 3
#define LAN966X_IS2_LOOKUPS 2
#define LAN966X_ES0_LOOKUPS 1

/* Access lookup bitfields */
#define ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS(l) BIT(l)
#define ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG(l) GENMASK(3 + (2 * l), 2 + (2 * l))
#define ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS(l) BIT(6 + l)
#define ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS(l)  BIT(8 + l)
#define ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS(l) BIT(10 + l)
#define ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS(l)  BIT(12 + 1)

#define ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS_SET(l, x) (((x) << l) & GENMASK(1, 0))
#define ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG_SET(l, x)                                   \
	((((x) << 2) << (2 * l)) & GENMASK(5, 2))
#define ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS_SET(l, x)                              \
	((((x) << 6) << l) & GENMASK(7, 6))
#define ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS_SET(l, x)                             \
	((((x) << 8) << l) & GENMASK(9, 8))
#define ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS_SET(l, x)                                   \
	((((x) << 10) << l) & GENMASK(11, 10))
#define ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS_SET(l, x)                                  \
	((((x) << 12) << l) & GENMASK(13, 12))

#define ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS_GET(l, x)                               \
	((((x)&GENMASK(1, 0)) >> l) & 0x1)
#define ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG_GET(l, x)                               \
	(((((x)&GENMASK(5, 2)) >> 2) >> (2 * l)) & 0x3)
#define ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS_GET(l, x)                          \
	(((((x)&GENMASK(7, 6)) >> 6) >> l) & 0x1)
#define ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS_GET(l, x)                         \
	(((((x)&GENMASK(9, 8)) >> 8) >> l) & 0x1)
#define ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS_GET(l, x)                               \
	(((((x)&GENMASK(11, 10)) >> 10) >> l) & 0x1)
#define ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS_GET(l, x)                              \
	(((((x)&GENMASK(13, 12)) >> 12) >> l) & 0x1)

#define LAN966X_STAT_ESDX_GRN_BYTES 0x300
#define LAN966X_STAT_ESDX_GRN_PKTS 0x301
#define LAN966X_STAT_ESDX_YEL_BYTES 0x302
#define LAN966X_STAT_ESDX_YEL_PKTS 0x303

/* Ingress Stage 1 traffic type classification */
enum vcap_is1_port_traffic_class {
	VCAP_IS1_PTC_OTHER,
	VCAP_IS1_PTC_IPV4,
	VCAP_IS1_PTC_IPV6,
	VCAP_IS1_PTC_RT,
	VCAP_IS1_PTC_MAX,
};

enum vcap_is1_port_sel_other {
	VCAP_IS1_PS_OTHER_NORMAL,
	VCAP_IS1_PS_OTHER_7TUPLE,
	VCAP_IS1_PS_OTHER_DBL_VID,
	VCAP_IS1_PS_OTHER_DMAC_VID,
};

enum vcap_is1_port_sel_ipv4 {
	VCAP_IS1_PS_IPV4_NORMAL,
	VCAP_IS1_PS_IPV4_7TUPLE,
	VCAP_IS1_PS_IPV4_5TUPLE_IP4,
	VCAP_IS1_PS_IPV4_DBL_VID,
	VCAP_IS1_PS_IPV4_DMAC_VID,
};

enum vcap_is1_port_sel_ipv6 {
	VCAP_IS1_PS_IPV6_NORMAL,
	VCAP_IS1_PS_IPV6_7TUPLE,
	VCAP_IS1_PS_IPV6_5TUPLE_IP4,
	VCAP_IS1_PS_IPV6_NORMAL_IP6,
	VCAP_IS1_PS_IPV6_5TUPLE_IP6,
	VCAP_IS1_PS_IPV6_DBL_VID,
	VCAP_IS1_PS_IPV6_DMAC_VID,
};

enum vcap_is1_port_sel_rt {
	VCAP_IS1_PS_RT_NORMAL,
	VCAP_IS1_PS_RT_7TUPLE,
	VCAP_IS1_PS_RT_DBL_VID,
	VCAP_IS1_PS_RT_DMAC_VID,
	VCAP_IS1_PS_RT_FOLLOW_OTHER = 7,
};

/* Ingress Stage2 traffic type classification */
enum vcap_is2_port_traffic_class {
	VCAP_IS2_PTC_ARP,
	VCAP_IS2_PTC_SNAP,
	VCAP_IS2_PTC_OAM,
	VCAP_IS2_PTC_IPV4_OTHER,
	VCAP_IS2_PTC_IPV4_TCPUDP,
	VCAP_IS2_PTC_IPV6,
	VCAP_IS2_PTC_MAX,
};

enum vcap_is2_port_sel_ipv6 {
	VCAP_IS2_PS_IPV6_TCPUDP_OTHER,
	VCAP_IS2_PS_IPV6_STD,
	VCAP_IS2_PS_IPV6_IP4_TCPUDP_IP4_OTHER,
	VCAP_IS2_PS_IPV6_MAC_ETYPE,
};

static struct lan966x_vcap_inst {
	enum vcap_type vtype; /* type of vcap */
	int tgt_inst; /* hardware instance number */
	int lookups; /* number of lookups in this vcap type */
	int first_cid; /* first chain id in this vcap */
	int last_cid; /* last chain id in this vcap */
	int count; /* number of available addresses */
} lan966x_vcap_inst_cfg[] = {
	{
		.vtype = VCAP_TYPE_ES0,
		.tgt_inst = 0,
		.lookups = LAN966X_ES0_LOOKUPS,
		.first_cid = LAN966X_VCAP_CID_ES0_L0,
		.last_cid = LAN966X_VCAP_CID_ES0_MAX,
		.count = 64,
	},
	{
		.vtype = VCAP_TYPE_IS1,
		.tgt_inst = 1,
		.lookups = LAN966X_IS1_LOOKUPS,
		.first_cid = LAN966X_VCAP_CID_IS1_L0,
		.last_cid = LAN966X_VCAP_CID_IS1_MAX,
		.count = 768,
	},
	{
		.vtype = VCAP_TYPE_IS2, /* IS2-0 */
		.tgt_inst = 2,
		.lookups = LAN966X_IS2_LOOKUPS,
		.first_cid = LAN966X_VCAP_CID_IS2_L0,
		.last_cid = LAN966X_VCAP_CID_IS2_MAX,
		.count = 256,
	},
};

static enum vcap_keyfield_set
	lan966x_vcap_is1_port_cfg[LAN966X_IS1_LOOKUPS][VCAP_IS1_PTC_MAX] = {
		{
			[VCAP_IS1_PTC_OTHER] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_IPV4] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_IPV6] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_RT] = VCAP_KFS_NO_VALUE,
		},
		{
			[VCAP_IS1_PTC_OTHER] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_IPV4] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_IPV6] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_RT] = VCAP_KFS_NO_VALUE,
		},
		{
			[VCAP_IS1_PTC_OTHER] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_IPV4] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_IPV6] = VCAP_KFS_7TUPLE,
			[VCAP_IS1_PTC_RT] = VCAP_KFS_NO_VALUE,
		},
	};

static enum vcap_keyfield_set
	lan966x_vcap_is2_port_cfg[LAN966X_IS2_LOOKUPS][VCAP_IS2_PTC_MAX] = {
		{
			[VCAP_IS2_PTC_ARP] = VCAP_KFS_ARP,
			[VCAP_IS2_PTC_SNAP] = VCAP_KFS_MAC_SNAP,
			[VCAP_IS2_PTC_OAM] = VCAP_KFS_OAM,
			[VCAP_IS2_PTC_IPV4_OTHER] = VCAP_KFS_IP4_OTHER,
			[VCAP_IS2_PTC_IPV4_TCPUDP] = VCAP_KFS_IP4_TCP_UDP,
			[VCAP_IS2_PTC_IPV6] = VCAP_KFS_IP6_TCP_UDP,
		},
		{
			[VCAP_IS2_PTC_ARP] = VCAP_KFS_ARP,
			[VCAP_IS2_PTC_SNAP] = VCAP_KFS_MAC_SNAP,
			[VCAP_IS2_PTC_OAM] = VCAP_KFS_OAM,
			[VCAP_IS2_PTC_IPV4_OTHER] = VCAP_KFS_IP4_OTHER,
			[VCAP_IS2_PTC_IPV4_TCPUDP] = VCAP_KFS_IP4_TCP_UDP,
			[VCAP_IS2_PTC_IPV6] = VCAP_KFS_IP6_TCP_UDP,
		}
	};

/* Get the keyset name from the LAN966X VCAP model */
const char *lan966x_vcap_keyset_name(struct net_device *ndev,
				     enum vcap_keyfield_set keyset)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct vcap_control *vctrl = port->lan966x->vcap_ctrl;

	return vctrl->stats->keyfield_set_names[keyset];
}

/* Get the key name from the LAN966X VCAP model */
const char *lan966x_vcap_key_name(struct net_device *ndev,
				  enum vcap_key_field key)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct vcap_control *vctrl = port->lan966x->vcap_ctrl;

	return vctrl->stats->keyfield_names[key];
}

static int
lan966x_vcap_is1_keyset_to_portsel(enum vcap_is1_port_traffic_class ptc,
				   enum vcap_keyfield_set keyset)
{
	int sel = 0;

	switch (ptc) {
	case VCAP_IS1_PTC_OTHER:
		switch (keyset) {
		case VCAP_KFS_7TUPLE:
			sel = VCAP_IS1_PS_OTHER_7TUPLE;
			break;
		case VCAP_KFS_NORMAL:
		case VCAP_KFS_NORMAL_DMAC:
			sel = VCAP_IS1_PS_OTHER_NORMAL;
			break;
		default:
			sel = VCAP_IS1_PS_OTHER_7TUPLE;
			break;
		}
		break;
	case VCAP_IS1_PTC_IPV4:
		switch (keyset) {
		case VCAP_KFS_7TUPLE:
			sel = VCAP_IS1_PS_IPV4_7TUPLE;
			break;
		case VCAP_KFS_5TUPLE_IP4:
			sel = VCAP_IS1_PS_IPV4_5TUPLE_IP4;
			break;
		case VCAP_KFS_NORMAL:
		case VCAP_KFS_NORMAL_DMAC:
			sel = VCAP_IS1_PS_IPV4_NORMAL;
			break;
		default:
			sel = VCAP_IS1_PS_IPV4_7TUPLE;
			break;
		}
		break;
	case VCAP_IS1_PTC_IPV6:
		switch (keyset) {
		case VCAP_KFS_NORMAL:
		case VCAP_KFS_NORMAL_DMAC:
			sel = VCAP_IS1_PS_IPV6_NORMAL;
			break;
		case VCAP_KFS_5TUPLE_IP6:
			sel = VCAP_IS1_PS_IPV6_5TUPLE_IP6;
			break;
		case VCAP_KFS_7TUPLE:
		case VCAP_KFS_NORMAL_7TUPLE:
			sel = VCAP_IS1_PS_IPV6_7TUPLE;
			break;
		case VCAP_KFS_5TUPLE_IP4:
			sel = VCAP_IS1_PS_IPV6_5TUPLE_IP4;
			break;
		case VCAP_KFS_NORMAL_IP6:
		case VCAP_KFS_NORMAL_IP6_DMAC:
			sel = VCAP_IS1_PS_IPV6_NORMAL_IP6;
			break;
		case VCAP_KFS_DMAC_VID:
			sel = VCAP_IS1_PS_IPV6_DMAC_VID;
			break;
		default:
			sel = VCAP_IS1_PS_IPV6_7TUPLE;
			break;
		}
		break;
		break;
	case VCAP_IS1_PTC_RT:
		switch (keyset) {
		case VCAP_KFS_NO_VALUE:
			sel = VCAP_IS1_PS_RT_FOLLOW_OTHER;
			break;
		case VCAP_KFS_NORMAL:
			sel = VCAP_IS1_PS_RT_NORMAL;
			break;
		case VCAP_KFS_7TUPLE:
			sel = VCAP_IS1_PS_RT_7TUPLE;
			break;
		default:
			sel = VCAP_IS1_PS_RT_7TUPLE;
			break;
		}
		break;
	default:
		sel = -EINVAL;
		break;
	}
	return sel;
}

static int
lan966x_vcap_is2_keyset_to_portsel(enum vcap_is2_port_traffic_class ptc,
				   enum vcap_keyfield_set keyset)
{
	int sel = 0;

	switch (ptc) {
	case VCAP_IS2_PTC_IPV6:
		switch (keyset) {
		case VCAP_KFS_IP6_OTHER:
			sel = VCAP_IS2_PS_IPV6_TCPUDP_OTHER;
			break;
		case VCAP_KFS_IP6_TCP_UDP:
			sel = VCAP_IS2_PS_IPV6_TCPUDP_OTHER;
			break;
		case VCAP_KFS_IP6_STD:
			sel = VCAP_IS2_PS_IPV6_STD;
			break;
		case VCAP_KFS_IP4_OTHER:
			sel = VCAP_IS2_PS_IPV6_IP4_TCPUDP_IP4_OTHER;
			break;
		case VCAP_KFS_IP4_TCP_UDP:
			sel = VCAP_IS2_PS_IPV6_IP4_TCPUDP_IP4_OTHER;
			break;
		default:
			sel = VCAP_IS2_PS_IPV6_MAC_ETYPE;
			break;
		}
		break;
	default:
		if (keyset == VCAP_KFS_MAC_ETYPE)
			sel = 1;
		break;
	}
	return sel;
}

static const char *lan966x_ifname(struct lan966x *lan966x, int portno)
{
	const char *ifname = "-";
	struct lan966x_port *port = lan966x->ports[portno];

	if (port)
		ifname = netdev_name(port->dev);
	return ifname;
}

static void lan966x_vcap_port_keys(int (*pf)(void *out, int arg, const char *f,
					     ...),
				   void *out, int arg, struct lan966x *lan966x,
				   struct vcap_admin *admin)
{
	int lookup, portno;
	u32 value;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno) {
			value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
			pf(out, arg, "\n  port[%02d] (%s): ", portno,
			   lan966x_ifname(lan966x, portno));
			pf(out, arg, "\n    state: ");
			if (ANA_VCAP_CFG_S1_ENA_GET(value))
				pf(out, arg, "on");
			else
				pf(out, arg, "off");
			for (lookup = 0; lookup < admin->lookups; ++lookup) {
				value = lan_rd(lan966x,
					       ANA_VCAP_S1_CFG(portno, lookup));
				pf(out, arg, "\n      L:%d:", lookup);
				pf(out, arg, "\n            other: ");
				switch (ANA_VCAP_S1_CFG_KEY_OTHER_CFG_GET(value)) {
				case VCAP_IS1_PS_OTHER_NORMAL:
					pf(out, arg, "normal");
					break;
				case VCAP_IS1_PS_OTHER_7TUPLE:
					pf(out, arg, "7tuple");
					break;
				case VCAP_IS1_PS_OTHER_DBL_VID:
					pf(out, arg, "dbl_vid");
					break;
				case VCAP_IS1_PS_OTHER_DMAC_VID:
					pf(out, arg, "dmac_vid");
					break;
				default:
					pf(out, arg, "-");
					break;
				}
				pf(out, arg, "\n            ipv4: ");
				switch (ANA_VCAP_S1_CFG_KEY_IP4_CFG_GET(value)) {
				case VCAP_IS1_PS_IPV4_NORMAL:
					pf(out, arg, "normal");
					break;
				case VCAP_IS1_PS_IPV4_7TUPLE:
					pf(out, arg, "7tuple");
					break;
				case VCAP_IS1_PS_IPV4_5TUPLE_IP4:
					pf(out, arg, "5tuple_ipv4");
					break;
				case VCAP_IS1_PS_IPV4_DBL_VID:
					pf(out, arg, "dbl_vid");
					break;
				case VCAP_IS1_PS_IPV4_DMAC_VID:
					pf(out, arg, "dmac_vid");
					break;
				default:
					pf(out, arg, "-");
					break;
				}
				pf(out, arg, "\n            ipv6: ");
				switch (ANA_VCAP_S1_CFG_KEY_IP6_CFG_GET(value)) {
				case VCAP_IS1_PS_IPV6_NORMAL:
					pf(out, arg, "normal");
					break;
				case VCAP_IS1_PS_IPV6_7TUPLE:
					pf(out, arg, "7tuple");
					break;
				case VCAP_IS1_PS_IPV6_5TUPLE_IP4:
					pf(out, arg, "5tuple_ip4");
					break;
				case VCAP_IS1_PS_IPV6_NORMAL_IP6:
					pf(out, arg, "normal_ip6");
					break;
				case VCAP_IS1_PS_IPV6_5TUPLE_IP6:
					pf(out, arg, "5tuple_ip6");
					break;
				case VCAP_IS1_PS_IPV6_DBL_VID:
					pf(out, arg, "dbl_vid");
					break;
				case VCAP_IS1_PS_IPV6_DMAC_VID:
					pf(out, arg, "dmac_vid");
					break;
				default:
					pf(out, arg, "-");
					break;
				}
				pf(out, arg, "\n            rt: ");
				switch (ANA_VCAP_S1_CFG_KEY_RT_CFG_GET(value)) {
				case VCAP_IS1_PS_RT_NORMAL:
					pf(out, arg, "normal");
					break;
				case VCAP_IS1_PS_RT_7TUPLE:
					pf(out, arg, "7tuple");
					break;
				case VCAP_IS1_PS_RT_DBL_VID:
					pf(out, arg, "dbl_vid");
					break;
				case VCAP_IS1_PS_RT_DMAC_VID:
					pf(out, arg, "dmac_vid");
					break;
				case VCAP_IS1_PS_RT_FOLLOW_OTHER:
					pf(out, arg, "follow_other");
					break;
				default:
					pf(out, arg, "-");
					break;
				}
			}
		}
		pf(out, arg, "\n");
		break;
	case VCAP_TYPE_IS2:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno) {
			value = lan_rd(lan966x, ANA_VCAP_S2_CFG(portno));
			pf(out, arg, "\n  port[%02d] (%s): ", portno,
			   lan966x_ifname(lan966x, portno));
			pf(out, arg, "\n    state: ");
			if (ANA_VCAP_S2_CFG_ENA_GET(value))
				pf(out, arg, "on");
			else
				pf(out, arg, "off");
			for (lookup = 0; lookup < admin->lookups; ++lookup) {
				pf(out, arg, "\n      L:%d:", lookup);
				pf(out, arg, "\n            snap: ");
				switch (ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS_GET(
					lookup, value)) {
				case 1:
					pf(out, arg, "mac_llc");
					break;
				default:
					pf(out, arg, "mac_snap");
					break;
				}
				pf(out, arg, "\n            arp: ");
				switch (ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS_GET(
					lookup, value)) {
				case 1:
					pf(out, arg, "mac_etype");
					break;
				default:
					pf(out, arg, "arp");
					break;
				}
				pf(out, arg, "\n            oam: ");
				switch (ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS_GET(
					lookup, value)) {
				case 1:
					pf(out, arg, "mac_etype");
					break;
				default:
					pf(out, arg, "oam");
					break;
				}
				pf(out, arg, "\n            ipv4_tcp_udp: ");
				switch (ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS_GET(
					lookup, value)) {
				case 1:
					pf(out, arg, "mac_etype");
					break;
				default:
					pf(out, arg, "ipv4_tcp_udp");
					break;
				}
				pf(out, arg, "\n            ipv4_other: ");
				switch (ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS_GET(
					lookup, value)) {
				case 1:
					pf(out, arg, "mac_etype");
					break;
				default:
					pf(out, arg, "ipv4_other");
					break;
				}
				pf(out, arg, "\n            ipv6: ");
				switch (ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG_GET(
					lookup, value)) {
				case 0:
					pf(out, arg,
					   "ipv6_tcp_udp or ipv6_other");
					break;
				case 1:
					pf(out, arg, "ipv6_std");
					break;
				case 2:
					pf(out, arg,
					   "ipv4_tcp_udp or ipv4_other");
					break;
				case 3:
					pf(out, arg, "mac_etype");
					break;
				}
			}
		}
		pf(out, arg, "\n");
		break;
	case VCAP_TYPE_ES0:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno) {
			value = lan_rd(lan966x, REW_PORT_CFG(portno));
			pf(out, arg, "\n  port[%02d] (%s): ", portno,
			   lan966x_ifname(lan966x, portno));
			pf(out, arg, "\n    state: ");
			if (REW_PORT_CFG_ES0_EN_GET(value))
				pf(out, arg, "on");
			else
				pf(out, arg, "off");
		}
		pf(out, arg, "\n");
		break;
	default:
		break;
	}
}

static int
lan966x_vcap_port_info(struct lan966x *lan966x, struct vcap_admin *admin,
		       int (*pf)(void *out, int arg, const char *f, ...),
		       void *out, int arg)
{
	struct vcap_control *vctrl = lan966x->vcap_ctrl;
	const struct vcap_info *vcap = &vctrl->vcaps[admin->vtype];

	pf(out, arg, "%s:\n", vcap->name);
	lan966x_vcap_port_keys(pf, out, arg, lan966x, admin);
	return 0;
}

/* The ESDX counter is only used/incremented if the frame has been classified
 * with an ISDX > 0 (e.g by a rule in IS0).  This is not mentioned in the
 * datasheet.
 */
static void lan966x_es0_read_esdx_counter(struct lan966x *lan966x,
					  struct vcap_admin *admin, u32 id)
{
	u32 counter;

	id = id & 0xff; /* counter limit */
	mutex_lock(&lan966x->stats_lock);
	lan_wr(SYS_STAT_CFG_STAT_VIEW_SET(id), lan966x, SYS_STAT_CFG);
	counter = lan_rd(lan966x, SYS_CNT(LAN966X_STAT_ESDX_GRN_PKTS)) +
		  lan_rd(lan966x, SYS_CNT(LAN966X_STAT_ESDX_YEL_PKTS));
	mutex_unlock(&lan966x->stats_lock);
	if (counter)
		admin->cache.counter = counter;
}

static void lan966x_es0_write_esdx_counter(struct lan966x *lan966x,
					   struct vcap_admin *admin, u32 id)
{
	id = id & 0xff; /* counter limit */
	mutex_lock(&lan966x->stats_lock);
	lan_wr(SYS_STAT_CFG_STAT_VIEW_SET(id), lan966x, SYS_STAT_CFG);
	lan_wr(0, lan966x, SYS_CNT(LAN966X_STAT_ESDX_GRN_BYTES));
	lan_wr(admin->cache.counter, lan966x,
	       SYS_CNT(LAN966X_STAT_ESDX_GRN_PKTS));
	lan_wr(0, lan966x, SYS_CNT(LAN966X_STAT_ESDX_YEL_BYTES));
	lan_wr(0, lan966x, SYS_CNT(LAN966X_STAT_ESDX_YEL_PKTS));
	mutex_unlock(&lan966x->stats_lock);
}

struct lan966x_vcap_cmd_cb {
	struct lan966x *lan966x;
	u32 instance;
};

static u32 lan966x_vcap_read_update_ctrl(const struct lan966x_vcap_cmd_cb *cb)
{
	return lan_rd(cb->lan966x, VCAP_UPDATE_CTRL(cb->instance));
}

static void lan966x_vcap_wait_update(struct lan966x *lan966x, int instance)
{
	const struct lan966x_vcap_cmd_cb cb = { .lan966x = lan966x,
						.instance = instance };
	u32 value;

	readx_poll_timeout(lan966x_vcap_read_update_ctrl, &cb, value,
			   (value & VCAP_UPDATE_CTRL_UPDATE_SHOT) == 0, 10,
			   100000);
}

/* Convert chain id to vcap lookup id */
int lan966x_vcap_cid_to_lookup(struct vcap_admin *admin, int cid)
{
	int lookup = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		break;
	case VCAP_TYPE_IS1:
		if (cid >= LAN966X_VCAP_CID_IS1_L1 &&
		    cid < LAN966X_VCAP_CID_IS1_L2)
			lookup = 1;
		else if (cid >= LAN966X_VCAP_CID_IS1_L2 &&
			 cid < LAN966X_VCAP_CID_IS1_MAX)
			lookup = 2;
		break;
	case VCAP_TYPE_IS2:
		if (cid >= LAN966X_VCAP_CID_IS2_L1 &&
		    cid < LAN966X_VCAP_CID_IS2_MAX)
			lookup = 1;
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		break;
	}
	return lookup;
}

static int
lan966x_vcap_es0_get_port_keysets(struct net_device *ndev,
				  struct vcap_keyset_list *keysetlist)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	int portno = port->chip_port;
	u32 value;

	/* Check if the port keyset selection is enabled */
	value = lan_rd(lan966x, REW_PORT_CFG(portno));
	if (!REW_PORT_CFG_ES0_EN_GET(value))
		return -ENOENT;

	return 0;
}

/* Return the list of keysets for the vcap port configuration */
static int
lan966x_vcap_is1_get_port_keysets(struct net_device *ndev, int lookup,
				  struct vcap_keyset_list *keysetlist,
				  u16 l3_proto)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	int portno = port->chip_port;
	u32 value;

	/* Check if the port keyset selection is enabled */
	value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
	if (!ANA_VCAP_CFG_S1_ENA_GET(value))
		return -ENOENT;
	value = lan_rd(lan966x, ANA_VCAP_S1_CFG(portno, lookup));

	/* Collect all keysets for the port in a list */
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IP) {
		switch (ANA_VCAP_S1_CFG_KEY_IP4_CFG_GET(value)) {
		case VCAP_IS1_PS_IPV4_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_7TUPLE);
			break;
		case VCAP_IS1_PS_IPV4_5TUPLE_IP4:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_5TUPLE_IP4);
			break;
		case VCAP_IS1_PS_IPV4_NORMAL:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_DMAC);
			break;
		}
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IPV6) {
		switch (ANA_VCAP_S1_CFG_KEY_IP6_CFG_GET(value)) {
		case VCAP_IS1_PS_IPV6_NORMAL:
		case VCAP_IS1_PS_IPV6_NORMAL_IP6:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_IP6);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_IP6_DMAC);
			break;
		case VCAP_IS1_PS_IPV6_5TUPLE_IP6:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_5TUPLE_IP6);
			break;
		case VCAP_IS1_PS_IPV6_7TUPLE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_7TUPLE);
			break;
		case VCAP_IS1_PS_IPV6_5TUPLE_IP4:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_5TUPLE_IP4);
			break;
		case VCAP_IS1_PS_IPV6_DMAC_VID:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_DMAC_VID);
			break;
		}
	}

	switch (ANA_VCAP_S1_CFG_KEY_OTHER_CFG_GET(value)) {
	case VCAP_IS1_PS_OTHER_7TUPLE:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_7TUPLE);
		break;
	case VCAP_IS1_PS_OTHER_NORMAL:
		vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL);
		vcap_keyset_list_add(keysetlist, VCAP_KFS_NORMAL_DMAC);
		break;
	}

	/* TODO: handle RT keyset/protocol */
	return 0;
}

/* Return the list of keysets for the vcap port configuration */
static int
lan966x_vcap_is2_get_port_keysets(struct net_device *ndev, int lookup,
				  struct vcap_keyset_list *keysetlist,
				  u16 l3_proto)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	int portno = port->chip_port;
	bool found = false;
	u32 value;

	/* Check if the port keyset selection is enabled */
	value = lan_rd(lan966x, ANA_VCAP_S2_CFG(portno));
	if (!ANA_VCAP_S2_CFG_ENA_GET(value))
		return -ENOENT;

	/* Collect all keysets for the port in a list */
	if (l3_proto == ETH_P_ALL)
		vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_802_2) {
		vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_LLC);
		found = true;
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_SNAP) {
		switch (ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS_GET(lookup, value)) {
		case 1:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_LLC);
			break;
		default:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_SNAP);
			break;
		}
		found = true;
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_SLOW ||
	    l3_proto == ETH_P_CFM || l3_proto == ETH_P_ELMI) {
		switch (ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS_GET(lookup, value)) {
		case 1:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		default:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_OAM);
			break;
		}
		found = true;
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_ARP) {
		switch (ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS_GET(lookup, value)) {
		case 1:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		default:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_ARP);
			break;
		}
		found = true;
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IP) {
		switch (ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS_GET(lookup, value)) {
		case 1:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		default:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
			break;
		}
		switch (ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS_GET(lookup, value)) {
		case 1:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		default:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
			break;
		}
		found = true;
	}
	if (l3_proto == ETH_P_ALL || l3_proto == ETH_P_IPV6) {
		switch (ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG_GET(lookup, value)) {
		case VCAP_IS2_PS_IPV6_TCPUDP_OTHER:
			/* The order is the priority */
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP6_OTHER);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP6_TCP_UDP);
			break;
		case VCAP_IS2_PS_IPV6_STD:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP6_STD);
			break;
		case VCAP_IS2_PS_IPV6_IP4_TCPUDP_IP4_OTHER:
			/* The order is the priority */
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_OTHER);
			vcap_keyset_list_add(keysetlist, VCAP_KFS_IP4_TCP_UDP);
			break;
		case VCAP_IS2_PS_IPV6_MAC_ETYPE:
			vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
			break;
		}
		found = true;
	}
	if (!found) /* IS2 non-classified frames generate MAC_ETYPE */
		vcap_keyset_list_add(keysetlist, VCAP_KFS_MAC_ETYPE);
	return 0;
}

static bool lan966x_vcap_is2_is_first_chain(struct vcap_rule *rule)
{
	return (rule->vcap_chain_id >= LAN966X_VCAP_CID_IS2_L0 &&
		rule->vcap_chain_id < LAN966X_VCAP_CID_IS2_L1);
}

static int lan966x_vcap_is1_lookup(struct vcap_rule *rule)
{
	if (rule->vcap_chain_id >= LAN966X_VCAP_CID_IS1_L0 &&
	    rule->vcap_chain_id < LAN966X_VCAP_CID_IS1_L1)
		return 0;
	if (rule->vcap_chain_id >= LAN966X_VCAP_CID_IS1_L1 &&
	    rule->vcap_chain_id < LAN966X_VCAP_CID_IS1_L2)
		return 1;
	return 2;
}

/* Set the ingress port mask on a rule */
static void lan966x_vcap_add_port_mask(struct vcap_rule *rule,
				       struct net_device *ndev)
{
	struct lan966x_port *port = netdev_priv(ndev);

	/* Port bit set to match-any */
	vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, 0,
			      ~BIT(port->chip_port));
}

/* Set the egress port mask on a rule */
static void lan966x_vcap_add_egr_port(struct vcap_rule *rule,
				      struct net_device *ndev)
{
	struct lan966x_port *port = netdev_priv(ndev);

	vcap_rule_add_key_u32(rule, VCAP_KF_IF_EGR_PORT_MASK, port->chip_port,
			      0xff);
}

static void lan966x_vcap_add_is1_default_fields(struct lan966x *lan966x,
						struct vcap_admin *admin,
						struct vcap_rule *rule,
						struct net_device *ndev)
{
	const struct vcap_field *field;
	int lookup;

	field = vcap_lookup_keyfield(rule, VCAP_KF_IF_IGR_PORT_MASK);
	if (field && field->width == 9)
		lan966x_vcap_add_port_mask(rule, ndev);
	else
		pr_err("%s:%d: %s: could not add an ingress port mask for: %s\n",
		       __func__, __LINE__, netdev_name(ndev),
		       lan966x_vcap_keyset_name(ndev, rule->keyset));
	switch (rule->keyset) {
	case VCAP_KFS_NORMAL:
	case VCAP_KFS_NORMAL_DMAC:
	case VCAP_KFS_5TUPLE_IP6:
	case VCAP_KFS_7TUPLE:
	case VCAP_KFS_NORMAL_7TUPLE:
	case VCAP_KFS_5TUPLE_IP4:
	case VCAP_KFS_NORMAL_IP6:
	case VCAP_KFS_NORMAL_IP6_DMAC:
		lookup = lan966x_vcap_is1_lookup(rule);
		vcap_rule_add_key_u32(rule, VCAP_KF_LOOKUP_INDEX, lookup, 0x3);
		/* Add any default actions */
		break;
	default:
		pr_err("%s:%d: %s - missing default handling\n",
		       __func__, __LINE__,
		       lan966x_vcap_keyset_name(ndev, rule->keyset));
		break;
	}
}

static void lan966x_vcap_add_is2_default_fields(struct lan966x *lan966x,
						struct vcap_admin *admin,
						struct vcap_rule *rule,
						struct net_device *ndev)
{
	const struct vcap_field *field;

	field = vcap_lookup_keyfield(rule, VCAP_KF_IF_IGR_PORT_MASK);
	if (field && field->width == 9)
		lan966x_vcap_add_port_mask(rule, ndev);
	else
		pr_err("%s:%d: %s: could not add an ingress port mask for: %s\n",
		       __func__, __LINE__, netdev_name(ndev),
		       lan966x_vcap_keyset_name(ndev, rule->keyset));
	switch (rule->keyset) {
	case VCAP_KFS_MAC_ETYPE:
	case VCAP_KFS_IP4_TCP_UDP:
	case VCAP_KFS_IP6_TCP_UDP:
	case VCAP_KFS_IP6_OTHER:
	case VCAP_KFS_IP4_OTHER:
	case VCAP_KFS_ARP:
	case VCAP_KFS_MAC_SNAP:
	case VCAP_KFS_OAM:
	case VCAP_KFS_MAC_LLC:
		if (lan966x_vcap_is2_is_first_chain(rule))
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS,
					      VCAP_BIT_1);
		else
			vcap_rule_add_key_bit(rule, VCAP_KF_LOOKUP_FIRST_IS,
					      VCAP_BIT_0);
		/* Add any default actions */
		break;
	default:
		pr_err("%s:%d: %s - missing default handling\n",
		       __func__, __LINE__,
		       lan966x_vcap_keyset_name(ndev, rule->keyset));
		break;
	}
}

static void lan966x_vcap_add_es0_default_fields(struct lan966x *lan966x,
						struct vcap_admin *admin,
						struct vcap_rule *rule,
						struct net_device *ndev)
{
	struct vcap_client_actionfield *af;
	const struct vcap_field *field;

	/* Find any ESDX rule counter id and store it in the rule information */
	af = vcap_find_actionfield(rule, VCAP_AF_ESDX);
	field = vcap_lookup_actionfield(rule, VCAP_AF_ESDX);
	if (af && field && field->type == VCAP_FIELD_U32)
		vcap_rule_set_counter_id(rule, af->data.u32.value);
	lan966x_vcap_add_egr_port(rule, ndev);
}

/* API callback used for adding default fields to a rule */
static void lan966x_vcap_add_default_fields(struct net_device *ndev,
					    struct vcap_admin *admin,
					    struct vcap_rule *rule)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		lan966x_vcap_add_is1_default_fields(lan966x, admin, rule, ndev);
		break;
	case VCAP_TYPE_IS2:
		lan966x_vcap_add_is2_default_fields(lan966x, admin, rule, ndev);
		break;
	case VCAP_TYPE_ES0:
		lan966x_vcap_add_es0_default_fields(lan966x, admin, rule, ndev);
		break;
	default:
		break;
	}
}

/* Initializing a VCAP address range */
static void _lan966x_vcap_range_init(struct lan966x *lan966x,
				     struct vcap_admin *admin, u32 addr,
				     u32 count)
{
	u32 size = count - 1;
	int instance;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
	case VCAP_TYPE_IS2:
	case VCAP_TYPE_ES0:
		instance = admin->tgt_inst;
		break;
	default:

		pr_err("%s:%d: vcap type %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		return;
	}
	pr_debug("%s:%d: size: %d, addr: %d\n", __func__, __LINE__, size, addr);

	lan_wr(VCAP_MV_CFG_MV_NUM_POS_SET(0) |
	       VCAP_MV_CFG_MV_SIZE_SET(size),
	       lan966x, VCAP_MV_CFG(instance));

	lan_wr(VCAP_UPDATE_CTRL_UPDATE_CMD_SET(VCAP_CMD_INITIALIZE) |
	       VCAP_UPDATE_CTRL_UPDATE_ENTRY_DIS_SET(0) |
	       VCAP_UPDATE_CTRL_UPDATE_ACTION_DIS_SET(0) |
	       VCAP_UPDATE_CTRL_UPDATE_CNT_DIS_SET(0) |
	       VCAP_UPDATE_CTRL_UPDATE_ADDR_SET(addr) |
	       VCAP_UPDATE_CTRL_CLEAR_CACHE_SET(true) |
	       VCAP_UPDATE_CTRL_UPDATE_SHOT_SET(1),
	       lan966x, VCAP_UPDATE_CTRL(instance));

	lan966x_vcap_wait_update(lan966x, instance);
}

/* API callback used for validating a field keyset (check the port keysets )*/
static enum vcap_keyfield_set
lan966x_vcap_validate_keyset(struct net_device *ndev,
			     struct vcap_admin *admin,
			     struct vcap_rule *rule,
			     struct vcap_keyset_list *kslist,
			     u16 l3_proto)
{
	struct vcap_keyset_list keysetlist = { 0 };
	enum vcap_keyfield_set keysets[12] = { 0 };
	int idx, jdx, lookup;

	/* Get the key selection for the (vcap, port, lookup) and compare with
	 * the suggested set, return an error of there is no match
	 */
	pr_debug("%s:%d: %d sets\n", __func__, __LINE__, kslist->cnt);
	lookup = lan966x_vcap_cid_to_lookup(admin, rule->vcap_chain_id);

	keysetlist.max = ARRAY_SIZE(keysets);
	keysetlist.keysets = keysets;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		lan966x_vcap_is1_get_port_keysets(ndev, lookup, &keysetlist,
						  l3_proto);
		break;
	case VCAP_TYPE_IS2:
		lan966x_vcap_is2_get_port_keysets(ndev, lookup, &keysetlist,
						  l3_proto);
		break;
	case VCAP_TYPE_ES0:
		if (lan966x_vcap_es0_get_port_keysets(ndev, &keysetlist) == 0)
			return kslist->keysets[0];
		break;
	default:
		pr_err("%s:%d: unsupported vcap type\n", __func__, __LINE__);
		break;
	}
	/* Check if there is a match and return the match */
	for (idx = 0; idx < kslist->cnt; ++idx)
		for (jdx = 0; jdx < keysetlist.cnt; ++jdx)
			if (kslist->keysets[idx] == keysets[jdx]) {
				pr_debug("%s:%d: keyset [%d]: %s\n",
					 __func__, __LINE__,
					 kslist->keysets[idx],
					 lan966x_vcap_keyset_name(ndev,
								  kslist->keysets[idx]));
				return kslist->keysets[idx];
			}
	pr_err("%s:%d: %s not supported in port key selection\n", __func__,
	       __LINE__, lan966x_vcap_keyset_name(ndev, kslist->keysets[0]));
	return -ENOENT;
}

static void lan966x_vcap_cache_erase(struct vcap_admin *admin)
{
	memset(admin->cache.keystream, 0, STREAMSIZE);
	memset(admin->cache.maskstream, 0, STREAMSIZE);
	memset(admin->cache.actionstream, 0, STREAMSIZE);
	memset(&admin->cache.counter, 0, sizeof(admin->cache.counter));
}

/* API callback used for writing to the VCAP cache */
static void lan966x_vcap_cache_write(struct net_device *ndev,
				     struct vcap_admin *admin,
				     enum vcap_selection sel, u32 start,
				     u32 count)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	u32 *keystr, *mskstr, *actstr;
	u32 instance;
	int idx;

	keystr = &admin->cache.keystream[start];
	mskstr = &admin->cache.maskstream[start];
	actstr = &admin->cache.actionstream[start];
	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
	case VCAP_TYPE_IS2:
	case VCAP_TYPE_ES0:
		instance = admin->tgt_inst;
		break;
	default:
		pr_err("%s:%d: vcap type %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		return;
	}
	switch (sel) {
	case VCAP_SEL_ENTRY:
		for (idx = 0; idx < count; ++idx) {
			/* Avoid 'match-off' by setting value & mask */
			lan_wr(keystr[idx] & mskstr[idx], lan966x,
			       VCAP_ENTRY_DAT(instance, idx));
			lan_wr(~mskstr[idx], lan966x,
			       VCAP_MASK_DAT(instance, idx));
		}
		for (idx = 0; idx < count; ++idx) {
			pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
				 __func__, __LINE__, start + idx, keystr[idx],
				 ~mskstr[idx]);
		}
		break;
	case VCAP_SEL_ACTION:
		for (idx = 0; idx < count; ++idx)
			lan_wr(actstr[idx], lan966x,
			       VCAP_ACTION_DAT(instance, idx));
		for (idx = 0; idx < count; ++idx) {
			pr_debug("%s:%d: actdata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, actstr[idx]);
		}
		break;
	case VCAP_SEL_COUNTER:
		pr_debug("%s:%d: cnt[%d] = %d\n", __func__, __LINE__, start,
			 admin->cache.counter);
		admin->cache.sticky = (admin->cache.counter > 0);
		lan_wr(admin->cache.counter, lan966x,
		       VCAP_CNT_DAT(instance, 0));
		if (admin->vtype == VCAP_TYPE_ES0)
			lan966x_es0_write_esdx_counter(lan966x, admin, start);
		break;
	case VCAP_SEL_ALL:
		pr_err("%s:%d: cannot write all streams at once\n", __func__,
		       __LINE__);
		break;
	}
}

/* API callback used for reading from the VCAP into the VCAP cache */
static void lan966x_vcap_cache_read(struct net_device *ndev,
				    struct vcap_admin *admin,
				    enum vcap_selection sel, u32 start,
				    u32 count)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	u32 *keystr, *mskstr, *actstr;
	int instance;
	int idx;

	keystr = &admin->cache.keystream[start];
	mskstr = &admin->cache.maskstream[start];
	actstr = &admin->cache.actionstream[start];
	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
	case VCAP_TYPE_IS2:
	case VCAP_TYPE_ES0:
		instance = admin->tgt_inst;
		break;
	default:
		pr_err("%s:%d: vcap type %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		return;
	}
	if (sel & VCAP_SEL_ENTRY) {
		for (idx = 0; idx < count; ++idx) {
			keystr[idx] =
				lan_rd(lan966x, VCAP_ENTRY_DAT(instance, idx));
			mskstr[idx] =
				~lan_rd(lan966x, VCAP_MASK_DAT(instance, idx));
		}
		for (idx = 0; idx < count; ++idx)
			pr_debug("%s:%d: keydata[%02d]: 0x%08x/%08x\n",
				 __func__, __LINE__, start + idx, keystr[idx],
				 ~mskstr[idx]);
	}
	if (sel & VCAP_SEL_ACTION) {
		for (idx = 0; idx < count; ++idx)
			actstr[idx] =
				lan_rd(lan966x, VCAP_ACTION_DAT(instance, idx));
		for (idx = 0; idx < count; ++idx) {
			pr_debug("%s:%d: actdata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, actstr[idx]);
		}
	}
	if (sel & VCAP_SEL_COUNTER) {
		admin->cache.counter =
			lan_rd(lan966x, VCAP_CNT_DAT(instance, 0));
		admin->cache.sticky = (admin->cache.counter > 0);
		if (admin->vtype == VCAP_TYPE_ES0)
			lan966x_es0_read_esdx_counter(lan966x, admin, start);
	}
}

/* API callback used for initializing a VCAP address range */
static void lan966x_vcap_range_init(struct net_device *ndev,
				    struct vcap_admin *admin, u32 addr,
				    u32 count)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;

	_lan966x_vcap_range_init(lan966x, admin, addr, count);
}

/* API callback used for updating the VCAP cache */
static void lan966x_vcap_update(struct net_device *ndev,
				struct vcap_admin *admin, enum vcap_command cmd,
				enum vcap_selection sel, u32 addr)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	char *cmdstr, *selstr;
	int instance;
	bool clear;

	switch (cmd) {
	case VCAP_CMD_WRITE:
		cmdstr = "write";
		break;
	case VCAP_CMD_READ:
		cmdstr = "read";
		break;
	case VCAP_CMD_MOVE_DOWN:
		cmdstr = "move_down";
		break;
	case VCAP_CMD_MOVE_UP:
		cmdstr = "move_up";
		break;
	case VCAP_CMD_INITIALIZE:
		cmdstr = "init";
		break;
	}
	switch (sel) {
	case VCAP_SEL_ENTRY:
		selstr = "entry";
		break;
	case VCAP_SEL_ACTION:
		selstr = "action";
		break;
	case VCAP_SEL_COUNTER:
		selstr = "counter";
		break;
	case VCAP_SEL_ALL:
		selstr = "all";
		break;
	}
	pr_debug("%s:%d: %s %s: addr: %d\n", __func__, __LINE__, cmdstr, selstr,
		 addr);
	clear = (cmd == VCAP_CMD_INITIALIZE);
	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
	case VCAP_TYPE_IS2:
	case VCAP_TYPE_ES0:
		instance = admin->tgt_inst;
		break;
	default:
		pr_err("%s:%d: vcap type %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		return;
	}

	lan_wr(VCAP_MV_CFG_MV_NUM_POS_SET(0) |
	       VCAP_MV_CFG_MV_SIZE_SET(0),
	       lan966x, VCAP_MV_CFG(instance));

	lan_wr(VCAP_UPDATE_CTRL_UPDATE_CMD_SET(cmd) |
	       VCAP_UPDATE_CTRL_UPDATE_ENTRY_DIS_SET(!(VCAP_SEL_ENTRY & sel)) |
	       VCAP_UPDATE_CTRL_UPDATE_ACTION_DIS_SET(!(VCAP_SEL_ACTION & sel)) |
	       VCAP_UPDATE_CTRL_UPDATE_CNT_DIS_SET(!(VCAP_SEL_COUNTER & sel)) |
	       VCAP_UPDATE_CTRL_UPDATE_ADDR_SET(addr) |
	       VCAP_UPDATE_CTRL_CLEAR_CACHE_SET(clear) |
	       VCAP_UPDATE_CTRL_UPDATE_SHOT,
	       lan966x, VCAP_UPDATE_CTRL(instance));

	lan966x_vcap_wait_update(lan966x, instance);
}

/* API callback used for moving a block of rules in the VCAP */
static void lan966x_vcap_move(struct net_device *ndev, struct vcap_admin *admin,
			      u32 addr, int offset, int count)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	enum vcap_command cmd;
	const char *dir;
	int instance;
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
	pr_debug(
		"%s:%d: move: addr: %u, offset: %d, count: %d, mv_num_pos: %u, mv_size: %u, dir: %s\n",
		__func__, __LINE__, addr, offset, count, mv_num_pos, mv_size,
		dir);
	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
	case VCAP_TYPE_IS2:
	case VCAP_TYPE_ES0:
		instance = admin->tgt_inst;
		break;
	default:
		pr_err("%s:%d: vcap type %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		return;
	}

	lan_wr(VCAP_MV_CFG_MV_NUM_POS_SET(mv_num_pos) |
	       VCAP_MV_CFG_MV_SIZE_SET(mv_size),
	       lan966x, VCAP_MV_CFG(instance));

	lan_wr(VCAP_UPDATE_CTRL_UPDATE_CMD_SET(cmd) |
	       VCAP_UPDATE_CTRL_UPDATE_ENTRY_DIS_SET(0) |
	       VCAP_UPDATE_CTRL_UPDATE_ACTION_DIS_SET(0) |
	       VCAP_UPDATE_CTRL_UPDATE_CNT_DIS_SET(0) |
	       VCAP_UPDATE_CTRL_UPDATE_ADDR_SET(addr) |
	       VCAP_UPDATE_CTRL_CLEAR_CACHE_SET(false) |
	       VCAP_UPDATE_CTRL_UPDATE_SHOT,
	       lan966x, VCAP_UPDATE_CTRL(instance));

	lan966x_vcap_wait_update(lan966x, instance);
}

/* Provide port information via a callback interface */
static int lan966x_port_info(struct net_device *ndev, enum vcap_type vtype,
			     int (*pf)(void *out, int arg, const char *fmt,
				       ...),
			     void *out, int arg)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	struct vcap_control *ctrl;
	struct vcap_admin *admin;

	ctrl = lan966x->vcap_ctrl;
	list_for_each_entry (admin, &ctrl->list, list) {
		if (admin->vtype == vtype)
			return lan966x_vcap_port_info(lan966x, admin, pf, out,
						      arg);
	}
	pf(out, arg, "VCAP not supported\n");
	return 0;
}

/* API callback operations */
static struct vcap_operations lan966x_vcap_ops = {
	.validate_keyset = lan966x_vcap_validate_keyset,
	.add_default_fields = lan966x_vcap_add_default_fields,
	.cache_erase = lan966x_vcap_cache_erase,
	.cache_write = lan966x_vcap_cache_write,
	.cache_read = lan966x_vcap_cache_read,
	.init = lan966x_vcap_range_init,
	.update = lan966x_vcap_update,
	.move = lan966x_vcap_move,
	.port_info = lan966x_port_info,
};

static int lan966x_seq_printf(void *out, int arg, const char *fmt, ...)
{
	struct seq_file *seqf = out;
	va_list args;

	va_start(args, fmt);
	seq_vprintf(seqf, fmt, args);
	va_end(args);
	return 0;
}

static int lan966x_vcap_debugfs_port_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	struct vcap_control *ctrl;
	struct vcap_admin *admin;

	ctrl = lan966x->vcap_ctrl;
	list_for_each_entry (admin, &ctrl->list, list) {
		lan966x_vcap_port_info(lan966x, admin, lan966x_seq_printf, m,
				       0);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_debugfs_port);

static u32 lan966x_vcap_is1_port_key_selection(int lookup)
{
	return ANA_VCAP_S1_CFG_KEY_RT_CFG_SET(lan966x_vcap_is1_keyset_to_portsel(VCAP_IS1_PTC_RT, lan966x_vcap_is1_port_cfg[lookup][VCAP_IS1_PTC_RT])) |
	       ANA_VCAP_S1_CFG_KEY_IP6_CFG_SET(lan966x_vcap_is1_keyset_to_portsel(VCAP_IS1_PTC_IPV6,lan966x_vcap_is1_port_cfg[lookup][VCAP_IS1_PTC_IPV6])) |
	       ANA_VCAP_S1_CFG_KEY_IP4_CFG_SET(lan966x_vcap_is1_keyset_to_portsel(VCAP_IS1_PTC_IPV4, lan966x_vcap_is1_port_cfg[lookup][VCAP_IS1_PTC_IPV4])) |
	       ANA_VCAP_S1_CFG_KEY_OTHER_CFG_SET(lan966x_vcap_is1_keyset_to_portsel(VCAP_IS1_PTC_OTHER, lan966x_vcap_is1_port_cfg[lookup][VCAP_IS1_PTC_OTHER]));
}

static u32 lan966x_vcap_is2_port_key_selection(u32 value, int lookup)
{
	value |= ANA_VCAP_S2_CFG_ISDX_ENA_SET(0) |
		 ANA_VCAP_S2_CFG_UDP_PAYLOAD_ENA_SET(0) |
		 ANA_VCAP_S2_CFG_ETYPE_PAYLOAD_ENA_SET(0) |
		 ANA_VCAP_S2_CFG_ENA_SET(true) |
		 ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_SNAP, lan966x_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_SNAP])) |
		 ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_ARP, lan966x_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_ARP])) |
		 ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_OAM, lan966x_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_OAM])) |
		 ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV4_TCPUDP, lan966x_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_IPV4_TCPUDP])) |
		 ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV4_OTHER, lan966x_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_IPV4_OTHER])) |
		 ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV6, lan966x_vcap_is2_port_cfg[lookup][VCAP_IS2_PTC_IPV6]));
	return value;
}

static void lan966x_vcap_port_key_selection(struct lan966x *lan966x,
					    struct vcap_admin *admin)
{
	int portno;
	int lookup;
	u32 value;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno) {
			if (lan966x->ports[portno] &&
			    lan966x->ports[portno]->dev) {
				/* Enable IS1 for this port */
				lan_wr(ANA_VCAP_CFG_S1_ENA_SET(1), lan966x,
				       ANA_VCAP_CFG(portno));
			}
		}
		for (lookup = 0; lookup < admin->lookups; ++lookup) {
			value = lan966x_vcap_is1_port_key_selection(lookup);
			for (portno = 0; portno < lan966x->num_phys_ports;
			     ++portno) {
				if (lan966x->ports[portno] &&
				    lan966x->ports[portno]->dev) {
					pr_debug(
						"%s:%d: [%d,%d]: IS1 portsel: %#08x\n",
						__func__, __LINE__, portno,
						lookup, value);
					lan_wr(value, lan966x,
					       ANA_VCAP_S1_CFG(portno, lookup));
				}
			}
		}
		break;
	case VCAP_TYPE_IS2:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno) {
			value = 0; /* Disable keyset selection for unused ports */
			if (lan966x->ports[portno] &&
			    lan966x->ports[portno]->dev) {
				for (lookup = 0; lookup < admin->lookups;
				     ++lookup) {
					value |=
						lan966x_vcap_is2_port_key_selection(
							value, lookup);
				}
			}
			pr_debug("%s:%d: [%d,%d]: IS2 portsel: %#08x\n",
				 __func__, __LINE__, portno, lookup, value);
			lan_wr(value, lan966x, ANA_VCAP_S2_CFG(portno));
		}
		break;
	case VCAP_TYPE_ES0:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno) {
			pr_debug("%s:%d: [%d]: ES0 enable, current state: %d\n",
				 __func__, __LINE__, portno,
				 lan_rd(lan966x, REW_PORT_CFG(portno)));
			lan_rmw(REW_PORT_CFG_ES0_EN_SET(1),
				REW_PORT_CFG_ES0_EN,
				lan966x, REW_PORT_CFG(portno));
		}
		/* Statistics: Use ESDX from ES0 if hit, otherwise no counting */
		lan_rmw(REW_STAT_CFG_STAT_MODE(1), REW_STAT_CFG_STAT_MODE_M,
			lan966x, REW_STAT_CFG);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		break;
	}
}

static void lan966x_vcap_port_key_deselection(struct lan966x *lan966x,
					      struct vcap_admin *admin)
{
	int portno;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno)
			lan_wr(0, lan966x, ANA_VCAP_CFG(portno));
		break;
	case VCAP_TYPE_IS2:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno)
			lan_wr(0, lan966x, ANA_VCAP_S2_CFG(portno));
		break;
	case VCAP_TYPE_ES0:
		for (portno = 0; portno < lan966x->num_phys_ports; ++portno)
			lan_wr(REW_PORT_CFG_ES0_EN_SET(0),
			       lan966x, REW_PORT_CFG(portno));
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		break;
	}
}

/* Get the port keyset for the vcap lookup */
int lan966x_vcap_get_port_keyset(struct net_device *ndev,
				 struct vcap_admin *admin, int cid,
				 u16 l3_proto,
				 struct vcap_keyset_list *keysetlist)
{
	int err = 0;
	int lookup;

	lookup = lan966x_vcap_cid_to_lookup(admin, cid);
	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		err = lan966x_vcap_is1_get_port_keysets(ndev, lookup,
							keysetlist, l3_proto);
		break;
	case VCAP_TYPE_IS2:
		err = lan966x_vcap_is2_get_port_keysets(ndev, lookup,
							keysetlist, l3_proto);
		break;
	case VCAP_TYPE_ES0:
		err = lan966x_vcap_es0_get_port_keysets(ndev, keysetlist);
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		break;
	}
	return err;
}

/* Set the port keyset for the vcap lookup */
void lan966x_vcap_set_port_keyset(struct net_device *ndev,
				  struct vcap_admin *admin, int cid,
				  u16 l3_proto, u8 l4_proto,
				  enum vcap_keyfield_set keyset)
{
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	int portno = port->chip_port;
	int lookup;
	u32 value;

	lookup = lan966x_vcap_cid_to_lookup(admin, cid);
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		/* No selection */
		break;
	case VCAP_TYPE_IS1:
		switch (l3_proto) {
		case ETH_P_IP:
			value = lan966x_vcap_is1_keyset_to_portsel(VCAP_IS1_PTC_IPV4, keyset);
			lan_rmw(ANA_VCAP_S1_CFG_KEY_IP4_CFG_SET(value),
				ANA_VCAP_S1_CFG_KEY_IP4_CFG,
				lan966x, ANA_VCAP_S1_CFG(portno, lookup));

			if (keyset == VCAP_KFS_NORMAL_DMAC) {
				value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
				value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
				value |= BIT(lookup);

				lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
					ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
					lan966x, ANA_VCAP_CFG(port->chip_port));
			}
			else {
				value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
				value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
				value &= !BIT(lookup);

				lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
					ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
					lan966x, ANA_VCAP_CFG(port->chip_port));
			}
			break;
		case ETH_P_IPV6:
			value = lan966x_vcap_is1_keyset_to_portsel(VCAP_IS1_PTC_IPV6, keyset);
			lan_rmw(ANA_VCAP_S1_CFG_KEY_IP6_CFG_SET(value),
				ANA_VCAP_S1_CFG_KEY_IP6_CFG,
				lan966x, ANA_VCAP_S1_CFG(portno, lookup));

			if (keyset == VCAP_KFS_NORMAL_IP6_DMAC) {
				value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
				value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
				value |= BIT(lookup);

				lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
					ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
					lan966x, ANA_VCAP_CFG(port->chip_port));
			}
			else {
				value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
				value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
				value &= !BIT(lookup);

				lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
					ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
					lan966x, ANA_VCAP_CFG(port->chip_port));
			}
			break;
		default:
			value = lan966x_vcap_is1_keyset_to_portsel(VCAP_IS1_PTC_OTHER, keyset);
			lan_rmw(ANA_VCAP_S1_CFG_KEY_OTHER_CFG_SET(value),
				ANA_VCAP_S1_CFG_KEY_OTHER_CFG,
				lan966x, ANA_VCAP_S1_CFG(portno, lookup));

			if (keyset == VCAP_KFS_NORMAL_DMAC) {
				value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
				value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
				value |= BIT(lookup);

				lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
					ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
					lan966x, ANA_VCAP_CFG(port->chip_port));
			}
			else {
				value = lan_rd(lan966x, ANA_VCAP_CFG(portno));
				value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
				value &= !BIT(lookup);

				lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
					ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
					lan966x, ANA_VCAP_CFG(port->chip_port));
			}
			break;
		}
		break;
	case VCAP_TYPE_IS2:
		switch (l3_proto) {
		case ETH_P_ARP:
			value = lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_ARP, keyset);
			lan_rmw(ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS_SET(lookup, value),
				ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS(lookup),
				lan966x, ANA_VCAP_S2_CFG(portno));
			break;
		case ETH_P_IP:
			value = ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV4_TCPUDP, keyset)) |
				ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV4_OTHER, keyset));
			lan_rmw(value,
				ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS(lookup) |
				ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS(lookup),
				lan966x, ANA_VCAP_S2_CFG(portno));
			break;
		case ETH_P_IPV6:
			value = ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG_SET(lookup, lan966x_vcap_is2_keyset_to_portsel(VCAP_IS2_PTC_IPV6, keyset));
			lan_rmw(value,
				ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG(lookup),
				lan966x, ANA_VCAP_S2_CFG(portno));
			break;
		default:
			value = ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS_SET(lookup, 1) |
				ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS_SET(lookup, 1) |
				ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS_SET(lookup, 1) |
				ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS_SET(lookup, 1) |
				ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS_SET(lookup, 1) |
				ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG_SET(lookup, VCAP_IS2_PS_IPV6_MAC_ETYPE);
			lan_rmw(value,
				ANA_VCAP_S2_CFG_OAM_LOOKUP_DIS(lookup) |
				ANA_VCAP_S2_CFG_SNAP_LOOKUP_DIS(lookup) |
				ANA_VCAP_S2_CFG_ARP_LOOKUP_DIS(lookup) |
				ANA_VCAP_S2_CFG_IP_TCPUDP_LOOKUP_DIS(lookup) |
				ANA_VCAP_S2_CFG_IP_OTHER_LOOKUP_DIS(lookup) |
				ANA_VCAP_S2_CFG_IP6_LOOKUP_CFG(lookup),
				lan966x, ANA_VCAP_S2_CFG(portno));
			break;
		}
		break;
	default:
		pr_err("%s:%d: vcap type: %d not supported\n", __func__,
		       __LINE__, admin->vtype);
		break;
	}
}

/* Allocate a vcap instance with a rule list and a cache area */
static struct vcap_admin *
lan966x_vcap_admin_alloc(struct lan966x *lan966x, struct vcap_control *ctrl,
			 const struct lan966x_vcap_inst *cfg)
{
	struct vcap_admin *admin;

	admin = devm_kzalloc(lan966x->dev, sizeof(*admin), GFP_KERNEL);
	if (admin) {
		INIT_LIST_HEAD(&admin->list);
		INIT_LIST_HEAD(&admin->rules);
		admin->vtype = cfg->vtype;
		admin->w32be = true;
		admin->tgt_inst = cfg->tgt_inst;
		admin->vinst = 0;
		mutex_init(&admin->lock);
		admin->lookups = cfg->lookups;
		admin->lookups_per_instance = admin->lookups;
		admin->first_cid = cfg->first_cid;
		admin->last_cid = cfg->last_cid;
		admin->cache.keystream =
			devm_kzalloc(lan966x->dev, STREAMSIZE, GFP_KERNEL);
		admin->cache.maskstream =
			devm_kzalloc(lan966x->dev, STREAMSIZE, GFP_KERNEL);
		admin->cache.actionstream =
			devm_kzalloc(lan966x->dev, STREAMSIZE, GFP_KERNEL);
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

static void lan966x_vcap_admin_free(struct lan966x *lan966x,
				    struct vcap_admin *admin)
{
	if (!admin)
		return;
	devm_kfree(lan966x->dev, admin->cache.keystream);
	devm_kfree(lan966x->dev, admin->cache.maskstream);
	devm_kfree(lan966x->dev, admin->cache.actionstream);
	devm_kfree(lan966x->dev, admin);
}

static void lan966x_vcap_block_init(struct lan966x *lan966x,
				    struct vcap_admin *admin)
{
	u32 instance = admin->tgt_inst;
	int idx, cores;

	cores = lan_rd(lan966x, VCAP_CORE_CNT(instance));
	pr_debug("%s:%d: instance: %d, cores: %d\n", __func__, __LINE__,
		 instance, cores);
	/* Map and enable cores */
	for (idx = 0; idx < cores; ++idx) {
		lan_wr(VCAP_CORE_IDX_CORE_IDX_SET(idx),
		       lan966x, VCAP_CORE_IDX(instance));
		lan_wr(VCAP_CORE_MAP_CORE_MAP_SET(1),
		       lan966x, VCAP_CORE_MAP(instance));
	}
	_lan966x_vcap_range_init(lan966x, admin, admin->first_valid_addr,
				 admin->last_valid_addr - admin->first_valid_addr);
}

/* Allocate a vcap control and vcap instances and configure the system */
int lan966x_vcap_init(struct lan966x *lan966x)
{
	struct vcap_control *ctrl =
		devm_kzalloc(lan966x->dev, sizeof(*ctrl), GFP_KERNEL);
	const struct lan966x_vcap_inst *cfg;
	struct vcap_admin *admin;
	int idx, err = 0;

	/* - Setup key selection for packet types per port and lookup
	 * - Create administrative state for each available VCAP
	 *   - Lists of rules
	 *   - Address information
	 *   - Key selection information
	 */
	if (ctrl) {
		lan966x->vcap_ctrl = ctrl;
		/* Setup callbacks to allow the API to use the VCAP HW */
		ctrl->ops = &lan966x_vcap_ops;
		INIT_LIST_HEAD(&ctrl->list);
		/* Do VCAP instance initialization */
		for (idx = 0; idx < ARRAY_SIZE(lan966x_vcap_inst_cfg); ++idx) {
			cfg = &lan966x_vcap_inst_cfg[idx];
			admin = lan966x_vcap_admin_alloc(lan966x, ctrl, cfg);
			if (IS_ERR(admin)) {
				err = PTR_ERR(admin);
				pr_err("%s:%d: vcap allocation failed: %d\n",
				       __func__, __LINE__, err);
				return err;
			}
			admin->first_valid_addr = 0;
			admin->last_used_addr = cfg->count;
			admin->last_valid_addr = cfg->count - 1;
			lan966x_vcap_block_init(lan966x, admin);
			lan966x_vcap_port_key_selection(lan966x, admin);
			pr_info("%s:%d: vcap: {%d,%d}, cid: [%d,%d]: addr: [%d,%d]\n",
				__func__, __LINE__, admin->vtype, admin->vinst,
				admin->first_cid, admin->last_cid,
				admin->first_valid_addr,
				admin->last_valid_addr);
			list_add_tail(&admin->list, &ctrl->list);
		}
		/* Start the netlink service with any available port */
		for (idx = 0; idx < LAN966X_MAX_PORTS; idx++) {
			if (lan966x->ports[idx] && lan966x->ports[idx]->dev) {
				vcap_netlink_init(ctrl,
						  lan966x->ports[idx]->dev);
				break;
			}
		}
		/* let the api know the vcap model and client */
		ctrl->vcaps = lan966x_vcaps;
		ctrl->stats = &lan966x_vcap_stats;
		vcap_api_set_client(ctrl);
		/* debug info about each vcap instance */
		vcap_debugfs(lan966x->debugfs_root, ctrl);
		/* debug info about port keyset config */
		debugfs_create_file("ports", 0444, lan966x->debugfs_root,
				    lan966x, &lan966x_vcap_debugfs_port_fops);
	}
	return err;
}

void lan966x_vcap_uninit(struct lan966x *lan966x)
{
	struct vcap_control *ctrl = lan966x->vcap_ctrl;
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
		list_for_each_entry_safe (admin, admin_next, &ctrl->list,
					  list) {
			lan966x_vcap_port_key_deselection(lan966x, admin);
			vcap_del_rules(admin);
			mutex_destroy(&admin->lock);
			list_del(&admin->list);
			lan966x_vcap_admin_free(lan966x, admin);
		}
		devm_kfree(lan966x->dev, ctrl);
		vcap_api_set_client(NULL);
	}
	lan966x->vcap_ctrl = NULL;
}
