// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/iopoll.h>
#include <linux/bitfield.h>

#include "lan9645x_main.h"

#include "afi_api.h"

#define LAN9645X_AFI_SLOW_INJ_CNT		128
#define LAN9645X_AFI_FRM_CNT			128
#define LAN9645X_AFI_DTI_CNT			4
#define LAN9645X_PRIO_SUPER			8

/* Fast-injection (DTI) requested-rate bounds in bits/s (layer 1). */
#define LAN9645X_AFI_FAST_INJ_BPS_MIN		1000ULL
#define LAN9645X_AFI_FAST_INJ_BPS_MAX		10000000000ULL

/* FRM_INFO field layout */
#define LAN9645X_AFI_FRM_INFO_FP		GENMASK(11, 0)
#define LAN9645X_AFI_FRM_INFO_FP_GET(x) \
	FIELD_GET(LAN9645X_AFI_FRM_INFO_FP, x)
#define LAN9645X_AFI_FRM_INFO_FP_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_INFO_FP, x)
#define LAN9645X_AFI_FRM_INFO_DSTP		GENMASK(15, 12)
#define LAN9645X_AFI_FRM_INFO_DSTP_GET(x) \
	FIELD_GET(LAN9645X_AFI_FRM_INFO_DSTP, x)
#define LAN9645X_AFI_FRM_INFO_DSTP_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_INFO_DSTP, x)
#define LAN9645X_AFI_FRM_INFO_EPRIO		GENMASK(18, 16)
#define LAN9645X_AFI_FRM_INFO_EPRIO_GET(x) \
	FIELD_GET(LAN9645X_AFI_FRM_INFO_EPRIO, x)
#define LAN9645X_AFI_FRM_INFO_FSHORT		GENMASK(19, 19)
#define LAN9645X_AFI_FRM_INFO_FSHORT_GET(x) \
	FIELD_GET(LAN9645X_AFI_FRM_INFO_FSHORT, x)

/* Same fields in FRM_ENTRY_PART0 register */
#define LAN9645X_AFI_FRM_PART0_FP		LAN9645X_AFI_FRM_INFO_FP
#define LAN9645X_AFI_FRM_PART0_FP_SET(x) \
	LAN9645X_AFI_FRM_INFO_FP_SET(x)
#define LAN9645X_AFI_FRM_PART0_DSTP		LAN9645X_AFI_FRM_INFO_DSTP
#define LAN9645X_AFI_FRM_PART0_DSTP_SET(x) \
	LAN9645X_AFI_FRM_INFO_DSTP_SET(x)
#define LAN9645X_AFI_FRM_PART0_INJ_CNT		GENMASK(23, 16)
#define LAN9645X_AFI_FRM_PART0_INJ_CNT_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_PART0_INJ_CNT, x)

/* For a delay entry the whole PART0 register holds the delay in clock cycles */
#define LAN9645X_AFI_FRM_PART0_DELAY		GENMASK(29, 0)
#define LAN9645X_AFI_FRM_PART0_DELAY_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_PART0_DELAY, x)

/* FRM_INFO fields in FRM_ENTRY_PART1 register */
#define LAN9645X_AFI_FRM_PART1_FSHORT		GENMASK(0, 0)
#define LAN9645X_AFI_FRM_PART1_FSHORT_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_PART1_FSHORT, x)
#define LAN9645X_AFI_FRM_PART1_EPRIO		GENMASK(3, 1)
#define LAN9645X_AFI_FRM_PART1_EPRIO_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_PART1_EPRIO, x)
#define LAN9645X_AFI_FRM_PART1_RM		GENMASK(4, 4)
#define LAN9645X_AFI_FRM_PART1_RM_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_PART1_RM, x)
#define LAN9645X_AFI_FRM_PART1_RM_GET(x) \
	FIELD_GET(LAN9645X_AFI_FRM_PART1_RM, x)
#define LAN9645X_AFI_FRM_PART1_GONE		GENMASK(5, 5)
#define LAN9645X_AFI_FRM_PART1_GONE_GET(x) \
	FIELD_GET(LAN9645X_AFI_FRM_PART1_GONE, x)
#define LAN9645X_AFI_FRM_PART1_GONE_SET(x) \
	FIELD_PREP(LAN9645X_AFI_FRM_PART1_GONE, x)

struct lan9645x_afi_qu_ref {
	u32 chip_port;
	u32 qu_num;
};

/* Encode port and prio as internal queue system reference. */
static void lan9645x_afi_port_prio_2_qu_ref(u32 port_no, u32 prio,
					    struct lan9645x_afi_qu_ref *qu_ref)
{
	qu_ref->chip_port = port_no;

	/* The internal HW representation of qu_num has the fields
	 *
	 * INP_IDX  GENMASK(5, 0)
	 * SE_IDX   GENMASK(6, 19)
	 */
	qu_ref->qu_num = port_no * 64 + 8 * prio;
}

/* Initialize TTI calendar to start at slot 0 */
static int lan9645x_afi_tti_cal_init(struct lan9645x *lan9645x)
{
	u32 val;

	lan_rmw(AFI_TTI_CTRL_TTI_INIT_SET(1),
		AFI_TTI_CTRL_TTI_INIT,
		lan9645x, AFI_TTI_CTRL);

	return lan9645x_rd_poll_timeout(lan9645x, AFI_TTI_CTRL, val,
					AFI_TTI_CTRL_TTI_INIT_GET(val) == 0);
}

/* Set remove bit for a frame entry. */
static int lan9645x_afi_frm_set_rm(struct lan9645x *lan9645x, s32 frm_idx)
{
	u32 frm_tbl_part1;
	u32 val;

	if (!afi_frm_idx_chk(lan9645x->afi_ctrl, frm_idx))
		return -EINVAL;

	frm_tbl_part1 = lan_rd(lan9645x, AFI_FRM_ENTRY_PART1(frm_idx));

	val = LAN9645X_AFI_FRM_PART1_RM_GET(frm_tbl_part1);
	if (val) {
		pr_info("frm_rm already set\n");
		return -EINVAL;
	}

	val = LAN9645X_AFI_FRM_PART1_GONE_GET(frm_tbl_part1);
	if (val) {
		pr_info("frm_gone already set\n");
		return -EINVAL;
	}

	frm_tbl_part1 |= LAN9645X_AFI_FRM_PART1_RM_SET(1);
	lan_wr(frm_tbl_part1, lan9645x, AFI_FRM_ENTRY_PART1(frm_idx));

	return 0;
}

static int lan9645x_afi_frm_gone_wait(struct lan9645x *lan9645x, s32 frm_idx)
{
	u32 val;

	return lan9645x_rd_poll_timeout(lan9645x,
					AFI_FRM_ENTRY_PART1(frm_idx), val,
					LAN9645X_AFI_FRM_PART1_GONE_GET(val) == 1);
}

/* Hijack a frame injected with AFI bit set in the IFH to setup a entry in the
 * afi frame table.
 */
static bool lan9645x_afi_frm_hijack(struct lan9645x *lan9645x, s32 frm_idx)
{
	struct afi_frm *frm = &lan9645x->afi_ctrl->frm_tbl[frm_idx];
	u32 frm_info, ret, val;

	ret = lan9645x_rd_poll_timeout(lan9645x, AFI_NEW_FRM_CTRL, val,
				       AFI_NEW_FRM_CTRL_VLD_GET(val) == 1);
	if (ret) {
		dev_dbg(lan9645x->dev, "timeeout frm_idx=%d", frm_idx);
		return ret;
	}

	/* Get frm_info for hijacked frame */
	frm_info = lan_rd(lan9645x, AFI_NEW_FRM_INFO);
	frm_info = AFI_NEW_FRM_INFO_FRM_INFO_GET(frm_info);

	frm->frm_info.fp = LAN9645X_AFI_FRM_INFO_FP_GET(frm_info);
	frm->frm_info.dstp = LAN9645X_AFI_FRM_INFO_DSTP_GET(frm_info);
	frm->frm_info.fshort = LAN9645X_AFI_FRM_INFO_FSHORT_GET(frm_info);
	frm->frm_info.eprio = LAN9645X_AFI_FRM_INFO_EPRIO_GET(frm_info);

	dev_dbg(lan9645x->dev, "frm_idx=%d fp=%u dstp=%u fshort=%u eprio=%u",
		frm_idx,
		frm->frm_info.fp,
		frm->frm_info.dstp,
		frm->frm_info.fshort,
		frm->frm_info.eprio);

	/* Setup FRM_TBL entry */
	lan_rmw(AFI_FRM_NXT_AND_TYPE_ENTRY_TYPE_SET(0),
		AFI_FRM_NXT_AND_TYPE_ENTRY_TYPE, lan9645x,
		AFI_FRM_NXT_AND_TYPE(frm_idx));

	lan_wr(LAN9645X_AFI_FRM_PART0_FP_SET(frm->frm_info.fp) |
	       LAN9645X_AFI_FRM_PART0_DSTP_SET(frm->frm_info.dstp),
	       lan9645x,
	       AFI_FRM_ENTRY_PART0(frm_idx));

	lan_wr(LAN9645X_AFI_FRM_PART1_FSHORT_SET(frm->frm_info.fshort) |
	       LAN9645X_AFI_FRM_PART1_EPRIO_SET(frm->frm_info.eprio),
	       lan9645x,
	       AFI_FRM_ENTRY_PART1(frm_idx));

	lan_wr(AFI_NEW_FRM_CTRL_VLD_SET(0), lan9645x, AFI_NEW_FRM_CTRL);

	return 0;
}

static void lan9645x_afi_tti_pause_resume(struct lan9645x *lan9645x,
					  u32 tti_idx, bool pause)
{
	struct afi_tti *tti = &lan9645x->afi_ctrl->tti_tbl[tti_idx];

	lan_rmw(AFI_TTI_TIMER_TIMER_ENA_SET(pause ? 0 : 1),
		AFI_TTI_TIMER_TIMER_ENA,
		lan9645x, AFI_TTI_TIMER(tti_idx));

	tti->paused = pause;
}

/* Update QSYS frame reference for an AFI frame table entry */
static void lan9645x_afi_tti_qu_ref_update(struct lan9645x *lan9645x, u32 tti_idx)
{
	struct afi_tti *tti = &lan9645x->afi_ctrl->tti_tbl[tti_idx];
	struct lan9645x_afi_qu_ref qu_ref;

	lan9645x_afi_port_prio_2_qu_ref(tti->port_no, tti->prio, &qu_ref);

	lan_wr(AFI_TTI_PORT_QU_PORT_NUM_SET(qu_ref.chip_port) |
	       AFI_TTI_PORT_QU_QU_NUM_SET(qu_ref.qu_num),
	       lan9645x, AFI_TTI_PORT_QU(tti_idx));
}

static int lan9645x_afi_afi_enable(struct afi_control *afi)
{
	struct lan9645x *lan9645x = afi->priv;
	int ret;

	/* Enable AFI first */
	lan_rmw(AFI_MISC_CTRL_AFI_ENA_SET(1),
		AFI_MISC_CTRL_AFI_ENA,
		lan9645x, AFI_MISC_CTRL);

	ret = lan9645x_afi_tti_cal_init(lan9645x);
	if (ret)
		return ret;

	return 0;
}

static int lan9645x_afi_ttis_enable(struct afi_control *afi)
{
	struct lan9645x *lan9645x = afi->priv;

	/* Enable */
	lan_rmw(AFI_TTI_CTRL_TTI_ENA_SET(1),
		AFI_TTI_CTRL_TTI_ENA,
		lan9645x, AFI_TTI_CTRL);

	return 0;
}

/* Initialize the 8 TTI tick period lengths */
static int lan9645x_afi_tick_init(struct afi_control *afi)
{
	struct lan9645x *lan9645x = afi->priv;
	u64 tick_base_len, val0, val1, idx;
	u64 t_ps[8], tick_base_ps;
	u64 tmp;

	afi->clk_period_ps = lan9645x_ptp_get_period_ps();

	tick_base_len = AFI_TTI_TICK_LEN0_US * 1000000LLU;
	do_div(tick_base_len, afi->clk_period_ps);
	lan_rmw(AFI_TTI_TICK_BASE_BASE_LEN_SET(tick_base_len),
		AFI_TTI_TICK_BASE_BASE_LEN,
		lan9645x, AFI_TTI_TICK_BASE);

	tick_base_ps = tick_base_len * afi->clk_period_ps;

	/* Configure tick lengths */
	tmp = AFI_TTI_TICK_LEN0_US * 1000000LLU;
	do_div(tmp, tick_base_ps);

	lan_wr(AFI_TTI_TICK_LEN_0_3_LEN0_SET(tmp) |
	       AFI_TTI_TICK_LEN_0_3_LEN1_SET(AFI_TTI_TICK_LEN1_US /
					     AFI_TTI_TICK_LEN0_US) |
	       AFI_TTI_TICK_LEN_0_3_LEN2_SET(AFI_TTI_TICK_LEN2_US /
					     AFI_TTI_TICK_LEN1_US) |
	       AFI_TTI_TICK_LEN_0_3_LEN3_SET(AFI_TTI_TICK_LEN3_US /
					     AFI_TTI_TICK_LEN2_US),
	       lan9645x, AFI_TTI_TICK_LEN_0_3);

	lan_wr(AFI_TTI_TICK_LEN_4_7_LEN4_SET(AFI_TTI_TICK_LEN4_US /
					     AFI_TTI_TICK_LEN3_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN5_SET(AFI_TTI_TICK_LEN5_US /
					     AFI_TTI_TICK_LEN4_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN6_SET(AFI_TTI_TICK_LEN6_US /
					     AFI_TTI_TICK_LEN5_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN7_SET(AFI_TTI_TICK_LEN7_US /
					     AFI_TTI_TICK_LEN6_US),
	       lan9645x, AFI_TTI_TICK_LEN_4_7);

	/* Now that we have made the rounding errors that will come from using
	 * these constants, update the array that the rest of the code uses.
	 */
	val0 = lan_rd(lan9645x, AFI_TTI_TICK_LEN_0_3);
	val1 = lan_rd(lan9645x, AFI_TTI_TICK_LEN_4_7);

	/* In order to not accumulate rounding errors, first compute the
	 * tick lengths in ps and then found them to microseconds.
	 */
	t_ps[0] = AFI_TTI_TICK_LEN_0_3_LEN0_GET(val0) * tick_base_ps;
	t_ps[1] = AFI_TTI_TICK_LEN_0_3_LEN1_GET(val0) * t_ps[0];
	t_ps[2] = AFI_TTI_TICK_LEN_0_3_LEN2_GET(val0) * t_ps[1];
	t_ps[3] = AFI_TTI_TICK_LEN_0_3_LEN3_GET(val0) * t_ps[2];
	t_ps[4] = AFI_TTI_TICK_LEN_4_7_LEN4_GET(val1) * t_ps[3];
	t_ps[5] = AFI_TTI_TICK_LEN_4_7_LEN5_GET(val1) * t_ps[4];
	t_ps[6] = AFI_TTI_TICK_LEN_4_7_LEN6_GET(val1) * t_ps[5];
	t_ps[7] = AFI_TTI_TICK_LEN_4_7_LEN7_GET(val1) * t_ps[6];

	for (idx = 0; idx < ARRAY_SIZE(afi->tick_len_us); idx++) {
		tmp = t_ps[idx];
		do_div(tmp, 1000000LLU);
		afi->tick_len_us[idx] = tmp;
	}

	for (idx = 0; idx < afi->consts->slow_inj_cnt; idx++)
		lan_rmw(AFI_TTI_TIMER_TIMER_ENA_SET(0),
			AFI_TTI_TIMER_TIMER_ENA, lan9645x,
			AFI_TTI_TIMER(idx));

	return 0;
}

static int lan9645x_afi_tti_frm_hijack(struct afi_control *afi, u32 tti_idx)
{
	struct lan9645x *lan9645x = afi->priv;

	return lan9645x_afi_frm_hijack(lan9645x, afi->tti_tbl[tti_idx].frm_idx);
}

/* To remove a frame entry it must be injected one last time. No frame will
 * egress on a port.
 */
static int lan9645x_afi_tti_frm_rm_inj(struct afi_control *afi, u32 tti_idx)
{
	struct lan9645x *lan9645x = afi->priv;
	struct afi_tti *tti;
	int ret;

	tti = &afi->tti_tbl[tti_idx];

	if (tti->state != AFI_ENTRY_STATE_STOPPED) {
		pr_info("ID = %u: Injection must be stopped before rm injection\n",
			tti_idx);
		return -EINVAL;
	}

	ret = lan9645x_afi_frm_set_rm(lan9645x, tti->frm_idx);
	if (ret)
		return ret;

	/* Start removal injection!
	 * Set TIMER_LEN to max value (=> inject ASAP)
	 */
	lan_rmw(AFI_TTI_TIMER_TIMER_LEN,
		AFI_TTI_TIMER_TIMER_LEN,
		lan9645x, AFI_TTI_TIMER(tti_idx));

	lan_rmw(AFI_TTI_TIMER_TIMER_ENA_SET(1),
		AFI_TTI_TIMER_TIMER_ENA,
		lan9645x, AFI_TTI_TIMER(tti_idx));

	/* Wait until the frame is gone. */
	lan9645x_afi_frm_gone_wait(lan9645x, tti->frm_idx);

	return 0;
}

static int lan9645x_afi_tti_start(struct afi_control *afi, u32 tti_idx, bool do_config)
{
	struct lan9645x *lan9645x = afi->priv;
	struct afi_tti *tti;
	u32 rand_tick_cnt;

	tti = &afi->tti_tbl[tti_idx];

	if (tti->state != AFI_ENTRY_STATE_STOPPED) {
		pr_info("TTI already started\n");
		return -EINVAL;
	}

	if (do_config) {
		lan9645x_afi_tti_qu_ref_update(lan9645x, tti_idx);

		lan_rmw(AFI_TTI_TIMER_TICK_IDX_SET(tti->tick_idx),
			AFI_TTI_TIMER_TICK_IDX, lan9645x,
			AFI_TTI_TIMER(tti_idx));

		lan_rmw(AFI_TTI_TIMER_TIMER_LEN_SET(tti->timer_len),
			AFI_TTI_TIMER_TIMER_LEN, lan9645x,
			AFI_TTI_TIMER(tti_idx));

		lan_wr(AFI_TTI_FRM_FRM_PTR_SET(tti->frm_idx),
		       lan9645x, AFI_TTI_FRM(tti_idx));
	}

	/* Set TICK_CNT to a random value in range [1-TIMER_LEN] */
	rand_tick_cnt = 1 + (get_random_u32() % tti->timer_len);

	lan_rmw(AFI_TTI_TICKS_TICK_CNT_SET(rand_tick_cnt),
		AFI_TTI_TICKS_TICK_CNT, lan9645x,
		AFI_TTI_TICKS(tti_idx));

	lan9645x_afi_tti_pause_resume(lan9645x, tti_idx, false);

	tti->state = AFI_ENTRY_STATE_STARTED;

	pr_debug("%s tti_idx=%u do_config=%u frm_idx=%u timer_len=%u",
		 __func__, tti_idx, do_config, tti->frm_idx, tti->timer_len);

	return 0;
}

static int lan9645x_afi_tti_stop(struct afi_control *afi, u32 tti_idx)
{
	struct lan9645x *lan9645x = afi->priv;
	struct afi_tti *tti;

	tti = &afi->tti_tbl[tti_idx];

	if (tti->state != AFI_ENTRY_STATE_STARTED) {
		pr_info("TTI not started: %d\n", tti_idx);
		return -EINVAL;
	}

	lan9645x_afi_tti_pause_resume(lan9645x, tti_idx, true);

	tti->state = AFI_ENTRY_STATE_STOPPED;

	return 0;
}

/* Transmit a frame towards a port with the AFI inject marker set so the AFI
 * block can hijack it. Manual injection is required on lan9645x: the AFI
 * cannot inject via the NPI port unless frame aging is disabled on front
 * ports, so this mirrors the MRP injection path.
 */
static int lan9645x_afi_frm_inject(struct afi_control *afi, u32 port_no,
				   u32 prio, struct sk_buff *skb)
{
	struct lan9645x *lan9645x = afi->priv;
	struct lan9645x_port *port;
	u8 ifh[LAN9645X_IFH_LEN];

	if (port_no >= NUM_PHYS_PORTS)
		return -EINVAL;

	port = lan9645x_to_port(lan9645x, port_no);
	if (!port)
		return -EINVAL;

	memset(ifh, 0, LAN9645X_IFH_LEN);
	LAN9645X_IFH_SET(ifh, IFH_BYPASS, 1);
	LAN9645X_IFH_SET(ifh, IFH_DSTS, port->chip_port);
	LAN9645X_IFH_SET(ifh, IFH_AFI, 1);
	LAN9645X_IFH_SET(ifh, IFH_QOS_CLASS, prio);

	return lan9645x_inj_xmit(port, skb, (__be32 *)ifh);
}

/* Capture a previously injected frame into the given frame table entry. The
 * DTI-specific fields (INJ_CNT, NEXT_PTR) are written later by dti_start.
 */
static int lan9645x_afi_dti_frm_hijack(struct afi_control *afi, u32 frm_idx)
{
	struct lan9645x *lan9645x = afi->priv;

	return lan9645x_afi_frm_hijack(lan9645x, frm_idx);
}

/* Write a frame table entry (frame or delay) and its sequence link. */
static void lan9645x_afi_dti_frm_entry_write(struct lan9645x *lan9645x,
					     struct afi_frm *frm, u32 frm_idx)
{
	if (frm->entry_type) {
		/* Delay entry: PART0 holds the delay. */
		lan_wr(LAN9645X_AFI_FRM_PART0_DELAY_SET(frm->delay),
		       lan9645x, AFI_FRM_ENTRY_PART0(frm_idx));
	} else {
		/* Frame entry: only update INJ_CNT, keep FP/DSTP from hijack. */
		lan_rmw(LAN9645X_AFI_FRM_PART0_INJ_CNT_SET(frm->inj_cnt),
			LAN9645X_AFI_FRM_PART0_INJ_CNT,
			lan9645x, AFI_FRM_ENTRY_PART0(frm_idx));
	}

	lan_wr(AFI_FRM_NXT_AND_TYPE_NEXT_PTR_SET(frm->next_idx) |
	       AFI_FRM_NXT_AND_TYPE_ENTRY_TYPE_SET(frm->entry_type),
	       lan9645x, AFI_FRM_NXT_AND_TYPE(frm_idx));
}

static void lan9645x_afi_dti_qu_ref_update(struct lan9645x *lan9645x,
					   u32 dti_idx)
{
	struct afi_dti *dti = &lan9645x->afi_ctrl->dti_tbl[dti_idx];
	struct lan9645x_afi_qu_ref qu_ref;

	lan9645x_afi_port_prio_2_qu_ref(dti->port_no, dti->prio, &qu_ref);

	lan_wr(AFI_DTI_PORT_QU_PORT_NUM_SET(qu_ref.chip_port) |
	       AFI_DTI_PORT_QU_QU_NUM_SET(qu_ref.qu_num),
	       lan9645x, AFI_DTI_PORT_QU(dti_idx));
}

static int lan9645x_afi_dti_start(struct afi_control *afi, u32 dti_idx,
				  bool do_frm_cfg, bool do_dti_cfg, bool start)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	struct lan9645x *lan9645x = afi->priv;
	u32 frm_idx;

	if (do_frm_cfg) {
		/* (Re)write the frame/delay sequence. */
		frm_idx = dti->first_frm_idx;
		while (frm_idx) {
			struct afi_frm *frm = &afi->frm_tbl[frm_idx];

			lan9645x_afi_dti_frm_entry_write(lan9645x, frm, frm_idx);
			frm_idx = frm->next_idx;
		}
	}

	if (do_dti_cfg) {
		lan_wr(AFI_DTI_MODE_MODE_SET(dti->mode) |
		       AFI_DTI_MODE_TRAILING_DELAY_SEQ_CNT_SET(dti->trailing_delay_seq_cnt) |
		       AFI_DTI_MODE_FRM_INJ_CNT_SET(0) |
		       AFI_DTI_MODE_DTI_NEXT_SET(dti->dti_next),
		       lan9645x, AFI_DTI_MODE(dti_idx));

		lan9645x_afi_dti_qu_ref_update(lan9645x, dti_idx);
	}

	/* Settings needed on every (re)start. */
	lan_wr(AFI_DTI_FRM_FIRST_FRM_PTR_SET(dti->first_frm_idx) |
	       AFI_DTI_FRM_NEXT_FRM_PTR_SET(dti->first_frm_idx),
	       lan9645x, AFI_DTI_FRM(dti_idx));

	lan_wr(AFI_DTI_CNT_CNT_SET(dti->mode == 1 ? 0 : dti->frm_inj_cnt),
	       lan9645x, AFI_DTI_CNT(dti_idx));

	lan_wr(0, lan9645x, AFI_DTI_CNT_DOWN(dti_idx));
	lan_wr(0, lan9645x, AFI_DTI_DURATION(dti_idx));

	lan_wr(AFI_DTI_CTRL_ENA_SET(start ? 1 : 0) |
	       AFI_DTI_CTRL_BW_SET(dti->bw),
	       lan9645x, AFI_DTI_CTRL(dti_idx));

	return 0;
}

static int lan9645x_afi_dti_stop(struct afi_control *afi, u32 dti_idx)
{
	struct lan9645x *lan9645x = afi->priv;

	lan_rmw(AFI_DTI_CTRL_ENA_SET(0), AFI_DTI_CTRL_ENA,
		lan9645x, AFI_DTI_CTRL(dti_idx));

	return 0;
}

/* To remove the frames of a DTI from buffer memory, inject the sequence one
 * last time with the FRM_RM bit set and all delays zeroed. No frame egresses.
 */
static int lan9645x_afi_dti_frm_rm_inj(struct afi_control *afi, u32 dti_idx)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	struct lan9645x *lan9645x = afi->priv;
	s32 frm_idx, last_frm_idx = 0;
	int ret;

	if (dti->state != AFI_ENTRY_STATE_STOPPED) {
		pr_info("ID = %u: Injection must be stopped before rm injection\n",
			dti_idx);
		return -EINVAL;
	}

	/* Set the FRM_RM bit for each frame entry to be removed. */
	for (frm_idx = dti->first_frm_idx; frm_idx > 0;
	     frm_idx = afi->frm_tbl[frm_idx].next_idx) {
		if (afi->frm_tbl[frm_idx].entry_type == 0) {
			ret = lan9645x_afi_frm_set_rm(lan9645x, frm_idx);
			if (ret)
				return ret;
			last_frm_idx = frm_idx;
		}
	}

	lan_rmw(AFI_DTI_FRM_NEXT_FRM_PTR_SET(dti->first_frm_idx),
		AFI_DTI_FRM_NEXT_FRM_PTR, lan9645x, AFI_DTI_FRM(dti_idx));

	if (dti->mode != 0)
		lan_rmw(AFI_DTI_MODE_MODE_SET(0) | AFI_DTI_MODE_FRM_INJ_CNT_SET(0),
			AFI_DTI_MODE_MODE | AFI_DTI_MODE_FRM_INJ_CNT,
			lan9645x, AFI_DTI_MODE(dti_idx));

	lan_rmw(AFI_DTI_CNT_CNT_SET(1), AFI_DTI_CNT_CNT,
		lan9645x, AFI_DTI_CNT(dti_idx));

	/* Zero all delays to speed up the removal. */
	for (frm_idx = dti->first_frm_idx; frm_idx > 0;
	     frm_idx = afi->frm_tbl[frm_idx].next_idx) {
		if (afi->frm_tbl[frm_idx].entry_type == 1)
			lan_wr(0, lan9645x, AFI_FRM_ENTRY_PART0(frm_idx));
	}

	lan_wr(0, lan9645x, AFI_DTI_CNT_DOWN(dti_idx));

	/* Start the removal injection. */
	lan_wr(AFI_DTI_CTRL_ENA_SET(1) | AFI_DTI_CTRL_BW_SET(0),
	       lan9645x, AFI_DTI_CTRL(dti_idx));

	if (last_frm_idx)
		lan9645x_afi_frm_gone_wait(lan9645x, last_frm_idx);

	return 0;
}

static int lan9645x_afi_dti_seq_cnt_get(struct afi_control *afi, u32 dti_idx,
					u32 *seq_cnt)
{
	struct lan9645x *lan9645x = afi->priv;

	*seq_cnt = AFI_DTI_CNT_CNT_GET(lan_rd(lan9645x, AFI_DTI_CNT(dti_idx)));

	return 0;
}

struct afi_consts lan9645x_afi_consts = {
	.frm_cnt = LAN9645X_AFI_FRM_CNT,
	.slow_inj_cnt = LAN9645X_AFI_SLOW_INJ_CNT,
	.dti_cnt = LAN9645X_AFI_DTI_CNT,
	.prio_super = LAN9645X_PRIO_SUPER,
	.fast_inj_bps_min = LAN9645X_AFI_FAST_INJ_BPS_MIN,
	.fast_inj_bps_max = LAN9645X_AFI_FAST_INJ_BPS_MAX,
};

struct afi_operations lan9645x_afi_operations = {
	.afi_enable = lan9645x_afi_afi_enable,
	.ttis_enable = lan9645x_afi_ttis_enable,
	.tick_init = lan9645x_afi_tick_init,
	.tti_frm_hijack = lan9645x_afi_tti_frm_hijack,
	.tti_frm_rm_inj = lan9645x_afi_tti_frm_rm_inj,
	.tti_start = lan9645x_afi_tti_start,
	.tti_stop = lan9645x_afi_tti_stop,
	.frm_inject = lan9645x_afi_frm_inject,
	.dti_frm_hijack = lan9645x_afi_dti_frm_hijack,
	.dti_frm_rm_inj = lan9645x_afi_dti_frm_rm_inj,
	.dti_start = lan9645x_afi_dti_start,
	.dti_stop = lan9645x_afi_dti_stop,
	.dti_seq_cnt_get = lan9645x_afi_dti_seq_cnt_get,
};

void lan9645x_afi_deinit(struct lan9645x *lan9645x)
{
	afi_deinit(lan9645x->afi_ctrl);

	kfree(lan9645x->afi_ctrl);
}

int lan9645x_afi_init(struct lan9645x *lan9645x)
{
	struct afi_control *afi_ctrl;

	afi_ctrl = kzalloc(sizeof(*afi_ctrl), GFP_KERNEL);
	if (!afi_ctrl)
		return -ENOMEM;

	afi_ctrl->ops = &lan9645x_afi_operations;
	afi_ctrl->consts = &lan9645x_afi_consts;
	afi_ctrl->priv = lan9645x;
	lan9645x->afi_ctrl = afi_ctrl;

	return afi_init(afi_ctrl);
}
