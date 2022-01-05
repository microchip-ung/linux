// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/netdevice.h>

#include "lan966x_main.h"

static void lan966x_get_pauseparam(struct net_device *dev,
				   struct ethtool_pauseparam *param)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	u32 val;

	val = lan_rd(lan966x, SYS_MAC_FC_CFG(port->chip_port));

	param->tx_pause = !!(SYS_MAC_FC_CFG_TX_FC_ENA_SET(1) & val);
	param->rx_pause = !!(SYS_MAC_FC_CFG_RX_FC_ENA_SET(1) & val);
	param->autoneg = 0;
}

static int lan966x_set_pauseparam(struct net_device *dev,
				  struct ethtool_pauseparam *param)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	u32 val;

	val = lan_rd(lan966x, ANA_PFC_CFG(port->chip_port));
	if (ANA_PFC_CFG_RX_PFC_ENA_GET(val) != 0) {
		netdev_err(port->dev, "802.3X FC and 802.1Qbb PFC cannot both be enabled\n");
		return -EOPNOTSUPP;
	}

	return phylink_ethtool_set_pauseparam(port->phylink, param);
}

static void lan966x_get_strings(struct net_device *netdev, u32 sset, u8 *data)
{
	struct lan966x_port *port = netdev_priv(netdev);
	struct lan966x *lan966x = port->lan966x;
	int i;

	if (sset != ETH_SS_STATS)
		return;

	for (i = 0; i < lan966x->num_stats; i++)
		memcpy(data + i * ETH_GSTRING_LEN,
		       lan966x->stats_layout[i].name, ETH_GSTRING_LEN);
}

static void lan966x_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	int i;

	/* check and update now */
	lan966x_update_stats(lan966x);

	/* Copy all counters */
	for (i = 0; i < lan966x->num_stats; i++)
		*data++ = lan966x->stats[port->chip_port *
					 lan966x->num_stats + i];
}

static int lan966x_get_sset_count(struct net_device *dev, int sset)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;

	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	return lan966x->num_stats;
}

static int lan966x_get_ts_info(struct net_device *dev,
			       struct ethtool_ts_info *info)
{
	struct lan966x_port *lan966x_port = netdev_priv(dev);
	struct lan966x *lan966x = lan966x_port->lan966x;

	info->phc_index = lan966x->ptp_domain[LAN966X_PTP_PORT_DOMAIN].clock ?
			  ptp_clock_index(lan966x->ptp_domain[LAN966X_PTP_PORT_DOMAIN].clock) : -1;
	info->so_timestamping |= SOF_TIMESTAMPING_TX_SOFTWARE |
				 SOF_TIMESTAMPING_RX_SOFTWARE |
				 SOF_TIMESTAMPING_SOFTWARE |
				 SOF_TIMESTAMPING_TX_HARDWARE |
				 SOF_TIMESTAMPING_RX_HARDWARE |
				 SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON) |
			 BIT(HWTSTAMP_TX_ONESTEP_SYNC);
	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL);

	return 0;
}

static int lan966x_get_eee(struct net_device *dev, struct ethtool_eee *eee)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct phylink *phylink = port->phylink;
	u32 val;
	int ret;

	if (!phylink)
		return -EIO;

	ret = phylink_ethtool_get_eee(phylink, eee);
	if (ret < 0)
		return ret;

	val = lan_rd(lan966x, DEV_EEE_CFG(port->chip_port));
	if (DEV_EEE_CFG_EEE_ENA_GET(val)) {
		eee->eee_enabled = true;
		eee->eee_active = !!(eee->advertised & eee->lp_advertised);
		eee->tx_lpi_enabled = true;

		eee->tx_lpi_timer = DEV_EEE_CFG_EEE_TIMER_WAKEUP_GET(val);
	} else {
		eee->eee_enabled = false;
		eee->eee_active = false;
		eee->tx_lpi_enabled = false;
		eee->tx_lpi_timer = 0;
	}

	return 0;
}

static int lan966x_set_eee(struct net_device *dev, struct ethtool_eee *eee)
{
	struct lan966x_port *port;
	struct phylink *phylink;
	struct lan966x *lan966x;
	int ret;

	if (!dev)
		return -EINVAL;

	port = netdev_priv(dev);
	if (!port)
		return -EINVAL;

	lan966x = port->lan966x;
	phylink = port->phylink;

	if (!phylink)
		return -EIO;

	if (eee->eee_enabled) {
		ret = phylink_init_eee(phylink, 0);
		if (ret)
			return ret;

		lan_rmw(DEV_EEE_CFG_EEE_ENA_SET(1) |
			DEV_EEE_CFG_EEE_TIMER_WAKEUP_SET(eee->tx_lpi_timer),
			DEV_EEE_CFG_EEE_ENA |
			DEV_EEE_CFG_EEE_TIMER_WAKEUP,
			lan966x, DEV_EEE_CFG(port->chip_port));
	} else {
		lan_rmw(DEV_EEE_CFG_EEE_ENA_SET(0),
			DEV_EEE_CFG_EEE_ENA,
			lan966x, DEV_EEE_CFG(port->chip_port));
	}

	return 0;
}

static int lan966x_get_link_ksettings(struct net_device *ndev,
				      struct ethtool_link_ksettings *cmd)
{
	struct lan966x_port *port = netdev_priv(ndev);

	return phylink_ethtool_ksettings_get(port->phylink, cmd);
}

static int lan966x_set_link_ksettings(struct net_device *ndev,
				      const struct ethtool_link_ksettings *cmd)
{
	struct lan966x_port *port = netdev_priv(ndev);

	return phylink_ethtool_ksettings_set(port->phylink, cmd);
}

const struct ethtool_ops lan966x_ethtool_ops = {
	.get_link_ksettings	= lan966x_get_link_ksettings,
	.set_link_ksettings	= lan966x_set_link_ksettings,
	.set_pauseparam		= lan966x_set_pauseparam,
	.get_pauseparam		= lan966x_get_pauseparam,
	.get_strings		= lan966x_get_strings,
	.get_ethtool_stats	= lan966x_get_ethtool_stats,
	.get_sset_count		= lan966x_get_sset_count,
	.get_link		= ethtool_op_get_link,
	.get_ts_info		= lan966x_get_ts_info,
	.get_eee		= lan966x_get_eee,
	.set_eee		= lan966x_set_eee,
};
