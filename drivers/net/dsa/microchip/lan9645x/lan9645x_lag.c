// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

int lan9645x_lag_join_prepare(struct lan9645x *lan9645x,
			      struct netdev_lag_upper_info *info,
			      struct netlink_ext_ack *extack)
{
	if (info->tx_type != NETDEV_LAG_TX_TYPE_HASH) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Can only offload LAG using hash TX type");
		return -EOPNOTSUPP;
	}

	switch (info->hash_type) {
	case NETDEV_LAG_HASH_L2:
	case NETDEV_LAG_HASH_L34:
	case NETDEV_LAG_HASH_L23:
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack,
				   "LAG device using unsupported hash type. Valid types: layer2, layer2+3 and layer3+4.");
		return -EINVAL;
	}

	return 0;
}

u32 lan9645x_lag_dev_get_mask(struct lan9645x *lan9645x,
			      struct net_device *bond)
{
	struct lan9645x_port *p;
	u32 mask = 0;
	int port;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	if (!bond)
		return mask;

	lan9645x_for_each_port(lan9645x, port, p) {
		if (p->bond == bond)
			mask |= BIT(port);
	}

	return mask;
}

static int lan9645x_lag_mask_get_id(struct lan9645x *lan9645x, u32 bond_mask)
{
	if (bond_mask)
		return __ffs(bond_mask);

	return -ENOENT;
}

int lan9645x_lag_dev_get_id(struct lan9645x *lan9645x, struct net_device *bond)
{
	u32 mask = lan9645x_lag_dev_get_mask(lan9645x, bond);

	return lan9645x_lag_mask_get_id(lan9645x, mask);
}

void lan9645x_lag_port_set_pgids(struct lan9645x *lan9645x, int port,
				 bool leaving, u32 bond_mask)
{
	u8 active_idx[PGID_AGGR_NUM];
	int num_active = 0;
	u32 pchoice;
	int p, ac;

	/* Update port pgids for this bond */
	if (leaving)
		lan_wr(BIT(port), lan9645x, ANA_PGID(port));

	lan9645x_for_each_chipport(lan9645x, p) {
		if (!(BIT(p) & bond_mask))
			continue;

		lan_wr(bond_mask, lan9645x, ANA_PGID(p));

		if (lan9645x->ports[p]->lag_tx_active)
			active_idx[num_active++] = p;
	}

	/* Update Aggregation PGIDS for this bond.
	 *
	 * Each aggregation code must pick out a single active port in the bond.
	 *
	 * When a port is leaving the bond, we must set the port bit to 1,
	 * ensuring the aggregation codes make no restriction on the forwarding
	 * decision for the leaving port.
	 */
	for (ac = PGID_AGGR; ac < PGID_SRC; ac++) {
		pchoice = leaving ? BIT(port) : 0;

		if (num_active)
			pchoice |= BIT(active_idx[ac % num_active]);

		lan_rmw(pchoice, bond_mask | BIT(port), lan9645x,
			ANA_PGID(ac));
	}
}

static void lan9645x_lag_set_logical_port_ids(struct lan9645x *lan9645x,
					      int port, u32 bond_mask,
					      int lag_id, bool leaving)
{
	int p;

	if (leaving)
		lan_rmw(ANA_PORT_CFG_PORTID_VAL_SET(port),
			ANA_PORT_CFG_PORTID_VAL, lan9645x,
			ANA_PORT_CFG(port));

	lan9645x_for_each_chipport(lan9645x, p)
	{
		if (!(BIT(p) & bond_mask))
			continue;

		/* PORTID_VAL is used as pgid when learning etc. */
		lan_rmw(ANA_PORT_CFG_PORTID_VAL_SET(lag_id),
			ANA_PORT_CFG_PORTID_VAL, lan9645x,
			ANA_PORT_CFG(p));
	}
}

static const char *lag_hash_types[16] = {
	[NETDEV_LAG_HASH_NONE] = "None",
	[NETDEV_LAG_HASH_L2] = "L2",
	[NETDEV_LAG_HASH_L34] = "L34",
	[NETDEV_LAG_HASH_L23] = "L23",
	[NETDEV_LAG_HASH_E23] = "E23",
	[NETDEV_LAG_HASH_E34] = "E34",
	[NETDEV_LAG_HASH_VLAN_SRCMAC] = "VLAN_SRCMAC",
	[NETDEV_LAG_HASH_UNKNOWN] = "Unknown",
};

static void __lan9645x_lag_apply_hash_type(struct lan9645x *lan9645x,
					   enum netdev_lag_hash hash_type)
{
	switch (hash_type) {
	case NETDEV_LAG_HASH_L2:
		lan_wr(ANA_AGGR_CFG_AC_DMAC_ENA_SET(1) |
		       ANA_AGGR_CFG_AC_SMAC_ENA_SET(1),
		       lan9645x, ANA_AGGR_CFG);
		break;
	case NETDEV_LAG_HASH_L34:
		lan_wr(ANA_AGGR_CFG_AC_IP4_SIPDIP_ENA_SET(1) |
		       ANA_AGGR_CFG_AC_IP6_FLOW_LBL_ENA_SET(1) |
		       ANA_AGGR_CFG_AC_IP6_TCPUDP_ENA_SET(1) |
		       ANA_AGGR_CFG_AC_IP4_TCPUDP_ENA_SET(1),
		       lan9645x, ANA_AGGR_CFG);
		break;
	case NETDEV_LAG_HASH_L23:
		lan_wr(ANA_AGGR_CFG_AC_DMAC_ENA_SET(1) |
		       ANA_AGGR_CFG_AC_SMAC_ENA_SET(1) |
		       ANA_AGGR_CFG_AC_IP4_SIPDIP_ENA_SET(1) |
		       ANA_AGGR_CFG_AC_IP6_FLOW_LBL_ENA_SET(1),
		       lan9645x, ANA_AGGR_CFG);
		break;
	default:
		dev_err(lan9645x->dev, "Unsupported LAG hash type: %s",
			lag_hash_types[hash_type]);
		return;
	}

	dev_info(lan9645x->dev, "Global LAG hashtype configured: %s",
		 lag_hash_types[hash_type]);
}

static int lan9645x_lag_get_hash_type(struct lan9645x *lan9645x)
{
	struct lan9645x_port *p;
	int port;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	lan9645x_for_each_port(lan9645x, port, p)
	{
		if (p->bond && p->hash_type != NETDEV_LAG_HASH_NONE)
			return p->hash_type;
	}

	return NETDEV_LAG_HASH_NONE;
}

int lan9645x_lag_apply_hash_type(struct lan9645x *lan9645x,
				 struct netdev_lag_upper_info *info,
				 struct netlink_ext_ack *extack)
{
	enum netdev_lag_hash ht;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	ht = lan9645x_lag_get_hash_type(lan9645x);
	if (ht == NETDEV_LAG_HASH_NONE) {
		__lan9645x_lag_apply_hash_type(lan9645x, info->hash_type);

	} else if (ht != info->hash_type) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "LAG can not change existing hashtype: %s. Hashtype is global for all LAGs.",
				       lag_hash_types[ht]);
		return -EINVAL;
	}

	return 0;
}

int lan9645x_lag_reconfigure(struct lan9645x *lan9645x, struct net_device *bond,
			     int port, bool leaving)
{
	u32 bond_mask;
	int new_lag_id;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	bond_mask = lan9645x_lag_dev_get_mask(lan9645x, bond);
	new_lag_id = lan9645x_lag_mask_get_id(lan9645x, bond_mask);

	lan9645x_lag_set_logical_port_ids(lan9645x, port, bond_mask, new_lag_id,
					  leaving);
	lan9645x_update_fwd_mask(lan9645x, !leaving);
	lan9645x_lag_port_set_pgids(lan9645x, port, leaving, bond_mask);

	return new_lag_id;
}
