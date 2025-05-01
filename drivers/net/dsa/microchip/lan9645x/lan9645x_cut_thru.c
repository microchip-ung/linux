// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

void lan9645x_cut_through_fwd(struct lan9645x *lan9645x)
{
	struct lan9645x_port *p;
	u32 tcs, spd;
	int port;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	/* A frame forwarded from port A to port B is cut-thru forwarded if
	 * these conditions are met:
	 *
	 *  - port.CUT_THRU_SPD > 0
	 *  - TC queue bit set in port.CUT_THRU_ENA
	 *  - A.CUT_THRU_SPD <= B.CUT_THRU_SPD
	 *
	 *  TODO: Can handle this with a locel/per-port API instead of modifying
	 *  all ports every time.
	 */
	lan9645x_for_each_port(lan9645x, port, p)
	{
		spd = 0;
		tcs = 0;
		if (p->speed) {
			spd = p->speed + 1;
			tcs = GENMASK(7, 0);
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
