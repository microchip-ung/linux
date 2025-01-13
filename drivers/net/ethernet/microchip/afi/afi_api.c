// SPDX-License-Identifier: GPL-2.0+
/* Microchip AFI API
 *
 * Copyright (c) 2024 Microchip Technology Inc. and its subsidiaries.
 */

#include "afi_api.h"

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
		afi->frm_tbl[prev_frm_tbl_idx].next_ptr = *frm_idx;

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

int afi_init(struct afi_control *afi)
{
	afi->frms_alloced = kcalloc((afi->consts->frm_cnt + 31) / 32, sizeof(u32), GFP_KERNEL);
	afi->ttis_alloced = kcalloc((afi->consts->slow_inj_cnt + 31) / 32, sizeof(u32), GFP_KERNEL);
	afi->frm_tbl = kcalloc(afi->consts->frm_cnt, sizeof(struct afi_frm), GFP_KERNEL);
	afi->tti_tbl = kcalloc(afi->consts->slow_inj_cnt, sizeof(struct afi_tti), GFP_KERNEL);

	if (!afi->frms_alloced ||
	    !afi->ttis_alloced ||
	    !afi->frm_tbl ||
	    !afi->tti_tbl)
		return -ENOMEM;

	return 0;
}

void afi_deinit(struct afi_control *afi)
{
	kfree(afi->tti_tbl);
	kfree(afi->frm_tbl);
	kfree(afi->ttis_alloced);
	kfree(afi->frms_alloced);
}

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
		return -EINVAL;

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
