// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/ptp_clock_kernel.h>

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "lan966x_main.h"
#include "lan966x_ptp.h"
#include "lan966x_vcap_impl.h"

#if defined(ASIC)
/* Represents 1ppm adjustment in 2^59 format with 6.037735849ns as reference
 * The value is calculated as following: (1/1000000)/((2^-59)/6.037735849)
 */
#define LAN966X_1PPM_FORMAT		3480517749723LL

/* Represents 1ppb adjustment in 2^29 format with 6.037735849ns as reference
 * The value is calculated as following: (1/1000000000)/((2^59)/6.037735849)
 */
#define LAN966X_1PPB_FORMAT		3480517749LL
#else
/* Represents 1ppm adjustment in 2^59 format with 15.125ns as reference
 * The value is calculated as following: (1/1000000)/((2^-59)/15.125)
 */
#define LAN966X_1PPM_FORMAT		8718968878589LL

/* Represents 1ppb adjustment in 2^29 format with 15.125ns as reference
 * The value is calculated as following: (1/1000000000)/((2^59)/15.125)
 */
#define LAN966X_1PPB_FORMAT		8718968878
#endif

#define LAN966X_PTP_TRANS_RULE_ID_OFFSET	1024
#define LAN966X_PTP_TRANS_RULES_CNT		3

struct lan966x_ptp_req_perout {
	struct delayed_work work;
	struct workqueue_struct *queue;

	struct lan966x *lan966x;
	int period_ns;
	int start_ns;
	int index;
	struct lan966x_ptp_domain *domain;
};

struct lan966x_ptp_req_input {
	struct delayed_work work;
	struct workqueue_struct *queue;

	struct lan966x *lan966x;
};

struct lan966x_ptp_req_extts {
	struct delayed_work work;
	struct workqueue_struct *queue;

	struct lan966x *lan966x;
};

static struct lan966x_ptp_req_perout lan966x_ptp_req_perout;
static struct lan966x_ptp_req_input lan966x_ptp_req_input;
static struct lan966x_ptp_req_extts lan966x_ptp_req_extts;

/* Returns system clock period in picoseconds */
u32 lan966x_clk_period_ps(struct lan966x *lan966x)
{
#if defined(ASIC)
	return 6038;
#else
	return 15125;
#endif
}

static u64 lan966x_ptp_get_nominal_value(void)
{
#if defined(ASIC)
	u64 res = 0x304d4873ecade305;
#else
	u64 res = 0x79000000;
	res <<= 32;
#endif
	return res;
}

static int lan966x_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct lan966x_ptp_domain *domain = container_of(ptp,
							 struct lan966x_ptp_domain,
							 info);
	struct lan966x *lan966x = domain->lan966x;
	unsigned long flags;
	bool neg_adj = 0;
	u64 tod_inc;
	u64 ref;

	if (!scaled_ppm)
		goto out;

	if (scaled_ppm < 0) {
		neg_adj = 1;
		scaled_ppm = -scaled_ppm;
	}

	tod_inc = lan966x_ptp_get_nominal_value();

	/* The multiplication is split in 2 separate additions because of
	 * overlfow issues. If scaled_ppm with 16bit fractional part was bigger
	 * than 20ppm then we got overflow.
	 */
	ref = LAN966X_1PPM_FORMAT * (scaled_ppm >> 16);
	ref += (LAN966X_1PPM_FORMAT * (0xffff & scaled_ppm)) >> 16;
	tod_inc = neg_adj ? tod_inc - ref : tod_inc + ref;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(1 << BIT(domain->index)),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan966x, PTP_DOM_CFG);

	lan_wr((u32)tod_inc & 0xFFFFFFFF,
	       lan966x, PTP_CLK_PER_CFG(domain->index, 0));
	lan_wr((u32)(tod_inc >> 32),
	       lan966x, PTP_CLK_PER_CFG(domain->index, 1));

	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(0),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan966x, PTP_DOM_CFG);

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);
out:
	return 0;
}

static int lan966x_ptp_settime64(struct ptp_clock_info *ptp,
				 const struct timespec64 *ts)
{
	struct lan966x_ptp_domain *domain = container_of(ptp,
							 struct lan966x_ptp_domain,
							 info);
	struct lan966x *lan966x = domain->lan966x;
	unsigned long flags;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	/* must be in IDLE mode before the time can be loaded */
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
		PTP_PIN_CFG_PIN_DOM_SET(domain->index) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

	/* set new value */
	lan_wr(PTP_TOD_SEC_MSB_TOD_SEC_MSB_SET(upper_32_bits(ts->tv_sec)),
	       lan966x, PTP_TOD_SEC_MSB(TOD_ACC_PIN));
	lan_wr(lower_32_bits(ts->tv_sec),
	       lan966x, PTP_TOD_SEC_LSB(TOD_ACC_PIN));
	lan_wr(ts->tv_nsec, lan966x, PTP_TOD_NSEC(TOD_ACC_PIN));

	/* apply new values */
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_LOAD) |
		PTP_PIN_CFG_PIN_DOM_SET(domain->index) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);

	return 0;
}

int lan966x_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct lan966x_ptp_domain *domain = container_of(ptp,
							 struct lan966x_ptp_domain,
							 info);
	struct lan966x *lan966x = domain->lan966x;
	unsigned long flags;
	time64_t s;
	s64 ns;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		PTP_PIN_CFG_PIN_DOM_SET(domain->index) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

	s = lan_rd(lan966x, PTP_TOD_SEC_MSB(TOD_ACC_PIN));
	s <<= 32;
	s |= lan_rd(lan966x, PTP_TOD_SEC_LSB(TOD_ACC_PIN));
	ns = lan_rd(lan966x, PTP_TOD_NSEC(TOD_ACC_PIN)) &
		PTP_TOD_NSEC_TOD_NSEC;

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);

	/*deal with negative values */
	if ((ns & 0xFFFFFFF0) == 0x3FFFFFF0) {
		s--;
		ns &= 0xf;
		ns += 999999984;
	}

	set_normalized_timespec64(ts, s, ns);
	return 0;
}

static int lan966x_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	if (delta > -(NSEC_PER_SEC / 2) && delta < (NSEC_PER_SEC / 2)) {
		struct lan966x_ptp_domain *domain = container_of(ptp,
							 struct lan966x_ptp_domain,
							 info);
		struct lan966x *lan966x = domain->lan966x;
		unsigned long flags;

		spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

		/* must be in IDLE mode before the time can be loaded */
		lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
			PTP_PIN_CFG_PIN_DOM_SET(domain->index) |
			PTP_PIN_CFG_PIN_SYNC_SET(0),
			PTP_PIN_CFG_PIN_ACTION |
			PTP_PIN_CFG_PIN_DOM |
			PTP_PIN_CFG_PIN_SYNC,
			lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

		lan_wr(PTP_TOD_NSEC_TOD_NSEC_SET(delta),
		       lan966x, PTP_TOD_NSEC(TOD_ACC_PIN));

		/* adjust time with the value of PTP_TOD_NSEC */
		lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_DELTA) |
			PTP_PIN_CFG_PIN_DOM_SET(domain->index) |
			PTP_PIN_CFG_PIN_SYNC_SET(0),
			PTP_PIN_CFG_PIN_ACTION |
			PTP_PIN_CFG_PIN_DOM |
			PTP_PIN_CFG_PIN_SYNC,
			lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

		spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);
	} else {
		/* Fall back using lan966x_ptp_settime64 which is not exact */
		struct timespec64 ts;
		u64 now;

		pr_info("%s %lld\n", __FUNCTION__, delta);

		lan966x_ptp_gettime64(ptp, &ts);

		now = ktime_to_ns(timespec64_to_ktime(ts));
		ts = ns_to_timespec64(now + delta);

		lan966x_ptp_settime64(ptp, &ts);
	}

	return 0;
}

void lan966x_get_hwtimestamp(struct lan966x *lan966x, struct timespec64 *ts,
			     u32 nsec)
{
	/* Read current PTP time to get seconds */
	unsigned long flags;
	u32 curr_nsec;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		PTP_PIN_CFG_PIN_DOM_SET(LAN966X_PTP_PORT_DOMAIN) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

	ts->tv_sec = lan_rd(lan966x, PTP_TOD_SEC_LSB(TOD_ACC_PIN));
	curr_nsec = lan_rd(lan966x, PTP_TOD_NSEC(TOD_ACC_PIN));

	ts->tv_nsec = nsec;

	/* Sec has incremented since the ts was registered */
	if (curr_nsec < nsec)
		ts->tv_sec--;

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);
}

static int lan966x_ptp_pps_idx(struct lan966x *lan966x,
			       int domain_idx,
			       int idx, int on,
			       int period, int delay)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	val = lan_rd(lan966x, PTP_PIN_CFG(idx));

	val &= ~(PTP_PIN_CFG_PIN_ACTION);
	if (on == 0) {
		val |= PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE);
	} else {
		val |= PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_CLOCK);
		val |= PTP_PIN_CFG_PIN_SYNC_SET(3);
	}
	val |= PTP_PIN_CFG_PIN_DOM_SET(domain_idx);
	lan_wr(val, lan966x, PTP_PIN_CFG(idx));

	/* HIGH_PERIOD represents the length of the period */
	lan_wr(period, lan966x, PTP_WF_HIGH_PERIOD(idx));
	/* LOW_PERIOD represents the delay from when pps is trigged */
	lan_wr(delay, lan966x, PTP_WF_LOW_PERIOD(idx));

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);

	return 0;
}

/* Pin 0 on Adaro and Pin 1 are used for input requests. Therefore pins
 * 2,3,4 are used for 1 pps output. Where Pin 2, correspond to domain 0,
 * Pin 3 correspond to domain 1 and Pin 4 coresspond to domain 3
 */
static int lan966x_ptp_pps(struct lan966x *lan966x,
			   struct lan966x_ptp_domain *domain, int on,
			   int period, int delay)
{
	lan966x_ptp_pps_idx(lan966x, domain->index, domain->index + 2,
			    on, period, delay);

	return 0;
}

static void lan966x_ptp_req_input_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan966x_ptp_req_input *input =
		container_of(del_work, struct lan966x_ptp_req_input, work);
	struct lan966x *lan966x = input->lan966x;
	struct timespec64 ts_cap;
	struct timespec64 ts;
	unsigned long flags;
	bool backwards = 0;
	s64 ns_phase;
	u64 tod_inc;
	time64_t s;
	u64 ref;
	s64 ns;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	/* For each 1pps there is an interrupt set */
	if (!(lan_rd(lan966x, PTP_PIN_INTR) & BIT(TOD_INPUT)))
		goto out;

	/* Enable to get the new interrupt. By writting 1 it clears the bit */
	lan_wr(BIT(TOD_INPUT), lan966x, PTP_PIN_INTR);

	/* Get current time */
	s = lan_rd(lan966x, PTP_TOD_SEC_MSB(TOD_INPUT));
	s <<= 32;
	s |= lan_rd(lan966x, PTP_TOD_SEC_LSB(TOD_INPUT));
	ns = lan_rd(lan966x, PTP_TOD_NSEC(TOD_INPUT)) & PTP_TOD_NSEC_TOD_NSEC;

	if ((ns & 0xFFFFFFF0) == 0x3FFFFFF0) {
		s--;
		ns &= 0xf;
		ns += 999999984;
	}

	set_normalized_timespec64(&ts, s, ns);
	set_normalized_timespec64(&ts_cap, s, 0);

	//pr_info("ts: %lld,%09ld ts_cap: %lld,%09ld\n",
	//	ts.tv_sec, ts.tv_nsec,
	//	ts_cap.tv_sec, ts_cap.tv_nsec);

	/* Calculate the difference to the closest second.
	 * The way to calculate, is to take the nsec part of the value
	 * and if it is bigger than 0.5 second it means that the closest second
	 * is the next second, then the difference is 1 sec - nsec otherwise is
	 * the previous second and the difference is the actual nsec part
	 */
	ns_phase = ts.tv_nsec;
	if (ns_phase > (NSEC_PER_SEC / 2)) {
		backwards = 0;
		ns_phase = NSEC_PER_SEC - ns_phase;
	} else {
		backwards = 1;
		ns_phase = ns_phase;
	}

	/* Update phase based on the direction, it is required to add or remove
	 * the nanoseconds
	 */
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
		PTP_PIN_CFG_PIN_DOM_SET(LAN966X_PTP_PORT_DOMAIN) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

	if (backwards)
		lan_wr(PTP_TOD_NSEC_TOD_NSEC_SET(-ns_phase),
		       lan966x, PTP_TOD_NSEC(TOD_ACC_PIN));

	else
		lan_wr(PTP_TOD_NSEC_TOD_NSEC_SET(ns_phase),
		       lan966x, PTP_TOD_NSEC(TOD_ACC_PIN));

	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_DELTA) |
		PTP_PIN_CFG_PIN_DOM_SET(LAN966X_PTP_PORT_DOMAIN) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan966x, PTP_PIN_CFG(TOD_ACC_PIN));

	/* Adjust the frequency only if is small enough, otherwise just jump */
	if (ns_phase < 200000 && ns_phase > -200000) {
		lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(1),
			PTP_DOM_CFG_CLKCFG_DIS,
			lan966x, PTP_DOM_CFG);

		tod_inc = lan_rd(lan966x, PTP_CLK_PER_CFG(0, 1));
		tod_inc <<= 32;
		tod_inc |= lan_rd(lan966x, PTP_CLK_PER_CFG(0, 0));

		ref = LAN966X_1PPB_FORMAT * ns_phase / 2;

		tod_inc = backwards ? tod_inc - ref : tod_inc + ref;

		lan_wr((u32)tod_inc & 0xFFFFFFFF, lan966x, PTP_CLK_PER_CFG(0, 0));
		lan_wr((u32)(tod_inc >> 32), lan966x, PTP_CLK_PER_CFG(0, 1));

		lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(0),
			PTP_DOM_CFG_CLKCFG_DIS,
			lan966x, PTP_DOM_CFG);
	}
out:
	/* Rearm to get the new 1pps input */
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		PTP_PIN_CFG_PIN_DOM_SET(LAN966X_PTP_PORT_DOMAIN) |
		PTP_PIN_CFG_PIN_SYNC_SET(1),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan966x, PTP_PIN_CFG(TOD_INPUT));

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);

	queue_delayed_work(lan966x_ptp_req_input.queue,
			   &lan966x_ptp_req_input.work,
			   msecs_to_jiffies(200));
}

static int lan966x_ptp_in_pps(struct lan966x *lan966x, int on)
{
	lan966x_ptp_req_input.lan966x = lan966x;

	if (on == 1)
		queue_delayed_work(lan966x_ptp_req_input.queue,
				   &lan966x_ptp_req_input.work,
				   msecs_to_jiffies(200));
	else
		cancel_delayed_work(&lan966x_ptp_req_input.work);

	return 0;
}

static void lan966x_ptp_req_perout_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan966x_ptp_req_perout *lan966x_ptp_req_perout =
		container_of(del_work, struct lan966x_ptp_req_perout, work);

	lan966x_ptp_pps_idx(lan966x_ptp_req_perout->lan966x,
			    lan966x_ptp_req_perout->domain->index,
			    lan966x_ptp_req_perout->index,
			    true,
			    lan966x_ptp_req_perout->period_ns,
			    lan966x_ptp_req_perout->start_ns);
}

static int lan966x_ptp_perout(struct lan966x *lan966x,
			      struct lan966x_ptp_domain *domain, int on,
			      struct ptp_perout_request *perout)
{
	if (perout->period.sec != 0) {
		dev_err(lan966x->dev, "Invalid parameter, can't support sec resolution for period\n");
		return -EOPNOTSUPP;
	}

	if (!on)
		return lan966x_ptp_pps_idx(lan966x, on, domain->index,
					   perout->index,
					   perout->period.nsec,
					   perout->start.nsec);

	lan966x_ptp_req_perout.start_ns = perout->start.nsec;
	lan966x_ptp_req_perout.period_ns = perout->period.nsec;
	lan966x_ptp_req_perout.index = perout->index;
	lan966x_ptp_req_perout.lan966x = lan966x;
	lan966x_ptp_req_perout.domain = domain;

	return !queue_delayed_work(lan966x_ptp_req_perout.queue,
				   &lan966x_ptp_req_perout.work,
				   msecs_to_jiffies(perout->start.sec * 1000));
}

static void lan966x_ptp_req_extts_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan966x_ptp_req_extts *extts =
		container_of(del_work, struct lan966x_ptp_req_extts, work);
	struct lan966x *lan966x = extts->lan966x;

	if (lan_rd(lan966x, PTP_PIN_INTR) & BIT(TOD_INPUT))
		lan966x_ptp_extts_handle(lan966x, lan966x->ptp_sync_irq);

	queue_delayed_work(lan966x_ptp_req_extts.queue,
			   &lan966x_ptp_req_extts.work,
			   msecs_to_jiffies(200));
}

int lan966x_ptp_extts_handle(struct lan966x *lan966x, int irq)
{
	struct ptp_clock_event ptp_event = {0};
	unsigned long flags;
	u64 time = 0;
	time64_t s;
	s64 ns;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	/* Enable to get the new interrupt. By writting 1 it clears the bit */
	lan_wr(BIT(TOD_INPUT), lan966x, PTP_PIN_INTR);

	/* Get current time */
	s = lan_rd(lan966x, PTP_TOD_SEC_MSB(TOD_INPUT));
	s <<= 32;
	s |= lan_rd(lan966x, PTP_TOD_SEC_LSB(TOD_INPUT));
	ns = lan_rd(lan966x, PTP_TOD_NSEC(TOD_INPUT)) & PTP_TOD_NSEC_TOD_NSEC;

	if ((ns & 0xFFFFFFF0) == 0x3FFFFFF0) {
		s--;
		ns &= 0xf;
		ns += 999999984;
	}

	time = ktime_set(s, ns);

	ptp_event.index = TOD_INPUT;
	ptp_event.timestamp = time;

	ptp_event.type = PTP_CLOCK_EXTTS;
	ptp_clock_event(lan966x->ptp_domain[LAN966X_PTP_PORT_DOMAIN].clock,
			&ptp_event);

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);

	return IRQ_HANDLED;
}

static int lan966x_ptp_extts(struct lan966x *lan966x,
			     struct ptp_clock_request *rq, int on)
{
	unsigned long flags;
	u32 val;

	if (rq->extts.index != TOD_INPUT)
		return -EINVAL;

	spin_lock_irqsave(&lan966x->ptp_clock_lock, flags);

	/* Configure the pin */
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		PTP_PIN_CFG_PIN_SYNC_SET(on ? 3 : 0) |
		PTP_PIN_CFG_PIN_DOM_SET(LAN966X_PTP_PORT_DOMAIN) |
		PTP_PIN_CFG_PIN_SELECT_SET(TOD_INPUT),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_SYNC |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SELECT,
		lan966x, PTP_PIN_CFG(TOD_INPUT));

	/* Enable interrupts */
	val = lan_rd(lan966x, PTP_PIN_INTR_ENA);
	if (on)
		val |= BIT(TOD_INPUT);
	else
		val &= ~BIT(TOD_INPUT);
	lan_wr(val, lan966x, PTP_PIN_INTR_ENA);

	/* In case we don't have interrupts */
	if (lan966x->ptp_sync_poll) {
		if (on) {
			lan966x_ptp_req_extts.lan966x = lan966x;

			queue_delayed_work(lan966x_ptp_req_extts.queue,
					   &lan966x_ptp_req_extts.work,
					   msecs_to_jiffies(200));
		}
		else {
			cancel_delayed_work(&lan966x_ptp_req_extts.work);
		}
	}

	spin_unlock_irqrestore(&lan966x->ptp_clock_lock, flags);

	return 0;
}

static int lan966x_ptp_enable(struct ptp_clock_info *ptp,
			      struct ptp_clock_request *req, int on)
{
	struct lan966x_ptp_domain *domain = container_of(ptp,
							 struct lan966x_ptp_domain,
							 info);
	struct lan966x *lan966x = domain->lan966x;

	int ret = -EOPNOTSUPP;

	switch (req->type) {
	case PTP_CLK_REQ_PPS:
		ret = lan966x_ptp_pps(lan966x, domain, on, 400, 0);
		break;
	case PTP_CLK_REQ_PEROUT:
		ret = lan966x_ptp_perout(lan966x, domain, on, &req->perout);
		break;
	case PTP_CLK_REQ_IN_PPS:
		ret = lan966x_ptp_in_pps(lan966x, on);
		break;
	case PTP_CLK_REQ_EXTTS:
		ret = lan966x_ptp_extts(lan966x, req, on);
		break;
	default:
		break;
	}

	return ret;
}

static struct ptp_clock_info lan966x_ptp_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "lan966x ptp",
	.max_adj	= 200000,
	.n_alarm	= 0,
	.n_ext_ts	= 1,
	.n_per_out	= 2,
	.n_pins		= 0,
	.pps		= 1,
	.gettime64	= lan966x_ptp_gettime64,
	.settime64	= lan966x_ptp_settime64,
	.adjtime	= lan966x_ptp_adjtime,
	.adjfine	= lan966x_ptp_adjfine,
	.enable		= lan966x_ptp_enable,
};

static void lan966x_ptp_transparent_enable(struct lan966x *lan966x,
					   struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + 0;
	int chain_id = LAN966X_VCAP_CID_IS2_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	/* PTP over Ethernet */
	rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
		  port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + 0;
	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return;

	err = vcap_rule_add_key_bit(vrule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_1);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_ETYPE, ETH_P_1588, ~0);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				     ~BIT(port->chip_port));
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_L2_PAYLOAD0, 0x2, 0xfeff);
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_REW_OP,
					IFH_REW_OP_RESIDENT_PTP);
	err |= vcap_val_rule(vrule, ETH_P_ALL);
	err |= vcap_add_rule(vrule);
	vcap_free_rule(vrule);
	if (err) {
		netdev_err(port->dev,
			   "Unable to add PTP over Ethernet\n");
		return;
	}

	/* PTP over IPv4 UDP dst port 319 */
	rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
		  port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + 1;
	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return;

	err = vcap_rule_add_key_bit(vrule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_1);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 319, ~0);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				     ~BIT(port->chip_port));
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_REW_OP,
					IFH_REW_OP_RESIDENT_PTP);
	err |= vcap_val_rule(vrule, ETH_P_ALL);
	err |= vcap_add_rule(vrule);
	vcap_free_rule(vrule);
	if (err) {
		netdev_err(port->dev,
			   "Unable to add PTP over IPV4\n");
		rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
			  port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + 0;
		vcap_del_rule(port->dev, rule_id);
		return;
	}

	/* PTP over IPv6 UDP dst port 319 */
	rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
		  port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + 2;
	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return;

	err = vcap_rule_add_key_bit(vrule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_1);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 319, ~0);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				     ~BIT(port->chip_port));
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_REW_OP,
					IFH_REW_OP_RESIDENT_PTP);
	err |= vcap_val_rule(vrule, ETH_P_ALL);
	err |= vcap_add_rule(vrule);
	vcap_free_rule(vrule);
	if (err) {
		netdev_err(port->dev,
			   "Unable to add PTP over IPV6\n");
		rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
			  port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + 0;
		vcap_del_rule(port->dev, rule_id);
		rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
			  port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + 1;
		vcap_del_rule(port->dev, rule_id);
		return;
	}
}

static void lan966x_ptp_transparent_disable(struct lan966x *lan966x,
					    struct lan966x_port *port)
{
	u32 rule_id;
	int i, err;

	for (i = 0; i < 3; i++) {
		rule_id = LAN966X_PTP_TRANS_RULE_ID_OFFSET +
			  port->chip_port * LAN966X_PTP_TRANS_RULES_CNT + i;

		err = vcap_del_rule(port->dev, rule_id);
		if (err)
			netdev_err(port->dev,
				   "Unable to disable PTP\n");
	}
}

static void lan966x_ptp_transparent(struct lan966x *lan966x,
				    struct lan966x_port *port, bool enable)
{
	if (enable)
		lan966x_ptp_transparent_enable(lan966x, port);
	else
		lan966x_ptp_transparent_disable(lan966x, port);
}

static struct lan966x *local_lan966x;
static struct proc_dir_entry *proc_ent;
static int lan966x_proc_open_(struct seq_file *f, void *v)
{
	struct lan966x *lan966x = local_lan966x;
	int i = 0;

	for (i = 0; i < lan966x->num_phys_ports; ++i)
		seq_printf(f, "port: %s ptp_trans: %d\n",
			   lan966x->ports[i]->dev->name,
			   lan966x->ports[i]->ptp_trans);

	return 0;
}

static int lan966x_proc_open(struct inode *inode, struct file *f)
{
	return single_open(f, lan966x_proc_open_, NULL);
}

#define TMP_SIZE 10
static ssize_t lan966x_proc_write(struct file *f, const char __user *buff,
				  size_t sz, loff_t *loff)
{
	struct lan966x *lan966x = local_lan966x;
	char tmp[TMP_SIZE];
	u32 port_index;
	u32 enable;
	int ret;

	if (sz > TMP_SIZE)
		return -EINVAL;

	if (copy_from_user(tmp, buff, TMP_SIZE) != 0)
		return -EFAULT;

	ret = sscanf(tmp, "%d %d", &enable, &port_index);
	if (ret != 2)
		return -EINVAL;

	if (port_index >= lan966x->num_phys_ports)
		return -EINVAL;

	lan966x_ptp_transparent(lan966x, lan966x->ports[port_index],
				enable);
	lan966x->ports[port_index]->ptp_trans = enable;

	return sz;
}

static const struct proc_ops proc_ops = {
	.proc_open = lan966x_proc_open,
	.proc_write = lan966x_proc_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release
};

static void lan966x_ptp_domain_init(struct lan966x *lan966x,
				    struct lan966x_ptp_domain *domain,
				    int index,
				    struct ptp_clock_info *clock_info)
{
	lan966x->ptp_domain[index].info = *clock_info;
	lan966x->ptp_domain[index].clock =
		ptp_clock_register(&lan966x->ptp_domain[index].info,
				   lan966x->dev);
	lan966x->ptp_domain[index].index = index;
	lan966x->ptp_domain[index].lan966x = lan966x;
}

int lan966x_timestamp_init(struct lan966x *lan966x)
{
	u64 tod_adj = lan966x_ptp_get_nominal_value();
	int i = 0;

	for (i = 0; i < LAN966X_PTP_DOMAINS; ++i)
		lan966x_ptp_domain_init(lan966x, &lan966x->ptp_domain[i], i,
					&lan966x_ptp_clock_info);

	spin_lock_init(&lan966x->ptp_clock_lock);
	mutex_init(&lan966x->ptp_lock);

	/* disable master counters */
	lan_wr(PTP_DOM_CFG_ENA_SET(0), lan966x, PTP_DOM_CFG);

	/* configure the nominal TOD increment per clock cycle */
	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(0x7),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan966x, PTP_DOM_CFG);

	for (i = 0; i < LAN966X_PTP_DOMAINS; ++i) {
		lan_wr((u32)tod_adj & 0xFFFFFFFF, lan966x, PTP_CLK_PER_CFG(i, 0));
		lan_wr((u32)(tod_adj >> 32), lan966x, PTP_CLK_PER_CFG(i, 1));
	}

	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(0),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan966x, PTP_DOM_CFG);

	/* enable master counters */
	lan_wr(PTP_DOM_CFG_ENA_SET(0x7), lan966x, PTP_DOM_CFG);

	/* There is no device reconfiguration, PTP Rx stamping is always
	 * enabled.
	 */
	lan966x->hwtstamp_config.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;

	/* Init workqueue for perout request */
	lan966x_ptp_req_perout.queue = create_singlethread_workqueue("perout");
	INIT_DELAYED_WORK(&lan966x_ptp_req_perout.work,
			  lan966x_ptp_req_perout_work);
	lan966x_ptp_req_input.queue = create_singlethread_workqueue("input");
	INIT_DELAYED_WORK(&lan966x_ptp_req_input.work,
			  lan966x_ptp_req_input_work);
	lan966x_ptp_req_extts.queue = create_singlethread_workqueue("extts");
	INIT_DELAYED_WORK(&lan966x_ptp_req_extts.work,
			  lan966x_ptp_req_extts_work);

	/* Init proc file to enable/disable transparent clock */
	proc_ent = proc_create_data("lan966x_trans_ptp", 0444, NULL, &proc_ops,
				    lan966x);
	local_lan966x = lan966x;

	return 0;
}

void lan966x_timestamp_deinit(struct lan966x *lan966x)
{
	int i = 0;

	/* Destroy the queue and disable pps */
	cancel_delayed_work(&lan966x_ptp_req_perout.work);
	cancel_delayed_work(&lan966x_ptp_req_input.work);
	cancel_delayed_work(&lan966x_ptp_req_extts.work);
	destroy_workqueue(lan966x_ptp_req_perout.queue);
	destroy_workqueue(lan966x_ptp_req_input.queue);
	destroy_workqueue(lan966x_ptp_req_extts.queue);
	for (i = 0; i < LAN966X_PTP_DOMAINS; ++i) {
		lan966x_ptp_pps(lan966x, &lan966x->ptp_domain[i], false, 0, 0);
		ptp_clock_unregister(lan966x->ptp_domain[i].clock);
	}
}
