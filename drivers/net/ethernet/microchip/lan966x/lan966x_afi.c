// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/iopoll.h>
#include <linux/bitfield.h>

#include "lan966x_main.h"
#include "lan966x_afi.h"

#define LAN966X_PRIO_SUPER			8

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

#define LAN966X_AFI_TTI_TICK_LEN0_US        52 /* 52us   */
#define LAN966X_AFI_TTI_TICK_LEN1_US       416 /* 416us  */
#define LAN966X_AFI_TTI_TICK_LEN2_US      3333 /* 3.333ms.*/
#define LAN966X_AFI_TTI_TICK_LEN3_US     10000 /* 10ms   */
#define LAN966X_AFI_TTI_TICK_LEN4_US    100000 /* 100ms  */
#define LAN966X_AFI_TTI_TICK_LEN5_US   1000000 /* 1s     */
#define LAN966X_AFI_TTI_TICK_LEN6_US  10000000 /* 10s    */
#define LAN966X_AFI_TTI_TICK_LEN7_US  60000000 /* 1min   */

#define LAN966X_WAIT_AFI_SLEEP_US		10
#define LAN966X_WAIT_AFI_TIMEOUT_US		1000000

struct lan966x_afi_qu_ref {
	u32 chip_port;
	u32 qu_num;
};

static void lan966x_afi_port_prio_2_qu_ref(struct lan966x *lan966x,
					   u32 port_no, u32 prio,
					   struct lan966x_afi_qu_ref *qu_ref)
{
	qu_ref->chip_port = port_no;
	qu_ref->qu_num = port_no * 64;
}

static bool lan966x_afi_tti_cal_init(struct lan966x *lan966x)
{
	u32  max_poll_cnt = 5;
	bool tti_init = true;
	u32  val;

	lan_rmw(AFI_TTI_CTRL_TTI_INIT_SET(1),
		AFI_TTI_CTRL_TTI_INIT,
		lan966x, AFI_TTI_CTRL);

	/* Wait for device to clear TTI_INIT */
	while (max_poll_cnt-- > 0) {
		val = lan_rd(lan966x, AFI_TTI_CTRL);
		tti_init = AFI_TTI_CTRL_TTI_INIT_GET(val);
		if (tti_init == 0)
			break;
	}

	if (tti_init == 1)
		return false;

	return true;
}

static bool lan966x_afi_enable(struct lan966x *lan966x)
{
	/* Enable AFI first */
	lan_rmw(AFI_MISC_CTRL_AFI_ENA_SET(1),
		AFI_MISC_CTRL_AFI_ENA,
		lan966x, AFI_MISC_CTRL);

	if (!lan966x_afi_tti_cal_init(lan966x))
		return false;

	return true;
}

static void lan966x_ttis_enable(struct lan966x *lan966x)
{
	/* Enable */
	lan_rmw(AFI_TTI_CTRL_TTI_ENA_SET(1),
		AFI_TTI_CTRL_TTI_ENA,
		lan966x, AFI_TTI_CTRL);
}

static void lan966x_afi_tti_tick_init(struct lan966x *lan966x)
{
	u64 tick_base_len, val0, val1, idx;
	u64 t_ps[8], tick_base_ps;
	u64 tmp;

	lan966x->afi.clk_period_ps = lan966x_ptp_get_period_ps();

	tick_base_len = LAN966X_AFI_TTI_TICK_LEN0_US * 1000000LLU;
	do_div(tick_base_len, lan966x->afi.clk_period_ps);
	lan_rmw(AFI_TTI_TICK_BASE_BASE_LEN_SET(tick_base_len),
		AFI_TTI_TICK_BASE_BASE_LEN,
		lan966x, AFI_TTI_TICK_BASE);

	tick_base_ps = tick_base_len * lan966x->afi.clk_period_ps;

	/* Configure tick lengths */
	tmp = LAN966X_AFI_TTI_TICK_LEN0_US * 1000000LLU;
	do_div(tmp, tick_base_ps);

	lan_wr(AFI_TTI_TICK_LEN_0_3_LEN0_SET(tmp) |
	       AFI_TTI_TICK_LEN_0_3_LEN1_SET(LAN966X_AFI_TTI_TICK_LEN1_US /
					     LAN966X_AFI_TTI_TICK_LEN0_US) |
	       AFI_TTI_TICK_LEN_0_3_LEN2_SET(LAN966X_AFI_TTI_TICK_LEN2_US /
					     LAN966X_AFI_TTI_TICK_LEN1_US) |
	       AFI_TTI_TICK_LEN_0_3_LEN3_SET(LAN966X_AFI_TTI_TICK_LEN3_US /
					     LAN966X_AFI_TTI_TICK_LEN2_US),
	       lan966x, AFI_TTI_TICK_LEN_0_3);

	lan_wr(AFI_TTI_TICK_LEN_4_7_LEN4_SET(LAN966X_AFI_TTI_TICK_LEN4_US /
					     LAN966X_AFI_TTI_TICK_LEN3_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN5_SET(LAN966X_AFI_TTI_TICK_LEN5_US /
					     LAN966X_AFI_TTI_TICK_LEN4_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN6_SET(LAN966X_AFI_TTI_TICK_LEN6_US /
					     LAN966X_AFI_TTI_TICK_LEN5_US) |
	       AFI_TTI_TICK_LEN_4_7_LEN7_SET(LAN966X_AFI_TTI_TICK_LEN7_US /
					     LAN966X_AFI_TTI_TICK_LEN6_US),
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

	for (idx = 0; idx < ARRAY_SIZE(lan966x->afi.tick_len_us); idx++) {
		tmp = t_ps[idx];
		do_div(tmp, 1000000LLU);
		lan966x->afi.tick_len_us[idx] = tmp;
	}

	for (idx = 0; idx < ARRAY_SIZE(lan966x->afi.tti_tbl); idx++)
		lan_rmw(AFI_TTI_TIMER_TIMER_ENA_SET(0),
			AFI_TTI_TIMER_TIMER_ENA, lan966x,
			AFI_TTI_TIMER(idx));
}

static bool lan966x_afi_res_is_free(u32 *alloc_table, u32 res_idx)
{
	u32 word_idx = res_idx / 32;
	u8  bit_idx  = res_idx - word_idx * 32;

	if ((alloc_table[word_idx] & (1 << bit_idx)) == 0) {
		/* Not allocated */
		return true;
	} else {
		return false;
	}
}

static bool lan966x_afi_res_alloc(struct lan966x *lan966x, u32 *alloc_table,
				  u32 res_cnt, u32 *alloced_res_idx,
				  u32 min_res_idx, u32 max_res_idx,
				  bool rand_mode)
{
	u32 res_idx, start_res_idx;

	if (!(min_res_idx < res_cnt && max_res_idx < res_cnt))
		return false;

	if (rand_mode) {
		/* Randomize resource allocation.
		 * This is intended for TTI allocation, where spreading the
		 * allocated TTIs throughout the TTI table will help reduce
		 * burstiness for many real-life configurations.
		 */
		start_res_idx = min_res_idx + get_random_u32() %
			(max_res_idx - min_res_idx + 1);
	} else {
		start_res_idx = min_res_idx;
	}

	res_idx = start_res_idx;

	do {
		if (lan966x_afi_res_is_free(alloc_table, res_idx)) {
			u32 word_idx = res_idx / 32;
			u8  bit_idx  = res_idx - word_idx * 32;

			alloc_table[word_idx] |= (1 << bit_idx);
			*alloced_res_idx = res_idx;
			return true;
		}

		if (++res_idx > max_res_idx)
			res_idx = min_res_idx;

	} while (res_idx != start_res_idx);

	/* Out of resources */
	return false;
}

static bool lan966x_afi_res_free(struct lan966x *lan966x, u32 *alloc_table,
				 u32 res_idx)
{
	u32 word_idx = res_idx / 32;
	u8 bit_idx = res_idx - word_idx * 32;
	bool res = true;

	if (lan966x_afi_res_is_free(alloc_table, res_idx))
		/* Not alloced! */
		res = false;

	alloc_table[word_idx] &= ~(u32)(1 << bit_idx);

	return res;
}

static bool lan966x_afi_frm_idx_chk(struct lan966x *lan966x, s32 frm_idx)
{
	if (frm_idx < 0 || frm_idx >= LAN966X_AFI_FRM_CNT)
		return false;

	if (lan966x_afi_res_is_free(lan966x->afi.frms_alloced, frm_idx))
		return false;

	return true;
}

static bool lan966x_afi_frm_set_rm(struct lan966x *lan966x, s32 frm_idx)
{
	u32 frm_tbl_part1;
	u32 val;

	lan966x_afi_frm_idx_chk(lan966x, frm_idx);

	frm_tbl_part1 = lan_rd(lan966x, AFI_FRM_ENTRY_PART1(frm_idx));

	val = FIELD_GET(LAN966X_AFI_FRM_TBL_PART1_RM_MASK, frm_tbl_part1);
	if (val) {
		pr_info("frm_rm already set\n");
		return false;
	}

	val = FIELD_GET(LAN966X_AFI_FRM_TBL_PART1_GONE_MASK, frm_tbl_part1);
	if (val) {
		pr_info("frm_gone already set\n");
		return false;
	}

	frm_tbl_part1 |= (1 << LAN966X_AFI_FRM_TBL_PART1_RM_POS);
	lan_wr(frm_tbl_part1, lan966x, AFI_FRM_ENTRY_PART1(frm_idx));

	return true;
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

	poll_cnt_max = (LAN966X_AFI_SLOW_INJ_CNT * 4) / 50;

	/* Poll for FRM_GONE == 1 for last frame */
	poll_cnt = 0;
	while (!frm_gone && poll_cnt++ < poll_cnt_max)
		lan966x_afi_frm_gone_get(lan966x, &frm_gone, frm_idx);

	return frm_gone != 0;
}

static inline int lan966x_afi_frm_get_ctrl(struct lan966x *lan966x)
{
	return lan_rd(lan966x, AFI_NEW_FRM_CTRL);
}

static bool lan966x_afi_frm_hijack(struct lan966x *lan966x, s32 frm_idx)
{
	struct lan966x_afi_frm *frm = &lan966x->afi.frm_tbl[frm_idx];
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
		return false;

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

	return true;
}

static void lan966x_afi_frm_init(struct lan966x_afi_frm *frm)
{
	memset(frm, 0, sizeof(*frm));
}

static bool lan966x_afi_frm_alloc(struct lan966x *lan966x, s32 *frm_idx,
				  s32 min_frm_idx, u8 entry_type,
				  s32 prev_frm_tbl_idx)
{
	struct lan966x_afi_frm *entry;

	if (!lan966x_afi_res_alloc(lan966x, lan966x->afi.frms_alloced,
				   LAN966X_AFI_FRM_CNT, (u32 *)frm_idx,
				   min_frm_idx, LAN966X_AFI_FRM_CNT - 1, false))
		return false;

	entry = &lan966x->afi.frm_tbl[*frm_idx];
	lan966x_afi_frm_init(entry);

	/* It's either a frame entry (0) or a delay entry (1) */
	entry->entry_type = entry_type;

	/* Link the previous entry to this one */
	if (prev_frm_tbl_idx >= 0)
		lan966x->afi.frm_tbl[prev_frm_tbl_idx].next_ptr = *frm_idx;

	return true;
}

static bool lan966x_afi_frm_free(struct lan966x *lan966x, s32 frm_idx)
{
	if (frm_idx >= LAN966X_AFI_FRM_CNT)
		return false;

	memset(&lan966x->afi.frm_tbl[frm_idx], 0,
	       sizeof(lan966x->afi.frm_tbl[frm_idx]));

	return lan966x_afi_res_free(lan966x, lan966x->afi.frms_alloced,
				    frm_idx);
}

static void lan966x_afi_tti_init(struct lan966x_afi_tti *tti)
{
	memset(tti, 0, sizeof(struct lan966x_afi_tti));

	/* frm_idx == -1 <=> No FRM allocated. */
	tti->frm_idx = -1;

	/* Not started yet */
	tti->paused  = 1;
}

static bool lan966x_afi_tti_free(struct lan966x *lan966x, u32 tti_idx)
{
	if (tti_idx >= LAN966X_AFI_SLOW_INJ_CNT) {
		pr_info("tti_idx=%u > %u", tti_idx, LAN966X_AFI_SLOW_INJ_CNT);
		return false;
	}

	/* Clear state before sending it back to free pool. */
	lan966x_afi_tti_init(&lan966x->afi.tti_tbl[tti_idx]);

	return lan966x_afi_res_free(lan966x, lan966x->afi.ttis_alloced,
				    tti_idx);
}

static bool lan966x_afi_tti_alloc(struct lan966x *lan966x, u32 *tti_idx,
				  u32 min_tti_idx, u32 max_tti_idx)
{
	if (!lan966x_afi_res_alloc(lan966x, lan966x->afi.ttis_alloced,
				   LAN966X_AFI_SLOW_INJ_CNT, tti_idx,
				   min_tti_idx, max_tti_idx, false)) {
		pr_info("Out of TTIs");
		return false;
	}

	lan966x_afi_tti_init(&lan966x->afi.tti_tbl[*tti_idx]);

	return true;
}

static bool lan966x_afi_tti_idx_chk(struct lan966x *lan966x, u32 tti_idx)
{
	if (tti_idx >= LAN966X_AFI_SLOW_INJ_CNT) {
		pr_info("tti_idx == %u illegal", tti_idx);
		return false;
	}

	if (lan966x_afi_res_is_free(lan966x->afi.ttis_alloced, tti_idx)) {
		pr_info("tti_idx == %u not alloced", tti_idx);
		return false;
	}

	return true;
}

static bool lan966x_afi_tti_frm_rm_inj(struct lan966x *lan966x, u32 tti_idx)
{
	struct lan966x_afi_tti *tti = &lan966x->afi.tti_tbl[tti_idx];

	if (tti->state != LAN966X_AFI_ENTRY_STATE_STOPPED) {
		pr_info("ID = %u: Injection must be stopped before rm injection",
			tti_idx);
		return false;
	}

	lan966x_afi_frm_set_rm(lan966x, tti->frm_idx);

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

	return true;
}

static bool lan966x_afi_tti_frm_hijack(struct lan966x *lan966x, u32 tti_idx)
{
	return lan966x_afi_frm_hijack(lan966x,
				      lan966x->afi.tti_tbl[tti_idx].frm_idx);
}

static void lan966x_afi_tti_pause_resume(struct lan966x *lan966x,
					 u32 tti_idx, bool pause)
{
	struct lan966x_afi_tti *tti = &lan966x->afi.tti_tbl[tti_idx];

	lan_rmw(AFI_TTI_TIMER_TIMER_ENA_SET(pause ? 0 : 1),
		AFI_TTI_TIMER_TIMER_ENA,
		lan966x, AFI_TTI_TIMER(tti_idx));

	tti->paused = pause;
}

static bool lan966x_afi_tti_stop(struct lan966x *lan966x, u32 tti_idx)
{
	struct lan966x_afi_tti *tti = &lan966x->afi.tti_tbl[tti_idx];

	if (tti->state != LAN966X_AFI_ENTRY_STATE_STARTED) {
		pr_info("TTI not started: %d\n", tti_idx);
		return false;
	}

	lan966x_afi_tti_pause_resume(lan966x, tti_idx, 1);

	tti->state = LAN966X_AFI_ENTRY_STATE_STOPPED;

	return true;
}

static void lan966x_afi_tti_qu_ref_update(struct lan966x *lan966x, u32 tti_idx)
{
	struct lan966x_afi_tti *tti = &lan966x->afi.tti_tbl[tti_idx];
	struct lan966x_afi_qu_ref qu_ref;

	lan966x_afi_port_prio_2_qu_ref(lan966x, tti->port_no, tti->prio,
				       &qu_ref);

	lan_wr(AFI_TTI_PORT_QU_PORT_NUM_SET(qu_ref.chip_port) |
	       AFI_TTI_PORT_QU_QU_NUM_SET(qu_ref.qu_num),
	       lan966x, AFI_TTI_PORT_QU(tti_idx));
}

static bool lan966x_afi_tti_start(struct lan966x *lan966x, u32 tti_idx,
				  bool do_config)
{
	struct lan966x_afi_tti *tti = &lan966x->afi.tti_tbl[tti_idx];
	u32 rand_tick_cnt;

	if (tti->state != LAN966X_AFI_ENTRY_STATE_STOPPED) {
		pr_info("TTI already started");
		return false;
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

	tti->state = LAN966X_AFI_ENTRY_STATE_STARTED;

	return true;
}

bool lan966x_afi_slow_inj_alloc(struct net_device *dev,
				struct lan966x_afi_slow_inj_alloc_cfg *cfg,
				u32 *slowid)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_afi_tti *tti;
	u32 tti_idx;
	s32 frm_idx;

	/* Argument checks */
	if (!cfg || !slowid) {
		pr_info("cfg or slowid is NULL");
		return false;
	}

	*slowid = 0;

	if (cfg->prio > LAN966X_PRIO_SUPER + 1) {
		pr_info("Illegal prio (%u)", cfg->prio);
		return false;
	}

	// On first alloc, enable AFI and TTIs (if not already done)
	if (!lan966x->afi.afi_ena) {
		lan966x_afi_tti_tick_init(lan966x);

		if (!lan966x_afi_enable(lan966x))
			return false;

		lan966x->afi.afi_ena = 1;
	}

	if (!lan966x->afi.tti_ena) {
		lan966x_ttis_enable(lan966x);

		lan966x->afi.tti_ena = 1;
	}

	/* Allocate a TTI */
	if (!lan966x_afi_tti_alloc(lan966x, &tti_idx, 0,
				   LAN966X_AFI_SLOW_INJ_CNT - 1)) {
		pr_info("Couldn't allocate tti\n");
		return false;
	}

	*slowid = tti_idx;
	tti = &lan966x->afi.tti_tbl[tti_idx];

	/* Allocate a FRM */
	if (!lan966x_afi_frm_alloc(lan966x, &frm_idx, 0, 0, -1)) {
		(void)lan966x_afi_tti_free(lan966x, tti_idx);
		return false;
	}

	tti->state = LAN966X_AFI_ENTRY_STATE_STOPPED;
	tti->frm_idx = frm_idx;
	tti->port_no = cfg->port_no;
	tti->prio = cfg->prio;

	return true;
}

bool lan966x_afi_slow_inj_free(struct net_device *dev, u32 slowid)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_afi_tti *tti;

	if (!lan966x_afi_tti_idx_chk(lan966x, slowid))
		return false;

	tti = &lan966x->afi.tti_tbl[slowid];

	if (tti->state != LAN966X_AFI_ENTRY_STATE_STOPPED) {
		pr_info("Injection must be stopped before freeing");
		return false;
	}

	/* Inject frame for removal - if any */
	if (tti->hijacked) {
		if (!lan966x_afi_tti_frm_rm_inj(lan966x, slowid)) {
			pr_info("Can't remove tti frm inj");
			return false;
		}
	}

	// Free resources
	if (!lan966x_afi_frm_free(lan966x,
				  lan966x->afi.tti_tbl[slowid].frm_idx))
		return false;

	if (!lan966x_afi_tti_free(lan966x, slowid))
		return false;

	return true;
}

bool lan966x_afi_slow_inj_frm_hijack(struct net_device *dev, u32 slowid)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;

	if (!lan966x_afi_tti_idx_chk(lan966x, slowid))
		return false;

	if (lan966x_afi_tti_frm_hijack(lan966x, slowid))
		// Frame is now transferred to H/W
		lan966x->afi.tti_tbl[slowid].hijacked = true;
	else
		return false;

	return true;
}

static u32 lan966x_afi_div_round32(u32 dividend, u32 divisor)
{
	return ((dividend + (divisor / 2)) / divisor);
}

static bool lan966x_afi_timer_prec_ok(u32 timer_len_us_requested,
				      u32 timer_len_us_actual, u32 prec_pct)
{
	bool result;
	u32 abs_diff  = timer_len_us_requested > timer_len_us_actual ?
		timer_len_us_requested - timer_len_us_actual :
		timer_len_us_actual - timer_len_us_actual;
	u64 alwd_diff = ((u64)prec_pct * timer_len_us_requested);
	do_div(alwd_diff, 100LLU);

	result = abs_diff <= alwd_diff;

	return result;
}

bool lan966x_afi_slow_inj_start(struct net_device *dev, u32 slowid,
				struct lan966x_afi_slow_inj_start_cfg *cfg)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	u64 timer_len_us, timer_len_ticks;
	struct lan966x_afi_tti *tti;
	bool tick_found = 0;
	bool do_config = 0;
	int tick_idx;

	/* Argument checking */
	if (!cfg) {
		pr_info("cfg is NULL");
		return false;
	}

	if (!lan966x_afi_tti_idx_chk(lan966x, slowid))
		return false;

	if (cfg->fph == 0) {
		pr_info("cfg->fph == 0");
		return false;
	}

	tti = &lan966x->afi.tti_tbl[slowid];

	if (tti->state != LAN966X_AFI_ENTRY_STATE_STOPPED) {
		pr_info("TTI already started");
		return false;
	}

	timer_len_us = (3600LLU * 1000000LLU);
	do_div(timer_len_us, cfg->fph);

	if (tti->start_cfg.fph == cfg->fph) {
		do_config = 0;
		goto start_tti;
	}

	tti->start_cfg.fph = cfg->fph;

	/* Choose slowest possible tick resulting in timer_len_ticks >= 8.
	 * This reduces the frequency with which TICK_CNT shall be
	 * decremented (thus making the walk-through of TTI_TBL as fast
	 * as possible) while ensuring some room for randomization of
	 * time to first injection.
	 */

	for (tick_idx = 7; tick_idx >= 0; tick_idx--) {
		u32 tick_len_us = lan966x->afi.tick_len_us[tick_idx];
		bool timer_prec_ok;

		timer_len_ticks = lan966x_afi_div_round32(timer_len_us,
							  tick_len_us);

		/* Check that resulting timer is correct within 5%
		 * If not within 5% then a faster tick must be used.
		 */
		timer_prec_ok = lan966x_afi_timer_prec_ok(timer_len_us,
							  timer_len_ticks *
							  tick_len_us, 5);

		if (timer_len_ticks >= 8 && timer_prec_ok) {
			lan966x->afi.tti_tbl[slowid].timer_len = timer_len_ticks;
			lan966x->afi.tti_tbl[slowid].tick_idx  = tick_idx;
			tick_found = 1;
			break;
		}
	}

	if (!tick_found) {
		pr_info("No tick found for fph = %lld", cfg->fph);
		return false;
	}
	do_config = 1;

start_tti:
	return lan966x_afi_tti_start(lan966x, slowid, do_config);
}

bool lan966x_afi_slow_inj_stop(struct net_device *dev, u32 slowid)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_afi_tti *tti;

	if (!lan966x_afi_tti_idx_chk(lan966x, slowid))
		return false;

	tti = &lan966x->afi.tti_tbl[slowid];

	if (tti->state != LAN966X_AFI_ENTRY_STATE_STARTED) {
		pr_info("%s TTI not started %d", __FUNCTION__, slowid);
		return false;
	}

	return lan966x_afi_tti_stop(lan966x, slowid);
}
