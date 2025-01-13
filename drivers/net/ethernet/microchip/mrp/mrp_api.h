/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2024 Microchip Technology Inc. and its subsidiaries.
 * Microchip MRP API
 */

#ifndef __MRP_API__
#define __MRP_API__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <net/switchdev.h>
#include <uapi/linux/mrp_bridge.h>

#include "afi_api.h"

#define MRP_MAX_PORTS	3

enum mrp_interrupt_status {
	MRP_INTERRUPT_STATUS_NONE,
	MRP_INTERRUPT_STATUS_OPEN,
	MRP_INTERRUPT_STATUS_CLOSED,
};

struct mrp_control {
	/* User data */
	void *priv;

	/* Operations */
	struct mrp_operations *ops;

	struct afi_control *afi_ctrl;

	struct list_head mrps_list;
	u8 loc_period_mask;
};

struct mrp_instance {
	struct list_head list;

	struct mrp_port *ports[MRP_MAX_PORTS];

	struct mrp_control *mrp_ctrl;

	enum br_mrp_ring_role_type ring_role;
	enum br_mrp_ring_state_type ring_state;
	enum br_mrp_in_role_type in_role;
	enum br_mrp_in_state_type in_state;

	bool mra_support;
	bool monitor;
	u32 ring_id;
	u32 in_id;

	u8 ring_loc_idx;
	u8 in_loc_idx;

	u32 ring_transitions;
	u32 in_transitions;

	struct delayed_work ring_loc_work;
	struct delayed_work in_loc_work;

};

struct mrp_port {
	/* User data */
	void *priv;

	struct net_device *dev;

	struct mrp_instance *mrp_inst;

	u32 afi_ring_test_id;
	u32 afi_in_test_id;

	enum br_mrp_port_role_type role;
	enum br_mrp_port_state_type state;

	enum mrp_interrupt_status ring_intr_status;
	enum mrp_interrupt_status in_intr_status;

	u32 ring_max_miss;
	u32 in_max_miss;

	u32 ring_interval;
	u32 in_interval;
};

struct mrp_operations {
	int (*mrp_port_init)(struct mrp_port *mrp_port, u16 prio);
	int (*mrp_port_uninit)(struct mrp_port *mrp_port);
	int (*mrp_port_update_mac)(struct mrp_port *mrp_port);
	int (*mrp_port_update_mrm_mac)(struct mrp_port *mrp_port,
				       const u8 mac[ETH_ALEN]);

	int (*mrp_port_set_ring_state)(struct mrp_port *mrp_port,
				       u32 ring_transitions,
				       enum br_mrp_ring_state_type ring_state);
	int (*mrp_port_set_in_state)(struct mrp_port *mrp_port,
				     u32 in_transitions,
				     enum br_mrp_in_state_type in_state);

	int (*mrp_port_set_port_role)(struct mrp_port *mrp_port,
				      enum br_mrp_port_role_type role);

	int (*mrp_port_redirect_control)(struct mrp_port *mrp_port);

	int (*mrp_port_hijack_test)(struct mrp_port *mrp_port,
				    struct sk_buff *skb);
	int (*mrp_port_afi_cfg)(struct mrp_port *mrp_port,
				struct afi_slow_inj_alloc_cfg *cfg);

	int (*mrp_port_terminate_ring_test)(struct mrp_port *mrp_port);
	int (*mrp_port_redirect_ring_test)(struct mrp_port *mrp_port,
					   bool redirect);
	int (*mrp_port_forward_ring_test)(struct mrp_port *mrp_port,
					  struct mrp_port *mrp_partner_port,
					  bool forward);
	int (*mrp_port_rewrite_ring_test)(struct mrp_port *mrp_port,
					  bool rewrite);
	int (*mrp_port_process_ring_test)(struct mrp_port *mrp_port,
					  bool process);

	enum mrp_interrupt_status (*mrp_port_get_ring_interrupt_status)(struct mrp_port *mrp_port);
	int (*mrp_port_disable_ring_interrupt)(struct mrp_port *mrp_port);
	int (*mrp_port_enable_ring_interrupt)(struct mrp_port *mrp_port,
					      u32 max);

	int (*mrp_port_terminate_in_test)(struct mrp_port *mrp_port);
	int (*mrp_port_forward_in_test)(struct mrp_port *mrp_port,
					struct mrp_port *mrp_partner_port_1,
					struct mrp_port *mrp_partner_port_2,
					bool forward);
	int (*mrp_port_forward_rem_in_test)(struct mrp_port *mrp_port,
					    struct mrp_port *mrp_partner_port,
					    bool forward);
	int (*mrp_port_rewrite_in_test)(struct mrp_port *mrp_port,
					bool rewrite);
	int (*mrp_port_process_in_test)(struct mrp_port *mrp_port,
					bool process);

	enum mrp_interrupt_status (*mrp_port_get_in_interrupt_status)(struct mrp_port *mrp_port);
	int (*mrp_port_disable_in_interrupt)(struct mrp_port *mrp_port);
	int (*mrp_port_enable_in_interrupt)(struct mrp_port *mrp_port, u32 max);
};

int mrp_init(struct mrp_control *mrp_ctrl);
int mrp_deinit(struct mrp_control *mrp_ctrl);

struct mrp_port *mrp_add_port(struct mrp_control *mrp_ctrl,
			      const struct switchdev_obj_mrp *mrp,
			      struct net_device *dev);
int mrp_del_port(struct mrp_control *mrp_ctrl,
		 const struct switchdev_obj_mrp *mrp,
		 struct mrp_port *mrp_port);

struct mrp_port *mrp_add_in_port(struct mrp_control *mrp_ctrl,
				 const struct switchdev_obj_in_role_mrp *mrp,
				 struct net_device *dev);
int mrp_del_in_port(struct mrp_control *mrp_ctrl,
		    const struct switchdev_obj_in_role_mrp *mrp,
		    struct mrp_port *mrp_port);

int mrp_ring_interrupt(struct mrp_control *mrp_ctrl);
int mrp_in_interrupt(struct mrp_control *mrp_ctrl);

int mrp_port_set_ring_state(struct mrp_port *mrp_port,
			    const struct switchdev_obj_ring_state_mrp *mrp);
int mrp_port_set_ring_role(struct mrp_port *mrp_port,
			   const struct switchdev_obj_ring_role_mrp *mrp);
int mrp_port_start_ring_test(struct mrp_port *mrp_port,
			     const struct switchdev_obj_ring_test_mrp *mrp);
int mrp_port_stop_ring_test(struct mrp_port *mrp_port,
			    const struct switchdev_obj_ring_test_mrp *mrp);

int mrp_port_set_in_state(struct mrp_port *mrp_port,
			  const struct switchdev_obj_in_state_mrp *mrp);
int mrp_port_set_in_role(struct mrp_port *mrp_port,
			 const struct switchdev_obj_in_role_mrp *mrp);
int mrp_port_start_in_test(struct mrp_port *mrp_port,
			   const struct switchdev_obj_in_test_mrp *mrp);
int mrp_port_stop_in_test(struct mrp_port *mrp_port,
			  const struct switchdev_obj_in_test_mrp *mrp);

int mrp_port_set_port_role(struct mrp_port *mrp_port,
			   enum br_mrp_port_role_type port_role);

#endif /* __MRP_API__ */
