// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <net/pkt_sched.h>

#include "lan9645x_main.h"

/*
 * Time-Aware Shaper (TAS) Overview
 * --------------------------------
 * TAS is a time-based scheduling mechanism defined in IEEE 802.1Qbv that
 * controls which traffic classes (queues) are allowed to transmit at specific
 * points in time. It operates using a repeating schedule called a Gate Control
 * List (GCL), where each entry specifies:
 *   - A gate mask: which queues are open (1) or closed (0)
 *   - An interval: how long this gate state remains active
 *
 * The schedule is synchronized to a base_time and repeats with a
 * fixed cycle_time. By carefully aligning open/close times with network
 * synchronization, TAS can guarantee deterministic low-latency transmission
 * for time-sensitive traffic while blocking lower-priority queues.
 *
 * Hardware Model
 * --------------
 * Each port supports:
 *   - Two "lists" (schedules) for double-buffered schedule replacement
 *   - A global pool of 900 GCL entries shared across all ports
 *   - Base time and cycle time registers per list
 *   - Per-TC QMAXSDU registers controlling guard band size
 *
 * List State Machine
 * ------------------
 * Each port's two lists can be in one of several states:
 *
 *   ADMIN       : Unused and safe for programming. New GCLs are always written
 *                 into a list in the ADMIN state.
 *
 *   ADVANCING   : Transition state after the driver has initiated a list
 *                 change. The hardware prepares the list for activation.
 *
 *   PENDING     : List is programmed and armed, waiting for base_time to be
 *                 reached. When base_time arrives, the hardware automatically
 *                 transitions to OPERATING.
 *
 *   OPERATING   : List is active and gating queues according to the programmed
 *                 schedule.
 *
 *   TERMINATING : A running list is being stopped. The hardware completes the
 *                 current cycle_time before transitioning to ADMIN
 *
 *   ADMIN -> ADVANCING -> PENDING -> OPERATING (normal activation)
 *   OPERATING -> TERMINATING -> ADMIN          (normal deactivation)
 */

#define TAS_TIMEOUT_MS			1000
#define TAS_MIN_CYCLE_TIME_NS		(1 * NSEC_PER_USEC)
#define TAS_MAX_CYCLE_TIME_NS		((1 * NSEC_PER_SEC) - 1)
#define TAS_ENTRIES_PER_PORT		2
#define TAS_STARTUP_TIME_DEFAULT	8 /* 8 * 256 ns = 2us advance notice */
#define TAS_QMAXSDU_GRANULARITY		64

/* TAS link speeds for calculation of guard band: */
enum lan9645x_taprio_link_speed {
	TAS_SPEED_NO_GB,
	TAS_SPEED_10,
	TAS_SPEED_100,
	TAS_SPEED_1000,
	TAS_SPEED_2500,
};

enum lan9645x_taprio_state {
	TAS_STATE_ADMIN,
	TAS_STATE_ADVANCING,
	TAS_STATE_PENDING,
	TAS_STATE_OPERATING,
	TAS_STATE_TERMINATING,
	NUM_TAS_STATE,
};

void lan9645x_new_base_time(struct lan9645x *lan9645x, const u32 cycle_time,
			    const ktime_t org_base_time, ktime_t *new_base_time)
{
	ktime_t current_time, threshold_time, new_time;
	struct timespec64 ts;
	u64 nr_of_cycles_p2;
	u64 nr_of_cycles;
	u64 diff_time;

	new_time = org_base_time;

	lan9645x_ptp_gettime64(&lan9645x->phc[LAN9645X_PHC_PORT].info, &ts);
	current_time = timespec64_to_ktime(ts);
	threshold_time = current_time + (2 * cycle_time);
	diff_time = threshold_time - new_time;
	nr_of_cycles = div_u64(diff_time, cycle_time);
	nr_of_cycles_p2 = 1; /* Use 2^0 as start value */

	if (new_time >= threshold_time) {
		*new_base_time = new_time;
		return;
	}

	/* Calculate the smallest power of 2 (nr_of_cycles_p2)
	 * that is larger than nr_of_cycles.
	 */
	while (nr_of_cycles_p2 < nr_of_cycles)
		nr_of_cycles_p2 <<= 1; /* Next (higher) power of 2 */

	/* Add as big chunks (power of 2 * cycle_time)
	 * as possible for each power of 2
	 */
	while (nr_of_cycles_p2) {
		if (new_time < threshold_time) {
			new_time += cycle_time * nr_of_cycles_p2;
			while (new_time < threshold_time)
				new_time += cycle_time * nr_of_cycles_p2;
			new_time -= cycle_time * nr_of_cycles_p2;
		}
		nr_of_cycles_p2 >>= 1; /* Next (lower) power of 2 */
	}

	new_time += cycle_time;
	*new_base_time = new_time;
}

static void lan9645x_taprio_set_list_num(struct lan9645x *lan9645x, int list)
{
	lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_SET(list),
		TAS_TAS_CFG_CTRL_LIST_NUM, lan9645x, TAS_TAS_CFG_CTRL);
}

static u32 lan9645x_taprio_list_state_get(struct lan9645x *lan9645x)
{
	u32 val = lan_rd(lan9645x, TAS_TAS_LIST_STATE);

	return TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
}

static void lan9645x_taprio_gcl_free(struct lan9645x *lan9645x,
				     int base, int count)
{
	if (base >= 0 && count > 0)
		bitmap_clear(lan9645x->tas_gcl_bitmap, base, count);
}

static void lan9645x_taprio_port_gcl_free(struct lan9645x_port *p, int slot)
{
	WARN_ON(slot < 0 || slot > 1);
	lan9645x_taprio_gcl_free(p->lan9645x,
				 p->tas.lists[slot].gcl_base,
				 p->tas.lists[slot].gcl_count);
	p->tas.lists[slot].gcl_base = -1;
	p->tas.lists[slot].gcl_count = 0;
}

/* Free GCL entries for any list that has reached ADMIN state. */
static void lan9645x_taprio_flush_pending_gcl(struct lan9645x *lan9645x)
{
	struct lan9645x_port *p;
	int i, j, list, state;

	lockdep_assert_held(&lan9645x->qos_lock);

	lan9645x_for_each_port(lan9645x, i, p) {
		for (j = 0; j < TAS_ENTRIES_PER_PORT; j++) {
			if (p->tas.lists[j].gcl_count == 0)
				continue;

			list = p->tas.list_base + j;
			lan9645x_taprio_set_list_num(lan9645x, list);
			state = lan9645x_taprio_list_state_get(lan9645x);

			if (state == TAS_STATE_ADMIN)
				lan9645x_taprio_port_gcl_free(p, j);
		}
	}
}

static int lan9645x_taprio_shutdown_pending(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;
	int i, list, state;
	unsigned long end;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list = p->tas.list_base + i;
		lan9645x_taprio_set_list_num(lan9645x, list);

		state = lan9645x_taprio_list_state_get(lan9645x);
		if (state != TAS_STATE_ADVANCING &&
		    state != TAS_STATE_PENDING)
			continue;

		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			lan_rmw(TAS_TAS_LIST_STATE_LIST_STATE_SET(TAS_STATE_ADMIN),
				TAS_TAS_LIST_STATE_LIST_STATE,
				lan9645x, TAS_TAS_LIST_STATE);

			state = lan9645x_taprio_list_state_get(lan9645x);
			if (state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_ADMIN)
			return -ETIME;

		lan9645x_taprio_port_gcl_free(p, i);
	}

	return 0;
}

static int lan9645x_taprio_shutdown_operating(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;
	int i, list, state;
	unsigned long end;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list = p->tas.list_base + i;
		lan9645x_taprio_set_list_num(lan9645x, list);

		state = lan9645x_taprio_list_state_get(lan9645x);
		if (state != TAS_STATE_OPERATING)
			continue;

		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			lan_rmw(TAS_TAS_LIST_STATE_LIST_STATE_SET(TAS_STATE_TERMINATING),
				TAS_TAS_LIST_STATE_LIST_STATE,
				lan9645x, TAS_TAS_LIST_STATE);

			state = lan9645x_taprio_list_state_get(lan9645x);
			if (state == TAS_STATE_TERMINATING ||
			    state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_TERMINATING &&
		    state != TAS_STATE_ADMIN)
			return -ETIME;

		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			state = lan9645x_taprio_list_state_get(lan9645x);
			if (state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_ADMIN)
			return -ETIME;
	}

	/* Restore gate state to "all-queues-open" */
	lan_wr(TAS_TAS_GATE_STATE_CTRL_HSCH_POS_SET(p->chip_port),
	       lan9645x,
	       TAS_TAS_GATE_STATE_CTRL);
	lan_wr(TAS_TAS_GATE_STATE_TAS_GATE_STATE_SET(0xff),
	       lan9645x,
	       TAS_TAS_GATE_STATE);

	return 0;
}

static int lan9645x_taprio_gcl_alloc(struct lan9645x *lan9645x, int count)
{
	int base, i;

	base = bitmap_find_next_zero_area(lan9645x->tas_gcl_bitmap,
					  LAN9645X_TAS_NUM_GCL, 0, count, 0);
	if (base >= LAN9645X_TAS_NUM_GCL)
		return -ENOSPC;

	bitmap_set(lan9645x->tas_gcl_bitmap, base, count);

	/* Initialize each allocated entry as a self-loop (empty list). */
	for (i = 0; i < count; i++) {
		lan_rmw(TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(base + i),
			TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM, lan9645x,
			TAS_TAS_CFG_CTRL);
		lan_wr(TAS_TAS_GCL_CTRL_CFG2_NEXT_GCL_SET(base + i),
		       lan9645x, TAS_TAS_GCL_CTRL_CFG2);
	}

	return base;
}

static int lan9645x_taprio_gcl_setup(struct lan9645x_port *port, int list,
				     int gcl_base,
				     struct tc_taprio_qopt_offload *qopt)
{
	struct lan9645x *lan9645x = port->lan9645x;
	u32 gcl_next;
	int i, state;

	/* Select the target list and verify it is in ADMIN state.
	 * The datasheet requires all list registers to be in ADMIN
	 * state before modification.
	 */
	lan9645x_taprio_set_list_num(lan9645x, list);

	state = lan9645x_taprio_list_state_get(lan9645x);
	if (state != TAS_STATE_ADMIN) {
		dev_err(lan9645x->dev,
			"TAS list %d not in ADMIN state (%d)\n", list, state);
		return -EBUSY;
	}

	lan_wr(TAS_TAS_LIST_BASE_ADDR_LIST_BASE_ADDR_SET(gcl_base),
	       lan9645x, TAS_TAS_LIST_BASE_ADDR);

	/* Associate TAS list with port, scheduler element, and TOD domain.
	 * Full write to clear any stale state from recycled lists.
	 */
	lan_wr(TAS_TAS_LIST_CFG_LIST_PORT_NUM_SET(port->chip_port) |
	       TAS_TAS_LIST_CFG_LIST_HSCH_POS_SET(port->chip_port) |
	       TAS_TAS_LIST_CFG_LIST_TOD_DOM_SET(0),
	       lan9645x, TAS_TAS_LIST_CFG);

	/* Program each GCL entry as a circular linked list */
	gcl_next = gcl_base;
	for (i = 0; i < qopt->num_entries; i++) {
		if (qopt->entries[i].command != TC_TAPRIO_CMD_SET_GATES)
			return -EINVAL;

		lan_rmw(TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(gcl_next),
			TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM, lan9645x,
			TAS_TAS_CFG_CTRL);

		gcl_next = (i >= qopt->num_entries - 1) ?
			   gcl_base : gcl_base + i + 1;

		lan_wr(TAS_TAS_GCL_CTRL_CFG_GATE_STATE_SET(qopt->entries[i].gate_mask),
		       lan9645x, TAS_TAS_GCL_CTRL_CFG);

		lan_wr(TAS_TAS_GCL_CTRL_CFG2_NEXT_GCL_SET(gcl_next), lan9645x,
		       TAS_TAS_GCL_CTRL_CFG2);

		lan_wr(qopt->entries[i].interval, lan9645x,
		       TAS_TAS_GCL_TIME_CFG);
	}

	return 0;
}

/* Configure QMAXSDU per-TC to set the guard band size.
 *
 * Guard band time = (QMAXSDU_GRANULARITY * 8) * QMAXSDU_VAL / LINK_SPEED.
 *
 * Use max_sdu[tc] when provided by user, otherwise fall back to the port MTU +
 * L2 overhead.
 *
 * For preemptible TCs (when frame preemption is active), guard banding is
 * disabled via SCH_TRAFFIC_QUEUES and MAC HOLD is used instead.  The
 * HOLDADVANCE register controls the hold timing.
 */
static void lan9645x_taprio_guard_bands_update(struct lan9645x_port *p,
					       struct tc_taprio_qopt_offload *qopt)
{
	struct dsa_port *dp = dsa_to_port(p->lan9645x->ds, p->chip_port);
	struct lan9645x *lan9645x = p->lan9645x;
	u8 preemptible_tcs = p->fp.admin_status;
	struct net_device *dev = dp->user;
	u32 max_guard_band = 0;
	u8 holdadv;
	int tc, i;

	for (tc = 0; tc < LAN9645X_NUM_TC; tc++) {
		u32 max_sdu_bytes, maxsdu_val, maxsdu_lsb;

		max_sdu_bytes = dev->mtu + ETH_HLEN + 2 * VLAN_HLEN +
				ETH_FCS_LEN;

		if (qopt && qopt->max_sdu[tc])
			max_sdu_bytes = qopt->max_sdu[tc] + ETH_HLEN +
					2 * VLAN_HLEN + ETH_FCS_LEN;

		maxsdu_val = max_sdu_bytes / TAS_QMAXSDU_GRANULARITY;
		maxsdu_lsb = max_sdu_bytes % TAS_QMAXSDU_GRANULARITY;

		/* Track the largest guard band across all TCs for validation */
		if (maxsdu_val > max_guard_band)
			max_guard_band = maxsdu_val;

		lan_rmw(TAS_TAS_QMAXSDU_CFG_QMAXSDU_VAL_SET(maxsdu_val),
			TAS_TAS_QMAXSDU_CFG_QMAXSDU_VAL,
			lan9645x,
			TAS_TAS_QMAXSDU_CFG(p->chip_port, tc));

		lan_rmw(TAS_QMAXSDU_DISC_CFG_QMAXSDU_LSB_SET(maxsdu_lsb) |
			TAS_QMAXSDU_DISC_CFG_QMAXSDU_DISC_ENA_SET(!!maxsdu_val),
			TAS_QMAXSDU_DISC_CFG_QMAXSDU_LSB |
			TAS_QMAXSDU_DISC_CFG_QMAXSDU_DISC_ENA,
			lan9645x,
			TAS_QMAXSDU_DISC_CFG(p->chip_port, tc));
	}

	/* Configure frame preemption TAS registers unconditionally.
	 * SCH_TRAFFIC_QUEUES marks queues where guard banding is disabled
	 * (preemptible queues use MAC HOLD instead).  HOLDADVANCE controls
	 * the hold timing based on minimum fragment size.
	 */
	holdadv = preemptible_tcs ? p->fp.add_frag_size + 1 : 0;

	lan_rmw(TAS_TAS_PROFILE_CONFIG_SCH_TRAFFIC_QUEUES_SET(preemptible_tcs),
		TAS_TAS_PROFILE_CONFIG_SCH_TRAFFIC_QUEUES,
		lan9645x,
		TAS_TAS_PROFILE_CONFIG(p->chip_port));

	lan_rmw(TAS_TAS_PROFILE_CONFIG_HOLDADVANCE_SET(holdadv),
		TAS_TAS_PROFILE_CONFIG_HOLDADVANCE,
		lan9645x,
		TAS_TAS_PROFILE_CONFIG(p->chip_port));

	/* Warn if any GCL entry interval is shorter than the largest guard
	 * band across all TCs.  This is a conservative check: the actual
	 * guard band for a given entry depends on which TCs are gated, so
	 * the effective guard band may be smaller than max_guard_band.
	 *
	 * Convert max_guard_band from QMAXSDU_VAL units to nanoseconds:
	 * gb_time_ns = 512000 * QMAXSDU_VAL / speed_mbps
	 * Skip when the port speed is unknown (port down).
	 */
	if (!qopt)
		return;

	switch (p->speed) {
	case LAN9645X_SPEED_10:
		max_guard_band *= 51200;
		break;
	case LAN9645X_SPEED_100:
		max_guard_band *= 5120;
		break;
	case LAN9645X_SPEED_1000:
		max_guard_band *= 512;
		break;
	case LAN9645X_SPEED_2500:
		/* 512000 / 2500 = 204.8, round up */
		max_guard_band *= 205;
		break;
	default:
		return;
	}

	for (i = 0; i < qopt->num_entries; i++) {
		if (qopt->entries[i].interval < max_guard_band)
			dev_warn(lan9645x->dev,
				 "port %d: GCL entry %d interval %u ns shorter than worst-case guard band %u ns (max across all TCs)\n",
				 p->chip_port, i,
				 qopt->entries[i].interval,
				 max_guard_band);
	}
}

static void lan9645x_taprio_guard_bands_reset(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;
	int tc;

	for (tc = 0; tc < LAN9645X_NUM_TC; tc++) {
		lan_rmw(TAS_TAS_QMAXSDU_CFG_QMAXSDU_VAL_SET(0),
			TAS_TAS_QMAXSDU_CFG_QMAXSDU_VAL,
			lan9645x,
			TAS_TAS_QMAXSDU_CFG(p->chip_port, tc));

		lan_rmw(TAS_QMAXSDU_DISC_CFG_QMAXSDU_LSB_SET(0) |
			TAS_QMAXSDU_DISC_CFG_QMAXSDU_DISC_ENA_SET(0),
			TAS_QMAXSDU_DISC_CFG_QMAXSDU_LSB |
			TAS_QMAXSDU_DISC_CFG_QMAXSDU_DISC_ENA,
			lan9645x,
			TAS_QMAXSDU_DISC_CFG(p->chip_port, tc));
	}

	/* Clear frame preemption TAS configuration */
	lan_rmw(TAS_TAS_PROFILE_CONFIG_SCH_TRAFFIC_QUEUES_SET(0),
		TAS_TAS_PROFILE_CONFIG_SCH_TRAFFIC_QUEUES,
		lan9645x,
		TAS_TAS_PROFILE_CONFIG(p->chip_port));

	lan_rmw(TAS_TAS_PROFILE_CONFIG_HOLDADVANCE_SET(0),
		TAS_TAS_PROFILE_CONFIG_HOLDADVANCE,
		lan9645x,
		TAS_TAS_PROFILE_CONFIG(p->chip_port));
}

int lan9645x_taprio_add(struct lan9645x *lan9645x, int port,
			struct tc_taprio_qopt_offload *qopt)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	int i, err = 0, new_list, obsolete, gcl_base;
	u64 cycle_time = qopt->cycle_time;
	u64 calculated_cycle_time = 0;
	struct timespec64 ts;
	ktime_t base_time;

	mutex_lock(&lan9645x->qos_lock);

	lan9645x_taprio_flush_pending_gcl(lan9645x);

	if (qopt->cycle_time_extension) {
		NL_SET_ERR_MSG_MOD(qopt->extack,
				   "Cycle time extension not supported");
		err = -EOPNOTSUPP;
		goto out;
	}

	if (cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		NL_SET_ERR_MSG_MOD(qopt->extack,
				   "Cycle time exceeds maximum (999999999 ns)");
		err = -EINVAL;
		goto out;
	}

	for (i = 0; i < qopt->num_entries; i++) {
		if (qopt->entries[i].interval < TAS_MIN_CYCLE_TIME_NS) {
			NL_SET_ERR_MSG_MOD(qopt->extack,
					   "Entry interval below minimum (1 us)");
			err = -EINVAL;
			goto out;
		}
		if (qopt->entries[i].interval > TAS_MAX_CYCLE_TIME_NS) {
			NL_SET_ERR_MSG_MOD(qopt->extack,
					   "Entry interval exceeds maximum");
			err = -EINVAL;
			goto out;
		}
		calculated_cycle_time += qopt->entries[i].interval;
	}
	if (calculated_cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		NL_SET_ERR_MSG_MOD(qopt->extack,
				   "Total entry time exceeds maximum");
		err = -EINVAL;
		goto out;
	}
	if (cycle_time < calculated_cycle_time) {
		NL_SET_ERR_MSG_MOD(qopt->extack,
				   "Cycle time less than sum of entry intervals");
		err = -EINVAL;
		goto out;
	}

	lan9645x_new_base_time(lan9645x, cycle_time, qopt->base_time, &base_time);

	/* Select list using software state */
	if (p->tas.active_list < 0) {
		/* No active schedule use first list */
		new_list = p->tas.list_base;
	} else {
		/* Schedule replacement use the other list.
		 * Shut down any PENDING list first.
		 */
		err = lan9645x_taprio_shutdown_pending(p);
		if (err) {
			NL_SET_ERR_MSG_MOD(qopt->extack,
					   "Failed to shut down pending list");
			goto out;
		}
		new_list = p->tas.list_base;
		if (new_list == p->tas.active_list)
			new_list = p->tas.list_base + 1;
	}
	obsolete = (p->tas.active_list < 0) ? new_list : p->tas.active_list;

	/* Allocate GCL entries from software bitmap */
	gcl_base = lan9645x_taprio_gcl_alloc(lan9645x, qopt->num_entries);
	if (gcl_base < 0) {
		NL_SET_ERR_MSG_MOD(qopt->extack,
				   "No free GCL entries available");
		err = -ENOSPC;
		goto out;
	}

	/* Setup GCL entries */
	err = lan9645x_taprio_gcl_setup(p, new_list, gcl_base, qopt);
	if (err) {
		NL_SET_ERR_MSG_MOD(qopt->extack,
				   "Failed to setup GCL entries");
		lan9645x_taprio_gcl_free(lan9645x, gcl_base,
					 qopt->num_entries);
		goto out;
	}

	/* Configure guard bands from max_sdu or port MTU */
	lan9645x_taprio_guard_bands_update(p, qopt);

	/* Setup TAS list */
	ts = ktime_to_timespec64(base_time);
	lan_wr(TAS_TAS_BASE_TIME_NSEC_BASE_TIME_NSEC_SET(ts.tv_nsec), lan9645x,
	       TAS_TAS_BASE_TIME_NSEC);

	lan_wr((ts.tv_sec & GENMASK(31, 0)), lan9645x,
	       TAS_TAS_BASE_TIME_SEC_LSB);

	lan_wr(TAS_TAS_BASE_TIME_SEC_MSB_BASE_TIME_SEC_MSB_SET(ts.tv_sec >> 32),
	       lan9645x, TAS_TAS_BASE_TIME_SEC_MSB);

	lan_wr(cycle_time, lan9645x, TAS_TAS_CYCLE_TIME_CFG);

	/* Write STARTUP_CFG as a full register write to clear the sticky
	 * STARTUP_ERROR flag and explicitly set STARTUP_TIME.  A zero
	 * STARTUP_TIME prevents the list from starting.
	 */
	lan_wr(TAS_TAS_STARTUP_CFG_OBSOLETE_IDX_SET(obsolete) |
	       TAS_TAS_STARTUP_CFG_STARTUP_TIME_SET(TAS_STARTUP_TIME_DEFAULT),
	       lan9645x, TAS_TAS_STARTUP_CFG);

	dev_dbg(lan9645x->dev,
		"TAS add: chip_port=%d list_base=%d new_list=%d obsolete=%d gcl_base=%d entries=%zu cycle=%llu\n",
		p->chip_port, p->tas.list_base, new_list, obsolete,
		gcl_base, qopt->num_entries, cycle_time);

	/* Start list processing */
	lan_rmw(TAS_TAS_LIST_STATE_LIST_STATE_SET(TAS_STATE_ADVANCING),
		TAS_TAS_LIST_STATE_LIST_STATE, lan9645x, TAS_TAS_LIST_STATE);

	/* Update software state. Store new GCL in the per-list slot.
	 * The old list may still be OPERATING in HW, so we can not free it's
	 * GCL entries yet.
	 * They will be freed by flush_pending_gcl when the old list eventually
	 * reaches ADMIN state.
	 */
	p->tas.lists[new_list - p->tas.list_base].gcl_base = gcl_base;
	p->tas.lists[new_list - p->tas.list_base].gcl_count = qopt->num_entries;
	p->tas.active_list = new_list;

	/* Store a refcounted copy of the taprio offload */
	if (p->tas.taprio)
		taprio_offload_free(p->tas.taprio);
	p->tas.taprio = taprio_offload_get(qopt);

out:
	mutex_unlock(&lan9645x->qos_lock);

	return err;
}

int lan9645x_taprio_del(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	int err;

	mutex_lock(&lan9645x->qos_lock);

	lan9645x_taprio_flush_pending_gcl(lan9645x);

	err = lan9645x_taprio_shutdown_pending(p);
	if (err)
		goto out;

	err = lan9645x_taprio_shutdown_operating(p);
	if (err)
		goto out;

	/* All lists in ADMIN so we can free both list slots GCL entries */
	for (int j = 0; j < TAS_ENTRIES_PER_PORT; j++)
		lan9645x_taprio_port_gcl_free(p, j);
	p->tas.active_list = -1;

	/* Free stored taprio offload */
	if (p->tas.taprio) {
		taprio_offload_free(p->tas.taprio);
		p->tas.taprio = NULL;
	}

	/* Reset guard bands to hardware defaults */
	lan9645x_taprio_guard_bands_reset(p);
out:
	mutex_unlock(&lan9645x->qos_lock);

	return err;
}

void lan9645x_taprio_init(struct lan9645x *lan9645x)
{
	u32 revisit = (256 * 1000) / lan9645x_ptp_get_period_ps();
	int num_tas_lists, port;

	num_tas_lists = lan9645x->num_phys_ports * TAS_ENTRIES_PER_PORT;

	mutex_init(&lan9645x->qos_lock);
	lan_wr(TAS_TAS_STATEMACHINE_CFG_REVISIT_DLY_SET(revisit), lan9645x,
	       TAS_TAS_STATEMACHINE_CFG);

	/* For now we always use guard band on all queues */
	lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_MAX_SET(num_tas_lists - 1) |
		TAS_TAS_CFG_CTRL_ALWAYS_GUARD_BAND_SCH_Q_SET(1),
		TAS_TAS_CFG_CTRL_LIST_NUM_MAX |
		TAS_TAS_CFG_CTRL_ALWAYS_GUARD_BAND_SCH_Q,
		lan9645x, TAS_TAS_CFG_CTRL);

	/* Reserve GCL entry 0 so it is never allocated.  Uninitialized
	 * NEXT_GCL fields default to 0, so entry 0 must not belong to any
	 * active schedule, otherwise other lists' stale NEXT_GCL pointers
	 * could accidentally chain into it.
	 */
	bitmap_set(lan9645x->tas_gcl_bitmap, 0, 1);

	/* Pre-compute per-port TAS list base indices and init state */
	for (port = 0; port < lan9645x->num_phys_ports; port++) {
		struct lan9645x_port *p = lan9645x->ports[port];

		p->tas.list_base = port * TAS_ENTRIES_PER_PORT;
		p->tas.active_list = -1;
		p->tas.taprio = NULL;
		for (int j = 0; j < TAS_ENTRIES_PER_PORT; j++) {
			p->tas.lists[j].gcl_base = -1;
			p->tas.lists[j].gcl_count = 0;
		}
	}
}

void lan9645x_taprio_deinit(struct lan9645x *lan9645x)
{
	int port;

	for (port = 0; port < lan9645x->num_phys_ports; ++port)
		lan9645x_taprio_del(lan9645x, port);
}

int lan9645x_taprio_speed_set(struct lan9645x_port *p, int speed)
{
	struct lan9645x *lan9645x = p->lan9645x;
	u8 spd;

	/* Update TAS profile speed */
	switch (speed) {
	case SPEED_10:
		spd = TAS_SPEED_10;
		break;
	case SPEED_100:
		spd = TAS_SPEED_100;
		break;
	case SPEED_1000:
		spd = TAS_SPEED_1000;
		break;
	case SPEED_2500:
		spd = TAS_SPEED_2500;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&lan9645x->qos_lock);

	lan_rmw(TAS_TAS_PROFILE_CONFIG_LINK_SPEED_SET(spd),
		TAS_TAS_PROFILE_CONFIG_LINK_SPEED, lan9645x,
		TAS_TAS_PROFILE_CONFIG(p->chip_port));

	/* Recalculate guard bands for the new link speed */
	if (p->tas.taprio)
		lan9645x_taprio_guard_bands_update(p, p->tas.taprio);

	mutex_unlock(&lan9645x->qos_lock);

	return 0;
}

void lan9645x_taprio_guard_bands_recalc(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;

	mutex_lock(&lan9645x->qos_lock);
	if (p->tas.taprio)
		lan9645x_taprio_guard_bands_update(p, p->tas.taprio);
	mutex_unlock(&lan9645x->qos_lock);
}
