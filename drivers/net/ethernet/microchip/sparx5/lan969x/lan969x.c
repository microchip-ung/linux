// SPDX-License-Identifier: GPL-2.0+

#include "lan969x.h"

#define LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI  10
#define LAN969X_DSM_CAL_TAXIS               5
#define LAN969X_SDLB_GROUP_COUNT            5

static const struct sparx5_main_io_resource lan969x_main_iomap[] =  {
	{ TARGET_CPU,                   0xc0000, 0 }, /* 0xe00c0000 */
	{ TARGET_FDMA,                  0xc0400, 0 }, /* 0xe00c0400 */
	{ TARGET_GCB,                 0x2010000, 1 }, /* 0xe2010000 */
	{ TARGET_QS,                  0x2030000, 1 }, /* 0xe2030000 */
	{ TARGET_PTP,                 0x2040000, 1 }, /* 0xe2040000 */
	{ TARGET_ANA_ACL,             0x2050000, 1 }, /* 0xe2050000 */
	{ TARGET_LRN,                 0x2060000, 1 }, /* 0xe2060000 */
	{ TARGET_VCAP_SUPER,          0x2080000, 1 }, /* 0xe2080000 */
	{ TARGET_QSYS,                0x20a0000, 1 }, /* 0xe20a0000 */
	{ TARGET_QFWD,                0x20b0000, 1 }, /* 0xe20b0000 */
	{ TARGET_XQS,                 0x20c0000, 1 }, /* 0xe20c0000 */
	{ TARGET_VCAP_ES2,            0x20d0000, 1 }, /* 0xe20d0000 */
	{ TARGET_VCAP_ES0,            0x20e0000, 1 }, /* 0xe20e0000 */
	{ TARGET_ANA_AC_POL,          0x2200000, 1 }, /* 0xe2200000 */
	{ TARGET_QRES,                0x2280000, 1 }, /* 0xe2280000 */
	{ TARGET_EACL,                0x22c0000, 1 }, /* 0xe22c0000 */
	{ TARGET_ANA_CL,              0x2400000, 1 }, /* 0xe2400000 */
	{ TARGET_ANA_L3,              0x2480000, 1 }, /* 0xe2480000 */
	{ TARGET_ANA_AC_SDLB,         0x2500000, 1 }, /* 0xe2500000 */
	{ TARGET_HSCH,                0x2580000, 1 }, /* 0xe2580000 */
	{ TARGET_REW,                 0x2600000, 1 }, /* 0xe2600000 */
	{ TARGET_ANA_L2,              0x2800000, 1 }, /* 0xe2800000 */
	{ TARGET_ANA_AC,              0x2900000, 1 }, /* 0xe2900000 */
	{ TARGET_VOP,                 0x2a00000, 1 }, /* 0xe2a00000 */
	{ TARGET_DEV2G5,              0x3004000, 1 }, /* 0xe3004000 */
	{ TARGET_DEV10G,              0x3008000, 1 }, /* 0xe3008000 */
	{ TARGET_PCS10G_BR,           0x300c000, 1 }, /* 0xe300c000 */
	{ TARGET_DEV2G5 +  1,         0x3010000, 1 }, /* 0xe3010000 */
	{ TARGET_DEV2G5 +  2,         0x3014000, 1 }, /* 0xe3014000 */
	{ TARGET_DEV2G5 +  3,         0x3018000, 1 }, /* 0xe3018000 */
	{ TARGET_DEV2G5 +  4,         0x301c000, 1 }, /* 0xe301c000 */
	{ TARGET_DEV10G +  1,         0x3020000, 1 }, /* 0xe3020000 */
	{ TARGET_PCS10G_BR +  1,      0x3024000, 1 }, /* 0xe3024000 */
	{ TARGET_DEV2G5 +  5,         0x3028000, 1 }, /* 0xe3028000 */
	{ TARGET_DEV2G5 +  6,         0x302c000, 1 }, /* 0xe302c000 */
	{ TARGET_DEV2G5 +  7,         0x3030000, 1 }, /* 0xe3030000 */
	{ TARGET_DEV2G5 +  8,         0x3034000, 1 }, /* 0xe3034000 */
	{ TARGET_DEV10G +  2,         0x3038000, 1 }, /* 0xe3038000 */
	{ TARGET_PCS10G_BR +  2,      0x303c000, 1 }, /* 0xe303c000 */
	{ TARGET_DEV2G5 +  9,         0x3040000, 1 }, /* 0xe3040000 */
	{ TARGET_DEV5G,               0x3044000, 1 }, /* 0xe3044000 */
	{ TARGET_PCS5G_BR,            0x3048000, 1 }, /* 0xe3048000 */
	{ TARGET_DEV2G5 + 10,         0x304c000, 1 }, /* 0xe304c000 */
	{ TARGET_DEV2G5 + 11,         0x3050000, 1 }, /* 0xe3050000 */
	{ TARGET_DEV2G5 + 12,         0x3054000, 1 }, /* 0xe3054000 */
	{ TARGET_DEV10G +  3,         0x3058000, 1 }, /* 0xe3058000 */
	{ TARGET_PCS10G_BR +  3,      0x305c000, 1 }, /* 0xe305c000 */
	{ TARGET_DEV2G5 + 13,         0x3060000, 1 }, /* 0xe3060000 */
	{ TARGET_DEV5G +  1,          0x3064000, 1 }, /* 0xe3064000 */
	{ TARGET_PCS5G_BR +  1,       0x3068000, 1 }, /* 0xe3068000 */
	{ TARGET_DEV2G5 + 14,         0x306c000, 1 }, /* 0xe306c000 */
	{ TARGET_DEV2G5 + 15,         0x3070000, 1 }, /* 0xe3070000 */
	{ TARGET_DEV2G5 + 16,         0x3074000, 1 }, /* 0xe3074000 */
	{ TARGET_DEV10G +  4,         0x3078000, 1 }, /* 0xe3078000 */
	{ TARGET_PCS10G_BR +  4,      0x307c000, 1 }, /* 0xe307c000 */
	{ TARGET_DEV2G5 + 17,         0x3080000, 1 }, /* 0xe3080000 */
	{ TARGET_DEV5G +  2,          0x3084000, 1 }, /* 0xe3084000 */
	{ TARGET_PCS5G_BR +  2,       0x3088000, 1 }, /* 0xe3088000 */
	{ TARGET_DEV2G5 + 18,         0x308c000, 1 }, /* 0xe308c000 */
	{ TARGET_DEV2G5 + 19,         0x3090000, 1 }, /* 0xe3090000 */
	{ TARGET_DEV2G5 + 20,         0x3094000, 1 }, /* 0xe3094000 */
	{ TARGET_DEV10G +  5,         0x3098000, 1 }, /* 0xe3098000 */
	{ TARGET_PCS10G_BR +  5,      0x309c000, 1 }, /* 0xe309c000 */
	{ TARGET_DEV2G5 + 21,         0x30a0000, 1 }, /* 0xe30a0000 */
	{ TARGET_DEV5G +  3,          0x30a4000, 1 }, /* 0xe30a4000 */
	{ TARGET_PCS5G_BR +  3,       0x30a8000, 1 }, /* 0xe30a8000 */
	{ TARGET_DEV2G5 + 22,         0x30ac000, 1 }, /* 0xe30ac000 */
	{ TARGET_DEV2G5 + 23,         0x30b0000, 1 }, /* 0xe30b0000 */
	{ TARGET_DEV2G5 + 24,         0x30b4000, 1 }, /* 0xe30b4000 */
	{ TARGET_DEV10G +  6,         0x30b8000, 1 }, /* 0xe30b8000 */
	{ TARGET_PCS10G_BR +  6,      0x30bc000, 1 }, /* 0xe30bc000 */
	{ TARGET_DEV2G5 + 25,         0x30c0000, 1 }, /* 0xe30c0000 */
	{ TARGET_DEV10G +  7,         0x30c4000, 1 }, /* 0xe30c4000 */
	{ TARGET_PCS10G_BR +  7,      0x30c8000, 1 }, /* 0xe30c8000 */
	{ TARGET_DEV2G5 + 26,         0x30cc000, 1 }, /* 0xe30cc000 */
	{ TARGET_DEV10G +  8,         0x30d0000, 1 }, /* 0xe30d0000 */
	{ TARGET_PCS10G_BR +  8,      0x30d4000, 1 }, /* 0xe30d4000 */
	{ TARGET_DEV2G5 + 27,         0x30d8000, 1 }, /* 0xe30d8000 */
	{ TARGET_DEV10G +  9,         0x30dc000, 1 }, /* 0xe30dc000 */
	{ TARGET_PCS10G_BR +  9,      0x30e0000, 1 }, /* 0xe30e0000 */
	{ TARGET_DSM,                 0x30ec000, 1 }, /* 0xe30ec000 */
	{ TARGET_PORT_CONF,           0x30f0000, 1 }, /* 0xe30f0000 */
	{ TARGET_ASM,                 0x3200000, 1 }, /* 0xe3200000 */
	{ TARGET_HSIO_WRAP,           0x3408000, 1 }, /* 0xe3408000 */
};

static u32 lan969x_taxi_ports[LAN969X_DSM_CAL_TAXIS][LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI] = {
	{  0,  4,  1,  2,  3,  5,  6,  7, 28, 29 },
	{  8, 12,  9, 13, 10, 11, 14, 15, 99, 99 },
	{ 16, 20, 17, 21, 18, 19, 22, 23, 99, 99 },
	{ 24, 25, 99, 99, 99, 99, 99, 99, 99, 99 },
	{ 26, 27, 99, 99, 99, 99, 99, 99, 99, 99 }
};

static const u32 lan969x_ifh[IFH_MAX][2] = {
	[IFH_MISC_CPU_MASK_DPORT]  = {  29,  8 },
	[IFH_MISC_PIPELINE_PT]     = {  37,  5 },
	[IFH_MISC_PIPELINE_ACT]    = {  42,  3 },
	[IFH_FWD_SRC_PORT]         = {  46,  6 },
	[IFH_FWD_SFLOW_ID]         = {  56,  7 },
	[IFH_FWD_UPDATE_FCS]       = {  66,  1 },
	[IFH_VSTAX_REW_CMD]        = { 105, 10 },
	[IFH_VSTAX_INGR_DROP_MODE] = { 128,  1 },
	[IFH_VSTAX_RSV]            = { 152,  1 },
	[IFH_DST_PDU_TYPE]         = { 190,  4 },
	[IFH_DST_PDU_W16_OFFSET]   = { 194,  6 },
	[IFH_TS_TSTAMP]            = { 232, 38 },
};

static struct sparx5_sdlb_group lan969x_sdlb_groups[LAN969X_SDLB_GROUP_COUNT] = {
	{ 1000000000,  8192 / 2, 64 }, /*    1 G */
	{  500000000,  8192 / 2, 64 }, /*  500 M */
	{  100000000,  8192 / 4, 64 }, /*  100 M */
	{   50000000,  8192 / 4, 64 }, /*   50 M */
	{    5000000,  8192 / 8, 64 }, /*   10 M */
};

static const u32 lan969x_hsch_max_group_rate[SPX5_HSCH_LEAK_GRP_CNT] = {
	655355, 1048568, 6553550, 10485680
};

const u32 lan969x_get_hsch_max_group_rate(int grp)
{
	return lan969x_hsch_max_group_rate[grp];
}

u32 *lan969x_get_taxi(int idx)
{
	return lan969x_taxi_ports[idx];
}

inline u32 lan969x_get_ifh_field_pos(enum sparx5_ifh_enum idx)
{
	return lan969x_ifh[idx][0];
}

inline u32 lan969x_get_ifh_field_width(enum sparx5_ifh_enum idx)
{
	return lan969x_ifh[idx][1];
}

u32 lan969x_get_packet_pipeline_pt(enum sparx5_packet_pipeline_pt pt)
{
	return pt;
}

int lan969x_port_mux_set(struct sparx5 *sparx5, struct sparx5_port *port,
			 struct sparx5_port_config *conf)
{
	u32 portno = port->portno;
	u32 inst;

	if (port->conf.portmode == conf->portmode)
		return 0; /* Nothing to do */

	switch (conf->portmode) {
	case PHY_INTERFACE_MODE_QSGMII: /* QSGMII: 4x2G5 devices. Mode Q'  */
		inst = (portno - portno % 4) / 4;
		spx5_rmw(BIT(inst), BIT(inst), sparx5, PORT_CONF_QSGMII_ENA);
		break;
	default:
		break;
	}
	return 0;
}

/* Get the bit position of the device, when configuring mode for 5G/10G devices */
u32 lan969x_get_dev_mode_bit(struct sparx5 *sparx5, int port)
{
	const struct sparx5_ops *ops = &sparx5->data->ops;

	if (ops->port_is_2g5(port) || ops->port_is_5g(port))
		return port;

	/* 10G */
	switch (port) {
	case 0:
		return 12;
	case 4:
		return 13;
	case 8:
		return 14;
	case 12:
		return 0;
	default:
		return port;
	}
}

u32 lan969x_port_dev_mapping(struct sparx5 *sparx5, int port)
{
	const struct sparx5_ops *ops = &sparx5->data->ops;

	if (ops->port_is_5g(port)) {
		switch (port) {
		case 9:
			return 0;
		case 13:
			return 1;
		case 17:
			return 2;
		case 21:
			return 3;
		}
	}

	if (ops->port_is_10g(port)) {
		switch (port) {
		case 0:
			return 0;
		case 4:
			return 1;
		case 8:
			return 2;
		case 12:
			return 3;
		case 16:
			return 4;
		case 20:
			return 5;
		case 24:
			return 6;
		case 25:
			return 7;
		case 26:
			return 8;
		case 27:
			return 9;
		}
	}

	/* 2g5 port */
	return port;
}

struct sparx5_sdlb_group *lan969x_get_sdlb_group(int idx)
{
	return &lan969x_sdlb_groups[idx];
}

const struct sparx5_match_data lan969x_desc = {
	.iomap = lan969x_main_iomap,
	.iomap_size = ARRAY_SIZE(lan969x_main_iomap),
	.ioranges = 2,
	.regs = {
		.gaddr = lan969x_gaddr,
		.gcnt = lan969x_gcnt,
		.gsize = lan969x_gsize,
		.raddr = lan969x_raddr,
		.rcnt = lan969x_rcnt,
		.fpos = lan969x_fpos,
	},
	.ops = {
		.port_mux_set = &lan969x_port_mux_set,
		.port_is_2g5 = &lan969x_port_is_2g5,
		.port_is_5g = &lan969x_port_is_5g,
		.port_is_10g = &lan969x_port_is_10g,
		.port_get_dev_index = &lan969x_port_dev_mapping,
		.get_dev_mode_bit = &lan969x_get_dev_mode_bit,
		.get_sdlb_group = &lan969x_get_sdlb_group,
		.get_ifh_field_pos = &lan969x_get_ifh_field_pos,
		.get_ifh_field_width = &lan969x_get_ifh_field_width,
		.get_pipeline_pt = &lan969x_get_packet_pipeline_pt,
		.get_taxi = &lan969x_get_taxi,
		.get_hsch_max_group_rate = &lan969x_get_hsch_max_group_rate,
		.fdma_stop = lan969x_fdma_stop,
		.fdma_start = lan969x_fdma_start,
		.fdma_xmit = lan969x_fdma_xmit,
	},
	.consts = {
		.chip_ports = 30,
		.chip_ports_all = 35,
		.buffer_memory = 1572864,
		.res_cfg_max_port_idx = 280,
		.res_cfg_max_prio_idx = 315,
		.res_cfg_max_colour_idx = 323,
		.hsch_l1_se_cnt = 32,
		.hsch_queue_cnt = 4,
		.lb_group_cnt = 5,
		.pgid_cnt = (1024 + 30),
		.dsm_cal_max_devs_per_taxi = 10,
		.dsm_cal_taxis = 5,
		.sio_clk_cnt = 1,
		.own_upsid_cnt = 1,
		.auto_cal_cnt = 4,
		.pol_acl_cnt = 32,
		.filter_cnt = 256,
		.gate_cnt = 256,
		.lb_cnt = 496,
		.tod_pin = 7,
		.vcaps = lan969x_vcaps,
		.vcaps_cfg = lan969x_vcap_inst_cfg,
		.vcap_stats = &lan969x_vcap_stats,
	},
};
MODULE_LICENSE("Dual MIT/GPL");
