// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <linux/netdevice.h>
#include <linux/phy/phy.h>

#include "lan966x_main.h"

#define RSRV_RAW_FC_NO_JUMBO (9 * 1518)
#define RSRV_RAW_NO_FC_JUMBO 12000
#define RSRV_RAW_FC_JUMBO 40000

/* Watermark encode */
static u32 lan966x_wm_enc(u32 value)
{
	value /= LAN966X_BUFFER_CELL_SZ;

	if (value >= MULTIPLIER_BIT) {
		value /= 16;
		if (value >= MULTIPLIER_BIT) {
			value = (MULTIPLIER_BIT - 1);
		}
		value |= MULTIPLIER_BIT;
	}

	return value;
}

static void lan966x_port_link_down(struct lan966x_port *port)
{
	struct lan966x *lan966x = port->lan966x;
	u32 val, delay = 0;

	/* 0.5: Disable any AFI */
	lan_rmw(AFI_PORT_CFG_FC_SKIP_TTI_INJ_SET(1) |
		AFI_PORT_CFG_FRM_OUT_MAX_SET(0),
		AFI_PORT_CFG_FC_SKIP_TTI_INJ |
		AFI_PORT_CFG_FRM_OUT_MAX,
		lan966x, AFI_PORT_CFG(port->chip_port));

	/* wait for reg afi_port_frm_out to become 0 for the port */
	do {
		val = lan_rd(lan966x, AFI_PORT_FRM_OUT(port->chip_port));
		val = AFI_PORT_FRM_OUT_FRM_OUT_CNT_GET(val);
		msleep(1);
		delay++;
		if (delay == 2000) {
			pr_err("AFI timeout chip port %u", port->chip_port);
			break;
		}
	} while (val);

	delay = 0;

	/* 1: Reset the PCS Rx clock domain  */
	lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(1),
		DEV_CLOCK_CFG_PCS_RX_RST,
		lan966x, DEV_CLOCK_CFG(port->chip_port));

	/* 2: Disable MAC frame reception */
	lan_rmw(DEV_MAC_ENA_CFG_RX_ENA_SET(0),
		DEV_MAC_ENA_CFG_RX_ENA,
		lan966x, DEV_MAC_ENA_CFG(port->chip_port));

	/* 3: Disable traffic being sent to or from switch port */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(0),
		QSYS_SW_PORT_MODE_PORT_ENA,
		lan966x, QSYS_SW_PORT_MODE(port->chip_port));

	/* 4: Disable dequeuing from the egress queues  */
	lan_rmw(QSYS_PORT_MODE_DEQUEUE_DIS_SET(1),
		QSYS_PORT_MODE_DEQUEUE_DIS,
		lan966x, QSYS_PORT_MODE(port->chip_port));

	/* 5: Disable Flowcontrol */
	lan_rmw(SYS_PAUSE_CFG_PAUSE_ENA_SET(0),
		SYS_PAUSE_CFG_PAUSE_ENA,
		lan966x, SYS_PAUSE_CFG(port->chip_port));

	/* 5.1: Disable PFC */
	lan_rmw(QSYS_SW_PORT_MODE_TX_PFC_ENA_SET(0),
	        QSYS_SW_PORT_MODE_TX_PFC_ENA,
	        lan966x, QSYS_SW_PORT_MODE(port->chip_port));

	/* 6: Wait a worst case time 8ms (jumbo/10Mbit) */
	msleep(8);

	/* 7: Disable HDX backpressure (Bugzilla 3203) */
	lan_rmw(SYS_FRONT_PORT_MODE_HDX_MODE_SET(0),
		SYS_FRONT_PORT_MODE_HDX_MODE,
		lan966x, SYS_FRONT_PORT_MODE(port->chip_port));

	/* 8: Flush the queues accociated with the port */
	lan_rmw(QSYS_SW_PORT_MODE_AGING_MODE_SET(3),
	        QSYS_SW_PORT_MODE_AGING_MODE,
	        lan966x, QSYS_SW_PORT_MODE(port->chip_port));

	/* 9: Enable dequeuing from the egress queues */
	lan_rmw(QSYS_PORT_MODE_DEQUEUE_DIS_SET(0),
		QSYS_PORT_MODE_DEQUEUE_DIS,
		lan966x, QSYS_PORT_MODE(port->chip_port));

	/* 10: Wait until flushing is complete */
	do {
		val = lan_rd(lan966x, QSYS_SW_STATUS(port->chip_port));
		msleep(1);
		delay++;
		if (delay == 2000) {
			pr_err("Flush timeout chip port %u", port->chip_port);
			break;
		}
	} while (val & QSYS_SW_STATUS_EQ_AVAIL);

	/* 11: Reset the Port and MAC clock domains */
	lan_rmw(DEV_MAC_ENA_CFG_TX_ENA_SET(0),
		DEV_MAC_ENA_CFG_TX_ENA,
		lan966x, DEV_MAC_ENA_CFG(port->chip_port)); /* Bugzilla#19076 */

	lan_rmw(DEV_CLOCK_CFG_PORT_RST_SET(1),
		DEV_CLOCK_CFG_PORT_RST,
		lan966x, DEV_CLOCK_CFG(port->chip_port));

	msleep(1);

	lan_rmw(DEV_CLOCK_CFG_MAC_TX_RST_SET(1) |
		DEV_CLOCK_CFG_MAC_RX_RST_SET(1) |
		DEV_CLOCK_CFG_PORT_RST_SET(1),
		DEV_CLOCK_CFG_MAC_TX_RST |
		DEV_CLOCK_CFG_MAC_RX_RST |
		DEV_CLOCK_CFG_PORT_RST,
		lan966x, DEV_CLOCK_CFG(port->chip_port));

	/* 12: Clear flushing */
	lan_rmw(QSYS_SW_PORT_MODE_AGING_MODE_SET(2),
	        QSYS_SW_PORT_MODE_AGING_MODE,
	        lan966x, QSYS_SW_PORT_MODE(port->chip_port));

	/* The port is disabled and flushed, now set up the port in the new operating mode */
}

static u32 lan966x_calculate_reverved_space(struct lan966x *lan966x,
					    u32 rsrv_raw_fc_no_jumbo,
					    u32 rsrv_raw_no_fc_jumbo,
					    u32 rsrv_raw_fc_jumbo)
{
	u32 pfc_mask, val, port_idx, mtu, rsrv_total = 0;
	bool fc_gen;

	/* Calculate the total reserved space for all ports */
	for (port_idx = 0; port_idx < lan966x->num_phys_ports; port_idx++) {
		if (!lan966x->ports[port_idx])
			continue;

		val = lan_rd(lan966x, SYS_MAC_FC_CFG(port_idx));
		fc_gen = (SYS_MAC_FC_CFG_TX_FC_ENA_GET(val) != 0) ? 1 : 0;
		val = lan_rd(lan966x, ANA_PFC_CFG(port_idx));
		pfc_mask = ANA_PFC_CFG_RX_PFC_ENA_GET(val);
		val = lan_rd(lan966x, DEV_MAC_MAXLEN_CFG(port_idx));
		mtu = DEV_MAC_MAXLEN_CFG_MAX_LEN_GET(val);
		if (pfc_mask) {
			/* Priority Flow Control */
			rsrv_total += rsrv_raw_no_fc_jumbo;
		} else if (mtu > VLAN_ETH_FRAME_LEN) {
			/* Standard Flow Control */
			if (fc_gen) { /* FC generation enabled */
				rsrv_total += rsrv_raw_fc_jumbo;
			} else { /* FC generation disabled */
				rsrv_total += rsrv_raw_no_fc_jumbo;
			}
		} else if (fc_gen) { /* FC generation enabled */
			rsrv_total += rsrv_raw_fc_no_jumbo;
		}
	}
	return rsrv_total;
}

static void lan966x_port_link_up(struct lan966x_port *port)
{
	struct lan966x_port_config *config = &port->config;
	u32 pfc_mask, val, mtu, rsrv_raw, rsrv_total = 0;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_path_delay *path_delay;
	u32 pause_stop  = 1;
	int speed, mode = 0;
	int tweaks = 5;
	int atop_wm;
	bool fc_gen;

	rsrv_total = lan966x_calculate_reverved_space(lan966x,
						      RSRV_RAW_FC_NO_JUMBO,
						      RSRV_RAW_NO_FC_JUMBO,
						      RSRV_RAW_FC_JUMBO);

	atop_wm = (lan966x->shared_queue_sz - rsrv_total);

	/* Calculate if PFC is enabled on this port */
	val = lan_rd(lan966x, ANA_PFC_CFG(port->chip_port));
	pfc_mask = ANA_PFC_CFG_RX_PFC_ENA_GET(val);

	/* Calculate if FC tx_pause is enabled on this port */
	val = lan_rd(lan966x, SYS_MAC_FC_CFG(port->chip_port));
	fc_gen = (SYS_MAC_FC_CFG_TX_FC_ENA_GET(val) != 0) ? 1 : 0;

	/* Calculate MTU size on this port */
	val = lan_rd(lan966x, DEV_MAC_MAXLEN_CFG(port->chip_port));
	mtu = DEV_MAC_MAXLEN_CFG_MAX_LEN_GET(val);

	/* Calculate FC/PFC dependent configuration values on this port */
	if (pfc_mask) {
		/* Priority Flow Control
		 * Each port can use this as max before tail dropping starts
		 */
		rsrv_raw = RSRV_RAW_FC_NO_JUMBO;
	} else {
		/* Standard Flow Control */
		if (mtu > VLAN_ETH_FRAME_LEN) {
			/* jumbo frame enabled */
			if (fc_gen) { /* FC generation enabled */
				pause_stop = 7;
				rsrv_raw = RSRV_RAW_FC_JUMBO;
			} else { /* FC generation disabled */
				rsrv_raw = RSRV_RAW_NO_FC_JUMBO;
			}
		} else {
			/* jumbo frame disabled */
			if (fc_gen) { /* FC generation enabled */
				pause_stop = 4;
				rsrv_raw = RSRV_RAW_FC_NO_JUMBO;
			} else { /* FC generation disabled */
				rsrv_raw = 0;
			}
		}
	}

	switch (config->speed) {
	case SPEED_10:
		speed = LAN966X_SPEED_10;
		break;
	case SPEED_100:
		speed = LAN966X_SPEED_100;
		break;
	case SPEED_1000:
		speed = LAN966X_SPEED_1000;
		mode = DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);
		break;
	case SPEED_2500:
		speed = LAN966X_SPEED_2500;
		mode = DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);
		break;
	}

	/* Also the GIGA_MODE_ENA(1) needs to be set regardless of the
	 * port speed for QSGMII ports.
	 */
	if (config->phy_mode == PHY_INTERFACE_MODE_QSGMII)
		mode = DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);

	/* Notify TAS about the speed */
	lan966x_tas_speed(port, config->speed);

	lan_wr(config->duplex | mode,
	       lan966x, DEV_MAC_MODE_CFG(port->chip_port));

	lan_rmw(DEV_MAC_IFG_CFG_TX_IFG_SET(config->duplex ? 6 : 5) |
		DEV_MAC_IFG_CFG_RX_IFG1_SET(config->speed == SPEED_10 ? 2 : 1) |
		DEV_MAC_IFG_CFG_RX_IFG2_SET(2),
		DEV_MAC_IFG_CFG_TX_IFG |
		DEV_MAC_IFG_CFG_RX_IFG1 |
		DEV_MAC_IFG_CFG_RX_IFG2,
		lan966x, DEV_MAC_IFG_CFG(port->chip_port));

	lan_rmw(DEV_MAC_HDX_CFG_SEED_SET(4) |
		DEV_MAC_HDX_CFG_SEED_LOAD_SET(1),
		DEV_MAC_HDX_CFG_SEED |
		DEV_MAC_HDX_CFG_SEED_LOAD,
		lan966x, DEV_MAC_HDX_CFG(port->chip_port));

#if defined(SUNRISE)
	if (config->phy_mode != PHY_INTERFACE_MODE_QSGMII) {
		if (config->speed == SPEED_1000)
			lan_wr(SUNRISE_TOP_GMII_CFG_GTX_CLK_ENA_SET(1),
			       lan966x, SUNRISE_TOP_GMII_CFG(port->chip_port));
		else
			lan_wr(SUNRISE_TOP_GMII_CFG_GTX_CLK_ENA_SET(0),
			       lan966x, SUNRISE_TOP_GMII_CFG(port->chip_port));
	}
#endif

#if defined(ASIC)
	if (config->phy_mode == PHY_INTERFACE_MODE_GMII) {
		if (config->speed == SPEED_1000)
			lan_rmw(CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA_SET(1),
				CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA,
				lan966x, CHIP_TOP_CUPHY_PORT_CFG(port->chip_port));
		else
			lan_rmw(CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA_SET(0),
				CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA,
				lan966x, CHIP_TOP_CUPHY_PORT_CFG(port->chip_port));
	}
#endif

	/* Configure the PFC link speed */
	lan_rmw(ANA_PFC_CFG_FC_LINK_SPEED_SET(speed),
		ANA_PFC_CFG_FC_LINK_SPEED,
		lan966x, ANA_PFC_CFG(port->chip_port));

	if (config->phy_mode == PHY_INTERFACE_MODE_QSGMII) {
		lan_rmw(DEV_PCS1G_CFG_PCS_ENA_SET(1),
			DEV_PCS1G_CFG_PCS_ENA,
			lan966x, DEV_PCS1G_CFG(port->chip_port));

		lan_rmw(DEV_PCS1G_SD_CFG_SD_ENA_SET(0),
			DEV_PCS1G_SD_CFG_SD_ENA,
			lan966x, DEV_PCS1G_SD_CFG(port->chip_port));
	}

	lan_rmw(DEV_PCS1G_CFG_PCS_ENA_SET(1),
		DEV_PCS1G_CFG_PCS_ENA,
		lan966x, DEV_PCS1G_CFG(port->chip_port));

	lan_rmw(DEV_PCS1G_SD_CFG_SD_ENA_SET(0),
		DEV_PCS1G_SD_CFG_SD_ENA,
		lan966x, DEV_PCS1G_SD_CFG(port->chip_port));

	/* Set Pause WM hysteresis, start/stop are in 1518 byte units */
	lan_rmw(SYS_PAUSE_CFG_PAUSE_STOP_SET(lan966x_wm_enc(pause_stop * 1518)) |
		SYS_PAUSE_CFG_PAUSE_START_SET(lan966x_wm_enc((pause_stop + 2) * 1518)),
		SYS_PAUSE_CFG_PAUSE_STOP |
		SYS_PAUSE_CFG_PAUSE_START,
		lan966x, SYS_PAUSE_CFG(port->chip_port));

	/* Set pause frames enable depending on FC tx_pause is enabled */
	lan_rmw(SYS_PAUSE_CFG_PAUSE_ENA_SET(0),
		SYS_PAUSE_CFG_PAUSE_ENA,
		lan966x, SYS_PAUSE_CFG(port->chip_port));

	/* Set SMAC of Pause frame (00:00:00:00:00:00) */
	lan_wr(0, lan966x, DEV_FC_MAC_LOW_CFG(port->chip_port));
	lan_wr(0, lan966x, DEV_FC_MAC_HIGH_CFG(port->chip_port));

	/* Flow control */
	lan_rmw(SYS_MAC_FC_CFG_FC_LINK_SPEED_SET(speed) |
		SYS_MAC_FC_CFG_FC_LATENCY_CFG_SET(7) |
		SYS_MAC_FC_CFG_ZERO_PAUSE_ENA_SET(1) |
		SYS_MAC_FC_CFG_PAUSE_VAL_CFG_SET(pfc_mask ? 0xff : 0xffff) |
		SYS_MAC_FC_CFG_RX_FC_ENA_SET(config->pause & MLO_PAUSE_RX ? 1 : 0) |
		SYS_MAC_FC_CFG_TX_FC_ENA_SET(config->pause & MLO_PAUSE_TX ? 1 : 0),
		SYS_MAC_FC_CFG_FC_LINK_SPEED |
		SYS_MAC_FC_CFG_FC_LATENCY_CFG |
		SYS_MAC_FC_CFG_ZERO_PAUSE_ENA |
		SYS_MAC_FC_CFG_PAUSE_VAL_CFG |
		SYS_MAC_FC_CFG_RX_FC_ENA |
		SYS_MAC_FC_CFG_TX_FC_ENA,
		lan966x, SYS_MAC_FC_CFG(port->chip_port));

	/* Enable PFC */
	lan_rmw(QSYS_SW_PORT_MODE_TX_PFC_ENA_SET(pfc_mask),
	        QSYS_SW_PORT_MODE_TX_PFC_ENA,
	        lan966x, QSYS_SW_PORT_MODE(port->chip_port));

	/* When 'port ATOP' and 'common ATOP_TOT' are exceeded,
	 * tail dropping is activated on port
	 */
	lan_wr(lan966x_wm_enc(rsrv_raw), lan966x, SYS_ATOP(port->chip_port));
	lan_wr(lan966x_wm_enc(atop_wm), lan966x, SYS_ATOP_TOT_CFG);

	/* Update RX/TX delay */
	list_for_each_entry(path_delay, &port->path_delays, list) {
		if (path_delay->speed == config->speed) {
			lan_wr(path_delay->rx_delay + port->rx_delay,
			       lan966x, SYS_PTP_RXDLY_CFG(port->chip_port));
			lan_wr(path_delay->tx_delay,
			       lan966x, SYS_PTP_TXDLY_CFG(port->chip_port));
		}
	}

	/* This needs to be at the end */
	/* Enable MAC module */
	lan_wr(DEV_MAC_ENA_CFG_RX_ENA_SET(1) |
	       DEV_MAC_ENA_CFG_TX_ENA_SET(1),
	       lan966x, DEV_MAC_ENA_CFG(port->chip_port));

	/* Take out the clock from reset */
	lan_wr(DEV_CLOCK_CFG_LINK_SPEED_SET(speed),
	       lan966x, DEV_CLOCK_CFG(port->chip_port));

	/* When running at 10 these tweaks need to be set */
	/* TODO - what does it mean */
	if (speed == LAN966X_SPEED_10)
		tweaks = 7;

	/* Enable phase detector */
	/* First it is needed to disable and then enable it and after that it
	 * needed to clear the failed bit which is set by default. Also there
	 * are 2 phase detector ctrl one for TX and one for RX
	 */
	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA,
		lan966x, DEV_PHAD_CTRL(port->chip_port, 0));

	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA,
		lan966x, DEV_PHAD_CTRL(port->chip_port, 1));

	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(1) |
		DEV_PHAD_CTRL_TWEAKS_SET(tweaks) |
		DEV_PHAD_CTRL_PHAD_FAILED_SET(1) |
		DEV_PHAD_CTRL_LOCK_ACC_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA |
		DEV_PHAD_CTRL_TWEAKS |
		DEV_PHAD_CTRL_PHAD_FAILED |
		DEV_PHAD_CTRL_LOCK_ACC,
		lan966x, DEV_PHAD_CTRL(port->chip_port, 0));

	lan_rmw(DEV_PHAD_CTRL_PHAD_ENA_SET(1) |
		DEV_PHAD_CTRL_TWEAKS_SET(tweaks) |
		DEV_PHAD_CTRL_PHAD_FAILED_SET(1) |
		DEV_PHAD_CTRL_LOCK_ACC_SET(0),
		DEV_PHAD_CTRL_PHAD_ENA |
		DEV_PHAD_CTRL_TWEAKS |
		DEV_PHAD_CTRL_PHAD_FAILED |
		DEV_PHAD_CTRL_LOCK_ACC,
		lan966x, DEV_PHAD_CTRL(port->chip_port, 1));

	/* To clear failed bit it is needed to write a 1 */
	lan_rmw(DEV_PHAD_CTRL_PHAD_FAILED_SET(1),
		DEV_PHAD_CTRL_PHAD_FAILED,
		lan966x, DEV_PHAD_CTRL(port->chip_port, 0));

	lan_rmw(DEV_PHAD_CTRL_PHAD_FAILED_SET(1),
		DEV_PHAD_CTRL_PHAD_FAILED,
		lan966x, DEV_PHAD_CTRL(port->chip_port, 1));

	/* Core: Enable port for frame transfer */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1) |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG_SET(1) |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE_SET(1) |
		QSYS_SW_PORT_MODE_AGING_MODE_SET(0),
		QSYS_SW_PORT_MODE_PORT_ENA |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE |
		QSYS_SW_PORT_MODE_AGING_MODE,
		lan966x, QSYS_SW_PORT_MODE(port->chip_port));

	lan_rmw(AFI_PORT_CFG_FC_SKIP_TTI_INJ_SET(0) |
		AFI_PORT_CFG_FRM_OUT_MAX_SET(16),
		AFI_PORT_CFG_FC_SKIP_TTI_INJ |
		AFI_PORT_CFG_FRM_OUT_MAX,
		lan966x, AFI_PORT_CFG(port->chip_port));
}

void lan966x_port_config_down(struct lan966x_port *port)
{
	lan966x_port_link_down(port);
}

void lan966x_port_config_up(struct lan966x_port *port)
{
	struct lan966x_port_config *config = &port->config;
	struct lan966x *lan966x = port->lan966x;
#if !defined(SUNRISE) && !defined(ASIC)
	struct net_device *dev = port->dev;
	u32 val;

	/* In mode 10/100 MAC expected to receive TX clock from the PHY.
	 * To support this, GPIO_OUT_x(5) is used to select TX clock for the
	 * MAC. 0 for GMII and 1 for 10/100
	 */
	val = lan_rd(lan966x, GCB_GPIO_OUT(dev->phydev->mdio.addr));
	if (config->speed == SPEED_10 ||
	    config->speed == SPEED_100)
		val |= BIT(5);
	else
		val &= ~BIT(5);
	lan_wr(val, lan966x, GCB_GPIO_OUT(dev->phydev->mdio.addr));
#endif

	lan966x_port_link_up(port);

	if ((config->phy_mode == PHY_INTERFACE_MODE_QSGMII ||
	     config->phy_mode == PHY_INTERFACE_MODE_SGMII) &&
	    !(lan_rd(lan966x, DEV_PCS1G_LINK_STATUS(port->chip_port)) &
	      DEV_PCS1G_LINK_STATUS_LINK_STATUS))
		lan966x_port_link_down(port);
}

void lan966x_port_status_get(struct lan966x_port *port,
			     struct phylink_link_state *state)
{
	struct lan966x *lan966x = port->lan966x;
	bool link_down;
	u16 bmsr = 0;
	u16 lp_adv;
	u32 val;

	/* Get PCS Link down sticky */
	val = lan_rd(lan966x, DEV_PCS1G_STICKY(port->chip_port));
	link_down = DEV_PCS1G_STICKY_LINK_DOWN_STICKY_GET(val);
	if (link_down)	/* Clear the sticky */
		lan_wr(val, lan966x, DEV_PCS1G_STICKY(port->chip_port));

	/* Get both current Link and Sync status */
	val = lan_rd(lan966x, DEV_PCS1G_LINK_STATUS(port->chip_port));
	state->link = DEV_PCS1G_LINK_STATUS_LINK_STATUS_GET(val) &&
		      DEV_PCS1G_LINK_STATUS_SYNC_STATUS_GET(val);
	state->link &= !link_down;

	/* Get PCS ANEG status register */
	val = lan_rd(lan966x, DEV_PCS1G_ANEG_STATUS(port->chip_port));

	/* Aneg complete provides more information  */
	if (DEV_PCS1G_ANEG_STATUS_ANEG_COMPLETE_GET(val)) {
		state->an_enabled = true;
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
		state->an_complete = false;
		state->an_enabled = false;
	}

	/* RX latency register is 2^8, so LSB = 1/(2^8)ns ~ 3.90625ps
	 * So for 1G we need to add 800ps per barrel shifter delay: 800 /
	 * 3.90625 = 0xCD
	 * So for 2.5G we need to add 320ps per barrel shifter delay: 320 /
	 * 3.90625 = 0x52
	 */
	if (state->link && state->speed == SPEED_1000) {
		port->rx_delay = DEV_PCS1G_LINK_STATUS_DELAY_VAR_GET(val) * 0xcd;
	} else if (state->link && state->speed == SPEED_2500) {
		port->rx_delay = DEV_PCS1G_LINK_STATUS_DELAY_VAR_GET(val) * 0x52;
	} else {
		port->rx_delay = 0;
	}
}

int lan966x_port_pcs_set(struct lan966x_port *port,
			 struct lan966x_port_config *config)
{
	struct lan966x *lan966x = port->lan966x;
	bool inband_aneg = false;
	bool outband;

	if (config->inband) {
		if (config->portmode == PHY_INTERFACE_MODE_SGMII ||
		    config->portmode == PHY_INTERFACE_MODE_QSGMII)
			inband_aneg = true; /* Cisco-SGMII in-band-aneg */
		else if (config->portmode == PHY_INTERFACE_MODE_1000BASEX &&
			 config->autoneg)
			inband_aneg = true; /* Clause-37 in-band-aneg */

		outband = false;
	} else {
		outband = true; /* Phy is connnected to the MAC */
	}

	/* Choose SGMII or 1000BaseX/2500BaseX PCS mode */
	lan_rmw(DEV_PCS1G_MODE_CFG_SGMII_MODE_ENA_SET(outband),
		DEV_PCS1G_MODE_CFG_SGMII_MODE_ENA,
		lan966x, DEV_PCS1G_MODE_CFG(port->chip_port));

	/* Enable PCS */
	lan_wr(DEV_PCS1G_CFG_PCS_ENA_SET(1),
	       lan966x, DEV_PCS1G_CFG(port->chip_port));

	if (inband_aneg) {
		u16 abil = phylink_mii_c22_pcs_encode_advertisement(config->portmode,
								    config->advertising);

		/* Enable in-band aneg */
		lan_wr(DEV_PCS1G_ANEG_CFG_ADV_ABILITY_SET(abil) |
		       DEV_PCS1G_ANEG_CFG_SW_RESOLVE_ENA_SET(1) |
		       DEV_PCS1G_ANEG_CFG_ENA_SET(1) |
		       DEV_PCS1G_ANEG_CFG_RESTART_ONE_SHOT_SET(1),
		       lan966x, DEV_PCS1G_ANEG_CFG(port->chip_port));
	} else {
		lan_wr(0, lan966x,
		       DEV_PCS1G_ANEG_CFG(port->chip_port));
	}

	/* Take PCS out of reset */
	lan_rmw(DEV_CLOCK_CFG_LINK_SPEED_SET(LAN966X_SPEED_1000) |
		DEV_CLOCK_CFG_PCS_RX_RST_SET(0) |
		DEV_CLOCK_CFG_PCS_TX_RST_SET(0),
		DEV_CLOCK_CFG_LINK_SPEED |
		DEV_CLOCK_CFG_PCS_RX_RST |
		DEV_CLOCK_CFG_PCS_TX_RST,
		lan966x, DEV_CLOCK_CFG(port->chip_port));

	port->config = *config;

	return 0;
}

void lan966x_port_init(struct lan966x_port *port)
{
	struct lan966x_port_config *config = &port->config;
	struct lan966x *lan966x = port->lan966x;

	lan966x_port_config_down(port);

	if (config->phy_mode != PHY_INTERFACE_MODE_QSGMII)
		return;

	lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(0) |
		DEV_CLOCK_CFG_PCS_TX_RST_SET(0) |
		DEV_CLOCK_CFG_LINK_SPEED_SET(LAN966X_SPEED_1000),
		DEV_CLOCK_CFG_PCS_RX_RST |
		DEV_CLOCK_CFG_PCS_TX_RST |
		DEV_CLOCK_CFG_LINK_SPEED,
		lan966x, DEV_CLOCK_CFG(port->chip_port));
}
