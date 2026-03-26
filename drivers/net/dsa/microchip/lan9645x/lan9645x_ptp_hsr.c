// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/ptp_classify.h>

#include "lan9645x_main.h"
#include "lan9645x_vcap_utils.h"

/* Add forwarding overwrite on frames from shadow port to a HSR port. This
 * enables directing frames to port A xor B.
 */
static int lan9645x_ptp_hsr_forwarding(struct lan9645x *lan9645x, int shadow,
				       int port, int rule_id)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	struct lan9645x_hsr_prp *hsr;
	struct lan9645x_port *p;
	struct net_device *ndev;
	struct vcap_rule *rule;
	int err, isdx;

	lockdep_assert_held(&lan9645x->hsr.lock);

	hsr = &lan9645x->hsr;
	isdx = hsr->isdx;
	p = lan9645x_to_port(lan9645x, port);
	ndev = lan9645x_chipport_to_ndev(lan9645x, port);

	if (vcap_rule_exists(lan9645x->vcap_ctrl, rule_id))
		return 0;

	lan9645x_is2_only_mac_etype_llc(lan9645x, S2_LOOKUP2, shadow);

	rule = vcap_alloc_rule(vctrl, ndev,
			       LAN9645X_VCAP_CID_IS2_L1,
			       VCAP_USER_HSR_PRP,
			       500, rule_id);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	/* Polymorphic rule type. Matches
	 * MAC_ETYPE, MAC_LLC and MAC_SNAP. They all have the keyfields below in
	 * the same position.
	 *
	 * Port key configuration ensures all frames are handled as one of these
	 * 3 types for this shadow port in this lookup.
	 */
	err = vcap_rule_add_key_u32(rule, VCAP_KF_TYPE, 0, 0xc);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, BIT(shadow), ~0);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_8021Q_VID_CLS, isdx, ~0);

	err |= vcap_rule_add_action_u32(rule, VCAP_AF_MASK_MODE, REDIRECT);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_PORT_MASK, BIT(port));
	err |= vcap_rule_add_action_bit(rule, VCAP_AF_CPU_DIS_MODE, VCAP_BIT_1);
	err |= vcap_rule_add_action_bit(rule, VCAP_AF_CPU_DIS, VCAP_BIT_1);
	err = err ? -EINVAL : lan9645x_vcap_rule_val_add(rule, ETH_P_ALL);
	vcap_free_rule(rule);
	return err;
}

static int lan9645x_ptp_l2_rew_cmd_add(struct lan9645x *lan9645x, int port,
				       u32 rew_op, u32 rule_id, int prio)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	struct lan9645x_hsr_prp *hsr;
	u32 pmask, payload_mask;
	struct net_device *ndev;
	struct vcap_rule *rule;
	int isdx, err = 0;

	hsr = &lan9645x->hsr;
	isdx = hsr->isdx;
	pmask = BIT(CPU_PORT) | BIT(hsr->shadow_ports[0]) |
		BIT(hsr->shadow_ports[1]);
	ndev = lan9645x_chipport_to_ndev(lan9645x, port);

	if (vcap_rule_exists(vctrl, rule_id))
		return 0;

	rule = vcap_alloc_rule(vctrl, ndev,
			       LAN9645X_VCAP_CID_IS2_L0,
			       VCAP_USER_HSR_PRP, prio, rule_id);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	err = vcap_set_rule_set_keyset(rule, VCAP_KFS_MAC_ETYPE);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, pmask, ~pmask);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_8021Q_VID_CLS, isdx, ~0);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_ETYPE, ETH_P_1588, ~0);

	/* L2_PAYLOAD0 matches the first 2 bytes in the payload. For PTP this
	 * contains:
	 *
	 * - transportSpecific (4bit)
	 * - messageType (4bit)
	 * - version (4bit)
	 * - reserved (4bit)
	 *
	 * One-step: match Sync only (msgtype == 0), mask all 4 msgtype bits.
	 * Two-step: match all event types (msgtype 0-3), mask bits [3:2] only.
	 */
	payload_mask = (rew_op == IFH_REW_OP_ONE_STEP_PTP) ? 0x0f00 : 0x0c00;
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_L2_PAYLOAD0, 0x0000,
				      payload_mask);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_REW_OP, rew_op);
	err = err ? -EINVAL : lan9645x_vcap_rule_val_add(rule, ETH_P_ALL);
	vcap_free_rule(rule);
	return err;
}

/* Add timestamping VCAP rules for L2 PTP event frames from HSR device. */
static int lan9645x_ptp_l2_rew_cmd(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p;
	int err;

	lockdep_assert_held(&lan9645x->hsr.lock);

	p = lan9645x_to_port(lan9645x, port);

	if (p->ptp_tx_cmd == IFH_REW_OP_ONE_STEP_PTP) {
		err = lan9645x_ptp_l2_rew_cmd_add(lan9645x, port,
						  IFH_REW_OP_ONE_STEP_PTP,
						  LAN9645X_VCAP_L2_PTP_SYNC_REW_CMD,
						  499);
		if (err)
			return err;
	}

	/* All event types (msgtype 0-3) with two-step. For one-step configs
	 * the Sync rule above takes priority for msgtype 0.
	 */
	return lan9645x_ptp_l2_rew_cmd_add(lan9645x, port,
					   IFH_REW_OP_TWO_STEP_PTP,
					   LAN9645X_VCAP_L2_PTP_REW_CMD, 500);
}

/* Add a single IP PTP REW_CMD VCAP rule.
 *
 * The 4 PTP message type bits are mapped to TCP flag fields in the
 * IP4/IP6_TCP_UDP keyset (overloaded for UDP PTP frames):
 *   bit 0: L4_SEQUENCE_EQ0_IS  (msgtype bit 0)
 *   bit 1: L4_FIN              (msgtype bit 1)
 *   bit 2: L4_SYN              (msgtype bit 2)
 *   bit 3: L4_RST              (msgtype bit 3)
 *
 * rew_op:  IFH_REW_OP_ONE_STEP_PTP matches Sync only (all 4 bits == 0).
 *          IFH_REW_OP_TWO_STEP_PTP matches all event types (bits [3:2] = 0,
 *          bits [1:0] = ANY).
 */
static int lan9645x_ptp_ip_rew_cmd_add(struct lan9645x *lan9645x, int port,
				       enum vcap_keyfield_set kset,
				       u32 rew_op, u32 rule_id, int prio)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	struct lan9645x_hsr_prp *hsr;
	struct net_device *ndev;
	struct vcap_rule *rule;
	int isdx, err = 0;
	enum vcap_bit b01;
	u32 pmask;
	u16 proto;

	hsr = &lan9645x->hsr;
	isdx = hsr->isdx;
	pmask = BIT(CPU_PORT) | BIT(hsr->shadow_ports[0]) |
		BIT(hsr->shadow_ports[1]);
	ndev = lan9645x_chipport_to_ndev(lan9645x, port);

	switch (kset) {
	case VCAP_KFS_IP4_TCP_UDP:
		proto = ETH_P_IP;
		break;
	case VCAP_KFS_IP6_TCP_UDP:
		proto = ETH_P_IPV6;
		break;
	default:
		return -EINVAL;
	}

	if (vcap_rule_exists(vctrl, rule_id))
		return 0;

	rule = vcap_alloc_rule(vctrl, ndev,
			       LAN9645X_VCAP_CID_IS2_L0,
			       VCAP_USER_HSR_PRP, prio, rule_id);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	b01 = (rew_op == IFH_REW_OP_ONE_STEP_PTP) ? VCAP_BIT_0 : VCAP_BIT_ANY;

	err = vcap_set_rule_set_keyset(rule, kset);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, pmask, ~pmask);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_8021Q_VID_CLS, isdx, ~0);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_L4_DPORT, PTP_EV_PORT, ~0);
	err |= vcap_rule_add_key_bit(rule, VCAP_KF_TCP_IS, VCAP_BIT_0);
	err |= vcap_rule_add_key_bit(rule, VCAP_KF_L4_SEQUENCE_EQ0_IS, b01);
	err |= vcap_rule_add_key_bit(rule, VCAP_KF_L4_FIN, b01);
	err |= vcap_rule_add_key_bit(rule, VCAP_KF_L4_SYN, VCAP_BIT_0);
	err |= vcap_rule_add_key_bit(rule, VCAP_KF_L4_RST, VCAP_BIT_0);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_REW_OP, rew_op);
	err = err ? -EINVAL : lan9645x_vcap_rule_val_add(rule, proto);

	vcap_free_rule(rule);
	return err;
}

/* Add timestamping VCAP rules for IP PTP event frames from HSR device. */
static int lan9645x_ptp_ip_rew_cmd(struct lan9645x *lan9645x, int port,
				   enum vcap_keyfield_set kset)
{
	struct lan9645x_port *p;
	u32 rid, sync_rid;
	int err;

	lockdep_assert_held(&lan9645x->hsr.lock);

	p = lan9645x_to_port(lan9645x, port);

	switch (kset) {
	case VCAP_KFS_IP4_TCP_UDP:
		rid = LAN9645X_VCAP_IPV4_PTP_REW_CMD;
		sync_rid = LAN9645X_VCAP_IPV4_PTP_SYNC_REW_CMD;
		break;
	case VCAP_KFS_IP6_TCP_UDP:
		rid = LAN9645X_VCAP_IPV6_PTP_REW_CMD;
		sync_rid = LAN9645X_VCAP_IPV6_PTP_SYNC_REW_CMD;
		break;
	default:
		return -EINVAL;
	}

	if (p->ptp_tx_cmd == IFH_REW_OP_ONE_STEP_PTP) {
		err = lan9645x_ptp_ip_rew_cmd_add(lan9645x, port, kset,
						  IFH_REW_OP_ONE_STEP_PTP,
						  sync_rid, 499);
		if (err)
			return err;
	}

	/* All event types with two-step. For one-step configs the Sync rule
	 * above takes priority for msgtype 0.
	 */
	return lan9645x_ptp_ip_rew_cmd_add(lan9645x, port, kset,
					    IFH_REW_OP_TWO_STEP_PTP,
					    rid, 500);
}

/* Remove all PTP HSR VCAP rules. */
static void __lan9645x_ptp_hsr_rules_delete(struct lan9645x *lan9645x)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	const int rules[] = {
		LAN9645X_VCAP_L2_PTP_REW_CMD,
		LAN9645X_VCAP_IPV4_PTP_REW_CMD,
		LAN9645X_VCAP_IPV6_PTP_REW_CMD,
		LAN9645X_VCAP_L2_PTP_SYNC_REW_CMD,
		LAN9645X_VCAP_IPV4_PTP_SYNC_REW_CMD,
		LAN9645X_VCAP_IPV6_PTP_SYNC_REW_CMD,
		LAN9645X_VCAP_IS2_HSR_FWD1,
		LAN9645X_VCAP_IS2_HSR_FWD2,
	};
	struct net_device *ndev;

	lockdep_assert_held(&lan9645x->hsr.lock);

	ndev = lan9645x_chipport_to_ndev(lan9645x, lan9645x->hsr.port_a);

	for (int j = 0; j < ARRAY_SIZE(rules); j++)
		vcap_del_rule(vctrl, ndev, rules[j]);
}

/* Restores HSR port to normal use, by removing PTP related VCAP rules. */
static void __lan9645x_ptp_hsr_port_deinit(struct lan9645x *lan9645x, int port)
{
	lockdep_assert_held(&lan9645x->hsr.lock);

	if (!lan9645x_port_is_hsr(lan9645x_to_port(lan9645x, port)))
		return;

	lan9645x->hsr.ptp_ports &= ~BIT(port);

	if (lan9645x->hsr.ptp_ports)
		return;

	__lan9645x_ptp_hsr_rules_delete(lan9645x);
}

static void lan9645x_ptp_hsr_port_deinit(struct lan9645x *lan9645x, int port)
{
	mutex_lock(&lan9645x->hsr.lock);
	__lan9645x_ptp_hsr_port_deinit(lan9645x, port);
	mutex_unlock(&lan9645x->hsr.lock);
}

/* Enable PTP on an existing HSR port, by adding VCAP rules to timestamp PTP
 * event frames, and forwarding overwrite from shadow ports, to direct PTP
 * frames to either A or B.
 */
static int lan9645x_ptp_hsr_port_init(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_hsr_prp *hsr;
	int err = 0;

	hsr = &lan9645x->hsr;
	mutex_lock(&hsr->lock);

	if (!lan9645x_port_is_hsr(lan9645x_to_port(lan9645x, port)))
		goto unlock;

	if (hsr->shadow_ports[1] < 0) {
		dev_warn(lan9645x->dev,
			 "PTP over HSR requires 2 shadow ports, skipping HSR PTP setup\n");
		goto unlock;
	}

	/* Detect stale state: if ptp_ports has bits for ports not in the
	 * current HSR pair, the old VCAP rules have wrong parameters. Delete
	 * them so they get recreated below.
	 */
	if (hsr->ptp_ports & ~(BIT(hsr->port_a) | BIT(hsr->port_b))) {
		dev_warn(lan9645x->dev,
			 "stale PTP HSR state (ptp_ports=0x%x), cleaning up\n",
			 hsr->ptp_ports);
		__lan9645x_ptp_hsr_rules_delete(lan9645x);
		hsr->ptp_ports = 0;
	}

	if (hsr->ptp_ports & BIT(port))
		goto unlock;

	hsr->ptp_ports |= BIT(port);

	/* Add VCAP rules to timestamp PTP event frames */
	err = lan9645x_ptp_ip_rew_cmd(lan9645x, port, VCAP_KFS_IP4_TCP_UDP);
	if (err)
		goto errout;

	err = lan9645x_ptp_ip_rew_cmd(lan9645x, port, VCAP_KFS_IP6_TCP_UDP);
	if (err)
		goto errout;

	err = lan9645x_ptp_l2_rew_cmd(lan9645x, port);
	if (err)
		goto errout;

	/* Add forwarding port mask overwrite for MAC lookup, so enable
	 * directing frames to specific HSR ports.
	 */
	err = lan9645x_ptp_hsr_forwarding(lan9645x, hsr->shadow_ports[0],
					  hsr->port_a,
					  LAN9645X_VCAP_IS2_HSR_FWD1);
	if (err)
		goto errout;

	err = lan9645x_ptp_hsr_forwarding(lan9645x, hsr->shadow_ports[1],
					  hsr->port_b,
					  LAN9645X_VCAP_IS2_HSR_FWD2);
	if (err)
		goto errout;

	mutex_unlock(&hsr->lock);
	return err;

errout:
	__lan9645x_ptp_hsr_port_deinit(lan9645x, port);
unlock:
	mutex_unlock(&hsr->lock);
	return err;
}

static int lan9645x_shadow_ports_valid(struct lan9645x *lan9645x)
{
	struct lan9645x_hsr_prp *hsr;

	hsr = &lan9645x->hsr;

	for (int i = 0; i < ARRAY_SIZE(hsr->shadow_ports); i++)
		if (!(0 <= hsr->shadow_ports[i] && hsr->shadow_ports[i] < CPU_PORT) ||
		    lan9645x_port_is_used(lan9645x, hsr->shadow_ports[i]))
			return false;

	return true;
}

static int lan9645x_hsr_find_shadows(struct lan9645x *lan9645x)
{
	struct lan9645x_hsr_prp *hsr;

	hsr = &lan9645x->hsr;

	hsr->shadow_ports[0] = -1;
	hsr->shadow_ports[1] = -1;

	if (!fwnode_property_read_u32_array(lan9645x->dev->fwnode,
					    "microchip,shadow-ports",
					    hsr->shadow_ports, 2)) {
		if (!lan9645x_shadow_ports_valid(lan9645x)) {
			dev_info(lan9645x->dev,
				 "Device tree shadow ports are invalid : %d, %d",
				 hsr->shadow_ports[0], hsr->shadow_ports[1]);
			return -EINVAL;
		}

		dev_info(lan9645x->dev, "Found shadow ports in device tree: %d, %d",
			 hsr->shadow_ports[0], hsr->shadow_ports[1]);

		lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1),
			QSYS_SW_PORT_MODE_PORT_ENA, lan9645x,
			QSYS_SW_PORT_MODE(hsr->shadow_ports[0]));

		lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1),
			QSYS_SW_PORT_MODE_PORT_ENA, lan9645x,
			QSYS_SW_PORT_MODE(hsr->shadow_ports[1]));
		return 0;
	}

	return -ENOSPC;
}

int lan9645x_ptp_hsr_init(struct lan9645x *lan9645x)
{
	if (lan9645x_hsr_find_shadows(lan9645x))
		dev_warn(lan9645x->dev,
			 "No port shadow ports found in device tree. PTP over HSR feature not available.");

	return 0;
}

int lan9645x_ptp_hsr_setup(struct lan9645x *lan9645x, int port,
			   struct kernel_hwtstamp_config *cfg)
{
	if (cfg->rx_filter == HWTSTAMP_FILTER_NONE) {
		lan9645x_ptp_hsr_port_deinit(lan9645x, port);
		return 0;
	} else {
		return lan9645x_ptp_hsr_port_init(lan9645x, port);
	}
}

/* Called in atomic context. Used in lan9645x_ptp.c */
struct sk_buff *lan9645x_ptp_hsr_tx_irq_skb_match(struct lan9645x_port *port)
{
	struct lan9645x *lan9645x = port->lan9645x;
	struct sk_buff *skb_match;
	struct sk_buff_head tmp;
	unsigned long flags;
	u32 tx_queue_sz;
	int purged;

	__skb_queue_head_init(&tmp);

	spin_lock_irqsave(&port->tx_skbs.lock, flags);
	tx_queue_sz = skb_queue_len(&port->tx_skbs);
	skb_match = __skb_dequeue_tail(&port->tx_skbs);

	/* Purge any remaining stale entries. */
	skb_queue_splice_init(&port->tx_skbs, &tmp);
	spin_unlock_irqrestore(&port->tx_skbs.lock, flags);

	purged = skb_queue_len(&tmp);
	__skb_queue_purge_reason(&tmp, SKB_DROP_REASON_QUEUE_PURGE);

	/* Next ts */
	lan_rmw(PTP_TWOSTEP_CTRL_NXT_SET(1), PTP_TWOSTEP_CTRL_NXT, lan9645x,
		PTP_TWOSTEP_CTRL);

	if (!skb_match) {
		dev_err_ratelimited(lan9645x->dev,
				    "Timestamp IRQ but no SKBs waiting for timestamp. Dropping timestamp on port %d.\n",
				    port->chip_port);
		return NULL;
	}

	spin_lock_irqsave(&lan9645x->ptp_ts_id_lock, flags);
	lan9645x->ptp_skbs -= 1 + purged;
	spin_unlock_irqrestore(&lan9645x->ptp_ts_id_lock, flags);

	if (purged) {
		dev_err_ratelimited(lan9645x->dev,
				    "Purged %d stale SKBs on port %d (had %u inflight)\n",
				    purged, port->chip_port, tx_queue_sz);
	}

	return skb_match;
}

static int lan9645x_ptp_hsr_egress_port(struct dsa_port *dp, u8 red_ports)
{
	struct lan9645x *lan9645x;

	lan9645x = dp->ds->priv;

	switch (red_ports) {
	case BIT(0):
		return lan9645x->hsr.shadow_ports[0];
	case BIT(1):
		return lan9645x->hsr.shadow_ports[1];
	default:
		return dp->ds->num_ports;
	}
}

/* Called in atomic context. Used in tag_lan9645x.c */
int lan9645x_ptp_hsr_xmit_masq_port(struct sk_buff *skb, struct dsa_port *dp)
{
	struct skb_redundancy_info *sred = skb_redinfo(skb);

	if (sred && REDINFO_T(skb) == DIRECTED_TX)
		return lan9645x_ptp_hsr_egress_port(dp, REDINFO_PORTS(skb));

	/* CPU port */
	return dp->ds->num_ports;
}

static u32 lan9645x_ptp_hsr_dp2ioport(struct dsa_port *dp)
{
	struct lan9645x *lan9645x;

	lan9645x = dp->ds->priv;

	if (!lan9645x_port_is_hsr(lan9645x_to_port(lan9645x, dp->index)))
		return 0;

	if (lan9645x->hsr.port_a == dp->index)
		return BIT(0);

	if (lan9645x->hsr.port_b == dp->index)
		return BIT(1);

	return 0;
}

/* Called in atomic context. Used in tag_lan9645x.c */
void lan964x5_set_redundancy_info(struct sk_buff *skb, int rtagd,
				  struct dsa_port *dp)
{
	struct skb_redundancy_info *sred;

	/* No HSR tag in frame */
	if (rtagd == 0)
		return;

	/* Only this field is used. The others can be read in the hsr tag
	 * fragment if necessary.
	 */
	sred = skb_redinfo(skb);
	sred->io_port = PTP_MSG_IN | lan9645x_ptp_hsr_dp2ioport(dp);
}

void lan9645x_ptp_hsr_flush_tx_skbs(struct lan9645x_port *port)
{
	struct lan9645x *lan9645x = port->lan9645x;
	struct sk_buff_head tmp;
	unsigned long flags;
	int purged;

	if (skb_queue_empty_lockless(&port->tx_skbs))
		return;

	__skb_queue_head_init(&tmp);

	spin_lock_irqsave(&port->tx_skbs.lock, flags);
	skb_queue_splice_init(&port->tx_skbs, &tmp);
	spin_unlock_irqrestore(&port->tx_skbs.lock, flags);

	purged = skb_queue_len(&tmp);

	__skb_queue_purge_reason(&tmp, SKB_DROP_REASON_QUEUE_PURGE);

	if (purged) {
		spin_lock_irqsave(&lan9645x->ptp_ts_id_lock, flags);
		lan9645x->ptp_skbs -= purged;
		spin_unlock_irqrestore(&lan9645x->ptp_ts_id_lock, flags);
	}
}
