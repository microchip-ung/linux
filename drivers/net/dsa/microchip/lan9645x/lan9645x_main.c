// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>

#include "lan9645x_main.h"
#include "lan9645x_stats.h"
#include "lan9645x_netlink_qos.h"
#include "lan9645x_netlink_frer.h"
#include "lan9645x_netlink_fp.h"
#include "lan9645x_mrp.h"

static const char *lan9645x_resource_names[NUM_TARGETS] = {
	[TARGET_ORG]          = "org",
	[TARGET_GCB]          = "gcb",
	[TARGET_QS]           = "qs",
	[TARGET_PTP]          = "ptp",
	[TARGET_CHIP_TOP]     = "chip_top",
	[TARGET_TAS]          = "tas",
	[TARGET_REW]          = "rew",
	[TARGET_VCAP]         = "vcap",
	[TARGET_VCAP + 1]     = "vcap1",
	[TARGET_VCAP + 2]     = "vcap2",
	[TARGET_MEP]          = "mep",
	[TARGET_SYS]          = "sys",
	[TARGET_HSIO]         = "hsio",
	[TARGET_DEV]          = "dev",
	[TARGET_DEV + 1]      = "dev1",
	[TARGET_DEV + 2]      = "dev2",
	[TARGET_DEV + 3]      = "dev3",
	[TARGET_DEV + 4]      = "dev4",
	[TARGET_DEV + 5]      = "dev5",
	[TARGET_DEV + 6]      = "dev6",
	[TARGET_DEV + 7]      = "dev7",
	[TARGET_DEV + 8]      = "dev8",
	[TARGET_UVOV]         = "uvov",
	[TARGET_QSYS]         = "qsys",
	[TARGET_AFI]          = "afi",
	[TARGET_ANA]          = "ana",
	[TARGET_IROM]         = "irom",
	[TARGET_IRAM]         = "iram",
	[TARGET_EROM_I2C]     = "erom_i2c",
	[TARGET_EROM_SPI]     = "erom_spi",
	[TARGET_CUPHY]        = "cuphy",
	[TARGET_UART]         = "uart",
	[TARGET_I2C]          = "i2c",
	[TARGET_I2C + 1]      = "i2c1",
	[TARGET_TIMERS]       = "timers",
	[TARGET_CPU]          = "cpu",
	[TARGET_OTP]          = "otp",
	[TARGET_WDT]          = "wdt",
};

struct lan9645x_host_flood_work {
	struct work_struct work;
	struct lan9645x *lan9645x;
	int port;
	bool uc;
	bool mc;
};

static struct regmap *lan9645x_request_regmap(struct lan9645x *lan9645x,
					      enum lan9645x_target target)
{
	const char *resource_name = lan9645x_resource_names[target];

	if (!resource_name) {
		dev_err(lan9645x->dev,
			"Requested regmap for target not found: %d\n", target);
		return NULL;
	}

	return dev_get_regmap(lan9645x->dev->parent, resource_name);
}

static int lan9645x_tag_npi_setup(struct dsa_switch *ds)
{
	struct dsa_port *dp, *first_cpu_dp = NULL;
	struct lan9645x *lan9645x = ds->priv;

	dsa_switch_for_each_user_port(dp, ds) {
		if (first_cpu_dp && dp->cpu_dp != first_cpu_dp) {
			dev_err(ds->dev, "Multiple NPI ports not supported\n");
			return -EINVAL;
		}

		first_cpu_dp = dp->cpu_dp;
	}

	if (!first_cpu_dp)
		return -EINVAL;

	lan9645x_npi_port_init(lan9645x, first_cpu_dp);

	return 0;
}

static enum dsa_tag_protocol lan9645x_get_tag_protocol(struct dsa_switch *ds,
						       int port,
						       enum dsa_tag_protocol tp)
{
	struct lan9645x *lan9645x = ds->priv;

	return lan9645x->tag_proto;
}

static int lan9645x_connect_tag_protocol(struct dsa_switch *ds,
					 enum dsa_tag_protocol proto)
{
	switch (proto) {
	case DSA_TAG_PROTO_LAN9645X:
		return 0;
	default:
		return -EPROTONOSUPPORT;
	}
}

static void lan9645x_teardown(struct dsa_switch *ds)
{
	struct lan9645x *lan9645x = ds->priv;

	debugfs_remove_recursive(lan9645x->debugfs_root);
	lan9645x_afi_deinit(lan9645x);
	lan9645x_mrp_uninit(lan9645x);
	lan9645x_netlink_fp_uninit();
	lan9645x_netlink_frer_uninit();
	lan9645x_netlink_qos_uninit();
	lan9645x_taprio_deinit(lan9645x);
	lan9645x_npi_port_deinit(lan9645x, lan9645x->npi);
	lan9645x_stats_deinit(lan9645x);
	lan9645x_mac_deinit(lan9645x);
	lan9645x_mdb_deinit(lan9645x);
	lan9645x_ptp_deinit(lan9645x);
	lan9645x_hsr_prp_deinit(lan9645x);
	lan9645x_streamt_deinit(lan9645x);
	lan9645x_vcap_deinit(lan9645x);
	lan9645x_bum_deinit(lan9645x);
	mutex_destroy(&lan9645x->link_isdx_lock);
	mutex_destroy(&lan9645x->psfp_lock);
	mutex_destroy(&lan9645x->esdx_lock);
}

static void lan9645x_port_phylink_get_caps(struct dsa_switch *ds, int port,
					   struct phylink_config *config)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_phylink_get_caps(lan9645x, port, config);
}

static int lan9645x_port_set_maxlen(struct lan9645x *lan9645x, int port,
				    size_t sdu)
{
	struct lan9645x_port *p = lan9645x->ports[port];

	dev_dbg(lan9645x->dev, "port=%d sdu=%zu", port, sdu);

	int maxlen = sdu + ETH_HLEN + ETH_FCS_LEN;

	if (port == lan9645x->npi) {
		maxlen += LAN9645X_IFH_LEN;
		maxlen += LAN9645X_LONG_PREFIX_LEN;
	}

	lan_wr(DEV_MAC_MAXLEN_CFG_MAX_LEN_SET(maxlen), lan9645x,
	       DEV_MAC_MAXLEN_CFG(p->chip_port));

	/* Set Pause WM hysteresis */
	lan_rmw(SYS_PAUSE_CFG_PAUSE_STOP_SET(lan9645x_wm_enc(4 * maxlen)) |
		SYS_PAUSE_CFG_PAUSE_START_SET(lan9645x_wm_enc(6 * maxlen)),
		SYS_PAUSE_CFG_PAUSE_START |
		SYS_PAUSE_CFG_PAUSE_STOP,
		lan9645x,
		SYS_PAUSE_CFG(p->chip_port));

	return 0;
}

static int lan9645x_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_port_set_maxlen(lan9645x, port, new_mtu);

	return 0;
}

static int lan9645x_get_max_mtu(struct dsa_switch *ds, int port)
{
	struct lan9645x *lan9645x = ds->priv;

	/* Actual MAC max MTU is around 16KB. We set 10000 - overhead which
	 * should be sufficient for all jumbo frames. Larger frames can cause
	 * problems especially with flow control, since we only have 160K queue
	 * buffer.
	 */
	int max_mtu = 10000 - ETH_HLEN - ETH_FCS_LEN;

	if (port == lan9645x->npi) {
		max_mtu -= LAN9645X_IFH_LEN;
		max_mtu -= LAN9645X_LONG_PREFIX_LEN;
	}

	return max_mtu;
}

static irqreturn_t lan9645x_ana_irq_handler(int virq, void *args)
{
	struct lan9645x *lan9645x = args;

	lan9645x_mrp_ring_open(lan9645x);
	lan9645x_mrp_in_open(lan9645x);

	return IRQ_HANDLED;
}

static int lan9645x_port_init(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x->ports[port];

	/* Disable learning on port */
	lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(0),
		ANA_PORT_CFG_LEARN_ENA,
		lan9645x, ANA_PORT_CFG(p->chip_port));

	p->learn_ena = false;

	lan9645x_port_set_maxlen(lan9645x, port, ETH_DATA_LEN);

	lan9645x_phylink_port_down(lan9645x, port);

	if (phy_interface_num_ports(p->phy_mode) == 4)
		lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(0) |
			DEV_CLOCK_CFG_PCS_TX_RST_SET(0),
			DEV_CLOCK_CFG_PCS_RX_RST |
			DEV_CLOCK_CFG_PCS_TX_RST,
			lan9645x, DEV_CLOCK_CFG(p->chip_port));

	/* Drop frames with multicast source address */
	lan_rmw(ANA_DROP_CFG_DROP_MC_SMAC_ENA_SET(1),
		ANA_DROP_CFG_DROP_MC_SMAC_ENA, lan9645x,
		ANA_DROP_CFG(p->chip_port));

	/* Enable receiving frames on the port, and activate auto-learning of
	 * MAC addresses.
	 */
	lan_rmw(ANA_PORT_CFG_LEARNAUTO_SET(1) |
		ANA_PORT_CFG_RECV_ENA_SET(1) |
		ANA_PORT_CFG_PORTID_VAL_SET(p->chip_port),
		ANA_PORT_CFG_LEARNAUTO |
		ANA_PORT_CFG_RECV_ENA |
		ANA_PORT_CFG_PORTID_VAL,
		lan9645x, ANA_PORT_CFG(p->chip_port));

	if (p->chip_port != lan9645x->npi)
		lan9645x_vlan_set_hostmode(p);

	return 0;
}

static int lan9645x_port_parse_delays(struct lan9645x_port *port,
				      struct fwnode_handle *portnp)
{
	struct fwnode_handle *delay;
	int err;

	INIT_LIST_HEAD(&port->path_delays);

	fwnode_for_each_available_child_node(portnp, delay) {
		struct lan9645x_path_delay *path_delay;
		s32 tx_delay;
		s32 rx_delay;
		u32 speed;

		err = fwnode_property_read_u32(delay, "speed", &speed);
		if (err)
			return err;

		err = fwnode_property_read_u32(delay, "rx_delay", &rx_delay);
		if (err)
			return err;

		err = fwnode_property_read_u32(delay, "tx_delay", &tx_delay);
		if (err)
			return err;

		path_delay = devm_kzalloc(port->lan9645x->dev,
					  sizeof(*path_delay), GFP_KERNEL);
		if (!path_delay)
			return -ENOMEM;

		path_delay->rx_delay = rx_delay;
		path_delay->tx_delay = tx_delay;
		path_delay->speed = speed;
		list_add_tail(&path_delay->list, &port->path_delays);
	}

	return 0;
}

static int lan9645x_port_setup_leds(struct lan9645x *lan9645x,
				    struct fwnode_handle *portnp)
{
	u32 val[LAN9645X_LED_PROP_CNT];
	int err;

	err = fwnode_property_read_u32_array(portnp, "microchip,led-drive-mode",
					     val, LAN9645X_LED_PROP_CNT);
	if (err)
		return err;

	lan_rmw(CHIP_TOP_CUPHY_LED_CFG_LED_DRIVE_MODE_SET(val[LAN9645X_LED_PROP_DRIVE]),
		CHIP_TOP_CUPHY_LED_CFG_LED_DRIVE_MODE,
		lan9645x,
		CHIP_TOP_CUPHY_LED_CFG(val[LAN9645X_LED_PROP_IDX]));

	return 0;
}

static int lan9645x_parse_ports_node(struct lan9645x *lan9645x)
{
	struct fwnode_handle *ports, *portnp;
	struct device *dev = lan9645x->dev;
	int max_ports, num_ports = 0;
	int err = 0;

	max_ports = NUM_PHYS_PORTS - lan9645x->num_port_dis;

	ports = device_get_named_child_node(dev, "ethernet-ports");
	if (!ports)
		ports = device_get_named_child_node(dev, "ports");
	if (!ports) {
		dev_err(dev, "no ethernet-ports or ports child found\n");
		return -ENODEV;
	}

	fwnode_for_each_available_child_node(ports, portnp) {
		phy_interface_t phy_mode;
		struct phy *serdes;
		u32 p;

		num_ports++;

		if (num_ports > max_ports) {
			dev_err(dev,
				"Too many ports in device tree. Max ports supported by SKU: %d \n",
				max_ports);
			err = -ENODEV;
			goto err_free_ports;
		}

		if (fwnode_property_read_u32(portnp, "reg", &p)) {
			dev_err(dev, "Port number not defined in device tree (property \"reg\")\n");
			err = -ENODEV;
			fwnode_handle_put(portnp);
			goto err_free_ports;
		}

		if (!(p >= 0 && p <= lan9645x->num_phys_ports)) {
			dev_err(dev,
				"Port number in device tree is invalid %u (property \"reg\")\n",
				p);
			err = -ENODEV;
			fwnode_handle_put(portnp);
			goto err_free_ports;
		}

		phy_mode = fwnode_get_phy_mode(portnp);
		if (phy_mode < 0) {
			dev_err(dev,
				"Failed to read phy-mode or phy-interface-type property for port %u: %pe\n",
				p, ERR_PTR(phy_mode));
			err = -ENODEV;
			fwnode_handle_put(portnp);
			goto err_free_ports;
		}

		fwnode_property_read_string(portnp, "label",
					    &lan9645x->ports[p]->name);

		lan9645x->ports[p]->phy_mode = phy_mode;
		lan9645x->ports[p]->fwnode = fwnode_handle_get(portnp);
		lan9645x_port_parse_delays(lan9645x->ports[p], portnp);
		lan9645x_port_setup_leds(lan9645x, portnp);

		serdes = devm_of_phy_optional_get(lan9645x->dev,
						  to_of_node(portnp), NULL);
		if (IS_ERR(serdes)) {
			err = PTR_ERR(serdes);
			goto err_free_ports;
		}
		lan9645x->ports[p]->serdes = serdes;
	}

err_free_ports:
	fwnode_handle_put(ports);
	return err;
}

static void lan9645x_cpu_port_init(struct lan9645x *lan9645x)
{
	lan_wr(BIT(CPU_PORT), lan9645x, ANA_PGID(PGID_CPU));

	lan_rmw(ANA_PORT_CFG_PORTID_VAL_SET(CPU_PORT) |
		ANA_PORT_CFG_RECV_ENA_SET(1),
		ANA_PORT_CFG_PORTID_VAL |
		ANA_PORT_CFG_RECV_ENA, lan9645x,
		ANA_PORT_CFG(CPU_PORT));

	/* Enable switching to/from cpu port. Keep default aging-mode. */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1) |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG_SET(1) |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE_SET(1),
		QSYS_SW_PORT_MODE_PORT_ENA |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE,
		lan9645x, QSYS_SW_PORT_MODE(CPU_PORT));
}

static int lan9645x_reset_switch(struct lan9645x *lan9645x)
{
	int val = 0;
	int err;

	lan_wr(SYS_RESET_CFG_CORE_ENA_SET(0), lan9645x, SYS_RESET_CFG);
	lan_wr(SYS_RAM_INIT_RAM_INIT_SET(1), lan9645x, SYS_RAM_INIT);
	err = lan9645x_rd_poll_timeout(lan9645x, SYS_RAM_INIT, val,
				       SYS_RAM_INIT_RAM_INIT_GET(val) == 0);
	if (err) {
		dev_err(lan9645x->dev, "Lan9645x setup: failed to init chip RAM.");
		return err;
	}
	lan_wr(SYS_RESET_CFG_CORE_ENA_SET(1), lan9645x, SYS_RESET_CFG);

	return 0;
}

static void lan9645x_igmp_snooping(struct lan9645x *lan9645x, bool enabled,
				   int chip_port)
{
	lan_rmw(ANA_CPU_FWD_CFG_IGMP_REDIR_ENA_SET(enabled) |
		ANA_CPU_FWD_CFG_MLD_REDIR_ENA_SET(enabled) |
		ANA_CPU_FWD_CFG_IPMC_CTRL_COPY_ENA_SET(enabled),
		ANA_CPU_FWD_CFG_IGMP_REDIR_ENA |
		ANA_CPU_FWD_CFG_MLD_REDIR_ENA |
		ANA_CPU_FWD_CFG_IPMC_CTRL_COPY_ENA,
		lan9645x, ANA_CPU_FWD_CFG(chip_port));

	/* Use CPU queues to communicate frame classification to the CPU */
	lan_rmw(ANA_CPUQ_CFG_CPUQ_IGMP_SET(LAN9645X_CPUQ_IGMP) |
		ANA_CPUQ_CFG_CPUQ_MLD_SET(LAN9645X_CPUQ_MLD) |
		ANA_CPUQ_CFG_CPUQ_IPMC_CTRL_SET(LAN9645X_CPUQ_IPMC_CTRL),
		ANA_CPUQ_CFG_CPUQ_IGMP |
		ANA_CPUQ_CFG_CPUQ_MLD |
		ANA_CPUQ_CFG_CPUQ_IPMC_CTRL,
		lan9645x, ANA_CPUQ_CFG);
}

static void lan9645x_set_tail_drop_wm(struct lan9645x *lan9645x)
{
	int shared_per_port;
	int port;

	/* Configure tail dropping watermark */
	shared_per_port =
		lan9645x->shared_queue_sz / (lan9645x->num_phys_ports + 1);

	/* The total memory size is diveded by number of front ports plus CPU
	 * port.
	 */
	lan9645x_for_each_chipport(lan9645x, port) {
		lan_wr(lan9645x_wm_enc(shared_per_port), lan9645x, SYS_ATOP(port));
	}

	/* Tail dropping active based only on per port ATOP wm */
	lan_wr(lan9645x_wm_enc(lan9645x->shared_queue_sz),
	       lan9645x, SYS_ATOP_TOT_CFG);
}

static int lan9645x_setup(struct dsa_switch *ds)
{
	struct lan9645x *lan9645x = ds->priv;
	struct device *dev = lan9645x->dev;
	u32 all_phys_ports, all_ports;
	struct dsa_port *dp;
	int err = 0;

	lan9645x->num_phys_ports = ds->num_ports;
	all_phys_ports = GENMASK(lan9645x->num_phys_ports - 1, 0);
	all_ports = all_phys_ports | BIT(CPU_PORT);

	lan9645x_reset_switch(lan9645x);

	lan9645x->debugfs_root = debugfs_create_dir("lan9645x_sw", NULL);

	lan9645x->ports = devm_kcalloc(lan9645x->dev, lan9645x->num_phys_ports,
				       sizeof(struct lan9645x_port *),
				       GFP_KERNEL);
	if (!lan9645x->ports)
		return -ENOMEM;

	for (int port = 0; port < lan9645x->num_phys_ports; port++) {
		struct lan9645x_port *p;

		p = devm_kzalloc(lan9645x->dev,
				 sizeof(struct lan9645x_port), GFP_KERNEL);
		if (!p) {
			dev_err(lan9645x->dev,
				"failed to allocate port memory\n");
			return -ENOMEM;
		}

		p->lan9645x = lan9645x;
		p->chip_port = port;
		p->phylink_pcs.poll = true;
		p->phylink_pcs.ops = &lan9645x_phylink_pcs_ops;
		lan9645x->ports[port] = p;
	}

	err = lan9645x_parse_ports_node(lan9645x);
	if (err) {
		dev_err(dev, "Lan9645x setup: failed to parse ports node.");
		return err;
	}

	err = lan9645x_vcap_init(lan9645x);
	if (err) {
		dev_err(dev, "Lan9645x setup: failed to setup VCAP.\n");
		return err;
	}

	err = lan9645x_streamt_init(lan9645x);
	if (err) {
		dev_err(dev, "Lan9645x setup: failed to setup stream table err: %d\n", err);
		return err;
	}

	lan9645x->ptp_irq = platform_get_irq_byname(to_platform_device(dev),
						    "lan9645x-ptp");
	lan9645x->ptp = !!(lan9645x->ptp_irq > 0);
	if (lan9645x->ptp) {
		lan9645x->ptp_ext_irq = platform_get_irq_byname(to_platform_device(dev),
								"lan9645x-ptp-ext");
	}

	err = lan9645x_tag_npi_setup(ds);
	if (err) {
		dev_err(dev, "Lan9645x setup: failed to setup NPI port.\n");
		return err;
	}

	INIT_LIST_HEAD(&lan9645x->link_isdx);
	mutex_init(&lan9645x->link_isdx_lock);
	mutex_init(&lan9645x->psfp_lock);
	mutex_init(&lan9645x->esdx_lock);
	mutex_init(&lan9645x->tx_lock);
	lan9645x_mac_init(lan9645x);
	lan9645x_vlan_init(lan9645x);
	err = lan9645x_qos_init(lan9645x);
	if (err)
		return dev_err_probe(dev, err, "QOS init error");
	lan9645x_mdb_init(lan9645x);
	err = lan9645x_ptp_init(lan9645x);
	if (err)
		return dev_err_probe(dev, err, "PTP init error");
	lan9645x_hsr_prp_init(lan9645x);

	/* ESDX index 0 is not useful and counts as no-esdx, similar to ISDX */
	set_bit(0, lan9645x->esdx_mask);
	lan9645x_fp_init(lan9645x);

	err = lan9645x_bum_init(lan9645x);
	if (err)
		return dev_err_probe(dev, err, "BUM init error");

	lan9645x_afi_init(lan9645x);
	lan9645x_mrp_init(lan9645x);

	/* Link Aggregation Mode: NETDEV_LAG_HASH_L2 */
	lan_wr(ANA_AGGR_CFG_AC_SMAC_ENA |
	       ANA_AGGR_CFG_AC_DMAC_ENA,
	       lan9645x, ANA_AGGR_CFG);

	/* Flush queues */
	lan_wr(GENMASK(1, 0), lan9645x, QS_XTR_FLUSH);

	/* Allow to drain */
	mdelay(1);

	/* All Queues normal */
	lan_wr(0x0, lan9645x, QS_XTR_FLUSH);

	/* Set MAC age time to default value, the entry is aged after
	 * 2 * AGE_PERIOD
	 */
	lan_wr(ANA_AUTOAGE_AGE_PERIOD_SET(BR_DEFAULT_AGEING_TIME / 2 / HZ),
	       lan9645x, ANA_AUTOAGE);

	/* Disable learning for frames discarded by VLAN ingress filtering */
	lan_rmw(ANA_ADVLEARN_VLAN_CHK_SET(1),
		ANA_ADVLEARN_VLAN_CHK,
		lan9645x, ANA_ADVLEARN);

	/* Queue system frame ageing. We target 2s ageing.
	 *
	 * Register unit is 1024 cycles.
	 *
	 * ASIC: 165.625 Mhz  ~ 6.0377 ns period
	 * FPGA:  66.125 Mhz  ~ 15.125ns period
	 *
	 * 1024 * 6.0377 ns =~ 6182 ns
	 * val = 2000000000ns / 6182ns
	 */
	lan_wr(SYS_FRM_AGING_AGE_TX_ENA_SET(1) |
	       SYS_FRM_AGING_MAX_AGE_SET((2000000000 / 6182)),
	       lan9645x,  SYS_FRM_AGING);

	/* Map the 8 CPU extraction queues to CPU port 9 (datasheet is wrong) */
	lan_wr(0, lan9645x, QSYS_CPU_GROUP_MAP);

	/* Configure second cpu port (chip_port 10) for manual frame injection.
	 * The AFI can not inject frames via the NPI port, unless frame aging is
	 * disabled on frontports, so we use manual injection for AFI frames.
	 */

	/* Set min-spacing of EOF to SOF on injected frames to 0, on cpu device
	 * 1. This is required when injecting with IFH.
	 * Default values emulates delay of std preamble/IFG setting on a front
	 * port.
	 */
	lan_rmw(QS_INJ_CTRL_GAP_SIZE_SET(0),
		QS_INJ_CTRL_GAP_SIZE,
		lan9645x, QS_INJ_CTRL(1));

	/* Injection: Mode: manual injection | Byte_swap */
	lan_wr(QS_INJ_GRP_CFG_MODE_SET(1) |
	       QS_INJ_GRP_CFG_BYTE_SWAP_SET(1),
	       lan9645x, QS_INJ_GRP_CFG(1));

	lan_rmw(QS_INJ_CTRL_GAP_SIZE_SET(0),
		QS_INJ_CTRL_GAP_SIZE,
		lan9645x, QS_INJ_CTRL(1));

	lan_wr(SYS_PORT_MODE_INCL_INJ_HDR_SET(1),
	       lan9645x, SYS_PORT_MODE(CPU_PORT+1));

	/* Setup flooding PGIDs for IPv4/IPv6 multicast. Control and dataplane
	 * use the same masks. Control frames are redirected to CPU, and
	 * the network stack is responsible for forwarding these.
	 * The dataplane is forwarding according to the offloaded MDB entries.
	 *
	 * In DSA it is not currently possible to know if the bridge is in
	 * snooping mode, and we default to the igmp_snooping 1 behaviour.
	 */
	lan_wr(ANA_FLOODING_IPMC_FLD_MC4_DATA_SET(PGID_MCIPV4) |
	       ANA_FLOODING_IPMC_FLD_MC4_CTRL_SET(PGID_MC) |
	       ANA_FLOODING_IPMC_FLD_MC6_DATA_SET(PGID_MCIPV6) |
	       ANA_FLOODING_IPMC_FLD_MC6_CTRL_SET(PGID_MC),
	       lan9645x, ANA_FLOODING_IPMC);

	/* There are 8 priorities */
	for (int prio = 0; prio < 8; ++prio)
		lan_wr(ANA_FLOODING_FLD_MULTICAST_SET(PGID_MC) |
		       ANA_FLOODING_FLD_UNICAST_SET(PGID_UC) |
		       ANA_FLOODING_FLD_BROADCAST_SET(PGID_BC),
		       lan9645x, ANA_FLOODING(prio));

	/* Set all the entries to obey VLAN_VLAN. */
	for (int i = 0; i < PGID_ENTRIES; ++i)
		lan_wr(ANA_PGID_CFG_OBEY_VLAN_SET(1),
		       lan9645x, ANA_PGID_CFG(i));

	/* Disable bridging by default */
	for (int p = 0; p < lan9645x->num_phys_ports; p++) {
		lan_wr(0, lan9645x, ANA_PGID(PGID_SRC + p));

		/* Do not forward BPDU frames to the front ports and copy them
		 * to CPU
		 */
		lan_wr(ANA_CPU_FWD_BPDU_CFG_BPDU_REDIR_ENA,
		       lan9645x, ANA_CPU_FWD_BPDU_CFG(p));
	}

	/* Set source buffer size for each priority and each port to 1700 bytes */
	for (int i = 0; i <= QSYS_Q_RSRV; ++i) {
		lan_wr(1700 / 64, lan9645x, QSYS_RES_CFG(i));
		lan_wr(1700 / 64, lan9645x, QSYS_RES_CFG(512 + i));
	}

	/* The CPU will only use its reserved buffer in the shared queue system
	 * and none of the shared buffer space, therefore we disable resource
	 * sharing in egress direction. We must not disable resource sharing in
	 * the ingress direction, because some traffic test scenarios require
	 * loads of buffer memory for frames initiated by the CPU.
	 */
	lan_rmw(QSYS_EGR_NO_SHARING_EGR_NO_SHARING_SET(BIT(CPU_PORT)),
		QSYS_EGR_NO_SHARING_EGR_NO_SHARING_SET(BIT(CPU_PORT)),
		lan9645x, QSYS_EGR_NO_SHARING);

	/* The CPU should also discard frames forwarded to it if it has run
	 * out of the reserved buffer space. Otherwise they will be held back
	 * in the ingress queues with potential head-of-line blocking effects.
	 */
	lan_rmw(QSYS_EGR_DROP_MODE_EGRESS_DROP_MODE_SET(BIT(CPU_PORT)),
		QSYS_EGR_DROP_MODE_EGRESS_DROP_MODE_SET(BIT(CPU_PORT)),
		lan9645x, QSYS_EGR_DROP_MODE);

	/* Configure and enable the CPU port */
	lan9645x_cpu_port_init(lan9645x);

	/* Multicast to all front ports */
	lan_wr(all_phys_ports, lan9645x, ANA_PGID(PGID_MC));

	/* Snooping on by default. This will be controlled by mrouter ports */
	lan_wr(0x0, lan9645x, ANA_PGID(PGID_MCIPV4));
	lan_wr(0x0, lan9645x, ANA_PGID(PGID_MCIPV6));

	/* Unicast to all front ports */
	lan_wr(all_phys_ports, lan9645x, ANA_PGID(PGID_UC));

	/* Broadcast to all front ports, CPU port BC controlled with mactable */
	lan_wr(all_phys_ports, lan9645x, ANA_PGID(PGID_BC));

	/* Transmit cpu frames as received without any tagging, timing or other
	 * updates
	 */
	lan_wr(REW_PORT_CFG_NO_REWRITE_SET(1),
	       lan9645x, REW_PORT_CFG(CPU_PORT));

	dsa_switch_for_each_available_port(dp, ds) {
		lan9645x_port_init(lan9645x, dp->index);
		lan9645x_police_port_init(lan9645x->ports[dp->index]);
	}

	dsa_switch_for_each_user_port(dp, ds) {
		lan9645x_igmp_snooping(lan9645x, true, dp->index);
	}

	err = lan9645x_stats_init(lan9645x);
	if (err) {
		dev_err(dev, "Lan9645x setup: failed to init stats.");
		return err;
	}

	lan9645x_set_tail_drop_wm(lan9645x);

	ds->mtu_enforcement_ingress = true;
	ds->assisted_learning_on_cpu_port = true;
	ds->fdb_isolation = true;

	if (lan9645x->ptp) {
		err = devm_request_threaded_irq(dev, lan9645x->ptp_irq, NULL,
						lan9645x_ptp_irq_handler,
						IRQF_ONESHOT,
						"lan9645x ptp irq", lan9645x);
		if (err)
			return dev_err_probe(dev, err, "Unable to use ptp irq");

		if (lan9645x->ptp_ext_irq > 0) {
			err = devm_request_threaded_irq(dev,
							lan9645x->ptp_ext_irq,
							NULL,
							lan9645x_ptp_ext_irq_handler,
							IRQF_ONESHOT,
							"lan9645x ptp-ext irq",
							lan9645x);
			if (err)
				return dev_err_probe(dev, err,
						     "Unable to use ptp-ext irq");
		}
	}

	lan9645x->ana_irq = platform_get_irq_byname(to_platform_device(lan9645x->dev),
						    "lan9645x-ana");
	if (lan9645x->ana_irq > 0) {
		err = devm_request_threaded_irq(lan9645x->dev, lan9645x->ana_irq,
						NULL, lan9645x_ana_irq_handler,
						IRQF_ONESHOT, "lan9645x ana irq",
						lan9645x);
		if (err)
			return dev_err_probe(lan9645x->dev, err,
					     "Unable to use ana irq");
	}

	lan9645x_taprio_init(lan9645x);

	err = lan9645x_netlink_qos_init(lan9645x);
	if (err) {
		dev_err(dev, "Failed to init QOS netlink api. err=%d", err);
		return err;
	}

	err = lan9645x_netlink_frer_init(lan9645x);
	if (err) {
		dev_err(dev, "Failed to init FRER netlink api. err=%d", err);
		return err;
	}

	err = lan9645x_netlink_fp_init(lan9645x);
	if (err) {
		dev_err(dev, "Failed to init Frame Preemption netlink api. err=%d", err);
		return err;
	}

	dev_info(lan9645x->dev,
		 "Setup complete. SKU features: tsn_dis=%d hsr_dis=%d max_ports=%d",
		 lan9645x->tsn_dis, lan9645x->dd_dis,
		 lan9645x->num_phys_ports - lan9645x->num_port_dis);

	return 0;
}

static int lan9645x_port_set_mac_address(struct dsa_switch *ds, int port,
					 const unsigned char *addr)
{
	struct lan9645x *lan9645x = ds->priv;
	/* Not allowed to program hardware off of this callback. Only veto. */

	dev_dbg(lan9645x->dev, "port=%d addr=%pM\n", port, addr);

	return 0;
}

/* Translate the DSA database API into the lan9645x switch library API,
 * which uses VID 4095 for all ports that aren't part of a bridge,
 * and expects the bridge_dev to be NULL in that case.
 */
static struct net_device *lan9645x_classify_db(struct dsa_db db)
{
	switch (db.type) {
	case DSA_DB_PORT:
	case DSA_DB_LAG:
		return NULL;
	case DSA_DB_BRIDGE:
		return db.bridge.dev;
	default:
		return ERR_PTR(-EOPNOTSUPP);
	}
}

u16 lan9645x_vlan_unaware_pvid(struct lan9645x *lan9645x, struct net_device *bridge)
{
	/* Logic must reflect lan9645x_vlan_port_get_pvid */
	if (!bridge)
		return HOST_PVID;

	return UNAWARE_PVID;
}

void lan9645x_port_set_learning(struct lan9645x *lan9645x, int port, bool enabled)
{
	lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(enabled), ANA_PORT_CFG_LEARN_ENA,
		lan9645x, ANA_PORT_CFG(port));

	if (port < lan9645x->num_phys_ports)
		lan9645x->ports[port]->learn_ena = enabled;
}

static void lan9645x_port_fast_age(struct dsa_switch *ds, int port)
{
	struct lan9645x *lan9645x = ds->priv;
	int err;

	dev_dbg(lan9645x->dev, "port=%d", port);

	err = lan9645x_mact_flush(lan9645x, port);
	if (err)
		dev_err(ds->dev, "Error flushing MAC table on port %d err=%d",
			port, err);
}

static int lan9645x_fdb_dump(struct dsa_switch *ds, int port,
			     dsa_fdb_dump_cb_t *cb, void *data)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	return lan9645x_mact_dsa_dump(lan9645x, port, cb, data);
}

static int lan9645x_fdb_add(struct dsa_switch *ds, int port,
			    const unsigned char *addr, u16 vid,
			    struct dsa_db db)
{
	struct net_device *br = lan9645x_classify_db(db);
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct lan9645x *lan9645x = ds->priv;

	if (IS_ERR(br))
		return PTR_ERR(br);

	if (dsa_port_is_cpu(dp) && !br &&
	    dsa_fdb_present_in_other_db(ds, port, addr, vid, db))
		return 0;

	if (!vid)
		vid = lan9645x_vlan_unaware_pvid(lan9645x, br);

	if (dsa_port_is_cpu(dp))
		return lan9645x_mact_learn(lan9645x, PGID_CPU, addr, vid,
					   ENTRYTYPE_LOCKED);

	return lan9645x_mact_entry_add(lan9645x, port, addr, vid);
}

static int lan9645x_fdb_del(struct dsa_switch *ds, int port,
			    const unsigned char *addr, u16 vid,
			    struct dsa_db db)
{
	struct net_device *br = lan9645x_classify_db(db);
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct lan9645x *lan9645x = ds->priv;
	int err;

	if (IS_ERR(br))
		return PTR_ERR(br);

	if (dsa_port_is_cpu(dp) && !br &&
	    dsa_fdb_present_in_other_db(ds, port, addr, vid, db))
		return 0;

	if (!vid)
		vid = lan9645x_vlan_unaware_pvid(lan9645x, br);

	if (dsa_port_is_cpu(dp))
		return lan9645x_mact_forget(lan9645x, addr, vid,
					    ENTRYTYPE_LOCKED);

	err = lan9645x_mact_entry_del(lan9645x, port, addr, vid);
	if (err == -ENOENT) {
		dev_dbg(lan9645x->dev,
			"fdb not found port=%d addr=%pM vid=%u\n", port, addr,
			vid);
		return 0;
	}

	return err;
}

static int lan9645x_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	u32 age_secs = max(1, msecs / 1000 / 2);
	struct lan9645x *lan9645x = ds->priv;

	/* Entry is must suffer two aging scans before it is removed, so an
	 * entry is aged after 2*AGE_PERIOD, and the unit is in seconds.
	 * An age period of 0 disabled automatic aging.
	 */
	lan_rmw(ANA_AUTOAGE_AGE_PERIOD_SET(age_secs),
		ANA_AUTOAGE_AGE_PERIOD,
		lan9645x, ANA_AUTOAGE);
	return 0;
}

static int lan9645x_port_pre_bridge_flags(struct dsa_switch *ds, int port,
					  struct switchdev_brport_flags flags,
					  struct netlink_ext_ack *extack)
{
	if (flags.mask &
	    ~(BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD))
		return -EINVAL;

	return 0;
}

void lan9645x_port_pgid_set(struct lan9645x *lan9645x, u16 pgid,
			    int chip_port, bool enabled)
{
	u32 reg_msk, port_msk;

	WARN_ON(chip_port > CPU_PORT);

	port_msk = enabled ? BIT(chip_port) : 0;
	reg_msk = BIT(chip_port) & ANA_PGID_PGID;

	lan_rmw(port_msk, reg_msk, lan9645x, ANA_PGID(pgid));
}

static int lan9645x_port_bridge_flags(struct dsa_switch *ds, int port,
				      struct switchdev_brport_flags f,
				      struct netlink_ext_ack *extack)
{
	struct lan9645x *l = ds->priv;

	if (WARN_ON(port == l->npi))
		return -EINVAL;

	if (f.mask & BR_LEARNING)
		lan9645x_port_set_learning(l, port, !!(f.val & BR_LEARNING));

	if (f.mask & BR_FLOOD)
		lan9645x_port_pgid_set(l, PGID_UC, port, !!(f.val & BR_FLOOD));

	if (f.mask & BR_MCAST_FLOOD)
		lan9645x_port_pgid_set(l, PGID_MC, port,
				       !!(f.val & BR_MCAST_FLOOD));

	if (f.mask & BR_BCAST_FLOOD)
		lan9645x_port_pgid_set(l, PGID_BC, port,
				       !!(f.val & BR_BCAST_FLOOD));

	return 0;
}

void lan9645x_update_fwd_mask(struct lan9645x *lan9645x, bool joining)
{
	struct lan9645x_port *p;
	int port;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	if (joining)
		lan9645x_cut_through_fwd(lan9645x);

	/* Updates the source port PGIDs, making sure frames from p
	 * are only forwarded to ports q != p, where q is relevant to forward
	 */
	lan9645x_for_each_port(lan9645x, port, p) {
		struct lan9645x_port *shadow_of;
		u32 mask = 0;

		shadow_of = lan9645x_port_shadow_of(p);

		if (lan9645x_port_is_bridged(p) &&
		    (lan9645x->bridge_fwd_mask & BIT(p->chip_port))) {
			mask = lan9645x->bridge_mask &
			       lan9645x->bridge_fwd_mask & ~BIT(p->chip_port);

			if (p->bond)
				mask &= ~lan9645x_lag_dev_get_mask(lan9645x,
								   p->bond);
		} else if (lan9645x_port_is_hsr(p)) {
			mask = lan9645x_hsr_prp_dev_get_mask(lan9645x, p->hsr) &
			       ~BIT(p->chip_port);
		} else if (shadow_of) {
			mask = lan9645x_hsr_prp_dev_get_mask(lan9645x,
							     shadow_of->hsr) &
				~BIT(p->chip_port);
		}

		lan_wr(mask, lan9645x, ANA_PGID(PGID_SRC + port));
	}

	if (!joining)
		lan9645x_cut_through_fwd(lan9645x);
}

static void __lan9645x_port_set_host_flood(struct lan9645x *lan9645x, int port,
					   bool uc, bool mc)
{
	bool mc_ena, uc_ena;

	lockdep_assert_held(&lan9645x->fwd_domain_lock);

	/* We want promiscuous and all_multi to affect standalone ports, for
	 * debug and test purposes.
	 *
	 * However, the linux bridge is incredibly eager to put bridged ports in
	 * promiscuous mode.
	 *
	 * This is unfortunate since lan9645x flood masks are global and not per
	 * ingress port. When some port triggers unknown uc/mc to the CPU, the
	 * traffic from any port is forwarded to the CPU.
	 *
	 * If the host CPU is weak, this can cause tremendous stress. Therefore,
	 * we compromise by ignoring this host flood request for bridged ports.
	 */
	if (lan9645x_port_is_bridged(lan9645x_to_port(lan9645x, port)))
		return;

	if (uc)
		lan9645x->host_flood_uc_mask |= BIT(port);
	else
		lan9645x->host_flood_uc_mask &= ~BIT(port);

	if (mc)
		lan9645x->host_flood_mc_mask |= BIT(port);
	else
		lan9645x->host_flood_mc_mask &= ~BIT(port);

	uc_ena = !!lan9645x->host_flood_uc_mask;
	lan9645x_port_pgid_set(lan9645x, PGID_UC, CPU_PORT, uc_ena);

	mc_ena = !!lan9645x->host_flood_mc_mask;
	lan9645x_port_pgid_set(lan9645x, PGID_MC, CPU_PORT, mc_ena);
	lan9645x_port_pgid_set(lan9645x, PGID_MCIPV4, CPU_PORT, mc_ena);
	lan9645x_port_pgid_set(lan9645x, PGID_MCIPV6, CPU_PORT, mc_ena);
}

static void lan9645x_host_flood_work_fn(struct work_struct *work)
{
	struct lan9645x_host_flood_work *w =
		container_of(work, struct lan9645x_host_flood_work, work);

	mutex_lock(&w->lan9645x->fwd_domain_lock);
	__lan9645x_port_set_host_flood(w->lan9645x, w->port, w->uc, w->mc);
	mutex_unlock(&w->lan9645x->fwd_domain_lock);
	kfree(w);
}

/* Called in atomic context */
static void lan9645x_port_set_host_flood(struct dsa_switch *ds, int port,
					 bool uc, bool mc)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_host_flood_work *w;

	w = kzalloc(sizeof(*w), GFP_ATOMIC);
	if (!w)
		return;

	INIT_WORK(&w->work, lan9645x_host_flood_work_fn);
	w->lan9645x = lan9645x;
	w->port = port;
	w->uc = uc;
	w->mc = mc;
	schedule_work(&w->work);
}

static int lan9645x_port_bridge_join(struct dsa_switch *ds, int port,
				     struct dsa_bridge bridge,
				     bool *tx_fwd_offload,
				     struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *lan9645x_port = lan9645x->ports[port];

	dev_dbg(lan9645x->dev, "port_bridge_join port=%d\n", port);

	if (lan9645x->bridge && lan9645x->bridge != bridge.dev) {
		NL_SET_ERR_MSG_MOD(extack, "Only one bridge supported");
		return -EBUSY;
	}

	mutex_lock(&lan9645x->fwd_domain_lock);

	if (!lan9645x->bridge_mask)
		lan9645x->bridge = bridge.dev;

	/* The bridge puts ports in IFF_ALLMULTI before calling
	 * port_bridge_join, so clean up before the port is marked as bridged.
	 */
	__lan9645x_port_set_host_flood(lan9645x, port, false, false);
	lan9645x->bridge_mask |= BIT(lan9645x_port->chip_port);

	mutex_unlock(&lan9645x->fwd_domain_lock);

	/* stp_state_set updates forwarding */

	return 0;
}

void lan9645x_port_stp_state_set(struct lan9645x *lan9645x, int port,
				 u8 state)
{
	struct lan9645x_port *p = lan9645x->ports[port];
	bool learn_ena;

	mutex_lock(&lan9645x->fwd_domain_lock);

	p->stp_state = state;

	if (state == BR_STATE_FORWARDING)
		lan9645x->bridge_fwd_mask |= BIT(p->chip_port);
	else
		lan9645x->bridge_fwd_mask &= ~BIT(p->chip_port);

	learn_ena =
		(state == BR_STATE_LEARNING || state == BR_STATE_FORWARDING) &&
		p->learn_ena;

	lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(learn_ena),
		ANA_PORT_CFG_LEARN_ENA, lan9645x,
		ANA_PORT_CFG(p->chip_port));

	dev_dbg(lan9645x->dev, "port=%d state=%u fwd_mask=0x%x\n", port, state,
		lan9645x->bridge_fwd_mask);
	lan9645x_update_fwd_mask(lan9645x, state == BR_STATE_FORWARDING);
	mutex_unlock(&lan9645x->fwd_domain_lock);
}

static void lan9645x_port_bridge_stp_state_set(struct dsa_switch *ds, int port,
					       u8 state)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_port_stp_state_set(lan9645x, port, state);
}


static void lan9645x_port_bridge_leave(struct dsa_switch *ds, int port,
				       struct dsa_bridge bridge)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *lan9645x_port = lan9645x->ports[port];

	dev_dbg(lan9645x->dev, "port_bridge_leave port=%d\n", port);

	mutex_lock(&lan9645x->fwd_domain_lock);

	lan9645x->bridge_mask &= ~BIT(lan9645x_port->chip_port);

	if (!lan9645x->bridge_mask)
		lan9645x->bridge = NULL;

	lan9645x_vlan_set_hostmode(lan9645x_port);
	lan9645x_update_fwd_mask(lan9645x, false);

	mutex_unlock(&lan9645x->fwd_domain_lock);
}

static int lan9645x_port_vlan_filtering(struct dsa_switch *ds, int port,
					bool enabled,
					struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p = lan9645x->ports[port];

	dev_dbg(lan9645x->dev, "port=%d enabled=%u\n", port, enabled);
	lan9645x_vlan_port_set_vlan_aware(p, enabled);
	lan9645x_vlan_port_apply(p);

	return 0;
}

static int lan9645x_port_vlan_add(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_vlan *vlan,
				  struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p = lan9645x->ports[port];
	bool pvid, untagged;
	int err;

	pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;

	dev_dbg(lan9645x->dev,
		"port=%d vid=%u pvid=%u untagged=%u changed=%d\n",
		port, vlan->vid, pvid, untagged, vlan->changed);

	err = lan9645x_port_vlan_prepare(p, vlan->vid, pvid, untagged, extack);
	if (err)
		return err;

	if (port == lan9645x->npi) {
		lan9645x_vlan_cpu_set_vlan(lan9645x, vlan->vid);
		lan9645x_mac_bc_flood_add(lan9645x, vlan->vid);
	}

	lan9645x_vlan_port_add_vlan(p, vlan->vid, pvid, untagged);

	return 0;
}

static int lan9645x_port_vlan_del(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_vlan *vlan)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p = lan9645x->ports[port];

	dev_dbg(lan9645x->dev, "port=%d vid=%u changed=%u flags=0x%x\n", port,
		vlan->vid, vlan->changed, vlan->flags);

	if (port == lan9645x->npi) {
		lan9645x_vlan_cpu_clear_vlan(lan9645x, vlan->vid);
		lan9645x_mac_bc_flood_del(lan9645x, vlan->vid);
	}

	lan9645x_vlan_port_del_vlan(p, vlan->vid);

	return 0;
}

static int lan9645x_lag_join(struct dsa_switch *ds, int port,
			     struct dsa_lag lag,
			     struct netdev_lag_upper_info *info,
			     struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p = lan9645x->ports[port];
	int old_lag_id, new_lag_id;
	int err;

	err = lan9645x_lag_join_prepare(lan9645x, info, extack);
	if (err)
		return err;

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	mutex_lock(&lan9645x->fwd_domain_lock);

	err = lan9645x_lag_apply_hash_type(lan9645x, info, extack);
	if (err) {
		mutex_unlock(&lan9645x->fwd_domain_lock);
		return err;
	}

	old_lag_id = lan9645x_lag_dev_get_id(lan9645x, lag.dev);
	p->bond = lag.dev;
	p->hash_type = info->hash_type;
	new_lag_id = lan9645x_lag_reconfigure(lan9645x, lag.dev, port, false);

	/* We could skip all migration on lag join. It does not matter
	 * that an entry is learned on a bond port != lag_id. On lag leave we
	 * would just have to migrate any entries remaining on the leaving port,
	 * to the lag_id, and flush it's dynamic entries.
	 *
	 * With that approach the bond mac entries in HW would point to some
	 * bond port, but not necessarily the lag_id.
	 *
	 * The current approach makes sure all static entries point to the
	 * current lag_id.
	 *
	 * We are keeping dynamic entries for the old_lag_id in HW without
	 * migration. New macs on the bond1 are learned on the new_lag_id,
	 * but doing fdb_dump will show old entries on old_lag_id until
	 * they are relearned or aged out.
	 */
	if (old_lag_id >= 0 && old_lag_id != new_lag_id)
		lan9645x_migrate_lag_fdb(lan9645x, lag.dev, old_lag_id,
					 new_lag_id);

	mutex_unlock(&lan9645x->fwd_domain_lock);

	return 0;
}

static int lan9645x_lag_leave(struct dsa_switch *ds, int port,
			      struct dsa_lag lag)
{
	struct lan9645x *lan9645x = ds->priv;
	int old_lag_id, new_lag_id;

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	mutex_lock(&lan9645x->fwd_domain_lock);

	old_lag_id = lan9645x_lag_dev_get_id(lan9645x, lag.dev);
	lan9645x->ports[port]->bond = NULL;
	lan9645x->ports[port]->hash_type = NETDEV_LAG_HASH_NONE;

	new_lag_id = lan9645x_lag_reconfigure(lan9645x, lag.dev, port, true);

	/* When the last port leaves a LAG, DSA will flush the entries for us.
	 * The sequence is something like:
	 *
	 * 1) lag_change tx_enabled=0
	 * 2) lag_fdb_del all static entries from lag_fdb_add
	 * 3) bridge_leave (if bridged)
	 * 4) port_fast_age (flushes learned entries)
	 * 5) lag_leave
	 * 6) link down sequence
	 *
	 * NOTE: This will clear our hw, but the static entries will remain in
	 * software as offload static. Fear not - if you add a port back to the
	 * bond, DSA will kindly call you with lag_fdb_add
	 */
	if (new_lag_id >= 0 && old_lag_id != new_lag_id)
		lan9645x_migrate_lag_fdb(lan9645x, lag.dev, old_lag_id,
					 new_lag_id);

	mutex_unlock(&lan9645x->fwd_domain_lock);

	return 0;
}

static int lan9645x_lag_change(struct dsa_switch *ds, int port)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p;
	u32 bond_mask;

	dev_dbg(lan9645x->dev, "port=%d lag_tx_enabled=%d\n", port,
		dp->lag_tx_enabled);

	p = lan9645x->ports[port];

	mutex_lock(&lan9645x->fwd_domain_lock);

	bond_mask = lan9645x_lag_dev_get_mask(lan9645x, p->bond);
	p->lag_tx_active = dp->lag_tx_enabled;
	lan9645x_lag_port_set_pgids(lan9645x, port, false, bond_mask);

	mutex_unlock(&lan9645x->fwd_domain_lock);

	return 0;
}

static int lan9645x_lag_fdb_add(struct dsa_switch *ds, struct dsa_lag lag,
				const unsigned char *addr, u16 vid,
				struct dsa_db db)
{
	struct net_device *br = lan9645x_classify_db(db);
	struct lan9645x *lan9645x = ds->priv;
	int lag_port;

	if (IS_ERR(br))
		return PTR_ERR(br);

	mutex_lock(&lan9645x->fwd_domain_lock);
	lag_port = lan9645x_lag_dev_get_id(lan9645x, lag.dev);
	mutex_unlock(&lan9645x->fwd_domain_lock);

	dev_dbg(lan9645x->dev, "mac=%pM vid=%u lag_id=%d\n", addr, vid,
		lag_port);

	if (lag_port < 0)
		return 0;

	if (!vid)
		vid = lan9645x_vlan_unaware_pvid(lan9645x, br);

	return lan9645x_mact_entry_add(lan9645x, lag_port, addr, vid);
}

static int lan9645x_lag_fdb_del(struct dsa_switch *ds, struct dsa_lag lag,
				const unsigned char *addr, u16 vid,
				struct dsa_db db)
{
	struct net_device *br = lan9645x_classify_db(db);
	struct lan9645x *lan9645x = ds->priv;
	int lag_id;
	int err;

	if (IS_ERR(br))
		return PTR_ERR(br);

	mutex_lock(&lan9645x->fwd_domain_lock);
	lag_id = lan9645x_lag_dev_get_id(lan9645x, lag.dev);
	mutex_unlock(&lan9645x->fwd_domain_lock);

	dev_dbg(lan9645x->dev, "mac=%pM vid=%u lag_id=%d\n", addr, vid, lag_id);

	if (lag_id < 0)
		return -ENOENT;

	if (!vid)
		vid = lan9645x_vlan_unaware_pvid(lan9645x, br);

	err = lan9645x_mact_entry_del(lan9645x, lag_id, addr, vid);
	if (err == -ENOENT) {
		dev_dbg(lan9645x->dev, "fdb not found mac %pM vid %u pgid %u\n",
			addr, vid, lag_id);
		return 0;
	}

	return err;
}

static int lan9645x_mdb_add(struct dsa_switch *ds, int port,
			    const struct switchdev_obj_port_mdb *mdb,
			    struct dsa_db db)
{
	struct net_device *bridge_dev = lan9645x_classify_db(db);
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d vid=%u addr=%pM\n", port, mdb->vid,
		mdb->addr);

	if (IS_ERR(bridge_dev))
		return PTR_ERR(bridge_dev);

	if (dsa_is_cpu_port(ds, port) && !bridge_dev &&
	    dsa_mdb_present_in_other_db(ds, port, mdb, db))
		return 0;

	if (port == lan9645x->npi)
		port = CPU_PORT;

	return lan9645x_mdb_port_add(lan9645x, port, mdb, bridge_dev);
}

static int lan9645x_mdb_del(struct dsa_switch *ds, int port,
			    const struct switchdev_obj_port_mdb *mdb,
			    struct dsa_db db)
{
	struct net_device *bridge_dev = lan9645x_classify_db(db);
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d vid=%u addr=%pM\n", port, mdb->vid,
		mdb->addr);

	if (IS_ERR(bridge_dev))
		return PTR_ERR(bridge_dev);

	if (dsa_is_cpu_port(ds, port) && !bridge_dev &&
	    dsa_mdb_present_in_other_db(ds, port, mdb, db))
		return 0;

	if (port == lan9645x->npi)
		port = CPU_PORT;

	return lan9645x_mdb_port_del(lan9645x, port, mdb, bridge_dev);
}

static void lan9645x_get_strings(struct dsa_switch *ds, int port, u32 stringset,
				 uint8_t *data)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_strings(lan9645x, port, stringset, data);
}

static void lan9645x_get_ethtool_stats(struct dsa_switch *ds, int port,
				       uint64_t *data)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_ethtool_stats(lan9645x, port, data);
}

static int lan9645x_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct lan9645x *lan9645x = ds->priv;

	return lan9645x_stats_get_sset_count(lan9645x, port, sset);
}

static void lan9645x_get_eth_mac_stats(struct dsa_switch *ds, int port,
				       struct ethtool_eth_mac_stats *mac_stats)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_eth_mac_stats(lan9645x, port, mac_stats);
}

static void
lan9645x_get_rmon_stats(struct dsa_switch *ds, int port,
			struct ethtool_rmon_stats *rmon_stats,
			const struct ethtool_rmon_hist_range **ranges)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_rmon_stats(lan9645x, port, rmon_stats, ranges);
}

static void lan9645x_get_stats64(struct dsa_switch *ds, int port,
				 struct rtnl_link_stats64 *s)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_stats64(lan9645x, port, s);
}

static void lan9645x_get_pause_stats(struct dsa_switch *ds, int port,
				     struct ethtool_pause_stats *pause_stats)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_pause_stats(lan9645x, port, pause_stats);
}

static void lan9645x_get_mm_stats(struct dsa_switch *ds, int port,
				  struct ethtool_mm_stats *stats)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_mm_stats(lan9645x, port, stats);
}

static void lan9645x_get_eth_phy_stats(struct dsa_switch *ds, int port,
				       struct ethtool_eth_phy_stats *phy_stats)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_eth_phy_stats(lan9645x, port, phy_stats);
}

static void
lan9645x_get_eth_ctrl_stats(struct dsa_switch *ds, int port,
			    struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct lan9645x *lan9645x = ds->priv;

	lan9645x_stats_get_eth_ctrl_stats(lan9645x, port, ctrl_stats);
}

static int lan9645x_port_hsr_join(struct dsa_switch *ds, int port,
				  struct net_device *hsr,
				  struct netlink_ext_ack *extack)
{
	struct dsa_port *dslrea = NULL, *dslreb, *dp;
	struct lan9645x *lan9645x = ds->priv;
	enum lan9645x_hsr_type type;
	int err;

	if (lan9645x->dd_dis)
		return -EOPNOTSUPP;

	dev_dbg(lan9645x->dev, "port=%d", port);

	err = lan9645x_hsr2type(hsr, &type);
	if (err)
		return err;

	err = lan9645x_hsr_prp_prepare(lan9645x, port, hsr, type, extack);
	if (err)
		return err;

	dslreb = dsa_to_port(ds, port);
	if (!dslreb)
		return -ENOTSUPP;

	dsa_hsr_foreach_port(dp, ds, hsr)
	{
		if (dp->index != port) {
			dslrea = dp;
			break;
		}
	}

	/* We must match port A/B in HW, and rely on the order of calls to
	 * identify which dsa interface is A and B.
	 */
	if (!dslrea)
		return 0;

	if (!ether_addr_equal(dslrea->mac, dslreb->mac)) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "MAC on Slave 1 (Port A) and Slave 2 (Port B) must be equal. MAC_A=%pM MAC_B=%pM",
				       dslrea->mac, dslreb->mac);
		return -EINVAL;
	}

	dev_dbg(lan9645x->dev, "lan_a=%d lan_b=%d type=%d\n", dslrea->index,
		dslreb->index, type);

	err = lan9645x_hsr_prp_pair_add(lan9645x, lan9645x->ports[dslrea->index],
					lan9645x->ports[dslreb->index],
					dslrea->user, hsr, type);
	if (err) {
		dev_err(lan9645x->dev,
			"HSR/PRP pair add err=%d ds_a=%p ds_b=%p\n", err,
			dslrea->user, dslreb->user);
		return err;
	}

	return 0;
}

static int lan9645x_port_hsr_leave(struct dsa_switch *ds, int port,
				   struct net_device *hsr)
{
	struct lan9645x *lan9645x = ds->priv;

	if (lan9645x->dd_dis)
		return -EOPNOTSUPP;

	dev_dbg(lan9645x->dev, "port=%d", port);

	return lan9645x_hsr_prp_pair_del(lan9645x, port, hsr);
}

static int lan9645x_port_mirror_add(struct dsa_switch *ds, int from,
				    struct dsa_mall_mirror_tc_entry *mirror,
				    bool ingress,
				    struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev,
		"port=%d to_local_port=%u m.ingress=%u ingress=%u\n", from,
		mirror->to_local_port, mirror->ingress, ingress);

	return lan9645x_mirror_port_add(lan9645x, from, mirror->to_local_port,
					ingress, extack);
}

static void lan9645x_port_mirror_del(struct dsa_switch *ds, int from,
				     struct dsa_mall_mirror_tc_entry *mirror)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d to_local_port=%u m.ingress=%u\n", from,
		mirror->to_local_port, mirror->ingress);

	lan9645x_mirror_port_del(lan9645x, from, mirror->ingress);
}

static int lan9645x_port_policer_add(struct dsa_switch *ds, int port,
				     struct dsa_mall_policer_tc_entry *policer)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_policer pol = {
		.rate = div_u64(policer->rate_bytes_per_sec, 1000) * 8,
		.burst = policer->burst,
	};

	dev_dbg(lan9645x->dev, "port=%d rate=%llu burst=%u\n", port,
		policer->rate_bytes_per_sec, policer->burst);

	return lan9645x_police_port_add(lan9645x, port, &pol);
}

static void lan9645x_port_policer_del(struct dsa_switch *ds, int port)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d\n", port);

	lan9645x_police_port_del(lan9645x, port);
}

static int lan9645x_cls_matchall_add(struct dsa_switch *ds, int port,
				     struct tc_cls_matchall_offload *cls,
				     bool ingress)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev,
		"%s port=%d cmd=%d chain=%u prio=%u proto=%x\n",
		__func__, port, cls->command, cls->common.chain_index,
		cls->common.prio, ntohs(cls->common.protocol));

	return lan9645x_tc_matchall_goto_add(p, cls);
}

static void lan9645x_cls_matchall_del(struct dsa_switch *ds, int port,
				      struct tc_cls_matchall_offload *cls)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d cmd=%d chain=%u prio=%u proto=%x\n",
		port, cls->command, cls->common.chain_index,
		cls->common.prio, ntohs(cls->common.protocol));

	lan9645x_tc_matchall_goto_del(p, cls);
}

static int lan9645x_port_get_default_prio(struct dsa_switch *ds, int port)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d", port);

	return lan9645x_dcb_port_get_default_prio(lan9645x, port);
}

static int lan9645x_port_set_default_prio(struct dsa_switch *ds, int port,
					  u8 prio)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d prio=%u", port, prio);

	return lan9645x_dcb_port_set_default_prio(lan9645x, port, prio);
}

static int lan9645x_port_get_dscp_prio(struct dsa_switch *ds, int port, u8 dscp)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d dscp=%u", port, dscp);

	return lan9645x_dcb_port_get_dscp_prio(lan9645x, port, dscp);
}

static int lan9645x_port_add_dscp_prio(struct dsa_switch *ds, int port, u8 dscp,
				       u8 prio)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d dscp=%u prio=%u", port, dscp, prio);

	return lan9645x_dcb_add_dscp_prio(lan9645x, dscp, prio);
}

static int lan9645x_port_del_dscp_prio(struct dsa_switch *ds, int port, u8 dscp,
				       u8 prio)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d dscp=%u prio=%u", port, dscp, prio);

	return lan9645x_dcb_del_dscp_prio(lan9645x, dscp, prio);
}

static int lan9645x_cls_flower_add(struct dsa_switch *ds, int port,
				   struct flow_cls_offload *cls, bool ingress)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p;

	dev_dbg(lan9645x->dev,
		"port=%d ingress=%u cmd=%d chain=%u prio=%u proto=%x cookie=%lu",
		port, ingress, cls->command, cls->common.chain_index,
		cls->common.prio, ntohs(cls->common.protocol), cls->cookie);

	p = lan9645x_to_port(lan9645x, port);

	return lan9645x_tc_flower_add(p, cls, ingress);
}

static int lan9645x_cls_flower_del(struct dsa_switch *ds, int port,
				   struct flow_cls_offload *cls, bool ingress)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p;

	dev_dbg(lan9645x->dev,
		"port=%d ingress=%u cmd=%d chain=%u prio=%u proto=%x cookie=%lu",
		port, ingress, cls->command, cls->common.chain_index,
		cls->common.prio, ntohs(cls->common.protocol), cls->cookie);

	p = lan9645x_to_port(lan9645x, port);

	return lan9645x_tc_flower_del(p, cls, ingress);
}

static int lan9645x_cls_flower_stats(struct dsa_switch *ds, int port,
				     struct flow_cls_offload *cls, bool ingress)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev,
		"port=%d ingress=%u cmd=%d chain=%u prio=%u proto=%x cookie=%lu",
		port, ingress, cls->command, cls->common.chain_index,
		cls->common.prio, ntohs(cls->common.protocol), cls->cookie);

	return lan9645x_tc_flower_stats(p, cls);
}

static int lan9645x_port_setup_cbs(struct dsa_switch *ds, int port,
				   struct tc_cbs_qopt_offload *cbs_qopt)
{
	struct lan9645x *lan9645x = ds->priv;

	if (cbs_qopt->queue >= ds->num_tx_queues)
		return -EINVAL;

	if (cbs_qopt->enable)
		return lan9645x_cbs_add(lan9645x, port, cbs_qopt);
	else
		return lan9645x_cbs_del(lan9645x, port, cbs_qopt);
}

static int lan9645x_port_setup_mqprio(struct dsa_switch *ds, int port,
				      struct tc_mqprio_qopt_offload *mqprio)
{
	struct lan9645x *lan9645x = ds->priv;
	int err;

	mutex_lock(&lan9645x->fwd_domain_lock);
	err = lan9645x_mqprio_set(lan9645x, port,  mqprio);
	mutex_unlock(&lan9645x->fwd_domain_lock);

	return err;
}

static int lan9645x_port_setup_tbf(struct dsa_switch *ds, int port,
				   struct tc_tbf_qopt_offload *qopt)
{
	struct lan9645x *lan9645x = ds->priv;

	switch (qopt->command) {
	case TC_TBF_REPLACE:
		return lan9645x_tbf_add(lan9645x, port, qopt);
	case TC_TBF_DESTROY:
		return lan9645x_tbf_del(lan9645x, port, qopt);
	default:
		return -EOPNOTSUPP;
	}

	return -EOPNOTSUPP;
}

static int lan9645x_port_setup_ets(struct dsa_switch *ds, int port,
				   struct tc_ets_qopt_offload *qopt)
{
	struct lan9645x *lan9645x = ds->priv;

	switch (qopt->command) {
	case TC_ETS_REPLACE:
		return lan9645x_ets_add(lan9645x, port, qopt);
	case TC_ETS_DESTROY:
		return lan9645x_ets_del(lan9645x, port, qopt);
	default:
		return -EOPNOTSUPP;
	}
}

static int lan9645x_tc_setup_qdisc_taprio(struct dsa_switch *ds, int port,
					  struct tc_taprio_qopt_offload *taprio)
{
	struct lan9645x *lan9645x = ds->priv;

	switch (taprio->cmd) {
	case TAPRIO_CMD_REPLACE:
		return lan9645x_taprio_add(lan9645x, port, taprio);
	case TAPRIO_CMD_DESTROY:
		return lan9645x_taprio_del(lan9645x, port);
	default:
		return -EOPNOTSUPP;
	}
}

static int lan9645x_port_setup_tc(struct dsa_switch *ds, int port,
				  enum tc_setup_type type, void *type_data)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d type=%d\n", port, type);

	switch (type) {
	case TC_SETUP_QDISC_MQPRIO:
		return lan9645x_port_setup_mqprio(ds, port, type_data);
	case TC_SETUP_QDISC_CBS:
		return lan9645x_port_setup_cbs(ds, port, type_data);
	case TC_SETUP_QDISC_TBF:
		return lan9645x_port_setup_tbf(ds, port, type_data);
	case TC_SETUP_QDISC_ETS:
		return lan9645x_port_setup_ets(ds, port, type_data);
	case TC_SETUP_QDISC_TAPRIO:
		if (lan9645x->tsn_dis)
			return -ENOTSUPP;
		return lan9645x_tc_setup_qdisc_taprio(ds, port, type_data);
	/* BLOCK and FT handled by dsa */
	default:
		return -ENOTSUPP;
	}
}

static int lan9645x_port_set_mac_eee(struct dsa_switch *ds, int port,
				     struct ethtool_keee *e)
{
	struct lan9645x *lan9645x = ds->priv;

	return lan9645x_eee_mac_set(lan9645x, port, e);
}

static bool lan9645x_port_support_eee(struct dsa_switch *ds, int port)
{
	return true;
}

static int lan9645x_port_set_apptrust(struct dsa_switch *ds, int port,
				      const u8 *sel, int nsel)
{
	struct lan9645x *lan9645x = ds->priv;

	return lan9645x_dcb_port_set_apptrust(lan9645x, port, sel, nsel);
}

static int lan9645x_port_get_apptrust(struct dsa_switch *ds, int port, u8 *sel,
				      int *nsel)
{
	struct lan9645x *lan9645x = ds->priv;

	return lan9645x_dcb_port_get_apptrust(lan9645x, port, sel, nsel);
}

static int lan9645x_port_get_pcp_dei_prio(struct dsa_switch *ds, int port,
					  u8 pcp, u8 dei)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d pcp=%u dei=%u", port, pcp, dei);

	return lan9645x_dcb_get_pcp_dei_prio(lan9645x, port, pcp, dei);
}

static int lan9645x_port_add_pcp_dei_prio(struct dsa_switch *ds, int port,
					  u8 pcp, u8 dei, u8 prio)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d pcp=%u dei=%u prio=%u", port, pcp, dei,
		prio);

	return lan9645x_dcb_add_pcp_dei_prio(lan9645x, port, pcp, dei, prio);
}

static int lan9645x_port_del_pcp_dei_prio(struct dsa_switch *ds, int port,
					  u8 pcp, u8 dei, u8 prio)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d pcp=%u dei=%u prio=%u", port, pcp, dei,
		prio);

	return lan9645x_dcb_del_pcp_dei_prio(lan9645x, port, pcp, dei, prio);
}

static int lan9645x_port_get_pfc(struct dsa_switch *ds, int port,
				 struct ieee_pfc *pfc)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d", port);

	return lan9645x_dcb_getpfc(lan9645x, port, pfc);
}

static int lan9645x_port_set_pfc(struct dsa_switch *ds, int port,
				 struct ieee_pfc *pfc)
{
	struct lan9645x *lan9645x = ds->priv;

	dev_dbg(lan9645x->dev, "port=%d pfc=%x", port, pfc->pfc_en);

	return lan9645x_dcb_setpfc(lan9645x, port, pfc->pfc_en);
}

static int lan9645x_port_enable(struct dsa_switch *ds, int port, struct phy_device *phy)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p;

	p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d", port);

	phy_power_on(p->serdes);

	return 0;
}

static void lan9645x_port_disable(struct dsa_switch *ds, int port)
{
	struct lan9645x *lan9645x = ds->priv;
	struct lan9645x_port *p;

	p = lan9645x_to_port(lan9645x, port);

	dev_dbg(lan9645x->dev, "port=%d", port);

	phy_power_off(p->serdes);
}

static int lan9645x_port_mrouter_set(struct dsa_switch *ds, int port, bool enable)
{
	struct lan9645x *lan9645x = ds->priv;

	return lan9645x_mdb_port_mrouter_set(lan9645x, port, enable);
}

static int
lan9645x_port_hsr_node_add(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_node_hsr *hsr_node)
{
	struct lan9645x *lan9645x = ds->priv;

	if (lan9645x->dd_dis)
		return -EOPNOTSUPP;

	dev_dbg(lan9645x->dev, "port=%d addrA=%pM\n",
		port, hsr_node->addr_A);

	return lan9645x_hsr_prp_dan_node_add(lan9645x, port,
					     hsr_node->hsr,
					     hsr_node->addr_A);
}

static int
lan9645x_port_hsr_node_del(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_node_hsr *hsr_node)
{
	struct lan9645x *lan9645x = ds->priv;

	if (lan9645x->dd_dis)
		return -EOPNOTSUPP;

	dev_dbg(lan9645x->dev, "port=%d addrA=%pM\n",
		 port, hsr_node->addr_A);

	lan9645x_hsr_prp_dan_node_del(lan9645x, port, hsr_node->hsr,
				      hsr_node->addr_A);
	return 0;
}

static int lan9645x_get_mm(struct dsa_switch *ds, int port,
			   struct ethtool_mm_state *state)
{
	struct lan9645x *lan9645x = ds->priv;

	if (lan9645x->tsn_dis)
		return -ENOTSUPP;

	return lan9645x_fp_ethtool_get_mm(lan9645x, port, state);
}

static int lan9645x_set_mm(struct dsa_switch *ds, int port,
			   struct ethtool_mm_cfg *cfg,
			   struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = ds->priv;

	if (lan9645x->tsn_dis)
		return -ENOTSUPP;

	return lan9645x_fp_ethtool_set_mm(lan9645x, port, cfg, extack);
}

static void lan9645x_port_mrp_update_mac(struct dsa_switch *ds, int port,
					 const unsigned char *br_addr)
{
	return lan9645x_mrp_port_update_mrp_mac(ds->priv, port, br_addr);
}

static int lan9645x_port_mrp_add(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_mrp *mrp)
{
	return lan9645x_handle_mrp_add_port(ds->priv, port, mrp);
}

static int lan9645x_port_mrp_del(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_mrp *mrp)
{
	return lan9645x_handle_mrp_del_port(ds->priv, port, mrp);
}

static int lan9645x_port_mrp_role(struct dsa_switch *ds, int port, u8 port_role)
{
	return lan9645x_handle_mrp_port_role(ds->priv, port, port_role);
}

static int
lan9645x_port_mrp_add_ring_role(struct dsa_switch *ds, int port,
				const struct switchdev_obj_ring_role_mrp *mrp)
{
	return lan9645x_handle_mrp_ring_role_add(ds->priv, port, mrp);
}

static int
lan9645x_port_mrp_del_ring_role(struct dsa_switch *ds, int port,
				const struct switchdev_obj_ring_role_mrp *mrp)
{
	return lan9645x_handle_mrp_ring_role_del(ds->priv, port, mrp);
}

static int lan9645x_port_mrp_add_ring_test(struct dsa_switch *ds, int port,
					   const struct switchdev_obj_ring_test_mrp *mrp)
{
	return lan9645x_handle_mrp_ring_test_add(ds->priv, port, mrp);
}

static int lan9645x_port_mrp_del_ring_test(struct dsa_switch *ds, int port,
					   const struct switchdev_obj_ring_test_mrp *mrp)
{
	return lan9645x_handle_mrp_ring_test_del(ds->priv, port, mrp);
}

static int
lan9645x_port_mrp_add_ring_state(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_ring_state_mrp *mrp)
{
	return lan9645x_handle_mrp_ring_state_add(ds->priv, port, mrp);
}

static int lan9645x_port_mrp_add_in_ring_state(struct dsa_switch *ds, int port,
					       const struct switchdev_obj_in_state_mrp *mrp)
{
	return lan9645x_handle_mrp_in_state_add(ds->priv, port, mrp);
}

static int
lan9645x_port_mrp_add_in_ring_test(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_in_test_mrp *mrp)
{
	return lan9645x_handle_mrp_in_test_add(ds->priv, port, mrp);
}

static int
lan9645x_port_mrp_del_in_ring_test(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_in_test_mrp *mrp)
{
	return lan9645x_handle_mrp_in_test_del(ds->priv, port, mrp);
}

static int
lan9645x_port_mrp_add_in_ring_role(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_in_role_mrp *mrp)
{
	return lan9645x_handle_mrp_in_role_add(ds->priv, port, mrp);
}

static int
lan9645x_port_mrp_del_in_ring_role(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_in_role_mrp *mrp)
{
	return lan9645x_handle_mrp_in_role_del(ds->priv, port, mrp);
}

static const struct dsa_switch_ops lan9645x_switch_ops = {
	.get_tag_protocol		= lan9645x_get_tag_protocol,
	.connect_tag_protocol		= lan9645x_connect_tag_protocol,

	.port_enable = lan9645x_port_enable,
	.port_disable = lan9645x_port_disable,

	.setup				= lan9645x_setup,
	.teardown			= lan9645x_teardown,

	/* Phylink integration */
	.phylink_get_caps		= lan9645x_port_phylink_get_caps,

	/* MTU  */
	.port_change_mtu		= lan9645x_change_mtu,
	.port_max_mtu			= lan9645x_get_max_mtu,

	/* Veto port MAC changes */
	.port_set_mac_address		= lan9645x_port_set_mac_address,

	/* Bridge integration */
	.set_ageing_time		= lan9645x_set_ageing_time,
	.port_pre_bridge_flags		= lan9645x_port_pre_bridge_flags,
	.port_bridge_flags		= lan9645x_port_bridge_flags,
	.port_bridge_join		= lan9645x_port_bridge_join,
	.port_bridge_leave		= lan9645x_port_bridge_leave,
	.port_stp_state_set		= lan9645x_port_bridge_stp_state_set,
	.port_set_host_flood		= lan9645x_port_set_host_flood,

	/* MAC table integration */
	.port_fast_age			= lan9645x_port_fast_age,
	.port_fdb_dump			= lan9645x_fdb_dump,
	.port_fdb_add			= lan9645x_fdb_add,
	.port_fdb_del			= lan9645x_fdb_del,

	/* VLAN integration */
	.port_vlan_filtering		= lan9645x_port_vlan_filtering,
	.port_vlan_add			= lan9645x_port_vlan_add,
	.port_vlan_del			= lan9645x_port_vlan_del,

	/* Link Aggregation Group integration */
	.port_lag_join			= lan9645x_lag_join,
	.port_lag_leave			= lan9645x_lag_leave,
	.port_lag_change		= lan9645x_lag_change,
	.lag_fdb_add			= lan9645x_lag_fdb_add,
	.lag_fdb_del			= lan9645x_lag_fdb_del,

	/* Multicast database */
	.port_mdb_add			= lan9645x_mdb_add,
	.port_mdb_del			= lan9645x_mdb_del,
	.port_mrouter_set		= lan9645x_port_mrouter_set,

	/* Port statistics counters. */
	.get_strings			= lan9645x_get_strings,
	.get_ethtool_stats		= lan9645x_get_ethtool_stats,
	.get_sset_count			= lan9645x_get_sset_count,
	.get_eth_mac_stats		= lan9645x_get_eth_mac_stats,
	.get_rmon_stats			= lan9645x_get_rmon_stats,
	.get_stats64			= lan9645x_get_stats64,
	.get_pause_stats		= lan9645x_get_pause_stats,
	.get_mm_stats			= lan9645x_get_mm_stats,
	.get_eth_phy_stats		= lan9645x_get_eth_phy_stats,
	.get_eth_ctrl_stats		= lan9645x_get_eth_ctrl_stats,

	/* HSR/PRP integration */
	.port_hsr_join			= lan9645x_port_hsr_join,
	.port_hsr_leave			= lan9645x_port_hsr_leave,
	.port_hsr_dan_node_add		= lan9645x_port_hsr_node_add,
	.port_hsr_dan_node_del		= lan9645x_port_hsr_node_del,

	/* TC integration */
	.port_mirror_add		= lan9645x_port_mirror_add,
	.port_mirror_del		= lan9645x_port_mirror_del,
	.port_policer_add		= lan9645x_port_policer_add,
	.port_policer_del		= lan9645x_port_policer_del,
	.cls_matchall_goto_add		= lan9645x_cls_matchall_add,
	.cls_matchall_goto_del		= lan9645x_cls_matchall_del,
	.cls_flower_add			= lan9645x_cls_flower_add,
	.cls_flower_del			= lan9645x_cls_flower_del,
	.cls_flower_stats		= lan9645x_cls_flower_stats,
	.port_setup_tc			= lan9645x_port_setup_tc,

	/* DCB integration */
	.port_get_default_prio		= lan9645x_port_get_default_prio,
	.port_set_default_prio		= lan9645x_port_set_default_prio,
	.port_get_dscp_prio		= lan9645x_port_get_dscp_prio,
	.port_add_dscp_prio		= lan9645x_port_add_dscp_prio,
	.port_del_dscp_prio		= lan9645x_port_del_dscp_prio,
	.port_set_apptrust		= lan9645x_port_set_apptrust,
	.port_get_apptrust		= lan9645x_port_get_apptrust,
	.port_getpfc			= lan9645x_port_get_pfc,
	.port_setpfc			= lan9645x_port_set_pfc,

	.port_get_pcp_dei_prio		= lan9645x_port_get_pcp_dei_prio,
	.port_add_pcp_dei_prio		= lan9645x_port_add_pcp_dei_prio,
	.port_del_pcp_dei_prio		= lan9645x_port_del_pcp_dei_prio,

	 /* MAC EEE settings */
	 .set_mac_eee			= lan9645x_port_set_mac_eee,
	 .support_eee			= lan9645x_port_support_eee,

	/*  ethtool timestamp info */
	.get_ts_info			= lan9645x_get_ts_info,

	/* PTP functionality */
	 .port_hwtstamp_get		= lan9645x_port_hwtstamp_get,
	 .port_hwtstamp_set		= lan9645x_port_hwtstamp_set,
	 .port_txtstamp			= lan9645x_txtstamp,
	 .port_rxtstamp			= lan9645x_rxtstamp_defer,
	 .port_rxtstamp_all		= lan9645x_rxtstamp_all_defer,

	 /* MAC merge */
	.get_mm				= lan9645x_get_mm,
	.set_mm				= lan9645x_set_mm,

	/*
	 * MRP integration
	 */
	.port_mrp_update_br_mac		= lan9645x_port_mrp_update_mac,
	.port_mrp_add			= lan9645x_port_mrp_add,
	.port_mrp_del			= lan9645x_port_mrp_del,
	.port_mrp_role			= lan9645x_port_mrp_role,
	.port_mrp_add_ring_role		= lan9645x_port_mrp_add_ring_role,
	.port_mrp_del_ring_role		= lan9645x_port_mrp_del_ring_role,
	.port_mrp_add_ring_test		= lan9645x_port_mrp_add_ring_test,
	.port_mrp_del_ring_test		= lan9645x_port_mrp_del_ring_test,
	.port_mrp_add_ring_state	= lan9645x_port_mrp_add_ring_state,
	.port_mrp_add_in_ring_state	= lan9645x_port_mrp_add_in_ring_state,
	.port_mrp_add_in_ring_test	= lan9645x_port_mrp_add_in_ring_test,
	.port_mrp_del_in_ring_test	= lan9645x_port_mrp_del_in_ring_test,
	.port_mrp_add_in_ring_role	= lan9645x_port_mrp_add_in_ring_role,
	.port_mrp_del_in_ring_role	= lan9645x_port_mrp_del_in_ring_role,

};

static int lan9645x_request_target_regmaps(struct lan9645x *lan9645x)
{
	struct regmap *tgt_map;

	for (int i = 0; i < NUM_TARGETS; i++) {
		tgt_map = lan9645x_request_regmap(lan9645x, i);
		if (IS_ERR(tgt_map)) {
			dev_err(lan9645x->dev,
				"Failed to get target regmap: %pe\n", tgt_map);
			return PTR_ERR(tgt_map);
		}

		lan9645x->rmap[i] = tgt_map;
	}

	return 0;
}

static void lan9645x_set_feat_dis(struct lan9645x *lan9645x)
{
	u32 feat_dis;

	feat_dis = lan_rd(lan9645x, GCB_FEAT_DISABLE);

	lan9645x->num_port_dis = GCB_FEAT_DISABLE_FEAT_NUM_PORTS_DIS_GET(feat_dis);
	lan9645x->dd_dis = GCB_FEAT_DISABLE_FEAT_DD_DIS_GET(feat_dis);
	lan9645x->tsn_dis = GCB_FEAT_DISABLE_FEAT_TSN_DIS_GET(feat_dis);
}

static int lan9645x_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lan9645x *lan9645x;
	struct dsa_switch *ds;
	int err = 0;

	lan9645x = devm_kzalloc(dev, sizeof(*lan9645x), GFP_KERNEL);
	if (!lan9645x)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate LAN9645X");

	dev_set_drvdata(dev, lan9645x);
	lan9645x->dev = dev;

	err = lan9645x_request_target_regmaps(lan9645x);
	if (err)
		return dev_err_probe(dev, err, "Failed to request regmaps");

	ds = devm_kzalloc(dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate DSA switch");

	ds->dev = dev;
	ds->num_ports = NUM_PHYS_PORTS;
	ds->num_tx_queues = NUM_PRIO_QUEUES;
	ds->dscp_prio_mapping_is_global = true;

	ds->ops = &lan9645x_switch_ops;
	ds->phylink_mac_ops = &lan9645x_phylink_mac_ops;
	ds->priv = lan9645x;

	lan9645x->ds = ds;
	lan9645x->tag_proto = DSA_TAG_PROTO_LAN9645X;
	lan9645x->shared_queue_sz = LAN9645X_BUFFER_MEMORY;

	lan9645x_set_feat_dis(lan9645x);

	err = dsa_register_switch(ds);
	if (err)
		return dev_err_probe(dev, err, "Failed to register DSA switch");

	return 0;
}

static void lan9645x_remove(struct platform_device *pdev)
{
	struct lan9645x *lan9645x = dev_get_drvdata(&pdev->dev);

	if (!lan9645x)
		return;

	/* Calls lan9645x DSA .teardown */
	dsa_unregister_switch(lan9645x->ds);
	dev_set_drvdata(&pdev->dev, NULL);
}

static void lan9645x_shutdown(struct platform_device *pdev)
{
	struct lan9645x *lan9645x = dev_get_drvdata(&pdev->dev);

	if (!lan9645x)
		return;

	dsa_switch_shutdown(lan9645x->ds);

	dev_set_drvdata(&pdev->dev, NULL);
}

static const struct of_device_id lan9645x_switch_of_match[] = {
	{ .compatible = "microchip,lan9645x-switch" },
	{},
};
MODULE_DEVICE_TABLE(of, lan9645x_switch_of_match);

static struct platform_driver lan9645x_switch_driver = {
	.driver = {
		.name = "lan9645x-switch",
		.of_match_table = lan9645x_switch_of_match,
	},
	.probe = lan9645x_probe,
	.remove = lan9645x_remove,
	.shutdown = lan9645x_shutdown,
};
module_platform_driver(lan9645x_switch_driver);

MODULE_DESCRIPTION("Lan9645x Switch Driver");
MODULE_AUTHOR("Jens Emil Schulz Østergaard <jensemil.schulzostergaard@microchip.com>");
MODULE_LICENSE("Dual MIT/GPL");
