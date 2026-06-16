/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2024 Microchip Technology Inc. and its subsidiaries.
 * Microchip AFI API
 */

#ifndef __AFI_API__
#define __AFI_API__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/netdevice.h>

#define AFI_TTI_TICK_LEN0_US        52 /* 52us   */
#define AFI_TTI_TICK_LEN1_US       416 /* 416us  */
#define AFI_TTI_TICK_LEN2_US      3333 /* 3.333ms.*/
#define AFI_TTI_TICK_LEN3_US     10000 /* 10ms   */
#define AFI_TTI_TICK_LEN4_US    100000 /* 100ms  */
#define AFI_TTI_TICK_LEN5_US   1000000 /* 1s     */
#define AFI_TTI_TICK_LEN6_US  10000000 /* 10s    */
#define AFI_TTI_TICK_LEN7_US  60000000 /* 1min   */

enum afi_entry_state {
	/* Entry is not in use */
	AFI_ENTRY_STATE_FREE,
	/* Entry is allocated and hijacked, but stopped by user */
	AFI_ENTRY_STATE_STOPPED,
	/* Entry is allocated and hijacked, and started by user */
	AFI_ENTRY_STATE_STARTED,
};

/**
 * Structure defining properties of a slow injection.
 */
struct afi_slow_inj_alloc_cfg {
	/* [IN]
	 * Port number onto which the frame shall be transmitted periodically.
	 */
	u32 port_no;

	/* [IN]
	 * Priority on which the frame sequence shall be transmitted.
	 */
	u8 prio;
};

/**
 * Structure defining properties of a slow injection.
 */
struct afi_slow_inj_start_cfg {
	/*[IN]
	 *Frames per hour.
	 */
	u64 fph;
};

/* Maximum number of frames in a fast-injection (DTI) sequence. */
#define AFI_FAST_INJ_FRM_CNT_MAX 8

/**
 * Structure defining properties of a fast injection (DTI).
 */
struct afi_fast_inj_alloc_cfg {
	/* [IN]
	 * Port number onto which the frame sequence shall be transmitted.
	 */
	u32 port_no;

	/* [IN]
	 * Priority on which the frame sequence shall be transmitted.
	 */
	u8 prio;

	/* [IN]
	 * Number of frames in the sequence, i.e. the number of frames to be
	 * hijacked using afi_fast_inj_frm_hijack(). Max
	 * AFI_FAST_INJ_FRM_CNT_MAX.
	 */
	u32 frm_cnt;
};

/**
 * Structure needed when hijacking a frame for fast injection.
 */
struct afi_fast_inj_frm_cfg {
	/* [IN]
	 * Frame size in bytes. Only used for bandwidth calculation and need
	 * not match the actual frame size. Add IFG and preamble to frm_size if
	 * they shall be included in the bandwidth.
	 */
	u32 frm_size;
};

/**
 * Structure defining properties of a fast injection start.
 */
struct afi_fast_inj_start_cfg {
	/* [IN]
	 * Requested injection rate in bits per second (layer 1 / line rate).
	 * 0 => inject at line speed.
	 */
	u64 bps;

	/* [IN]
	 * Number of frames to inject. 0 => inject until stopped.
	 *
	 * The DTI injects whole sequences (DTI_CNT counts sequences) and the
	 * rate engine may repeat the frame INJ_CNT times per sequence to fine
	 * tune the rate, so the requested frame count is rounded up to the
	 * nearest whole sequence. The achieved count is reported in seq_actual.
	 */
	u32 seq_cnt;

	/* [OUT]
	 * Actual injection rate in bits per second. Always <= bps.
	 */
	u64 bps_actual;

	/* [OUT]
	 * Actual number of frames that will be injected: seq_cnt rounded up to
	 * a whole number of sequences. 0 when seq_cnt is 0 (inject forever).
	 */
	u32 seq_actual;
};

struct afi_tti {
	/* Arguments to most recent call to afi_slow_inj_start() */
	struct afi_slow_inj_start_cfg start_cfg;

	/* State of this entry (free/user-started/user-stopped) */
	enum afi_entry_state state;

	/* TTI is paused by driver due to missing link on either down- or
	 * up-port.  For a flow to be started, state must be
	 * AFI_ENTRY_STATE_STARTED and paused must be 0.
	 */
	bool paused;

	/* TTI frame has been hijacked */
	bool hijacked;

	/* TTI_TIMER fields (except for timer_ena) */
	u8  tick_idx;
	u16 timer_len;

	/* TTI_FRM.FRM_PTR. -1 => No FRM allocated. */
	s32 frm_idx;

	/* TTI_PORT_QU fields */
	u32 port_no;
	u32 prio;
};

struct afi_frm_info {
	u32 fp;
	u8 dstp;
	u8 fshort;
	u8 eprio;
};

/* FRM_TBL entry */
struct afi_frm {
	/* 0 = Frame, 1 = Delay */
	u8  entry_type;
	/* Index of next FRM_TBL entry in sequence */
	u32 next_idx;

	struct afi_frm_info frm_info;
	/* Number of times to inject this frame (DTI). TTI always uses 1. */
	u32 inj_cnt;
	/* Frame size used for DTI bandwidth calculation only. */
	u32 frm_size;
	/* Delay entry (entry_type == 1): delay in system clock cycles. */
	u32 delay;
};

/* DTI_TBL entry */
struct afi_dti {
	/* Arguments to most recent call to afi_fast_inj_start() */
	struct afi_fast_inj_start_cfg start_cfg;

	/* State of this entry (free/user-started/user-stopped) */
	enum afi_entry_state state;

	/* DTI is paused by driver due to missing link. For a flow to be
	 * started, state must be AFI_ENTRY_STATE_STARTED and paused must be 0.
	 */
	bool paused;

	/* DTI_MODE.MODE:
	 * 0 = inject sequence frm_inj_cnt times,
	 * 1 = inject until stopped,
	 * 2 = inject frm_inj_cnt times, then chain to dti_next.
	 */
	u8 mode;
	/* DTI_CTRL.BW: arbitration precedence for high-bandwidth DTIs. */
	u8 bw;
	/* DTI to enable when this one completes (mode 2). */
	u8 dti_next;

	/* DTI_MODE.TRAILING_DELAY_SEQ_CNT */
	u32 trailing_delay_seq_cnt;
	/* Number of sequences to inject (DTI_CNT.CNT). 0 => until stopped. */
	u32 frm_inj_cnt;

	/* DTI_PORT_QU fields */
	u32 port_no;
	u32 prio;

	/* First entry in frm_tbl[] for this flow. 0 => none allocated. */
	u32 first_frm_idx;

	/* Number of frames in sequence. */
	u8 frm_cnt;
};

struct afi_control {
	/* User data */
	void *priv;

	/* Operation */
	struct afi_operations *ops;

	/* Constants */
	struct afi_consts *consts;

	/* FRM_TBL/TTI_TBL/DTI_TBL allocation.
	 * One bit per entry.
	 */
	u32 *frms_alloced;
	u32 *ttis_alloced;
	u32 *dtis_alloced;

	/* MISC_CTRL.AFI_ENA. Set when TTI/DTI is alloced. */
	u8 afi_ena;

	/* TTI_CTRL.TTI_ENA. Set when first TTI is alloced. */
	u8 tti_ena;

	/* FRM_TBL */
	struct afi_frm *frm_tbl;

	/* TTI_TBL */
	struct afi_tti *tti_tbl;

	/* DTI_TBL */
	struct afi_dti *dti_tbl;

	/* TICK length */
	u32 tick_len_us[8];

	/* Switch core's clock period in picoseconds */
	u64 clk_period_ps;
};

struct afi_operations {
	int (*afi_enable)(struct afi_control *afi);
	int (*ttis_enable)(struct afi_control *afi);
	int (*tick_init)(struct afi_control *afi);

	int (*tti_frm_hijack)(struct afi_control *afi, u32 slowid);
	int (*tti_frm_rm_inj)(struct afi_control *afi, u32 slowid);

	int (*tti_start)(struct afi_control *afi, u32 slowid, bool do_config);
	int (*tti_stop)(struct afi_control *afi, u32 slowid);

	/* Send a frame with the AFI inject marker set so it can be hijacked.
	 * Optional; required for fast injection.
	 */
	int (*frm_inject)(struct afi_control *afi, u32 port_no, u32 prio,
			  struct sk_buff *skb);

	/* DTI (fast injection) ops. Optional; NULL on chips without DTI.
	 *
	 * Lifecycle of a fast-injection flow:
	 *   1. frm_inject() sends a normal frame tagged with the AFI inject
	 *      marker; the AFI block captures it into a FRM_TBL entry (the
	 *      template). Called once per frame in the sequence, so a
	 *      multi-frame flow injects several marked frames.
	 *   2. dti_frm_hijack(frm_idx) binds the next captured marked frame to
	 *      FRM_TBL entry frm_idx, i.e. it tells AFI which template slot the
	 *      injected frame belongs to. One call per template frame.
	 *   3. dti_start() begins periodic injection of the sequence.
	 *   4. dti_stop() ends injection.
	 *   5. dti_frm_rm_inj() releases the hijacked template entries; required
	 *      once per flow before its FRM_TBL slots can be reused (not once
	 *      per generated frame).
	 *   dti_seq_cnt_get() returns the number of sequences generated so far,
	 *   not the number of template frames.
	 */
	int (*dti_frm_hijack)(struct afi_control *afi, u32 frm_idx);
	int (*dti_frm_rm_inj)(struct afi_control *afi, u32 dti_idx);
	int (*dti_start)(struct afi_control *afi, u32 dti_idx,
			 bool do_frm_cfg, bool do_dti_cfg, bool start);
	int (*dti_stop)(struct afi_control *afi, u32 dti_idx);
	int (*dti_seq_cnt_get)(struct afi_control *afi, u32 dti_idx,
			       u32 *seq_cnt);
};

struct afi_consts {
	u8 frm_cnt;
	u8 slow_inj_cnt;
	u8 dti_cnt;
	u8 prio_super;

	/* Fast-injection (DTI) requested-rate bounds, bits/s (layer 1). Only
	 * used by chips that implement the DTI ops.
	 */
	u64 fast_inj_bps_min;
	u64 fast_inj_bps_max;
};

/**
 * \brief Initialize internal arrays
 *
 * \param afi    [IN]  AFI controller
 *
 * \return Return code.
 **/
int afi_init(struct afi_control *afi);

/**
 * \brief Free internal arrays
 *
 * \param afi    [IN]  AFI controller
 *
 **/
void afi_deinit(struct afi_control *afi);

/**
 * \brief Allocate AFI slow injection resource
 *
 * \param afi    [IN]  AFI controller.
 * \param cfg    [IN]  Injection descriptor.
 * \param slowid [OUT] ID used for referencing the allocated resource.
 *
 * \return Return code.
 **/
int afi_slow_inj_alloc(struct afi_control *afi,
		       struct afi_slow_inj_alloc_cfg *cfg,
		       u32 *slowid);

/**
 * \brief Free AFI slow injection resource
 *
 * Before resources are freed, slow injection must be stopped.
 *
 * \param afi    [IN] AFI controller.
 * \param slowid [IN] Slow injection ID.
 *
 * \return Return code.
 **/
int afi_slow_inj_free(struct afi_control *afi,
		      u32 slowid);

/**
 * \brief Setup frame for slow injection.
 *
 * \param afi    [IN] AFI controller.
 * \param slowid [IN] Slow injection ID.
 *
 * \return Return code.
 **/
int afi_slow_inj_frm_hijack(struct afi_control *afi,
			    u32 slowid);

/**
 * \brief Check if slow injection already started
 *
 * \param afi    [IN] AFI controller.
 * \param slowid [IN] Slow injection ID.
 *
 * \return Return code.
 **/
bool afi_slow_inj_started(struct afi_control *afi,
			  u32 slowid);

/**
 * \brief Start slow injection.
 *
 * \param afi    [IN] AFI controller.
 * \param slowid [IN] Slow injection ID.
 * \param cfg    [IN] Slow injection configuration.
 *
 * \return Return code.
 **/
int afi_slow_inj_start(struct afi_control *afi,
		       u32 slowid,
		       struct afi_slow_inj_start_cfg *cfg);

/**
 * \brief Stop slow injection.
 *
 * \param afi    [IN] AFI controller.
 * \param slowid [IN] Slow injection ID.
 *
 * \return Return code.
 **/
int afi_slow_inj_stop(struct afi_control *afi,
		      u32 slowid);

/**
 * \brief Check if the frm_idx is used
 *
 * \param afi     [IN] AFI controller.
 * \param frm_idx [IN] Index in the frame table
 *
 * \return Return true if it is used otherwise false
 **/
bool afi_frm_idx_chk(struct afi_control *afi, s32 frm_idx);

/**
 * \brief Allocate AFI fast injection (DTI) resource
 *
 * \param afi    [IN]  AFI controller.
 * \param cfg    [IN]  Injection descriptor.
 * \param fastid [OUT] ID used for referencing the allocated resource.
 *
 * \return Return code.
 **/
int afi_fast_inj_alloc(struct afi_control *afi,
		       struct afi_fast_inj_alloc_cfg *cfg,
		       u32 *fastid);

/**
 * \brief Free AFI fast injection resource
 *
 * Before resources are freed, fast injection must be stopped.
 *
 * \param afi    [IN] AFI controller.
 * \param fastid [IN] Fast injection ID.
 *
 * \return Return code.
 **/
int afi_fast_inj_free(struct afi_control *afi, u32 fastid);

/**
 * \brief Setup a frame for fast injection.
 *
 * When a sequence of frames is configured, the frame order is the order in
 * which frames are set up.
 *
 * \param afi    [IN] AFI controller.
 * \param fastid [IN] Fast injection ID.
 * \param cfg    [IN] Frame setup configuration.
 *
 * \return Return code.
 **/
int afi_fast_inj_frm_hijack(struct afi_control *afi, u32 fastid,
			    struct afi_fast_inj_frm_cfg *cfg);

/**
 * \brief Start fast injection.
 *
 * \param afi    [IN]    AFI controller.
 * \param fastid [IN]    Fast injection ID.
 * \param cfg    [INOUT] Fast injection configuration. cfg->bps_actual is set
 *                       to the actual rate on success.
 *
 * \return Return code.
 **/
int afi_fast_inj_start(struct afi_control *afi, u32 fastid,
		       struct afi_fast_inj_start_cfg *cfg);

/**
 * \brief Stop fast injection.
 *
 * \param afi    [IN] AFI controller.
 * \param fastid [IN] Fast injection ID.
 *
 * \return Return code.
 **/
int afi_fast_inj_stop(struct afi_control *afi, u32 fastid);

/**
 * \brief Check if fast injection is already started.
 *
 * \param afi    [IN] AFI controller.
 * \param fastid [IN] Fast injection ID.
 *
 * \return true if started, otherwise false.
 **/
bool afi_fast_inj_started(struct afi_control *afi, u32 fastid);

/**
 * \brief Get remaining sequence count for a fast injection.
 *
 * If seq_cnt was 0 (inject until stopped) in afi_fast_inj_start(), the value
 * read is the number of frames injected. Otherwise it is the remaining number
 * of sequences to inject.
 *
 * \param afi     [IN]  AFI controller.
 * \param fastid  [IN]  Fast injection ID.
 * \param seq_cnt [OUT] Sequence/frame count read.
 *
 * \return Return code.
 **/
int afi_fast_inj_seq_cnt_get(struct afi_control *afi, u32 fastid,
			     u32 *seq_cnt);

#endif /* __AFI_API__ */
