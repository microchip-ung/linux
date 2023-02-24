/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN966X_MRP_H_
#define _LAN966X_MRP_H_

#include <net/switchdev.h>

#include "lan966x_main.h"

void lan966x_mrp_init(struct lan966x *lan966x);
void lan966x_mrp_uninit(struct lan966x *lan966x);
void lan966x_mrp_update_mac(struct lan966x *lan966x, const u8 mac[ETH_ALEN]);

void lan966x_mrp_ring_open(struct lan966x *lan966x);
void lan966x_mrp_in_open(struct lan966x *lan966x);

int lan966x_handle_mrp_port_state(struct lan966x_port *port,
				  enum br_mrp_port_state_type state);
int lan966x_handle_mrp_port_role(struct lan966x_port *port,
				 enum br_mrp_port_role_type role);

int lan966x_handle_mrp_add(struct lan966x_port *port,
			   const struct switchdev_obj *obj);
int lan966x_handle_mrp_del(struct lan966x_port *port,
			   const struct switchdev_obj *obj);

int lan966x_handle_mrp_ring_test_add(struct lan966x_port *port,
				     const struct switchdev_obj *obj);
int lan966x_handle_mrp_ring_test_del(struct lan966x_port *port,
				     const struct switchdev_obj *obj);
int lan966x_handle_mrp_ring_state_add(struct lan966x_port *port,
				      const struct switchdev_obj *obj);

int lan966x_handle_mrp_in_test_add(struct lan966x_port *port,
				   const struct switchdev_obj *obj);
int lan966x_handle_mrp_in_test_del(struct lan966x_port *port,
				   const struct switchdev_obj *obj);
int lan966x_handle_mrp_in_state_add(struct lan966x_port *port,
				    const struct switchdev_obj *obj);

int lan966x_handle_mrp_ring_role_add(struct lan966x_port *port,
				     const struct switchdev_obj *obj);
int lan966x_handle_mrp_ring_role_del(struct lan966x_port *port,
				     const struct switchdev_obj *obj);

int lan966x_handle_mrp_in_role_add(struct lan966x_port *port,
				   const struct switchdev_obj *obj);
int lan966x_handle_mrp_in_role_del(struct lan966x_port *port,
				   const struct switchdev_obj *obj);

#endif
