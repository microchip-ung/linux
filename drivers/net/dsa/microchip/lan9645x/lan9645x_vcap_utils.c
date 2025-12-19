// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"
#include "lan9645x_vcap_utils.h"
#include "vcap_ag_api.h"
#include "vcap_api.h"
#include "vcap_api_client.h"

#define LAN9645X_MRP_RULE_ID_OFFSET	512

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

	err = vcap_add_rule(rule);
	if (err)
		return err;

	return err;
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
