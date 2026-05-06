// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <uapi/linux/mrp_bridge.h>
#include <linux/dsa/lan9645x.h>
#include <linux/if_bridge.h>
#include <linux/netdevice.h>

#include "afi_api.h"
#include "mrp_api.h"

#include "lan9645x_mrp.h"
#include "lan9645x_main.h"
#include "lan9645x_vcap_utils.h"

#define MRP_FWD_NOP		0
#define MRP_FWD_COPY		1
#define MRP_FWD_REDIR		2
#define MRP_FWD_DISC		3

#define CONFIG_TEST		0
#define CONFIG_IN_TEST		1

static const u8 mrp_test_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x1 };
static const u8 mrp_in_test_dmac[ETH_ALEN] = { 0x1, 0x15, 0x4e, 0x0, 0x0, 0x3 };
static const u8 mrp_dmac_mask[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8 };

void lan9645x_mrp_ring_open(struct lan9645x *lan9645x)
{
	u32 mep_intr = lan_rd(lan9645x, MEP_INTR);

	/* If this is not an interrupt for MRP then just ignore it */
	if (!MEP_INTR_MRP_INTR_GET(mep_intr))
		return;

	mrp_ring_interrupt(lan9645x->mrp_ctrl);
}

void lan9645x_mrp_in_open(struct lan9645x *lan9645x)
{
	/* If this is not an interrupt for MRP then just ignore it */
	if (!MEP_INTR_MRP_INTR_GET(lan_rd(lan9645x, MEP_INTR)))
		return;

	mrp_in_interrupt(lan9645x->mrp_ctrl);
}

int lan9645x_handle_mrp_port_role(struct lan9645x *lan9645x, int port,
				  enum br_mrp_port_role_type role)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	return mrp_port_set_port_role(p->mrp_port, role);
}

int lan9645x_handle_mrp_add_port(struct lan9645x *lan9645x, int port,
				 const struct switchdev_obj_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct net_device *dev;

	dev_dbg(lan9645x->dev,
		"port=%d ring_id=%d prio=%d p_port=%s s_port=%s\n", port,
		mrp->ring_id, mrp->prio,
		netdev_name(mrp->p_port),
		netdev_name(mrp->s_port));

	dev = lan9645x_port_to_ndev(p);

	if (mrp->p_port != dev && mrp->s_port != dev)
		return 0;

	p->mrp_port = mrp_add_port(lan9645x->mrp_ctrl, mrp, dev);
	if (IS_ERR(p->mrp_port))
		return PTR_ERR(p->mrp_port);

	lan9645x_add_prio_is1_rule(p, VCAP_USER_MRP,
				   &p->mrp_is1_p_port_rule_id);

	return 0;
}

int lan9645x_handle_mrp_del_port(struct lan9645x *lan9645x, int port,
				 const struct switchdev_obj_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct net_device *dev;
	int ret;

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	dev = lan9645x_port_to_ndev(p);

	if (mrp->p_port != dev && mrp->s_port != dev)
		return 0;

	ret = mrp_del_port(lan9645x->mrp_ctrl, mrp, p->mrp_port);
	if (ret)
		return ret;

	p->mrp_port = NULL;
	vcap_del_rule(p->lan9645x->vcap_ctrl, dev,
		      p->mrp_is1_p_port_rule_id);

	return 0;
}

int lan9645x_handle_mrp_ring_test_add(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_test_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	return mrp_port_start_ring_test(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_ring_test_del(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_test_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	return mrp_port_stop_ring_test(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_ring_state_add(struct lan9645x *lan9645x, int port,
				       const struct switchdev_obj_ring_state_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	return mrp_port_set_ring_state(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_ring_role_add(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_role_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d ring_id=%d ring_role=%d\n", port,
		mrp->ring_id, mrp->ring_role);

	return mrp_port_set_ring_role(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_ring_role_del(struct lan9645x *lan9645x, int port,
				      const struct switchdev_obj_ring_role_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	return mrp_port_set_ring_role(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_in_test_add(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_test_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	return mrp_port_start_in_test(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_in_test_del(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_test_mrp *mrp)
{
	dev_dbg(lan9645x->dev, "port=%d\n", port);
	return mrp_port_stop_in_test(lan9645x_to_port(lan9645x, port)->mrp_port, mrp);
}

int lan9645x_handle_mrp_in_state_add(struct lan9645x *lan9645x, int port,
				     const struct switchdev_obj_in_state_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	return mrp_port_set_in_state(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_in_role_add(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_role_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct net_device *dev;

	dev = lan9645x_port_to_ndev(p);

	dev_dbg(lan9645x->dev, "port=%d\n", port);
	if (mrp->i_port != dev)
		return 0;

	p->mrp_port = mrp_add_in_port(lan9645x->mrp_ctrl, mrp, dev);
	if (IS_ERR(p->mrp_port))
		return PTR_ERR(p->mrp_port);

	return mrp_port_set_in_role(p->mrp_port, mrp);
}

int lan9645x_handle_mrp_in_role_del(struct lan9645x *lan9645x, int port,
				    const struct switchdev_obj_in_role_mrp *mrp)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct net_device *dev;
	int ret;

	dev_dbg(lan9645x->dev, "port=%d\n", port);
	dev = lan9645x_port_to_ndev(p);

	if (mrp->i_port != dev)
		return 0;

	ret = mrp_port_set_in_role(p->mrp_port, mrp);
	if (ret)
		return ret;

	ret = mrp_del_in_port(lan9645x->mrp_ctrl, mrp, p->mrp_port);
	if (ret)
		return ret;

	p->mrp_port = NULL;

	return 0;
}

static int lan9645x_mrp_port_init(struct mrp_port *mrp_port, u16 prio)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(lan9645x->dev, "port=%d prio=%u\n", port->chip_port, prio);

	/* Enable MEP and LOC_SCAN */
	lan_rmw(MEP_MEP_CTRL_LOC_SCAN_ENA_SET(1) |
		MEP_MEP_CTRL_MEP_ENA_SET(1),
		MEP_MEP_CTRL_LOC_SCAN_ENA |
		MEP_MEP_CTRL_MEP_ENA,
		lan9645x, MEP_MEP_CTRL);

	/* Enable MEP to process MRP frames */
	lan_rmw(ANA_OAM_CFG_MRP_ENA_SET(1),
		ANA_OAM_CFG_MRP_ENA,
		lan9645x, ANA_OAM_CFG(port->chip_port));

	lan_rmw(ANA_VCAP_CFG_PAG_VAL_SET(BIT(6)),
		ANA_VCAP_CFG_PAG_VAL,
		lan9645x, ANA_VCAP_CFG(port->chip_port));

	/* Enable MEP to process Y.1731 frames */
	lan_rmw(ANA_OAM_CFG_OAM_CFG_SET(1),
		ANA_OAM_CFG_OAM_CFG,
		lan9645x, ANA_OAM_CFG(port->chip_port));

	/* Activate MRP endpoint */
	lan_rmw(MEP_MRP_CTRL_MRP_ENA_SET(1),
		MEP_MRP_CTRL_MRP_ENA,
		lan9645x, MEP_MRP_CTRL(port->chip_port));

	lan_rmw(MEP_TST_PRIO_CFG_OWN_PRIO_SET(prio),
		MEP_TST_PRIO_CFG_OWN_PRIO,
		lan9645x, MEP_TST_PRIO_CFG(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_uninit(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(lan9645x->dev, "port=%d\n", port->chip_port);

	/* Disable MEP to process MRP frames */
	lan_rmw(ANA_OAM_CFG_MRP_ENA_SET(0),
		ANA_OAM_CFG_MRP_ENA,
		lan9645x, ANA_OAM_CFG(port->chip_port));

	/* Disactivate MRP endpoint */
	lan_rmw(MEP_MRP_CTRL_MRP_ENA_SET(0),
		MEP_MRP_CTRL_MRP_ENA,
		lan9645x, MEP_MRP_CTRL(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_update_mac(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;
	u32 macl, mach;

	dev_dbg(lan9645x->dev, "port=%d addr=%pM\n",
		port->chip_port, lan9645x->bridge->dev_addr);

	mach = lan9645x->bridge->dev_addr[0] << 8;
	mach |= lan9645x->bridge->dev_addr[1] << 0;
	macl = lan9645x->bridge->dev_addr[2] << 24;
	macl |= lan9645x->bridge->dev_addr[3] << 16;
	macl |= lan9645x->bridge->dev_addr[4] << 8;
	macl |= lan9645x->bridge->dev_addr[5] << 0;

	lan_wr(macl, lan9645x, MEP_MRP_MAC_LSB(port->chip_port));
	lan_wr(mach, lan9645x, MEP_MRP_MAC_MSB(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_update_mrm_mac(struct mrp_port *mrp_port,
					    const u8 mac[ETH_ALEN])
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;
	u32 macl, mach;

	dev_dbg(lan9645x->dev, "port=%d addr=%pM\n", port->chip_port, mac);
	mach = mac[0] << 8;
	mach |= mac[1] << 0;
	macl = mac[2] << 24;
	macl |= mac[3] << 16;
	macl |= mac[4] << 8;
	macl |= mac[5] << 0;

	lan_wr(MEP_BEST_MAC_MSB_BEST_MAC_MSB_SET(mach),
	       lan9645x, MEP_BEST_MAC_MSB(port->chip_port));
	lan_wr(macl, lan9645x, MEP_BEST_MAC_LSB(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_set_ring_state(struct mrp_port *mrp_port,
					    u32 ring_transitions,
					    enum br_mrp_ring_state_type ring_state)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(lan9645x->dev, "port=%d ring_transitions=%u state=%u\n",
		port->chip_port, ring_transitions, ring_state);

	lan_rmw(REW_MRP_TX_CFG_MRP_STATE_SET(ring_state),
		REW_MRP_TX_CFG_MRP_STATE,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_TEST));

	lan_rmw(REW_MRP_TX_CFG_MRP_TRANS_SET(ring_transitions),
		REW_MRP_TX_CFG_MRP_TRANS,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_TEST));

	/* In case the ring is closed, it means that a test frame arrived to the
	 * CPU, so allow again the HW to notify the SW when the ring is open
	 */
	if (ring_state == BR_MRP_RING_STATE_CLOSED)
		lan_rmw(MEP_MRP_STICKY_TST_LOC_STICKY_SET(1),
			MEP_MRP_STICKY_TST_LOC_STICKY,
			lan9645x, MEP_MRP_STICKY(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_set_in_state(struct mrp_port *mrp_port,
					  u32 in_transitions,
					  enum br_mrp_in_state_type in_state)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(lan9645x->dev, "port=%d in_transitions=%u state=%u\n",
		port->chip_port, in_transitions, in_state);

	lan_rmw(REW_MRP_TX_CFG_MRP_STATE_SET(in_state),
		REW_MRP_TX_CFG_MRP_STATE,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_IN_TEST));

	lan_rmw(REW_MRP_TX_CFG_MRP_TRANS_SET(in_transitions),
		REW_MRP_TX_CFG_MRP_TRANS,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_IN_TEST));

	/* In case the ring is closed, it means that a test frame arrived to the
	 * CPU, so allow again the HW to notify the SW when the ring is open
	 */
	if (in_state == BR_MRP_IN_STATE_CLOSED)
		lan_rmw(MEP_MRP_STICKY_ITST_LOC_STICKY_SET(1),
			MEP_MRP_STICKY_ITST_LOC_STICKY,
			lan9645x, MEP_MRP_STICKY(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_set_port_role(struct mrp_port *mrp_port,
					   enum br_mrp_port_role_type role)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(lan9645x->dev, "port=%d role=%d\n", port->chip_port, role);
	/* Update the port role in HW for test and interconnect test frames
	 * even if the there will not be a interconnect ring. Because these
	 * values will be applied to the frame only if the bit MRO_MISC_UPD_ENA
	 * is set
	 */
	lan_rmw(REW_MRP_TX_CFG_MRP_PORTROLE_SET(role),
		REW_MRP_TX_CFG_MRP_PORTROLE,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_TEST));

	lan_rmw(REW_MRP_TX_CFG_MRP_PORTROLE_SET(role),
		REW_MRP_TX_CFG_MRP_PORTROLE,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_IN_TEST));

	return 0;
}

static int lan9645x_mrp_port_hijack_test(struct mrp_port *mrp_port,
					 struct sk_buff *skb)
{
	struct lan9645x_port *port = mrp_port->priv;
	u8 ifh[LAN9645X_IFH_LEN];
	struct net_device *dev;
	u64 seq_num;

	dev = lan9645x_port_to_ndev(port);

	/* The AFI can not inject frames via the NPI port, unless frame aging is
	 * disabled on frontports, so we use manual injection for AFI frames.
	 */
	seq_num = NUM_PHYS_PORTS * 4 + port->chip_port;
	memset(ifh, 0, LAN9645X_IFH_LEN);
	LAN9645X_IFH_SET(ifh, IFH_BYPASS, 1);
	LAN9645X_IFH_SET(ifh, IFH_DSTS, port->chip_port);
	LAN9645X_IFH_SET(ifh, IFH_AFI, 1);
	LAN9645X_IFH_SET(ifh, IFH_REW_OAM, 1);
	LAN9645X_IFH_SET(ifh, IFH_PDU_TYPE, 2);
	LAN9645X_IFH_SET(ifh, IFH_INJ_TIMESTAMP, 0);
	LAN9645X_IFH_SET(ifh, IFH_SEQ_NUM, seq_num);

	return lan9645x_inj_xmit(port, skb, (__be32 *)ifh);
}

static int lan9645x_mrp_port_afi_cfg(struct mrp_port *mrp_port,
				     struct afi_slow_inj_alloc_cfg *cfg)
{
	struct lan9645x_port *port = mrp_port->priv;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
	cfg->port_no = port->chip_port;
	cfg->prio = 0;

	return 0;
}

static int lan9645x_mrp_port_redirect_control(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
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
		lan9645x, MEP_MRP_FWD_CTRL(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_redirect_ring_test(struct mrp_port *mrp_port,
						bool redirect)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
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
	lan_rmw(MEP_TST_CFG_CHK_BEST_MRM_ENA_SET(1) |
		MEP_TST_CFG_CHK_REM_PRIO_ENA_SET(1),
		MEP_TST_CFG_CHK_BEST_MRM_ENA |
		MEP_TST_CFG_CHK_REM_PRIO_ENA,
		lan9645x, MEP_TST_CFG(port->chip_port));

	lan_rmw(MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(2),
		MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL,
		lan9645x, MEP_TST_FWD_CTRL(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_terminate_ring_test(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
	/* Terminate frames */
	/* When the frame is discard means that not to forward the frame
	 * to other front ports but if other block enables the copy to the
	 * CPU then the frame will go to CPU. In this case, the multicast
	 * frames are flooded also to the CPU, so the fix consists
	 * of adding entries to MAC table to disable copying of the frames
	 * to CPU
	 */
	lan_rmw(MEP_TST_FWD_CTRL_REM_FWD_SEL_SET(3) |
		MEP_TST_FWD_CTRL_OWN_FWD_SEL_SET(3) |
		MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL_SET(3) |
		MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL_SET(3),
		MEP_TST_FWD_CTRL_REM_FWD_SEL |
		MEP_TST_FWD_CTRL_OWN_FWD_SEL |
		MEP_TST_FWD_CTRL_LO_PRIO_FWD_SEL |
		MEP_TST_FWD_CTRL_HI_PRIO_FWD_SEL,
		lan9645x, MEP_TST_FWD_CTRL(port->chip_port));

	lan9645x_mact_learn(lan9645x, PGID_MRP, mrp_test_dmac,
			    port->pvid, ENTRYTYPE_LOCKED);

	return 0;
}

static int lan9645x_mrp_port_forward_ring_test(struct mrp_port *mrp_port,
					       struct mrp_port *mrp_partner_port,
					       bool forward)
{
	struct lan9645x_port *partner = mrp_partner_port->priv;
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d partner=%d forward=%d\n",
		port->chip_port, partner->chip_port, forward);

	lan_rmw(MEP_RING_MASK_CFG_RING_PORTMASK_SET(BIT(partner->chip_port)),
		MEP_RING_MASK_CFG_RING_PORTMASK,
		lan9645x, MEP_RING_MASK_CFG(port->chip_port));

	if (forward)
		lan9645x_mact_learn(lan9645x, PGID_MRP, mrp_test_dmac,
				    port->pvid, ENTRYTYPE_LOCKED);
	else
		lan9645x_mact_forget(lan9645x, mrp_test_dmac,
				     port->pvid, ENTRYTYPE_LOCKED);

	lan_rmw(MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL_SET(MRP_FWD_NOP) |
		MEP_MRP_FWD_CTRL_RING_MASK_ENA_SET(forward),
		MEP_MRP_FWD_CTRL_MRP_TST_FWD_SEL |
		MEP_MRP_FWD_CTRL_RING_MASK_ENA,
		lan9645x, MEP_MRP_FWD_CTRL(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_process_ring_test(struct mrp_port *mrp_port,
					       bool process)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
	/* Enable processing of the frame */
	lan_rmw(MEP_MRP_CTRL_MRP_TST_ENA_SET(process),
		MEP_MRP_CTRL_MRP_TST_ENA,
		lan9645x, MEP_MRP_CTRL(port->chip_port));

	/* 51 represents 10 usec */
	/* MRP loc index 0 represents that there is no LOC used, while index 1
	 * in MRP represents index 0 in MEP, therefor subtract 1
	 */
	lan_wr(mrp_port->ring_interval / 10 * 51,
	       lan9645x, MEP_LOC_PERIOD_CFG(mrp_port->mrp_inst->ring_loc_idx - 1));

	/* Set LOC */
	lan_rmw(MEP_TST_CFG_CLR_MISS_CNT_ENA_SET(process) |
		MEP_TST_CFG_LOC_PERIOD_SET(mrp_port->mrp_inst->ring_loc_idx),
		MEP_TST_CFG_CLR_MISS_CNT_ENA |
		MEP_TST_CFG_LOC_PERIOD,
		lan9645x, MEP_TST_CFG(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_rewrite_ring_test(struct mrp_port *mrp_port,
					       bool rewrite)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d rew=%d\n", port->chip_port,
		rewrite);

	lan_rmw(REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD_SET(rewrite) |
		REW_MRP_TX_CFG_MRP_SEQ_UPD_SET(rewrite) |
		REW_MRP_TX_CFG_MRP_MISC_UPD_SET(rewrite),
		REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD |
		REW_MRP_TX_CFG_MRP_SEQ_UPD |
		REW_MRP_TX_CFG_MRP_MISC_UPD,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_TEST));

	return 0;
}

static enum mrp_interrupt_status lan9645x_mrp_port_get_ring_intr_status(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;
	u32 val;

	val = lan_rd(lan9645x, MEP_MRP_STICKY(port->chip_port));

	if (!MEP_MRP_STICKY_TST_LOC_STICKY_GET(val))
		return MRP_INTERRUPT_STATUS_NONE;

	/* RMW does not clear the sticky. */
	lan_wr(val | MEP_MRP_STICKY_TST_LOC_STICKY,
	       lan9645x,
	       MEP_MRP_STICKY(port->chip_port));

	val = lan_rd(lan9645x, MEP_TST_CFG(port->chip_port));
	val = MEP_TST_CFG_MISS_CNT_GET(val);

	if (val == mrp_port->ring_max_miss)
		return MRP_INTERRUPT_STATUS_OPEN;

	return MRP_INTERRUPT_STATUS_CLOSED;
}

static int lan9645x_mrp_port_disable_ring_intr(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	lan_rmw(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(0),
		MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
		lan9645x, MEP_MRP_INTR_ENA(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_enable_ring_intr(struct mrp_port *mrp_port,
					      u32 max)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	lan_rmw(MEP_TST_CFG_MAX_MISS_CNT_SET(max),
		MEP_TST_CFG_MAX_MISS_CNT,
		lan9645x, MEP_TST_CFG(port->chip_port));

	lan_rmw(MEP_TST_CFG_MISS_CNT_SET(0),
		MEP_TST_CFG_MISS_CNT,
		lan9645x, MEP_TST_CFG(port->chip_port));

	lan_rmw(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(1),
		MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA,
		lan9645x, MEP_MRP_INTR_ENA(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_terminate_in_test(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
	lan_rmw(MEP_ITST_FWD_CTRL_OWN_FWD_SEL_SET(3),
		MEP_ITST_FWD_CTRL_OWN_FWD_SEL,
		lan9645x, MEP_ITST_FWD_CTRL(port->chip_port));

	lan9645x_mact_learn(lan9645x, PGID_MRP, mrp_in_test_dmac,
			    port->pvid, ENTRYTYPE_LOCKED);

	return 0;
}

static int lan9645x_mrp_port_forward_in_test(struct mrp_port *mrp_port,
					     struct mrp_port *mrp_partner_port_1,
					     struct mrp_port *mrp_partner_port_2,
					     bool forward)
{
	struct lan9645x_port *partner_1 = mrp_partner_port_1->priv;
	struct lan9645x_port *partner_2 = mrp_partner_port_2->priv;
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;
	u32 mask;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
	mask = BIT(partner_1->chip_port) | BIT(partner_2->chip_port);

	lan_rmw(MEP_ITST_FWD_CTRL_OWN_FWD_SEL_SET(3),
		MEP_ITST_FWD_CTRL_OWN_FWD_SEL,
		lan9645x, MEP_ITST_FWD_CTRL(port->chip_port));

	lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
		MEP_ICON_MASK_CFG_ICON_PORTMASK,
		lan9645x, MEP_ICON_MASK_CFG(port->chip_port));

	if (forward)
		lan9645x_mact_learn(lan9645x, PGID_MRP, mrp_in_test_dmac,
				    port->pvid, ENTRYTYPE_LOCKED);
	else
		lan9645x_mact_forget(lan9645x, mrp_in_test_dmac,
				     port->pvid, ENTRYTYPE_LOCKED);

	lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
		MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
		lan9645x, MEP_MRP_FWD_CTRL(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_forward_rem_in_test(struct mrp_port *mrp_port,
						 struct mrp_port *mrp_partner_port,
						 bool forward)
{
	struct lan9645x_port *partner = mrp_partner_port->priv;
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;
	u32 mask;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
	/* If the frame came on a ring port and it is from a remote MIM
	 * then it is required to forward the frame only to the other
	 * ring port. If the frame is itself then the miss count should
	 * be clear, this is done by HW.
	 */
	lan_rmw(MEP_ITST_FWD_CTRL_REM_FWD_SEL_SET(0),
		MEP_ITST_FWD_CTRL_REM_FWD_SEL,
		lan9645x, MEP_ITST_FWD_CTRL(port->chip_port));

	mask = BIT(partner->chip_port);

	lan_rmw(MEP_ICON_MASK_CFG_ICON_PORTMASK_SET(mask),
		MEP_ICON_MASK_CFG_ICON_PORTMASK,
		lan9645x, MEP_ICON_MASK_CFG(port->chip_port));

	if (forward)
		lan9645x_mact_learn(lan9645x, PGID_MRP, mrp_in_test_dmac,
				    port->pvid, ENTRYTYPE_LOCKED);
	else
		lan9645x_mact_forget(lan9645x, mrp_in_test_dmac,
				     port->pvid, ENTRYTYPE_LOCKED);

	lan_rmw(MEP_MRP_FWD_CTRL_ICON_MASK_ENA_SET(1),
		MEP_MRP_FWD_CTRL_ICON_MASK_ENA,
		lan9645x, MEP_MRP_FWD_CTRL(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_process_in_test(struct mrp_port *mrp_port,
					     bool process)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d\n", port->chip_port);
	/* Enable processing of the frame */
	lan_rmw(MEP_MRP_CTRL_MRP_ITST_ENA_SET(process),
		MEP_MRP_CTRL_MRP_ITST_ENA,
		lan9645x, MEP_MRP_CTRL(port->chip_port));

	/* 51 represents 10 usec */
	/* MRP loc index 0 represents that there is no LOC used, while index 1
	 * in MRP represets index 0 in MEP, therefor subtract 1
	 */
	lan_wr(mrp_port->in_interval / 10 * 51, lan9645x,
	       MEP_LOC_PERIOD_CFG(mrp_port->mrp_inst->in_loc_idx - 1));

	/* Set LOC */
	lan_rmw(MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA_SET(process) |
		MEP_ITST_CFG_ITST_LOC_PERIOD_SET(mrp_port->mrp_inst->in_loc_idx),
		MEP_ITST_CFG_ITST_CLR_MISS_CNT_ENA |
		MEP_ITST_CFG_ITST_LOC_PERIOD,
		lan9645x, MEP_ITST_CFG(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_rewrite_in_test(struct mrp_port *mrp_port,
					     bool rewrite)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	dev_dbg(port->lan9645x->dev, "port=%d rew=%d\n", port->chip_port,
		rewrite);

	lan_rmw(REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD_SET(rewrite) |
		REW_MRP_TX_CFG_MRP_SEQ_UPD_SET(rewrite) |
		REW_MRP_TX_CFG_MRP_MISC_UPD_SET(rewrite),
		REW_MRP_TX_CFG_MRP_TIMESTAMP_UPD |
		REW_MRP_TX_CFG_MRP_SEQ_UPD |
		REW_MRP_TX_CFG_MRP_MISC_UPD,
		lan9645x, REW_MRP_TX_CFG(port->chip_port, CONFIG_IN_TEST));

	return 0;
}

static enum mrp_interrupt_status lan9645x_mrp_port_get_in_intr_status(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;
	u32 val;

	val = lan_rd(lan9645x, MEP_MRP_STICKY(port->chip_port));

	if (!MEP_MRP_STICKY_ITST_LOC_STICKY_GET(val))
		return MRP_INTERRUPT_STATUS_NONE;

	/* RMW does not clear the sticky here. */
	lan_wr(val | MEP_MRP_STICKY_ITST_LOC_STICKY,
	       lan9645x,
	       MEP_MRP_STICKY(port->chip_port));

	val = lan_rd(lan9645x, MEP_ITST_CFG(port->chip_port));
	val = MEP_ITST_CFG_ITST_MISS_CNT_GET(val);

	if (val == mrp_port->in_max_miss)
		return MRP_INTERRUPT_STATUS_OPEN;

	return MRP_INTERRUPT_STATUS_CLOSED;
}

static int lan9645x_mrp_port_disable_in_intr(struct mrp_port *mrp_port)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(0),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		lan9645x, MEP_MRP_INTR_ENA(port->chip_port));

	return 0;
}

static int lan9645x_mrp_port_enable_in_intr(struct mrp_port *mrp_port, u32 max)
{
	struct lan9645x_port *port = mrp_port->priv;
	struct lan9645x *lan9645x = port->lan9645x;

	lan_rmw(MEP_ITST_CFG_ITST_MAX_MISS_CNT_SET(max),
		MEP_ITST_CFG_ITST_MAX_MISS_CNT,
		lan9645x, MEP_ITST_CFG(port->chip_port));

	lan_rmw(MEP_ITST_CFG_ITST_MISS_CNT_SET(0),
		MEP_ITST_CFG_ITST_MISS_CNT,
		lan9645x, MEP_ITST_CFG(port->chip_port));

	lan_rmw(MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
		MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA,
		lan9645x, MEP_MRP_INTR_ENA(port->chip_port));

	return 0;
}

static void *lan9645x_mrp_port_priv_from_netdev(struct net_device *dev)
{
	return lan9645x_port_from_netdev(dev);
}

static struct mrp_operations lan9645x_mrp_operations = {
	.mrp_port_init = lan9645x_mrp_port_init,
	.mrp_port_uninit = lan9645x_mrp_port_uninit,
	.mrp_port_update_mac = lan9645x_mrp_port_update_mac,
	.mrp_port_update_mrm_mac = lan9645x_mrp_port_update_mrm_mac,
	.mrp_port_priv_from_netdev = lan9645x_mrp_port_priv_from_netdev,

	.mrp_port_set_ring_state = lan9645x_mrp_port_set_ring_state,
	.mrp_port_set_in_state = lan9645x_mrp_port_set_in_state,

	.mrp_port_set_port_role = lan9645x_mrp_port_set_port_role,

	.mrp_port_hijack_test = lan9645x_mrp_port_hijack_test,
	.mrp_port_afi_cfg = lan9645x_mrp_port_afi_cfg,

	.mrp_port_redirect_control = lan9645x_mrp_port_redirect_control,

	.mrp_port_terminate_ring_test = lan9645x_mrp_port_terminate_ring_test,
	.mrp_port_redirect_ring_test = lan9645x_mrp_port_redirect_ring_test,
	.mrp_port_forward_ring_test = lan9645x_mrp_port_forward_ring_test,
	.mrp_port_rewrite_ring_test = lan9645x_mrp_port_rewrite_ring_test,
	.mrp_port_process_ring_test = lan9645x_mrp_port_process_ring_test,

	.mrp_port_get_ring_interrupt_status = lan9645x_mrp_port_get_ring_intr_status,
	.mrp_port_disable_ring_interrupt = lan9645x_mrp_port_disable_ring_intr,
	.mrp_port_enable_ring_interrupt = lan9645x_mrp_port_enable_ring_intr,

	.mrp_port_get_in_interrupt_status = lan9645x_mrp_port_get_in_intr_status,
	.mrp_port_disable_in_interrupt = lan9645x_mrp_port_disable_in_intr,
	.mrp_port_enable_in_interrupt = lan9645x_mrp_port_enable_in_intr,

	.mrp_port_terminate_in_test = lan9645x_mrp_port_terminate_in_test,
	.mrp_port_forward_in_test = lan9645x_mrp_port_forward_in_test,
	.mrp_port_forward_rem_in_test = lan9645x_mrp_port_forward_rem_in_test,
	.mrp_port_process_in_test = lan9645x_mrp_port_process_in_test,
	.mrp_port_rewrite_in_test = lan9645x_mrp_port_rewrite_in_test,
};

int lan9645x_mrp_init(struct lan9645x *lan9645x)
{
	struct mrp_control *mrp_ctrl;
	struct lan9645x_port *port;
	int i;

	mrp_ctrl = kzalloc(sizeof(*mrp_ctrl), GFP_KERNEL);
	if (!mrp_ctrl)
		return -ENOMEM;

	mrp_ctrl->ops = &lan9645x_mrp_operations;
	mrp_ctrl->priv = lan9645x;
	mrp_ctrl->afi_ctrl = lan9645x->afi_ctrl;
	lan9645x->mrp_ctrl = mrp_ctrl;

	/* Initialize the controller before initialize the local */
	mrp_init(mrp_ctrl);

	/* Forward all MRP frames - by default */
	lan9645x_for_each_port(lan9645x, i, port) {
		if (!lan9645x_port_is_used(lan9645x, i))
			continue;

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
			lan9645x, MEP_MRP_FWD_CTRL(port->chip_port));

		lan9645x_mact_learn(lan9645x, PGID_MRP, mrp_test_dmac,
				    port->pvid, ENTRYTYPE_LOCKED);

		lan_wr(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(1) |
		       MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(1),
		       lan9645x, MEP_MRP_INTR_ENA(port->chip_port));
	}

	lan_rmw(MEP_INTR_CTRL_OAM_MEP_INTR_ENA_SET(1),
		MEP_INTR_CTRL_OAM_MEP_INTR_ENA,
		lan9645x, MEP_INTR_CTRL);

	/* Add PGID entry to discard all the frames */
	lan_rmw(0, ANA_PGID_PGID, lan9645x, ANA_PGID(PGID_MRP));

	return 0;
}

void lan9645x_mrp_uninit(struct lan9645x *lan9645x)
{
	struct lan9645x_port *port;
	int i;

	/* Forward all MRP frames - by default */
	lan9645x_for_each_port(lan9645x, i, port) {
		if (!lan9645x_port_is_used(lan9645x, i))
			continue;

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
			lan9645x, MEP_MRP_FWD_CTRL(port->chip_port));

		lan9645x_mact_forget(lan9645x, mrp_test_dmac,
				     port->pvid, ENTRYTYPE_LOCKED);

		lan_wr(MEP_MRP_INTR_ENA_TST_LOC_INTR_ENA_SET(0) |
		       MEP_MRP_INTR_ENA_ITST_LOC_INTR_ENA_SET(0),
		       lan9645x, MEP_MRP_INTR_ENA(port->chip_port));
	}

	lan_rmw(MEP_INTR_CTRL_OAM_MEP_INTR_ENA_SET(0),
		MEP_INTR_CTRL_OAM_MEP_INTR_ENA,
		lan9645x, MEP_INTR_CTRL);

	mrp_deinit(lan9645x->mrp_ctrl);

	kfree(lan9645x->mrp_ctrl);
}

void lan9645x_mrp_port_update_mrp_mac(struct lan9645x *lan9645x, int port,
				      const u8 mac[ETH_ALEN])
{
	struct lan9645x_port *p;

	p = lan9645x_to_port(lan9645x, port);

	if (!p->mrp_port)
		return;

	dev_dbg(lan9645x->dev, "port=%d mac=%pM\n", port, mac);

	lan9645x_mrp_port_update_mac(p->mrp_port);
	lan9645x_mrp_port_update_mrm_mac(p->mrp_port, mac);
}
