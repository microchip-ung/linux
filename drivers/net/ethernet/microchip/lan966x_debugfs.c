// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <linux/netdevice.h>
#include <linux/phy/phy.h>

#include "lan966x_main.h"

/* Watermark decode */
static u32 lan966x_wm_dec(u32 value, bool bytes)
{
	if (value & MULTIPLIER_BIT) {
		value = ((value & (MULTIPLIER_BIT - 1)) * 16);
	}
	if (bytes) {
		value *= LAN966X_BUFFER_CELL_SZ;
	}
	return value;
}

static u32 lan966x_wm_dec_bytes(u32 value)
{
	return lan966x_wm_dec(value, 1);
}


static u32 lan966x_wm_dec_frames(u32 value)
{
	return lan966x_wm_dec(value, 0);
}

static void lan966x_wm_queue_show(struct seq_file *m, const char *name,
				  u32 *val, bool bytes)
{
	int q;
	seq_printf(m, "%-26s", name);
	for (q = 0; q < 8; q++) {
		if (val)
			seq_printf(m, "%6u ", lan966x_wm_dec(val[q], bytes));
		else
			seq_printf(m, "%6u ", q);
	}
	seq_printf(m, "\n");
}

static void lan966x_wm_port_status_show(struct seq_file *m,
					struct lan966x_port *p)
{
	struct lan966x *lan966x = m->private;
	u32 value;
	u8 port;
	int q;

	if (p)
		port = p->chip_port;
	else
		port = CPU_PORT;

	for (q = 0; q < 8; q++) {
		value = lan_rd(lan966x, QSYS_RES_STAT(port * 8 + q + 0));
		if (value > 0) {
			seq_printf(m, "Chip port %u (%s), ingress qu %u: Inuse:%u bytes, Maxuse:%u bytes\n",
			   port, p ? p->dev->name : "cpu", q,
			   lan966x_wm_dec_bytes(QSYS_RES_STAT_INUSE_GET(value)),
			   lan966x_wm_dec_bytes(QSYS_RES_STAT_MAXUSE_GET(value)));
		}
	}
}

static void lan966x_wm_port_show(struct seq_file *m, struct lan966x_port *p)
{
	u32 val1[8] = {0}, val2[8] = {0}, val3[8] = {0}, val4[8] = {0};
	struct lan966x *lan966x = m->private;
	u32 value;
	u8 port;
	int q;

	if (p) {
		port = p->chip_port;
		seq_printf(m, "Port : %u (%s)\n", port, p->dev->name);
		seq_printf(m, "---------------\n");
		value = lan_rd(lan966x, SYS_MAC_FC_CFG(port));
		seq_printf(m, "FC Pause Tx ena     : %lu\n",
			   SYS_MAC_FC_CFG_TX_FC_ENA_GET(value));
		seq_printf(m, "FC Pause Rx ena     : %lu\n",
			   SYS_MAC_FC_CFG_RX_FC_ENA_GET(value));
		seq_printf(m, "FC Pause Time Value : 0x%lx\n",
			   SYS_MAC_FC_CFG_PAUSE_VAL_CFG_GET(value));
		seq_printf(m, "FC Zero pause       : %lu\n",
			   SYS_MAC_FC_CFG_ZERO_PAUSE_ENA_GET(value));
		value = lan_rd(lan966x, SYS_PAUSE_CFG(port));
		seq_printf(m, "FC Pause Ena        : %lu\n",
			   SYS_PAUSE_CFG_PAUSE_ENA_GET(value));
		seq_printf(m, "FC Pause Start WM   : %u bytes\n",
			   lan966x_wm_dec_bytes(SYS_PAUSE_CFG_PAUSE_START_GET(value)));
		seq_printf(m, "FC Pause Stop WM    : %u bytes\n",
			   lan966x_wm_dec_bytes(SYS_PAUSE_CFG_PAUSE_STOP_GET(value)));
		value = lan_rd(lan966x, ANA_PFC_CFG(port));
		value = ANA_PFC_CFG_RX_PFC_ENA_GET(value);
		seq_printf(m, "PFC Enable [0-7]    : ");
		for (q = 0; q < 8; q++) {
			seq_printf(m, "%u", value & BIT(q) ? 1 : 0);
		}
		seq_printf(m, "\n\n");
	} else {
		/* CPU port */
		port = CPU_PORT;
		seq_printf(m, "Port : %u (cpu)\n", port);
		seq_printf(m, "--------------\n");
	}

	value = lan_rd(lan966x, SYS_ATOP(port));
	seq_printf(m, "FC TailDrop ATOP WM : %u bytes\n",
		   lan966x_wm_dec_bytes(SYS_ATOP_ATOP_GET(value)));
	value = lan_rd(lan966x, QSYS_SW_PORT_MODE(port));
	seq_printf(m, "Ingress Drop Mode   : %lu\n",
		   QSYS_SW_PORT_MODE_INGRESS_DROP_MODE_GET(value));
	value = lan_rd(lan966x, QSYS_EGR_DROP_MODE);
	seq_printf(m, "Egress Drop Mode    : %u\n",
		   QSYS_EGR_DROP_MODE_EGRESS_DROP_MODE_GET(value) & BIT(port) ? 1 : 0);
	value = lan_rd(lan966x, QSYS_IGR_NO_SHARING);
	seq_printf(m, "Ingress No Sharing  : %u\n",
		   QSYS_IGR_NO_SHARING_IGR_NO_SHARING_GET(value) & BIT(port) ? 1 : 0);
	value = lan_rd(lan966x, QSYS_EGR_NO_SHARING);
	seq_printf(m, "Egress No Sharing   : %u\n",
		   QSYS_EGR_NO_SHARING_EGR_NO_SHARING_GET(value) & BIT(port) ? 1 : 0);
	value = lan_rd(lan966x, QSYS_PORT_MODE(port));
	seq_printf(m, "Dequeuing disabled  : %lu\n",
		   QSYS_PORT_MODE_DEQUEUE_DIS_GET(value));
	seq_printf(m, "\n");

	for (q = 0; q < 8; q++) {
		val1[q] = lan_rd(lan966x, QSYS_RES_CFG(port * 8 + q + 0));
		val2[q] = lan_rd(lan966x, QSYS_RES_CFG(port * 8 + q + 256));
		val3[q] = lan_rd(lan966x, QSYS_RES_CFG(port * 8 + q + 512));
		val4[q] = lan_rd(lan966x, QSYS_RES_CFG(port * 8 + q + 768));
	}
	lan966x_wm_queue_show(m, "Queue level rsrv WMs:", NULL, false);
	lan966x_wm_queue_show(m, "Qu Ingr Buf Rsrv (Bytes) :", val1, true);
	lan966x_wm_queue_show(m, "Qu Ingr Ref Rsrv (Frames):", val2, false);
	lan966x_wm_queue_show(m, "Qu Egr Buf Rsrv  (Bytes) :", val3, true);
	lan966x_wm_queue_show(m, "Qu Egr Ref Rsrv  (Frames):", val4, false);
	seq_printf(m, "\n");

	/* Configure reserved space for port */
	val1[0] = lan_rd(lan966x, QSYS_RES_CFG(port + 224 +   0));
	val2[0] = lan_rd(lan966x, QSYS_RES_CFG(port + 224 + 256));
	val3[0] = lan_rd(lan966x, QSYS_RES_CFG(port + 224 + 512));
	val4[0] = lan_rd(lan966x, QSYS_RES_CFG(port + 224 + 768));
	seq_printf(m, "Port level rsrv WMs:\n");
	seq_printf(m, "Port Ingress Buf Rsrv: %u Bytes\n",
		   lan966x_wm_dec_bytes(val1[0]));
	seq_printf(m, "Port Ingress Ref Rsrv: %u Frames\n",
		   lan966x_wm_dec_frames(val2[0]));
	seq_printf(m, "Port Egress  Buf Rsrv: %u Bytes\n",
		   lan966x_wm_dec_bytes(val3[0]));
	seq_printf(m, "Port Egress  Ref Rsrv: %u Frames\n",
		   lan966x_wm_dec_frames(val4[0]));
	seq_printf(m, "\n");
}

static int lan966x_wm_show(struct seq_file *m, void *unused)
{
	u32 val1[8] = {0}, val2[8] = {0}, val3[8] = {0}, val4[8] = {0}, val5[8] = {0};
	struct lan966x *lan966x = m->private;
	const char *txt;
	int i, q, dp;
	u32 value;

	seq_printf(m, "Global configuration:\n");
	seq_printf(m, "---------------------\n");
	seq_printf(m, "Total buffer memory     : %d bytes\n",
		   LAN966X_BUFFER_MEMORY);
	seq_printf(m, "Total frame references  : %d frames\n",
		   LAN966X_BUFFER_REFERENCE);
	seq_printf(m, "\n");
	value = lan_rd(lan966x, SYS_PAUSE_TOT_CFG);
	seq_printf(m, "FC Pause TOT_START WM   : %d bytes\n",
		   lan966x_wm_dec_bytes(SYS_PAUSE_TOT_CFG_PAUSE_TOT_START_GET(value)));
	seq_printf(m, "FC Pause TOT_STOP WM    : %d bytes\n",
		   lan966x_wm_dec_bytes(SYS_PAUSE_TOT_CFG_PAUSE_TOT_STOP_GET(value)));
	value = lan_rd(lan966x, SYS_ATOP_TOT_CFG);
	seq_printf(m, "FC TailDrop ATOP_TOT WM : %d bytes\n",
		   lan966x_wm_dec_bytes(SYS_ATOP_TOT_CFG_ATOP_TOT_GET(value)));
	seq_printf(m, "\n");

	/* Front ports */
	for (i = 0; i < lan966x->num_phys_ports; i++) {
		if (!lan966x->ports[i])
			continue;
		lan966x_wm_port_show(m, lan966x->ports[i]);
	}

	/* CPU port */
	lan966x_wm_port_show(m, NULL);

	seq_printf(m, "Shared :\n");
	seq_printf(m, "--------\n");
	/* Shared space for all QoS classes */
	value = lan_rd(lan966x, QSYS_RES_QOS_MODE);
	value = QSYS_RES_QOS_MODE_RES_QOS_RSRVD_GET(value);

	for (q = 0; q < 8; q++) {
		val1[q] = lan_rd(lan966x, QSYS_RES_CFG(q + 216 +   0));
		val2[q] = lan_rd(lan966x, QSYS_RES_CFG(q + 216 + 256));
		val3[q] = lan_rd(lan966x, QSYS_RES_CFG(q + 216 + 512));
		val4[q] = lan_rd(lan966x, QSYS_RES_CFG(q + 216 + 768));
		val5[q] = (value & BIT(q) ? 1 : 0);
	}
	lan966x_wm_queue_show(m, "QoS level:", NULL, false);
	lan966x_wm_queue_show(m, "QoS Ingr Buf (Bytes) :", val1, true);
	lan966x_wm_queue_show(m, "QoS Ingr Ref (Frames):", val2, false);
	lan966x_wm_queue_show(m, "QoS Egr Buf  (Bytes) :", val3, true);
	lan966x_wm_queue_show(m, "QoS Egr Ref  (Frames):", val4, false);
	lan966x_wm_queue_show(m, "QoS Reservation Mode :", val5, false);
	seq_printf(m, "\n");

	seq_printf(m, "Color level:\n");
	seq_printf(m, "------------\n");
	/* Configure shared space for both DP levels         */
	/* In this context dp:0 is yellow and dp:1 is green */
	for (dp = 0; dp < 2; dp++) {
		val1[0] = lan_rd(lan966x, QSYS_RES_CFG(dp + 254 +   0));
		val2[0] = lan_rd(lan966x, QSYS_RES_CFG(dp + 254 + 256));
		val3[0] = lan_rd(lan966x, QSYS_RES_CFG(dp + 254 + 512));
		val4[0] = lan_rd(lan966x, QSYS_RES_CFG(dp + 254 + 768));
		txt = (dp ? "Green " : "Yellow");
		seq_printf(m, "Port DP:%s Ingress Buf : %u Bytes\n", txt,
			   lan966x_wm_dec_bytes(val1[0]));
		seq_printf(m, "Port DP:%s Ingress Ref : %u Frames\n",txt,
			   lan966x_wm_dec_frames(val2[0]));
		seq_printf(m, "Port DP:%s Egress  Buf : %u Bytes\n", txt,
			   lan966x_wm_dec_bytes(val3[0]));
		seq_printf(m, "Port DP:%s Egress  Ref : %u Frames\n",txt,
			   lan966x_wm_dec_frames(val4[0]));
	}
	seq_printf(m, "\n");

	seq_printf(m, "WRED config:\n");
	seq_printf(m, "------------\n");
	seq_printf(m, "Queue Dpl WM_HIGH  bytes RED_LOW  bytes RED_HIGH  bytes\n");
	//  xxxxx xxx 0xxxxx xxxxxxx 0xxxxx xxxxxxx 0xxxxx  xxxxxxx
	for (q = 0; q < 8; q++) {
		u32 wm_high, red_profile, wm_red_low, wm_red_high;
		/* Shared ingress high watermark for queue */
		wm_high = lan_rd(lan966x, QSYS_RES_CFG(q + 216));
		for (dp = 0; dp < 2; dp++) {
			/* Red profile for queue, dpl */
			red_profile = lan_rd(lan966x, QSYS_RED_PROFILE(q + (8 * dp)));
			wm_red_low  = QSYS_RED_PROFILE_WM_RED_LOW_GET(red_profile);
			wm_red_high = QSYS_RED_PROFILE_WM_RED_HIGH_GET(red_profile);
			seq_printf(m, "%5u %3u  0x%04x %6u  0x%04x %6u   0x%04x %6u\n",
			   q,
			   dp,
			   wm_high,
			   lan966x_wm_dec_bytes(wm_high),
			   wm_red_low,
			   wm_red_low * 1024,
			   wm_red_high,
			   wm_red_high * 1024);
		}
	}
	seq_printf(m, "\n");

	/* Front ports */
	for (i = 0; i < lan966x->num_phys_ports; i++) {
		if (!lan966x->ports[i])
			continue;
		lan966x_wm_port_status_show(m, lan966x->ports[i]);
	}

	/* CPU port */
	lan966x_wm_port_status_show(m, NULL);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_wm);

static int lan966x_cpu_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	int i;

	/* check and update now */
	lan966x_update_stats(lan966x);

	/* Copy all counters */
	for (i = 0; i < lan966x->num_stats; i++)
		seq_printf(m, "%s: %lld\n",
			   lan966x->stats_layout[i].name,
			   lan966x->stats[CPU_PORT * lan966x->num_stats + i]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_cpu);

void lan966x_debugfs_init(struct lan966x *lan966x)
{
	debugfs_create_file("wm_show", 0444, lan966x->debugfs_root, lan966x,
			&lan966x_wm_fops);
	debugfs_create_file("cpu_counters", 0444, lan966x->debugfs_root, lan966x,
			    &lan966x_cpu_fops);
}
