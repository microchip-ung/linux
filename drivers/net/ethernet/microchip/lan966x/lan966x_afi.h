/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN966X_AFI_H_
#define _LAN966X_AFI_H_

#include <linux/netdevice.h>

#define LAN966X_AFI_SLOW_INJ_CNT	64
#define LAN966X_AFI_FRM_CNT		64

enum lan966x_afi_entry_state {
	/* Entry is not in use */
	LAN966X_AFI_ENTRY_STATE_FREE,
	/* Entry is allocated and hijacked, but stopped by user */
	LAN966X_AFI_ENTRY_STATE_STOPPED,
	/* Entry is allocated and hijacked, and started by user */
	LAN966X_AFI_ENTRY_STATE_STARTED,
};

/**
 * Structure defining properties of a slow injection.
 */
struct lan966x_afi_slow_inj_alloc_cfg {
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
 * \brief Allocate AFI slow injection resource
 *
 * \param inst   [IN]  Target instance reference.
 * \param cfg    [IN]  Injection descriptor.
 * \param slowid [OUT] ID used for referencing the allocated resource.
 *
 * \return Return code.
 **/
bool lan966x_afi_slow_inj_alloc(struct net_device *dev,
				struct lan966x_afi_slow_inj_alloc_cfg *cfg,
				u32 *slowid);

/**
 * \brief Free AFI slow injection resource
 *
 * Before resources are freed, slow injection must be stopped.
 *
 * \param inst   [IN] Target instance reference.
 * \param slowid [IN] Slow injection ID.
 *
 * \return Return code.
 **/
bool lan966x_afi_slow_inj_free(struct net_device *dev, u32 slowid);

/**
 * \brief Setup frame for slow injection.
 *
 * \param inst   [IN] Target instance reference.
 * \param slowid [IN] Slow injection ID.
 *
 * \return Return code.
 **/
bool lan966x_afi_slow_inj_frm_hijack(struct net_device *dev, u32 slowid);

/**
 * Structure defining properties of a slow injection.
 */
struct lan966x_afi_slow_inj_start_cfg {
	/*[IN]
	 *Frames per hour.
	 */
	u64 fph;
};

/**
 * \brief Start slow injection.
 *
 * \param inst   [IN] Target instance reference.
 * \param slowid [IN] Slow injection ID.
 * \param cfg    [IN] Slow injection configuration.
 *
 * \return Return code.
 **/
bool lan966x_afi_slow_inj_start(struct net_device *dev, u32 slowid,
				struct lan966x_afi_slow_inj_start_cfg *cfg);

/**
 * \brief Stop slow injection.
 *
 * \param inst   [IN] Target instance reference.
 * \param slowid [IN] Slow injection ID.
 *
 * \return Return code.
 **/
bool lan966x_afi_slow_inj_stop(struct net_device *dev, u32 slowid);

struct lan966x_afi_tti {
	/* Arguments to most recent call to lan966x_afi_slow_inj_start() */
	struct lan966x_afi_slow_inj_start_cfg start_cfg;

	/* State of this entry (free/user-started/user-stopped) */
	enum lan966x_afi_entry_state state;

	/* TTI is paused by driver due to missing link on either down- or
	 * up-port.  For a flow to be started, state must be
	 * LAN966X_AFI_ENTRY_STATE_STARTED and paused must be 0.
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

struct lan966x_frm_info {
	u32 fp;
	u8 dstp;
	u8 fshort;
	u8 eprio;
};

/* FRM_TBL entry */
struct lan966x_afi_frm {
	/* 0 = Frame, 1 = Delay */
	u8  entry_type;
	/* Index of next FRM_TBL entry in sequence */
	u32 next_ptr;

	struct lan966x_frm_info frm_info;
};

struct lan966x_afi {
	/* FRM_TBL/DTI_TBL/TTI_TBL allocation.
	 * One bit per entry.
	 */
	u32 frms_alloced[(LAN966X_AFI_FRM_CNT      + 31) / 32];
	u32 ttis_alloced[(LAN966X_AFI_SLOW_INJ_CNT + 31) / 32];

	/* MISC_CTRL.AFI_ENA. Set when TTI/DTI is alloced. */
	u8  afi_ena;

	/* TTI_CTRL.TTI_ENA. Set when first TTI is alloced. */
	u8  tti_ena;

	/* FRM_TBL */
	struct lan966x_afi_frm frm_tbl[LAN966X_AFI_FRM_CNT];

	/* DTI_TBL */
	struct lan966x_afi_tti tti_tbl[LAN966X_AFI_SLOW_INJ_CNT];

	/* TICK length */
	u32 tick_len_us[8];

	/* Switch core's clock period in picoseconds */
	u64 clk_period_ps;
};

#endif /* _LAN966X_PTP_H_ */
