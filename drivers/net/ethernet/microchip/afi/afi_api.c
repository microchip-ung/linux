// SPDX-License-Identifier: GPL-2.0+
/* Microchip AFI API
 *
 * Copyright (c) 2024 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/export.h>
#include <linux/math64.h>
#include <linux/units.h>

#include "afi_api.h"

/* Minimum delay (in system clock cycles) between two frame injections; the
 * AFI needs this long to process a frame/delay entry pair.
 */
#define AFI_DELAY_CC_MIN		12

/* Max value of DTI_MODE.TRAILING_DELAY_SEQ_CNT, also reused as the max
 * INJ_CNT considered when fine-tuning a single-frame flow's rate.
 */
#define AFI_TRAILING_DELAY_SEQ_CNT_MAX	63

/* Width of the DELAY field in FRM_ENTRY_PART0. 30 bits on all currently
 * supported chips; promote to afi_consts if a future chip differs.
 */
#define AFI_FRM_DELAY_WID		30

/* DTIs at or above this rate get arbitration precedence (DTI_CTRL.BW). Not
 * critical on low-port-speed chips.
 */
#define AFI_DTI_BW1_THRES_BPS		5000000000ULL

/* Fixed-point scale for the delay sum and the bits/s result: delays are
 * accumulated as clock cycles * AFI_FP_SCALE and the final result is rounded
 * (+ AFI_FP_SCALE / 2) and divided back down by AFI_FP_SCALE. The rate numerator
 * is PICO * AFI_FP_SCALE because clk_period_ps is in picoseconds (PICO ps == 1s).
 */
#define AFI_FP_SCALE			1000000ULL

static bool afi_res_is_free(u32 *alloc_table, u32 res_idx)
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

static int afi_res_alloc(u32 *alloc_table,
			 u32 res_cnt, u32 *alloced_res_idx,
			 u32 min_res_idx, u32 max_res_idx,
			 bool rand_mode)
{
	u32 res_idx, start_res_idx;

	if (!(min_res_idx < res_cnt && max_res_idx < res_cnt))
		return -EINVAL;

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
		if (afi_res_is_free(alloc_table, res_idx)) {
			u32 word_idx = res_idx / 32;
			u8  bit_idx  = res_idx - word_idx * 32;

			alloc_table[word_idx] |= (1 << bit_idx);
			*alloced_res_idx = res_idx;
			return 0;
		}

		if (++res_idx > max_res_idx)
			res_idx = min_res_idx;

	} while (res_idx != start_res_idx);

	/* Out of resources */
	return -ENOMEM;
}

static int afi_res_free(u32 *alloc_table, u32 res_idx)
{
	u32 word_idx = res_idx / 32;
	u8 bit_idx = res_idx - word_idx * 32;
	int ret = 0;

	if (afi_res_is_free(alloc_table, res_idx))
		/* Not alloced! */
		ret = -EINVAL;

	alloc_table[word_idx] &= ~(u32)(1 << bit_idx);

	return ret;
}

static void afi_tti_init(struct afi_tti *tti)
{
	memset(tti, 0, sizeof(struct afi_tti));

	/* frm_idx == -1 <=> No FRM allocated. */
	tti->frm_idx = -1;

	/* Not started yet */
	tti->paused  = 1;
}

static int afi_tti_alloc(struct afi_control *afi, u32 *tti_idx,
			 u32 min_tti_idx, u32 max_tti_idx)
{
	int ret;

	ret = afi_res_alloc(afi->ttis_alloced, afi->consts->slow_inj_cnt,
			    tti_idx, min_tti_idx, max_tti_idx, false);
	if (ret)
		return ret;

	afi_tti_init(&afi->tti_tbl[*tti_idx]);

	return 0;
}

static int afi_tti_free(struct afi_control *afi, u32 tti_idx)
{
	if (tti_idx >= afi->consts->slow_inj_cnt)
		return -EINVAL;

	/* Clear state before sending it back to free pool. */
	afi_tti_init(&afi->tti_tbl[tti_idx]);

	return afi_res_free(afi->ttis_alloced, tti_idx);
}

static bool afi_tti_idx_chk(struct afi_control *afi, u32 tti_idx)
{
	if (tti_idx >= afi->consts->slow_inj_cnt)
		return false;

	if (afi_res_is_free(afi->ttis_alloced, tti_idx))
		return false;

	return true;
}

static void afi_frm_init(struct afi_frm *frm)
{
	memset(frm, 0, sizeof(*frm));
}

static int afi_frm_alloc(struct afi_control *afi, s32 *frm_idx,
			 s32 min_frm_idx, u8 entry_type, s32 prev_frm_tbl_idx)
{
	struct afi_frm *entry;
	int ret;

	ret = afi_res_alloc(afi->frms_alloced, afi->consts->frm_cnt,
			    (u32 *)frm_idx, min_frm_idx,
			    afi->consts->frm_cnt - 1, false);
	if (ret)
		return ret;

	entry = &afi->frm_tbl[*frm_idx];
	afi_frm_init(entry);

	/* It's either a frame entry (0) or a delay entry (1) */
	entry->entry_type = entry_type;

	/* Link the previous entry to this one */
	if (prev_frm_tbl_idx >= 0)
		afi->frm_tbl[prev_frm_tbl_idx].next_idx = *frm_idx;

	return 0;
}

static int afi_frm_free(struct afi_control *afi, s32 frm_idx)
{
	if (frm_idx >= afi->consts->frm_cnt)
		return false;

	memset(&afi->frm_tbl[frm_idx], 0, sizeof(afi->frm_tbl[frm_idx]));

	return afi_res_free(afi->frms_alloced, frm_idx);
}

bool afi_frm_idx_chk(struct afi_control *afi, s32 frm_idx)
{
	u32 *frms_alloced = afi->frms_alloced;

	if (frm_idx < 0 || frm_idx >= afi->consts->frm_cnt)
		return false;

	if (afi_res_is_free(frms_alloced, frm_idx))
		return false;

	return true;
}
EXPORT_SYMBOL_GPL(afi_frm_idx_chk);

int afi_init(struct afi_control *afi)
{
	afi->frms_alloced = kcalloc((afi->consts->frm_cnt + 31) / 32, sizeof(u32), GFP_KERNEL);
	afi->ttis_alloced = kcalloc((afi->consts->slow_inj_cnt + 31) / 32, sizeof(u32), GFP_KERNEL);
	afi->dtis_alloced = kcalloc((afi->consts->dti_cnt + 31) / 32, sizeof(u32), GFP_KERNEL);
	afi->frm_tbl = kcalloc(afi->consts->frm_cnt, sizeof(struct afi_frm), GFP_KERNEL);
	afi->tti_tbl = kcalloc(afi->consts->slow_inj_cnt, sizeof(struct afi_tti), GFP_KERNEL);
	afi->dti_tbl = kcalloc(afi->consts->dti_cnt, sizeof(struct afi_dti), GFP_KERNEL);

	if (!afi->frms_alloced ||
	    !afi->ttis_alloced ||
	    !afi->dtis_alloced ||
	    !afi->frm_tbl ||
	    !afi->tti_tbl ||
	    !afi->dti_tbl)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL_GPL(afi_init);

void afi_deinit(struct afi_control *afi)
{
	kfree(afi->dti_tbl);
	kfree(afi->tti_tbl);
	kfree(afi->frm_tbl);
	kfree(afi->dtis_alloced);
	kfree(afi->ttis_alloced);
	kfree(afi->frms_alloced);
}
EXPORT_SYMBOL_GPL(afi_deinit);

int afi_slow_inj_alloc(struct afi_control *afi,
		       struct afi_slow_inj_alloc_cfg *cfg,
		       u32 *slowid)
{
	struct afi_tti *tti;
	u32 tti_idx;
	s32 frm_idx;
	int ret;

	/* Argument checks */
	if (!cfg || !slowid)
		return -EINVAL;

	*slowid = 0;

	if (cfg->prio > afi->consts->prio_super + 1)
		return -EINVAL;

	// On first alloc, enable AFI and TTIs (if not already done)
	if (!afi->afi_ena) {
		afi->ops->tick_init(afi);

		ret = afi->ops->afi_enable(afi);
		if (ret)
			return ret;

		afi->afi_ena = 1;
	}

	if (!afi->tti_ena) {
		ret = afi->ops->ttis_enable(afi);
		if (ret)
			return ret;

		afi->tti_ena = 1;
	}

	/* Allocate a TTI */
	ret = afi_tti_alloc(afi, &tti_idx, 0, afi->consts->slow_inj_cnt - 1);
	if (ret)
		return ret;

	*slowid = tti_idx;
	tti = &afi->tti_tbl[tti_idx];

	/* Allocate a FRM */
	ret = afi_frm_alloc(afi, &frm_idx, 0, 0, -1);
	if (ret) {
		afi_tti_free(afi, tti_idx);
		return ret;
	}

	tti->state = AFI_ENTRY_STATE_STOPPED;
	tti->frm_idx = frm_idx;
	tti->port_no = cfg->port_no;
	tti->prio = cfg->prio;

	return 0;
}
EXPORT_SYMBOL_GPL(afi_slow_inj_alloc);

int afi_slow_inj_free(struct afi_control *afi, u32 slowid)
{
	struct afi_tti *tti;
	int ret;

	if (!afi_tti_idx_chk(afi, slowid))
		return false;

	tti = &afi->tti_tbl[slowid];

	if (tti->state != AFI_ENTRY_STATE_STOPPED)
		return -EINVAL;

	/* Inject frame for removal - if any */
	if (tti->hijacked) {
		ret = afi->ops->tti_frm_rm_inj(afi, slowid);
		if (ret)
			return ret;
	}

	/* Free resources */
	ret = afi_frm_free(afi, afi->tti_tbl[slowid].frm_idx);
	if (ret)
		return ret;

	ret = afi_tti_free(afi, slowid);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(afi_slow_inj_free);

int afi_slow_inj_frm_hijack(struct afi_control *afi,
			    u32 slowid)
{
	int ret;

	if (!afi_tti_idx_chk(afi, slowid))
		return -EINVAL;

	ret = afi->ops->tti_frm_hijack(afi, slowid);
	if (ret)
		return ret;

	afi->tti_tbl[slowid].hijacked = true;

	return 0;
}
EXPORT_SYMBOL_GPL(afi_slow_inj_frm_hijack);

static u32 afi_div_round32(u32 dividend, u32 divisor)
{
	return ((dividend + (divisor / 2)) / divisor);
}

static bool afi_timer_prec_ok(u32 timer_len_us_requested,
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

bool afi_slow_inj_started(struct afi_control *afi,
			  u32 slowid)
{
	struct afi_tti *tti;

	if (!afi_tti_idx_chk(afi, slowid))
		return false;

	tti = &afi->tti_tbl[slowid];

	return tti->state == AFI_ENTRY_STATE_STARTED;
}

int afi_slow_inj_start(struct afi_control *afi,
		       u32 slowid,
		       struct afi_slow_inj_start_cfg *cfg)
{
	u64 timer_len_us, timer_len_ticks;
	bool tick_found = false;
	bool do_config = false;
	struct afi_tti *tti;
	int tick_idx;

	/* Argument checking */
	if (!cfg)
		return -EINVAL;

	if (!afi_tti_idx_chk(afi, slowid))
		return -EINVAL;

	if (cfg->fph == 0)
		return -EINVAL;

	tti = &afi->tti_tbl[slowid];

	if (tti->state != AFI_ENTRY_STATE_STOPPED)
		return -EINVAL;

	timer_len_us = (3600LLU * 1000000LLU);
	do_div(timer_len_us, cfg->fph);

	if (tti->start_cfg.fph == cfg->fph) {
		do_config = false;
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
		u32 tick_len_us = afi->tick_len_us[tick_idx];
		bool timer_prec_ok;

		timer_len_ticks = afi_div_round32(timer_len_us, tick_len_us);

		/* Check that resulting timer is correct within 5%
		 * If not within 5% then a faster tick must be used.
		 */
		timer_prec_ok = afi_timer_prec_ok(timer_len_us,
						  timer_len_ticks * tick_len_us,
						  5);

		if (timer_len_ticks >= 8 && timer_prec_ok) {
			afi->tti_tbl[slowid].timer_len = timer_len_ticks;
			afi->tti_tbl[slowid].tick_idx  = tick_idx;
			tick_found = true;
			break;
		}
	}

	if (!tick_found)
		return -EINVAL;

	do_config = true;

start_tti:
	return afi->ops->tti_start(afi, slowid, do_config);
}
EXPORT_SYMBOL_GPL(afi_slow_inj_start);

int afi_slow_inj_stop(struct afi_control *afi,
		      u32 slowid)
{
	struct afi_tti *tti;

	if (!afi_tti_idx_chk(afi, slowid))
		return -EINVAL;

	tti = &afi->tti_tbl[slowid];

	if (tti->state != AFI_ENTRY_STATE_STARTED)
		return -EINVAL;

	return afi->ops->tti_stop(afi, slowid);
}
EXPORT_SYMBOL_GPL(afi_slow_inj_stop);

static void afi_dti_init(struct afi_dti *dti)
{
	memset(dti, 0, sizeof(*dti));

	/* Not started yet */
	dti->paused = true;
}

static int afi_dti_alloc(struct afi_control *afi, u32 *dti_idx)
{
	int ret;

	ret = afi_res_alloc(afi->dtis_alloced, afi->consts->dti_cnt, dti_idx,
			    0, afi->consts->dti_cnt - 1, false);
	if (ret)
		return ret;

	afi_dti_init(&afi->dti_tbl[*dti_idx]);

	return 0;
}

static int afi_dti_free(struct afi_control *afi, u32 dti_idx)
{
	if (dti_idx >= afi->consts->dti_cnt)
		return -EINVAL;

	/* Clear state before sending it back to free pool. */
	afi_dti_init(&afi->dti_tbl[dti_idx]);

	return afi_res_free(afi->dtis_alloced, dti_idx);
}

static bool afi_dti_idx_chk(struct afi_control *afi, u32 dti_idx)
{
	if (dti_idx >= afi->consts->dti_cnt)
		return false;

	if (afi_res_is_free(afi->dtis_alloced, dti_idx))
		return false;

	return true;
}

/* Free frame table entries belonging to a DTI flow. If delay_entries_only is
 * true, only delay entries are freed and the remaining frame entries are
 * re-linked into a sequence with inj_cnt reset to 1; otherwise both frame and
 * delay entries are freed.
 */
static int afi_dti_frm_free(struct afi_control *afi, struct afi_dti *dti,
			    bool delay_entries_only)
{
	struct afi_frm *frm_tbl = afi->frm_tbl;
	struct afi_frm *prev_frm_entry = NULL;
	u32 frm_idx;

	frm_idx = dti->first_frm_idx;
	dti->trailing_delay_seq_cnt = 0;

	while (frm_idx) {
		struct afi_frm *entry = &frm_tbl[frm_idx];
		u32 next_idx = entry->next_idx;

		if (delay_entries_only && entry->entry_type == 0) {
			/* Keep this frame entry in the list. */
			if (prev_frm_entry)
				prev_frm_entry->next_idx = frm_idx;

			prev_frm_entry = entry;
			entry->next_idx = 0;
			entry->inj_cnt = 1;
		} else {
			afi_frm_free(afi, frm_idx);
		}

		frm_idx = next_idx;
	}

	return 0;
}

/* Count entries in a DTI flow's frame table sequence: total entries, frame
 * entries (frm_cnt) and hijacked frame entries (those with frm_size set).
 */
static void afi_dti_cnt_get(struct afi_control *afi, u32 dti_idx,
			    u32 *total_cnt, u32 *frm_cnt, u32 *hijacked_cnt)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	struct afi_frm *frm_tbl = afi->frm_tbl;
	u32 frm_idx;

	*total_cnt = 0;
	*frm_cnt = 0;
	*hijacked_cnt = 0;

	for (frm_idx = dti->first_frm_idx; frm_idx != 0;
	     frm_idx = frm_tbl[frm_idx].next_idx) {
		(*total_cnt)++;

		if (frm_tbl[frm_idx].entry_type == 0) {
			(*frm_cnt)++;

			if (frm_tbl[frm_idx].frm_size)
				(*hijacked_cnt)++;
		}
	}
}

/* Allocate and link a delay entry into a DTI flow's sequence. */
static int afi_dti_delay_alloc(struct afi_control *afi, struct afi_dti *dti,
			       s32 *frm_idx, s32 prev_idx, u32 next_idx)
{
	int ret;

	ret = afi_frm_alloc(afi, frm_idx, 1, 1, prev_idx);
	if (ret) {
		/* Free delay entries, keep the frame entries. */
		afi_dti_frm_free(afi, dti, true);
		return ret;
	}

	afi->frm_tbl[*frm_idx].next_idx = next_idx;

	return 0;
}

/* Assign a delay to a delay entry, honoring the per-entry minimum and the
 * frame table delay field width. delay_xm is the requested delay in clock
 * cycles scaled by @scale; the remaining (unassigned) delay is returned, also
 * scaled by @scale. *saturated is set when the entry's delay field is maxed.
 */
static u64 afi_dti_delay_assign(u32 *delay_cc, u64 delay_xm, u64 scale,
				bool *saturated)
{
	const u32 delay_cc_max = (1U << AFI_FRM_DELAY_WID) - 1;
	u64 act_delay_xm, rem_xm, d;

	/* Truncate to whole clock cycles, but at least AFI_DELAY_CC_MIN. */
	d = max_t(u64, div64_u64(delay_xm, scale), AFI_DELAY_CC_MIN);

	/* Don't overflow the delay field in the frame table. */
	*delay_cc = min_t(u64, d, delay_cc_max);
	*saturated = (*delay_cc == delay_cc_max);

	act_delay_xm = (u64)*delay_cc * scale;

	/* A minimum of AFI_DELAY_CC_MIN may make act_delay_xm exceed the
	 * requested delay, in which case there is no remainder.
	 */
	rem_xm = act_delay_xm < delay_xm ? delay_xm - act_delay_xm : 0;

	return rem_xm;
}

/* Compute and allocate the delay entries needed to inject the flow at the
 * requested rate (cfg->bps).
 */
static int afi_dti_delay_calc_do(struct afi_control *afi, u32 dti_idx,
				 struct afi_fast_inj_start_cfg *cfg)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	u32 frame_indices[AFI_FAST_INJ_FRM_CNT_MAX];
	u32 delay_slot_cnt = 0, frm_idx, cnt = 0, i;
	s32 frm_tbl_delay_idx, last_frm_tbl_idx = 0;
	struct afi_frm *frm_tbl = afi->frm_tbl;
	struct afi_frm *last_delay_slot = NULL;
	bool last_delay_slot_saturated = false;
	u64 exs_delay_cc = 0, delay_xm = 0;
	struct afi_frm *last_frm = NULL;
	const u64 scale = 1000000ULL;
	bool add_to_last;
	int ret;

	/* Collect the frame entries of this flow in injection order. */
	memset(frame_indices, 0, sizeof(frame_indices));
	for (frm_idx = dti->first_frm_idx; frm_idx != 0;
	     frm_idx = frm_tbl[frm_idx].next_idx) {
		if (cnt >= ARRAY_SIZE(frame_indices))
			return -EINVAL;

		frame_indices[cnt++] = frm_idx;
	}

	if (cnt != dti->frm_cnt)
		return -EINVAL;

	/* 1) Assign a delay proportional to each frame's size, resulting in a
	 *    rate equal to or higher than requested. Variables suffixed "_xm"
	 *    are scaled by @scale to retain the fractional clock cycles, which
	 *    are folded into a trailing delay afterwards.
	 */
	for (i = 0; i < dti->frm_cnt; i++) {
		u32 factor1;
		u64 factor2;

		last_frm_tbl_idx = frame_indices[i];
		last_frm = &frm_tbl[last_frm_tbl_idx];
		last_frm->inj_cnt = 1;

		/* delay_xm = frm_size * 8 * (1e18 / bps) / clk_period_ps.
		 * Pull a factor of 10 out of the division for very low rates so
		 * the dividend cannot overflow 64 bits.
		 */
		factor1 = cfg->bps < 10000 ? 10 : 1;
		factor2 = div_u64(scale, factor1);

		delay_xm += factor1 *
			div64_u64((u64)last_frm->frm_size * 8ULL *
				  div64_u64(1000000000000ULL * factor2, cfg->bps),
				  afi->clk_period_ps);

		/* There must be a minimum delay between frames. */
		delay_xm = max_t(u64, delay_xm, (u64)AFI_DELAY_CC_MIN * scale);

		/* Keep allocating delay entries until only the residual (less
		 * than one clock cycle) is left.
		 */
		while (delay_xm >= scale) {
			delay_slot_cnt++;

			ret = afi_dti_delay_alloc(afi, dti, &frm_tbl_delay_idx,
						  last_frm_tbl_idx,
						  i < (u32)(dti->frm_cnt - 1) ?
						  frame_indices[i + 1] : 0);
			if (ret)
				return ret;

			last_frm_tbl_idx = frm_tbl_delay_idx;
			last_delay_slot = &frm_tbl[frm_tbl_delay_idx];

			delay_xm = afi_dti_delay_assign(&last_delay_slot->delay,
							delay_xm, scale,
							&last_delay_slot_saturated);
		}

		exs_delay_cc += last_delay_slot->delay - AFI_DELAY_CC_MIN;
	}

	/* delay_xm now holds the residual fraction to apply as a trailing
	 * delay (or to the last delay entry if that is not possible).
	 */
	if (delay_xm == 0)
		return 0;

	add_to_last = false;
	if (delay_slot_cnt != dti->frm_cnt) {
		/* More than one delay entry per frame: cannot use INJ_CNT or
		 * trailing-delay fine-tuning; add a clock cycle to the last
		 * delay entry instead.
		 */
		add_to_last = true;
	}

	if (dti->frm_cnt == 1 && !add_to_last) {
		/* Single frame: fine-tune by repeating the frame INJ_CNT times
		 * followed by one trailing delay.
		 */
		s32 carry_xm_diff_best = (s32)scale;
		u32 delay_best = 0, inj_cnt_best = 0;
		u32 inj_cnt, delay;

		for (inj_cnt = 1; inj_cnt <= AFI_TRAILING_DELAY_SEQ_CNT_MAX;
		     inj_cnt++) {
			for (delay = 1; delay <= inj_cnt; delay++) {
				s32 carry_xm_diff =
					(s32)(div_u64((u64)delay * scale,
						      inj_cnt) - delay_xm);

				/* Delay must exceed the carry, else the rate
				 * would end up above the requested rate.
				 */
				if (carry_xm_diff < 0)
					continue;

				/* Need enough excess delay to process the extra
				 * delay entry.
				 */
				if ((last_delay_slot->delay - AFI_DELAY_CC_MIN) *
				    inj_cnt < AFI_DELAY_CC_MIN)
					continue;

				if (carry_xm_diff < carry_xm_diff_best) {
					carry_xm_diff_best = carry_xm_diff;
					inj_cnt_best = inj_cnt;
					delay_best = delay;
				}
			}
		}

		if (delay_best != 0) {
			ret = afi_dti_delay_alloc(afi, dti, &frm_tbl_delay_idx,
						  last_frm_tbl_idx, 0);
			if (ret)
				return ret;

			last_frm->inj_cnt = inj_cnt_best;
			frm_tbl[frm_tbl_delay_idx].delay = delay_best;
		} else {
			add_to_last = true;
		}
	} else if (dti->frm_cnt > 1 && !add_to_last) {
		/* Multiple frames: fine-tune using the trailing delay, applied
		 * only every TRAILING_DELAY_SEQ_CNT'th sequence. Needs at least
		 * AFI_DELAY_CC_MIN cycles of excess delay to process it.
		 */
		if (exs_delay_cc >= AFI_DELAY_CC_MIN) {
			s32 carry_xm_diff_best = (s32)scale;
			u32 td_seq_cnt_best = 0, td_delay_best = 0;
			u32 tds, delay;

			for (tds = 1; tds <= AFI_TRAILING_DELAY_SEQ_CNT_MAX;
			     tds++) {
				for (delay = 1; delay <= tds; delay++) {
					s32 carry_xm_diff =
						(s32)(div_u64((u64)delay * scale,
							      tds) - delay_xm);

					if (carry_xm_diff < 0)
						continue;

					if (carry_xm_diff < carry_xm_diff_best) {
						carry_xm_diff_best = carry_xm_diff;
						td_seq_cnt_best = tds;
						td_delay_best = delay;
					}
				}
			}

			if (td_seq_cnt_best != 0) {
				ret = afi_dti_delay_alloc(afi, dti,
							  &frm_tbl_delay_idx,
							  last_frm_tbl_idx, 0);
				if (ret)
					return ret;

				dti->trailing_delay_seq_cnt = td_seq_cnt_best;
				frm_tbl[frm_tbl_delay_idx].delay = td_delay_best;
			} else {
				/* No trailing-delay split improved on the
				 * remaining carry; absorb the excess delay into
				 * the last delay slot instead of dropping it.
				 */
				add_to_last = true;
			}
		} else {
			add_to_last = true;
		}
	}

	if (add_to_last) {
		if (last_delay_slot_saturated) {
			/* No room in the last entry; allocate a new one. */
			ret = afi_dti_delay_alloc(afi, dti, &frm_tbl_delay_idx,
						  last_frm_tbl_idx, 0);
			if (ret)
				return ret;

			frm_tbl[frm_tbl_delay_idx].delay = AFI_DELAY_CC_MIN;
		} else {
			last_delay_slot->delay++;
		}
	}

	return 0;
}

/* Compute the actual rate (cfg->bps_actual) of the configured frame/delay
 * sequence. Different expressions are used depending on the magnitude of the
 * delay sum to avoid 64-bit overflow while keeping precision.
 */
static int afi_dti_bps_actual_calc(struct afi_control *afi, u32 dti_idx,
				   struct afi_fast_inj_start_cfg *cfg)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	struct afi_frm *frm_tbl = afi->frm_tbl;
	struct afi_frm *frm_prev = NULL;
	u32 frm_idx, frm_size_sum = 0;
	u64 delay_sum_xm = 0;

	frm_idx = dti->first_frm_idx;
	if (!afi_frm_idx_chk(afi, frm_idx))
		return -EINVAL;

	/* Sum the bytes and the delay (in clock cycles, scaled by AFI_FP_SCALE)
	 * over the whole sequence.
	 */
	while (frm_idx) {
		struct afi_frm *entry = &frm_tbl[frm_idx];

		if (entry->entry_type == 0) {
			frm_size_sum += entry->frm_size * entry->inj_cnt;
		} else if (frm_prev && frm_prev->entry_type == 0) {
			/* Delay after a frame is applied per injection. */
			delay_sum_xm += (u64)entry->delay *
					(u64)frm_prev->inj_cnt * AFI_FP_SCALE;
		} else if (entry->next_idx == 0 &&
			   dti->trailing_delay_seq_cnt > 0) {
			/* Trailing delay applied every Nth sequence. */
			delay_sum_xm += div64_u64((u64)entry->delay * AFI_FP_SCALE,
						  dti->trailing_delay_seq_cnt);
		} else {
			delay_sum_xm += (u64)entry->delay * AFI_FP_SCALE;
		}

		frm_prev = entry;
		frm_idx = entry->next_idx;
	}

	if (frm_size_sum == 0 || delay_sum_xm == 0)
		return -EINVAL;

	/* The thresholds below pick the division order that keeps the
	 * intermediates within 64 bits as delay_sum_xm grows.
	 */
	if (delay_sum_xm > PICO)
		cfg->bps_actual =
			div64_u64(div64_u64((u64)frm_size_sum * 8ULL *
					    div64_u64(PICO * AFI_FP_SCALE,
						      div64_u64(delay_sum_xm, AFI_FP_SCALE)),
					    afi->clk_period_ps) + AFI_FP_SCALE / 2,
				  AFI_FP_SCALE);
	else if (delay_sum_xm > 10000000000ULL)
		cfg->bps_actual =
			div64_u64((u64)frm_size_sum * 8ULL *
				  div64_u64(AFI_FP_SCALE *
					    div64_u64(PICO * AFI_FP_SCALE, delay_sum_xm),
					    afi->clk_period_ps) + AFI_FP_SCALE / 2,
				  AFI_FP_SCALE);
	else
		cfg->bps_actual =
			div64_u64((u64)frm_size_sum * 8ULL * AFI_FP_SCALE *
				  div64_u64(div64_u64(PICO * AFI_FP_SCALE, delay_sum_xm),
					    afi->clk_period_ps) + AFI_FP_SCALE / 2,
				  AFI_FP_SCALE);

	return 0;
}

/* Update the frame/delay sequence to match the requested rate. */
static int afi_dti_delay_calc(struct afi_control *afi, u32 dti_idx,
			      struct afi_fast_inj_start_cfg *cfg)
{
	struct afi_dti *dti = &afi->dti_tbl[dti_idx];
	int ret;

	if (!afi_dti_idx_chk(afi, dti_idx))
		return -EINVAL;

	/* Release current delay entries and re-link the frame entries. */
	ret = afi_dti_frm_free(afi, dti, true);
	if (ret)
		return ret;

	if (cfg->bps == 0) {
		/* Inject as fast as possible and rely on FRM_OUT_MAX to
		 * throttle down to the port's bandwidth.
		 */
		cfg->bps_actual = 0;
		return 0;
	}

	ret = afi_dti_delay_calc_do(afi, dti_idx, cfg);
	if (ret)
		return ret;

	return afi_dti_bps_actual_calc(afi, dti_idx, cfg);
}

/* Frames injected per DTI sequence. One sequence (one DTI_CNT decrement) walks
 * the frame chain once; the rate engine may repeat a frame INJ_CNT times to
 * fine tune the rate, so the per-sequence frame count is the sum of the frame
 * entries' INJ_CNT.
 */
static u32 afi_dti_frames_per_seq(struct afi_control *afi, struct afi_dti *dti)
{
	struct afi_frm *frm_tbl = afi->frm_tbl;
	u32 frm_idx = dti->first_frm_idx;
	u32 frames = 0;

	while (frm_idx) {
		struct afi_frm *frm = &frm_tbl[frm_idx];

		if (frm->entry_type == 0)
			frames += frm->inj_cnt;
		frm_idx = frm->next_idx;
	}

	return frames ? frames : 1;
}

static int afi_dti_inj_start(struct afi_control *afi, u32 fastid,
			     struct afi_fast_inj_start_cfg *cfg, bool start_flow)
{
	struct afi_dti *dti = &afi->dti_tbl[fastid];
	bool do_frm_delay_config = false;
	u32 total_cnt, frm_cnt, hijacked_cnt;
	int ret;

	if (dti->state != AFI_ENTRY_STATE_STOPPED)
		return -EINVAL;

	if (dti->frm_cnt == 0)
		return -EINVAL;

	afi_dti_cnt_get(afi, fastid, &total_cnt, &frm_cnt, &hijacked_cnt);

	if (frm_cnt != hijacked_cnt || frm_cnt != dti->frm_cnt)
		return -EINVAL;

	if (cfg->bps) {
		if (cfg->bps > afi->consts->fast_inj_bps_max)
			return -EINVAL;

		if (cfg->bps < afi->consts->fast_inj_bps_min)
			return -EINVAL;
	}

	if (cfg->bps != dti->start_cfg.bps) {
		/* Bandwidth changed => recalculate delays. */
		ret = afi_dti_delay_calc(afi, fastid, cfg);
		if (ret)
			return ret;

		dti->bw = cfg->bps >= AFI_DTI_BW1_THRES_BPS;
		do_frm_delay_config = true;
	}

	dti->mode = cfg->seq_cnt == 0 ? 1 : 0;
	if (cfg->seq_cnt) {
		/* seq_cnt is a frame count, but DTI_CNT counts whole sequences.
		 * Round up to the nearest sequence and report the achieved
		 * frame count (>= requested) in seq_actual.
		 */
		u32 fps = afi_dti_frames_per_seq(afi, dti);

		dti->frm_inj_cnt = DIV_ROUND_UP(cfg->seq_cnt, fps);
		cfg->seq_actual = dti->frm_inj_cnt * fps;
	} else {
		dti->frm_inj_cnt = 0;
		cfg->seq_actual = 0;
	}
	dti->start_cfg = *cfg;

	ret = afi->ops->dti_start(afi, fastid, do_frm_delay_config, true,
				  start_flow);
	if (ret)
		return ret;

	if (start_flow)
		dti->state = AFI_ENTRY_STATE_STARTED;

	return 0;
}

int afi_fast_inj_alloc(struct afi_control *afi,
		       struct afi_fast_inj_alloc_cfg *cfg, u32 *fastid)
{
	struct afi_dti *dti;
	s32 frm_idx, prev_frm_idx;
	u32 dti_idx, i;
	int ret;

	if (!cfg || !fastid)
		return -EINVAL;

	*fastid = 0;

	if (cfg->frm_cnt == 0 || cfg->frm_cnt > AFI_FAST_INJ_FRM_CNT_MAX)
		return -EINVAL;

	if (cfg->prio > afi->consts->prio_super + 1)
		return -EINVAL;

	/* On first alloc, enable AFI (this also initializes clk_period_ps,
	 * which the rate calculation needs).
	 */
	if (!afi->afi_ena) {
		afi->ops->tick_init(afi);

		ret = afi->ops->afi_enable(afi);
		if (ret)
			return ret;

		afi->afi_ena = 1;
	}

	ret = afi_dti_alloc(afi, &dti_idx);
	if (ret)
		return ret;

	*fastid = dti_idx;
	dti = &afi->dti_tbl[dti_idx];

	/* Allocate the frame entries. Delay entries are allocated later, when
	 * the flow is started with a particular rate.
	 */
	prev_frm_idx = -1;
	for (i = 0; i < cfg->frm_cnt; i++) {
		ret = afi_frm_alloc(afi, &frm_idx, 1, 0, prev_frm_idx);
		if (ret) {
			afi_dti_frm_free(afi, dti, false);
			afi_dti_free(afi, dti_idx);
			return ret;
		}

		if (i == 0)
			dti->first_frm_idx = frm_idx;

		prev_frm_idx = frm_idx;
	}

	dti->state = AFI_ENTRY_STATE_STOPPED;
	dti->port_no = cfg->port_no;
	dti->prio = cfg->prio;

	return 0;
}
EXPORT_SYMBOL_GPL(afi_fast_inj_alloc);

int afi_fast_inj_frm_hijack(struct afi_control *afi, u32 fastid,
			    struct afi_fast_inj_frm_cfg *cfg)
{
	struct afi_frm *frm_tbl = afi->frm_tbl;
	u32 total_cnt, frm_cnt, hijacked_cnt;
	u32 frm_idx, target = 0;
	struct afi_dti *dti;
	int ret;

	if (!cfg || !cfg->frm_size)
		return -EINVAL;

	if (!afi_dti_idx_chk(afi, fastid))
		return -EINVAL;

	dti = &afi->dti_tbl[fastid];

	if (!afi_frm_idx_chk(afi, dti->first_frm_idx))
		return -EINVAL;

	afi_dti_cnt_get(afi, fastid, &total_cnt, &frm_cnt, &hijacked_cnt);

	if (hijacked_cnt != dti->frm_cnt)
		return -EINVAL;

	if (dti->frm_cnt >= frm_cnt)
		/* More hijacks than frames requested at alloc. */
		return -EINVAL;

	/* Hijack into the next un-hijacked frame entry. */
	for (frm_idx = dti->first_frm_idx; frm_idx != 0;
	     frm_idx = frm_tbl[frm_idx].next_idx) {
		if (frm_tbl[frm_idx].entry_type == 0 &&
		    frm_tbl[frm_idx].frm_size == 0) {
			target = frm_idx;
			break;
		}
	}

	if (!target)
		return -EINVAL;

	frm_tbl[target].frm_size = cfg->frm_size;

	ret = afi->ops->dti_frm_hijack(afi, target);
	if (ret) {
		frm_tbl[target].frm_size = 0;
		return ret;
	}

	dti->frm_cnt++;

	if (dti->frm_cnt == frm_cnt) {
		/* All frames hijacked. Pseudo-start (configure but don't
		 * enable) so the flow can be freed via removal injection even
		 * if it is never really started.
		 */
		struct afi_fast_inj_start_cfg pseudo_cfg = {
			.bps = afi->consts->fast_inj_bps_min,
		};

		ret = afi_dti_inj_start(afi, fastid, &pseudo_cfg, false);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(afi_fast_inj_frm_hijack);

int afi_fast_inj_start(struct afi_control *afi, u32 fastid,
		       struct afi_fast_inj_start_cfg *cfg)
{
	if (!cfg)
		return -EINVAL;

	if (!afi_dti_idx_chk(afi, fastid))
		return -EINVAL;

	return afi_dti_inj_start(afi, fastid, cfg, true);
}
EXPORT_SYMBOL_GPL(afi_fast_inj_start);

int afi_fast_inj_stop(struct afi_control *afi, u32 fastid)
{
	struct afi_dti *dti;
	int ret;

	if (!afi_dti_idx_chk(afi, fastid))
		return -EINVAL;

	dti = &afi->dti_tbl[fastid];

	if (dti->state != AFI_ENTRY_STATE_STARTED)
		return -EINVAL;

	ret = afi->ops->dti_stop(afi, fastid);
	if (ret)
		return ret;

	dti->state = AFI_ENTRY_STATE_STOPPED;

	return 0;
}
EXPORT_SYMBOL_GPL(afi_fast_inj_stop);

bool afi_fast_inj_started(struct afi_control *afi, u32 fastid)
{
	if (!afi_dti_idx_chk(afi, fastid))
		return false;

	return afi->dti_tbl[fastid].state == AFI_ENTRY_STATE_STARTED;
}
EXPORT_SYMBOL_GPL(afi_fast_inj_started);

int afi_fast_inj_seq_cnt_get(struct afi_control *afi, u32 fastid, u32 *seq_cnt)
{
	if (!seq_cnt)
		return -EINVAL;

	if (!afi_dti_idx_chk(afi, fastid))
		return -EINVAL;

	if (!afi->ops->dti_seq_cnt_get)
		return -EOPNOTSUPP;

	return afi->ops->dti_seq_cnt_get(afi, fastid, seq_cnt);
}
EXPORT_SYMBOL_GPL(afi_fast_inj_seq_cnt_get);

int afi_fast_inj_free(struct afi_control *afi, u32 fastid)
{
	struct afi_dti *dti;
	int ret;

	if (!afi_dti_idx_chk(afi, fastid))
		return -EINVAL;

	dti = &afi->dti_tbl[fastid];

	if (dti->state != AFI_ENTRY_STATE_STOPPED)
		return -EINVAL;

	/* Inject frames one last time to remove them from buffer memory. */
	if (dti->frm_cnt) {
		ret = afi->ops->dti_frm_rm_inj(afi, fastid);
		if (ret)
			return ret;
	}

	ret = afi_dti_frm_free(afi, dti, false);
	if (ret)
		return ret;

	return afi_dti_free(afi, fastid);
}
EXPORT_SYMBOL_GPL(afi_fast_inj_free);
