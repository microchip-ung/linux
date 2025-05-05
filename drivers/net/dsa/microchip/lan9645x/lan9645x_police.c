// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

/* 0-9 : 10 port policers */
#define POL_IDX_PORT	0

/* Policer order: Serial (QoS -> Port -> VCAP) */
#define POL_ORDER	0x1d3

int lan9645x_police_add(struct lan9645x_port *p,
			struct lan9645x_policer *pol, int pol_idx)
{
	struct lan9645x *lan9645x = p->lan9645x;
	u32 rate, burst;

	/* Rate unit is 33 1/3 kpps */
	rate = DIV_ROUND_UP(pol->rate * 3, 100);
	burst = DIV_ROUND_UP(pol->burst ? pol->burst : 1, 4096);

	/* TODO: What if burst is < MTU? */

	if (!FIELD_FIT(ANA_POL_PIR_CFG_PIR_RATE, rate) ||
	    !FIELD_FIT(ANA_POL_PIR_CFG_PIR_BURST, burst))
		return -EINVAL;

	lan_wr(ANA_POL_MODE_DROP_ON_YELLOW_ENA_SET(0) |
	       ANA_POL_MODE_MARK_ALL_FRMS_RED_ENA_SET(0) |
	       ANA_POL_MODE_IPG_SIZE_SET(20) |
	       ANA_POL_MODE_FRM_MODE_SET(1) |
	       ANA_POL_MODE_OVERSHOOT_ENA_SET(1),
	       lan9645x, ANA_POL_MODE(pol_idx));

	lan_wr(ANA_POL_PIR_STATE_PIR_LVL_SET(0), lan9645x,
	       ANA_POL_PIR_STATE(pol_idx));

	lan_wr(ANA_POL_PIR_CFG_PIR_RATE_SET(rate) |
	       ANA_POL_PIR_CFG_PIR_BURST_SET(burst),
	       lan9645x, ANA_POL_PIR_CFG(pol_idx));

	return 0;
}

void lan9645x_police_del(struct lan9645x *lan9645x, u16 pol_idx)
{
	lan_wr(ANA_POL_MODE_DROP_ON_YELLOW_ENA_SET(0) |
	       ANA_POL_MODE_MARK_ALL_FRMS_RED_ENA_SET(0) |
	       ANA_POL_MODE_IPG_SIZE_SET(20) |
	       ANA_POL_MODE_FRM_MODE_SET(2) |
	       ANA_POL_MODE_OVERSHOOT_ENA_SET(1),
	       lan9645x, ANA_POL_MODE(pol_idx));

	lan_wr(ANA_POL_PIR_STATE_PIR_LVL_SET(0),
	       lan9645x, ANA_POL_PIR_STATE(pol_idx));

	lan_wr(ANA_POL_PIR_CFG_PIR_RATE_SET(GENMASK(14, 0)) |
	       ANA_POL_PIR_CFG_PIR_BURST_SET(0),
	       lan9645x, ANA_POL_PIR_CFG(pol_idx));
}

int lan9645x_police_port_add(struct lan9645x *lan9645x, int port,
			     struct lan9645x_policer *pol)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	int err;

	err = lan9645x_police_add(p, pol, POL_IDX_PORT + p->chip_port);
	if (err) {
		NL_SET_ERR_MSG_MOD(NULL, "Failed to add policer to port");
		return err;
	}

	lan_rmw(ANA_POL_CFG_PORT_POL_ENA_SET(1),
		ANA_POL_CFG_PORT_POL_ENA,
		lan9645x, ANA_POL_CFG(p->chip_port));

	return 0;
}

void lan9645x_police_port_del(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	lan9645x_police_del(lan9645x, POL_IDX_PORT + p->chip_port);

	lan_rmw(ANA_POL_CFG_PORT_POL_ENA_SET(0),
		ANA_POL_CFG_PORT_POL_ENA,
		lan9645x, ANA_POL_CFG(p->chip_port));
}

void lan9645x_police_port_init(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;

	lan_rmw(ANA_POL_CFG_POL_ORDER_SET(POL_ORDER) |
		ANA_POL_CFG_POL_CPU_REDIR_8021_SET(1) |
		ANA_POL_CFG_POL_CPU_REDIR_IP_SET(1),
		ANA_POL_CFG_POL_ORDER |
		ANA_POL_CFG_POL_CPU_REDIR_8021 |
		ANA_POL_CFG_POL_CPU_REDIR_IP,
		lan9645x, ANA_POL_CFG(p->chip_port));
}
