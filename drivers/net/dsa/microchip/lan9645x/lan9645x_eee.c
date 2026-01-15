// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

int lan9645x_eee_mac_set(struct lan9645x *lan9645x, int port,
			 struct ethtool_keee *e)
{
	struct phylink *pl = lan9645x_get_phylink(lan9645x, port);
	struct lan9645x_port *p;
	int err;

	p = lan9645x_to_port(lan9645x, port);
	if (!p)
		return -EINVAL;

	if (e->eee_enabled) {
		err = phylink_init_eee(pl, 0);
		if (err)
			return err;

		lan_rmw(DEV_EEE_CFG_EEE_ENA_SET(1) |
			DEV_EEE_CFG_EEE_TIMER_WAKEUP_SET(e->tx_lpi_timer),
			DEV_EEE_CFG_EEE_ENA |
			DEV_EEE_CFG_EEE_TIMER_WAKEUP,
			lan9645x, DEV_EEE_CFG(port));
	} else {
		lan_rmw(DEV_EEE_CFG_EEE_ENA_SET(0),
			DEV_EEE_CFG_EEE_ENA,
			lan9645x, DEV_EEE_CFG(port));
	}

	return 0;
}
