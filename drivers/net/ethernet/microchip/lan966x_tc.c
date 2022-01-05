/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include "lan966x_main.h"
#include "lan966x_tc_dbg.h"

/*******************************************************************************
 * tc block
 ******************************************************************************/
static int lan966x_tc_block_cb(enum tc_setup_type type,
			       void *type_data,
			       void *cb_priv, bool ingress)
{
	struct lan966x_port *port = cb_priv;

	netdev_dbg(port->dev, "type %s, %s\n",
		   tc_dbg_tc_setup_type(type), ingress ? "ingress" : "egress");

	switch (type) {
	case TC_SETUP_CLSMATCHALL:
		return lan966x_tc_matchall(port, type_data, ingress);
	case TC_SETUP_CLSFLOWER:
		return lan966x_tc_flower(port, type_data, ingress);
	default:
		return -EOPNOTSUPP;
	}
}

static int lan966x_tc_block_cb_ingress(enum tc_setup_type type,
				       void *type_data,
				       void *cb_priv)
{
	return lan966x_tc_block_cb(type, type_data, cb_priv, true);
}

static int lan966x_tc_block_cb_egress(enum tc_setup_type type,
				      void *type_data,
				      void *cb_priv)
{
	return lan966x_tc_block_cb(type, type_data, cb_priv, false);
}

static LIST_HEAD(lan966x_block_cb_list);

static int lan966x_tc_setup_block(struct lan966x_port *port,
				  struct flow_block_offload *f)
{
	flow_setup_cb_t *cb;

	netdev_dbg(port->dev, "command %s binder_type %s shared %d unlocked %d\n",
		   tc_dbg_flow_block_command(f->command),
		   tc_dbg_flow_block_binder_type(f->binder_type),
		   f->block_shared, f->unlocked_driver_cb);

	if (f->binder_type == FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS) {
		cb = lan966x_tc_block_cb_ingress;
		port->tc.block_shared[1] = f->block_shared;
	} else if (f->binder_type == FLOW_BLOCK_BINDER_TYPE_CLSACT_EGRESS) {
		cb = lan966x_tc_block_cb_egress;
		port->tc.block_shared[0] = f->block_shared;
	} else {
		return -EOPNOTSUPP;
	}

	return flow_block_cb_setup_simple(f, &lan966x_block_cb_list,
					  cb, port, port, false);
}

/*******************************************************************************
 * tc mqprio qdisc
 ******************************************************************************/
static int lan966x_tc_setup_qdisc_mqprio(struct lan966x_port *port,
					 struct tc_mqprio_qopt_offload *m)
{
	u8 num_tc;
	int i;

	m->qopt.hw = TC_MQPRIO_HW_OFFLOAD_TCS;
	num_tc = m->qopt.num_tc;

	if (!num_tc) {
		netdev_reset_tc(port->dev);
		netdev_dbg(port->dev, "dev->num_tc %u dev->real_num_tx_queues %u\n",
			   port->dev->num_tc, port->dev->real_num_tx_queues);
		return 0;
	}

	if (num_tc != LAN966X_NUM_TC) {
		netdev_err(port->dev, "Only %d traffic classes supported\n",
			   LAN966X_NUM_TC);
		return -EINVAL;
	}

	netdev_set_num_tc(port->dev, num_tc);

	for (i = 0; i < num_tc; i++)
		netdev_set_tc_queue(port->dev, i, 1, i);

	netdev_dbg(port->dev, "dev->num_tc %u dev->real_num_tx_queues %u\n",
		   port->dev->num_tc, port->dev->real_num_tx_queues);
	return 0;
}

/*******************************************************************************
 * tc taprio qdisc
 ******************************************************************************/
static int lan966x_tc_setup_qdisc_taprio(struct lan966x_port *port,
					 struct tc_taprio_qopt_offload *qopt)
{
	int i, err;

	netdev_dbg(port->dev,
		   "port %u enable %d\n",
		   port->chip_port, qopt->enable);
	if (qopt->enable) {
		netdev_dbg(port->dev,
			   "base_time %lld cycle_time %llu cycle_time_extension %llu\n",
			   qopt->base_time, qopt->cycle_time,
			   qopt->cycle_time_extension);
		for (i = 0; i < qopt->num_entries; i++) {
			netdev_dbg(port->dev,
				   "[%d]: command %u gate_mask %x interval %u\n",
				   i, qopt->entries[i].command,
				   qopt->entries[i].gate_mask,
				   qopt->entries[i].interval);
		}
		err = lan966x_tas_enable(port, qopt);
	} else
		err = lan966x_tas_disable(port);

	return err;
}


/*******************************************************************************
 * tc cbs qdisc
 ******************************************************************************/
static int lan966x_tc_setup_qdisc_cbs(struct lan966x_port *port,
				      struct tc_cbs_qopt_offload *qopt)
{
	struct lan966x_tc_cbs cbs = { 0 };

	netdev_dbg(port->dev, "enable %u, queue %d, hicredit %d, locredit %d, idleslope %d sendslope %d\n",
		   qopt->enable, qopt->queue, qopt->hicredit, qopt->locredit,
		   qopt->idleslope, qopt->sendslope);

	if (qopt->enable) {
		cbs.idleslope = qopt->idleslope;
		cbs.sendslope = qopt->sendslope;
		cbs.hicredit = qopt->hicredit;
		cbs.locredit = qopt->locredit;
		return lan966x_tc_cbs_add(port, qopt->queue, &cbs);
	} else {
		return lan966x_tc_cbs_del(port, qopt->queue);
	}
}

/*******************************************************************************
 * tc tbf qdisc
 ******************************************************************************/
static int lan966x_tc_setup_qdisc_tbf(struct lan966x_port *port,
				      struct tc_tbf_qopt_offload *qopt)
{
	struct lan966x_tc_tbf tbf = { 0 };
	bool root = (qopt->parent == TC_H_ROOT);
	u32 queue = 0;

	netdev_dbg(port->dev, "command %d, handle 0x%08x, parent 0x%08x\n",
		   qopt->command, qopt->handle, qopt->parent);

	if (!root) {
		queue = TC_H_MIN(qopt->parent) - 1;
		if (queue >= PRIO_COUNT) {
			netdev_err(port->dev, "Invalid queue %u!\n", queue);
			return -EOPNOTSUPP;
		}
	}

	switch (qopt->command) {
	case TC_TBF_REPLACE:
		tbf.rate = div_u64(qopt->replace_params.rate.rate_bytes_ps, 1000) * 8;
		tbf.burst = qopt->replace_params.max_size;
		return lan966x_tc_tbf_add(port, root, queue, &tbf);
	case TC_TBF_DESTROY:
		return lan966x_tc_tbf_del(port, root, queue);
	case TC_TBF_STATS:
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
	return -EOPNOTSUPP;
}

/*******************************************************************************
 * tc root qdisc
 ******************************************************************************/
static int lan966x_tc_setup_root_qdisc(struct lan966x_port *port,
				       struct tc_root_qopt_offload *o)
{
	netdev_dbg(port->dev, "command %s handle 0x%08x ingress %d\n",
		   tc_dbg_root_command(o->command), o->handle, o->ingress);
	return -EOPNOTSUPP;
}

/*******************************************************************************
 * tc root qdisc
 ******************************************************************************/
static int lan966x_tc_setup_qdisc_ets(struct lan966x_port *port,
				      struct tc_ets_qopt_offload *o)
{
	struct tc_ets_qopt_offload_replace_params *replace_params;
	struct tc_ets_qopt_offload_graft_params *graft_params;
	u32 i, dwrr_count, first_dwrr_band, strict_idx;
	struct mchp_qos_port_conf cfg = {};
	struct tc_qopt_offload_stats *stats;
	int err;

	switch (o->command) {
	case TC_ETS_REPLACE:
		replace_params = &o->replace_params;
		if (replace_params->bands != PRIO_COUNT) {
			netdev_err(port->dev, "Only eight bands are supported\n");
			return -EINVAL;
		}

		err = lan966x_qos_port_conf_get(port, &cfg);
		if (err) {
			return -EINVAL;
		}

		cfg.dwrr_enable = true;
		/* Note that in the switch the DWRR is always on the lowest consecutive priorities.
		 */
		for (i = 0, dwrr_count = 0; i < PRIO_COUNT; ++i) {
			printk(KERN_DEBUG "%s %d   weights %u\n",__FUNCTION__,__LINE__,replace_params->weights[i]);
			if (replace_params->quanta[i] != 0) {
				/* The first band in replace_params->weights has highest priority. */
				cfg.dwrr_queue_pct[7 - i] = replace_params->weights[i];
				++dwrr_count;
			}
		}
		for (i = 0; i < PRIO_COUNT; ++i)
			printk(KERN_DEBUG "%s %d   dwrr_queue_pct %u\n",__FUNCTION__,__LINE__,cfg.dwrr_queue_pct[i]);

		cfg.dwrr_count = dwrr_count;

		/* Note that in the switch the DWRR is always on the lowest consecutive priorities.
		 * Due to this, the first priority (priomap[0]) must map to the first DWRR band.
		 * Consecutive priorities must map to consecutive bands.
		 */
		first_dwrr_band = 8 - dwrr_count;
		strict_idx = 0;
		for (i = 0; i < 8; ++i) {
			if (replace_params->priomap[i] != (7 - i)) {
				netdev_err(port->dev,
				"\nArgument priomap:\n"\
				"    STRICT band 0 has the highest priority.\n"\
				"    In the switch the highest priority is 7\n"\
				"    Therefore the map must be reverse 1:1.\n");
				return -EINVAL;
			}
		}

		err = lan966x_qos_port_conf_set(port, &cfg);
		if (err) {
			return -EINVAL;
		}

		return 0;
	case TC_ETS_DESTROY:
		err = lan966x_qos_port_conf_get(port, &cfg);
		if (err) {
			return -EINVAL;
		}
		cfg.dwrr_enable = false;
		err = lan966x_qos_port_conf_set(port, &cfg);
		if (err) {
			return -EINVAL;
		}
		return 0;
	case TC_ETS_STATS:
		stats = &o->stats;
		return 0;
	case TC_ETS_GRAFT:
		graft_params = &o->graft_params;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
	return -EOPNOTSUPP;
}

int lan966x_setup_tc(struct net_device *dev, enum tc_setup_type type,
		     void *type_data)
{
	struct lan966x_port *port = netdev_priv(dev);

	netdev_dbg(dev, "type %s\n", tc_dbg_tc_setup_type(type));

	switch (type) {
	case TC_SETUP_BLOCK:
		return lan966x_tc_setup_block(port, type_data);
	case TC_SETUP_QDISC_MQPRIO:
		return lan966x_tc_setup_qdisc_mqprio(port, type_data);
	case TC_SETUP_QDISC_TAPRIO:
		return lan966x_tc_setup_qdisc_taprio(port, type_data);
	case TC_SETUP_QDISC_CBS:
		return lan966x_tc_setup_qdisc_cbs(port, type_data);
	case TC_SETUP_QDISC_TBF:
		return lan966x_tc_setup_qdisc_tbf(port, type_data);
	case TC_SETUP_ROOT_QDISC:
		return lan966x_tc_setup_root_qdisc(port, type_data);
	case TC_SETUP_QDISC_ETS:
		return lan966x_tc_setup_qdisc_ets(port, type_data);
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}
