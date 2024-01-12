/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2021 Microchip Technology Inc. */

#include "lan966x_main.h"
#include "lan966x_vcap_utils.h"
#include "vcap_api_client.h"

#define LAN966X_MRP_RULE_ID_OFFSET	512
#define LAN966X_CFM_RULE_ID_OFFSET	520

static const u8 prio_oui_mrp[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x0 };
static const u8 prio_oui_raps[ETH_ALEN] = { 0x1, 0x19, 0xa7, 0x0, 0x0, 0x0 };
static const u8 prio_oui_mask[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0x0, 0x0, 0x0 };

static int lan966x_is1_add_ether(struct lan966x_port *port,
				 enum vcap_user user,
				 u32 *rule_id, u32 ethertype)
{
	int chain_id = LAN966X_VCAP_CID_IS1_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->lan966x->vcap_ctrl, port->dev, chain_id,
				user, prio, *rule_id);
	if (!vrule || IS_ERR(vrule)) {
		netdev_dbg(port->dev, "Failed to add rule based on etype");
		return 1;
	}

	/* Try first to match with ether type */
	err = vcap_rule_add_key_bit(vrule, VCAP_KF_LOOKUP_INDEX, 0);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				     ~BIT(port->chip_port));
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_ETYPE, ethertype, ~0);
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_S1);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, 7);
	err |= vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		netdev_dbg(port->dev, "Failed to add rule based on etype");
		vcap_free_rule(vrule);
		return 1;
	}

	err = vcap_add_rule(vrule);
	if (err) {
		netdev_dbg(port->dev, "Failed to add rule based on etype");
		vcap_free_rule(vrule);
		return 1;
	}

	vcap_free_rule(vrule);
	return 0;
}

static int lan966x_is1_add_dmac(struct lan966x_port *port,
				enum vcap_user user,
				u32 *rule_id, u8 *oui)
{
	int chain_id = LAN966X_VCAP_CID_IS1_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_u48_key dmac;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->lan966x->vcap_ctrl, port->dev, chain_id,
				user, prio, *rule_id);
	if (!vrule || IS_ERR(vrule))
		return 1;

	memcpy(dmac.value, oui, ETH_ALEN);
	memcpy(dmac.mask, prio_oui_mask, ETH_ALEN);

	/* Try first to match with ether type */
	err = vcap_rule_add_key_bit(vrule, VCAP_KF_LOOKUP_INDEX, 0);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				     ~BIT(port->chip_port));
	err |= vcap_rule_add_key_u48(vrule, VCAP_KF_L2_DMAC, &dmac);
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_S1);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, 7);
	err |= vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		netdev_dbg(port->dev, "Failed to add rule based on dmac");
		vcap_free_rule(vrule);
		return 1;
	}

	err = vcap_add_rule(vrule);
	if (err) {
		netdev_dbg(port->dev, "Failed to add rule based on dmac");
		vcap_free_rule(vrule);
		return 1;
	}

	vcap_free_rule(vrule);
	return 0;
}

int lan966x_add_prio_is1_rule(struct lan966x_port *port, enum vcap_user user, u32 *rule_id)
{
	u8 oui[ETH_ALEN];
	u32 ethertype;

	*rule_id = 0; /* Returned rule_id to be used when deleting */

	if (user == VCAP_USER_MRP) {
		ethertype = ETH_P_MRP;
		ether_addr_copy(oui, prio_oui_mrp);
		*rule_id = LAN966X_MRP_RULE_ID_OFFSET + port->chip_port;
	} else if (user == VCAP_USER_CFM) {
		ethertype = ETH_P_CFM;
		ether_addr_copy(oui, prio_oui_raps);
		*rule_id = LAN966X_CFM_RULE_ID_OFFSET + port->chip_port;
	} else
		return 1;

	if (lan966x_is1_add_ether(port, user, rule_id, ethertype))
		if (lan966x_is1_add_dmac(port, user, rule_id, oui))
			return 1;

	return 0;
}

void lan966x_del_prio_is1_rule(struct lan966x_port *port, u32 rule_id)
{
	vcap_del_rule(port->lan966x->vcap_ctrl, port->dev, rule_id);
}

void lan966x_dmac_enable(struct lan966x_port *port, int lookup, bool enable)
{
	struct lan966x *lan966x = port->lan966x;
	u32 value;

	if (enable) {
		value = lan_rd(lan966x, ANA_VCAP_CFG(port->chip_port));
		value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
		value |= BIT(lookup);

		lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
			ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
			lan966x, ANA_VCAP_CFG(port->chip_port));
	}
	else {
		value = lan_rd(lan966x, ANA_VCAP_CFG(port->chip_port));
		value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
		value &= !BIT(lookup);

		lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
			ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
			lan966x, ANA_VCAP_CFG(port->chip_port));
	}
}
