/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2022 Microchip Technology Inc. */

#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include "sparx5_port.h"
#include "sparx5_qos.h"

static u32 sparx5_policer_acl_stat_get(struct sparx5 *sparx5, u32 idx, u32 cnt)
{
	return spx5_rd(sparx5, ANA_AC_PORT_STAT_LSB_CNT(idx, cnt));
}

static u32 debugfs_pol_acl_idx;

static int sparx5_policer_acl_stat_show(struct seq_file *s, void *unused)
{
	struct sparx5 *sparx5 = s->private;
	u32 count;

	count = sparx5_policer_acl_stat_get(sparx5, debugfs_pol_acl_idx, 0);
	seq_printf(s, "%d\n", count);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_policer_acl_stat);

static void sparx5_port_policer_show(struct seq_file *s,
				     struct sparx5_port *port)
{
	struct flow_stats *stats;
	u32 mask, burst, rate;
	int polidx;

	for (polidx = 0; polidx < SPX5_POLICERS_PER_PORT; ++polidx) {
		mask = ANA_AC_POL_POL_PORT_CFG_TRAFFIC_TYPE_MASK_GET(
			spx5_rd(port->sparx5,
				ANA_AC_POL_POL_PORT_CFG(port->portno, polidx)));
		if (!mask)
			continue;
		rate = spx5_rd(port->sparx5,
			       ANA_AC_POL_POL_PORT_RATE_CFG(
				       port->portno * SPX5_POLICERS_PER_PORT +
				       polidx));
		burst = spx5_rd(port->sparx5,
				ANA_AC_POL_POL_PORT_THRES_CFG_0(
					port->portno * SPX5_POLICERS_PER_PORT +
					polidx));

		sparx5_policer_port_stats_update(port, polidx);
		stats = &port->tc.port_policer[polidx].stats;
		seq_printf(s, "  %02d: %d: rate: %llu, burst: %d: passed: %llu, dropped: %llu:",
			   port->portno, polidx,
			   (u64)rate * SPX5_POLICER_RATE_UNIT,
			   burst * SPX5_POLICER_BYTE_BURST_UNIT,
			   stats->pkts,
			   stats->drops);
		if (mask & BIT(0))
			seq_printf(s, " known-MC");
		if (mask & BIT(1))
			seq_printf(s, " known-BC");
		if (mask & BIT(2))
			seq_printf(s, " known-UC");
		if (mask & BIT(3))
			seq_printf(s, " unknown-MC");
		if (mask & BIT(4))
			seq_printf(s, " unknown-BC");
		if (mask & BIT(5))
			seq_printf(s, " unknown-UC");
		if (mask & BIT(6))
			seq_printf(s, " cpu-q-bypass ");
		else
			seq_printf(s, " cpu-q ");
		if (mask & BIT(7))
			seq_printf(s, " lrn");
		seq_printf(s, "\n");
	}
}

static int sparx5_port_policers_debugfs_show(struct seq_file *s, void *unused)
{
	struct sparx5 *sparx5 = s->private;
	int portno;

	seq_printf(s, "Port Policers\n");
	for (portno = 0; portno < SPX5_PORTS; ++portno) {
		if (sparx5->ports[portno])
			sparx5_port_policer_show(s, sparx5->ports[portno]);

	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_port_policers_debugfs);

static u32 debugfs_layer_idx;
static u32 debugfs_se_idx;

/**
 * Export layer information to debugfs.
 *
 * Scheduler element information is only exported for a single element of the
 * layer. Layer- and scheduler element index is set through writeable debugfs
 * files.
 */
static int sparx5_layer_debugfs_show(struct seq_file *s, void *unused)
{
	struct sparx5 *sparx5 = s->private;
	struct sparx5_lg *lg;
	u32 itr, first, next, dwrr_count, dwrr_cost;
	const char *status;
	char buf[128];
	int i;

	seq_printf(s, "Layer #%d\n", debugfs_layer_idx);
	seq_printf(s, "scheduler element: #%d\n", debugfs_se_idx);

	spx5_rmw(HSCH_HSCH_CFG_CFG_HSCH_LAYER_SET(debugfs_layer_idx),
		HSCH_HSCH_CFG_CFG_HSCH_LAYER, sparx5, HSCH_HSCH_CFG_CFG);

	dwrr_count = HSCH_SE_CFG_SE_DWRR_CNT_GET(spx5_rd(sparx5,
		HSCH_SE_CFG(debugfs_se_idx)));

	seq_printf(s, "dwrr:\n");
	seq_printf(s, "  count: %d\n", dwrr_count);
	seq_printf(s, "  cost: [");
	for (i = 0; i < dwrr_count; i++) {
		dwrr_cost = HSCH_DWRR_ENTRY_DWRR_COST_GET(spx5_rd(sparx5,
			HSCH_DWRR_ENTRY(i)));
		seq_printf(s, " %d", dwrr_cost);
	}
	seq_printf(s, " ]\n");

	seq_printf(s, "Leak groups:\n");

	for (i = 0; i < SPX5_HSCH_LEAK_GRP_CNT; i++) {

		lg = &sparx5_layers[debugfs_layer_idx].leak_groups[i];

		if (sparx5_lg_is_empty(sparx5, debugfs_layer_idx, i))
			status = "Disabled";
		else
			status = "Enabled";

		seq_printf(s,""
			"  idx: %d\n"
			"  status: %s\n"
			"  leak_time: %d\n"
			"  resolution: %d\n"
			"  max_elements: %d\n",
			i,
			status,
			lg->leak_time,
			lg->resolution,
			lg->max_ses);


		first = sparx5_lg_get_first(sparx5, debugfs_layer_idx, i);
		itr = first;

		*buf = '\0';

		for (;;) {
			next = sparx5_lg_get_next(sparx5, debugfs_layer_idx, i, itr);

			snprintf(buf + strlen(buf), sizeof(buf) +
				strlen(buf), "%d -> ", itr);

			if (itr == next) {
				snprintf(buf + strlen(buf), sizeof(buf) -
					strlen(buf), "%d", next);
				break;
			}

			itr = next;
		}

		seq_printf(s, "  leak_list: %s\n\n", buf);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_layer_debugfs);

static int sparx5_psfp_debugfs_show(struct seq_file *s, void *unused)
{
	u32 group, xlb_next, xlb_start, xlb_itr, xlb_pup_token,
		group_pup_interval, group_thres_shift, gate_max_octets, sfid,
		fmid, sgid, gate_state, gate_ips, gate_interval, pending, isdx,
		enabled;
	struct sparx5 *sparx5 = s->private;
	struct sparx5_pool_entry *e;
	int i, ii, gate_num_entries;
	struct timespec64 ts;
	const char *status;
	char buf[128];

	for (i = 0; i < SPX5_PSFP_SF_CNT; i++) {
		isdx = sparx5_pool_idx_to_id(i);
		e = &sparx5_psfp_sf_pool[i];

		if (e->ref_cnt <= 0)
		 	continue;

		/* Get sfid, sgid and fmid */
		sfid = ANA_L2_TSN_CFG_TSN_SFID_GET(
			spx5_rd(sparx5, ANA_L2_TSN_CFG(isdx)));
		fmid = ANA_L2_DLB_CFG_DLB_IDX_GET(
			spx5_rd(sparx5, ANA_L2_DLB_CFG(isdx)));
		sgid = ANA_AC_TSN_SF_CFG_TSN_SGID_GET(
			spx5_rd(sparx5, ANA_AC_TSN_SF_CFG(sfid)));

		/* Set stream gate id */
		spx5_wr(ANA_AC_SG_ACCESS_CTRL_SGID_SET(sgid), sparx5,
			ANA_AC_SG_ACCESS_CTRL);

		/* Get xlb's from lb group */
		group = ANA_AC_SDLB_XLB_NEXT_LBGRP_GET(
			spx5_rd(sparx5, ANA_AC_SDLB_XLB_NEXT(fmid)));

		/* Get lb group settings */
		group_pup_interval = ANA_AC_SDLB_PUP_INTERVAL_PUP_INTERVAL_GET(
			spx5_rd(sparx5, ANA_AC_SDLB_PUP_INTERVAL(group)));
		group_thres_shift = ANA_AC_SDLB_LBGRP_MISC_THRES_SHIFT_GET(
			spx5_rd(sparx5, ANA_AC_SDLB_LBGRP_MISC(group)));

		xlb_pup_token = ANA_AC_SDLB_PUP_TOKENS_PUP_TOKENS_GET(
			spx5_rd(sparx5, ANA_AC_SDLB_PUP_TOKENS(fmid, 0)));

		/* Gate PSFP Oper values */
		gate_ips = ANA_AC_SG_STATUS_REG_3_IPS_GET(
			spx5_rd(sparx5, ANA_AC_SG_STATUS_REG_3));
		gate_state = ANA_AC_SG_STATUS_REG_3_GATE_STATE_GET(
			spx5_rd(sparx5, ANA_AC_SG_STATUS_REG_3));

		/* Get PTP time */
		sparx5_ptp_gettime64(&sparx5->phc[SPARX5_PHC_PORT].info, &ts);

		gate_num_entries = ANA_AC_SG_CONFIG_REG_3_LIST_LENGTH_GET(
			spx5_rd(sparx5, ANA_AC_SG_CONFIG_REG_3));

		pending = ANA_AC_SG_STATUS_REG_3_CONFIG_PENDING_GET(
			spx5_rd(sparx5, ANA_AC_SG_STATUS_REG_3));

		enabled = ANA_AC_SG_CONFIG_REG_3_GATE_ENABLE_GET(
			spx5_rd(sparx5, ANA_AC_SG_CONFIG_REG_3));

		seq_printf(s, "\nisdx: %d\n", isdx);

		if (sparx5_sdlb_group_is_empty(sparx5, group))
			status = "Disabled";
		else
			status = "Enabled";

		/* Print lb group information */
		seq_printf(s, "  sfid: %d\n", sfid);
		seq_printf(s, "    sgid: %d (%s)\n", sgid,
			   sgid == 0 ? "Disabled" : "Enabled");
		seq_printf(s,
			   ""
			   "      init_ips: %d\n"
			   "      init_gate_state: %d\n"
			   "      num_gcl_entries: %d\n"
			   "      config_pending: %d\n"
			   "      enabled: %d\n",
			   gate_ips, gate_state, gate_num_entries, pending, enabled);
		for (ii = 0; ii < gate_num_entries; ii++) {
			gate_state = ANA_AC_SG_GCL_GS_CONFIG_GATE_STATE_GET(
				spx5_rd(sparx5, ANA_AC_SG_GCL_GS_CONFIG(ii)));
			gate_ips = ANA_AC_SG_GCL_GS_CONFIG_IPS_GET(
				spx5_rd(sparx5, ANA_AC_SG_GCL_GS_CONFIG(ii)));
			gate_interval =
				spx5_rd(sparx5, ANA_AC_SG_GCL_TI_CONFIG(ii));
			gate_max_octets =
				spx5_rd(sparx5, ANA_AC_SG_GCL_OCT_CONFIG(ii));
			seq_printf(s,
				   ""
				   "      gcl_entry: %d\n"
				   "        state: %d\n"
				   "        ips: %d\n"
				   "        interval: %d\n"
				   "        max_octets: %d\n",
				   ii, gate_state, gate_ips, gate_interval,
				   gate_max_octets);
		}

		seq_printf(s, "  fmid: %d (%s)\n", fmid,
			   fmid == 0 ? "Disabled" : "Enabled");
		seq_printf(s, "    rate: %llu\n",
			   div64_u64((xlb_pup_token * 5000000000),
				     group_pup_interval));
		seq_printf(s, "    lb group: %d (%s):\n", group,
			   sparx5_sdlb_group_is_empty(sparx5, group) ?
				   "Disabled" :
				   "Enabled");
		seq_printf(s,
			   ""
			   "      pup_interval: %d\n"
			   "      thres_shift: %d\n",
			   group_pup_interval, group_thres_shift);

		xlb_start = sparx5_sdlb_group_get_first(sparx5, group);
		xlb_itr = xlb_start;

		*buf = '\0';

		for (;;) {
			xlb_next = sparx5_sdlb_group_get_next(sparx5, group,
							      xlb_itr);

			snprintf(buf + strlen(buf), sizeof(buf) + strlen(buf),
				 "%d -> ", xlb_itr);

			if (xlb_itr == xlb_next) {
				snprintf(buf + strlen(buf),
					 sizeof(buf) - strlen(buf), "%d",
					 xlb_next);
				break;
			}

			xlb_itr = xlb_next;
		}

		seq_printf(s, "      leak_list: %s\n", buf);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_psfp_debugfs);

#define P(X, Y) \
	seq_printf(m, "%-20s: %12d\n", X, Y)
#define P_STR(X, Y) \
	seq_printf(m, "%-20s: %12s\n", X, Y)
#define P_TIME(X, S, NS) \
	seq_printf(m, "%-20s: %12llu.%09llu sec\n",\
		   X, (unsigned long long)S, (unsigned long long)NS)

static int sparx5_tas_show(struct seq_file *m, void *unused)
{
	u32 sec, nsec, val, base, time, ctrl, gcl, length;
	int pidx, entry, list, state;
	struct sparx5_port *port;
	struct sparx5 *sparx5;
	struct timespec64 ts;
	int num_tas_lists;
	int num_ports;

	sparx5 = m->private;
	port = sparx5->ports[0];
	num_ports = sparx5->port_count;
	num_tas_lists = num_ports * SPX5_TAS_ENTRIES_PER_PORT;

	P("num_ports", num_ports);
	P("num_tas_lists", num_tas_lists);
	sparx5_ptp_gettime64(&sparx5->phc[SPARX5_PHC_PORT].info,
			     &ts);
	P_TIME("current time", ts.tv_sec, ts.tv_nsec);

	mutex_lock(&sparx5->tas_lock);
	for (pidx = 0; pidx < SPX5_PORTS; pidx++) {
		port = sparx5->ports[pidx];
		if (!port)
			continue;
		for (entry = 0; entry < SPX5_TAS_ENTRIES_PER_PORT; entry++) {
			list = sparx5_tas_list_index(port, entry);
			spx5_rmw(HSCH_TAS_CFG_CTRL_LIST_NUM_SET(list),
				 HSCH_TAS_CFG_CTRL_LIST_NUM,
				 sparx5,
				 HSCH_TAS_CFG_CTRL);
			val = spx5_rd(sparx5, HSCH_TAS_LIST_STATE);
			state = HSCH_TAS_LIST_STATE_LIST_STATE_GET(val);
			if (state == SPX5_TAS_STATE_ADMIN)
				continue;

			seq_printf(m, "\n%s:\n", port->ndev->name);
			P(" portno", port->portno);
			P(" entry", entry);
			P(" list", list);
			P_STR(" state", sparx5_tas_state_to_str(state));
			sec = spx5_rd(sparx5, HSCH_TAS_BASE_TIME_SEC_LSB);
			nsec = spx5_rd(sparx5, HSCH_TAS_BASE_TIME_NSEC);
			P_TIME(" base time", sec, nsec);
			ctrl = spx5_rd(sparx5, HSCH_TAS_CYCLE_TIME_CFG);
			P_TIME(" cycle_time", 0, ctrl);
			val = spx5_rd(sparx5, HSCH_TAS_LIST_CFG);
			base = HSCH_TAS_LIST_CFG_LIST_BASE_ADDR_GET(val);
			length = HSCH_TAS_LIST_CFG_LIST_LENGTH_GET(val);
			P(" gcl base", base);
			P(" gcl length", length);
			for (gcl = 0; gcl < length; gcl++) {
				spx5_rmw(HSCH_TAS_CFG_CTRL_GCL_ENTRY_NUM_SET(gcl),
					 HSCH_TAS_CFG_CTRL_GCL_ENTRY_NUM,
					 sparx5,
					 HSCH_TAS_CFG_CTRL);

				ctrl = spx5_rd(sparx5, HSCH_TAS_GCL_CTRL_CFG);
				time = spx5_rd(sparx5, HSCH_TAS_GCL_TIME_CFG);

				seq_printf(m, "  gcl %d: command %lu gatemask 0x%02lx interval %u ns\n",
					   base + gcl,
					   0l,
					   HSCH_TAS_GCL_CTRL_CFG_GATE_STATE_GET(ctrl),
					   time);
			}
		}
	}
	mutex_unlock(&sparx5->tas_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sparx5_tas);

void sparx5_qos_debugfs(struct sparx5 *sparx5)
{
	debugfs_create_x32("pol_acl_idx", 0644, sparx5->debugfs_root,
			   &debugfs_pol_acl_idx);
	debugfs_create_file("pol_acl_stat", 0444, sparx5->debugfs_root,
			    sparx5, &sparx5_policer_acl_stat_fops);

	debugfs_create_file("port_policers", 0444, sparx5->debugfs_root,
			    sparx5, &sparx5_port_policers_debugfs_fops);
	debugfs_create_x32("layer_idx", 0666, sparx5->debugfs_root,
			   &debugfs_layer_idx);
	debugfs_create_x32("se_idx", 0666, sparx5->debugfs_root,
			   &debugfs_se_idx);
	debugfs_create_file("layer", 0444, sparx5->debugfs_root, sparx5,
			    &sparx5_layer_debugfs_fops);
	debugfs_create_file("tas_show", 0444, sparx5->debugfs_root,
			    sparx5, &sparx5_tas_fops);
	debugfs_create_file("psfp", 0444, sparx5->debugfs_root, sparx5,
			    &sparx5_psfp_debugfs_fops);
}
