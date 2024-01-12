// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <uapi/linux/mrp_bridge.h>
#include <linux/if_bridge.h>

#include "lan966x_afi.h"
#include "lan966x_vcap_utils.h"

#include "lan966x_mrp.h"
#include "lan966x_main.h"

#define MRP_FWD_NOP		0
#define MRP_FWD_COPY		1
#define MRP_FWD_REDIR		2
#define MRP_FWD_DISC		3

#define CONFIG_TEST		0
#define CONFIG_IN_TEST		1

#define CCM_PRIO		0
#define LOC_PERIOD_CNT		7

static const u8 mrp_test_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x1 };
static const u8 mrp_in_test_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x3 };
static const u8 mrp_dmac_mask[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8 };

static void lan966x_mrp_ring_loc_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan966x_mrp *mrp = container_of(del_work, struct lan966x_mrp,
					       ring_loc_work);
	struct lan966x *lan966x = mrp->lan966x;
	u32 val;

	if (!mrp->p_port || !mrp->s_port)
		return;

	if (mrp->p_port->mrp.ring_loc_interrupt) {
		val = lan_rd(lan966x, MEP_TST_CFG(mrp->p_port->chip_port));
		val = MEP_TST_CFG_MISS_CNT_GET(val);

		if (val == mrp->max_miss)
			br_mrp_ring_port_open(mrp->p_port->dev, true);
		else
			br_mrp_ring_port_open(mrp->p_port->dev, false);

		mrp->p_port->mrp.ring_loc_interrupt = false;
	}

	if (mrp->s_port->mrp.ring_loc_interrupt) {
		val = lan_rd(lan966x, MEP_TST_CFG(mrp->s_port->chip_port));
		val = MEP_TST_CFG_MISS_CNT_GET(val);

		if (val == mrp->max_miss)
			br_mrp_ring_port_open(mrp->s_port->dev, true);
		else
			br_mrp_ring_port_open(mrp->s_port->dev, false);

		mrp->s_port->mrp.ring_loc_interrupt = false;
	}
}

void lan966x_mrp_ring_open(struct lan966x *lan966x)
{
	struct lan966x_mrp *mrp;

	/* If this is not an interrupt for MRP then just ignore it */
	if (!MEP_INTR_MRP_INTR_GET(lan_rd(lan966x, MEP_INTR)))
		return;

	list_for_each_entry(mrp, &lan966x->mrp_list, list) {
		u32 secondary = 0;
		u32 primary = 0;

		if (!mrp->p_port || !mrp->s_port)
			continue;

		primary = lan_rd(lan966x, MEP_MRP_STICKY(mrp->p_port->chip_port));
		secondary = lan_rd(lan966x, MEP_MRP_STICKY(mrp->s_port->chip_port));

		if ((MEP_MRP_STICKY_TST_LOC_STICKY_GET(primary)) ||
		    (MEP_MRP_STICKY_TST_LOC_STICKY_GET(secondary))) {

			if (MEP_MRP_STICKY_TST_LOC_STICKY_GET(primary)) {
				lan_rmw(MEP_MRP_STICKY_TST_LOC_STICKY_SET(1),
					MEP_MRP_STICKY_TST_LOC_STICKY,
					lan966x, MEP_MRP_STICKY(mrp->p_port->chip_port));
				mrp->p_port->mrp.ring_loc_interrupt = true;
			}

			if (MEP_MRP_STICKY_TST_LOC_STICKY_GET(secondary)) {
				lan_rmw(MEP_MRP_STICKY_TST_LOC_STICKY_SET(1),
					MEP_MRP_STICKY_TST_LOC_STICKY,
					lan966x, MEP_MRP_STICKY(mrp->s_port->chip_port));
				mrp->s_port->mrp.ring_loc_interrupt = true;
			}

			queue_delayed_work(system_wq, &mrp->ring_loc_work, 0);
		}
	}
}

static void lan966x_mrp_in_loc_rc_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan966x_mrp *mrp = container_of(del_work, struct lan966x_mrp,
					       in_loc_rc_work);
	struct lan966x *lan966x = mrp->lan966x;
	u32 val;

	if (!mrp->p_port || !mrp->s_port || !mrp->i_port)
		return;

	if (mrp->in_state != BR_MRP_IN_STATE_CLOSED)
		return;

	if (mrp->p_port->mrp.in_loc_interrupt) {
		val = lan_rd(lan966x, MEP_ITST_CFG(mrp->p_port->chip_port));
		val = MEP_ITST_CFG_ITST_MISS_CNT_GET(val);

		if (val == mrp->max_miss)
			br_mrp_in_port_open(mrp->p_port->dev, true);
		else
			br_mrp_in_port_open(mrp->p_port->dev, false);

		mrp->p_port->mrp.in_loc_interrupt = false;
	}

	if (!mrp->s_port->mrp.in_loc_interrupt) {
		val = lan_rd(lan966x, MEP_ITST_CFG(mrp->s_port->chip_port));
		val = MEP_ITST_CFG_ITST_MISS_CNT_GET(val);

		if (val == mrp->max_miss)
			br_mrp_in_port_open(mrp->s_port->dev, true);
		else
			br_mrp_in_port_open(mrp->s_port->dev, false);

		mrp->s_port->mrp.in_loc_interrupt = false;
	}

	if (!mrp->i_port->mrp.in_loc_interrupt) {
		val = lan_rd(lan966x, MEP_ITST_CFG(mrp->i_port->chip_port));
		val = MEP_ITST_CFG_ITST_MISS_CNT_GET(val);

		if (val == mrp->max_miss)
			br_mrp_in_port_open(mrp->i_port->dev, true);
		else
			br_mrp_in_port_open(mrp->i_port->dev, false);

		mrp->i_port->mrp.in_loc_interrupt = false;
	}
}

void lan966x_mrp_in_open(struct lan966x *lan966x)
{
	struct lan966x_mrp *mrp;

	/* If this is not an interrupt for MRP then just ignore it */
	if (!MEP_INTR_MRP_INTR_GET(lan_rd(lan966x, MEP_INTR)))
		return;

	list_for_each_entry(mrp, &lan966x->mrp_list, list) {
		u32 interconnect = 0;
		u32 secondary = 0;
		u32 primary = 0;

		if (!mrp->p_port || !mrp->s_port || !mrp->i_port)
			continue;

		primary = lan_rd(lan966x, MEP_MRP_STICKY(mrp->p_port->chip_port));
		secondary = lan_rd(lan966x, MEP_MRP_STICKY(mrp->s_port->chip_port));
		interconnect = lan_rd(lan966x, MEP_MRP_STICKY(mrp->i_port->chip_port));

		if (MEP_MRP_STICKY_ITST_LOC_STICKY_GET(primary) ||
		    MEP_MRP_STICKY_ITST_LOC_STICKY_GET(secondary) ||
		    MEP_MRP_STICKY_ITST_LOC_STICKY_GET(interconnect)) {

			if (MEP_MRP_STICKY_ITST_LOC_STICKY_GET(primary)) {
				lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
					MEP_MRP_STICKY_ITST_LOC_STICKY,
					lan966x, MEP_MRP_STICKY(mrp->p_port->chip_port));
				mrp->p_port->mrp.in_loc_interrupt = true;
			}

			if (MEP_MRP_STICKY_ITST_LOC_STICKY_GET(secondary)) {
				lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
					MEP_MRP_STICKY_ITST_LOC_STICKY,
					lan966x, MEP_MRP_STICKY(mrp->s_port->chip_port));
				mrp->s_port->mrp.in_loc_interrupt = true;
			}

			if (MEP_MRP_STICKY_ITST_LOC_STICKY_GET(interconnect)) {
				lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
					MEP_MRP_STICKY_ITST_LOC_STICKY,
					lan966x, MEP_MRP_STICKY(mrp->i_port->chip_port));
				mrp->i_port->mrp.in_loc_interrupt = true;
			}

			queue_delayed_work(system_wq, &mrp->in_loc_rc_work, 0);
		}
	}
}

static void lan966x_mrp_alloc_loc_period(struct lan966x *lan966x,
					 struct lan966x_mrp *mrp)
{
	u8 i;

	/* Start from index 5, because the index 0-4 is used for CCM frames for
	 * period 3.3ms - 10ms - 100ms - 1s - 10s.
	 * This gives 5 timer for MRP that is also the needed max for
	 * two Interconnected MRPs (four timers) and one Ring MRP (one timer)
	 */
	for (i = 5; i < LOC_PERIOD_CNT; ++i) {
		if (BIT(i) & lan966x->loc_period_mask)
			continue;

		/* In MRP the LOC_PERIOD 0 means that there is no LOC period in
		 * used therefore add 1 to use the index 0 of the LOC period from
		 * MEP
		 */
		mrp->ring_loc_idx = i + 1;
		mrp->in_loc_idx = mrp->ring_loc_idx + 1;
		break;
	}
}

static struct lan966x_mrp *lan966x_mrp_find_in_ring(struct lan966x *lan966x,
						    u32 in_id)
{
	struct lan966x_mrp *mrp;

	list_for_each_entry(mrp, &lan966x->mrp_list, list) {
		if (mrp->in_id == in_id)
			return mrp;
	}

	return NULL;
}

static struct lan966x_mrp *lan966x_mrp_find_ring(struct lan966x *lan966x,
						 u32 ring_id)
{
	struct lan966x_mrp *mrp;

	list_for_each_entry(mrp, &lan966x->mrp_list, list) {
		if (mrp->ring_id == ring_id)
			return mrp;
	}

	/* No entries founded create one */
	mrp = devm_kzalloc(lan966x->dev, sizeof(struct lan966x_mrp),
			   GFP_KERNEL);
	if (!mrp)
		return NULL;

	mrp->ring_id = ring_id;
	mrp->lan966x = lan966x;
	mrp->interval = -1;
	mrp->max_miss = -1;
	mrp->mra_support = false;
	mrp->monitor = false;
	lan966x_mrp_alloc_loc_period(lan966x, mrp);

	INIT_DELAYED_WORK(&mrp->ring_loc_work, lan966x_mrp_ring_loc_work);
	INIT_DELAYED_WORK(&mrp->in_loc_rc_work, lan966x_mrp_in_loc_rc_work);

	list_add_tail(&mrp->list, &lan966x->mrp_list);

	return mrp;
}

static void lan966x_mrp_delete_ring(struct lan966x *lan966x, u32 ring_id)
{
	struct lan966x_mrp *mrp, *tmp;

	list_for_each_entry_safe(mrp, tmp, &lan966x->mrp_list, list) {
		if (mrp->ring_id != ring_id)
			continue;

		cancel_delayed_work_sync(&mrp->ring_loc_work);
		cancel_delayed_work_sync(&mrp->in_loc_rc_work);

		list_del(&mrp->list);
		devm_kfree(lan966x->dev, mrp);

		return;
	}
}

static void lan966x_mrp_port_update_mac(struct lan966x_port *p,
					const u8 mac[ETH_ALEN])
{
	struct lan966x *lan966x = p->lan966x;
	u32 macl = 0, mach = 0;

	mach |= mac[0] << 8;
	mach |= mac[1] << 0;
	macl |= mac[2] << 24;
	macl |= mac[3] << 16;
	macl |= mac[4] << 8;
	macl |= mac[5] << 0;

	lan_wr(macl, lan966x, MEP_MRP_MAC_LSB(p->chip_port));
	lan_wr(mach, lan966x, MEP_MRP_MAC_MSB(p->chip_port));
}

/* All the frames except the Test and IntTest will be redirected to the CPU */
static void lan966x_mrp_mc_control_forwarding(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;
	u32 p = port->chip_port;

	/* All the frames except Test and IntTest frames need to be redirected
	 * to CPU and allow SW to process and forward the frames
	 */
	lan_rmw(MEP_MRP_FWD_CTRL_ERR_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_LD_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_LU_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_TC_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL_SET(2) |
		MEP_MRP_FWD_CTRL_OTHER_FWD_SEL_SET(2),
		MEP_MRP_FWD_CTRL_ERR_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_LD_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_LU_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_TC_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL |
		MEP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL |
		MEP_MRP_FWD_CTRL_OTHER_FWD_SEL,
		lan966x, MEP_MRP_FWD_CTRL(p));
}

static void lan966x_mrp_cpu_redirect_ring_test(struct lan966x_mrp *mrp)
{
	struct lan966x *lan966x = mrp->lan966x;

	/* Redirect test frames with a lower priority to the CPU */
	/* In case the node is in MRM and has support for MRA then in case the
	 * is a test frame with a lower priority the node should send a
	 * TestMgrNAck to tell the remote node to stop sending the frames. The
	 * SW will generate this frame therefore it is required to send these
	 * tests frames to SW so it can detect this scenario. The frames with a
	 * higher priority are not needed to be copy to CPU because the remote
	 * node should send TestMgrNAck and the SW should process this frame and
	 * then terminate the transmitions of Test frames and go in MRC mode
	 */
	if (mrp->p_port) {
		lan_rmw(MEP_TST_CFG_CHK_BEST_MRM_ENA_SET(1) |
			MEP_TST_CFG_CHK_REM_PRIO_ENA_SET(1),
			MEP_TST_CFG_CHK_BEST_MRM_ENA |
			MEP_TST_CFG_CHK_REM_PRIO_ENA,
			lan966x, MEP_TST_CFG(mrp->p_port->chip_port));

		lan_rmw(MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(2),
			MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL,
			lan966x, MEP_TST_FWD_CTRL(mrp->p_port->chip_port));
	}

	if (mrp->s_port) {
		lan_rmw(MEP_TST_CFG_CHK_BEST_MRM_ENA_SET(1) |
			MEP_TST_CFG_CHK_REM_PRIO_ENA_SET(1),
			MEP_TST_CFG_CHK_BEST_MRM_ENA |
			MEP_TST_CFG_CHK_REM_PRIO_ENA,
			lan966x, MEP_TST_CFG(mrp->s_port->chip_port));

		lan_rmw(MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(2),
			MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL,
			lan966x, MEP_TST_FWD_CTRL(mrp->s_port->chip_port));
	}
}

static void lan966x_mrp_terminate_ring_test(struct lan966x_mrp *mrp)
{
	struct lan966x *lan966x = mrp->lan966x;

	/* Terminate frames */
	/* When the frame is discard means that not to forward the frame
	 * to other front ports but if other block enables the copy to the
	 * CPU then the frame will go to CPU. In this case, the multicast
	 * frames are flooded also to the CPU therefor, so the fix consists
	 * of adding entries to MAC table to disable copying of the frames
	 * to CPU
	 */
	if (mrp->p_port) {
		lan_rmw(MEP_TST_FWD_CTRL_REM_FWD_SEL_SET(3) |
			MEP_TST_FWD_CTRL_OWN_FWD_SEL_SET(3) |
			MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(3) |
			MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL_SET(3),
			MEP_TST_FWD_CTRL_REM_FWD_SEL |
			MEP_TST_FWD_CTRL_OWN_FWD_SEL |
			MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL |
			MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL,
			lan966x, MEP_TST_FWD_CTRL(mrp->p_port->chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_test_dmac,
				  mrp->p_port->pvid, ENTRYTYPE_LOCKED);
	}

	if (mrp->s_port) {
		lan_rmw(MEP_TST_FWD_CTRL_REM_FWD_SEL_SET(3) |
			MEP_TST_FWD_CTRL_OWN_FWD_SEL_SET(3) |
			MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(3) |
			MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL_SET(3),
			MEP_TST_FWD_CTRL_REM_FWD_SEL |
			MEP_TST_FWD_CTRL_OWN_FWD_SEL |
			MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL |
			MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL,
			lan966x, MEP_TST_FWD_CTRL(mrp->s_port->chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_test_dmac,
				  mrp->s_port->pvid, ENTRYTYPE_LOCKED);
	}
}

static void lan966x_mrp_forward_ring_test(struct lan966x_mrp *mrp, bool forward,
					  u32 fwd_op)
{
	struct lan966x *lan966x = mrp->lan966x;
	u32 chip_port;
	u32 mask;

	if (mrp->p_port) {
		pr_info("%s\n", __FUNCTION__);

		mask = 0;
		chip_port = mrp->p_port->chip_port;
		if (mrp->s_port)
			mask = BIT(mrp->s_port->chip_port);

		lan_rmw(MEP_RING_MASK_CFG_RING_PORTMASK_SET(mask),
			MEP_RING_MASK_CFG_RING_PORTMASK,
			lan966x, MEP_RING_MASK_CFG(chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_test_dmac,
				  mrp->p_port->pvid, ENTRYTYPE_LOCKED);


		lan_rmw(MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL_SET(fwd_op) |
			MEP_MRP_FWD_CTRL_RING_MASK_ENA_SET(forward ? 1 : 0),
			MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL |
			MEP_MRP_FWD_CTRL_RING_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(chip_port));
	}

	if (mrp->s_port) {
		pr_info("%s\n", __FUNCTION__);

		mask = 0;
		chip_port = mrp->s_port->chip_port;
		if (mrp->p_port)
			mask = BIT(mrp->p_port->chip_port);

		lan_rmw(MEP_RING_MASK_CFG_RING_PORTMASK_SET(mask),
			MEP_RING_MASK_CFG_RING_PORTMASK,
			lan966x, MEP_RING_MASK_CFG(chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_test_dmac,
				  mrp->s_port->pvid, ENTRYTYPE_LOCKED);

		lan_rmw(MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL_SET(fwd_op) |
			MEP_MRP_FWD_CTRL_RING_MASK_ENA_SET(forward ? 1 : 0),
			MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL |
			MEP_MRP_FWD_CTRL_RING_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(chip_port));
	}
}

/* This forwarding is used only when the node process test frames */
static void lan966x_mrp_forward_high_prio_ring_test(struct lan966x_mrp *mrp)
{
	struct lan966x *lan966x = mrp->lan966x;

	if (mrp->p_port) {
		pr_info("%s\n", __FUNCTION__);

		lan_rmw(MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL_SET(0),
			MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL,
			lan966x, MEP_TST_FWD_CTRL(mrp->p_port->chip_port));
	}

	if (mrp->s_port) {
		pr_info("%s\n", __FUNCTION__);

		lan_rmw(MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL_SET(0),
			MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL,
			lan966x, MEP_TST_FWD_CTRL(mrp->s_port->chip_port));
	}
}

static void lan966x_mrp_process_ring_test(struct lan966x_mrp *mrp,
					  bool process)
{
	struct lan966x *lan966x = mrp->lan966x;

	/* Enable processing of the frame */
	if (mrp->p_port)
		lan_rmw(MEP_MRP_CTRL_MRP_TST_ENA_SET(process),
			MEP_MRP_CTRL_MRP_TST_ENA,
			lan966x, MEP_MRP_CTRL(mrp->p_port->chip_port));

	if (mrp->s_port)
		lan_rmw(MEP_MRP_CTRL_MRP_TST_ENA_SET(process),
			MEP_MRP_CTRL_MRP_TST_ENA,
			lan966x, MEP_MRP_CTRL(mrp->s_port->chip_port));

	/* 51 represents 10 usec */
	/* MRP loc index 0 represents that there is no LOC used, while index 1
	 * in MRP represents index 0 in MEP, therefor substract 1
	 */
	lan_wr(mrp->ring_interval / 10 * 51,
	       lan966x, MEP_LOC_PERIOD_CFG(mrp->ring_loc_idx - 1));

	/* Set LOC */
	if (mrp->p_port)
		lan_rmw(MEP_TST_CFG_CLR_MISS_CNT_ENA_SET(process) |
			MEP_TST_CFG_LOC_PERIOD_SET(mrp->ring_loc_idx),
			MEP_TST_CFG_CLR_MISS_CNT_ENA |
			MEP_TST_CFG_LOC_PERIOD,
			lan966x, MEP_TST_CFG(mrp->p_port->chip_port));

	if (mrp->s_port)
		lan_rmw(MEP_TST_CFG_CLR_MISS_CNT_ENA_SET(process) |
			MEP_TST_CFG_LOC_PERIOD_SET(mrp->ring_loc_idx),
			MEP_TST_CFG_CLR_MISS_CNT_ENA |
			MEP_TST_CFG_LOC_PERIOD,
			lan966x, MEP_TST_CFG(mrp->s_port->chip_port));
}

static void lan966x_mrp_rewrite_ring_test(struct lan966x_mrp *mrp, bool update)
{
	struct lan966x *lan966x = mrp->lan966x;

	if (mrp->p_port)
		lan_rmw(REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_SEQ_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_MISC_UPD_SET(update),
			REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD |
			REW_MRP_TX_CFG_MRP_SEQ_UPD |
			REW_MRP_TX_CFG_MRP_MISC_UPD,
			lan966x, REW_MRP_TX_CFG(mrp->p_port->chip_port, CONFIG_TEST));

	if (mrp->s_port)
		lan_rmw(REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_SEQ_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_MISC_UPD_SET(update),
			REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD |
			REW_MRP_TX_CFG_MRP_SEQ_UPD |
			REW_MRP_TX_CFG_MRP_MISC_UPD,
			lan966x, REW_MRP_TX_CFG(mrp->s_port->chip_port, CONFIG_TEST));
}

static void lan966x_mrp_terminate_in_test(struct lan966x_mrp *mrp)
{
	struct lan966x *lan966x = mrp->lan966x;
	u32 mask;

	/* Terminate the frame */
	if (mrp->p_port) {
		/* If the frame came on a ring port and it is from a remote MIM
		 * then it is required to forward the frame only to the other
		 * ring port. If the frame is itself then the miss count should
		 * be clear, this is done by HW.
		 */
		lan_rmw(MEP_ITST_FWD_CTRL_REM_FWD_SEL_SET(0) |
			MEP_ITST_FWD_CTRL_OWN_FWD_SEL_SET(3),
			MEP_ITST_FWD_CTRL_REM_FWD_SEL |
			MEP_ITST_FWD_CTRL_OWN_FWD_SEL,
			lan966x, MEP_ITST_FWD_CTRL(mrp->p_port->chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_in_test_dmac,
				  mrp->p_port->pvid, ENTRYTYPE_LOCKED);

		mask = 0;
		if (mrp->s_port)
			mask = mrp->s_port->chip_port;

		lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
			MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(mrp->p_port->chip_port));

		lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
			MEP_ICON_MASK_CFG_ICON_PORTMASK,
			lan966x, MEP_ICON_MASK_CFG(mrp->p_port->chip_port));
	}

	if (mrp->s_port) {
		/* If the frame came on a ring port and it is from a remote MIM
		 * then it is required to forward the frame only to the other
		 * ring port. If the frame is itself then the miss count should
		 * be clear, this is done by HW.
		 */
		lan_rmw(MEP_ITST_FWD_CTRL_REM_FWD_SEL_SET(0) |
			MEP_ITST_FWD_CTRL_OWN_FWD_SEL_SET(3),
			MEP_ITST_FWD_CTRL_REM_FWD_SEL |
			MEP_ITST_FWD_CTRL_OWN_FWD_SEL,
			lan966x, MEP_ITST_FWD_CTRL(mrp->s_port->chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_in_test_dmac,
				  mrp->s_port->pvid, ENTRYTYPE_LOCKED);

		mask = 0;
		if (mrp->p_port)
			mask = mrp->p_port->chip_port;

		lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
			MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(mrp->s_port->chip_port));

		lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
			MEP_ICON_MASK_CFG_ICON_PORTMASK,
			lan966x, MEP_ICON_MASK_CFG(mrp->s_port->chip_port));
	}

	if (mrp->i_port) {
		/* If the frame came on interconnect port then just drop it but
		 * this case should not happend, if it happens then it is a bug
		 */
		lan_rmw(MEP_ITST_FWD_CTRL_REM_FWD_SEL_SET(3) |
			MEP_ITST_FWD_CTRL_OWN_FWD_SEL_SET(3),
			MEP_ITST_FWD_CTRL_REM_FWD_SEL |
			MEP_ITST_FWD_CTRL_OWN_FWD_SEL,
			lan966x, MEP_ITST_FWD_CTRL(mrp->i_port->chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_in_test_dmac,
				  mrp->i_port->pvid, ENTRYTYPE_LOCKED);

		lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(0),
			MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(mrp->i_port->chip_port));
	}
}

static void lan966x_mrp_forward_in_test(struct lan966x_mrp *mrp)
{
	struct lan966x *lan966x = mrp->lan966x;
	u32 mask;

	if (mrp->p_port) {
		mask = 0;
		if (mrp->s_port)
			mask |= BIT(mrp->s_port->chip_port);
		if (mrp->i_port)
			mask |= BIT(mrp->i_port->chip_port);

		lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
			MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(mrp->p_port->chip_port));

		lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
			MEP_ICON_MASK_CFG_ICON_PORTMASK,
			lan966x, MEP_ICON_MASK_CFG(mrp->p_port->chip_port));
	}

	if (mrp->s_port) {
		mask = 0;
		if (mrp->p_port)
			mask |= BIT(mrp->p_port->chip_port);
		if (mrp->i_port)
			mask |= BIT(mrp->i_port->chip_port);

		lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
			MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(mrp->s_port->chip_port));

		lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
			MEP_ICON_MASK_CFG_ICON_PORTMASK,
			lan966x, MEP_ICON_MASK_CFG(mrp->s_port->chip_port));
	}

	if (mrp->i_port) {
		mask = 0;
		if (mrp->p_port)
			mask |= BIT(mrp->p_port->chip_port);
		if (mrp->s_port)
			mask |= BIT(mrp->s_port->chip_port);

		lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
			MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
			lan966x, MEP_MRP_FWD_CTRL(mrp->i_port->chip_port));

		lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
			MEP_ICON_MASK_CFG_ICON_PORTMASK,
			lan966x, MEP_ICON_MASK_CFG(mrp->i_port->chip_port));
	}
}

static void
lan966x_mrp_forward_in_test_between_ring_ports(struct lan966x_mrp *mrp,
					       bool forward)
{
	struct lan966x *lan966x = mrp->lan966x;
	u32 port_mask;

	port_mask = lan_rd(lan966x, MEP_ICON_MASK_CFG(mrp->p_port->chip_port));
	port_mask = MEP_ICON_MASK_CFG_ICON_PORTMASK_GET(port_mask);
	if (forward)
		port_mask |= BIT(mrp->s_port->chip_port);
	else
		port_mask  &= ~(BIT(mrp->s_port->chip_port));
	lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(port_mask),
		MEP_ICON_MASK_CFG_ICON_PORTMASK,
		lan966x, MEP_ICON_MASK_CFG(mrp->p_port->chip_port));

	port_mask = lan_rd(lan966x, MEP_ICON_MASK_CFG(mrp->s_port->chip_port));
	port_mask = MEP_ICON_MASK_CFG_ICON_PORTMASK_GET(port_mask);
	if (forward)
		port_mask |= BIT(mrp->p_port->chip_port);
	else
		port_mask  &= ~(BIT(mrp->p_port->chip_port));
	lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(port_mask),
		MEP_ICON_MASK_CFG_ICON_PORTMASK,
		lan966x, MEP_ICON_MASK_CFG(mrp->s_port->chip_port));
}

static void lan966x_mrp_process_in_test(struct lan966x_mrp *mrp, bool process)
{
	struct lan966x *lan966x = mrp->lan966x;

	/* Enable processing of the frame */
	if (mrp->p_port)
		lan_rmw(MEP_MRP_CTRL_MRP_ITST_ENA_SET(process),
			MEP_MRP_CTRL_MRP_ITST_ENA,
			lan966x, MEP_MRP_CTRL(mrp->p_port->chip_port));

	if (mrp->s_port)
		lan_rmw(MEP_MRP_CTRL_MRP_ITST_ENA_SET(process),
			MEP_MRP_CTRL_MRP_ITST_ENA,
			lan966x, MEP_MRP_CTRL(mrp->s_port->chip_port));

	if (mrp->i_port)
		lan_rmw(MEP_MRP_CTRL_MRP_ITST_ENA_SET(process),
			MEP_MRP_CTRL_MRP_ITST_ENA,
			lan966x, MEP_MRP_CTRL(mrp->i_port->chip_port));

	/* 51 represents 10 usec */
	/* MRP loc index 0 represents that there is no LOC used, while index 1
	 * in MRP represets index 0 in MEP, therefor substract 1
	 */
	lan_wr(mrp->in_interval / 10 * 51, lan966x,
	       MEP_LOC_PERIOD_CFG(mrp->in_loc_idx - 1));

	/* Set LOC */
	if (mrp->p_port)
		lan_rmw(MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA_SET(process) |
			MEP_ITST_CFG_ITST_LOC_PERIOD_SET(mrp->in_loc_idx),
			MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA |
			MEP_ITST_CFG_ITST_LOC_PERIOD,
			lan966x, MEP_ITST_CFG(mrp->p_port->chip_port));

	if (mrp->s_port)
		lan_rmw(MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA_SET(process) |
			MEP_ITST_CFG_ITST_LOC_PERIOD_SET(mrp->in_loc_idx),
			MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA |
			MEP_ITST_CFG_ITST_LOC_PERIOD,
			lan966x, MEP_ITST_CFG(mrp->s_port->chip_port));

	if (mrp->i_port)
		lan_rmw(MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA_SET(process) |
			MEP_ITST_CFG_ITST_LOC_PERIOD_SET(mrp->in_loc_idx),
			MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA |
			MEP_ITST_CFG_ITST_LOC_PERIOD,
			lan966x, MEP_ITST_CFG(mrp->i_port->chip_port));
}

static void lan966x_mrp_rewrite_in_test(struct lan966x_mrp *mrp, bool update)
{
	struct lan966x *lan966x = mrp->lan966x;

	if (mrp->p_port)
		lan_rmw(REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_SEQ_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_MISC_UPD_SET(update),
			REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD |
			REW_MRP_TX_CFG_MRP_SEQ_UPD |
			REW_MRP_TX_CFG_MRP_MISC_UPD,
			lan966x, REW_MRP_TX_CFG(mrp->p_port->chip_port, CONFIG_IN_TEST));

	if (mrp->s_port)
		lan_rmw(REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_SEQ_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_MISC_UPD_SET(update),
			REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD |
			REW_MRP_TX_CFG_MRP_SEQ_UPD |
			REW_MRP_TX_CFG_MRP_MISC_UPD,
			lan966x, REW_MRP_TX_CFG(mrp->s_port->chip_port, CONFIG_IN_TEST));

	if (mrp->i_port)
		lan_rmw(REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_SEQ_UPD_SET(update) |
			REW_MRP_TX_CFG_MRP_MISC_UPD_SET(update),
			REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD |
			REW_MRP_TX_CFG_MRP_SEQ_UPD |
			REW_MRP_TX_CFG_MRP_MISC_UPD,
			lan966x, REW_MRP_TX_CFG(mrp->i_port->chip_port, CONFIG_IN_TEST));
}

static int lan966x_mrp_afi_alloc_ring_test(struct lan966x_port *src,
					   u32 *slowid)
{
	struct lan966x_afi_slow_inj_alloc_cfg cfg;
	__be32 ifh[IFH_LEN];
	struct sk_buff *skb;
	int ret;

	cfg.port_no = src->chip_port;
	cfg.prio = 0;

	ret = lan966x_afi_slow_inj_alloc(src->dev, &cfg, slowid);
	if (!ret)
		return ret;

	skb = br_mrp_alloc_test(src->dev, src->mrp.role);
	if (!skb)
		return -ENOMEM;

	memset(ifh, 0x0, sizeof(__be32) * IFH_LEN);
	lan966x_ifh_set_bypass(ifh, 1);
	lan966x_ifh_set_afi(ifh, true);
	lan966x_ifh_set_rew_oam(ifh, true);
	lan966x_ifh_set_oam_type(ifh, 2);
	lan966x_ifh_set_timestamp(ifh, 0);
	lan966x_ifh_set_seq_num(ifh, NUM_PHYS_PORTS * 4 + src->chip_port);
	lan966x_ifh_set_port(ifh, BIT_ULL(src->chip_port));

	pr_info("%s flowid: %d chip_port: %d\n",
		__FUNCTION__, *slowid, src->chip_port);

	lan966x_xmit(src, skb, ifh);

	return lan966x_afi_slow_inj_frm_hijack(src->dev, *slowid);
}

static int lan966x_mrp_start_ring_test(struct lan966x_mrp *mrp, u32 interval,
				       u32 max)
{
	struct lan966x_afi_slow_inj_start_cfg cfg;

	pr_info("%s %d max: %d\n", __FUNCTION__, interval, max);

	lan966x_mrp_rewrite_ring_test(mrp, true);

	/* Update the number of miss count frames and disable
	 * the interrupts while stoping/starting the AFI
	 */
	if (mrp->p_port) {
		lan_rmw(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(0),
			MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
			mrp->lan966x, MEP_MRP_INTR_ENA(mrp->p_port->chip_port));

		lan_rmw(MEP_TST_CFG_MAX_MISS_CNT_SET(max),
			MEP_TST_CFG_MAX_MISS_CNT,
			mrp->lan966x, MEP_TST_CFG(mrp->p_port->chip_port));
	}
	if (mrp->s_port) {
		lan_rmw(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(0),
			MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
			mrp->lan966x, MEP_MRP_INTR_ENA(mrp->s_port->chip_port));

		lan_rmw(MEP_TST_CFG_MAX_MISS_CNT_SET(max),
			MEP_TST_CFG_MAX_MISS_CNT,
			mrp->lan966x, MEP_TST_CFG(mrp->s_port->chip_port));
	}

	/* Now start the streams. The interval is in us but the fph represents
	 * frames per hour, so it is needed a conversion
	 */
	cfg.fph = (u64)3600 * (u64)1000 * (u64)1000 / interval;
	mrp->ring_interval = interval;

	/* Start injecting the frames on both ports */
	/* There are cases when SW just changes the interval of the test frames
	 * without stopping the injection. In this case for the AFI to use the
	 * new interval it needs first to stop and then to start again
	 */
	lan966x_afi_slow_inj_stop(mrp->p_port->dev,
				  mrp->p_port->mrp.ring_test_flow);
	lan966x_afi_slow_inj_start(mrp->p_port->dev,
				   mrp->p_port->mrp.ring_test_flow,
				   &cfg);

	lan966x_afi_slow_inj_stop(mrp->s_port->dev,
				  mrp->s_port->mrp.ring_test_flow);
	lan966x_afi_slow_inj_start(mrp->s_port->dev,
				   mrp->s_port->mrp.ring_test_flow,
				   &cfg);

	/* Process the frames */
	lan966x_mrp_process_ring_test(mrp, true);

	/* Stop forwarding the frames. If the register MRP_TST_FWD_SEL has a
	 * value different than 0 when process MRP frames then the frames will
	 * be copy to the CPU. So set it to 0 because it is not needed to CPU
	 */
	lan966x_mrp_forward_ring_test(mrp, false, MRP_FWD_NOP);

	/* Enable back the interrupts */
	if (mrp->p_port) {
		lan_rmw(MEP_TST_CFG_MISS_CNT_SET(0),
			MEP_TST_CFG_MISS_CNT,
			mrp->lan966x, MEP_TST_CFG(mrp->p_port->chip_port));

		lan_rmw(MEP_MRP_STICKY_TST_LOC_STICKY_SET(1),
			MEP_MRP_STICKY_TST_LOC_STICKY,
			mrp->lan966x, MEP_MRP_STICKY(mrp->p_port->chip_port));

		lan_rmw(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(1),
			MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
			mrp->lan966x, MEP_MRP_INTR_ENA(mrp->p_port->chip_port));
	}

	if (mrp->s_port) {
		lan_rmw(MEP_TST_CFG_MISS_CNT_SET(0),
			MEP_TST_CFG_MISS_CNT,
			mrp->lan966x, MEP_TST_CFG(mrp->s_port->chip_port));

		lan_rmw(MEP_MRP_STICKY_TST_LOC_STICKY_SET(1),
			MEP_MRP_STICKY_TST_LOC_STICKY,
			mrp->lan966x, MEP_MRP_STICKY(mrp->s_port->chip_port));

		lan_rmw(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(1),
			MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
			mrp->lan966x, MEP_MRP_INTR_ENA(mrp->s_port->chip_port));
	}

	return 0;
}

static int lan966x_mrp_stop_ring_test(struct lan966x_mrp *mrp)
{
	pr_info("%s\n", __FUNCTION__);

	cancel_delayed_work(&mrp->ring_loc_work);

	lan966x_mrp_process_ring_test(mrp, false);

	if (mrp->p_port)
		lan966x_afi_slow_inj_stop(mrp->p_port->dev,
					  mrp->p_port->mrp.ring_test_flow);

	if (mrp->s_port)
		lan966x_afi_slow_inj_stop(mrp->s_port->dev,
					  mrp->s_port->mrp.ring_test_flow);

	lan966x_mrp_rewrite_ring_test(mrp, false);

	lan966x_mrp_forward_ring_test(mrp, false, MRP_FWD_DISC);

	return 0;
}

static int lan966x_mrp_alloc_ring_test(struct lan966x_mrp *mrp)
{
	lan966x_mrp_afi_alloc_ring_test(mrp->p_port,
					&mrp->p_port->mrp.ring_test_flow);
	lan966x_mrp_afi_alloc_ring_test(mrp->s_port,
					&mrp->s_port->mrp.ring_test_flow);

	return 0;
}

static int lan966x_mrp_free_ring_test(struct lan966x_mrp *mrp)
{
	lan966x_afi_slow_inj_free(mrp->p_port->dev,
				  mrp->p_port->mrp.ring_test_flow);
	lan966x_afi_slow_inj_free(mrp->s_port->dev,
				  mrp->s_port->mrp.ring_test_flow);

	return 0;
}

static int lan966x_mrp_afi_alloc_in_test(struct lan966x_port *src,
					 u32 *slowid)
{
	struct lan966x_afi_slow_inj_alloc_cfg cfg;
	__be32 ifh[IFH_LEN];
	struct sk_buff *skb;
	int ret;

	cfg.port_no = src->chip_port;
	cfg.prio = 0;

	ret = lan966x_afi_slow_inj_alloc(src->dev, &cfg, slowid);
	if (!ret)
		return ret;

	skb = br_mrp_alloc_in_test(src->dev, src->mrp.role);
	if (!skb)
		return -ENOMEM;

	pr_info("%s flowid: %d chip_port: %d\n",
		__FUNCTION__, *slowid, src->chip_port);

	memset(ifh, 0x0, sizeof(__be32) * IFH_LEN);
	lan966x_ifh_set_bypass(ifh, 1);
	lan966x_ifh_set_afi(ifh, true);
	lan966x_ifh_set_rew_oam(ifh, true);
	lan966x_ifh_set_oam_type(ifh, 3);
	lan966x_ifh_set_timestamp(ifh, 0);
	lan966x_ifh_set_seq_num(ifh, NUM_PHYS_PORTS * 4 + src->chip_port);
	lan966x_ifh_set_port(ifh, BIT_ULL(src->chip_port));

	pr_info("%s flowid: %d chip_port: %d\n",
		__FUNCTION__, *slowid, src->chip_port);

	lan966x_xmit(src, skb, ifh);

	return lan966x_afi_slow_inj_frm_hijack(src->dev, *slowid);
}

static int lan966x_mrp_start_in_test(struct lan966x_mrp *mrp, u32 interval,
				     u32 max)
{
	struct lan966x_afi_slow_inj_start_cfg cfg;
	struct phylink_link_state status;

	pr_info("%s\n", __FUNCTION__);

	lan966x_mrp_rewrite_in_test(mrp, true);

	/* Update the number of miss count frames */
	lan_rmw(MEP_ITST_CFG_ITST_MAX_MISS_CNT_SET(max),
		MEP_ITST_CFG_ITST_MAX_MISS_CNT,
		mrp->lan966x, MEP_ITST_CFG(mrp->p_port->chip_port));

	lan_rmw(MEP_ITST_CFG_ITST_MAX_MISS_CNT_SET(max),
		MEP_ITST_CFG_ITST_MAX_MISS_CNT,
		mrp->lan966x, MEP_ITST_CFG(mrp->i_port->chip_port));

	lan_rmw(MEP_ITST_CFG_ITST_MAX_MISS_CNT_SET(max),
		MEP_ITST_CFG_ITST_MAX_MISS_CNT,
		mrp->lan966x, MEP_ITST_CFG(mrp->s_port->chip_port));

	/* Disable the interrupts not to get an interrupt while stoping/starting
	 * the AFI because the LOC counters are continously incrementing
	 */
	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(0),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		mrp->lan966x, MEP_MRP_INTR_ENA(mrp->p_port->chip_port));

	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(0),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		mrp->lan966x, MEP_MRP_INTR_ENA(mrp->s_port->chip_port));

	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(0),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		mrp->lan966x, MEP_MRP_INTR_ENA(mrp->i_port->chip_port));

	/* Now start the streams. The interval is in us but the fph represents
	 * frames per hour, so it is needed a conversion
	 */
	cfg.fph = (u64)3600 * (u64)1000 * (u64)1000 / interval;
	mrp->in_interval = interval;

	memset(&status, 0x0, sizeof(status));
	lan966x_port_status_get(mrp->p_port, &status);
	if (status.link)
		lan966x_afi_slow_inj_start(mrp->p_port->dev,
					   mrp->p_port->mrp.in_test_flow, &cfg);

	memset(&status, 0x0, sizeof(status));
	lan966x_port_status_get(mrp->s_port, &status);
	if (status.link)
		lan966x_afi_slow_inj_start(mrp->s_port->dev,
					   mrp->s_port->mrp.in_test_flow, &cfg);

	memset(&status, 0x0, sizeof(status));
	lan966x_port_status_get(mrp->i_port, &status);
	if (status.link)
		lan966x_afi_slow_inj_start(mrp->i_port->dev,
					   mrp->i_port->mrp.in_test_flow, &cfg);

	/* Process the frames */
	lan966x_mrp_process_in_test(mrp, true);

	/* Now clear any pending interrupt and enable the interrupts again */
	lan_rmw(MEP_ITST_CFG_ITST_MISS_CNT_SET(0),
		MEP_ITST_CFG_ITST_MISS_CNT,
		mrp->lan966x, MEP_ITST_CFG(mrp->p_port->chip_port));

	lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
		MEP_MRP_STICKY_ITST_LOC_STICKY,
		mrp->lan966x, MEP_MRP_STICKY(mrp->p_port->chip_port));

	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		mrp->lan966x, MEP_MRP_INTR_ENA(mrp->p_port->chip_port));

	lan_rmw(MEP_ITST_CFG_ITST_MISS_CNT_SET(0),
		MEP_ITST_CFG_ITST_MISS_CNT,
		mrp->lan966x, MEP_ITST_CFG(mrp->s_port->chip_port));

	lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
		MEP_MRP_STICKY_ITST_LOC_STICKY,
		mrp->lan966x, MEP_MRP_STICKY(mrp->s_port->chip_port));

	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		mrp->lan966x, MEP_MRP_INTR_ENA(mrp->s_port->chip_port));

	lan_rmw(MEP_ITST_CFG_ITST_MISS_CNT_SET(0),
		MEP_ITST_CFG_ITST_MISS_CNT,
		mrp->lan966x, MEP_ITST_CFG(mrp->i_port->chip_port));

	lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
		MEP_MRP_STICKY_ITST_LOC_STICKY,
		mrp->lan966x, MEP_MRP_STICKY(mrp->i_port->chip_port));

	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		mrp->lan966x, MEP_MRP_INTR_ENA(mrp->i_port->chip_port));

	return 0;
}

static int lan966x_mrp_stop_in_test(struct lan966x_mrp *mrp)
{
	pr_info("%s\n", __FUNCTION__);

	cancel_delayed_work(&mrp->in_loc_rc_work);

	lan966x_mrp_process_in_test(mrp, false);

	if (mrp->p_port)
		lan966x_afi_slow_inj_stop(mrp->p_port->dev,
					  mrp->p_port->mrp.in_test_flow);

	if (mrp->s_port)
		lan966x_afi_slow_inj_stop(mrp->s_port->dev,
					  mrp->s_port->mrp.in_test_flow);

	if (mrp->i_port)
		lan966x_afi_slow_inj_stop(mrp->i_port->dev,
					  mrp->i_port->mrp.in_test_flow);

	lan966x_mrp_rewrite_in_test(mrp, false);

	return 0;
}

static int lan966x_mrp_alloc_in_test(struct lan966x_mrp *mrp)
{
	lan966x_mrp_afi_alloc_in_test(mrp->p_port,
				      &mrp->p_port->mrp.in_test_flow);
	lan966x_mrp_afi_alloc_in_test(mrp->s_port,
				      &mrp->s_port->mrp.in_test_flow);
	lan966x_mrp_afi_alloc_in_test(mrp->i_port,
				      &mrp->i_port->mrp.in_test_flow);

	return 0;
}

static int lan966x_mrp_free_in_test(struct lan966x_mrp *mrp)
{
	lan966x_afi_slow_inj_free(mrp->p_port->dev,
				  mrp->p_port->mrp.in_test_flow);
	lan966x_afi_slow_inj_free(mrp->s_port->dev,
				  mrp->s_port->mrp.in_test_flow);
	lan966x_afi_slow_inj_free(mrp->i_port->dev,
				  mrp->i_port->mrp.in_test_flow);

	return 0;
}

static int lan966x_mrp_update_forwarding(struct lan966x_mrp *mrp)
{
	if (mrp->ring_role == BR_MRP_RING_ROLE_DISABLED)
		return 0;

	if (mrp->p_port)
		lan966x_mrp_mc_control_forwarding(mrp->p_port);
	if (mrp->s_port)
		lan966x_mrp_mc_control_forwarding(mrp->s_port);
	if (mrp->i_port)
		lan966x_mrp_mc_control_forwarding(mrp->i_port);

	/* The order is important here. */
	if (mrp->in_role == BR_MRP_IN_ROLE_MIM)
		lan966x_mrp_terminate_in_test(mrp);

	if (mrp->in_role == BR_MRP_IN_ROLE_MIC)
		lan966x_mrp_forward_in_test(mrp);

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRM ||
	    (mrp->ring_role == BR_MRP_RING_ROLE_MRA && !mrp->monitor)) {
		if (!mrp->p_port || !mrp->s_port)
			return 0;

		lan966x_mrp_terminate_ring_test(mrp);

		if (mrp->p_port->mrp.state == BR_MRP_PORT_STATE_BLOCKED ||
		    mrp->s_port->mrp.state == BR_MRP_PORT_STATE_BLOCKED) {
			/* forward in_test frames between ring ports */
			lan966x_mrp_forward_in_test_between_ring_ports(mrp,
								       true);
		} else {
			/* don't forward in_test frames between ring ports */
			lan966x_mrp_forward_in_test_between_ring_ports(mrp,
								       false);
		}

		/* All the frames with a lower priority need to be processed
		 * by the CPU to notify the remote MRA to not send anymore
		 * test frames
		 */
		if (mrp->mra_support)
			lan966x_mrp_cpu_redirect_ring_test(mrp);
	}

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRC ||
	    (mrp->ring_role == BR_MRP_RING_ROLE_MRA && mrp->monitor)) {
		if (!mrp->p_port || !mrp->s_port)
			return 0;

		lan966x_mrp_forward_ring_test(mrp, true, MRP_FWD_NOP);

		if (mrp->in_role == BR_MRP_IN_ROLE_DISABLED) {
			/* forward in_test frames between ring ports */
			lan966x_mrp_forward_in_test_between_ring_ports(mrp,
								       true);
		}

		/* In case there is support for MRA then it needs to check if
		 * test frames are still present, in case there are not in needs
		 * to go in MRM role
		 */
		if (mrp->mra_support) {
			/* Initially MRA starts in MRM mode meaning that it
			 * has a ring_interval that can be used, and then when
			 * it goes to MRC it still needs to look for test frames
			 * with the expected interval. And it needs to notify
			 * the SW in case it didn't receive test frames
			 */
			lan966x_mrp_process_ring_test(mrp, true);

			/* Now that the frames are process and it checks for
			 * priority, the forwarding is decided by the registers
			 * HI/LO_PRIO_FWD_SEL. And it needs to forward frames
			 * with a higher priority on the mrp ports.
			 */
			lan966x_mrp_forward_high_prio_ring_test(mrp);
		}
	}

	return 0;
}

static void lan966x_mrp_update_ring_role(struct lan966x_mrp *mrp,
					 enum br_mrp_ring_role_type role)
{
	if (mrp->ring_role == BR_MRP_RING_ROLE_MRM ||
	    mrp->ring_role == BR_MRP_RING_ROLE_MRA) {
		cancel_delayed_work(&mrp->ring_loc_work);

		lan966x_mrp_stop_ring_test(mrp);
		lan966x_mrp_free_ring_test(mrp);

		/* Stop modifying the Test frames */
		lan966x_mrp_rewrite_ring_test(mrp, false);

		/* Disable processing of Test frames */
		lan966x_mrp_process_ring_test(mrp, false);
	}

	mrp->ring_role = role;

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRM ||
	    mrp->ring_role == BR_MRP_RING_ROLE_MRA) {
		lan966x_mrp_alloc_ring_test(mrp);

		/* Enable processing of Test frames only when it starts to send
		 * frames
		 */
	}
}

static void lan966x_mrp_update_in_role(struct lan966x_mrp *mrp,
					enum br_mrp_in_role_type role)
{
	if (mrp->in_role == BR_MRP_IN_ROLE_MIM) {
		cancel_delayed_work(&mrp->in_loc_rc_work);

		lan966x_mrp_stop_in_test(mrp);
		lan966x_mrp_free_in_test(mrp);

		/* Stop modifying the Int Test frames */
		lan966x_mrp_rewrite_in_test(mrp, false);

		/* Disable processing of Int Test frames */
		lan966x_mrp_process_in_test(mrp, false);
	}

	mrp->in_role = role;

	if (mrp->in_role == BR_MRP_IN_ROLE_MIM) {
		lan966x_mrp_alloc_in_test(mrp);

		/* Enable processing of Int Test frames only when it starts to
		 * send frames
		 */
	}
}

static void lan966x_mrp_init_port(struct lan966x_port *port, u16 prio)
{
	struct lan966x *lan966x = port->lan966x;
	u32 p = port->chip_port;

	/* Enable MEP and LOC_SCAN */
	lan_rmw(MEP_MEP_CTRL_LOC_SCAN_ENA_SET(1) |
		MEP_MEP_CTRL_MEP_ENA_SET(1),
		MEP_MEP_CTRL_LOC_SCAN_ENA |
		MEP_MEP_CTRL_MEP_ENA,
		lan966x, MEP_MEP_CTRL);

	/* Enable MEP to process MRP frames */
	lan_rmw(ANA_OAM_CFG_MRP_ENA_SET(1),
		ANA_OAM_CFG_MRP_ENA,
		lan966x, ANA_OAM_CFG(p));

	lan_rmw(ANA_VCAP_CFG_PAG_VAL_SET(BIT(6)),
		ANA_VCAP_CFG_PAG_VAL,
		lan966x, ANA_VCAP_CFG(p));

	/* Enable MEP to process Y.1731 frames */
	lan_rmw(ANA_OAM_CFG_OAM_CFG_SET(1),
		ANA_OAM_CFG_OAM_CFG,
		lan966x, ANA_OAM_CFG(p));

	/* Activate MRP endpoint */
	lan_rmw(MEP_MRP_CTRL_MRP_ENA_SET(1),
		MEP_MRP_CTRL_MRP_ENA,
		lan966x, MEP_MRP_CTRL(p));

	lan_rmw(MEP_TST_PRIO_CFG_OWN_PRIO_SET(prio),
		MEP_TST_PRIO_CFG_OWN_PRIO,
		lan966x, MEP_TST_PRIO_CFG(port->chip_port));
}

static void lan966x_mrp_uninit_port(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;
	u32 p = port->chip_port;

	/* Disable MEP to process MRP frames */
	lan_rmw(ANA_OAM_CFG_MRP_ENA_SET(0),
		ANA_OAM_CFG_MRP_ENA,
		lan966x, ANA_OAM_CFG(p));

	/* Disactivate MRP endpoint */
	lan_rmw(MEP_MRP_CTRL_MRP_ENA_SET(0),
		MEP_MRP_CTRL_MRP_ENA,
		lan966x, MEP_MRP_CTRL(p));
}

int lan966x_handle_mrp_port_state(struct lan966x_port *port,
				  enum br_mrp_port_state_type state)
{
	struct lan966x *lan966x = port->lan966x;
	u32 port_cfg;
	int i;

	pr_info("%s %s state %d\n", __FUNCTION__, port->dev->name, state);

	port_cfg = lan_rd(lan966x, ANA_PORT_CFG(port->chip_port));

	if (state == BR_MRP_PORT_STATE_FORWARDING) {
		port_cfg |= ANA_PORT_CFG_LEARN_ENA_SET(1);
		lan966x->bridge_fwd_mask |= BIT(port->chip_port);
	} else {
		port_cfg &= ~ANA_PORT_CFG_LEARN_ENA_SET(1);
		lan966x->bridge_fwd_mask &= ~BIT(port->chip_port);
	}

	lan_wr(port_cfg, lan966x, ANA_PORT_CFG(port->chip_port));

	/* apply the bridge_fwd_mask to all the ports part of the bridge */
	for (i = 0; i < lan966x->num_phys_ports; i++) {
		if (lan966x->bridge_fwd_mask & BIT(i))
			/* but don't forward to it's own port */
			lan_rmw(ANA_PGID_PGID_SET((lan966x->bridge_fwd_mask & ~BIT(i)) | BIT(CPU_PORT)),
				ANA_PGID_PGID,
				lan966x, ANA_PGID(i + PGID_SRC));
		else
			lan_rmw(ANA_PGID_PGID_SET(BIT(CPU_PORT)),
				ANA_PGID_PGID,
				lan966x, ANA_PGID(i + PGID_SRC));
	}

	return 0;
}

int lan966x_handle_mrp_port_role(struct lan966x_port *port,
				 enum br_mrp_port_role_type role)
{
	pr_info("%s %s\n", __FUNCTION__, port->dev->name);

	port->mrp.role = role;

	/* Update the port role in HW for test and interconnect test frames
	 * even if the there will not be a interconnect ring. Because these
	 * values will be applied to the frame only if the bit MRO_MISC_UPD_ENA
	 * is set
	 */
	lan_rmw(REW_MRP_TX_CFG_MRP_PORTROLE_SET(role),
		REW_MRP_TX_CFG_MRP_PORTROLE,
		port->lan966x, REW_MRP_TX_CFG(port->chip_port, CONFIG_TEST));

	lan_rmw(REW_MRP_TX_CFG_MRP_PORTROLE_SET(role),
		REW_MRP_TX_CFG_MRP_PORTROLE,
		port->lan966x, REW_MRP_TX_CFG(port->chip_port, CONFIG_IN_TEST));

	return 0;
}

int lan966x_handle_mrp_add(struct lan966x_port *port,
			   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_mrp *mrp = SWITCHDEV_OBJ_MRP(obj);
	struct lan966x_mrp *mrp_instance = {0};
	struct lan966x *lan966x = port->lan966x;

	if (mrp->p_port != port->dev && mrp->s_port != port->dev)
		return 0;

	pr_info("%s: %s %d\n", __FUNCTION__, lan966x->bridge->name,
		mrp->ring_id);

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	mrp_instance->p_port = netdev_priv(mrp->p_port);
	mrp_instance->p_port->mrp.mrp = mrp_instance;
	mrp_instance->p_port->mrp.ring_loc_interrupt = false;
	mrp_instance->p_port->mrp.in_loc_interrupt = false;
	mrp_instance->p_port->mrp.ring_test_flow = -1;
	mrp_instance->p_port->mrp.in_test_flow = -1;
	mrp_instance->p_port->mrp.ring_id = mrp->ring_id;
	lan966x_mrp_init_port(mrp_instance->p_port, mrp->prio);
	lan966x_mrp_port_update_mac(mrp_instance->p_port,
				    lan966x->bridge->dev_addr);

	mrp_instance->s_port = netdev_priv(mrp->s_port);
	mrp_instance->s_port->mrp.mrp = mrp_instance;
	mrp_instance->s_port->mrp.ring_loc_interrupt = false;
	mrp_instance->s_port->mrp.in_loc_interrupt = false;
	mrp_instance->s_port->mrp.ring_test_flow = -1;
	mrp_instance->s_port->mrp.in_test_flow = -1;
	mrp_instance->s_port->mrp.ring_id = mrp->ring_id;
	lan966x_mrp_init_port(mrp_instance->s_port, mrp->prio);
	lan966x_mrp_port_update_mac(mrp_instance->s_port,
				    lan966x->bridge->dev_addr);

	lan966x_add_prio_is1_rule(mrp_instance->p_port, VCAP_USER_MRP,
				  &mrp_instance->p_port->mrp_is1_p_port_rule_id);
	lan966x_add_prio_is1_rule(mrp_instance->s_port, VCAP_USER_MRP,
				  &mrp_instance->s_port->mrp_is1_s_port_rule_id);

	return lan966x_mrp_update_forwarding(mrp_instance);
}

int lan966x_handle_mrp_del(struct lan966x_port *port,
			   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_mrp *mrp = SWITCHDEV_OBJ_MRP(obj);
	struct lan966x_mrp *tmp, *mrp_instance = {0};
	struct lan966x *lan966x = port->lan966x;

	pr_info("%s %s\n", __FUNCTION__, lan966x->bridge->name);

	/* Don't create again the instance if already is deleted. Because we
	 * will just delete it later. In that case just return OK
	 */
	list_for_each_entry(tmp, &lan966x->mrp_list, list) {
		if (tmp->ring_id == mrp->ring_id) {
			mrp_instance = tmp;
			break;
		}
	}

	if (!mrp_instance)
		return 0;

	lan966x_del_prio_is1_rule(mrp_instance->p_port,
				  mrp_instance->p_port->mrp_is1_p_port_rule_id);
	lan966x_del_prio_is1_rule(mrp_instance->s_port,
				  mrp_instance->s_port->mrp_is1_s_port_rule_id);

	if (mrp_instance->i_port)
		lan966x_del_prio_is1_rule(mrp_instance->i_port,
					  mrp_instance->i_port->mrp_is1_i_port_rule_id);

	lan966x_mrp_update_forwarding(mrp_instance);

	lan966x_mrp_uninit_port(mrp_instance->p_port);
	lan966x_mrp_uninit_port(mrp_instance->s_port);

	lan966x_mrp_delete_ring(lan966x, mrp->ring_id);

	return 0;
}

static void lan966x_mrp_mrm_mac(struct lan966x_port *port,
				const u8 mac[ETH_ALEN])
{
	u32 macl = 0, mach = 0;

	pr_info("%s %s\n", __FUNCTION__, port->dev->name);

	mach |= mac[0] << 8;
	mach |= mac[1] << 0;
	macl |= mac[2] << 24;
	macl |= mac[3] << 16;
	macl |= mac[4] << 8;
	macl |= mac[5] << 0;

	lan_wr(MEP_BEST_MAC_MSB_BEST_MAC_MSB_SET(mach),
	       port->lan966x, MEP_BEST_MAC_MSB(port->chip_port));
	lan_wr(macl, port->lan966x, MEP_BEST_MAC_LSB(port->chip_port));
}

int lan966x_handle_mrp_ring_test_add(struct lan966x_port *port,
				     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_test_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	mrp = SWITCHDEV_OBJ_RING_TEST_MRP(obj);
	if (port->mrp.ring_id != mrp->ring_id)
		return 0;

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	mrp_instance->interval = mrp->interval;
	mrp_instance->max_miss = mrp->max_miss;
	mrp_instance->monitor = mrp->monitor;

	if (mrp_instance->ring_role == BR_MRP_RING_ROLE_MRA) {
		lan966x_mrp_mrm_mac(mrp_instance->p_port, mrp->best_mac);
		lan966x_mrp_mrm_mac(mrp_instance->s_port, mrp->best_mac);
	}

	if (mrp_instance->ring_role == BR_MRP_RING_ROLE_MRM ||
	    (mrp_instance->ring_role == BR_MRP_RING_ROLE_MRA && !mrp_instance->monitor)) {
		lan966x_mrp_start_ring_test(mrp_instance, mrp->interval,
					    mrp->max_miss);
	} else  {
		lan966x_mrp_update_forwarding(mrp_instance);
	}

	return 0;
}

int lan966x_handle_mrp_ring_test_del(struct lan966x_port *port,
				     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_test_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	mrp = SWITCHDEV_OBJ_RING_TEST_MRP(obj);
	if (port->mrp.ring_id != mrp->ring_id)
		return 0;

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	pr_info("%s\n", __FUNCTION__);

	return lan966x_mrp_stop_ring_test(mrp_instance);
}

static void lan966x_mrp_port_ring_state(struct lan966x_port* port,
					struct lan966x_mrp *mrp,
					enum br_mrp_ring_state_type state)
{
	pr_info("%s %s\n", __FUNCTION__, port->dev->name);

	lan_rmw(REW_MRP_TX_CFG_MRP_STATE_SET(state),
		REW_MRP_TX_CFG_MRP_STATE,
		port->lan966x, REW_MRP_TX_CFG(port->chip_port, CONFIG_TEST));

	lan_rmw(REW_MRP_TX_CFG_MRP_TRANS_SET(mrp->ring_transitions),
		REW_MRP_TX_CFG_MRP_TRANS,
		port->lan966x, REW_MRP_TX_CFG(port->chip_port, CONFIG_TEST));

	/* In case the ring is closed, it means that a test frame arrived to the
	 * CPU, so allow again the HW to notify the SW when the ring is open
	 */
	if (state == BR_MRP_RING_STATE_CLOSED)
		lan_rmw(MEP_MRP_STICKY_TST_LOC_STICKY_SET(1),
			MEP_MRP_STICKY_TST_LOC_STICKY,
			port->lan966x, MEP_MRP_STICKY(port->chip_port));
}

int lan966x_handle_mrp_ring_state_add(struct lan966x_port *port,
				      const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_state_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	pr_info("%s \n", __func__);

	mrp = SWITCHDEV_OBJ_RING_STATE_MRP(obj);
	if (port->mrp.ring_id != mrp->ring_id)
		return 0;

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	if (mrp_instance->ring_state == mrp->ring_state)
		return 0;

	if (mrp_instance->ring_state == BR_MRP_RING_STATE_CLOSED &&
	    mrp->ring_state == BR_MRP_RING_STATE_OPEN)
		mrp_instance->ring_transitions++;

	mrp_instance->ring_state = mrp->ring_state;

	lan966x_mrp_port_ring_state(mrp_instance->p_port, mrp_instance, mrp->ring_state);
	lan966x_mrp_port_ring_state(mrp_instance->s_port, mrp_instance, mrp->ring_state);

	return 0;
}

int lan966x_handle_mrp_ring_role_add(struct lan966x_port *port,
				     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_role_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	pr_info("%s \n", __func__);

	mrp = SWITCHDEV_OBJ_RING_ROLE_MRP(obj);
	if (port->mrp.ring_id != mrp->ring_id)
		return 0;

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRA)
		mrp_instance->mra_support = true;

	lan966x_mrp_update_ring_role(mrp_instance, mrp->ring_role);

	return lan966x_mrp_update_forwarding(mrp_instance);
}

int lan966x_handle_mrp_ring_role_del(struct lan966x_port *port,
				     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_ring_role_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	pr_info("%s \n", __func__);

	mrp = SWITCHDEV_OBJ_RING_ROLE_MRP(obj);
	if (port->mrp.ring_id != mrp->ring_id)
		return 0;

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	lan966x_mrp_update_ring_role(mrp_instance, mrp->ring_role);

	return lan966x_mrp_update_forwarding(mrp_instance);
}

static void lan966x_mrp_port_in_state(struct lan966x_port* port,
				      struct lan966x_mrp *mrp,
				      enum br_mrp_in_state_type state)
{
	pr_info("%s %s\n", __FUNCTION__, port->dev->name);

	lan_rmw(REW_MRP_TX_CFG_MRP_STATE_SET(state),
		REW_MRP_TX_CFG_MRP_STATE,
		port->lan966x, REW_MRP_TX_CFG(port->chip_port, CONFIG_IN_TEST));

	lan_rmw(REW_MRP_TX_CFG_MRP_TRANS_SET(mrp->in_transitions),
		REW_MRP_TX_CFG_MRP_TRANS,
		port->lan966x, REW_MRP_TX_CFG(port->chip_port, CONFIG_IN_TEST));

	/* In case the ring is closed, it means that a test frame arrived to the
	 * CPU, so allow again the HW to notify the SW when the ring is open
	 */
	if (state == BR_MRP_IN_STATE_CLOSED)
		lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
			MEP_MRP_STICKY_ITST_LOC_STICKY,
			port->lan966x, MEP_MRP_STICKY(port->chip_port));
}



int lan966x_handle_mrp_in_test_add(struct lan966x_port *port,
				   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_test_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	mrp = SWITCHDEV_OBJ_IN_TEST_MRP(obj);
	if (port->mrp.in_id != mrp->in_id)
		return 0;

	mrp_instance = lan966x_mrp_find_in_ring(lan966x, mrp->in_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	pr_info("%s\n", __FUNCTION__);

	return lan966x_mrp_start_in_test(mrp_instance, mrp->interval,
					 mrp->max_miss);
}

int lan966x_handle_mrp_in_test_del(struct lan966x_port *port,
				   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_test_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	mrp = SWITCHDEV_OBJ_IN_TEST_MRP(obj);
	if (port->mrp.in_id != mrp->in_id)
		return 0;

	mrp_instance = lan966x_mrp_find_in_ring(lan966x, mrp->in_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	return lan966x_mrp_stop_in_test(mrp_instance);
}

int lan966x_handle_mrp_in_state_add(struct lan966x_port *port,
				    const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_state_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	mrp = SWITCHDEV_OBJ_IN_STATE_MRP(obj);
	if (port->mrp.in_id != mrp->in_id)
		return 0;

	mrp_instance = lan966x_mrp_find_in_ring(lan966x, mrp->in_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	if (mrp_instance->in_state == mrp->in_state)
		return 0;

	if (mrp_instance->in_state == BR_MRP_IN_STATE_CLOSED &&
	    mrp->in_state == BR_MRP_IN_STATE_OPEN)
		mrp_instance->in_transitions++;

	mrp_instance->in_state = mrp->in_state;

	lan966x_mrp_port_in_state(mrp_instance->p_port, mrp_instance, mrp->in_state);
	lan966x_mrp_port_in_state(mrp_instance->s_port, mrp_instance, mrp->in_state);
	lan966x_mrp_port_in_state(mrp_instance->i_port, mrp_instance, mrp->in_state);

	return 0;
}

int lan966x_handle_mrp_in_role_add(struct lan966x_port *port,
				   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_role_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	mrp = SWITCHDEV_OBJ_IN_ROLE_MRP(obj);
	if (mrp->i_port != port->dev)
		return 0;

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	mrp_instance->in_id = mrp->in_id;
	mrp_instance->i_port = netdev_priv(mrp->i_port);
	mrp_instance->i_port->mrp.mrp = mrp_instance;
	mrp_instance->i_port->mrp.in_loc_interrupt = false;
	mrp_instance->i_port->mrp.ring_loc_interrupt = false;
	mrp_instance->i_port->mrp.ring_test_flow = -1;
	mrp_instance->i_port->mrp.in_test_flow = -1;
	mrp_instance->i_port->mrp.in_id = mrp->in_id;
	lan966x_mrp_port_update_mac(mrp_instance->i_port,
				    lan966x->bridge->dev_addr);

	lan966x_add_prio_is1_rule(mrp_instance->i_port, VCAP_USER_MRP,
				  &mrp_instance->i_port->mrp_is1_i_port_rule_id);

	lan966x_mrp_update_in_role(mrp_instance, mrp->in_role);

	return lan966x_mrp_update_forwarding(mrp_instance);
}

int lan966x_handle_mrp_in_role_del(struct lan966x_port *port,
				   const struct switchdev_obj *obj)
{
	const struct switchdev_obj_in_role_mrp *mrp;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mrp *mrp_instance;

	mrp = SWITCHDEV_OBJ_IN_ROLE_MRP(obj);
	if (mrp->i_port != port->dev)
		return 0;

	mrp_instance = lan966x_mrp_find_ring(lan966x, mrp->ring_id);
	if (!mrp_instance)
		return -EOPNOTSUPP;

	lan966x_mrp_update_in_role(mrp_instance, mrp->in_role);

	return lan966x_mrp_update_forwarding(mrp_instance);
}

void lan966x_mrp_update_mac(struct lan966x *lan966x, const u8 mac[ETH_ALEN])
{
	struct lan966x_mrp *mrp;
	u32 macl = 0, mach = 0;

	mach |= mac[0] << 8;
	mach |= mac[1] << 0;
	macl |= mac[2] << 24;
	macl |= mac[3] << 16;
	macl |= mac[4] << 8;
	macl |= mac[5] << 0;

	list_for_each_entry(mrp, &lan966x->mrp_list, list) {
		if (mrp->p_port) {
			lan966x_mrp_port_update_mac(mrp->p_port, mac);
			lan966x_mrp_mrm_mac(mrp->p_port, mac);
		}
		if (mrp->s_port) {
			lan966x_mrp_port_update_mac(mrp->s_port, mac);
			lan966x_mrp_mrm_mac(mrp->s_port, mac);
		}
		if (mrp->i_port) {
			lan966x_mrp_port_update_mac(mrp->i_port, mac);
			lan966x_mrp_mrm_mac(mrp->i_port, mac);
		}
	}
}

void lan966x_mrp_init(struct lan966x *lan966x)
{
	struct lan966x_port *port;
	u32 chip_port;
	int i;

	INIT_LIST_HEAD(&lan966x->mrp_list);
	lan966x->loc_period_mask = 0x0;

	/* Forward all MRP frames - by default */
	for (i = 0; i < lan966x->num_phys_ports; ++i) {
		port = lan966x->ports[i];
		if (!port)
			continue;

		chip_port = port->chip_port;

		lan_rmw(MEP_MRP_FWD_CTRL_ERR_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_LD_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_LU_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_TC_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_ITST_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL_SET(0) |
			MEP_MRP_FWD_CTRL_OTHER_FWD_SEL_SET(0),
			MEP_MRP_FWD_CTRL_ERR_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_TPM_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_LD_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_LU_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_TC_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_ITST_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_ITC_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_ILD_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_ILU_FWD_SEL |
			MEP_MRP_FWD_CTRL_MRP_ILSP_FWD_SEL |
			MEP_MRP_FWD_CTRL_OTHER_FWD_SEL,
			lan966x, MEP_MRP_FWD_CTRL(chip_port));

		lan966x_mac_learn(lan966x, PGID_MRP, mrp_test_dmac,
				  port->pvid, ENTRYTYPE_LOCKED);

		lan_wr(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(1) |
		       MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
		       lan966x, MEP_MRP_INTR_ENA(chip_port));
	}

	lan_rmw(MEP_INTR_CTRL_OAM_MEP_INTR_ENA_SET(1),
		MEP_INTR_CTRL_OAM_MEP_INTR_ENA,
		lan966x, MEP_INTR_CTRL);

	/* Add PGID entry to discard all the frames */
	lan_rmw(0, ANA_PGID_PGID, lan966x, ANA_PGID(PGID_MRP));
}

void lan966x_mrp_uninit(struct lan966x *lan966x)
{
	/* Nothing to do here */
}
