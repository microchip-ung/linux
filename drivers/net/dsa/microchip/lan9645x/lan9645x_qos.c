// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/errno.h>

#include "lan9645x_main.h"

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

static void lan9645x_qos_port_init(struct lan9645x_port *p)
{
	int pcp, dei, qos, dpl;

	mutex_init(&p->qos_lock);

	p->qos.pfc_enable = 0;
	p->qos.i_mode.tag_map_enable = false;
	p->qos.i_mode.dscp_map_enable = false;
	/* Setup ingress 1:1 mapping between tag [PCP,DEI] and [PRIO,DPL].
	 * PCP determines the priority (0..7) of the frame and
	 * DEI determines the color (green og yellow) of the frame.
	 */
	for (pcp = 0; pcp < 8; pcp++) {
		for (dei = 0; dei < 2; dei++) {
			p->qos.i_map[pcp][dei].prio = pcp;
			p->qos.i_map[pcp][dei].dpl = dei;
		}
	}

	/* Setup egress 1:1 mapping between [PRIO,DPL] and [PCP,DEI].
	 * priority determines the PCP value (0..7) in the frame and
	 * DPL determines the DEI value (0..1) in the frame.
	 */
	for (qos = 0; qos < 8; qos++) {
		for (dpl = 0; dpl < 2; dpl++) {
			p->qos.e_map[qos][dpl].pcp = qos;
			p->qos.e_map[qos][dpl].dei = dpl;
		}
	}

	p->qos.e_mode = E_MODE_CLASSIFIED;
}

static int __lan9645x_qos_setpfc(struct lan9645x *lan9645x, int port,
				 u8 pfc_enable)
{
	struct lan9645x_port *p;
	struct net_device *dev;
	int fc_cfg;

	p = lan9645x_to_port(lan9645x, port);
	dev = lan9645x_port_to_ndev(p);

	lockdep_assert_held(&p->qos_lock);

	if (pfc_enable) {
		fc_cfg = lan_rd(lan9645x, SYS_MAC_FC_CFG(p->chip_port));
		if ((SYS_MAC_FC_CFG_RX_FC_ENA_GET(fc_cfg) != 0) ||
		    (SYS_MAC_FC_CFG_TX_FC_ENA_GET(fc_cfg) != 0)) {
			netdev_err(dev,
				   "802.3X FC and 802.1Qbb PFC cannot both be enabled.\n");
			return -EOPNOTSUPP;
		}
	}

	if (p->qos.pfc_enable != pfc_enable) {
		lan_rmw(ANA_VLAN_CFG_VLAN_PFC_ENA_SET(pfc_enable ? 1 : 0),
			ANA_VLAN_CFG_VLAN_PFC_ENA, lan9645x,
			ANA_VLAN_CFG(p->chip_port));

		lan_rmw(DEV_PORT_MISC_FWD_CTRL_ENA_SET(pfc_enable ? 1 : 0),
			DEV_PORT_MISC_FWD_CTRL_ENA, lan9645x,
			DEV_PORT_MISC(p->chip_port));

		lan_rmw(ANA_PFC_CFG_RX_PFC_ENA_SET(pfc_enable),
			ANA_PFC_CFG_RX_PFC_ENA, lan9645x,
			ANA_PFC_CFG(p->chip_port));

		lan_rmw(QSYS_SW_PORT_MODE_TX_PFC_ENA_SET(pfc_enable),
			QSYS_SW_PORT_MODE_TX_PFC_ENA, lan9645x,
			QSYS_SW_PORT_MODE(p->chip_port));

		p->qos.pfc_enable = pfc_enable;

		if (dev->flags & IFF_UP) {
			dev_close(dev);
			return dev_open(dev, NULL);
		}
	}

	return 0;
}

int __lan9645x_qos_portconf_set(struct lan9645x_port *p,
				struct lan9645x_port_qos *cfg)
{
	struct lan9645x *lan9645x = p->lan9645x;
	u32 pcp, dei;
	u8 prio, dpl;
	int err = 0;

	lockdep_assert_held(&p->qos_lock);

	/* Setup port ingress default DEI and PCP */
	lan_rmw(ANA_VLAN_CFG_VLAN_DEI_SET(!!cfg->i_default_dei) |
		ANA_VLAN_CFG_VLAN_PCP_SET(cfg->i_default_pcp),
		ANA_VLAN_CFG_VLAN_DEI |
		ANA_VLAN_CFG_VLAN_PCP,
		lan9645x, ANA_VLAN_CFG(p->chip_port));

	/* Setup port ingress default DPL and Priority */
	lan_rmw(ANA_QOS_CFG_DP_DEFAULT_VAL_SET(!!cfg->i_default_dpl) |
		ANA_QOS_CFG_QOS_DEFAULT_VAL_SET(cfg->i_default_prio) |
		ANA_QOS_CFG_QOS_PCP_ENA_SET(!!cfg->i_mode.tag_map_enable) |
		ANA_QOS_CFG_QOS_DSCP_ENA_SET(!!cfg->i_mode.dscp_map_enable),
		ANA_QOS_CFG_DP_DEFAULT_VAL |
		ANA_QOS_CFG_QOS_DEFAULT_VAL |
		ANA_QOS_CFG_QOS_DSCP_ENA |
		ANA_QOS_CFG_QOS_PCP_ENA,
		lan9645x, ANA_QOS_CFG(p->chip_port));

	/* Setup port ingress mapping between [PCP,DEI] and [Priority]. */
	/* Setup port ingress mapping between [PCP,DEI] and [DPL]. */
	for (pcp = 0; pcp < LAN9645X_PCP_COUNT; pcp++) {
		for (dei = 0; dei < LAN9645X_DEI_COUNT; dei++) {
			prio = cfg->i_map[pcp][dei].prio;
			dpl = cfg->i_map[pcp][dei].dpl;
			lan_wr(ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL_SET(prio) |
			       ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL_SET(!!dpl),
			       lan9645x,
			       ANA_PCP_DEI_CFG(p->chip_port, LAN9645X_PCP_COUNT * dei + pcp));
		}
	}

	dei = (cfg->e_mode == E_MODE_QOS_DP ? cfg->e_default_dei : 0);

	/* Setup port egress default DEI and PCP */
	lan_rmw(REW_PORT_VLAN_CFG_PORT_DEI_SET(!!dei) |
		REW_PORT_VLAN_CFG_PORT_PCP_SET(cfg->e_default_pcp),
		REW_PORT_VLAN_CFG_PORT_DEI |
		REW_PORT_VLAN_CFG_PORT_PCP,
		lan9645x, REW_PORT_VLAN_CFG(p->chip_port));

	/* Setup port egress mapping between [Priority] and [PCP,DEI]. */
	/* Setup port egress mapping between [DPL] and [PCP,DEI]. */
	for (prio = 0; prio < LAN9645X_PRIO_COUNT; prio++) {
		for (dpl = 0; dpl < LAN9645X_DPL_COUNT; dpl++) {
			pcp = cfg->e_map[prio][dpl].pcp;
			dei = cfg->e_map[prio][dpl].dei;
			lan_wr(REW_PCP_DEI_CFG_DEI_QOS_VAL_SET(!!dei) |
			       REW_PCP_DEI_CFG_PCP_QOS_VAL_SET(pcp),
			       lan9645x,
			       REW_PCP_DEI_CFG(p->chip_port,
					       LAN9645X_PRIO_COUNT * dpl + prio));
		}
	}

	/* Setup the egress TAG PCP,DEI generation mode */
	lan_rmw(REW_TAG_CFG_TAG_PCP_CFG_SET(cfg->e_mode) |
		REW_TAG_CFG_TAG_DEI_CFG_SET(cfg->e_mode),
		REW_TAG_CFG_TAG_PCP_CFG |
		REW_TAG_CFG_TAG_DEI_CFG,
		lan9645x, REW_TAG_CFG(p->chip_port));

	err = __lan9645x_qos_setpfc(lan9645x, p->chip_port, cfg->pfc_enable);
	if (err)
		cfg->pfc_enable = p->qos.pfc_enable;

	p->qos = *cfg;

	return err;
}

int lan9645x_qos_portconf_set(struct lan9645x_port *p,
			      struct lan9645x_port_qos *cfg)
{
	int err;

	mutex_lock(&p->qos_lock);
	err = __lan9645x_qos_portconf_set(p, cfg);
	mutex_unlock(&p->qos_lock);
	return err;
}

void __lan9645x_qos_portconf_get(struct lan9645x_port *p,
				 struct lan9645x_port_qos *cfg)
{
	lockdep_assert_held(&p->qos_lock);
	*cfg = p->qos;
}

void lan9645x_qos_portconf_get(struct lan9645x_port *p,
			       struct lan9645x_port_qos *cfg)
{
	mutex_lock(&p->qos_lock);
	__lan9645x_qos_portconf_get(p, cfg);
	mutex_unlock(&p->qos_lock);
}

static int lan9645x_qos_dscp_validate(struct lan9645x *lan9645x, u8 dscp)
{
	if (dscp >= LAN9645X_DSCP_COUNT)
		return -ERANGE;

	return 0;
}

int __lan9645x_qos_dscp_conf_set(struct lan9645x *lan9645x, u8 dscp,
				 struct lan9645x_ig_dscp *cfg)
{
	int err;

	lockdep_assert_held(&lan9645x->qos_lock);

	err = lan9645x_qos_dscp_validate(lan9645x, dscp);
	if (err)
		return err;

	/* Setup switch ingress mapping between [DSCP] and [Priority]. */
	/* Setup switch ingress mapping between [DSCP] and [DPL]. */
	lan_rmw(ANA_DSCP_CFG_DP_DSCP_VAL_SET(!!cfg->dpl) |
		ANA_DSCP_CFG_QOS_DSCP_VAL_SET(cfg->prio) |
		ANA_DSCP_CFG_DSCP_TRUST_ENA_SET(!!cfg->trust),
		ANA_DSCP_CFG_DP_DSCP_VAL |
		ANA_DSCP_CFG_QOS_DSCP_VAL |
		ANA_DSCP_CFG_DSCP_TRUST_ENA,
		lan9645x, ANA_DSCP_CFG(dscp));

	lan9645x->i_dscp_map[dscp] = *cfg;
	return 0;
}

int __lan9645x_qos_dscp_conf_get(struct lan9645x *lan9645x,
				 u8 dscp,
				 struct lan9645x_ig_dscp *cfg)
{
	int err;

	lockdep_assert_held(&lan9645x->qos_lock);

	err = lan9645x_qos_dscp_validate(lan9645x, dscp);
	if (err)
		return err;

	*cfg = lan9645x->i_dscp_map[dscp];
	return 0;
}

int lan9645x_qos_init(struct lan9645x *lan9645x)
{
	struct lan9645x_port *p;
	int port, err = 0;

	mutex_init(&lan9645x->qos_lock);

	/* Init port qos state structs and apply it to HW. */
	lan9645x_for_each_port(lan9645x, port, p) {
		lan9645x_qos_port_init(p);
		err = lan9645x_qos_portconf_set(p, &p->qos);
		if (err)
			return err;
	}

	return err;
}
