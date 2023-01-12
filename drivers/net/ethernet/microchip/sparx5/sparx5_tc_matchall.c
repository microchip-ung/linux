/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip VCAP API
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#include "sparx5_tc.h"
#include "sparx5_tc_dbg.h"
#include "sparx5_main_regs.h"
#include "sparx5_main.h"

static void sparx5_get_dev_flow_stats(struct net_device *ndev, bool ingress,
				      struct flow_stats *fstats)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_port_stats stats;

	memset(fstats, 0, sizeof(*fstats));
	sparx5_get_port_stats(sparx5, port->portno, &stats);
	if (ingress) {
		fstats->bytes = stats.rx_bytes;
		fstats->pkts = stats.rx_unicast + stats.rx_broadcast +
			stats.rx_multicast;
	} else {
		fstats->bytes = stats.tx_bytes;
		fstats->pkts = stats.tx_unicast + stats.tx_broadcast +
			stats.tx_multicast;
	}
	fstats->lastused = jiffies;
}

static void sparx5_init_mirror_stats(struct net_device *ndev, bool ingress)
{
	struct sparx5_port *port = netdev_priv(ndev);

	sparx5_get_dev_flow_stats(ndev, ingress, &port->tc.prev_mirror_stats);
}

static int sparx5_update_mirror_stats(struct net_device *ndev,
				      struct tc_cls_matchall_offload *tmo,
				      bool ingress)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct flow_stats *prev_stats;
	struct flow_stats fstats;

	prev_stats = &port->tc.prev_mirror_stats;
	sparx5_get_dev_flow_stats(ndev, ingress, &fstats);
	if (fstats.bytes == prev_stats->bytes)
		return 0;
	flow_stats_update(&tmo->stats,
			  fstats.bytes - prev_stats->bytes,
			  fstats.pkts - prev_stats->pkts,
			  0,
			  prev_stats->lastused,
			  FLOW_ACTION_HW_STATS_IMMEDIATE);
	prev_stats->bytes = fstats.bytes;
	prev_stats->pkts = fstats.pkts;
	prev_stats->lastused = jiffies;
	return 0;
}

static int sparx5_alloc_mirror_probe(struct net_device *ndev,
				     bool ingress, struct net_device *mdev)
{
	struct sparx5_port *port = netdev_priv(mdev);
	struct sparx5 *sparx5 = port->sparx5;
	int idx;

	/* Check if the device is already used as a monitor */
	for (idx = 0; idx < SPX5_MIRROR_PROBE_MAX; ++idx)
		if (sparx5->mirror_probe[idx].mdev == ndev)
		    	return -EINVAL;
	/* Find a probe with same direction and monitor port */
	for (idx = 0; idx < SPX5_MIRROR_PROBE_MAX; ++idx)
		if (sparx5->mirror_probe[idx].ingress == ingress &&
		    sparx5->mirror_probe[idx].mdev == mdev)
		    	return idx;
	/* Find a free probe */
	for (idx = 0; idx < SPX5_MIRROR_PROBE_MAX; ++idx)
		if (!sparx5->mirror_probe[idx].mdev)
		    	return idx;
	return -ENOENT;
}

static int sparx5_free_mirror_probe(struct net_device *ndev,
				    bool ingress, struct net_device *mdev)
{
	struct sparx5_port *port = netdev_priv(mdev);
	struct sparx5 *sparx5 = port->sparx5;
	int idx;

	/* Find a probe with same direction and monitor port */
	for (idx = 0; idx < SPX5_MIRROR_PROBE_MAX; ++idx)
		if (sparx5->mirror_probe[idx].ingress == ingress &&
		    sparx5->mirror_probe[idx].mdev == mdev)
		    	return idx;
	return -ENOENT;
}

static int sparx5_mirror_port_add(struct net_device *ndev,
				  bool ingress, struct net_device *mdev)
{
	enum sparx5_mirrorprobe_dir dir = SPX5_MP_EGRESS;
	struct sparx5_port *sport = netdev_priv(ndev);
	struct sparx5_port *dport = netdev_priv(mdev);
	struct sparx5 *sparx5 = sport->sparx5;
	struct sparx5_mirror_probe *probe;
	int srcreg = sport->portno;
	int pidx;
	int val;

	if (ndev == mdev)
		return -EINVAL;
	pidx = sparx5_alloc_mirror_probe(ndev, ingress, mdev);
	pr_debug("%s:%d: sport: %d, dport: %d, probe: %d\n",
		 __func__, __LINE__, sport->portno, dport->portno, pidx);
	if (pidx < 0)
		return pidx;
	/* Store the probe info */
	probe = &sparx5->mirror_probe[pidx];
	probe->ingress = ingress;
	if (test_bit(sport->portno, probe->srcports))
		return -EEXIST;
	set_bit(sport->portno, probe->srcports);

	/* Set monitor direction */
	if (ingress)
		dir = SPX5_MP_INGRESS;
	spx5_rmw(ANA_AC_PROBE_CFG_PROBE_DIRECTION_SET(dir),
		ANA_AC_PROBE_CFG_PROBE_DIRECTION,
		sparx5,
		ANA_AC_PROBE_CFG(pidx));

	/* Mirror probe source config */
	val = BIT(do_div(srcreg, 32));
	if (srcreg == 0)
		spx5_rmw(val, val, sparx5, ANA_AC_PROBE_PORT_CFG(pidx));
	else if (srcreg == 1)
		spx5_rmw(val, val, sparx5, ANA_AC_PROBE_PORT_CFG1(pidx));
	else if (srcreg == 2)
		spx5_rmw(val, val, sparx5, ANA_AC_PROBE_PORT_CFG2(pidx));

	/* Leave if the monitor port has been configured */
	if (probe->mdev)
		return 0;

	/* Mirror probe destination config in queue system */
	spx5_rmw(QFWD_FRAME_COPY_CFG_FRMC_PORT_VAL_SET(dport->portno),
		 QFWD_FRAME_COPY_CFG_FRMC_PORT_VAL,
		 sparx5, QFWD_FRAME_COPY_CFG(pidx + SPX5_QFWD_MP_OFFSET));
	probe->mdev = mdev;
	pr_debug("%s:%d: configured monitorport: %s, probe: %d, port_cfg: %#08x\n",
		 __func__, __LINE__, netdev_name(mdev), pidx, val);
	return 0;
}

static int sparx5_mirror_port_delete(struct net_device *ndev,
				     bool ingress, struct net_device *mdev)
{
	struct sparx5_port *sport = netdev_priv(ndev);
	struct sparx5_port *dport = netdev_priv(mdev);
	struct sparx5 *sparx5 = sport->sparx5;
	struct sparx5_mirror_probe *probe;
	int srcreg = sport->portno;
	int defport = 65;
	int pidx;
	int val;

	pidx = sparx5_free_mirror_probe(ndev, ingress, mdev);
	pr_debug("%s:%d: sport: %d, dport: %d, probe: %d\n",
		 __func__, __LINE__, sport->portno, dport->portno, pidx);
	if (pidx < 0)
		return pidx;
	probe = &sparx5->mirror_probe[pidx];
	clear_bit(sport->portno, probe->srcports);

	/* Mirror probe source config */
	val = BIT(do_div(srcreg, 32));
	if (srcreg == 0)
		spx5_rmw(0, val, sparx5, ANA_AC_PROBE_PORT_CFG(pidx));
	else if (srcreg == 1)
		spx5_rmw(0, val, sparx5, ANA_AC_PROBE_PORT_CFG1(pidx));
	else if (srcreg == 2)
		spx5_rmw(0, val, sparx5, ANA_AC_PROBE_PORT_CFG2(pidx));
	if (bitmap_empty(sparx5->mirror_probe[pidx].srcports, SPX5_PORTS)) {
		/* Remove the monitor probe */
		spx5_rmw(ANA_AC_PROBE_CFG_PROBE_DIRECTION_SET(SPX5_MP_DISABLED),
			 ANA_AC_PROBE_CFG_PROBE_DIRECTION,
			 sparx5,
			 ANA_AC_PROBE_CFG(pidx));
		/* Mirror probe destination config in queue system */
		spx5_rmw(QFWD_FRAME_COPY_CFG_FRMC_PORT_VAL_SET(defport),
			 QFWD_FRAME_COPY_CFG_FRMC_PORT_VAL,
			 sparx5, QFWD_FRAME_COPY_CFG(pidx + SPX5_QFWD_MP_OFFSET));
		probe->mdev = NULL;
		pr_debug("%s:%d: removed monitorport: %s, probe: %d\n",
			 __func__, __LINE__, netdev_name(mdev), pidx);
	}
	return 0;
}

static int sparx5_get_port_policer_idx(struct sparx5_port *port,
				       unsigned long cookie)
{
	int idx;

	/* Find the policer (cookie) */
	for (idx = 0; idx < SPX5_POLICERS_PER_PORT; ++idx)
		if (port->tc.port_policer[idx].policer == cookie)
			return idx;
	return -ENOENT;
}

static int sparx5_alloc_port_policer_idx(struct sparx5_port *port,
					 unsigned long cookie)
{
	int polidx;

	/* Check if the this policer (cookie) already exists */
	for (polidx = 0; polidx < SPX5_POLICERS_PER_PORT; ++polidx)
		if (port->tc.port_policer[polidx].policer == cookie)
		    	return polidx;
	/* Find a free port policer */
	for (polidx = 0; polidx < SPX5_POLICERS_PER_PORT; ++polidx)
		if (!port->tc.port_policer[polidx].policer)
		    	return polidx;
	return -ENOENT;
}

static int sparx5_free_port_policer_idx(struct sparx5_port *port,
					unsigned long cookie)
{
	int polidx;

	/* Find existing policer (cookie) */
	for (polidx = 0; polidx < SPX5_POLICERS_PER_PORT; ++polidx)
		if (port->tc.port_policer[polidx].policer == cookie)
		    	return polidx;
	return -ENOENT;
}

static void sparx5_init_port_policer_stats(struct sparx5_port *port,
					   struct sparx5_policer *pol,
					   int polidx)
{
	sparx5_policer_stats_update(port->sparx5, pol);
	port->tc.port_policer[polidx].prev.drops =
		port->tc.port_policer[polidx].stats.drops;
	port->tc.port_policer[polidx].prev.pkts =
		port->tc.port_policer[polidx].stats.pkts;
}

/* The policer only counts packets or bytes, not both */
static void sparx5_get_port_policer_stats(struct sparx5_port *port,
					  int polidx)
{
	struct sparx5_policer pol = {0};

	pol.type = SPX5_POL_PORT;
	pol.idx = port->portno * SPX5_POLICERS_PER_PORT + polidx;
	sparx5_policer_stats_update(port->sparx5, &pol);
}

static int sparx5_update_port_policer_stats(struct net_device *ndev,
					    struct tc_cls_matchall_offload *tmo)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct flow_stats *prev_stats;
	struct flow_stats *stats;
	int polidx;

	polidx = sparx5_get_port_policer_idx(port, tmo->cookie);
	if (polidx < 0)
		return polidx;
	sparx5_get_port_policer_stats(port, polidx);
	stats = &port->tc.port_policer[polidx].stats;
	prev_stats = &port->tc.port_policer[polidx].prev;
	if (stats->pkts == prev_stats->pkts)
		return 0;
	flow_stats_update(&tmo->stats,
			  0,
			  stats->pkts - prev_stats->pkts,
			  stats->drops - prev_stats->drops,
			  prev_stats->lastused,
			  FLOW_ACTION_HW_STATS_IMMEDIATE);
	prev_stats->pkts = stats->pkts;
	prev_stats->drops = stats->drops;
	prev_stats->lastused = jiffies;
	return 0;
}

static int sparx5_add_port_policer(struct net_device *ndev,
				   struct tc_cls_matchall_offload *tmo,
				   bool ingress,
				   struct flow_action_entry *action)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_policer pol = {0};
	int idx, err;

	if (!ingress)
		return -EINVAL;
	if (port->tc.block_shared[1])
		return -EOPNOTSUPP;

	pr_debug("%s:%d: %s cookie: %lu\n", __func__, __LINE__,
		 netdev_name(ndev), tmo->cookie);
	idx = sparx5_alloc_port_policer_idx(port, tmo->cookie);
	if (idx < 0)
		return idx;
	pol.type = SPX5_POL_PORT;
	pol.rate = action->police.rate_bytes_ps * 8;
	pol.burst = action->police.burst;
	pol.idx = port->portno * SPX5_POLICERS_PER_PORT + idx;

	err = sparx5_policer_conf_set(sparx5, &pol);
	if (err)
		return err;
	pr_debug("%s:%d: %s: added policer %d\n", __func__, __LINE__,
		 netdev_name(ndev), idx);
	port->tc.port_policer[idx].policer = tmo->cookie;
	sparx5_init_port_policer_stats(port, &pol, idx);
	return 0;
}

static int sparx5_delete_port_policer(struct net_device *ndev,
				      struct tc_cls_matchall_offload *tmo,
				      bool ingress,
				      struct flow_action_entry *action)
{
	struct sparx5_port *port = netdev_priv(ndev);
	struct sparx5 *sparx5 = port->sparx5;
	struct sparx5_policer pol = {0};
	int idx, err;

	if (!ingress)
		return -EINVAL;
	if (port->tc.block_shared[1])
		return -EOPNOTSUPP;

	pr_debug("%s:%d: %s cookie: %lu\n", __func__, __LINE__,
		 netdev_name(ndev), tmo->cookie);
	idx = sparx5_free_port_policer_idx(port, tmo->cookie);
	if (idx < 0)
		return idx;
	pol.type = SPX5_POL_PORT;
	pol.idx = port->portno * SPX5_POLICERS_PER_PORT + idx;

	err = sparx5_policer_conf_set(sparx5, &pol);
	if (err)
		return err;
	pr_debug("%s:%d: %s: deleted policer %d\n", __func__, __LINE__,
		 netdev_name(ndev), idx);
	port->tc.port_policer[idx].policer = 0;
	return 0;
}

static int sparx5_tc_matchall_replace(struct net_device *ndev,
				      struct tc_cls_matchall_offload *tmo,
				      bool ingress)
{
	struct flow_action_entry *action;
	int err;

	if (!flow_offload_has_one_action(&tmo->rule->action)) {
		NL_SET_ERR_MSG_MOD(tmo->common.extack,
				   "Only one action per filter is supported");
		return -EOPNOTSUPP;
	}
	action = &tmo->rule->action.entries[0];

	switch (action->id) {
	case FLOW_ACTION_MIRRED:
		pr_debug("%s:%d: port mirroring from %s to %s: ingress: %d\n",
			 __func__, __LINE__,
			 netdev_name(ndev),
			 netdev_name(action->dev),
			 ingress);
		err = sparx5_mirror_port_add(ndev, ingress, action->dev);
		if (err) {
			switch (err) {
			case -EEXIST:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "Mirroring already exists");
				break;
			case -EINVAL:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "Cannot mirror a mirror monitor port");
				break;
			case -ENOENT:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "No more monitor ports available");
				break;
			default:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "Unknown error");
				break;
			}
			return err;
		}
		sparx5_init_mirror_stats(ndev, ingress);
		break;
	case FLOW_ACTION_POLICE:
		err = sparx5_add_port_policer(ndev, tmo, ingress, action);
		if (err) {
			switch (err) {
			case -EINVAL:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "Policer is not supported on egress");
				break;
			case -EOPNOTSUPP:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "Policer is not supported on shared ingress blocks");
				break;
			case -ENOENT:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "No more port policers available");
				break;
			default:
				NL_SET_ERR_MSG_MOD(tmo->common.extack,
						   "Could not add policer");
				break;
			}
			return err;
		}
		break;
	default:
		NL_SET_ERR_MSG_MOD(tmo->common.extack, "Unsupported action");
		return -EOPNOTSUPP;
	}
	return 0;
}

static int sparx5_tc_matchall_destroy(struct net_device *ndev,
				      struct tc_cls_matchall_offload *tmo,
				      bool ingress)
{
	struct flow_action_entry *action;
	int err;

	action = &tmo->rule->action.entries[0];

	switch (action->id) {
	case FLOW_ACTION_MIRRED:
		pr_debug("%s:%d: port mirroring from %s to %s: ingress: %d\n",
			 __func__, __LINE__,
			 netdev_name(ndev),
			 netdev_name(action->dev),
			 ingress);
		err = sparx5_mirror_port_delete(ndev, ingress, action->dev);
		if (err) {
			NL_SET_ERR_MSG_MOD(tmo->common.extack,
					   "Could not delete mirroring");
			return err;
		}
		break;
	case FLOW_ACTION_POLICE:
		err = sparx5_delete_port_policer(ndev, tmo, ingress, action);
		if (err) {
			NL_SET_ERR_MSG_MOD(tmo->common.extack,
					   "Could not delete port policer");
			return err;
		}
		break;
	default:
		NL_SET_ERR_MSG_MOD(tmo->common.extack, "Unsupported action");
		return -EOPNOTSUPP;
	}
	return 0;
}

static int sparx5_tc_matchall_stats(struct net_device *ndev,
				    struct tc_cls_matchall_offload *tmo,
				    bool ingress)
{
	struct flow_action_entry *action;

	action = &tmo->rule->action.entries[0];

	switch (action->id) {
	case FLOW_ACTION_MIRRED:
		sparx5_update_mirror_stats(ndev, tmo, ingress);
		break;
	case FLOW_ACTION_POLICE:
		sparx5_update_port_policer_stats(ndev, tmo);
		break;
	default:
		NL_SET_ERR_MSG_MOD(tmo->common.extack, "Unsupported action");
		return -EOPNOTSUPP;
	}
	return 0;
}

int sparx5_tc_matchall(struct net_device *ndev,
		     struct tc_cls_matchall_offload *tmo,
		     bool ingress)
{
	pr_debug("%s:%d: %s: command: %s, chain: %u, proto: 0x%04x, prio: %u, cookie: %lx\n",
		 __func__, __LINE__,
		 netdev_name(ndev),
		 tc_dbg_tc_matchall_command(tmo->command), tmo->common.chain_index,
		 be16_to_cpu(tmo->common.protocol), tmo->common.prio, tmo->cookie);
	if (tmo->rule) {
		tc_dbg_match_dump(ndev, tmo->rule);
		tc_dbg_actions_dump(ndev, tmo->rule);
	}
	if (!tc_cls_can_offload_and_chain0(ndev, &tmo->common)) {
		NL_SET_ERR_MSG_MOD(tmo->common.extack,
				   "Only chain zero is supported");
		return -EOPNOTSUPP;
	}

	switch (tmo->command) {
	case TC_CLSMATCHALL_REPLACE:
		return sparx5_tc_matchall_replace(ndev, tmo, ingress);
	case TC_CLSMATCHALL_DESTROY:
		return sparx5_tc_matchall_destroy(ndev, tmo, ingress);
	case TC_CLSMATCHALL_STATS:
		return sparx5_tc_matchall_stats(ndev, tmo, ingress);
	default:
		return -EOPNOTSUPP;
	}
}
