// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/phy.h>
#include <linux/phy/phy.h>

#include "lan9645x_main.h"

void lan9645x_phylink_get_caps(struct lan9645x *lan9645x, int port,
			       struct phylink_config *c)
{
	dev_dbg(lan9645x->dev, "port=%d\n", port);

	c->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE | MAC_10 |
			      MAC_100 | MAC_1000FD | MAC_25000FD;

	switch (port) {
	case 0 ... 3:
		__set_bit(PHY_INTERFACE_MODE_GMII, c->supported_interfaces);
		break;
	case 4:
		__set_bit(PHY_INTERFACE_MODE_GMII, c->supported_interfaces);
		phy_interface_set_rgmii(c->supported_interfaces);
		break;
	case 5 ... 6:
		__set_bit(PHY_INTERFACE_MODE_QSGMII, c->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX, c->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, c->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_SGMII, c->supported_interfaces);
		break;
	case 7 ... 8:
		__set_bit(PHY_INTERFACE_MODE_QSGMII, c->supported_interfaces);
		phy_interface_set_rgmii(c->supported_interfaces);
		break;
	default:
		break;
	}
}

void lan9645x_phylink_mac_config(struct lan9645x *lan9645x, int port,
				 unsigned int mode,
				 const struct phylink_link_state *state)
{
	struct lan9645x_port *p = lan9645x->ports[port];

	dev_dbg(lan9645x->dev, "%s: port=%d mode=%d interface=%d\n", __func__, port, mode, state->interface);

	if (p->serdes) {
		if (phy_set_mode_ext(p->serdes, PHY_MODE_ETHERNET, state->interface)) {
			dev_err(lan9645x->dev,
				"Could not set mode of serdes port=%d mode=%u",
				port, state->interface);
		}
	}
}

static int lan9645x_port_is_cuphy(struct lan9645x *lan9645x, int port,
				  phy_interface_t interface)
{
	return 0 <= port && port <= 4 && interface == PHY_INTERFACE_MODE_GMII;
}

void lan9645x_phylink_mac_link_up(struct lan9645x *lan9645x, int port,
				  unsigned int link_an_mode,
				  phy_interface_t interface,
				  struct phy_device *phydev, int speed,
				  int duplex, bool tx_pause, bool rx_pause)
{
	struct lan9645x_port *p = lan9645x->ports[port];
	int rx_ifg1, rx_ifg2, tx_ifg, gtx_clk = 0;
	struct lan9645x_path_delay *path_delay;
	int gspeed = LAN9645X_SPEED_DISABLED;
	u8 tweaks = 5;
	int mode = 0;
	int fc_spd;

	dev_dbg(lan9645x->dev,
		"port=%d link_an_mode=%u interface=%u speed=%d duplex=%d tx_pause=%u rx_pause=%u\n",
		port, link_an_mode, interface, speed, duplex, tx_pause,
		rx_pause);

	if (phy_interface_mode_is_rgmii(interface) && p->serdes)
		phy_set_speed(p->serdes, speed);

	if (duplex == DUPLEX_FULL) {
		mode |= DEV_MAC_MODE_CFG_FDX_ENA_SET(1);
		/* These magic IFG constants do not match datasheet (datasheet is
		 * wrong). The correct values are found by HW validation after
		 * stream out.
		 */
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x5);

	} else {
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x6);
	}

	switch (speed) {
	case SPEED_10:
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x2);
		gspeed = LAN9645X_SPEED_10;
		break;
	case SPEED_100:
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x1);
		gspeed = LAN9645X_SPEED_100;
		break;
	case SPEED_1000:
		gspeed = LAN9645X_SPEED_1000;
		mode |= DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);
		mode |= DEV_MAC_MODE_CFG_FDX_ENA_SET(1);
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x1);
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x6);
		gtx_clk = 1;
		break;
	case SPEED_2500:
		gspeed = LAN9645X_SPEED_2500;
		mode |= DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);
		mode |= DEV_MAC_MODE_CFG_FDX_ENA_SET(1);
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x1);
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x6);
		break;
	default:
		dev_err(lan9645x->dev, "Unsupported speed on port %d: %d\n",
			p->chip_port, speed);
		return;
	}

	p->speed = gspeed;
	fc_spd = lan9645x_speed_fc_enc(p->speed);

	/* TODO: add taprio speed set */

	if (phy_interface_num_ports(interface) == 4 ||
	    interface == PHY_INTERFACE_MODE_SGMII)
		mode |= DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);

	lan_rmw(mode,
		DEV_MAC_MODE_CFG_FDX_ENA |
		DEV_MAC_MODE_CFG_GIGA_MODE_ENA,
		lan9645x, DEV_MAC_MODE_CFG(p->chip_port));

	lan_rmw(tx_ifg | rx_ifg1 | rx_ifg2,
		DEV_MAC_IFG_CFG_TX_IFG |
		DEV_MAC_IFG_CFG_RX_IFG1 |
		DEV_MAC_IFG_CFG_RX_IFG2,
		lan9645x, DEV_MAC_IFG_CFG(p->chip_port));

	lan_rmw(DEV_MAC_HDX_CFG_SEED_SET(p->chip_port) |
		DEV_MAC_HDX_CFG_SEED_LOAD_SET(1),
		DEV_MAC_HDX_CFG_SEED |
		DEV_MAC_HDX_CFG_SEED_LOAD, lan9645x,
		DEV_MAC_HDX_CFG(p->chip_port));

	if (lan9645x_port_is_cuphy(lan9645x, port, interface)) {
		lan_rmw(CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA_SET(gtx_clk),
			CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA, lan9645x,
			CHIP_TOP_CUPHY_PORT_CFG(p->chip_port));
	}

	lan_rmw(DEV_MAC_HDX_CFG_SEED_LOAD_SET(0),
		DEV_MAC_HDX_CFG_SEED_LOAD, lan9645x,
		DEV_MAC_HDX_CFG(p->chip_port));

	if (rx_pause || tx_pause) {
		if (p->qos.pfc_enable)
			dev_info(lan9645x->dev,
				 "802.3X FC and 802.1Qbb PFC cannot both be enabled on port=%d, disabling 802.1Qbb PFC.",
				 port);

		p->qos.pfc_enable = 0;
	}

	/* Set PFC link speed and enable map */
	lan_rmw(ANA_PFC_CFG_FC_LINK_SPEED_SET(fc_spd) |
		ANA_PFC_CFG_RX_PFC_ENA_SET(p->qos.pfc_enable),
		ANA_PFC_CFG_FC_LINK_SPEED |
		ANA_PFC_CFG_RX_PFC_ENA,
		lan9645x, ANA_PFC_CFG(p->chip_port));

	lan_rmw(DEV_PCS1G_CFG_PCS_ENA_SET(1),
		DEV_PCS1G_CFG_PCS_ENA, lan9645x,
		DEV_PCS1G_CFG(p->chip_port));

	lan_rmw(DEV_PCS1G_SD_CFG_SD_ENA_SET(0),
		DEV_PCS1G_SD_CFG_SD_ENA,
		lan9645x, DEV_PCS1G_SD_CFG(p->chip_port));

	lan_rmw(SYS_PAUSE_CFG_PAUSE_ENA_SET(1),
		SYS_PAUSE_CFG_PAUSE_ENA,
		lan9645x, SYS_PAUSE_CFG(p->chip_port));

	/* Set SMAC of Pause frame (00:00:00:00:00:00) */
	lan_wr(0, lan9645x, DEV_FC_MAC_LOW_CFG(p->chip_port));
	lan_wr(0, lan9645x, DEV_FC_MAC_HIGH_CFG(p->chip_port));

	/* Flow control */
	lan_rmw(SYS_MAC_FC_CFG_FC_LINK_SPEED_SET(fc_spd) |
		SYS_MAC_FC_CFG_FC_LATENCY_CFG_SET(0x7) |
		SYS_MAC_FC_CFG_ZERO_PAUSE_ENA_SET(1) |
		SYS_MAC_FC_CFG_PAUSE_VAL_CFG_SET(0xffff) |
		SYS_MAC_FC_CFG_RX_FC_ENA_SET(rx_pause ? 1 : 0) |
		SYS_MAC_FC_CFG_TX_FC_ENA_SET(tx_pause ? 1 : 0),
		SYS_MAC_FC_CFG_FC_LINK_SPEED |
		SYS_MAC_FC_CFG_FC_LATENCY_CFG |
		SYS_MAC_FC_CFG_ZERO_PAUSE_ENA |
		SYS_MAC_FC_CFG_PAUSE_VAL_CFG |
		SYS_MAC_FC_CFG_RX_FC_ENA |
		SYS_MAC_FC_CFG_TX_FC_ENA,
		lan9645x, SYS_MAC_FC_CFG(p->chip_port));

	list_for_each_entry(path_delay, &p->path_delays, list) {
		if (path_delay->speed == speed) {
			lan_wr(path_delay->rx_delay + p->rx_delay,
			       lan9645x, SYS_PTP_RXDLY_CFG(p->chip_port));
			lan_wr(path_delay->tx_delay,
			       lan9645x, SYS_PTP_TXDLY_CFG(p->chip_port));
		}
	}

	/* Enable MAC module */
	lan_wr(DEV_MAC_ENA_CFG_RX_ENA_SET(1) |
	       DEV_MAC_ENA_CFG_TX_ENA_SET(1),
	       lan9645x, DEV_MAC_ENA_CFG(p->chip_port));

	/* port _must_ be taken out of reset before MAC. */
	lan_rmw(DEV_CLOCK_CFG_PORT_RST_SET(0),
		DEV_CLOCK_CFG_PORT_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	/* Take out the clock from reset. Note this write will set all these
	 * fields to zero:
	 *
	 * DEV_CLOCK_CFG[*].MAC_TX_RST
	 * DEV_CLOCK_CFG[*].MAC_RX_RST
	 * DEV_CLOCK_CFG[*].PCS_TX_RST
	 * DEV_CLOCK_CFG[*].PCS_RX_RST
	 * DEV_CLOCK_CFG[*].PORT_RST
	 * DEV_CLOCK_CFG[*].PHY_RST
	 *
	 * Note link_down will assert PORT_RST, MAC_RX_RST and MAC_TX_RST, so
	 * we are effectively taking the mac tx/rx clocks out of reset.
	 *
	 * This linkspeed field has a slightly different encoding from others:
	 *
	 * - 0 is no-link
	 * - 1 is both 2500/1000
	 * - 2 is 100mbit
	 * - 3 is 10mbit
	 *
	 */
	lan_wr(DEV_CLOCK_CFG_LINK_SPEED_SET(fc_spd == 0 ? 1 : fc_spd),
	       lan9645x,
	       DEV_CLOCK_CFG(p->chip_port));

	mutex_lock(&lan9645x->fwd_domain_lock);
	lan9645x_cut_through_fwd(lan9645x);
	mutex_unlock(&lan9645x->fwd_domain_lock);

	/* Enable phase detector */
	/* When running at 10 these tweaks need to be set */
	if (gspeed == LAN9645X_SPEED_10)
		tweaks = 7;
	else
		tweaks = 5;
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
		DEV_PHAD_CTRL_TWEAKS_SET(tweaks) |
		DEV_PHAD_CTRL_PHAD_FAILED_SET(1) |
		DEV_PHAD_CTRL_LOCK_ACC_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA |
		DEV_PHAD_CTRL_TWEAKS |
		DEV_PHAD_CTRL_PHAD_FAILED |
		DEV_PHAD_CTRL_LOCK_ACC,
		lan9645x, DEV_PHAD_CTRL(p->chip_port, 0));

	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(1) |
		DEV_PHAD_CTRL_TWEAKS_SET(tweaks) |
		DEV_PHAD_CTRL_PHAD_FAILED_SET(1) |
		DEV_PHAD_CTRL_LOCK_ACC_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA |
		DEV_PHAD_CTRL_TWEAKS |
		DEV_PHAD_CTRL_PHAD_FAILED |
		DEV_PHAD_CTRL_LOCK_ACC,
		lan9645x, DEV_PHAD_CTRL(p->chip_port, 1));

	/* Core: Enable port for frame transfer */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1) |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG_SET(1) |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE_SET(1) |
		QSYS_SW_PORT_MODE_TX_PFC_ENA_SET(p->qos.pfc_enable),
		QSYS_SW_PORT_MODE_PORT_ENA |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE |
		QSYS_SW_PORT_MODE_TX_PFC_ENA,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	lan_rmw(AFI_PORT_CFG_FC_SKIP_TTI_INJ_SET(0) |
		AFI_PORT_CFG_FRM_OUT_MAX_SET(16),
		AFI_PORT_CFG_FC_SKIP_TTI_INJ |
		AFI_PORT_CFG_FRM_OUT_MAX,
		lan9645x, AFI_PORT_CFG(p->chip_port));
}

void lan9645x_phylink_port_down(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x->ports[port];
	u32 val, delay = 0;

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	/* 0.5: Disable any AFI */
	lan_rmw(AFI_PORT_CFG_FC_SKIP_TTI_INJ_SET(1) |
		AFI_PORT_CFG_FRM_OUT_MAX_SET(0),
		AFI_PORT_CFG_FC_SKIP_TTI_INJ |
		AFI_PORT_CFG_FRM_OUT_MAX,
		lan9645x, AFI_PORT_CFG(p->chip_port));

	/* wait for reg afi_port_frm_out to become 0 for the port */
	while (true) {
		val = lan_rd(lan9645x, AFI_PORT_FRM_OUT(p->chip_port));
		if (!AFI_PORT_FRM_OUT_FRM_OUT_CNT_GET(val))
			break;

		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		delay++;
		if (delay == 2000) {
			dev_err(lan9645x->dev, "AFI timeout chip port %u",
				p->chip_port);
			break;
		}
	}

	delay = 0;

	/* 2: Disable MAC frame reception */
	lan_rmw(DEV_MAC_ENA_CFG_RX_ENA_SET(0),
		DEV_MAC_ENA_CFG_RX_ENA,
		lan9645x, DEV_MAC_ENA_CFG(p->chip_port));

	/* 1: Reset the PCS Rx clock domain  */
	lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(1),
		DEV_CLOCK_CFG_PCS_RX_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	mutex_lock(&lan9645x->fwd_domain_lock);
	p->speed = LAN9645X_SPEED_DISABLED;
	lan9645x_cut_through_fwd(lan9645x);
	mutex_unlock(&lan9645x->fwd_domain_lock);

	/* 3: Disable traffic being sent to or from switch port */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(0),
		QSYS_SW_PORT_MODE_PORT_ENA,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	/* 4: Disable dequeuing from the egress queues  */
	lan_rmw(QSYS_PORT_MODE_DEQUEUE_DIS_SET(1),
		QSYS_PORT_MODE_DEQUEUE_DIS,
		lan9645x, QSYS_PORT_MODE(p->chip_port));

	/* 5: Disable Flowcontrol */
	lan_rmw(SYS_PAUSE_CFG_PAUSE_ENA_SET(0),
		SYS_PAUSE_CFG_PAUSE_ENA,
		lan9645x, SYS_PAUSE_CFG(p->chip_port));

	/* 5.1: Disable PFC */
	lan_rmw(QSYS_SW_PORT_MODE_TX_PFC_ENA_SET(0),
		QSYS_SW_PORT_MODE_TX_PFC_ENA,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	/* 6: Wait a worst case time 8ms (10K jumbo/10Mbit) */
	usleep_range(8 * USEC_PER_MSEC, 9 * USEC_PER_MSEC);

	/* 7: Disable HDX backpressure. */
	lan_rmw(SYS_FRONT_PORT_MODE_HDX_MODE_SET(0),
		SYS_FRONT_PORT_MODE_HDX_MODE,
		lan9645x, SYS_FRONT_PORT_MODE(p->chip_port));

	/* 8: Flush the queues accociated with the port */
	lan_rmw(QSYS_SW_PORT_MODE_AGING_MODE_SET(3),
		QSYS_SW_PORT_MODE_AGING_MODE,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	/* 9: Enable dequeuing from the egress queues */
	lan_rmw(QSYS_PORT_MODE_DEQUEUE_DIS_SET(0),
		QSYS_PORT_MODE_DEQUEUE_DIS,
		lan9645x, QSYS_PORT_MODE(p->chip_port));

	/* 10: Wait until flushing is complete */
	while (true) {
		val = lan_rd(lan9645x, QSYS_SW_STATUS(p->chip_port));
		if (!QSYS_SW_STATUS_EQ_AVAIL_GET(val))
			break;

		usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		delay++;
		if (delay >= 2000) {
			dev_err(lan9645x->dev, "Flush timeout chip port %u", port);
			break;
		}
	}

	/* 11: Disable MAC tx */
	lan_rmw(DEV_MAC_ENA_CFG_TX_ENA_SET(0),
		DEV_MAC_ENA_CFG_TX_ENA,
		lan9645x, DEV_MAC_ENA_CFG(p->chip_port));

	/* 12: Reset the Port and MAC clock domains */
	lan_rmw(DEV_CLOCK_CFG_PORT_RST_SET(1),
		DEV_CLOCK_CFG_PORT_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	lan_rmw(DEV_CLOCK_CFG_MAC_TX_RST_SET(1) |
		DEV_CLOCK_CFG_MAC_RX_RST_SET(1) |
		DEV_CLOCK_CFG_PORT_RST_SET(1),
		DEV_CLOCK_CFG_MAC_TX_RST |
		DEV_CLOCK_CFG_MAC_RX_RST |
		DEV_CLOCK_CFG_PORT_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	/* 13: Clear flushing */
	lan_rmw(QSYS_SW_PORT_MODE_AGING_MODE_SET(2),
		QSYS_SW_PORT_MODE_AGING_MODE,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));
}

void lan9645x_phylink_mac_link_down(struct lan9645x *lan9645x, int port,
				    unsigned int link_an_mode,
				    phy_interface_t interface)
{
	struct lan9645x_port *p = lan9645x->ports[port];

	dev_dbg(lan9645x->dev, "port=%d link_an_mode=%u interface=%u\n", port,
		link_an_mode, interface);

	lan9645x_phylink_port_down(lan9645x, port);

	/* 14: Take PCS out of reset */
	lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(0) |
		DEV_CLOCK_CFG_PCS_TX_RST_SET(0),
		DEV_CLOCK_CFG_PCS_RX_RST |
		DEV_CLOCK_CFG_PCS_TX_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));
}

struct phylink_pcs *lan9645x_phylink_mac_select_pcs(struct lan9645x *lan9645x,
						    int port,
						    phy_interface_t iface)
{
	struct lan9645x_port *p;

	dev_dbg(lan9645x->dev, "port=%d phy_iface=%d\n", port, iface);

	p = lan9645x_to_port(lan9645x, port);
	if (!p)
		return NULL;

	return &p->phylink_pcs;
}

void lan9645x_pcs_aneg_restart(struct phylink_pcs *pcs)
{
	/* Currently not used */
}

int lan9645x_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			phy_interface_t interface,
			const unsigned long *advertising,
			bool permit_pause_to_mac)
{
	struct lan9645x_port *p =
		container_of(pcs, struct lan9645x_port, phylink_pcs);
	struct lan9645x *lan9645x = p->lan9645x;
	bool full_preamble = false;
	bool inband_aneg = false;
	bool outband;

	dev_dbg(lan9645x->dev, "interface=%d permit_pause_to_mac=%d\n", interface,
		permit_pause_to_mac);

	if (interface == PHY_INTERFACE_MODE_QUSGMII)
		full_preamble = true;

	if (neg_mode & PHYLINK_PCS_NEG_INBAND) {
		if (interface == PHY_INTERFACE_MODE_SGMII ||
		    phy_interface_num_ports(interface) == 4)
			inband_aneg = true; /* Cisco-SGMII in-band-aneg */
		else if (interface == PHY_INTERFACE_MODE_1000BASEX &&
			 neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
			inband_aneg = true; /* Clause-37 in-band-aneg */

		outband = false;
	} else {
		outband = true;
	}

	/* Disable or enable inband.
	 * For QUSGMII, we rely on the preamble to transmit data such as
	 * timestamps, therefore force full preamble transmission, and prevent
	 * premable shortening
	 */
	lan_rmw(DEV_PCS1G_MODE_CFG_SGMII_MODE_ENA_SET(outband) |
		DEV_PCS1G_MODE_CFG_SAVE_PREAMBLE_ENA_SET(full_preamble),
		DEV_PCS1G_MODE_CFG_SGMII_MODE_ENA |
		DEV_PCS1G_MODE_CFG_SAVE_PREAMBLE_ENA,
		lan9645x, DEV_PCS1G_MODE_CFG(p->chip_port));

	/* Enable PCS */
	lan_wr(DEV_PCS1G_CFG_PCS_ENA_SET(1), lan9645x,
	       DEV_PCS1G_CFG(p->chip_port));

	if (inband_aneg) {
		int adv = phylink_mii_c22_pcs_encode_advertisement(interface,
								   advertising);
		if (adv >= 0)
			/* Enable in-band aneg */
			lan_wr(DEV_PCS1G_ANEG_CFG_ADV_ABILITY_SET(adv) |
			       DEV_PCS1G_ANEG_CFG_SW_RESOLVE_ENA_SET(1) |
			       DEV_PCS1G_ANEG_CFG_ENA_SET(1) |
			       DEV_PCS1G_ANEG_CFG_RESTART_ONE_SHOT_SET(1),
			       lan9645x, DEV_PCS1G_ANEG_CFG(p->chip_port));
	} else {
		lan_wr(0, lan9645x, DEV_PCS1G_ANEG_CFG(p->chip_port));
	}

	/* Take PCS out of reset */
	lan_rmw(DEV_CLOCK_CFG_LINK_SPEED_SET(1) |
		DEV_CLOCK_CFG_PCS_RX_RST_SET(0) |
		DEV_CLOCK_CFG_PCS_TX_RST_SET(0),
		DEV_CLOCK_CFG_LINK_SPEED |
		DEV_CLOCK_CFG_PCS_RX_RST |
		DEV_CLOCK_CFG_PCS_TX_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	return 0;
}

void lan9645x_pcs_get_state(struct phylink_pcs *pcs,
			    struct phylink_link_state *state)
{
	struct lan9645x_port *p =
		container_of(pcs, struct lan9645x_port, phylink_pcs);
	struct lan9645x *lan9645x = p->lan9645x;
	bool link_down;
	u16 bmsr = 0;
	u16 lp_adv;
	u32 val;

	dev_dbg(lan9645x->dev, "speed=%d link=%u interface=%d duplex=%d\n",
		state->speed, state->link, state->interface, state->duplex);

	val = lan_rd(lan9645x, DEV_PCS1G_STICKY(p->chip_port));
	link_down = DEV_PCS1G_STICKY_LINK_DOWN_STICKY_GET(val);
	if (link_down)
		lan_wr(val, lan9645x, DEV_PCS1G_STICKY(p->chip_port));

	/* Get both current Link and Sync status */
	val = lan_rd(lan9645x, DEV_PCS1G_LINK_STATUS(p->chip_port));
	state->link = DEV_PCS1G_LINK_STATUS_LINK_STATUS_GET(val) &&
		DEV_PCS1G_LINK_STATUS_SYNC_STATUS_GET(val);
	state->link &= !link_down;

	/* Get PCS ANEG status register */
	val = lan_rd(lan9645x, DEV_PCS1G_ANEG_STATUS(p->chip_port));
	/* Aneg complete provides more information  */
	if (DEV_PCS1G_ANEG_STATUS_ANEG_COMPLETE_GET(val)) {
		state->an_complete = true;

		bmsr |= state->link ? BMSR_LSTATUS : 0;
		bmsr |= BMSR_ANEGCOMPLETE;

		lp_adv = DEV_PCS1G_ANEG_STATUS_LP_ADV_GET(val);
		phylink_mii_c22_pcs_decode_state(state, bmsr, lp_adv);
	} else {
		if (!state->link)
			return;

		if (state->interface == PHY_INTERFACE_MODE_1000BASEX)
			state->speed = SPEED_1000;
		else if (state->interface == PHY_INTERFACE_MODE_2500BASEX)
			state->speed = SPEED_2500;

		state->duplex = DUPLEX_FULL;
	}

	/* RX latency register is 2^8, so LSB = 1/(2^8)ns ~ 3.90625ps
	 * So for 1G we need to add 800ps per barrel shifter delay: 800 /
	 * 3.90625 = 0xCD
	 * So for 2.5G we need to add 320ps per barrel shifter delay: 320 /
	 * 3.90625 = 0x52
	 *
	 * TODO: update for lan9645x?
	 */
	if (state->link && state->speed == SPEED_1000) {
		p->rx_delay =
			DEV_PCS1G_LINK_STATUS_DELAY_VAR_GET(val) * 0xcd;
	} else if (state->link && state->speed == SPEED_2500) {
		p->rx_delay =
			DEV_PCS1G_LINK_STATUS_DELAY_VAR_GET(val) * 0x52;
	} else {
		p->rx_delay = 0;
	}
}
