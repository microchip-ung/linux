// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */

#include "sparx5_main_regs.h"
#include "sparx5_main.h"

#define EAPOL_MASK         0xc0 /* Maps to 01:80:C2:00:00:03 in BPDU register */
#define EAPOL_FORWARD      FIELD_PREP(EAPOL_MASK, 0)
#define EAPOL_TO_CPU       FIELD_PREP(EAPOL_MASK, 1)

void sparx5_psec_set(struct sparx5_port *port, bool enable)
{
	struct sparx5_mact_entry *mact_entry, *tmp;
	struct sparx5 *sparx5 = port->sparx5;

	if (enable)
		set_bit(port->portno, sparx5->bridge_psec_mask);
	else
		clear_bit(port->portno, sparx5->bridge_psec_mask);

	/* Ensure EAPOL frames (multicast address 01:80:C2:00:00:03) are always
	 * trapped to the CPU, even when the port is locked and unknown source
	 * MACs are dropped. This allows user space to handle 802.1X
	 * authentication.
	 */
	spx5_rmw(enable ? EAPOL_TO_CPU : EAPOL_FORWARD,
		 EAPOL_MASK,
		 sparx5,
		 ANA_CL_CAPTURE_BPDU_CFG(port->portno));

	/* Recalculate forwarding masks to reflect any changes in locked
	 * ports.
	 */
	sparx5_update_fwd(sparx5);

	/* Flush all FDB entries (both static and learned) for this port.
	 * This prevents previously installed MAC addresses from bypassing
	 * port security after it has been (re-)enabled.
	 */
	mutex_lock(&sparx5->mact_lock);
	list_for_each_entry_safe(mact_entry, tmp, &sparx5->mact_entries, list) {
		if (mact_entry->port == port->portno) {
			sparx5_mact_forget(sparx5, mact_entry->mac,
					   mact_entry->vid);
		}
	}
	mutex_unlock(&sparx5->mact_lock);
}
