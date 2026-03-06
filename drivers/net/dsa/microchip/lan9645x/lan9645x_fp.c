// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/debugfs.h>

#include "lan9645x_main.h"

#define LAN9645X_FP_CHECK_DELAY	(2 * HZ)
#define FP_MAX_VERIFY_TIME		128
#define FP_MAX_FRAG_SIZE		3
#define FP_HOLD_ADVANCE		42
#define FP_RELEASE_ADVANCE		84

enum fp_status_verify {
	FP_STATUS_VERIFY_INITIAL = 0,
	FP_STATUS_VERIFY_IDLE_SEND_WAIT = 1,
	FP_STATUS_VERIFY_SUCCEEDED = 2,
	FP_STATUS_VERIFY_FAILED = 3,
	FP_STATUS_VERIFY_DISABLED = 4,
};

void lan9645x_fp_link_change(struct lan9645x_port *p, bool link)
{
	dev_dbg(p->lan9645x->dev, "port=%d link=%d\n", p->chip_port, link);

	lan9645x_fp_set(p, &p->fp, link);
}

int lan9645x_fp_get(struct lan9645x_port *p,
		    struct lan9645x_fp_port_conf *c)
{
	mutex_lock(&p->fp_lock);
	*c = p->fp;
	mutex_unlock(&p->fp_lock);
	return 0;
}

static int __lan9645x_fp_set(struct lan9645x_port *p,
			     struct lan9645x_fp_port_conf *c, bool link)
{
	bool verify_dis, capable, fp_ena;
	struct net_device *dev;
	u32 mask;

	dev = lan9645x_port_to_ndev(p);

	netdev_dbg(dev,
		   "lan9645x_fp_set() admin_status=%u enable_tx=%d verify_dis=%d verify_time=%u add_frag_sz=%u\n",
		   c->admin_status, c->enable_tx, c->verify_disable_tx,
		   c->verify_time, c->add_frag_size);

	if ((c->verify_time < 1) || (c->verify_time > FP_MAX_VERIFY_TIME)) {
		netdev_err(dev, "Invalid verify_time (%u)\n", c->verify_time);
		return -EINVAL;
	}

	if (c->add_frag_size > FP_MAX_FRAG_SIZE) {
		netdev_err(dev, "Invalid add_frag_size (%u)\n",
			   c->add_frag_size);
		return -EINVAL;
	}

	capable = p->duplex == DUPLEX_FULL && p->speed >= LAN9645X_SPEED_100;
	fp_ena = capable && link && c->enable_tx;
	verify_dis = !(fp_ena && !c->verify_disable_tx);
	mask = fp_ena ? c->admin_status : 0;

	/* Toggle verification state machine on reconfiguration. */
	lan_rmw(DEV_VERIF_CONFIG_PRM_VERIFY_DIS_SET(1),
		DEV_VERIF_CONFIG_PRM_VERIFY_DIS,
		p->lan9645x, DEV_VERIF_CONFIG(p->chip_port));

	lan_wr(DEV_VERIF_CONFIG_PRM_VERIFY_DIS_SET(verify_dis) |
	       DEV_VERIF_CONFIG_PRM_VERIFY_TIME_SET(c->verify_time) |
	       DEV_VERIF_CONFIG_VERIF_TIMER_UNITS_SET(
			p->speed == LAN9645X_SPEED_2500 ? 2 : 0),
		p->lan9645x, DEV_VERIF_CONFIG(p->chip_port));

	lan_rmw(SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE_SET(c->add_frag_size),
		SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE,
		p->lan9645x,
		SYS_FRONT_PORT_MODE(p->chip_port));

	lan_rmw(QSYS_PREEMPT_CFG_P_QUEUES_SET(mask),
		QSYS_PREEMPT_CFG_P_QUEUES,
		p->lan9645x,
		QSYS_PREEMPT_CFG(p->chip_port));

	p->fp = *c;
	return 0;
}

int lan9645x_fp_set(struct lan9645x_port *p,
		    struct lan9645x_fp_port_conf *c, bool link)
{
	int err;

	mutex_lock(&p->fp_lock);
	err = __lan9645x_fp_set(p, c, link);
	mutex_unlock(&p->fp_lock);
	return err;
}

void lan9645x_fp_change_preemptable_tcs(struct lan9645x_port *p,
					unsigned long preemptible_tcs)
{
	struct lan9645x_fp_port_conf c = {};
	struct net_device *dev;

	dev = lan9645x_port_to_ndev(p);

	mutex_lock(&p->fp_lock);
	c = p->fp;
	c.admin_status = preemptible_tcs;
	__lan9645x_fp_set(p, &c, netif_carrier_ok(dev));
	mutex_unlock(&p->fp_lock);

	/* Recalculate TAS guard bands: preemptible TCs use MAC HOLD
	 * instead of guard banding, so the guard band configuration
	 * changes when the preemptible TC set changes.
	 */
	lan9645x_taprio_guard_bands_recalc(p);
}

int lan9645x_fp_status(struct lan9645x_port *p,
		       struct lan9645x_fp_port_status *s)
{
	struct net_device *dev;
	u32 status;
	u32 state;
	int sv;

	dev = lan9645x_port_to_ndev(p);

	s->hold_advance = FP_HOLD_ADVANCE;
	s->release_advance = FP_RELEASE_ADVANCE;

	status = lan_rd(p->lan9645x, DEV_MM_STATUS(p->chip_port));
	s->preemption_active = !!DEV_MM_STATUS_PRMPT_ACTIVE_STATUS_GET(status);
	sv = DEV_MM_STATUS_PRMPT_VERIFY_STATE_GET(status);

	if (!netif_carrier_ok(dev))
		/* Always INIT when no link */
		s->status_verify = 0;
	else
		/* Chip combines IDLE, SEND and WAIT into one */
		s->status_verify = sv == 0 ? 0 : (sv + 2);

	dev_dbg(p->lan9645x->dev, "port=%d verify_state=%lu\n", p->chip_port,
		DEV_MM_STATUS_PRMPT_VERIFY_STATE_GET(status));

	state = lan_rd(p->lan9645x, SYS_FPORT_STATE(p->chip_port));
	s->hold_request = SYS_FPORT_STATE_MAC_HOLD_GET(state);

	return 0;
}

static int lan9645x_fp_ena_dfs_show(struct seq_file *file, void *unused)
{
	struct lan9645x_port *p = file->private;
	bool enable_tx;

	mutex_lock(&p->fp_lock);
	enable_tx = p->fp.enable_tx;
	mutex_unlock(&p->fp_lock);

	seq_printf(file, "%d\n", enable_tx);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan9645x_fp_ena_dfs);

static int lan9645x_fp_active_dfs_show(struct seq_file *file, void *unused)
{
	struct lan9645x_port *p = file->private;
	u32 status;

	mutex_lock(&p->fp_lock);
	status = lan_rd(p->lan9645x, DEV_MM_STATUS(p->chip_port));
	mutex_unlock(&p->fp_lock);

	seq_printf(file, "%d\n",
		   DEV_MM_STATUS_PRMPT_ACTIVE_STATUS_GET(status) ? 1 : 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan9645x_fp_active_dfs);

static void lan9645x_fp_check_port(struct lan9645x_port *p)
{
	struct net_device *dev = lan9645x_port_to_ndev(p);
	struct lan9645x *lan9645x = p->lan9645x;
	u32 status, verify_state, act_sticky;

	mutex_lock(&p->fp_lock);

	if (!p->fp.enable_tx || p->fp.verify_disable_tx ||
	    (dev && !netif_carrier_ok(dev)))
		goto unlock;

	status = lan_rd(lan9645x, DEV_MM_STATUS(p->chip_port));
	verify_state = DEV_MM_STATUS_PRMPT_VERIFY_STATE_GET(status);
	act_sticky = DEV_MM_STATUS_PRMPT_ACTIVE_STICKY_GET(status);

	if (act_sticky)
		lan_rmw(DEV_MM_STATUS_PRMPT_ACTIVE_STICKY_SET(0),
			DEV_MM_STATUS_PRMPT_ACTIVE_STICKY,
			lan9645x, DEV_MM_STATUS(p->chip_port));

	if (verify_state != FP_STATUS_VERIFY_FAILED)
		goto unlock;

	/* Restart verification after fail. */
	lan_rmw(DEV_VERIF_CONFIG_PRM_VERIFY_DIS_SET(1),
		DEV_VERIF_CONFIG_PRM_VERIFY_DIS,
		lan9645x, DEV_VERIF_CONFIG(p->chip_port));
	lan_rmw(DEV_VERIF_CONFIG_PRM_VERIFY_DIS_SET(0),
		DEV_VERIF_CONFIG_PRM_VERIFY_DIS,
		lan9645x, DEV_VERIF_CONFIG(p->chip_port));

	dev_dbg(lan9645x->dev,
		"Restart Frame Preemption verification port=%d status=%lu sticky=%u verify=%u", p->chip_port,
		DEV_MM_STATUS_PRMPT_ACTIVE_STATUS_GET(status),
		act_sticky,
		verify_state);

unlock:
	mutex_unlock(&p->fp_lock);
}

static void lan9645x_fp_check_all(struct lan9645x *lan9645x)
{
	struct lan9645x_port *p;
	int i;

	lan9645x_for_each_port(lan9645x, i, p)
		lan9645x_fp_check_port(p);
}

static void lan9645x_fp_check(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan9645x *lan9645x;

	lan9645x = container_of(del_work, struct lan9645x, fp_work);

	/* TODO: We could use the DEV irq here. Register is:
	 * DEV_MM_STATUS_PRMPT_ACTIVE_STICKY_GET(status)
	 */
	lan9645x_fp_check_all(lan9645x);

	queue_delayed_work(lan9645x->queue, &lan9645x->fp_work,
			   LAN9645X_FP_CHECK_DELAY);
}

int lan9645x_fp_init(struct lan9645x *lan9645x)
{
	struct dentry *fp_dfs_root, *port_dfs_root;
	struct lan9645x_port *p;
	char def_name[32];
	int port;
	u32 val;

	lan9645x->queue = alloc_ordered_workqueue("lan9645x-fpreempt", 0);
	if (!lan9645x->queue)
		return -ENOMEM;

	fp_dfs_root = debugfs_create_dir("fp", lan9645x->debugfs_root);

	/* Initialize frame-preemption and sync config with defaults */
	lan9645x_for_each_port(lan9645x, port, p) {
		mutex_init(&p->fp_lock);

		/* Always enable MAC-MERGE Layer tx/rx, and let preemption be
		 * controlled by P_QUEUES
		 */
		lan_rmw(DEV_ENABLE_CONFIG_MM_RX_ENA_SET(1) |
			DEV_ENABLE_CONFIG_MM_TX_ENA_SET(1) |
			DEV_ENABLE_CONFIG_KEEP_S_AFTER_D_SET(0),
			DEV_ENABLE_CONFIG_MM_RX_ENA |
			DEV_ENABLE_CONFIG_MM_TX_ENA |
			DEV_ENABLE_CONFIG_KEEP_S_AFTER_D,
			lan9645x, DEV_ENABLE_CONFIG(port));

		/* Meet strict bandwidth requirements */
		lan_rmw(QSYS_PREEMPT_CFG_STRICT_IPG_SET(0),
			QSYS_PREEMPT_CFG_STRICT_IPG,
			lan9645x, QSYS_PREEMPT_CFG(port));

		val = lan_rd(lan9645x, QSYS_PREEMPT_CFG(port));
		p->fp.admin_status = QSYS_PREEMPT_CFG_P_QUEUES_GET(val);
		p->fp.enable_tx = 0;

		val = lan_rd(lan9645x, DEV_VERIF_CONFIG(port));
		p->fp.verify_disable_tx = DEV_VERIF_CONFIG_PRM_VERIFY_DIS_GET(val);
		p->fp.verify_time = DEV_VERIF_CONFIG_PRM_VERIFY_TIME_GET(val);

		val = lan_rd(lan9645x, SYS_FRONT_PORT_MODE(port));
		p->fp.add_frag_size = SYS_FRONT_PORT_MODE_ADD_FRAG_SIZE_GET(val);

		/* Add per interface debugfs files for e.g. LLDP. At this point
		 * the net_devices attached to DSA ports are NULL, so we can
		 * not use the name of the net_device.
		 */

		if (lan9645x->npi == port ||
		    !lan9645x_port_is_used(lan9645x, port))
			continue;

		snprintf(def_name, sizeof(def_name), "lan%d", port);
		port_dfs_root = debugfs_create_dir(p->name ? p->name : def_name,
						   fp_dfs_root);
		debugfs_create_file("fp-enabled", 0444, port_dfs_root, p,
				    &lan9645x_fp_ena_dfs_fops);
		debugfs_create_file("fp-active", 0444, port_dfs_root, p,
				    &lan9645x_fp_active_dfs_fops);
	}

	INIT_DELAYED_WORK(&lan9645x->fp_work, lan9645x_fp_check);
	queue_delayed_work(lan9645x->queue, &lan9645x->fp_work,
			   LAN9645X_FP_CHECK_DELAY);
	return 0;
}

int lan9645x_fp_ethtool_set_mm(struct lan9645x *lan9645x, int port,
			       struct ethtool_mm_cfg *cfg,
			       struct netlink_ext_ack *extack)
{
	struct lan9645x_fp_port_conf c = {};
	struct lan9645x_port *p;
	struct net_device *dev;
	u32 add_frag_size;
	int err;

	p = lan9645x_to_port(lan9645x, port);
	dev = lan9645x_port_to_ndev(p);

	err = lan9645x_fp_get(p, &c);
	if (err)
		return err;

	err = ethtool_mm_frag_size_min_to_add(cfg->tx_min_frag_size,
					      &add_frag_size, extack);
	if (err)
		return err;

	c.enable_tx = cfg->tx_enabled;
	c.verify_disable_tx = !cfg->verify_enabled;

	c.verify_time = cfg->verify_time;
	c.add_frag_size = add_frag_size;

	/* c.admin_status is set by mqprio */

	return lan9645x_fp_set(p, &c, netif_carrier_ok(dev));
}

int lan9645x_fp_ethtool_get_mm(struct lan9645x *lan9645x, int port,
			       struct ethtool_mm_state *state)
{
	struct lan9645x_fp_port_status s = {};
	struct lan9645x_fp_port_conf c = {};
	struct lan9645x_port *p;
	u32 mm_cfg;
	int err;

	p = lan9645x_to_port(lan9645x, port);

	err = lan9645x_fp_get(p, &c);
	if (err)
		return err;

	err = lan9645x_fp_status(p, &s);
	if (err)
		return err;

	mm_cfg = lan_rd(lan9645x, DEV_ENABLE_CONFIG(port));

	/* Shows whether transmission on the pMAC is administratively enabled. */
	state->tx_enabled = c.enable_tx;

	/* Shows whether the pMAC is enabled and capable of receiving traffic
	 *  and SMD-V frames (and responding to them with SMD-R replies).
	 */
	state->pmac_enabled = DEV_ENABLE_CONFIG_MM_RX_ENA_GET(mm_cfg);

	state->verify_enabled = !c.verify_disable_tx;
	state->verify_time = c.verify_time;
	state->max_verify_time = FP_MAX_VERIFY_TIME;

	/* tx-min-frag-size = 64 * (1 + addFragSize) - 4 */
	state->tx_min_frag_size = ethtool_mm_frag_size_add_to_min(c.add_frag_size);
	state->rx_min_frag_size = ETH_ZLEN;

	/* Shows whether transmission on the pMAC is active (verification is either successful, or was disabled). */
	state->tx_active = s.preemption_active;

	switch (s.status_verify) {
	case 0:
		state->verify_status = ETHTOOL_MM_VERIFY_STATUS_INITIAL;
		break;
	case 1 ... 3:
		state->verify_status = ETHTOOL_MM_VERIFY_STATUS_VERIFYING;
		break;
	case 4:
		state->verify_status = ETHTOOL_MM_VERIFY_STATUS_SUCCEEDED;
		break;
	case 5:
		state->verify_status = ETHTOOL_MM_VERIFY_STATUS_FAILED;
		break;
	case 6:
		state->verify_status = ETHTOOL_MM_VERIFY_STATUS_DISABLED;
		break;
	default:
		state->verify_status = ETHTOOL_MM_VERIFY_STATUS_UNKNOWN;
		break;
	}

	return 0;
}
