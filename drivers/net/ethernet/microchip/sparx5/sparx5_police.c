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

#define SPX5_BUM_POLICER_CNT 1024
#define SPX5_BUM_POLICER_LB_CNT 3
#define SPX5_BUM_POLICER_LB_OPEN 0xfff /* Bucket always open (no policing) */
#define SPX5_BUM_POLICER_LB_BURST_SIZE 2048

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

enum bum_granularity {
	BUM_GRANULARITY_0,
	BUM_GRANULARITY_1,
	BUM_GRANULARITY_2,
	BUM_GRANULARITY_3,
	BUM_GRANULARITY_MAX,
};

/* kbps */
static enum bum_granularity bum_rate_granularities[BUM_GRANULARITY_MAX] = {
	8193, 1024, 128, 16
};

/* kbps */
static u64 bum_rate_max[BUM_GRANULARITY_MAX] = {
	33600000, 4190000, 523000, 65000
};

/* Pool of available BUM policers */
static struct sparx5_pool_entry sparx5_bum_pool[SPX5_BUM_POLICER_CNT];

static int sparx5_policer_bum_get(struct sparx5 *sparx5, u32 *id)
{
	const struct sparx5_consts *consts = sparx5->data->consts;

	return sparx5_pool_get(sparx5_bum_pool, consts->bum_slb_cnt, id);
}

static int sparx5_policer_bum_put(struct sparx5 *sparx5, u32 id)
{
	const struct sparx5_consts *consts = sparx5->data->consts;

	return sparx5_pool_put(sparx5_bum_pool, consts->bum_slb_cnt, id);
}

void sparx5_policer_bum_init(struct sparx5 *sparx5)
{
	/* Each BUM traffic type can be separated using tc flower. However,
	 * known/unknown traffic within each type cannot. Therefore, we split
	 * each traffic type into its own bucket, so that no known/unknown
	 * traffic for the same traffic type, is policed by the same bucket.
	 * Using this technique, we can police known/unknown traffic type, by
	 * configuring and opening the corresponding buckets.
	 *
	 * Example:
	 *
	 * For unknown unicast, we configure bucket 2 with the specified
	 * rate and burst, and open bucket 0 and 1.
	 *
	 * Using the VCAP, we are already making sure that all but unicast
	 * traffic is being filtered. For any unicast traffic, known or
	 * unknown, that hits the BUM policer indexed by ISDX, only unknown
	 * unicast will be policed, since bucket 2 is configured, and
	 * bucket 0 is open.
	 */

	/* Bucket 0 is unknown bc and known uc */
	spx5_wr(SPX5_BUM_UNKNOWN_BROADCAST | SPX5_BUM_KNOWN_UNICAST,
		sparx5, ANA_AC_POL_SLB_TRAFFIC_MASK_CFG(0));

	/* Bucket 1 is unknown mc and known bc */
	spx5_wr(SPX5_BUM_UNKNOWN_MULTICAST | SPX5_BUM_KNOWN_BROADCAST,
		sparx5, ANA_AC_POL_SLB_TRAFFIC_MASK_CFG(1));

	/* Bucket 2 is unknown uc and known mc */
	spx5_wr(SPX5_BUM_UNKNOWN_UNICAST | SPX5_BUM_KNOWN_MULTICAST,
		sparx5, ANA_AC_POL_SLB_TRAFFIC_MASK_CFG(2));
}

static void sparx5_policer_bum_lb_conf_set(struct sparx5 *sparx5,
					   struct sparx5_policer *pol,
					   enum bum_granularity selector)
{
	u32 rate, burst, lb_idx;

	if (pol->event_mask & (SPX5_BUM_UNKNOWN_BROADCAST |
			       SPX5_BUM_KNOWN_UNICAST))
		lb_idx = 0;
	if (pol->event_mask & (SPX5_BUM_UNKNOWN_MULTICAST |
			       SPX5_BUM_KNOWN_BROADCAST))
		lb_idx = 1;
	if (pol->event_mask & (SPX5_BUM_UNKNOWN_UNICAST |
			       SPX5_BUM_KNOWN_MULTICAST))
		lb_idx = 2;

	for (int i = 0; i < SPX5_BUM_POLICER_LB_CNT; i++) {
		if (i == lb_idx) {
			/* Police frames in this bucket. */
			/* Burst = n * 2048 */
			burst = DIV_ROUND_UP(pol->burst,
					     SPX5_BUM_POLICER_LB_BURST_SIZE);

			/* Rate = n * rate granularity*/
			rate = DIV_ROUND_UP(pol->rate,
					    bum_rate_granularities[selector]);
		} else {
			/* Do not police frames in this bucket. */
			rate = SPX5_BUM_POLICER_LB_OPEN;
			burst = 0;
		}

		spx5_rmw(ANA_AC_POL_SLB_LB_CFG_RATE_VAL_SET(rate) |
			 ANA_AC_POL_SLB_LB_CFG_THRES_VAL_SET(burst),
			 ANA_AC_POL_SLB_LB_CFG_RATE_VAL |
			 ANA_AC_POL_SLB_LB_CFG_THRES_VAL,
			 sparx5, ANA_AC_POL_SLB_LB_CFG(pol->idx, i));
	}
}

static int sparx5_policer_bum_conf_set(struct sparx5 *sparx5,
				       struct sparx5_policer *pol, u32 isdx)
{
	bool enable = !!(pol->rate > 0);
	int selector = -1;

	/* Clear the per-ISDX BUM enable and idx fields and return.
	 * The stale HW state must not survive into the next allocator user.
	 */
	if (!enable) {
		spx5_rmw(ANA_L2_MISC_CFG_BUM_SLB_ENA_SET(0) |
			 ANA_L2_MISC_CFG_BUM_SLB_IDX_SET(0),
			 ANA_L2_MISC_CFG_BUM_SLB_ENA |
			 ANA_L2_MISC_CFG_BUM_SLB_IDX,
			 sparx5, ANA_L2_MISC_CFG(isdx));
		return 0;
	}

	/* Is the requested rate lower than the lowest rate granularity? */
	if (pol->rate < bum_rate_granularities[BUM_GRANULARITY_3])
		return -ERANGE;

	/* Get the granularity selector, or bail out if the rate is higher than
	 * the maximum rate.
	 */
	for (int i = 0; i < BUM_GRANULARITY_MAX; i++) {
		if (pol->rate > bum_rate_max[i])
			continue;
		selector = i;
	}

	if (selector < 0)
		return -ERANGE;

	/* Set rate and burst for the three leaky buckets. */
	sparx5_policer_bum_lb_conf_set(sparx5, pol, selector);

	/* Set rate granularity */
	spx5_rmw(ANA_AC_POL_SLB_SLB_CFG_TIMESCALE_VAL_SET(selector),
		 ANA_AC_POL_SLB_SLB_CFG_TIMESCALE_VAL,
		sparx5, ANA_AC_POL_SLB_SLB_CFG(pol->idx));

	/* Select BUM policer index and enable it. */
	spx5_rmw(ANA_L2_MISC_CFG_BUM_SLB_ENA_SET(enable) |
		 ANA_L2_MISC_CFG_BUM_SLB_IDX_SET(enable ? pol->idx : 0),
		 ANA_L2_MISC_CFG_BUM_SLB_ENA |
		 ANA_L2_MISC_CFG_BUM_SLB_IDX,
		 sparx5, ANA_L2_MISC_CFG(isdx));

	/* Set frame rate mode. */
	spx5_wr(0, sparx5, ANA_AC_POL_SLB_MISC_CFG(pol->idx));

	return 0;
}

int sparx5_policer_bum_id_get(struct sparx5 *sparx5, u32 isdx)
{
	u32 val;

	val = spx5_rd(sparx5, ANA_L2_MISC_CFG(isdx));

	return ANA_L2_MISC_CFG_BUM_SLB_IDX_GET(val);
}

int sparx5_policer_bum_add(struct sparx5 *sparx5, struct sparx5_policer *pol,
			   u32 *id)
{
	u32 isdx;
	int ret;

	ret = sparx5_policer_bum_get(sparx5, &pol->idx);
	if (ret < 0)
		return ret;

	ret = sparx5_isdx_get(sparx5, &isdx);
	if (ret < 0)
		return ret;

	*id = isdx;

	return sparx5_policer_bum_conf_set(sparx5, pol, isdx);
}

int sparx5_policer_bum_del(struct sparx5 *sparx5, u32 id)
{
	struct sparx5_policer pol = { 0 };
	u32 isdx = id;

	pol.idx = sparx5_policer_bum_id_get(sparx5, id);

	sparx5_policer_bum_put(sparx5, pol.idx);

	sparx5_isdx_put(sparx5, isdx);

	return sparx5_policer_bum_conf_set(sparx5, &pol, isdx);
}

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
	u32 idx, pup_tokens, max_pup_tokens, burst, thres;
	const struct sparx5_ops *ops = sparx5->data->ops;
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
	case SPX5_POL_BUM:
		return sparx5_policer_bum_conf_set(sparx5, pol, 0);
	default:
		break;
	}

	return 0;
}

void sparx5_policer_reset_counters(struct sparx5 *sparx5)
{
	int value;

	/* Reset port policer statistics */
	spx5_rmw(ANA_AC_STAT_RESET_RESET_SET(1),
		 ANA_AC_STAT_RESET_RESET,
		 sparx5, ANA_AC_STAT_RESET);

	/* Wait for policer statistics reset to complete */
	read_poll_timeout(spx5_rd, value,
			  !ANA_AC_STAT_RESET_RESET_GET(value),
			  500, 10000, false, sparx5, ANA_AC_STAT_RESET);
}

int sparx5_policer_init(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = sparx5->data->consts;

	/* Setup global count events for acl policers.
	 * Count all discarded frames with unmasked event and no errors.
	 */
	u8 acl_event_mask = (SPX5_POL_ACL_STAT_CNT_CPU_DISCARDED |
			     SPX5_POL_ACL_STAT_CNT_FPORT_DISCADED);
	u8 frm_type = SPX5_POL_ACL_STAT_CNT_UNMASKED_NO_ERR;

	/* Configure discard policer (zero rate and burst; closed) */
	struct sparx5_policer pol = {
		.type = SPX5_POL_ACL,
		.idx =  consts->n_pol_acl - 1, /* last ACL policer */
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

	return sparx5_policer_conf_set(sparx5, &pol);
}

static int sparx5_get_port_policer_idx(struct sparx5_port *port,
				       unsigned long cookie)
{
	int idx;

	/* Find the policer (cookie) */
	for (idx = 0; idx < SPX5_POLICERS_PER_PORT; ++idx)
		if (port->tc.port_policer[idx].policer == cookie)
			return idx;
	return -ENOENT;
}

static int sparx5_alloc_port_policer_idx(struct sparx5_port *port,
					 unsigned long cookie)
{
	int polidx;

	/* Check if the this policer (cookie) already exists */
	for (polidx = 0; polidx < SPX5_POLICERS_PER_PORT; ++polidx)
		if (port->tc.port_policer[polidx].policer == cookie)
			return polidx;
	/* Find a free port policer */
	for (polidx = 0; polidx < SPX5_POLICERS_PER_PORT; ++polidx)
		if (!port->tc.port_policer[polidx].policer)
			return polidx;
	return -ENOENT;
}

static int sparx5_free_port_policer_idx(struct sparx5_port *port,
					unsigned long cookie)
{
	int polidx;

	/* Find existing policer (cookie) */
	for (polidx = 0; polidx < SPX5_POLICERS_PER_PORT; ++polidx)
		if (port->tc.port_policer[polidx].policer == cookie)
			return polidx;
	return -ENOENT;
}

static void sparx5_init_port_policer_stats(struct sparx5_port *port,
					   struct sparx5_policer *pol,
					   int polidx)
{
	sparx5_policer_stats_update(port->sparx5, pol);
	port->tc.port_policer[polidx].prev.drops =
		port->tc.port_policer[polidx].stats.drops;
	port->tc.port_policer[polidx].prev.pkts =
		port->tc.port_policer[polidx].stats.pkts;
}

/* The policer only counts packets or bytes, not both */
static void sparx5_get_port_policer_stats(struct sparx5_port *port, int polidx)
{
	struct sparx5_policer pol = { 0 };

	pol.type = SPX5_POL_PORT;
	pol.idx = port->portno * SPX5_POLICERS_PER_PORT + polidx;
	sparx5_policer_stats_update(port->sparx5, &pol);
}

int sparx5_update_port_policer_stats(struct net_device *ndev,
				     struct tc_cls_matchall_offload *tmo)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct flow_stats *prev_stats;
	struct flow_stats *stats;
	int polidx;

	polidx = sparx5_get_port_policer_idx(port, tmo->cookie);
	if (polidx < 0)
		return polidx;
	sparx5_get_port_policer_stats(port, polidx);
	stats = &port->tc.port_policer[polidx].stats;
	prev_stats = &port->tc.port_policer[polidx].prev;
	if (stats->pkts == prev_stats->pkts)
		return 0;
	flow_stats_update(&tmo->stats, 0, stats->pkts - prev_stats->pkts,
			  stats->drops - prev_stats->drops,
			  prev_stats->lastused, FLOW_ACTION_HW_STATS_IMMEDIATE);
	prev_stats->pkts = stats->pkts;
	prev_stats->drops = stats->drops;
	prev_stats->lastused = jiffies;
	return 0;
}

int sparx5_add_port_policer(struct sparx5_mall_entry *entry)
{
	struct flow_action_entry *action = &entry->port_policer.action;
	struct sparx5_port *port = entry->port;
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_policer pol = { 0 };
	int idx, err;

	if (!entry->ingress)
		return -EINVAL;

	if (entry->port->tc.block_shared[1])
		return -ENOKEY;

	if (action->police.exceed.act_id != FLOW_ACTION_DROP)
		return -ENOSYS;

	if (action->police.notexceed.act_id != FLOW_ACTION_PIPE &&
	    action->police.notexceed.act_id != FLOW_ACTION_ACCEPT)
		return -EOPNOTSUPP;

	if (action->police.peakrate_bytes_ps || action->police.avrate ||
	    action->police.overhead)
		return -EOPNOTSUPP;

	if (action->police.rate_pkt_ps)
		return -EOPNOTSUPP;

	pr_info("%s:%d: %s cookie: %lu\n", __func__, __LINE__,
		netdev_name(port->ndev), entry->cookie);

	idx = sparx5_alloc_port_policer_idx(port, entry->cookie);
	if (idx < 0)
		return idx;

	pol.type = SPX5_POL_PORT;
	pol.rate = action->police.rate_bytes_ps * 8;
	pol.burst = action->police.burst;
	pol.idx = port->portno * SPX5_POLICERS_PER_PORT + idx;

	err = sparx5_policer_conf_set(sparx5, &pol);
	if (err)
		return err;

	pr_info("%s:%d: %s: added policer %d\n", __func__, __LINE__,
		netdev_name(port->ndev), idx);

	port->tc.port_policer[idx].policer = entry->cookie;
	sparx5_init_port_policer_stats(port, &pol, idx);

	return 0;
}

int sparx5_delete_port_policer(struct sparx5_mall_entry *entry)
{
	struct sparx5_port *port = entry->port;
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_policer pol = { 0 };
	int idx, err;

	if (!entry->ingress)
		return -EINVAL;
	if (entry->port->tc.block_shared[1])
		return -EOPNOTSUPP;

	pr_debug("%s:%d: %s cookie: %lu\n", __func__, __LINE__,
		 netdev_name(port->ndev), entry->cookie);

	idx = sparx5_free_port_policer_idx(port, entry->cookie);
	if (idx < 0)
		return idx;

	pol.type = SPX5_POL_PORT;
	pol.idx = port->portno * SPX5_POLICERS_PER_PORT + idx;

	err = sparx5_policer_conf_set(sparx5, &pol);
	if (err)
		return err;

	pr_debug("%s:%d: %s: deleted policer %d\n", __func__, __LINE__,
		 netdev_name(port->ndev), idx);

	port->tc.port_policer[idx].policer = 0;

	return 0;
}
