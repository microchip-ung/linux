/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip Sparx5 Switch driver debug filesystem support
 *
 * Copyright (c) 2023 Microchip Technology Inc. and its subsidiaries.
 */

#ifndef __LAN969X_H__
#define __LAN969X_H__

#include "../sparx5_main.h"

/* lan969x_vcap_impl.c */
extern const struct sparx5_vcap_inst lan969x_vcap_inst_cfg[];

/* lan969x_vcap_ag_api.c */
extern const struct vcap_info lan969x_vcaps[];
extern const struct vcap_statistics lan969x_vcap_stats;

/* lan969x.c */
extern const struct sparx5_match_data lan969x_desc;

extern const unsigned int lan969x_tsize[TSIZE_LAST];
extern const unsigned int lan969x_raddr[RADDR_LAST];
extern const unsigned int lan969x_rcnt[RCNT_LAST];
extern const unsigned int lan969x_gaddr[GADDR_LAST];
extern const unsigned int lan969x_gcnt[GCNT_LAST];
extern const unsigned int lan969x_gsize[GSIZE_LAST];
extern const unsigned int lan969x_fpos[FPOS_LAST];
extern const unsigned int lan969x_fsize[FSIZE_LAST];

static inline bool lan969x_port_is_2g5(int portno)
{
	return portno == 1  || portno == 2  || portno == 3  ||
	       portno == 5  || portno == 6  || portno == 7  ||
	       portno == 10 || portno == 11 || portno == 14 ||
	       portno == 15 || portno == 18 || portno == 19 ||
	       portno == 22 || portno == 23;
}

static inline bool lan969x_port_is_5g(int portno)
{
	return portno == 9 || portno == 13 || portno == 17 ||
	       portno == 21;
}

static inline bool lan969x_port_is_10g(int portno)
{
	return portno == 0  || portno == 4  || portno == 8  ||
	       portno == 12 || portno == 16 || portno == 20 ||
	       portno == 24 || portno == 25 || portno == 26 ||
	       portno == 27;
}

static inline bool lan969x_port_is_rgmii(int portno)
{
	return portno == 28 || portno == 29;
}

int lan969x_fdma_stop(struct sparx5 *sparx5);
int lan969x_fdma_start(struct sparx5 *sparx5);
int lan969x_fdma_xmit(struct sparx5 *sparx5, u32 *ifh, struct sk_buff *skb);

u32 lan969x_get_ifh_field_pos(enum sparx5_ifh_enum idx);
u32 lan969x_get_ifh_field_width(enum sparx5_ifh_enum idx);
u32 lan969x_get_packet_pipeline_pt(enum sparx5_packet_pipeline_pt pt);

enum sparx5_cal_bw lan969x_get_internal_port_cal_speed(struct sparx5 *sparx5,
						       u32 portno);

int lan969x_dsm_calendar_calc(struct sparx5 *sparx5, u32 taxi,
			      struct sparx5_calendar_data *data, u32 *cal_len);

#endif
