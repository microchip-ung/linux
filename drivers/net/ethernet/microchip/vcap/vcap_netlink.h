/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip VCAP API Netlink interface
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#ifndef __VCAP_NETLINK_H__
#define __VCAP_NETLINK_H__

#include "vcap_api_client.h"

int vcap_netlink_init(struct vcap_control *ctrl, struct net_device *ndev);
void vcap_netlink_uninit(void);

#endif /* __VCAP_NETLINK_H__ */
