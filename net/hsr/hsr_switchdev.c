// SPDX-License-Identifier: GPL-2.0

#include <net/switchdev.h>

#include "hsr_framereg.h"
#include "hsr_switchdev.h"

static struct net_device *hsr_port_dev_get(struct hsr_priv *hsr,
					   enum hsr_port_type pt)
{
	struct hsr_port *hp;

	hp = hsr_port_get_hsr(hsr, pt);
	return hp ? hp->dev : NULL;
}

int hsr_node_switchdev_add(struct hsr_priv *hsr, struct hsr_node *node)
{
	struct switchdev_obj_node_hsr hsr_obj = {
		.obj.orig_dev = hsr_port_dev_get(hsr, HSR_PT_SLAVE_A),
		.obj.id = SWITCHDEV_OBJ_ID_NODE_HSR,
		.obj.flags = SWITCHDEV_F_DEFER,
		.hsr = hsr_port_dev_get(hsr, HSR_PT_MASTER),
	};

	if (!hsr_obj.obj.orig_dev)
		return 0;

	ether_addr_copy(hsr_obj.addr_A, node->macaddress_A);

	return switchdev_port_obj_add(hsr_obj.obj.orig_dev, &hsr_obj.obj, NULL);
}

int hsr_node_switchdev_del(struct hsr_priv *hsr, struct hsr_node *node)
{
	struct switchdev_obj_node_hsr hsr_obj = {
		.obj.orig_dev = hsr_port_dev_get(hsr, HSR_PT_SLAVE_A),
		.obj.id = SWITCHDEV_OBJ_ID_NODE_HSR,
		.obj.flags = SWITCHDEV_F_DEFER,
		.hsr = hsr_port_dev_get(hsr, HSR_PT_MASTER),
	};

	if (!hsr_obj.obj.orig_dev)
		return 0;

	ether_addr_copy(hsr_obj.addr_A, node->macaddress_A);

	return switchdev_port_obj_del(hsr_obj.obj.orig_dev, &hsr_obj.obj);
}
