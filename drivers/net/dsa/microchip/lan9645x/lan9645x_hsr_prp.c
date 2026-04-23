// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/debugfs.h>

#include "lan9645x_stats.h"
#include "vcap_api_client.h"
#include "lan9645x_vcap_utils.h"
#include "lan9645x_main.h"

/* Lan9645x can not create the PRP trailer, SW must add a template in the frame,
 * then lan9645x will write  seq nums and lanid in this prepared trailer.
 *
 *
 * PRP offload:
 *
 * - Duplicate discard. There is no NETIF flag for this.
 *
 * - NETIF_F_HW_HSR_TAG_RM: Lan9645x does not remove PRP trailer. But we need this
 *   to avoid hsr driver validating the sequence number. We remove the RCT in the
 *   tag driver.
 *
 * - NETIF_F_HW_HSR_DUP: Lan9645x will duplicate PRP frames with RCT. Frames
 *   to SAN nodes, which are moving between A and B, can not be duplicated in
 *   hw.
 *
 * - NETIF_F_HW_HSR_TAG_INS: Do not set this. Lan9645x can partially offload RCT
 *   insertion. SW must insert 6-byte RCT trailer and preformat trailer with PRP
 *   suffix and frame size. Lan9645x will rewrite sequence number and LAN
 *   identifier.
 *
 *
 * HSR offload:
 *
 * - Duplicate discard. There is no NETIF flag for this.
 * - NETIF_F_HW_HSR_FWD.
 * - NETIF_F_HW_HSR_TAG_INS.
 * - NETIF_F_HW_HSR_DUP
 * - NETIF_F_HW_HSR_TAG_RM: Lan9645x does not remove the HTAG. But it does strip
 *   the htag etype in the tag, for alignment reasons. Our tag driver must take
 *   care to pop the remaining htag fragment.
 *
 */
#define LAN9645X_SUPPORTED_PRP_FEATURES					\
	(NETIF_F_HW_HSR_TAG_RM | NETIF_F_HW_HSR_FWD | NETIF_F_HW_HSR_DUP)

#define LAN9645X_SUPPORTED_HSR_FEATURES					\
	(NETIF_F_HW_HSR_TAG_INS | NETIF_F_HW_HSR_TAG_RM | NETIF_F_HW_HSR_FWD | \
	 NETIF_F_HW_HSR_DUP)

#define for_each_hsr_port(_lan_id, _port, _porta, _portb)	\
	for ((_lan_id) = 0, (_port) = (_porta); (_lan_id) < 2;	\
	     _lan_id++, (_port) = (_portb))

#define PORT_CPU_A_B 0x7
#define PORT_CPU 0x4

#define HSR_ASIC_KHZ 165625
#define DD_TBL_NUM_ROWS (8 * 64)
#define DD_AUTOAGE_UNITSIZE 1

struct lan9645x_hsr_prp_node_counters {
	u32 time_last_seen_a;
	u32 time_last_seen_b;
	u32 cnt_rcv_a;
	u32 cnt_rcv_b;
	u32 cnt_err_wrng_lan_a;
	u32 cnt_err_wrng_lan_b;
};

struct lan9645x_hsr_prp_dan_node {
	struct list_head list;
	unsigned char smac[ETH_ALEN];
	u32 vrule_a;
	u32 vrule_b;
	int isdx_a;
	int isdx_b;
};

enum lan9645x_hsr_type lan9645x_hsr_prp_ver_to_type(enum hsr_version ver)
{
	switch (ver) {
	case HSR_V1:
		return LAN9645X_HSR;
	case PRP_V1:
		return LAN9645X_PRP;
	default:
		return LAN9645X_HSR_UNSUPPORTED;
	}
}

int lan9645x_hsr2type(struct net_device *hsr, enum lan9645x_hsr_type *type)
{
	enum hsr_version ver;
	int err;

	err = hsr_get_version(hsr, &ver);
	if (err)
		return err;

	switch (ver) {
	case HSR_V1:
		*type = LAN9645X_HSR;
		break;
	case PRP_V1:
		*type = LAN9645X_PRP;
		break;
	default:
		*type = LAN9645X_HSR_UNSUPPORTED;
		break;
	}

	return 0;
}

u32 lan9645x_hsr_prp_dev_get_mask(struct lan9645x *lan9645x, struct net_device *hsr)
{
	struct lan9645x_port *port;
	u32 mask = 0;
	int p;

	if (!hsr)
		return mask;

	for (p = 0; p < lan9645x->num_phys_ports; p++) {
		port = lan9645x->ports[p];
		if (!port)
			continue;

		if (port->hsr == hsr)
			mask |= BIT(p);
	}

	return mask;
}

static int lan9645x_vcap_s2_hsr_smac_kill(struct lan9645x *lan9645x,
					  u32 port_ab_mask, unsigned char *mac,
					  struct net_device *dev, u32 *id)
{
	struct vcap_rule *rule;
	int err;

	rule = vcap_alloc_rule(lan9645x->vcap_ctrl, dev,
			       VCAP_CID_INGRESS_STAGE2_L1, VCAP_USER_HSR_PRP, 0,
			       0);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	*id = rule->id;

	/* strictly speaking this is polymorphic, not mac_etype */
	err = vcap_set_rule_set_keyset(rule, VCAP_KFS_MAC_ETYPE);
	/* We want a polymorphic VCAP rule type, to implement the desired
	 * functionality with minimal VCAP entries.
	 *
	 * This X2 rule matches the type_ids 0b00xx:
	 *
	 * 0000: mac_etype
	 * 0001: mac_llc
	 * 0010: mac_snap
	 * 0011: arp
	 *
	 * The field layout for these are the same w.r.p. the fields we use,
	 * except ARP where L2_SMAC has a different position.
	 *
	 * To make this work we set ARP_DIS=1 in VCAP_S2_CFG, so ensure ARP
	 * frames are matched with MAC_ETYPE keys.
	 *
	 * Therefore, this rule is meant to match MAC_ETYPE, MAC_LLC and MAC_SNAP
	 * types.
	 */
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_TYPE, 0, 0xc);
	/* Match any ports in mask */
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, 0,
				     ~port_ab_mask);
	err |= lan9645x_vcap_add_key_mac(rule, VCAP_KF_L2_SMAC, mac);
	err |= vcap_rule_add_action_bit(rule, VCAP_AF_CPU_DIS_MODE, VCAP_BIT_1);
	err |= vcap_rule_add_action_bit(rule, VCAP_AF_CPU_DIS, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_MASK_MODE, PERMIT_MASK);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_PORT_MASK, 0x0);
	err = err ? -EINVAL : lan9645x_vcap_rule_val_add(rule, ETH_P_ALL);
	vcap_free_rule(rule);
	return err;
}

static int lan9645x_vcap_s1_normal_isdx_clf(struct lan9645x *lan9645x,
					    unsigned char *mac, u32 igr_pmsk,
					    u32 isdx_choice, u16 vid_choice,
					    bool rtagged,
					    struct net_device *dev,
					    u32 *vrule_id)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	struct vcap_rule *rule;
	int err = 0;

	rule = vcap_alloc_rule(vctrl, dev, LAN9645X_VCAP_CID_IS1_L1,
			       VCAP_USER_HSR_PRP, 0, 0);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	if (vrule_id)
		*vrule_id = rule->id;

	/* Use S1_NORMAL for all frame types from CPU_PORT in lookup 1. Otherwise,
	 * we would have to make multiple rules.
	 *
	 * For PRP this is not necessary, because IFH.RCT_INJ=1 alters the
	 * frametype classifcation. However, for HSR it is.
	 *
	 * We do not lose much, since CPU_PORT rules only apply for when
	 * IFH.BYPASS=0 which is always HSR/PRP frames.
	 */
	lan_rmw(ANA_VCAP_S1_CFG_KEY_IP6_CFG_SET(VCAP_IS1_PS_IPV6_NORMAL) |
		ANA_VCAP_S1_CFG_KEY_IP4_CFG_SET(VCAP_IS1_PS_IPV4_NORMAL) |
		ANA_VCAP_S1_CFG_KEY_OTHER_CFG_SET(VCAP_IS1_PS_OTHER_NORMAL),
		ANA_VCAP_S1_CFG_KEY_IP6_CFG |
		ANA_VCAP_S1_CFG_KEY_IP4_CFG |
		ANA_VCAP_S1_CFG_KEY_OTHER_CFG,
		lan9645x, ANA_VCAP_S1_CFG(CPU_PORT, 1));

	err = vcap_set_rule_set_keyset(rule, VCAP_KFS_NORMAL);
	err |= vcap_rule_add_key_bit(rule, VCAP_KF_R_TAGGED_IS, vcap2bit(rtagged));
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, igr_pmsk, ~igr_pmsk);
	err |= lan9645x_vcap_add_key_mac(rule, VCAP_KF_L2_SMAC, mac);
	err |= vcap_rule_add_action_bit(rule, VCAP_AF_ISDX_REPLACE_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_ISDX_ADD_VAL, isdx_choice);
	if (vid_choice) {
		err |= vcap_rule_add_action_bit(rule, VCAP_AF_VID_REPLACE_ENA,
						VCAP_BIT_1);
		err |= vcap_rule_add_action_u32(rule, VCAP_AF_VID_VAL,
						vid_choice);
	}
	err = err ? -EINVAL : 0;
	if (!err)
		err = lan9645x_vcap_rule_val_add(rule, ETH_P_ALL);
	vcap_free_rule(rule);
	return err;
}

static int lan9645x_vcap_dan_isdx_counter(struct lan9645x *lan9645x,
					  unsigned char *smac,
					  u32 igr_pmsk,
					  u32 isdx,
					  struct net_device *dev,
					  u32 *vrule_id)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	struct vcap_rule *rule;
	int err = 0;

	rule = vcap_alloc_rule(vctrl, dev, LAN9645X_VCAP_CID_IS1_L2,
			       VCAP_USER_HSR_PRP, 0, 0);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	if (vrule_id)
		*vrule_id = rule->id;

	err = vcap_set_rule_set_keyset(rule, VCAP_KFS_DMAC_VID);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_IF_IGR_PORT_MASK, igr_pmsk, ~0);
	/* This is configured to match SMAC */
	err |= lan9645x_vcap_add_key_mac(rule, VCAP_KF_L2_DMAC, smac);
	err |= vcap_rule_add_action_bit(rule, VCAP_AF_ISDX_REPLACE_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_ISDX_ADD_VAL, isdx);
	err = err ? -EINVAL : lan9645x_vcap_rule_val_add(rule, ETH_P_ALL);
	vcap_free_rule(rule);
	return err;
}

int lan9645x_hsr_prp_dan_node_add(struct lan9645x *lan9645x,
				  int port,
				  struct net_device *hsr,
				  const unsigned char *smac)
{
	struct lan9645x_hsr_prp *h = &lan9645x->hsr;
	struct lan9645x_streamt_entry entry = { 0 };
	struct lan9645x_hsr_prp_dan_node *n;
	struct lan9645x_port *p;
	struct net_device *dev;
	int isdx_a, isdx_b;
	int err = 0;

	p = lan9645x_to_port(lan9645x, port);
	dev = lan9645x_chipport_to_ndev(lan9645x, port);

	mutex_lock(&h->lock);

	if (!lan9645x->hsr.enabled || hsr != p->hsr) {
		err = -EOPNOTSUPP;
		goto unlock;
	}

	list_for_each_entry(n, &h->nodes , list)
		if (ether_addr_equal(n->smac, smac))
			goto unlock;

	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n) {
		err =  -ENOMEM;
		goto unlock;
	}

	/* To track HSR/PRP nodestable counters it is necessary to provision
	 * this in hardware:
	 *
	 * - 2x ISDX's, one for frames on LAN A resp. B.
	 * - 2x Stream table entries, one for each isdx. These entries track
	 *   TimeLastSeen and LanidErr
	 * - 2x IS1 VCAP rules to classify ingress frames on A/B to their ISDX.
	 *
	 * The regular ISDX counters are used for total frame counts on A/B.
	 */

	isdx_a = lan9645x_stream_isdx_alloc(lan9645x);
	if (isdx_a < 0) {
		err = isdx_a;
		goto free_n;
	}

	isdx_b = lan9645x_stream_isdx_alloc(lan9645x);
	if (isdx_b < 0) {
		lan9645x_stream_isdx_free(lan9645x, isdx_a);
		err = isdx_b;
		goto free_n;
	}

	entry.input_port_mask = BIT(h->port_a);
	err = lan9645x_streamt_write(lan9645x, isdx_a, &entry);
	if (err)
		goto free_isdx;

	entry.input_port_mask = BIT(h->port_b);
	err = lan9645x_streamt_write(lan9645x, isdx_b, &entry);
	if (err) {
		lan9645x_streamt_del(lan9645x, isdx_a);
		goto free_isdx;
	}

	ether_addr_copy(n->smac, smac);

	err = lan9645x_vcap_dan_isdx_counter(lan9645x, n->smac, BIT(h->port_a),
					     isdx_a, dev, &n->vrule_a);
	if (err)
		goto free_streamt;

	err = lan9645x_vcap_dan_isdx_counter(lan9645x, n->smac, BIT(h->port_b),
					     isdx_b, dev, &n->vrule_b);
	if (err) {
		vcap_del_rule(lan9645x->vcap_ctrl, dev, n->vrule_a);
		goto free_streamt;
	}

	n->isdx_a = isdx_a;
	n->isdx_b = isdx_b;
	list_add_tail(&n->list, &h->nodes);
	mutex_unlock(&h->lock);
	return 0;

free_streamt:
	lan9645x_streamt_del(lan9645x, isdx_a);
	lan9645x_streamt_del(lan9645x, isdx_b);
free_isdx:
	lan9645x_stream_isdx_free(lan9645x, isdx_b);
	lan9645x_stream_isdx_free(lan9645x, isdx_a);
free_n:
	kfree(n);
unlock:
	mutex_unlock(&h->lock);
	return err;
}

static void lan9645x_hsr_prp_dan_node_dealloc(struct lan9645x *lan9645x,
					      struct net_device *dev,
					      struct lan9645x_hsr_prp_dan_node *n)
{
	if (!n)
		return;

	vcap_del_rule(lan9645x->vcap_ctrl, dev, n->vrule_a);
	vcap_del_rule(lan9645x->vcap_ctrl, dev, n->vrule_b);
	lan9645x_streamt_del(lan9645x, n->isdx_a);
	lan9645x_streamt_del(lan9645x, n->isdx_b);
	lan9645x_stream_isdx_free(lan9645x, n->isdx_a);
	lan9645x_stream_isdx_free(lan9645x, n->isdx_b);
	kfree(n);
}

static void lan9645x_hsr_prp_nodestable_flush(struct lan9645x *lan9645x)
{
	struct lan9645x_hsr_prp_dan_node *n, *tmp;
	struct lan9645x_hsr_prp *h = &lan9645x->hsr;
	struct net_device *dev;

	lockdep_assert_held(&h->lock);

	dev = lan9645x_chipport_to_ndev(lan9645x, h->port_a);

	list_for_each_entry_safe(n, tmp, &h->nodes, list) {
		list_del(&n->list);
		lan9645x_hsr_prp_dan_node_dealloc(lan9645x, dev, n);
	}
}

void lan9645x_hsr_prp_dan_node_del(struct lan9645x *lan9645x,
				   int port,
				   struct net_device *hsr,
				   const unsigned char *smac)
{
	struct lan9645x_hsr_prp_dan_node *tmp, *n = NULL;
	struct lan9645x_hsr_prp *h = &lan9645x->hsr;
	struct lan9645x_port *p;
	struct net_device *dev;

	mutex_lock(&h->lock);

	p = lan9645x_to_port(lan9645x, port);
	dev = lan9645x_chipport_to_ndev(lan9645x, port);

	if (!lan9645x->hsr.enabled || hsr != p->hsr) {
		mutex_unlock(&h->lock);
		return;
	}

	list_for_each_entry(tmp, &h->nodes, list) {
		if (ether_addr_equal(tmp->smac, smac)) {
			n = tmp;
			list_del(&n->list);
			break;
		}
	}
	mutex_unlock(&h->lock);

	lan9645x_hsr_prp_dan_node_dealloc(lan9645x, dev, n);
}

static netdev_features_t
lan9645x_hsr_get_features(enum lan9645x_hsr_type if_type)
{
	switch (if_type) {
	case LAN9645X_HSR:
		return LAN9645X_SUPPORTED_HSR_FEATURES;
	case LAN9645X_PRP:
		return LAN9645X_SUPPORTED_PRP_FEATURES;
	default:
		return 0;
	}
}

static void lan9645x_hsr_features_set(struct lan9645x *lan9645x, int port,
				      enum lan9645x_hsr_type if_type)
{
	struct net_device *ndev = dsa_to_port(lan9645x->ds, port)->user;

	ndev->features |= lan9645x_hsr_get_features(if_type);
}

static void lan9645x_hsr_features_del(struct lan9645x *lan9645x, int port,
				      enum lan9645x_hsr_type if_type)
{
	struct net_device *ndev = dsa_to_port(lan9645x->ds, port)->user;

	ndev->features &= ~lan9645x_hsr_get_features(if_type);
}

static u32 lan9645x_dd_unitsize_to_cycles(u32 unit_size)
{
	switch (unit_size & 0x3) {
	case 0:
		return 32;
	case 1:
		return 256;
	case 2:
		return 4096;
	case 3:
		return 65536;
	default:
		return 32;
	}
}

static void lan9645x_hsr_ddiscard_autoage(struct lan9645x *lan9645x,
					  u32 per_row_ms)
{
	u32 p, cycles, unitsize;
	/* Clock freq 328.125 Mhz
	 * ASIC: 165.625 Mhz
	 * FPGA: 66.125 Mhz
	 *
	 * DD Rows:     R (FPGA 256 - ASIC 512?)
	 * Clock freq:  X Khz
	 * UNIT_SIZE:   Y Cycles
	 * Period_val:  P
	 *
	 * Desired autoage period PER row: N ms
	 *
	 * N ms = P * Y * clk_period * R
	 * N ms / ( Y * 1/X ms * R) = P
	 * N * X / (Y * R) = P
	 *
	 * N=100ms, unit_size=1 => P ~= 501
	 * N=200ms, unit_size=1 FPGA => 200 * 66_125 / (256 * 64) ~= 807
	 */
	unitsize = DD_AUTOAGE_UNITSIZE;
	cycles = lan9645x_dd_unitsize_to_cycles(unitsize);
	p = (per_row_ms * HSR_ASIC_KHZ) / (cycles * DD_TBL_NUM_ROWS);

	lan_wr(QSYS_DISC_AUTOAGE_CFG_UNIT_SIZE_SET(unitsize) |
	       QSYS_DISC_AUTOAGE_CFG_PERIOD_VAL_SET(p),
	       lan9645x, QSYS_DISC_AUTOAGE_CFG);

	lan_wr(QSYS_DISC_AUTOAGE_CFG_1_AUTOAGE_INTERVAL_ENA_SET(1),
	       lan9645x, QSYS_DISC_AUTOAGE_CFG_1);
}

int lan9645x_hsr_prp_prepare(struct lan9645x *lan9645x, int port,
			     struct net_device *hsr, enum lan9645x_hsr_type type,
			     struct netlink_ext_ack *extack)
{
	/* TODO: Do not accept interlink. */

	if (lan9645x->npi == port) {
		NL_SET_ERR_MSG_MOD(extack,
				   "CPU port can not be part of HSR/PRP pair");
		return -ENOTSUPP;
	}

	if (lan9645x->hsr.enabled) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Can only offload one pair of HSR/PRP ports");
		return -ENOSPC;
	}

	if (type == LAN9645X_HSR_UNSUPPORTED) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Only HSR v1 and PRP v1 can be offloaded");
		return -ENOTSUPP;
	}

	return 0;
}

static u32 lan9645x_hsr_shadow_mask(struct lan9645x *lan9645x)
{
	struct lan9645x_hsr_prp *h = &lan9645x->hsr;
	u32 mask = 0;

	if (h->type != LAN9645X_HSR)
		return mask;

	for (int i = 0; i < 2; i++)
		if (h->shadow_ports[i] >= 0)
			mask |= BIT(h->shadow_ports[i]);

	return mask;
}

int lan9645x_hsr_prp_pair_add(struct lan9645x *lan9645x, struct lan9645x_port *lrea,
			      struct lan9645x_port *lreb,
			      struct net_device *lrea_dev, struct net_device *hsr,
			      enum lan9645x_hsr_type type)
{
	struct lan9645x_hsr_prp *h = &lan9645x->hsr;
	struct lan9645x_streamt_entry entry = { 0 };
	unsigned char mac[ETH_ALEN];
	int port, lan_id, net_id;
	int lrea_port, lreb_port;
	u32 port_ab_mask;
	int isdx, err;
	bool rtagged;
	u8 dd_mask;

	mutex_lock(&h->lock);

	if (h->enabled) {
		err = -ENOSPC;
		goto mutex_unlock;
	}

	/* HSR has DD on port A, B, CPU
	 * PRP has DD on port       CPU
	 */
	dd_mask = type == LAN9645X_HSR ? PORT_CPU_A_B : PORT_CPU;
	net_id = type == LAN9645X_HSR ? HSR_NETID : PRP_NETID;
	lrea_port = lrea->chip_port;
	lreb_port = lreb->chip_port;
	port_ab_mask = BIT(lrea_port) | BIT(lreb_port);

	ether_addr_copy(mac, hsr->dev_addr);

	isdx = lan9645x_stream_isdx_alloc(lan9645x);
	if (isdx < 0) {
		err = isdx;
		goto mutex_unlock;
	}

	h->type = type;
	h->port_a = lrea_port;
	h->port_b = lreb_port;
	h->isdx = isdx;
	h->enabled = true;

	entry.input_port_mask = BIT(CPU_PORT) | lan9645x_hsr_shadow_mask(lan9645x);
	entry.seq_gen_ena = true;

	err = lan9645x_streamt_write(lan9645x, isdx, &entry);
	if (err)
		goto free_isdx;

	/* Classify CPU injected to HSR isdx and reserved VLAN. Frames are
	 * identified by:
	 *
	 * - Ingress port == CPU_PORT
	 * - HSR interface SMAC
	 * - Has Rtag
	 *
	 */

	rtagged = h->type == LAN9645X_PRP;
	err = lan9645x_vcap_s1_normal_isdx_clf(lan9645x, mac,
					       entry.input_port_mask, isdx,
					       VLAN_HSR_PRP, rtagged, lrea_dev,
					       &h->isdx_vrule_id);
	if (err)
		goto free_isdx;

	/* TODO: Add supervision frame addr data to if_hsr.h then learn
	 * supervision frame mc addr here, to avoid the flood.
	 */
	err = lan9645x_mact_learn(lan9645x, PGID_CPU, mac, VLAN_HSR_PRP,
				  ENTRYTYPE_LOCKED);
	if (err)
		goto s1_normal_del;

	if (type == LAN9645X_HSR) {
		/* HSR: Add vcap rule to discard own SMAC frames on port A/B.
		 * We use IS2 lookup 2 for these rules. The rules require
		 * reconfiguring the active keys in the port/vcap/lookup to make
		 * sure we match all frames. This makes it challenging to use
		 * this vcap/lookup for other purposes.
		 * We leave lookup 1 to the user, where they can operate as
		 * normal.
		 */
		err = lan9645x_vcap_s2_hsr_smac_kill(lan9645x, port_ab_mask, mac, lrea_dev,
						     &h->local_ring_vrule_id);
		if (err)
			goto mac_forget;
	}

	for_each_hsr_port(lan_id, port, lrea_port, lreb_port) {
		/* Set PVID to reserved for port AB (rx pid) */
		lan_rmw(ANA_VLAN_CFG_VLAN_VID_SET(VLAN_HSR_PRP),
			ANA_VLAN_CFG_VLAN_VID,
			lan9645x, ANA_VLAN_CFG(port));

		/* Set PVID in rewriter, to enable no-tagging for PVID */
		lan_rmw(REW_PORT_VLAN_CFG_PORT_VID_SET(VLAN_HSR_PRP),
			REW_PORT_VLAN_CFG_PORT_VID,
			lan9645x, REW_PORT_VLAN_CFG(port));

		/* Use TPID=0x8100 and disable egress tagging for vid=0 and
		 * vid=VLAN_HSR_PRP
		 */
		lan_rmw(REW_TAG_CFG_TAG_TPID_CFG_SET(0) |
			REW_TAG_CFG_TAG_CFG_SET(LAN9645X_TAG_NO_PVID_NO_UNAWARE),
			REW_TAG_CFG_TAG_TPID_CFG |
			REW_TAG_CFG_TAG_CFG,
			lan9645x, REW_TAG_CFG(port));

		lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(true),
			ANA_PORT_CFG_LEARN_ENA,
			lan9645x, ANA_PORT_CFG(port));

		lan_rmw(ANA_RED_CFG_PRP_AWARE_ENA_SET(type == LAN9645X_PRP) |
			ANA_RED_CFG_HSR_AWARE_ENA_SET(type == LAN9645X_HSR) |
			ANA_RED_CFG_LANID_SET(!lan_id) |
			ANA_RED_CFG_NETID_SET(net_id),
			ANA_RED_CFG_PRP_AWARE_ENA |
			ANA_RED_CFG_HSR_AWARE_ENA |
			ANA_RED_CFG_LANID |
			ANA_RED_CFG_NETID,
			lan9645x, ANA_RED_CFG(port));

		/* HSR specific. */
		lan_rmw(REW_RED_TAG_CFG_HSR_TAG_ENA_SET(type == LAN9645X_HSR) |
			REW_RED_TAG_CFG_NETID_SET(net_id) |
			REW_RED_TAG_CFG_LANID_SET(lan_id) |
			REW_RED_TAG_CFG_RED_TAG_CFG_SET(type == LAN9645X_HSR) |
			/* is this rct_ena field not used by hw? */
			REW_RED_TAG_CFG_PRP_RCT_TAG_ENA_SET(type == LAN9645X_PRP),
			REW_RED_TAG_CFG_HSR_TAG_ENA |
			REW_RED_TAG_CFG_NETID |
			REW_RED_TAG_CFG_LANID |
			REW_RED_TAG_CFG_RED_TAG_CFG |
			REW_RED_TAG_CFG_PRP_RCT_TAG_ENA,
			lan9645x, REW_RED_TAG_CFG(port));

		/* Use IS1 lookup 3 for node table entry isdx classification.
		 * We configure S1_DMAC_VID to use SMAC instead.
		 */
		lan_rmw(ANA_VCAP_CFG_S1_SMAC_ENA_SET(S1_LOOKUP3),
			ANA_VCAP_CFG_S1_SMAC_ENA,
			lan9645x, ANA_VCAP_CFG(port));

		/* Use S1_DMAC_VID for all frame types in IS1 Lookup 3. */
		lan_rmw(ANA_VCAP_S1_CFG_KEY_IP6_CFG_SET(6) |
			ANA_VCAP_S1_CFG_KEY_IP4_CFG_SET(4) |
			ANA_VCAP_S1_CFG_KEY_OTHER_CFG_SET(3),
			ANA_VCAP_S1_CFG_KEY_IP6_CFG |
			ANA_VCAP_S1_CFG_KEY_IP4_CFG |
			ANA_VCAP_S1_CFG_KEY_OTHER_CFG,
			 lan9645x, ANA_VCAP_S1_CFG(port, 2));

		if (type == LAN9645X_HSR) {
			/* HSR only: Treat the frame types
			 *
			 * - ARP
			 * - IPv4 TCP/UDP
			 * - IPv4 non-tcp/udp
			 * - OAM
			 * - IP6
			 *
			 * as MAC_ETYPE in S2 second lookup. This will ensure our
			 * SMAC kill rule, discards all frames. The downside is
			 * this keyset selection configuration is global for S2
			 * lookup2. But it is the only way to discard all frames
			 * based on ingress/smac.
			 */
			lan9645x_is2_only_mac_etype_llc(lan9645x, S2_LOOKUP2,
							port);

			/* Enable ETYPE hiding/extension. HW requires HSR header
			 * to be aligned on a 32bit boundary. On ingress it will
			 * remove etype, and on egress it will add it.
			 */
			lan_rmw(DEV_PORT_MISC_RTAG48_ENA_SET(1) |
				DEV_PORT_MISC_RTAG_TYPE_SET(1), // 0x892F HSR
				DEV_PORT_MISC_RTAG48_ENA |
				DEV_PORT_MISC_RTAG_TYPE,
				lan9645x, DEV_PORT_MISC(port));

			lan_rmw(ANA_PORT_MODE_REDTAG_PARSE_CFG_SET(1),
				ANA_PORT_MODE_REDTAG_PARSE_CFG,
				lan9645x, ANA_PORT_MODE(port));

			/* Redir tagless frames to CPU */
			lan_rmw(ANA_CPU_FWD_CFG_NO_HSR_REDIR_ENA_SET(1),
				ANA_CPU_FWD_CFG_NO_HSR_REDIR_ENA,
				lan9645x, ANA_CPU_FWD_CFG(port));
		} else {
			/* RCT rewrite seqnum and lanID. Software must add
			 * trailer to frame with LSDU size and PRP suffix.
			 */
			lan_rmw(SYS_PORT_MODE_PRP_LANID_SET(lan_id) |
				SYS_PORT_MODE_PRP_ENA_SET(1),
				SYS_PORT_MODE_PRP_LANID |
				SYS_PORT_MODE_PRP_ENA,
				lan9645x, SYS_PORT_MODE(port));
		}

		/* Piggyback on LAG functionality to pick a port to use
		 * as representant for mac table purposes. This avoids
		 * mac table thrashing when the same MAC arrives on both A and
		 * B.
		 */
		lan_rmw(ANA_PORT_CFG_PORTID_VAL_SET(lrea_port),
			ANA_PORT_CFG_PORTID_VAL,
			lan9645x,
			ANA_PORT_CFG(port));

		/* Trap BPDU frames. Is this required? */
		lan_rmw(ANA_CPU_FWD_BPDU_CFG_BPDU_REDIR_ENA_SET(0xffff),
			ANA_CPU_FWD_BPDU_CFG_BPDU_REDIR_ENA,
			lan9645x, ANA_CPU_FWD_BPDU_CFG(port));

		/* MAC lookup hit for port A/B adds both ports to dest mask */
		lan_wr(port_ab_mask, lan9645x, ANA_PGID(port));
		lan9645x_hsr_features_set(lan9645x, port, type);
	}

	if (type == LAN9645X_PRP) {
		lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(0),
			ANA_PORT_CFG_LEARN_ENA,
			lan9645x,
			ANA_PORT_CFG(CPU_PORT));
	}

	/* Setup duplicate discard in queue system */
	lan_wr(QSYS_MISC_DROP_CFG_FRER_ENA_SET(true),
	       lan9645x, QSYS_MISC_DROP_CFG);

	/* Set duplicate discard autoage period per row */
	lan9645x_hsr_ddiscard_autoage(lan9645x, 200);

	/* Setup duplicate discard ports in queue system */
	lan_rmw(QSYS_DD_CFG_LREB_PORT_SET(lreb_port) |
		QSYS_DD_CFG_LREA_PORT_SET(lrea_port) |
		QSYS_DD_CFG_CPU_PORT_SET(lan9645x->npi) |
		QSYS_DD_CFG_DUPL_DISC_MASK_SET(dd_mask) |
		QSYS_DD_CFG_UPD_DISC_TBL_MASK_SET(dd_mask),
		QSYS_DD_CFG_LREB_PORT |
		QSYS_DD_CFG_LREA_PORT |
		QSYS_DD_CFG_CPU_PORT |
		QSYS_DD_CFG_DUPL_DISC_MASK |
		QSYS_DD_CFG_UPD_DISC_TBL_MASK,
		lan9645x, QSYS_DD_CFG);

	lan_rmw(ANA_RED_MISC_CFG_LREA_PORT_SET(lrea_port) |
		ANA_RED_MISC_CFG_LREB_PORT_SET(lreb_port) |
		ANA_RED_MISC_CFG_CHK_LANID_ENA_SET(1) |
		ANA_RED_MISC_CFG_DUPL_DISC_ENA_SET(1) |
		ANA_RED_MISC_CFG_PRP_ENA_SET(type == LAN9645X_PRP),
		ANA_RED_MISC_CFG_LREA_PORT |
		ANA_RED_MISC_CFG_LREB_PORT |
		ANA_RED_MISC_CFG_CHK_LANID_ENA |
		ANA_RED_MISC_CFG_DUPL_DISC_ENA |
		ANA_RED_MISC_CFG_PRP_ENA,
		lan9645x, ANA_RED_MISC_CFG);

	mutex_lock(&lan9645x->fwd_domain_lock);
	lan9645x_vlan_set_port_mask(lan9645x, HOST_PVID,
				    lan9645x->vlans[HOST_PVID].portmask &
				    ~BIT(lrea->chip_port) &
				    ~BIT(lreb->chip_port));
	lan9645x_vlan_set_port_mask(lan9645x, VLAN_HSR_PRP,
				    port_ab_mask | BIT(CPU_PORT) |
				    lan9645x_hsr_shadow_mask(lan9645x));
	lrea->hsr = hsr;
	lreb->hsr = hsr;
	lan9645x->mc_flood_mask |= port_ab_mask;
	lan9645x->mrouter_mask |= port_ab_mask;
	__lan9645x_pgid_mc_update(lan9645x);
	lan9645x_update_fwd_mask(lan9645x, true);
	mutex_unlock(&lan9645x->fwd_domain_lock);

	mutex_unlock(&h->lock);

	return 0;

mac_forget:
	lan9645x_mact_forget(lan9645x, mac, VLAN_HSR_PRP, ENTRYTYPE_LOCKED);
s1_normal_del:
	vcap_del_rule(lan9645x->vcap_ctrl, lrea_dev, h->isdx_vrule_id);
free_isdx:
	lan9645x_stream_isdx_free(lan9645x, isdx);
mutex_unlock:
	mutex_unlock(&h->lock);
	return err;
}

int lan9645x_hsr_prp_pair_del(struct lan9645x *lan9645x, int port,
			      struct net_device *hsr)
{
	struct lan9645x_hsr_prp *h = &lan9645x->hsr;
	struct lan9645x_port *lrea, *lreb;
	enum lan9645x_hsr_type type;
	int lrea_port, lreb_port;
	int lan_id;
	int err = 0;

	err = lan9645x_hsr2type(hsr, &type);
	if (err)
		return err;

	mutex_lock(&h->lock);

	if (!h->enabled)
		goto unlock;

	dev_dbg(lan9645x->dev, "port=%d lan_a=%d lan_b=%d type=%d\n", port,
		h->port_a, h->port_b, h->type);

	if (h->type != type) {
		dev_err(lan9645x->dev, "HSR deleting unexpected HSR type\n");
		err = -ENOTSUPP;
		goto unlock;
	}

	if (port == h->port_a)
		goto unlock;

	if (port != h->port_b) {
		dev_err(lan9645x->dev, "HSR removing unexpected port=%d\n",
			port);
		err = -EINVAL;
		goto unlock;
	}

	lrea_port = h->port_a;
	lreb_port = h->port_b;
	lrea = lan9645x_to_port(lan9645x, lrea_port);
	lreb = lan9645x_to_port(lan9645x, lreb_port);

	mutex_lock(&lan9645x->fwd_domain_lock);
	lrea->hsr = NULL;
	lreb->hsr = NULL;
	lan9645x->mc_flood_mask &= ~(BIT(lrea_port) | BIT(lreb_port));
	lan9645x->mrouter_mask &= ~(BIT(lrea_port) | BIT(lreb_port));
	__lan9645x_pgid_mc_update(lan9645x);
	lan9645x_update_fwd_mask(lan9645x, false);
	lan9645x_vlan_set_hostmode(lrea);
	lan9645x_vlan_set_hostmode(lreb);
	lan9645x_vlan_set_port_mask(lan9645x, VLAN_HSR_PRP, 0);
	mutex_unlock(&lan9645x->fwd_domain_lock);

	lan9645x_streamt_del(lan9645x, h->isdx);
	lan9645x_stream_isdx_free(lan9645x, h->isdx);

	/* NOTE: need some non-NULL net_device for the vcap_api. */
	err = vcap_del_rule(lan9645x->vcap_ctrl, hsr, h->isdx_vrule_id);
	if (err)
		dev_err(lan9645x->dev, "hsr remove vcap rule: %u err: %d\n",
			h->isdx_vrule_id, err);

	lan9645x_mact_forget(lan9645x, h->mac, VLAN_HSR_PRP, ENTRYTYPE_LOCKED);

	if (type == LAN9645X_HSR) {
		err = vcap_del_rule(lan9645x->vcap_ctrl, hsr,
				    h->local_ring_vrule_id);
		if (err)
			dev_err(lan9645x->dev,
				"hsr remove vcap rule: %u err: %d\n",
				h->local_ring_vrule_id, err);
		err = vcap_del_rule(lan9645x->vcap_ctrl, hsr,
				    h->ptp_dd_vrule_id);
		if (err)
			dev_err(lan9645x->dev,
				"hsr remove vcap rule: %u err: %d\n",
				h->ptp_dd_vrule_id, err);
	}

	lan9645x_hsr_prp_nodestable_flush(lan9645x);

	lan_wr(0, lan9645x, QSYS_DD_CFG);

	lan_wr(QSYS_MISC_DROP_CFG_FRER_ENA_SET(false),
	       lan9645x, QSYS_MISC_DROP_CFG);

	lan_rmw(ANA_RED_MISC_CFG_CHK_LANID_ENA_SET(false) |
		ANA_RED_MISC_CFG_DUPL_DISC_ENA_SET(false) |
		ANA_RED_MISC_CFG_PRP_ENA_SET(false),
		ANA_RED_MISC_CFG_CHK_LANID_ENA |
		ANA_RED_MISC_CFG_DUPL_DISC_ENA |
		ANA_RED_MISC_CFG_PRP_ENA,
		lan9645x, ANA_RED_MISC_CFG);

	for_each_hsr_port(lan_id, port, h->port_a, h->port_b) {
		lan_wr(0, lan9645x, ANA_RED_CFG(port));

		/* Disable PRP trailer rewrites */
		lan_rmw(SYS_PORT_MODE_PRP_ENA_SET(false),
			SYS_PORT_MODE_PRP_ENA,
			lan9645x, SYS_PORT_MODE(port));

		lan_rmw(REW_RED_TAG_CFG_RED_TAG_CFG_SET(1),
			REW_RED_TAG_CFG_RED_TAG_CFG,
			lan9645x, REW_RED_TAG_CFG(port));

		lan_rmw(REW_RED_TAG_CFG_HSR_TAG_ENA_SET(false) |
			REW_RED_TAG_CFG_RED_TAG_CFG_SET(true),
			REW_RED_TAG_CFG_HSR_TAG_ENA |
			REW_RED_TAG_CFG_RED_TAG_CFG,
			lan9645x, REW_RED_TAG_CFG(port));

		lan9645x_is2_default_conf(lan9645x, S2_LOOKUP2, port);

		lan_rmw(DEV_PORT_MISC_RTAG48_ENA_SET(false) |
			DEV_PORT_MISC_RTAG_TYPE_SET(0), // 0xf1c1 freer
			DEV_PORT_MISC_RTAG48_ENA |
			DEV_PORT_MISC_RTAG_TYPE,
			lan9645x, DEV_PORT_MISC(port));

		lan_rmw(ANA_PORT_MODE_REDTAG_PARSE_CFG_SET(0),
			ANA_PORT_MODE_REDTAG_PARSE_CFG,
			lan9645x, ANA_PORT_MODE(port));

		lan_rmw(SYS_PORT_MODE_PRP_ENA_SET(false),
			SYS_PORT_MODE_PRP_ENA,
			lan9645x, SYS_PORT_MODE(port));

		lan_rmw(ANA_CPU_FWD_BPDU_CFG_BPDU_REDIR_ENA_SET(0),
			ANA_CPU_FWD_BPDU_CFG_BPDU_REDIR_ENA,
			lan9645x, ANA_CPU_FWD_BPDU_CFG(port));

		if (type == LAN9645X_PRP) {
			lan_rmw(ANA_PORT_CFG_PORTID_VAL_SET(port),
				ANA_PORT_CFG_PORTID_VAL, lan9645x,
				ANA_PORT_CFG(port));
		}

		lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(false),
			ANA_PORT_CFG_LEARN_ENA,
			lan9645x, ANA_PORT_CFG(port));

		lan_wr(BIT(port), lan9645x, ANA_PGID(port));
		lan9645x_hsr_features_del(lan9645x, port, type);
	}

	lan_wr(QSYS_DISC_AUTOAGE_CFG_1_AUTOAGE_INTERVAL_ENA_SET(false),
	       lan9645x, QSYS_DISC_AUTOAGE_CFG_1);

	lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(true),
		ANA_PORT_CFG_LEARN_ENA,
		lan9645x,
		ANA_PORT_CFG(CPU_PORT));

	h->enabled = false;
unlock:
	mutex_unlock(&h->lock);
	return err;
}

static void
lan9645x_hsr_prp_dan_update(struct lan9645x *lan9645x,
			    struct lan9645x_hsr_prp_dan_node *node,
			    struct lan9645x_hsr_prp_node_counters *ncnt)
{
	struct lan9645x_streamt_entry entry = { 0 };
	u64 *isdx_cnt;

	lockdep_assert_held(&lan9645x->hsr.lock);

	isdx_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_ISDX, node->isdx_a);
	ncnt->cnt_rcv_a = isdx_cnt[SCNT_ISDX_GREEN_PKT] +
			  isdx_cnt[SCNT_ISDX_YELLOW_PKT] +
			  isdx_cnt[SCNT_ISDX_RED_PKT];

	isdx_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_ISDX, node->isdx_b);
	ncnt->cnt_rcv_b = isdx_cnt[SCNT_ISDX_GREEN_PKT] +
			  isdx_cnt[SCNT_ISDX_YELLOW_PKT] +
			  isdx_cnt[SCNT_ISDX_RED_PKT];

	lan9645x_streamt_read(lan9645x, node->isdx_a, &entry);
	ncnt->cnt_err_wrng_lan_a = entry.lanid_err;
	ncnt->time_last_seen_a = entry.time_last_seen;
	lan9645x_streamt_read(lan9645x, node->isdx_b, &entry);
	ncnt->cnt_err_wrng_lan_b = entry.lanid_err;
	ncnt->time_last_seen_b = entry.time_last_seen;
}

static int lan9645x_hsr_prp_nodestable_show(struct seq_file *m,
					    void *unused)
{
	struct lan9645x_hsr_prp_node_counters ncnt = { 0 };
	struct lan9645x *lan9645x = m->private;
	struct lan9645x_hsr_prp_dan_node *n;
	struct lan9645x_hsr_prp *h;
	int i = 0;

	h = &lan9645x->hsr;

	/* Update ISDX counters */
	lan9645x_stats_view_update(lan9645x, LAN9645X_STAT_ISDX);

	seq_printf(m, "Inst MAC Address       Node Type   RxA        RxB        LastSeenA  LastSeenB  ErrLanidA  ErrLanidB\n");
	seq_printf(m, "---- ----------------- ----------- ---------- ---------- ---------- ---------- ---------- ----------\n");

	list_for_each_entry(n, &h->nodes, list) {
		mutex_lock(&h->lock);
		lan9645x_hsr_prp_dan_update(lan9645x, n, &ncnt);
		mutex_unlock(&h->lock);

		seq_printf(m, "%4u %pM %-11s %10u %10u %10u %10u %10u %10u\n",
			   i, n->smac,
			   "DAN",
			   ncnt.cnt_rcv_a,
			   ncnt.cnt_rcv_b,
			   ncnt.time_last_seen_a,
			   ncnt.time_last_seen_b,
			   ncnt.cnt_err_wrng_lan_a,
			   ncnt.cnt_err_wrng_lan_b);
		i++;
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan9645x_hsr_prp_nodestable);

int lan9645x_hsr_prp_init(struct lan9645x *lan9645x)
{
	struct dentry *dir;
	int err;

	mutex_init(&lan9645x->hsr.lock);
	INIT_LIST_HEAD(&lan9645x->hsr.nodes);

	err = lan9645x_ptp_hsr_init(lan9645x);
	if (err)
		return err;

	dir = debugfs_create_dir("hsr", lan9645x->debugfs_root);
	if (PTR_ERR_OR_ZERO(dir))
		return 0;

	debugfs_create_file("nodestable", 0444, dir, lan9645x,
			    &lan9645x_hsr_prp_nodestable_fops);

	return 0;
}

void lan9645x_hsr_prp_deinit(struct lan9645x *lan9645x)
{
	mutex_destroy(&lan9645x->hsr.lock);
}
