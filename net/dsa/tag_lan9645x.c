// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/dsa/lan9645x.h>
#include <linux/if_ether.h>
#include <linux/if_hsr.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/dsa.h>

#include "tag.h"

#define LAN9645X_NAME "lan9645x"

/* The HSR header is 6 bytes, but lan9645x must align it to a 32bit boundary. This
 * means the HSR eth type is stripped from the frame in the chip. This is
 * automatically rectified on front-port egress, but not during CPU extraction.
 * Lan9645x will communicate the state via the IFH in the RTAGD=0,1,2 field.
 *
 * We must pop the 4 byte HSR fragment before passing the frame on. If a frame
 * is VLAN tagged, the HSR tag is always inner.
 */
#define HSR_FRG_HLEN 4

enum {
	HSR_MISSING = 0,
	HSR_OUTER = 1,
	HSR_INNER = 2,
};

static int lan9645x_pop_hsr_tag(struct sk_buff *skb, int rtagd)
{
	int outer_len;
	int offset;
	int err;

	if (rtagd == HSR_MISSING)
		return 0;

	offset = skb->data - skb_mac_header(skb);

	if (WARN_ONCE(offset,
		      "%s got skb with skb->data not at mac header (offset %d)\n",
		      __func__, offset)) {
		return -EINVAL;
	}

	outer_len = rtagd == HSR_INNER ? VLAN_HLEN : 0;

	err = skb_ensure_writable(skb, ETH_HLEN + outer_len + HSR_FRG_HLEN);
	if (unlikely(err))
		return err;

	skb_postpull_rcsum(skb, skb->data + (2 * ETH_ALEN + outer_len),
			   HSR_FRG_HLEN);

	memmove(skb->data + HSR_FRG_HLEN, skb->data, 2 * ETH_ALEN + outer_len);
	skb_pull(skb, HSR_FRG_HLEN);

	skb_reset_mac_header(skb);

	if (skb_network_offset(skb) < ETH_HLEN)
		skb_set_network_header(skb, ETH_HLEN);

	skb_reset_mac_len(skb);

	return err;
}

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

static void lan9645x_offload_fwd_mark(struct sk_buff *skb, u32 rtagd,
				      u32 acl_id, u32 acl_hit, u32 cpuq)
{
	u32 cpu_redir;

	if (acl_hit && (acl_id == 1 || (acl_id >> 3) == 1)) {
		/* frame trapped by IS2 VCAP. Let network stack handle it. */
		skb->offload_fwd_mark = 0;
		return;
	}

	/* IGMP/MLD are trapped to CPU, and must be forwarded by the stack */
	cpu_redir = BIT(LAN9645X_CPUQ_IGMP) | BIT(LAN9645X_CPUQ_MLD);

	if (cpuq & cpu_redir) {
		skb->offload_fwd_mark = 0;
		return;
	}

	if (rtagd > 0) {
		/* Not strictly necessary due to the fwd offload flag in the HSR
		 * driver.
		 */
		skb->offload_fwd_mark = !!(rtagd > 0);
		return;
	}

	/* Is port bridged? */
	return dsa_default_offload_fwd_mark(skb);
}

static void lan9645x_xmit_ptp(struct sk_buff *skb, void *ifh)
{
	struct sk_buff *clone = LAN9645X_SKB_CB(skb)->clone;

	if (LAN9645X_SKB_CB(skb)->rew_op != IFH_REW_OP_NOOP) {
		LAN9645X_IFH_SET(ifh, IFH_REW_CMD, LAN9645X_SKB_CB(skb)->rew_op);
		LAN9645X_IFH_SET(ifh, IFH_PDU_TYPE, LAN9645X_SKB_CB(skb)->pdu_type);
		if (clone)
			LAN9645X_IFH_SET(ifh, IFH_INJ_TIMESTAMP,
					 LAN9645X_SKB_CB(clone)->ts_id << 2);
	}
}

static bool lan9645x_is_prp_dev(struct net_device *hsr)
{
	enum hsr_version v;

	if (!hsr)
		return false;

	hsr_get_version(hsr, &v);

	return v == PRP_V1;
}

static struct sk_buff *lan9645x_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct dsa_port *dp = dsa_user_to_port(ndev);
	struct dsa_switch *ds = dp->ds;
	u32 cpu_port = ds->num_ports;
	u64 vlan_tci, tag_type;
	u64 qos_class;
	int src_port;
	void *ifh;

	lan9645x_xmit_get_vlan_info(skb, dsa_port_bridge_dev_get(dp), &vlan_tci,
				    &tag_type);

	if (__skb_put_padto(skb, ETH_ZLEN, false))
		return NULL;

	qos_class = netdev_get_num_tc(ndev) ?
		netdev_get_prio_tc_map(ndev, skb->priority) :
		skb->priority;

	/* Make room for IFH */
	ifh = skb_push(skb, LAN9645X_IFH_LEN);
	memset(ifh, 0, LAN9645X_IFH_LEN);

	if (dp->hsr_dev) {
		/* At the moment the HSR driver does not implement special
		 * handling of PRP San nodes. The standard suggests that DANP
		 * nodes which maintain a nodetable should:
		 * Xmit frames to SAN nodes only on the LAN(s) where it is
		 * detected, without RCT.
		 *
		 * If a SAN moves between A and B, frames must be xmitted on both
		 * for the duration of NodeForgetTime, after which frames are
		 * once again only xmitted on new LAN.
		 *
		 * At the moment RCT is always added, and frame is sent out on
		 * both A and B, which causes a lot of known UC flood noise on
		 * the LAN where the node does not exist.
		 *
		 * IF this is implemented, we will likely have a bug here. We set
		 * the NETIF_F_HW_HSR_DUP flag, and lan9645x can offload
		 * duplication, but only for PRP frames with RCT, not SAN nodes,
		 * since lan9645x can not offload the node table.
		 *
		 * If an skb is meant for a SAN, and hsr driver did not prepare
		 * the RCT, then we want the BYPASS=1 logic below.
		 *
		 * At the moment we will get two kinds of bugs:
		 *
		 * 1) This branch is taken, and we set BYPASS=0 and RCT_INJ=1.
		 * This tells lan9645x that the tail of the frame is a prepared
		 * RCT, so it will insert seq number and lanid, leading to a
		 * potentially corrupted frame, unless padding bytes were
		 * modified.
		 *
		 * 2) The HSR driver will only xmit the frame on one interface,
		 * since we set the DUP flag. But for these SAN frames the HW
		 * does not offload duplication.
		 *
		 * Handling this in the tag driver is a bit delicate, since we
		 * only get the SKB, and does not have access to the decisions
		 * made in the HSR driver.
		 */
		LAN9645X_IFH_SET(ifh, IFH_BYPASS, 0);
		/* When bypass=0 the IFH is just thrown away by the port.
		 * However, a few fields get speciel treatment by the port, and
		 * are passed to the IFH created in the analyzer.
		 */
		src_port = lan9645x_ptp_hsr_xmit_masq_port(skb, dp);
		LAN9645X_IFH_SET(ifh, IFH_MASQ, 1);
		LAN9645X_IFH_SET(ifh, IFH_MASQ_PORT, src_port);
		LAN9645X_IFH_SET(ifh, IFH_SRCPORT, src_port);
		LAN9645X_IFH_SET(ifh, IFH_RCT_INJ, lan9645x_is_prp_dev(dp->hsr_dev));
	} else {
		LAN9645X_IFH_SET(ifh, IFH_BYPASS, 1);
		LAN9645X_IFH_SET(ifh, IFH_SRCPORT, cpu_port);
		LAN9645X_IFH_SET(ifh, IFH_QOS_CLASS, qos_class);
		LAN9645X_IFH_SET(ifh, IFH_TCI, vlan_tci);
		LAN9645X_IFH_SET(ifh, IFH_TAG_TYPE, tag_type);
		/* Mirroring is calculated by the forwarding engine, which
		 * we are bypassing. To implement egress mirroring of standalone
		 * ports, we need to query mirroring state from switch driver.
		 */
		LAN9645X_IFH_SET(ifh, IFH_DSTS,
				 BIT_ULL(dp->index) |
				 (u64)lan9645x_emirror_get_dst(dp));
	}

	lan9645x_xmit_ptp(skb, ifh);

	return skb;
}

static struct sk_buff *lan9645x_rcv(struct sk_buff *skb, struct net_device *ndev)
{
	u64 vlan_tci, tag_type, popcnt, etype_ofs, acl_id, acl_hit, cpuq;
	u64 src_port, qos_class, rtagd, rct, rx_ts;
	u8 *orig_skb_data = skb->data;
	struct dsa_port *dp;
	u32 ifh_gap_len = 0;
	u16 vlan_tpid;
	u8 *ifh;
	int err;

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
	rct = LAN9645X_IFH_GET(ifh, IFH_RCT_AVAIL);
	rtagd = LAN9645X_IFH_GET(ifh, IFH_RTAGD);
	acl_id = LAN9645X_IFH_GET(ifh, IFH_ACL_IDX);
	acl_hit = LAN9645X_IFH_GET(ifh, IFH_ACL_HIT);
	rx_ts = LAN9645X_IFH_GET(ifh, IFH_TIMESTAMP);
	cpuq = LAN9645X_IFH_GET(ifh, IFH_CPUQ);

	/* Set skb->data at start of real header
	 *
	 * Since NO_REWRITE=0 is required on the NPI port for HSR to work, we
	 * need to account for any tags popped by the hardware, as that will
	 * leave a gap between the IFH and DMAC.
	 */
	if (popcnt == 0 && etype_ofs == 0)
		ifh_gap_len = 2 * VLAN_HLEN;
	else if (popcnt == 3)
		ifh_gap_len = VLAN_HLEN;

	skb_pull(skb, LAN9645X_IFH_LEN + ifh_gap_len);
	skb_reset_mac_header(skb);
	/* Reset network header, so our IGMP/MLD checks function properly. */
	skb_set_network_header(skb, ETH_HLEN);
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

	if (rtagd > 0) {
		lan964x5_set_redundancy_info(skb, rtagd,
					     dsa_user_to_port(skb->dev));
		skb_push_rcsum(skb, ETH_HLEN);
		err = lan9645x_pop_hsr_tag(skb, rtagd);
		if (err) {
			dev_err(&ndev->dev, "%s: HSR tag pop error=%d",
				__func__, err);
			return NULL;
		}
		skb_pull_rcsum(skb, ETH_HLEN);
	} else if (rct) {
		err = pskb_trim_rcsum(skb, skb->len - HSR_HLEN);
		if (err) {
			dev_err(&ndev->dev, "%s: PRP RCT trailer trim err=%d",
				__func__, err);
			return NULL;
		}
	}

	lan9645x_offload_fwd_mark(skb, rtagd, acl_id, acl_hit, cpuq);

	skb->priority = qos_class;
	LAN9645X_SKB_CB(skb)->rx_ts_ns = rx_ts >> 8;
	LAN9645X_SKB_CB(skb)->rx_ts_subns = rx_ts & GENMASK(7, 0);

	/* While we have REW_PORT_NO_REWRITE=0 on the NPI port, we still disable
	 * port VLAN tagging with REW_TAG_CFG. Any classified VID, different
	 * from a VID in the frame, will not be written to the frame, but is
	 * only communicated via the IFH. So for VLAN-aware ports we add the IFH
	 * vlan to the skb.
	 */
	dp = dsa_user_to_port(skb->dev);
	vlan_tpid = tag_type ? ETH_P_8021AD : ETH_P_8021Q;

	if (dsa_port_is_vlan_filtering(dp) && vlan_tci)
		__vlan_hwaccel_put_tag(skb, htons(vlan_tpid), vlan_tci);

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
MODULE_AUTHOR("Jens Emil Schulz Østergaard <jensemil.schulzostergaard@microchip.com>");
module_dsa_tag_driver(lan9645x_netdev_ops);
