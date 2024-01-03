// SPDX-License-Identifier: GPL-2.0-or-later

#include <net/switchdev.h>

#include "br_private_cfm.h"

int br_cfm_switchdev_mep_create(struct net_bridge *br,
				const u32 instance,
				struct br_cfm_mep_create *const create,
				struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_mep cfm_obj;
	struct net_bridge_port *port;

	list_for_each_entry(port, &br->port_list, list)
		if (port->dev->ifindex == create->ifindex) {
			break;
		}

	cfm_obj.obj.orig_dev = br->dev;
	cfm_obj.obj.id = SWITCHDEV_OBJ_ID_MEP_CFM;
	cfm_obj.obj.flags = 0;
	cfm_obj.instance = instance;
	cfm_obj.domain = create->domain;
	cfm_obj.direction = create->direction;
	cfm_obj.port = rtnl_dereference(port)->dev;

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}

int br_cfm_switchdev_mep_delete(struct net_bridge *br,
				struct br_cfm_mep *mep,
				struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_mep cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_MEP_CFM,
		.obj.flags = 0,
		.instance = mep->instance,
		.domain = 0,
		.direction = 0,
		.port = NULL,
	};

	return switchdev_port_obj_del(br->dev, &cfm_obj.obj);
}

int br_cfm_switchdev_mep_config_set(struct net_bridge *br,
				    struct br_cfm_mep *mep,
				    const struct br_cfm_mep_config *const config,
				    struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_mep_config_set cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_MEP_CONFIG_CFM,
		.obj.flags = 0,
		.instance = mep->instance,
		.mdlevel = config->mdlevel,
		.mepid = config->mepid,
	};
	memcpy(&cfm_obj.unicast_mac, &config->unicast_mac, sizeof(cfm_obj.unicast_mac));

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}

int br_cfm_switchdev_cc_config_set(struct net_bridge *br,
				   struct br_cfm_mep *mep,
				   const struct br_cfm_cc_config *const config,
				   struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_cc_config_set cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_CC_CONFIG_CFM,
		.obj.flags = 0,
		.instance = mep->instance,
		.interval = config->exp_interval,
		.enable = config->enable,
	};
	memcpy(&cfm_obj.maid, &config->exp_maid, sizeof(cfm_obj.maid));

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}

int br_cfm_switchdev_cc_peer_mep_add(struct net_bridge *br,
				     struct br_cfm_mep *mep,
				     u32 peer_mep_id,
				     struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_cc_peer_mep cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_CC_PEER_MEP_CFM,
		.obj.flags = 0,
		.instance = mep->instance,
		.peer_mep_id = peer_mep_id,
	};

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}

int br_cfm_switchdev_cc_peer_mep_remove(struct net_bridge *br,
					struct br_cfm_mep *mep,
					u32 peer_mep_id,
					struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_cc_peer_mep cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_CC_PEER_MEP_CFM,
		.obj.flags = 0,
		.instance = mep->instance,
		.peer_mep_id = peer_mep_id,
	};

	return switchdev_port_obj_del(br->dev, &cfm_obj.obj);
}

int br_cfm_switchdev_cc_rdi_set(struct net_bridge *br,
				struct br_cfm_mep *mep,
				const bool rdi,
				struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_cc_rdi_set cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_CC_RDI_CFM,
		.obj.flags = 0,
		.instance = mep->instance,
		.rdi = rdi,
	};

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}

int br_cfm_switchdev_cc_ccm_tx(struct net_bridge *br,
			       struct br_cfm_mep *mep,
			       struct sk_buff *skb,
			       enum br_cfm_ccm_interval interval,
			       bool seq_no_update,
			       struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_cc_ccm_tx cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_CC_CCM_TX_CFM,
		.obj.flags = 0,
		.instance = mep->instance,
		.skb = skb,
		.interval = interval,
		.seq_no_update = seq_no_update,
	};

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}

int br_cfm_switchdev_mep_status_get(struct net_bridge *br,
				    struct br_cfm_mep *mep,
				    struct br_cfm_mep_status *const status)
{
	struct switchdev_cfm_mep_status mep_status;
	struct switchdev_attr attr;
	u32 ret;

	memset(&mep_status, 0, sizeof(mep_status));
	mep_status.instance = mep->instance;

	attr.orig_dev = mep->b_port->dev,
	attr.id = SWITCHDEV_ATTR_ID_CFM_MEP_STATUS_GET,
	attr.u.cfm_mep_status = &mep_status,

	ret = switchdev_port_attr_get(mep->b_port->dev, &attr, NULL);

	status->opcode_unexp_seen = mep_status.opcode_unexp_seen;
	status->rx_level_low_seen = mep_status.rx_level_low_seen;

	/* Not supported */
	status->version_unexp_seen = false;

	return ret;
}

int br_cfm_switchdev_cc_peer_status_get(struct net_bridge *br,
				        struct br_cfm_mep *mep,
					u32 peer_mep_id,
				        struct br_cfm_cc_peer_status *const status)
{
	struct switchdev_cfm_cc_peer_status cc_peer_status;
	struct switchdev_attr attr;
	u32 ret;

	memset(&cc_peer_status, 0, sizeof(cc_peer_status));
	cc_peer_status.instance = mep->instance;
	cc_peer_status.mepid = peer_mep_id;

	attr.orig_dev = mep->b_port->dev,
	attr.id = SWITCHDEV_ATTR_ID_CFM_CC_PEER_STATUS_GET,
	attr.u.cfm_cc_peer_status = &cc_peer_status,

	ret = switchdev_port_attr_get(mep->b_port->dev, &attr, NULL);

	status->ccm_defect = cc_peer_status.ccm_defect;
	status->rdi = cc_peer_status.rdi;
	status->seen = cc_peer_status.seen;
	status->seq_unexp_seen = cc_peer_status.seq_unexp_seen;

	/* Not supported */
	status->port_tlv_value = 0;
	status->if_tlv_value = 0;
	status->tlv_seen = 0;

	return ret;
}

int br_cfm_switchdev_mip_create(struct net_bridge *br,
				const u32 instance,
				struct br_cfm_mip_create *const create,
				struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_mip cfm_obj;
	struct net_bridge_port *port;
	struct net_device *dev;
	u16 vlan_id;

	list_for_each_entry(port, &br->port_list, list)
		if (port->dev->ifindex == create->port_ifindex)
			break;
	if (!port)
		return -EOPNOTSUPP;
	if (port->dev->ifindex != create->port_ifindex)
		return -EOPNOTSUPP;

	dev = dev_get_by_index(&init_net, create->vlan_ifindex);
	if (!dev)
		return -EOPNOTSUPP;
	if (!is_vlan_dev(dev))
		return -EOPNOTSUPP;
	vlan_id = vlan_dev_vlan_id(dev);
	dev_put(dev);

	cfm_obj.obj.orig_dev = br->dev;
	cfm_obj.obj.id = SWITCHDEV_OBJ_ID_MIP_CFM;
	cfm_obj.obj.flags = 0;
	cfm_obj.instance = instance;
	cfm_obj.direction = create->direction;
	cfm_obj.vid = vlan_id;
	cfm_obj.port = rtnl_dereference(port)->dev;

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}

int br_cfm_switchdev_mip_delete(struct net_bridge *br,
				struct br_cfm_mip *mip,
				struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_mip cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_MIP_CFM,
		.obj.flags = 0,
		.instance = mip->instance,
		.direction = 0,
		.vid = 0,
		.port = NULL,
	};

	return switchdev_port_obj_del(br->dev, &cfm_obj.obj);
}

int br_cfm_switchdev_mip_config_set(struct net_bridge *br,
				    struct br_cfm_mip *mip,
				    const struct br_cfm_mip_config *const config,
				    struct netlink_ext_ack *extack)
{
	struct switchdev_obj_cfm_mip_config_set cfm_obj = {
		.obj.orig_dev = br->dev,
		.obj.id = SWITCHDEV_OBJ_ID_MIP_CONFIG_CFM,
		.obj.flags = 0,
		.instance = mip->instance,
		.mdlevel = config->mdlevel,
		.raps_handling = config->raps_handling,
	};
	memcpy(&cfm_obj.unicast_mac, &config->unicast_mac, sizeof(cfm_obj.unicast_mac));

	return switchdev_port_obj_add(br->dev, &cfm_obj.obj, NULL);
}
