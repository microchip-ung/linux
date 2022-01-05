/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#include "lan966x_main.h"
#include "lan966x_tc_dbg.h"

/*******************************************************************************
 * tc matchall classifier
 ******************************************************************************/
static int lan966x_tc_matchall_replace(struct lan966x_port *port,
				       struct tc_cls_matchall_offload *f,
				       bool ingress)
{
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_tc_policer pol = { 0 };
	struct flow_action_entry *action;
	struct flow_stats *prev_stats;
	u64 rate;
	int err, i, idx;

	if (!flow_offload_has_one_action(&f->rule->action)) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Only one action per filter is supported");
		return -EOPNOTSUPP;
	}

	lan966x_update_stats(lan966x);

	idx = port->chip_port * lan966x->num_stats;
	action = &f->rule->action.entries[0];

	switch (action->id) {
	case FLOW_ACTION_POLICE:
		if (!ingress) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Policer is not supported on egress");
			return -EOPNOTSUPP;
		}

		if (port->tc.block_shared[1]) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Policer is not supported on shared ingress blocks");
			return -EOPNOTSUPP;
		}

		if (port->tc.police_id && port->tc.police_id != f->cookie) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Only one policer per port is supported");
			return -EEXIST;
		}

		rate = action->police.rate_bytes_ps;
		pol.rate = div_u64(rate, 1000) * 8;
		pol.burst = action->police.burst;

		err = lan966x_tc_port_policer_add(port, &pol);
		if (err) {
			NL_SET_ERR_MSG_MOD(f->common.extack, "Could not add policer");
			return err;
		}

		/* Initialize previous statistics */
		prev_stats = &port->tc.police_stats;
		mutex_lock(&lan966x->stats_lock);

		prev_stats->bytes =
			lan966x->stats[idx + SYS_COUNT_RX_OCT] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_OCT];

		prev_stats->pkts =
			lan966x->stats[idx + SYS_COUNT_RX_UC] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_UC] +
			lan966x->stats[idx + SYS_COUNT_RX_MC] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_MC] +
			lan966x->stats[idx + SYS_COUNT_RX_BC] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_BC];

		prev_stats->drops = 0;
		for (i = 0; i < LAN966X_NUM_TC; i++)
			prev_stats->drops +=
				lan966x->stats[idx + SYS_COUNT_RX_RED_PRIO_0 + i];

		mutex_unlock(&lan966x->stats_lock);

		prev_stats->lastused = jiffies;
		port->tc.police_id = f->cookie;
		break;
	case FLOW_ACTION_MIRRED:
		err = lan966x_mirror_port_add(port, ingress,
					      netdev_priv(action->dev));
		if (err) {
			switch (err) {
			case -EEXIST:
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Mirroring already exists");
				break;
			case -EBUSY:
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot change the mirror monitor port while in use");
				break;
			case -EINVAL:
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Cannot mirror the mirror monitor port");
				break;
			default:
				NL_SET_ERR_MSG_MOD(f->common.extack,
						   "Unknown error");
				break;
			}
			return err;
		}

		/* Initialize previous statistics */
		prev_stats = &port->tc.mirror_stats[ingress];
		mutex_lock(&lan966x->stats_lock);

		if (ingress) {
			prev_stats->bytes =
				lan966x->stats[idx + SYS_COUNT_RX_OCT] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_OCT];

			prev_stats->pkts =
				lan966x->stats[idx + SYS_COUNT_RX_UC] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_UC] +
				lan966x->stats[idx + SYS_COUNT_RX_MC] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_MC] +
				lan966x->stats[idx + SYS_COUNT_RX_BC] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_BC];
		} else {
			prev_stats->bytes =
				lan966x->stats[idx + SYS_COUNT_TX_OCT] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_OCT];

			prev_stats->pkts =
				lan966x->stats[idx + SYS_COUNT_TX_UC] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_UC] +
				lan966x->stats[idx + SYS_COUNT_TX_MC] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_MC] +
				lan966x->stats[idx + SYS_COUNT_TX_BC] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_BC];
		}

		mutex_unlock(&lan966x->stats_lock);

		prev_stats->lastused = jiffies;
		break;
	default:
		NL_SET_ERR_MSG_MOD(f->common.extack, "Unsupported action");
		return -EOPNOTSUPP;
	}

	port->tc.offload_cnt++;

	return 0;
}

static int lan966x_tc_matchall_destroy(struct lan966x_port *port,
				       struct tc_cls_matchall_offload *f,
				       bool ingress)
{
	struct flow_action_entry *action;
	int err;

	action = &f->rule->action.entries[0];

	switch (action->id) {
	case FLOW_ACTION_POLICE:
		if (port->tc.police_id != f->cookie)
			return -ENOENT;

		err = lan966x_tc_port_policer_del(port);
		if (err) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Could not delete policer");
			return err;
		}

		port->tc.police_id = 0;
		break;
	case FLOW_ACTION_MIRRED:
		err = lan966x_mirror_port_del(port, ingress);
		if (err) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Could not delete mirroring");
			return err;
		}
		break;
	default:
		NL_SET_ERR_MSG_MOD(f->common.extack, "Unsupported action");
		return -EOPNOTSUPP;
	}

	port->tc.offload_cnt--;

	return 0;
}

static int lan966x_tc_matchall_stats(struct lan966x_port *port,
				     struct tc_cls_matchall_offload *f,
				     bool ingress)
{
	struct lan966x *lan966x = port->lan966x;
	struct flow_action_entry *action;
	struct flow_stats *prev_stats;
	struct flow_stats stats = {};
	int i, idx;

	lan966x_update_stats(lan966x);

	idx = port->chip_port * lan966x->num_stats;
	action = &f->rule->action.entries[0];

	switch (action->id) {
	case FLOW_ACTION_POLICE:
		prev_stats = &port->tc.police_stats;
		mutex_lock(&lan966x->stats_lock);

		stats.bytes =
			lan966x->stats[idx + SYS_COUNT_RX_OCT] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_OCT];

		if (stats.bytes == prev_stats->bytes) {
			mutex_unlock(&lan966x->stats_lock);
			return 0;
		}

		stats.pkts =
			lan966x->stats[idx + SYS_COUNT_RX_UC] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_UC] +
			lan966x->stats[idx + SYS_COUNT_RX_MC] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_MC] +
			lan966x->stats[idx + SYS_COUNT_RX_BC] +
			lan966x->stats[idx + SYS_COUNT_RX_PMAC_BC];

		for (i = 0; i < LAN966X_NUM_TC; i++)
			stats.drops +=
				lan966x->stats[idx + SYS_COUNT_RX_RED_PRIO_0 + i];

		mutex_unlock(&lan966x->stats_lock);

		flow_stats_update(&f->stats,
				  stats.bytes - prev_stats->bytes,
				  stats.pkts - prev_stats->pkts,
				  stats.drops - prev_stats->drops,
				  prev_stats->lastused,
				  FLOW_ACTION_HW_STATS_IMMEDIATE);

		prev_stats->bytes = stats.bytes;
		prev_stats->pkts = stats.pkts;
		prev_stats->drops = stats.drops;
		prev_stats->lastused = jiffies;
		break;
	case FLOW_ACTION_MIRRED:
		prev_stats = &port->tc.mirror_stats[ingress];
		mutex_lock(&lan966x->stats_lock);

		if (ingress) {
			stats.bytes =
				lan966x->stats[idx + SYS_COUNT_RX_OCT] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_OCT];

			if (stats.bytes == prev_stats->bytes) {
				mutex_unlock(&lan966x->stats_lock);
				return 0;
			}

			stats.pkts =
				lan966x->stats[idx + SYS_COUNT_RX_UC] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_UC] +
				lan966x->stats[idx + SYS_COUNT_RX_MC] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_MC] +
				lan966x->stats[idx + SYS_COUNT_RX_BC] +
				lan966x->stats[idx + SYS_COUNT_RX_PMAC_BC];
		} else {
			stats.bytes =
				lan966x->stats[idx + SYS_COUNT_TX_OCT] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_OCT];

			if (stats.bytes == prev_stats->bytes) {
				mutex_unlock(&lan966x->stats_lock);
				return 0;
			}

			stats.pkts =
				lan966x->stats[idx + SYS_COUNT_TX_UC] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_UC] +
				lan966x->stats[idx + SYS_COUNT_TX_MC] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_MC] +
				lan966x->stats[idx + SYS_COUNT_TX_BC] +
				lan966x->stats[idx + SYS_COUNT_TX_PMAC_BC];
		}

		mutex_unlock(&lan966x->stats_lock);

		flow_stats_update(&f->stats,
				  stats.bytes - prev_stats->bytes,
				  stats.pkts - prev_stats->pkts,
				  0,
				  prev_stats->lastused,
				  FLOW_ACTION_HW_STATS_IMMEDIATE);

		prev_stats->bytes = stats.bytes;
		prev_stats->pkts = stats.pkts;
		prev_stats->lastused = jiffies;
		break;
	default:
		NL_SET_ERR_MSG_MOD(f->common.extack, "Unsupported action");
		return -EOPNOTSUPP;
	}
	return 0;
}

int lan966x_tc_matchall(struct lan966x_port *port,
			struct tc_cls_matchall_offload *f,
			bool ingress)
{
	netdev_dbg(port->dev, "command %s chain %u proto 0x%04x prio %u cookie %lx\n",
		   tc_dbg_tc_matchall_command(f->command), f->common.chain_index,
		   be16_to_cpu(f->common.protocol), f->common.prio, f->cookie);

	if (f->rule) {
		tc_dbg_match_dump(port->dev, f->rule);
		tc_dbg_actions_dump(port->dev, f->rule);
	}

	if (!tc_cls_can_offload_and_chain0(port->dev, &f->common)) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Only chain zero is supported");
		return -EOPNOTSUPP;
	}

	switch (f->command) {
	case TC_CLSMATCHALL_REPLACE:
		return lan966x_tc_matchall_replace(port, f, ingress);
	case TC_CLSMATCHALL_DESTROY:
		return lan966x_tc_matchall_destroy(port, f, ingress);
	case TC_CLSMATCHALL_STATS:
		return lan966x_tc_matchall_stats(port, f, ingress);
	default:
		return -EOPNOTSUPP;
	}
}
