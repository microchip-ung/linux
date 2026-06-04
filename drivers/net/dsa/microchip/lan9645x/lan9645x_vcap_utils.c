// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"
#include "lan9645x_vcap_utils.h"
#include "vcap_ag_api.h"
#include "vcap_api.h"
#include "vcap_api_client.h"

#define LAN9645X_MRP_RULE_ID_OFFSET	512

/* Deterministic ES0 rule id (one per chip port) for the reserved-VID untag
 * rule, so it can be removed without tracking per-port state.
 */
#define LAN9645X_ES0_RSV_UNTAG_RID_BASE	1024

/* PUSH_OUTER_TAG action encoding (REW, datasheet "Tagging combinations"):
 * value 3 pushes no tag and overrules the port's REW TAG_CFG.
 */
#define LAN9645X_ES0_PUSH_OUTER_NO_TAG	3

static const u8 prio_oui_mrp[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x0 };
static const u8 prio_oui_mask[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0x0, 0x0, 0x0 };

enum vcap_bit vcap2bit(u32 val)
{
	return !!val ? VCAP_BIT_1 : VCAP_BIT_0;
}

int lan9645x_vcap_add_key_mac(struct vcap_rule *rule,
			      enum vcap_key_field mac_field, unsigned char *mac)
{
	struct vcap_u48_key smac;

	/* VCAP fields must be little endian, since we write smac.value[0] first
	 * in stream.
	 */
	vcap_netbytes_copy(smac.value, mac, ETH_ALEN);
	memset(smac.mask, 0xff, ETH_ALEN);

	return vcap_rule_add_key_u48(rule, mac_field, &smac);
}

int lan9645x_vcap_rule_val_add(struct vcap_rule *rule, u16 l3_proto)
{
	int err;

	err = vcap_val_rule(rule, l3_proto);
	if (err)
		return err;

	return vcap_add_rule(rule);
}

static int lan9645x_is1_add_ether(struct lan9645x_port *port,
				  enum vcap_user user,
				  u32 *rule_id, u32 ethertype)
{
	int chain_id = LAN9645X_VCAP_CID_IS1_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	struct net_device *dev;
	int err;

	dev = lan9645x_port_to_ndev(port);

	vrule = vcap_alloc_rule(port->lan9645x->vcap_ctrl, dev, chain_id,
				user, prio, *rule_id);
	if (!vrule || IS_ERR(vrule)) {
		netdev_dbg(dev, "Failed to add rule based on etype");
		return 1;
	}

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				    ~BIT(port->chip_port));
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_ETYPE, ethertype, ~0);
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_S1);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, 7);
	err = err ? -EINVAL : 0;
	if (!err)
		err = lan9645x_vcap_rule_val_add(vrule, ETH_P_ALL);
	vcap_free_rule(vrule);
	return err;
}

static int lan9645x_is1_add_dmac(struct lan9645x_port *port,
				 enum vcap_user user,
				 u32 *rule_id, u8 *oui)
{
	int chain_id = LAN9645X_VCAP_CID_IS1_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_u48_key dmac;
	struct vcap_rule *vrule;
	struct net_device *dev;
	int err;

	dev = lan9645x_port_to_ndev(port);

	vrule = vcap_alloc_rule(port->lan9645x->vcap_ctrl, dev, chain_id,
				user, prio, *rule_id);
	if (!vrule || IS_ERR(vrule))
		return 1;

	vcap_netbytes_copy(dmac.value, oui, ETH_ALEN);
	vcap_netbytes_copy(dmac.mask, (u8 *)prio_oui_mask, ETH_ALEN);

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				    ~BIT(port->chip_port));
	err |= vcap_rule_add_key_u48(vrule, VCAP_KF_L2_DMAC, &dmac);
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_S1);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, 7);
	err = err ? -EINVAL : 0;
	if (!err)
		err = lan9645x_vcap_rule_val_add(vrule, ETH_P_ALL);
	vcap_free_rule(vrule);
	return 0;
}

int lan9645x_add_prio_is1_rule(struct lan9645x_port *port, enum vcap_user user,
			       u32 *rule_id)
{
	u32 ethertype;

	*rule_id = 0; /* Returned rule_id to be used when deleting */

	if (user == VCAP_USER_MRP) {
		ethertype = ETH_P_MRP;
		*rule_id = LAN9645X_MRP_RULE_ID_OFFSET + port->chip_port;
	} else
		return 1;

	if (lan9645x_is1_add_ether(port, user, rule_id, ethertype))
		if (lan9645x_is1_add_dmac(port, user, rule_id,
					  (u8 *)prio_oui_mrp))
			return 1;

	return 0;
}

void lan9645x_is2_only_mac_etype_llc(struct lan9645x *lan9645x, u32 lookup,
				     int port)
{
	u32 val, mask;

	/* This configures VCAP IS2 (port, lookup) to treat all frame types
	 * as MAC_ETYPE or MAC_LLC.
	 * SNAP and LLC framesa are handled by MAC_LLC rule types, and all others
	 * are handled by MAC_ETYPE.
	 */
	val = ANA_VCAP_S2_CFG_ARP_DIS_SET(lookup) |
	      ANA_VCAP_S2_CFG_IP_TCPUDP_DIS_SET(lookup) |
	      ANA_VCAP_S2_CFG_IP_OTHER_DIS_SET(lookup) |
	      ANA_VCAP_S2_CFG_OAM_DIS_SET(lookup);

	mask = ANA_VCAP_S2_CFG_ARP_DIS_SET(lookup) |
		ANA_VCAP_S2_CFG_IP_TCPUDP_DIS_SET(lookup) |
		ANA_VCAP_S2_CFG_IP_OTHER_DIS_SET(lookup) |
		ANA_VCAP_S2_CFG_OAM_DIS_SET(lookup);

	switch (lookup) {
	case S2_LOOKUP1:
		val |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1_SET(IP6_MAC_ETYPE);
		mask |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1_SET(IP6_MAC_ETYPE);
		break;
	case S2_LOOKUP2:
		val |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP2_SET(IP6_MAC_ETYPE);
		mask |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP2_SET(IP6_MAC_ETYPE);
		break;
	default:
		WARN(true, "invalid lookup value");
	}

	lan_rmw(val, mask, lan9645x, ANA_VCAP_S2_CFG(port));
}

/* Reserved VIDs (VLAN_RSV_RANGE_START..VLAN_N_VID-1) are internal only: the
 * per-bridge VLAN-unaware PVIDs and the HSR/PRP VID. They must never leave the
 * switch as a wire tag.
 */
int lan9645x_es0_add_reserved_vid_untag(struct lan9645x *lan9645x, int port)
{
	struct net_device *dev;
	struct vcap_rule *rule;
	u32 rule_id, vid_mask;
	int err;

	/* The reserved range is matched with a single VID value/mask pair, which
	 * is only exact if [VLAN_RSV_RANGE_START, VLAN_N_VID) is a power-of-two
	 * block aligned to its own size.
	 */
	BUILD_BUG_ON((VLAN_N_VID - VLAN_RSV_RANGE_START) &
		     (VLAN_N_VID - VLAN_RSV_RANGE_START - 1));
	BUILD_BUG_ON(VLAN_RSV_RANGE_START &
		     (VLAN_N_VID - VLAN_RSV_RANGE_START - 1));

	/* care bits = the fixed prefix of the aligned reserved block */
	vid_mask = (VLAN_N_VID - 1) & ~(VLAN_N_VID - VLAN_RSV_RANGE_START - 1);

	dev = lan9645x_chipport_to_ndev(lan9645x, port);
	rule_id = LAN9645X_ES0_RSV_UNTAG_RID_BASE + port;

	rule = vcap_alloc_rule(lan9645x->vcap_ctrl, dev,
			       LAN9645X_VCAP_CID_ES0_L0, VCAP_USER_SYS_LOW_PRIO,
			       0, rule_id);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	err = vcap_set_rule_set_actionset(rule, VCAP_AFS_VID);
	/* The egress-port key (VCAP_KF_IF_EGR_PORT_NO) is added by
	 * lan9645x_vcap_es0_add_default_fields().
	 */
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_8021Q_VID_CLS,
				     VLAN_RSV_RANGE_START, vid_mask);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_PUSH_OUTER_TAG,
					LAN9645X_ES0_PUSH_OUTER_NO_TAG);
	err = err ? -EINVAL : 0;
	if (!err)
		err = lan9645x_vcap_rule_val_add(rule, ETH_P_ALL);
	vcap_free_rule(rule);
	return err;
}

void lan9645x_es0_del_reserved_vid_untag(struct lan9645x *lan9645x, int port)
{
	vcap_del_rule(lan9645x->vcap_ctrl, lan9645x_chipport_to_ndev(lan9645x, port),
		      LAN9645X_ES0_RSV_UNTAG_RID_BASE + port);
}

void lan9645x_is2_default_conf(struct lan9645x *lan9645x, u32 lookup,
			       int port)
{
	u32 val, mask;

	val = ANA_VCAP_S2_CFG_ARP_DIS_SET(0) |
	      ANA_VCAP_S2_CFG_IP_TCPUDP_DIS_SET(0) |
	      ANA_VCAP_S2_CFG_IP_OTHER_DIS_SET(0) |
	      ANA_VCAP_S2_CFG_OAM_DIS_SET(0);

	mask = ANA_VCAP_S2_CFG_ARP_DIS_SET(lookup) |
		ANA_VCAP_S2_CFG_IP_TCPUDP_DIS_SET(lookup) |
		ANA_VCAP_S2_CFG_IP_OTHER_DIS_SET(lookup) |
		ANA_VCAP_S2_CFG_OAM_DIS_SET(lookup);

	switch (lookup) {
	case S2_LOOKUP1:
		val |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1_SET(IP6_TCP_UDP_OR_OTHER);
		mask |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1_SET(IP6_TCP_UDP_OR_OTHER);
		break;
	case S2_LOOKUP2:
		val |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP2_SET(IP6_TCP_UDP_OR_OTHER);
		mask |= ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP2_SET(IP6_TCP_UDP_OR_OTHER);
		break;
	default:
		WARN(true, "invalid lookup value");
	}

	lan_rmw(val, mask, lan9645x, ANA_VCAP_S2_CFG(port));
}
