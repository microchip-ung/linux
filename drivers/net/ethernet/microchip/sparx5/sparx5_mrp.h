/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2024 Microchip Technology Inc. */

#ifndef _SPARX5_MRP_H_
#define _SPARX5_MRP_H_

#include <net/switchdev.h>

#include "sparx5_main.h"

int sparx5_mrp_init(struct sparx5 *sparx5);
void sparx5_mrp_deinit(struct sparx5 *sparx5);

void sparx5_mrp_ring_open(struct sparx5 *sparx5);
void sparx5_mrp_in_open(struct sparx5 *sparx5);

void sparx5_mrp_port_update_mrp_mac(struct sparx5_port *port,
				    const u8 mac[ETH_ALEN]);

int sparx5_handle_mrp_port_state(struct sparx5_port *port,
				 enum br_mrp_port_state_type state);
int sparx5_handle_mrp_port_role(struct sparx5_port *port,
				enum br_mrp_port_role_type role);

int sparx5_handle_mrp_add(struct net_device *dev,
			  const struct switchdev_obj *obj);
int sparx5_handle_mrp_del(struct net_device *dev,
			  const struct switchdev_obj *obj);

int sparx5_handle_mrp_ring_test_add(struct net_device *dev,
				    const struct switchdev_obj *obj);
int sparx5_handle_mrp_ring_test_del(struct net_device *dev,
				    const struct switchdev_obj *obj);
int sparx5_handle_mrp_ring_state_add(struct net_device *dev,
				     const struct switchdev_obj *obj);

int sparx5_handle_mrp_in_test_add(struct net_device *dev,
				  const struct switchdev_obj *obj);
int sparx5_handle_mrp_in_test_del(struct net_device *dev,
				  const struct switchdev_obj *obj);
int sparx5_handle_mrp_in_state_add(struct net_device *dev,
				   const struct switchdev_obj *obj);

int sparx5_handle_mrp_ring_role_add(struct net_device *dev,
				    const struct switchdev_obj *obj);
int sparx5_handle_mrp_ring_role_del(struct net_device *dev,
				    const struct switchdev_obj *obj);

int sparx5_handle_mrp_in_role_add(struct net_device *dev,
				  const struct switchdev_obj *obj);
int sparx5_handle_mrp_in_role_del(struct net_device *dev,
				  const struct switchdev_obj *obj);

#endif
