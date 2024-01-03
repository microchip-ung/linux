/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2021 Microchip Technology Inc. */

#ifndef _LAN966X_VCAP_UTILS_H_
#define _LAN966X_VCAP_UTILS_H_

#include "lan966x_main.h"
#include "vcap_api.h"

/**
 * lan966x_add_prio_is1_rule - Adding IS1 rules on all ports that match on the
 * specified ethertype and generate the specified classified priority.
 * @lan966x: switch device.
 * @user: The VCAP user. Only LAN966X_VCAP_USER_MRP and LAN966X_VCAP_USER_ERPS is supported.
 *
 * A rule ID is returned that must be used when calling lan966x_del_prio_is1_rule()
 * Value '1' is returned if error occurs.
 */
int lan966x_add_prio_is1_rule(struct lan966x_port *port, enum vcap_user user,
			      u32 *rule_id);

/**
 * lan966x_del_prio_is1_rule - Deleting IS1 rules created by lan966x_add_prio_is1_rule()
 * @lan966x: switch device.
 * @rule_id: rule ID is returned by lan966x_add_prio_is1_rule().
 */
void lan966x_del_prio_is1_rule(struct lan966x_port *port, u32 rule_id);

void lan966x_dmac_enable(struct lan966x_port *port, int lookup, bool enable);

#endif
