// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2021 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/if_bridge.h>
#include <linux/if_hsr.h>
#include <net/bonding.h>
#include <net/switchdev.h>

#include "lan969x/lan969x.h"

#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include "sparx5_mrp.h"

static struct workqueue_struct *sparx5_owq;

struct sparx5_switchdev_event_work {
	struct work_struct work;
	struct switchdev_notifier_fdb_info fdb_info;
	struct net_device *dev;
	struct net_device *orig_dev;
	struct sparx5 *sparx5;
	unsigned long event;
};

static int sparx5_port_attr_pre_bridge_flags(struct sparx5_port *port,
					     struct switchdev_brport_flags flags)
{
	if (flags.mask & ~(BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD))
		return -EINVAL;

	return 0;
}

static void sparx5_port_update_mcast_ip_flood(struct sparx5_port *port,
					      bool flood_flag)
{
	bool should_flood = flood_flag || port->is_mrouter;
	struct sparx5 *sparx5 = port->sparx5;
	int pgid;

	for (pgid = sparx5_get_pgid_index(sparx5, PGID_IPV4_MC_DATA);
	     pgid <= sparx5_get_pgid_index(sparx5, PGID_IPV6_MC_CTRL); pgid++)
		sparx5_pgid_update_mask(port, pgid, should_flood);
}

static void sparx5_port_attr_bridge_flags(struct sparx5_port *port,
					  struct switchdev_brport_flags flags)
{
	struct sparx5 *sparx5 = port->sparx5;

	if (flags.mask & BR_MCAST_FLOOD) {
		sparx5_pgid_update_mask(port,
					sparx5_get_pgid_index(sparx5, PGID_MC_FLOOD),
					!!(flags.val & BR_MCAST_FLOOD));
		sparx5_port_update_mcast_ip_flood(port,
						  !!(flags.val & BR_MCAST_FLOOD));
	}

	if (flags.mask & BR_FLOOD)
		sparx5_pgid_update_mask(port,
					sparx5_get_pgid_index(sparx5, PGID_UC_FLOOD),
					!!(flags.val & BR_FLOOD));
	if (flags.mask & BR_BCAST_FLOOD)
		sparx5_pgid_update_mask(port, sparx5_get_pgid_index(sparx5, PGID_BCAST),
					!!(flags.val & BR_BCAST_FLOOD));
}

void sparx5_attr_stp_state_set(struct sparx5_port *port, u8 state)
{
	struct sparx5 *sparx5 = port->sparx5;

	if (!test_bit(port->portno, sparx5->bridge_mask) && !port->lag_master) {
		netdev_err(port->ndev,
			   "Controlling non-bridged port %d?\n", port->portno);
		return;
	}

	switch (state) {
	case BR_STATE_FORWARDING:
		set_bit(port->portno, sparx5->bridge_fwd_mask);
		fallthrough;
	case BR_STATE_LEARNING:
		set_bit(port->portno, sparx5->bridge_lrn_mask);
		break;

	default:
		/* All other states treated as blocking */
		clear_bit(port->portno, sparx5->bridge_fwd_mask);
		clear_bit(port->portno, sparx5->bridge_lrn_mask);
		break;
	}

	/* apply the bridge_fwd_mask to all the ports */
	sparx5_update_fwd(sparx5);
}

static void sparx5_port_attr_ageing_set(struct sparx5_port *port,
					unsigned long ageing_clock_t)
{
	unsigned long ageing_jiffies = clock_t_to_jiffies(ageing_clock_t);
	u32 ageing_time = jiffies_to_msecs(ageing_jiffies);

	sparx5_set_ageing(port->sparx5, ageing_time);
}

static void sparx5_port_attr_mrouter_set(struct sparx5_port *port,
					 struct net_device *orig_dev,
					 bool enable)
{
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_mdb_entry *e;
	bool flood_flag;

	if ((enable && port->is_mrouter) || (!enable && !port->is_mrouter))
		return;

	/* Add/del mrouter port on all active mdb entries in HW.
	 * Don't change entry port mask, since that represents
	 * ports that actually joined that group.
	 */
	mutex_lock(&sparx5->mdb_lock);
	list_for_each_entry(e, &sparx5->mdb_entries, list) {
		if (!test_bit(port->portno, e->port_mask) &&
		    ether_addr_is_ip_mcast(e->addr))
			sparx5_pgid_update_mask(port, e->pgid_idx, enable);
	}
	mutex_unlock(&sparx5->mdb_lock);

	/* Enable/disable flooding depending on if port is mrouter port
	 * or if mcast flood is enabled.
	 */
	port->is_mrouter = enable;
	flood_flag = br_port_flag_is_set(port->ndev, BR_MCAST_FLOOD);
	sparx5_port_update_mcast_ip_flood(port, flood_flag);
}

static int sparx5_port_attr_set(struct net_device *dev, const void *ctx,
				const struct switchdev_attr *attr,
				struct netlink_ext_ack *extack)
{
	struct sparx5_port *port = netdev_priv(dev);

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
		return sparx5_port_attr_pre_bridge_flags(port,
							 attr->u.brport_flags);
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		sparx5_port_attr_bridge_flags(port, attr->u.brport_flags);
		break;
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		sparx5_attr_stp_state_set(port, attr->u.stp_state);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		sparx5_port_attr_ageing_set(port, attr->u.ageing_time);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		/* Used PVID 1 when default_pvid is 0, to avoid
		 * collision with non-bridged ports.
		 */
		if (port->pvid == 0)
			port->pvid = 1;
		port->vlan_aware = attr->u.vlan_filtering;
		sparx5_vlan_port_apply(port->sparx5, port);
		break;
	case SWITCHDEV_ATTR_ID_PORT_MROUTER:
		sparx5_port_attr_mrouter_set(port,
					     attr->orig_dev,
					     attr->u.mrouter);
		break;
	case SWITCHDEV_ATTR_ID_MRP_PORT_ROLE:
		sparx5_handle_mrp_port_role(port, attr->u.mrp_port_role);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int sparx5_port_bridge_join(struct sparx5_port *port,
				   struct net_device *brport_dev,
				   struct net_device *bridge,
				   struct netlink_ext_ack *extack)
{
	struct sparx5 *sparx5 = port->sparx5;
	struct net_device *ndev = port->ndev;
	int err;

	if (bitmap_empty(sparx5->bridge_mask, SPX5_PORTS))
		/* First bridged port */
		sparx5->hw_bridge_dev = bridge;
	else
		if (sparx5->hw_bridge_dev != bridge)
			/* This is adding the port to a second bridge, this is
			 * unsupported
			 */
			return -ENODEV;

	set_bit(port->portno, sparx5->bridge_mask);

	err = switchdev_bridge_port_offload(brport_dev, ndev, NULL, NULL, NULL,
					    false, extack);
	if (err)
		goto err_switchdev_offload;

	/* Remove standalone port entry */
	sparx5_mact_forget(sparx5, ndev->dev_addr, 0);

	/* Port enters in bridge mode therefore don't need to copy to CPU
	 * frames for multicast in case the bridge is not requesting them
	 */
	__dev_mc_unsync(ndev, sparx5_mc_unsync);

	return 0;

err_switchdev_offload:
	clear_bit(port->portno, sparx5->bridge_mask);
	return err;
}

static void sparx5_port_bridge_leave(struct sparx5_port *port,
				     struct net_device *bridge)
{
	struct sparx5 *sparx5 = port->sparx5;

	clear_bit(port->portno, sparx5->bridge_mask);
	if (bitmap_empty(sparx5->bridge_mask, SPX5_PORTS))
		sparx5->hw_bridge_dev = NULL;

	/* Clear bridge vlan settings before updating the port settings */
	port->vlan_aware = 0;
	port->pvid = NULL_VID;
	port->vid = NULL_VID;

	/* Forward frames to CPU */
	sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_CPU),
			  port->ndev->dev_addr, 0);

	/* Port enters in host more therefore restore mc list */
	__dev_mc_sync(port->ndev, sparx5_mc_sync, sparx5_mc_unsync);
}

int
sparx5_port_prechangeupper(struct net_device *dev,
			   struct net_device *brport_dev,
			   struct netdev_notifier_changeupper_info *info)
{
	struct sparx5_port *port = netdev_priv(dev);
	int err = NOTIFY_DONE;

	if (netif_is_bridge_master(info->upper_dev)) {
		if (info->linking)
			return 0;
		else
			switchdev_bridge_port_unoffload(dev, port, NULL, NULL);
	}

	if (netif_is_lag_master(info->upper_dev)) {
		err = sparx5_lag_aggr_code_set(dev, info);
		if (err)
			return err;

		if (info->linking)
			return 0;

		switchdev_bridge_port_unoffload(brport_dev, port, NULL, NULL);
	}

	return err;
}

int sparx5_port_changeupper(struct net_device *dev,
				   struct net_device *brport_dev,
				   struct netdev_notifier_changeupper_info *info)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct netlink_ext_ack *extack;
	int err = 0;

	extack = netdev_notifier_info_to_extack(&info->info);

	if (netif_is_bridge_master(info->upper_dev)) {
		if (info->linking)
			err = sparx5_port_bridge_join(port,
						      brport_dev,
						      info->upper_dev,
						      extack);
		else
			sparx5_port_bridge_leave(port, info->upper_dev);

		sparx5_vlan_port_apply(port->sparx5, port);
	}

	if (is_hsr_master(info->upper_dev)) {
		if (!sparx5_has_feature(port->sparx5, SPX5_FEATURE_REDBOX))
			return -EOPNOTSUPP;

		if (info->linking)
			err = lan969x_hsr_join(info->upper_dev, dev);
		else
			lan969x_hsr_leave(info->upper_dev, dev);
	}

	if (netif_is_lag_master(info->upper_dev)) {
		/* Upper device is a LAG master, add this device to the LAG. */
		if (info->linking)
			err = sparx5_lag_join(port,
					      info->upper_dev,
					      info->upper_dev,
					      extack);
		else
			sparx5_lag_leave(port, info->upper_dev);
	}

	return err;
}

static int
sparx5_port_changelower(struct net_device *dev,
			struct netdev_notifier_changelowerstate_info *info)
{
	struct netdev_lag_lower_state_info *lag = info->lower_state_info;
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *sparx5 = port->sparx5;
	bool is_active;

	if (netif_is_lag_port(dev)) {
		if (!port->lag_master)
			return NOTIFY_DONE;

		is_active = lag->link_up && lag->tx_enabled;

		if (port->lag_tx_active == is_active)
			return NOTIFY_DONE;

		port->lag_tx_active = is_active;

		sparx5_update_dst_fwd(sparx5);
		sparx5_lag_aggr_masks_set(port, false);
	}

	return NOTIFY_OK;
}

static int sparx5_port_add_addr(struct net_device *dev, bool up)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *sparx5 = port->sparx5;
	u16 vid = port->pvid;

	if (up)
		sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_CPU),
				  port->ndev->dev_addr, vid);
	else
		sparx5_mact_forget(sparx5, port->ndev->dev_addr, vid);

	return 0;
}

static int sparx5_netdevice_port_event(struct net_device *dev,
				       struct notifier_block *nb,
				       unsigned long event, void *ptr)
{
	int err = 0;

	sparx5_qos_port_event(dev, event);

	switch (event) {
	case NETDEV_PRECHANGEUPPER:
		/* When a port is directly attached to a bridge, the brport_dev
		 * and dev are identical.
		 */

		sparx5_port_prechangeupper(dev, dev, ptr);
		break;
	case NETDEV_CHANGEUPPER:
		/* When a port is directly attached to a bridge, the brport_dev
		 * and dev are identical.
		 */

		err = sparx5_port_changeupper(dev, dev, ptr);
		break;
	case NETDEV_CHANGELOWERSTATE:
		err = sparx5_port_changelower(dev, ptr);
		break;
	case NETDEV_PRE_UP:
		err = sparx5_port_add_addr(dev, true);
		break;
	case NETDEV_DOWN:
		err = sparx5_port_add_addr(dev, false);
		break;
	}

	return err;
}

static int
sparx5_netdevice_lag_event(struct net_device *dev, struct notifier_block *nb,
			   unsigned long event,
			   struct netdev_notifier_changeupper_info *info)
{
	/* Event for lag master. Walk the lower devices. */
	struct sparx5_port *port;
	struct net_device *lower;
	struct list_head *iter;
	int err = 0;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		port = netdev_priv(lower);
		if (port->lag_master != dev)
			continue;

		switch (event) {
		case NETDEV_PRECHANGEUPPER:
			err = sparx5_port_prechangeupper(lower, dev, info);
			break;
		case NETDEV_CHANGEUPPER:
			err = sparx5_port_changeupper(lower, dev, info);
			break;
		default:
			break;
		}

		if (err)
			return err;
	}

	return NOTIFY_DONE;
}

static int
sparx5_netdevice_foreign_event(struct net_device *dev,
			       struct notifier_block *nb, unsigned long event,
			       struct netdev_notifier_changeupper_info *info)
{
	switch (event) {
	case NETDEV_PRECHANGEUPPER:
	case NETDEV_CHANGEUPPER:
		/* Do not allow bridging or bonding of foreign devices. */
		pr_info("Bridging or bonding of foreign devices is not supported");
		return -EOPNOTSUPP;
	default:
		return NOTIFY_DONE;
	}
}

static int sparx5_netdevice_event(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	int ret = 0;

	if (netif_is_lag_master(dev)) {
		/* This event is for a lag master device e.g bond0 */

		ret = sparx5_netdevice_lag_event(dev, nb, event, ptr);
	} else if (netif_is_bridge_master(dev)) {
		/* This event is for a bridge master device e.g br0 */

		ret = 0;
	} else if (sparx5_netdevice_check(dev)) {
		/* This event is for a sparx5 port device e.g eth0 */

		ret = sparx5_netdevice_port_event(dev, nb, event, ptr);
	} else {
		/* Anything else */
		ret = sparx5_netdevice_foreign_event(dev, nb, event, ptr);
	}

	return notifier_from_errno(ret);
}

static void sparx5_switchdev_bridge_fdb_event_work(struct work_struct *work)
{
	struct sparx5_switchdev_event_work *switchdev_work =
		container_of(work, struct sparx5_switchdev_event_work, work);
	struct net_device *orig_dev = switchdev_work->orig_dev;
	struct net_device *dev = switchdev_work->dev;
	struct switchdev_notifier_fdb_info *fdb_info;
	struct sparx5_port *port;
	struct sparx5 *sparx5;
	bool host_addr;
	u16 vid;

	sparx5 = switchdev_work->sparx5;

	rtnl_lock();

	if (sparx5_netdevice_check(orig_dev)) {
		/* The notification was for a netdevice */

		port = netdev_priv(orig_dev);
		host_addr = false;
	} else if (netif_is_bridge_master(orig_dev)) {
		/* The notification was for a bridge master - add host addr */

		port = netdev_priv(orig_dev);
		host_addr = true;
	} else if (netif_is_lag_master(orig_dev)) {
		/* The notification was for a LAG master - add FDB for first
		 * port in LAG. LAG ports can join and leave the LAG while FDB
		 * events are being handled.
		 */

		if (!sparx5_lag_is_first(orig_dev, dev))
			goto out;

		port = netdev_priv(dev);
		host_addr = false;
	} else {
		/* Foreign device - do nothing */

		goto out;
	}

	fdb_info = &switchdev_work->fdb_info;

	/* Used PVID 1 when default_pvid is 0, to avoid
	 * collision with non-bridged ports.
	 */
	if (fdb_info->vid == 0)
		vid = 1;
	else
		vid = fdb_info->vid;

	switch (switchdev_work->event) {
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
		if (host_addr)
			sparx5_add_mact_entry(sparx5, dev,
					      sparx5_get_pgid_index(sparx5, PGID_CPU),
					      fdb_info->addr, vid);
		else
			sparx5_add_mact_entry(sparx5, port->ndev, port->portno,
					      fdb_info->addr, vid);
		break;
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		sparx5_del_mact_entry(sparx5, fdb_info->addr, vid);
		break;
	}

out:
	rtnl_unlock();
	kfree(switchdev_work->fdb_info.addr);
	kfree(switchdev_work);
	dev_put(dev);
}

static void sparx5_schedule_work(struct work_struct *work)
{
	queue_work(sparx5_owq, work);
}

static int
sparx5_switchdev_handle_fdb(struct net_device *dev,
			    struct net_device *orig_dev,
			    unsigned long event, const void *ctx,
			    const struct switchdev_notifier_fdb_info *fdb_info)
{
	struct sparx5_switchdev_event_work *switchdev_work;
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *sparx5 = port->sparx5;

	switch (event) {
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
		fallthrough;
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		if (sparx5_netdevice_check(orig_dev) &&
		    !fdb_info->added_by_user)
			break;

		switchdev_work = kzalloc(sizeof(*switchdev_work), GFP_ATOMIC);
		if (!switchdev_work)
			return NOTIFY_BAD;

		switchdev_work->dev = dev;
		switchdev_work->orig_dev = orig_dev;
		switchdev_work->event = event;
		switchdev_work->sparx5 = sparx5;

		INIT_WORK(&switchdev_work->work,
			  sparx5_switchdev_bridge_fdb_event_work);
		memcpy(&switchdev_work->fdb_info,
		       fdb_info,
		       sizeof(switchdev_work->fdb_info));
		switchdev_work->fdb_info.addr = kzalloc(ETH_ALEN, GFP_ATOMIC);
		if (!switchdev_work->fdb_info.addr)
			goto err_addr_alloc;

		ether_addr_copy((u8 *)switchdev_work->fdb_info.addr,
				fdb_info->addr);
		dev_hold(dev);

		sparx5_schedule_work(&switchdev_work->work);
		break;
	}

	return NOTIFY_DONE;
err_addr_alloc:
	kfree(switchdev_work);
	return NOTIFY_BAD;
}

static bool sparx5_foreign_device_check(const struct net_device *dev,
					const struct net_device *foreign_dev)
{
	return false;
}

static int sparx5_switchdev_event(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	int err;

	switch (event) {
	case SWITCHDEV_PORT_ATTR_SET:
		err = switchdev_handle_port_attr_set(dev,
						     ptr,
						     sparx5_netdevice_check,
						     sparx5_port_attr_set);
		return notifier_from_errno(err);
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
			fallthrough;
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		err = switchdev_handle_fdb_event_to_device(dev,
							   event,
							   ptr,
							   sparx5_netdevice_check,
							   sparx5_foreign_device_check,
							   sparx5_switchdev_handle_fdb);
		return notifier_from_errno(err);
	}

	return NOTIFY_DONE;
}

static int sparx5_handle_port_vlan_add(struct net_device *dev,
				       struct notifier_block *nb,
				       const struct switchdev_obj_port_vlan *v)
{
	struct sparx5_port *port = netdev_priv(dev);

	if (netif_is_bridge_master(dev)) {
		struct sparx5 *sparx5 =
			container_of(nb, struct sparx5,
				     switchdev_blocking_nb);

		/* Flood broadcast to CPU */
		sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_BCAST),
				  dev->broadcast,
				  v->vid);

		return 0;
	}

	if (netif_is_lag_master(dev)) {
		/* Walk lower devices and make each port member of the bridge
		 * VLAN.
		 */
		struct net_device *lower;
		struct list_head *iter;
		int err;

		 netdev_for_each_lower_dev(dev, lower, iter) {
			if (!sparx5_netdevice_check(lower))
				continue;

			port = netdev_priv(lower);
			if (port->lag_master != dev)
				continue;

			err = sparx5_vlan_vid_add(port,
						  v->vid,
						  v->flags & BRIDGE_VLAN_INFO_PVID,
						  v->flags & BRIDGE_VLAN_INFO_UNTAGGED);
			if (err)
				return err;
		}

		return 0;
	}

	if (!sparx5_netdevice_check(dev))
		return -EOPNOTSUPP;

	return sparx5_vlan_vid_add(port, v->vid,
				  v->flags & BRIDGE_VLAN_INFO_PVID,
				  v->flags & BRIDGE_VLAN_INFO_UNTAGGED);
}

static int sparx5_handle_port_obj_add(struct net_device *dev,
				      struct notifier_block *nb,
				      struct switchdev_notifier_port_obj_info *info)
{
	const struct switchdev_obj *obj = info->obj;
	int err;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = sparx5_handle_port_vlan_add(dev, nb,
						  SWITCHDEV_OBJ_PORT_VLAN(obj));
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		err = sparx5_handle_mdb_add(dev, nb,
					    SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
	case SWITCHDEV_OBJ_ID_MRP:
		err = sparx5_handle_mrp_add(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_RING_TEST_MRP:
		err = sparx5_handle_mrp_ring_test_add(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_RING_ROLE_MRP:
		err = sparx5_handle_mrp_ring_role_add(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_RING_STATE_MRP:
		err = sparx5_handle_mrp_ring_state_add(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_IN_TEST_MRP:
		err = sparx5_handle_mrp_in_test_add(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_IN_ROLE_MRP:
		err = sparx5_handle_mrp_in_role_add(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_IN_STATE_MRP:
		err = sparx5_handle_mrp_in_state_add(dev, obj);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	info->handled = true;
	return err;
}

static int sparx5_handle_port_vlan_del(struct net_device *dev,
				       struct notifier_block *nb,
				       u16 vid)
{
	struct sparx5_port *port = netdev_priv(dev);
	int ret;

	/* Master bridge? */
	if (netif_is_bridge_master(dev)) {
		struct sparx5 *sparx5 =
			container_of(nb, struct sparx5,
				     switchdev_blocking_nb);

		sparx5_mact_forget(sparx5, dev->broadcast, vid);
		return 0;
	}

	if (netif_is_lag_master(dev)) {
		/* Walk lower devices and make each port member of the bridge
		 * VLAN.
		 */
		struct net_device *lower;
		struct list_head *iter;

		netdev_for_each_lower_dev(dev, lower, iter) {
			if (!sparx5_netdevice_check(lower))
				continue;

			port = netdev_priv(lower);
			if (port->lag_master != dev)
				continue;

			ret = sparx5_vlan_vid_del(port, vid);
			if (ret)
				return ret;
		}

		return 0;
	}

	if (!sparx5_netdevice_check(dev))
		return -EOPNOTSUPP;

	ret = sparx5_vlan_vid_del(port, vid);
	if (ret)
		return ret;

	return 0;
}

static int sparx5_handle_port_obj_del(struct net_device *dev,
				      struct notifier_block *nb,
				      struct switchdev_notifier_port_obj_info *info)
{
	const struct switchdev_obj *obj = info->obj;
	int err;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = sparx5_handle_port_vlan_del(dev, nb,
						  SWITCHDEV_OBJ_PORT_VLAN(obj)->vid);
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		err = sparx5_handle_mdb_del(dev, nb,
					    SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
	case SWITCHDEV_OBJ_ID_MRP:
		err = sparx5_handle_mrp_del(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_RING_TEST_MRP:
		err = sparx5_handle_mrp_ring_test_del(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_RING_ROLE_MRP:
		err = sparx5_handle_mrp_ring_role_del(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_IN_TEST_MRP:
		err = sparx5_handle_mrp_in_test_del(dev, obj);
		break;
	case SWITCHDEV_OBJ_ID_IN_ROLE_MRP:
		err = sparx5_handle_mrp_in_role_del(dev, obj);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	info->handled = true;
	return err;
}

static int sparx5_switchdev_blocking_event(struct notifier_block *nb,
					   unsigned long event,
					   void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	int err;

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD:
		err = sparx5_handle_port_obj_add(dev, nb, ptr);
		return notifier_from_errno(err);
	case SWITCHDEV_PORT_OBJ_DEL:
		err = sparx5_handle_port_obj_del(dev, nb, ptr);
		return notifier_from_errno(err);
	case SWITCHDEV_PORT_ATTR_SET:
		err = switchdev_handle_port_attr_set(dev, ptr,
						     sparx5_netdevice_check,
						     sparx5_port_attr_set);
		return notifier_from_errno(err);
	}

	return NOTIFY_DONE;
}

int sparx5_register_notifier_blocks(struct sparx5 *s5)
{
	int err;

	s5->netdevice_nb.notifier_call = sparx5_netdevice_event;
	err = register_netdevice_notifier(&s5->netdevice_nb);
	if (err)
		return err;

	s5->switchdev_nb.notifier_call = sparx5_switchdev_event;
	err = register_switchdev_notifier(&s5->switchdev_nb);
	if (err)
		goto err_switchdev_nb;

	s5->switchdev_blocking_nb.notifier_call = sparx5_switchdev_blocking_event;
	err = register_switchdev_blocking_notifier(&s5->switchdev_blocking_nb);
	if (err)
		goto err_switchdev_blocking_nb;

	sparx5_owq = alloc_ordered_workqueue("sparx5_order", 0);
	if (!sparx5_owq) {
		err = -ENOMEM;
		goto err_switchdev_blocking_nb;
	}

	return 0;

err_switchdev_blocking_nb:
	unregister_switchdev_notifier(&s5->switchdev_nb);
err_switchdev_nb:
	unregister_netdevice_notifier(&s5->netdevice_nb);

	return err;
}

void sparx5_unregister_notifier_blocks(struct sparx5 *s5)
{
	destroy_workqueue(sparx5_owq);

	unregister_switchdev_blocking_notifier(&s5->switchdev_blocking_nb);
	unregister_switchdev_notifier(&s5->switchdev_nb);
	unregister_netdevice_notifier(&s5->netdevice_nb);
}
