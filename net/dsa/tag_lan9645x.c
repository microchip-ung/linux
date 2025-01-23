// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/dsa/lan9645x.h>
#include <linux/if_ether.h>
#include <linux/if_hsr.h>
#include <linux/if_vlan.h>
#include <linux/igmp.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/addrconf.h>
#include <net/dsa.h>

#include "tag.h"

#define LAN9645X_NAME "lan9645x"

/* The switch allows any DMAC/SMAC pairs in the long prefix for injection. For
 * extraction the chip uses SMAC 0xfeffffffffff. For injection we pick
 * SMAC=0xfcffffffffff to make it easier to distinguish the two cases.
 */
static u8 LONG_PREFIX[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			    0xfc, 0xff, 0xff, 0xff, 0xff, 0xff,
			    0x88, 0x80, 0x00, 0x11 };

static void lan9645x_rcv_dbg(struct sk_buff *skb, struct net_device *ndev,
			   u8 *ifh, u32 ifh_gap_len)
{
	u64 seqnum, cpuq, flood, dd, rct, rtagd, popcnt, etype_ofs, acl_id,
		acl_hit;
	u64 src_port, qos_class, vlan_tci, tag_type, flen, dsts, ts;

	src_port = LAN9645X_IFH_GET(ifh, IFH_SRCPORT);
	qos_class = LAN9645X_IFH_GET(ifh, IFH_QOS_CLASS);
	tag_type = LAN9645X_IFH_GET(ifh, IFH_TAG_TYPE);
	vlan_tci = LAN9645X_IFH_GET(ifh, IFH_TCI);
	ts = LAN9645X_IFH_GET(ifh, IFH_TIMESTAMP);
	flen = LAN9645X_IFH_GET(ifh, IFH_LEN);
	dsts = LAN9645X_IFH_GET(ifh, IFH_DSTS);
	seqnum = LAN9645X_IFH_GET(ifh, IFH_SEQ_NUM);
	cpuq = LAN9645X_IFH_GET(ifh, IFH_CPUQ);
	flood = LAN9645X_IFH_GET(ifh, IFH_FLOOD);
	dd = LAN9645X_IFH_GET(ifh, IFH_DUPL_DISC_ENA);
	rct = LAN9645X_IFH_GET(ifh, IFH_RCT_AVAIL);
	rtagd = LAN9645X_IFH_GET(ifh, IFH_RTAGD);
	popcnt = LAN9645X_IFH_GET(ifh, IFH_POP_CNT);
	etype_ofs = LAN9645X_IFH_GET(ifh, IFH_ETYPE_OFS);
	acl_id = LAN9645X_IFH_GET(ifh, IFH_ACL_IDX);
	acl_hit = LAN9645X_IFH_GET(ifh, IFH_ACL_HIT);

	dev_dbg(&ndev->dev,
		"lan9645x_rcv ifh: %*phN tag_type=%llu src_port=%llu tci=%llu qos=%llu ts=%llu flen=%llu dsts=%llu seqnum=%llu cpuq=%llu flood=%llu dd=%llu rct=%llu rtagd=%llu popcnt=%llu etype_ofs=%llu ifh_gap=%u skb_offload=%u acl_id=%llu acl_hit=%llu\n",
		(unsigned)LAN9645X_IFH_LEN, ifh, tag_type, src_port, vlan_tci, qos_class,
		ts, flen, dsts, seqnum, cpuq, flood, dd, rct, rtagd, popcnt,
		etype_ofs, ifh_gap_len, skb->offload_fwd_mark, acl_id, acl_hit);
}

static bool lan9645x_is_mld(struct sk_buff *skb)
{
	return IS_ENABLED(CONFIG_IPV6) && skb->protocol == htons(ETH_P_IPV6) &&
	       ipv6_addr_is_multicast(&ipv6_hdr(skb)->daddr) &&
	       ipv6_mc_check_mld(skb);
}

static void lan9645x_offload_fwd_mark(struct sk_buff *skb)
{
	/* IGMP/MLD are trapped to CPU, and must be forwarded by network stack.
	 */
	if (!ip_mc_check_igmp(skb) || lan9645x_is_mld(skb)) {
		skb->offload_fwd_mark = 0;
		return;
	}

	/* Is port bridged? */
	return dsa_default_offload_fwd_mark(skb);
}

static struct sk_buff *lan9645x_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct dsa_port *dp = dsa_user_to_port(ndev);
	struct dsa_switch *ds = dp->ds;
	u32 cpu_port = ds->num_ports;
	u64 vlan_tci, tag_type;
	void *long_prefix;
	u64 qos_class;
	void *ifh;

	BUILD_BUG_ON(ARRAY_SIZE(LONG_PREFIX) != LAN9645X_LONG_PREFIX_LEN);

	lan9645x_xmit_get_vlan_info(skb, dsa_port_bridge_dev_get(dp), &vlan_tci,
				  &tag_type);

	qos_class = netdev_get_num_tc(ndev) ?
			    netdev_get_prio_tc_map(ndev, skb->priority) :
			    skb->priority;

	/* Make room for IFH */
	ifh = skb_push(skb, LAN9645X_IFH_LEN);
	memset(ifh, 0, LAN9645X_IFH_LEN);

	/* Add long prefix to start of frame */
	long_prefix = skb_push(skb, ARRAY_SIZE(LONG_PREFIX));
	memcpy(long_prefix, LONG_PREFIX, ARRAY_SIZE(LONG_PREFIX));

	LAN9645X_IFH_SET(ifh, IFH_BYPASS, 1);
	LAN9645X_IFH_SET(ifh, IFH_SRCPORT, cpu_port);
	LAN9645X_IFH_SET(ifh, IFH_QOS_CLASS, qos_class);
	LAN9645X_IFH_SET(ifh, IFH_TCI, vlan_tci);
	LAN9645X_IFH_SET(ifh, IFH_TAG_TYPE, tag_type);
	LAN9645X_IFH_SET(ifh, IFH_DSTS, BIT_ULL(dp->index));
	netdev_dbg(ndev, "xmit: dst=%u\n", dp->index);

	return skb;
}

static struct sk_buff *lan9645x_rcv(struct sk_buff *skb, struct net_device *ndev)
{
	u8 *orig_skb_data = skb->data;
	u64 src_port, qos_class;
	u64 vlan_tci, tag_type;
	u64 popcnt, etype_ofs;
	struct dsa_port *dp;
	u32 ifh_gap_len = 0;
	u16 vlan_tpid;
	u8 *ifh;

	/* DSA master already consumed DMAC,SMAC,ETYPE from long prefix. Go back
	 * to beginning of frame.
	 */
	skb_push(skb, ETH_HLEN);
	/* IFH starts after our long prefix */
	ifh = skb_pull(skb, LAN9645X_LONG_PREFIX_LEN);

	src_port = LAN9645X_IFH_GET(ifh, IFH_SRCPORT);
	qos_class = LAN9645X_IFH_GET(ifh, IFH_QOS_CLASS);
	tag_type = LAN9645X_IFH_GET(ifh, IFH_TAG_TYPE);
	vlan_tci = LAN9645X_IFH_GET(ifh, IFH_TCI);
	popcnt = LAN9645X_IFH_GET(ifh, IFH_POP_CNT);
	etype_ofs = LAN9645X_IFH_GET(ifh, IFH_ETYPE_OFS);

	/* Set skb->data at start of real header
	 *
	 * Since NO_REWRITE=0 is required on the NPI port for HSR to work, we
	 * need to account for any tags popped by the hardware, as that will
	 * leave a gap between the IFH and DMAC.
	 */
	if (popcnt == 0 && etype_ofs == 0) {
		ifh_gap_len = 2 * VLAN_HLEN;
	} else if (popcnt == 3) {
		ifh_gap_len = VLAN_HLEN;
	}
	skb_pull(skb, LAN9645X_IFH_LEN + ifh_gap_len);
	skb_reset_mac_header(skb);
	skb_reset_mac_len(skb);

	/* Reset skb->data past the actual ethernet header. */
	skb_pull(skb, ETH_HLEN);
	skb_postpull_rcsum(skb, orig_skb_data,
			   LAN9645X_TOTAL_TAG_LEN + ifh_gap_len);

	lan9645x_rcv_dbg(skb, ndev, ifh, ifh_gap_len);

	skb->dev = dsa_conduit_find_user(ndev, 0, src_port);
	if (WARN_ON_ONCE(!skb->dev)) {
		/* This should never happen since src_port is always set, and
		 * src_port=CPU_PORT is not possible since we have disabled
		 * reflection back to CPU_PORT.
		 */
		return NULL;
	}

	lan9645x_offload_fwd_mark(skb);

	skb->priority = qos_class;

	/* Pushing tags is disabled in the rewriter must be disabled on
	 * NPI/CPU_PORT with NO_REWRITE=1. Any rewrite action is communicated via
	 * the IFH, and must be performed by software. For VLAN-aware ports we
	 * add the VLAN tag from the IFH to the skb.
	 *
	 * NOTE: In VLAN-unaware mode, we don't want to do that, we want the
	 * frame to remain unmodified, because the classified VLAN is always
	 * equal to the pvid of the ingress port and should not be used for
	 * processing.
	 */
	dp = dsa_user_to_port(skb->dev);
	vlan_tpid = tag_type ? ETH_P_8021AD : ETH_P_8021Q;

	if (dsa_port_is_vlan_filtering(dp) &&
	    eth_hdr(skb)->h_proto == htons(vlan_tpid)) {
		u16 dummy_vlan_tci;

		skb_push_rcsum(skb, ETH_HLEN);
		__skb_vlan_pop(skb, &dummy_vlan_tci);
		skb_pull_rcsum(skb, ETH_HLEN);
		__vlan_hwaccel_put_tag(skb, htons(vlan_tpid), vlan_tci);
	}

	return skb;
}

static const struct dsa_device_ops lan9645x_netdev_ops = {
	.name = LAN9645X_NAME,
	.proto = DSA_TAG_PROTO_LAN9645X,
	.xmit = lan9645x_xmit,
	.rcv = lan9645x_rcv,
	.needed_headroom = LAN9645X_TOTAL_TAG_LEN,
	.promisc_on_conduit = false,

};

MODULE_LICENSE("GPL v2");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_LAN9645X, LAN9645X_NAME);

module_dsa_tag_driver(lan9645x_netdev_ops);
