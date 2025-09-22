/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef __LAN9645X_NETLINK_QOS_H__
#define __LAN9645X_NETLINK_QOS_H__

#include "lan9645x_main.h"
#include "../../../ethernet/microchip/mchp_ui_qos.h"

struct lan9645x_netlink_qos {
	struct lan9645x *lan9645x;
};

int lan9645x_netlink_qos_init(struct lan9645x *lan9645x);
void lan9645x_netlink_qos_uninit(void);

#endif /* __LAN9645X_NETLINK_QOS_H__ */
