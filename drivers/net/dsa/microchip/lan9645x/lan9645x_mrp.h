/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN9645X_MRP_H_
#define _LAN9645X_MRP_H_

#include <net/switchdev.h>

#include "lan9645x_main.h"

int lan9645x_mrp_init(struct lan9645x *lan9645x);
void lan9645x_mrp_uninit(struct lan9645x *lan9645x);

void lan9645x_mrp_ring_open(struct lan9645x *lan9645x);
void lan9645x_mrp_in_open(struct lan9645x *lan9645x);

void lan9645x_mrp_port_update_mrp_mac(struct lan9645x *lan9645x, int port,
				      const u8 mac[ETH_ALEN]);
int lan9645x_handle_mrp_port_state(struct lan9645x *lan9645x, int port,
				   enum br_mrp_port_state_type state);
int lan9645x_handle_mrp_port_role(struct lan9645x *lan9645x, int port,
				  enum br_mrp_port_role_type role);
int lan9645x_handle_mrp_add_port(struct lan9645x *lan9645x, int port,
				 const struct switchdev_obj_mrp *mrp);
int lan9645x_handle_mrp_del_port(struct lan9645x *lan9645x, int port,
				 const struct switchdev_obj_mrp *mrp);
int lan9645x_handle_mrp_ring_test_add(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_test_mrp *mrp);
int lan9645x_handle_mrp_ring_test_del(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_test_mrp *mrp);
int lan9645x_handle_mrp_ring_state_add(struct lan9645x *lan9645x, int port,
				       const struct switchdev_obj_ring_state_mrp *mrp);
int lan9645x_handle_mrp_in_test_add(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_test_mrp *mrp);
int lan9645x_handle_mrp_in_test_del(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_test_mrp *mrp);
int lan9645x_handle_mrp_in_state_add(struct lan9645x *lan9645x, int port,
				     const struct switchdev_obj_in_state_mrp *mrp);
int lan9645x_handle_mrp_ring_role_add(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_role_mrp *mrp);
int lan9645x_handle_mrp_ring_role_del(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_role_mrp *mrp);
int lan9645x_handle_mrp_in_role_add(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_role_mrp *mrp);
int lan9645x_handle_mrp_in_role_del(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_role_mrp *mrp);

#endif
