// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"
#include "lan9645x_regs.h"

/*
 * Per-Stream Filtering and Policing (PSFP) Overview
 * -------------------------------------------------
 * PSFP, defined in IEEE 802.1Qci, provides per-stream ingress policing and
 * gating to protect the network against misbehaving or malicious traffic.
 * It operates on identified streams (based on VLAN, MAC, or other filters)
 * and applies two primary mechanisms:
 *
 *   - Stream Filter (SFI): Checks incoming frames against criteria such as
 *     maximum SDU size. Frames may be blocked if they exceed limits or if
 *     oversize blocking is enabled.
 *
 *   - Stream Gate (SGI): Applies time-based gating to the stream using a
 *     Gate Control List (GCL). Each GCL entry specifies:
 *       * Gate state (open/closed)
 *       * Interval duration
 *       * Optional priority (IPV) and maximum octet budget
 *
 * Together, SFIs and SGIs ensure that traffic conforms to policy and only
 * passes during permitted time windows.
 *
 * Hardware Model
 * --------------
 * Each device maintains:
 *   - A fixed pool of Stream Filters (SFIs) and Stream Gates (SGIs)
 *   - Control registers to program basetime, cycletime, and gate entries
 *   - Tables for maximum SDU length, oversize frame blocking, and forced
 *     blocking behavior
 *
 * The driver is responsible for:
 *   - Allocating free SFI and SGI indices from global masks
 *   - Programming stream filter rules (oversize enable, max_sdu, etc.)
 *   - Building the relative intervals for the gate schedule
 *   - Converting base_time into absolute hardware time
 *   - Programming GCL entries with gate state, IPV, and max octets
 *   - Triggering a configuration change and waiting for completion
 *
 * Stream Gate State Machine
 * -------------------------
 * A stream gate follows a simpler model than TAS:
 *
 *   - ADMIN       : Gate is disabled, registers can be safely programmed.
 *
 *   - CONFIGURING : A new gate schedule is being written by the driver.
 *
 *   - OPERATING   : Gate is active and cycles through the programmed GCL.
 *
 * Normal workflow:
 *     1. Reserve an SGI index with lan9645x_sgi_get()
 *     2. Compute basetime using lan9645x_new_base_time()
 *     3. Program intervals, gate states, IPV, and budgets with
 *        lan9645x_psfp_sg_set()
 *     4. Start the gate by setting CONFIG_CHANGE in ANA_SG_ACCESS_CTRL
 *     5. Release the SGI index with lan9645x_sgi_put() when done
 *
 * Stream filters (SFIs) follow a similar lifecycle, using
 * lan9645x_sfi_get(), lan9645x_psfp_sf_set(), and lan9645x_sfi_put().
 *
 * Driver Workflow
 * ---------------
 * 1. Validate the PSFP configuration (filter size, gate entries, cycle time)
 * 2. Allocate SFI and/or SGI indices
 * 3. Program SFI rules (oversize frame handling, max_sdu)
 * 4. Compute gate basetime and intervals
 * 5. Program SGI with GCL entries
 * 6. Trigger CONFIG_CHANGE and wait for hardware to complete
 * 7. Release indices and reset configuration when no longer needed
 */

#define SFID_UPDATE_SLEEP_US       10
#define SFID_UPDATE_TIMEOUT_US 100000
#define SFIDACCESS_CMD_IDLE         0
#define SFIDACCESS_CMD_READ         1
#define SFIDACCESS_CMD_WRITE        2
#define SFIDACCESS_CMD_INIT         3

#define SGID_UPDATE_SLEEP_US       10
#define SGID_UPDATE_TIMEOUT_US 100000
#define SGID_IPS_VALID_BIT        0x8

static inline int lan9645x_sfid_get_status(struct lan9645x *lan9645x)
{
	return lan_rd(lan9645x, ANA_SFIDACCESS);
}

static inline int lan9645x_sfid_wait_for_completion(struct lan9645x *lan9645x)
{
	u32 val;

	return readx_poll_timeout(lan9645x_sfid_get_status,
				  lan9645x, val,
				  (ANA_SFIDACCESS_SFID_TBL_CMD_GET(val)) ==
				  SFIDACCESS_CMD_IDLE,
				  SFID_UPDATE_SLEEP_US,
				  SFID_UPDATE_TIMEOUT_US);
}

int lan9645x_psfp_sf_set(struct lan9645x *lan9645x, const u32 sfi_ix,
			 const struct lan9645x_psfp_sf_cfg *const c)
{
	dev_dbg(lan9645x->dev, "sfi_ix %u boe %d bo %d fb %d ms %u\n",
		sfi_ix,
		c->block_oversize_ena,
		c->block_oversize,
		c->force_block,
		c->max_sdu);

	if (sfi_ix >= LAN9645X_PSFP_NUM_SFI) {
		dev_err(lan9645x->dev, "Invalid sfi_ix %u\n", sfi_ix);
		return -EINVAL;
	}

	/* Select the stream filter to configure */
	lan_wr(ANA_SFIDTIDX_SFID_INDEX_SET(sfi_ix), lan9645x, ANA_SFIDTIDX);

	lan_wr(ANA_SFIDACCESS_B_O_FRM_SET(c->block_oversize) |
	       ANA_SFIDACCESS_B_O_FRM_ENA_SET(c->block_oversize_ena) |
	       ANA_SFIDACCESS_MAX_SDU_LEN_SET(c->max_sdu) |
	       ANA_SFIDACCESS_SFID_TBL_CMD_SET(SFIDACCESS_CMD_WRITE),
	       lan9645x, ANA_SFIDACCESS);

	return lan9645x_sfid_wait_for_completion(lan9645x);
}

static int lan9645x_psfp_sf_reset(struct lan9645x *lan9645x, const u32 sfi_ix)
{
	dev_dbg(lan9645x->dev, "sfi_ix %u\n", sfi_ix);

	/* Select the stream filter to configure and write zeroes */
	lan_wr(ANA_SFIDTIDX_SFID_INDEX_SET(sfi_ix),
	       lan9645x, ANA_SFIDTIDX);

	lan_wr(ANA_SFIDACCESS_SFID_TBL_CMD_SET(SFIDACCESS_CMD_WRITE),
	       lan9645x, ANA_SFIDACCESS);

	return lan9645x_sfid_wait_for_completion(lan9645x);
}

static inline int lan9645x_sgid_get_status(struct lan9645x *lan9645x)
{
	return lan_rd(lan9645x, ANA_SG_ACCESS_CTRL);
}

static inline int lan9645x_sgid_wait_for_completion(struct lan9645x *lan9645x)
{
	u32 val;

	return readx_poll_timeout(lan9645x_sgid_get_status,
				  lan9645x,
				  val,
				  !(ANA_SG_ACCESS_CTRL_CONFIG_CHANGE_GET(val)),
				  SGID_UPDATE_SLEEP_US,
				  SGID_UPDATE_TIMEOUT_US);
}

int lan9645x_psfp_sg_set(struct lan9645x *lan9645x, const u32 sgi_ix,
			 const struct lan9645x_psfp_sg_cfg *const sg)
{
	u32 relative_time_interval[LAN9645X_PSFP_NUM_GCE] = {0};
	u32 accumulated_time_interval = 0;
	struct timespec64 ts;
	ktime_t basetime;
	int i, ret = 0;
	u32 ipv = 0;

	dev_dbg(lan9645x->dev, "sgi_ix %u ipv %d bt %llu ct %u cte %u gl %u\n",
		sgi_ix, sg->ipv, sg->basetime, sg->cycletime, sg->cycletimeext,
		sg->num_entries);

	for (i = 0; i < sg->num_entries; i++) {
		accumulated_time_interval += sg->gce[i].interval;
		relative_time_interval[i] = accumulated_time_interval;
	}

	lan9645x_new_base_time(lan9645x, sg->cycletime, sg->basetime,
			       &basetime);

	ts = ktime_to_timespec64(basetime);

	if (sg->ipv >= 0)
		ipv = sg->ipv | SGID_IPS_VALID_BIT;
	else
		ipv = 0;

	/* Select stream gate */
	lan_wr(ANA_SG_ACCESS_CTRL_SGID_SET(sgi_ix),
	       lan9645x, ANA_SG_ACCESS_CTRL);

	/* Set all sg registers */
	lan_wr(ts.tv_nsec, lan9645x, ANA_SG_CFG_1);
	lan_wr(ts.tv_sec & 0xffffffff, lan9645x, ANA_SG_CFG_2);
	lan_wr(ANA_SG_CFG_3_BASE_TIME_SEC_MSB_SET(ts.tv_sec >> 32) |
	       ANA_SG_CFG_3_LIST_LENGTH_SET(sg->num_entries) |
	       ANA_SG_CFG_3_GATE_ENABLE_SET(1) |
	       ANA_SG_CFG_3_INIT_IPS_SET(ipv) |
	       ANA_SG_CFG_3_INIT_GATE_STATE_SET(sg->gate_state),
	       lan9645x, ANA_SG_CFG_3);
	lan_wr(sg->cycletime, lan9645x, ANA_SG_CFG_4);
	lan_wr(sg->cycletimeext, lan9645x, ANA_SG_CFG_5);

	/* Set all gcl registers */
	for (i = 0; i < sg->num_entries; i++) {
		if (sg->gce[i].ipv >= 0)
			ipv = sg->gce[i].ipv | SGID_IPS_VALID_BIT;
		else
			ipv = 0;

		lan_wr(ANA_SG_GCL_GS_CFG_IPS_SET(ipv) |
		       ANA_SG_GCL_GS_CFG_GATE_STATE_SET(sg->gce[i].gate_state),
		       lan9645x, ANA_SG_GCL_GS_CFG(i));

		lan_wr(relative_time_interval[i],
		       lan9645x, ANA_SG_GCL_TI_CFG(i));

		lan_wr(max(sg->gce[i].maxoctets, 0),
		       lan9645x, ANA_SG_GCL_OCT_CFG(i));
	}

	/* Start configuration change */
	lan_wr(ANA_SG_ACCESS_CTRL_SGID_SET(sgi_ix) |
	       ANA_SG_ACCESS_CTRL_CONFIG_CHANGE_SET(1),
	       lan9645x, ANA_SG_ACCESS_CTRL);

	ret = lan9645x_sgid_wait_for_completion(lan9645x);
	if (ret)
		dev_err(lan9645x->dev, "sgi %u: Config change timeout\n", sgi_ix);

	return ret;
}

/* Reset PSFP Stream Gate */
static int lan9645x_psfp_sg_reset(struct lan9645x *lan9645x, const u32 sgi_ix)
{
	int i;

	dev_dbg(lan9645x->dev, "sgi_ix %u\n", sgi_ix);

	/* Select stream gate */
	lan_wr(ANA_SG_ACCESS_CTRL_SGID_SET(sgi_ix), lan9645x, ANA_SG_ACCESS_CTRL);

	/* Set all stream gate registers to default values */
	lan_wr(0, lan9645x, ANA_SG_CFG_1);
	lan_wr(0, lan9645x, ANA_SG_CFG_2);
	lan_wr(ANA_SG_CFG_3_INIT_GATE_STATE, lan9645x, ANA_SG_CFG_3);
	lan_wr(0, lan9645x, ANA_SG_CFG_4);
	lan_wr(0, lan9645x, ANA_SG_CFG_5);
	for (i = 0; i < LAN9645X_PSFP_NUM_GCE; i++) {
		lan_wr(0, lan9645x, ANA_SG_GCL_GS_CFG(i));
		lan_wr(0, lan9645x, ANA_SG_GCL_TI_CFG(i));
		lan_wr(0, lan9645x, ANA_SG_GCL_OCT_CFG(i));
	}

	return 0;
}

int lan9645x_sfi_get(struct lan9645x *lan9645x, u32 *sfi_ix)
{
	u32 ix = find_first_zero_bit(lan9645x->sfi_idx_mask,
				     LAN9645X_PSFP_NUM_SFI);
	if (ix == LAN9645X_PSFP_NUM_SFI)
		return -ENOSPC;

	set_bit(ix, lan9645x->sfi_idx_mask);
	*sfi_ix = ix;

	dev_dbg(lan9645x->dev, "reserve sfi_ix %u\n", *sfi_ix);

	return 0;
}

int lan9645x_sfi_put(struct lan9645x *lan9645x, u32 sfi_ix)
{
	dev_dbg(lan9645x->dev, "release sfi_ix %u\n", sfi_ix);

	if (sfi_ix >= LAN9645X_PSFP_NUM_SFI)
		return -EINVAL;

	if (!test_and_clear_bit(sfi_ix, lan9645x->sfi_idx_mask))
		return -EINVAL;

	dev_dbg(lan9645x->dev, "Disable stream filter %d\n", sfi_ix);
	return lan9645x_psfp_sf_reset(lan9645x, sfi_ix);
}

int lan9645x_sgi_get(struct lan9645x *lan9645x, u32 *sgi_ix)
{
	u32 ix = find_first_zero_bit(lan9645x->sgi_idx_mask,
				      LAN9645X_PSFP_NUM_SGI);
	if (ix == LAN9645X_PSFP_NUM_SGI)
		return -ENOSPC;

	set_bit(ix, lan9645x->sgi_idx_mask);
	*sgi_ix = ix;

	dev_dbg(lan9645x->dev, "reserve sgi_ix %u\n", *sgi_ix);

	return 0;
}

int lan9645x_sgi_put(struct lan9645x *lan9645x, u32 sgi_ix)
{
	dev_dbg(lan9645x->dev, "release sgi_ix %u\n", sgi_ix);

	if (sgi_ix >= LAN9645X_PSFP_NUM_SGI)
		return -EINVAL;

	if (!test_and_clear_bit(sgi_ix, lan9645x->sgi_idx_mask))
		return -EINVAL;

	dev_dbg(lan9645x->dev, "Disable stream gate %d\n", sgi_ix);

	return lan9645x_psfp_sg_reset(lan9645x, sgi_ix);
}
