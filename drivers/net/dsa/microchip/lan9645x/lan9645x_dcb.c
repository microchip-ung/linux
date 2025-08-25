// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

int lan9645x_dcb_port_get_default_prio(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct lan9645x_port_qos cfg;

	lan9645x_qos_portconf_get(p, &cfg);

	return cfg.i_default_prio;
}

int lan9645x_dcb_port_set_default_prio(struct lan9645x *lan9645x, int port,
				       u8 prio)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct lan9645x_port_qos cfg;
	int err;

	mutex_lock(&p->qos_lock);
	__lan9645x_qos_portconf_get(p, &cfg);
	cfg.i_default_prio = prio;
	err = __lan9645x_qos_portconf_set(p, &cfg);
	mutex_unlock(&p->qos_lock);
	return err;
}

int lan9645x_dcb_port_get_dscp_prio(struct lan9645x *lan9645x, int port,
				    u8 dscp)
{
	struct lan9645x_ig_dscp cfg;

	mutex_lock(&lan9645x->qos_lock);
	__lan9645x_qos_dscp_conf_get(lan9645x, dscp, &cfg);
	mutex_unlock(&lan9645x->qos_lock);

	if (!cfg.trust)
		return -EOPNOTSUPP;

	return cfg.prio;
}

int lan9645x_dcb_add_dscp_prio(struct lan9645x *lan9645x,
			       u8 dscp, u8 prio)
{
	struct lan9645x_ig_dscp cfg;
	int err;

	if (prio >= LAN9645X_NUM_TC)
		return -ERANGE;

	mutex_lock(&lan9645x->qos_lock);
	__lan9645x_qos_dscp_conf_get(lan9645x, dscp, &cfg);
	cfg.prio = prio;
	cfg.trust = true;
	err = __lan9645x_qos_dscp_conf_set(lan9645x, dscp, &cfg);
	mutex_unlock(&lan9645x->qos_lock);

	return err;
}

int lan9645x_dcb_del_dscp_prio(struct lan9645x *lan9645x,
			       u8 dscp, u8 prio)
{
	struct lan9645x_ig_dscp cfg;
	int err;

	if (prio >= LAN9645X_NUM_TC)
		return -ERANGE;

	mutex_lock(&lan9645x->qos_lock);
	__lan9645x_qos_dscp_conf_get(lan9645x, dscp, &cfg);

	/* During a dcb app replace command, the new app table entry will be
	 * added first, then the old one will be deleted. But the hardware only
	 * supports one QoS class per DSCP value (duh), so if we blindly delete
	 * the app table entry for this DSCP value, we end up deleting the
	 * entry with the new priority. Avoid that by checking whether user
	 * space wants to delete the priority which is currently configured, or
	 * something else which is no longer current.
	 */
	if (cfg.prio != prio) {
		err = 0;
		goto unlock;
	}

	cfg.dpl = 0;
	cfg.prio = 0;
	cfg.trust = false;

	err = __lan9645x_qos_dscp_conf_set(lan9645x, dscp, &cfg);

unlock:
	mutex_unlock(&lan9645x->qos_lock);
	return err;
}

int lan9645x_dcb_port_set_apptrust(struct lan9645x *lan9645x, int port,
				   const u8 *sel, int nsel)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	bool trust_dscp = false, trust_pcp = false;
	struct lan9645x_port_qos cfg;
	int err;

	/* We can either do PCP, DSCP or DSCP,PCP (this order) */
	switch (nsel) {
	case 0:
		break;
	case 1:
		if (!(sel[0] == DCB_APP_SEL_PCP ||
		      sel[0] == IEEE_8021QAZ_APP_SEL_DSCP))
			return -EOPNOTSUPP;

		break;
	case 2:
		/* DSCP always takes priority over PCP in hw */
		if (sel[0] != IEEE_8021QAZ_APP_SEL_DSCP ||
		    sel[1] != DCB_APP_SEL_PCP)
			return -EOPNOTSUPP;
		break;
	default:
		return -EOPNOTSUPP;
	}

	for (int i = 0; i < nsel; i++) {
		switch (sel[i]) {
		case DCB_APP_SEL_PCP:
			trust_pcp = true;
			break;
		case IEEE_8021QAZ_APP_SEL_DSCP:
			trust_dscp = true;
			break;
		default:
			continue;
		}
	}

	mutex_lock(&p->qos_lock);
	__lan9645x_qos_portconf_get(p, &cfg);
	cfg.i_mode.tag_map_enable = trust_pcp;
	cfg.i_mode.dscp_map_enable = trust_dscp;
	err = __lan9645x_qos_portconf_set(p, &cfg);
	mutex_unlock(&p->qos_lock);

	return err;
}

int lan9645x_dcb_port_get_apptrust(struct lan9645x *lan9645x, int port, u8 *sel,
				   int *nsel)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct lan9645x_port_qos cfg;

	*nsel = 0;

	mutex_lock(&p->qos_lock);
	__lan9645x_qos_portconf_get(p, &cfg);
	mutex_unlock(&p->qos_lock);

	if (cfg.i_mode.dscp_map_enable)
		sel[(*nsel)++] = IEEE_8021QAZ_APP_SEL_DSCP;

	if (cfg.i_mode.tag_map_enable)
		sel[(*nsel)++] = DCB_APP_SEL_PCP;

	return 0;
}

int lan9645x_dcb_getpfc(struct lan9645x *lan9645x, int port,
			struct ieee_pfc *pfc)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	mutex_lock(&p->qos_lock);
	pfc->pfc_en = p->qos.pfc_enable;
	pfc->pfc_cap = LAN9645X_NUM_TC;
	mutex_unlock(&p->qos_lock);

	return 0;
}

int lan9645x_dcb_setpfc(struct lan9645x *lan9645x, int port, u8 pfc_enable)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct lan9645x_port_qos cfg;
	int err;

	mutex_lock(&p->qos_lock);
	__lan9645x_qos_portconf_get(p, &cfg);
	cfg.pfc_enable = pfc_enable;
	err = __lan9645x_qos_portconf_set(p, &cfg);
	mutex_unlock(&p->qos_lock);
	return err;
}

int lan9645x_dcb_get_pcp_dei_prio(struct lan9645x *lan9645x, int port, u8 pcp,
				  u8 dei)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct lan9645x_port_qos cfg;

	lan9645x_qos_portconf_get(p, &cfg);

	return cfg.i_map[pcp][dei].prio;
}

int lan9645x_dcb_add_pcp_dei_prio(struct lan9645x *lan9645x, int port, u8 pcp,
				  u8 dei, u8 prio)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct lan9645x_port_qos cfg;
	int err;

	mutex_lock(&p->qos_lock);
	__lan9645x_qos_portconf_get(p, &cfg);

	/* dcbnl can only map to prio, but HW has prio/dpl. We silently map
	 * dei -> dpl in a 1:1 manner, even though the user did not and can not
	 * request it.
	 */
	cfg.i_map[pcp][dei].prio = prio;
	cfg.i_map[pcp][dei].dpl = dei;
	err = __lan9645x_qos_portconf_set(p, &cfg);
	mutex_unlock(&p->qos_lock);

	return err;
}

int lan9645x_dcb_del_pcp_dei_prio(struct lan9645x *lan9645x, int port, u8 pcp,
				  u8 dei, u8 prio)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	struct lan9645x_port_qos cfg;
	int err = 0;

	mutex_lock(&p->qos_lock);
	__lan9645x_qos_portconf_get(p, &cfg);

	if (cfg.i_map[pcp][dei].prio != prio)
		goto unlock;

	cfg.i_map[pcp][dei].prio = 0;
	cfg.i_map[pcp][dei].dpl = 0;
	err = __lan9645x_qos_portconf_set(p, &cfg);

unlock:
	mutex_unlock(&p->qos_lock);
	return err;
}
