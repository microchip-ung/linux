/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef __LAN9645X_NETLINK_FRER_H__
#define __LAN9645X_NETLINK_FRER_H__

#include "lan9645x_main.h"
#include "../../../ethernet/microchip/mchp_ui_qos.h"

#define LAN9645X_FRER_NUM_MSI     256 /* Number of Member Stream Instances */
#define LAN9645X_FRER_NUM_CSI     128 /* Number of Compound Stream Instances */
#define LAN9645X_FRER_NUM_FLOW    127 /* Number of Flows (ISDX) */
#define LAN9645X_FRER_FLOW_MIN      1 /* Cannot use ISDX zero */
#define MCHP_FRER_MAX_PORTS     2 /* Max # of ports for split and mstreams */
#define LAN9645X_FRER_HLEN_MIN      2 /* Minimum history length */
#define LAN9645X_FRER_HLEN_MAX     32 /* Maximum history length */
#define LAN9645X_FRER_RESET_MIN     0 /* Minimum reset time */
#define LAN9645X_FRER_RESET_MAX  4095 /* Maximum reset_time */

struct lan9645x_frer_prev_cnt {
	u32 out_of_order_packets; /* frerCpsSeqRcvyOutOfOrderPackets */
	u32 rogue_packets;        /* frerCpsSeqRcvyRoguePackets */
	u32 passed_packets;       /* frerCpsSeqRcvyPassedPackets */
	u32 discarded_packets;    /* frerCpsSeqRcvyDiscardedPackets */
	u32 lost_packets;         /* frerCpsSeqRcvyLostPackets */
	u32 tagless_packets;      /* frerCpsSeqRcvyTaglessPackets */
	u32 resets;               /* frerCpsSeqRcvyResets */
};

struct lan9645x_frer_ms_adm {
	u16 port_mask; /* Zero means unallocated */
};

struct lan9645x_nl_frer {
	struct lan9645x *lan9645x;

	struct mchp_frer_stream_cfg ms_cfg[LAN9645X_FRER_NUM_MSI];
	struct mchp_frer_stream_cfg cs_cfg[LAN9645X_FRER_NUM_CSI];
	struct mchp_frer_cnt ms_cnt[LAN9645X_FRER_NUM_MSI];
	struct lan9645x_frer_prev_cnt ms_prev_cnt[LAN9645X_FRER_NUM_MSI];
	struct mchp_frer_cnt cs_cnt[LAN9645X_FRER_NUM_CSI];
	struct lan9645x_frer_prev_cnt cs_prev_cnt[LAN9645X_FRER_NUM_CSI];
	struct mchp_iflow_cfg iflow_cfg[LAN9645X_FRER_NUM_FLOW];
	struct lan9645x_frer_ms_adm
		ms_adm[LAN9645X_FRER_NUM_MSI / MCHP_FRER_MAX_PORTS];
};

int lan9645x_frer_cs_cfg_set(struct lan9645x_nl_frer *frer, const u16 cs_id,
			     const struct mchp_frer_stream_cfg *const cfg);
int lan9645x_frer_cs_cfg_get(struct lan9645x_nl_frer *frer, const u16 cs_id,
			     struct mchp_frer_stream_cfg *const cfg);
int lan9645x_frer_cs_cnt_clear(struct lan9645x_nl_frer *frer, const u16 cs_id);
int lan9645x_frer_cs_cnt_get(struct lan9645x_nl_frer *frer, const u16 cs_id,
			     struct mchp_frer_cnt *const cnt);

int lan9645x_frer_ms_alloc(struct lan9645x_nl_frer *frer,
			   struct net_device *dev1, struct net_device *dev2,
			   u16 *const ms_id);
int lan9645x_frer_ms_free(struct lan9645x_nl_frer *frer, const u16 ms_id);

int lan9645x_frer_ms_cfg_set(struct lan9645x_nl_frer *frer,
			     struct net_device *dev, const u16 ms_id,
			     struct mchp_frer_stream_cfg *const cfg);

int lan9645x_frer_ms_cfg_get(struct lan9645x_nl_frer *frer,
			     struct net_device *dev, const u16 ms_id,
			     struct mchp_frer_stream_cfg *const cfg);

int lan9645x_frer_ms_cnt_get(struct lan9645x_nl_frer *frer,
			     struct net_device *dev, const u16 ms_id,
			     struct mchp_frer_cnt *const cnt);
int lan9645x_frer_ms_cnt_clear(struct lan9645x_nl_frer *frer,
			       struct net_device *dev, const u16 ms_id);

int lan9645x_iflow_cfg_set(struct lan9645x_nl_frer *frer,
			   struct net_device *dev1,
			   struct net_device *dev2,
			   const u16 isdx,
			   struct mchp_iflow_cfg *cfg);
int lan9645x_iflow_cfg_get(struct lan9645x_nl_frer *frer,
			   const u16 isdx,
			   struct mchp_iflow_cfg *const cfg);
int lan9645x_frer_vlan_cfg_set(struct lan9645x_nl_frer *frer, const u16 vid,
			       const struct mchp_frer_vlan_cfg *const cfg);
int lan9645x_frer_vlan_cfg_get(struct lan9645x_nl_frer *frer, const u16 vid,
			       struct mchp_frer_vlan_cfg *const cfg);

int lan9645x_frer_init(struct lan9645x_nl_frer *frer);
int lan9645x_netlink_frer_init(struct lan9645x *lan9645x);
void lan9645x_netlink_frer_uninit(void);

#endif /* __LAN9645X_NETLINK_FRER_H__ */
