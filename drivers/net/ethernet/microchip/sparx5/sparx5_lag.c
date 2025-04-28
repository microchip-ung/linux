// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/if_bridge.h>
#include <linux/netdevice.h>

#include <net/switchdev.h>

#include "sparx5_main.h"

#define LAG_MASKS_CNT 16

static void sparx5_lag_add(struct sparx5_port *port,
			   struct net_device *lag_master)
{
	lockdep_assert_held(&port->sparx5->lock);

	port->lag_master = lag_master;
}

static void sparx5_lag_del(struct sparx5_port *port)
{
	lockdep_assert_held(&port->sparx5->lock);

	port->lag_master = NULL;
}

static void sparx5_lag_add_locked(struct sparx5_port *port,
				  struct net_device *lag_master)
{
	struct sparx5 *sparx5 = port->sparx5;

	spin_lock(&sparx5->lock);
	sparx5_lag_add(port, lag_master);
	spin_unlock(&sparx5->lock);
}

static void sparx5_lag_del_locked(struct sparx5_port *port)
{
	struct sparx5 *sparx5 = port->sparx5;

	spin_lock(&sparx5->lock);
	sparx5_lag_del(port);
	spin_unlock(&sparx5->lock);
}

void sparx5_lag_mask_get(struct sparx5 *sparx5, struct net_device *lag_master,
			 unsigned long *lag_mask)
{
	struct sparx5_port *port;

	bitmap_zero(lag_mask, SPX5_PORTS);

	if (!lag_master)
		return;

	for (int i = 0; i < sparx5->data->consts.chip_ports; i++) {
		port = sparx5->ports[i];
		if (!port)
			continue;

		if (port->lag_master == lag_master)
			set_bit(i, lag_mask);
	}
}

bool sparx5_lag_is_first(struct net_device *lag_master, struct net_device *dev)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *sparx5 = port->sparx5;
	DECLARE_BITMAP(lag_mask, SPX5_PORTS);

	if (port->lag_master != lag_master)
		return false;

	sparx5_lag_mask_get(sparx5, lag_master, lag_mask);

	return port->portno == find_first_bit(lag_mask, SPX5_PORTS);
}

/* The LAG id is the lowest physical port number within the LAG. */
static int sparx5_lag_id_get(struct sparx5 *sparx5,
			     struct net_device *lag_master)
{
	DECLARE_BITMAP(lag_mask, SPX5_PORTS);

	sparx5_lag_mask_get(sparx5, lag_master, lag_mask);

	if (bitmap_empty(lag_mask, SPX5_PORTS))
		return -EINVAL;

	return find_first_bit(lag_mask, SPX5_PORTS);
}

static bool sparx5_lag_hash_type_check(struct sparx5 *sparx5,
				       enum netdev_lag_hash hash_type)
{
	for (int i = 0; i < sparx5->data->consts.chip_ports; i++) {
		struct sparx5_port *port = sparx5->ports[i];

		if (!port || !port->lag_master)
			continue;

		if (port->lag_hash_type != hash_type)
			return false;
	}

	return true;
}

int sparx5_lag_aggr_code_set(struct net_device *dev,
			     struct netdev_notifier_changeupper_info *info)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct netdev_lag_upper_info *upper_info;
	struct sparx5 *sparx5 = port->sparx5;
	struct netlink_ext_ack *extack;

	extack = netdev_notifier_info_to_extack(&info->info);

	upper_info = info->upper_info;
	if (!upper_info) {
		port->lag_hash_type = NETDEV_LAG_HASH_NONE;
		return NOTIFY_DONE;
	}

	if (upper_info->tx_type != NETDEV_LAG_TX_TYPE_HASH) {
		NL_SET_ERR_MSG_MOD(extack,
				   "LAG device using unsupported TX type");
		return -EINVAL;
	}

	if (!sparx5_lag_hash_type_check(sparx5, upper_info->hash_type)) {
		NL_SET_ERR_MSG_MOD(extack, "LAG devices must have the same hash_type");
		return -EINVAL;
	}

	switch (upper_info->hash_type) {
	case NETDEV_LAG_HASH_L2:
				/* L2 */
		spx5_wr(ANA_CL_AGGR_CFG_AGGR_DMAC_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_SMAC_ENA_SET(1),
			sparx5, ANA_CL_AGGR_CFG);
		break;
	case NETDEV_LAG_HASH_L34:
				/* L3 */
		spx5_wr(ANA_CL_AGGR_CFG_AGGR_IP4_SIP_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_IP4_DIP_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_IP6_SIP_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_IP6_DIP_ENA_SET(1) |
				/* L4 */
				ANA_CL_AGGR_CFG_AGGR_IP4_TCPUDP_PORT_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_IP6_TCPUDP_PORT_ENA_SET(1),
			sparx5, ANA_CL_AGGR_CFG);
		break;
	case NETDEV_LAG_HASH_L23:
				/* L2 */
		spx5_wr(ANA_CL_AGGR_CFG_AGGR_DMAC_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_SMAC_ENA_SET(1) |
				/* L3 */
				ANA_CL_AGGR_CFG_AGGR_IP4_SIP_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_IP4_DIP_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_IP6_SIP_ENA_SET(1) |
				ANA_CL_AGGR_CFG_AGGR_IP6_DIP_ENA_SET(1),
			sparx5, ANA_CL_AGGR_CFG);
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack,
				   "LAG device using unsupported hash type");
		return -EINVAL;
	}

	port->lag_hash_type = upper_info->hash_type;

	return NOTIFY_OK;
}

static void sparx5_lag_map_logical_ports(struct sparx5 *sparx5)
{
	struct sparx5_port *port;
	int logical_port;

	for (int i = 0; i < sparx5->data->consts.chip_ports; i++) {
		port = sparx5->ports[i];
		if (!port)
			continue;

		if (port->lag_master) {
			/* If this port is part of a LAG, the logical port
			 * number is the physical port number of the port with
			 * the lowest port number within the LAG.
			 */
			logical_port = sparx5_lag_id_get(sparx5,
							 port->lag_master);
			if (logical_port < 0)
				/* This should not happen. */
				logical_port = port->portno;
		} else {
			/* By default, the logical port number is the same as
			 * the physical port number.
			 */
			logical_port = port->portno;
		}

		spx5_rmw(ANA_CL_PORT_ID_CFG_LPORT_NUM_SET(logical_port),
			 ANA_CL_PORT_ID_CFG_LPORT_NUM, sparx5,
			 ANA_CL_PORT_ID_CFG(i));
	}
}

/* Set the aggregation masks.
 *
 * By default all aggregation masks will be all ones, corresponding to no
 * forwarding restrictions wrt. link aggregation. For port devices under a LAG,
 * only one port per LAG must be set in each aggregation mask. Effectively, this
 * means that a frame will only get forwarded to one port per LAG.
 *
 * To enforce this, we get a mask of each port member of a LAG, and distribute
 * each port across all aggregation masks.
 */
int sparx5_lag_aggr_masks_set(struct sparx5_port *port, bool leaving)
{
	const struct sparx5_consts *consts = &port->sparx5->data->consts;
	struct sparx5 *sparx5 = port->sparx5;
	DECLARE_BITMAP(lag_mask, SPX5_PORTS);
	int n_active_ports = 0, portno = 0;
	u8 active_idx[LAG_MASKS_CNT];
	u32 mask;

	sparx5_lag_mask_get(sparx5, port->lag_master, lag_mask);

	for_each_set_bit(portno, lag_mask, consts->chip_ports)
		if (sparx5->ports[portno]->lag_tx_active)
			active_idx[n_active_ports++] = portno;

	if (!leaving && n_active_ports > LAG_MASKS_CNT)
		return -EOPNOTSUPP;

	for (int i = 0; i < LAG_MASKS_CNT; i++) {
		mask = leaving ? BIT(port->portno) : 0;

		if (n_active_ports)
			mask |= BIT(active_idx[i % n_active_ports]);

		spx5_rmw(mask, lag_mask[0] | BIT(port->portno), sparx5,
			 ANA_AC_AGGR_CFG(i));

		if (is_sparx5(sparx5))
			spx5_rmw(mask, lag_mask[1] | BIT(port->portno), sparx5,
				 ANA_AC_AGGR_CFG(i));
	}

	return 0;
}

static int sparx5_lag_replace_mact_entries(struct sparx5_port *src_port,
					   struct sparx5_port *dst_port)
{
	struct sparx5_mact_entry *mact_entry, *tmp;
	struct sparx5 *sparx5 = src_port->sparx5;

	mutex_lock(&sparx5->mact_lock);
	list_for_each_entry_safe(mact_entry, tmp, &sparx5->mact_entries, list) {
		if (mact_entry->port == src_port->portno && mact_entry->lag) {
			sparx5_mact_forget(sparx5, mact_entry->mac,
					   mact_entry->vid);

			sparx5_mact_learn(sparx5, dst_port->portno,
					  mact_entry->mac, mact_entry->vid);

			mact_entry->port = dst_port->portno;
		}
	}
	mutex_unlock(&sparx5->mact_lock);

	return 0;
}

static int sparx5_lag_remove_mact_entries(struct sparx5_port *src_port)
{
	struct sparx5_mact_entry *mact_entry, *tmp;
	struct sparx5 *sparx5 = src_port->sparx5;

	mutex_lock(&sparx5->mact_lock);
	list_for_each_entry_safe(mact_entry, tmp, &sparx5->mact_entries, list) {
		if (mact_entry->port == src_port->portno && mact_entry->lag) {
			sparx5_mact_forget(sparx5, mact_entry->mac,
					   mact_entry->vid);

			list_del(&mact_entry->list);
			devm_kfree(sparx5->dev, mact_entry);
		}
	}
	mutex_unlock(&sparx5->mact_lock);

	return 0;
}

int sparx5_lag_join(struct sparx5_port *port, struct net_device *brport_dev,
		    struct net_device *lag_master,
		    struct netlink_ext_ack *extack)
{
	struct sparx5 *sparx5 = port->sparx5;
	struct net_device *dev = port->ndev;
	struct net_device *br;
	int lag_id, err;

	br = netdev_master_upper_dev_get(lag_master);
	if (br && netif_is_bridge_master(br)) {
		/* If a port is joining a LAG, which in turn is under a bridge,
		 * we have to make sure the port joining the LAG, is also
		 * properly joining the bridge. The easiest way to do this, is
		 * to generate an event for the joining port, with the upper_dev
		 * being the bridge, instead of the LAG.
		 */

		struct netdev_notifier_changeupper_info info = {
			.linking = true,
			.upper_dev = br,
		};

		/* Port PVID is not set in case the netdevice event was for a
		 * lag device. Set it here. Is reset on bridge_leave();
		 */
		port->pvid = 1;

		sparx5_port_prechangeupper(port->ndev, port->ndev, &info);
		sparx5_port_changeupper(port->ndev, port->ndev, &info);

		/* Must be called _after_ the port joins the bridge */
		sparx5_attr_stp_state_set(port,
					  br_port_get_stp_state(brport_dev));
	}

	/* Get the lagid before adding the new port. */
	lag_id = sparx5_lag_id_get(sparx5, lag_master);

	sparx5_lag_add_locked(port, lag_master);

	/* MAC table entries for LAG ports, are added for the LAG port
	 * with the lowest physical port number (lagid). If a new port, with a
	 * lower physical port number than the current lagid joins the lag, all
	 * LAG entries have to be updated.
	 */
	if (lag_id >= 0 && sparx5_lag_is_first(lag_master, dev))
		sparx5_lag_replace_mact_entries(sparx5->ports[lag_id], port);

	sparx5_update_dst_fwd(sparx5);

	err = sparx5_lag_aggr_masks_set(port, false);
	if (err)
		return err;

	sparx5_lag_map_logical_ports(sparx5);

	return 0;
}

void sparx5_lag_leave(struct sparx5_port *port, struct net_device *lag_master)
{
	DECLARE_BITMAP(lag_mask, SPX5_PORTS);
	struct sparx5 *sparx5 = port->sparx5;
	struct net_device *br;
	int lag_id;

	br = netdev_master_upper_dev_get(lag_master);
	if (br && netif_is_bridge_master(br)) {
		/* If a port is leaving a LAG, which in turn is under a bridge,
		 * we have to make sure the port leaving the LAG, is also
		 * properly leaving the bridge. The easiest way to do this, is
		 * to generate an event for the leaving port, with the upper_dev
		 * being the bridge, instead og the LAG.
		 */

		struct netdev_notifier_changeupper_info info = {
			.linking = false,
			.upper_dev = br,
		};

		/* Must be called _before_ the port leaves the bridge */
		sparx5_attr_stp_state_set(port,
					  br_port_get_stp_state(port->ndev));

		sparx5_port_prechangeupper(port->ndev, port->ndev, &info);
		sparx5_port_changeupper(port->ndev, port->ndev, &info);
	}

	if (sparx5_lag_is_first(lag_master, port->ndev)) {
		/* MAC table entries for LAG ports, are added for the LAG port
		 * with the lowest physical port number (lagid). If that port
		 * leaves the lag, all LAG entries have to be either updated to
		 * the new lagid, or removed entirely.
		 */

		sparx5_lag_del_locked(port);

		sparx5_lag_mask_get(sparx5, lag_master, lag_mask);

		if (!bitmap_empty(lag_mask, SPX5_PORTS)) {
			lag_id = sparx5_lag_id_get(sparx5, lag_master);

			sparx5_lag_replace_mact_entries(port,
							sparx5->ports[lag_id]);
		} else {
			sparx5_lag_remove_mact_entries(port);
		}
	} else {
		sparx5_lag_del_locked(port);
	}


	sparx5_update_dst_fwd(sparx5);
	/* Ignore return value */
	sparx5_lag_aggr_masks_set(port, true);
	sparx5_lag_map_logical_ports(sparx5);
}
