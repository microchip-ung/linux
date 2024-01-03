// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <uapi/linux/mrp_bridge.h>
#include <linux/if_bridge.h>

#include "afi_api.h"
#include "mrp_api.h"

#include "sparx5_mrp.h"
#include "sparx5_main.h"

#define MRP_FWD_NOP		0
#define MRP_FWD_COPY		1
#define MRP_FWD_REDIR		2
#define MRP_FWD_DISC		3

#define CONFIG_TEST		0
#define CONFIG_IN_TEST		1

#define SPARX5_MRP_RULE_ID_OFFSET	3072

static const u8 mrp_test_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x1 };
static const u8 mrp_in_test_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x3 };
static const u8 mrp_dmac_mask[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8 };

static int32_t sparx5_mrp_port_to_voe(struct sparx5_port *port)
{
	return port->portno + 32;
}

void sparx5_mrp_ring_open(struct sparx5 *sparx5)
{
	if (is_sparx5(sparx5))
		return;

	/* If this is not an interrupt for MRP then just ignore it */
	if (!VOP_MASTER_INTR_CTRL_OAM_MEP_INTR_GET(spx5_rd(sparx5,
							   VOP_MASTER_INTR_CTRL)))
		return;

	mrp_ring_interrupt(sparx5->mrp_ctrl);
}

void sparx5_mrp_in_open(struct sparx5 *sparx5)
{
	if (is_sparx5(sparx5))
		return;

	/* If this is not an interrupt for MRP then just ignore it */
	if (!VOP_MASTER_INTR_CTRL_OAM_MEP_INTR_GET(spx5_rd(sparx5,
							   VOP_MASTER_INTR_CTRL)))
		return;

	mrp_in_interrupt(sparx5->mrp_ctrl);
}

int sparx5_handle_mrp_port_role(struct sparx5_port *port,
				enum br_mrp_port_role_type role)
{
	struct mrp_port *mrp_port = port->mrp_port;

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_set_port_role(mrp_port, role);
}

static int sparx5_mrp_add_vcap_rule(struct sparx5_port *port)
{
	int rule_id = SPARX5_MRP_RULE_ID_OFFSET + port->portno;
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(vctrl, port->ndev, SPARX5_VCAP_CID_IS0_L1,
				VCAP_USER_MRP, (port->portno << 8), rule_id);
	if (!vrule || IS_ERR(vrule))
		return PTR_ERR(vrule);

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_ETYPE, ETH_P_MRP, ~0);
	if (err) {
		vcap_del_rule(vctrl, port->ndev, rule_id);
		return err;
	}

	err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_FULL);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_OAM_MRP_ENA, VCAP_BIT_1);
	if (err) {
		vcap_free_rule(vrule);
		return err;
	}

	err = vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		vcap_free_rule(vrule);
		return err;
	}

	err = vcap_add_rule(vrule);

	/* Free the local copy of the rule */
	vcap_free_rule(vrule);
	return err;
}

static int __sparx5_handle_mrp_add(struct sparx5_port *port,
				   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_mrp *mrp = SWITCHDEV_OBJ_MRP(obj);
	struct sparx5 *sparx5 = port->sparx5;

	if (is_sparx5(sparx5))
		return -EOPNOTSUPP;

	if (mrp->p_port != port->ndev && mrp->s_port != port->ndev)
		return 0;

	port->mrp_port = mrp_add_port(sparx5->mrp_ctrl, mrp, port->ndev);
	if (IS_ERR(port->mrp_port))
		return PTR_ERR(port->mrp_port);

	sparx5_mrp_add_vcap_rule(port);

	return 0;
}

int sparx5_handle_mrp_add(struct net_device *dev,
			  const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_add(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int sparx5_mrp_del_vcap_rule(struct sparx5_port *port)
{
	int rule_id = SPARX5_MRP_RULE_ID_OFFSET + port->portno;

	return vcap_del_rule(port->sparx5->vcap_ctrl, port->ndev, rule_id);
}

static int __sparx5_handle_mrp_del(struct sparx5_port *port,
				   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_mrp *mrp = SWITCHDEV_OBJ_MRP(obj);
	struct sparx5 *sparx5 = port->sparx5;
	int ret;

	if (is_sparx5(sparx5))
		return -EOPNOTSUPP;

	if (mrp->p_port != port->ndev && mrp->s_port != port->ndev)
		return 0;

	ret = sparx5_mrp_del_vcap_rule(port);
	if (ret)
		return ret;

	ret = mrp_del_port(sparx5->mrp_ctrl, mrp, port->mrp_port);
	if (ret)
		return ret;

	port->mrp_port = NULL;

	return 0;
}

int sparx5_handle_mrp_del(struct net_device *dev,
			  const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_del(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_ring_test_add(struct sparx5_port *port,
					     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_test_mrp *mrp = SWITCHDEV_OBJ_RING_TEST_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_start_ring_test(port->mrp_port, mrp);
}

int sparx5_handle_mrp_ring_test_add(struct net_device *dev,
				    const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_ring_test_add(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_ring_test_del(struct sparx5_port *port,
					     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_test_mrp *mrp = SWITCHDEV_OBJ_RING_TEST_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_stop_ring_test(port->mrp_port, mrp);
}

int sparx5_handle_mrp_ring_test_del(struct net_device *dev,
				    const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_ring_test_del(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_ring_state_add(struct sparx5_port *port,
					      const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_state_mrp *mrp = SWITCHDEV_OBJ_RING_STATE_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_set_ring_state(port->mrp_port, mrp);
}

int sparx5_handle_mrp_ring_state_add(struct net_device *dev,
				     const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_ring_state_add(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_ring_role_add(struct sparx5_port *port,
					     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_role_mrp *mrp = SWITCHDEV_OBJ_RING_ROLE_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_set_ring_role(port->mrp_port, mrp);
}

int sparx5_handle_mrp_ring_role_add(struct net_device *dev,
				    const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_ring_role_add(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_ring_role_del(struct sparx5_port *port,
					     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_role_mrp *mrp = SWITCHDEV_OBJ_RING_ROLE_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_set_ring_role(port->mrp_port, mrp);
}

int sparx5_handle_mrp_ring_role_del(struct net_device *dev,
				    const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_ring_role_del(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_in_test_add(struct sparx5_port *port,
					   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_test_mrp *mrp = SWITCHDEV_OBJ_IN_TEST_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_start_in_test(port->mrp_port, mrp);
}

int sparx5_handle_mrp_in_test_add(struct net_device *dev,
				  const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_in_test_add(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_in_test_del(struct sparx5_port *port,
					   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_test_mrp *mrp = SWITCHDEV_OBJ_IN_TEST_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_stop_in_test(port->mrp_port, mrp);
}

int sparx5_handle_mrp_in_test_del(struct net_device *dev,
				  const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_in_test_del(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_in_state_add(struct sparx5_port *port,
					    const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_state_mrp *mrp = SWITCHDEV_OBJ_IN_STATE_MRP(obj);

	if (is_sparx5(port->sparx5))
		return -EOPNOTSUPP;

	return mrp_port_set_in_state(port->mrp_port, mrp);
}

int sparx5_handle_mrp_in_state_add(struct net_device *dev,
				   const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_in_state_add(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_in_role_add(struct sparx5_port *port,
					   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_role_mrp *mrp = SWITCHDEV_OBJ_IN_ROLE_MRP(obj);
	struct sparx5 *sparx5 = port->sparx5;

	if (is_sparx5(sparx5))
		return -EOPNOTSUPP;

	if (mrp->i_port != port->ndev)
		return 0;

	port->mrp_port = mrp_add_in_port(sparx5->mrp_ctrl, mrp, port->ndev);
	if (IS_ERR(port->mrp_port))
		return PTR_ERR(port->mrp_port);

	return mrp_port_set_in_role(port->mrp_port, mrp);
}

int sparx5_handle_mrp_in_role_add(struct net_device *dev,
				  const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_in_role_add(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int __sparx5_handle_mrp_in_role_del(struct sparx5_port *port,
					   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_role_mrp *mrp = SWITCHDEV_OBJ_IN_ROLE_MRP(obj);
	struct sparx5 *sparx5 = port->sparx5;
	int ret;

	if (is_sparx5(sparx5))
		return -EOPNOTSUPP;

	if (mrp->i_port != port->ndev)
		return 0;

	ret = mrp_port_set_in_role(port->mrp_port, mrp);
	if (ret)
		return ret;

	ret = mrp_del_in_port(sparx5->mrp_ctrl, mrp, port->mrp_port);
	if (ret)
		return ret;

	port->mrp_port = NULL;

	return 0;
}

int sparx5_handle_mrp_in_role_del(struct net_device *dev,
				  const struct switchdev_obj *obj)
{
	struct net_device *lower;
	struct list_head *iter;
	int err;

	netdev_for_each_lower_dev(dev, lower, iter) {
		if (!sparx5_netdevice_check(lower))
			continue;

		err = __sparx5_handle_mrp_in_role_del(netdev_priv(lower), obj);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return 0;
}

static int sparx5_mrp_port_init(struct mrp_port *mrp_port, u16 prio)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* Enable MEP and LOC_SCAN */
	spx5_rmw(VOP_VOP_CTRL_LOC_SCAN_ENA_SET(1) |
		 VOP_VOP_CTRL_VOP_ENA_SET(1),
		 VOP_VOP_CTRL_LOC_SCAN_ENA |
		 VOP_VOP_CTRL_VOP_ENA,
		 sparx5, VOP_VOP_CTRL);

	spx5_rmw(VOP_VOE_MISC_CONFIG_VOE_ENA_SET(4),
		 VOP_VOE_MISC_CONFIG_VOE_ENA,
		 sparx5, VOP_VOE_MISC_CONFIG(sparx5_mrp_port_to_voe(port)));

	/* Activate MRP endpoint */
	spx5_rmw(VOP_MRP_MRP_CTRL_MRP_ENA_SET(1),
		 VOP_MRP_MRP_CTRL_MRP_ENA,
		 sparx5, VOP_MRP_MRP_CTRL(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_TST_PRIO_CFG_OWN_PRIO_SET(prio),
		 VOP_MRP_TST_PRIO_CFG_OWN_PRIO,
		 sparx5, VOP_MRP_TST_PRIO_CFG(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_uninit(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* Disable MEP and LOC_SCAN */
	spx5_rmw(VOP_VOP_CTRL_LOC_SCAN_ENA_SET(0) |
		 VOP_VOP_CTRL_VOP_ENA_SET(0),
		 VOP_VOP_CTRL_LOC_SCAN_ENA |
		 VOP_VOP_CTRL_VOP_ENA,
		 sparx5, VOP_VOP_CTRL);

	spx5_rmw(VOP_VOE_MISC_CONFIG_VOE_ENA_SET(0),
		 VOP_VOE_MISC_CONFIG_VOE_ENA,
		 sparx5, VOP_VOE_MISC_CONFIG(sparx5_mrp_port_to_voe(port)));

	/* Dissactivate  MRP endpoint */
	spx5_rmw(VOP_MRP_MRP_CTRL_MRP_ENA_SET(0),
		 VOP_MRP_MRP_CTRL_MRP_ENA,
		 sparx5, VOP_MRP_MRP_CTRL(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_TST_PRIO_CFG_OWN_PRIO_SET(0),
		 VOP_MRP_TST_PRIO_CFG_OWN_PRIO,
		 sparx5, VOP_MRP_TST_PRIO_CFG(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_update_mac(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;
	u32 macl, mach;

	mach = sparx5->hw_bridge_dev->dev_addr[0] << 8;
	mach |= sparx5->hw_bridge_dev->dev_addr[1] << 0;
	macl = sparx5->hw_bridge_dev->dev_addr[2] << 24;
	macl |= sparx5->hw_bridge_dev->dev_addr[3] << 16;
	macl |= sparx5->hw_bridge_dev->dev_addr[4] << 8;
	macl |= sparx5->hw_bridge_dev->dev_addr[5] << 0;

	spx5_wr(macl, sparx5, VOP_MRP_MRP_MAC_LSB(sparx5_mrp_port_to_voe(port)));
	spx5_wr(mach, sparx5, VOP_MRP_MRP_MAC_MSB(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_update_mrm_mac(struct mrp_port *mrp_port,
					  const u8 mac[ETH_ALEN])
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;
	u32 macl, mach;

	mach = mac[0] << 8;
	mach |= mac[1] << 0;
	macl = mac[2] << 24;
	macl |= mac[3] << 16;
	macl |= mac[4] << 8;
	macl |= mac[5] << 0;

	spx5_wr(VOP_MRP_BEST_MAC_MSB_BEST_MAC_MSB_SET(mach),
		sparx5, VOP_MRP_BEST_MAC_MSB(sparx5_mrp_port_to_voe(port)));
	spx5_wr(macl, sparx5, VOP_MRP_BEST_MAC_LSB(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_set_ring_state(struct mrp_port *mrp_port,
					  u32 ring_transitions,
					  enum br_mrp_ring_state_type ring_state)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_STATE_SET(ring_state),
		 VOP_MRP_MRP_TX_CFG_MRP_STATE,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_TEST));

	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_TRANS_SET(ring_transitions),
		 VOP_MRP_MRP_TX_CFG_MRP_TRANS,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_TEST));

	/* In case the ring is closed, it means that a test frame arrived to the
	 * CPU, so allow again the HW to notify the SW when the ring is open
	 */
	if (ring_state == BR_MRP_RING_STATE_CLOSED)
		spx5_rmw(VOP_MRP_MRP_STICKY_TST_LOC_STICKY_SET(1),
			 VOP_MRP_MRP_STICKY_TST_LOC_STICKY,
			 sparx5, VOP_MRP_MRP_STICKY(sparx5_mrp_port_to_voe(port)));
	return 0;
}

static int sparx5_mrp_port_set_in_state(struct mrp_port *mrp_port,
					u32 in_transitions,
					enum br_mrp_in_state_type in_state)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_STATE_SET(in_state),
		 VOP_MRP_MRP_TX_CFG_MRP_STATE,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_IN_TEST));

	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_TRANS_SET(in_transitions),
		 VOP_MRP_MRP_TX_CFG_MRP_TRANS,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_IN_TEST));

	/* In case the ring is closed, it means that a test frame arrived to the
	 * CPU, so allow again the HW to notify the SW when the ring is open
	 */
	if (in_state == BR_MRP_IN_STATE_CLOSED)
		spx5_rmw(VOP_MRP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
			 VOP_MRP_MRP_STICKY_ITST_LOC_STICKY,
			 sparx5, VOP_MRP_MRP_STICKY(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_set_port_role(struct mrp_port *mrp_port,
					 enum br_mrp_port_role_type role)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* Update the port role in HW for test and interconnect test frames
	 * even if the there will not be a interconnect ring. Because these
	 * values will be applied to the frame only if the bit MRO_MISC_UPD_ENA
	 * is set
	 */
	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_PORTROLE_SET(role),
		 VOP_MRP_MRP_TX_CFG_MRP_PORTROLE,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_TEST));

	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_PORTROLE_SET(role),
		 VOP_MRP_MRP_TX_CFG_MRP_PORTROLE,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_IN_TEST));

	return 0;
}

static int sparx5_mrp_port_hijack_test(struct mrp_port *mrp_port,
				       struct sk_buff *skb)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;
	u32 ifh[IFH_LEN];

	memset(ifh, 0x0, sizeof(u32) * IFH_LEN);

	/* The port number needs to be 0 for AFI injected frames or the REW will
	 * see this a CPU queue mask and not work as expected.
	 */
	sparx5_set_port_ifh(sparx5, ifh, 0,
			    SPX5_PACKET_PIPELINE_PT_REW_PORT_VOE);
	sparx5_set_port_ifh_afi(sparx5, ifh, true);
	sparx5_set_port_ifh_sp(sparx5, ifh, true);
	sparx5_set_port_ifh_cl_qos(sparx5, ifh, 7);
	sparx5_set_port_ifh_pdu_type(sparx5, ifh, 10);
	sparx5_set_port_ifh_pdu_w16_offset(sparx5, ifh, 7);

	sparx5_port_xmit(port, skb, ifh);

	return 0;
}

static int sparx5_mrp_port_afi_cfg(struct mrp_port *mrp_port,
				   struct afi_slow_inj_alloc_cfg *cfg)
{
	struct sparx5_port *port = mrp_port->priv;

	cfg->port_no = port->portno;
	cfg->prio = 0;

	return 0;
}

static int sparx5_mrp_port_redirect_control(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* All the frames except Test and IntTest frames need to be redirected
	 * to CPU and allow SW to process and forward the frames
	 */
	spx5_rmw(VOP_MRP_MRP_FWD_CTRL_ERR_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_LD_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_LU_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_TC_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL_SET(2) |
		 VOP_MRP_MRP_FWD_CTRL_OTHER_FWD_SEL_SET(2),
		 VOP_MRP_MRP_FWD_CTRL_ERR_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_LD_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_LU_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_TC_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_OTHER_FWD_SEL,
		 sparx5, VOP_MRP_MRP_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_redirect_ring_test(struct mrp_port *mrp_port,
					      bool redirect)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* Redirect test frames with a lower priority to the CPU */
	/* In case the node is in MRM and has support for MRA then in case the
	 * is a test frame with a lower priority the node should send a
	 * TestMgrNAck to tell the remote node to stop sending the frames. The
	 * SW will generate this frame therefore it is required to send these
	 * tests frames to SW so it can detect this scenario. The frames with a
	 * higher priority are not needed to be copy to CPU because the remote
	 * node should send TestMgrNAck and the SW should process this frame and
	 * then terminate the transmitions of Test frames and go in MRC mode
	 */
	spx5_rmw(VOP_MRP_TST_CFG_CHK_BEST_MRM_ENA_SET(1) |
		 VOP_MRP_TST_CFG_CHK_REM_PRIO_ENA_SET(1),
		 VOP_MRP_TST_CFG_CHK_BEST_MRM_ENA |
		 VOP_MRP_TST_CFG_CHK_REM_PRIO_ENA,
		 sparx5, VOP_MRP_TST_CFG(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(2),
		 VOP_MRP_TST_FWD_CTRL_LO_PRIO_FWD_SEL,
		 sparx5, VOP_MRP_TST_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_terminate_ring_test(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* Terminate frames */
	/* When the frame is discard means that not to forward the frame
	 * to other front ports but if other block enables the copy to the
	 * CPU then the frame will go to CPU. In this case, the multicast
	 * frames are flooded also to the CPU, so the fix consists
	 * of adding entries to MAC table to disable copying of the frames
	 * to CPU
	 */
	spx5_rmw(VOP_MRP_TST_FWD_CTRL_REM_FWD_SEL_SET(3) |
		 VOP_MRP_TST_FWD_CTRL_OWN_FWD_SEL_SET(3) |
		 VOP_MRP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(3) |
		 VOP_MRP_TST_FWD_CTRL_HI_PRIO_FWD_SEL_SET(3),
		 VOP_MRP_TST_FWD_CTRL_REM_FWD_SEL |
		 VOP_MRP_TST_FWD_CTRL_OWN_FWD_SEL |
		 VOP_MRP_TST_FWD_CTRL_LO_PRIO_FWD_SEL |
		 VOP_MRP_TST_FWD_CTRL_HI_PRIO_FWD_SEL,
		 sparx5, VOP_MRP_TST_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_MRP),
			  mrp_test_dmac, port->pvid);

	return 0;
}

static int sparx5_mrp_port_forward_ring_test(struct mrp_port *mrp_port,
					     struct mrp_port *mrp_partner_port,
					     bool forward)
{
	struct sparx5_port *partner = mrp_partner_port->priv;
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_RING_MASK_CFG_RING_PORTMASK_SET(BIT(partner->portno)),
		 VOP_MRP_RING_MASK_CFG_RING_PORTMASK,
		 sparx5, VOP_MRP_RING_MASK_CFG(sparx5_mrp_port_to_voe(port)));

	if (forward)
		sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_MRP),
				  mrp_test_dmac, port->pvid);
	else
		sparx5_mact_forget(sparx5, mrp_test_dmac, port->pvid);

	spx5_rmw(VOP_MRP_MRP_FWD_CTRL_MRP_TST_FWD_SEL_SET(MRP_FWD_NOP) |
		 VOP_MRP_MRP_FWD_CTRL_RING_MASK_ENA_SET(forward),
		 VOP_MRP_MRP_FWD_CTRL_MRP_TST_FWD_SEL |
		 VOP_MRP_MRP_FWD_CTRL_RING_MASK_ENA,
		 sparx5, VOP_MRP_MRP_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_process_ring_test(struct mrp_port *mrp_port,
					     bool process)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* Enable processing of the frame */
	spx5_rmw(VOP_MRP_MRP_CTRL_MRP_TST_ENA_SET(process),
		 VOP_MRP_MRP_CTRL_MRP_TST_ENA,
		 sparx5, VOP_MRP_MRP_CTRL(sparx5_mrp_port_to_voe(port)));

	/* 51 represents 10 usec */
	/* MRP loc index 0 represents that there is no LOC used, while index 1
	 * in MRP represents index 0 in MEP, therefor subtract 1
	 */
	spx5_wr(mrp_port->ring_interval / 10 * 51, sparx5,
		VOP_LOC_PERIOD_CFG(mrp_port->mrp_inst->ring_loc_idx - 1));

	/* Set LOC */
	spx5_rmw(VOP_MRP_TST_CFG_CLR_MISS_CNT_ENA_SET(process) |
		 VOP_MRP_TST_CFG_LOC_PERIOD_SET(mrp_port->mrp_inst->ring_loc_idx),
		 VOP_MRP_TST_CFG_CLR_MISS_CNT_ENA |
		 VOP_MRP_TST_CFG_LOC_PERIOD,
		 sparx5, VOP_MRP_TST_CFG(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_rewrite_ring_test(struct mrp_port *mrp_port,
					     bool rewrite)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_TIMESTAMP_UPD_ENA_SET(rewrite) |
		 VOP_MRP_MRP_TX_CFG_MRP_SEQ_UPD_ENA_SET(rewrite) |
		 VOP_MRP_MRP_TX_CFG_MRP_MISC_UPD_ENA_SET(rewrite),
		 VOP_MRP_MRP_TX_CFG_MRP_TIMESTAMP_UPD_ENA |
		 VOP_MRP_MRP_TX_CFG_MRP_SEQ_UPD_ENA |
		 VOP_MRP_MRP_TX_CFG_MRP_MISC_UPD_ENA,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_TEST));

	return 0;
}

static enum mrp_interrupt_status sparx5_mrp_port_get_ring_intr_status(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;
	u32 val;

	val = spx5_rd(sparx5, VOP_MRP_MRP_STICKY(sparx5_mrp_port_to_voe(port)));

	if (!VOP_MRP_MRP_STICKY_TST_LOC_STICKY_GET(val))
		return MRP_INTERRUPT_STATUS_NONE;

	spx5_rmw(VOP_MRP_MRP_STICKY_TST_LOC_STICKY_SET(1),
		 VOP_MRP_MRP_STICKY_TST_LOC_STICKY,
		 sparx5, VOP_MRP_MRP_STICKY(sparx5_mrp_port_to_voe(port)));

	val = spx5_rd(sparx5, VOP_MRP_TST_STAT(sparx5_mrp_port_to_voe(port)));
	val = VOP_MRP_TST_STAT_MISS_CNT_GET(val);

	if (val == mrp_port->ring_max_miss) {
		sparx5_attr_stp_state_set(port, BR_STATE_FORWARDING);
		return MRP_INTERRUPT_STATUS_OPEN;
	}

	sparx5_attr_stp_state_set(port, BR_STATE_BLOCKING);
	return MRP_INTERRUPT_STATUS_CLOSED;
}

static int sparx5_mrp_port_disable_ring_intr(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(0),
		 VOP_MRP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
		 sparx5, VOP_MRP_MRP_INTR_ENA(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_enable_ring_intr(struct mrp_port *mrp_port,
					    u32 max)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_TST_STAT_MAX_MISS_CNT_SET(max),
		 VOP_MRP_TST_STAT_MAX_MISS_CNT,
		 sparx5, VOP_MRP_TST_STAT(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_TST_STAT_MISS_CNT_SET(0),
		 VOP_MRP_TST_STAT_MISS_CNT,
		 sparx5, VOP_MRP_TST_STAT(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(1),
		 VOP_MRP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
		 sparx5, VOP_MRP_MRP_INTR_ENA(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_terminate_in_test(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_ITST_FWD_CTRL_ITST_OWN_FWD_SEL_SET(3),
		 VOP_MRP_ITST_FWD_CTRL_ITST_OWN_FWD_SEL,
		 sparx5, VOP_MRP_ITST_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_MRP),
			  mrp_in_test_dmac, port->pvid);

	return 0;
}

static int sparx5_mrp_port_forward_in_test(struct mrp_port *mrp_port,
					   struct mrp_port *mrp_partner_port_1,
					   struct mrp_port *mrp_partner_port_2,
					   bool forward)
{
	struct sparx5_port *partner_1 = mrp_partner_port_1->priv;
	struct sparx5_port *partner_2 = mrp_partner_port_2->priv;
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;
	u32 mask;

	mask = BIT(partner_1->portno) | BIT(partner_2->portno);

	spx5_rmw(VOP_MRP_ITST_FWD_CTRL_ITST_OWN_FWD_SEL_SET(3),
		 VOP_MRP_ITST_FWD_CTRL_ITST_OWN_FWD_SEL,
		 sparx5, VOP_MRP_ITST_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
		 VOP_MRP_ICON_MASK_CFG_ICON_PORTMASK,
		 sparx5, VOP_MRP_ICON_MASK_CFG(sparx5_mrp_port_to_voe(port)));

	if (forward)
		sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_MRP),
				  mrp_in_test_dmac, port->pvid);
	else
		sparx5_mact_forget(sparx5, mrp_in_test_dmac, port->pvid);

	spx5_rmw(VOP_MRP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
		 VOP_MRP_MRP_FWD_CTRL_ICON_MASK_ENA,
		 sparx5, VOP_MRP_MRP_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_forward_rem_in_test(struct mrp_port *mrp_port,
					       struct mrp_port *mrp_partner_port,
					       bool forward)
{
	struct sparx5_port *partner = mrp_partner_port->priv;
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;
	u32 mask;

	/* If the frame came on a ring port and it is from a remote MIM
	 * then it is required to forward the frame only to the other
	 * ring port. If the frame is itself then the miss count should
	 * be clear, this is done by HW.
	 */
	spx5_rmw(VOP_MRP_ITST_FWD_CTRL_ITST_REM_FWD_SEL_SET(0),
		 VOP_MRP_ITST_FWD_CTRL_ITST_REM_FWD_SEL,
		 sparx5, VOP_MRP_ITST_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	mask = BIT(partner->portno);

	spx5_rmw(VOP_MRP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
		 VOP_MRP_ICON_MASK_CFG_ICON_PORTMASK,
		 sparx5, VOP_MRP_ICON_MASK_CFG(sparx5_mrp_port_to_voe(port)));

	if (forward)
		sparx5_mact_learn(sparx5, sparx5_get_pgid_index(sparx5, PGID_MRP),
				  mrp_in_test_dmac, port->pvid);
	else
		sparx5_mact_forget(sparx5, mrp_in_test_dmac, port->pvid);

	spx5_rmw(VOP_MRP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
		 VOP_MRP_MRP_FWD_CTRL_ICON_MASK_ENA,
		 sparx5, VOP_MRP_MRP_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_process_in_test(struct mrp_port *mrp_port,
					   bool process)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	/* Enable processing of the frame */
	spx5_rmw(VOP_MRP_MRP_CTRL_MRP_ITST_ENA_SET(process),
		 VOP_MRP_MRP_CTRL_MRP_ITST_ENA,
		 sparx5, VOP_MRP_MRP_CTRL(sparx5_mrp_port_to_voe(port)));

	/* 51 represents 10 usec */
	/* MRP loc index 0 represents that there is no LOC used, while index 1
	 * in MRP represets index 0 in MEP, therefor subtract 1
	 */
	spx5_wr(mrp_port->in_interval / 10 * 51, sparx5,
		VOP_LOC_PERIOD_CFG(mrp_port->mrp_inst->in_loc_idx - 1));

	/* Set LOC */
	spx5_rmw(VOP_MRP_ITST_CFG_ITST_CLR_MISS_CNT_ENA_SET(process) |
		 VOP_MRP_ITST_CFG_ITST_LOC_PERIOD_SET(mrp_port->mrp_inst->in_loc_idx),
		 VOP_MRP_ITST_CFG_ITST_CLR_MISS_CNT_ENA |
		 VOP_MRP_ITST_CFG_ITST_LOC_PERIOD,
		 sparx5, VOP_MRP_ITST_CFG(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_rewrite_in_test(struct mrp_port *mrp_port,
					   bool rewrite)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_MRP_TX_CFG_MRP_TIMESTAMP_UPD_ENA_SET(rewrite) |
		 VOP_MRP_MRP_TX_CFG_MRP_SEQ_UPD_ENA_SET(rewrite) |
		 VOP_MRP_MRP_TX_CFG_MRP_MISC_UPD_ENA_SET(rewrite),
		 VOP_MRP_MRP_TX_CFG_MRP_TIMESTAMP_UPD_ENA |
		 VOP_MRP_MRP_TX_CFG_MRP_SEQ_UPD_ENA |
		 VOP_MRP_MRP_TX_CFG_MRP_MISC_UPD_ENA,
		 sparx5, VOP_MRP_MRP_TX_CFG(sparx5_mrp_port_to_voe(port), CONFIG_IN_TEST));

	return 0;
}

static enum mrp_interrupt_status sparx5_mrp_port_get_in_intr_status(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;
	u32 val;

	val = spx5_rd(sparx5, VOP_MRP_MRP_STICKY(sparx5_mrp_port_to_voe(port)));

	if (!VOP_MRP_MRP_STICKY_ITST_LOC_STICKY_GET(val))
		return MRP_INTERRUPT_STATUS_NONE;

	spx5_rmw(VOP_MRP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
		 VOP_MRP_MRP_STICKY_ITST_LOC_STICKY,
		 sparx5, VOP_MRP_MRP_STICKY(sparx5_mrp_port_to_voe(port)));

	val = spx5_rd(sparx5, VOP_MRP_ITST_STAT(sparx5_mrp_port_to_voe(port)));
	val = VOP_MRP_ITST_STAT_ITST_MISS_CNT_GET(val);

	if (val == mrp_port->in_max_miss) {
		sparx5_attr_stp_state_set(port, BR_STATE_FORWARDING);
		return MRP_INTERRUPT_STATUS_OPEN;
	}

	sparx5_attr_stp_state_set(port, BR_STATE_BLOCKING);
	return MRP_INTERRUPT_STATUS_CLOSED;
}

static int sparx5_mrp_port_disable_in_intr(struct mrp_port *mrp_port)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(0),
		 VOP_MRP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		 sparx5, VOP_MRP_MRP_INTR_ENA(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static int sparx5_mrp_port_enable_in_intr(struct mrp_port *mrp_port,
					  u32 max)
{
	struct sparx5_port *port = mrp_port->priv;
	struct sparx5 *sparx5 = port->sparx5;

	spx5_rmw(VOP_MRP_ITST_STAT_ITST_MAX_MISS_CNT_SET(max),
		 VOP_MRP_ITST_STAT_ITST_MAX_MISS_CNT,
		 sparx5, VOP_MRP_ITST_STAT(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_ITST_STAT_ITST_MISS_CNT_SET(0),
		 VOP_MRP_ITST_STAT_ITST_MISS_CNT,
		 sparx5, VOP_MRP_ITST_STAT(sparx5_mrp_port_to_voe(port)));

	spx5_rmw(VOP_MRP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
		 VOP_MRP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		 sparx5, VOP_MRP_MRP_INTR_ENA(sparx5_mrp_port_to_voe(port)));

	return 0;
}

static struct mrp_operations sparx5_mrp_operations = {
	.mrp_port_init = sparx5_mrp_port_init,
	.mrp_port_uninit = sparx5_mrp_port_uninit,
	.mrp_port_update_mac = sparx5_mrp_port_update_mac,
	.mrp_port_update_mrm_mac = sparx5_mrp_port_update_mrm_mac,

	.mrp_port_set_ring_state = sparx5_mrp_port_set_ring_state,
	.mrp_port_set_in_state = sparx5_mrp_port_set_in_state,

	.mrp_port_set_port_role = sparx5_mrp_port_set_port_role,

	.mrp_port_hijack_test = sparx5_mrp_port_hijack_test,
	.mrp_port_afi_cfg = sparx5_mrp_port_afi_cfg,

	.mrp_port_redirect_control = sparx5_mrp_port_redirect_control,

	.mrp_port_terminate_ring_test = sparx5_mrp_port_terminate_ring_test,
	.mrp_port_redirect_ring_test = sparx5_mrp_port_redirect_ring_test,
	.mrp_port_forward_ring_test = sparx5_mrp_port_forward_ring_test,
	.mrp_port_rewrite_ring_test = sparx5_mrp_port_rewrite_ring_test,
	.mrp_port_process_ring_test = sparx5_mrp_port_process_ring_test,

	.mrp_port_get_ring_interrupt_status = sparx5_mrp_port_get_ring_intr_status,
	.mrp_port_disable_ring_interrupt = sparx5_mrp_port_disable_ring_intr,
	.mrp_port_enable_ring_interrupt = sparx5_mrp_port_enable_ring_intr,

	.mrp_port_get_in_interrupt_status = sparx5_mrp_port_get_in_intr_status,
	.mrp_port_disable_in_interrupt = sparx5_mrp_port_disable_in_intr,
	.mrp_port_enable_in_interrupt = sparx5_mrp_port_enable_in_intr,

	.mrp_port_terminate_in_test = sparx5_mrp_port_terminate_in_test,
	.mrp_port_forward_in_test = sparx5_mrp_port_forward_in_test,
	.mrp_port_forward_rem_in_test = sparx5_mrp_port_forward_rem_in_test,
	.mrp_port_process_in_test = sparx5_mrp_port_process_in_test,
	.mrp_port_rewrite_in_test = sparx5_mrp_port_rewrite_in_test,
};

int sparx5_mrp_init(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	struct mrp_control *mrp_ctrl;
	struct sparx5_port *port;

	if (is_sparx5(sparx5))
		return 0;

	mrp_ctrl = kzalloc(sizeof(*mrp_ctrl), GFP_KERNEL);
	if (!mrp_ctrl)
		return -ENOMEM;

	mrp_ctrl->ops = &sparx5_mrp_operations;
	mrp_ctrl->priv = sparx5;
	mrp_ctrl->afi_ctrl = sparx5->afi_ctrl;
	sparx5->mrp_ctrl = mrp_ctrl;

	/* Initialize the controller before initialize the local */
	mrp_init(mrp_ctrl);

	for (int i = 0; i < consts->chip_ports; ++i) {
		port = sparx5->ports[i];
		if (!port)
			continue;

		spx5_rmw(VOP_MRP_MRP_FWD_CTRL_ERR_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TST_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LD_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LU_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TC_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITST_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_OTHER_FWD_SEL_SET(0),
			 VOP_MRP_MRP_FWD_CTRL_ERR_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TST_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LD_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LU_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TC_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITST_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_OTHER_FWD_SEL,
			 sparx5, VOP_MRP_MRP_FWD_CTRL(sparx5_mrp_port_to_voe(port)));

		sparx5_mact_learn(sparx5,
				  sparx5_get_pgid_index(sparx5, PGID_MRP),
				  mrp_test_dmac, port->pvid);

		spx5_wr(VOP_MRP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(1) |
			VOP_MRP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
			sparx5, VOP_MRP_MRP_INTR_ENA(sparx5_mrp_port_to_voe(port)));
	}

	spx5_rmw(VOP_MASTER_INTR_CTRL_OAM_MEP_INTR_ENA_SET(1),
		 VOP_MASTER_INTR_CTRL_OAM_MEP_INTR_ENA,
		 sparx5, VOP_MASTER_INTR_CTRL);

	/* Add PGID entry to discard all the frames */
	spx5_wr(0, sparx5,
		ANA_AC_PGID_MISC_CFG(sparx5_get_pgid_index(sparx5, PGID_MRP)));

	return 0;
}

void sparx5_mrp_deinit(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	struct sparx5_port *port;

	if (is_sparx5(sparx5))
		return;

	/* Forward all MRP frames - by default */
	for (int i = 0; i < consts->chip_ports; ++i) {
		port = sparx5->ports[i];
		if (!port)
			continue;

		spx5_rmw(VOP_MRP_MRP_FWD_CTRL_ERR_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TST_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LD_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LU_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TC_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITST_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL_SET(0) |
			 VOP_MRP_MRP_FWD_CTRL_OTHER_FWD_SEL_SET(0),
			 VOP_MRP_MRP_FWD_CTRL_ERR_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TST_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LD_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_LU_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_TC_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITST_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL |
			 VOP_MRP_MRP_FWD_CTRL_OTHER_FWD_SEL,
			 sparx5, VOP_MRP_MRP_FWD_CTRL(port->portno + 32));

		sparx5_mact_forget(sparx5, mrp_test_dmac, port->pvid);

		spx5_wr(VOP_MRP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(0) |
			VOP_MRP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(0),
			sparx5, VOP_MRP_MRP_INTR_ENA(port->portno + 32));
	}

	spx5_rmw(VOP_MASTER_INTR_CTRL_OAM_MEP_INTR_ENA_SET(0),
		 VOP_MASTER_INTR_CTRL_OAM_MEP_INTR_ENA,
		 sparx5, VOP_MASTER_INTR_CTRL);

	mrp_deinit(sparx5->mrp_ctrl);

	kfree(sparx5->mrp_ctrl);
}

void sparx5_mrp_port_update_mrp_mac(struct sparx5_port *port,
				    const u8 mac[ETH_ALEN])
{
	struct sparx5 *sparx5 = port->sparx5;

	if (!port->mrp_port)
		return;

	if (is_sparx5(sparx5))
		return;

	sparx5_mrp_port_update_mac(port->mrp_port);
	sparx5_mrp_port_update_mrm_mac(port->mrp_port, mac);
}
