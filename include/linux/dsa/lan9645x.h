// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef _NET_DSA_TAG_LAN9645X_H_
#define _NET_DSA_TAG_LAN9645X_H_

#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <net/dsa.h>

#define LAN9645X_IFH_TAG_TYPE_C 0
#define LAN9645X_IFH_TAG_TYPE_S 1
#define LAN9645X_IFH_LEN_U32 7
#define LAN9645X_IFH_LEN (LAN9645X_IFH_LEN_U32 * sizeof(u32))
#define LAN9645X_SHORT_PREFIX_LEN 4
#define LAN9645X_LONG_PREFIX_LEN 16
#define LAN9645X_TOTAL_TAG_LEN (LAN9645X_LONG_PREFIX_LEN + LAN9645X_IFH_LEN)

#define IFH_INJ_TIMESTAMP                   192
#define IFH_BYPASS                          191
#define IFH_MASQ                            190
#define IFH_TIMESTAMP                       186
#define IFH_MASQ_PORT                       186
#define IFH_RCT_INJ                         185
#define IFH_LEN                             171
#define IFH_WRDMODE                         169
#define IFH_RTAGD                           167
#define IFH_CUTTHRU                         166
#define IFH_REW_CMD                         156
#define IFH_REW_OAM                         155
#define IFH_PDU_TYPE                        151
#define IFH_FCS_UPD                         150
#define IFH_DP                              149
#define IFH_RTE_INB_UPDATE                  148
#define IFH_POP_CNT                         146
#define IFH_ETYPE_OFS                       144
#define IFH_SRCPORT                         140
#define IFH_SEQ_NUM                         120
#define IFH_TAG_TYPE                        119
#define IFH_TCI                             103
#define IFH_DSCP                            97
#define IFH_QOS_CLASS                       94
#define IFH_CPUQ                            86
#define IFH_LEARN_FLAGS                     84
#define IFH_SFLOW_ID                        80
#define IFH_ACL_HIT                         79
#define IFH_ACL_IDX                         73
#define IFH_ISDX                            65
#define IFH_DSTS                            55
#define IFH_FLOOD                           53
#define IFH_SEQ_OP                          51
#define IFH_IPV                             48
#define IFH_AFI                             47
#define IFH_RTP_ID                          37
#define IFH_RTP_SUBID                       36
#define IFH_PN_DATA_STATUS                  28
#define IFH_PN_TRANSF_STATUS_ZERO           27
#define IFH_PN_CC                           11
#define IFH_DUPL_DISC_ENA                   10
#define IFH_RCT_AVAIL                       9

#define IFH_INJ_TIMESTAMP_SZ                32
#define IFH_BYPASS_SZ                       1
#define IFH_MASQ_SZ                         1
#define IFH_TIMESTAMP_SZ                    38
#define IFH_MASQ_PORT_SZ                    4
#define IFH_RCT_INJ_SZ                      1
#define IFH_LEN_SZ                          14
#define IFH_WRDMODE_SZ                      2
#define IFH_RTAGD_SZ                        2
#define IFH_CUTTHRU_SZ                      1
#define IFH_REW_CMD_SZ                      10
#define IFH_REW_OAM_SZ                      1
#define IFH_PDU_TYPE_SZ                     4
#define IFH_FCS_UPD_SZ                      1
#define IFH_DP_SZ                           1
#define IFH_RTE_INB_UPDATE_SZ               1
#define IFH_POP_CNT_SZ                      2
#define IFH_ETYPE_OFS_SZ                    2
#define IFH_SRCPORT_SZ                      4
#define IFH_SEQ_NUM_SZ                      16
#define IFH_TAG_TYPE_SZ                     1
#define IFH_TCI_SZ                          16
#define IFH_DSCP_SZ                         6
#define IFH_QOS_CLASS_SZ                    3
#define IFH_CPUQ_SZ                         8
#define IFH_LEARN_FLAGS_SZ                  2
#define IFH_SFLOW_ID_SZ                     4
#define IFH_ACL_HIT_SZ                      1
#define IFH_ACL_IDX_SZ                      6
#define IFH_ISDX_SZ                         8
#define IFH_DSTS_SZ                         10
#define IFH_FLOOD_SZ                        2
#define IFH_SEQ_OP_SZ                       2
#define IFH_IPV_SZ                          3
#define IFH_AFI_SZ                          1
#define IFH_RTP_ID_SZ                       10
#define IFH_RTP_SUBID_SZ                    1
#define IFH_PN_DATA_STATUS_SZ               8
#define IFH_PN_TRANSF_STATUS_ZERO_SZ        1
#define IFH_PN_CC_SZ                        16
#define IFH_DUPL_DISC_ENA_SZ                1
#define IFH_RCT_AVAIL_SZ                    1

#define BTM_MSK { 0x1, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff }
#define TOP_MSK { 0xff, 0xfe, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x80 }

#define LAN9645X_IFH_GET(_ifh, _fld) \
	lan9645x_ifh_get((_ifh), LAN9645X_IFH_LEN, (_fld), _fld##_SZ)
#define LAN9645X_IFH_SET(_ifh, _fld, _val) \
	lan9645x_ifh_set((_ifh), LAN9645X_IFH_LEN, (_val), (_fld), _fld##_SZ)

/* Get mask of ports which mirror traffic egressing dp */
u32 lan9645x_emirror_get_dst(struct dsa_port *dp);

static inline u8 merge_mask(u8 on_zero, u8 on_one, u8 mask)
{
	return on_zero ^ ((on_zero ^ on_one) & mask);
}

static inline u64 lan9645x_ifh_get(u8 *ifh, size_t ifh_sz, size_t pos,
				   size_t length)
{
	size_t end = (pos + length) - 1;
	size_t start_u8 = pos >> 3;
	size_t end_u8 = end >> 3;
	size_t idx = ifh_sz - 1 - end_u8;
	u64 end_rem = end & 0x7;
	u64 pos_rem = pos & 0x7;
	u8 btm_msk[8] = BTM_MSK;
	u8 top_msk[8] = TOP_MSK;
	u64 val = 0;
	size_t j = 1;

	// handle endbyte
	val = ifh[idx] & btm_msk[end_rem];

	if (end_u8 == start_u8)
		return (val & top_msk[pos_rem]) >> (pos_rem);

	for (; j < end_u8 - start_u8; j++)
		val = val << 8 | ifh[idx + j];

	return val << (8 - pos_rem) |
	       (ifh[idx + j] & top_msk[pos_rem]) >> pos_rem;
}

static inline void lan9645x_ifh_set(u8 *ifh, size_t ifh_sz, u64 val, size_t pos,
				    size_t length)
{
	size_t end = (pos + length) - 1;
	size_t start_u8 = pos >> 3;
	size_t end_u8 = end >> 3;
	size_t idx = ifh_sz - 1 - end_u8;
	u64 end_rem = end & 0x7;
	u64 pos_rem = pos & 0x7;
	u8 btm_msk[8] = BTM_MSK;
	u8 top_msk[8] = TOP_MSK;
	size_t val_shift;
	size_t j = 1;
	u8 *v;

	v = &ifh[idx];

	if (end_u8 == start_u8) {
		*v = merge_mask(*v, val << pos_rem,
				btm_msk[end_rem] & top_msk[pos_rem]);
		return;
	}

	val_shift = length - end_rem - 1;
	*v = merge_mask(*v, val >> val_shift, btm_msk[end_rem]);

	for (; j < end_u8 - start_u8; j++) {
		val_shift -= 8;
		ifh[idx + j] = val >> val_shift;
	}

	v = &ifh[idx + j];
	*v = merge_mask(*v, val << pos_rem, top_msk[pos_rem]);
}

static inline void lan9645x_xmit_get_vlan_info(struct sk_buff *skb,
					       struct net_device *br,
					       u64 *vlan_tci, u64 *tag_type)
{
	struct vlan_ethhdr *hdr;
	u16 proto, tci;

	if (!br || !br_vlan_enabled(br)) {
		*vlan_tci = 0;
		*tag_type = LAN9645X_IFH_TAG_TYPE_C;
		return;
	}

	hdr = (struct vlan_ethhdr *)skb_mac_header(skb);
	br_vlan_get_proto(br, &proto);

	if (ntohs(hdr->h_vlan_proto) == proto) {
		vlan_remove_tag(skb, &tci);
		*vlan_tci = tci;
	} else {
		rcu_read_lock();
		br_vlan_get_pvid_rcu(br, &tci);
		rcu_read_unlock();
		*vlan_tci = tci;
	}

	*tag_type = (proto != ETH_P_8021Q) ? LAN9645X_IFH_TAG_TYPE_S :
					     LAN9645X_IFH_TAG_TYPE_C;
}

#endif /* _NET_DSA_TAG_LAN9645X_H_ */
