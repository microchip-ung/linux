// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

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
 *   - Two "lists" (schedules), each containing its own GCL
 *   - A finite pool of hardware GCL entries shared across all ports
 *   - Base time and cycle time registers per list
 *
 * The driver is responsible for:
 *   - Selecting which list to program based on current list states
 *   - Allocating GCL entries from the global pool
 *   - Programming each GCL entry with gate mask and interval
 *   - Setting base_time and cycle_time in hardware
 *   - Initiating state transitions so the hardware starts or stops schedules
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
 *                 automatically.
 *
 * Normal activation flow:
 *     ADMIN → ADVANCING → PENDING → OPERATING
 *
 * Normal deactivation flow:
 *     OPERATING → TERMINATING → ADMIN
 *
 * Driver Workflow
 * ---------------
 * 1. Validate the taprio qdisc configuration (cycle_time, intervals, etc.)
 * 2. Compute a safe base_time using lan9645x_new_base_time()
 * 3. Select a suitable list via lan9645x_taprio_list_find():
 *       - If one list is OPERATING, use the other list as PENDING
 *       - If no list is active, use an ADMIN list directly
 * 4. Allocate a contiguous block of free GCL entries
 * 5. Program the GCL with gate masks and intervals
 * 6. Write base_time and cycle_time registers
 * 7. Start the schedule by moving the list to ADVANCING (or terminating the
 *    current OPERATING list if replacing)
 */

#define TAS_TIMEOUT_MS 1000
#define TAS_MIN_CYCLE_TIME_NS (1 * NSEC_PER_USEC)
#define TAS_MAX_CYCLE_TIME_NS ((1 * NSEC_PER_SEC) - 1)
#define TAS_NUM_GCL		900
#define TAS_ENTRIES_PER_PORT 2

/* TAS link speeds for calculation of guard band: */
enum lan9645x_taprio_link_speed {
	TAS_SPEED_NO_GB,
	TAS_SPEED_10,
	TAS_SPEED_100,
	TAS_SPEED_1000,
	TAS_SPEED_2500,
};

/* TAS list states: */
enum sparx5_taprio_state {
	TAS_STATE_ADMIN,
	TAS_STATE_ADVANCING,
	TAS_STATE_PENDING,
	TAS_STATE_OPERATING,
	TAS_STATE_TERMINATING,
	NUM_TAS_STATE,
};

static void lan9645x_new_base_time(struct lan9645x *lan9645x,
				   const u32 cycle_time,
				   const ktime_t org_base_time,
				   ktime_t *new_base_time)
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

static int lan9645x_taprio_list_index(struct lan9645x_port *port, u8 tas_entry)
{
	int portno, pidx = 0;

	/* Limit the index to available ports */
	for (portno = 0; portno < port->lan9645x->num_phys_ports; ++portno) {
		if (port->lan9645x->ports[portno])
			pidx++;
		if (portno == port->chip_port)
			return (pidx * TAS_ENTRIES_PER_PORT) + tas_entry;
	}

	return 0;
}

static int lan9645x_taprio_shutdown_pending(struct lan9645x_port *port)
{
	struct lan9645x *lan9645x = port->lan9645x;
	int i, list, state;
	unsigned long end;
	u32 val;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list = lan9645x_taprio_list_index(port, i);
		lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_SET(list),
			TAS_TAS_CFG_CTRL_LIST_NUM,
			lan9645x, TAS_TAS_CFG_CTRL);

		val = lan_rd(lan9645x, TAS_TAS_LIST_STATE);
		state = TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
		if (state != TAS_STATE_ADVANCING &&
		    state != TAS_STATE_PENDING)
			continue;

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			lan_rmw(TAS_TAS_LIST_STATE_LIST_STATE_SET(TAS_STATE_ADMIN),
				TAS_TAS_LIST_STATE_LIST_STATE,
				lan9645x,
				TAS_TAS_LIST_STATE);

			val = lan_rd(lan9645x, TAS_TAS_LIST_STATE);
			state = TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
			if (state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_ADMIN)
			return -ETIME;
	}

	return 0;
}

static int lan9645x_taprio_shutdown_operating(struct lan9645x_port *port)
{
	struct lan9645x *lan9645x = port->lan9645x;
	int i, list, state;
	unsigned long end;
	u32 val;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list = lan9645x_taprio_list_index(port, i);
		lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_SET(list),
			TAS_TAS_CFG_CTRL_LIST_NUM,
			lan9645x,
			TAS_TAS_CFG_CTRL);

		val = lan_rd(lan9645x, TAS_TAS_LIST_STATE);
		state = TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
		if (state != TAS_STATE_OPERATING)
			continue;

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			lan_rmw(TAS_TAS_LIST_STATE_LIST_STATE_SET(TAS_STATE_TERMINATING),
				TAS_TAS_LIST_STATE_LIST_STATE,
				lan9645x,
				TAS_TAS_LIST_STATE);

			val = lan_rd(lan9645x, TAS_TAS_LIST_STATE);
			state = TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
			if (state == TAS_STATE_TERMINATING ||
			    state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_TERMINATING &&
		    state != TAS_STATE_ADMIN)
			return -ETIME;

		/* Do not wait forever for the state change */
		end = jiffies + msecs_to_jiffies(TAS_TIMEOUT_MS);
		do {
			val = lan_rd(lan9645x, TAS_TAS_LIST_STATE);
			state = TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
			if (state == TAS_STATE_ADMIN)
				break;

			cond_resched();
		} while (!time_after(jiffies, end));

		if (state != TAS_STATE_ADMIN)
			return -ETIME;

		/* Restore gate state to "all-queues-open" */
		/* Select port n on layer 2 of Hierarchical Scheduler */
		lan_wr(TAS_TAS_GATE_STATE_CTRL_HSCH_POS_SET(port->chip_port),
		       lan9645x,
		       TAS_TAS_GATE_STATE_CTRL);
		/* Set gate state to "all-queues-open" */
		lan_wr(TAS_TAS_GATE_STATE_TAS_GATE_STATE_SET(0xff),
		       lan9645x,
		       TAS_TAS_GATE_STATE);
	}

	return 0;
}

static int lan9645x_taprio_list_find(struct lan9645x_port *port, int *new,
				     int *obsolete)
{
	int i, err, state_cnt[NUM_TAS_STATE] = {0};
	struct lan9645x *lan9645x = port->lan9645x;
	int state[TAS_ENTRIES_PER_PORT];
	int list[TAS_ENTRIES_PER_PORT];
	bool valid = false;
	int oper = -1;
	u32 val;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		list[i] = lan9645x_taprio_list_index(port, i);
		lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_SET(list[i]),
			TAS_TAS_CFG_CTRL_LIST_NUM,
			lan9645x,
			TAS_TAS_CFG_CTRL);

		val = lan_rd(lan9645x, TAS_TAS_LIST_STATE);
		state[i] = TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
		if (state[i] >= NUM_TAS_STATE)
			return -EINVAL;

		if (state[i] == TAS_STATE_OPERATING)
			oper = list[i];

		state_cnt[state[i]]++;
	}

	if (state_cnt[TAS_STATE_ADMIN] == 2)
		valid = true;
	if (state_cnt[TAS_STATE_ADMIN] == 1 &&
	    state_cnt[TAS_STATE_PENDING] == 1)
		valid = true;
	if (state_cnt[TAS_STATE_ADMIN] == 1 &&
	    state_cnt[TAS_STATE_OPERATING] == 1)
		valid = true;
	if (state_cnt[TAS_STATE_OPERATING] == 1 &&
	    state_cnt[TAS_STATE_PENDING] == 1)
		valid = true;

	if (!valid)
		return -EINVAL;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		if (state[i] == TAS_STATE_PENDING) {
			err = lan9645x_taprio_shutdown_pending(port);
			if (err)
				return err;
			*new = list[i];
			*obsolete = (oper == -1) ? *new : oper;
			return 0;
		}
	}

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		if (state[i] == TAS_STATE_ADMIN) {
			*new = list[i];
			*obsolete = (oper == -1) ? *new : oper;
			return 0;
		}
	}

	return -ENOENT; /* No suitable list found */
}

static u32 lan9645x_taprio_list_state_get(struct lan9645x_port *port)
{
	u32 val;

	val = lan_rd(port->lan9645x, TAS_TAS_LIST_STATE);

	return TAS_TAS_LIST_STATE_LIST_STATE_GET(val);
}

static u32 lan9645x_taprio_list_index_state_get(struct lan9645x_port *port,
						u32 list)
{
	lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_SET(list), TAS_TAS_CFG_CTRL_LIST_NUM,
		port->lan9645x, TAS_TAS_CFG_CTRL);

	return lan9645x_taprio_list_state_get(port);
}

static int lan9645x_taprio_gcl_free_get(struct lan9645x_port *port,
					unsigned long *free_list)
{
	struct lan9645x *lan9645x = port->lan9645x;
	u32 num_free, state, list;
	u32 base, next, max_list;

	/* By default everything is free */
	bitmap_fill(free_list, TAS_NUM_GCL);
	num_free = TAS_NUM_GCL;

	/* Iterate over all gcl entries and find out which are free. And mark
	 * those that are not free.
	 */
	max_list = lan9645x->num_phys_ports * TAS_ENTRIES_PER_PORT;
	for (list = 0; list < max_list; ++list) {
		state = lan9645x_taprio_list_index_state_get(port, list);
		if (state == TAS_STATE_ADMIN)
			continue;

		base = lan_rd(lan9645x, TAS_TAS_LIST_BASE_ADDR);
		base = TAS_TAS_LIST_BASE_ADDR_LIST_BASE_ADDR_GET(base);
		next = base;

		do {
			clear_bit(next, free_list);
			num_free--;

			lan_rmw(TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(next),
				TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM,
				lan9645x, TAS_TAS_CFG_CTRL);

			next = lan_rd(lan9645x, TAS_TAS_GCL_CTRL_CFG2);
			next = TAS_TAS_GCL_CTRL_CFG2_NEXT_GCL_GET(next);
		} while (base != next);
	}

	return num_free;
}

/* Find N continuous GCL entries */
static int lan9645x_taprio_gcl_base_get(unsigned long *free_list,
					int num_entries)
{
	int i, empty_found;

	empty_found = 0;
	for (i = 0; i < TAS_NUM_GCL; i++) {
		if (test_bit(i, free_list))
			empty_found++;
		else
			empty_found = 0;

		if (empty_found == num_entries)
			return (i - num_entries) + 1;
	}

	return -ENOENT;
}

/* Setup GCLs for a specific list */
static int lan9645x_taprio_gcl_setup(struct lan9645x_port *port, int list,
				     struct tc_taprio_qopt_offload *qopt)
{
	DECLARE_BITMAP(free_list, TAS_NUM_GCL);
	struct lan9645x *lan9645x = port->lan9645x;
	int i, num_free, base;

	num_free = lan9645x_taprio_gcl_free_get(port, free_list);
	if (num_free < (int)qopt->num_entries)
		return -EINVAL;

	base = lan9645x_taprio_gcl_base_get(free_list, qopt->num_entries);
	if (base < 0)
		return -EINVAL;

	for (i = 0; i < TAS_ENTRIES_PER_PORT; i++) {
		lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_SET(list + i),
			TAS_TAS_CFG_CTRL_LIST_NUM, lan9645x, TAS_TAS_CFG_CTRL);

		lan_rmw(TAS_TAS_LIST_BASE_ADDR_LIST_BASE_ADDR_SET(base),
			TAS_TAS_LIST_BASE_ADDR_LIST_BASE_ADDR, lan9645x,
			TAS_TAS_LIST_BASE_ADDR);

		/* Associate TAS list with physical port number and
		 * scheduler element.
		 */
		lan_rmw(TAS_TAS_LIST_CFG_LIST_PORT_NUM_SET(port->chip_port) |
			TAS_TAS_LIST_CFG_LIST_HSCH_POS_SET(port->chip_port),
			TAS_TAS_LIST_CFG_LIST_PORT_NUM |
			TAS_TAS_LIST_CFG_LIST_HSCH_POS,
			lan9645x,
			TAS_TAS_LIST_CFG);
	}

	for (i = 0; i < qopt->num_entries; i++) {
		u32 gcl_next = (i >= qopt->num_entries - 1) ? base :
							      base + i + 1;
		/* GCL index is relative to BASE_ADDR */
		lan_rmw(TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(i),
			TAS_TAS_CFG_CTRL_GCL_ENTRY_NUM, lan9645x,
			TAS_TAS_CFG_CTRL);

		if (qopt->entries[i].command != TC_TAPRIO_CMD_SET_GATES)
			return -EINVAL;

		/* Set gate states for this GCL */
		lan_rmw(TAS_TAS_GCL_CTRL_CFG_GATE_STATE_SET(qopt->entries[i].gate_mask),
			TAS_TAS_GCL_CTRL_CFG_GATE_STATE, lan9645x,
			TAS_TAS_GCL_CTRL_CFG);

		lan_wr(TAS_TAS_GCL_CTRL_CFG2_NEXT_GCL_SET(gcl_next), lan9645x,
		       TAS_TAS_GCL_CTRL_CFG2);

		lan_wr(qopt->entries[i].interval, lan9645x,
		       TAS_TAS_GCL_TIME_CFG);
	}

	return 0;
}

int lan9645x_taprio_add(struct lan9645x *lan9645x, int port,
			struct tc_taprio_qopt_offload *qopt)
{
	int i, err, new_list = -1, obsolete = -1;
	u64 cycle_time = qopt->cycle_time;
	u64 calculated_cycle_time = 0;
	struct timespec64 ts;
	ktime_t base_time;

	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	mutex_lock(&lan9645x->qos_lock);
	if (cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		err = -EINVAL;
		goto out;
	}
	for (i = 0; i < qopt->num_entries; i++) {
		if (qopt->entries[i].interval < TAS_MIN_CYCLE_TIME_NS) {
			err = -EINVAL;
			goto out;
		}
		if (qopt->entries[i].interval > TAS_MAX_CYCLE_TIME_NS) {
			err = -EINVAL;
			goto out;
		}
		calculated_cycle_time += qopt->entries[i].interval;
	}
	if (calculated_cycle_time > TAS_MAX_CYCLE_TIME_NS) {
		err = -EINVAL;
		goto out;
	}
	if (cycle_time < calculated_cycle_time) {
		err = -EINVAL;
		goto out;
	}

	lan9645x_new_base_time(lan9645x, cycle_time, qopt->base_time, &base_time);

	/* Select an appropriate entry to use */
	err = lan9645x_taprio_list_find(p, &new_list, &obsolete);
	if (err) {
		err = -EINVAL;
		goto out;
	}

	/* Setup GCL entries */
	err = lan9645x_taprio_gcl_setup(p, new_list, qopt);
	if (err) {
		err = -EINVAL;
		goto out;
	}

	/* Setup TAS list */
	ts = ktime_to_timespec64(base_time);
	lan_wr(TAS_TAS_BASE_TIME_NSEC_BASE_TIME_NSEC_SET(ts.tv_nsec), lan9645x,
	       TAS_TAS_BASE_TIME_NSEC);

	lan_wr((ts.tv_sec & GENMASK(31, 0)), lan9645x,
	       TAS_TAS_BASE_TIME_SEC_LSB);

	lan_wr(TAS_TAS_BASE_TIME_SEC_MSB_BASE_TIME_SEC_MSB_SET(ts.tv_sec >> 32),
	       lan9645x, TAS_TAS_BASE_TIME_SEC_MSB);

	lan_wr(cycle_time, lan9645x, TAS_TAS_CYCLE_TIME_CFG);

	lan_rmw(TAS_TAS_STARTUP_CFG_OBSOLETE_IDX_SET(obsolete),
		TAS_TAS_STARTUP_CFG_OBSOLETE_IDX, lan9645x,
		TAS_TAS_STARTUP_CFG);

	/* Start list processing */
	lan_rmw(TAS_TAS_LIST_STATE_LIST_STATE_SET(TAS_STATE_ADVANCING),
		TAS_TAS_LIST_STATE_LIST_STATE, lan9645x, TAS_TAS_LIST_STATE);

out:
	mutex_unlock(&lan9645x->qos_lock);

	return err;
}

int lan9645x_taprio_del(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	int err;

	mutex_lock(&lan9645x->qos_lock);
	err = lan9645x_taprio_shutdown_pending(p);
	if (err)
		goto out;

	err = lan9645x_taprio_shutdown_operating(p);
out:
	mutex_unlock(&lan9645x->qos_lock);

	return err;
}

void lan9645x_taprio_init(struct lan9645x *lan9645x)
{
	u32 revisit = (256 * 1000) / lan9645x_ptp_get_period_ps();
	int num_tas_lists;

	num_tas_lists = lan9645x->num_phys_ports * TAS_ENTRIES_PER_PORT;

	mutex_init(&lan9645x->qos_lock);
	lan_wr(TAS_TAS_STATEMACHINE_CFG_REVISIT_DLY_SET(revisit), lan9645x,
	       TAS_TAS_STATEMACHINE_CFG);

	/* For now we always use guard band on all queues */
	lan_rmw(TAS_TAS_CFG_CTRL_LIST_NUM_MAX_SET(num_tas_lists) |
		TAS_TAS_CFG_CTRL_ALWAYS_GUARD_BAND_SCH_Q_SET(1),
		TAS_TAS_CFG_CTRL_LIST_NUM_MAX |
		TAS_TAS_CFG_CTRL_ALWAYS_GUARD_BAND_SCH_Q,
		lan9645x, TAS_TAS_CFG_CTRL);
}

void lan9645x_taprio_deinit(struct lan9645x *lan9645x)
{
	int p;

	for (p = 0; p < lan9645x->num_phys_ports; ++p) {
		if (!lan9645x->ports[p])
			continue;

		lan9645x_taprio_del(lan9645x, p);
	}
}

int lan9645x_taprio_speed_set(struct lan9645x_port *port, int speed)
{
	struct lan9645x *lan9645x = port->lan9645x;
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

	lan_rmw(TAS_TAS_PROFILE_CONFIG_LINK_SPEED_SET(spd),
		TAS_TAS_PROFILE_CONFIG_LINK_SPEED, lan9645x,
		TAS_TAS_PROFILE_CONFIG(port->chip_port));

	return 0;
}
