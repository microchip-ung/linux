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
	u32 next_ptr;

	struct afi_frm_info frm_info;
};

struct afi_control {
	/* User data */
	void *priv;

	/* Operation */
	struct afi_operations *ops;

	/* Constants */
	struct afi_consts *consts;

	/* FRM_TBL/TTI_TBL allocation.
	 * One bit per entry.
	 */
	u32 *frms_alloced;
	u32 *ttis_alloced;

	/* MISC_CTRL.AFI_ENA. Set when TTI/DTI is alloced. */
	u8 afi_ena;

	/* TTI_CTRL.TTI_ENA. Set when first TTI is alloced. */
	u8 tti_ena;

	/* FRM_TBL */
	struct afi_frm *frm_tbl;

	/* TTI_TBL */
	struct afi_tti *tti_tbl;

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
};

struct afi_consts {
	u8 frm_cnt;
	u8 slow_inj_cnt;
	u8 prio_super;
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

#endif /* __AFI_API__ */
