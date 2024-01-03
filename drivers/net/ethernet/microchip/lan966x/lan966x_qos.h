/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#ifndef _LAN966X_QOS_H_
#define _LAN966X_QOS_H_

#include <linux/types.h>
#include <net/pkt_sched.h>
#include "../mchp_ui_qos.h"

struct lan966x;
struct lan966x_port;

/*******************************************************************************
 * QOS Port configuration
 ******************************************************************************/
int lan966x_qos_port_conf_get(const struct lan966x_port *const port,
                              struct mchp_qos_port_conf *const conf);
int lan966x_qos_port_conf_set(struct lan966x_port *const port,
                              struct mchp_qos_port_conf *const conf);

int lan966x_qos_dscp_prio_dpl_get(const struct lan966x *lan966x,
				  const u32 dscp,
				  struct mchp_qos_dscp_prio_dpl *const conf);
int lan966x_qos_dscp_prio_dpl_set(struct lan966x *lan966x,
				  const u32 dscp,
				  const struct mchp_qos_dscp_prio_dpl *const conf);

#define LAN966X_NUM_TC 8 /* Number of traffic classes */

/* PSFP constants */
#if defined(ASIC)
#define LAN966X_PSFP_NUM_SFI 256 /* Number of Stream Filter Instances */
#define LAN966X_PSFP_NUM_SGI 256 /* Number of Stream Gate Instances */
#else
#define LAN966X_PSFP_NUM_SFI 192 /* Number of Stream Filter Instances */
#define LAN966X_PSFP_NUM_SGI 192 /* Number of Stream Gate Instances */
#endif
#define LAN966X_PSFP_NUM_GCE   4 /* Number of Gate Control Entries/gate */

/* Minimum supported cycle time in nanoseconds */
#define LAN966X_PSFP_SG_MIN_CYCLE_TIME_NS (1 * NSEC_PER_USEC) /* 1 usec */

/* Maximum supported cycle time in nanoseconds */
#define LAN966X_PSFP_SG_MAX_CYCLE_TIME_NS ((1 * NSEC_PER_SEC) - 1) /* 999.999.999 nsec */

/* Maximum IPV value */
#define LAN966X_PSFP_SG_MAX_IPV 7

/* Policer indexes */
#define LAN966X_POL_IX_PORT      0 /* 0-8    : 9 port policers */
#define LAN966X_POL_IX_QUEUE     9 /* 9-80   : 72 queue policers (9p * 8q) */
#define LAN966X_POL_IX_POOL     81 /* 81-343 : 263 PSFP and VCAP IS2 policers */
#define LAN966X_POL_IX_DISCARD 344 /* 344    : 1 discard policer */
#define LAN966X_NUM_POL_POOL   (LAN966X_POL_IX_DISCARD - LAN966X_POL_IX_POOL)

/*******************************************************************************
 * Resource pool administration.
 * Used to administer PSFP stream gates (IS1) and policers (IS1 and IS2)
 ******************************************************************************/
/**
 * enum lan966x_res_pool_user - Enumerates the users of the resource pool
 *
 * The pool is split up in different users in order to allow the same ID to be
 * used in different contexts.
 */
enum lan966x_res_pool_user {
	LAN966X_RES_POOL_FREE,
	LAN966X_RES_POOL_USER_IS1,
	LAN966X_RES_POOL_USER_IS2,

	/* used to define LAN966X_RES_POOL_USER_MAX below */
	__LAN966X_RES_POOL_USER_AFTER_LAST,
	LAN966X_RES_POOL_USER_MAX = __LAN966X_RES_POOL_USER_AFTER_LAST - 1,
};

/**
 * struct lan966x_res_pool_entry - Entry for each resource in pool
 * @user: The user of this entry.
 * @ref_cnt: The current number of references to this resource.
 * @id: The allocated ID for this resource.
 */
struct lan966x_res_pool_entry {
	enum lan966x_res_pool_user user;
	u16 ref_cnt;
	u32 id;
};

/* QoS complete configuration/state */
#define DSCP_COUNT 64
struct lan966x_qos_conf {
	DECLARE_BITMAP(sfi_pool, LAN966X_PSFP_NUM_SFI);
	struct lan966x_res_pool_entry sgi_pool[LAN966X_PSFP_NUM_SGI];
	struct lan966x_res_pool_entry pol_pool[LAN966X_NUM_POL_POOL];
	struct mchp_qos_dscp_prio_dpl dscp_prio_dpl_map[DSCP_COUNT];
};

/**
 * lan966x_sfi_ix_reserve - Reserve a PSFP stram filter instance (sfi) index
 * @lan966x: switch device.
 * @sfi_ix: The returned sfi index.
 *
 * Find first free entry. These entries are not shared so no ref_cnt here.
 *
 * Note that it is up to the caller to configure the stream filter instance.
 *
 * Returns
 *  0 if entry was found.
 *  -ENOSPC if there is no more free entries.
 */
int lan966x_sfi_ix_reserve(struct lan966x *lan966x,
			   u32 *sfi_ix);

/**
 * lan966x_sfi_ix_release - Release a PSFP stream filter instance (sfi) index
 * @lan966x: switch device.
 * @sfi_ix: The sfi index to release.
 *
 * Release sfi index and disable stream filter.
 *
 * Returns
 *  0 if no errors
 *  -EINVAL if sfi_ix is invalid or already cleared..
 */
int lan966x_sfi_ix_release(struct lan966x *lan966x,
			   u32 sfi_ix);

/**
 * lan966x_sgi_ix_reserve - Reserve a PSFP stream gate instance (sgi) index
 * @lan966x: switch device.
 * @user: The user this entry belongs to.
 * @id: Id associated with this sgi index.
 * @pol_ix: The returned sgi index.
 *
 * Search through the sgi pool and if an entry with same user and id is
 * found then increment ref_cnt and return sgi index.
 * If not found then find a free entry and initialize it with given user and id,
 * set ref_cnt to 1 and return sgi index.
 *
 * Note that it is up to the caller to configure the stream gate instance.
 *
 * Returns
 *  0 if entry was found.
 *  1 if entry was created.
 *  -EINVAL if user is invalid.
 *  -ENOSPC if there is no more free entries.
 */
int lan966x_sgi_ix_reserve(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id,
			   u32 *sgi_ix);

/**
 * lan966x_sgi_ix_release - Release a PSFP stream gate instance (sgi) index
 * @lan966x: switch device.
 * @user: The user this entry belongs to.
 * @id: Id associated with this sgi index.
 *
 * Release sgi index and disable stream gate if ref_cnt becomes zero.
 *
 * Returns
 *  0 if entry was found and released
 *  -EINVAL if user is invalid.
 *  -ENOENT if user and id was not found.
 */
int lan966x_sgi_ix_release(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id);

/**
 * lan966x_pol_ix_reserve - Reserve a PSFP or ACL policer index
 * @lan966x: switch device.
 * @user: The user this entry belongs to.
 * @id: Id associated with this policer index.
 * @pol_ix: The returned policer index.
 *
 * Search through the policer pool and if an entry with same user and id is
 * found then increment ref_cnt and return policer index.
 * If not found then find a free entry and initialize it with given user and id,
 * set ref_cnt to 1 and return policer index.
 *
 * Note that it is up to the caller to configure the policer.
 *
 * Returns
 *  0 if entry was found.
 *  1 if entry was created.
 *  -EINVAL if user is invalid.
 *  -ENOSPC if there is no more free entries.
 */
int lan966x_pol_ix_reserve(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id,
			   u32 *pol_ix);

/**
 * lan966x_pol_ix_release - Release a PSFP or ACL policer index
 * @lan966x: switch device.
 * @user: The user this entry belongs to.
 * @id: Id associated with this policer index.
 *
 * Release policer index and disable policer if ref_cnt becomes zero.
 *
 * Returns
 *  0 if entry was found and released
 *  -EINVAL if user is invalid.
 *  -ENOENT if user and id was not found.
 */
int lan966x_pol_ix_release(struct lan966x *lan966x,
			   enum lan966x_res_pool_user user,
			   u32 id);

/*******************************************************************************
 * TC (Linux Traffic Control)
 ******************************************************************************/
/* --- Credit Based Shaper in tc qdisc --- */
struct lan966x_tc_cbs {
	s32 idleslope; /* kilobit per second */
	s32 sendslope; /* kilobit per second */
	s32 hicredit; /* bytes */
	s32 locredit; /* bytes */
};

int lan966x_tc_cbs_add(struct lan966x_port *port, u8 queue,
		       struct lan966x_tc_cbs *cbs);

int lan966x_tc_cbs_del(struct lan966x_port *port, u8 queue);

/* --- Token Bucket Filter (shaper) in tc qdisc --- */
/**
 * struct lan966x_yc_tbf - Shaper configuration
 * @rate: Rate in kilobits per second.
 * @burst: Burst size in bytes.
 */
struct lan966x_tc_tbf {
	u32 rate;
	u32 burst;
};

/**
 * lan966x_tc_tbf_add - Add a port/priority shaper
 * @port: egress port
 * @root: true if port shaper (attached to root qdisc) - false if priority shaper
 * @queue: queue - only valid if priority shaper
 * @tbf: shaper configuration
 * returns zero on success
 */
int lan966x_tc_tbf_add(struct lan966x_port *port, bool root, u32 queue,
		       struct lan966x_tc_tbf *tbf);

/**
 * lan966x_tc_tbf_del - Delete a port/priority shaper
 * @port: egress port
 * @root: true if port shaper (attached to root qdisc) - false if priority shaper
 * @queue: queue - only valid if priority shaper
 * returns zero on success
 */
int lan966x_tc_tbf_del(struct lan966x_port *port, bool root, u32 queue);

/*******************************************************************************
 * TAS (Time Aware Shaper - 802.1Qbv)
 ******************************************************************************/
int lan966x_tas_enable(struct lan966x_port *port,
		       struct tc_taprio_qopt_offload *qopt);

int lan966x_tas_disable(struct lan966x_port *port);

/* The current speed is needed in order to calculate the guard band */
void lan966x_tas_speed(struct lan966x_port *port, int speed);

/*******************************************************************************
 * PSFP (Per-Stream Filtering and Policing - 802.1Qci)
 ******************************************************************************/
/* PSFP Stream Filter configuration */
struct lan966x_psfp_sf_cfg {
	bool block_oversize_ena; /* StreamBlockedDueToOversizeFrameEnable */
	bool block_oversize; /* StreamBlockedDueToOversizeFrame */
	bool force_block; /* Block all frames matching filter */
	u32 max_sdu; /* Maximum SDU size (zero disables SDU check) */
};

/* Set PSFP Stream Filter */
int lan966x_psfp_sf_set(struct lan966x *lan966x,
			const u32 sfi_ix,
			const struct lan966x_psfp_sf_cfg *const c);

/* PSFP Gate Control Entry configuration */
struct lan966x_psfp_gce_cfg {
	bool gate_state;   /* StreamGateState (true = enabled) */
	u32 interval; /* TimeInterval (nsec) */
	s32 ipv;           /* IPV (-1 disables IPV) */
	s32 maxoctets;     /* IntervalOctetMax (-1 disables check) */
};

/* PSFP Stream Gate configuration */
struct lan966x_psfp_sg_cfg {
	bool gate_state;  /* PSFPAdminGateStates: Initial gate state (true = enabled) */
	s32 ipv;          /* PSFPAdminIPV  (-1 disables IPV) */
	u64 basetime;     /* PSFPAdminBaseTime */
	u32 cycletime;    /* PSFPAdminCycleTime */
	u32 cycletimeext; /* PSFPAdminCycleTimeExtension */
	u32 num_entries;  /* PSFPAdminControlListLength */
	struct lan966x_psfp_gce_cfg gce[LAN966X_PSFP_NUM_GCE];
};

/* Set PSFP Stream Gate */
int lan966x_psfp_sg_set(struct lan966x *lan966x,
			const u32 sgi_ix,
			const struct lan966x_psfp_sg_cfg *const c);

/* PSFP Stream Filter Counters */
struct lan966x_psfp_sfc {
	u64 matching_frames_count;
	u64 not_passing_frames_count;
	u64 not_passing_sdu_count;
	u64 red_frames_count;
	ulong lastused; /* jiffies when last change was detected in above */
};

/* Get PSFP Stream Filter Counters */
void lan966x_psfp_stats_get(struct lan966x *lan966x,
			    const u32 sfi_ix,
			    struct lan966x_psfp_sfc *const c);

/* PSFP TC Stream Filter Counters */
struct lan966x_psfp_tcsfc {
	u64 drops; /* Remember previous number of drops */
};

/* PSFP configuration/state */
struct lan966x_psfp_conf {
	struct lan966x_psfp_sfc cnt[LAN966X_PSFP_NUM_SFI];
	struct lan966x_psfp_tcsfc tcsfc[LAN966X_PSFP_NUM_SFI];
};

/*******************************************************************************
 * FP (Frame Preemption - 802.1Qbu/802.3br)
 ******************************************************************************/
struct lan966x_fp_port_conf {
	u8 admin_status;        /* IEEE802.1Qbu: framePreemptionStatusTable */
	bool enable_tx;         /* IEEE802.3br: aMACMergeEnableTx */
	bool verify_disable_tx; /* IEEE802.3br: aMACMergeVerifyDisableTx */
	u8 verify_time;         /* IEEE802.3br: aMACMergeVerifyTime [msec] */
	u8 add_frag_size;       /* IEEE802.3br: aMACMergeAddFragSize */
};

int lan966x_fp_set(struct lan966x_port *port,
		   struct lan966x_fp_port_conf *conf);

int lan966x_fp_get(struct lan966x_port *port,
		   struct lan966x_fp_port_conf *conf);

int lan966x_fp_status(struct lan966x_port *port,
		      struct mchp_qos_fp_port_status *status);


/*******************************************************************************
 * FRER (Frame Replication and Elimination for Reliability - 802.1CB)
 ******************************************************************************/
#define LAN966X_FRER_NUM_MSI     512 /* Number of Member Stream Instances */
#define LAN966X_FRER_NUM_CSI     256 /* Number of Compound Stream Instances */
#define LAN966X_FRER_NUM_FLOW    256 /* Number of Flows (ISDX) */
#define LAN966X_FRER_FLOW_MIN      1 /* Cannot use ISDX zero */
#define MCHP_FRER_MAX_PORTS     2 /* Max # of ports for split and mstreams */
#define LAN966X_FRER_HLEN_MIN      2 /* Minimum history length */
#define LAN966X_FRER_HLEN_MAX     32 /* Maximum history length */
#define LAN966X_FRER_RESET_MIN     0 /* Minimum reset time */
#define LAN966X_FRER_RESET_MAX  4095 /* Maximum reset_time */

struct lan966x_frer_prev_cnt {
	u32 out_of_order_packets; /* frerCpsSeqRcvyOutOfOrderPackets */
	u32 rogue_packets;        /* frerCpsSeqRcvyRoguePackets */
	u32 passed_packets;       /* frerCpsSeqRcvyPassedPackets */
	u32 discarded_packets;    /* frerCpsSeqRcvyDiscardedPackets */
	u32 lost_packets;         /* frerCpsSeqRcvyLostPackets */
	u32 tagless_packets;      /* frerCpsSeqRcvyTaglessPackets */
	u32 resets;               /* frerCpsSeqRcvyResets */
};

/* - FRER compound streams ----------------------------------------- */
/*
 * lan966x_frer_cs_cfg_get - Get compound stream configuration
 * @lan966x: (in) bridge device
 * @cs_id: (in) compound stream id
 * @cfg: (out) compound stream configuration
 * returns zero on success
 */
int lan966x_frer_cs_cfg_get(struct lan966x *lan966x,
			    const u16 cs_id,
			    struct mchp_frer_stream_cfg *const cfg);

/*
 * lan966x_frer_cs_cfg_set - Set compound stream configuration
 * @lan966x: (in) bridge device
 * @cs_id: (in) compound stream id
 * @cfg: (in) compound stream configuration
 * returns zero on success
 */
int lan966x_frer_cs_cfg_set(struct lan966x *lan966x,
			    const u16 cs_id,
			    const struct mchp_frer_stream_cfg *const cfg);

/*
 * lan966x_frer_cs_cnt_get - Get compound stream counters
 * @lan966x: (in) bridge device
 * @cs_id: (in) compound stream id
 * @cnt: (out) compound stream counters
 * returns zero on success
 */
int lan966x_frer_cs_cnt_get(struct lan966x *lan966x,
			    const u16 cs_id,
			    struct mchp_frer_cnt *const cnt);

/*
 * lan966x_frer_cs_cnt_clear - Clear compound stream counters
 * @lan966x: (in) bridge device
 * @cs_id: (in) compound stream id
 * returns zero on success
 */
int lan966x_frer_cs_cnt_clear(struct lan966x *lan966x,
			      const u16 cs_id);

/* - FRER member streams ------------------------------------------- */
/* A block of member stream IDs must be allocated for an egress port list.
 * For each stream ID and egress port, configuration and counters are available.
 * Individual recovery can be enabled to eliminate duplicate frames.
 * Frames can also be mapped to a compound stream.
 */

/*
 * lan966x_frer_ms_alloc - Allocate FRER member stream ID block
 * @lan966x: (in) bridge device
 * @port_mask: (in) egress ports bit mask
 * @ms_id: (out) member stream id
 * returns zero on success
 */
int lan966x_frer_ms_alloc(struct lan966x *lan966x,
			  const u8 port_mask,
			  u16 *const ms_id);

/*
 * lan966x_frer_ms_free - Free FRER member stream ID block
 * @lan966x: (in) bridge device
 * @ms_id: (in) member stream id
 * returns zero on success
 */
int lan966x_frer_ms_free(struct lan966x *lan966x,
			 const u16 ms_id);

/*
 * lan966x_frer_ms_cfg_get - Get member stream configuration
 * @port: (in) egress port
 * @ms_id: (in) member stream id
 * @cfg: (out) member stream configuration
 * returns zero on success
 */
int lan966x_frer_ms_cfg_get(struct lan966x_port *port,
			    const u16 ms_id,
			    struct mchp_frer_stream_cfg *const cfg);

/*
 * lan966x_frer_ms_cfg_set - Set member stream configuration
 * @port: (in) egress port
 * @ms_id: (in) member stream id
 * @cfg: (in) member stream configuration
 * returns zero on success
 */
int lan966x_frer_ms_cfg_set(struct lan966x_port *port,
			    const u16 ms_id,
			    const struct mchp_frer_stream_cfg *const cfg);

/*
 * lan966x_frer_ms_cnt_get - Get member stream counters
 * @port: (in) egress port
 * @ms_id: (in) member stream id
 * @cnt: (out) member stream counters
 * returns zero on success
 */
int lan966x_frer_ms_cnt_get(struct lan966x_port *port,
			    const u16 ms_id,
			    struct mchp_frer_cnt *const cnt);

/*
 * lan966x_frer_ms_cnt_clear - Clear member stream counters
 * @port: (in) egress port
 * @ms_id: (in) member stream id
 * returns zero on success
 */
int lan966x_frer_ms_cnt_clear(struct lan966x_port *port,
			      const u16 ms_id);

/*
 * lan966x_iflow_cfg_get - Get ingress flow configuration
 * @lan966x: (in) bridge device
 * @id: (in) ingress flow id (isdx)
 * @cfg: (out) ingress flow configuration
 * returns zero on success
 */
int lan966x_iflow_cfg_get(struct lan966x *lan966x,
			  const u16 id,
			  struct mchp_iflow_cfg *const cfg);

/*
 * lan966x_iflow_cfg_set - Set ingress flow configuration
 * @lan966x: (in) bridge device
 * @id: (in) ingress flow id (isdx)
 * @cfg: (in) ingress flow configuration
 * returns zero on success
 */
int lan966x_iflow_cfg_set(struct lan966x *lan966x,
			  const u16 id,
			  const struct mchp_iflow_cfg *const cfg);

/* FRER member stream administration */
struct lan966x_frer_ms_adm {
	u8 port_mask; /* Zero means unallocated */
};

/* FRER complete configuration/state */
struct lan966x_frer_conf {
	struct mchp_frer_stream_cfg ms_cfg[LAN966X_FRER_NUM_MSI];
	struct mchp_frer_stream_cfg cs_cfg[LAN966X_FRER_NUM_CSI];
	struct mchp_frer_cnt ms_cnt[LAN966X_FRER_NUM_MSI];
	struct lan966x_frer_prev_cnt ms_prev_cnt[LAN966X_FRER_NUM_MSI];
	struct mchp_frer_cnt cs_cnt[LAN966X_FRER_NUM_CSI];
	struct lan966x_frer_prev_cnt cs_prev_cnt[LAN966X_FRER_NUM_CSI];
	struct mchp_iflow_cfg iflow_cfg[LAN966X_FRER_NUM_FLOW];
	struct lan966x_frer_ms_adm ms_adm[LAN966X_FRER_NUM_MSI /
					  MCHP_FRER_MAX_PORTS];
};

/*******************************************************************************
 * VLAN support for FRER
 ******************************************************************************/
/*
 * lan966x_frer_vlan_cfg_get - Get FRER VLAN configuration
 * @lan966x: (in) bridge device
 * @vid: (in) VLAN id
 * @cfg: (out) FRER VLAN configuration
 * returns zero on success
 */
int lan966x_frer_vlan_cfg_get(struct lan966x *lan966x,
			      const u16 vid,
			      struct mchp_frer_vlan_cfg *const cfg);

/*
 * lan966x_frer_vlan_cfg_set - Set FRER VLAN configuration
 * @lan966x: (in) bridge device
 * @vid: (in) VLAN id
 * @cfg: (in) FRER VLAN configuration
 * returns zero on success
 */
int lan966x_frer_vlan_cfg_set(struct lan966x *lan966x,
			       const u16 vid,
			       const struct mchp_frer_vlan_cfg *const cfg);

/*******************************************************************************
 * QoS port notification
 ******************************************************************************/
int lan966x_qos_port_event(struct net_device *dev, unsigned long event);

/*******************************************************************************
 * QoS Statistics
 ******************************************************************************/
void lan966x_qos_update_stats(struct lan966x *lan966x);

/*******************************************************************************
 * QoS Initialization
 ******************************************************************************/
int lan966x_qos_init(struct lan966x *lan966x);

#endif /* _LAN966X_QOS_H_ */
