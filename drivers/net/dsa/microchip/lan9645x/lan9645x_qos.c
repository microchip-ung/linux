// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/errno.h>

#include "lan9645x_main.h"

#define LAN9645X_PORT_QOS_DSCP_COUNT	64

enum rew_pcp_dei_mode {
	CLASSIFIED = 0,
	PORT_BASED = 1,
	MAPPED = 2,
	QOS_DP_LEVEL = 3,
};

static int __lan9645x_qos_polix_alloc(struct lan9645x *lan9645x)
{
	int polix;

	lockdep_assert_held(&lan9645x->qos_lock);

	polix = find_first_zero_bit(lan9645x->pol_idx_mask,
				    LAN9645X_NUM_POL_POOL);
	if (polix >= LAN9645X_ISDX_MAX)
		return -ENOSPC;

	set_bit(polix, lan9645x->pol_idx_mask);

	return polix + LAN9645X_POL_IX_POOL;
}

int lan9645x_qos_polix_alloc(struct lan9645x *lan9645x)
{
	int ret;

	mutex_lock(&lan9645x->qos_lock);
	ret = __lan9645x_qos_polix_alloc(lan9645x);
	mutex_unlock(&lan9645x->qos_lock);

	return ret;
}

static void __lan9645x_qos_polix_free(struct lan9645x *lan9645x, u16 polix)
{
	lockdep_assert_held(&lan9645x->qos_lock);

	if (polix < LAN9645X_POL_IX_POOL)
		return;

	clear_bit(polix - LAN9645X_POL_IX_POOL, lan9645x->pol_idx_mask);
}

void lan9645x_qos_polix_free(struct lan9645x *lan9645x, u16 polix)
{
	mutex_lock(&lan9645x->qos_lock);
	__lan9645x_qos_polix_free(lan9645x, polix);
	mutex_unlock(&lan9645x->qos_lock);
}

int lan9645x_qos_init(struct lan9645x *lan9645x)
{
	mutex_init(&lan9645x->qos_lock);
	return 0;
}

void lan9645x_qos_port_init(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;
	int pcp, dei, qos, dpl;
	u8 tag_cfg;

	/* Setup ingress 1:1 mapping between tag [PCP,DEI] and [PRIO,DPL].
	 * PCP determines the priority (0..7) of the frame and
	 * DEI determines the color (green og yellow) of the frame.
	 */
	for (pcp = 0; pcp < 8; pcp++) {
		for (dei = 0; dei < 2; dei++) {
			lan_wr(ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL_SET(dei) |
			       ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL_SET(pcp),
			       lan9645x,
			       ANA_PCP_DEI_CFG(p->chip_port, 8 * dei + pcp));
		}
	}

	/* Setup egress 1:1 mapping between [PRIO,DPL] and [PCP,DEI].
	 * priority determines the PCP value (0..7) in the frame and
	 * DPL determines the DEI value (0..1) in the frame.
	 */
	for (qos = 0; qos < 8; qos++) {
		for (dpl = 0; dpl < 2; dpl++) {
			lan_wr(REW_PCP_DEI_CFG_DEI_QOS_VAL_SET(dpl) |
			       REW_PCP_DEI_CFG_PCP_QOS_VAL_SET(qos),
			       lan9645x,
			       REW_PCP_DEI_CFG(p->chip_port, 8 * dpl + qos));
		}
	}

	/* We setup 1:1 maps pcp/dei -> qos/dpl -> pcp/dei, so tag_cfg=2 is
	 * equivalent to tag_cfg=0 (use classified PCP/DEI) for tagged frames.
	 */
	tag_cfg = CLASSIFIED;
	lan_rmw(REW_TAG_CFG_TAG_PCP_CFG_SET(tag_cfg) |
		REW_TAG_CFG_TAG_DEI_CFG_SET(tag_cfg),
		REW_TAG_CFG_TAG_PCP_CFG |
		REW_TAG_CFG_TAG_DEI_CFG,
		lan9645x, REW_TAG_CFG(p->chip_port));

	lan_rmw(ANA_QOS_CFG_QOS_PCP_ENA_SET(0),
		ANA_QOS_CFG_QOS_PCP_ENA,
		lan9645x, ANA_QOS_CFG(p->chip_port));
}

int lan9645x_qos_port_get_default_prio(struct lan9645x *lan9645x, int port)
{
	u32 qos_cfg;

	qos_cfg = lan_rd(lan9645x, ANA_QOS_CFG(port));

	return ANA_QOS_CFG_QOS_DEFAULT_VAL_GET(qos_cfg);
}

int lan9645x_qos_port_set_default_prio(struct lan9645x *lan9645x, int port,
				       u8 prio)
{
	if (prio >= LAN9645X_NUM_TC)
		return -ERANGE;

	lan_rmw(ANA_QOS_CFG_QOS_DEFAULT_VAL_SET(prio),
		ANA_QOS_CFG_QOS_DEFAULT_VAL,
		lan9645x, ANA_QOS_CFG(port));

	return 0;
}

int lan9645x_qos_port_get_dscp_prio(struct lan9645x *lan9645x, int port,
				    u8 dscp)
{
	u32 qos_cfg = lan_rd(lan9645x, ANA_QOS_CFG(port));
	u32 dscp_cfg = lan_rd(lan9645x, ANA_DSCP_CFG(dscp));

	if (!ANA_QOS_CFG_QOS_DSCP_ENA_GET(qos_cfg))
		return -EOPNOTSUPP;

	if (ANA_QOS_CFG_DSCP_TRANSLATE_ENA_GET(qos_cfg)) {
		dscp = ANA_DSCP_CFG_DSCP_TRANSLATE_VAL_GET(dscp_cfg);
		dscp_cfg = lan_rd(lan9645x, ANA_DSCP_CFG(dscp));
	}

	if (!ANA_DSCP_CFG_DSCP_TRUST_ENA_GET(dscp_cfg))
		return -EOPNOTSUPP;

	return ANA_DSCP_CFG_QOS_DSCP_VAL_GET(dscp_cfg);
}

int lan9645x_qos_port_add_dscp_prio(struct lan9645x *lan9645x, int port,
				    u8 dscp, u8 prio)
{
	if (prio >= LAN9645X_NUM_TC)
		return -ERANGE;

	/* dcbnl does not support DSCP translation. */
	lan_rmw(ANA_QOS_CFG_QOS_DSCP_ENA_SET(1) |
		ANA_QOS_CFG_DSCP_TRANSLATE_ENA_SET(0),
		ANA_QOS_CFG_QOS_DSCP_ENA |
		ANA_QOS_CFG_DSCP_TRANSLATE_ENA,
		lan9645x, ANA_QOS_CFG(port));

	lan_rmw(ANA_DSCP_CFG_DSCP_TRUST_ENA_SET(1) |
		ANA_DSCP_CFG_QOS_DSCP_VAL_SET(prio) |
		ANA_DSCP_CFG_DP_DSCP_VAL_SET(0),
		ANA_DSCP_CFG_DSCP_TRUST_ENA |
		ANA_DSCP_CFG_QOS_DSCP_VAL |
		ANA_DSCP_CFG_DP_DSCP_VAL,
		lan9645x, ANA_DSCP_CFG(dscp));

	return 0;
}

int lan9645x_qos_port_del_dscp_prio(struct lan9645x *lan9645x, int port,
				    u8 dscp, u8 prio)
{
	u32 dscp_cfg = lan_rd(lan9645x, ANA_DSCP_CFG(dscp));
	int i;

	/* During a "dcb app replace" command, the new app table entry will be
	 * added first, then the old one will be deleted. But the hardware only
	 * supports one QoS class per DSCP value (duh), so if we blindly delete
	 * the app table entry for this DSCP value, we end up deleting the
	 * entry with the new priority. Avoid that by checking whether user
	 * space wants to delete the priority which is currently configured, or
	 * something else which is no longer current.
	 */
	if (ANA_DSCP_CFG_QOS_DSCP_VAL_GET(dscp_cfg) != prio)
		return 0;

	lan_wr(0x0, lan9645x, ANA_DSCP_CFG(dscp));

	for (i = 0; i < LAN9645X_PORT_QOS_DSCP_COUNT; i++) {
		dscp_cfg = lan_rd(lan9645x, ANA_DSCP_CFG(i));

		if (ANA_DSCP_CFG_DSCP_TRUST_ENA_GET(dscp_cfg))
			return 0;
	}

	lan_rmw(0,
		ANA_QOS_CFG_QOS_DSCP_ENA |
		ANA_QOS_CFG_DSCP_TRANSLATE_ENA,
		lan9645x, ANA_QOS_CFG(port));

	return 0;
}
