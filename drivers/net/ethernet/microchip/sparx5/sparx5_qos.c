/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2022 Microchip Technology Inc. */

#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include "sparx5_port.h"
#include "sparx5_qos.h"
#include "sparx5_qos_debugfs.h"
#include <linux/iopoll.h>
#include <linux/genetlink.h>
#include <net/pkt_cls.h>

enum sparx5_ver_tmr_units {
	SPX5_VTU_1MS_AT_1G = 0,      /* 1 ms (Default) at 1G/100M/10M speeds */
	SPX5_VTU_1040NS_AT_1G = 1,   /* 1040 ns = 1.04 us at 1G/100M/10M speeds */
	SPX5_VTU_1MS_AT_2G5 = 2,     /* 1 ms time at 2.5G speeds */
	SPX5_VTU_1001NS_AT_2G5 = 3,  /* 1001.6ns = 1.0016 us time at 2.5G speeds */
};

/*******************************************************************************
 * QOS Port configuration
 ******************************************************************************/
int sparx5_qos_port_conf_get(const struct sparx5_port *const port,
							 struct mchp_qos_port_conf *const conf)
{
	*conf = port->qos_port_conf;
	return 0;
}

int sparx5_qos_port_conf_set(struct sparx5_port *const port,
							 struct mchp_qos_port_conf *const conf)
{
	u32 pcp, dei, prio, dpl, retval = 0, e_mode;

	/* Setup port ingress default DEI and PCP */
	spx5_rmw(ANA_CL_VLAN_CTRL_PORT_PCP_SET(conf->i_default_pcp) |
			 ANA_CL_VLAN_CTRL_PORT_DEI_SET(conf->i_default_dei),
			 ANA_CL_VLAN_CTRL_PORT_PCP |
			 ANA_CL_VLAN_CTRL_PORT_DEI,
			 port->sparx5,
			 ANA_CL_VLAN_CTRL(port->portno));

	/* Setup port ingress default DPL and Priority */
	spx5_rmw(ANA_CL_QOS_CFG_DEFAULT_QOS_VAL_SET(conf->i_default_prio) |
			 ANA_CL_QOS_CFG_DEFAULT_DP_VAL_SET(conf->i_default_dpl) |
			 ANA_CL_QOS_CFG_PCP_DEI_QOS_ENA_SET(conf->i_mode.tag_map_enable) |
			 ANA_CL_QOS_CFG_DSCP_QOS_ENA_SET(conf->i_mode.dscp_map_enable),
			 ANA_CL_QOS_CFG_DEFAULT_QOS_VAL |
			 ANA_CL_QOS_CFG_DEFAULT_DP_VAL |
			 ANA_CL_QOS_CFG_PCP_DEI_QOS_ENA |
			 ANA_CL_QOS_CFG_DSCP_QOS_ENA,
			 port->sparx5,
			 ANA_CL_QOS_CFG(port->portno));

	/* Setup port ingress mapping between [PCP,DEI] and [Priority]. */
	/* Setup port ingress mapping between [PCP,DEI] and [DPL]. */
	for (pcp = 0; pcp < PCP_COUNT; pcp++) {
		for (dei = 0; dei < DEI_COUNT; dei++) {
			prio = conf->i_pcp_dei_prio_dpl_map[pcp][dei].prio;
			dpl = conf->i_pcp_dei_prio_dpl_map[pcp][dei].dpl;

			spx5_rmw(ANA_CL_PCP_DEI_MAP_CFG_PCP_DEI_DP_VAL_SET(dpl) |
					 ANA_CL_PCP_DEI_MAP_CFG_PCP_DEI_QOS_VAL_SET(prio),
					 ANA_CL_PCP_DEI_MAP_CFG_PCP_DEI_DP_VAL |
					 ANA_CL_PCP_DEI_MAP_CFG_PCP_DEI_QOS_VAL,
					 port->sparx5,
					 ANA_CL_PCP_DEI_MAP_CFG(port->portno, (8 * dei + pcp)));
		}
	}

	dei = (conf->e_mode == MCHP_E_MODE_DEFAULT ? conf->e_default_dei : 0);
	/* Setup port egress default DEI and PCP */
	spx5_rmw(REW_PORT_VLAN_CFG_PORT_PCP_SET(conf->e_default_pcp) |
			 REW_PORT_VLAN_CFG_PORT_DEI_SET(dei),
			 REW_PORT_VLAN_CFG_PORT_PCP |
			 REW_PORT_VLAN_CFG_PORT_DEI,
			 port->sparx5,
			 REW_PORT_VLAN_CFG(port->portno));

	/* Setup port egress mapping between [Priority] and [PCP,DEI]. */
	/* Setup port egress mapping between [DPL] and [PCP,DEI]. */
	for (prio = 0; prio < PRIO_COUNT; prio++) {
			pcp = conf->e_prio_dpl_pcp_dei_map[prio][0].pcp;
			spx5_rmw(REW_PCP_MAP_DE0_PCP_DE0_SET(pcp),
					 REW_PCP_MAP_DE0_PCP_DE0,
					 port->sparx5,
					 REW_PCP_MAP_DE0(port->portno, prio));

			pcp = conf->e_prio_dpl_pcp_dei_map[prio][1].pcp;
			spx5_rmw(REW_PCP_MAP_DE1_PCP_DE1_SET(pcp),
					 REW_PCP_MAP_DE1_PCP_DE1,
					 port->sparx5,
					 REW_PCP_MAP_DE1(port->portno, prio));

			dei = conf->e_prio_dpl_pcp_dei_map[prio][0].dei;
			spx5_rmw(REW_DEI_MAP_DE0_DEI_DE0_SET(pcp),
					 REW_DEI_MAP_DE0_DEI_DE0,
					 port->sparx5,
					 REW_DEI_MAP_DE0(port->portno, prio));

			dei = conf->e_prio_dpl_pcp_dei_map[prio][1].dei;
			spx5_rmw(REW_DEI_MAP_DE1_DEI_DE1_SET(pcp),
					 REW_DEI_MAP_DE1_DEI_DE1,
					 port->sparx5,
					 REW_DEI_MAP_DE1(port->portno, prio));
	}

	/* Setup the egress TAG PCP,DEI generation mode */
	switch (conf->e_mode) {
	case MCHP_E_MODE_DEFAULT:
		e_mode = 1; /* PORT_PCP/PORT_DEI */
	break;
	case MCHP_E_MODE_MAPPED:
		e_mode = 2; /* MAPPED */
	break;
	default:
		e_mode = 0; /* Classified PCP/DEI */
	break;
	}
	spx5_rmw(REW_TAG_CTRL_TAG_PCP_CFG_SET(e_mode) |
			 REW_TAG_CTRL_TAG_DEI_CFG_SET(e_mode),
			 REW_TAG_CTRL_TAG_PCP_CFG |
			 REW_TAG_CTRL_TAG_DEI_CFG,
			 port->sparx5,
			 REW_TAG_CTRL(port->portno));

	port->qos_port_conf = *conf;
	return retval;
}

/*******************************************************************************
 * FP (Frame Preemption - 802.1Qbu/802.3br)
 ******************************************************************************/
static void sparx5_fp_enable(struct sparx5_port *port, struct sparx5_fp_port_conf *c, bool enable_tx)
{
	u32 unit, val, i;

	switch (port->conf.speed) {
	case SPEED_10:
	case SPEED_100:
	case SPEED_1000:
	case SPEED_5000:
		unit = SPX5_VTU_1MS_AT_1G;
		break;
	default:
		unit = SPX5_VTU_1MS_AT_2G5;
		break;
	}

	SPX5_DEV_WR(DEV2G5_VERIF_CONFIG_PRM_VERIFY_DIS_SET(c->verify_disable_tx) |
				DEV2G5_VERIF_CONFIG_PRM_VERIFY_TIME_SET(c->verify_time) |
				DEV2G5_VERIF_CONFIG_VERIF_TIMER_UNITS_SET(unit),
				port,
				VERIF_CONFIG);

	spx5_wr(DSM_PREEMPT_CFG_P_MIN_SIZE_SET(c->add_frag_size),
			port->sparx5,
			DSM_PREEMPT_CFG(port->portno));

	spx5_wr(DSM_IPG_SHRINK_CFG_IPG_SHRINK_ENA_SET(enable_tx),
			port->sparx5,
			DSM_IPG_SHRINK_CFG(port->portno));

	for (i = 0; i < 8; i++) {
		int idx = sparx5_hsch_l0_get_idx(port->sparx5, port->portno, i);
		/* Set queue to be express (0) or preemtable (1) */
		val = (enable_tx && (c->admin_status & BIT(i))) ? 0xff : 0;
		spx5_rmw(HSCH_HSCH_L0_CFG_P_QUEUES_SET(val),
			HSCH_HSCH_L0_CFG_P_QUEUES,
			port->sparx5,
			HSCH_HSCH_L0_CFG(idx));

		/* Force update of an element  */
		spx5_wr(HSCH_HSCH_FORCE_CTRL_HFORCE_LAYER_SET(0) |
			HSCH_HSCH_FORCE_CTRL_HFORCE_SE_IDX_SET(idx) |
			HSCH_HSCH_FORCE_CTRL_HFORCE_1SHOT_SET(1), port->sparx5,
			HSCH_HSCH_FORCE_CTRL);
	}
}

static void sparx5_fp_update(struct sparx5_port *port, struct sparx5_fp_port_conf *c)
{
	const struct sparx5_ops *ops = &port->sparx5->data->ops;

	if (ops->port_is_rgmii(port->portno))
		return;

	if (c->enable_tx &&
		netif_carrier_ok(port->ndev) &&
		(port->conf.speed >= SPEED_100) &&
		(port->conf.duplex == DUPLEX_FULL))
		sparx5_fp_enable(port, c, true);
	else
		sparx5_fp_enable(port, c, false);
}

int sparx5_fp_set(struct sparx5_port *port,
		   struct sparx5_fp_port_conf *c)
{
	bool unlock = false;

	netdev_dbg(port->ndev,
		   "sparx5_fp_set() as %u et %d vdt %d vt %u afs %u\n",
		   c->admin_status,
		   c->enable_tx,
		   c->verify_disable_tx,
		   c->verify_time,
		   c->add_frag_size);

	if ((c->verify_time < 1) || (c->verify_time > 128)) {
		netdev_err(port->ndev, "Invalid verify_time (%u)\n",
			   c->verify_time);
		return -EINVAL;
	}

	if (c->add_frag_size > 3) {
		netdev_err(port->ndev, "Invalid add_frag_size (%u)\n",
			   c->add_frag_size);
		return -EINVAL;
	}

	/* Manually take rtnl_lock() if it isn't locked.
	 * This can be removed later when frame preemption is controlled by
	 * a standard user space tool such as ethtool, which aquires the lock.
	 */
	if (!rtnl_is_locked()) {
		unlock = true;
		rtnl_lock();
	}

	sparx5_fp_update(port, c);
	port->fp = *c;
	if (unlock)
		rtnl_unlock();
	return 0;
}

int sparx5_fp_get(struct sparx5_port *port,
		   struct sparx5_fp_port_conf *c)
{
	bool unlock = false;

	if (!rtnl_is_locked()) {
		unlock = true;
		rtnl_lock();
	}

	*c = port->fp;

	if (unlock)
		rtnl_unlock();
	return 0;
}

int sparx5_fp_status(struct sparx5_port *port,
			struct mchp_qos_fp_port_status *s)
{
	bool unlock = false;
	u32 status, v;

	if (!rtnl_is_locked()) {
		unlock = true;
		rtnl_lock();
	}

	SPX5_DEV_RD(status, port, MM_STATUS);
	s->preemption_active = !!DEV2G5_MM_STATUS_PRMPT_ACTIVE_STATUS_GET(status);

	if (port->fp.verify_disable_tx) {
		v = MCHP_MM_STATUS_VERIFY_DISABLED;
	} else {
		v = DEV2G5_MM_STATUS_PRMPT_VERIFY_STATE_GET(status);
		/* DEV2G5 does not support full state */
		v = sparx5_is_baser(port->conf.portmode) || v == 0 ? v : (v + 2);
	}

	s->status_verify = v;

	if (unlock)
		rtnl_unlock();

	return 0;
}

static void sparx5_fp_init(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	const struct sparx5_ops *ops = &sparx5->data->ops;
	struct sparx5_port *port;
	void __iomem *devinst;
	u32 val, pix, dev;
	int p;

	/* Initialize frame-preemption and sync config with defaults */
	for (p = 0; p < consts->chip_ports; p++) {
		port = sparx5->ports[p];
		if (!port || (port && ops->port_is_rgmii(port->portno)))
			continue;

		/* Always enable MAC-MERGE Layer block, queue controls FP */
		SPX5_DEV_WR(DEV2G5_ENABLE_CONFIG_MM_RX_ENA_SET(1) |
					DEV2G5_ENABLE_CONFIG_MM_TX_ENA_SET(1) |
					DEV2G5_ENABLE_CONFIG_KEEP_S_AFTER_D_SET(0),
					port,
					ENABLE_CONFIG);

		SPX5_DEV_RMW(DEV10G_DEV_PFRAME_CFG_DEV_FRAGMENT_IFG_SET(12),
					 DEV10G_DEV_PFRAME_CFG_DEV_FRAGMENT_IFG,
					 port,
					 DEV_PFRAME_CFG);

		if (sparx5_is_baser(port->conf.portmode)) {
			pix = sparx5_port_dev_index(sparx5, port->portno);
			dev = sparx5_to_high_dev(sparx5, port->portno);
			devinst = spx5_inst_get(port->sparx5, dev, pix);
			spx5_inst_rmw(DEV10G_MAC_ADV_CHK_CFG_SFD_CHK_ENA_SET(0),
						  DEV10G_MAC_ADV_CHK_CFG_SFD_CHK_ENA,
						  devinst,
						  DEV10G_MAC_ADV_CHK_CFG(0));
		}

		SPX5_DEV_RMW(DEV2G5_VERIF_CONFIG_PRM_VERIFY_DIS_SET(1),
					 DEV2G5_VERIF_CONFIG_PRM_VERIFY_DIS,
					 port,
					 VERIF_CONFIG);

		port->fp.admin_status = 0;

		SPX5_DEV_RD(val, port, ENABLE_CONFIG);
		port->fp.enable_tx = false;

		SPX5_DEV_RD(val, port, VERIF_CONFIG);
		port->fp.verify_disable_tx = true;
		port->fp.verify_time = DEV2G5_VERIF_CONFIG_PRM_VERIFY_TIME_GET(val);

		val = spx5_rd(port->sparx5, DSM_PREEMPT_CFG(port->portno));
		port->fp.add_frag_size = DSM_PREEMPT_CFG_P_MIN_SIZE_GET(val);
	}
}

/*******************************************************************************
 * QoS port notification
 ******************************************************************************/
int sparx5_qos_port_event(struct net_device *dev, unsigned long event)
{
	struct sparx5_port *port;

	if (sparx5_netdevice_check(dev)) {
		port = netdev_priv(dev);
		switch (event) {
		case NETDEV_DOWN:
		case NETDEV_CHANGE:
			sparx5_fp_update(port, &port->fp);
			break;
		default:
			/* Nothing */
			break;
		}
	}
	return NOTIFY_DONE;
}

void sparx5_update_u64_counter(u64 *cntr, u32 msb, u32 lsb)
{
	*cntr = (u64)lsb;
	*cntr |= (u64)msb << 32;
}

struct sparx5_layer sparx5_layers[SPX5_HSCH_LAYER_CNT];

static u32 sparx5_lg_get_leak_time(struct sparx5 *sparx5, u32 layer, u32 group)
{
	u32 value;

	value = spx5_rd(sparx5, HSCH_HSCH_TIMER_CFG(layer, group));
	return HSCH_HSCH_TIMER_CFG_LEAK_TIME_GET(value);
}

static void sparx5_lg_set_leak_time(struct sparx5 *sparx5, u32 layer, u32 group,
				    u32 leak_time)
{
	spx5_wr(HSCH_HSCH_TIMER_CFG_LEAK_TIME_SET(leak_time), sparx5,
		HSCH_HSCH_TIMER_CFG(layer, group));
}

u32 sparx5_lg_get_first(struct sparx5 *sparx5, u32 layer, u32 group)
{
	u32 value;

	value = spx5_rd(sparx5, HSCH_HSCH_LEAK_CFG(layer, group));
	return HSCH_HSCH_LEAK_CFG_LEAK_FIRST_GET(value);
}

u32 sparx5_lg_get_next(struct sparx5 *sparx5, u32 layer, u32 group,
			      u32 idx)

{
	u32 value;

	value = spx5_rd(sparx5, HSCH_SE_CONNECT(idx));
	return HSCH_SE_CONNECT_SE_LEAK_LINK_GET(value);
}

static u32 sparx5_lg_get_last(struct sparx5 *sparx5, u32 layer, u32 group)
{
	u32 itr, next;

	itr = sparx5_lg_get_first(sparx5, layer, group);

	for (;;) {
		next = sparx5_lg_get_next(sparx5, layer, group, itr);
		if (itr == next)
			return itr;

		itr = next;
	}
}

static bool sparx5_lg_is_last(struct sparx5 *sparx5, u32 layer, u32 group,
			      u32 idx)
{
	return idx == sparx5_lg_get_next(sparx5, layer, group, idx);
}

static bool sparx5_lg_is_first(struct sparx5 *sparx5, u32 layer, u32 group,
			       u32 idx)
{
	return idx == sparx5_lg_get_first(sparx5, layer, group);
}

bool sparx5_lg_is_empty(struct sparx5 *sparx5, u32 layer, u32 group)
{
	return sparx5_lg_get_leak_time(sparx5, layer, group) == 0;
}

static bool sparx5_lg_is_singular(struct sparx5 *sparx5, u32 layer, u32 group)
{
	if (sparx5_lg_is_empty(sparx5, layer, group))
		return false;

	return sparx5_lg_get_first(sparx5, layer, group) ==
	       sparx5_lg_get_last(sparx5, layer, group);
}

static void sparx5_lg_enable(struct sparx5 *sparx5, u32 layer, u32 group,
			     u32 leak_time)
{
	sparx5_lg_set_leak_time(sparx5, layer, group, leak_time);
}

static void sparx5_lg_disable(struct sparx5 *sparx5, u32 layer, u32 group)
{
	sparx5_lg_set_leak_time(sparx5, layer, group, 0);
}

static int sparx5_lg_get_group_by_index(struct sparx5 *sparx5, u32 layer,
					u32 idx, u32 *group)
{
	u32 itr, next;
	int i;

	for (i = 0; i < SPX5_HSCH_LEAK_GRP_CNT; i++) {
		if (sparx5_lg_is_empty(sparx5, layer, i))
			continue;

		itr = sparx5_lg_get_first(sparx5, layer, i);

		for (;;) {
			next = sparx5_lg_get_next(sparx5, layer, i, itr);

			if (itr == idx) {
				*group = i;
				return 0; /* Found it */
			}
			if (itr == next)
				break; /* Was not found */

			itr = next;
		}
	}

	return -1;
}

static int sparx5_lg_get_group_by_rate(u32 layer, u32 rate, u32 *group)
{
	struct sparx5_layer *l = &sparx5_layers[layer];
	struct sparx5_lg *lg;
	u32 i;

	for (i = 0; i < SPX5_HSCH_LEAK_GRP_CNT; i++) {
		lg = &l->leak_groups[i];
		if (rate <= lg->max_rate) {
			*group = i;
			return 0;
		}
	}

	return -1;
}

static int sparx5_lg_get_adjacent(struct sparx5 *sparx5, u32 layer, u32 group,
				  u32 idx, u32 *prev, u32 *next, u32 *first)
{
	u32 itr;

	*first = sparx5_lg_get_first(sparx5, layer, group);
	*prev = *first;
	*next = *first;
	itr = *first;

	for (;;) {
		*next = sparx5_lg_get_next(sparx5, layer, group, itr);

		if (itr == idx)
			return 0; /* Found it */

		if (itr == *next)
			return -1; /* Was not found */

		*prev = itr;
		itr = *next;
	}

	return -1;
}

static int sparx5_lg_conf_set(struct sparx5 *sparx5, u32 layer, u32 group,
			      u32 se_first, u32 idx, u32 idx_next, bool empty)
{
	u32 leak_time = sparx5_layers[layer].leak_groups[group].leak_time;

	/* Stop leaking */
	sparx5_lg_disable(sparx5, layer, group);

	if (empty)
		return 0;

	/* Select layer */
	spx5_rmw(HSCH_HSCH_CFG_CFG_HSCH_LAYER_SET(layer),
		 HSCH_HSCH_CFG_CFG_HSCH_LAYER, sparx5, HSCH_HSCH_CFG_CFG);

	/* Link elements */
	spx5_wr(HSCH_SE_CONNECT_SE_LEAK_LINK_SET(idx_next), sparx5,
		HSCH_SE_CONNECT(idx));

	/* Set the first element. */
	spx5_rmw(HSCH_HSCH_LEAK_CFG_LEAK_FIRST_SET(se_first),
		 HSCH_HSCH_LEAK_CFG_LEAK_FIRST, sparx5,
		 HSCH_HSCH_LEAK_CFG(layer, group));

	/* Start leaking */
	sparx5_lg_enable(sparx5, layer, group, leak_time);

	return 0;
}

/*******************************************************************************
 * QoS TAS Initialization
 ******************************************************************************/

#define P(X, Y) \
	seq_printf(m, "%-20s: %12d\n", X, Y)
#define P_STR(X, Y) \
	seq_printf(m, "%-20s: %12s\n", X, Y)
#define P_TIME(X, S, NS) \
	seq_printf(m, "%-20s: %12llu.%09llu sec\n",\
		   X, (unsigned long long)S, (unsigned long long)NS)

/* Calculate new base_time based on cycle_time.
 *
 * The hardware requires a base_time that is always in the future.
 * We define threshold_time as current_time + (2 * cycle_time).
 * If base_time is below threshold_time this function recalculates it to be in
 * the interval:
 * threshold_time <= base_time < (threshold_time + cycle_time)
 *
 * A very simple algorithm could be like this:
 * new_base_time = org_base_time + N * cycle_time
 * using the lowest N so (new_base_time >= threshold_time
 *
 * The algorithm has been optimized as the above code is extremely slow.
 *
 * sparx5 [IN] Target instance reference.
 * cycle_time [IN] In nanoseconds.
 * org_base_time [IN] Original base time.
 * new_base_time [OUT] New base time.
 */
void sparx5_new_base_time(struct sparx5 *sparx5, const u32 cycle_time,
			  const ktime_t org_base_time, ktime_t *new_base_time)
{
	ktime_t current_time, threshold_time, new_time;
	struct timespec64 ts;
	u64 nr_of_cycles_p2;
	u64 nr_of_cycles;
	u64 diff_time;

	new_time = org_base_time;

	sparx5_ptp_gettime64(&sparx5->phc[SPARX5_PHC_PORT].info,
			     &ts);
	current_time = timespec64_to_ktime(ts);
	threshold_time = current_time + (2 * cycle_time);
	diff_time = threshold_time - new_time;
	nr_of_cycles = div_u64(diff_time, cycle_time);
	nr_of_cycles_p2 = 1; /* Use 2^0 as start value */

	if (new_time >= threshold_time) {
		*new_base_time = new_time;
		dev_dbg(sparx5->dev,
			"\nUNCHANGED!\n"
			"cycle_time     %20u\n"
			"org_base_time  %20lld\n"
			"cur_time       %20lld\n"
			"threshold_time %20lld\n"
			"new_base_time  %20lld\n",
			cycle_time, org_base_time, current_time,
			threshold_time, new_time);
		return;
	}

	/* Calculate the smallest power of 2 (nr_of_cycles_p2)
	 * that is larger than nr_of_cycles.
	 */
	while (nr_of_cycles_p2 < nr_of_cycles)
		nr_of_cycles_p2 <<= 1; /* Next (higher) power of 2 */

	/* Add as big chunks (power of 2 * cycle_time)
	 * as possible for each power of 2
	 */
	while (nr_of_cycles_p2) {
		if (new_time < threshold_time) {
			new_time += cycle_time * nr_of_cycles_p2;
			while (new_time < threshold_time)
				new_time += cycle_time * nr_of_cycles_p2;
			new_time -= cycle_time * nr_of_cycles_p2;
		}
		nr_of_cycles_p2 >>= 1; /* Next (lower) power of 2 */
	}
	new_time += cycle_time;
	*new_base_time = new_time;

	dev_dbg(sparx5->dev,
		"\nCHANGED!\n"
		"cycle_time     %20u\n"
		"org_base_time  %20lld\n"
		"cur_time       %20lld\n"
		"threshold_time %20lld\n"
		"new_base_time  %20lld\n"
		"nr_of_cycles   %20lld\n",
		cycle_time, org_base_time, current_time, threshold_time,
		new_time, nr_of_cycles);
}

/*******************************************************************************
 * TAS (Time Aware Shaper - 802.1Qbv)
 ******************************************************************************/
/* Maximum time in milliseconds to wait for TAS state transitions */
#define TAS_TIMEOUT_MS 1000

/* Minimum supported cycle time in nanoseconds */
#define TAS_MIN_CYCLE_TIME_NS (1 * NSEC_PER_USEC) /* 1 usec */

/* Maximum supported cycle time in nanoseconds */
#define TAS_MAX_CYCLE_TIME_NS ((1 * NSEC_PER_SEC) - 1) /* 999.999.999 nsec */

/* TAS link speeds for calculation of guard band: */
enum sparx5_tas_link_speed {
	TAS_SPEED_NO_GB,
	TAS_SPEED_10,
	TAS_SPEED_100,
	TAS_SPEED_1000,
	TAS_SPEED_2500,
	TAS_SPEED_5000,
	TAS_SPEED_10000,
	TAS_SPEED_25000,
};

#define TAS_NUM_GCL		10000 /* Total number of TAS GCL entries */

/* We use 2 TAS list entries per port:
 * num_tas_lists = num_ports * SPX5_TAS_ENTRIES_PER_PORT;
 *
 * The index for the 2 entries per port is calculated as:
 * index_1 = portno * SPX5_TAS_ENTRIES_PER_PORT;
 * index_2 = index_1 + 1;
 *
 * GCL entries are allocated from a free list which is generated each time we
 * want to add a new schedule.
 *
 * The free list is organized as a bitmap with one bit for each GCL entry.
 * All bits in the free list are first set to 1 (free) and then all schedules
 * with state != SPX5_TAS_STATE_ADMIN are examined.
 * The GCL entries in use are removed from the free list by setting the
 * coresponding bit to zero.
 *
 * We use 1 TAS profile per port:
 * index = portno;
 *
 * We do not yet support frame preemption and the default guard band
 * of 1536 bytes is always used on all queues.
 *
 * These are the possible combinations of states for the two TAS list in a
 * running system (the temporary states ADVANCING and TERMINATING are not
 * considered here):
 *
 * ADMIN/ADMIN: No schedules are currently operating or pending.
 *
 * ADMIN/PENDING: No schedule are currently operating but one is pending and
 * when current time exceeds the configured base time it will automatically
 * enter the OPERATING state. The state is now ADMIN/OPERATING.
 *
 * ADMIN/OPERATING: A schedule is currently operating and will run until it is
 * terminated manually.
 *
 * OPERATING/PENDING: A schedule is currently operating and another is pending.
 * When current time exceeds the configured base time for the pending schedule
 * it will automatically stop the operating schedule and enter the OPERATING
 * state. The state is now ADMIN/OPERATING.
 *
 * When we want to disable TAS on a port, we must stop schedules that are in
 * state PENDING or OPERATING.
 * Pending schedules are stopped first and then operating schedules.
 * The gate state for the port must be restored to "all-queues-open" manually
 * in case the schedule was stopped with one or more of the queues closed.
 *
 * When we add a schedule we always use a base time in the future, where base
 * time is at least current time + (2 * cycle time). This is a requirement from
 * the hardware. This means that a schedule will always start in state PENDING.
 * It also means that we always use the new schedule to stop an eventually
 * operating schedule.
 *
 * When we want to add a new schedule we must consider the current state of the
 * two TAS list entries:
 *
 * ADMIN/ADMIN: Just add the new schedule in one of the TAS lists.
 *
 * ADMIN/PENDING: Stop the current pending schedule and add the new one.
 *
 * ADMIN/OPERATING: Add the new schedule in the TAS list that is currently in
 * admin state and configure it to stop the currently operating schedule when
 * current time exceeds the configured base time in the new schedule.
 *
 * OPERATING/PENDING: Stop the current pending schedule, add the new one and
 * configure it to stop the currently operating schedule when
 * current time exceeds the configured base time in the new schedule.
 *
 */

int sparx5_tas_scheduler_get(struct sparx5 *sparx5, int portno)
{
	if (is_sparx5(sparx5))
		return 5040 + 64 + portno;
	else
		return 1120 + portno;
}

int sparx5_tas_list_index(struct sparx5_port *port, u8 tas_entry)
{
	const struct sparx5_consts *consts = &port->sparx5->data->consts;
	int portno, pidx = 0;

	/* Limit the index to available ports */
	for (portno = 0; portno < consts->chip_ports; ++portno) {
		if (port->sparx5->ports[portno])
			pidx++;
		if (portno == port->portno)
			return (pidx * SPX5_TAS_ENTRIES_PER_PORT) + tas_entry;
	}
	return 0;
}

char *sparx5_tas_state_to_str(int state)
{
	switch (state) {
	case SPX5_TAS_STATE_ADMIN:
		return "ADMIN";
	case SPX5_TAS_STATE_ADVANCING:
		return "ADVANCING";
	case SPX5_TAS_STATE_PENDING:
		return "PENDING";
	case SPX5_TAS_STATE_OPERATING:
		return "OPERATING";
	case SPX5_TAS_STATE_TERMINATING:
		return "TERMINATING";
	default:
		return "??";
	}
}

static int sparx5_tas_shutdown_pending(struct sparx5_port *port)
{
	struct sparx5 *sparx5 = port->sparx5;
	int i, list, state;
	unsigned long end;
	u32 val;

	netdev_dbg(port->ndev, "portno %u\n", port->portno);
	for (i = 0; i < SPX5_TAS_ENTRIES_PER_PORT; i++) {
		list = sparx5_tas_list_index(port, i);
		spx5_rmw(HSCH_TAS_CFG_CTRL_LIST_NUM_SET(list),
			 HSCH_TAS_CFG_CTRL_LIST_NUM,
			 sparx5,
			 HSCH_TAS_CFG_CTRL);

		val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
		state = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
		if (state != SPX5_TAS_STATE_ADVANCING &&
		    state != SPX5_TAS_STATE_PENDING)
			continue;

		netdev_dbg(port->ndev, "state %s found in list %d\n",
			   sparx5_tas_state_to_str(state), list);

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			spx5_rmw(HSCH_TAS_LIST_STATE_LIST_STATE_SET(SPX5_TAS_STATE_ADMIN),
				 HSCH_TAS_LIST_STATE_LIST_STATE,
				 sparx5,
				 HSCH_TAS_LIST_STATE);

			val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
			state = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
			if (state == SPX5_TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != SPX5_TAS_STATE_ADMIN) {
			netdev_err(port->ndev,
				   "Timeout switching TAS state %s in list %d\n",
				   sparx5_tas_state_to_str(state), list);
			return -ETIME;
		}
	}
	return 0;
}

static int sparx5_tas_shutdown_operating(struct sparx5_port *port)
{
	struct sparx5 *sparx5 = port->sparx5;
	int i, list, state;
	unsigned long end;
	u32 val, sched;

	sched = sparx5_tas_scheduler_get(sparx5, port->portno);

	netdev_dbg(port->ndev, "portno %u\n", port->portno);
	for (i = 0; i < SPX5_TAS_ENTRIES_PER_PORT; i++) {
		list = sparx5_tas_list_index(port, i);
		spx5_rmw(HSCH_TAS_CFG_CTRL_LIST_NUM_SET(list),
			 HSCH_TAS_CFG_CTRL_LIST_NUM,
			 sparx5,
			 HSCH_TAS_CFG_CTRL);

		val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
		state = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
		if (state != SPX5_TAS_STATE_OPERATING)
			continue;

		netdev_dbg(port->ndev, "state %s found in list %d\n",
			   sparx5_tas_state_to_str(state), list);

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			spx5_rmw(HSCH_TAS_LIST_STATE_LIST_STATE_SET(SPX5_TAS_STATE_TERMINATING),
				 HSCH_TAS_LIST_STATE_LIST_STATE,
				 sparx5,
				 HSCH_TAS_LIST_STATE);

			val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
			state = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
			if (state == SPX5_TAS_STATE_TERMINATING ||
			    state == SPX5_TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != SPX5_TAS_STATE_TERMINATING &&
		    state != SPX5_TAS_STATE_ADMIN) {
			netdev_err(port->ndev,
				   "Timeout switching TAS state %s in list %d\n",
				   sparx5_tas_state_to_str(state), list);
			return -ETIME;
		}

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
			state = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
			if (state == SPX5_TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != SPX5_TAS_STATE_ADMIN) {
			netdev_err(port->ndev,
				   "Timeout switching TAS state %s in list %d\n",
				   sparx5_tas_state_to_str(state), list);
			return -ETIME;
		}

		/* Restore gate state to "all-queues-open" */
		/* Select port n on layer 2 of Hierarchical Scheduler */
		spx5_wr(HSCH_TAS_GATE_STATE_CTRL_HSCH_POS_SET(sched),
			sparx5,
			HSCH_TAS_GATE_STATE_CTRL);
		/* Set gate state to "all-queues-open" */
		spx5_wr(HSCH_TAS_GATE_STATE_TAS_GATE_STATE_SET(0xff),
			sparx5,
			HSCH_TAS_GATE_STATE);
	}
	return 0;
}

/* Find a suitable list for a new schedule.
 * First priority is a list in state pending.
 * Second priority is a list in state admin.
 * If list found is in state pending it is shut down here.
 * Index of found list is returned in new.
 * If an operating list is found, the index is returned in obsolete.
 * This list must be configured to be shut down when the new list starts.
 */
static int sparx5_tas_list_find(struct sparx5_port *port, int *new,
				int *obsolete)
{
	int i, err, state_cnt[SPX5_NUM_TAS_STATE] = {0};
	struct sparx5 *sparx5 = port->sparx5;
	int state[SPX5_TAS_ENTRIES_PER_PORT];
	int list[SPX5_TAS_ENTRIES_PER_PORT];
	bool valid = false;
	int oper = -1;
	u32 val;

	for (i = 0; i < SPX5_TAS_ENTRIES_PER_PORT; i++) {
		list[i] = sparx5_tas_list_index(port, i);
		spx5_rmw(HSCH_TAS_CFG_CTRL_LIST_NUM_SET(list[i]),
			 HSCH_TAS_CFG_CTRL_LIST_NUM,
			 sparx5,
			 HSCH_TAS_CFG_CTRL);

		val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
		state[i] = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
		if (state[i] >= SPX5_NUM_TAS_STATE) {
			netdev_err(port->ndev, "Invalid tas list state %u %u %d\n",
				   state[i], port->portno, i);
			return -EINVAL;
		}

		if (state[i] == SPX5_TAS_STATE_OPERATING)
			oper = list[i];

		state_cnt[state[i]]++;
	}

	if (state_cnt[SPX5_TAS_STATE_ADMIN] == 2)
		valid = true;
	if (state_cnt[SPX5_TAS_STATE_ADMIN] == 1 && state_cnt[SPX5_TAS_STATE_PENDING] == 1)
		valid = true;
	if (state_cnt[SPX5_TAS_STATE_ADMIN] == 1 && state_cnt[SPX5_TAS_STATE_OPERATING] == 1)
		valid = true;
	if (state_cnt[SPX5_TAS_STATE_OPERATING] == 1 && state_cnt[SPX5_TAS_STATE_PENDING] == 1)
		valid = true;

	if (!valid) {
		netdev_err(port->ndev, "Invalid tas state combination: %d %d %d %d %d\n",
			   state_cnt[SPX5_TAS_STATE_ADMIN],
			   state_cnt[SPX5_TAS_STATE_ADVANCING],
			   state_cnt[SPX5_TAS_STATE_PENDING],
			   state_cnt[SPX5_TAS_STATE_OPERATING],
			   state_cnt[SPX5_TAS_STATE_TERMINATING]);
		return -1;
	}

	for (i = 0; i < SPX5_TAS_ENTRIES_PER_PORT; i++) {
		if (state[i] == SPX5_TAS_STATE_PENDING) {
			err = sparx5_tas_shutdown_pending(port);
			if (err)
				return err;
			*new = list[i];
			*obsolete = (oper == -1) ? *new : oper;
			return 0;
		}
	}

	for (i = 0; i < SPX5_TAS_ENTRIES_PER_PORT; i++) {
		if (state[i] == SPX5_TAS_STATE_ADMIN) {
			*new = list[i];
			*obsolete = (oper == -1) ? *new : oper;
			return 0;
		}
	}
	return -1; /* No suitable list found */
}

/* Get a bitmap of all free GCLs
 * Return number of free GCLs found
 */
static int sparx5_tas_gcl_free_get(struct sparx5_port *port, unsigned long *free_list)
{
	struct sparx5 *sparx5 = port->sparx5;
	int num_free = TAS_NUM_GCL;
	u32 base, curr, length;
	int num_tas_lists;
	int state, list;
	u32 val, cfg;

	num_tas_lists = sparx5->port_count * SPX5_TAS_ENTRIES_PER_PORT;

	bitmap_fill(free_list, TAS_NUM_GCL); /* Start with all free */

	for (list = 0; list < num_tas_lists; list++) {
		spx5_rmw(HSCH_TAS_CFG_CTRL_LIST_NUM_SET(list),
			 HSCH_TAS_CFG_CTRL_LIST_NUM,
			 sparx5,
			 HSCH_TAS_CFG_CTRL);

		val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
		state = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
		if (state == SPX5_TAS_STATE_ADMIN)
			continue;

		cfg = spx5_rd(sparx5, HSCH_TAS_LIST_CFG);
		base = HSCH_TAS_LIST_CFG_LIST_BASE_ADDR_GET(cfg);
		length = HSCH_TAS_LIST_CFG_LIST_LENGTH_GET(cfg);

		for (curr = base; curr < base + length; curr++) {
			if (!test_bit(curr, free_list)) {
				netdev_err(port->ndev,
					   "List %d: GCL entry %u used multiple times!\n",
					   list, curr);
				return -EEXIST;
			}
			clear_bit(curr, free_list); /* Mark as not free */
			num_free--;

			spx5_rmw(HSCH_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(curr),
				 HSCH_TAS_CFG_CTRL_GCL_ENTRY_NUM,
				 sparx5,
				 HSCH_TAS_CFG_CTRL);
		}
	}
	return num_free;
}

/* Find N continuous GCL entries */
static int sparx5_tas_gcl_base_get(unsigned long *free_list, int num_entries)
{
	int i, empty_found;

	empty_found = 0;
	for (i = 0; i < TAS_NUM_GCL; i++) {
		if (test_bit(i, free_list))
			empty_found++;
		else
			empty_found = 0;

		if (empty_found == num_entries)
			return (i - num_entries) + 1;
	}

	return -1;
}

/* Setup GCLs for a specific list */
static int sparx5_tas_gcl_setup(struct sparx5_port *port, int list,
				struct tc_taprio_qopt_offload *qopt)
{
	DECLARE_BITMAP(free_list, TAS_NUM_GCL);
	struct sparx5 *sparx5 = port->sparx5;
	int i, num_free, base;
	u32 sched;

	sched = sparx5_tas_scheduler_get(sparx5, port->portno);

	num_free = sparx5_tas_gcl_free_get(port, free_list);
	if (num_free < (int)qopt->num_entries) {
		netdev_info(port->ndev, "Not enough free GCL entries!\n");
		return -1;
	}

	base = sparx5_tas_gcl_base_get(free_list, qopt->num_entries);
	if (base < 0) {
		netdev_err(port->ndev, "Can't find %lu continuous GCL entries\n",
			   qopt->num_entries);
		return -1;
	}

	netdev_dbg(port->ndev, "gcl setup list %d, base %d num_free %d\n",
		   list, base, num_free);

	for (i = 0; i < SPX5_TAS_ENTRIES_PER_PORT; i++) {
		spx5_rmw(HSCH_TAS_CFG_CTRL_LIST_NUM_SET(list + i),
			 HSCH_TAS_CFG_CTRL_LIST_NUM,
			 sparx5,
			 HSCH_TAS_CFG_CTRL);

		spx5_rmw(HSCH_TAS_LIST_CFG_LIST_BASE_ADDR_SET(base),
			 HSCH_TAS_LIST_CFG_LIST_BASE_ADDR,
			 sparx5,
			 HSCH_TAS_LIST_CFG);

		if (is_sparx5(sparx5)) {
			spx5_rmw(HSCH_TAS_LIST_CFG_LIST_LENGTH_SET(qopt->num_entries),
				 HSCH_TAS_LIST_CFG_LIST_LENGTH,
				 sparx5,
				 HSCH_TAS_LIST_CFG);
		} else {
			/* Associate TAS list with physical port number and
			 * scheduler element.
			 */
			spx5_rmw(HSCH_TAS_LIST_CFG_LIST_PORT_NUM_SET(port->portno),
				 HSCH_TAS_LIST_CFG_LIST_PORT_NUM, sparx5,
				 HSCH_TAS_LIST_CFG);
			spx5_rmw(HSCH_TAS_LIST_CFG_LIST_HSCH_POS_SET(sched),
				 HSCH_TAS_LIST_CFG_LIST_HSCH_POS, sparx5,
				 HSCH_TAS_LIST_CFG);
		}
	}

	for (i = 0; i < qopt->num_entries; i++) {
		u32 gcl_next = (i >= qopt->num_entries - 1) ? base :
							      base + i + 1;
		/* GCL index is relative to BASE_ADDR */
		spx5_rmw(HSCH_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(i),
			 HSCH_TAS_CFG_CTRL_GCL_ENTRY_NUM,
			 sparx5,
			 HSCH_TAS_CFG_CTRL);

		/* These are configured through TAS Profiles */
		switch (qopt->entries[i].command) {
		case TC_TAPRIO_CMD_SET_GATES:
			/*cmd = TAS_GCL_CMD_SET_GATE_STATES;*/
			break;
		/*case TC_TAPRIO_CMD_SET_AND_HOLD:*/
			/*cmd = TAS_GCL_CMD_SET_AND_HOLD_MAC;*/
			/*break;*/
		/*case TC_TAPRIO_CMD_SET_AND_RELEASE:*/
			/*cmd = TAS_GCL_CMD_SET_AND_RELEASE_MAC;*/
			/*break;*/
		default:
			netdev_err(port->ndev,
				   "TAS: Unsupported GCL command: %d\n",
				   qopt->entries[i].command);
			return -1;
		}

		/* Set gate states for this GCL */
		spx5_rmw(HSCH_TAS_GCL_CTRL_CFG_GATE_STATE_SET(qopt->entries[i].gate_mask),
			 HSCH_TAS_GCL_CTRL_CFG_GATE_STATE, sparx5,
			 HSCH_TAS_GCL_CTRL_CFG);

		if (is_sparx5(sparx5)) {
			spx5_rmw(HSCH_TAS_GCL_CTRL_CFG_HSCH_POS_SET(sched) |
				HSCH_TAS_GCL_CTRL_CFG_PORT_PROFILE_SET(port->portno),
				HSCH_TAS_GCL_CTRL_CFG_HSCH_POS |
				HSCH_TAS_GCL_CTRL_CFG_PORT_PROFILE, sparx5,
				HSCH_TAS_GCL_CTRL_CFG);
		} else {
			/* The GCL list is a linked list on lan969x */
			spx5_wr(HSCH_TAS_GCL_CTRL_CFG2_NEXT_GCL_SET(gcl_next),
				sparx5, HSCH_TAS_GCL_CTRL_CFG2);
		}

		spx5_wr(qopt->entries[i].interval,
			sparx5,
			HSCH_TAS_GCL_TIME_CFG);
	}
	return 0;
}

int sparx5_tas_enable(struct sparx5_port *port,
		      struct tc_taprio_qopt_offload *qopt)
{
	int i, err, new_list = -1, obsolete = -1;
	struct sparx5 *sparx5 = port->sparx5;
	u64 cycle_time = qopt->cycle_time;
	u64 calculated_cycle_time = 0;
	struct timespec64 ts;
	ktime_t base_time;

	mutex_lock(&port->sparx5->tas_lock);
	if (cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		netdev_err(port->ndev, "Invalid cycle_time %llu\n",
			   (unsigned long long)cycle_time);
		err = -EINVAL;
		goto out;
	}
	for (i = 0; i < qopt->num_entries; i++) {
		if (qopt->entries[i].interval < TAS_MIN_CYCLE_TIME_NS) {
			netdev_err(port->ndev, "Invalid minimum cycle time %llu\n",
				   (unsigned long long)qopt->entries[i].interval);
			err = -EINVAL;
			goto out;
		}
		if (qopt->entries[i].interval > TAS_MAX_CYCLE_TIME_NS) {
			netdev_err(port->ndev, "Invalid maximum cycle time %llu\n",
				   (unsigned long long)qopt->entries[i].interval);
			err = -EINVAL;
			goto out;
		}
		calculated_cycle_time += qopt->entries[i].interval;
	}
	if (calculated_cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		netdev_err(port->ndev, "Invalid calculated_cycle_time %llu\n",
			   (unsigned long long)calculated_cycle_time);
		err = -EINVAL;
		goto out;
	}
	if (cycle_time < calculated_cycle_time) {
		netdev_err(port->ndev, "Invalid cycle_time %llu\n",
			   (unsigned long long)cycle_time);
		err = -EINVAL;
		goto out;
	}

	sparx5_new_base_time(sparx5, cycle_time, qopt->base_time, &base_time);

	/* Select an appropriate entry to use */
	err = sparx5_tas_list_find(port, &new_list, &obsolete);
	netdev_dbg(port->ndev, "sparx5_tas_list_find() returned %d %d %d\n",
		   err, new_list, obsolete);
	if (err) {
		err = -EINVAL;
		goto out;
	}

	/* Setup GCL entries */
	err = sparx5_tas_gcl_setup(port, new_list, qopt);
	if (err) {
		err = -EINVAL;
		goto out;
	}

	/* Setup TAS list */
	ts = ktime_to_timespec64(base_time);
	spx5_wr(HSCH_TAS_BASE_TIME_NSEC_BASE_TIME_NSEC_SET(ts.tv_nsec),
		sparx5,
		HSCH_TAS_BASE_TIME_NSEC);

	spx5_wr((ts.tv_sec & GENMASK(31, 0)),
		sparx5,
		HSCH_TAS_BASE_TIME_SEC_LSB);

	spx5_wr(HSCH_TAS_BASE_TIME_SEC_MSB_BASE_TIME_SEC_MSB_SET(ts.tv_sec >> 32),
		sparx5,
		HSCH_TAS_BASE_TIME_SEC_MSB);

	spx5_wr(cycle_time,
		sparx5,
		HSCH_TAS_CYCLE_TIME_CFG);

	spx5_rmw(HSCH_TAS_STARTUP_CFG_OBSOLETE_IDX_SET(obsolete),
		 HSCH_TAS_STARTUP_CFG_OBSOLETE_IDX,
		 sparx5,
		 HSCH_TAS_STARTUP_CFG);

	/* Start list processing */
	spx5_rmw(HSCH_TAS_LIST_STATE_LIST_STATE_SET(SPX5_TAS_STATE_ADVANCING),
		 HSCH_TAS_LIST_STATE_LIST_STATE,
		 sparx5,
		 HSCH_TAS_LIST_STATE);

out:
	mutex_unlock(&port->sparx5->tas_lock);
	return err;
}

int sparx5_tas_disable(struct sparx5_port *port)
{
	int err;

	mutex_lock(&port->sparx5->tas_lock);
	err = sparx5_tas_shutdown_pending(port);
	if (err)
		goto out;

	err = sparx5_tas_shutdown_operating(port);
out:
	mutex_unlock(&port->sparx5->tas_lock);
	return err;
}

static int sparx5_tas_init(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	int i, num_ports, num_tas_lists;

	/* There are only 128 TAS lists, not enough for the whole port range */
	num_ports = consts->chip_ports;
	num_tas_lists = sparx5->port_count * SPX5_TAS_ENTRIES_PER_PORT;

	mutex_init(&sparx5->tas_lock);
	spx5_wr(HSCH_TAS_STATEMACHINE_CFG_REVISIT_DLY_SET((256 * 1000) /
					    sparx5_clk_period(sparx5->coreclock)),
		sparx5,
		HSCH_TAS_STATEMACHINE_CFG);

	/* For now we always use guard band on all queues */
	spx5_rmw(HSCH_TAS_CFG_CTRL_LIST_NUM_MAX_SET(num_tas_lists) |
		 HSCH_TAS_CFG_CTRL_ALWAYS_GUARD_BAND_SCH_Q_SET(1),
		 HSCH_TAS_CFG_CTRL_LIST_NUM_MAX |
		 HSCH_TAS_CFG_CTRL_ALWAYS_GUARD_BAND_SCH_Q,
		 sparx5,
		 HSCH_TAS_CFG_CTRL);

	/* Associate profile with port (profile idx = port on lan969x)*/
	if (is_sparx5(sparx5)) {
		for (i = 0; i < num_ports; i++) {
			if (!sparx5->ports[i])
				continue;
			spx5_rmw(HSCH_TAS_PROFILE_CONFIG_PORT_NUM_SET(i),
				 HSCH_TAS_PROFILE_CONFIG_PORT_NUM,
				 sparx5,
				 HSCH_TAS_PROFILE_CONFIG(i));
		}
	}


	return 0;
}

void sparx5_tas_speed(struct sparx5_port *port, int speed)
{
	struct sparx5 *sparx5 = port->sparx5;
	u8 spd;

	netdev_dbg(port->ndev, "speed %d\n", speed);

	/* Update TAS profile speed */
	switch (speed) {
	case SPEED_10:
		spd = TAS_SPEED_10;
		break;
	case SPEED_100:
		spd = TAS_SPEED_100;
		break;
	case SPEED_1000:
		spd = TAS_SPEED_1000;
		break;
	case SPEED_2500:
		spd = TAS_SPEED_2500;
		break;
	case SPEED_5000:
		spd = TAS_SPEED_5000;
		break;
	case SPEED_10000:
		spd = TAS_SPEED_10000;
		break;
	case SPEED_25000:
		spd = TAS_SPEED_25000;
		break;
	default:
		netdev_info(port->ndev, "TAS: Unsupported speed: %d\n", speed);
		return;
	}

	spx5_rmw(HSCH_TAS_PROFILE_CONFIG_LINK_SPEED_SET(spd),
		 HSCH_TAS_PROFILE_CONFIG_LINK_SPEED,
		 sparx5,
		 HSCH_TAS_PROFILE_CONFIG(port->portno));
}

/*******************************************************************************/
int sparx5_hsch_l0_get_idx(struct sparx5 *sparx5, int port, int queue)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;

	return (consts->hsch_l1_se_cnt * port) +
	       (consts->hsch_queue_cnt * queue);
}

static int sparx5_lg_del(struct sparx5 *sparx5, u32 layer, u32 group, u32 idx)
{
	u32 first, next, prev;
	bool empty = false;

	/* idx *must* be present in the leak group */
	WARN_ON(sparx5_lg_get_adjacent(sparx5, layer, group, idx, &prev, &next,
				       &first) < 0);

	if (sparx5_lg_is_singular(sparx5, layer, group)) {
		empty = true;
	} else if (sparx5_lg_is_last(sparx5, layer, group, idx)) {
		/* idx is removed, prev is now last */
		idx = prev;
		next = prev;
	} else if (sparx5_lg_is_first(sparx5, layer, group, idx)) {
		/* idx is removed and points to itself, first is next */
		first = next;
		next = idx;
	} else {
		/* Next is not touched */
		idx = prev;
	}

	return sparx5_lg_conf_set(sparx5, layer, group, first, idx, next,
				  empty);
}


static int sparx5_lg_add(struct sparx5 *sparx5, u32 layer, u32 new_group,
			 u32 idx)
{
	u32 first, next, old_group;

	pr_debug("ADD: layer: %d, new_group: %d, idx: %d", layer, new_group,
		 idx);

	/* Is this SE already shaping ? */
	if (sparx5_lg_get_group_by_index(sparx5, layer, idx, &old_group) >= 0) {
		if (old_group != new_group) {
			/* Delete from old group */
			sparx5_lg_del(sparx5, layer, old_group, idx);
		} else {
			/* Nothing to do here */
			return 0;
		}
	}

	/* We always add to head of the list */
	first = idx;

	if (sparx5_lg_is_empty(sparx5, layer, new_group))
		next = idx;
	else
		next = sparx5_lg_get_first(sparx5, layer, new_group);

	return sparx5_lg_conf_set(sparx5, layer, new_group, first, idx, next,
				  false);
}

static int sparx5_shaper_conf_set(struct sparx5_port *port,
				  const struct sparx5_shaper *sh, u32 layer,
				  u32 idx, u32 group)
{
	int (*sparx5_lg_action)(struct sparx5 *, u32, u32, u32);
	struct sparx5 *sparx5 = port->sparx5;

	if (!sh->rate && !sh->burst)
		sparx5_lg_action = &sparx5_lg_del;
	else
		sparx5_lg_action = &sparx5_lg_add;

	/* Select layer */
	spx5_rmw(HSCH_HSCH_CFG_CFG_HSCH_LAYER_SET(layer),
		 HSCH_HSCH_CFG_CFG_HSCH_LAYER, sparx5, HSCH_HSCH_CFG_CFG);

	/* Set frame mode */
	spx5_rmw(HSCH_SE_CFG_SE_FRM_MODE_SET(sh->mode),
		 HSCH_SE_CFG_SE_FRM_MODE,
		 sparx5, HSCH_SE_CFG(idx));

	/* Set committed rate and burst */
	spx5_wr(HSCH_CIR_CFG_CIR_RATE_SET(sh->rate) |
		HSCH_CIR_CFG_CIR_BURST_SET(sh->burst),
		sparx5, HSCH_CIR_CFG(idx));

	/* This has to be done after the shaper configuration has been set */
	sparx5_lg_action(sparx5, layer, group, idx);

	return 0;
}

static int sparx5_dwrr_conf_set(struct sparx5_port *port,
				  struct sparx5_dwrr *dwrr)
{
	u32 layer = is_sparx5(port->sparx5) ? 2 : 1;
	int i;

	spx5_rmw(HSCH_HSCH_CFG_CFG_HSCH_LAYER_SET(layer) |
		HSCH_HSCH_CFG_CFG_CFG_SE_IDX_SET(port->portno),
		HSCH_HSCH_CFG_CFG_HSCH_LAYER |
		HSCH_HSCH_CFG_CFG_CFG_SE_IDX,
		port->sparx5, HSCH_HSCH_CFG_CFG);

	/* Number of *lower* indexes that are arbitrated dwrr */
	spx5_rmw(HSCH_SE_CFG_SE_DWRR_CNT_SET(dwrr->count),
		HSCH_SE_CFG_SE_DWRR_CNT, port->sparx5, HSCH_SE_CFG(port->portno));

	for (i = 0; i < dwrr->count; i++) {
		spx5_rmw(HSCH_DWRR_ENTRY_DWRR_COST_SET(dwrr->cost[i]),
			HSCH_DWRR_ENTRY_DWRR_COST, port->sparx5,
			HSCH_DWRR_ENTRY(i));
	}

	return 0;
}

int sparx5_tc_mqprio_add(struct net_device *ndev, u8 num_tc)
{
	int i;

	if (num_tc != SPX5_PRIOS) {
		netdev_err(ndev, "Only %d traffic classes supported\n",
			   SPX5_PRIOS);
		return -EINVAL;
	}

	netdev_set_num_tc(ndev, num_tc);

	for (i = 0; i < num_tc; i++)
		netdev_set_tc_queue(ndev, i, 1, i);

	netdev_dbg(ndev, "dev->num_tc %u dev->real_num_tx_queues %u\n",
		   ndev->num_tc, ndev->real_num_tx_queues);

	return 0;
}

int sparx5_tc_mqprio_del(struct net_device *ndev)
{
	netdev_reset_tc(ndev);

	netdev_dbg(ndev, "dev->num_tc %u dev->real_num_tx_queues %u\n",
		   ndev->num_tc, ndev->real_num_tx_queues);

	return 0;
}

int sparx5_tc_tbf_add(struct sparx5_port *port,
		      struct tc_tbf_qopt_offload_replace_params *params,
		      u32 layer, u32 idx)
{
	struct sparx5_shaper sh = {
		.mode = SPX5_SE_MODE_DATARATE,
		.rate = div_u64(params->rate.rate_bytes_ps, 1000) * 8,
		.burst = params->max_size,
	};
	struct sparx5_lg *lg;
	u32 group;

	/* Find suitable group for this se */
	if (sparx5_lg_get_group_by_rate(layer, sh.rate, &group) < 0) {
		pr_debug("Could not find leak group for se with rate: %d",
			 sh.rate);
		return -EINVAL;
	}

	lg = &sparx5_layers[layer].leak_groups[group];

	pr_debug("Found matching group (speed: %d)\n", lg->max_rate);

	if (sh.rate < SPX5_SE_RATE_MIN || sh.burst < SPX5_SE_BURST_MIN)
		return -EINVAL;

	/* Calculate committed rate and burst */
	sh.rate = DIV_ROUND_UP(sh.rate, lg->resolution);
	sh.burst = DIV_ROUND_UP(sh.burst, SPX5_SE_BURST_UNIT);

	if (sh.rate > SPX5_SE_RATE_MAX || sh.burst > SPX5_SE_BURST_MAX)
		return -EINVAL;

	return sparx5_shaper_conf_set(port, &sh, layer, idx, group);
}

int sparx5_tc_tbf_del(struct sparx5_port *port, u32 layer, u32 idx)
{
	struct sparx5_shaper sh = {0};
	u32 group;

	sparx5_lg_get_group_by_index(port->sparx5, layer, idx, &group);

	return sparx5_shaper_conf_set(port, &sh, layer, idx, group);
}

int sparx5_cbs_add(struct sparx5_port *port, struct tc_cbs_qopt_offload *qopt)
{
	struct sparx5_shaper sh;
	struct sparx5_lg *lg;
	u32 group, se_idx;

	se_idx = sparx5_hsch_l0_get_idx(port->sparx5, port->portno,
					qopt->queue);

	/* Check for invalid values */
	if (qopt->idleslope <= 0 ||
	    qopt->sendslope >= 0 ||
	    qopt->locredit >= qopt->hicredit)
		return -EINVAL;

	sh.mode = SPX5_SE_MODE_DATARATE;
	sh.rate = qopt->idleslope;
	sh.burst = (qopt->idleslope - qopt->sendslope) *
		   (qopt->hicredit - qopt->locredit) / -qopt->sendslope;

	/* Find suitable group for this se */
	if (sparx5_lg_get_group_by_rate(0, sh.rate, &group) < 0) {
		pr_debug("Could not find leak group for se with rate: %d",
			 sh.rate);
		return -EINVAL;
	}

	lg = &sparx5_layers[0].leak_groups[group];

	/* Calculate committed rate and burst */
	sh.rate = DIV_ROUND_UP(sh.rate, lg->resolution);
	sh.burst = DIV_ROUND_UP(sh.burst, SPX5_SE_BURST_UNIT);

	/* Check that actually the result can be written */
	if (sh.rate > GENMASK(15, 0) ||
	    sh.burst > GENMASK(6, 0))
		return -EINVAL;

	return sparx5_shaper_conf_set(port, &sh, 0, se_idx, group);
}

int sparx5_cbs_del(struct sparx5_port *port, struct tc_cbs_qopt_offload *qopt)
{
	struct sparx5_shaper sh = {0};
	u32 group, se_idx;

	se_idx = sparx5_hsch_l0_get_idx(port->sparx5, port->portno,
					qopt->queue);

	sparx5_lg_get_group_by_index(port->sparx5, 0, se_idx, &group);

	return sparx5_shaper_conf_set(port, &sh, 0, se_idx, group);
}

int sparx5_tc_ets_add(struct sparx5_port *port,
	struct tc_ets_qopt_offload_replace_params *params)
{
	struct sparx5_dwrr dwrr = {0};
	unsigned int weight_min = 100;
	int i;

	/* Find minimum weight for all dwrr bands */
	for (i = 0; i < SPX5_PRIOS; i++) {
		if (params->quanta[i] == 0)
			continue;
		weight_min = min(weight_min, params->weights[i]);
	}

	for (i = 0; i < SPX5_PRIOS; i++) {

		/* Strict band; skip */
		if (params->quanta[i] == 0)
			continue;

		dwrr.count++;

		/**
		 * On the sparx5, bands with higher indexes are preferred and arbitrated
		 * strict. Strict bands are put in the lower indexes, by tc, so we reverse
		 * the bands here.
		 *
		 * Also convert the weight to something the hardware understands.
		 */
		dwrr.cost[PRIO_COUNT - i - 1] = ((((SPX5_DWRR_COST_MAX << 4) *
			weight_min / params->weights[i]) + 8) >> 4) - 1;
	}

	return sparx5_dwrr_conf_set(port, &dwrr);
}

int sparx5_tc_ets_del(struct sparx5_port *port)
{
	struct sparx5_dwrr dwrr = {0};

	return sparx5_dwrr_conf_set(port, &dwrr);
}

/* Max rates for leak groups */
static const u32 sparx5_hsch_max_group_rate[SPX5_HSCH_LEAK_GRP_CNT] = {
	1048568, 2621420, 10485680, 26214200
};

u32 sparx5_get_hsch_max_group_rate(int grp)
{
	return sparx5_hsch_max_group_rate[grp];
}

static int sparx5_leak_groups_init(struct sparx5 *sparx5)
{
	const struct sparx5_ops *ops = &sparx5->data->ops;
	u32 spx5_hsch_max_group_rate[SPX5_HSCH_LEAK_GRP_CNT];
	struct sparx5_layer *layer;
	u32 sys_clk_per_100ps;
	struct sparx5_lg *lg;
	u32 leak_time_us;
	int i, ii;

	/* Max rates for leak groups */
	for (i = 0; i < SPX5_HSCH_LEAK_GRP_CNT; i++)
		spx5_hsch_max_group_rate[i] = ops->get_hsch_max_group_rate(i);

	sys_clk_per_100ps = sparx5_clk_period(sparx5->coreclock) / 100;

	for (i = 0; i < SPX5_HSCH_LAYER_CNT; i++) {
		layer = &sparx5_layers[i];
		for (ii = 0; ii < SPX5_HSCH_LEAK_GRP_CNT; ii++) {
			lg = &layer->leak_groups[ii];
			lg->max_rate = spx5_hsch_max_group_rate[ii];

			/* Calculate the leak time in us, to serve a maximum
			 * rate of 'max_rate' for this group
			 */
			leak_time_us = (SPX5_SE_RATE_MAX * 1000) / lg->max_rate;

			/* Hardware wants leak time in ns */
			lg->leak_time = 1000 * leak_time_us;

			/* Calculate resolution */
			lg->resolution = 1000 / leak_time_us;

			/* Maximum number of shapers that can be served by
			 * this leak group
			 */
			lg->max_ses = (1000 * leak_time_us) / sys_clk_per_100ps;

			/* Example:
			 * Wanted bandwidth is 100Mbit:
			 *
			 * 100 mbps can be served by leak group zero.
			 *
			 * leak_time is 125000 ns.
			 * resolution is: 8
			 *
			 * cir          = 100000 / 8 = 12500
			 * leaks_pr_sec = 125000 / 10^9 = 8000
			 * bw           = 12500 * 8000 = 10^8 (100 Mbit)
			 */

			/* Disable by default - this also indicates an empty
			 * leak group
			 */
			sparx5_lg_disable(sparx5, i, ii);
		}
	}

	return 0;
}

/**
 * Setup default QoS port configuration
 *
 * @param lan966x
 * @param port
 */
void sparx5_qos_port_setup(struct sparx5 *sparx5, int portno)
{
	struct sparx5_port *port = sparx5->ports[portno];
	struct mchp_qos_port_conf conf;
	int pcp, dei;

	sparx5_qos_port_conf_get(port, &conf);

	/* Ingress default pcp and dei */
	conf.i_default_pcp = 0;
	conf.i_default_dei = 0;

	/* Ingress default prio and dpl */
	conf.i_default_prio = 0;
	conf.i_default_dpl = 0;
	conf.i_mode.tag_map_enable = false;
	conf.i_mode.dscp_map_enable = false;

	/* Egress default pcp and dei */
	conf.e_mode = MCHP_E_MODE_CLASSIFIED;
	conf.e_default_pcp = 0;
	conf.e_default_dei = 0;

	/* Default pcp/dei mapping for both ingress and egress */
	for (pcp = 0; pcp < PRIO_COUNT; pcp++) {
		for (dei = 0; dei < 2; dei++) {
			conf.i_pcp_dei_prio_dpl_map[pcp][dei].prio = pcp;
			conf.i_pcp_dei_prio_dpl_map[pcp][dei].dpl = dei;
			conf.e_prio_dpl_pcp_dei_map[pcp][dei].pcp = pcp;
			conf.e_prio_dpl_pcp_dei_map[pcp][dei].dei = dei;
		}
	}

	sparx5_qos_port_conf_set(port, &conf);
}

int sparx5_qos_init(struct sparx5 *sparx5)
{
	int err;

	sparx5_fp_init(sparx5);

	err = sparx5_policer_init(sparx5);
	if (err)
		return err;

	err = sparx5_leak_groups_init(sparx5);
	if (err)
		return err;

	err = sparx5_tas_init(sparx5);
	if (err)
		return err;

	sparx5_psfp_init(sparx5);

	sparx5_qos_debugfs(sparx5);
	return err;
}
