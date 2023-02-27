// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <uapi/linux/cfm_bridge.h>
#include <linux/if_bridge.h>

#include "lan966x_afi.h"
#include "lan966x_vcap_impl.h"
#include "lan966x_vcap_utils.h"

#include "lan966x_cfm.h"

#define PRIO_CNT 8
#define MEP_CCM_MEGID_CFG_REPLICATION 12

static u32 interval_to_period(enum switchdev_cfm_ccm_interval p)
{
	switch (p) {
	case SWITCHDEV_CFM_CCM_INTERVAL_3_3_MS:
		return 1;
	case SWITCHDEV_CFM_CCM_INTERVAL_10_MS:
		return 2;
	case SWITCHDEV_CFM_CCM_INTERVAL_100_MS:
		return 3;
	case SWITCHDEV_CFM_CCM_INTERVAL_1_SEC:
		return 4;
	case SWITCHDEV_CFM_CCM_INTERVAL_10_SEC:
		return 5;
	default:
		return 4;
	}
}

static u64 interval_to_micro(enum switchdev_cfm_ccm_interval p)
{
	switch (p) {
	case SWITCHDEV_CFM_CCM_INTERVAL_3_3_MS:
		return 3300;
	case SWITCHDEV_CFM_CCM_INTERVAL_10_MS:
		return 10000;
	case SWITCHDEV_CFM_CCM_INTERVAL_100_MS:
		return 100000;
	case SWITCHDEV_CFM_CCM_INTERVAL_1_SEC:
		return 1000000;
	case SWITCHDEV_CFM_CCM_INTERVAL_10_SEC:
		return 10000000;
	default:
		return 1000000;
	}
}

void lan966x_cfm_init(struct lan966x *lan966x)
{
	u32 i, clk_period_in_ps, base_tick_ps, reminder;
	u64 value;
	u32 mac;

	INIT_HLIST_HEAD(&lan966x->mep_list);

	/* All VOEs are default enabled by hardware, we disable them here. */
	for (i = 0; i < lan966x->num_phys_ports; ++i)
		lan_wr(0, lan966x, MEP_BASIC_CTRL(i));

	/* Enable master interrupt */
	lan_rmw(MEP_INTR_CTRL_OAM_MEP_INTR_ENA_SET(1),
		MEP_INTR_CTRL_OAM_MEP_INTR_ENA,
		lan966x, MEP_INTR_CTRL);

	/* Configure LOC base tick count */
	clk_period_in_ps = lan966x_ptp_get_period_ps();
	base_tick_ps = 200 * 1000;    /* Base tick target is 200 ns */
	value = base_tick_ps / clk_period_in_ps; /* clk_period * value = base_tick */
	lan_rmw(MEP_LOC_CTRL_BASE_TICK_CNT_SET(value),
		MEP_LOC_CTRL_BASE_TICK_CNT,
		lan966x, MEP_LOC_CTRL);
	base_tick_ps = clk_period_in_ps * value;

	/* Configure LOC periods used for CCM LOC. Note that MRP is using the last five LOC timers */
	value = div_u64_rem(3300000000LLU, base_tick_ps, &reminder);
	value += ((reminder) ? 1 : 0);
	lan_wr(value, lan966x, MEP_LOC_PERIOD_CFG(0));
	value = div_u64_rem(10000000000, base_tick_ps, &reminder);
	value += ((reminder) ? 1 : 0);
	lan_wr(value, lan966x, MEP_LOC_PERIOD_CFG(1));
	value = div_u64_rem(100000000000, base_tick_ps, &reminder);
	value += ((reminder) ? 1 : 0);
	lan_wr(value, lan966x, MEP_LOC_PERIOD_CFG(2));
	value = div_u64_rem(1000000000000, base_tick_ps, &reminder);
	value += ((reminder) ? 1 : 0);
	lan_wr(value, lan966x, MEP_LOC_PERIOD_CFG(3));
	value = div_u64_rem(10000000000000, base_tick_ps, &reminder);
	value += ((reminder) ? 1 : 0);
	lan_wr(value, lan966x, MEP_LOC_PERIOD_CFG(4));

	/* Configure analyzer to default mark OAM as untagged */
	for (i = 0; i < lan966x->num_phys_ports; ++i)
		lan_wr(ANA_OAM_CFG_OAM_CFG_SET(1),
		       lan966x, ANA_OAM_CFG(i));

	/* Multicast MAC configuration */
	value = (0x01 << 8) | 0x80;
	lan_wr(MEP_MC_MAC_MSB_MEP_MC_MAC_MSB_SET(value),
	       lan966x, MEP_MC_MAC_MSB);

	mac = (0xC2 << 24) | (0x00 << 16) | (0x00 <<  8) | 0x30;
	mac >>= 4;   /* Value in reg. field doesn't include the lower 4 bits */
	lan_wr(MEP_MC_MAC_LSB_MEP_MC_MAC_LSB_SET(mac),
	       lan966x, MEP_MC_MAC_LSB);

	/* Enable VOP */
	lan_wr(MEP_MEP_CTRL_MEP_ENA_SET(1) |
	       MEP_MEP_CTRL_LOC_SCAN_ENA_SET(1),
	       lan966x, MEP_MEP_CTRL);
}

void lan966x_cfm_uninit(struct lan966x *lan966x)
{
	u32 i;
	/* All VOEs are disabled. */
	for (i = 0; i < lan966x->num_phys_ports; ++i) {
		lan_wr(0, lan966x, MEP_BASIC_CTRL(i));
		/* Disable CCM handling */
		lan_rmw(MEP_HW_CTRL_CCM_ENA_SET(0),
			MEP_HW_CTRL_CCM_ENA,
			lan966x, MEP_HW_CTRL(i));
		/* Disable LOC interrupt */
		lan_rmw(MEP_INTR_ENA_CCM_LOC_INTR_ENA_SET(0),
			MEP_INTR_ENA_CCM_LOC_INTR_ENA,
			lan966x, MEP_INTR_ENA(i));
	}

	/* Disable master interrupt */
	lan_rmw(MEP_INTR_CTRL_OAM_MEP_INTR_ENA_SET(0),
		MEP_INTR_CTRL_OAM_MEP_INTR_ENA,
		lan966x, MEP_INTR_CTRL);

	/* Disable VOP */
	lan_wr(MEP_MEP_CTRL_MEP_ENA_SET(0) |
	       MEP_MEP_CTRL_LOC_SCAN_ENA_SET(0),
	       lan966x, MEP_MEP_CTRL);

}

static struct lan966x_mep *mep_find(struct lan966x *lan966x, u32 instance)
{
	struct lan966x_mep *mep;

	hlist_for_each_entry(mep, &lan966x->mep_list, head) {
		if (mep->instance == instance)
			return mep;
	}

	return NULL;
}

static struct lan966x_mep *voe_mep_find(struct lan966x *lan966x, u32 voe_idx)
{
	struct lan966x_mep *mep;

	hlist_for_each_entry(mep, &lan966x->mep_list, head) {
		if (mep->voe_idx == voe_idx)
			return mep;
	}

	return NULL;
}

int lan966x_handle_cfm_mep_add(struct lan966x_port *port,
			       const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_mep *config = SWITCHDEV_OBJ_CFM_MEP(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;
	u32 voe_idx, offset, i;

	if (config->port != port->dev)
		return 0;

	if (mep_find(lan966x, config->instance))
		/* MEP instance already exists - cannot be added then */
		return -EOPNOTSUPP;

	/* Create MEP instance */
	mep = devm_kzalloc(lan966x->dev, sizeof(struct lan966x_mep),
			   GFP_KERNEL);
	if (!mep)
		return -EOPNOTSUPP;

	port = netdev_priv(config->port);
	voe_idx = port->chip_port;

	/* Initialize instance and add to list */
	mep->instance = config->instance;
	mep->voe_idx = voe_idx;
	mep->afi_id = MEP_AFI_ID_NONE;
	mep->port = port;
	hlist_add_head(&mep->head, &lan966x->mep_list);

	/* Enable VOE */
	lan_rmw(MEP_BASIC_CTRL_VOE_ENA_SET(1),
		MEP_BASIC_CTRL_VOE_ENA,
		lan966x, MEP_BASIC_CTRL(voe_idx));

	/* Clear assorted counters: */
	lan_wr(0, lan966x, MEP_CCM_RX_VL_FC_CNT(voe_idx));
	lan_wr(0, lan966x, MEP_CCM_RX_IV_FC_CNT(voe_idx));
	lan_wr(0, lan966x, REW_PTP_SEQ_NO(voe_idx));
	lan_wr(0, lan966x, MEP_CCM_RX_SEQ_CFG(voe_idx));
	lan_wr(0, lan966x, MEP_RX_SEL_CNT(voe_idx));
	lan_wr(0, lan966x, MEP_RX_FRM_CNT(voe_idx));

	/* sticky bits cleared by writing 1 to them */
	lan_wr(0xFFFFFF, lan966x, MEP_RX_STICKY(voe_idx));
	lan_wr(0xFF, lan966x, MEP_STICKY(voe_idx));
	lan_rmw(MEP_INTR_ENA_CCM_LOC_INTR_ENA_SET(1),
		MEP_INTR_ENA_CCM_LOC_INTR_ENA,
		lan966x, MEP_INTR_ENA(voe_idx));

	/* Clear LM counters */
	offset = voe_idx * PRIO_CNT;
	for (i = 0; i < PRIO_CNT; ++i)
		lan_wr(0, lan966x, MEP_PORT_RX_FRM_CNT(offset + i));

	return 0;
}

int lan966x_handle_cfm_mep_del(struct lan966x_port *port,
			       const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_mep *config = SWITCHDEV_OBJ_CFM_MEP(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;
	u32 i, value;

	if (config->port != port->dev)
		return 0;

	if (!(mep = mep_find(lan966x, config->instance)))
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	/* Update the level filtering mask
	 * OAM frames on this level or lower, must be forwarded to this port
	 */
	value = lan_rd(lan966x, MEP_MEL_CTRL(mep->voe_idx));
	value = MEP_MEL_CTRL_MEL_VAL_GET(value);
	for (i = 0; i <= value; ++i) {
		lan_rmw(MEP_MEL_FILTERING_CFG_MEL_PORTMASK_SET(1 << mep->voe_idx),
			MEP_MEL_FILTERING_CFG_MEL_PORTMASK_SET(1 << mep->voe_idx),
			lan966x, MEP_MEL_FILTERING_CFG(i));
	}

	/* Disable CCM handling */
	lan_rmw(MEP_HW_CTRL_CCM_ENA_SET(0),
		MEP_HW_CTRL_CCM_ENA,
		lan966x, MEP_HW_CTRL(mep->voe_idx));

	/* Sticky bits cleared by writing 1 to them */
	lan_wr(0xFFFFFF, lan966x, MEP_RX_STICKY(mep->voe_idx));
	lan_wr(0xFF, lan966x, MEP_STICKY(mep->voe_idx));
	lan_rmw(MEP_INTR_ENA_CCM_LOC_INTR_ENA_SET(0),
		MEP_INTR_ENA_CCM_LOC_INTR_ENA,
		lan966x, MEP_INTR_ENA(mep->voe_idx));

	/* Disable VOE */
	lan_rmw(MEP_BASIC_CTRL_VOE_ENA_SET(0),
		MEP_BASIC_CTRL_VOE_ENA,
		lan966x, MEP_BASIC_CTRL(mep->voe_idx));

	hlist_del(&mep->head);
	devm_kfree(lan966x->dev, mep);

	return 0;
}

int lan966x_handle_cfm_mep_config_add(struct lan966x_port *port,
				      const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_mep_config_set *config = SWITCHDEV_OBJ_CFM_MEP_CONFIG_SET(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;
	u32 value, i;

	if ((mep = mep_find(lan966x, config->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	if (mep->port != port)
		return 0;

	/* Configure the unicast MAC */
	value = MEP_UC_MAC_MSB_MEP_UC_MAC_MSB_SET((config->unicast_mac.addr[0] << 8) |
						  config->unicast_mac.addr[1]);
	lan_wr(value, lan966x, MEP_UC_MAC_MSB(mep->voe_idx));
	value = (config->unicast_mac.addr[2] << 24) | (config->unicast_mac.addr[3] << 16) |
		(config->unicast_mac.addr[4] <<  8) | (config->unicast_mac.addr[5]);
	lan_wr(value, lan966x, MEP_UC_MAC_LSB(mep->voe_idx));

	/* Configure MEG level */
	lan_rmw(MEP_MEL_CTRL_MEL_VAL_SET(config->mdlevel),
		MEP_MEL_CTRL_MEL_VAL,
		lan966x, MEP_MEL_CTRL(mep->voe_idx));

	/* Configure the DMAC check type to check against both UC and MC */
	lan_rmw(MEP_BASIC_CTRL_RX_DMAC_CHK_SEL_SET(0x03),
		MEP_BASIC_CTRL_RX_DMAC_CHK_SEL,
		lan966x, MEP_BASIC_CTRL(mep->voe_idx));

	/* Update the level filtering mask
	 * OAM frames on this level or lower, must not be forwarded to this port
	 */
	for (i = 0; i <= config->mdlevel; ++i)
		lan_rmw(MEP_MEL_FILTERING_CFG_MEL_PORTMASK_SET(0),
			MEP_MEL_FILTERING_CFG_MEL_PORTMASK_SET(1 << mep->voe_idx),
			lan966x, MEP_MEL_FILTERING_CFG(i));

	return 0;
}

int lan966x_handle_cfm_cc_peer_mep_add(struct lan966x_port *port,
				       const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_cc_peer_mep *config = SWITCHDEV_OBJ_CFM_CC_PEER_MEP(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;

	if ((mep = mep_find(lan966x, config->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	if (mep->port != port)
		return 0;

	/* Configure peer MEP id */
	lan_wr(MEP_CCM_MEPID_CFG_CCM_MEPID_SET(config->peer_mep_id),
	       lan966x, MEP_CCM_MEPID_CFG(mep->voe_idx));

	/* Enable MEPID check */
	lan_rmw(MEP_CCM_CFG_CCM_MEPID_CHK_ENA_SET(1),
		MEP_CCM_CFG_CCM_MEPID_CHK_ENA,
		lan966x, MEP_CCM_CFG(mep->voe_idx));

	return 0;
}

int lan966x_handle_cfm_cc_peer_mep_del(struct lan966x_port *port,
				       const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_cc_peer_mep *config = SWITCHDEV_OBJ_CFM_CC_PEER_MEP(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;

	if ((mep = mep_find(lan966x, config->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	if (mep->port != port)
		return 0;

	/* Disable MEPID check */
	lan_rmw(MEP_CCM_CFG_CCM_MEPID_CHK_ENA_SET(0),
		MEP_CCM_CFG_CCM_MEPID_CHK_ENA,
		lan966x, MEP_CCM_CFG(mep->voe_idx));

	return 0;
}

int lan966x_handle_cfm_cc_config_add(struct lan966x_port *port,
				     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_cc_config_set *config = SWITCHDEV_OBJ_CFM_CC_CONFIG_SET(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;
	u32 i, value, mask;
	const u8 *maid;

	if ((config->interval == SWITCHDEV_CFM_CCM_INTERVAL_NONE) ||
	    (config->interval >= SWITCHDEV_CFM_CCM_INTERVAL_10_SEC))
		/* Illegal interval */
		return -EOPNOTSUPP;

	if ((mep = mep_find(lan966x, config->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	if (mep->port != port)
		return 0;

	/* Clear sequence numbers */
	value = lan_rd(lan966x, MEP_HW_CTRL(mep->voe_idx));
	/* The Tx and Rx sequence numbers are cleared when CC is enabled */
	if (config->enable && (MEP_HW_CTRL_CCM_ENA_GET(value) == 0)) {
		lan_wr(1, lan966x, REW_PTP_SEQ_NO(mep->voe_idx));
		lan_wr(0, lan966x, MEP_CCM_RX_SEQ_CFG(mep->voe_idx));
	}

	/* Configure expected period, MEGID and MEPID. */
	value = MEP_CCM_CFG_CCM_RX_SEQ_CHK_ENA_SET(1) |
		MEP_CCM_CFG_CCM_PERIOD_SET(interval_to_period(config->interval)) |
		MEP_CCM_CFG_CCM_MEGID_CHK_ENA_SET(1) |
		MEP_CCM_CFG_CCM_MEPID_CHK_ENA_SET(1);
	mask = MEP_CCM_CFG_CCM_RX_SEQ_CHK_ENA |
	       MEP_CCM_CFG_CCM_PERIOD |
	       MEP_CCM_CFG_CCM_MEGID_CHK_ENA |
	       MEP_CCM_CFG_CCM_MEPID_CHK_ENA;
	lan_rmw(value, mask, lan966x, MEP_CCM_CFG(mep->voe_idx));

	/* Configure MEGID */
	maid = &config->maid.data[47];    // MSB
	for (i = 0; i < MEP_CCM_MEGID_CFG_REPLICATION; ++i, maid -= 4) {
		value = (*(maid - 3) << 24) | (*(maid - 2) << 16) |
			(*(maid - 1) <<  8) | (*(maid - 0));
		lan_wr(value, lan966x, MEP_CCM_MEGID_CFG(mep->voe_idx, i));
	}

	/* Enable/Disable CCM handling */
	lan_rmw(MEP_HW_CTRL_CCM_ENA_SET(config->enable ? 1 : 0),
		MEP_HW_CTRL_CCM_ENA,
		lan966x, MEP_HW_CTRL(mep->voe_idx));

	return 0;
}

int lan966x_handle_cfm_cc_rdi_add(struct lan966x_port *port,
				  const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_cc_rdi_set *config = SWITCHDEV_OBJ_CFM_CC_RDI_SET(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;

	if ((mep = mep_find(lan966x, config->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	if (mep->port != port)
		return 0;

	lan_rmw(REW_CCM_TX_CFG_CCM_TX_RDI_SET(config->rdi ? 1 : 0),
		REW_CCM_TX_CFG_CCM_TX_RDI,
		lan966x, REW_CCM_TX_CFG(mep->voe_idx));

	return 0;
}

int lan966x_handle_cfm_cc_ccm_tx_add(struct lan966x_port *port,
				     const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_cc_ccm_tx *config = SWITCHDEV_OBJ_CFM_CC_CCM_TX(obj);
	struct lan966x_afi_slow_inj_alloc_cfg alloc_cfg;
	struct lan966x_afi_slow_inj_start_cfg start_cfg;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;
	__be32 ifh[IFH_LEN];
	bool stop;

	if ((mep = mep_find(lan966x, config->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	if (mep->port != port)
		return 0;

	stop = (config->interval != SWITCHDEV_CFM_CCM_INTERVAL_NONE) ? false : true;

	if (!stop && (mep->afi_id == MEP_AFI_ID_NONE)) {
		/* This is a start of a transmission
		 * AFI must be allocated
		 */
		alloc_cfg.port_no = mep->voe_idx; /* voe_idx is the chip port */
		alloc_cfg.prio = 0;

		if (!lan966x_afi_slow_inj_alloc(mep->port->dev, &alloc_cfg, &mep->afi_id))
			return -EOPNOTSUPP;
	} else {
		/* This is a stop or transmission is ongoing */
		if (mep->afi_id != MEP_AFI_ID_NONE)
			/* Stop any ongoing AFI transmission */
			(void)lan966x_afi_slow_inj_stop(mep->port->dev, mep->afi_id);
	}

	if (stop) {
		/* This is a stop */
		if (mep->afi_id != MEP_AFI_ID_NONE)
			/* AFI must be freed */
			lan966x_afi_slow_inj_free(mep->port->dev, mep->afi_id);
		mep->afi_id = MEP_AFI_ID_NONE;
		return 0;
	}

	/* Hijack the frame in the AFI */
	memset(ifh, 0x0, sizeof(__be32) * IFH_LEN);
	lan966x_ifh_set_bypass(ifh, 1);
	lan966x_ifh_set_afi(ifh, true);
	lan966x_ifh_set_rew_oam(ifh, true);
	lan966x_ifh_set_oam_type(ifh, 1);
	lan966x_ifh_set_timestamp(ifh, 0);
	lan966x_ifh_set_seq_num(ifh, NUM_PHYS_PORTS * 4 + port->chip_port);
	lan966x_ifh_set_port(ifh, BIT_ULL(mep->voe_idx));
	lan966x_xmit(port, config->skb, ifh);
	if (!lan966x_afi_slow_inj_frm_hijack(mep->port->dev, mep->afi_id)) {
		if (mep->afi_id != MEP_AFI_ID_NONE)
			/* AFI must be freed */
			lan966x_afi_slow_inj_free(mep->port->dev, mep->afi_id);
		mep->afi_id = MEP_AFI_ID_NONE;
		return -EOPNOTSUPP;
	}

	/* Start the AFI transmission */
	start_cfg.fph = div_u64(((u64)3600 * (u64)1000 * (u64)1000),
				interval_to_micro(config->interval));
	lan966x_afi_slow_inj_start(mep->port->dev, mep->afi_id, &start_cfg);

	/* Configure the sequence number update enable in rewriter */
	lan_rmw(REW_CCM_TX_CFG_CCM_SEQ_UPD_ENA_SET(config->seq_no_update ? 1 : 0),
		REW_CCM_TX_CFG_CCM_SEQ_UPD_ENA,
		lan966x, REW_CCM_TX_CFG(mep->voe_idx));

	return 0;
}

int lan966x_handle_cfm_mep_status_get(struct lan966x_port *port,
				      struct switchdev_cfm_mep_status *status)
{
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;
	u32 value, value1;

	if ((mep = mep_find(lan966x, status->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	value = lan_rd(lan966x, MEP_RX_STICKY(mep->voe_idx));
	status->opcode_unexp_seen = MEP_RX_STICKY_UNK_OPCODE_RX_STICKY_GET(value) != 0;

	/* Clear the sticky bits that has been detected */
	value = value & MEP_RX_STICKY_UNK_OPCODE_RX_STICKY;
	lan_wr(value, lan966x, MEP_RX_STICKY(mep->voe_idx));

	/* The MEP_STICKY_OAM_MEL_STICKY is not a "seen" indication - it is an indication that the
	 * MEP_CCM_CFG_OAM_MEL_ERR status has changed state.
	 * An OAM PDU with low level has been seen if the state has changed or
	 * the state is active
	 */
	value = lan_rd(lan966x, MEP_STICKY(mep->voe_idx));
	value1 = lan_rd(lan966x, MEP_CCM_CFG(mep->voe_idx));
	status->rx_level_low_seen = (MEP_STICKY_OAM_MEL_STICKY_GET(value) != 0) ||
				     (MEP_CCM_CFG_OAM_MEL_ERR_GET(value1) != 0);

	/* Clear the sticky bits that has been detected  */
	value = value & MEP_STICKY_OAM_MEL_STICKY;
	lan_wr(value, lan966x, MEP_STICKY(mep->voe_idx));
	/* The MEP_CCM_CFG_OAM_MEL_ERR status is cleared to detect
	 * if low level is still received.
	 */
	lan_rmw(MEP_CCM_CFG_OAM_MEL_ERR_SET(0),
		MEP_CCM_CFG_OAM_MEL_ERR,
		lan966x, MEP_CCM_CFG(mep->voe_idx));

	return 0;
}

int lan966x_handle_cfm_cc_peer_status_get(struct lan966x_port *port,
					  struct switchdev_cfm_cc_peer_status *status)
{
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mep *mep;
	u32 value;

	if ((mep = mep_find(lan966x, status->instance)) == NULL)
		/* MEP instance does not exists */
		return -EOPNOTSUPP;

	value = lan_rd(lan966x, MEP_CCM_CFG(mep->voe_idx));
	status->rdi = MEP_CCM_CFG_CCM_RX_RDI_GET(value) != 0;
	status->ccm_defect = MEP_CCM_CFG_CCM_MISS_CNT_GET(value) == 0x07;

	value = lan_rd(lan966x, MEP_RX_STICKY(mep->voe_idx));
	status->seen = MEP_RX_STICKY_CCM_RX_STICKY_GET(value) ||
		       MEP_RX_STICKY_CCM_LM_RX_STICKY_GET(value);
	status->seq_unexp_seen = MEP_RX_STICKY_CCM_RX_SEQ_ERR_STICKY_GET(value);

	/* Clear the sticky bits that has been detected */
	value = value & (MEP_RX_STICKY_CCM_RX_STICKY |
			 MEP_RX_STICKY_CCM_LM_RX_STICKY |
			 MEP_RX_STICKY_CCM_RX_SEQ_ERR_STICKY);
	lan_wr(value, lan966x, MEP_RX_STICKY(mep->voe_idx));

	return 0;
}

int lan966x_handle_cfm_interrupt(struct lan966x *lan966x)
{
	u32 enable_mask, sticky_mask, event_mask, value, i;
	struct br_cfm_notif_info notif_info;
	struct lan966x_mep *mep;

	/* All VOEs are checked for active interrupt. */
	for (i = 0; i < lan966x->num_phys_ports; ++i) {
		if ((mep = voe_mep_find(lan966x, i)) == NULL)
			/* MEP instance does not exists */
			continue;

		/* Read the interrupt enable mask */
		enable_mask = lan_rd(lan966x, MEP_INTR_ENA(i));

		/* Read sticky bits and clear the enabled ones */
		sticky_mask = lan_rd(lan966x, MEP_STICKY(i));
		sticky_mask &= enable_mask;
		/* Sticky bits cleared by writing 1 to them */
		lan_wr(sticky_mask, lan966x, MEP_STICKY(i));

		/* Translate sticky mask to returned event mask */
		event_mask = (((sticky_mask & MEP_STICKY_CCM_PERIOD_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_INTERVAL : 0) |
			      (((sticky_mask & MEP_STICKY_CCM_PRIO_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_PRIO : 0) |
			      (((sticky_mask & MEP_STICKY_CCM_ZERO_PERIOD_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_ZERO_INTERVAL : 0) |
			      (((sticky_mask & MEP_STICKY_CCM_RX_RDI_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_RDI : 0) |
			      (((sticky_mask & MEP_STICKY_CCM_LOC_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_DEFECT : 0) |
			      (((sticky_mask & MEP_STICKY_CCM_MEPID_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_MEPID : 0) |
			      (((sticky_mask & MEP_STICKY_CCM_MEGID_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_MEGID : 0) |
			      (((sticky_mask & MEP_STICKY_OAM_MEL_STICKY) != 0) ?
				BR_CFM_EVENT_CCM_LEVEL : 0);

		/* Check for any events for this instance */
		if (!event_mask)
			continue;

		notif_info.instance = mep->instance;
		notif_info.events = event_mask;

		/* Read the peer MEPID */
		value = lan_rd(lan966x, MEP_CCM_MEPID_CFG(i));
		notif_info.peer_mepid = MEP_CCM_MEPID_CFG_CCM_MEPID_GET(value);

		/* Read the CCM defect status */
		value = lan_rd(lan966x, MEP_CCM_CFG(mep->voe_idx));
		notif_info.ccm_defect = MEP_CCM_CFG_CCM_MISS_CNT_GET(value) == 0x07;

		br_cfm_notification(lan966x->bridge, &notif_info);
	}
	return 0;
}

static struct lan966x_mip *mip_find(struct lan966x *lan966x, u32 instance)
{
	struct lan966x_mip *mip;

	hlist_for_each_entry(mip, &lan966x->mip_list, head) {
		if (mip->instance == instance)
			return mip;
	}

	return NULL;
}

#define LAN966X_CFM_RULE_ID_OFFSET 100
static int lan966x_add_raps_is2_rule(struct lan966x_port *port, u32 vid)
{
	int rule_id = LAN966X_CFM_RULE_ID_OFFSET + port->chip_port;
	struct lan966x *lan966x = port->lan966x;
	int chain_id = LAN966X_VCAP_CID_IS2_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_CFM, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return -ENOMEM;

	err = vcap_rule_add_key_bit(vrule, VCAP_KF_LOOKUP_FIRST_IS, VCAP_BIT_1);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, BIT(port->chip_port),
				     0xffff);
	err |= vcap_rule_add_key_bit(vrule, VCAP_KF_8021Q_VLAN_TAGGED_IS, VCAP_BIT_1);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_8021Q_VID_CLS, vid, 0xfff);
	err |= vcap_rule_add_key_bit(vrule, VCAP_KF_OAM_Y1731_IS, VCAP_BIT_1);
	err |= vcap_rule_add_key_u32(vrule, VCAP_KF_OAM_OPCODE, BR_CFM_OPCODE_RAPS, 0xff);
	err |= vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_CPU_QUEUE_NUM, 7);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PORT_MASK, lan966x->bridge_mask);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, LAN966X_PMM_NO_ACTION);
	err |= vcap_val_rule(vrule, ETH_P_ALL);
	if (err)
		goto free_rule;

	err = vcap_add_rule(vrule);

free_rule:
	vcap_free_rule(vrule);
	return err;
}

static int lan966x_mod_raps_is2_rule(struct lan966x_port *port,
				     enum switchdev_cfm_raps_handling raps_handling,
				     u32 mdlevel)
{
	int rule_id = LAN966X_CFM_RULE_ID_OFFSET + port->chip_port;
	struct vcap_rule *vrule;
	bool copy_cpu;
	u8 port_mask;
	u8 mask_mode;
	int err;

	vrule = vcap_get_rule(port->dev, rule_id);
	if (!vrule || IS_ERR(vrule))
		return 1;

	copy_cpu = raps_handling == SWITCHDEV_CFM_RAPS_HANDLING_COPY_CPU ||
		   raps_handling == SWITCHDEV_CFM_RAPS_HANDLING_REDIR_CPU ? true : false;
	port_mask = raps_handling == SWITCHDEV_CFM_RAPS_HANDLING_COPY_CPU ||
		    raps_handling == SWITCHDEV_CFM_RAPS_HANDLING_REDIR_CPU ? 0xff : 0;
	mask_mode = raps_handling == SWITCHDEV_CFM_RAPS_HANDLING_REDIR_CPU ?
		    LAN966X_PMM_REDIRECT : LAN966X_PMM_NO_ACTION;

	err = vcap_rule_mod_key_u32(vrule, VCAP_KF_OAM_MEL_FLAGS, ~(0xff << mdlevel), 0xff);
	err |= vcap_rule_mod_action_u32(vrule, VCAP_AF_CPU_COPY_ENA, copy_cpu);
	err |= vcap_rule_mod_action_u32(vrule, VCAP_AF_PORT_MASK, port_mask);
	err |= vcap_rule_mod_action_u32(vrule, VCAP_AF_MASK_MODE, mask_mode);
	err |= vcap_mod_rule(vrule);

	vcap_free_rule(vrule);
	return err;
}

static void lan966x_del_raps_is2_rule(struct lan966x_port *port)
{
	int rule_id = LAN966X_CFM_RULE_ID_OFFSET + port->chip_port;

	vcap_del_rule(port->dev, rule_id);
}

static void lan966x_mip_destroy(struct lan966x_port *port, struct lan966x_mip *mip)
{
	struct lan966x *lan966x = port->lan966x;

	lan966x_del_prio_is1_rule(port, port->raps_is1_rule_id);
	lan966x_del_raps_is2_rule(port);

	hlist_del(&mip->head);
	devm_kfree(lan966x->dev, mip);
}

int lan966x_handle_cfm_mip_add(struct lan966x_port *port,
			       const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_mip *config = SWITCHDEV_OBJ_CFM_MIP(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mip *mip;
	int err;

	if (config->port != port->dev)
		return 0;

	if (mip_find(lan966x, config->instance))
		/* MIP instance already exists - cannot be added then */
		return -EOPNOTSUPP;

	/* Create MIP instance */
	mip = devm_kzalloc(lan966x->dev, sizeof(struct lan966x_mip),
			   GFP_KERNEL);
	if (!mip)
		return -ENOMEM;

	port = netdev_priv(config->port);

	/* Initialize instance and add to list */
	mip->instance = config->instance;
	mip->port = port;
	hlist_add_head(&mip->head, &lan966x->mip_list);

	/* Create IS1 rules to give classified priority 7 to RAPS frames */
	if (!lan966x->raps_is1_rule_id) {
		if (lan966x_add_prio_is1_rule(port,
					      VCAP_USER_CFM,
					      &port->raps_is1_rule_id)) {
			err = -EOPNOTSUPP;
			goto error;
		}
	}

	if ((err = lan966x_add_raps_is2_rule(port, config->vid)))
		goto error;

	return 0;

error:
	lan966x_mip_destroy(port, mip);
	return err;
}

int lan966x_handle_cfm_mip_del(struct lan966x_port *port,
			       const struct switchdev_obj *obj)
{
	const struct switchdev_obj_cfm_mip *config = SWITCHDEV_OBJ_CFM_MIP(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mip *mip;

	if (config->port != port->dev)
		return 0;

	if (!(mip = mip_find(lan966x, config->instance)))
		/* MIP instance does not exists */
		return -EOPNOTSUPP;

	lan966x_mip_destroy(port, mip);

	return 0;
}

int lan966x_handle_cfm_mip_config_add(struct lan966x_port *port,
				      const struct switchdev_obj* obj)
{
	const struct switchdev_obj_cfm_mip_config_set *config = SWITCHDEV_OBJ_CFM_MIP_CONFIG_SET(obj);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_mip *mip;
	int err;

	if ((mip = mip_find(lan966x, config->instance)) == NULL)
		/* MIP instance does not exists */
		return -EOPNOTSUPP;

	if (mip->port != port)
		return 0;

	/* Modify IS2 rules to control RAPS frame handling */
	if ((err = lan966x_mod_raps_is2_rule(port, config->raps_handling,
					     config->mdlevel)))
		return -EOPNOTSUPP;

	return 0;
}
