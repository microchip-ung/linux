// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2021 Microchip Technology Inc. and its subsidiaries.
 *
 * The Sparx5 Chip Register Model can be browsed at this location:
 * https://github.com/microchip-ung/sparx-5_reginfo
 */
#include <linux/ptp_classify.h>

#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include "sparx5_tc.h"
#include "sparx5_vcap_impl.h"
#include "vcap_api_client.h"

#define SPARX5_PTP_RULE_ID_OFFSET 2048

#define LAN969X_WFH_ERROR_NS      73741824

enum {
	PTP_PIN_ACTION_IDLE = 0,
	PTP_PIN_ACTION_LOAD,
	PTP_PIN_ACTION_SAVE,
	PTP_PIN_ACTION_CLOCK,
	PTP_PIN_ACTION_DELTA,
	PTP_PIN_ACTION_TOD
};

static u64 sparx5_ptp_get_1ppm(struct sparx5 *sparx5)
{
	/* Represents 1ppm adjustment in 2^59 format with 1.59687500000(625)
	 * 1.99609375000(500), 3.04761904762(380), 3.99218750000(250),
	 * 5.56521739130(180) as reference
	 * The value is calculated as following:
	 * (1/1000000)/((2^-59)/X)
	 */

	u64 res = 0;

	switch (sparx5->coreclock) {
	case SPX5_CORE_CLOCK_180MHZ:
		if (sparx5->coreclockref == SPX5_CORE_CLOCK_REF_25MHZ)
			res = 3207484700609;
		else
			res = 3208129404120;
		break;
	case SPX5_CORE_CLOCK_250MHZ:
		res = 2301339409586;
		break;
	case SPX5_CORE_CLOCK_328MHZ:
		if (sparx5->coreclockref == SPX5_CORE_CLOCK_REF_25MHZ)
			res = 1756479716445;
		else
			res = 1756832768924;
		break;
	case SPX5_CORE_CLOCK_500MHZ:
		res = 1150669704793;
		break;
	case SPX5_CORE_CLOCK_625MHZ:
		res =  920535763834;
		break;
	default:
		WARN(1, "Invalid core clock");
		break;
	}

	return res;
}

static u64 sparx5_ptp_get_nominal_value(struct sparx5 *sparx5)
{
	u64 res = 0;

	switch (sparx5->coreclock) {
	case SPX5_CORE_CLOCK_180MHZ:
		if (sparx5->coreclockref == SPX5_CORE_CLOCK_REF_25MHZ)
			res = 0x2C8346575A51DBE7;
		else
			res = 0x2C834656FFBDCFFA;
		break;
	case SPX5_CORE_CLOCK_250MHZ:
		res = 0x1FF0000000000000;
		break;
	case SPX5_CORE_CLOCK_328MHZ:
		if (sparx5->coreclockref == SPX5_CORE_CLOCK_REF_25MHZ)
			res = 0x186044FEF1EA9C10;
		else
			res = 0x18604697DD0F9B5B;
		break;
	case SPX5_CORE_CLOCK_500MHZ:
		res = 0x0FF8000000000000;
		break;
	case SPX5_CORE_CLOCK_625MHZ:
		res = 0x0CC6666666666666;
		break;
	default:
		WARN(1, "Invalid core clock");
		break;
	}

	return res;
}

#define SPARX5_PTP_TRAP_RULES_CNT	5
static struct vcap_rule *sparx5_ptp_add_l2_key(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 0;
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;
	int chain_id = SPARX5_VCAP_CID_IS2_L0;
	int prio = (port->portno << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(vctrl, port->ndev, chain_id, VCAP_USER_PTP,
				prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return vrule;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_ETYPE, ETH_P_1588, ~0);
	if (err) {
		vcap_del_rule(vctrl, port->ndev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *sparx5_ptp_add_ipv4_event_key(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 1;
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;
	int chain_id = SPARX5_VCAP_CID_IS2_L1;
	int prio = (port->portno << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(vctrl, port->ndev, chain_id, VCAP_USER_PTP,
				prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return vrule;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 319, ~0);
	if (err) {
		vcap_del_rule(vctrl, port->ndev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *sparx5_ptp_add_ipv4_general_key(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 2;
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;
	int chain_id = SPARX5_VCAP_CID_IS2_L1;
	int prio = (port->portno << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(vctrl, port->ndev, chain_id, VCAP_USER_PTP,
				prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return vrule;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 320, ~0);
	if (err) {
		vcap_del_rule(vctrl, port->ndev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *sparx5_ptp_add_ipv6_event_key(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 3;
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;
	int chain_id = SPARX5_VCAP_CID_IS2_L2;
	int prio = (port->portno << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(vctrl, port->ndev, chain_id, VCAP_USER_PTP,
				prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return vrule;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 319, ~0);
	if (err) {
		vcap_del_rule(vctrl, port->ndev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *sparx5_ptp_add_ipv6_general_key(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 4;
	struct vcap_control *vctrl = port->sparx5->vcap_ctrl;
	int chain_id = SPARX5_VCAP_CID_IS2_L2;
	int prio = (port->portno << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(vctrl, port->ndev, chain_id, VCAP_USER_PTP,
				prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return vrule;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 320, ~0);
	if (err) {
		vcap_del_rule(vctrl, port->ndev, rule_id);
		return NULL;
	}

	return vrule;
}

static int sparx5_ptp_add_trap(struct sparx5_port *port,
			       struct vcap_rule* (*sparx5_add_ptp_key)(struct sparx5_port*),
			       u16 proto)
{
	struct vcap_rule *vrule;
	int err;

	vrule = sparx5_add_ptp_key(port);
	if (!vrule || IS_ERR(vrule)) {
		if (PTR_ERR(vrule) == -EEXIST)
			return 0;

		return -ENOMEM;
	}

	err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_COPY_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, SPX5_PMM_REPLACE_ALL);
	err |= vcap_val_rule(vrule, proto);
	if (err)
		goto free_rule;

	err = vcap_add_rule(vrule);

free_rule:
	/* Free the local copy of the rule */
	vcap_free_rule(vrule);
	return err;
}

static int sparx5_ptp_del(struct sparx5_port *port, int rule_id)
{
	return vcap_del_rule(port->sparx5->vcap_ctrl, port->ndev, rule_id);
}

static int sparx5_ptp_del_l2(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 0;

	return sparx5_ptp_del(port, rule_id);
}

static int sparx5_ptp_del_ipv4_event(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 1;

	return sparx5_ptp_del(port, rule_id);
}

static int sparx5_ptp_del_ipv4_general(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 2;

	return sparx5_ptp_del(port, rule_id);
}

static int sparx5_ptp_del_ipv6_event(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 3;

	return sparx5_ptp_del(port, rule_id);
}

static int sparx5_ptp_del_ipv6_general(struct sparx5_port *port)
{
	int rule_id = SPARX5_PTP_RULE_ID_OFFSET +
		      port->portno * SPARX5_PTP_TRAP_RULES_CNT + 4;

	return sparx5_ptp_del(port, rule_id);
}

static int sparx5_ptp_add_l2_rule(struct sparx5_port *port)
{
	return sparx5_ptp_add_trap(port, sparx5_ptp_add_l2_key, ETH_P_ALL);
}

static int sparx5_ptp_del_l2_rule(struct sparx5_port *port)
{
	return sparx5_ptp_del_l2(port);
}

static int sparx5_ptp_add_ipv4_rules(struct sparx5_port *port)
{
	int err;

	err = sparx5_ptp_add_trap(port, sparx5_ptp_add_ipv4_event_key,
				  ETH_P_IP);
	if (err)
		return err;

	err = sparx5_ptp_add_trap(port, sparx5_ptp_add_ipv4_general_key,
				  ETH_P_IP);
	if (err)
		sparx5_ptp_del_ipv4_event(port);

	return err;
}

static int sparx5_ptp_del_ipv4_rules(struct sparx5_port *port)
{
	int err;

	err = sparx5_ptp_del_ipv4_event(port);
	err |= sparx5_ptp_del_ipv4_general(port);

	return err;
}

static int sparx5_ptp_add_ipv6_rules(struct sparx5_port *port)
{
	int err;

	err = sparx5_ptp_add_trap(port, sparx5_ptp_add_ipv6_event_key,
				  ETH_P_IPV6);
	if (err)
		return err;

	err = sparx5_ptp_add_trap(port, sparx5_ptp_add_ipv6_general_key,
				  ETH_P_IPV6);
	if (err)
		sparx5_ptp_del_ipv6_event(port);

	return err;
}

static int sparx5_ptp_del_ipv6_rules(struct sparx5_port *port)
{
	int err;

	err = sparx5_ptp_del_ipv6_event(port);
	err |= sparx5_ptp_del_ipv6_general(port);

	return err;
}

static int sparx5_ptp_add_traps(struct sparx5_port *port)
{
	int err;

	err = sparx5_ptp_add_l2_rule(port);
	if (err)
		goto err_l2;

	err = sparx5_ptp_add_ipv4_rules(port);
	if (err)
		goto err_ipv4;

	err = sparx5_ptp_add_ipv6_rules(port);
	if (err)
		goto err_ipv6;

	return err;

err_ipv6:
	sparx5_ptp_del_ipv4_rules(port);
err_ipv4:
	sparx5_ptp_del_l2_rule(port);
err_l2:
	return err;
}

int sparx5_ptp_del_traps(struct sparx5_port *port)
{
	int err;

	err = sparx5_ptp_del_l2_rule(port);
	err |= sparx5_ptp_del_ipv4_rules(port);
	err |= sparx5_ptp_del_ipv6_rules(port);

	return err;
}

int sparx5_ptp_setup_traps(struct sparx5_port *port, struct kernel_hwtstamp_config *cfg)
{
	if (cfg->rx_filter == HWTSTAMP_FILTER_NONE)
		return sparx5_ptp_del_traps(port);
	else
		return sparx5_ptp_add_traps(port);
}

int sparx5_ptp_hwtstamp_set(struct sparx5_port *port,
			    struct kernel_hwtstamp_config *cfg,
			    struct netlink_ext_ack *extack)
{
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_phc *phc;

	switch (cfg->tx_type) {
	case HWTSTAMP_TX_ON:
		port->ptp_tx_cmd = IFH_REW_OP_TWO_STEP_PTP;
		break;
	case HWTSTAMP_TX_ONESTEP_SYNC:
		port->ptp_tx_cmd = IFH_REW_OP_ONE_STEP_PTP;
		break;
	case HWTSTAMP_TX_OFF:
		port->ptp_tx_cmd = IFH_REW_OP_NOOP;
		break;
	default:
		return -ERANGE;
	}

	switch (cfg->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		port->ptp_rx_cmd = false;
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
		port->ptp_rx_cmd = true;
		cfg->rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	default:
		return -ERANGE;
	}

	/* Commit back the result & save it */
	mutex_lock(&sparx5->ptp_lock);
	phc = &sparx5->phc[SPARX5_PHC_PORT];
	phc->hwtstamp_config = *cfg;
	mutex_unlock(&sparx5->ptp_lock);

	return 0;
}

void sparx5_ptp_hwtstamp_get(struct sparx5_port *port,
			     struct kernel_hwtstamp_config *cfg)
{
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_phc *phc;

	phc = &sparx5->phc[SPARX5_PHC_PORT];
	*cfg = phc->hwtstamp_config;
}

static void sparx5_ptp_classify(struct sparx5_port *port, struct sk_buff *skb,
				u8 *rew_op, u8 *pdu_type, u8 *pdu_w16_offset)
{
	struct ptp_header *header;
	u8 msgtype;
	int type;

	if (port->ptp_tx_cmd == IFH_REW_OP_NOOP) {
		*rew_op = IFH_REW_OP_NOOP;
		*pdu_type = IFH_PDU_TYPE_NONE;
		*pdu_w16_offset = 0;
		return;
	}

	type = ptp_classify_raw(skb);
	if (type == PTP_CLASS_NONE) {
		*rew_op = IFH_REW_OP_NOOP;
		*pdu_type = IFH_PDU_TYPE_NONE;
		*pdu_w16_offset = 0;
		return;
	}

	header = ptp_parse_header(skb, type);
	if (!header) {
		*rew_op = IFH_REW_OP_NOOP;
		*pdu_type = IFH_PDU_TYPE_NONE;
		*pdu_w16_offset = 0;
		return;
	}

	*pdu_w16_offset = 7;
	if (type & PTP_CLASS_L2)
		*pdu_type = IFH_PDU_TYPE_PTP;
	if (type & PTP_CLASS_IPV4)
		*pdu_type = IFH_PDU_TYPE_IPV4_UDP_PTP;
	if (type & PTP_CLASS_IPV6)
		*pdu_type = IFH_PDU_TYPE_IPV6_UDP_PTP;

	if (port->ptp_tx_cmd == IFH_REW_OP_TWO_STEP_PTP) {
		*rew_op = IFH_REW_OP_TWO_STEP_PTP;
		return;
	}

	/* If it is sync and run 1 step then set the correct operation,
	 * otherwise run as 2 step
	 */
	msgtype = ptp_get_msgtype(header, type);
	if ((msgtype & 0xf) == 0) {
		*rew_op = IFH_REW_OP_ONE_STEP_PTP;
		return;
	}

	*rew_op = IFH_REW_OP_TWO_STEP_PTP;
}

static void sparx5_ptp_txtstamp_old_release(struct sparx5_port *port)
{
	struct sk_buff *skb, *skb_tmp;
	unsigned long flags;

	spin_lock_irqsave(&port->tx_skbs.lock, flags);
	skb_queue_walk_safe(&port->tx_skbs, skb, skb_tmp) {
		if time_after(SPARX5_SKB_CB(skb)->jiffies + SPARX5_PTP_TIMEOUT,
			      jiffies)
			break;

		__skb_unlink(skb, &port->tx_skbs);
		dev_kfree_skb_any(skb);
	}
	spin_unlock_irqrestore(&port->tx_skbs.lock, flags);
}

int sparx5_ptp_txtstamp_request(struct sparx5_port *port,
				struct sk_buff *skb)
{
	struct sparx5 *sparx5 = port->sparx5;
	u8 rew_op, pdu_type, pdu_w16_offset;
	unsigned long flags;

	sparx5_ptp_classify(port, skb, &rew_op, &pdu_type, &pdu_w16_offset);
	SPARX5_SKB_CB(skb)->rew_op = rew_op;
	SPARX5_SKB_CB(skb)->pdu_type = pdu_type;
	SPARX5_SKB_CB(skb)->pdu_w16_offset = pdu_w16_offset;

	if (rew_op != IFH_REW_OP_TWO_STEP_PTP)
		return 0;

	sparx5_ptp_txtstamp_old_release(port);

	spin_lock_irqsave(&sparx5->ptp_ts_id_lock, flags);
	if (sparx5->ptp_skbs == SPARX5_MAX_PTP_ID) {
		spin_unlock_irqrestore(&sparx5->ptp_ts_id_lock, flags);
		return -EBUSY;
	}

	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;

	skb_queue_tail(&port->tx_skbs, skb);
	SPARX5_SKB_CB(skb)->ts_id = port->ts_id;
	SPARX5_SKB_CB(skb)->jiffies = jiffies;

	sparx5->ptp_skbs++;
	port->ts_id++;
	if (port->ts_id == SPARX5_MAX_PTP_ID)
		port->ts_id = 0;

	spin_unlock_irqrestore(&sparx5->ptp_ts_id_lock, flags);

	return 0;
}

void sparx5_ptp_txtstamp_release(struct sparx5_port *port,
				 struct sk_buff *skb)
{
	struct sparx5 *sparx5 = port->sparx5;
	unsigned long flags;

	spin_lock_irqsave(&sparx5->ptp_ts_id_lock, flags);
	port->ts_id--;
	sparx5->ptp_skbs--;
	skb_unlink(skb, &port->tx_skbs);
	spin_unlock_irqrestore(&sparx5->ptp_ts_id_lock, flags);
}

void sparx5_ptp_get_hwtimestamp(struct sparx5 *sparx5,
				struct timespec64 *ts,
				u32 nsec)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	/* Read current PTP time to get seconds */
	unsigned long flags;
	u32 curr_nsec;

	spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);

	spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(SPARX5_PHC_PORT) |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
		 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
		 sparx5, PTP_PTP_PIN_CFG(consts->tod_pin));

	ts->tv_sec = spx5_rd(sparx5, PTP_PTP_TOD_SEC_LSB(consts->tod_pin));
	curr_nsec = spx5_rd(sparx5, PTP_PTP_TOD_NSEC(consts->tod_pin));

	ts->tv_nsec = nsec;

	/* Sec has incremented since the ts was registered */
	if (curr_nsec < nsec)
		ts->tv_sec--;

	spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);
}

irqreturn_t sparx5_ptp_irq_handler(int irq, void *args)
{
	int budget = SPARX5_MAX_PTP_ID;
	struct sparx5 *sparx5 = args;

	while (budget--) {
		struct sk_buff *skb, *skb_tmp, *skb_match = NULL;
		struct skb_shared_hwtstamps shhwtstamps;
		struct sparx5_port *port;
		struct timespec64 ts;
		unsigned long flags;
		u32 val, id, txport;
		u32 delay;

		val = spx5_rd(sparx5, REW_PTP_TWOSTEP_CTRL);

		/* Check if a timestamp can be retrieved */
		if (!(val & REW_PTP_TWOSTEP_CTRL_PTP_VLD))
			break;

		WARN_ON(val & REW_PTP_TWOSTEP_CTRL_PTP_OVFL);

		if (!(val & REW_PTP_TWOSTEP_CTRL_STAMP_TX))
			continue;

		/* Retrieve the ts Tx port */
		txport = REW_PTP_TWOSTEP_CTRL_STAMP_PORT_GET(val);

		/* Retrieve its associated skb */
		port = sparx5->ports[txport];

		/* Retrieve the delay */
		delay = spx5_rd(sparx5, REW_PTP_TWOSTEP_STAMP);
		delay = REW_PTP_TWOSTEP_STAMP_STAMP_NSEC_GET(delay);

		/* Get next timestamp from fifo, which needs to be the
		 * rx timestamp which represents the id of the frame
		 */
		spx5_rmw(REW_PTP_TWOSTEP_CTRL_PTP_NXT_SET(1),
			 REW_PTP_TWOSTEP_CTRL_PTP_NXT,
			 sparx5, REW_PTP_TWOSTEP_CTRL);

		val = spx5_rd(sparx5, REW_PTP_TWOSTEP_CTRL);

		/* Check if a timestamp can be retried */
		if (!(val & REW_PTP_TWOSTEP_CTRL_PTP_VLD))
			break;

		/* Read RX timestamping to get the ID */
		id = spx5_rd(sparx5, REW_PTP_TWOSTEP_STAMP);
		id <<= 8;
		id |= spx5_rd(sparx5, REW_PTP_TWOSTEP_STAMP_SUBNS);

		spin_lock_irqsave(&port->tx_skbs.lock, flags);
		skb_queue_walk_safe(&port->tx_skbs, skb, skb_tmp) {
			if (SPARX5_SKB_CB(skb)->ts_id != id)
				continue;

			__skb_unlink(skb, &port->tx_skbs);
			skb_match = skb;
			break;
		}
		spin_unlock_irqrestore(&port->tx_skbs.lock, flags);

		/* Next ts */
		spx5_rmw(REW_PTP_TWOSTEP_CTRL_PTP_NXT_SET(1),
			 REW_PTP_TWOSTEP_CTRL_PTP_NXT,
			 sparx5, REW_PTP_TWOSTEP_CTRL);

		if (WARN_ON(!skb_match))
			continue;

		spin_lock(&sparx5->ptp_ts_id_lock);
		sparx5->ptp_skbs--;
		spin_unlock(&sparx5->ptp_ts_id_lock);

		/* Get the h/w timestamp */
		sparx5_ptp_get_hwtimestamp(sparx5, &ts, delay);

		/* Set the timestamp into the skb */
		shhwtstamps.hwtstamp = ktime_set(ts.tv_sec, ts.tv_nsec);
		skb_tstamp_tx(skb_match, &shhwtstamps);

		dev_kfree_skb_any(skb_match);
	}

	return IRQ_HANDLED;
}

static int sparx5_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct sparx5_phc *phc = container_of(ptp, struct sparx5_phc, info);
	struct sparx5 *sparx5 = phc->sparx5;
	unsigned long flags;
	bool neg_adj = 0;
	u64 tod_inc;
	u64 ref;

	if (!scaled_ppm)
		return 0;

	if (scaled_ppm < 0) {
		neg_adj = 1;
		scaled_ppm = -scaled_ppm;
	}

	tod_inc = sparx5_ptp_get_nominal_value(sparx5);

	/* The multiplication is split in 2 separate additions because of
	 * overflow issues. If scaled_ppm with 16bit fractional part was bigger
	 * than 20ppm then we got overflow.
	 */
	ref = sparx5_ptp_get_1ppm(sparx5) * (scaled_ppm >> 16);
	ref += (sparx5_ptp_get_1ppm(sparx5) * (0xffff & scaled_ppm)) >> 16;
	tod_inc = neg_adj ? tod_inc - ref : tod_inc + ref;

	spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);

	spx5_rmw(PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS_SET(1 << BIT(phc->index)),
		 PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS,
		 sparx5, PTP_PTP_DOM_CFG);

	spx5_wr((u32)tod_inc & 0xFFFFFFFF, sparx5,
	       PTP_CLK_PER_CFG(phc->index, 0));
	spx5_wr((u32)(tod_inc >> 32), sparx5,
	       PTP_CLK_PER_CFG(phc->index, 1));

	spx5_rmw(PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS_SET(0),
		 PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS, sparx5,
		 PTP_PTP_DOM_CFG);

	spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);

	return 0;
}

static int sparx5_ptp_settime64(struct ptp_clock_info *ptp,
				const struct timespec64 *ts)
{
	struct sparx5_phc *phc = container_of(ptp, struct sparx5_phc, info);
	struct sparx5 *sparx5 = phc->sparx5;
	const struct sparx5_consts *consts;
	unsigned long flags;

	consts = &sparx5->data->consts;

	spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);

	/* Must be in IDLE mode before the time can be loaded */
	spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
		 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
		 sparx5, PTP_PTP_PIN_CFG(consts->tod_pin));

	/* Set new value */
	spx5_wr(PTP_PTP_TOD_SEC_MSB_PTP_TOD_SEC_MSB_SET(upper_32_bits(ts->tv_sec)),
	       sparx5, PTP_PTP_TOD_SEC_MSB(consts->tod_pin));
	spx5_wr(lower_32_bits(ts->tv_sec),
	       sparx5, PTP_PTP_TOD_SEC_LSB(consts->tod_pin));
	spx5_wr(ts->tv_nsec, sparx5, PTP_PTP_TOD_NSEC(consts->tod_pin));

	/* Apply new values */
	spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_LOAD) |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
		 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
		 sparx5, PTP_PTP_PIN_CFG(consts->tod_pin));

	spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);

	return 0;
}

int sparx5_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct sparx5_phc *phc = container_of(ptp, struct sparx5_phc, info);
	struct sparx5 *sparx5 = phc->sparx5;
	const struct sparx5_consts *consts;
	unsigned long flags;
	time64_t s;
	s64 ns;

	consts = &sparx5->data->consts;

	spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);

	spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
		 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
		 sparx5, PTP_PTP_PIN_CFG(consts->tod_pin));

	s = spx5_rd(sparx5, PTP_PTP_TOD_SEC_MSB(consts->tod_pin));
	s <<= 32;
	s |= spx5_rd(sparx5, PTP_PTP_TOD_SEC_LSB(consts->tod_pin));
	ns = spx5_rd(sparx5, PTP_PTP_TOD_NSEC(consts->tod_pin));
	ns &= PTP_PTP_TOD_NSEC_PTP_TOD_NSEC;

	spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);

	/* Deal with negative values */
	if ((ns & 0xFFFFFFF0) == 0x3FFFFFF0) {
		s--;
		ns &= 0xf;
		ns += 999999984;
	}

	set_normalized_timespec64(ts, s, ns);
	return 0;
}

static int sparx5_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct sparx5_phc *phc = container_of(ptp, struct sparx5_phc, info);
	struct sparx5 *sparx5 = phc->sparx5;
	const struct sparx5_consts *consts;

	consts = &sparx5->data->consts;

	if (delta > -(NSEC_PER_SEC / 2) && delta < (NSEC_PER_SEC / 2)) {
		unsigned long flags;

		spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);

		/* Must be in IDLE mode before the time can be loaded */
		spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
			 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
			 sparx5, PTP_PTP_PIN_CFG(consts->tod_pin));

		spx5_wr(PTP_PTP_TOD_NSEC_PTP_TOD_NSEC_SET(delta),
			sparx5, PTP_PTP_TOD_NSEC(consts->tod_pin));

		/* Adjust time with the value of PTP_TOD_NSEC */
		spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_DELTA) |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
			 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
			 sparx5, PTP_PTP_PIN_CFG(consts->tod_pin));

		spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);
	} else {
		/* Fall back using sparx5_ptp_settime64 which is not exact */
		struct timespec64 ts;
		u64 now;

		sparx5_ptp_gettime64(ptp, &ts);

		now = ktime_to_ns(timespec64_to_ktime(ts));
		ts = ns_to_timespec64(now + delta);

		sparx5_ptp_settime64(ptp, &ts);
	}

	return 0;
}

static int sparx5_ptp_verify(struct ptp_clock_info *ptp, unsigned int pin,
			     enum ptp_pin_function func, unsigned int chan)
{
	struct sparx5_phc *phc = container_of(ptp, struct sparx5_phc, info);
	struct sparx5 *sparx5 = phc->sparx5;
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
	for (i = 0; i < SPARX5_PHC_COUNT; ++i) {
		info = &sparx5->phc[i].info;

		/* Ignore the check with ourself */
		if (ptp == info)
			continue;

		if (info->pin_config[pin].func == PTP_PF_PEROUT ||
		    info->pin_config[pin].func == PTP_PF_EXTTS)
			return -1;
	}

	return 0;
}

static int sparx5_ptp_perout(struct ptp_clock_info *ptp,
			     struct ptp_clock_request *rq, int on)
{
	struct sparx5_phc *phc = container_of(ptp, struct sparx5_phc, info);
	struct sparx5 *sparx5 = phc->sparx5;
	const struct sparx5_consts *consts = &sparx5->data->consts;
	struct timespec64 ts_phase, ts_period;
	unsigned long flags;
	s64 wf_high, wf_low;
	bool pps = false;
	int pin;

	if (rq->perout.flags & ~(PTP_PEROUT_DUTY_CYCLE |
				 PTP_PEROUT_PHASE))
		return -EOPNOTSUPP;

	pin = ptp_find_pin(phc->clock, PTP_PF_PEROUT, rq->perout.index);
	if (pin == -1 || pin >= consts->ptp_pins)
		return -EINVAL;

	if (!on) {
		spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);
		spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_IDLE) |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
			 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
			 sparx5, PTP_PTP_PIN_CFG(pin));
		spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);
		return 0;
	}

	/* On lan969x there is an issue with the HW, if the period is bigger
	 * than 2^30 (1073741824ns) then the high waveform will be shorter by
	 * 73741824ns. To fix this check for this conditions and then compensate
	 * in SW for missing period. This is needed to do both for high waveform
	 * and the total period otherwise if it is done only for high waveform
	 * then the total period will still be shorter and if it is done for the
	 * total period then the high waveform will be shorted.
	 */
	if (!is_sparx5(sparx5)) {
		struct timespec64 period;

		period.tv_sec = rq->perout.period.sec;
		period.tv_nsec = rq->perout.period.nsec;

		if (timespec64_to_ns(&period) >= NSEC_PER_SEC +
						 LAN969X_WFH_ERROR_NS) {

			rq->perout.on.nsec += LAN969X_WFH_ERROR_NS;
			if (rq->perout.on.nsec >= NSEC_PER_SEC) {
				rq->perout.on.nsec -= NSEC_PER_SEC;
				rq->perout.on.sec += 1;
			}

			rq->perout.period.nsec += LAN969X_WFH_ERROR_NS;
			if (rq->perout.period.nsec >= NSEC_PER_SEC) {
				rq->perout.period.nsec -= NSEC_PER_SEC;
				rq->perout.period.sec += 1;
			}
		}
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
		dev_warn(sparx5->dev,
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
		spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);
		spx5_wr(PTP_PIN_WF_LOW_PERIOD_PIN_WFL_SET(ts_phase.tv_nsec),
			sparx5, PTP_PIN_WF_LOW_PERIOD(pin));
		spx5_wr(PTP_PIN_WF_HIGH_PERIOD_PIN_WFH_SET(wf_high),
			sparx5, PTP_PIN_WF_HIGH_PERIOD(pin));
		spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_CLOCK) |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(3),
			 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
			 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
			 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
			 sparx5, PTP_PTP_PIN_CFG(pin));
		spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);
		return 0;
	}

	ts_period.tv_sec = rq->perout.period.sec;
	ts_period.tv_nsec = rq->perout.period.nsec;

	wf_low = timespec64_to_ns(&ts_period);
	wf_low -= wf_high;

	if ((wf_low >> 30) != 0 || (wf_high >> 30) != 0) {
		dev_warn(sparx5->dev,
			 "WFL or WFH can't be bigger than 2^30\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);
	spx5_wr(PTP_PIN_WF_LOW_PERIOD_PIN_WFL_SET(wf_low),
		sparx5, PTP_PIN_WF_LOW_PERIOD(pin));
	spx5_wr(PTP_PIN_WF_HIGH_PERIOD_PIN_WFH_SET(wf_high),
		sparx5, PTP_PIN_WF_HIGH_PERIOD(pin));
	spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_CLOCK) |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(0),
		 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC,
		 sparx5, PTP_PTP_PIN_CFG(pin));
	spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);

	return 0;
}

irqreturn_t sparx5_ptp_ext_irq_handler(int irq, void *args)
{
	struct sparx5 *sparx5 = args;
	struct sparx5_phc *phc;
	unsigned long flags;
	u64 time = 0;
	time64_t s;
	int pin, i;
	s64 ns;

	if (!(spx5_rd(sparx5, PTP_PTP_PIN_INTR)))
		return IRQ_NONE;

	/* Go through all domains and see which pin generated the interrupt */
	for (i = 0; i < SPARX5_PHC_COUNT; ++i) {
		struct ptp_clock_event ptp_event = {0};

		phc = &sparx5->phc[i];
		pin = ptp_find_pin_unlocked(phc->clock, PTP_PF_EXTTS, 0);
		if (pin == -1)
			continue;

		if (!(spx5_rd(sparx5, PTP_PTP_PIN_INTR) & BIT(pin)))
			continue;

		spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);

		/* Enable to get the new interrupt.
		 * By writing 1 it clears the bit
		 */
		spx5_wr(BIT(pin), sparx5, PTP_PTP_PIN_INTR);

		/* Get current time */
		s = spx5_rd(sparx5, PTP_PTP_TOD_SEC_MSB(pin));
		s <<= 32;
		s |= spx5_rd(sparx5, PTP_PTP_TOD_SEC_LSB(pin));
		ns = spx5_rd(sparx5, PTP_PTP_TOD_NSEC(pin));
		ns &= PTP_PTP_TOD_NSEC_PTP_TOD_NSEC;

		spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);

		if ((ns & 0xFFFFFFF0) == 0x3FFFFFF0) {
			s--;
			ns &= 0xf;
			ns += 999999984;
		}
		time = ktime_set(s, ns);

		ptp_event.index = 0;
		ptp_event.timestamp = time;
		ptp_event.type = PTP_CLOCK_EXTTS;
		ptp_clock_event(phc->clock, &ptp_event);
	}

	return IRQ_HANDLED;
}

static int sparx5_ptp_extts(struct ptp_clock_info *ptp,
			    struct ptp_clock_request *rq, int on)
{
	struct sparx5_phc *phc = container_of(ptp, struct sparx5_phc, info);
	struct sparx5 *sparx5 = phc->sparx5;
	const struct sparx5_consts *consts = &sparx5->data->consts;
	unsigned long flags;
	int pin;
	u32 val;

	if (sparx5->ptp_ext_irq <= 0)
		return -EOPNOTSUPP;

	/* Reject requests with unsupported flags */
	if (rq->extts.flags & ~(PTP_ENABLE_FEATURE |
				PTP_RISING_EDGE |
				PTP_STRICT_FLAGS))
		return -EOPNOTSUPP;

	pin = ptp_find_pin(phc->clock, PTP_PF_EXTTS, rq->extts.index);
	if (pin == -1 || pin >= consts->ptp_pins)
		return -EINVAL;

	spin_lock_irqsave(&sparx5->ptp_clock_lock, flags);
	spx5_rmw(PTP_PTP_PIN_CFG_PTP_PIN_ACTION_SET(PTP_PIN_ACTION_SAVE) |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC_SET(on ? 3 : 0) |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM_SET(phc->index) |
		 PTP_PTP_PIN_CFG_PTP_PIN_SELECT_SET(pin),
		 PTP_PTP_PIN_CFG_PTP_PIN_ACTION |
		 PTP_PTP_PIN_CFG_PTP_PIN_SYNC |
		 PTP_PTP_PIN_CFG_PTP_PIN_DOM |
		 PTP_PTP_PIN_CFG_PTP_PIN_SELECT,
		 sparx5, PTP_PTP_PIN_CFG(pin));

	val = spx5_rd(sparx5, PTP_PTP_PIN_INTR_ENA);
	if (on)
		val |= BIT(pin);
	else
		val &= ~BIT(pin);
	spx5_wr(val, sparx5, PTP_PTP_PIN_INTR_ENA);

	spin_unlock_irqrestore(&sparx5->ptp_clock_lock, flags);

	return 0;
}

static int sparx5_ptp_enable(struct ptp_clock_info *ptp,
			     struct ptp_clock_request *rq, int on)
{
	switch (rq->type) {
	case PTP_CLK_REQ_PEROUT:
		return sparx5_ptp_perout(ptp, rq, on);
	case PTP_CLK_REQ_EXTTS:
		return sparx5_ptp_extts(ptp, rq, on);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static struct ptp_clock_info sparx5_ptp_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "sparx5 ptp",
	.max_adj	= 2000000,
	.gettime64	= sparx5_ptp_gettime64,
	.settime64	= sparx5_ptp_settime64,
	.adjtime	= sparx5_ptp_adjtime,
	.adjfine	= sparx5_ptp_adjfine,
	.verify		= sparx5_ptp_verify,
	.enable		= sparx5_ptp_enable,
};

static int sparx5_ptp_phc_init(struct sparx5 *sparx5,
			       int index,
			       struct ptp_clock_info *clock_info)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	struct sparx5_phc *phc = &sparx5->phc[index];
	struct ptp_pin_desc *p;
	int i;

	clock_info->n_per_out = consts->ptp_pins;
	clock_info->n_ext_ts = consts->ptp_pins;
	clock_info->n_pins = consts->ptp_pins;

	for (i = 0; i < consts->ptp_pins; i++) {
		p = &phc->pins[i];

		snprintf(p->name, sizeof(p->name), "pin%d", i);
		p->index = i;
		p->func = PTP_PF_NONE;
	}

	phc->info = *clock_info;
	phc->info.pin_config = &phc->pins[0];
	phc->clock = ptp_clock_register(&phc->info, sparx5->dev);
	if (IS_ERR(phc->clock))
		return PTR_ERR(phc->clock);

	phc->index = index;
	phc->sparx5 = sparx5;

	return 0;
}

int sparx5_ptp_init(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	u64 tod_adj = sparx5_ptp_get_nominal_value(sparx5);
	struct sparx5_port *port;
	int err, i;

	/* We need PTP TOD on lan969x for QoS and TSN features - for now
	 * always initialize on lan969x.
	 */
	if (!sparx5->ptp && is_sparx5(sparx5))
		return 0;

	for (i = 0; i < SPARX5_PHC_COUNT; ++i) {
		err = sparx5_ptp_phc_init(sparx5, i, &sparx5_ptp_clock_info);
		if (err)
			return err;
	}

	spin_lock_init(&sparx5->ptp_clock_lock);
	spin_lock_init(&sparx5->ptp_ts_id_lock);
	mutex_init(&sparx5->ptp_lock);

	/* Disable master counters */
	spx5_wr(PTP_PTP_DOM_CFG_PTP_ENA_SET(0), sparx5, PTP_PTP_DOM_CFG);

	/* Configure the nominal TOD increment per clock cycle */
	spx5_rmw(PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS_SET(0x7),
		 PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS,
		 sparx5, PTP_PTP_DOM_CFG);

	for (i = 0; i < SPARX5_PHC_COUNT; ++i) {
		spx5_wr((u32)tod_adj & 0xFFFFFFFF, sparx5,
		       PTP_CLK_PER_CFG(i, 0));
		spx5_wr((u32)(tod_adj >> 32), sparx5,
		       PTP_CLK_PER_CFG(i, 1));
	}

	spx5_rmw(PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS_SET(0),
		 PTP_PTP_DOM_CFG_PTP_CLKCFG_DIS,
		 sparx5, PTP_PTP_DOM_CFG);

	/* Enable master counters */
	spx5_wr(PTP_PTP_DOM_CFG_PTP_ENA_SET(0x7), sparx5, PTP_PTP_DOM_CFG);

	for (i = 0; i < consts->chip_ports; i++) {
		port = sparx5->ports[i];
		if (!port)
			continue;

		skb_queue_head_init(&port->tx_skbs);
	}

	return 0;
}

void sparx5_ptp_deinit(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	struct sparx5_port *port;
	int i;

	for (i = 0; i < consts->chip_ports; i++) {
		port = sparx5->ports[i];
		if (!port)
			continue;

		skb_queue_purge(&port->tx_skbs);
	}

	for (i = 0; i < SPARX5_PHC_COUNT; ++i)
		ptp_clock_unregister(sparx5->phc[i].clock);
}

void sparx5_ptp_rxtstamp(struct sparx5 *sparx5, struct sk_buff *skb,
			 u64 src_port, u64 timestamp)
{
	struct skb_shared_hwtstamps *shhwtstamps;
	struct sparx5_phc *phc;
	struct timespec64 ts;
	u64 full_ts_in_ns;

	if (!sparx5->ptp ||
	    !sparx5->ports[src_port]->ptp_rx_cmd)
		return;

	phc = &sparx5->phc[SPARX5_PHC_PORT];
	sparx5_ptp_gettime64(&phc->info, &ts);

	if (ts.tv_nsec < timestamp)
		ts.tv_sec--;
	ts.tv_nsec = timestamp;
	full_ts_in_ns = ktime_set(ts.tv_sec, ts.tv_nsec);

	shhwtstamps = skb_hwtstamps(skb);
	shhwtstamps->hwtstamp = full_ts_in_ns;
}
EXPORT_SYMBOL_GPL(sparx5_ptp_rxtstamp);
