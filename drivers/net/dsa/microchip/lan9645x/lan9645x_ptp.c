// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/ptp_classify.h>
#include <linux/debugfs.h>
#include <linux/dsa/lan9645x.h>

#include "lan9645x_main.h"
#include "lan9645x_vcap_utils.h"

#define LAN9645X_PTP_TIMEOUT		msecs_to_jiffies(10)

#define LAN9645X_MAX_PTP_ID	512

/* Represents 1ppm adjustment in 2^59 format with 6.037735849ns as reference
 * The value is calculated as following: (1/1000000)/((2^-59)/6.037735849)
 */
#define LAN9645X_1PPM_FORMAT		3480517749723LL

/* Represents 1ppb adjustment in 2^29 format with 6.037735849ns as reference
 * The value is calculated as following: (1/1000000000)/((2^59)/6.037735849)
 */
#define LAN9645X_1PPB_FORMAT		3480517749LL

#define TOD_ACC_PIN		0x4

enum {
	PTP_PIN_ACTION_IDLE = 0,
	PTP_PIN_ACTION_LOAD,
	PTP_PIN_ACTION_SAVE,
	PTP_PIN_ACTION_CLOCK,
	PTP_PIN_ACTION_DELTA,
	PTP_PIN_ACTION_TOD
};

static u64 lan9645x_ptp_get_nominal_value(void)
{
	/* This is the default value that for each system clock, the time of day
	 * is increased. It has the format 5.59 nanosecond.
	 */
	return 0x304d4873ecade304;
}

static int lan9645x_ptp_add_trap(struct lan9645x_port *port,
				 int (*add_ptp_key)(struct vcap_rule *vrule,
						    struct lan9645x_port*),
				 u32 rule_id,
				 u16 proto)
{
	struct lan9645x *lan9645x = port->lan9645x;
	struct vcap_rule *vrule;
	struct net_device *dev;
	int err;

	dev = lan9645x_port_to_ndev(port);

	vrule = vcap_get_rule(lan9645x->vcap_ctrl, rule_id);
	if (!IS_ERR(vrule)) {
		u32 value, mask;

		/* Just modify the ingress port mask and exit */
		err = vcap_rule_get_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK,
					    &value, &mask);
		if (err)
			goto free_rule;

		mask &= ~BIT(port->chip_port);
		err = vcap_rule_mod_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK,
					    value, mask);
		if (!err)
			err = vcap_mod_rule(vrule);

		goto free_rule;
	}

	vrule = vcap_alloc_rule(lan9645x->vcap_ctrl, dev,
				LAN9645X_VCAP_CID_IS2_L0,
				VCAP_USER_PTP, 0, rule_id);
	if (IS_ERR(vrule))
		return PTR_ERR(vrule);

	err = add_ptp_key(vrule, port);
	if (err)
		goto free_rule;

	err = vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_COPY_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, PERMIT_MASK);
	err |= vcap_val_rule(vrule, proto);
	if (err)
		goto free_rule;

	err = vcap_add_rule(vrule);

free_rule:
	/* Free the local copy of the rule */
	vcap_free_rule(vrule);
	return err;
}

static int lan9645x_ptp_del_trap(struct lan9645x_port *port,
				 u32 rule_id)
{
	struct lan9645x *lan9645x = port->lan9645x;
	struct vcap_rule *vrule;
	struct net_device *dev;
	u32 value, mask;
	int err;

	dev = lan9645x_port_to_ndev(port);

	vrule = vcap_get_rule(lan9645x->vcap_ctrl, rule_id);
	if (IS_ERR(vrule))
		return -EEXIST;

	err = vcap_rule_get_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, &value,
				    &mask);
	if (err)
		goto free_rule;

	mask |= BIT(port->chip_port);

	/* No other port requires this trap, so it is safe to remove it */
	if (mask == GENMASK(lan9645x->num_phys_ports, 0)) {
		err = vcap_del_rule(lan9645x->vcap_ctrl, dev, rule_id);
		goto free_rule;
	}

	err = vcap_rule_mod_key_u32(vrule, VCAP_KF_IF_IGR_PORT_MASK, value,
				    mask);
	if (!err)
		err = vcap_mod_rule(vrule);

free_rule:
	vcap_free_rule(vrule);
	return err;
}

static int lan9645x_ptp_add_l2_key(struct vcap_rule *vrule,
				   struct lan9645x_port *port)
{
	return vcap_rule_add_key_u32(vrule, VCAP_KF_ETYPE, ETH_P_1588, ~0);
}

static int lan9645x_ptp_add_ip_event_key(struct vcap_rule *vrule,
					 struct lan9645x_port *port)
{
	return vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, PTP_EV_PORT, ~0) ||
		vcap_rule_add_key_bit(vrule, VCAP_KF_TCP_IS, VCAP_BIT_0);
}

static int lan9645x_ptp_add_ip_general_key(struct vcap_rule *vrule,
					   struct lan9645x_port *port)
{
	return vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, PTP_GEN_PORT, ~0) ||
		vcap_rule_add_key_bit(vrule, VCAP_KF_TCP_IS, VCAP_BIT_0);
}

static int lan9645x_ptp_add_l2_rule(struct lan9645x_port *port)
{
	return lan9645x_ptp_add_trap(port, lan9645x_ptp_add_l2_key,
				     LAN9645X_VCAP_L2_PTP_TRAP, ETH_P_ALL);
}

static int lan9645x_ptp_add_ipv4_rules(struct lan9645x_port *port)
{
	int err;

	err = lan9645x_ptp_add_trap(port, lan9645x_ptp_add_ip_event_key,
				    LAN9645X_VCAP_IPV4_EV_PTP_TRAP, ETH_P_IP);
	if (err)
		return err;

	err = lan9645x_ptp_add_trap(port, lan9645x_ptp_add_ip_general_key,
				    LAN9645X_VCAP_IPV4_GEN_PTP_TRAP, ETH_P_IP);
	if (err)
		lan9645x_ptp_del_trap(port, LAN9645X_VCAP_IPV4_EV_PTP_TRAP);

	return err;
}

static int lan9645x_ptp_add_ipv6_rules(struct lan9645x_port *port)
{
	int err;

	err = lan9645x_ptp_add_trap(port, lan9645x_ptp_add_ip_event_key,
				    LAN9645X_VCAP_IPV6_EV_PTP_TRAP, ETH_P_IPV6);
	if (err)
		return err;

	err = lan9645x_ptp_add_trap(port, lan9645x_ptp_add_ip_general_key,
				    LAN9645X_VCAP_IPV6_GEN_PTP_TRAP, ETH_P_IPV6);
	if (err)
		lan9645x_ptp_del_trap(port, LAN9645X_VCAP_IPV6_EV_PTP_TRAP);

	return err;
}

static int lan9645x_ptp_del_l2_rule(struct lan9645x_port *port)
{
	return lan9645x_ptp_del_trap(port, LAN9645X_VCAP_L2_PTP_TRAP);
}

static int lan9645x_ptp_del_ipv4_rules(struct lan9645x_port *port)
{
	int err;

	err = lan9645x_ptp_del_trap(port, LAN9645X_VCAP_IPV4_EV_PTP_TRAP);
	if (err)
		return err;

	return lan9645x_ptp_del_trap(port, LAN9645X_VCAP_IPV4_GEN_PTP_TRAP);
}

static int lan9645x_ptp_del_ipv6_rules(struct lan9645x_port *port)
{
	int err;

	err = lan9645x_ptp_del_trap(port, LAN9645X_VCAP_IPV6_EV_PTP_TRAP);
	if (err)
		return err;

	return lan9645x_ptp_del_trap(port, LAN9645X_VCAP_IPV6_GEN_PTP_TRAP);
}

static int lan9645x_ptp_add_traps(struct lan9645x_port *port)
{
	int err;

	err = lan9645x_ptp_add_l2_rule(port);
	if (err)
		goto err_l2;

	err = lan9645x_ptp_add_ipv4_rules(port);
	if (err)
		goto err_ipv4;

	err = lan9645x_ptp_add_ipv6_rules(port);
	if (err)
		goto err_ipv6;

	return err;

err_ipv6:
	lan9645x_ptp_del_ipv4_rules(port);
err_ipv4:
	lan9645x_ptp_del_l2_rule(port);
err_l2:
	return err;
}

static int lan9645x_ptp_del_traps(struct lan9645x_port *port)
{
	int err;

	err = lan9645x_ptp_del_l2_rule(port);
	if (err)
		return err;

	err = lan9645x_ptp_del_ipv4_rules(port);
	if (err)
		return err;

	return lan9645x_ptp_del_ipv6_rules(port);
}

static int lan9645x_ptp_setup_traps(struct lan9645x_port *port,
				    struct kernel_hwtstamp_config *cfg)
{
	if (cfg->rx_filter == HWTSTAMP_FILTER_NONE)
		return lan9645x_ptp_del_traps(port);
	else
		return lan9645x_ptp_add_traps(port);
}

int lan9645x_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config,
			       struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = ds->priv;
	struct kernel_hwtstamp_config cfg;
	struct lan9645x_phc *phc;
	struct lan9645x_port *p;
	int err = 0;

	memcpy(&cfg, config, sizeof(struct kernel_hwtstamp_config));

	p = lan9645x_to_port(lan9645x, port);

	switch (cfg.tx_type) {
	case HWTSTAMP_TX_ON:
		p->ptp_tx_cmd = IFH_REW_OP_TWO_STEP_PTP;
		break;
	case HWTSTAMP_TX_ONESTEP_SYNC:
		/* The rewriter applies REW_OP uniformly to all egress
		 * copies of a frame, including the CPU trap copy taken
		 * from the IS2 hit. Under HSR Hybrid Clock every Sync is
		 * trapped to the CPU for software forwarding to the
		 * opposite ring leg, so a 1-step originTimestamp/cF
		 * update on the line side would also corrupt the trapped
		 * copy that ptp4l forwards. Reject 1-step on HSR ports
		 * up front instead of producing silently broken Syncs.
		 */
		if (lan9645x_port_is_hsr(p)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "1-step Sync not supported on HSR ports; use HWTSTAMP_TX_ON");
			return -EOPNOTSUPP;
		}
		p->ptp_tx_cmd = IFH_REW_OP_ONE_STEP_PTP;
		break;
	case HWTSTAMP_TX_OFF:
		p->ptp_tx_cmd = IFH_REW_OP_NOOP;
		break;
	default:
		return -ERANGE;
	}

	switch (cfg.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		p->ptp_rx_cmd = false;
		lan9645x_ptp_hsr_flush_tx_skbs(p);
		break;
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
	case HWTSTAMP_FILTER_NTP_ALL:
		p->ptp_rx_cmd = true;
		cfg.rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	default:
		return -ERANGE;
	}

	err = lan9645x_ptp_setup_traps(p, &cfg);
	if (err)
		return err;

	err = lan9645x_ptp_hsr_setup(lan9645x, port, &cfg);
	if (err)
		return err;

	/* Commit back the result & save it */
	mutex_lock(&lan9645x->ptp_lock);
	phc = &lan9645x->phc[LAN9645X_PHC_PORT];
	phc->hwtstamp_config = cfg;
	mutex_unlock(&lan9645x->ptp_lock);

	return 0;
}

int lan9645x_port_hwtstamp_get(struct dsa_switch *ds, int port,
                               struct kernel_hwtstamp_config *config)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_phc *phc;

	dev_dbg(lan9645x->dev, "port=%d", port);
	phc = &lan9645x->phc[LAN9645X_PHC_PORT];
	*config = phc->hwtstamp_config;

	return 0;
}

static void lan9645x_ptp_classify(struct lan9645x_port *port, struct sk_buff *skb,
				  u8 *rew_op, u8 *pdu_type)
{
	struct ptp_header *header;
	u8 msgtype;
	int type;

	if (port->ptp_tx_cmd == IFH_REW_OP_NOOP) {
		*rew_op = IFH_REW_OP_NOOP;
		*pdu_type = IFH_PDU_TYPE_NONE;
		return;
	}

	type = ptp_classify_raw(skb);
	if (type == PTP_CLASS_NONE) {
		*rew_op = IFH_REW_OP_NOOP;
		*pdu_type = IFH_PDU_TYPE_NONE;
		return;
	}

	header = ptp_parse_header(skb, type);
	if (!header) {
		*rew_op = IFH_REW_OP_NOOP;
		*pdu_type = IFH_PDU_TYPE_NONE;
		return;
	}

	if (type & PTP_CLASS_L2)
		*pdu_type = IFH_PDU_TYPE_NONE;
	if (type & PTP_CLASS_IPV4)
		*pdu_type = IFH_PDU_TYPE_IPV4;
	if (type & PTP_CLASS_IPV6)
		*pdu_type = IFH_PDU_TYPE_IPV6;

	if (port->ptp_tx_cmd == IFH_REW_OP_TWO_STEP_PTP) {
		*rew_op = IFH_REW_OP_TWO_STEP_PTP;
		return;
	}

	/* If it is sync and run 1 step then set the correct operation,
	 * otherwise run as 2 step
	 */
	msgtype = ptp_get_msgtype(header, type);
	if ((msgtype & 0xf) == PTP_MSGTYPE_SYNC) {
		*rew_op = IFH_REW_OP_ONE_STEP_PTP;
		return;
	}

	*rew_op = IFH_REW_OP_TWO_STEP_PTP;
}

static void lan9645x_ptp_txtstamp_old_release(struct lan9645x_port *p)
{
	struct sk_buff *skb, *skb_tmp;
	unsigned long flags;

	spin_lock_irqsave(&p->tx_skbs.lock, flags);
	skb_queue_walk_safe(&p->tx_skbs, skb, skb_tmp) {
		if time_after(LAN9645X_SKB_CB(skb)->jiffies + LAN9645X_PTP_TIMEOUT,
			      jiffies)
			break;
		dev_warn_ratelimited(p->lan9645x->dev,
				     "port %d invalidating stale timestamp ID %u which seems lost\n",
				     p->chip_port, LAN9645X_SKB_CB(skb)->ts_id);
		__skb_unlink(skb, &p->tx_skbs);
		kfree_skb(skb);
	}
	spin_unlock_irqrestore(&p->tx_skbs.lock, flags);
}

static int lan9645x_ptp_txtstamp_request(struct lan9645x *lan9645x, int port,
					 struct sk_buff *skb,
					 struct sk_buff **clone)
{
	struct lan9645x_port *p;
	unsigned long flags;
	u8 pdu_type;
	u8 rew_op;

	if (!lan9645x->ptp)
		return 0;

	p = lan9645x_to_port(lan9645x, port);

	lan9645x_ptp_classify(p, skb, &rew_op, &pdu_type);

	/* If the packet is not a ptp packet, ptp_classify will return
	 * IFH_REW_OP_NOOP. However if a socket option is used to enable
	 * hardware tx timestamping, we still want to timestamp the packet,
	 * regardless of the packet type.
	 */
	if (rew_op == IFH_REW_OP_NOOP &&
	    p->ptp_tx_cmd == IFH_REW_OP_TWO_STEP_PTP)
		rew_op = IFH_REW_OP_TWO_STEP_PTP;

	LAN9645X_SKB_CB(skb)->rew_op = rew_op;
	LAN9645X_SKB_CB(skb)->pdu_type = pdu_type;

	if (rew_op != IFH_REW_OP_TWO_STEP_PTP)
		return 0;

	*clone = skb_clone_sk(skb);
	if (!(*clone))
		return -ENOMEM;

	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	lan9645x_ptp_txtstamp_old_release(p);

	spin_lock_irqsave(&lan9645x->ptp_ts_id_lock, flags);
	if (lan9645x->ptp_skbs == LAN9645X_MAX_PTP_ID) {
		spin_unlock_irqrestore(&lan9645x->ptp_ts_id_lock, flags);
		kfree_skb(*clone);
		return -EBUSY;
	}

	skb_shinfo(*clone)->tx_flags |= SKBTX_IN_PROGRESS;
	LAN9645X_SKB_CB(*clone)->pdu_type = pdu_type;
	LAN9645X_SKB_CB(*clone)->ts_id = p->ts_id;
	LAN9645X_SKB_CB(*clone)->jiffies = jiffies;
	skb_queue_tail(&p->tx_skbs, *clone);

	lan9645x->ptp_skbs++;
	p->ts_id++;
	if (p->ts_id >= LAN9645X_MAX_PTP_ID)
		p->ts_id = 0;

	spin_unlock_irqrestore(&lan9645x->ptp_ts_id_lock, flags);

	return 0;
}

static void lan9645x_get_hwtimestamp(struct lan9645x *lan9645x,
				     struct timespec64 *ts,
				     u32 nsec)
{
	/* Read current PTP time to get seconds */
	u32 curr_nsec;

	mutex_lock(&lan9645x->ptp_clock_lock);

	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		PTP_PIN_CFG_PIN_DOM_SET(LAN9645X_PHC_PORT) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan9645x, PTP_PIN_CFG(TOD_ACC_PIN));

	ts->tv_sec = lan_rd(lan9645x, PTP_TOD_SEC_LSB(TOD_ACC_PIN));
	curr_nsec = lan_rd(lan9645x, PTP_TOD_NSEC(TOD_ACC_PIN));

	ts->tv_nsec = nsec;

	/* Sec has incremented since the ts was registered */
	if (curr_nsec < nsec)
		ts->tv_sec--;

	mutex_unlock(&lan9645x->ptp_clock_lock);
}

static struct sk_buff *lan9645x_ptp_tx_irq_skb_match(struct lan9645x_port *port,
						     u32 id)
{
	struct sk_buff *skb, *skb_tmp, *skb_match = NULL;
	struct lan9645x *lan9645x = port->lan9645x;
	unsigned long flags;

	spin_lock_irqsave(&port->tx_skbs.lock, flags);
	skb_queue_walk_safe(&port->tx_skbs, skb, skb_tmp)
	{
		if (LAN9645X_SKB_CB(skb)->ts_id != id)
			continue;

		__skb_unlink(skb, &port->tx_skbs);
		skb_match = skb;
		break;
	}
	spin_unlock_irqrestore(&port->tx_skbs.lock, flags);

	/* Next ts */
	lan_rmw(PTP_TWOSTEP_CTRL_NXT_SET(1), PTP_TWOSTEP_CTRL_NXT, lan9645x,
		PTP_TWOSTEP_CTRL);

	if (WARN_ON(!skb_match))
		return NULL;

	spin_lock_irqsave(&lan9645x->ptp_ts_id_lock, flags);
	lan9645x->ptp_skbs--;
	spin_unlock_irqrestore(&lan9645x->ptp_ts_id_lock, flags);

	return skb_match;
}

irqreturn_t lan9645x_ptp_irq_handler(int irq, void *args)
{
	int budget = LAN9645X_MAX_PTP_ID;
	struct lan9645x *lan9645x = args;

	while (budget--) {
		struct skb_shared_hwtstamps shhwtstamps;
		struct lan9645x_port *port;
		struct sk_buff *skb_match;
		struct timespec64 ts;
		u32 val, id, txport;
		u32 sub_ns;
		u32 delay;

		val = lan_rd(lan9645x, PTP_TWOSTEP_CTRL);

		/* Check if a timestamp can be retrieved */
		if (!(val & PTP_TWOSTEP_CTRL_VLD))
			break;

		WARN_ON(val & PTP_TWOSTEP_CTRL_OVFL);

		if (!(val & PTP_TWOSTEP_CTRL_STAMP_TX))
			continue;

		/* Retrieve the ts Tx port */
		txport = PTP_TWOSTEP_CTRL_STAMP_PORT_GET(val);

		/* Retrieve its associated skb */
		port = lan9645x->ports[txport];

		/* Retrieve the delay */
		delay = lan_rd(lan9645x, PTP_TWOSTEP_STAMP_NSEC);
		delay = PTP_TWOSTEP_STAMP_NSEC_STAMP_NSEC_GET(delay);

		sub_ns = lan_rd(lan9645x, PTP_TWOSTEP_STAMP_SUBNS);
		sub_ns = PTP_TWOSTEP_STAMP_SUBNS_STAMP_SUB_NSEC_GET(sub_ns);

		/* Get next timestamp from fifo, which needs to be the
		 * rx timestamp which represents the id of the frame
		 */
		lan_rmw(PTP_TWOSTEP_CTRL_NXT_SET(1),
			PTP_TWOSTEP_CTRL_NXT,
			lan9645x, PTP_TWOSTEP_CTRL);

		val = lan_rd(lan9645x, PTP_TWOSTEP_CTRL);

		/* Check if a timestamp can be retried */
		if (!(val & PTP_TWOSTEP_CTRL_VLD))
			break;

		/* Read RX timestamping to get the ID */
		id = lan_rd(lan9645x, PTP_TWOSTEP_STAMP_NSEC);

		if (lan9645x_port_is_hsr(port))
			skb_match = lan9645x_ptp_hsr_tx_irq_skb_match(port);
		else
			skb_match = lan9645x_ptp_tx_irq_skb_match(port, id);
		if (IS_ERR_OR_NULL(skb_match))
			continue;

		/* Get the h/w timestamp */
		lan9645x_get_hwtimestamp(lan9645x, &ts, delay);

		lan9645x_ptp_log_tx(lan9645x, skb_match, ts, sub_ns);

		/* Set the timestamp into the skb */
		shhwtstamps.hwtstamp = ktime_set(ts.tv_sec, ts.tv_nsec);
		skb_tstamp_tx(skb_match, &shhwtstamps);

		dev_kfree_skb_any(skb_match);
	}

	return IRQ_HANDLED;
}

irqreturn_t lan9645x_ptp_ext_irq_handler(int irq, void *args)
{
	struct lan9645x *lan9645x = args;
	struct lan9645x_phc *phc;
	u64 time = 0;
	time64_t s;
	int pin, i;
	s64 ns;

	if (!(lan_rd(lan9645x, PTP_PIN_INTR)))
		return IRQ_NONE;

	/* Go through all domains and see which pin generated the interrupt */
	for (i = 0; i < LAN9645X_PHC_COUNT; ++i) {
		struct ptp_clock_event ptp_event = {0};

		phc = &lan9645x->phc[i];
		pin = ptp_find_pin_unlocked(phc->clock, PTP_PF_EXTTS, 0);
		if (pin == -1)
			continue;

		if (!(lan_rd(lan9645x, PTP_PIN_INTR) & BIT(pin)))
			continue;

		mutex_lock(&lan9645x->ptp_clock_lock);

		/* Enable to get the new interrupt.
		 * By writing 1 it clears the bit
		 */
		lan_wr(BIT(pin), lan9645x, PTP_PIN_INTR);

		/* Get current time */
		s = lan_rd(lan9645x, PTP_TOD_SEC_MSB(pin));
		s <<= 32;
		s |= lan_rd(lan9645x, PTP_TOD_SEC_LSB(pin));
		ns = lan_rd(lan9645x, PTP_TOD_NSEC(pin));
		ns &= PTP_TOD_NSEC_TOD_NSEC;

		mutex_unlock(&lan9645x->ptp_clock_lock);

		if ((ns & 0xFFFFFFF0) == 0x3FFFFFF0) {
			s--;
			ns &= 0xf;
			ns += 999999984;
		}
		time = ktime_set(s, ns);

		ptp_event.index = pin;
		ptp_event.timestamp = time;
		ptp_event.type = PTP_CLOCK_EXTTS;
		ptp_clock_event(phc->clock, &ptp_event);
	}

	return IRQ_HANDLED;
}

static int lan9645x_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;
	bool neg_adj = 0;
	u64 tod_inc;
	u64 ref;

	if (!scaled_ppm)
		return 0;

	if (scaled_ppm < 0) {
		neg_adj = 1;
		scaled_ppm = -scaled_ppm;
	}

	tod_inc = lan9645x_ptp_get_nominal_value();

	/* The multiplication is split in 2 separate additions because of
	 * overflow issues. If scaled_ppm with 16bit fractional part was bigger
	 * than 20ppm then we got overflow.
	 */
	ref = LAN9645X_1PPM_FORMAT * (scaled_ppm >> 16);
	ref += (LAN9645X_1PPM_FORMAT * (0xffff & scaled_ppm)) >> 16;
	tod_inc = neg_adj ? tod_inc - ref : tod_inc + ref;

	mutex_lock(&lan9645x->ptp_clock_lock);

	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(1 << BIT(phc->index)),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan9645x, PTP_DOM_CFG);

	lan_wr((u32)tod_inc & 0xFFFFFFFF, lan9645x,
	       PTP_CLK_PER_CFG(phc->index, 0));
	lan_wr((u32)(tod_inc >> 32), lan9645x,
	       PTP_CLK_PER_CFG(phc->index, 1));

	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(0),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan9645x, PTP_DOM_CFG);

	mutex_unlock(&lan9645x->ptp_clock_lock);

	return 0;
}

static int lan9645x_ptp_settime64(struct ptp_clock_info *ptp,
				  const struct timespec64 *ts)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;

	mutex_lock(&lan9645x->ptp_clock_lock);

	/* Must be in IDLE mode before the time can be loaded */
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
		PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan9645x, PTP_PIN_CFG(TOD_ACC_PIN));

	/* Set new value */
	lan_wr(PTP_TOD_SEC_MSB_TOD_SEC_MSB_SET(upper_32_bits(ts->tv_sec)),
	       lan9645x, PTP_TOD_SEC_MSB(TOD_ACC_PIN));
	lan_wr(lower_32_bits(ts->tv_sec),
	       lan9645x, PTP_TOD_SEC_LSB(TOD_ACC_PIN));
	lan_wr(ts->tv_nsec, lan9645x, PTP_TOD_NSEC(TOD_ACC_PIN));

	/* Apply new values */
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_LOAD) |
		PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan9645x, PTP_PIN_CFG(TOD_ACC_PIN));

	mutex_unlock(&lan9645x->ptp_clock_lock);

	return 0;
}

int lan9645x_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;
	time64_t s;
	s64 ns;

	mutex_lock(&lan9645x->ptp_clock_lock);

	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan9645x, PTP_PIN_CFG(TOD_ACC_PIN));

	s = lan_rd(lan9645x, PTP_TOD_SEC_MSB(TOD_ACC_PIN));
	s <<= 32;
	s |= lan_rd(lan9645x, PTP_TOD_SEC_LSB(TOD_ACC_PIN));
	ns = lan_rd(lan9645x, PTP_TOD_NSEC(TOD_ACC_PIN));
	ns &= PTP_TOD_NSEC_TOD_NSEC;

	mutex_unlock(&lan9645x->ptp_clock_lock);

	/* Deal with negative values */
	if ((ns & 0xFFFFFFF0) == 0x3FFFFFF0) {
		s--;
		ns &= 0xf;
		ns += 999999984;
	}

	set_normalized_timespec64(ts, s, ns);
	return 0;
}

static int lan9645x_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;

	if (delta > -(NSEC_PER_SEC / 2) && delta < (NSEC_PER_SEC / 2)) {
		mutex_lock(&lan9645x->ptp_clock_lock);

		/* Must be in IDLE mode before the time can be loaded */
		lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
			PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
			PTP_PIN_CFG_PIN_SYNC_SET(0),
			PTP_PIN_CFG_PIN_ACTION |
			PTP_PIN_CFG_PIN_DOM |
			PTP_PIN_CFG_PIN_SYNC,
			lan9645x, PTP_PIN_CFG(TOD_ACC_PIN));

		lan_wr(PTP_TOD_NSEC_TOD_NSEC_SET(delta),
		       lan9645x, PTP_TOD_NSEC(TOD_ACC_PIN));

		/* Adjust time with the value of PTP_TOD_NSEC */
		lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_DELTA) |
			PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
			PTP_PIN_CFG_PIN_SYNC_SET(0),
			PTP_PIN_CFG_PIN_ACTION |
			PTP_PIN_CFG_PIN_DOM |
			PTP_PIN_CFG_PIN_SYNC,
			lan9645x, PTP_PIN_CFG(TOD_ACC_PIN));

		mutex_unlock(&lan9645x->ptp_clock_lock);
	} else {
		/* Fall back using lan9645x_ptp_settime64 which is not exact */
		struct timespec64 ts;
		u64 now;

		lan9645x_ptp_gettime64(ptp, &ts);

		now = ktime_to_ns(timespec64_to_ktime(ts));
		ts = ns_to_timespec64(now + delta);

		lan9645x_ptp_settime64(ptp, &ts);
	}

	return 0;
}

static int lan9645x_ptp_verify(struct ptp_clock_info *ptp, unsigned int pin,
			       enum ptp_pin_function func, unsigned int chan)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;
	struct ptp_clock_info *info;
	int i;

	/* Currently support only 1 channel */
	if (chan != 0)
		return -1;

	switch (func) {
	case PTP_PF_NONE:
	case PTP_PF_PEROUT:
	case PTP_PF_EXTTS:
		break;
	default:
		return -1;
	}

	/* The PTP pins are shared by all the PHC. So it is required to see if
	 * the pin is connected to another PHC. The pin is connected to another
	 * PHC if that pin already has a function on that PHC.
	 */
	for (i = 0; i < LAN9645X_PHC_COUNT; ++i) {
		info = &lan9645x->phc[i].info;

		/* Ignore the check with ourself */
		if (ptp == info)
			continue;

		if (info->pin_config[pin].func == PTP_PF_PEROUT ||
		    info->pin_config[pin].func == PTP_PF_EXTTS)
			return -1;
	}

	return 0;
}

static int lan9645x_ptp_perout(struct ptp_clock_info *ptp,
			       struct ptp_clock_request *rq, int on)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;
	struct timespec64 ts_phase, ts_period;
	s64 wf_high, wf_low;
	bool pps = false;
	int pin;

	if (rq->perout.flags & ~(PTP_PEROUT_DUTY_CYCLE |
				 PTP_PEROUT_PHASE))
		return -EOPNOTSUPP;

	pin = ptp_find_pin(phc->clock, PTP_PF_PEROUT, rq->perout.index);
	if (pin == -1 || pin >= LAN9645X_PHC_PINS_NUM)
		return -EINVAL;

	if (!on) {
		mutex_lock(&lan9645x->ptp_clock_lock);
		lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
			PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
			PTP_PIN_CFG_PIN_SYNC_SET(0),
			PTP_PIN_CFG_PIN_ACTION |
			PTP_PIN_CFG_PIN_DOM |
			PTP_PIN_CFG_PIN_SYNC,
			lan9645x, PTP_PIN_CFG(pin));
		mutex_unlock(&lan9645x->ptp_clock_lock);
		return 0;
	}

	if (rq->perout.period.sec == 1 &&
	    rq->perout.period.nsec == 0)
		pps = true;

	if (rq->perout.flags & PTP_PEROUT_PHASE) {
		ts_phase.tv_sec = rq->perout.phase.sec;
		ts_phase.tv_nsec = rq->perout.phase.nsec;
	} else {
		ts_phase.tv_sec = rq->perout.start.sec;
		ts_phase.tv_nsec = rq->perout.start.nsec;
	}

	if (ts_phase.tv_sec || (ts_phase.tv_nsec && !pps)) {
		dev_warn(lan9645x->dev,
			 "Absolute time not supported!\n");
		return -EINVAL;
	}

	if (rq->perout.flags & PTP_PEROUT_DUTY_CYCLE) {
		struct timespec64 ts_on;

		ts_on.tv_sec = rq->perout.on.sec;
		ts_on.tv_nsec = rq->perout.on.nsec;

		wf_high = timespec64_to_ns(&ts_on);
	} else {
		wf_high = 5000;
	}

	if (pps) {
		mutex_lock(&lan9645x->ptp_clock_lock);
		lan_wr(FIELD_PREP(PTP_WF_LOW_PERIOD_PIN_WFL, ts_phase.tv_nsec),
		       lan9645x, PTP_WF_LOW_PERIOD(pin));
		lan_wr(FIELD_PREP(PTP_WF_HIGH_PERIOD_PIN_WFH, wf_high),
		       lan9645x, PTP_WF_HIGH_PERIOD(pin));
		lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_CLOCK) |
			PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
			PTP_PIN_CFG_PIN_SYNC_SET(3),
			PTP_PIN_CFG_PIN_ACTION |
			PTP_PIN_CFG_PIN_DOM |
			PTP_PIN_CFG_PIN_SYNC,
			lan9645x, PTP_PIN_CFG(pin));
		mutex_unlock(&lan9645x->ptp_clock_lock);
		return 0;
	}

	ts_period.tv_sec = rq->perout.period.sec;
	ts_period.tv_nsec = rq->perout.period.nsec;

	wf_low = timespec64_to_ns(&ts_period);
	wf_low -= wf_high;

	mutex_lock(&lan9645x->ptp_clock_lock);
	lan_wr(FIELD_PREP(PTP_WF_LOW_PERIOD_PIN_WFL, wf_low),
	       lan9645x, PTP_WF_LOW_PERIOD(pin));
	lan_wr(FIELD_PREP(PTP_WF_HIGH_PERIOD_PIN_WFH, wf_high),
	       lan9645x, PTP_WF_HIGH_PERIOD(pin));
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_CLOCK) |
		PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
		PTP_PIN_CFG_PIN_SYNC_SET(0),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SYNC,
		lan9645x, PTP_PIN_CFG(pin));
	mutex_unlock(&lan9645x->ptp_clock_lock);

	return 0;
}

static int lan9645x_ptp_extts(struct ptp_clock_info *ptp,
			      struct ptp_clock_request *rq, int on)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;
	int pin;
	u32 val;

	if (lan9645x->ptp_ext_irq <= 0)
		return -EOPNOTSUPP;

	/* Reject requests with unsupported flags */
	if (rq->extts.flags & ~(PTP_ENABLE_FEATURE |
				PTP_RISING_EDGE |
				PTP_STRICT_FLAGS))
		return -EOPNOTSUPP;

	pin = ptp_find_pin(phc->clock, PTP_PF_EXTTS, rq->extts.index);
	if (pin == -1 || pin >= LAN9645X_PHC_PINS_NUM)
		return -EINVAL;

	mutex_lock(&lan9645x->ptp_clock_lock);
	lan_rmw(PTP_PIN_CFG_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		PTP_PIN_CFG_PIN_SYNC_SET(on ? 3 : 0) |
		PTP_PIN_CFG_PIN_DOM_SET(phc->index) |
		PTP_PIN_CFG_PIN_SELECT_SET(pin),
		PTP_PIN_CFG_PIN_ACTION |
		PTP_PIN_CFG_PIN_SYNC |
		PTP_PIN_CFG_PIN_DOM |
		PTP_PIN_CFG_PIN_SELECT,
		lan9645x, PTP_PIN_CFG(pin));

	val = lan_rd(lan9645x, PTP_PIN_INTR_ENA);
	if (on)
		val |= BIT(pin);
	else
		val &= ~BIT(pin);
	lan_wr(val, lan9645x, PTP_PIN_INTR_ENA);

	mutex_unlock(&lan9645x->ptp_clock_lock);

	return 0;
}

static int lan9645x_ptp_enable(struct ptp_clock_info *ptp,
			       struct ptp_clock_request *rq, int on)
{
	switch (rq->type) {
	case PTP_CLK_REQ_PEROUT:
		return lan9645x_ptp_perout(ptp, rq, on);
	case PTP_CLK_REQ_EXTTS:
		return lan9645x_ptp_extts(ptp, rq, on);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static void lan9645x_rxtstamp_port_work(struct lan9645x *lan9645x, int port)
{
	struct skb_shared_hwtstamps *shhwtstamps;
	struct sk_buff_head received;
	struct lan9645x_phc *phc;
	struct lan9645x_port *p;
	struct timespec64 ts;
	unsigned long flags;
	struct sk_buff *skb;
	u32 rx_ts;

	p = lan9645x_to_port(lan9645x, port);
	phc = &lan9645x->phc[LAN9645X_PHC_PORT];

	__skb_queue_head_init(&received);
	spin_lock_irqsave(&p->rx_skbs.lock, flags);
	skb_queue_splice_tail_init(&p->rx_skbs, &received);
	spin_unlock_irqrestore(&p->rx_skbs.lock, flags);

	while ((skb = __skb_dequeue(&received)) != NULL) {
		lan9645x_ptp_gettime64(&phc->info, &ts);
		rx_ts = LAN9645X_SKB_CB(skb)->rx_ts_ns;
		if (ts.tv_nsec < rx_ts)
			ts.tv_sec--;
		ts.tv_nsec = rx_ts;
		shhwtstamps = skb_hwtstamps(skb);
		shhwtstamps->hwtstamp = ktime_set(ts.tv_sec, ts.tv_nsec);
		lan9645x_ptp_log_rx(lan9645x, skb, ts,
				    LAN9645X_SKB_CB(skb)->rx_ts_subns);
		netif_rx(skb);
	}
}

static long lan9645x_rxtstamp_work(struct ptp_clock_info *ptp)
{
	struct lan9645x_phc *phc = container_of(ptp, struct lan9645x_phc, info);
	struct lan9645x *lan9645x = phc->lan9645x;
	int i;

	for (i = 0; i < lan9645x->num_phys_ports; i++) {
		if (!dsa_is_user_port(lan9645x->ds, i))
			continue;

		lan9645x_rxtstamp_port_work(lan9645x, i);
	}

	/* Don't restart */
	return -1;
}

static struct ptp_clock_info lan9645x_ptp_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "lan9645x ptp",
	.max_adj	= 200000,
	.gettime64	= lan9645x_ptp_gettime64,
	.settime64	= lan9645x_ptp_settime64,
	.adjtime	= lan9645x_ptp_adjtime,
	.adjfine	= lan9645x_ptp_adjfine,
	.verify		= lan9645x_ptp_verify,
	.enable		= lan9645x_ptp_enable,
	.do_aux_work	= lan9645x_rxtstamp_work,
	.n_per_out	= LAN9645X_PHC_PINS_NUM,
	.n_ext_ts	= LAN9645X_PHC_PINS_NUM,
	.n_pins		= LAN9645X_PHC_PINS_NUM,
	.supported_extts_flags = PTP_RISING_EDGE |
				 PTP_STRICT_FLAGS,
	.supported_perout_flags = PTP_PEROUT_DUTY_CYCLE |
				  PTP_PEROUT_PHASE,
};

static int lan9645x_ptp_phc_init(struct lan9645x *lan9645x,
				 int index,
				 struct ptp_clock_info *clock_info)
{
	struct lan9645x_phc *phc = &lan9645x->phc[index];
	struct ptp_pin_desc *p;
	int i;

	for (i = 0; i < LAN9645X_PHC_PINS_NUM; i++) {
		p = &phc->pins[i];

		snprintf(p->name, sizeof(p->name), "pin%d", i);
		p->index = i;
		p->func = PTP_PF_NONE;
	}

	phc->info = *clock_info;
	phc->info.pin_config = &phc->pins[0];
	phc->clock = ptp_clock_register(&phc->info, lan9645x->dev);
	if (IS_ERR(phc->clock))
		return PTR_ERR(phc->clock);

	phc->index = index;
	phc->lan9645x = lan9645x;

	return 0;
}

int lan9645x_ptp_init(struct lan9645x *lan9645x)
{
	u64 tod_adj = lan9645x_ptp_get_nominal_value();
	struct lan9645x_port *port;
	int err, i;

	if (!lan9645x->ptp)
		return 0;

	mutex_init(&lan9645x->ptp_clock_lock);
	lan9645x_ptp_log_init(lan9645x);

	for (i = 0; i < LAN9645X_PHC_COUNT; ++i) {
		err = lan9645x_ptp_phc_init(lan9645x, i, &lan9645x_ptp_clock_info);
		if (err)
			return err;
	}

	spin_lock_init(&lan9645x->ptp_ts_id_lock);
	mutex_init(&lan9645x->ptp_lock);

	/* Disable master counters */
	lan_wr(PTP_DOM_CFG_ENA_SET(0), lan9645x, PTP_DOM_CFG);

	/* Configure the nominal TOD increment per clock cycle */
	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(0x7),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan9645x, PTP_DOM_CFG);

	for (i = 0; i < LAN9645X_PHC_COUNT; ++i) {
		lan_wr((u32)tod_adj & 0xFFFFFFFF, lan9645x,
		       PTP_CLK_PER_CFG(i, 0));
		lan_wr((u32)(tod_adj >> 32), lan9645x,
		       PTP_CLK_PER_CFG(i, 1));
	}

	lan_rmw(PTP_DOM_CFG_CLKCFG_DIS_SET(0),
		PTP_DOM_CFG_CLKCFG_DIS,
		lan9645x, PTP_DOM_CFG);

	/* Enable master counters */
	lan_wr(PTP_DOM_CFG_ENA_SET(0x7), lan9645x, PTP_DOM_CFG);

	lan9645x_for_each_port(lan9645x, i, port) {
		skb_queue_head_init(&port->tx_skbs);
		skb_queue_head_init(&port->rx_skbs);
	}

	return 0;
}

void lan9645x_ptp_deinit(struct lan9645x *lan9645x)
{
	struct lan9645x_port *port;
	int i;

	if (!lan9645x->ptp)
		return;

	lan9645x_for_each_port(lan9645x, i, port) {
		skb_queue_purge(&port->tx_skbs);
		skb_queue_purge(&port->rx_skbs);
	}

	for (i = 0; i < LAN9645X_PHC_COUNT; ++i)
		ptp_clock_unregister(lan9645x->phc[i].clock);

	lan9645x_ptp_log_deinit(lan9645x);
}

/* Called by dsa_skb_defer_rx_timestamp. Return true if we defer skb rx until
 * a timestamp is ready.
 * This is necessary since this function is called from atomic context, so we
 * are not allowed to call lan9645x_ptp_gettime64, and must schedule the work
 * instead.
 */
bool lan9645x_rxtstamp_defer(struct dsa_switch *ds, int port,
			     struct sk_buff *skb, unsigned int type)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p;
	struct lan9645x_phc *phc;

	p = lan9645x_to_port(lan9645x, port);

	if (!lan9645x->ptp || !p->ptp_rx_cmd)
		return false;

	phc = &lan9645x->phc[LAN9645X_PHC_PORT];

	skb_queue_tail(&p->rx_skbs, skb);
	ptp_schedule_worker(phc->clock, 0);
	return true;
}

/* Called by dsa_skb_defer_rx_timestamp. Return true if we defer skb rx until
 * a timestamp is ready.
 * This is necessary since this function is called from atomic context, so we
 * are not allowed to call lan9645x_ptp_gettime64, and must schedule the work
 * instead.
 */
bool lan9645x_rxtstamp_all_defer(struct dsa_switch *ds, int port,
				 struct sk_buff *skb, unsigned int type)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_phc *phc;
	struct lan9645x_port *p;

	p = lan9645x_to_port(lan9645x, port);

	if (!lan9645x->ptp || !p->ptp_rx_cmd)
		return false;

	if (ntohs(skb->protocol) != ETH_P_1588)
		return false;

	phc = &lan9645x->phc[LAN9645X_PHC_PORT];

	skb_queue_tail(&p->rx_skbs, skb);
	ptp_schedule_worker(phc->clock, 0);
	return true;
}

void lan9645x_txtstamp(struct dsa_switch *ds, int port, struct sk_buff *skb)
{
	struct lan9645x *lan9645x = ds->priv;
	struct sk_buff *clone = NULL;

	dev_dbg(lan9645x->dev, "port=%d", port);

	if (!(lan9645x->ptp && skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))
		return;

	if (lan9645x_ptp_txtstamp_request(lan9645x, port, skb, &clone)) {
		dev_err_ratelimited(ds->dev,
				    "port %d delivering skb without TX timestamp\n",
				    port);
		return;
	}

	if (clone)
		LAN9645X_SKB_CB(skb)->clone = clone;
}

int lan9645x_get_ts_info(struct dsa_switch *ds, int port,
			 struct kernel_ethtool_ts_info *info)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_phc *phc;
	struct lan9645x_port *p;
	struct net_device *dev;

	p = lan9645x_to_port(lan9645x, port);
	dev = lan9645x_port_to_ndev(p);

	if (!lan9645x->ptp)
		return ethtool_op_get_ts_info(dev, info);

	phc = &lan9645x->phc[LAN9645X_PHC_PORT];

	if (phc->clock) {
		info->phc_index = ptp_clock_index(phc->clock);
	} else {
		info->so_timestamping |= SOF_TIMESTAMPING_TX_SOFTWARE;
		return 0;
	}
	info->so_timestamping |= SOF_TIMESTAMPING_TX_SOFTWARE |
				 SOF_TIMESTAMPING_TX_HARDWARE |
				 SOF_TIMESTAMPING_RX_HARDWARE |
				 SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON) |
			 BIT(HWTSTAMP_TX_ONESTEP_SYNC);
	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			   BIT(HWTSTAMP_FILTER_ALL);

	return 0;
}

u32 lan9645x_ptp_get_period_ps(void)
{
	 /* System clock period in picoseconds. */
	return 6038;
}

void lan9645x_ptp_improvements(struct lan9645x *lan9645x,
			       struct lan9645x_port *p,
			       phy_interface_t interface, int speed, int duplex)
{
	int div_cfg, rx_stamp_sel, tx_stamp_sel;

	div_cfg = 0;
	rx_stamp_sel = 0;
	tx_stamp_sel = 3;

	/* The following table was received from validation people describing
	 * which values need to be set to get working the timestamping at lower
	 * speeds 10/100. While at this also improve the timestamping at higher
	 * speeds.
	 *
	 * Mode            div_cfg rx_stamp_sel tx_stamp_sel
	 * 1000-BaseT         4        0            3
	 * 10/100-BaseT       2        1            2
	 * 1000-BaseX         3        0            3
	 * 10/100-BaseX FDX   3        0            1
	 * 10/100-BaseX HDX   3        0            3
	 * 2500-BaseX         7        0            3
	*/

	switch (speed) {
	case LAN9645X_SPEED_DISABLED:
		break;
	case LAN9645X_SPEED_10:
	case LAN9645X_SPEED_100:
		if (phy_interface_mode_is_rgmii(interface) ||
		    interface == PHY_INTERFACE_MODE_GMII) {
			div_cfg = 2;
			rx_stamp_sel = 1;
			tx_stamp_sel = 3;
		} else {
			if (duplex == DUPLEX_FULL) {
				div_cfg = 3;
				rx_stamp_sel = 0;
				tx_stamp_sel = 1;
			} else {
				div_cfg = 3;
				rx_stamp_sel = 0;
				tx_stamp_sel = 3;
			}
		}
		break;
	case LAN9645X_SPEED_1000:
		if (phy_interface_mode_is_rgmii(interface) ||
		    interface == PHY_INTERFACE_MODE_GMII) {
			div_cfg = 4;
			rx_stamp_sel = 0;
			tx_stamp_sel = 3;
		} else {
			div_cfg = 3;
			rx_stamp_sel = 0;
			tx_stamp_sel = 3;
		}
		break;
	case LAN9645X_SPEED_2500:
		div_cfg = 7;
		rx_stamp_sel = 0;
		tx_stamp_sel = 3;
		break;
	}

	lan_rmw(DEV_PTP_MISC_CFG_RX_STAMP_SEL_SET(rx_stamp_sel),
		DEV_PTP_MISC_CFG_RX_STAMP_SEL,
		lan9645x, DEV_PTP_MISC_CFG(p->chip_port));

	lan_rmw(DEV_PTP_MISC_CFG_TX_STAMP_SEL_SET(tx_stamp_sel),
		DEV_PTP_MISC_CFG_TX_STAMP_SEL,
		lan9645x, DEV_PTP_MISC_CFG(p->chip_port));

	/* First it is needed to disable and then enable it and after that it
	 * needed to clear the failed bit which is set by default. Also there
	 * are 2 phase detector ctrl one for TX and one for RX
	 */
	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA,
		lan9645x, DEV_PHAD_CTRL(p->chip_port, 0));

	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA,
		lan9645x, DEV_PHAD_CTRL(p->chip_port, 1));

	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(1) |
		DEV_PHAD_CTRL_DIV_CFG_SET(div_cfg) |
		DEV_PHAD_CTRL_PHAD_FAILED_SET(1) |
		DEV_PHAD_CTRL_LOCK_ACC_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA |
		DEV_PHAD_CTRL_DIV_CFG |
		DEV_PHAD_CTRL_PHAD_FAILED |
		DEV_PHAD_CTRL_LOCK_ACC,
		lan9645x, DEV_PHAD_CTRL(p->chip_port, 0));

	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(1) |
		DEV_PHAD_CTRL_DIV_CFG_SET(div_cfg) |
		DEV_PHAD_CTRL_PHAD_FAILED_SET(1) |
		DEV_PHAD_CTRL_LOCK_ACC_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA |
		DEV_PHAD_CTRL_DIV_CFG |
		DEV_PHAD_CTRL_PHAD_FAILED |
		DEV_PHAD_CTRL_LOCK_ACC,
		lan9645x, DEV_PHAD_CTRL(p->chip_port, 1));
}
