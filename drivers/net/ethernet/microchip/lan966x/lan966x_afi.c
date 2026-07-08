// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/iopoll.h>
#include <linux/bitfield.h>

#include "lan966x_main.h"

#include "afi_api.h"
#include "afi_netlink.h"

#define LAN966X_AFI_SLOW_INJ_CNT		64
#define LAN966X_AFI_FRM_CNT			64
#define LAN966X_AFI_DTI_CNT			16
#define LAN966X_PRIO_SUPER			8

#define LAN966X_AFI_FAST_INJ_BPS_MIN		1000ULL
#define LAN966X_AFI_FAST_INJ_BPS_MAX		2500000000ULL

#define LAN966X_AFI_FRM_PART0_INJ_CNT		GENMASK(20, 13)
#define LAN966X_AFI_FRM_PART0_DELAY		GENMASK(29, 0)

#define LAN966X_AFI_FRM_PART0_FP_POS		0
#define LAN966X_AFI_FRM_PART0_DSTP_POS		9
#define LAN966X_AFI_FRM_PART1_FSHORT_POS	0
#define LAN966X_AFI_FRM_PART1_EPRIO_POS		1

#define LAN966X_AFI_FRM_INFO_FP_MASK		GENMASK(8, 0)
#define LAN966X_AFI_FRM_INFO_DSTP_MASK		GENMASK(12, 9)
#define LAN966X_AFI_FRM_INFO_EPRIO_MASK		GENMASK(15, 13)
#define LAN966X_AFI_FRM_INFO_FSHORT_MASK	GENMASK(17, 16)

#define LAN966X_AFI_FRM_TBL_PART1_RM_POS	4

#define LAN966X_AFI_FRM_TBL_PART1_RM_MASK	GENMASK(5, 4)
#define LAN966X_AFI_FRM_TBL_PART1_GONE_MASK	GENMASK(7, 6)

#define LAN966X_AFI_TTI_TBL_TIMER_LEN_WID	9

#define LAN966X_WAIT_AFI_SLEEP_US		10
#define LAN966X_WAIT_AFI_TIMEOUT_US		1000000

struct lan966x_afi_qu_ref {
	u32 chip_port;
	u32 qu_num;
};

static void lan966x_afi_port_prio_2_qu_ref(u32 port_no, u32 prio,
					   struct lan966x_afi_qu_ref *qu_ref)
{
	qu_ref->chip_port = port_no;
	qu_ref->qu_num = port_no * 64;
}

static int lan966x_afi_tti_cal_init(struct lan966x *lan966x)
{
	u32  max_poll_cnt = 5;
	bool tti_init = true;
	u32  val;

	lan_rmw(AFI_TTI_CTRL_TTI_INIT_SET(1),
		AFI_TTI_CTRL_TTI_INIT,
		lan966x, AFI_TTI_CTRL);

	/* Wait for device to clear TTI_INIT */
	/* Replace with a readx_poll_timeout */
	while (max_poll_cnt-- > 0) {
		val = lan_rd(lan966x, AFI_TTI_CTRL);
		tti_init = AFI_TTI_CTRL_TTI_INIT_GET(val);
		if (tti_init == 0)
			break;
	}

	if (tti_init == 1)
		return -ETIMEDOUT;

	return 0;
}

static int lan966x_afi_frm_set_rm(struct lan966x *lan966x, s32 frm_idx)
{
	u32 frm_tbl_part1;
	u32 val;

	if (!afi_frm_idx_chk(lan966x->afi_ctrl, frm_idx))
		return -EINVAL;

	frm_tbl_part1 = lan_rd(lan966x, AFI_FRM_ENTRY_PART1(frm_idx));

	val = FIELD_GET(LAN966X_AFI_FRM_TBL_PART1_RM_MASK, frm_tbl_part1);
	if (val) {
		pr_info("frm_rm already set\n");
		return -EINVAL;
	}

	val = FIELD_GET(LAN966X_AFI_FRM_TBL_PART1_GONE_MASK, frm_tbl_part1);
	if (val) {
		pr_info("frm_gone already set\n");
		return -EINVAL;
	}

	frm_tbl_part1 |= BIT(LAN966X_AFI_FRM_TBL_PART1_RM_POS);
	lan_wr(frm_tbl_part1, lan966x, AFI_FRM_ENTRY_PART1(frm_idx));

	return 0;
}

static void lan966x_afi_frm_gone_get(struct lan966x *lan966x,
				     u8 *const frm_gone, s32 frm_idx)
{
	u32 frm_tbl_part1;

	frm_tbl_part1 = lan_rd(lan966x, AFI_FRM_ENTRY_PART1(frm_idx));
	*frm_gone = FIELD_GET(LAN966X_AFI_FRM_TBL_PART1_GONE_MASK,
			      frm_tbl_part1);
}

static bool lan966x_afi_frm_gone_wait(struct lan966x *lan966x, u32 idx,
				      u32 port_no, s32 frm_idx, bool is_dti)
{
	u32 poll_cnt, poll_cnt_max;
	u8 frm_gone = 0;

	poll_cnt_max = (lan966x->afi_ctrl->consts->slow_inj_cnt * 4) / 50;

	/* Poll for FRM_GONE == 1 for last frame */
	/* Replace with a readx_poll_timeout */
	poll_cnt = 0;
	while (!frm_gone && poll_cnt++ < poll_cnt_max)
		lan966x_afi_frm_gone_get(lan966x, &frm_gone, frm_idx);

	return frm_gone != 0;
}

static int lan966x_afi_frm_get_ctrl(struct lan966x *lan966x)
{
	return lan_rd(lan966x, AFI_NEW_FRM_CTRL);
}

static bool lan966x_afi_frm_hijack(struct lan966x *lan966x, s32 frm_idx)
{
	struct afi_frm *frm = &lan966x->afi_ctrl->frm_tbl[frm_idx];
	u32 frm_info, ret, val;

	/* Wait for frame to be hijacked. This can take up to an unspecified
	 * amount of time, because it depends on the time between the
	 * application transmits the frame and then calls the hijack function.
	 * In the Microsemi application, the AFI module waits for an
	 * acknowledgment from the packet module that the frame is transmitted
	 * before invoking the hijack function. The problem is that this
	 * acknowledgment may come way before the frame has actually hit the
	 * hardware (under Linux). Let's compensate for that and allow up to
	 * ten seconds to elapse here.
	 */
	ret = readx_poll_timeout(lan966x_afi_frm_get_ctrl, lan966x, val,
				 (AFI_NEW_FRM_CTRL_VLD_GET(val) == 1),
				  LAN966X_WAIT_AFI_SLEEP_US,
				  LAN966X_WAIT_AFI_TIMEOUT_US);
	if (ret)
		return ret;

	/* Get frm_info for hijacked frame */
	frm_info = lan_rd(lan966x, AFI_NEW_FRM_INFO);
	frm_info = AFI_NEW_FRM_INFO_FRM_INFO_GET(frm_info);

	frm->frm_info.fp = FIELD_GET(LAN966X_AFI_FRM_INFO_FP_MASK, frm_info);
	frm->frm_info.dstp = FIELD_GET(LAN966X_AFI_FRM_INFO_DSTP_MASK,
				       frm_info);
	frm->frm_info.fshort = FIELD_GET(LAN966X_AFI_FRM_INFO_FSHORT_MASK,
					 frm_info);
	frm->frm_info.eprio = FIELD_GET(LAN966X_AFI_FRM_INFO_EPRIO_MASK,
					frm_info);

	/* Setup FRM_TBL entry */
	lan_rmw(AFI_FRM_NXT_AND_TYPE_ENTRY_TYPE_SET(0),
		AFI_FRM_NXT_AND_TYPE_ENTRY_TYPE, lan966x,
		AFI_FRM_NXT_AND_TYPE(frm_idx));

	lan_wr(frm->frm_info.fp << LAN966X_AFI_FRM_PART0_FP_POS |
	       frm->frm_info.dstp << LAN966X_AFI_FRM_PART0_DSTP_POS,
	       lan966x, AFI_FRM_ENTRY_PART0(frm_idx));

	lan_wr(frm->frm_info.fshort << LAN966X_AFI_FRM_PART1_FSHORT_POS |
	       frm->frm_info.eprio << LAN966X_AFI_FRM_PART1_EPRIO_POS,
	       lan966x, AFI_FRM_ENTRY_PART1(frm_idx));

	lan_wr(AFI_NEW_FRM_CTRL_VLD_SET(0), lan966x, AFI_NEW_FRM_CTRL);

	return 0;
}

static void lan966x_afi_tti_pause_resume(struct lan966x *lan966x,
					 u32 tti_idx, bool pause)
{
	struct afi_tti *tti = &lan966x->afi_ctrl->tti_tbl[tti_idx];

	lan_rmw(AFI_TTI_TIMER_TIMER_ENA_SET(pause ? 0 : 1),
		AFI_TTI_TIMER_TIMER_ENA,
		lan966x, AFI_TTI_TIMER(tti_idx));

	tti->paused = pause;
}

static void lan966x_afi_tti_qu_ref_update(struct lan966x *lan966x, u32 tti_idx)
{
	struct afi_tti *tti = &lan966x->afi_ctrl->tti_tbl[tti_idx];
	struct lan966x_afi_qu_ref qu_ref;

	lan966x_afi_port_prio_2_qu_ref(tti->port_no, tti->prio, &qu_ref);

	lan_wr(AFI_TTI_PORT_QU_PORT_NUM_SET(qu_ref.chip_port) |
	       AFI_TTI_PORT_QU_QU_NUM_SET(qu_ref.qu_num),
	       lan966x, AFI_TTI_PORT_QU(tti_idx));
}

static int lan966x_afi_afi_enable(struct afi_control *afi)
{
	struct lan966x *lan966x = afi->priv;
	int ret;

	/* Enable AFI first */
	lan_rmw(AFI_MISC_CTRL_AFI_ENA_SET(1),
		AFI_MISC_CTRL_AFI_ENA,
		lan966x, AFI_MISC_CTRL);

	ret = lan966x_afi_tti_cal_init(lan966x);
	if (ret)
		return ret;

	return 0;
}

static int lan966x_afi_ttis_enable(struct afi_control *afi)
{
	struct lan966x *lan966x = afi->priv;

	/* Enable */
	lan_rmw(AFI_TTI_CTRL_TTI_ENA_SET(1),
		AFI_TTI_CTRL_TTI_ENA,
		lan966x, AFI_TTI_CTRL);

	return 0;
}

static int lan966x_afi_tick_init(struct afi_control *afi)
{
	struct lan966x *lan966x = afi->priv;
	u64 tick_base_len, val0, val1, idx;
	u64 t_ps[8], tick_base_ps;
	u64 tmp;

	afi->clk_period_ps = lan966x_ptp_get_period_ps();

	tick_base_len = AFI_TTI_TICK_LEN0_US * 1000000LLU;
	do_div(tick_base_len, afi->clk_period_ps);
	lan_rmw(AFI_TTI_TICK_BASE_BASE_LEN_SET(tick_base_len),
		AFI_TTI_TICK_BASE_BASE_LEN,
		lan966x, AFI_TTI_TICK_BASE);

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
	       lan966x, AFI_TTI_TICK_LEN_0_3);

	lan_wr(AFI_TTI_TICK_LEN_4_7_LEN4_SET(AFI_TTI_TICK_LEN4_US /
					     AFI_TTI_TICK_LEN3_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN5_SET(AFI_TTI_TICK_LEN5_US /
					     AFI_TTI_TICK_LEN4_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN6_SET(AFI_TTI_TICK_LEN6_US /
					     AFI_TTI_TICK_LEN5_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN7_SET(AFI_TTI_TICK_LEN7_US /
					     AFI_TTI_TICK_LEN6_US),
	       lan966x, AFI_TTI_TICK_LEN_4_7);

	/* Now that we have made the rounding errors that will come from using
	 * these constants, update the array that the rest of the code uses.
	 */
	val0 = lan_rd(lan966x, AFI_TTI_TICK_LEN_0_3);
	val1 = lan_rd(lan966x, AFI_TTI_TICK_LEN_4_7);

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
			AFI_TTI_TIMER_TIMER_ENA, lan966x,
			AFI_TTI_TIMER(idx));

	return 0;
}

static int lan966x_afi_tti_frm_hijack(struct afi_control *afi, u32 tti_idx)
{
	struct lan966x *lan966x = afi->priv;

	return lan966x_afi_frm_hijack(lan966x,
				      afi->tti_tbl[tti_idx].frm_idx);
}

static int lan966x_afi_tti_frm_rm_inj(struct afi_control *afi, u32 tti_idx)
{
	struct lan966x *lan966x = afi->priv;
	struct afi_tti *tti;
	int ret;

	tti = &afi->tti_tbl[tti_idx];

	if (tti->state != AFI_ENTRY_STATE_STOPPED) {
		pr_info("ID = %u: Injection must be stopped before rm injection\n",
			tti_idx);
		return -EINVAL;
	}

	ret = lan966x_afi_frm_set_rm(lan966x, tti->frm_idx);
	if (ret)
		return ret;

	/* Start removal injection!
	 * Set TIMER_LEN to max value (=> inject ASAP)
	 */
	lan_rmw(AFI_TTI_TIMER_TIMER_LEN_SET((1 << LAN966X_AFI_TTI_TBL_TIMER_LEN_WID) - 1),
		AFI_TTI_TIMER_TIMER_LEN,
		lan966x, AFI_TTI_TIMER(tti_idx));

	lan_rmw(AFI_TTI_TIMER_TIMER_ENA_SET(1),
		AFI_TTI_TIMER_TIMER_ENA,
		lan966x, AFI_TTI_TIMER(tti_idx));

	/* Wait until the frame is gone. */
	lan966x_afi_frm_gone_wait(lan966x, tti_idx, tti->port_no,
				  tti->frm_idx, false);

	return 0;
}

static int lan966x_afi_tti_start(struct afi_control *afi, u32 tti_idx, bool do_config)
{
	struct lan966x *lan966x = afi->priv;
	struct afi_tti *tti;
	u32 rand_tick_cnt;

	tti = &afi->tti_tbl[tti_idx];

	if (tti->state != AFI_ENTRY_STATE_STOPPED) {
		pr_info("TTI already started\n");
		return -EINVAL;
	}

	if (do_config) {
		lan966x_afi_tti_qu_ref_update(lan966x, tti_idx);

		lan_rmw(AFI_TTI_TIMER_TICK_IDX_SET(tti->tick_idx),
			AFI_TTI_TIMER_TICK_IDX, lan966x,
			AFI_TTI_TIMER(tti_idx));

		lan_rmw(AFI_TTI_TIMER_TIMER_LEN_SET(tti->timer_len),
			AFI_TTI_TIMER_TIMER_LEN, lan966x,
			AFI_TTI_TIMER(tti_idx));

		lan_wr(AFI_TTI_FRM_FRM_PTR_SET(tti->frm_idx),
		       lan966x, AFI_TTI_FRM(tti_idx));
	}

	/* Set TICK_CNT to a random value in range [1-TIMER_LEN] */
	rand_tick_cnt = 1 + (get_random_u32() % tti->timer_len);

	lan_rmw(AFI_TTI_TICKS_TICK_CNT_SET(rand_tick_cnt),
		AFI_TTI_TICKS_TICK_CNT, lan966x,
		AFI_TTI_TICKS(tti_idx));

	lan966x_afi_tti_pause_resume(lan966x, tti_idx, false);

	tti->state = AFI_ENTRY_STATE_STARTED;

	return 0;
}

static int lan966x_afi_tti_stop(struct afi_control *afi, u32 tti_idx)
{
	struct lan966x *lan966x = afi->priv;
	struct afi_tti *tti;

	tti = &afi->tti_tbl[tti_idx];

	if (tti->state != AFI_ENTRY_STATE_STARTED) {
		pr_info("TTI not started: %d\n", tti_idx);
		return -EINVAL;
	}

	lan966x_afi_tti_pause_resume(lan966x, tti_idx, true);

	tti->state = AFI_ENTRY_STATE_STOPPED;

	return 0;
}

static int lan966x_afi_frm_inject(struct afi_control *afi, u32 port_no,
				  u32 prio, struct sk_buff *skb)
{
	struct lan966x *lan966x = afi->priv;
	struct lan966x_port *port;
	__be32 ifh[IFH_LEN];
	netdev_tx_t tx;

	if (port_no >= NUM_PHYS_PORTS)
		return -EINVAL;

	port = lan966x->ports[port_no];
	if (!port)
		return -EINVAL;

	memset(ifh, 0, sizeof(__be32) * IFH_LEN);
	lan966x_ifh_set_bypass(ifh, 1);
	lan966x_ifh_set_port(ifh, BIT_ULL(port->chip_port));
	lan966x_ifh_set_afi(ifh, 1);

	local_bh_disable();
	tx = lan966x_xmit(port, skb, ifh);
	local_bh_enable();

	if (tx != NETDEV_TX_OK)
		return -EBUSY;

	return 0;
}

static int lan966x_afi_dti_frm_hijack(struct afi_control *afi, u32 frm_idx)
{
	struct lan966x *lan966x = afi->priv;

	return lan966x_afi_frm_hijack(lan966x, frm_idx);
}

static void lan966x_afi_dti_frm_entry_write(struct lan966x *lan966x,
					    struct afi_frm *frm, u32 frm_idx)
{
	if (frm->entry_type)
		lan_wr(FIELD_PREP(LAN966X_AFI_FRM_PART0_DELAY, frm->delay),
		       lan966x, AFI_FRM_ENTRY_PART0(frm_idx));
	else
		lan_rmw(FIELD_PREP(LAN966X_AFI_FRM_PART0_INJ_CNT, frm->inj_cnt),
			LAN966X_AFI_FRM_PART0_INJ_CNT,
			lan966x, AFI_FRM_ENTRY_PART0(frm_idx));

	lan_wr(AFI_FRM_NXT_AND_TYPE_NEXT_PTR_SET(frm->next_idx) |
	       AFI_FRM_NXT_AND_TYPE_ENTRY_TYPE_SET(frm->entry_type),
	       lan966x, AFI_FRM_NXT_AND_TYPE(frm_idx));
}

static void lan966x_afi_dti_qu_ref_update(struct lan966x *lan966x, u32 dti_idx)
{
	struct afi_dti *dti = &lan966x->afi_ctrl->dti_tbl[dti_idx];
	struct lan966x_afi_qu_ref qu_ref;

	lan966x_afi_port_prio_2_qu_ref(dti->port_no, dti->prio, &qu_ref);

	lan_wr(AFI_DTI_PORT_QU_PORT_NUM_SET(qu_ref.chip_port) |
	       AFI_DTI_PORT_QU_QU_NUM_SET(qu_ref.qu_num),
	       lan966x, AFI_DTI_PORT_QU(dti_idx));
}

static int lan966x_afi_dti_start(struct afi_control *afi, u32 dti_idx,
				 bool do_frm_cfg, bool do_dti_cfg, bool start)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	struct lan966x *lan966x = afi->priv;
	u32 frm_idx;

	if (do_frm_cfg) {
		frm_idx = dti->first_frm_idx;
		while (frm_idx) {
			struct afi_frm *frm = &afi->frm_tbl[frm_idx];

			lan966x_afi_dti_frm_entry_write(lan966x, frm, frm_idx);
			frm_idx = frm->next_idx;
		}
	}

	if (do_dti_cfg) {
		lan_wr(AFI_DTI_MODE_MODE_SET(dti->mode) |
		       AFI_DTI_MODE_TRAILING_DELAY_SEQ_CNT_SET(dti->trailing_delay_seq_cnt) |
		       AFI_DTI_MODE_FRM_INJ_CNT_SET(0) |
		       AFI_DTI_MODE_DTI_NEXT_SET(dti->dti_next),
		       lan966x, AFI_DTI_MODE(dti_idx));

		lan966x_afi_dti_qu_ref_update(lan966x, dti_idx);
	}

	lan_wr(AFI_DTI_FRM_FIRST_FRM_PTR_SET(dti->first_frm_idx) |
	       AFI_DTI_FRM_NEXT_FRM_PTR_SET(dti->first_frm_idx),
	       lan966x, AFI_DTI_FRM(dti_idx));

	lan_wr(AFI_DTI_CNT_CNT_SET(dti->mode == 1 ? 0 : dti->frm_inj_cnt),
	       lan966x, AFI_DTI_CNT(dti_idx));

	lan_wr(0, lan966x, AFI_DTI_CNT_DOWN(dti_idx));
	lan_wr(0, lan966x, AFI_DTI_DURATION(dti_idx));

	lan_wr(AFI_DTI_CTRL_ENA_SET(start ? 1 : 0) |
	       AFI_DTI_CTRL_BW_SET(dti->bw),
	       lan966x, AFI_DTI_CTRL(dti_idx));

	return 0;
}

static int lan966x_afi_dti_stop(struct afi_control *afi, u32 dti_idx)
{
	struct lan966x *lan966x = afi->priv;

	lan_rmw(AFI_DTI_CTRL_ENA_SET(0), AFI_DTI_CTRL_ENA,
		lan966x, AFI_DTI_CTRL(dti_idx));

	return 0;
}

static int lan966x_afi_dti_frm_rm_inj(struct afi_control *afi, u32 dti_idx)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	struct lan966x *lan966x = afi->priv;
	s32 frm_idx, last_frm_idx = 0;
	int ret;

	if (dti->state != AFI_ENTRY_STATE_STOPPED) {
		pr_info("ID = %u: Injection must be stopped before rm injection\n",
			dti_idx);
		return -EINVAL;
	}

	for (frm_idx = dti->first_frm_idx; frm_idx > 0;
	     frm_idx = afi->frm_tbl[frm_idx].next_idx) {
		if (afi->frm_tbl[frm_idx].entry_type == 0) {
			ret = lan966x_afi_frm_set_rm(lan966x, frm_idx);
			if (ret)
				return ret;
			last_frm_idx = frm_idx;
		}
	}

	lan_rmw(AFI_DTI_FRM_NEXT_FRM_PTR_SET(dti->first_frm_idx),
		AFI_DTI_FRM_NEXT_FRM_PTR, lan966x, AFI_DTI_FRM(dti_idx));

	if (dti->mode != 0)
		lan_rmw(AFI_DTI_MODE_MODE_SET(0) | AFI_DTI_MODE_FRM_INJ_CNT_SET(0),
			AFI_DTI_MODE_MODE | AFI_DTI_MODE_FRM_INJ_CNT,
			lan966x, AFI_DTI_MODE(dti_idx));

	lan_rmw(AFI_DTI_CNT_CNT_SET(1), AFI_DTI_CNT_CNT,
		lan966x, AFI_DTI_CNT(dti_idx));

	for (frm_idx = dti->first_frm_idx; frm_idx > 0;
	     frm_idx = afi->frm_tbl[frm_idx].next_idx) {
		if (afi->frm_tbl[frm_idx].entry_type == 1)
			lan_wr(0, lan966x, AFI_FRM_ENTRY_PART0(frm_idx));
	}

	lan_wr(0, lan966x, AFI_DTI_CNT_DOWN(dti_idx));

	lan_wr(AFI_DTI_CTRL_ENA_SET(1) | AFI_DTI_CTRL_BW_SET(0),
	       lan966x, AFI_DTI_CTRL(dti_idx));

	if (last_frm_idx)
		lan966x_afi_frm_gone_wait(lan966x, dti_idx, dti->port_no,
					  last_frm_idx, true);

	return 0;
}

static int lan966x_afi_dti_seq_cnt_get(struct afi_control *afi, u32 dti_idx,
				       u32 *seq_cnt)
{
	struct lan966x *lan966x = afi->priv;

	*seq_cnt = AFI_DTI_CNT_CNT_GET(lan_rd(lan966x, AFI_DTI_CNT(dti_idx)));

	return 0;
}

struct afi_consts lan966x_afi_consts = {
	.frm_cnt = LAN966X_AFI_FRM_CNT,
	.slow_inj_cnt = LAN966X_AFI_SLOW_INJ_CNT,
	.dti_cnt = LAN966X_AFI_DTI_CNT,
	.prio_super = LAN966X_PRIO_SUPER,
	.fast_inj_bps_min = LAN966X_AFI_FAST_INJ_BPS_MIN,
	.fast_inj_bps_max = LAN966X_AFI_FAST_INJ_BPS_MAX,
};

struct afi_operations lan966x_afi_operations = {
	.afi_enable = lan966x_afi_afi_enable,
	.ttis_enable = lan966x_afi_ttis_enable,
	.tick_init = lan966x_afi_tick_init,
	.tti_frm_hijack = lan966x_afi_tti_frm_hijack,
	.tti_frm_rm_inj = lan966x_afi_tti_frm_rm_inj,
	.tti_start = lan966x_afi_tti_start,
	.tti_stop = lan966x_afi_tti_stop,
	.frm_inject = lan966x_afi_frm_inject,
	.dti_frm_hijack = lan966x_afi_dti_frm_hijack,
	.dti_frm_rm_inj = lan966x_afi_dti_frm_rm_inj,
	.dti_start = lan966x_afi_dti_start,
	.dti_stop = lan966x_afi_dti_stop,
	.dti_seq_cnt_get = lan966x_afi_dti_seq_cnt_get,
};

static int lan966x_afi_nl_port_of(void *priv, struct net_device *ndev)
{
	struct lan966x *lan966x = priv;
	struct lan966x_port *port;

	if (!lan966x_netdevice_check(ndev))
		return -ENODEV;

	port = netdev_priv(ndev);
	if (port->lan966x != lan966x)
		return -ENODEV;

	return port->chip_port;
}

static const struct afi_nl_ops lan966x_afi_nl_ops = {
	.port_of = lan966x_afi_nl_port_of,
};

int lan966x_afi_init(struct lan966x *lan966x)
{
	struct afi_control *afi_ctrl;
	int ret;

	afi_ctrl = kzalloc(sizeof(*afi_ctrl), GFP_KERNEL);
	if (!afi_ctrl)
		return -ENOMEM;

	afi_ctrl->ops = &lan966x_afi_operations;
	afi_ctrl->consts = &lan966x_afi_consts;
	afi_ctrl->priv = lan966x;
	lan966x->afi_ctrl = afi_ctrl;

	ret = afi_init(afi_ctrl);
	if (ret)
		goto err_free;

	ret = afi_netlink_register(afi_ctrl, lan966x, &lan966x_afi_nl_ops);
	if (ret)
		goto err_deinit;

	return 0;

err_deinit:
	afi_deinit(afi_ctrl);
err_free:
	kfree(afi_ctrl);
	lan966x->afi_ctrl = NULL;
	return ret;
}

void lan966x_afi_deinit(struct lan966x *lan966x)
{
	afi_netlink_unregister(lan966x->afi_ctrl);

	afi_deinit(lan966x->afi_ctrl);

	kfree(lan966x->afi_ctrl);
}
