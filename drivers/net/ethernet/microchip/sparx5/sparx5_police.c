// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2023 Microchip Technology Inc. and its subsidiaries.
 */

#include "sparx5_main_regs.h"
#include "sparx5_main.h"

#define SPX5_PORT_POLICER_ALL_COUNTER 0
#define SPX5_PORT_POLICER_FILTER_COUNTER 1
#define SPX5_PORT_POLICER_PASS_COUNTER 2

#define SPX5_PORT_POLICER_0_PASS_EVENT BIT(4)
#define SPX5_PORT_POLICER_1_PASS_EVENT BIT(5)
#define SPX5_PORT_POLICER_2_PASS_EVENT BIT(6)
#define SPX5_PORT_POLICER_3_PASS_EVENT BIT(7)
#define SPX5_PORT_POLICER_0_FILTER_EVENT BIT(8)
#define SPX5_PORT_POLICER_1_FILTER_EVENT BIT(9)
#define SPX5_PORT_POLICER_2_FILTER_EVENT BIT(10)
#define SPX5_PORT_POLICER_3_FILTER_EVENT BIT(11)

/* Set GAP to 20 bytes (12 bytes of IFG and 8 bytes of preamble) to measure line
 * rate
 */
#define SPX5_POLICER_LINE_RATE_GAP 20

enum sparx5_port_policer_stat_event_mask {
	SPX5_PPEM_NONE,
	SPX5_PPEM_EVENT_NO_ERROR,
	SPX5_PPEM_EVENT_AND_ERROR,
	SPX5_PPEM_EVENT,
	SPX5_PPEM_ERROR_NO_EVENT,
	SPX5_PPEM_ERROR,
};

int sparx5_policer_stats_update(struct sparx5 *sparx5,
				struct sparx5_policer *pol)
{
	struct sparx5_port *port;
	int portno, polidx;

	switch (pol->type) {
	case SPX5_POL_PORT:
		portno = pol->idx;
		polidx = do_div(portno, SPX5_POLICERS_PER_PORT);
		port = sparx5->ports[portno];
		return sparx5_policer_port_stats_update(port, polidx);
	default:
		break;
	}

	return 0;
}

static int sparx5_policer_service_conf_set(struct sparx5 *sparx5,
					   struct sparx5_policer *pol)
{
	const struct sparx5_ops *ops = &sparx5->data->ops;
	u32 idx, pup_tokens, max_pup_tokens, burst, thres;
	struct sparx5_sdlb_group *g;
	u64 rate;

	g = ops->get_sdlb_group(pol->group);
	idx = pol->idx;

	rate = pol->rate * 1000;
	burst = pol->burst;

	pup_tokens = sparx5_sdlb_pup_token_get(sparx5, g->pup_interval, rate);
	max_pup_tokens =
		sparx5_sdlb_pup_token_get(sparx5, g->pup_interval, g->max_rate);

	thres = DIV_ROUND_UP(burst, g->min_burst);

	spx5_wr(ANA_AC_SDLB_PUP_TOKENS_PUP_TOKENS_SET(pup_tokens), sparx5,
		ANA_AC_SDLB_PUP_TOKENS(idx, 0));

	spx5_rmw(ANA_AC_SDLB_INH_CTRL_PUP_TOKENS_MAX_SET(max_pup_tokens),
		 ANA_AC_SDLB_INH_CTRL_PUP_TOKENS_MAX, sparx5,
		 ANA_AC_SDLB_INH_CTRL(idx, 0));

	spx5_rmw(ANA_AC_SDLB_THRES_THRES_SET(thres), ANA_AC_SDLB_THRES_THRES,
		 sparx5, ANA_AC_SDLB_THRES(idx, 0));

	return 0;
}

static int sparx5_policer_acl_conf_set(struct sparx5 *sparx5,
				       struct sparx5_policer *pol)
{
	/* Set rate */
	spx5_rmw(ANA_AC_POL_POL_ACL_RATE_CFG_ACL_RATE_SET(pol->rate),
		 ANA_AC_POL_POL_ACL_RATE_CFG_ACL_RATE, sparx5,
		 ANA_AC_POL_POL_ACL_RATE_CFG(pol->idx));

	/* Set burst */
	spx5_rmw(ANA_AC_POL_POL_ACL_THRES_CFG_ACL_THRES_SET(pol->rate),
		 ANA_AC_POL_POL_ACL_THRES_CFG_ACL_THRES, sparx5,
		 ANA_AC_POL_POL_ACL_THRES_CFG(pol->idx));

	return 0;
}

int sparx5_policer_port_stats_update(struct sparx5_port *port, int polidx)
{
	u32 lsb, msb;

	lsb = spx5_rd(port->sparx5,
		      ANA_AC_PORT_STAT_LSB_CNT(port->portno,
					       SPX5_PORT_POLICER_FILTER_COUNTER));
	msb = spx5_rd(port->sparx5,
		      ANA_AC_PORT_STAT_MSB_CNT(port->portno,
					       SPX5_PORT_POLICER_FILTER_COUNTER));
	sparx5_update_u64_counter(&port->tc.port_policer[polidx].stats.drops,
				   msb, lsb);
	lsb = spx5_rd(port->sparx5,
		      ANA_AC_PORT_STAT_LSB_CNT(port->portno,
					       SPX5_PORT_POLICER_PASS_COUNTER));
	msb = spx5_rd(port->sparx5,
		      ANA_AC_PORT_STAT_MSB_CNT(port->portno,
					       SPX5_PORT_POLICER_PASS_COUNTER));
	sparx5_update_u64_counter(&port->tc.port_policer[polidx].stats.pkts,
				  msb, lsb);
	return 0;
}

static int sparx5_policer_port_conf_set(struct sparx5 *sparx5,
					struct sparx5_policer *pol)
{
	int polidx, portno = pol->idx;
	u32 rate, burst, mask;

	polidx = do_div(portno, SPX5_POLICERS_PER_PORT);
	/* Set rate */
	rate = DIV_ROUND_UP(pol->rate, SPX5_POLICER_RATE_UNIT);
	spx5_wr(rate, sparx5,
		ANA_AC_POL_POL_PORT_RATE_CFG(pol->idx));
	/* Set burst */
	burst = DIV_ROUND_UP(pol->burst, SPX5_POLICER_BYTE_BURST_UNIT);
	spx5_wr(burst, sparx5, ANA_AC_POL_POL_PORT_THRES_CFG_0(pol->idx));
	pr_debug("%s:%d: offset: %d => portno: %d, polidx: %d, rate: %d, burst: %d\n",
		 __func__, __LINE__, pol->idx, portno, polidx, rate, burst);
	/* Set traffic type mask */
	if (rate == 0 && burst == 0) /* Disable policer */
		mask = 0;
	else  /* Known and unknown BUM traffic, cpu queue, and learn */
		mask = 0x7f;
	spx5_rmw(ANA_AC_POL_POL_PORT_CFG_TRAFFIC_TYPE_MASK_SET(mask),
		 ANA_AC_POL_POL_PORT_CFG_TRAFFIC_TYPE_MASK,
		 sparx5,
		 ANA_AC_POL_POL_PORT_CFG(portno, polidx));
	/* Set statistics counter, count policer events */
	spx5_rmw(ANA_AC_PORT_STAT_CFG_CFG_CNT_FRM_TYPE_SET(SPX5_PPEM_EVENT),
		 ANA_AC_PORT_STAT_CFG_CFG_CNT_FRM_TYPE,
		 sparx5, ANA_AC_PORT_STAT_CFG(portno, polidx));
	/* Count frames, not bytes */
	spx5_rmw(ANA_AC_PORT_STAT_CFG_CFG_CNT_BYTE_SET(0),
		 ANA_AC_PORT_STAT_CFG_CFG_CNT_BYTE,
		 sparx5, ANA_AC_PORT_STAT_CFG(portno, polidx));
	spx5_rmw(ANA_AC_POL_POL_ACL_CTRL_GAP_VALUE_SET(SPX5_POLICER_LINE_RATE_GAP),
		 ANA_AC_POL_POL_ACL_CTRL_GAP_VALUE,
		 sparx5, ANA_AC_POL_POL_ACL_CTRL(portno));
	/* Enable count of all 8 priorities */
	spx5_rmw(ANA_AC_PORT_STAT_CFG_CFG_PRIO_MASK_SET(0xff),
		 ANA_AC_PORT_STAT_CFG_CFG_PRIO_MASK,
		 sparx5, ANA_AC_PORT_STAT_CFG(portno, polidx));
	return 0;
}

int sparx5_policer_conf_set(struct sparx5 *sparx5,
			    struct sparx5_policer *pol)
{
	switch (pol->type) {
	case SPX5_POL_ACL:
		return sparx5_policer_acl_conf_set(sparx5, pol);
	case SPX5_POL_PORT:
		return sparx5_policer_port_conf_set(sparx5, pol);
	case SPX5_POL_SERVICE:
		return sparx5_policer_service_conf_set(sparx5, pol);
	default:
		break;
	}

	return 0;
}

int sparx5_policer_init(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;

	/* Setup global count events for acl policers.
	 * Count all discarded frames with unmasked event and no errors.
	 */
	u8 acl_event_mask = (SPX5_POL_ACL_STAT_CNT_CPU_DISCARDED |
			     SPX5_POL_ACL_STAT_CNT_FPORT_DISCADED);
	u8 frm_type = SPX5_POL_ACL_STAT_CNT_UNMASKED_NO_ERR;

	/* Configure discard policer (zero rate and burst; closed) */
	struct sparx5_policer pol = {
		.type = SPX5_POL_ACL,
		.idx =  consts->pol_acl_cnt - 1, /* last ACL policer */
	};
	u8 counter = 0;
	u32 value;

	/* Initialize all ACL and Port policers before usage */
	spx5_rmw(ANA_AC_POL_POL_ALL_CFG_ACL_FORCE_INIT_SET(1) |
		 ANA_AC_POL_POL_ALL_CFG_FORCE_INIT_SET(1),
		 ANA_AC_POL_POL_ALL_CFG_ACL_FORCE_INIT |
		 ANA_AC_POL_POL_ALL_CFG_FORCE_INIT,
		 sparx5, ANA_AC_POL_POL_ALL_CFG);

	/* Wait for policer initialization to complete */
	read_poll_timeout(spx5_rd, value,
			  !(ANA_AC_POL_POL_ALL_CFG_ACL_FORCE_INIT_GET(value) |
			  ANA_AC_POL_POL_ALL_CFG_FORCE_INIT_GET(value)),
			  500, 10000, false, sparx5, ANA_AC_POL_POL_ALL_CFG);

	spx5_rmw(ANA_AC_ACL_GLOBAL_CNT_FRM_TYPE_CFG_GLOBAL_CFG_CNT_FRM_TYPE_SET(frm_type),
		 ANA_AC_ACL_GLOBAL_CNT_FRM_TYPE_CFG_GLOBAL_CFG_CNT_FRM_TYPE,
		 sparx5, ANA_AC_ACL_GLOBAL_CNT_FRM_TYPE_CFG(counter));

	spx5_rmw(ANA_AC_ACL_STAT_GLOBAL_EVENT_MASK_GLOBAL_EVENT_MASK_SET(acl_event_mask),
		 ANA_AC_ACL_STAT_GLOBAL_EVENT_MASK_GLOBAL_EVENT_MASK,
		 sparx5, ANA_AC_ACL_STAT_GLOBAL_EVENT_MASK(counter));

	/* Configure 3 port policer counters */
	spx5_wr(SPX5_PORT_POLICER_0_FILTER_EVENT |
		SPX5_PORT_POLICER_1_FILTER_EVENT |
		SPX5_PORT_POLICER_2_FILTER_EVENT |
		SPX5_PORT_POLICER_3_FILTER_EVENT |
		SPX5_PORT_POLICER_0_PASS_EVENT |
		SPX5_PORT_POLICER_1_PASS_EVENT |
		SPX5_PORT_POLICER_2_PASS_EVENT |
		SPX5_PORT_POLICER_3_PASS_EVENT,
		sparx5, ANA_AC_PORT_SGE_CFG(SPX5_PORT_POLICER_ALL_COUNTER));
	spx5_wr(SPX5_PORT_POLICER_0_FILTER_EVENT |
		SPX5_PORT_POLICER_1_FILTER_EVENT |
		SPX5_PORT_POLICER_2_FILTER_EVENT |
		SPX5_PORT_POLICER_3_FILTER_EVENT,
		sparx5, ANA_AC_PORT_SGE_CFG(SPX5_PORT_POLICER_FILTER_COUNTER));
	spx5_wr(SPX5_PORT_POLICER_0_PASS_EVENT |
		SPX5_PORT_POLICER_1_PASS_EVENT |
		SPX5_PORT_POLICER_2_PASS_EVENT |
		SPX5_PORT_POLICER_3_PASS_EVENT,
		sparx5, ANA_AC_PORT_SGE_CFG(SPX5_PORT_POLICER_PASS_COUNTER));

	/* Reset port policer statistics */
	spx5_rmw(ANA_AC_STAT_RESET_RESET_SET(1),
		 ANA_AC_STAT_RESET_RESET,
		 sparx5, ANA_AC_STAT_RESET);

	/* Wait for policer statistics reset to complete */
	read_poll_timeout(spx5_rd, value,
			  !ANA_AC_STAT_RESET_RESET_GET(value),
			  500, 10000, false, sparx5, ANA_AC_STAT_RESET);

	return sparx5_policer_conf_set(sparx5, &pol);
}
