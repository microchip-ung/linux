// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/if_bridge.h>
#include <net/switchdev.h>

#include "lan966x_main.h"

#ifdef CONFIG_BRIDGE_MRP
#include "lan966x_mrp.h"
#endif
#ifdef CONFIG_BRIDGE_CFM
#include "lan966x_cfm.h"
#endif

static struct workqueue_struct *lan966x_owq;

struct lan966x_switchdev_event_work {
	struct work_struct work;
	struct switchdev_notifier_fdb_info fdb_info;
	struct net_device *dev;
	unsigned long event;
};

//#define LAN966X_NETDEV_DBG

#ifdef LAN966X_NETDEV_DBG
static const char * const netdev_cmd_strings[] = {
	[0]                             = "NETDEV_UNDEFINED",
	[NETDEV_UP]                     = "NETDEV_UP",
	[NETDEV_DOWN]                   = "NETDEV_DOWN",
	[NETDEV_REBOOT]                 = "NETDEV_REBOOT",
	[NETDEV_CHANGE]                 = "NETDEV_CHANGE",
	[NETDEV_REGISTER]               = "NETDEV_REGISTER",
	[NETDEV_UNREGISTER]             = "NETDEV_UNREGISTER",
	[NETDEV_CHANGEMTU]              = "NETDEV_CHANGEMTU",
	[NETDEV_CHANGEADDR]             = "NETDEV_CHANGEADDR",
	[NETDEV_PRE_CHANGEADDR]         = "NETDEV_PRE_CHANGEADDR",
	[NETDEV_GOING_DOWN]             = "NETDEV_GOING_DOWN",
	[NETDEV_CHANGENAME]             = "NETDEV_CHANGENAME",
	[NETDEV_FEAT_CHANGE]            = "NETDEV_FEAT_CHANGE",
	[NETDEV_BONDING_FAILOVER]       = "NETDEV_BONDING_FAILOVER",
	[NETDEV_PRE_UP]                 = "NETDEV_PRE_UP",
	[NETDEV_PRE_TYPE_CHANGE]        = "NETDEV_PRE_TYPE_CHANGE",
	[NETDEV_POST_TYPE_CHANGE]       = "NETDEV_POST_TYPE_CHANGE",
	[NETDEV_POST_INIT]              = "NETDEV_POST_INIT",
	[NETDEV_RELEASE]                = "NETDEV_RELEASE",
	[NETDEV_NOTIFY_PEERS]           = "NETDEV_NOTIFY_PEERS",
	[NETDEV_JOIN]                   = "NETDEV_JOIN",
	[NETDEV_CHANGEUPPER]            = "NETDEV_CHANGEUPPER",
	[NETDEV_RESEND_IGMP]            = "NETDEV_RESEND_IGMP",
	[NETDEV_PRECHANGEMTU]           = "NETDEV_PRECHANGEMTU",
	[NETDEV_CHANGEINFODATA]         = "NETDEV_CHANGEINFODATA",
	[NETDEV_BONDING_INFO]           = "NETDEV_BONDING_INFO",
	[NETDEV_PRECHANGEUPPER]         = "NETDEV_PRECHANGEUPPER",
	[NETDEV_CHANGELOWERSTATE]       = "NETDEV_CHANGELOWERSTATE",
	[NETDEV_UDP_TUNNEL_PUSH_INFO]   = "NETDEV_UDP_TUNNEL_PUSH_INFO",
	[NETDEV_UDP_TUNNEL_DROP_INFO]   = "NETDEV_UDP_TUNNEL_DROP_INFO",
	[NETDEV_CHANGE_TX_QUEUE_LEN]    = "NETDEV_CHANGE_TX_QUEUE_LEN",
	[NETDEV_CVLAN_FILTER_PUSH_INFO] = "NETDEV_CVLAN_FILTER_PUSH_INFO",
	[NETDEV_CVLAN_FILTER_DROP_INFO] = "NETDEV_CVLAN_FILTER_DROP_INFO",
	[NETDEV_SVLAN_FILTER_PUSH_INFO] = "NETDEV_SVLAN_FILTER_PUSH_INFO",
	[NETDEV_SVLAN_FILTER_DROP_INFO] = "NETDEV_SVLAN_FILTER_DROP_INFO",
};

static void lan966x_netdev_dbg(struct net_device *dev,
			       struct notifier_block *nb,
			       unsigned long event, void *ptr)
{
	const char *mst = "";

	if (netif_is_bridge_master(dev))
		mst = "BRIDGE_MASTER:";
	else if (netif_is_lag_master(dev))
		mst = "LAG_MASTER:";

	switch (event) {
	case NETDEV_CHANGE:
	case NETDEV_CHANGEINFODATA: {
		struct netdev_notifier_change_info *i = ptr;
		netdev_dbg(dev,
			   "%s%s: i_dev %s flags_changed 0x%x\n",
			   mst,
			   netdev_cmd_strings[event],
			   i->info.dev->name,
			   i->flags_changed);
		break;
	}
	case NETDEV_CHANGEMTU: {
		struct netdev_notifier_info_ext *i = ptr;
		netdev_dbg(dev,
			   "%s%s: i_dev %s mtu %u\n",
			   mst,
			   netdev_cmd_strings[event],
			   i->info.dev->name,
			   i->ext.mtu);
		break;
	}
	case NETDEV_PRE_CHANGEADDR: {
		struct netdev_notifier_pre_changeaddr_info *i = ptr;
		const char *dev_addr = "<none>";

		if (i->dev_addr && strlen(i->dev_addr))
			dev_addr = i->dev_addr;

		netdev_dbg(dev,
			   "%s%s: i_dev %s addr %s\n",
			   mst,
			   netdev_cmd_strings[event],
			   i->info.dev->name,
			   dev_addr);
		break;
	}
	case NETDEV_CHANGEUPPER:
	case NETDEV_PRECHANGEUPPER: {
		struct netdev_notifier_changeupper_info *i = ptr;
		struct netdev_lag_upper_info *ui = i->upper_info;
		netdev_dbg(dev,
			   "%s%s: i_dev %s u_dev %s master %d linking %d tx_type %d hash_type %d\n",
			   mst,
			   netdev_cmd_strings[event],
			   i->info.dev->name,
			   i->upper_dev->name,
			   i->master,
			   i->linking,
			   ui ? ui->tx_type : -1, /* e.g. NETDEV_LAG_TX_TYPE_HASH */
			   ui ? ui->hash_type : -1); /* e.g. NETDEV_LAG_HASH_L2 */
		break;
	}
	case NETDEV_BONDING_INFO: {
		struct netdev_notifier_bonding_info *i = ptr;
		ifslave *s = &i->bonding_info.slave;
		ifbond *m = &i->bonding_info.master;
		netdev_dbg(dev,
			   "%s%s: i_dev %s s_id %d s_name %s s_link %d s_state %d s_lfc %u m_bond_mode %d m_num_slaves %d m_miimon %d\n",
			   mst,
			   netdev_cmd_strings[event],
			   i->info.dev->name,
			   s->slave_id,
			   s->slave_name,
			   s->link, /* e.g. BOND_LINK_UP */
			   s->state, /* e.g. BOND_STATE_ACTIVE */
			   s->link_failure_count,
			   m->bond_mode, /* e.g. BOND_MODE_XOR */
			   m->num_slaves,
			   m->miimon);
		break;
	}
	case NETDEV_CHANGELOWERSTATE: {
		struct netdev_notifier_changelowerstate_info *i = ptr;
		struct netdev_lag_lower_state_info *lsi = i->lower_state_info;
		netdev_dbg(dev,
			   "%s%s: i_dev %s link_up %d tx_enabled %d\n",
			   mst,
			   netdev_cmd_strings[event],
			   i->info.dev->name,
			   lsi->link_up,
			   lsi->tx_enabled);
		break;
	}
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_REBOOT:
	case NETDEV_REGISTER:
	case NETDEV_UNREGISTER:
	case NETDEV_CHANGEADDR:
	case NETDEV_GOING_DOWN:
	case NETDEV_CHANGENAME:
	case NETDEV_FEAT_CHANGE:
	case NETDEV_BONDING_FAILOVER:
	case NETDEV_PRE_UP:
	case NETDEV_PRE_TYPE_CHANGE:
	case NETDEV_POST_TYPE_CHANGE:
	case NETDEV_POST_INIT:
	case NETDEV_RELEASE:
	case NETDEV_NOTIFY_PEERS:
	case NETDEV_JOIN:
	case NETDEV_RESEND_IGMP:
	case NETDEV_PRECHANGEMTU:
	case NETDEV_UDP_TUNNEL_PUSH_INFO:
	case NETDEV_UDP_TUNNEL_DROP_INFO:
	case NETDEV_CHANGE_TX_QUEUE_LEN:
	case NETDEV_CVLAN_FILTER_PUSH_INFO:
	case NETDEV_CVLAN_FILTER_DROP_INFO:
	case NETDEV_SVLAN_FILTER_PUSH_INFO:
	case NETDEV_SVLAN_FILTER_DROP_INFO: {
		struct netdev_notifier_info *i = ptr;
		netdev_dbg(dev,
			   "%s%s: i_dev %s\n",
			   mst,
			   netdev_cmd_strings[event],
			   i->dev->name);
		break;
	}
	default:
		netdev_dbg(dev,
			   "!!! Unhandled event %lu !!!\n",
			   event);
	}
}
#endif /* LAN966X_NETDEV_DBG */

static void lan966x_multicast_clear(struct lan966x *lan966x,
				    struct lan966x_port *port);
static void lan966x_multicast_restore(struct lan966x *lan966x,
				      struct lan966x_port *port);

static int lan966x_port_bridge_join(struct lan966x_port *lan966x_port,
				    struct net_device *bridge,
				    struct netlink_ext_ack *extack)
{
	struct lan966x *lan966x = lan966x_port->lan966x;
	struct net_device *dev = lan966x_port->dev;
	int err;

	err = switchdev_bridge_port_offload(dev, dev, NULL, NULL, NULL,
					    false, extack);
	if (err)
		return err;

	if (!lan966x->bridge_mask) {
		lan966x->hw_bridge_dev = bridge;
	} else {
		if (lan966x->hw_bridge_dev != bridge)
			/* This is adding the port to a second bridge, this is
			 * unsupported
			 */
			return -ENODEV;
	}

	lan966x->bridge_mask |= BIT(lan966x_port->chip_port);
	/* The port can't be in promisc mode when it is under the bridge */
	lan966x_set_promisc(lan966x_port, false, true);

	/* Port enters in bridge mode therefor don't need to copy to CPU
	 * frames for multicast in case the bridge is not requesting them
	 */
	__dev_mc_unsync(lan966x_port->dev, lan966x_mc_unsync);

	return 0;
}

static void lan966x_port_bridge_leave(struct lan966x_port *lan966x_port,
				      struct net_device *bridge)
{
	struct lan966x *lan966x = lan966x_port->lan966x;

	switchdev_bridge_port_unoffload(lan966x_port->dev, NULL, NULL, NULL);

	lan966x->bridge_mask &= ~BIT(lan966x_port->chip_port);

	if (!lan966x->bridge_mask)
		lan966x->hw_bridge_dev = NULL;

	if (lan966x_port->promisc_mode)
		lan966x_set_promisc(lan966x_port, true, true);

	/* Clear bridge vlan settings before calling lan966x_vlan_port_apply */
	lan966x_port->vlan_aware = 0;
	lan966x_port->vid = 0;

	lan966x_port->pvid = PORT_PVID;
	lan966x->vlan_mask[lan966x_port->pvid] |= BIT(lan966x_port->chip_port);
	lan966x_vlant_set_mask(lan966x, lan966x_port->pvid);

	/* Port enters in host more therefore restore mc list */
	__dev_mc_sync(lan966x_port->dev, lan966x_mc_sync, lan966x_mc_unsync);
}

static unsigned long lan966x_get_bond_mask(struct lan966x *lan966x,
					   struct net_device *bond,
					   bool only_active_ports)
{
	unsigned long mask = 0;
	int p;

	for (p = 0; p < lan966x->num_phys_ports; p++) {
		struct lan966x_port *port = lan966x->ports[p];

		if (!port)
			continue;

		if (port->bond == bond) {
			if (only_active_ports && !port->lag_tx_active)
				continue;

			mask |= BIT(p);
		}
	}

	return mask;
}

void lan966x_apply_bridge_fwd_mask(struct lan966x *lan966x)
{
	int p;

	/* Apply FWD mask. The loop is needed to add/remove the current port as
	 * a source for the other ports.
	 */
	for (p = 0; p < lan966x->num_phys_ports; p++) {
		struct lan966x_port *port = lan966x->ports[p];
		unsigned long mask = 0;

		if (port && (lan966x->bridge_fwd_mask & BIT(p))) {
			mask = lan966x->bridge_fwd_mask & ~BIT(p);

			if (port->bond) /* Also remove all bond ports */
				mask &= ~lan966x_get_bond_mask(lan966x,
							       port->bond, false);
		}

		mask |= BIT(CPU_PORT);

		lan_wr(ANA_PGID_PGID_SET(mask), lan966x, ANA_PGID(PGID_SRC + p));
	}
}

static void lan966x_set_aggr_pgids(struct lan966x *lan966x)
{
	unsigned long visited = GENMASK(lan966x->num_phys_ports - 1, 0);
	int i, p, lag;

	/* Reset destination and aggregation PGIDS */
	for_each_unicast_dest_pgid(lan966x, p)
		lan_wr(ANA_PGID_PGID_SET(BIT(p)), lan966x, ANA_PGID(p));

	for_each_aggr_pgid(lan966x, i)
		lan_wr(ANA_PGID_PGID_SET(GENMASK(lan966x->num_phys_ports - 1, 0)),
		       lan966x, ANA_PGID(i));

	/* The visited ports bitmask holds the list of ports offloading any
	 * bonding interface. Initially we mark all these ports as unvisited,
	 * then every time we visit a port in this bitmask, we know that it is
	 * the lowest numbered port, i.e. the one whose logical ID == physical
	 * port ID == LAG ID. So we mark as visited all further ports in the
	 * bitmask that are offloading the same bonding interface. This way,
	 * we set up the aggregation PGIDs only once per bonding interface.
	 */

	for (p = 0; p < lan966x->num_phys_ports; p++) {
		struct lan966x_port *port = lan966x->ports[p];

		if (!port || !port->bond)
			continue;

		visited &= ~BIT(p);
	}

	/* Now, set PGIDs for each active LAG */
	for (lag = 0; lag < lan966x->num_phys_ports; lag++) {
		struct lan966x_port *lag_port = lan966x->ports[lag];
		int num_active_ports = 0;
		struct net_device *bond;
		unsigned long bond_mask;
		u8 aggr_idx[16];

		if (!lag_port || !lag_port->bond || (visited & BIT(lag)))
			continue;

		bond = lag_port->bond;
		bond_mask = lan966x_get_bond_mask(lan966x, bond, true);

		for_each_set_bit(p, &bond_mask, lan966x->num_phys_ports) {
			// Destination mask
			lan_wr(ANA_PGID_PGID_SET(bond_mask), lan966x, ANA_PGID(p));
			aggr_idx[num_active_ports++] = p;
		}

		for_each_aggr_pgid(lan966x, i) {
			u32 ac;

			ac = lan_rd(lan966x, ANA_PGID(i));
			ac &= ~bond_mask;
			/* Don't do division by zero if there was no active
			 * port. Just make all aggregation codes zero.
			 */
			if (num_active_ports)
				ac |= BIT(aggr_idx[i % num_active_ports]);
			lan_wr(ANA_PGID_PGID_SET(ac), lan966x, ANA_PGID(i));
		}

		/* Mark all ports in the same LAG as visited to avoid applying
		 * the same config again.
		 */
		for (p = lag; p < lan966x->num_phys_ports; p++) {
			struct lan966x_port *port = lan966x->ports[p];

			if (!port)
				continue;

			if (port->bond == bond)
				visited |= BIT(p);
		}
	}
}

/* When offloading a bonding interface, the switch ports configured under the
 * same bond must have the same logical port ID, equal to the physical port ID
 * of the lowest numbered physical port in that bond. Otherwise, in standalone/
 * bridged mode, each port has a logical port ID equal to its physical port ID.
 */
static void lan966x_setup_logical_port_ids(struct lan966x *lan966x)
{
	int p;

	for (p = 0; p < lan966x->num_phys_ports; p++) {
		struct lan966x_port *port = lan966x->ports[p];
		struct net_device *bond;
		unsigned long bond_mask;
		int lag = p; /* Default is physical port ID */

		if (!port)
			continue;

		bond = port->bond;
		if (bond) {
			bond_mask = lan966x_get_bond_mask(lan966x, bond, false);
			if (bond_mask)
				lag = __ffs(bond_mask);
		}

		lan_rmw(ANA_PORT_CFG_PORTID_VAL_SET(lag),
			ANA_PORT_CFG_PORTID_VAL,
			lan966x, ANA_PORT_CFG(p));
	}
}

static int lan966x_port_lag_join(struct lan966x_port *port,
				 struct net_device *bond)
{
	struct lan966x *lan966x = port->lan966x;

	port->bond = bond;

	lan966x_setup_logical_port_ids(lan966x);
	lan966x_apply_bridge_fwd_mask(lan966x);
	lan966x_set_aggr_pgids(lan966x);

	return 0;
}

static void lan966x_port_lag_leave(struct lan966x_port *port,
				   struct net_device *bond)
{
	struct lan966x *lan966x = port->lan966x;

	port->bond = NULL;

	lan966x_setup_logical_port_ids(lan966x);
	lan966x_apply_bridge_fwd_mask(lan966x);
	lan966x_set_aggr_pgids(lan966x);
}

void lan966x_port_lag_change(struct lan966x_port *port, bool lag_tx_active)
{
	struct lan966x *lan966x = port->lan966x;

	port->lag_tx_active = lag_tx_active;

	/* Rebalance the LAGs */
	lan966x_set_aggr_pgids(lan966x);
}

static int
lan966x_netdevice_prechangeupper(struct net_device *dev,
				 struct netdev_notifier_changeupper_info *info)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	int err = 0;

	if (netif_is_lag_master(info->upper_dev)) {
		struct netdev_lag_upper_info *lui = info->upper_info;

		if (!lui)
			return notifier_from_errno(err);

		if (lui->tx_type != NETDEV_LAG_TX_TYPE_HASH) {
			NL_SET_ERR_MSG_MOD(netdev_notifier_info_to_extack(&info->info),
					   "LAG device using unsupported Tx type");
			return notifier_from_errno(-EINVAL);
		}

		switch (lui->hash_type) {
		case NETDEV_LAG_HASH_L2:
			lan_wr(ANA_AGGR_CFG_AC_DMAC_ENA_SET(1) |
			       ANA_AGGR_CFG_AC_SMAC_ENA_SET(1),
			       lan966x, ANA_AGGR_CFG);
			break;
		case NETDEV_LAG_HASH_L34:
			lan_wr(ANA_AGGR_CFG_AC_IP6_TCPUDP_ENA_SET(1) |
			       ANA_AGGR_CFG_AC_IP4_TCPUDP_ENA_SET(1) |
			       ANA_AGGR_CFG_AC_IP4_SIPDIP_ENA_SET(1),
			       lan966x, ANA_AGGR_CFG);
			break;
		case NETDEV_LAG_HASH_L23:
			lan_wr(ANA_AGGR_CFG_AC_DMAC_ENA_SET(1) |
			       ANA_AGGR_CFG_AC_SMAC_ENA_SET(1) |
			       ANA_AGGR_CFG_AC_IP6_TCPUDP_ENA_SET(1) |
			       ANA_AGGR_CFG_AC_IP4_TCPUDP_ENA_SET(1),
			       lan966x, ANA_AGGR_CFG);
			break;
		default:
			NL_SET_ERR_MSG_MOD(netdev_notifier_info_to_extack(&info->info),
					   "LAG device using unsupported hash type");
			return notifier_from_errno(-EINVAL);
		}
	}

	return notifier_from_errno(err);
}

static int
lan966x_netdevice_changeupper(struct net_device *dev,
			      struct netdev_notifier_changeupper_info *info)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct netlink_ext_ack *extack;
	int err = 0;

	extack = netdev_notifier_info_to_extack(&info->info);

	if (netif_is_bridge_master(info->upper_dev)) {
		if (info->linking)
			err = lan966x_port_bridge_join(port, info->upper_dev,
						       extack);
		else
			lan966x_port_bridge_leave(port, info->upper_dev);

		lan966x_vlan_port_apply(lan966x, port);
	}

	if (netif_is_lag_master(info->upper_dev)) {
		if (info->linking)
			err = lan966x_port_lag_join(port, info->upper_dev);
		else
			lan966x_port_lag_leave(port, info->upper_dev);
	}

	return notifier_from_errno(err);
}

static int
lan966x_netdevice_lag_changeupper(struct net_device *dev,
				  struct netdev_notifier_changeupper_info *info)
{
	struct net_device *lower;
	struct list_head *iter;

	/* TODO - Is this correct? */
	netdev_for_each_lower_dev(dev, lower, iter) {
		int err = lan966x_netdevice_changeupper(lower, info);
		if (err != NOTIFY_OK)
			return err;
	}

	return NOTIFY_DONE;
}

static int
lan966x_netdevice_changelowerstate(struct net_device *dev,
				   struct netdev_lag_lower_state_info *info)
{
	struct lan966x_port *port = netdev_priv(dev);
	bool is_active = info->link_up && info->tx_enabled;

	if (!port->bond)
		return NOTIFY_DONE;

	if (port->lag_tx_active == is_active)
		return NOTIFY_DONE;

	lan966x_port_lag_change(port, is_active);

	return NOTIFY_OK;
}

static int lan966x_netdevice_event(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	struct lan966x *lan966x = container_of(nb, struct lan966x, netdevice_nb);
#endif
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

#ifdef LAN966X_NETDEV_DBG
	lan966x_netdev_dbg(dev, nb, event, ptr);
#endif

	lan966x_qos_port_event(dev, event);

	switch (event) {
	case NETDEV_PRECHANGEUPPER: {
		struct netdev_notifier_changeupper_info *info = ptr;

		if (lan966x_netdevice_check(dev))
			return lan966x_netdevice_prechangeupper(dev, info);

		break;
	}
	case NETDEV_CHANGEUPPER: {
		struct netdev_notifier_changeupper_info *info = ptr;

		if (lan966x_netdevice_check(dev))
			return lan966x_netdevice_changeupper(dev, info);

		if (netif_is_lag_master(dev))
			return lan966x_netdevice_lag_changeupper(dev, info);

		break;
	}
	case NETDEV_CHANGELOWERSTATE: {
		struct netdev_notifier_changelowerstate_info *info = ptr;

		if (!lan966x_netdevice_check(dev))
			break;

		return lan966x_netdevice_changelowerstate(dev,
							  info->lower_state_info);
	}
#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	case NETDEV_CHANGEADDR:
		if (netif_is_bridge_master(dev) &&
		    (lan966x->hw_bridge_dev == dev))
			lan966x_mrp_update_mac(lan966x, dev->dev_addr);
		break;
#endif
	default:
		break;
	}

	return NOTIFY_DONE;
}

static void lan966x_attr_stp_state_set(struct lan966x_port *lan966x_port,
				       u8 state)
{
	struct lan966x *lan966x = lan966x_port->lan966x;
	u32 port_cfg;

	if (!(BIT(lan966x_port->chip_port) & lan966x->bridge_mask))
		return;

	port_cfg = lan_rd(lan966x, ANA_PORT_CFG(lan966x_port->chip_port));

	switch (state) {
	case BR_STATE_FORWARDING:
		lan966x->bridge_fwd_mask |= BIT(lan966x_port->chip_port);
		fallthrough;
	case BR_STATE_LEARNING:
		port_cfg |= ANA_PORT_CFG_LEARN_ENA_SET(1);
		break;
	default:
		port_cfg &= ~ANA_PORT_CFG_LEARN_ENA_SET(1);
		lan966x->bridge_fwd_mask &= ~BIT(lan966x_port->chip_port);
		break;
	}

	lan_wr(port_cfg, lan966x, ANA_PORT_CFG(lan966x_port->chip_port));

	lan966x_apply_bridge_fwd_mask(lan966x);
}

static void lan966x_port_attr_ageing_set(struct lan966x_port *lan966x_port,
					 unsigned long ageing_clock_t)
{
	unsigned long ageing_jiffies = clock_t_to_jiffies(ageing_clock_t);
	u32 ageing_time = jiffies_to_msecs(ageing_jiffies) / 1000;
	struct lan966x *lan966x = lan966x_port->lan966x;

	lan_wr(ANA_AUTOAGE_AGE_PERIOD_SET(ageing_time / 2),
	       lan966x, ANA_AUTOAGE);
}

static void lan966x_port_mc_flooding(struct lan966x_port *port, u32 pgid_ip)
{
	u32 val_ip, val;

	val = lan_rd(port->lan966x, ANA_PGID(PGID_MC));
	val = ANA_PGID_PGID_GET(val);

	val_ip = lan_rd(port->lan966x, ANA_PGID(pgid_ip));
	val_ip = ANA_PGID_PGID_GET(val_ip);

	/* If igmp is not enabled or is a router port then use mcast flood mask
	 * to decide to enable multicast flooding, otherwise don't flood
	 */
	if ((port->mrouter_port || !port->igmp_snooping_enabled)) {
		if (val & BIT(port->chip_port))
			val_ip |= BIT(port->chip_port);
		else
			val_ip &= ~BIT(port->chip_port);
	} else {
		val_ip &= ~BIT(port->chip_port);
	}

	lan_rmw(ANA_PGID_PGID_SET(val_ip),
		ANA_PGID_PGID,
		port->lan966x, ANA_PGID(pgid_ip));
}

static void lan966x_port_attr_mc_set(struct lan966x_port *port, bool mc)
{
	struct lan966x *lan966x = port->lan966x;
	u32 val;

	port->igmp_snooping_enabled = mc;
	val = lan_rd(lan966x, ANA_CPU_FWD_CFG(port->chip_port));

	if (mc) {
		val |= ANA_CPU_FWD_CFG_IGMP_REDIR_ENA_SET(1) |
		       ANA_CPU_FWD_CFG_MLD_REDIR_ENA_SET(1) |
		       ANA_CPU_FWD_CFG_IPMC_CTRL_COPY_ENA_SET(1);
		lan966x_multicast_restore(lan966x, port);
	} else {
		val &= ~(ANA_CPU_FWD_CFG_IGMP_REDIR_ENA_SET(1) |
		         ANA_CPU_FWD_CFG_MLD_REDIR_ENA_SET(1) |
		         ANA_CPU_FWD_CFG_IPMC_CTRL_COPY_ENA_SET(1));
		lan966x_multicast_clear(lan966x, port);
	}
	lan_wr(val, lan966x, ANA_CPU_FWD_CFG(port->chip_port));

	lan966x_port_mc_flooding(port, PGID_MCIPV4);
	lan966x_port_mc_flooding(port, PGID_MCIPV6);
}

static int lan966x_cpu_vlan_add(struct lan966x *lan966x, int vid)
{
	int ret;

	/* add br0 unicast */
	ret = lan966x_mact_learn(lan966x, PGID_CPU,
				 lan966x->hw_bridge_dev->dev_addr, vid,
				 ENTRYTYPE_LOCKED);
	if (ret)
		return ret;

	lan966x->vlan_mask[vid] |= BIT(CPU_PORT);
	lan966x_vlant_set_mask(lan966x, vid);
	return 0;
}

static int lan966x_cpu_vlan_del(struct lan966x *lan966x, int vid)
{
	const unsigned char mac[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff,
					      0xff };
	int ret;

	/* forget br0 the unicast */
	ret = lan966x_mact_forget(lan966x,
				  lan966x->hw_bridge_dev->dev_addr,
				  vid, ENTRYTYPE_LOCKED);
	if (ret)
		return ret;

	/* forget the broadcast */
	ret = lan966x_mact_forget(lan966x, mac, vid,
				  ENTRYTYPE_LOCKED);
	if (ret)
		return ret;

	lan966x->vlan_mask[vid] &= ~BIT(CPU_PORT);
	lan966x_vlant_set_mask(lan966x, vid);
	return 0;
}

static void lan966x_vlan_cpu_apply(struct lan966x *lan966x, bool enable)
{
	int i;

	for (i = 0; i < lan966x->num_phys_ports; ++i) {
		struct lan966x_port *port = lan966x->ports[i];

		if (!port)
			continue;

		if (!enable)
			lan966x_mact_learn(lan966x, PGID_CPU,
					    port->dev->dev_addr,
					    1, ENTRYTYPE_LOCKED);
		else
			lan966x_mact_forget(lan966x, port->dev->dev_addr,
					    1, ENTRYTYPE_LOCKED);
	}

	/* If bridge is not vlan enable, everything is classified as vlan 1
	 * and all the broadcast frames need to go to CPU, therefore add
	 * an entry in vlan 1
	 */
	if (!enable)
		lan966x_cpu_vlan_add(lan966x, 1);
	else
		lan966x_cpu_vlan_del(lan966x, 1);

}

static void lan966x_port_set_mcast_flood(struct lan966x_port *port,
					 bool enabled)
{
	u32 val = lan_rd(port->lan966x, ANA_PGID(PGID_MC));

	val = ANA_PGID_PGID_GET(val);
	if (enabled)
		val |= BIT(port->chip_port);
	else
		val &= ~BIT(port->chip_port);

	lan_rmw(ANA_PGID_PGID_SET(val),
		ANA_PGID_PGID,
		port->lan966x, ANA_PGID(PGID_MC));

	/* If igmp is not enabled then change also flooding mask of the ip
	 * frames
	 */
	if (!port->igmp_snooping_enabled) {
		lan966x_port_mc_flooding(port, PGID_MCIPV4);
		lan966x_port_mc_flooding(port, PGID_MCIPV6);
	}
}

static void lan966x_port_set_ucast_flood(struct lan966x_port *port,
					 bool enabled)
{
	u32 val = lan_rd(port->lan966x, ANA_PGID(PGID_UC));

	val = ANA_PGID_PGID_GET(val);
	if (enabled)
		val |= BIT(port->chip_port);
	else
		val &= ~BIT(port->chip_port);

	lan_rmw(ANA_PGID_PGID_SET(val),
		ANA_PGID_PGID,
		port->lan966x, ANA_PGID(PGID_UC));
}

static void lan966x_port_set_bcast_flood(struct lan966x_port *port,
					 bool enabled)
{
	u32 val = lan_rd(port->lan966x, ANA_PGID(PGID_BC));

	val = ANA_PGID_PGID_GET(val);
	if (enabled)
		val |= BIT(port->chip_port);
	else
		val &= ~BIT(port->chip_port);

	lan_rmw(ANA_PGID_PGID_SET(val),
		ANA_PGID_PGID,
		port->lan966x, ANA_PGID(PGID_BC));
}

static void lan966x_port_attr_bridge_flags(struct lan966x_port *port,
					   struct switchdev_brport_flags flags)
{
	if (flags.mask & BR_MCAST_FLOOD)
		lan966x_port_set_mcast_flood(port,
					     !!(flags.val & BR_MCAST_FLOOD));

	if (flags.mask & BR_FLOOD)
		lan966x_port_set_ucast_flood(port,
					     !!(flags.val & BR_FLOOD));

	if (flags.mask & BR_BCAST_FLOOD)
		lan966x_port_set_bcast_flood(port,
					     !!(flags.val & BR_BCAST_FLOOD));
}

static int lan966x_port_attr_pre_bridge_flags(struct lan966x_port *port,
					      struct switchdev_brport_flags flags)
{
	if (flags.mask & ~(BR_MCAST_FLOOD | BR_FLOOD | BR_BCAST_FLOOD))
		return -EINVAL;

	return 0;
}

static void lan966x_port_attr_mrouter(struct lan966x_port *port,
				      bool is_mc_router)
{
	struct lan966x *lan966x = port->lan966x;

	port->mrouter_port = is_mc_router;

	if (is_mc_router)
		lan966x_multicast_restore(lan966x, port);
	else
		lan966x_multicast_clear(lan966x, port);

	lan966x_port_mc_flooding(port, PGID_MCIPV4);
	lan966x_port_mc_flooding(port, PGID_MCIPV6);
}

static int lan966x_port_attr_set(struct net_device *dev, const void *ctx,
				 const struct switchdev_attr *attr,
				 struct netlink_ext_ack *extack)
{
	struct lan966x_port *lan966x_port = netdev_priv(dev);
	int err = 0;

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		lan966x_port_attr_bridge_flags(lan966x_port, attr->u.brport_flags);
		break;
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
		err = lan966x_port_attr_pre_bridge_flags(lan966x_port, attr->u.brport_flags);
		break;
	case SWITCHDEV_ATTR_ID_PORT_MROUTER:
		lan966x_port_attr_mrouter(lan966x_port, attr->u.mrouter);
		break;
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		lan966x_attr_stp_state_set(lan966x_port, attr->u.stp_state);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		lan966x_port_attr_ageing_set(lan966x_port, attr->u.ageing_time);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		lan966x_port->vlan_aware = attr->u.vlan_filtering;
		lan966x_vlan_port_apply(lan966x_port->lan966x, lan966x_port);
		/* When enable/disable vlan_filtering, it is need to add/remove
		 * all the broadcast addresses for the vlans in MAC table
		 */
		lan966x_vlan_cpu_apply(lan966x_port->lan966x,
				       lan966x_port->vlan_aware);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_MC_DISABLED:
		lan966x_port_attr_mc_set(lan966x_port, !attr->u.mc_disabled);
		break;
#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	case SWITCHDEV_ATTR_ID_MRP_PORT_ROLE:
		lan966x_handle_mrp_port_role(dev, attr->u.mrp_port_role);
		break;
#endif
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static int lan966x_port_attr_get(struct net_device *dev, const void *ctx,
				 const struct switchdev_attr *attr,
				 struct netlink_ext_ack *extack)
{
	switch (attr->id) {
#if IS_ENABLED(CONFIG_BRIDGE_CFM)
	case SWITCHDEV_ATTR_ID_CFM_CC_PEER_STATUS_GET:
		lan966x_handle_cfm_cc_peer_status_get(dev,
						      attr->u.cfm_cc_peer_status);
		break;
	case SWITCHDEV_ATTR_ID_CFM_MEP_STATUS_GET:
		lan966x_handle_cfm_mep_status_get(dev,
						  attr->u.cfm_mep_status);
		break;
#endif
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int lan966x_ext_port_obj_add_vlan(struct net_device *dev,
					 struct lan966x *lan966x,
					 const struct switchdev_obj_port_vlan *vlan)
{
	int ret;

	ret = lan966x_mact_learn(lan966x, PGID_CPU, dev->dev_addr, vlan->vid,
				 ENTRYTYPE_LOCKED);
	if (ret)
		return ret;

	/* Need to put also the CPU port in the Unicast flooding mask, because
	 * in case of flooding then also the external port needs to flood the
	 * frame
	 */
	lan_rmw(ANA_PGID_PGID_SET(BIT(CPU_PORT) | GENMASK(lan966x->num_phys_ports, 0)),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(PGID_UC));

	lan966x->ext_port++;

	return 0;
}

static int lan966x_port_obj_add_vlan(struct net_device *dev,
				     const struct switchdev_obj_port_vlan *vlan)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	u16 vid = vlan->vid;
	int ret;

	/* Copy the frames to CPU only if also the CPU is part
	 * of the vlan. Because otherwise the frame will be copy
	 * to CPU but it would be discard.
	 */
	if (lan966x->vlan_mask[vid] & BIT(CPU_PORT)) {
		ret = lan966x_mact_learn(lan966x, PGID_CPU,
					 dev->dev_addr, vid,
					 ENTRYTYPE_LOCKED);
		if (ret)
			return ret;
	}

	ret = lan966x_vlan_vid_add(dev, vid, pvid, untagged);
	if (ret)
		return ret;

	return 0;
}

static int lan966x_cpu_obj_add_vlan(struct lan966x *lan966x,
				    const unsigned char addr[ETH_ALEN],
				    const struct switchdev_obj_port_vlan *vlan)
{
	int i, ret;
	u16 vid  = vlan->vid;

	for (i = 0; i < lan966x->num_phys_ports; i++) {
		struct lan966x_port *port = lan966x->ports[i];

		if (!port)
			continue;

		if (!(lan966x->vlan_mask[vid] & BIT(port->chip_port)))
			continue;

		ret = lan966x_mact_learn(lan966x, PGID_CPU,
					 port->dev->dev_addr, vid,
					 ENTRYTYPE_LOCKED);
		if (ret)
			return ret;
	}

	ret = lan966x_cpu_vlan_add(lan966x, vid);
	if (ret)
		return ret;

	return 0;
}

static int lan966x_port_obj_del_vlan(struct net_device *dev,
				     const struct switchdev_obj_port_vlan *vlan)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	int ret;
	u16 vid = vlan->vid;

	if (lan966x->vlan_mask[vid] & BIT(CPU_PORT)) {
		ret = lan966x_mact_forget(lan966x, dev->dev_addr, vid,
					  ENTRYTYPE_LOCKED);
		if (ret)
			return ret;
	}

	ret = lan966x_vlan_vid_del(dev, vid);
	if (ret)
		return ret;

	return 0;
}

static int lan966x_ext_port_obj_del_vlan(struct net_device *dev,
					 struct lan966x *lan966x,
					 const struct switchdev_obj_port_vlan *vlan)
{
	int ret;
	u16 vid = vlan->vid;

	ret = lan966x_mact_forget(lan966x, dev->dev_addr, vid,
				  ENTRYTYPE_LOCKED);

	if (ret)
		return ret;

	lan966x->ext_port--;
	if (lan966x->ext_port == 0)
		lan_rmw(GENMASK(lan966x->num_phys_ports, 0),
			ANA_PGID_PGID,
			lan966x, ANA_PGID(PGID_UC));

	return 0;
}

static int lan966x_cpu_obj_del_vlan(struct lan966x *lan966x,
				    const struct switchdev_obj_port_vlan *vlan)
{
	int i, ret;
	u16 vid = vlan->vid;

	for (i = 0; i < lan966x->num_phys_ports; i++) {
		struct lan966x_port *port = lan966x->ports[i];

		if (!port)
			continue;

		if (!(lan966x->vlan_mask[vid] & BIT(port->chip_port)))
			continue;

		ret = lan966x_mact_forget(lan966x, port->dev->dev_addr,
					  vid, ENTRYTYPE_LOCKED);
		if (ret)
			return ret;
	}

	ret = lan966x_cpu_vlan_del(lan966x, vid);
	if (ret)
		return ret;

	return 0;
}

static int
lan966x_handle_port_vlan_add(struct net_device *dev, struct notifier_block *nb,
			     const struct switchdev_obj_port_vlan *vlan)
{
	struct lan966x *lan966x;

	/* When adding a port to a vlan, we get a callback for the port but
	 * also for the bridge. When get the callback for the bridge just bail
	 * out. Then when the bridge is added to the vlan, then we get a
	 * callback here but in this case the flags has set:
	 * BRIDGE_VLAN_INFO_BRENTRY. In this case it means that the CPU
	 * port is added to the vlan, so the broadcast frames and unicast frames
	 * with dmac of the bridge should be foward to CPU.
	 */
	if (netif_is_bridge_master(dev) &&
	    !(vlan->flags & BRIDGE_VLAN_INFO_BRENTRY))
		return 0;

	lan966x = container_of(nb, struct lan966x, switchdev_blocking_nb);

	/* In case the physical port gets called */
	if (!(vlan->flags & BRIDGE_VLAN_INFO_BRENTRY)) {
		if (!lan966x_netdevice_check(dev))
			return lan966x_ext_port_obj_add_vlan(dev, lan966x, vlan);

		return lan966x_port_obj_add_vlan(dev, vlan);
	}

	/* In case the bridge gets called */
	if (!lan966x->hw_bridge_dev)
		return 0;

	return lan966x_cpu_obj_add_vlan(lan966x, dev->dev_addr, vlan);
}

static struct lan966x_multicast *
lan966x_multicast_get(struct lan966x *lan966x, const unsigned char *addr,
		      u16 vid)
{
	struct lan966x_multicast *mc;

	list_for_each_entry(mc, &lan966x->multicast, list) {
		if (ether_addr_equal(mc->addr, addr) && mc->vid == vid)
			return mc;
	}

	return NULL;
}

static int lan966x_handle_ipv4_mdb_add(struct lan966x *lan966x,
				       struct lan966x_port *port,
				       struct lan966x_multicast *mc,
				       unsigned char addr[ETH_ALEN],
				       bool new, u16 vid)
{
	u32 mask = 0;
	u32 i;

	addr[0] = 0;

	/* According Q5 in RFC 4541, forward to all ports */
	if (addr[3] == 0x0 && addr[4] == 0x0)
		mask = GENMASK(lan966x->num_phys_ports, 0);

	if (!new) {
		addr[2] = mc->ports << 0;
		addr[1] = mc->ports << 8;
		lan966x_mact_forget(lan966x, addr, vid, ENTRYTYPE_MACV4);
	}

	for (i = 0; i < lan966x->num_phys_ports; ++i) {
		if (!lan966x->ports[i])
			continue;

		if (lan966x->ports[i]->mrouter_port)
			mask |= BIT(i);
	}

	mask |= BIT(port->chip_port);

	mc->ports |= mask;
	addr[2] = mc->ports << 0;
	addr[1] = mc->ports << 8;

	return lan966x_mact_learn(lan966x, 0, addr, vid, ENTRYTYPE_MACV4);
}

static int lan966x_handle_ipv6_mdb_add(struct lan966x *lan966x,
				       struct lan966x_port *port,
				       struct lan966x_multicast *mc,
				       unsigned char addr[ETH_ALEN],
				       bool new, u16 vid)
{
	if (!new) {
		addr[1] = mc->ports << 0;
		addr[0] = mc->ports << 8;
		lan966x_mact_forget(lan966x, addr, vid, ENTRYTYPE_MACV6);
	}

	mc->ports |= BIT(port->chip_port);
	addr[1] = mc->ports << 0;
	addr[0] = mc->ports << 8;

	return lan966x_mact_learn(lan966x, 0, addr, vid, ENTRYTYPE_MACV6);
}

static void lan966x_multicast_restore(struct lan966x *lan966x,
				      struct lan966x_port *port)
{
	struct lan966x_multicast *mc;
	unsigned char addr[ETH_ALEN];
	int i;

	list_for_each_entry(mc, &lan966x->multicast, list) {
		if (!(mc->ports & BIT(port->chip_port)))
			continue;

		for (i = 0; i < lan966x->num_phys_ports; ++i) {
			if (!lan966x->ports[i])
				continue;

			if (lan966x->ports[i]->mrouter_port)
				mc->ports |= BIT(i);
		}

		memcpy(addr, mc->addr, ETH_ALEN);
		if (addr[0] == 0x01)
			lan966x_handle_ipv4_mdb_add(lan966x, port, mc,
						    addr, false, mc->vid);
		else
			lan966x_handle_ipv6_mdb_add(lan966x, port, mc,
						    addr, false, mc->vid);
	}
}

static int
lan966x_handle_port_mdb_add(struct net_device *dev,
			    const struct switchdev_obj_port_mdb *mdb)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_multicast *mc;
	unsigned char addr[ETH_ALEN];
	u16 vid = mdb->vid;
	bool new = false;

	if (!vid)
		vid = port->pvid;

	mc = lan966x_multicast_get(lan966x, mdb->addr, vid);
	if (!mc) {
		mc = devm_kzalloc(lan966x->dev, sizeof(*mc), GFP_KERNEL);
		if (!mc)
			return -ENOMEM;

		memcpy(mc->addr, mdb->addr, ETH_ALEN);
		mc->vid = vid;

		list_add_tail(&mc->list, &lan966x->multicast);
		new = true;
	}

	memcpy(addr, mc->addr, ETH_ALEN);
	if (addr[0] == 0x01)
		return lan966x_handle_ipv4_mdb_add(lan966x, port, mc, addr, new,
						   vid);

	return lan966x_handle_ipv6_mdb_add(lan966x, port, mc, addr, new, vid);
}

static int
lan966x_handle_port_obj_add(struct net_device *dev, struct notifier_block *nb,
			    struct switchdev_notifier_port_obj_info *info)
{
	const struct switchdev_obj *obj = info->obj;
	int err;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = lan966x_handle_port_vlan_add(dev, nb,
						   SWITCHDEV_OBJ_PORT_VLAN(obj));
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
		err = lan966x_handle_port_mdb_add(dev,
						  SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	case SWITCHDEV_OBJ_ID_MRP:
		err = lan966x_handle_mrp_add(dev, nb, SWITCHDEV_OBJ_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_RING_TEST_MRP:
		err = lan966x_handle_ring_test_add(dev, nb, SWITCHDEV_OBJ_RING_TEST_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_RING_ROLE_MRP:
		err = lan966x_handle_ring_role_add(dev, nb, SWITCHDEV_OBJ_RING_ROLE_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_RING_STATE_MRP:
		err = lan966x_handle_ring_state_add(dev, nb, SWITCHDEV_OBJ_RING_STATE_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_IN_TEST_MRP:
		err = lan966x_handle_in_test_add(dev, nb, SWITCHDEV_OBJ_IN_TEST_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_IN_ROLE_MRP:
		err = lan966x_handle_in_role_add(dev, nb, SWITCHDEV_OBJ_IN_ROLE_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_IN_STATE_MRP:
		err = lan966x_handle_in_state_add(dev, nb, SWITCHDEV_OBJ_IN_STATE_MRP(obj));
		break;
#endif
#if IS_ENABLED(CONFIG_BRIDGE_CFM)
	case SWITCHDEV_OBJ_ID_MEP_CFM:
		err = lan966x_handle_cfm_mep_add(dev, nb, SWITCHDEV_OBJ_CFM_MEP(obj));
		break;
	case SWITCHDEV_OBJ_ID_CC_PEER_MEP_CFM:
		err = lan966x_handle_cfm_cc_peer_mep_add(dev, nb, SWITCHDEV_OBJ_CFM_CC_PEER_MEP(obj));
		break;
	case SWITCHDEV_OBJ_ID_MEP_CONFIG_CFM:
		err = lan966x_handle_cfm_mep_config_add(dev, nb, SWITCHDEV_OBJ_CFM_MEP_CONFIG_SET(obj));
		break;
	case SWITCHDEV_OBJ_ID_CC_CONFIG_CFM:
		err = lan966x_handle_cfm_cc_config_add(dev, nb, SWITCHDEV_OBJ_CFM_CC_CONFIG_SET(obj));
		break;
	case SWITCHDEV_OBJ_ID_CC_RDI_CFM:
		err = lan966x_handle_cfm_cc_rdi_add(dev, nb, SWITCHDEV_OBJ_CFM_CC_RDI_SET(obj));
		break;
	case SWITCHDEV_OBJ_ID_CC_CCM_TX_CFM:
		err = lan966x_handle_cfm_cc_ccm_tx_add(dev, nb, SWITCHDEV_OBJ_CFM_CC_CCM_TX(obj));
		break;
	case SWITCHDEV_OBJ_ID_MIP_CFM:
		err = lan966x_handle_cfm_mip_add(dev, nb, SWITCHDEV_OBJ_CFM_MIP(obj));
		break;
	case SWITCHDEV_OBJ_ID_MIP_CONFIG_CFM:
		err = lan966x_handle_cfm_mip_config_add(dev, nb, SWITCHDEV_OBJ_CFM_MIP_CONFIG_SET(obj));
		break;
#endif
	default:
		err = -EOPNOTSUPP;
		break;
	}

	info->handled = true;
	return err;
}

static int
lan966x_handle_port_vlan_del(struct net_device *dev, struct notifier_block *nb,
			     const struct switchdev_obj_port_vlan *vlan)
{
	struct lan966x *lan966x;

	lan966x = container_of(nb, struct lan966x, switchdev_blocking_nb);

	/* In case the physical port gets called */
	if (!netif_is_bridge_master(dev)) {
		if (!lan966x_netdevice_check(dev))
			return lan966x_ext_port_obj_del_vlan(dev, lan966x, vlan);

		return lan966x_port_obj_del_vlan(dev, vlan);
	}

	/* In case the bridge gets gets called */
	if (!lan966x->hw_bridge_dev)
		return 0;

	return lan966x_cpu_obj_del_vlan(lan966x, vlan);
}

static int lan966x_handle_ipv4_mdb_del(struct lan966x *lan966x,
				       struct lan966x_port *port,
				       struct lan966x_multicast *mc,
				       unsigned char addr[ETH_ALEN], u16 vid,
				       bool clear_entry)
{
	addr[2] = mc->ports << 0;
	addr[1] = mc->ports << 8;
	addr[0] = 0;
	lan966x_mact_forget(lan966x, addr, vid, ENTRYTYPE_MACV4);

	mc->ports &= ~BIT(port->chip_port);
	if (!mc->ports) {
		/* It is not needed to be clear from SW because the entries
		 * must be restored
		 */
		if (clear_entry) {
			list_del(&mc->list);
			devm_kfree(lan966x->dev, mc);
		} else {
			mc->ports |= BIT(port->chip_port);
		}
		return 0;
	}

	addr[2] = mc->ports << 0;
	addr[1] = mc->ports << 8;

	/* It means that is needed to be deleted only in HW, because the
	 * igmp is disabled. It is clear in HW only because when igmp is enabled
	 * the ports need to be added back to igmp groups
	 */
	if (!clear_entry)
		mc->ports |= BIT(port->chip_port);

	return lan966x_mact_learn(lan966x, 0, addr, vid, ENTRYTYPE_MACV4);
}

static int lan966x_handle_ipv6_mdb_del(struct lan966x *lan966x,
				       struct lan966x_port *port,
				       struct lan966x_multicast *mc,
				       unsigned char addr[ETH_ALEN], u16 vid,
				       bool clear_entry)
{
	addr[1] = mc->ports << 0;
	addr[0] = mc->ports << 8;
	lan966x_mact_forget(lan966x, addr, vid, ENTRYTYPE_MACV6);

	mc->ports &= ~BIT(port->chip_port);
	if (!mc->ports) {
		/* It is not needed to be clear from SW because the entries
		 * must be restored
		 */if (clear_entry) {
			list_del(&mc->list);
			devm_kfree(lan966x->dev, mc);
		} else {
			mc->ports |= BIT(port->chip_port);
		}
		return 0;
	}

	addr[1] = mc->ports << 0;
	addr[0] = mc->ports << 8;

	/* It means that is needed to be deleted only in HW, because the
	 * igmp is disabled. It is clear in HW only because when igmp is enabled
	 * the ports need to be added back to igmp groups
	 */
	if (!clear_entry)
		mc->ports |= BIT(port->chip_port);

	return lan966x_mact_learn(lan966x, 0, addr, vid, ENTRYTYPE_MACV6);
}

static int
lan966x_handle_port_mdb_del(struct net_device *dev,
			    const struct switchdev_obj_port_mdb *mdb)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_multicast *mc;
	unsigned char addr[ETH_ALEN];
	u16 vid = mdb->vid;

	if (!vid)
		vid = port->pvid;

	mc = lan966x_multicast_get(lan966x, mdb->addr, vid);
	if (!mc)
		return -ENOENT;

	memcpy(addr, mc->addr, ETH_ALEN);
	if (addr[0] == 0x01)
		return lan966x_handle_ipv4_mdb_del(lan966x, port, mc, addr,
						   vid, true);

	return lan966x_handle_ipv6_mdb_del(lan966x, port, mc, addr, vid, true);
}

static void lan966x_multicast_clear(struct lan966x *lan966x,
				    struct lan966x_port *port)
{
	struct lan966x_multicast *mc;
	unsigned char addr[ETH_ALEN];

	list_for_each_entry(mc, &lan966x->multicast, list) {
		if (!(mc->ports & BIT(port->chip_port)))
			continue;

		memcpy(addr, mc->addr, ETH_ALEN);
		if (addr[0] == 0x01)
			lan966x_handle_ipv4_mdb_del(lan966x, port, mc,
						    addr, mc->vid, false);
		else
			lan966x_handle_ipv6_mdb_del(lan966x, port, mc,
						    addr, mc->vid, false);
	}
}

static int
lan966x_handle_port_obj_del(struct net_device *dev, struct notifier_block *nb,
			    struct switchdev_notifier_port_obj_info *info)
{
	const struct switchdev_obj *obj = info->obj;
	int err;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = lan966x_handle_port_vlan_del(dev, nb,
						   SWITCHDEV_OBJ_PORT_VLAN(obj));
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
		err = lan966x_handle_port_mdb_del(dev,
						  SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	case SWITCHDEV_OBJ_ID_MRP:
		err = lan966x_handle_mrp_del(dev, nb,
					     SWITCHDEV_OBJ_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_RING_TEST_MRP:
		err = lan966x_handle_ring_test_del(dev, nb,
						   SWITCHDEV_OBJ_RING_TEST_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_RING_ROLE_MRP:
		err = lan966x_handle_ring_role_del(dev, nb,
						   SWITCHDEV_OBJ_RING_ROLE_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_IN_TEST_MRP:
		err = lan966x_handle_in_test_del(dev, nb,
						 SWITCHDEV_OBJ_IN_TEST_MRP(obj));
		break;
	case SWITCHDEV_OBJ_ID_IN_ROLE_MRP:
		err = lan966x_handle_in_role_del(dev, nb,
						 SWITCHDEV_OBJ_IN_ROLE_MRP(obj));
		break;
#endif
#if IS_ENABLED(CONFIG_BRIDGE_CFM)
	case SWITCHDEV_OBJ_ID_CC_PEER_MEP_CFM:
		err = lan966x_handle_cfm_cc_peer_mep_del(dev, nb,
							 SWITCHDEV_OBJ_CFM_CC_PEER_MEP(obj));
		break;
	case SWITCHDEV_OBJ_ID_MEP_CFM:
		err = lan966x_handle_cfm_mep_del(dev, nb,
						 SWITCHDEV_OBJ_CFM_MEP(obj));
		break;
	case SWITCHDEV_OBJ_ID_MIP_CFM:
		err = lan966x_handle_cfm_mip_del(dev, nb,
						 SWITCHDEV_OBJ_CFM_MIP(obj));
		break;
#endif
	default:
		err = -EOPNOTSUPP;
		break;
	}

	info->handled = true;
	return err;
}

static void lan966x_switchdev_bridge_fdb_event_work(struct work_struct *work)
{
	struct lan966x_switchdev_event_work *switchdev_work =
		container_of(work, struct lan966x_switchdev_event_work, work);
	struct net_device *dev = switchdev_work->dev;
	struct switchdev_notifier_fdb_info *fdb_info;
	struct lan966x_port *port;
	struct lan966x* lan966x;

	rtnl_lock();

	if (!lan966x_netdevice_check(dev))
		goto out;

	port = netdev_priv(dev);
	lan966x = port->lan966x;
	fdb_info = &switchdev_work->fdb_info;

	switch(switchdev_work->event) {
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
		if (!fdb_info->added_by_user || fdb_info->is_local)
			break;

		lan966x_add_mact_entry(lan966x, port, fdb_info->addr,
				       fdb_info->vid);
		break;
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		if (!fdb_info->added_by_user || fdb_info->is_local)
			break;

		lan966x_del_mact_entry(lan966x, fdb_info->addr, fdb_info->vid);
		break;
	}

out:
	rtnl_unlock();
	kfree(switchdev_work->fdb_info.addr);
	kfree(switchdev_work);
	dev_put(dev);
}

static void lan966x_schedule_work(struct work_struct *work)
{
	queue_work(lan966x_owq, work);
}

static int lan966x_switchdev_event(struct notifier_block *unused,
				   unsigned long event, void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	struct lan966x_switchdev_event_work *switchdev_work;
	struct switchdev_notifier_fdb_info *fdb_info;
	struct switchdev_notifier_info *info = ptr;
	int err;

	switch (event) {
	case SWITCHDEV_PORT_ATTR_SET:
		err = switchdev_handle_port_attr_set(dev, ptr,
						     lan966x_netdevice_check,
						     lan966x_port_attr_set);
		return notifier_from_errno(err);
	case SWITCHDEV_PORT_ATTR_GET:
		err = switchdev_handle_port_attr_get(dev, ptr,
						     lan966x_netdevice_check,
						     lan966x_port_attr_get);
		return notifier_from_errno(err);
	case SWITCHDEV_FDB_ADD_TO_DEVICE: /* fall through */
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		switchdev_work = kzalloc(sizeof(*switchdev_work), GFP_ATOMIC);
		if (!switchdev_work)
			return NOTIFY_BAD;

		/* If it's a LAG device then replace it with the lower device that has
		 * the lowest physical port number */
		if (netif_is_lag_master(dev)) {
			struct net_device *lower_dev;
			struct lan966x_port *port;
			struct lan966x* lan966x;
			unsigned long bond_mask;
			struct list_head *iter;

			netdev_for_each_lower_dev(dev, lower_dev, iter) {
				if (!lan966x_netdevice_check(lower_dev))
					continue;

				port = netdev_priv(lower_dev);
				lan966x = port->lan966x;
				bond_mask = lan966x_get_bond_mask(lan966x, dev, false);
				if (bond_mask) {
					dev = lan966x->ports[__ffs(bond_mask)]->dev;
					break;
				}
			}
		}

		switchdev_work->dev = dev;
		switchdev_work->event = event;

		fdb_info = container_of(info,
					struct switchdev_notifier_fdb_info,
					info);
		INIT_WORK(&switchdev_work->work,
			  lan966x_switchdev_bridge_fdb_event_work);
		memcpy(&switchdev_work->fdb_info, ptr,
		       sizeof(switchdev_work->fdb_info));
		switchdev_work->fdb_info.addr = kzalloc(ETH_ALEN, GFP_ATOMIC);
		if (!switchdev_work->fdb_info.addr)
			goto err_addr_alloc;

		ether_addr_copy((u8*)switchdev_work->fdb_info.addr,
				fdb_info->addr);
		dev_hold(dev);

		lan966x_schedule_work(&switchdev_work->work);
		break;
	}


	return NOTIFY_DONE;
err_addr_alloc:
	kfree(switchdev_work);
	return NOTIFY_BAD;
}

static int lan966x_switchdev_blocking_event(struct notifier_block *nb,
					    unsigned long event, void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	struct net_device *lower_dev;
	struct list_head *iter;
	int err = 0;

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD: {
		/* We do not call switchdev_handle_port_obj_add(), so we will
		 * need to handle LAG devices manually */
		if (netif_is_lag_master(dev)) {
			netdev_for_each_lower_dev(dev, lower_dev, iter) {
				err = lan966x_handle_port_obj_add(lower_dev, nb, ptr);
				if (err)
					return notifier_from_errno(err);
			}
		} else {
			err = lan966x_handle_port_obj_add(dev, nb, ptr);
		}
		return notifier_from_errno(err);
	}
	case SWITCHDEV_PORT_OBJ_DEL: {
		/* We do not call switchdev_handle_port_obj_del(), so we will
		 * need to handle LAG devices manually */
		if (netif_is_lag_master(dev)) {
			netdev_for_each_lower_dev(dev, lower_dev, iter) {
				err = lan966x_handle_port_obj_del(lower_dev, nb, ptr);
				if (err)
					return notifier_from_errno(err);
			}
		} else {
			err = lan966x_handle_port_obj_del(dev, nb, ptr);
		}
		return notifier_from_errno(err);
	}
	case SWITCHDEV_PORT_ATTR_SET:
		err = switchdev_handle_port_attr_set(dev, ptr,
						     lan966x_netdevice_check,
						     lan966x_port_attr_set);
		return notifier_from_errno(err);
	case SWITCHDEV_PORT_ATTR_GET:
		err = switchdev_handle_port_attr_get(dev, ptr,
						     lan966x_netdevice_check,
						     lan966x_port_attr_get);
		return notifier_from_errno(err);
	}

	return NOTIFY_DONE;
}

int lan966x_register_notifier_blocks(struct lan966x *lan966x)
{
	int err = 0;

	lan966x->netdevice_nb.notifier_call = lan966x_netdevice_event;
	err = register_netdevice_notifier(&lan966x->netdevice_nb);
	if (err)
		return err;

	if (!lan966x->hw_offload)
		return 0;

	lan966x->switchdev_nb.notifier_call = lan966x_switchdev_event;
	err = register_switchdev_notifier(&lan966x->switchdev_nb);
	if (err)
		goto err_switchdev_nb;

	lan966x->switchdev_blocking_nb.notifier_call =
		lan966x_switchdev_blocking_event;
	err = register_switchdev_blocking_notifier(
		&lan966x->switchdev_blocking_nb);
	if (err)
		goto err_switchdev_blocking_nb;

	lan966x_owq = alloc_ordered_workqueue("lan966x_order", 0);
	if (!lan966x_owq)
		goto err_switchdev_blocking_nb;

	return 0;

err_switchdev_blocking_nb:
	unregister_switchdev_notifier(&lan966x->switchdev_nb);
err_switchdev_nb:
	unregister_netdevice_notifier(&lan966x->netdevice_nb);

	return err;
}

void lan966x_unregister_notifier_blocks(struct lan966x *lan966x)
{
	destroy_workqueue(lan966x_owq);

	unregister_switchdev_blocking_notifier(&lan966x->switchdev_blocking_nb);
	unregister_switchdev_notifier(&lan966x->switchdev_nb);
	unregister_netdevice_notifier(&lan966x->netdevice_nb);
}
