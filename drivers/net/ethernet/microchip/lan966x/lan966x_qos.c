/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include "lan966x_main.h"
#include <linux/iopoll.h>
#include <linux/genetlink.h>

/*******************************************************************************
 * lan966x common qos
 ******************************************************************************/
/* Helper macros for sysfs debug functions */
#define P(X, Y) \
	seq_printf(m, "%-20s: %12d\n", X, Y)
#define P_STR(X, Y) \
	seq_printf(m, "%-20s: %12s\n", X, Y)
#define P_HEX(X, Y) \
	seq_printf(m, "%-20s: 0x%08x\n", X, Y)
#define P_TIME(X, S, NS) \
	seq_printf(m, "%-20s: %12llu.%09llu sec\n",\
		   X, (long long unsigned)S, (long long unsigned)NS)

enum qos_rate_mode {
	RATE_MODE_DISABLED, /* Policer/shaper disabled */
	RATE_MODE_LINE, /* Measure line rate in kbps incl. IPG */
	RATE_MODE_DATA, /* Measures data rate in kbps excl. IPG */
	RATE_MODE_FRAME, /* Measures frame rate in fps */
	__RATE_MODE_END,
	NUM_RATE_MODE = __RATE_MODE_END,
	RATE_MODE_MAX = __RATE_MODE_END - 1,
};

/*
 * Calculate new base_time based on cycle_time.
 *
 * The hardware requires a base_time that is always in the future.
 * We define threshold_time as current_time + (2 * cycle_time).
 * If base_time is below threshold_time this function recalculates it to be in
 * the interval:
 * threshold_time <= base_time < (threshold_time + cycle_time)
 *
 * A very simple algorith could be like this:
 * new_base_time = org_base_time + N * cycle_time
 * using the lowest N so (new_base_time >= threshold_time
 *
 * The algorithm has been optimized as the above code is extremely slow.
 *
 * lan966x [IN] Target instance reference.
 * cycle_time [IN] In nanoseconds.
 * org_base_time [IN] Original base time.
 * new_base_time [OUT] New base time.
 *
 */
static void lan966x_new_base_time(struct lan966x *lan966x,
				  const u32 cycle_time,
				  const ktime_t org_base_time,
				  ktime_t *new_base_time)
{
	ktime_t current_time, threshold_time, new_time = org_base_time;
	struct timespec64 ts;
	u64 nr_of_cycles_p2;
	u64 nr_of_cycles;
	u64 diff_time;

	lan966x_ptp_gettime64(&lan966x->phc[LAN966X_PHC_PORT].info, &ts);
	current_time = timespec64_to_ktime(ts);
	threshold_time = current_time + (2 * cycle_time);
	diff_time = threshold_time - new_time;
	nr_of_cycles = div_u64(diff_time, cycle_time);
	nr_of_cycles_p2 = 1; /* Use 2^0 as start value */

	if (new_time >= threshold_time) {
		*new_base_time = new_time;
		dev_dbg(lan966x->dev,
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
	   that is larger than nr_of_cycles. */
	while (nr_of_cycles_p2 < nr_of_cycles) {
		nr_of_cycles_p2 <<= 1; /* Next (higher) power of 2 */
	}

	/* Add as big chunks (power of 2 * cycle_time)
	 * as possible for each power of 2 */
	while (nr_of_cycles_p2) {
		if (new_time < threshold_time) {
			new_time += cycle_time * nr_of_cycles_p2;
			while (new_time < threshold_time) {
				new_time += cycle_time * nr_of_cycles_p2;
			}
			new_time -= cycle_time * nr_of_cycles_p2;
		}
		nr_of_cycles_p2 >>= 1; /* Next (lower) power of 2 */
	}
	new_time += cycle_time;
	*new_base_time = new_time;

	dev_dbg(lan966x->dev,
		"\nCHANGED!\n"
		"cycle_time     %20u\n"
		"org_base_time  %20lld\n"
		"cur_time       %20lld\n"
		"threshold_time %20lld\n"
		"new_base_time  %20lld\n"
		"nr_of_cycles   %20lld\n",
		cycle_time, org_base_time, current_time, threshold_time,
		new_time, nr_of_cycles);
	return;
}

/*******************************************************************************
 * QOS Port configuration
 ******************************************************************************/
int lan966x_qos_port_conf_get(const struct lan966x_port *const port,
			      struct mchp_qos_port_conf *const conf)
{
	*conf = port->qos_port_conf;
	return 0;
}

#define TERMINAL_SE_INDEX_OFFSET 80
#define DWRR_COST_BIT_WIDTH 5
#define MIN(x,y) ((x) < (y) ? (x) : (y))
#define MAX(x,y) ((x) > (y) ? (x) : (y))

int lan966x_qos_port_conf_set(struct lan966x_port *const port,
			      struct mchp_qos_port_conf *const conf)
{
	u32 pcp, dei, prio, dpl, e_mode, dwrr_count;
	u8 dwrr_cost[PRIO_COUNT] = {0};
	u32 terminal_se, retval = 0;
	u32 i, c, c_max, val;
	u8 w_min = 100;

	/* Setup port ingress default DEI and PCP */
	lan_rmw(ANA_VLAN_CFG_VLAN_DEI_SET(conf->i_default_dei ? 1 : 0) |
		ANA_VLAN_CFG_VLAN_PCP_SET(conf->i_default_pcp),
		ANA_VLAN_CFG_VLAN_DEI |
		ANA_VLAN_CFG_VLAN_PCP,
		port->lan966x, ANA_VLAN_CFG(port->chip_port));

	/* Setup port ingress default DPL and Priority */
	lan_rmw(ANA_QOS_CFG_DP_DEFAULT_VAL_SET(conf->i_default_dpl ? 1 : 0) |
		ANA_QOS_CFG_QOS_DEFAULT_VAL_SET(conf->i_default_prio) |
		ANA_QOS_CFG_QOS_PCP_ENA_SET(conf->i_mode.tag_map_enable ? 1 : 0) |
		ANA_QOS_CFG_QOS_DSCP_ENA_SET(conf->i_mode.dscp_map_enable ? 1 : 0),
		ANA_QOS_CFG_DP_DEFAULT_VAL |
		ANA_QOS_CFG_QOS_DEFAULT_VAL |
		ANA_QOS_CFG_QOS_DSCP_ENA |
		ANA_QOS_CFG_QOS_PCP_ENA,
		port->lan966x, ANA_QOS_CFG(port->chip_port));

	/* Setup port ingress mapping between [PCP,DEI] and [Priority]. */
	/* Setup port ingress mapping between [PCP,DEI] and [DPL]. */
	for (pcp = 0; pcp < PCP_COUNT; pcp++) {
		for (dei = 0; dei < DEI_COUNT; dei++) {
			prio = conf->i_pcp_dei_prio_dpl_map[pcp][dei].prio;
			dpl = conf->i_pcp_dei_prio_dpl_map[pcp][dei].dpl;
			lan_rmw(ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL_SET(prio) |
				ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL_SET(dpl ? 1 : 0),
				ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL |
				ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL,
				port->lan966x,
				ANA_PCP_DEI_CFG(port->chip_port, PCP_COUNT * dei + pcp));
		}
	}

	dei = (conf->e_mode == MCHP_E_MODE_DEFAULT ? conf->e_default_dei : 0);

	/* Setup port egress default DEI and PCP */
	lan_rmw(REW_PORT_VLAN_CFG_PORT_DEI_SET(dei ? 1 : 0) |
		REW_PORT_VLAN_CFG_PORT_PCP_SET(conf->e_default_pcp),
		REW_PORT_VLAN_CFG_PORT_DEI |
		REW_PORT_VLAN_CFG_PORT_PCP,
		port->lan966x, REW_PORT_VLAN_CFG(port->chip_port));

	/* Setup port egress mapping between [Priority] and [PCP,DEI]. */
	/* Setup port egress mapping between [DPL] and [PCP,DEI]. */
	for (prio = 0; prio < PRIO_COUNT; prio++) {
		for (dpl = 0; dpl < DPL_COUNT; dpl++) {
			pcp = conf->e_prio_dpl_pcp_dei_map[prio][dpl].pcp;
			dei = conf->e_prio_dpl_pcp_dei_map[prio][dpl].dei;
			lan_rmw(REW_PCP_DEI_CFG_DEI_QOS_VAL_SET(dei ? 1 : 0) |
				REW_PCP_DEI_CFG_PCP_QOS_VAL_SET(pcp),
				REW_PCP_DEI_CFG_DEI_QOS_VAL |
				REW_PCP_DEI_CFG_PCP_QOS_VAL,
				port->lan966x,
				REW_PCP_DEI_CFG(port->chip_port, (PRIO_COUNT * dpl + prio)));
		}
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
	lan_rmw(REW_TAG_CFG_TAG_PCP_CFG_SET(e_mode) |
		REW_TAG_CFG_TAG_DEI_CFG_SET(e_mode),
		REW_TAG_CFG_TAG_PCP_CFG |
		REW_TAG_CFG_TAG_DEI_CFG,
		port->lan966x, REW_TAG_CFG(port->chip_port));

	terminal_se = TERMINAL_SE_INDEX_OFFSET + port->chip_port;

	/* Adjust the DWRR queue count to a valid number */
	if (conf->dwrr_enable) {
		dwrr_count = conf->dwrr_count;
		/* Number of allowed queues in DWRR mode is 1..8 */
		if (dwrr_count < 1) {
			dwrr_count = 1;
		} else if (dwrr_count > PRIO_COUNT) {
			dwrr_count = PRIO_COUNT;
		}
	} else {
		dwrr_count = 0; /* All queues are running in strict mode */
	}

	/* Configure the DWRR queue count */
	lan_rmw(QSYS_SE_CFG_SE_DWRR_CNT_SET(dwrr_count),
		QSYS_SE_CFG_SE_DWRR_CNT |
		QSYS_SE_CFG_SE_RR_ENA,
		port->lan966x, QSYS_SE_CFG(terminal_se));

	/* Adjust the DWRR queue percentage to a valid number */
	for (i = 0; i < dwrr_count; i++) {
		if (conf->dwrr_queue_pct[i] < 1) {
			conf->dwrr_queue_pct[i] = 1;
		} else if (conf->dwrr_queue_pct[i] > 100) {
			conf->dwrr_queue_pct[i] = 100;
		}
	}

	/* Convert the DWRR queue bandwidth percentage to the DWRR queue cost to be configured */
	c_max = 1 << DWRR_COST_BIT_WIDTH;
	for (i = 0; i < dwrr_count; i++) {
		w_min = MIN(w_min, conf->dwrr_queue_pct[i]);
	}
	for (i = 0; i < dwrr_count; i++) {
	/* Round half up: Multiply with 16 before division, add 8 and divide result with 16 again */
		c = (((c_max << 4) * w_min / conf->dwrr_queue_pct[i]) + 8) >> 4;
		dwrr_cost[i] = MAX(1, c) - 1; /* Force range to be 0..(c_max - 1) */
	}

	/* Configure the DWRR queue cost */
	for (i = 0; i < PRIO_COUNT; i++) {
		lan_wr(dwrr_cost[i],
		       port->lan966x, QSYS_SE_DWRR_CFG(terminal_se, i));
	}

	if (conf->pfc_enable != 0) {
		val = lan_rd(port->lan966x, SYS_MAC_FC_CFG(port->chip_port));
		if ((SYS_MAC_FC_CFG_RX_FC_ENA_GET(val) != 0) || (SYS_MAC_FC_CFG_TX_FC_ENA_GET(val) != 0)) {
			netdev_err(port->dev, "802.3X FC and 802.1Qbb PFC cannot both be enabled\n");
			retval = -EOPNOTSUPP;
			conf->pfc_enable = 0;
		}
	}

	/* Configure PFC - only when changed as it required port down and up */
	if (port->qos_port_conf.pfc_enable != conf->pfc_enable) {
		lan_rmw(ANA_PFC_CFG_RX_PFC_ENA_SET(conf->pfc_enable),
			ANA_PFC_CFG_RX_PFC_ENA,
			port->lan966x, ANA_PFC_CFG(port->chip_port));

		lan_rmw(DEV_PORT_MISC_FWD_CTRL_ENA_SET(conf->pfc_enable ? 1 : 0),
			DEV_PORT_MISC_FWD_CTRL_ENA,
			port->lan966x, DEV_PORT_MISC(port->chip_port));

		/* This will bring the port down and back up */
		lan966x_port_config_down(port);
		lan966x_port_config_up(port);
	}

	port->qos_port_conf = *conf;

	return retval;
}

int lan966x_qos_dscp_prio_dpl_get(const struct lan966x *lan966x,
				  const u32 dscp,
				  struct mchp_qos_dscp_prio_dpl *const conf)
{
	*conf = lan966x->qos.dscp_prio_dpl_map[dscp];
	return 0;
}

int lan966x_qos_dscp_prio_dpl_set(struct lan966x *lan966x,
				  const u32 dscp,
				  const struct mchp_qos_dscp_prio_dpl *const conf)
{
	/* Setup switch ingress mapping between [DSCP] and [Priority]. */
	/* Setup switch ingress mapping between [DSCP] and [DPL]. */
	lan_rmw(ANA_DSCP_CFG_DP_DSCP_VAL_SET(conf->dpl ? 1 : 0) |
		ANA_DSCP_CFG_QOS_DSCP_VAL_SET(conf->prio) |
		ANA_DSCP_CFG_DSCP_TRUST_ENA_SET(conf->trust ? 1 : 0),
		ANA_DSCP_CFG_DP_DSCP_VAL |
		ANA_DSCP_CFG_QOS_DSCP_VAL |
		ANA_DSCP_CFG_DSCP_TRUST_ENA,
		lan966x, ANA_DSCP_CFG(dscp));

	lan966x->qos.dscp_prio_dpl_map[dscp] = *conf;
	return 0;
}

/*******************************************************************************
 * lan966x policers
 ******************************************************************************/
/* Types for ANA:POL[0-192]:POL_MODE.FRM_MODE */
#define POL_MODE_LINERATE   0 /* Incl IPG. Unit: 33 1/3 kbps, 4096 bytes */
#define POL_MODE_DATARATE   1 /* Excl IPG. Unit: 33 1/3 kbps, 4096 bytes  */
#define POL_MODE_FRMRATE_HI 2 /* Unit: 33 1/3 fps, 32.8 frames */
#define POL_MODE_FRMRATE_LO 3 /* Unit: 1/3 fps, 0.3 frames */

/* Default policer order */
#define POL_ORDER 0x1d3 /* Policer order: Serial (QoS -> Port -> VCAP) */

struct qos_policer_conf {
	enum qos_rate_mode mode;
	bool dlb; /* Enable DLB (dual leaky bucket mode */
	bool cf;  /* Coupling flag (ignored in SLB mode) */
	u32  cir; /* CIR in kbps/fps (ignored in SLB mode) */
	u32  cbs; /* CBS in bytes/frames (ignored in SLB mode) */
	u32  pir; /* PIR in kbps/fps */
	u32  pbs; /* PBS in bytes/frames */
	bool drop_on_yellow;
	bool mark_red_enable;
	bool mark_red;
};

static int policer_conf_set(struct lan966x *lan966x, u32 pol_ix,
			    const struct qos_policer_conf *conf)
{
	u32  cir = 0, cbs = 0, pir, pbs, mode, frm_mode = POL_MODE_LINERATE;
	u32  cir_ena = 0, cf = 0, cbs_max = 0, pbs_max = 0;
	bool cir_discard = 0, pir_discard = 0;
	bool mark_all_red = 0;

	pir = conf->pir;
	pbs = conf->pbs;

	switch (conf->mode) {
	case RATE_MODE_LINE:
	case RATE_MODE_DATA:
		if (conf->mode == RATE_MODE_LINE)
			frm_mode = POL_MODE_LINERATE;
		else
			frm_mode = POL_MODE_DATARATE;

		if (conf->dlb) {
			cir = conf->cir;
			cbs = conf->cbs;
			mark_all_red = conf->mark_red_enable;

			if (mark_all_red && conf->mark_red) {
				cir = 0;
				cbs = 0;
				pir = 0;
				pbs = 0;
			}

			if (conf->drop_on_yellow) {
				pir = 0;
				pbs = 0;
			}

			if (cir == 0 && cbs == 0)
				cir_discard = 1; /* Discard CIR frames */

			cir = DIV_ROUND_UP(cir * 3, 100); /* Unit is 33 1/3 kbps */
			cbs = (cbs ? cbs : 1); /* Avoid zero burst size */
			cbs = DIV_ROUND_UP(cbs, 4096); /* Unit is 4kB */
			cbs_max = 60; /* Limit burst size */
			cir_ena = 1;
			cf = conf->cf;

			if (cf) {
				pir += conf->cir;
				pbs += conf->cbs;
			}
		}

		if (pir == 0 && pbs == 0)
			pir_discard = 1; /* Discard PIR frames */

		pir = DIV_ROUND_UP(pir * 3, 100); /* Unit is 33 1/3 kbps */
		pbs = (pbs ? pbs : 1); /* Avoid zero burst size */
		pbs = DIV_ROUND_UP(pbs, 4096); /* Unit is 4kB */
		pbs_max = 60; /* Limit burst size */
		break;
	case RATE_MODE_FRAME:
		/* There are two frame rate "modes" that has 33 1/3 frame or 1/3
		 * frame resolution. Use 1/3 frame resolution if possible.
		 * The pir configuration bit field is 15 bit wide */
		if (pir >= (0x7FFF / 3)) {  /* 33 1/3 frame resolution */
			frm_mode = POL_MODE_FRMRATE_HI;
			pir = DIV_ROUND_UP(pir * 3, 100); /* Unit is 33 1/3 fps */
			pbs = (pbs * 10 / 328);  /* Unit is 32.8 frames */
			pbs++; /* Round up burst size */
			pbs_max = GENMASK(6, 0); /* Limit burst size */
		} else {  /* 1/3 frame resolution */
			frm_mode = POL_MODE_FRMRATE_LO;
			if (pir == 0 && pbs == 0) {
				pir_discard = 1; /* Discard PIR frames */
				cir_discard = 1; /* Discard CIR frames */
			} else {
				pir *= 3; /* Unit is 1/3 fps */
				pbs = (pbs * 10) / 3; /* Unit is 0.3 frames */
				pbs++; /* Round up burst size */
				pbs_max = 61; /* Limit burst size */
			}
		}
		break;
	default: /* RATE_MODE_DISABLED */
		/* Disable policer using maximum rate and zero burst */
		pir = GENMASK(15, 0);
		pbs = 0;
		break;
	}

	/* Check limits */
	if (pir > GENMASK(15, 0)) {
		dev_err(lan966x->dev, "ix %u: Invalid pir %u\n", pol_ix, pir);
		return -EINVAL;
	}

	if (cir > GENMASK(15, 0)) {
		dev_err(lan966x->dev, "ix %u: Invalid cir %u\n", pol_ix, cir);
		return -EINVAL;
	}

	if (pbs > pbs_max) {
		dev_err(lan966x->dev, "ix %u: Invalid pbs %u\n", pol_ix, pbs);
		return -EINVAL;
	}

	if (cbs > cbs_max) {
		dev_err(lan966x->dev, "ix %u: Invalid cbs %u\n", pol_ix, cbs);
		return -EINVAL;
	}

	/* Setup with RED_ENA = 0 and LVL = 0 to be able to clear POL_STATE
	 * drop_on_yellow is implemented by discarding yellow frames instead of
	 * setting DROP_ON_YELLOW_ENA to 1.
	 */
	mode = (ANA_POL_MODE_DROP_ON_YELLOW_ENA_SET(0) |
		ANA_POL_MODE_MARK_ALL_FRMS_RED_ENA_SET(0) |
		ANA_POL_MODE_IPG_SIZE_SET(20) |
		ANA_POL_MODE_DLB_COUPLED_SET(cf ? 1 : 0) |
		ANA_POL_MODE_CIR_ENA_SET(cir_ena ? 1 : 0) |
		ANA_POL_MODE_FRM_MODE_SET(frm_mode) |
		ANA_POL_MODE_OVERSHOOT_ENA_SET(1));

	lan_wr(mode, lan966x, ANA_POL_MODE(pol_ix));
	lan_wr(ANA_POL_PIR_STATE_PIR_LVL_SET(0),
	       lan966x, ANA_POL_PIR_STATE(pol_ix));
	lan_wr(ANA_POL_CIR_STATE_CIR_LVL_SET(0),
	       lan966x, ANA_POL_CIR_STATE(pol_ix));
	lan_rmw(ANA_POL_STATE_MARK_ALL_FRMS_RED_SET_SET(0),
		ANA_POL_STATE_MARK_ALL_FRMS_RED_SET,
		lan966x, ANA_POL_STATE(pol_ix));

	/* Now setup with new RED_ENA mode */
	lan_wr(ANA_POL_PIR_CFG_PIR_RATE_SET(pir) |
	       ANA_POL_PIR_CFG_PIR_BURST_SET(pbs),
	       lan966x, ANA_POL_PIR_CFG(pol_ix));

	lan_wr(ANA_POL_PIR_STATE_PIR_LVL_SET(pir_discard ?
					     ANA_POL_PIR_STATE_PIR_LVL : 0),
	       lan966x, ANA_POL_PIR_STATE(pol_ix));

	lan_wr(ANA_POL_CIR_CFG_CIR_RATE_SET(cir) |
	       ANA_POL_CIR_CFG_CIR_BURST_SET(cbs),
	       lan966x, ANA_POL_CIR_CFG(pol_ix));

	lan_wr(ANA_POL_CIR_STATE_CIR_LVL_SET(cir_discard ?
					     ANA_POL_CIR_STATE_CIR_LVL : 0),
	       lan966x, ANA_POL_CIR_STATE(pol_ix));

	lan_wr(mode | ANA_POL_MODE_MARK_ALL_FRMS_RED_ENA_SET(mark_all_red),
	       lan966x, ANA_POL_MODE(pol_ix));

	return 0;
}

/*******************************************************************************
 * lan966x shapers
 ******************************************************************************/
/* Types for QSYS:HSCH:SE_CFG.SE_FRM_MODE */
#define SE_MODE_LINERATE   0 /* Line rate. Incl IPG. Unit: 100 kbps, 4096 bytes */
#define SE_MODE_DATARATE   1 /* Data rate. Excl IPG. Unit: 100 kbps, 4096 bytes  */
#define SE_MODE_FRMRATE_HI 2 /* Frame rate. Unit: 100 fps, 32.8 frames */
#define SE_MODE_FRMRATE_LO 3 /* Frame rate. Unit: 1 fps, 0.3 frames */

/* Scheduling elements */
#define SE_IX_PORT   80 /* 80-89 : Port scheduler elements */
#define SE_IX_QUEUE   0 /* 0-79  : Queue scheduler elements */

struct qos_shaper_conf {
	enum qos_rate_mode mode;
	bool credit; /* Enable AVB mode */
	bool dlb; /* Enable dual leaky bucket mode */
	u8 port; /* Port to sense on (ignored in single leaky bucket mode) */
	u8 prio; /* Priority to sense on (ignored in single leaky bucket mode) */
	u32  cir; /* CIR in kbps/fps */
	u32  cbs; /* CBS in bytes/frames */
	u32  eir; /* EIR in kbps/fps (ignored in single leaky bucket mode) */
	u32  ebs; /* EBS in bytes/frames (ignored in single leaky bucket mode) */
};

static int qos_shaper_conf_set(struct lan966x *lan966x,
			       u32 se_ix,
			       struct qos_shaper_conf *conf)
{
	u32 cir = 0, cbs = 0, eir = 0, ebs = 0;
	u32 frm_mode = 0, dport = 0, prio = 0;
	bool dport_ena = false, prio_ena = false;

	switch (conf->mode) {
	case RATE_MODE_LINE:
	case RATE_MODE_DATA:
		if (conf->mode == RATE_MODE_LINE)
			frm_mode = SE_MODE_LINERATE;
		else
			frm_mode = SE_MODE_DATARATE;

		cir = DIV_ROUND_UP(conf->cir, 100);  /* Rate unit is 100 kbps */
		cir = (cir ? cir : 1);               /* Avoid using zero rate */
		cbs = DIV_ROUND_UP(conf->cbs, 4096); /* Burst unit is 4kB */
		cbs = (cbs ? cbs : 1);               /* Avoid using zero burst size */
		if (conf->dlb) {
			eir = DIV_ROUND_UP(conf->eir, 100);  /* Rate unit is 100 kbps */
			eir = (eir ? eir : 1);               /* Avoid using zero rate */
			ebs = DIV_ROUND_UP(conf->ebs, 4096); /* Burst unit is 4kB */
			ebs = (ebs ? ebs : 1);               /* Avoid using zero burst size */
			dport = conf->port;
			dport_ena = true;
			prio = conf->prio;
			prio_ena = true;
		}
		break;
	case RATE_MODE_FRAME:
		if (cir >= 100) {
			frm_mode = SE_MODE_FRMRATE_HI;
			cir = DIV_ROUND_UP(conf->cir, 100); /* Rate unit is 100 fps */
			cbs = (conf->cbs * 10) / 328;       /* Burst unit is 32.8 frames */
		} else {
			frm_mode = SE_MODE_FRMRATE_LO;
			cir = conf->cir;            /* Rate unit is 1 fps */
			cbs = (conf->cbs * 10) / 3; /* Burst unit is 0.3 frames */
		}
		cir = (cir ? cir : 1); /* Avoid using zero rate */
		cbs = (cbs ? cbs : 1); /* Avoid using zero burst size */
		break;
	default: /* MSCC_QOS_RATE_MODE_DISABLED */
		/* Disable shaper by using default values */
		break;
	}

	/* Check limits */
	if (cir > GENMASK(15, 0)) {
		dev_err(lan966x->dev, "ix %u: Invalid cir %u\n", se_ix, cir);
		return -EINVAL;
	}

	if (cbs > GENMASK(6, 0)) {
		dev_err(lan966x->dev, "ix %u: Invalid cbs %u\n", se_ix, cbs);
		return -EINVAL;
	}

	if (eir > GENMASK(15, 0)) {
		dev_err(lan966x->dev, "ix %u: Invalid eir %u\n", se_ix, eir);
		return -EINVAL;
	}

	if (ebs > GENMASK(6, 0)) {
		dev_err(lan966x->dev, "ix %u: Invalid ebs %u\n", se_ix, ebs);
		return -EINVAL;
	}

	lan_rmw(QSYS_SE_CFG_SE_AVB_ENA_SET(conf->credit) |
		QSYS_SE_CFG_SE_FRM_MODE_SET(frm_mode),
		QSYS_SE_CFG_SE_AVB_ENA |
		QSYS_SE_CFG_SE_FRM_MODE,
		lan966x, QSYS_SE_CFG(se_ix));

	lan_wr(QSYS_CIR_CFG_CIR_RATE_SET(cir) |
	       QSYS_CIR_CFG_CIR_BURST_SET(cbs),
	       lan966x, QSYS_CIR_CFG(se_ix));

	lan_wr(QSYS_EIR_CFG_EIR_RATE_SET(eir) |
	       QSYS_EIR_CFG_EIR_BURST_SET(ebs),
	       lan966x, QSYS_EIR_CFG(se_ix));

	lan_rmw(QSYS_SE_DLB_SENSE_SE_DLB_PRIO_SET(prio) |
		QSYS_SE_DLB_SENSE_SE_DLB_DPORT_SET(dport) |
		QSYS_SE_DLB_SENSE_SE_DLB_PRIO_ENA_SET(prio_ena) |
		QSYS_SE_DLB_SENSE_SE_DLB_DPORT_ENA_SET(dport_ena),
		QSYS_SE_DLB_SENSE_SE_DLB_PRIO |
		QSYS_SE_DLB_SENSE_SE_DLB_DPORT |
		QSYS_SE_DLB_SENSE_SE_DLB_PRIO_ENA |
		QSYS_SE_DLB_SENSE_SE_DLB_DPORT_ENA,
		lan966x, QSYS_SE_DLB_SENSE(se_ix));

	return 0;
}

/*******************************************************************************
 * TC (Linux Traffic Control)
 ******************************************************************************/
int lan966x_tc_cbs_add(struct lan966x_port *port, u8 queue,
		       struct lan966x_tc_cbs *cbs)
{
	struct qos_shaper_conf s = { 0 };
	u8 cp = port->chip_port;
	int tx_rate;

	if (!cbs)
		return -EINVAL;

	if (cbs->idleslope <= 0) {
		netdev_err(port->dev,
			   "idleslope must be a positive number\n");
		return -EINVAL;
	}

	if (cbs->sendslope >= 0) {
		netdev_err(port->dev,
			   "sendslope must be a negative number\n");
		return -EINVAL;
	}

	if (cbs->locredit >= cbs->hicredit) {
		netdev_err(port->dev,
			   "hicredit must be greater than locredit\n");
		return -EINVAL;
	}

	tx_rate = cbs->idleslope - cbs->sendslope;
	s.mode = RATE_MODE_DATA;
	s.credit = true;
	s.cir = cbs->idleslope;
	s.cbs = (tx_rate * (cbs->hicredit - cbs->locredit)) / -cbs->sendslope;

	netdev_dbg(port->dev,
		   "port %u queue %u cir %u cbs %u\n",
		   cp, queue, s.cir, s.cbs);

	return qos_shaper_conf_set(port->lan966x, SE_IX_QUEUE + cp * 8 + queue,
				   &s);
}

int lan966x_tc_cbs_del(struct lan966x_port *port, u8 queue)
{
	struct qos_shaper_conf s = { 0 };
	u8 cp = port->chip_port;

	netdev_dbg(port->dev, "port %u queue %u\n", cp, queue);

	s.mode = RATE_MODE_DISABLED;

	return qos_shaper_conf_set(port->lan966x, SE_IX_QUEUE + cp * 8 + queue,
				   &s);
}

int lan966x_tc_tbf_add(struct lan966x_port *port, bool root, u32 queue,
		       struct lan966x_tc_tbf *tbf)
{
	struct qos_shaper_conf s = { 0 };
	u8 cp = port->chip_port;
	u32 se_ix;

	if (root) /* Port shaper */
		se_ix = SE_IX_PORT + cp;
	else /* Priority shaper */
		se_ix = SE_IX_QUEUE + cp * 8 + queue;

	s.mode = RATE_MODE_DATA;
	s.cir = tbf->rate;
	s.cbs = tbf->burst;

	netdev_dbg(port->dev, "port %u root %d queue %u se_ix %u cir %u cbs %u\n",
		   cp, root, queue, se_ix, s.cir, s.cbs);

	return qos_shaper_conf_set(port->lan966x, se_ix, &s);
}

int lan966x_tc_tbf_del(struct lan966x_port *port, bool root, u32 queue)
{
	struct qos_shaper_conf s = { 0 };
	u8 cp = port->chip_port;
	u32 se_ix;

	if (root) /* Port shaper */
		se_ix = SE_IX_PORT + cp;
	else /* Priority shaper */
		se_ix = SE_IX_QUEUE + cp * 8 + queue;

	netdev_dbg(port->dev, "port %u root %d queue %u se_ix %u\n",
		   cp, root, queue, se_ix);

	s.mode = RATE_MODE_DISABLED;
	return qos_shaper_conf_set(port->lan966x, se_ix, &s);
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
enum lan966x_tas_link_speed {
	TAS_SPEED_NO_GB,
	TAS_SPEED_10,
	TAS_SPEED_100,
	TAS_SPEED_1000,
	TAS_SPEED_2500,
};

/* TAS list states: */
enum lan966x_tas_state {
	TAS_STATE_ADMIN,
	TAS_STATE_ADVANCING,
	TAS_STATE_PENDING,
	TAS_STATE_OPERATING,
	TAS_STATE_TERMINATING,
	NUM_TAS_STATE,
};

/* TAS GCL command: */
enum lan966x_tas_gcl_cmd {
	TAS_GCL_CMD_SET_GATE_STATES = 0,
	TAS_GCL_CMD_SET_AND_RELEASE_MAC = 2,
	TAS_GCL_CMD_SET_AND_HOLD_MAC = 3,
};

#define TAS_ENTRIES_PER_PORT	2
#define TAS_NUM_GCL		256 /* Total number of TAS GCL entries */

static u8 num_ports;
static u8 num_tas_lists;

/*
 * We use 2 TAS list entries per port:
 * num_tas_lists = num_ports * TAS_ENTRIES_PER_PORT;
 *
 * The index for the 2 entries per port is calculated as:
 * index_1 = chip_port * TAS_ENTRIES_PER_PORT;
 * index_2 = index_1 + 1;
 *
 * GCL entries are allocated from a free list which is generated each time we
 * want to add a new schedule.
 *
 * The free list is organized as a bitmap with one bit for each GCL entry.
 * All bits in the free list are first set to 1 (free) and then all schedules
 * with state != TAS_STATE_ADMIN are examined.
 * The GCL entries in use are removed from the free list by setting the
 * coresponding bit to zero.
 *
 * The base GCL index is always the first free entry found in the free list.
 *
 * We use 1 TAS profile per port:
 * index = chip_port;
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

static int lan966x_tas_list_index(struct lan966x_port *port, u8 tas_entry)
{
	return (port->chip_port * TAS_ENTRIES_PER_PORT) + tas_entry;
}

static char *lan966x_tas_state_to_str(int state)
{
	switch (state) {
	case TAS_STATE_ADMIN:
		return "ADMIN";
	case TAS_STATE_ADVANCING:
		return "ADVANCING";
	case TAS_STATE_PENDING:
		return "PENDING";
	case TAS_STATE_OPERATING:
		return "OPERATING";
	case TAS_STATE_TERMINATING:
		return "TERMINATING";
	default:
		return "??";
	}
}

static int lan966x_tas_shutdown_pending(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;
	int i, list, state;
	unsigned long end;

	netdev_dbg(port->dev, "chip_port %u\n", port->chip_port);
	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list = lan966x_tas_list_index(port, i);
		lan_rmw(QSYS_TAS_CFG_CTRL_LIST_NUM_SET(list),
			QSYS_TAS_CFG_CTRL_LIST_NUM,
			lan966x, QSYS_TAS_CFG_CTRL);

		state = QSYS_TAS_LST_LIST_STATE_GET(lan_rd(lan966x,
							 QSYS_TAS_LST));
		if ((state != TAS_STATE_ADVANCING) &&
		    (state != TAS_STATE_PENDING))
			continue;

		netdev_dbg(port->dev, "state %s found in list %d\n",
			   lan966x_tas_state_to_str(state), list);

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			lan_rmw(QSYS_TAS_LST_LIST_STATE_SET(TAS_STATE_ADMIN),
				QSYS_TAS_LST_LIST_STATE,
				lan966x, QSYS_TAS_LST);

			state = QSYS_TAS_LST_LIST_STATE_GET(lan_rd(lan966x,
								   QSYS_TAS_LST));
			if (state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_ADMIN) {
			netdev_err(port->dev,
				   "Timeout switching TAS state %s in list %d\n",
				   lan966x_tas_state_to_str(state), list);
			return -ETIME;
		}
	}
	return 0;
}

static int lan966x_tas_shutdown_operating(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;
	int i, list, state;
	unsigned long end;

	netdev_dbg(port->dev, "chip_port %u\n", port->chip_port);
	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list = lan966x_tas_list_index(port, i);
		lan_rmw(QSYS_TAS_CFG_CTRL_LIST_NUM_SET(list),
			QSYS_TAS_CFG_CTRL_LIST_NUM,
			lan966x, QSYS_TAS_CFG_CTRL);

		state = QSYS_TAS_LST_LIST_STATE_GET(lan_rd(lan966x,
							   QSYS_TAS_LST));
		if (state != TAS_STATE_OPERATING)
			continue;

		netdev_dbg(port->dev, "state %s found in list %d\n",
			   lan966x_tas_state_to_str(state), list);

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			lan_rmw(QSYS_TAS_LST_LIST_STATE_SET(TAS_STATE_TERMINATING),
				QSYS_TAS_LST_LIST_STATE,
				lan966x, QSYS_TAS_LST);

			state = QSYS_TAS_LST_LIST_STATE_GET(lan_rd(lan966x,
								   QSYS_TAS_LST));
			if ((state == TAS_STATE_TERMINATING) ||
			    (state == TAS_STATE_ADMIN))
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if ((state != TAS_STATE_TERMINATING) &&
		    (state != TAS_STATE_ADMIN)) {
			netdev_err(port->dev,
				   "Timeout switching TAS state %s in list %d\n",
				   lan966x_tas_state_to_str(state), list);
			return -ETIME;
		}

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			state = QSYS_TAS_LST_LIST_STATE_GET(lan_rd(lan966x,
								   QSYS_TAS_LST));
			if (state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_ADMIN) {
			netdev_err(port->dev,
				   "Timeout switching TAS state %s in list %d\n",
				   lan966x_tas_state_to_str(state), list);
			return -ETIME;
		}

		/* Restore gate state to "all-queues-open" */
		/* Select port */
		lan_wr(QSYS_TAS_GS_CTRL_HSCH_POS_SET(port->chip_port),
		       lan966x, QSYS_TAS_GS_CTRL);
		/* Set gate state to "all-queues-open" */
		lan_wr(QSYS_TAS_GATE_STATE_TAS_GATE_STATE_SET(0xff),
		       lan966x, QSYS_TAS_GATE_STATE);

		/* Clear MAC_HOLD */
		lan_wr(SYS_FPORT_STATE_MAC_HOLD_SET(0),
		       lan966x, SYS_FPORT_STATE(port->chip_port));
	}
	return 0;
}

/*
 * Find a suitable list for a new schedule.
 * First priority is a list in state pending.
 * Second priority is a list in state admin.
 * If list found is in state pending it is shut down here.
 * Index of found list is returned in new.
 * If an operating list is found, the index is returned in obsolete.
 * This list must be configured to be shut down when the new list starts.
 *
 */
static int lan966x_tas_list_find(struct lan966x_port *port, int *new,
				 int *obsolete)
{
	int i, err, state_cnt[NUM_TAS_STATE] = {0};
	struct lan966x *lan966x = port->lan966x;
	int state[TAS_ENTRIES_PER_PORT];
	int list[TAS_ENTRIES_PER_PORT];
	bool valid = false;
	int oper = -1;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list[i] = lan966x_tas_list_index(port, i);
		lan_rmw(QSYS_TAS_CFG_CTRL_LIST_NUM_SET(list[i]),
			QSYS_TAS_CFG_CTRL_LIST_NUM,
			lan966x, QSYS_TAS_CFG_CTRL);

		state[i] = QSYS_TAS_LST_LIST_STATE_GET(lan_rd(lan966x,
							      QSYS_TAS_LST));
		if (state[i] >= NUM_TAS_STATE) {
			netdev_err(port->dev, "Invalid tas list state %u %u %d\n",
				   state[i], port->chip_port, i);
			return -EINVAL;
		}

		if (state[i] == TAS_STATE_OPERATING)
			oper = list[i];

		state_cnt[state[i]]++;
	}

	if (state_cnt[TAS_STATE_ADMIN] == 2)
		valid = true;
	if (state_cnt[TAS_STATE_ADMIN] == 1 && state_cnt[TAS_STATE_PENDING] == 1)
		valid = true;
	if (state_cnt[TAS_STATE_ADMIN] == 1 && state_cnt[TAS_STATE_OPERATING] == 1)
		valid = true;
	if (state_cnt[TAS_STATE_OPERATING] == 1 && state_cnt[TAS_STATE_PENDING] == 1)
		valid = true;

	if (!valid) {
		netdev_err(port->dev, "Invalid tas state combination: %d %d %d %d %d\n",
			   state_cnt[TAS_STATE_ADMIN],
			   state_cnt[TAS_STATE_ADVANCING],
			   state_cnt[TAS_STATE_PENDING],
			   state_cnt[TAS_STATE_OPERATING],
			   state_cnt[TAS_STATE_TERMINATING]);
		return -1;
	}

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		if (state[i] == TAS_STATE_PENDING) {
			err = lan966x_tas_shutdown_pending(port);
			if (err)
				return err;
			else {
				*new = list[i];
				*obsolete = (oper == -1) ? *new : oper;
				return 0;
			}
		}
	}

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		if (state[i] == TAS_STATE_ADMIN) {
			*new = list[i];
			*obsolete = (oper == -1) ? *new : oper;
			return 0;
		}
	}
	return -1; /* No suitable list found */
}

/*
 * Get a bitmap of all free GCLs
 * Return number of free GCLs found
 */
static int lan966x_tas_gcl_free_get(struct lan966x_port *port, unsigned long *free_list)
{
	struct lan966x *lan966x = port->lan966x;
	int num_free = TAS_NUM_GCL;
	int state, list;
	u32 base, next;

	bitmap_fill(free_list, TAS_NUM_GCL); /* Start with all free */

	for (list = 0; list < num_tas_lists; list++) {
		lan_rmw(QSYS_TAS_CFG_CTRL_LIST_NUM_SET(list),
			QSYS_TAS_CFG_CTRL_LIST_NUM,
			lan966x, QSYS_TAS_CFG_CTRL);

		state = QSYS_TAS_LST_LIST_STATE_GET(lan_rd(lan966x,
							   QSYS_TAS_LST));
		if (state == TAS_STATE_ADMIN)
			continue;

		base = QSYS_TAS_LIST_CFG_LIST_BASE_ADDR_GET(
			lan_rd(lan966x, QSYS_TAS_LIST_CFG));

		next = base;
		do {
			if (!test_bit(next, free_list)) {
				netdev_err(port->dev,
					   "List %d: GCL entry %u used multiple times!\n",
					   list, next);
				return -EEXIST;
			}
			clear_bit(next, free_list); /* Mark as not free */
			num_free--;

			lan_rmw(QSYS_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(next),
				QSYS_TAS_CFG_CTRL_GCL_ENTRY_NUM,
				lan966x, QSYS_TAS_CFG_CTRL);
			next = QSYS_TAS_GCL_CT_CFG2_NEXT_GCL_GET(
				lan_rd(lan966x, QSYS_TAS_GCL_CT_CFG2));

		} while (base != next);
	}
	return num_free;
}

/*
 * Setup GCLs for a specific list
 */
static int lan966x_tas_gcl_setup(struct lan966x_port *port, int list,
				 struct tc_taprio_qopt_offload *qopt)
{
	struct lan966x *lan966x = port->lan966x;
	DECLARE_BITMAP(free_list, TAS_NUM_GCL);
	int i, num_free, base, next, cmd;

	num_free = lan966x_tas_gcl_free_get(port, free_list);
	if (num_free < (int)qopt->num_entries) {
		netdev_info(port->dev, "Not enough free GCL entries!\n");
		return -1;
	}

	base = find_first_bit(free_list, TAS_NUM_GCL);
	if (base == TAS_NUM_GCL) {
		netdev_err(port->dev, "No more free GCL entries!\n");
		return -1;
	}

	netdev_dbg(port->dev, "gcl setup list %d, base %d num_free %d\n",
		   list, base, num_free);

	lan_rmw(QSYS_TAS_CFG_CTRL_LIST_NUM_SET(list),
		QSYS_TAS_CFG_CTRL_LIST_NUM,
		lan966x, QSYS_TAS_CFG_CTRL);

	lan_rmw(QSYS_TAS_LIST_CFG_LIST_BASE_ADDR_SET(base),
		QSYS_TAS_LIST_CFG_LIST_BASE_ADDR,
		lan966x, QSYS_TAS_LIST_CFG);

	next = base;
	for (i = 0; i < qopt->num_entries; i++) {
		lan_rmw(QSYS_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(next),
			QSYS_TAS_CFG_CTRL_GCL_ENTRY_NUM,
			lan966x, QSYS_TAS_CFG_CTRL);

		if (i == (qopt->num_entries - 1)) { /* This is the last entry */
			next = base; /* Point back to the start of the list */
		} else {
			next = find_next_bit(free_list, TAS_NUM_GCL, next + 1);
			if (next == TAS_NUM_GCL) {
				netdev_err(port->dev,
					   "No more free GCL entries!\n");
				return -1;
			}
		}

		switch (qopt->entries[i].command) {
		case 0:
			cmd = TAS_GCL_CMD_SET_GATE_STATES;
			break;
		case 1:
			cmd = TAS_GCL_CMD_SET_AND_HOLD_MAC;
			break;
		case 2:
			cmd = TAS_GCL_CMD_SET_AND_RELEASE_MAC;
			break;
		default:
			netdev_err(port->dev,
				   "TAS: Unsupported GCL command: %d\n",
				   qopt->entries[i].command);
			return -1;
		}

		lan_wr(QSYS_TAS_GCL_CT_CFG_GATE_STATE_SET(qopt->entries[i].gate_mask) |
		       QSYS_TAS_GCL_CT_CFG_HSCH_POS_SET(port->chip_port) |
		       QSYS_TAS_GCL_CT_CFG_OP_TYPE_SET(cmd),
		       lan966x, QSYS_TAS_GCL_CT_CFG);

		lan_wr(QSYS_TAS_GCL_CT_CFG2_PORT_PROFILE_SET(port->chip_port) |
		       QSYS_TAS_GCL_CT_CFG2_NEXT_GCL_SET(next),
		       lan966x, QSYS_TAS_GCL_CT_CFG2);

		lan_wr(qopt->entries[i].interval,
		       lan966x, QSYS_TAS_GCL_TM_CFG);
	}
	return 0;
}

int lan966x_tas_enable(struct lan966x_port *port,
		       struct tc_taprio_qopt_offload *qopt)
{

	int i, err, new_list = -1, obsolete = -1;
	struct lan966x *lan966x = port->lan966x;
	u64 cycle_time = qopt->cycle_time;
	u64 calculated_cycle_time = 0;
	struct timespec64 ts;
	ktime_t base_time;

	if (cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		netdev_err(port->dev, "Invalid cycle_time %llu\n",
			   (unsigned long long)cycle_time);
		return -EINVAL;
	}
	for (i = 0; i < qopt->num_entries; i++) {
		if (qopt->entries[i].interval < TAS_MIN_CYCLE_TIME_NS) {
			netdev_err(port->dev, "Invalid minimum cycle time %llu\n",
				   (unsigned long long)qopt->entries[i].interval);
			return -EINVAL;
		}
		if (qopt->entries[i].interval > TAS_MAX_CYCLE_TIME_NS) {
			netdev_err(port->dev, "Invalid maximum cycle time %llu\n",
				   (unsigned long long)qopt->entries[i].interval);
			return -EINVAL;
		}
		calculated_cycle_time += qopt->entries[i].interval;
	}
	if (calculated_cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		netdev_err(port->dev, "Invalid calculated_cycle_time %llu\n",
			   (unsigned long long)calculated_cycle_time);
		return -EINVAL;
	}
	if (cycle_time < calculated_cycle_time) {
		netdev_err(port->dev, "Invalid cycle_time %llu\n",
			   (unsigned long long)cycle_time);
		return -EINVAL;
	}

	lan966x_new_base_time(lan966x, cycle_time, qopt->base_time, &base_time);

	/* Select an apropriate entry to use */
	err = lan966x_tas_list_find(port, &new_list, &obsolete);
	netdev_dbg(port->dev, "lan966x_tas_list_find() returned %d %d %d\n", err, new_list, obsolete);
	if (err)
		return err;

	/* Setup GCL entries */
	err = lan966x_tas_gcl_setup(port, new_list, qopt);
	if (err)
		return err;

	/* Setup TAS list */
	ts = ktime_to_timespec64(base_time);
	lan_wr(QSYS_TAS_BT_NSEC_NSEC_SET(ts.tv_nsec),
	       lan966x, QSYS_TAS_BT_NSEC);

	lan_wr((ts.tv_sec & GENMASK(31, 0)),
	       lan966x, QSYS_TAS_BT_SEC_LSB);

	lan_wr(QSYS_TAS_BT_SEC_MSB_SEC_MSB_SET(ts.tv_sec >> 32),
	       lan966x, QSYS_TAS_BT_SEC_MSB);

	lan_wr(cycle_time, lan966x, QSYS_TAS_CT_CFG);

	lan_rmw(QSYS_TAS_STARTUP_CFG_OBSOLETE_IDX_SET(obsolete),
		QSYS_TAS_STARTUP_CFG_OBSOLETE_IDX,
		lan966x, QSYS_TAS_STARTUP_CFG);

	/* Start list processing */
	lan_rmw(QSYS_TAS_LST_LIST_STATE_SET(TAS_STATE_ADVANCING),
		QSYS_TAS_LST_LIST_STATE,
		lan966x, QSYS_TAS_LST);

	return err;
}

int lan966x_tas_disable(struct lan966x_port *port)
{
	int err;

	err = lan966x_tas_shutdown_pending(port);
	if (err)
		goto out;

	err = lan966x_tas_shutdown_operating(port);
out:
	return err;
}

static int lan966x_tas_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	struct lan966x_port *port = lan966x->ports[0];
	u32 sec, nsec, val, base, next, now, ct, c2, tm;
	int p, cp, e, list, state;
	struct timespec64 ts;

	rtnl_lock();
	P("num_ports", num_ports);
	P("num_tas_lists", num_tas_lists);
	lan966x_ptp_gettime64(&lan966x->phc[LAN966X_PHC_PORT].info, &ts);
	P_TIME("current time", ts.tv_sec, ts.tv_nsec);

	for (p = 0; p < num_ports; p++) {
		port = lan966x->ports[p];
		if (!port)
			continue;
		cp = port->chip_port;
		for (e = 0; e < TAS_ENTRIES_PER_PORT; e++) {
			list = (cp * TAS_ENTRIES_PER_PORT) + e;
			lan_rmw(QSYS_TAS_CFG_CTRL_LIST_NUM_SET(list),
				QSYS_TAS_CFG_CTRL_LIST_NUM,
				lan966x, QSYS_TAS_CFG_CTRL);
			val = lan_rd(lan966x, QSYS_TAS_LST);
			state = QSYS_TAS_LST_LIST_STATE_GET(val);
			if (state == TAS_STATE_ADMIN)
				continue;

			seq_printf(m, "\n%s:\n", port->dev->name);
			P(" chip_port", cp);
			P(" entry", e);
			P(" list", list);
			P_STR(" state", lan966x_tas_state_to_str(state));
			sec = lan_rd(lan966x, QSYS_TAS_BT_SEC_LSB);
			nsec = lan_rd(lan966x, QSYS_TAS_BT_NSEC);
			P_TIME(" base time", sec, nsec);
			ct = lan_rd(lan966x, QSYS_TAS_CT_CFG);
			P_TIME(" cycle_time", 0, ct);
			val = lan_rd(lan966x, QSYS_TAS_LIST_CFG);
			base = QSYS_TAS_LIST_CFG_LIST_BASE_ADDR_GET(val);
			P(" gcl base", base);
			next = base;
			do {
				lan_rmw(QSYS_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(next),
					QSYS_TAS_CFG_CTRL_GCL_ENTRY_NUM,
					lan966x, QSYS_TAS_CFG_CTRL);

				ct = lan_rd(lan966x, QSYS_TAS_GCL_CT_CFG);
				c2 = lan_rd(lan966x, QSYS_TAS_GCL_CT_CFG2);
				tm = lan_rd(lan966x, QSYS_TAS_GCL_TM_CFG);
				now = next;
				next = QSYS_TAS_GCL_CT_CFG2_NEXT_GCL_GET(c2);

				seq_printf(m, "  gcl %d: next %d command %lu gatemask 0x%02lx interval %u ns\n",
					   now,
					   next,
					   QSYS_TAS_GCL_CT_CFG_OP_TYPE_GET(ct),
					   QSYS_TAS_GCL_CT_CFG_GATE_STATE_GET(ct),
					   tm);

			} while (base != next);
		}
	}
	rtnl_unlock();
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_tas);

static int lan966x_tas_init(struct lan966x *lan966x)
{
	int i;

	num_ports = lan966x->num_phys_ports;
	num_tas_lists = num_ports * TAS_ENTRIES_PER_PORT;

	lan_wr(QSYS_TAS_STM_CFG_REVISIT_DLY_SET((256 * 1000) /
						lan966x_ptp_get_period_ps()),
	       lan966x, QSYS_TAS_STM_CFG);

	/* For now we always use guard band on all queues */
	lan_rmw(QSYS_TAS_CFG_CTRL_LIST_NUM_MAX_SET(num_tas_lists) |
		QSYS_TAS_CFG_CTRL_ALWAYS_GB_SCH_Q_SET(1),
		QSYS_TAS_CFG_CTRL_LIST_NUM_MAX |
		QSYS_TAS_CFG_CTRL_ALWAYS_GB_SCH_Q,
		lan966x, QSYS_TAS_CFG_CTRL);

	/* Associate profile with port */
	for (i = 0; i < num_ports; i++)
		lan_rmw(QSYS_TAS_PROFILE_CFG_PORT_NUM_SET(i),
			QSYS_TAS_PROFILE_CFG_PORT_NUM,
			lan966x, QSYS_TAS_PROFILE_CFG(i));

	debugfs_create_file("tas_show", 0444, lan966x->debugfs_root, lan966x,
			&lan966x_tas_fops);
	return 0;
}

void lan966x_tas_speed(struct lan966x_port *port, int speed)
{
	struct lan966x *lan966x = port->lan966x;
	u8 spd;

	netdev_dbg(port->dev, "speed %d\n", speed);

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
	default:
		netdev_info(port->dev, "TAS: Unsupported speed: %d\n", speed);
		return;
	}

	lan_rmw(QSYS_TAS_PROFILE_CFG_LINK_SPEED_SET(spd),
		QSYS_TAS_PROFILE_CFG_LINK_SPEED,
		lan966x, QSYS_TAS_PROFILE_CFG(port->chip_port));
}

/*******************************************************************************
 * PSFP (Per Stream Filtering and Policing - 802.1Qci)
 ******************************************************************************/
#define SFID_UPDATE_SLEEP_US       10
#define SFID_UPDATE_TIMEOUT_US 100000
#define SFIDACCESS_CMD_IDLE         0
#define SFIDACCESS_CMD_READ         1
#define SFIDACCESS_CMD_WRITE        2
#define SFIDACCESS_CMD_INIT         3

#define SGID_UPDATE_SLEEP_US       10
#define SGID_UPDATE_TIMEOUT_US 100000

#define PSFP_CNT_OFFSET 0x200 /* Stream filter counter offset */

static void lan966x_psfp_stats_upd(struct lan966x *lan966x, const u32 sfi_ix)
{
	struct lan966x_psfp_sfc *cnt = &lan966x->psfp.cnt[sfi_ix];
	struct lan966x_psfp_sfc old = *cnt;

	WARN_ON(!mutex_is_locked(&lan966x->stats_lock));

	lan_wr(SYS_STAT_CFG_STAT_VIEW_SET(sfi_ix), lan966x, SYS_STAT_CFG);
	lan966x_add_cnt(&cnt->matching_frames_count,
			lan_rd(lan966x, SYS_CNT(PSFP_CNT_OFFSET + 0)));
	lan966x_add_cnt(&cnt->not_passing_frames_count,
			lan_rd(lan966x, SYS_CNT(PSFP_CNT_OFFSET + 1)));
	lan966x_add_cnt(&cnt->not_passing_sdu_count,
			lan_rd(lan966x, SYS_CNT(PSFP_CNT_OFFSET + 2)));
	lan966x_add_cnt(&cnt->red_frames_count,
			lan_rd(lan966x, SYS_CNT(PSFP_CNT_OFFSET + 3)));
	/*
	 * The missing HW counters can be calculated as:
	 * passing_frames_count = matching_frames_count -
	 *			  not_passing_frames_count
	 * passing_sdu_count = passing_frames_count -
	 *		       not_passing_sdu_count
	 */

	/* Remember time for last change */
	if ((old.matching_frames_count    != cnt->matching_frames_count)    ||
	    (old.not_passing_frames_count != cnt->not_passing_frames_count) ||
	    (old.not_passing_sdu_count    != cnt->not_passing_sdu_count)    ||
	    (old.red_frames_count         != cnt->red_frames_count))
		cnt->lastused = jiffies;
}

/* Update all PSFP Stream Filter Counters */
static void lan966x_psfp_stats_upd_all(struct lan966x *lan966x)
{
	unsigned long sfi_ix;

	mutex_lock(&lan966x->stats_lock);

	for_each_set_bit(sfi_ix, lan966x->qos.sfi_pool, LAN966X_PSFP_NUM_SFI)
		lan966x_psfp_stats_upd(lan966x, sfi_ix);

	mutex_unlock(&lan966x->stats_lock);
}

/* Clear PSFP Stream Filter Counters */
static void lan966x_psfp_stats_clr(struct lan966x *lan966x, const u32 sfi_ix)
{
	struct lan966x_psfp_sfc *cnt = &lan966x->psfp.cnt[sfi_ix];
	struct lan966x_psfp_tcsfc *tcsfc = &lan966x->psfp.tcsfc[sfi_ix];

	mutex_lock(&lan966x->stats_lock);

	/* update lastused and clear sw counters */
	cnt->lastused = jiffies;

	cnt->matching_frames_count = 0;
	cnt->not_passing_frames_count = 0;
	cnt->not_passing_sdu_count = 0;
	cnt->red_frames_count = 0;

	tcsfc->drops = 0;

	/* clear hw counters */
	lan_wr(SYS_STAT_CFG_STAT_CLEAR_SHOT_SET(0x10) |
	       SYS_STAT_CFG_STAT_VIEW_SET(sfi_ix),
	       lan966x, SYS_STAT_CFG);

	mutex_unlock(&lan966x->stats_lock);
}

/* Get PSFP Stream Filter Counters */
void lan966x_psfp_stats_get(struct lan966x *lan966x,
			    const u32 sfi_ix,
			    struct lan966x_psfp_sfc *const c)
{
	struct lan966x_psfp_sfc *cnt = &lan966x->psfp.cnt[sfi_ix];

	mutex_lock(&lan966x->stats_lock);

	lan966x_psfp_stats_upd(lan966x, sfi_ix);
	*c = *cnt;

	mutex_unlock(&lan966x->stats_lock);
}

static inline int lan966x_sfid_get_status(struct lan966x *lan966x)
{
	return lan_rd(lan966x, ANA_SFIDACCESS);
}

static inline int lan966x_sfid_wait_for_completion(struct lan966x *lan966x)
{
	u32 val;

	return readx_poll_timeout(lan966x_sfid_get_status,
				  lan966x, val,
				  (ANA_SFIDACCESS_SFID_TBL_CMD_GET(val)) ==
				  SFIDACCESS_CMD_IDLE,
				  SFID_UPDATE_SLEEP_US,
				  SFID_UPDATE_TIMEOUT_US);
}

/* Set PSFP Stream Filter */
int lan966x_psfp_sf_set(struct lan966x *lan966x,
			const u32 sfi_ix,
			const struct lan966x_psfp_sf_cfg *const c)
{
	dev_dbg(lan966x->dev, "sfi_ix %u boe %d bo %d fb %d ms %u\n",
		sfi_ix,
		c->block_oversize_ena,
		c->block_oversize,
		c->force_block,
		c->max_sdu);

	if (sfi_ix >= LAN966X_PSFP_NUM_SFI) {
		dev_err(lan966x->dev, "Invalid sfi_ix %u\n", sfi_ix);
		return -EINVAL;
	}

	/* Clear stream filter statistics */
	lan966x_psfp_stats_clr(lan966x, sfi_ix);

	/* Select the stream filter to configure */
	lan_wr(ANA_SFIDTIDX_SFID_INDEX_SET(sfi_ix), lan966x, ANA_SFIDTIDX);

	lan_wr(ANA_SFIDACCESS_B_O_FRM_SET(c->block_oversize) |
	       ANA_SFIDACCESS_B_O_FRM_ENA_SET(c->block_oversize_ena) |
	       ANA_SFIDACCESS_MAX_SDU_LEN_SET(c->max_sdu) |
	       ANA_SFIDACCESS_SFID_TBL_CMD_SET(SFIDACCESS_CMD_WRITE),
	       lan966x, ANA_SFIDACCESS);

	return lan966x_sfid_wait_for_completion(lan966x);
}

/* Reset PSFP Stream Filter */
static int lan966x_psfp_sf_reset(struct lan966x *lan966x,
				 const u32 sfi_ix)
{
	dev_dbg(lan966x->dev, "sfi_ix %u\n", sfi_ix);

	/* Select the stream filter to configure and write zeroes */
	lan_wr(ANA_SFIDTIDX_SFID_INDEX_SET(sfi_ix),
	       lan966x, ANA_SFIDTIDX);

	lan_wr(ANA_SFIDACCESS_SFID_TBL_CMD_SET(SFIDACCESS_CMD_WRITE),
	       lan966x, ANA_SFIDACCESS);

	return lan966x_sfid_wait_for_completion(lan966x);
}

static inline int lan966x_sgid_get_status(struct lan966x *lan966x)
{
	return lan_rd(lan966x, ANA_SG_ACCESS_CTRL);
}

/* Wait for sg config change to complete or timeout. */
static inline int lan966x_sgid_wait_for_completion(struct lan966x *lan966x)
{
	u32 val;

	return readx_poll_timeout(lan966x_sgid_get_status,
				  lan966x,
				  val,
				  !(ANA_SG_ACCESS_CTRL_CONFIG_CHANGE_GET(val)),
				  SGID_UPDATE_SLEEP_US,
				  SGID_UPDATE_TIMEOUT_US);
}

/* Set PSFP Stream gate */
int lan966x_psfp_sg_set(struct lan966x *lan966x,
			const u32 sgi_ix,
			const struct lan966x_psfp_sg_cfg *const sg)
{
	u32 relative_time_interval[LAN966X_PSFP_NUM_GCE] = {0};
	u32 accumulated_time_interval = 0;
	struct timespec64 ts;
	ktime_t basetime;
	int i, ret = 0;
	u32 ipv = 0;

	dev_dbg(lan966x->dev, "sgi_ix %u ipv %d bt %llu ct %u cte %u gl %u\n",
		sgi_ix, sg->ipv, sg->basetime, sg->cycletime, sg->cycletimeext,
		sg->num_entries);

	for (i = 0; i < sg->num_entries; i++) {
		accumulated_time_interval += sg->gce[i].interval;
		relative_time_interval[i] = accumulated_time_interval;
	}

	lan966x_new_base_time(lan966x, sg->cycletime,
			      sg->basetime, &basetime);

	ts = ktime_to_timespec64(basetime);

	if (sg->ipv >= 0)
		ipv = sg->ipv | 0x08;
	else
		ipv = 0;

	/* Select stream gate */
	lan_wr(ANA_SG_ACCESS_CTRL_SGID_SET(sgi_ix),
	       lan966x, ANA_SG_ACCESS_CTRL);

	/* Set all sg registers */
	lan_wr(ts.tv_nsec, lan966x, ANA_SG_CFG_1);
	lan_wr(ts.tv_sec & 0xffffffff, lan966x, ANA_SG_CFG_2);
	lan_wr(ANA_SG_CFG_3_BASE_TIME_SEC_MSB_SET(ts.tv_sec >> 32) |
	       ANA_SG_CFG_3_LIST_LENGTH_SET(sg->num_entries) |
	       ANA_SG_CFG_3_GATE_ENABLE_SET(1) |
	       ANA_SG_CFG_3_INIT_IPS_SET(ipv) |
	       ANA_SG_CFG_3_INIT_GATE_STATE_SET(sg->gate_state),
	       lan966x, ANA_SG_CFG_3);
	lan_wr(sg->cycletime, lan966x, ANA_SG_CFG_4);
	lan_wr(sg->cycletimeext, lan966x, ANA_SG_CFG_5);

	/* Set all gcl registers */
	for (i = 0; i < sg->num_entries; i++) {
		if (sg->gce[i].ipv >= 0)
			ipv = sg->gce[i].ipv | 0x08;
		else
			ipv = 0;

		lan_wr(ANA_SG_GCL_GS_CFG_IPS_SET(ipv) |
		       ANA_SG_GCL_GS_CFG_GATE_STATE_SET(sg->gce[i].gate_state),
		       lan966x, ANA_SG_GCL_GS_CFG(i));

		lan_wr(relative_time_interval[i],
		       lan966x, ANA_SG_GCL_TI_CFG(i));

		lan_wr(max(sg->gce[i].maxoctets, 0),
		       lan966x, ANA_SG_GCL_OCT_CFG(i));

	}

	/* Start configuration change */
	lan_wr(ANA_SG_ACCESS_CTRL_SGID_SET(sgi_ix) |
	       ANA_SG_ACCESS_CTRL_CONFIG_CHANGE_SET(1),
	       lan966x, ANA_SG_ACCESS_CTRL);

	ret = lan966x_sgid_wait_for_completion(lan966x);
	if (ret)
		dev_err(lan966x->dev, "sgi %u: Config change timeout\n", sgi_ix);

	return ret;
}

/* Reset PSFP Stream Gate */
static int lan966x_psfp_sg_reset(struct lan966x *lan966x,
				 const u32 sgi_ix)
{
	int i;

	dev_dbg(lan966x->dev, "sgi_ix %u\n", sgi_ix);

	/* Select stream gate */
	lan_wr(ANA_SG_ACCESS_CTRL_SGID_SET(sgi_ix), lan966x, ANA_SG_ACCESS_CTRL);

	/* Set all stream gate registers to default values */
	lan_wr(0, lan966x, ANA_SG_CFG_1);
	lan_wr(0, lan966x, ANA_SG_CFG_2);
	lan_wr(ANA_SG_CFG_3_INIT_GATE_STATE, lan966x, ANA_SG_CFG_3);
	lan_wr(0, lan966x, ANA_SG_CFG_4);
	lan_wr(0, lan966x, ANA_SG_CFG_5);
	for (i = 0; i < LAN966X_PSFP_NUM_GCE; i++) {
		lan_wr(0, lan966x, ANA_SG_GCL_GS_CFG(i));
		lan_wr(0, lan966x, ANA_SG_GCL_TI_CFG(i));
		lan_wr(0, lan966x, ANA_SG_GCL_OCT_CFG(i));
	}

	return 0;
}

/* Reset PSFP Flow Meter */
static int lan966x_psfp_fm_reset(struct lan966x *lan966x,
				 const u32 pol_ix)
{
	struct qos_policer_conf pp = { 0 };

	dev_dbg(lan966x->dev, "pol_ix %u\n", pol_ix);

	pp.mode = RATE_MODE_DISABLED;
	return policer_conf_set(lan966x, pol_ix, &pp);
}

/**
 * lan966x_res_ix_reserve - Reserve a resource index
 * @lan966x: switch device.
 * @pool: The resource pool.
 * @pool_sz: The resource pool size.
 * @user: The user this entry belongs to.
 * @id: Id associated with this resource index.
 * @offset: Offset to add to res_ix.
 * @res_ix: The returned resource index.
 *
 * Search through the resource pool and if an entry with same user and id is
 * found then increment ref_cnt and return resource index.
 * If not found then find a free entry and initialize it with given user and id,
 * set ref_cnt to 1 and return resource index.
 *
 * Note that it is up to the caller to configure the resource.
 *
 * Returns
 *  0 if entry was found.
 *  1 if entry was created.
 *  -EINVAL if user is invalid.
 *  -ENOSPC if there is no more free entries.
 */
static int lan966x_res_ix_reserve(struct lan966x *lan966x,
				  struct lan966x_res_pool_entry *pool,
				  u32 pool_sz,
				  enum lan966x_res_pool_user user,
				  u32 id,
				  u16 offset,
				  u32 *res_ix)
{
	struct lan966x_res_pool_entry *e;
	int i, free = -1;

	if (user <= LAN966X_RES_POOL_FREE ||
	    user > LAN966X_RES_POOL_USER_MAX)
		return -EINVAL;

	for (i = 0; i < pool_sz; i++) {
		e = &pool[i];
		if (user == e->user && id == e->id) {
			e->ref_cnt++;
			*res_ix = i + offset;
			dev_dbg(lan966x->dev, "res_ix %u\n", *res_ix);
			return 0;
		}
		if (e->user == LAN966X_RES_POOL_FREE && free == -1)
			free = i; /* Save the first free entry */
	}
	if (free == -1) {
		return -ENOSPC;
	} else { /* free is the index of the first free entry */
		e = &pool[free];
		e->user = user;
		e->id = id;
		e->ref_cnt++;
		*res_ix = free + offset;
		dev_dbg(lan966x->dev, "res_ix %u\n", *res_ix);
		return 1;
	}
}

/**
 * lan966x_res_ix_release - Release a resource index
 * @lan966x: switch device.
 * @pool: The resource pool.
 * @pool_sz: The resource pool size.
 * @user: The user this entry belongs to.
 * @id: Id associated with this policer index.
 * @offset: Offset to add to res_ix.
 * @res_ix: The returned resource index.
 *
 * Search through the resource pool and if an entry with same user and id is
 * found then decrement ref_cnt.
 * If ref_cnt becomes zero then mark entry as free.
 *
 * Returns
 *  ref_cnt if entry was found and released.
 *  -EINVAL if user is invalid.
 *  -ENOENT if user and id was not found.
 */
static int lan966x_res_ix_release(struct lan966x *lan966x,
				  struct lan966x_res_pool_entry *pool,
				  u32 pool_sz,
				  enum lan966x_res_pool_user user,
				  u32 id,
				  u16 offset,
				  u32 *res_ix)
{
	struct lan966x_res_pool_entry *e;
	int i;

	if (user <= LAN966X_RES_POOL_FREE ||
	    user > LAN966X_RES_POOL_USER_MAX)
		return -EINVAL;

	for (i = 0; i < pool_sz; i++) {
		e = &pool[i];
		if (user == e->user && id == e->id) {
			if (e->ref_cnt == 0) {
				dev_err(lan966x->dev,
					"ref_cnt zero before decrement in entry%d\n",
					offset + i);
				return -EINVAL;
			}
			e->ref_cnt--;
			*res_ix = i + offset;
			if (e->ref_cnt == 0) {
				e->user = LAN966X_RES_POOL_FREE;
				e->id = 0;
			}
			return e->ref_cnt;
		}
	}

	return -ENOENT;
}

int lan966x_sfi_ix_reserve(struct lan966x *lan966x,
			   u32 *sfi_ix)
{
	u32 ix = find_first_zero_bit(lan966x->qos.sfi_pool,
				      LAN966X_PSFP_NUM_SFI);
	if (ix == LAN966X_PSFP_NUM_SFI)
		return -ENOSPC;

	set_bit(ix, lan966x->qos.sfi_pool);
	*sfi_ix = ix;

	dev_dbg(lan966x->dev, "reserve sfi_ix %u\n", *sfi_ix);

	return 0;
}

int lan966x_sfi_ix_release(struct lan966x *lan966x,
			   u32 sfi_ix)
{
	dev_dbg(lan966x->dev, "release sfi_ix %u\n", sfi_ix);

	if (sfi_ix >= LAN966X_PSFP_NUM_SFI)
		return -EINVAL;

	if (!test_and_clear_bit(sfi_ix, lan966x->qos.sfi_pool))
		return -EINVAL;

	dev_dbg(lan966x->dev, "Disable stream filter %d\n", sfi_ix);
	return lan966x_psfp_sf_reset(lan966x, sfi_ix);
}

int lan966x_sgi_ix_reserve(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id,
			   u32 *sgi_ix)
{
	dev_dbg(lan966x->dev, "user %d id %u\n", user, id);

	return lan966x_res_ix_reserve(lan966x,
				      lan966x->qos.sgi_pool,
				      LAN966X_PSFP_NUM_SGI,
				      user,
				      id,
				      0,
				      sgi_ix);
}

int lan966x_sgi_ix_release(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id)
{
	int ref_cnt;
	u32 sgi_ix;

	dev_dbg(lan966x->dev, "user %d id %u\n", user, id);

	ref_cnt = lan966x_res_ix_release(lan966x,
					 lan966x->qos.sgi_pool,
					 LAN966X_PSFP_NUM_SGI,
					 user,
					 id,
					 0,
					 &sgi_ix);
	if (ref_cnt < 0)
		return ref_cnt;

	if (ref_cnt == 0) {
		dev_dbg(lan966x->dev, "Disable stream gate %d\n", sgi_ix);
		return lan966x_psfp_sg_reset(lan966x, sgi_ix);
	}

	return 0;
}

int lan966x_pol_ix_reserve(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id,
			   u32 *pol_ix)
{
	dev_dbg(lan966x->dev, "user %d id %u\n", user, id);

	return lan966x_res_ix_reserve(lan966x,
				      lan966x->qos.pol_pool,
				      LAN966X_NUM_POL_POOL,
				      user,
				      id,
				      LAN966X_POL_IX_POOL,
				      pol_ix);
}

int lan966x_pol_ix_release(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id)
{
	int ref_cnt;
	u32 pol_ix;

	dev_dbg(lan966x->dev, "user %d id %u\n", user, id);

	ref_cnt = lan966x_res_ix_release(lan966x,
					 lan966x->qos.pol_pool,
					 LAN966X_NUM_POL_POOL,
					 user,
					 id,
					 LAN966X_POL_IX_POOL,
					 &pol_ix);
	if (ref_cnt < 0)
		return ref_cnt;

	if (ref_cnt == 0) {
		dev_dbg(lan966x->dev, "Disable policer %d\n", pol_ix);
		return lan966x_psfp_fm_reset(lan966x, pol_ix);
	}

	return 0;
}

static int lan966x_qos_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	struct lan966x_res_pool_entry *e;
	struct lan966x_psfp_sfc c;
	struct timespec64 ts;
	u64 pfc, psc;
	bool empty;
	int i;

	rtnl_lock();

	seq_printf(m, "PSFP Stream Filter Pool:\n");
	empty = true;
	for_each_set_bit(i, lan966x->qos.sfi_pool, LAN966X_PSFP_NUM_SFI) {
		empty = false;
		lan966x_psfp_stats_get(lan966x, i, &c);
		pfc = c.matching_frames_count - c.not_passing_frames_count;
		psc = pfc - c.not_passing_sdu_count;
		seq_printf(m,
			   "  ix %u: mfc: %llu pfc: %llu npfc: %llu psc: %llu npsc: %llu rfc: %llu lu: %lu\n",
			   i,
			   c.matching_frames_count,
			   pfc,
			   c.not_passing_frames_count,
			   psc,
			   c.not_passing_sdu_count,
			   c.red_frames_count,
			   c.lastused);
	}
	if (empty)
		seq_printf(m, "  no entries!\n");

	seq_printf(m, "PSFP Stream Gate Pool:\n");
	empty = true;
	for (i = 0; i < LAN966X_PSFP_NUM_SGI; i++) {
		e = &lan966x->qos.sgi_pool[i];
		if (e->user || e->ref_cnt || e->id) {
			empty = false;
			seq_printf(m, "  ix %d: user %d id %u ref_cnt %u\n",
				   i, e->user, e->id, e->ref_cnt);
		}
	}
	if (empty)
		seq_printf(m, "  no entries!\n");

	seq_printf(m, "PSFP and ACL Policer Pool:\n");
	empty = true;
	for (i = 0; i < LAN966X_NUM_POL_POOL; i++) {
		e = &lan966x->qos.pol_pool[i];
		if (e->user || e->ref_cnt || e->id) {
			empty = false;
			seq_printf(m, "  ix %d: user %d id %u ref_cnt %u\n",
				   LAN966X_POL_IX_POOL + i,
				   e->user, e->id, e->ref_cnt);
		}
	}
	if (empty)
		seq_printf(m, "  no entries!\n");

	lan966x_ptp_gettime64(&lan966x->phc[LAN966X_PHC_PORT].info, &ts);
	P_TIME("ptp current time", ts.tv_sec, ts.tv_nsec);

	rtnl_unlock();
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_qos);

static int lan966x_qos_hw_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	bool empty;
	int i;

	rtnl_lock();

	seq_printf(m, "PSFP Stream Filter:\n");
	empty = true;
	for (i = 0; i < LAN966X_PSFP_NUM_SFI; i++) {
		u32 cfg;
		/* Select the stream filter to read */
		lan_wr(ANA_SFIDTIDX_SFID_INDEX_SET(i),
		       lan966x, ANA_SFIDTIDX);

		lan_wr(ANA_SFIDACCESS_SFID_TBL_CMD_SET(SFIDACCESS_CMD_READ),
		       lan966x, ANA_SFIDACCESS);

		if (lan966x_sfid_wait_for_completion(lan966x)) {
			seq_printf(m, "ERROR: Timeout reading SFI %d!\n", i);
			break;
		}
		cfg = lan_rd(lan966x, ANA_SFIDACCESS);
		if (cfg) {
			empty = false;
			seq_printf(m, "  ix %d: %s, %s, %s, MAX_SDU %lx\n",
				   i,
				   ANA_SFIDACCESS_B_O_FRM_GET(cfg) ? "BOF" : "bof",
				   ANA_SFIDACCESS_B_O_FRM_ENA_GET(cfg) ? "BOFE" : "bofe",
				   ANA_SFIDACCESS_FORCE_BLOCK_GET(cfg) ? "FB" : "fb",
				   ANA_SFIDACCESS_MAX_SDU_LEN_GET(cfg));
		}
	}
	if (empty)
		seq_printf(m, "  no entries!\n");

	seq_printf(m, "PSFP Stream Gate:\n");
	empty = true;
	for (i = 0; i < LAN966X_PSFP_NUM_SGI; i++) {
		u32 any, cfg1, cfg2, cfg3, cfg4, cfg5;
		u32 gcl1[LAN966X_PSFP_NUM_GCE];
		u32 gcl2[LAN966X_PSFP_NUM_GCE];
		u32 gcl3[LAN966X_PSFP_NUM_GCE];
		int j;

		/* Select stream gate to read */
		lan_wr(ANA_SG_ACCESS_CTRL_SGID_SET(i), lan966x, ANA_SG_ACCESS_CTRL);

		/* Read all stream gate registers */
		cfg1 = lan_rd(lan966x, ANA_SG_CFG_1);
		any = cfg1;
		cfg2 = lan_rd(lan966x, ANA_SG_CFG_2);
		any |= cfg2;
		cfg3 = lan_rd(lan966x, ANA_SG_CFG_3);
		any |= cfg3 & ~ANA_SG_CFG_3_INIT_GATE_STATE;
		cfg4 = lan_rd(lan966x, ANA_SG_CFG_4);
		any |= cfg4;
		cfg5 = lan_rd(lan966x, ANA_SG_CFG_5);
		any |= cfg5;
		for (j = 0; j < LAN966X_PSFP_NUM_GCE; j++) {
			gcl1[j] = lan_rd(lan966x, ANA_SG_GCL_GS_CFG(j));
			any |= gcl1[j];
			gcl2[j] = lan_rd(lan966x, ANA_SG_GCL_TI_CFG(j));
			any |= gcl2[j];
			gcl3[j] = lan_rd(lan966x, ANA_SG_GCL_OCT_CFG(j));
			any |= gcl3[j];
		}

		if (any) {
			empty = false;
			seq_printf(m, "  ix %d: %x %x %x %x %x G0: %x %x %x "
				   "G1: %x %x %x G2: %x %x %x G3: %x %x %x\n",
				   i, cfg1, cfg2, cfg3, cfg4, cfg5,
				   gcl1[0], gcl2[0], gcl3[0],
				   gcl1[1], gcl2[1], gcl3[1],
				   gcl1[2], gcl2[2], gcl3[2],
				   gcl1[3], gcl2[3], gcl3[3]);
		}
	}
	if (empty)
		seq_printf(m, "  no entries!\n");

	seq_printf(m, "PSFP and ACL Policer:\n");
	empty = true;
	for (i = 0; i < LAN966X_NUM_POL_POOL; i++) {
		u32 pir, cir, mode, pir_state, cir_state, state;
		int ix = i + LAN966X_POL_IX_POOL;

		/* Read all policer registers */
		pir = lan_rd(lan966x, ANA_POL_PIR_CFG(ix));
		cir = lan_rd(lan966x, ANA_POL_CIR_CFG(ix));
		mode = lan_rd(lan966x, ANA_POL_MODE(ix));
		pir_state = lan_rd(lan966x, ANA_POL_PIR_STATE(ix));
		cir_state = lan_rd(lan966x, ANA_POL_CIR_STATE(ix));
		state = lan_rd(lan966x, ANA_POL_STATE(ix));
		if (pir || cir) {
			empty = false;
			seq_printf(m, "  ix %d: pr %lx pb %lx cr %lx cb %lx "
				   "%s %s ipg %lx mode %lx %s %s %s "
				   "pl %lx cl %lx %s lt %lx\n",
				   ix,
				   ANA_POL_PIR_CFG_PIR_RATE_GET(pir),
				   ANA_POL_PIR_CFG_PIR_BURST_GET(pir),
				   ANA_POL_CIR_CFG_CIR_RATE_GET(cir),
				   ANA_POL_CIR_CFG_CIR_BURST_GET(cir),
				   ANA_POL_MODE_DROP_ON_YELLOW_ENA_GET(mode) ? "DOYE" : "doye",
				   ANA_POL_MODE_MARK_ALL_FRMS_RED_ENA_GET(mode) ? "MAFRE" : "mafre",
				   ANA_POL_MODE_IPG_SIZE_GET(mode),
				   ANA_POL_MODE_FRM_MODE_GET(mode),
				   ANA_POL_MODE_DLB_COUPLED_GET(mode) ? "DC" : "dc",
				   ANA_POL_MODE_CIR_ENA_GET(mode) ? "CE" : "ce",
				   ANA_POL_MODE_OVERSHOOT_ENA_GET(mode) ? "OE" : "oe",
				   ANA_POL_PIR_STATE_PIR_LVL_GET(pir_state),
				   ANA_POL_CIR_STATE_CIR_LVL_GET(cir_state),
				   ANA_POL_STATE_MARK_ALL_FRMS_RED_SET_GET(state) ? "MAFRS" : "mafrs",
				   ANA_POL_STATE_LEAK_TIME_GET(state));
		}
	}
	if (empty)
		seq_printf(m, "  no entries!\n");

	rtnl_unlock();
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_qos_hw);

/*******************************************************************************
 * FP (Frame Preemption - 802.1Qbu/802.3br)
 ******************************************************************************/
static void lan966x_fp_enable(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;

	lan_rmw(DEV_VERIF_CONFIG_PRM_VERIFY_DIS_SET(port->fp.verify_disable_tx) |
		DEV_VERIF_CONFIG_PRM_VERIFY_TIME_SET(port->fp.verify_time),
		DEV_VERIF_CONFIG_PRM_VERIFY_DIS |
		DEV_VERIF_CONFIG_PRM_VERIFY_TIME,
		lan966x, DEV_VERIF_CONFIG(port->chip_port));

	lan_rmw(SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE_SET(port->fp.add_frag_size),
		SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE,
		lan966x, SYS_FRONT_PORT_MODE(port->chip_port));

	lan_rmw(DEV_ENABLE_CONFIG_MM_TX_ENA_SET(1),
		DEV_ENABLE_CONFIG_MM_TX_ENA,
		lan966x, DEV_ENABLE_CONFIG(port->chip_port));

	/* Enable queues after enabling port */
	lan_rmw(QSYS_PREEMPT_CFG_P_QUEUES_SET(port->fp.admin_status),
		QSYS_PREEMPT_CFG_P_QUEUES,
		lan966x, QSYS_PREEMPT_CFG(port->chip_port));
}

static void lan966x_fp_disable(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;

	/* Disable all queues before disabling port */
	lan_rmw(QSYS_PREEMPT_CFG_P_QUEUES_SET(0),
		QSYS_PREEMPT_CFG_P_QUEUES,
		lan966x, QSYS_PREEMPT_CFG(port->chip_port));

	/* Wait a little while for queued PMAC traffic to be sent */
	usleep_range(5000, 20000);

	/* Set registers to default values */
	lan_rmw(DEV_ENABLE_CONFIG_MM_TX_ENA_SET(0),
		DEV_ENABLE_CONFIG_MM_TX_ENA,
		lan966x, DEV_ENABLE_CONFIG(port->chip_port));

	lan_rmw(DEV_VERIF_CONFIG_PRM_VERIFY_DIS_SET(1) |
		DEV_VERIF_CONFIG_PRM_VERIFY_TIME_SET(10),
		DEV_VERIF_CONFIG_PRM_VERIFY_DIS |
		DEV_VERIF_CONFIG_PRM_VERIFY_TIME,
		lan966x, DEV_VERIF_CONFIG(port->chip_port));

	lan_rmw(SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE_SET(0),
		SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE,
		lan966x, SYS_FRONT_PORT_MODE(port->chip_port));
}

static void lan966x_fp_update(struct lan966x_port *port)
{
	if (port->fp.enable_tx &&
	    netif_carrier_ok(port->dev) &&
	    port->dev->phydev &&
	    (port->dev->phydev->speed >= SPEED_100) &&
	    (port->dev->phydev->duplex == DUPLEX_FULL))
		lan966x_fp_enable(port);
	else
		lan966x_fp_disable(port);
}

/*
 * In test scenarious where two ports from the same switch are connected back
 * to back the verification sometimes fails.
 * lan966x_fp_check() checks if verification has failed and restarts the
 * verification.
 */
static void lan966x_fp_check(struct lan966x *lan966x)
{
	struct lan966x_port *port;
	u32 status, verify_state;
	int i;

	for (i = 0; i < lan966x->num_phys_ports; i++) {
		port = lan966x->ports[i];
		if (!port)
			continue;

		rtnl_lock();
		status = lan_rd(lan966x, DEV_MM_STATUS(port->chip_port));
		verify_state = DEV_MM_STATUS_PRMPT_VERIFY_STATE_GET(status);
		if (port->fp.enable_tx &&
		    !port->fp.verify_disable_tx &&
		    netif_carrier_ok(port->dev) &&
		    /* Chip combines IDLE, SEND and WAIT into one */
		    (verify_state == (MCHP_MM_STATUS_VERIFY_FAILED - 2))) {
			netdev_dbg(port->dev, "Restart verification");
			lan966x_fp_disable(port);
			usleep_range(10000, 20000);
			lan966x_fp_update(port);
		}
		rtnl_unlock();
	}
}

int lan966x_fp_set(struct lan966x_port *port,
		   struct lan966x_fp_port_conf *c)
{
	bool unlock = false;

	netdev_dbg(port->dev,
		   "lan966x_fp_set() as %u et %d vdt %d vt %u afs %u\n",
		   c->admin_status,
		   c->enable_tx,
		   c->verify_disable_tx,
		   c->verify_time,
		   c->add_frag_size);

	if ((c->verify_time < 1) || (c->verify_time > 128)) {
		netdev_err(port->dev, "Invalid verify_time (%u)\n",
			   c->verify_time);
		return -EINVAL;
	}

	if (c->add_frag_size > 3) {
		netdev_err(port->dev, "Invalid add_frag_size (%u)\n",
			   c->add_frag_size);
		return -EINVAL;
	}

	/*
	 * Manually take rtnl_lock() if it isn't locked.
	 * This can be removed later when frame preemption is controlled by
	 * a standard user space tool such as ethtool, which aquires the lock.
	 */
	if (!rtnl_is_locked()) {
		unlock = true;
		rtnl_lock();
	}

	port->fp = *c;
	lan966x_fp_update(port);

	if (unlock)
		rtnl_unlock();
	return 0;
}

int lan966x_fp_get(struct lan966x_port *port,
		   struct lan966x_fp_port_conf *c)
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

int lan966x_fp_status(struct lan966x_port *port,
		      struct mchp_qos_fp_port_status *s)
{
	struct lan966x *lan966x = port->lan966x;
	bool unlock = false;
	u32 status;
	u32 state;
	int sv;

	if (!rtnl_is_locked()) {
		unlock = true;
		rtnl_lock();
	}

	s->hold_advance = 42; /* TBD */
	s->release_advance = 84; /* TBD */

	status = lan_rd(lan966x, DEV_MM_STATUS(port->chip_port));
	s->preemption_active = !!DEV_MM_STATUS_PRMPT_ACTIVE_STATUS_GET(status);

	if (!netif_carrier_ok(port->dev)) {
		/* Always INIT when no link */
		s->status_verify = 0;
	} else {
		/* Chip combines IDLE, SEND and WAIT into one */
		sv = DEV_MM_STATUS_PRMPT_VERIFY_STATE_GET(status);
		s->status_verify = sv == 0 ? 0 : (sv + 2);
	}

	state = lan_rd(lan966x, SYS_FPORT_STATE(port->chip_port));
	s->hold_request = SYS_FPORT_STATE_MAC_HOLD_GET(state);

	if (unlock)
		rtnl_unlock();

	return 0;
}

static int lan966x_fp_dbgfs_enabled(struct seq_file *file, void *offset)
{
	struct device *dev = file->private;
	struct net_device *ndev = to_net_dev(dev);
	struct lan966x_port *port = netdev_priv(ndev);
	bool enable_tx;

	rtnl_lock();
	enable_tx = port->fp.enable_tx;
	rtnl_unlock();

	seq_printf(file, "%d\n", enable_tx);
	return 0;
}

static int lan966x_fp_dbgfs_active(struct seq_file *file, void *offset)
{
	struct device *dev = file->private;
	struct net_device *ndev = to_net_dev(dev);
	struct lan966x_port *port = netdev_priv(ndev);
	struct lan966x *lan966x = port->lan966x;
	u32 status;

	rtnl_lock();
	status = lan_rd(lan966x, DEV_MM_STATUS(port->chip_port));
	rtnl_unlock();

	seq_printf(file, "%d\n",
		   DEV_MM_STATUS_PRMPT_ACTIVE_STATUS_GET(status) ? 1 : 0);
	return 0;
}

static int lan966x_fp_init(struct lan966x *lan966x)
{
	struct dentry *debugfs_dev_root;
	struct lan966x_port *port;
	u32 chip_port, val;
	int i;

	/* Initialize frame-preemption and sync config with defaults */
	for (i = 0; i < lan966x->num_phys_ports; i++) {
		port = lan966x->ports[i];
		if (!port)
			continue;
		chip_port = port->chip_port;

		/* Always enable MAC-MERGE Layer Receive block */
		lan_rmw(DEV_ENABLE_CONFIG_MM_RX_ENA_SET(1),
			DEV_ENABLE_CONFIG_MM_RX_ENA,
			lan966x, DEV_ENABLE_CONFIG(chip_port));

		/* Meet strict bandwidth requirements */
		lan_rmw(QSYS_PREEMPT_CFG_STRICT_IPG_SET(0),
			QSYS_PREEMPT_CFG_STRICT_IPG,
			lan966x, QSYS_PREEMPT_CFG(chip_port));

		val = lan_rd(lan966x, QSYS_PREEMPT_CFG(port->chip_port));
		port->fp.admin_status = QSYS_PREEMPT_CFG_P_QUEUES_GET(val);

		val = lan_rd(lan966x, DEV_ENABLE_CONFIG(chip_port));
		port->fp.enable_tx = DEV_ENABLE_CONFIG_MM_TX_ENA_GET(val);

		val = lan_rd(lan966x, DEV_VERIF_CONFIG(chip_port));
		port->fp.verify_disable_tx = DEV_VERIF_CONFIG_PRM_VERIFY_DIS_GET(val);
		port->fp.verify_time = DEV_VERIF_CONFIG_PRM_VERIFY_TIME_GET(val);

		val = lan_rd(lan966x, SYS_FRONT_PORT_MODE(chip_port));
		port->fp.add_frag_size = SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE_GET(val);

		/* Add per interface debugfs files for e.g. LLDP */
		debugfs_dev_root = debugfs_create_dir(port->dev->name,
						      lan966x->debugfs_root);
		debugfs_create_devm_seqfile(&port->dev->dev, "fp-enabled",
					    debugfs_dev_root,
					    lan966x_fp_dbgfs_enabled);
		debugfs_create_devm_seqfile(&port->dev->dev, "fp-active",
					    debugfs_dev_root,
					    lan966x_fp_dbgfs_active);
	}
	return 0;
}

/*******************************************************************************
 * FRER (Frame Replication and Elimination for Reliability - 802.1CB)
 ******************************************************************************/
#define FRER_UPDATE_SLEEP_US       10
#define FRER_UPDATE_TIMEOUT_US 100000
#define STREAMACCESS_CMD_IDLE       0
#define STREAMACCESS_CMD_READ       1
#define STREAMACCESS_CMD_WRITE      2
#define STREAMACCESS_CMD_INIT       3

static struct mchp_frer_stream_cfg cs_def_cfg;
static struct mchp_frer_stream_cfg ms_def_cfg;

/* Update 64 bit counter with difference from previous to current.
 * 32 bit unsigned arithmetic takes care of wrapping.
 * Frer counters are read-only and therefore not possible to clear.
 */

static inline void lan966x_upd_cnt(u64 *cnt, u32 *prev, u32 val)
{
	*cnt += val - *prev;
	*prev = val;
}
/* Update FRER compound stream counters */
static void lan966x_frer_update_cs_stats(struct lan966x *lan966x,
					 const u16 cs_id)
{
	struct mchp_frer_cnt *cnt = &lan966x->frer.cs_cnt[cs_id];
	struct lan966x_frer_prev_cnt *prev = &lan966x->frer.cs_prev_cnt[cs_id];

	WARN_ON(!mutex_is_locked(&lan966x->stats_lock));

	lan966x_upd_cnt(&cnt->out_of_order_packets, &prev->out_of_order_packets,
			lan_rd(lan966x, QSYS_CNT_CMP_OO(cs_id)));
	lan966x_upd_cnt(&cnt->rogue_packets, &prev->rogue_packets,
			lan_rd(lan966x, QSYS_CNT_CMP_RG(cs_id)));
	lan966x_upd_cnt(&cnt->passed_packets, &prev->passed_packets,
			lan_rd(lan966x, QSYS_CNT_CMP_PS(cs_id)));
	lan966x_upd_cnt(&cnt->discarded_packets, &prev->discarded_packets,
			lan_rd(lan966x, QSYS_CNT_CMP_DC(cs_id)));
	lan966x_upd_cnt(&cnt->lost_packets, &prev->lost_packets,
			lan_rd(lan966x, QSYS_CNT_CMP_LS(cs_id)));
	lan966x_upd_cnt(&cnt->tagless_packets, &prev->tagless_packets,
			lan_rd(lan966x, QSYS_CNT_CMP_TL(cs_id)));
	lan966x_upd_cnt(&cnt->resets, &prev->resets,
			lan_rd(lan966x, QSYS_CNT_CMP_RS(cs_id)));
}

/* Update FRER member stream counters */
static void lan966x_frer_update_ms_stats(struct lan966x *lan966x,
					 const u16 ms_id)
{
	struct mchp_frer_cnt *cnt = &lan966x->frer.ms_cnt[ms_id];
	struct lan966x_frer_prev_cnt *prev = &lan966x->frer.ms_prev_cnt[ms_id];

	WARN_ON(!mutex_is_locked(&lan966x->stats_lock));

	lan966x_upd_cnt(&cnt->out_of_order_packets, &prev->out_of_order_packets,
			lan_rd(lan966x, QSYS_CNT_MBM_OO(ms_id)));
	lan966x_upd_cnt(&cnt->rogue_packets, &prev->rogue_packets,
			lan_rd(lan966x, QSYS_CNT_MBM_RG(ms_id)));
	lan966x_upd_cnt(&cnt->passed_packets, &prev->passed_packets,
			lan_rd(lan966x, QSYS_CNT_MBM_PS(ms_id)));
	lan966x_upd_cnt(&cnt->discarded_packets, &prev->discarded_packets,
			lan_rd(lan966x, QSYS_CNT_MBM_DC(ms_id)));
	lan966x_upd_cnt(&cnt->lost_packets, &prev->lost_packets,
			lan_rd(lan966x, QSYS_CNT_MBM_LS(ms_id)));
	lan966x_upd_cnt(&cnt->tagless_packets, &prev->tagless_packets,
			lan_rd(lan966x, QSYS_CNT_MBM_TL(ms_id)));
	lan966x_upd_cnt(&cnt->resets, &prev->resets,
			lan_rd(lan966x, QSYS_CNT_MBM_RS(ms_id)));
}

/* Update all FRER Stream Counters */
static void lan966x_frer_update_stats_all(struct lan966x *lan966x)
{
	int i;

	mutex_lock(&lan966x->stats_lock);

	/* Update all compound stream counters */
	for (i = 0; i < LAN966X_FRER_NUM_CSI; i++) {
		if (lan966x->frer.cs_cfg[i].enable) {
			lan966x_frer_update_cs_stats(lan966x, i);
		}
	}

	/* Update all member stream counters */
	for (i = 0; i < LAN966X_FRER_NUM_MSI; i++) {
		if (lan966x->frer.ms_cfg[i].enable) {
			lan966x_frer_update_ms_stats(lan966x, i);
		}
	}

	mutex_unlock(&lan966x->stats_lock);
}

static int lan966x_frer_cs_cfg_update(struct lan966x *lan966x, const u16 cs_id)
{
	struct mchp_frer_stream_cfg *c = &lan966x->frer.cs_cfg[cs_id];
	bool vector = (c->alg == MCHP_FRER_REC_ALG_VECTOR);
	u8 hlen = (c->hlen < 2) ? 1 : (c->hlen - 1);
	u16 rt = (c->reset_time) ? c->reset_time : 1;

	lan_wr(QSYS_FRER_CFG_CMP_TAKE_NO_SEQUENCE_SET(c->take_no_seq) |
	       QSYS_FRER_CFG_CMP_VECTOR_ALGORITHM_SET(vector) |
	       QSYS_FRER_CFG_CMP_HISTORY_LENGTH_SET(hlen) |
	       QSYS_FRER_CFG_CMP_RESET_TICKS_SET(rt) |
	       QSYS_FRER_CFG_CMP_RESET_SET(1) |
	       QSYS_FRER_CFG_CMP_ENABLE_SET(c->enable),
	       lan966x, QSYS_FRER_CFG_CMP(cs_id));
	return 0;
}

static int lan966x_frer_ms_cfg_update(struct lan966x *lan966x, const u16 ms_id)
{
	struct mchp_frer_stream_cfg *c = &lan966x->frer.ms_cfg[ms_id];
	bool vector = (c->alg == MCHP_FRER_REC_ALG_VECTOR);
	u8 hlen = (c->hlen < 2) ? 1 : (c->hlen - 1);
	u16 rt = (c->reset_time) ? c->reset_time : 1;

	lan_wr(QSYS_FRER_CFG_MBM_TAKE_NO_SEQUENCE_SET(c->take_no_seq) |
	       QSYS_FRER_CFG_MBM_VECTOR_ALGORITHM_SET(vector) |
	       QSYS_FRER_CFG_MBM_HISTORY_LENGTH_SET(hlen) |
	       QSYS_FRER_CFG_MBM_RESET_TICKS_SET(rt) |
	       QSYS_FRER_CFG_MBM_RESET_SET(1) |
	       QSYS_FRER_CFG_MBM_ENABLE_SET(c->enable) |
	       QSYS_FRER_CFG_MBM_COMPOUND_HANDLE_SET(c->cs_id),
	       lan966x, QSYS_FRER_CFG_MBM(ms_id));
	return 0;
}

static inline int lan966x_frer_get_status(struct lan966x *lan966x)
{
	return lan_rd(lan966x, ANA_STREAMACCESS);
}

static inline int lan966x_frer_wait_for_completion(struct lan966x *lan966x)
{
	u32 val;

	return readx_poll_timeout(lan966x_frer_get_status,
				  lan966x, val,
				  (ANA_STREAMACCESS_STREAM_TBL_CMD_GET(val)) ==
				  STREAMACCESS_CMD_IDLE,
				  FRER_UPDATE_SLEEP_US,
				  FRER_UPDATE_TIMEOUT_US);
}
static int lan966x_frer_iflow_update(struct lan966x *lan966x, const u16 id)
{
	struct mchp_frer_iflow_cfg *c = &lan966x->frer.iflow_cfg[id].frer;
	u16 alloc_ix = c->ms_id / MCHP_FRER_MAX_PORTS;
	unsigned long port_mask;
	u8 chip_port;
	int i, ret;

	lan_wr(ANA_SPLIT_MASK_SPLIT_MASK_SET(c->split_mask),
	       lan966x, ANA_SPLIT_MASK);
	lan_wr(ANA_INPUT_PORT_MASK_INPUT_PORT_MASK_SET(GENMASK(CPU_PORT, 0)),
	       lan966x, ANA_INPUT_PORT_MASK);
	lan_wr(ANA_STREAMTIDX_S_INDEX_SET(id) |
	       ANA_STREAMTIDX_STREAM_SPLIT_SET(!!c->split_mask),
	       lan966x, ANA_STREAMTIDX);
	lan_wr(ANA_STREAMACCESS_GEN_SEQ_NUM_SET(0) |
	       ANA_STREAMACCESS_RTAG_POP_ENA_SET(c->pop ? 1 : 0) |
	       ANA_STREAMACCESS_SEQ_GEN_ENA_SET(c->generation ? 1 : 0) |
	       ANA_STREAMACCESS_STREAM_TBL_CMD_SET(STREAMACCESS_CMD_WRITE),
	       lan966x, ANA_STREAMACCESS);

	ret = lan966x_frer_wait_for_completion(lan966x);
	if (ret) {
		dev_err(lan966x->dev, "id %u: Config change timeout\n", id);
		return ret;
	}

	if (c->ms_enable) {
		port_mask = lan966x->frer.ms_adm[alloc_ix].port_mask;
		/* Map all member ports */
		lan_wr(QSYS_FRER_FIRST_FRER_FIRST_MEMBER_SET(c->ms_id),
		       lan966x, QSYS_FRER_FIRST(id));
		i = 0;
		for_each_set_bit(chip_port, &port_mask, 8) {
			lan_wr(QSYS_FRER_PORT_FRER_EGR_PORT_SET(chip_port),
			       lan966x, QSYS_FRER_PORT(id, i));
			i++;
		}
		for (; i < 4; i++) { /* Disable FRER for the rest */
			lan_wr(QSYS_FRER_PORT_FRER_EGR_PORT_SET(0xf),
			       lan966x, QSYS_FRER_PORT(id, i));
		}
	}

	return 0;
}

int lan966x_frer_cs_cfg_get(struct lan966x *lan966x,
			    const u16 cs_id,
			    struct mchp_frer_stream_cfg *const cfg)
{
	dev_dbg(lan966x->dev, "cs_id %u\n", cs_id);

	if (cs_id >= ARRAY_SIZE(lan966x->frer.cs_cfg)) {
		dev_err(lan966x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(lan966x->frer.cs_cfg) - 1);
		return -EINVAL;
	}

	*cfg = lan966x->frer.cs_cfg[cs_id];
	return 0;
}

int lan966x_frer_cs_cfg_set(struct lan966x *lan966x,
			    const u16 cs_id,
			    const struct mchp_frer_stream_cfg *const cfg)
{
	dev_dbg(lan966x->dev, "cs_id %u e %d a %s h %d r %d t %d\n",
		cs_id, cfg->enable,
		(cfg->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M",
		cfg->hlen, cfg->reset_time, cfg->take_no_seq);

	if (cs_id >= ARRAY_SIZE(lan966x->frer.cs_cfg)) {
		dev_err(lan966x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(lan966x->frer.cs_cfg) - 1);
		return -EINVAL;
	}

	if ((cfg->alg != MCHP_FRER_REC_ALG_VECTOR) &&
	    (cfg->alg != MCHP_FRER_REC_ALG_MATCH)) {
		dev_err(lan966x->dev, "Invalid alg (%d). Use %d for vector and %d for match\n",
			cfg->alg, MCHP_FRER_REC_ALG_VECTOR,
			MCHP_FRER_REC_ALG_MATCH);
		return -EINVAL;
	}

	if ((cfg->hlen < LAN966X_FRER_HLEN_MIN) ||
	    (cfg->hlen > LAN966X_FRER_HLEN_MAX)) {
		dev_err(lan966x->dev, "Invalid hlen (%d). Use %d..%d\n",
			cfg->hlen, LAN966X_FRER_HLEN_MIN, LAN966X_FRER_HLEN_MAX);
		return -EINVAL;
	}

	if ((cfg->reset_time < LAN966X_FRER_RESET_MIN) ||
	    (cfg->reset_time > LAN966X_FRER_RESET_MAX)) {
		dev_err(lan966x->dev, "Invalid reset_time (%d). Use %d..%d\n",
			cfg->reset_time, LAN966X_FRER_RESET_MIN,
			LAN966X_FRER_RESET_MAX);
		return -EINVAL;
	}

	lan966x->frer.cs_cfg[cs_id] = *cfg;
	return lan966x_frer_cs_cfg_update(lan966x, cs_id);
}

int lan966x_frer_cs_cnt_get(struct lan966x *lan966x,
			    const u16 cs_id,
			    struct mchp_frer_cnt *const cnt)
{
	dev_dbg(lan966x->dev, "cs_id %u\n", cs_id);

	if (cs_id >= ARRAY_SIZE(lan966x->frer.cs_cfg)) {
		dev_err(lan966x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(lan966x->frer.cs_cfg) - 1);
		return -EINVAL;
	}

	mutex_lock(&lan966x->stats_lock);
	lan966x_frer_update_cs_stats(lan966x, cs_id);
	*cnt = lan966x->frer.cs_cnt[cs_id];
	mutex_unlock(&lan966x->stats_lock);
	return 0;
}

int lan966x_frer_cs_cnt_clear(struct lan966x *lan966x,
			      const u16 cs_id)
{
	struct mchp_frer_cnt *cnt;

	dev_dbg(lan966x->dev, "cs_id %u\n", cs_id);

	if (cs_id >= ARRAY_SIZE(lan966x->frer.cs_cfg)) {
		dev_err(lan966x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(lan966x->frer.cs_cfg) - 1);
		return -EINVAL;
	}

	mutex_lock(&lan966x->stats_lock);
	cnt = &lan966x->frer.cs_cnt[cs_id];
	cnt->out_of_order_packets = 0;
	cnt->rogue_packets = 0;
	cnt->passed_packets = 0;
	cnt->discarded_packets = 0;
	cnt->lost_packets = 0;
	cnt->tagless_packets = 0;
	cnt->resets = 0;
	mutex_unlock(&lan966x->stats_lock);
	return 0;
}

int lan966x_frer_ms_alloc(struct lan966x *lan966x,
			  const u8 port_mask,
			  u16 *const ms_id)
{
	int i;

	dev_dbg(lan966x->dev, "port_mask 0x%x\n", port_mask);

	for (i = 0; i < ARRAY_SIZE(lan966x->frer.ms_adm); i++) {
		if (!lan966x->frer.ms_adm[i].port_mask)
			break;  /* Found a free entry */
	}

	if (i >= ARRAY_SIZE(lan966x->frer.ms_adm))
		return -1; /* No more free entries */

	if (!port_mask) {
		dev_err(lan966x->dev, "No ports\n");
		return -EINVAL;
	}
	if (hweight8(port_mask) > MCHP_FRER_MAX_PORTS) {
		dev_err(lan966x->dev, "More than %d ports\n",
			MCHP_FRER_MAX_PORTS);
		return -EINVAL;
	}

	lan966x->frer.ms_adm[i].port_mask = port_mask; /* Mark as in use */
	*ms_id = i * MCHP_FRER_MAX_PORTS;
	return 0;
}

int lan966x_frer_ms_free(struct lan966x *lan966x,
			 const u16 ms_id)
{
	u16 alloc_ix = ms_id / MCHP_FRER_MAX_PORTS;
	int i;

	dev_dbg(lan966x->dev, "ms_id %u\n", ms_id);

	if ((ms_id % MCHP_FRER_MAX_PORTS) ||
	    (ms_id >= ARRAY_SIZE(lan966x->frer.ms_cfg))) {
		dev_err(lan966x->dev, "Invalid ms_id (%d)\n", ms_id);
		return -EINVAL;
	}

	if (!lan966x->frer.ms_adm[alloc_ix].port_mask) {
		dev_err(lan966x->dev, "Unused ms_id (%d)\n", ms_id);
		return -EINVAL;
	}

	lan966x->frer.ms_adm[alloc_ix].port_mask = 0; /* Mark as free */

	/* Set involved member streams to default values */
	for (i = 0; i < MCHP_FRER_MAX_PORTS; i++) {
		lan966x->frer.ms_cfg[ms_id + i] = ms_def_cfg;
		lan966x_frer_ms_cfg_update(lan966x, ms_id + i);
	}

	/* Unmap all member ports */
	lan_wr(QSYS_FRER_FIRST_FRER_FIRST_MEMBER_SET(0),
	       lan966x, QSYS_FRER_FIRST(alloc_ix));
	for (i = 0; i < 4; i++) {
		lan_wr(QSYS_FRER_PORT_FRER_EGR_PORT_SET(0xf),
		       lan966x, QSYS_FRER_PORT(alloc_ix, i));
	}
	return 0;
}

/* Check if ms_id is within limits and if port is part of member stream */
/* Returns port index or negative error code */
static int lan966x_frer_ms_check(struct lan966x *lan966x,
				 struct lan966x_port *port,
				 const u16 ms_id)
{
	u16 alloc_ix = ms_id / MCHP_FRER_MAX_PORTS;
	unsigned long port_mask;
	u8 chip_port;
	int ix = 0;

	if ((alloc_ix >= ARRAY_SIZE(lan966x->frer.ms_adm)) ||
	    (ms_id % MCHP_FRER_MAX_PORTS) ||
	    (ms_id >= ARRAY_SIZE(lan966x->frer.ms_cfg))) {
		dev_err(lan966x->dev, "Invalid ms_id (%d). Use even numbers from 0 to %u\n",
			ms_id, (u32)ARRAY_SIZE(lan966x->frer.ms_adm) - 2);
		return -EINVAL;
	}

	port_mask = lan966x->frer.ms_adm[alloc_ix].port_mask;

	/* Check if port is part of member stream */
	if (!(port_mask & BIT(port->chip_port))) {
		dev_err(lan966x->dev, "Port is not member of ms_id %d\n",
			ms_id);
		return -EINVAL;
	}

	for_each_set_bit(chip_port, &port_mask, 8) {
		if (chip_port == port->chip_port)
			break;
		ix++;
	}

	if (ix >= MCHP_FRER_MAX_PORTS) {
		dev_err(lan966x->dev, "Invalid port_mask 0x%02lx\n", port_mask);
		return -EINVAL;
	}

	return ix;
}

int lan966x_frer_ms_cfg_get(struct lan966x_port *port,
			    const u16 ms_id,
			    struct mchp_frer_stream_cfg *const cfg)
{
	struct lan966x *lan966x = port->lan966x;
	int i;

	dev_dbg(lan966x->dev, "dev %s ms_id %u\n", port->dev->name, ms_id);

	i = lan966x_frer_ms_check(lan966x, port, ms_id);
	if (i < 0)
		return i;

	*cfg = lan966x->frer.ms_cfg[ms_id + i];
	return 0;
}

int lan966x_frer_ms_cfg_set(struct lan966x_port *port,
			    const u16 ms_id,
			    const struct mchp_frer_stream_cfg *const cfg)
{
	struct lan966x *lan966x = port->lan966x;
	int i;

	dev_dbg(lan966x->dev, "dev %s ms_id %u e %d a %s h %d r %d t %d cs %d\n",
		port->dev->name, ms_id, cfg->enable,
		(cfg->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M",
		cfg->hlen, cfg->reset_time, cfg->take_no_seq, cfg->cs_id);

	if ((cfg->alg != MCHP_FRER_REC_ALG_VECTOR) &&
	    (cfg->alg != MCHP_FRER_REC_ALG_MATCH)) {
		dev_err(lan966x->dev, "Invalid alg (%d). Use %d for vector and %d for match\n",
			cfg->alg, MCHP_FRER_REC_ALG_VECTOR,
			MCHP_FRER_REC_ALG_MATCH);
		return -EINVAL;
	}

	if ((cfg->hlen < LAN966X_FRER_HLEN_MIN) ||
	    (cfg->hlen > LAN966X_FRER_HLEN_MAX)) {
		dev_err(lan966x->dev, "Invalid hlen (%d). Use %d..%d\n",
			cfg->hlen, LAN966X_FRER_HLEN_MIN, LAN966X_FRER_HLEN_MAX);
		return -EINVAL;
	}

	if ((cfg->reset_time < LAN966X_FRER_RESET_MIN) ||
	    (cfg->reset_time > LAN966X_FRER_RESET_MAX)) {
		dev_err(lan966x->dev, "Invalid reset_time (%d). Use %d..%d\n",
			cfg->reset_time, LAN966X_FRER_RESET_MIN,
			LAN966X_FRER_RESET_MAX);
		return -EINVAL;
	}

	if (cfg->cs_id >= LAN966X_FRER_NUM_CSI) {
		dev_err(lan966x->dev, "Invalid cs_id (%d). Use 0..%d\n",
			cfg->cs_id, LAN966X_FRER_NUM_CSI - 1);
		return -EINVAL;
	}

	i = lan966x_frer_ms_check(lan966x, port, ms_id);
	if (i < 0)
		return i;

	lan966x->frer.ms_cfg[ms_id + i] = *cfg;
	return lan966x_frer_ms_cfg_update(lan966x, ms_id + i);
}

int lan966x_frer_ms_cnt_get(struct lan966x_port *port,
			    const u16 ms_id,
			    struct mchp_frer_cnt *const cnt)
{
	struct lan966x *lan966x = port->lan966x;
	u16 id;
	int i;

	dev_dbg(lan966x->dev, "dev %s ms_id %u\n", port->dev->name, ms_id);

	i = lan966x_frer_ms_check(lan966x, port, ms_id);
	if (i < 0)
		return i;

	id = ms_id + i;
	mutex_lock(&lan966x->stats_lock);
	lan966x_frer_update_ms_stats(lan966x, id);
	*cnt = lan966x->frer.ms_cnt[id];
	mutex_unlock(&lan966x->stats_lock);
	return 0;
}

int lan966x_frer_ms_cnt_clear(struct lan966x_port *port,
			      const u16 ms_id)
{
	struct lan966x *lan966x = port->lan966x;
	struct mchp_frer_cnt *cnt;
	u16 id;
	int i;

	dev_dbg(lan966x->dev, "dev %s ms_id %u\n", port->dev->name, ms_id);

	i = lan966x_frer_ms_check(lan966x, port, ms_id);
	if (i < 0)
		return i;

	id = ms_id + i;
	mutex_lock(&lan966x->stats_lock);
	cnt = &lan966x->frer.ms_cnt[id];
	cnt->out_of_order_packets = 0;
	cnt->rogue_packets = 0;
	cnt->passed_packets = 0;
	cnt->discarded_packets = 0;
	cnt->lost_packets = 0;
	cnt->tagless_packets = 0;
	cnt->resets = 0;
	mutex_unlock(&lan966x->stats_lock);
	return 0;
}

int lan966x_iflow_cfg_get(struct lan966x *lan966x,
			  const u16 id,
			  struct mchp_iflow_cfg *const cfg)
{
	dev_dbg(lan966x->dev, "id %u\n", id);

	if ((id < LAN966X_FRER_FLOW_MIN) ||
	    (id >= ARRAY_SIZE(lan966x->frer.iflow_cfg))) {
		dev_err(lan966x->dev, "Invalid id (%u). Use %d..%u\n",
			id, LAN966X_FRER_FLOW_MIN,
			(u32)ARRAY_SIZE(lan966x->frer.iflow_cfg) - 1);
		return -EINVAL;
	}

	*cfg = lan966x->frer.iflow_cfg[id];
	return 0;
}

int lan966x_iflow_cfg_set(struct lan966x *lan966x,
			  const u16 id,
			  const struct mchp_iflow_cfg *const cfg)
{
	dev_dbg(lan966x->dev, "id %d me %d m %u g %d p %d sm 0x%x\n",
		id, cfg->frer.ms_enable, cfg->frer.ms_id, cfg->frer.generation,
		cfg->frer.pop, cfg->frer.split_mask);

	if ((id < LAN966X_FRER_FLOW_MIN) ||
	    (id >= ARRAY_SIZE(lan966x->frer.iflow_cfg))) {
		dev_err(lan966x->dev, "Invalid id (%u). Use %d..%u\n",
			id, LAN966X_FRER_FLOW_MIN,
			(u32)ARRAY_SIZE(lan966x->frer.iflow_cfg) - 1);
		return -EINVAL;
	}

	if (cfg->frer.ms_id >= LAN966X_FRER_NUM_MSI) {
		dev_err(lan966x->dev, "Invalid ms_id (%u). Use 0..%d\n",
			cfg->frer.ms_id, LAN966X_FRER_NUM_MSI - 1);
		return -EINVAL;
	}

	if (cfg->frer.ms_enable && cfg->frer.generation) {
		dev_err(lan966x->dev,
			"Cannot have member stream together with generation\n");
		return -EINVAL;
	}

	if (cfg->frer.generation && cfg->frer.pop) {
		dev_err(lan966x->dev, "Cannot have generation together with pop\n");
		return -EINVAL;
	}

	if (hweight8(cfg->frer.split_mask) > MCHP_FRER_MAX_PORTS) {
		dev_err(lan966x->dev, "Cannot have more than %d ports\n",
			MCHP_FRER_MAX_PORTS);
		return -EINVAL;
	}

	lan966x->frer.iflow_cfg[id] = *cfg;
	return lan966x_frer_iflow_update(lan966x, id);
}

int lan966x_frer_vlan_cfg_get(struct lan966x *lan966x,
			      const u16 vid,
			      struct mchp_frer_vlan_cfg *const cfg)
{
	dev_dbg(lan966x->dev, "vid %u\n", vid);

	if (vid >= ARRAY_SIZE(lan966x->vlan_flags)) {
		dev_err(lan966x->dev, "Invalid vid (%u). Use 0..%u\n",
			vid, (u32)ARRAY_SIZE(lan966x->vlan_flags) - 1);
		return -EINVAL;
	}

	cfg->flood_disable =
		!!(lan966x->vlan_flags[vid] & LAN966X_VLAN_FLOOD_DIS);
	cfg->learn_disable =
		!!(lan966x->vlan_flags[vid] & LAN966X_VLAN_LEARN_DISABLED);
	return 0;
}

int lan966x_frer_vlan_cfg_set(struct lan966x *lan966x,
			      const u16 vid,
			      const struct mchp_frer_vlan_cfg *const cfg)
{
	dev_dbg(lan966x->dev, "vid %u fd %d ld %d\n",
		vid, cfg->flood_disable, cfg->learn_disable);

	if (vid >= ARRAY_SIZE(lan966x->vlan_flags)) {
		dev_err(lan966x->dev, "Invalid vid (%u). Use 0..%u\n",
			vid, (u32)ARRAY_SIZE(lan966x->vlan_flags) - 1);
		return -EINVAL;
	}

	if (cfg->flood_disable)
		lan966x->vlan_flags[vid] |= LAN966X_VLAN_FLOOD_DIS;
	else
		lan966x->vlan_flags[vid] &= ~LAN966X_VLAN_FLOOD_DIS;

	if (cfg->learn_disable)
		lan966x->vlan_flags[vid] |= LAN966X_VLAN_LEARN_DISABLED;
	else
		lan966x->vlan_flags[vid] &= ~LAN966X_VLAN_LEARN_DISABLED;

	lan966x_vlan_set_mask(lan966x, vid);
	return 0;
}

static int lan966x_frer_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	struct mchp_frer_stream_cfg *s;
	struct mchp_frer_iflow_cfg *f;
	u8 val;
	int i;

	rtnl_lock();
	seq_printf(m, "ISDX config:\n");
	for (i = 0; i < ARRAY_SIZE(lan966x->frer.iflow_cfg); i++) {
		f = &lan966x->frer.iflow_cfg[i].frer;
		if (f->ms_enable || f->generation || f->pop || f->split_mask) {
			seq_printf(m, "isdx %d me %d m %u g %d p %d sm 0x%x\n",
				   i, f->ms_enable, f->ms_id, f->generation,
				   f->pop, f->split_mask);
		}
	}
	seq_printf(m, "MS allocation:\n");
	for (i = 0; i < ARRAY_SIZE(lan966x->frer.ms_adm); i++) {
		val = lan966x->frer.ms_adm[i].port_mask;
		if (val) {
			seq_printf(m, "ms_id %d pm 0x%x\n",
				   i * MCHP_FRER_MAX_PORTS, val);
		}
	}
	seq_printf(m, "MS config:\n");
	for (i = 0; i < ARRAY_SIZE(lan966x->frer.ms_cfg); i++) {
		s = &lan966x->frer.ms_cfg[i];
		if (!s->enable)
			continue;
		seq_printf(m, "ms_id %d alg %s hl %u rt %u tns %d cs %u\n",
			   i,
			   (s->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M",
			   s->hlen, s->reset_time, s->take_no_seq, s->cs_id);
	}
	seq_printf(m, "CS config:\n");
	for (i = 0; i < ARRAY_SIZE(lan966x->frer.cs_cfg); i++) {
		s = &lan966x->frer.cs_cfg[i];
		if (!s->enable)
			continue;
		seq_printf(m, "cs_id %d alg %s hl %u rt %u tns %d\n",
			   i,
			   (s->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M",
			   s->hlen, s->reset_time, s->take_no_seq);
	}
	seq_printf(m, "VLAN config:\n");
	for (i = 0; i < ARRAY_SIZE(lan966x->vlan_flags); i++) {
		if (!lan966x->vlan_flags[i])
			continue;
		seq_printf(m, "vid %d flags 0x%x fd %d ld %d\n",
			   i, lan966x->vlan_flags[i],
			   !!(lan966x->vlan_flags[i] & LAN966X_VLAN_FLOOD_DIS),
			   !!(lan966x->vlan_flags[i] &
			      LAN966X_VLAN_LEARN_DISABLED));
	}
	rtnl_unlock();
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_frer);

static int lan966x_frer_init(struct lan966x *lan966x)
{
	u64 val;
	int i;

	/* Set FRER TicksPerSecond to 1000 */
	/* Formular: val = ClockFrequency / (TicsPerSecond * 8 * 512) */
	val = 1000000000000;
	do_div(val, lan966x_ptp_get_period_ps());
	do_div(val, 1000 * 8 * 512);
	lan_wr(QSYS_FRER_CFG_WATCHDOG_PRESCALER_SET(val),
	       lan966x, QSYS_FRER_CFG);

	/* Enable R-tag parsing and 48-bit R-tagging for all ports */
	for (i = 0; i < lan966x->num_phys_ports; i++) {
		if (!lan966x->ports[i])
			continue;

		lan_rmw(ANA_PORT_MODE_REDTAG_PARSE_CFG_SET(1),
			ANA_PORT_MODE_REDTAG_PARSE_CFG,
			lan966x, ANA_PORT_MODE(i));
		lan_rmw(DEV_PORT_MISC_RTAG48_ENA_SET(1),
			DEV_PORT_MISC_RTAG48_ENA,
			lan966x, DEV_PORT_MISC(i));
	}

	/* Get default cstream register content */
	val = lan_rd(lan966x, QSYS_FRER_CFG_CMP(0));
	cs_def_cfg.enable = QSYS_FRER_CFG_CMP_ENABLE_GET(val);
	cs_def_cfg.alg = QSYS_FRER_CFG_CMP_VECTOR_ALGORITHM_GET(val) ?
		MCHP_FRER_REC_ALG_VECTOR :
		MCHP_FRER_REC_ALG_MATCH;
	cs_def_cfg.hlen = QSYS_FRER_CFG_CMP_HISTORY_LENGTH_GET(val) + 1;
	cs_def_cfg.reset_time = QSYS_FRER_CFG_CMP_RESET_TICKS_GET(val);
	cs_def_cfg.take_no_seq = QSYS_FRER_CFG_CMP_TAKE_NO_SEQUENCE_GET(val);

	/* Sync configurstion with default values */
	for (i = 0; i < ARRAY_SIZE(lan966x->frer.cs_cfg); i++) {
		lan966x->frer.cs_cfg[i] = cs_def_cfg;
	}

	/* Get default mstream register content */
	val = lan_rd(lan966x, QSYS_FRER_CFG_MBM(0));
	val = lan_rd(lan966x, QSYS_FRER_CFG_CMP(0));
	ms_def_cfg.enable = QSYS_FRER_CFG_CMP_ENABLE_GET(val);
	ms_def_cfg.alg = QSYS_FRER_CFG_CMP_VECTOR_ALGORITHM_GET(val) ?
		MCHP_FRER_REC_ALG_VECTOR :
		MCHP_FRER_REC_ALG_MATCH;
	ms_def_cfg.hlen = QSYS_FRER_CFG_CMP_HISTORY_LENGTH_GET(val) + 1;
	ms_def_cfg.reset_time = QSYS_FRER_CFG_CMP_RESET_TICKS_GET(val);
	ms_def_cfg.take_no_seq = QSYS_FRER_CFG_CMP_TAKE_NO_SEQUENCE_GET(val);
	ms_def_cfg.cs_id = 0;

	/* Sync configurstion with default values */
	for (i = 0; i < ARRAY_SIZE(lan966x->frer.ms_cfg); i++) {
		lan966x->frer.ms_cfg[i] = ms_def_cfg;
	}

	/* Always apply split mask even if destination port set is empty */
	lan_rmw(ANA_AGENCTRL_APPLY_SPLIT_MASK_SET(1),
		ANA_AGENCTRL_APPLY_SPLIT_MASK,
		lan966x, ANA_AGENCTRL);

	/* Enable FRER */
	lan_wr(QSYS_MISC_DROP_CFG_FRER_ENA_SET(1),
	       lan966x, QSYS_MISC_DROP_CFG);

	debugfs_create_file("frer_show", 0444, lan966x->debugfs_root, lan966x,
			&lan966x_frer_fops);
	return 0;
}

/*******************************************************************************
 * QoS port notification
 ******************************************************************************/
int lan966x_qos_port_event(struct net_device *dev, unsigned long event)
{
	struct lan966x_port *port;
	if (lan966x_netdevice_check(dev)) {
		port = netdev_priv(dev);
		switch (event) {
		case NETDEV_DOWN:
		case NETDEV_CHANGE:
			lan966x_fp_update(port);
			break;
		default:
			/* Nothing */
			break;
		}
	}
	return NOTIFY_DONE;
}

/*******************************************************************************
 * QoS Statistics
 ******************************************************************************/
void lan966x_qos_update_stats(struct lan966x *lan966x)
{
	lan966x_psfp_stats_upd_all(lan966x);
	lan966x_frer_update_stats_all(lan966x);
	lan966x_fp_check(lan966x);
}

/*******************************************************************************
 * QoS Initialization
 ******************************************************************************/
int lan966x_qos_init(struct lan966x *lan966x)
{
	struct qos_policer_conf p = { 0 };
	int err;

        /* Setup discard policer */
	p.mode = RATE_MODE_FRAME;
	err = policer_conf_set(lan966x, LAN966X_POL_IX_DISCARD, &p);
	if (err)
		return err;

	debugfs_create_file("qos_show", 0444, lan966x->debugfs_root, lan966x,
			&lan966x_qos_fops);

	debugfs_create_file("qos_show_hw", 0444, lan966x->debugfs_root, lan966x,
			&lan966x_qos_hw_fops);

	err = lan966x_tas_init(lan966x);
	if (err)
		return err;

	err = lan966x_fp_init(lan966x);
	if (err)
		return err;

	err = lan966x_frer_init(lan966x);
	if (err)
		return err;

	return 0;
}

void lan966x_qos_port_init(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;
	int pcp, dei, cos, dpl;
	u8 tag_cfg;

	/* Setup ingress 1:1 mapping between tag [PCP,DEI] and [PRIO,DPL].
	 * PCP determines the priority (0..7) of the frame and
	 * DEI determines the color (green og yellow) of the frame. */
	for (pcp = 0; pcp < 8; pcp++) {
		for (dei = 0; dei < 2; dei++) {
			lan_wr(ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL_SET(dei) |
			       ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL_SET(pcp),
			       lan966x,
			       ANA_PCP_DEI_CFG(port->chip_port, 8 * dei + pcp));
			port->qos_port_conf.i_pcp_dei_prio_dpl_map[pcp][dei].prio = pcp;
			port->qos_port_conf.i_pcp_dei_prio_dpl_map[pcp][dei].dpl = dei;
		}
	}

	port->qos_port_conf.i_default_prio = 0;
	port->qos_port_conf.i_default_dpl = 0;
	port->qos_port_conf.i_mode.tag_map_enable = false;
	port->qos_port_conf.i_mode.dscp_map_enable = false;
	port->qos_port_conf.i_default_pcp = 0;
	port->qos_port_conf.i_default_dei = 0;

	/* Setup egress 1:1 mapping between [PRIO,DPL] and [PCP,DEI].
	 * priority determines the PCP value (0..7) in the frame and
	 * DPL determines the DEI value (0..1) in the frame. */
	for (cos = 0; cos < 8; cos++) {
		for (dpl = 0; dpl < 2; dpl++) {
			lan_wr(REW_PCP_DEI_CFG_DEI_QOS_VAL_SET(dpl) |
			       REW_PCP_DEI_CFG_PCP_QOS_VAL_SET(cos),
			       lan966x,
			       REW_PCP_DEI_CFG(port->chip_port, 8 * dpl + cos));
			port->qos_port_conf.e_prio_dpl_pcp_dei_map[cos][dpl].pcp = cos;
			port->qos_port_conf.e_prio_dpl_pcp_dei_map[cos][dpl].dei = dpl;
		}
	}

	tag_cfg = 0; /* Classified [PCP,DEI] */
// 	tag_cfg = 1; /* Port based [PCP,DEI] */
//	tag_cfg = 2; /* [COS,DPL] via map to [PCP,DEI] */
//	tag_cfg = 3; /* [COS,DPL] 1:1 to [PCP,DEI] */
	lan_rmw(REW_TAG_CFG_TAG_PCP_CFG_SET(tag_cfg) |
		REW_TAG_CFG_TAG_DEI_CFG_SET(tag_cfg),
		REW_TAG_CFG_TAG_PCP_CFG |
		REW_TAG_CFG_TAG_DEI_CFG,
		lan966x, REW_TAG_CFG(port->chip_port));
	port->qos_port_conf.e_mode = MCHP_E_MODE_CLASSIFIED;

	port->qos_port_conf.e_default_pcp = 0;
	port->qos_port_conf.e_default_dei = 0;
}
