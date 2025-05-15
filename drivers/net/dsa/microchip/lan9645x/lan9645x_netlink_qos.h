/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef __LAN9645X_NETLINK_QOS_H__
#define __LAN9645X_NETLINK_QOS_H__

#include "lan9645x_main.h"
#include "../../../ethernet/microchip/mchp_ui_qos.h"

#define DSCP_COUNT 64

struct lan9645x_netlink_qos {
	struct lan9645x *lan9645x;
	struct mchp_qos_dscp_prio_dpl dscp_map[DSCP_COUNT];
	struct mchp_qos_port_conf qos_map[NUM_PHYS_PORTS];
};

int lan9645x_qos_port_conf_set(struct lan9645x_netlink_qos *q,
			       struct net_device *dev,
			       struct mchp_qos_port_conf *cfg);
int lan9645x_qos_port_conf_get(struct lan9645x_netlink_qos *q,
			       struct net_device *dev,
			       struct mchp_qos_port_conf *cfg);
int lan9645x_qos_dscp_prio_dpl_set(struct lan9645x_netlink_qos *q, u8 dscp,
				   struct mchp_qos_dscp_prio_dpl *cfg);
int lan9645x_qos_dscp_prio_dpl_get(struct lan9645x_netlink_qos *q, u8 dscp,
				   struct mchp_qos_dscp_prio_dpl *cfg);
int lan9645x_netlink_qos_init(struct lan9645x *lan9645x);
void lan9645x_netlink_qos_uninit(void);

#endif /* __LAN9645X_NETLINK_QOS_H__ */
