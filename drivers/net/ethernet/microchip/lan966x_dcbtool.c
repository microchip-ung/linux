// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2021 Microchip Technology Inc. */

#include <linux/netdevice.h>
#include <linux/types.h>
#include <net/dcbnl.h>

#include "lan966x_main.h"

#define DCBX_MAX_APP_PROTOCOL		1
#define DCBX_CAPS		(DCB_CAP_DCBX_LLD_MANAGED | \
				DCB_CAP_DCBX_VER_CEE | DCB_CAP_DCBX_STATIC)

static int lan966x_ieee_getpfc(struct net_device *dev, struct ieee_pfc *ieee_pfc)
{
	struct mchp_qos_port_conf cfg = {};
	struct lan966x_port *port;
	u32 err;

	if (!dev)
		return -EINVAL;

	port = netdev_priv(dev);
	if (!port)
		return -EINVAL;

	err = lan966x_qos_port_conf_get(port, &cfg);
	if (err)
		return -EINVAL;

	ieee_pfc->pfc_en = cfg.pfc_enable;
	ieee_pfc->pfc_cap = 8;

	return 0;
}

static int lan966x_ieee_setpfc(struct net_device *dev, struct ieee_pfc *ieee_pfc)
{
	struct mchp_qos_port_conf cfg = {};
	struct lan966x_port *port;
	u32 err;

	if (!dev)
		return -EINVAL;

	port = netdev_priv(dev);
	if (!port)
		return -EINVAL;

	err = lan966x_qos_port_conf_get(port, &cfg);
	if (err)
		return -EINVAL;

	cfg.pfc_enable = ieee_pfc->pfc_en;

	return lan966x_qos_port_conf_set(port, &cfg);
}

static u8 lan966x_getdcbx(struct net_device *dev)
{
	return DCB_CAP_DCBX_VER_CEE | DCB_CAP_DCBX_HOST | DCB_CAP_DCBX_VER_IEEE;
}

static u8 lan966x_setdcbx(struct net_device *dev, u8 mode)
{
	/* Nothing to do here */
	return 0;
}

const struct dcbnl_rtnl_ops lan966x_dcbnl_ops = {
	.ieee_getpfc = lan966x_ieee_getpfc,
	.ieee_setpfc = lan966x_ieee_setpfc,
	.getdcbx = lan966x_getdcbx,
	.setdcbx = lan966x_setdcbx,
};
