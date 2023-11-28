// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver debug filesystem support
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/types.h>

#include <linux/sfp.h>

#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include "sparx5_port.h"
#include "sparx5_tc.h"
#include "vcap_api.h"
#include "vcap_api_client.h"
#include "vcap_api_debugfs.h"
#include "sparx5_vcap_impl.h"

static void sparx5_mirror_probe_debugfs_show_probe(struct seq_file *m, int idx)
{
	struct sparx5 *sparx5 = m->private;
	const struct sparx5_consts *consts;
	int portno;

	consts = &sparx5->data->consts;

	seq_printf(m, "%d: monitor: %s, %s, sources: ",
		   idx,
		   netdev_name(sparx5->mirror_probe[idx].mdev),
		   sparx5->mirror_probe[idx].ingress ? "ingress" : "egress");
	for (portno = 0; portno < consts->chip_ports; ++portno)
		if (test_bit(portno, sparx5->mirror_probe[idx].srcports))
			seq_printf(m, "%s ",
				   netdev_name(sparx5->ports[portno]->ndev));
	seq_printf(m, "\n");
}

static int sparx5_mirror_probe_debugfs_show(struct seq_file *m, void *unused)
{
	struct sparx5 *sparx5 = m->private;
	int idx;

	/* Show all configured probes */
	for (idx = 0; idx < SPX5_MIRROR_PROBE_MAX; ++idx)
		if (sparx5->mirror_probe[idx].mdev)
			sparx5_mirror_probe_debugfs_show_probe(m, idx);
		else
			seq_printf(m, "%d: none\n", idx);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_mirror_probe_debugfs); /* sparx5_mirror_probe_debugfs_fops */

void sparx5_mirror_probe_debugfs(struct sparx5 *sparx5)
{
	debugfs_create_file("mirrorprobes", 0444, sparx5->debugfs_root, sparx5,
			    &sparx5_mirror_probe_debugfs_fops);
}

static int sparx5_mactable_debugfs_show(struct seq_file *m, void *unused)
{
	struct sparx5 *sparx5 = m->private;
	unsigned char mac[ETH_ALEN];
	u16 vid;
	u32 cfg2;
	int cnt = 0;

	vid = 0;
	memset(mac, 0, sizeof(mac));

	while (sparx5_mact_getnext(sparx5, mac, &vid, &cfg2)) {
		u16 addr = LRN_MAC_ACCESS_CFG_2_MAC_ENTRY_ADDR_GET(cfg2);

		seq_printf(m, "%4d: %pM %d:%d (%08x)\n",
			   cnt++, mac, vid, addr, cfg2);
	}

	return 0;

}
DEFINE_SHOW_ATTRIBUTE(sparx5_mactable_debugfs); /* sparx5_mactable_debugfs_fops */

static void sparx5_debugfs_cpuportstats(struct seq_file *m, int portno)
{
	struct sparx5 *sparx5 = m->private;
	const char *name;
	u64 val;
	int idx;

	sparx5_update_cpuport_stats(sparx5, portno);
	seq_printf(m, "Port %u\n", portno);
	for (idx = 0; idx < sparx5->num_stats; ++idx) {
		if (sparx5_get_cpuport_stats(sparx5, portno, idx, &name, &val)) {
			seq_printf(m, "%-*s: %llu\n", ETH_GSTRING_LEN,
				   name, val);
		}
	}
	seq_puts(m, "\n");
}

static int sparx5_cpuport0_debugfs_show(struct seq_file *m, void *unused)
{
	struct sparx5 *sparx5 = m->private;

	sparx5_debugfs_cpuportstats(m, sparx5_get_internal_port(sparx5,
								PORT_CPU_0));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_cpuport0_debugfs); /* sparx5_cpuport0_debugfs_fops */

static int sparx5_cpuport1_debugfs_show(struct seq_file *m, void *unused)
{
	struct sparx5 *sparx5 = m->private;

	sparx5_debugfs_cpuportstats(m, sparx5_get_internal_port(sparx5,
								PORT_CPU_1));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_cpuport1_debugfs); /* sparx5_cpuport1_debugfs_fops */

static int sparx5_portstat_debugfs_show(struct seq_file *m, void *unused)
{
	struct sparx5_port *port = m->private;
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_port_stats stats;
	int portno = port->portno;

	sparx5_get_port_stats(sparx5, portno, &stats);
	seq_printf(m, "Port %u\n", portno);
	seq_printf(m, "tx_unicast: %llu\n", stats.tx_unicast);
	seq_printf(m, "tx_multicast: %llu\n", stats.tx_multicast);
	seq_printf(m, "tx_broadcast: %llu\n", stats.tx_broadcast);
	seq_printf(m, "rx_unicast: %llu\n", stats.rx_unicast);
	seq_printf(m, "rx_multicast: %llu\n", stats.rx_multicast);
	seq_printf(m, "rx_broadcast: %llu\n", stats.rx_broadcast);
	seq_puts(m, "\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_portstat_debugfs); /* sparx5_portstat_debugfs_fops */

static char get_bool(bool val)
{
	if (val)
		return 'Y';
	return ' ';
}

static void sparx5_show_portstate(struct seq_file *m,
				  struct sparx5_port *port,
				  struct ethtool_eeprom *sfp_eeprom)
{
	struct sfp_eeprom_id *id = (struct sfp_eeprom_id *)&sfp_eeprom->data[0];
	struct net_device *ndev = port->ndev;
	struct sparx5_port_status status;
	u32 value;
	bool phy_link = false, aneg_enabled = false, aneg_complete = false;

	if (!ndev)
		return;
	if (ndev->phydev) {
		phy_link = ndev->phydev->link;
		aneg_enabled = ndev->phydev->autoneg;
		aneg_complete = ndev->phydev->autoneg_complete;
	} else {
		value = spx5_rd(port->sparx5, DEV2G5_PCS1G_ANEG_CFG(port->portno));
		aneg_enabled = DEV2G5_PCS1G_ANEG_CFG_ANEG_ENA_GET(value);
		value = spx5_rd(port->sparx5, DEV2G5_PCS1G_ANEG_STATUS(port->portno));
		aneg_complete = DEV2G5_PCS1G_ANEG_STATUS_ANEG_COMPLETE_GET(value);
	}
	sparx5_get_port_status(port->sparx5, port, &status);

	seq_printf(m, " %02d  %-15s %-12s   %c        %c      %c      %c",
		   port->portno,
		   phy_modes(port->conf.portmode),
		   status.speed ? phy_speed_to_str(status.speed) : "",
		   get_bool(status.link),
		   get_bool(phy_link),
		   get_bool(aneg_enabled),
		   get_bool(aneg_complete));
	if (ndev->sfp_bus) {
		struct ethtool_modinfo modinfo;

		if (sfp_get_module_info(ndev->sfp_bus, &modinfo))
			return;
		sfp_eeprom->offset = 0;
		sfp_eeprom->len = modinfo.eeprom_len;
		if (sfp_get_module_eeprom(ndev->sfp_bus, sfp_eeprom,
					  &sfp_eeprom->data[0]) == 0) {
			seq_printf(m, "    SFP: %.16s %.16s",
				   id->base.vendor_name, id->base.vendor_pn);
		}
	}
	if (ndev->phydev)
		seq_printf(m, "    PHY: %-20s", ndev->phydev->drv->name);
	seq_puts(m, "\n");
}

static int sparx5_portstates_debugfs_show(struct seq_file *m, void *v)
{
	struct sparx5 *sparx5 = m->private;
	const struct sparx5_consts *consts;
	struct ethtool_eeprom *sfp_eeprom;
	int idx;

	consts = &sparx5->data->consts;

	sfp_eeprom = kzalloc(sizeof(*sfp_eeprom) +
			     ETH_MODULE_SFF_8472_LEN, GFP_KERNEL);
	if (!sfp_eeprom)
		return 0;
	seq_puts(m, "Port Mode            Speed        ");
	seq_puts(m, "PLink   PhyLink ANegEn ANegCp SFP/PHY\n");
	for (idx = 0; idx < consts->chip_ports; idx++)
		if (sparx5->ports[idx])
			sparx5_show_portstate(m, sparx5->ports[idx], sfp_eeprom);
	kfree(sfp_eeprom);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_portstates_debugfs); /* sparx5_portstates_debugfs_fops */

void sparx5_debugfs(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;
	struct dentry *dir;
	char portname[32];
	int portno;

	debugfs_create_file("mactable", 0444, sparx5->debugfs_root, sparx5,
			    &sparx5_mactable_debugfs_fops);
	debugfs_create_file("portstates", 0444, sparx5->debugfs_root, sparx5,
			    &sparx5_portstates_debugfs_fops);
	dir = debugfs_create_dir("stats", sparx5->debugfs_root);
	if (PTR_ERR_OR_ZERO(dir))
		return;
	debugfs_create_file("cpuport0", 0444, dir, sparx5, &sparx5_cpuport0_debugfs_fops);
	debugfs_create_file("cpuport1", 0444, dir, sparx5, &sparx5_cpuport1_debugfs_fops);
	for (portno = 0; portno < consts->chip_ports; portno++)
		if (sparx5->ports[portno]) {
			snprintf(portname, sizeof(portname), "port%02d", portno);
			debugfs_create_file(portname, 0444, dir,
					    sparx5->ports[portno],
					    &sparx5_portstat_debugfs_fops);
		}
	sparx5_mirror_probe_debugfs(sparx5);
}
