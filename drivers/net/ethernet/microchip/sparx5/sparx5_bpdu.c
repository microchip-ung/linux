// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */

#include "sparx5_main.h"

/*
 * Reserved link-local BPDU / control protocol destination MAC addresses
 * in the range 01:80:C2:00:00:0X, as defined by IEEE 802.1 standards.
 *
 * These frames are never forwarded by bridges and are intended to be
 * consumed by the local control plane.
 *
 * ANA_CL_CAPTURE_BPDU_CFG provides a 2-bit action field per address slot:
 *
 *   bits 0–1   -> 01:80:C2:00:00:00  (slot ::00)
 *   bits 2–3   -> 01:80:C2:00:00:01  (slot ::01)
 *   bits 4–5   -> 01:80:C2:00:00:02  (slot ::02)
 *   ...
 *
 * Well-known assignments:
 *
 *   ::00  01:80:C2:00:00:00  STP / RSTP / MSTP (IEEE 802.1D / 802.1Q)
 *   ::01  01:80:C2:00:00:01  PAUSE (IEEE 802.3x)
 *   ::02  01:80:C2:00:00:02  LACP (IEEE 802.1AX / 802.3ad)
 *   ::03  01:80:C2:00:00:03  Reserved (historical / vendor use)
 *   ::04–::0D                Reserved by IEEE
 *   ::0E  01:80:C2:00:00:0E  LLDP (IEEE 802.1AB, ethertype 0x88cc)
 *   ::0F  01:80:C2:00:00:0F  Reserved
 *
 * Action encoding per 2-bit field:
 *   00b = forward (no special handling)
 *   01b = redirect to CPU
 *   10b = copy to CPU
 *   11b = discard
 */

#define BPDU_FORWARD	0x0
#define BPDU_REDIR	0x1
#define BPDU_COPY	0x2
#define BPDU_DISCARD	0x3

#define BPDU_MASK	0x3

enum sparx5_bpdu_type {
	BPDU_LACP,
	BPDU_MAX,
};

static const u32 bpdu_shift[BPDU_MAX] = {
	[BPDU_LACP] = 4, /* 01:80:C2:00:00:02  LACP (IEEE 802.1AX / 802.3ad) */
};

static void sparx5_bpdu_trap(struct sparx5_port *port,
			     enum sparx5_bpdu_type type, u32 action)
{
	u32 pos = bpdu_shift[type];

	spx5_rmw(action << pos, BPDU_MASK << pos, port->sparx5,
		 ANA_CL_CAPTURE_BPDU_CFG(port->portno));
}

void sparx5_bpdu_lacp_redir(struct sparx5_port *port)
{
	sparx5_bpdu_trap(port, BPDU_LACP, BPDU_REDIR);
}
