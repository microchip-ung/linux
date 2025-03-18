// SPDX-License-Identifier: GPL-2.0+
/* Microchip lan969x Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */

/* Single RedBox setup:
 *
 * The redbox forwards frames on lrea, lreb and lrec
 * (Link Redundancy Entity, ports A, B and C), using a number of forwarding
 * masks, selected based on host table lookups. Everything forwarded to lrec
 * and into the switch core is subject to normal switch forwarding. The redbox
 * to SAN, and vice-versa, traffic, is isolated from the rest of theports, using
 * source port masks.
 *
 * When operating in DANH or DANP mode, no frames are forwarded to the switch
 * core via the interlink. Supervision frames are redirected to the CPU.
 *
 * Port A and port B ports must be physically attached to the same taxi bus.
 *
 *         lrec
 *          |
 * +-----------------+
 * |   Switch core   |
 * +-----------------+
 *          | Interlink (lrec)
 * +----------------+
 * |     RedBox     |
 * +----------------+
 *     |       |
 *   lrea    lreb
 */

#include "lan969x.h"

#define RB_NETID 5 /* Default netid. */

/* Table timeouts. */
#define RB_TABLE_SLEEP_US 10
#define RB_TABLE_TIMEOUT_US 100000

/* Commands for configuring and querying the host- and discard table. */
#define RB_TABLE_CMD_LEARN 0
#define RB_TABLE_CMD_UNLEARN 1
#define RB_TABLE_CMD_LOOKUP 2
#define RB_TABLE_CMD_READ 3
#define RB_TABLE_CMD_WRITE 4
#define RB_TABLE_CMD_CLEAR 7

/* Number of host- and discard table entries. */
#define RB_HTABLE_CNT 4096
#define RB_DTABLE_CNT 2048

/* HSR / PRP standard modes. */
#define RB_MODE_PRP_SAN 0
#define RB_MODE_HSR_SAN 1

/* Host entry type. */
#define RB_HT_PROXY 0
#define RB_HT_DAN 1
#define RB_HT_SAN 2
#define RB_HT_LOCAL 3
#define RB_HT_NONE 4

/* Tag modes. */
#define RB_TAG_NONE 0
#define RB_TAG_PRP_NONE 1
#define RB_TAG_HSR 2
#define RB_TAG_PRP 3

/* HSR tag filter. */
#define RB_FLT_NONE 0
#define RB_FLT_HSR 1
#define RB_FLT_NOT_HSR 2
#define RB_FLT_REDIR 3

/* Supervision frame forwarding. */
#define RB_SV_FORWARD 0
#define RB_SV_CPU_COPY 1
#define RB_SV_CPU_ONLY 2
#define RB_SV_DISCARD 3

/* Forwarding masks */
#define RB_FWD_NONE 0x0 /* Do not foward. */
#define RB_FWD_A 0x1 /* Forward to LREA. */
#define RB_FWD_B 0x2 /* Forward to LREB. */
#define RB_FWD_AB 0x3 /* Forward to LREA, LREB. */
#define RB_FWD_C 0x4 /* Forward to LREC. */
#define RB_FWD_AC 0x5 /* Forward to LREA, LREC. */
#define RB_FWD_BC 0x6 /* Forward to LREB, LREC. */
#define RB_FWD_ALL 0x7 /* Forward to LREA, LREB, LREC. */

#define RB_FWD_SEL_NO_CHANGE 0
#define RB_FWD_SEL_COPY_CPU 1
#define RB_FWD_SEL_REDIR_CPU 2
#define RB_FWD_SEL_DISCARD 3

static u32 debugfs_redbox_idx;

enum {
	LAN969X_RB_LREA,
	LAN969X_RB_LREB,
	LAN969X_RB_LREC,
	LAN969X_RB_PORT_MAX,
};

enum {
	PROXY_SRC_FWD_MASK,
	LOCAL_SRC_FWD_MASK,
	NODE_SRC_FWD_MASK,
	FLD_DST_FWD_MASK,
	PROXY_DST_FWD_MASK,
	LOCAL_DST_FWD_MASK,
	NODE_DST_FWD_MASK,
	FWD_MASK_MAX,
};

struct lan969x_rb_htable_port {
	u8 age;
	u8 fwd;
	u8 rct;
	u32 rx;
	u32 rx_wrong_lan;
};

struct lan969x_rb_htable_node {
	u8 idx;
	unsigned char mac[ETH_ALEN];
	bool locked;
	bool valid;
	u8 age_interval;
	u8 type;
	bool pdan;
	u16 seq_no;
	struct lan969x_rb_htable_port ports[3];
};

struct lan969x_rb_dtable_port {
	u8 n_disc;
};

struct lan969x_rb_dtable_node {
	unsigned char mac[ETH_ALEN];
	u16 seq_no;
	u8 age;
	struct lan969x_rb_dtable_port ports[3];
};

struct lan969x_redbox_params {
	/* Egress port tag mode. */
	u8 tag;

	/* Acceptance filtering of HSR-tagged frames. */
	u8 filter;

	/* HSR tags are detected and used. */
	bool hsr;

	/* PRP tags are detected and used. */
	bool prp;

	/* ID of originating port. */
	u8 lan_id;

	/* Netid. */
	u8 net_id;

	/* Host type. */
	u8 host_type;

	/* Forwarding masks. */
	u8 masks[FWD_MASK_MAX];

	/* Forward selector for supervision frames. */
	u8 spv_sel;
};

struct lan969x_redbox {
	/* HSR master. */
	struct net_device *hsr_master;

	/* LREA, LREB, LREC. */
	struct sparx5_port *ports[3];

	/* Pointer to port params for this redbox instance. */
	struct lan969x_redbox_params *params;

	/* Overall mode of operation for redbox. */
	u8 mode;

	 /* Taxi instance in hardware, maps 1:1 to redbox instance. */
	u8 taxi;
};

static struct lan969x_redbox_params hsr_san_params[LAN969X_RB_PORT_MAX] = {
	[LAN969X_RB_LREA]  = {
		.hsr       = true,
		.lan_id    = 0,
		.net_id    = RB_NETID,
		.tag       = RB_TAG_HSR,
		.host_type = RB_HT_DAN,
		.filter    = RB_FLT_NOT_HSR,
		/* Supervision frames are subject to forwarding. Copy to CPU
		 * also.
		 */
		.spv_sel   = RB_FWD_SEL_COPY_CPU,
		.masks     = {
			[PROXY_SRC_FWD_MASK] = RB_FWD_NONE,
			[LOCAL_SRC_FWD_MASK] = RB_FWD_NONE,
			[NODE_SRC_FWD_MASK]  = RB_FWD_BC,
			[FLD_DST_FWD_MASK]   = RB_FWD_BC,
			[PROXY_DST_FWD_MASK] = RB_FWD_C,
			[LOCAL_DST_FWD_MASK] = RB_FWD_C,
			[NODE_DST_FWD_MASK]  = RB_FWD_B,
		},
	},
	[LAN969X_RB_LREB]  = {
		.hsr       = true,
		.lan_id    = 1,
		.net_id    = RB_NETID,
		.tag       = RB_TAG_HSR,
		.host_type = RB_HT_DAN,
		.filter    = RB_FLT_NOT_HSR,
		/* Supervision frames are subject to forwarding. Copy to CPU
		 * also.
		 */
		.spv_sel   = RB_FWD_SEL_COPY_CPU,
		.masks     = {
			[PROXY_SRC_FWD_MASK] = RB_FWD_NONE,
			[LOCAL_SRC_FWD_MASK] = RB_FWD_NONE,
			[NODE_SRC_FWD_MASK]  = RB_FWD_AC,
			[FLD_DST_FWD_MASK]   = RB_FWD_AC,
			[PROXY_DST_FWD_MASK] = RB_FWD_C,
			[LOCAL_DST_FWD_MASK] = RB_FWD_C,
			[NODE_DST_FWD_MASK]  = RB_FWD_A,
		},
	},
	[LAN969X_RB_LREC]  = {
		.hsr       = false,
		.tag       = RB_TAG_NONE,
		.host_type = RB_HT_PROXY,
		.filter    = RB_FLT_HSR,
		/* Supervision frames are subject to forwarding. Copy to CPU
		 * also.
		 */
		.spv_sel   = RB_FWD_SEL_COPY_CPU,
		.masks     = {
			[PROXY_SRC_FWD_MASK] = RB_FWD_AB,
			[LOCAL_SRC_FWD_MASK] = RB_FWD_AB,
			[NODE_SRC_FWD_MASK]  = RB_FWD_NONE,
			[FLD_DST_FWD_MASK]   = RB_FWD_AB,
			[PROXY_DST_FWD_MASK] = RB_FWD_NONE,
			[LOCAL_DST_FWD_MASK] = RB_FWD_NONE,
			[NODE_DST_FWD_MASK]  = RB_FWD_AB,
		},
	},
};

static struct lan969x_redbox_params danh_params[LAN969X_RB_PORT_MAX] = {
	[LAN969X_RB_LREA]  = {
		.hsr       = true,
		.lan_id    = 0,
		.net_id    = RB_NETID,
		.tag       = RB_TAG_HSR,
		.host_type = RB_HT_DAN,
		.filter    = RB_FLT_NOT_HSR,
		/* Supervision frames are subject to forwarding. Copy to CPU
		 * also.
		 */
		.spv_sel   = RB_FWD_SEL_COPY_CPU,
		.masks     = {
			/* Frames from/to node and frames to unknown destinations
			 * are allowed on B.
			 */
			[NODE_SRC_FWD_MASK]  = RB_FWD_B,
			[FLD_DST_FWD_MASK]   = RB_FWD_B,
			[NODE_DST_FWD_MASK]  = RB_FWD_B,
		},
	},
	[LAN969X_RB_LREB]  = {
		.hsr       = true,
		.lan_id    = 1,
		.net_id    = RB_NETID,
		.tag       = RB_TAG_HSR,
		.host_type = RB_HT_DAN,
		.filter    = RB_FLT_NOT_HSR,
		/* Supervision frames are subject to forwarding. Copy to CPU
		 * also.
		 */
		.spv_sel   = RB_FWD_SEL_COPY_CPU,
		.masks     = {
			/* Frames from/to node and frames to unknown destinations
			 * are allowed on A.
			 */
			[NODE_SRC_FWD_MASK]  = RB_FWD_A,
			[FLD_DST_FWD_MASK]   = RB_FWD_A,
			[NODE_DST_FWD_MASK]  = RB_FWD_A,
		},
	},
	[LAN969X_RB_LREC]  = {
		.tag       = RB_TAG_NONE,
		.host_type = RB_HT_PROXY,
		.filter    = RB_FLT_HSR,
		/* Supervision frames are subject to forwarding. Copy to CPU
		 * also.
		 */
		.spv_sel   = RB_FWD_SEL_COPY_CPU,
		.masks     = {
			/* Frames to node, frames to unknown destinations, and
			 * frames from node listed as local, are allowed on AB.
			 */
			[LOCAL_SRC_FWD_MASK] = RB_FWD_AB,
			[FLD_DST_FWD_MASK]   = RB_FWD_AB,
			[NODE_DST_FWD_MASK]  = RB_FWD_AB,
		},
	},
};

static struct lan969x_redbox_params danp_params[LAN969X_RB_PORT_MAX] = {
	[LAN969X_RB_LREA] = {
		.prp       = true,
		.lan_id    = 0,
		.net_id    = RB_NETID,
		.tag       = RB_TAG_PRP_NONE,
		.host_type = RB_HT_DAN,
		/* Supervision frames are not subject to forwarding. Redirect to
		 * CPU.
		 */
		.spv_sel   = RB_FWD_SEL_REDIR_CPU,
		.masks     = {
			/* No ingress frame forwarding on A. Frames from/to
			 * destinations listes as local, are redirected to CPU.
			 */
			0,
		}
	},
	[LAN969X_RB_LREB] = {
		.prp       = true,
		.lan_id    = 1,
		.net_id    = RB_NETID,
		.tag       = RB_TAG_PRP_NONE,
		.host_type = RB_HT_DAN,
		/* Supervision frames are not subject to forwarding. Redirect to
		 * CPU.
		 */
		.spv_sel   = RB_FWD_SEL_REDIR_CPU,

		.masks     = {
			/* No ingress frame forwarding on B. Frames from/to
			 * destinations listes as local, are redirected to CPU.
			 */
			 0,
		}
	},
	[LAN969X_RB_LREC] = {
		.tag       = RB_TAG_NONE,
		/* Supervision frames are not subject to forwarding. Redirect to
		 * CPU.
		 */
		.spv_sel   = RB_FWD_SEL_REDIR_CPU,
		.masks     = {
			/* CPU injected frames from node listed as local, and to
			 * unknown destinations, are allowed on AB.
			 */
			[LOCAL_SRC_FWD_MASK] = RB_FWD_AB,
			[FLD_DST_FWD_MASK]   = RB_FWD_AB,
		}
	},
};

static struct lan969x_redbox redboxes[LAN969X_RB_REDBOX_CNT];

static void lan969x_rb_add_port(struct lan969x_redbox *redbox,
				struct sparx5_port *port, u8 type)
{
	redbox->ports[type] = port;
}

static void lan969x_rb_add_lrea(struct lan969x_redbox *redbox,
				struct sparx5_port *port)
{
	return lan969x_rb_add_port(redbox, port, LAN969X_RB_LREA);
}

static void lan969x_rb_add_lreb(struct lan969x_redbox *redbox,
				struct sparx5_port *port)
{
	return lan969x_rb_add_port(redbox, port, LAN969X_RB_LREB);
}

static void lan969x_rb_add_lrec(struct lan969x_redbox *redbox,
				struct sparx5_port *port)
{
	return lan969x_rb_add_port(redbox, port, LAN969X_RB_LREC);
}

static void lan969x_rb_del_port(struct lan969x_redbox *redbox, u8 type)
{
	redbox->ports[type] = NULL;
}

static void lan969x_rb_del_lrea(struct lan969x_redbox *redbox)
{
	return lan969x_rb_del_port(redbox, LAN969X_RB_LREA);
}

static void lan969x_rb_del_lreb(struct lan969x_redbox *redbox)
{
	return lan969x_rb_del_port(redbox, LAN969X_RB_LREB);
}

static void lan969x_rb_del_lrec(struct lan969x_redbox *redbox)
{
	return lan969x_rb_del_port(redbox, LAN969X_RB_LREC);
}

static bool lan969x_rb_has_port(struct lan969x_redbox *redbox, u8 idx)
{
	return redbox->ports[idx];
}

static bool lan969x_rb_has_lrea(struct lan969x_redbox *redbox)
{
	return lan969x_rb_has_port(redbox, LAN969X_RB_LREA);
}

static bool lan969x_rb_has_lreb(struct lan969x_redbox *redbox)
{
	return lan969x_rb_has_port(redbox, LAN969X_RB_LREB);
}

static bool lan969x_rb_has_lrec(struct lan969x_redbox *redbox)
{
	return lan969x_rb_has_port(redbox, LAN969X_RB_LREC);
}

static int lan969x_rb_port_taxi_get(struct sparx5_port *port, u8 *taxi_port,
				    u8 *taxi_idx)
{
	for (int i = 0; i < LAN969X_RB_REDBOX_CNT; i++) {
		u32 *taxi_ports = port->sparx5->data->ops.get_taxi(i);

		for (int j = 0; j < LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI; j++) {
			if (taxi_ports[j] == port->portno) {
				*taxi_port = j;
				*taxi_idx = i;
				return 0;
			}
		}
	}

	/* Should not happen. */
	return -EINVAL;
}

static int lan969x_rb_mode_get(enum hsr_version version, u8 *mode)
{
	switch (version) {
	case HSR_V0:
	case HSR_V1:
		*mode = RB_MODE_HSR_SAN;
		break;
	case PRP_V1:
		*mode = RB_MODE_PRP_SAN;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static void lan969x_rb_port_params_get(u8 mode, struct lan969x_redbox *rb)
{
	switch (mode) {
	case RB_MODE_HSR_SAN:
		if (!lan969x_rb_has_lrec(rb))
			rb->params = danh_params;
		else
			rb->params = hsr_san_params;
		break;
	case RB_MODE_PRP_SAN:
		rb->params = danp_params;
		break;
	}
}

static void lan969x_rb_fcs_upd_set(struct sparx5 *sparx5, u8 port, bool enable)
{
	spx5_rmw(ANA_CL_FILTER_CTRL_FORCE_FCS_UPDATE_ENA_SET(enable),
		 ANA_CL_FILTER_CTRL_FORCE_FCS_UPDATE_ENA,
		 sparx5, ANA_CL_FILTER_CTRL(port));
}

/* Enable ERI extraction to interlink port (via lrea port). */
static void lan969x_rb_eri_set(struct sparx5 *sparx5, u8 port, bool enable)
{
	spx5_rmw(ASM_PORT_CFG_RB_ENA_SET(enable),
		 ASM_PORT_CFG_RB_ENA,
		 sparx5, ASM_PORT_CFG(port));
}

/* Connect LREA and LREB to front ports (by means of taxi ports). */
static void lan969x_rb_taxi_ports_set(struct sparx5 *sparx5, u8 lrea_taxi_port,
				      u8 lreb_taxi_port, u8 taxi_bus)
{
	spx5_rmw(RB_TAXI_IF_CFG_LREA_NEXT_SET(0) |
		 RB_TAXI_IF_CFG_LREB_NEXT_SET(0) |
		 RB_TAXI_IF_CFG_LREA_PORT_NO_SET(lrea_taxi_port) |
		 RB_TAXI_IF_CFG_LREB_PORT_NO_SET(lreb_taxi_port),
		 RB_TAXI_IF_CFG_LREA_NEXT | RB_TAXI_IF_CFG_LREB_NEXT |
		 RB_TAXI_IF_CFG_LREA_PORT_NO |
		 RB_TAXI_IF_CFG_LREB_PORT_NO,
		 sparx5, RB_TAXI_IF_CFG(taxi_bus));
}

static void lan969x_rb_port_hsr_filter_set(struct sparx5 *sparx5, u8 rb,
					   u8 port, u8 filter)
{
	spx5_rmw(RB_PORT_CFG_HSR_FILTER_CFG_SET(filter),
		 RB_PORT_CFG_HSR_FILTER_CFG, sparx5,
		 RB_PORT_CFG(rb, port));
}

static void lan969x_rb_port_hsr_aware_set(struct sparx5 *sparx5, u8 rb,
					  u8 port, bool hsr)
{
	spx5_rmw(RB_PORT_CFG_HSR_AWARE_ENA_SET(hsr),
		 RB_PORT_CFG_HSR_AWARE_ENA, sparx5,
		 RB_PORT_CFG(rb, port));
}

static void lan969x_rb_port_prp_aware_set(struct sparx5 *sparx5, u8 rb,
					  u8 port, bool prp)
{
	spx5_rmw(RB_PORT_CFG_PRP_AWARE_ENA_SET(prp),
		 RB_PORT_CFG_PRP_AWARE_ENA, sparx5,
		 RB_PORT_CFG(rb, port));
}

static void lan969x_rb_port_tag_mode_set(struct sparx5 *sparx5, u8 rb,
					 u8 port, u8 mode)
{
	spx5_rmw(RB_PORT_CFG_TAG_MODE_SET(mode),
		 RB_PORT_CFG_TAG_MODE, sparx5,
		 RB_PORT_CFG(rb, port));
}

static void lan969x_rb_port_forward_masks_set(struct sparx5 *sparx5, u8 rb,
					      u8 port, struct lan969x_redbox_params *params)
{
	u8 *masks = params->masks;

	spx5_rmw(RB_FWD_CFG_PROXY_DST_FWD_MASK_SET(masks[PROXY_DST_FWD_MASK]) |
		 RB_FWD_CFG_PROXY_SRC_FWD_MASK_SET(masks[PROXY_SRC_FWD_MASK]) |
		 RB_FWD_CFG_NODE_DST_FWD_MASK_SET(masks[NODE_DST_FWD_MASK]) |
		 RB_FWD_CFG_NODE_SRC_FWD_MASK_SET(masks[NODE_SRC_FWD_MASK]) |
		 RB_FWD_CFG_LOCAL_SRC_FWD_MASK_SET(masks[LOCAL_SRC_FWD_MASK]) |
		 RB_FWD_CFG_LOCAL_DST_FWD_MASK_SET(masks[LOCAL_DST_FWD_MASK]) |
		 RB_FWD_CFG_FLD_DST_FWD_MASK_SET(masks[FLD_DST_FWD_MASK]),
		 RB_FWD_CFG_PROXY_DST_FWD_MASK |
		 RB_FWD_CFG_PROXY_SRC_FWD_MASK |
		 RB_FWD_CFG_NODE_DST_FWD_MASK |
		 RB_FWD_CFG_NODE_SRC_FWD_MASK |
		 RB_FWD_CFG_LOCAL_SRC_FWD_MASK |
		 RB_FWD_CFG_LOCAL_DST_FWD_MASK |
		 RB_FWD_CFG_FLD_DST_FWD_MASK,
		 sparx5,
		 RB_FWD_CFG(rb, port));
}

static void lan969x_rb_port_lanid_set(struct sparx5 *sparx5, u8 rb, u8 port,
				      u8 lanid)
{
	spx5_rmw(RB_PORT_CFG_LANID_SET(lanid) |
		 RB_PORT_CFG_RING_LANID_SET(lanid),
		 RB_PORT_CFG_LANID |
		 RB_PORT_CFG_RING_LANID,
		 sparx5, RB_PORT_CFG(rb, port));
}

static void lan969x_rb_port_netid_set(struct sparx5 *sparx5, u8 rb, u8 port,
				      u8 netid)
{
	spx5_rmw(RB_PORT_CFG_NETID_SET(netid),
		 RB_PORT_CFG_NETID,
		 sparx5, RB_PORT_CFG(rb, port));
}

static void lan969x_rb_port_hsr_spv_fwd_set(struct sparx5 *sparx5, u8 rb,
					    u8 port, u8 sel)
{
	spx5_rmw(RB_PORT_CFG_HSR_SPV_FWD_SEL_SET(sel),
		 RB_PORT_CFG_HSR_SPV_FWD_SEL,
		 sparx5, RB_PORT_CFG(rb, port));
}

static void lan969x_rb_ena(struct sparx5 *sparx5, u8 rb, bool enable)
{
	spx5_rmw(RB_RB_CFG_RB_ENA_SET(enable),
		 RB_RB_CFG_RB_ENA,
		 sparx5, RB_RB_CFG(rb));
}

static void lan969x_rb_autoage_configure(struct sparx5 *sparx5, u8 rb)
{
	/* The host table has 1,024 rows. The clock frequency is 328.125 MHz
	 * giving a clock period of 3.048 ns. To achieve a 10 seconds aging
	 * period per row, then each row must be aged every ~10ms. We choose
	 * UNIT_SIZE = 3 (65,536*3.048ns = 199.75us) and PERIOD_VAL = 50.
	 */
	spx5_rmw(RB_HOST_AUTOAGE_CFG_UNIT_SIZE_SET(3) |
		 RB_HOST_AUTOAGE_CFG_PERIOD_VAL_SET(50),
		 RB_HOST_AUTOAGE_CFG_UNIT_SIZE |
		 RB_HOST_AUTOAGE_CFG_PERIOD_VAL,
		 sparx5, RB_HOST_AUTOAGE_CFG(rb, 0));

	/* The duplicate discard  table has 256 rows. The clock frequency is
	 * 328.125 MHz giving a clock period of 3.048 ns. To achieve a 100ms
	 * aging period per row, then each row must be aged every ~10ms. We
	 * choose UNIT_SIZE = 1 (256*3.048ns = 780.3ns) and PERIOD_VAL = 501.
	 */
	spx5_rmw(RB_DISC_AUTOAGE_CFG_UNIT_SIZE_SET(1) |
		 RB_DISC_AUTOAGE_CFG_PERIOD_VAL_SET(501),
		 RB_DISC_AUTOAGE_CFG_UNIT_SIZE |
		 RB_DISC_AUTOAGE_CFG_PERIOD_VAL,
		 sparx5, RB_DISC_AUTOAGE_CFG(rb));

	spx5_rmw(RB_HOST_AUTOAGE_CFG_1_AUTOAGE_INTERVAL_ENA_SET(1),
		 RB_HOST_AUTOAGE_CFG_1_AUTOAGE_INTERVAL_ENA,
		 sparx5, RB_HOST_AUTOAGE_CFG_1(rb, 0));

	spx5_rmw(RB_DISC_AUTOAGE_CFG_1_AUTOAGE_INTERVAL_ENA_SET(1),
		 RB_DISC_AUTOAGE_CFG_1_AUTOAGE_INTERVAL_ENA,
		 sparx5, RB_DISC_AUTOAGE_CFG_1(rb));
}

static void lan969x_rb_port_configure(struct sparx5 *sparx5,
				      struct lan969x_redbox_params *params,
				      u8 port, u8 rb)
{
	lan969x_rb_port_tag_mode_set(sparx5, rb, port, params->tag);
	lan969x_rb_port_hsr_aware_set(sparx5, rb, port, params->hsr);
	lan969x_rb_port_prp_aware_set(sparx5, rb, port, params->prp);
	lan969x_rb_port_hsr_filter_set(sparx5, rb, port, params->filter);
	lan969x_rb_port_forward_masks_set(sparx5, rb, port, params);
	lan969x_rb_port_lanid_set(sparx5, rb, port, params->lan_id);
	lan969x_rb_port_netid_set(sparx5, rb, port, params->net_id);
	lan969x_rb_port_hsr_spv_fwd_set(sparx5, rb, port, params->spv_sel);
}

static void lan969x_rb_table_configure(struct sparx5 *sparx5,
				       struct lan969x_redbox_params *params,
				       u8 port, u8 rb)
{
	spx5_rmw(RB_TBL_CFG_CLR_AGE_FLAG_DIS_SET(1) |
		 RB_TBL_CFG_DUPL_DISC_ENA_SET(1) |
		 RB_TBL_CFG_HOST_TYPE_SET(params->host_type) |
		 RB_TBL_CFG_HOST_AGE_INTERVAL_SET(0) |
		 RB_TBL_CFG_UPD_DISC_TBL_ENA_SET(1) |
		 RB_TBL_CFG_UPD_HOST_TBL_ENA_SET(1) |
		 RB_TBL_CFG_UPD_SEQ_NUM_ENA_SET(1) |
		 RB_TBL_CFG_NEW_HOST_TBL_DIS_SET(0),
		 RB_TBL_CFG_CLR_AGE_FLAG_DIS |
		 RB_TBL_CFG_DUPL_DISC_ENA |
		 RB_TBL_CFG_HOST_TYPE |
		 RB_TBL_CFG_HOST_AGE_INTERVAL |
		 RB_TBL_CFG_UPD_DISC_TBL_ENA |
		 RB_TBL_CFG_UPD_HOST_TBL_ENA |
		 RB_TBL_CFG_UPD_SEQ_NUM_ENA |
		 RB_TBL_CFG_NEW_HOST_TBL_DIS,
		 sparx5, RB_TBL_CFG(rb, port));
}

static void lan969x_rb_common_configure(struct sparx5 *sparx5, u8 rb, u8 mode)
{
	/* Enable redbox before writing to any redbox registers. */
	lan969x_rb_ena(sparx5, rb, true);

	spx5_rmw(RB_RB_CFG_RB_MODE_SET(mode) |
		 RB_RB_CFG_LOCAL_DST_REDIR_ENA_SET(1) |
		 RB_RB_CFG_REWRITE_REDIR_ENA_SET(1),
		 RB_RB_CFG_RB_MODE |
		 RB_RB_CFG_LOCAL_DST_REDIR_ENA |
		 RB_RB_CFG_REWRITE_REDIR_ENA,
		 sparx5, RB_RB_CFG(rb));

	/* Supervision frames inbound on the interlink, goes to the CPU. */
	spx5_rmw(RB_SPV_CFG_HSR_SPV_INT_FWD_SEL_SET(RB_FWD_SEL_REDIR_CPU),
		 RB_SPV_CFG_HSR_SPV_INT_FWD_SEL,
		 sparx5, RB_SPV_CFG(rb));

	/* Frames redirected to CPU is also subject to duplicate discard. */
	spx5_rmw(RB_CPU_CFG_DUPL_DISC_CPU_ENA_SET(1),
		 RB_CPU_CFG_DUPL_DISC_CPU_ENA,
		 sparx5, RB_CPU_CFG(rb));
}

static int lan969x_rb_table_status(void *addr)
{
	return readl(addr);
}

static int lan969x_rb_htable_wait_complete(struct sparx5 *sparx5, u8 rb)
{
	void __iomem *addr;
	u32 val;

	addr = spx5_addr(sparx5->regs, RB_HOST_ACCESS_CTRL(rb));

	return readx_poll_timeout_atomic(lan969x_rb_table_status,
					 addr,
					 val,
					 RB_HOST_ACCESS_CTRL_ACCESS_SHOT_GET(val) == 0,
					 RB_TABLE_SLEEP_US,
					 RB_TABLE_TIMEOUT_US);
}

static void lan969x_rb_htable_cmd_set(struct sparx5 *sparx5, u8 rb, u8 cmd)
{
	spx5_rmw(RB_HOST_ACCESS_CTRL_CMD_SET(cmd), RB_HOST_ACCESS_CTRL_CMD,
		 sparx5, RB_HOST_ACCESS_CTRL(rb));
}

static void lan969x_rb_htable_row_set(struct sparx5 *sparx5, u8 rb, u32 row)
{
	spx5_rmw(RB_HOST_ACCESS_CTRL_DIRECT_ROW_SET(row),
		 RB_HOST_ACCESS_CTRL_DIRECT_ROW, sparx5,
		 RB_HOST_ACCESS_CTRL(rb));
}

static void lan969x_rb_htable_col_set(struct sparx5 *sparx5, u8 rb, u32 col)
{
	spx5_rmw(RB_HOST_ACCESS_CTRL_DIRECT_COL_SET(col),
		 RB_HOST_ACCESS_CTRL_DIRECT_COL, sparx5,
		 RB_HOST_ACCESS_CTRL(rb));
}

static int lan969x_rb_dtable_wait_complete(struct sparx5 *sparx5, u8 rb)
{
	void __iomem *addr;
	u32 val;

	addr = spx5_addr(sparx5->regs, RB_DISC_ACCESS_CTRL(rb));

	return readx_poll_timeout_atomic(lan969x_rb_table_status,
					 addr,
					 val,
					 RB_DISC_ACCESS_CTRL_ACCESS_SHOT_GET(val) == 0,
					 RB_TABLE_SLEEP_US,
					 RB_TABLE_TIMEOUT_US);
}

static void lan969x_rb_dtable_cmd_set(struct sparx5 *sparx5, u8 rb, u8 cmd)
{
	spx5_rmw(RB_DISC_ACCESS_CTRL_CMD_SET(cmd),
		 RB_DISC_ACCESS_CTRL_CMD, sparx5,
		 RB_DISC_ACCESS_CTRL(rb));
}

static void lan969x_rb_dtable_col_set(struct sparx5 *sparx5, u8 rb, u32 col)
{
	spx5_rmw(RB_DISC_ACCESS_CTRL_DIRECT_COL_SET(col),
		 RB_DISC_ACCESS_CTRL_DIRECT_COL, sparx5,
		 RB_DISC_ACCESS_CTRL(rb));
}

static void lan969x_rb_dtable_row_set(struct sparx5 *sparx5, u8 rb, u32 row)
{
	spx5_rmw(RB_DISC_ACCESS_CTRL_DIRECT_ROW_SET(row),
		 RB_DISC_ACCESS_CTRL_DIRECT_ROW, sparx5,
		 RB_DISC_ACCESS_CTRL(rb));
}

static void lan969x_rb_table_read_mac(u32 mach, u32 macl,
				      unsigned char mac[ETH_ALEN], u8 rb)
{
	mac[0] = (mach >> 8) & 0xff;
	mac[1] = (mach >> 0) & 0xff;
	mac[2] = (macl >> 24) & 0xff;
	mac[3] = (macl >> 16) & 0xff;
	mac[4] = (macl >> 8) & 0xff;
	mac[5] = (macl >> 0) & 0xff;
}

static void lan969x_rb_htable_read_mac(struct sparx5 *sparx5,
				       struct lan969x_rb_htable_node *node,
				       u8 rb)
{
	u32 mach = RB_HOST_ACCESS_CFG_0_HOST_ENTRY_MAC_MSB_GET(spx5_rd(sparx5, RB_HOST_ACCESS_CFG_0(rb)));
	u32 macl = spx5_rd(sparx5, RB_HOST_ACCESS_CFG_1(rb));

	lan969x_rb_table_read_mac(mach, macl, node->mac, rb);
}

static void lan969x_rb_htable_read_ports(struct sparx5 *sparx5,
					 struct lan969x_rb_htable_node *node,
					 u8 rb)
{
	u32 val, stats;
	u8 fwd_mask;

	val = spx5_rd(sparx5, RB_HOST_ACCESS_CFG_2(rb));
	stats = spx5_rd(sparx5, RB_HOST_ACCESS_STAT_3(rb));
	fwd_mask = RB_HOST_ACCESS_CFG_2_PORTMASK_GET(val);

	/* Read data for LREA. */
	node->ports[0].rct = RB_HOST_ACCESS_CFG_2_RCT_VALID_0_GET(val);
	node->ports[0].age = RB_HOST_ACCESS_CFG_2_AGE_FLAG_0_GET(val);
	node->ports[0].fwd = fwd_mask;
	node->ports[0].rx = spx5_rd(sparx5, RB_HOST_ACCESS_STAT_0(rb));
	node->ports[0].rx_wrong_lan =
		RB_HOST_ACCESS_STAT_3_CNT_RX_WRONG_LAN_0_GET(stats);

	/* Read data for LREB. */
	node->ports[1].rct = RB_HOST_ACCESS_CFG_2_RCT_VALID_1_GET(val);
	node->ports[1].age = RB_HOST_ACCESS_CFG_2_AGE_FLAG_1_GET(val);
	node->ports[1].fwd = fwd_mask;
	node->ports[1].rx = spx5_rd(sparx5, RB_HOST_ACCESS_STAT_1(rb));
	node->ports[1].rx_wrong_lan =
		RB_HOST_ACCESS_STAT_3_CNT_RX_WRONG_LAN_1_GET(stats);

	/* Read data for LREC. */
	node->ports[2].rct = RB_HOST_ACCESS_CFG_2_RCT_MISSING_GET(val);
	node->ports[2].age = RB_HOST_ACCESS_CFG_2_AGE_FLAG_2_GET(val);
	node->ports[2].fwd = fwd_mask;
	node->ports[2].rx = spx5_rd(sparx5, RB_HOST_ACCESS_STAT_2(rb));
	node->ports[2].rx_wrong_lan =
		RB_HOST_ACCESS_STAT_3_CNT_RX_WRONG_LAN_2_GET(stats);
}

static int lan969x_rb_htable_read(struct sparx5 *sparx5,
				  struct lan969x_rb_htable_node *node, u8 rb,
				  u32 idx)
{
	u32 val1, val2;

	lan969x_rb_htable_cmd_set(sparx5, rb, RB_TABLE_CMD_READ);

	lan969x_rb_htable_row_set(sparx5, rb, idx / 4);
	lan969x_rb_htable_col_set(sparx5, rb, idx % 4);

	/* Start CPU access to host table. */
	spx5_rmw(RB_HOST_ACCESS_CTRL_ACCESS_SHOT_SET(1),
		 RB_HOST_ACCESS_CTRL_ACCESS_SHOT, sparx5,
		 RB_HOST_ACCESS_CTRL(rb));

	/* Wait for completion. */
	lan969x_rb_htable_wait_complete(sparx5, rb);

	val1 = spx5_rd(sparx5, RB_HOST_ACCESS_CFG_0(rb));
	val2 = spx5_rd(sparx5, RB_HOST_ACCESS_CFG_2(rb));

	if (!RB_HOST_ACCESS_CFG_2_VLD_GET(val2))
		return -EINVAL;

	node->idx = rb;
	node->seq_no = RB_HOST_ACCESS_CFG_0_HOST_ENTRY_SEQ_NO_GET(val1);
	node->locked = RB_HOST_ACCESS_CFG_2_LOCKED_GET(val2);
	node->type = RB_HOST_ACCESS_CFG_2_TYPE_GET(val2);
	node->pdan = RB_HOST_ACCESS_CFG_2_PROXY_DAN_GET(val2);
	node->valid = RB_HOST_ACCESS_CFG_2_VLD_GET(val2);
	node->age_interval = RB_HOST_ACCESS_CFG_2_AGE_INTERVAL_GET(val2);

	lan969x_rb_htable_read_mac(sparx5, node, rb);
	lan969x_rb_htable_read_ports(sparx5, node, rb);

	return 0;
}

static int lan969x_rb_htable_learn(struct sparx5 *sparx5, u8 rb,
				   const unsigned char mac[ETH_ALEN])
{
	u32 macl = 0, mach = 0;

	mach |= mac[0] << 8;
	mach |= mac[1] << 0;
	macl |= mac[2] << 24;
	macl |= mac[3] << 16;
	macl |= mac[4] << 8;
	macl |= mac[5] << 0;

	lan969x_rb_htable_cmd_set(sparx5, rb, RB_TABLE_CMD_LEARN);

	spx5_wr(mach, sparx5, RB_HOST_ACCESS_CFG_0(rb));
	spx5_wr(macl, sparx5, RB_HOST_ACCESS_CFG_1(rb));

	spx5_rmw(RB_HOST_ACCESS_CFG_2_LOCKED_SET(1) |
		 RB_HOST_ACCESS_CFG_2_VLD_SET(1)    |
		 RB_HOST_ACCESS_CFG_2_TYPE_SET(RB_HT_LOCAL) |
		 RB_HOST_ACCESS_CFG_2_AGE_INTERVAL_SET(1),
		 RB_HOST_ACCESS_CFG_2_LOCKED        |
		 RB_HOST_ACCESS_CFG_2_VLD           |
		 RB_HOST_ACCESS_CFG_2_TYPE          |
		 RB_HOST_ACCESS_CFG_2_AGE_INTERVAL,
		 sparx5, RB_HOST_ACCESS_CFG_2(rb));

	/* Start CPU access to host table. */
	spx5_rmw(RB_HOST_ACCESS_CTRL_ACCESS_SHOT_SET(1),
		 RB_HOST_ACCESS_CTRL_ACCESS_SHOT, sparx5,
		 RB_HOST_ACCESS_CTRL(rb));

	/* Wait for completion. */
	lan969x_rb_htable_wait_complete(sparx5, rb);

	return 0;
}

static const char *lan969x_rb_type_to_str(u8 type)
{
	switch (type) {
	case RB_HT_PROXY:
		return "PROXY";
	case RB_HT_DAN:
		return "DAN";
	case RB_HT_SAN:
		return "SAN";
	case RB_HT_LOCAL:
		return "LOCAL";
	case RB_HT_NONE:
		return "NONE";
	default:
		return "NONE";
	}
}

static const char *lan969x_rb_mode_to_str(struct lan969x_redbox *rb)
{
	switch (rb->mode) {
	case RB_MODE_HSR_SAN:
		if (lan969x_rb_has_lrec(rb))
			return "HSR-SAN";
		else
			return "DANH";
	case RB_MODE_PRP_SAN:
		if (lan969x_rb_has_lrec(rb))
			return "PRP-SAN";
		else
			return "DANP";
	default:
		return "NONE";
	}
}

static void lan969x_rb_htable_print(struct sparx5 *sparx5)
{
	if (debugfs_redbox_idx >= 5) {
		pr_err("Invalid redbox index: %u", debugfs_redbox_idx);
		return;
	}

	pr_info("                                   Rx                    Last Seen             Rx Wrong LAN\n");
	pr_info("                                   --------------------- --------------------- ---------------------\n");
	pr_info("Inst MAC Address       Node Type   Port A     Port B     Port A     Port B     Port A     Port B\n");
	pr_info("---- ----------------- ----------- ---------- ---------- ---------- ---------- ---------- ----------\n");

	for (int j = 0; j < RB_HTABLE_CNT; j++) {
		struct lan969x_rb_htable_node node = { 0 };

		if (lan969x_rb_htable_read(sparx5, &node, debugfs_redbox_idx,
					   j))
			continue;

		struct lan969x_rb_htable_port *port_a = &node.ports[LAN969X_RB_LREA];
		struct lan969x_rb_htable_port *port_b = &node.ports[LAN969X_RB_LREB];

		pr_info("%4u %pM %-11s %10u %10u %10u %10u %10u %10u\n\n",
			debugfs_redbox_idx,
			node.mac,
			lan969x_rb_type_to_str(node.type),
			port_a->rx,
			port_b->rx,
			port_a->age,
			port_b->age,
			port_a->rx_wrong_lan,
			port_b->rx_wrong_lan);
	}
}

static void lan969x_rb_dtable_read_mac(struct sparx5 *sparx5,
				       struct lan969x_rb_dtable_node *node,
				       u8 rb)
{
	u32 mach = RB_DISC_ACCESS_CFG_0_DISC_ENTRY_SMAC_MSB_GET(spx5_rd(sparx5, RB_DISC_ACCESS_CFG_0(rb)));
	u32 macl = spx5_rd(sparx5, RB_DISC_ACCESS_CFG_1(rb));

	lan969x_rb_table_read_mac(mach, macl, node->mac, rb);
}

static void lan969x_rb_dtable_read_ports(struct sparx5 *sparx5,
					 struct lan969x_rb_dtable_node *node,
					 u8 rb)
{
	u32 val;

	val = spx5_rd(sparx5, RB_DISC_ACCESS_CFG_2(rb));

	/* Read data for LREA. */
	node->ports[0].n_disc = RB_DISC_ACCESS_CFG_2_DISC_CNT_0_GET(val);

	/* Read data for LREB. */
	node->ports[1].n_disc = RB_DISC_ACCESS_CFG_2_DISC_CNT_1_GET(val);

	/* Read data for LREC. */
	node->ports[2].n_disc = RB_DISC_ACCESS_CFG_2_DISC_CNT_2_GET(val);
}

static int lan969x_rb_dtable_read(struct sparx5 *sparx5,
				  struct lan969x_rb_dtable_node *node, u8 rb,
				  u32 idx)
{
	u32 val1, val2;

	lan969x_rb_dtable_cmd_set(sparx5, rb, RB_TABLE_CMD_READ);
	lan969x_rb_dtable_row_set(sparx5, rb, idx / 8);
	lan969x_rb_dtable_col_set(sparx5, rb, idx % 8);

	/* Start CPU access to host table. */
	spx5_rmw(RB_DISC_ACCESS_CTRL_ACCESS_SHOT_SET(1),
		 RB_DISC_ACCESS_CTRL_ACCESS_SHOT, sparx5,
		 RB_DISC_ACCESS_CTRL(rb));

	/* Wait for completion. */
	lan969x_rb_dtable_wait_complete(sparx5, rb);

	val1 = spx5_rd(sparx5, RB_DISC_ACCESS_CFG_0(rb));
	val2 = spx5_rd(sparx5, RB_DISC_ACCESS_CFG_2(rb));

	if (!RB_DISC_ACCESS_CFG_2_DISC_ENTRY_VLD_GET(val2))
		return -EINVAL;

	node->seq_no = RB_DISC_ACCESS_CFG_0_DISC_ENTRY_SEQ_NO_GET(val1);
	node->age = RB_DISC_ACCESS_CFG_2_DISC_ENTRY_AGE_FLAG_GET(val2);

	lan969x_rb_dtable_read_mac(sparx5, node, rb);
	lan969x_rb_dtable_read_ports(sparx5, node, rb);

	return 0;
}

static void lan969x_rb_dtable_print(struct sparx5 *sparx5)
{
	if (debugfs_redbox_idx >= 5) {
		pr_err("Invalid redbox index: %u", debugfs_redbox_idx);
		return;
	}

	pr_info("                                   Discard count\n");
	pr_info("                                   --------------------------------\n");
	pr_info("Inst MAC Address       Seqno Age   Port A     Port B     Port C    \n");
	pr_info("---- ----------------- ----- ----- ---------- ---------- ----------\n");

	for (int j = 0; j < RB_DTABLE_CNT; j++) {
		struct lan969x_rb_dtable_node node = { 0 };

		if (lan969x_rb_dtable_read(sparx5, &node, debugfs_redbox_idx,
					   j))
			continue;

		pr_info("%4u %pM %5u %5u %10u %10u %10u\n\n",
			debugfs_redbox_idx,
			node.mac,
			node.seq_no,
			node.age,
			node.ports[LAN969X_RB_LREA].n_disc,
			node.ports[LAN969X_RB_LREB].n_disc,
			lan969x_rb_has_lrec(&redboxes[debugfs_redbox_idx]) ?
				node.ports[LAN969X_RB_LREC].n_disc :
				0);
	}
}

static bool lan969x_rb_has_warnings(struct sparx5 *sparx5, u8 rb)
{
	struct lan969x_rb_htable_node node;

	/* Get port stats. */
	lan969x_rb_htable_read_ports(sparx5, &node, rb);

	if (node.ports[LAN969X_RB_LREA].rx_wrong_lan ||
	    node.ports[LAN969X_RB_LREB].rx_wrong_lan ||
	    node.ports[LAN969X_RB_LREC].rx_wrong_lan)
		return true;

	return false;
}

static void lan969x_rb_print(struct sparx5 *sparx5)
{
	pr_info("Inst Oper. State Taxi Mode    Port A     Port B     Port C     Warnings\n");
	pr_info("---- ----------- ---- ------- ---------- ---------- ---------- --------\n");

	for (int i = 0; i < LAN969X_RB_REDBOX_CNT; i++) {
		struct lan969x_redbox *rb = &redboxes[i];
		u8 taxi = rb->taxi;

		if (!rb->hsr_master) {
			pr_info("%4u %-11s", i, "Inactive");
			continue;
		}

		pr_info("%4u %-11s %4u %7s %-10s %-10s %-10s %-8s\n",
			i,
			"Active",
			taxi,
			lan969x_rb_mode_to_str(rb),
			rb->ports[LAN969X_RB_LREA]->ndev->name,
			rb->ports[LAN969X_RB_LREB]->ndev->name,
			rb->ports[LAN969X_RB_LREC] ? rb->ports[LAN969X_RB_LREC]->ndev->name : "N/A",
			lan969x_rb_has_warnings(sparx5, i) ? "YES" : "no");
	}
}

static int lan969x_rb_show(struct seq_file *s, void *unused)
{
	struct sparx5 *sparx5 = s->private;

	lan969x_rb_print(sparx5);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan969x_rb);

static int lan969x_rb_htable_show(struct seq_file *s, void *unused)
{
	struct sparx5 *sparx5 = s->private;

	lan969x_rb_htable_print(sparx5);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan969x_rb_htable);

static int lan969x_rb_dtable_show(struct seq_file *s, void *unused)
{
	struct sparx5 *sparx5 = s->private;

	lan969x_rb_dtable_print(sparx5);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan969x_rb_dtable);

void lan969x_rb_debugfs(struct sparx5 *sparx5)
{
	debugfs_create_x32("redbox_idx", 0644, sparx5->debugfs_root,
			   &debugfs_redbox_idx);

	debugfs_create_file("redbox", 0444, sparx5->debugfs_root, sparx5,
			    &lan969x_rb_fops);

	debugfs_create_file("redbox_htable", 0444, sparx5->debugfs_root, sparx5,
			    &lan969x_rb_htable_fops);

	debugfs_create_file("redbox_dtable", 0444, sparx5->debugfs_root, sparx5,
			    &lan969x_rb_dtable_fops);
}

/* Retrieve a redbox for this master. */
static struct lan969x_redbox *lan969x_rb_get(struct net_device *master)
{
	/* Check if a redbox with this master already exists. */
	for (int i = 0; i < LAN969X_RB_REDBOX_CNT; i++)
		if (redboxes[i].hsr_master == master)
			return &redboxes[i];

	/* Check if a free redbox exists. */
	for (int i = 0; i < LAN969X_RB_REDBOX_CNT; i++)
		if (!redboxes[i].hsr_master) {
			redboxes[i].hsr_master = master;
			return &redboxes[i];
		}

	/* No redboxes available. */
	return ERR_PTR(-ENOENT);
}

static void lan969x_rb_put(struct lan969x_redbox *rb)
{
	lan969x_rb_del_lrea(rb);
	lan969x_rb_del_lreb(rb);
	lan969x_rb_del_lrec(rb);

	rb->hsr_master = NULL;
}

int lan969x_hsr_join(struct net_device *master, struct net_device *slave)
{
	u8 mode, lrea_taxi_port, lreb_taxi_port, lrea_taxi_idx, lreb_taxi_idx;
	struct sparx5_port *port = netdev_priv(slave);
	struct sparx5 *sparx5 = port->sparx5;
	struct lan969x_redbox *redbox;
	enum hsr_version version;
	int err;

	/* Retrieve a redbox instance for this device. */
	redbox = lan969x_rb_get(master);
	if (IS_ERR(redbox))
		return PTR_ERR(redbox);

	err = hsr_get_version(master, &version);
	if (err)
		return err;

	err = lan969x_rb_mode_get(version, &mode);
	if (err)
		return err;

	/* LREA */
	if (!lan969x_rb_has_lrea(redbox)) {
		lan969x_rb_add_lrea(redbox, port);
		return 0;
	}

	/* LREB */
	if (!lan969x_rb_has_lreb(redbox)) {
		lan969x_rb_add_lreb(redbox, port);
		if (is_hsr_redbox(master))
			/* Need LREC to sign up as well. */
			return 0;
	}

	/* LREC */
	if (!lan969x_rb_has_lrec(redbox) && is_hsr_redbox(master))
		lan969x_rb_add_lrec(redbox, port);

	err = lan969x_rb_port_taxi_get(redbox->ports[LAN969X_RB_LREA],
				       &lrea_taxi_port,
				       &lrea_taxi_idx);
	if (err)
		return err;

	err = lan969x_rb_port_taxi_get(redbox->ports[LAN969X_RB_LREB],
				       &lreb_taxi_port,
				       &lreb_taxi_idx);
	if (err)
		return err;

	if (lrea_taxi_idx != lreb_taxi_idx) {
		pr_err("LREA and LREB must be attached to the same taxi bus.");
		lan969x_rb_put(redbox);
		return -EINVAL;
	}

	/* At this point, we know that we have a valid redbox configuration. */
	redbox->mode = mode;
	redbox->taxi = lrea_taxi_idx;

	/* Enable and do common configuration of redbox. */
	lan969x_rb_common_configure(sparx5, redbox->taxi, mode);

	/* Configure autoaging */
	lan969x_rb_autoage_configure(sparx5, redbox->taxi);

	/* Connect front ports to taxi ports. */
	lan969x_rb_taxi_ports_set(sparx5,
				  lrea_taxi_port,
				  lreb_taxi_port,
				  redbox->taxi);

	if (is_hsr_redbox(master)) {
		/* Forward traffic from lrea to lrec and from lrec to lrea. */
		sparx5_update_fwd(sparx5);

		/* Interlink port delivers frames where the FCS must be
		 * corrected on the way out.
		 */
		lan969x_rb_fcs_upd_set(sparx5,
				       redbox->ports[LAN969X_RB_LREC]->portno,
				       true);
	}

	/* Configure each port. */
	for (int i = 0; i < LAN969X_RB_PORT_MAX; i++) {
		struct lan969x_redbox_params *params;

		/* Get the redbox port parameters. */
		lan969x_rb_port_params_get(mode, redbox);

		params = &redbox->params[i];

		/* Configure the port. */
		lan969x_rb_port_configure(sparx5, params, i, redbox->taxi);

		/* Configure the host and duplicate discard tables. */
		lan969x_rb_table_configure(sparx5, params, i, redbox->taxi);
	}

	/* Learn LREA and LREB as static entries, with type LOCAL. */
	lan969x_rb_htable_learn(sparx5,
				redbox->taxi,
				redbox->ports[LAN969X_RB_LREA]->ndev->dev_addr);

	lan969x_rb_htable_learn(sparx5,
				redbox->taxi,
				redbox->ports[LAN969X_RB_LREB]->ndev->dev_addr);

	/* Enable Extraction Redundancy Information towards the switch core. */
	lan969x_rb_eri_set(sparx5,
			   redbox->ports[LAN969X_RB_LREA]->portno,
			   true);

	lan969x_rb_eri_set(sparx5,
			   redbox->ports[LAN969X_RB_LREB]->portno,
			   true);

	return 0;
}

int lan969x_hsr_leave(struct net_device *master, struct net_device *slave)
{
	struct sparx5_port *port = netdev_priv(slave);
	struct sparx5 *sparx5 = port->sparx5;
	struct lan969x_redbox *redbox;
	bool has_interlink = false;

	/* Retrieve a redbox instance for this device. */
	redbox = lan969x_rb_get(master);
	if (IS_ERR(redbox))
		return PTR_ERR(redbox);

	if (lan969x_rb_has_lrec(redbox))
		has_interlink = true;

	/* Disable redbox. */
	lan969x_rb_ena(sparx5, redbox->taxi, false);

	/* This will remove all ports and nullify the hsr master. This is done
	 * for all linked ports, but that should be fine.
	 */
	lan969x_rb_put(redbox);

	/* Clear source port masks for lrea and lrec when HSR-SAN */
	if (has_interlink)
		sparx5_update_fwd(sparx5);

	return 0;
}

void lan969x_rb_fwd_mask_get(unsigned long *mask, u8 rb)
{
	struct lan969x_redbox *redbox = &redboxes[rb];

	if (!redbox->hsr_master)
		return;

	if (!is_hsr_redbox(redbox->hsr_master))
		return;

	*mask |= BIT(redbox->ports[LAN969X_RB_LREA]->portno);
	*mask |= BIT(redbox->ports[LAN969X_RB_LREC]->portno);
}
