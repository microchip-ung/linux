/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2022 Microchip Technology Inc.
 * Microchip Sparx5 Switch driver
 */

#ifndef _SPARX5_QOS_H_
#define _SPARX5_QOS_H_

#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include <linux/types.h>
#include <net/pkt_sched.h>
#include "../mchp_ui_qos.h"

struct sparx5;
struct sparx5_port;

/*******************************************************************************
 * QOS Port configuration
 ******************************************************************************/
int sparx5_qos_port_conf_get(const struct sparx5_port *const port,
			     struct mchp_qos_port_conf *const conf);
int sparx5_qos_port_conf_set(struct sparx5_port *const port,
			     struct mchp_qos_port_conf *const conf);

/*******************************************************************************
 * FP (Frame Preemption - 802.1Qbu/802.3br)
 ******************************************************************************/
struct sparx5_fp_port_conf {
	u8 admin_status;        /* IEEE802.1Qbu: framePreemptionStatusTable */
	bool enable_tx;         /* IEEE802.3br: aMACMergeEnableTx */
	bool verify_disable_tx; /* IEEE802.3br: aMACMergeVerifyDisableTx */
	u8 verify_time;         /* IEEE802.3br: aMACMergeVerifyTime [msec] */
	u8 add_frag_size;       /* IEEE802.3br: aMACMergeAddFragSize */
};

int sparx5_fp_set(struct sparx5_port *port,
		   struct sparx5_fp_port_conf *conf);

int sparx5_fp_get(struct sparx5_port *port,
		   struct sparx5_fp_port_conf *conf);

int sparx5_fp_status(struct sparx5_port *port,
		      struct mchp_qos_fp_port_status *status);

/*******************************************************************************
 * QoS port notification
 ******************************************************************************/
int sparx5_qos_port_event(struct net_device *dev, unsigned long event);

/*******************************************************************************
 * TAS (Time Aware Shaper - 802.1Qbv)
 ******************************************************************************/
#define SPX5_TAS_ENTRIES_PER_PORT 2

int sparx5_tas_list_index(struct sparx5_port *port, u8 tas_entry);

int sparx5_tas_enable(struct sparx5_port *port,
		      struct tc_taprio_qopt_offload *qopt);

int sparx5_tas_disable(struct sparx5_port *port);

/* The current speed is needed in order to calculate the guard band */
void sparx5_tas_speed(struct sparx5_port *port, int speed);

/* Provide state info */
char *sparx5_tas_state_to_str(int state);

/* TAS list states: */
enum sparx5_tas_state {
	SPX5_TAS_STATE_ADMIN,
	SPX5_TAS_STATE_ADVANCING,
	SPX5_TAS_STATE_PENDING,
	SPX5_TAS_STATE_OPERATING,
	SPX5_TAS_STATE_TERMINATING,
	SPX5_NUM_TAS_STATE,
};

/*******************************************************************************
 * QoS Initialization
 ******************************************************************************/
int sparx5_qos_init(struct sparx5 *sparx5);

/* Number of Layers */
#define SPX5_HSCH_LAYER_CNT 3

/* Scheduling elements per layer */
#define SPX5_HSCH_L0_SE_CNT 5040
#define SPX5_HSCH_L1_SE_CNT 64
#define SPX5_HSCH_L2_SE_CNT 64

/* Calculate Layer 0 Scheduler Element when using normal hierarchy */
int sparx5_hsch_l0_get_idx(struct sparx5 *sparx5, int port, int queue);

/* Number of leak groups */
#define SPX5_HSCH_LEAK_GRP_CNT 4

/* Scheduler modes */
#define SPX5_SE_MODE_LINERATE 0
#define SPX5_SE_MODE_DATARATE 1

/* Rate and burst */
#define SPX5_SE_RATE_MAX 131071
#define SPX5_SE_BURST_MAX 127
#define SPX5_SE_RATE_MIN 1
#define SPX5_SE_BURST_MIN 1
#define SPX5_SE_BURST_UNIT 4096

/* Dwrr */
#define SPX5_DWRR_COST_MAX (1 << 5)

enum sparx5_qos_rate_mode {
	SPX5_RATE_MODE_DISABLED,   /* Policer/shaper disabled */
	SPX5_RATE_MODE_LINE,       /* Measure line rate in kbps incl. IPG */
	SPX5_RATE_MODE_DATA,       /* Measures data rate in kbps excl. IPG */
	SPX5_RATE_MODE_FRAME,      /* Measures frame rate in fps */
	__SPX5_RATE_MODE_END,
	SPX5_NUM_RATE_MODE = __SPX5_RATE_MODE_END,
	SPX5_RATE_MODE_MAX = __SPX5_RATE_MODE_END - 1,
};

struct sparx5_dwrr {
	/* Number of inputs running dwrr */
	u32 count;

	/* Cost of each input running dwrr */
	u8 cost[PRIO_COUNT];
};

struct sparx5_shaper {
	u32 mode;
	u32 rate;
	u32 burst;
};

struct sparx5_lg {
	u32 max_rate;
	u32 resolution;
	u32 leak_time;
	u32 max_ses;
};

struct sparx5_layer {
	struct sparx5_lg leak_groups[SPX5_HSCH_LEAK_GRP_CNT];
};

int sparx5_qos_init(struct sparx5 *sparx5);

void sparx5_qos_port_setup(struct sparx5 *sparx5, int portno);

/* Multi-Queue Priority */
int sparx5_tc_mqprio_add(struct net_device *ndev, u8 num_tc);
int sparx5_tc_mqprio_del(struct net_device *ndev);

/* Token Bucket Filter */
extern struct sparx5_layer sparx5_layers[SPX5_HSCH_LAYER_CNT];
struct tc_tbf_qopt_offload_replace_params;
int sparx5_tc_tbf_add(struct sparx5_port *port,
		      struct tc_tbf_qopt_offload_replace_params *params,
		      u32 layer, u32 idx);
int sparx5_tc_tbf_del(struct sparx5_port *port, u32 layer, u32 idx);

/* Enhanced Transmission Selection */
struct tc_ets_qopt_offload_replace_params;
int sparx5_tc_ets_add(struct sparx5_port *port,
	struct tc_ets_qopt_offload_replace_params *params);
int sparx5_tc_ets_del(struct sparx5_port *port);

/* Credit-Based Shaper */
int sparx5_cbs_add(struct sparx5_port *port, struct tc_cbs_qopt_offload *qopt);
int sparx5_cbs_del(struct sparx5_port *port, struct tc_cbs_qopt_offload *qopt);

/* Hierarchical Scheduler */
u32 sparx5_lg_get_first(struct sparx5 *sparx5, u32 layer, u32 group);
u32 sparx5_lg_get_next(struct sparx5 *sparx5, u32 layer, u32 group,
		       u32 idx);
bool sparx5_lg_is_empty(struct sparx5 *sparx5, u32 layer, u32 group);
const u32 sparx5_get_hsch_max_group_rate(int grp);
#endif /* _SPARX5_QOS_H_ */
