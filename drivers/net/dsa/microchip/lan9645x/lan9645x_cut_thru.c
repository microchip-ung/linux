// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

void lan9645x_cut_through_fwd(struct lan9645x *lan9645x)
{
	struct lan9645x_port *p;
	int max_speed = 0;
	u32 tcs, spd;
	int port;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	lan9645x_for_each_port(lan9645x, port, p) {
		if (p->cut_thru_ena &&
		    (lan9645x_port_is_bridged(p) || lan9645x_port_is_hsr(p)))
			max_speed = p->speed > max_speed ? p->speed : max_speed;
	}

	if (max_speed == 0)
		return;

	/* A frame forwarded from port A to port B is cut-thru forwarded if
	 * these conditions are met:
	 *
	 *  - port.CUT_THRU_SPD > 0
	 *  - TC queue bit set in port.CUT_THRU_ENA
	 *  - A.CUT_THRU_SPD <= B.CUT_THRU_SPD
	 *
	 * However, due to a bug in the hw, we can not cut-thru from A to B,
	 * unless they run the same speed. So we have to restrict cut-thru
	 * to the fastest ports in the forwarding domain.
	 */
	lan9645x_for_each_port(lan9645x, port, p)
	{
		spd = 0;
		tcs = 0;

		if (!(p->cut_thru_ena &&
		      p->speed > 0 &&
		      (lan9645x_port_is_bridged(p) || lan9645x_port_is_hsr(p))))
			continue;

		/* Only enable cut-through for the fastest ports in the
		 * forwarding domain.
		 * We must exclude queues with PFC enabled.
		 */
		if (max_speed > 0 && p->speed == max_speed) {
			spd = p->speed + 1;
			tcs = GENMASK(7, 0) & ~p->qos.pfc_enable;
		}

		/* For HSR the chip needs to know the LSDU size before it can
		 * write the tag in the header. Therefore, we must not enable
		 * cut through from CPU and NPI ports.
		 */
		if (lan9645x->npi == port)
			spd = 0;

		/* TODO: Must remove traffic classes that have oversized dropped
		 * enabled (TAS feature), and preemtiple traffic classes, once
		 * tsn features are implemented.
		 */
		lan_wr(ANA_CUT_THRU_CFG_CUT_THRU_SPD_SET(spd) |
		       ANA_CUT_THRU_CFG_CUT_THRU_ENA_SET(tcs),
		       lan9645x, ANA_CUT_THRU_CFG(port));
	}
}
