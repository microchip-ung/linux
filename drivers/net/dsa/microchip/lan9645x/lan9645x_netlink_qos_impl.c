// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "linux/err.h"
#include <linux/errno.h>
#include <linux/rtnetlink.h>

#include "lan9645x_netlink_qos.h"

int lan9645x_qos_port_conf_set(struct lan9645x_netlink_qos *q,
			       struct net_device *dev,
			       struct mchp_qos_port_conf *cfg)
{
	struct lan9645x *lan9645x;
	struct lan9645x_port *p;
	u32 pcp, dei, e_mode;
	u8 prio, dpl;
	int err = 0;

	ASSERT_RTNL();

	lan9645x = q->lan9645x;

	p = lan9645x_port_from_netdev(dev);
	if (IS_ERR_OR_NULL(p))
		return -ENOTSUPP;

	dev_dbg(lan9645x->dev, "port=%d", p->chip_port);

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
	for (pcp = 0; pcp < PCP_COUNT; pcp++) {
		for (dei = 0; dei < DEI_COUNT; dei++) {
			prio = cfg->i_pcp_dei_prio_dpl_map[pcp][dei].prio;
			dpl = cfg->i_pcp_dei_prio_dpl_map[pcp][dei].dpl;
			lan_wr(ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL_SET(prio) |
			       ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL_SET(!!dpl),
			       lan9645x,
			       ANA_PCP_DEI_CFG(p->chip_port, PCP_COUNT * dei + pcp));
		}
	}

	dei = (cfg->e_mode == MCHP_E_MODE_DEFAULT ? cfg->e_default_dei : 0);

	/* Setup port egress default DEI and PCP */
	lan_rmw(REW_PORT_VLAN_CFG_PORT_DEI_SET(!!dei) |
		REW_PORT_VLAN_CFG_PORT_PCP_SET(cfg->e_default_pcp),
		REW_PORT_VLAN_CFG_PORT_DEI |
		REW_PORT_VLAN_CFG_PORT_PCP,
		lan9645x, REW_PORT_VLAN_CFG(p->chip_port));

	/* Setup port egress mapping between [Priority] and [PCP,DEI]. */
	/* Setup port egress mapping between [DPL] and [PCP,DEI]. */
	for (prio = 0; prio < PRIO_COUNT; prio++) {
		for (dpl = 0; dpl < DPL_COUNT; dpl++) {
			pcp = cfg->e_prio_dpl_pcp_dei_map[prio][dpl].pcp;
			dei = cfg->e_prio_dpl_pcp_dei_map[prio][dpl].dei;
			lan_wr(REW_PCP_DEI_CFG_DEI_QOS_VAL_SET(!!dei) |
			       REW_PCP_DEI_CFG_PCP_QOS_VAL_SET(pcp),
			       lan9645x,
			       REW_PCP_DEI_CFG(p->chip_port,
					       PRIO_COUNT * dpl + prio));
		}
	}

	/* Setup the egress TAG PCP,DEI generation mode */
	switch (cfg->e_mode) {
	case MCHP_E_MODE_DEFAULT:
		e_mode = 1; /* PORT_PCP/PORT_DEI */
		break;
	case MCHP_E_MODE_MAPPED:
		e_mode = 2; /* MAPPED */
		break;
	default:
		e_mode = 0; /* Classified PCP/DEI */
		break;
	}

	lan_rmw(REW_TAG_CFG_TAG_PCP_CFG_SET(e_mode) |
		REW_TAG_CFG_TAG_DEI_CFG_SET(e_mode),
		REW_TAG_CFG_TAG_PCP_CFG |
		REW_TAG_CFG_TAG_DEI_CFG,
		lan9645x, REW_TAG_CFG(p->chip_port));

	err = lan9645x_qos_setpfc(lan9645x, p->chip_port, cfg->pfc_enable);
	if (err)
		cfg->pfc_enable = p->qos.pfc_enable;

	q->qos_map[p->chip_port] = *cfg;

	return err;
}

int lan9645x_qos_port_conf_get(struct lan9645x_netlink_qos *q,
			       struct net_device *dev,
			       struct mchp_qos_port_conf *cfg)
{
	struct lan9645x_port *p;

	ASSERT_RTNL();

	p = lan9645x_port_from_netdev(dev);
	if (IS_ERR_OR_NULL(p))
		return -ENOTSUPP;

	dev_dbg(q->lan9645x->dev, "port=%d", p->chip_port);

	*cfg = q->qos_map[p->chip_port];

	return 0;
}

int lan9645x_qos_dscp_prio_dpl_set(struct lan9645x_netlink_qos *q, u8 dscp,
				   struct mchp_qos_dscp_prio_dpl *cfg)
{
	struct lan9645x *lan9645x = q->lan9645x;

	/* TODO: add e_dscp_map subcmd to qos-utils */
	ASSERT_RTNL();

	if (dscp >= DSCP_COUNT)
		return -ERANGE;

	dev_dbg(q->lan9645x->dev, "dscp=%u dpl=%u prio=%u trust=%u", dscp,
		cfg->dpl, cfg->prio, cfg->trust);

	/* Setup switch ingress mapping between [DSCP] and [Priority]. */
	/* Setup switch ingress mapping between [DSCP] and [DPL]. */
	lan_rmw(ANA_DSCP_CFG_DP_DSCP_VAL_SET(!!cfg->dpl) |
		ANA_DSCP_CFG_QOS_DSCP_VAL_SET(cfg->prio) |
		ANA_DSCP_CFG_DSCP_TRUST_ENA_SET(!!cfg->trust),
		ANA_DSCP_CFG_DP_DSCP_VAL |
		ANA_DSCP_CFG_QOS_DSCP_VAL |
		ANA_DSCP_CFG_DSCP_TRUST_ENA,
		lan9645x, ANA_DSCP_CFG(dscp));

	q->dscp_map[dscp] = *cfg;
	return 0;
}

int lan9645x_qos_dscp_prio_dpl_get(struct lan9645x_netlink_qos *q, u8 dscp,
				   struct mchp_qos_dscp_prio_dpl *cfg)
{
	ASSERT_RTNL();

	if (dscp >= DSCP_COUNT)
		return -ERANGE;

	*cfg = q->dscp_map[dscp];
	dev_dbg(q->lan9645x->dev, "dscp=%u dpl=%u prio=%u trust=%u", dscp,
		cfg->dpl, cfg->prio, cfg->trust);
	return 0;
}
