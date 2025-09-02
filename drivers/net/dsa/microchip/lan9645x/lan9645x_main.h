/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef __LAN9645X_MAIN_H__
#define __LAN9645X_MAIN_H__

#include <linux/dsa/lan9645x.h>
#include <linux/if_hsr.h>
#include <linux/regmap.h>
#include <linux/ptp_clock_kernel.h>
#include <net/dsa.h>

#include <vcap_api.h>
#include <vcap_api_client.h>

#include "lan9645x_regs.h"

#define lan9645x_for_each_chipport(_lan9645x, _i) \
	for ((_i) = 0; (_i) < (_lan9645x)->num_phys_ports; (_i)++)

#define lan9645x_for_each_port(_lan9645x, _i, _p)    \
	for ((_i) = 0, (_p) = (_lan9645x)->ports[0]; \
	     (_i) < (_lan9645x)->num_phys_ports;      \
	     (_p) = (_lan9645x)->ports[++(_i)])

/* Ports index 0-8 are front ports
 * Ports 9-10 are CPU ports
 */
#define NUM_PHYS_PORTS		9
#define CPU_PORT		9
#define NUM_PRIO_QUEUES		8
#define LAN9645X_NUM_TC		8

/* 0-87 : Queue scheduler elements
 * 8 queues per egress port
 * 9 phys ports
 * 2 cpu ports
 * 11*8 = 88
 */
#define LAN9645X_QSCHD_IDX(port, queue) ((port) * NUM_PRIO_QUEUES + (queue))
/* 88-98 : Port schedular elements */
#define LAN9645X_PSCHD_IDX(port) (88 + (port))

/* Reserved amount for (SRC, PRIO) at index 8*SRC + PRIO
 * See QSYS:RES_CTRL[*]:RES_CFG description
 */
#define QSYS_Q_RSRV			95

#define LAN9645X_ISDX_MAX 128
#define LAN9645X_ESDX_MAX 128
#define LAN9645X_SFID_MAX 256

#define HSR_NETID 0x0
#define PRP_NETID 0x5
#define PRP_LANID_A 0x0
#define PRP_LANID_B 0x1
/* Reserved VLAN IDs.
 *
 * We use these to enable isolated vlan unaware standalone ports, and vlan
 * unware bridged ports.
 *
 * Standalone: RX frames, with DMAC == iface mac, should be tapped to the CPU,
 * but no egress on any front ports.
 * This is achieved with PGID SRC_PORT set to 0x0, and using HOST_PVID as pvid
 * for unaware standalone ports.
 *
 * Trapping is ensured with MAC table entries (iface mac, HOST_PVID) which point
 * to the PGID_CPU.
 *
 * Bridged: Similar trick with UNAWARE_PVID instead.
 */
#define UNAWARE_PVID			0
#define HOST_PVID			4095
#define VLAN_HSR_PRP			4094
#define VLAN_MAX			(VLAN_HSR_PRP - 1)

#define VLAN_N_VID 4096

/* VLAN flags for VLAN table defined in ANA_VLANTIDX */
#define LAN9645X_VLAN_SRC_CHK		0x01
#define LAN9645X_VLAN_MIRROR		0x02
#define LAN9645X_VLAN_LEARN_DISABLED	0x04
#define LAN9645X_VLAN_PRIV_VLAN	0x08
#define LAN9645X_VLAN_FLOOD_DIS	0x10
#define LAN9645X_VLAN_SEC_FWD_ENA	0x20

/* 160KiB / 1.25Mbit */
#define LAN9645X_BUFFER_MEMORY (160 * 1024)

/* Port Group Identifiers (PGID) are port-masks applied to all frames.
 * The replicated registers are organized like so in HW:
 *
 * 0-63:         Destination analysis
 * 64-79:        Aggregation analysis
 * 80-(80+10-1): Source port analysis
 *
 * Destination: By default the first 9 port masks == BIT(port_num). Never change
 * these except for aggregation. Remaining dst masks are for L2 MC and
 * flooding. (See FLOODING and FLOODING_IPMC).
 *
 * Aggregation: Used to pick a port within an aggregation group. If no
 * aggregation is configured, these are all-ones.
 *
 * Source: Used to prevent frames from being looped back on receiving ports,
 * and must be updated according to aggregation configuration.
 * A frame that is received on port n, uses mask 80+n as a mask to filter out
 * destination ports to avoid loopback, or to facilitate port grouping
 * (port-based VLANs). The default values are that all bits are set except for
 * the index number.
 *
 * We reserve destination PGIDs at the end of the range.
 */

#define PGID_AGGR			64
#define PGID_SRC			80
#define PGID_ENTRIES			89

#define PGID_AGGR_NUM			(PGID_SRC - PGID_AGGR)

/* Reserved PGIDs */
#define PGID_GP_START			CPU_PORT
#define PGID_GP_END			PGID_MRP

#define PGID_MRP			(PGID_AGGR - 7)
#define PGID_CPU			(PGID_AGGR - 6)
#define PGID_UC				(PGID_AGGR - 5)
#define PGID_BC				(PGID_AGGR - 4)
#define PGID_MC				(PGID_AGGR - 3)
#define PGID_MCIPV4			(PGID_AGGR - 2)
#define PGID_MCIPV6			(PGID_AGGR - 1)

/* Flooding PGIDS:
 * PGID_UC
 * PGID_MC*
 * PGID_BC
 *
 * The flooding masks only fluctuate between two states. All phys ports, or all
 * ports (incl. cpu).
 *
 * The PGID_BC is always all ports. So it would suffice to reserve two PGIDS
 * for flooding, one with all_ports and one with all_phys_ports.
 * and then switch flooding between these instead of updating the port masks in
 * the PGIDS.
 *
 * This way we save some pgids. The downside is we require 8 register writes
 * to change FLD_UC, FLD_BC and FLD_MC
 *
 * and two for IP_MC (ctrl + data).
 */

#define LAN9645X_NUM_MACT_ROWS 2048

#define RD_SLEEP_US 3
#define RD_SLEEPTIMEOUT_US 100000

#define lan9645x_rd_poll_timeout(_lan9645x, _reg_macro, _val, _cond)     \
	regmap_read_poll_timeout(lan_rmap((_lan9645x), _reg_macro),	\
				 lan_rel_addr(_reg_macro), (_val),	\
				 (_cond), RD_SLEEP_US, RD_SLEEPTIMEOUT_US)

#define LAN9645X_VCAP_CID_IS1_L0 VCAP_CID_INGRESS_L0 /* IS1 lookup 0 */
#define LAN9645X_VCAP_CID_IS1_L1 VCAP_CID_INGRESS_L1 /* IS1 lookup 1 */
#define LAN9645X_VCAP_CID_IS1_L2 VCAP_CID_INGRESS_L2 /* IS1 lookup 2 */
#define LAN9645X_VCAP_CID_IS1_MAX (VCAP_CID_INGRESS_L3 - 1) /* IS1 Max */

#define LAN9645X_VCAP_CID_IS2_L0 VCAP_CID_INGRESS_STAGE2_L0 /* IS2 lookup 0 */
#define LAN9645X_VCAP_CID_IS2_L1 VCAP_CID_INGRESS_STAGE2_L1 /* IS2 lookup 1 */
#define LAN9645X_VCAP_CID_IS2_MAX (VCAP_CID_INGRESS_STAGE2_L2 - 1) /* IS2 Max */

#define LAN9645X_VCAP_CID_ES0_L0 VCAP_CID_EGRESS_L0 /* ES0 lookup 0 */
#define LAN9645X_VCAP_CID_ES0_MAX (VCAP_CID_EGRESS_L1 - 1) /* ES0 Max */

/* Policer indexes */
#define LAN9645X_POL_IX_PORT      0 /* 0-9    : 10 port policers */
#define LAN9645X_POL_IX_QUEUE    10 /* 10-90  : 80 queue policers (10p * 8q) */
#define LAN9645X_POL_IX_POOL     91 /* 91-344 : 263 PSFP and VCAP IS1/IS2 policers */
#define LAN9645X_POL_IX_MAX      344
#define LAN9645X_NUM_POL_POOL   (LAN9645X_POL_IX_MAX + 1 - LAN9645X_POL_IX_POOL)

#define LAN9645X_PHC_COUNT		3
#define LAN9645X_PHC_PORT		0
#define LAN9645X_PHC_PINS_NUM		4

#define IFH_PDU_TYPE_NONE		0
#define IFH_PDU_TYPE_IPV4		7
#define IFH_PDU_TYPE_IPV6		8

#define LAN9645X_LED_PROP_CNT		2
#define LAN9645X_LED_PROP_IDX		0
#define LAN9645X_LED_PROP_DRIVE		1

#define LAN9645X_PSFP_NUM_SFI 128 /* Number of Stream Filter Instances */
#define LAN9645X_PSFP_NUM_SGI 128 /* Number of Stream Gate Instances */
#define LAN9645X_PSFP_NUM_GCE 4 /* Number of Gate Control Entries/gate */

/* Minimum supported cycle time in nanoseconds */
#define LAN9645X_PSFP_SG_MIN_CYCLE_TIME_NS (1 * NSEC_PER_USEC) /* 1 usec */

/* Maximum supported cycle time in nanoseconds */
#define LAN9645X_PSFP_SG_MAX_CYCLE_TIME_NS \
	((1 * NSEC_PER_SEC) - 1) /* 999.999.999 nsec */

/* Maximum IPV value */
#define LAN9645X_PSFP_SG_MAX_IPV 7

/* QOS port configuration */
#define LAN9645X_DSCP_COUNT		64
#define LAN9645X_DEI_COUNT		2
#define LAN9645X_DPL_COUNT		2
#define LAN9645X_PCP_COUNT		8
#define LAN9645X_PRIO_COUNT		8

/* Rewriter VLAN port tagging encoding for REW:PORT[0-10]:TAG_CFG.TAG_CFG
 *
 * 0: Port tagging disabled.
 * 1: Tag all frames, except when VID=PORT_VLAN_CFG.PORT_VID or VID=0.
 * 2: Tag all frames, except when VID=0.
 * 3: Tag all frames.
 */
enum lan9645x_vlan_port_tag {
	LAN9645X_TAG_DISABLED = 0,
	LAN9645X_TAG_NO_PVID_NO_UNAWARE = 1,
	LAN9645X_TAG_NO_UNAWARE = 2,
	LAN9645X_TAG_ALL = 3,
};

/* NPI port prefix config encoding
 *
 * 0: No CPU extraction header (normal frames)
 * 1: CPU extraction header without prefix
 * 2: CPU extraction header with short prefix
 * 3: CPU extraction header with long prefix
 */
enum lan9645x_tag_prefix {
	LAN9645X_TAG_PREFIX_DISABLED = 0,
	LAN9645X_TAG_PREFIX_NONE = 1,
	LAN9645X_TAG_PREFIX_SHORT = 2,
	LAN9645X_TAG_PREFIX_LONG = 3,
};

enum {
	LAN9645X_SPEED_DISABLED = 0,
	LAN9645X_SPEED_10 = 1,
	LAN9645X_SPEED_100 = 2,
	LAN9645X_SPEED_1000 = 3,
	LAN9645X_SPEED_2500 = 4,
};

/* MAC table entry types.
 * ENTRYTYPE_NORMAL is subject to aging.
 * ENTRYTYPE_LOCKED is not subject to aging.
 * ENTRYTYPE_MACv4 is not subject to aging. For IPv4 multicast.
 * ENTRYTYPE_MACv6 is not subject to aging. For IPv6 multicast.
 */
enum macaccess_entry_type {
	ENTRYTYPE_NORMAL = 0,
	ENTRYTYPE_LOCKED,
	ENTRYTYPE_MACV4,
	ENTRYTYPE_MACV6,
};

struct lan9645x_mact_common {
	struct lan9645x_mact_key {
		u16 vid;
		u8 mac[ETH_ALEN] __aligned(2);
	} key;
	u32 row: 11, /* 2048 rows, 4 buckets each */
	    pgid: 6, /* 0-63 general purpose pgids. */
	    type: 2,
	    valid: 1,
	    processed: 1,
	    dyn_learned: 1;
};

struct lan9645x_mact_entry {
	struct lan9645x_mact_common common;
	struct list_head list;
	struct net_device *bond;
};

enum vcap_is2_port_sel_ipv6 {
	VCAP_IS2_PS_IPV6_TCPUDP_OTHER,
	VCAP_IS2_PS_IPV6_STD,
	VCAP_IS2_PS_IPV6_IP4_TCPUDP_IP4_OTHER,
	VCAP_IS2_PS_IPV6_MAC_ETYPE,
};

enum vcap_is1_port_sel_other {
	VCAP_IS1_PS_OTHER_NORMAL,
	VCAP_IS1_PS_OTHER_7TUPLE,
	VCAP_IS1_PS_OTHER_DBL_VID,
	VCAP_IS1_PS_OTHER_DMAC_VID,
};

enum vcap_is1_port_sel_ipv4 {
	VCAP_IS1_PS_IPV4_NORMAL,
	VCAP_IS1_PS_IPV4_7TUPLE,
	VCAP_IS1_PS_IPV4_5TUPLE_IP4,
	VCAP_IS1_PS_IPV4_DBL_VID,
	VCAP_IS1_PS_IPV4_DMAC_VID,
};

enum vcap_is1_port_sel_ipv6 {
	VCAP_IS1_PS_IPV6_NORMAL,
	VCAP_IS1_PS_IPV6_7TUPLE,
	VCAP_IS1_PS_IPV6_5TUPLE_IP4,
	VCAP_IS1_PS_IPV6_NORMAL_IP6,
	VCAP_IS1_PS_IPV6_5TUPLE_IP6,
	VCAP_IS1_PS_IPV6_DBL_VID,
	VCAP_IS1_PS_IPV6_DMAC_VID,
};

struct lan9645x_streamt_entry {
	u32 time_last_seen;
	u16 gen_seq_num;
	u16 isdx;
	u16 split_mask;
	u16 input_port_mask;
	bool rtag_pop_ena;
	bool seq_gen_ena;
	bool stream_split;
	bool seq_gen_err_status;
};

struct lan9645x_stream {
	struct mutex lock; /* Lock for stream table access and ISDX allocation. */
	/* Track allocated ISDXs indices in hw */
	DECLARE_BITMAP(isdx_mask, LAN9645X_ISDX_MAX);
};

enum lan9645x_hsr_type {
	LAN9645X_HSR_UNSUPPORTED = 0,
	LAN9645X_HSR,
	LAN9645X_PRP,
};

struct lan9645x_hsr_prp {
	unsigned char mac[ETH_ALEN];
	struct mutex lock; /* Lock HSR/PRP management. */
	u32 isdx_vrule_id; /* TX isdx classification for seqnum generation */
	u32 local_ring_vrule_id; /* HSR only: kill own frames on ring */
	u32 ptp_dd_vrule_id; /* HSR only: disable DD for ptp */
	u16 isdx; /* Allocated ISDX for tx stream */
	int port_a;
	int port_b;
	bool enabled;
	enum lan9645x_hsr_type type; /* HSR or PRP */
};

struct lan9645x_mirror {
	refcount_t refcount;
	int to;
};

struct lan9645x_policer {
	/* kilobit per second */
	u32 rate;
	/* bytes */
	u32 burst;
};

struct lan9645x_phc {
	struct ptp_clock *clock;
	struct ptp_clock_info info;
	struct ptp_pin_desc pins[LAN9645X_PHC_PINS_NUM];
	struct hwtstamp_config hwtstamp_config;
	struct lan9645x *lan9645x;
	u8 index;
};

struct lan9645x_ig_dscp {
	u8 prio;
	u8 dpl;
	bool trust;
};

struct lan9645x {
	struct device *dev;
	struct dsa_switch *ds;
	enum dsa_tag_protocol tag_proto;
	struct regmap *rmap[NUM_TARGETS];

	u32 host_flood_uc_mask;
	u32 host_flood_mc_mask;

	int shared_queue_sz;

	/* NPI chip_port */
	int npi;

	u8 num_phys_ports;
	struct lan9645x_port **ports;

	/* debugfs */
	struct dentry *debugfs_root;

	/* Forwarding Database */
	struct list_head mac_entries;
	struct mutex mact_lock; /* lock access to mact_table */
	struct mutex mac_entry_lock; /* lock for mac_entries list */
	struct net_device *bridge; /* Only support single bridge */
	u16 bridge_mask; /* Mask for bridged ports */
	u16 bridge_fwd_mask; /* Mask for forwarding bridged ports */
	struct mutex fwd_domain_lock; /* lock forwarding configuration */

	/* VLAN */
	u16 vlan_mask[VLAN_N_VID]; /* Port mask per vlan */
	u8 vlan_flags[VLAN_N_VID];
	DECLARE_BITMAP(cpu_vlan_mask, VLAN_N_VID); /* CPU port VLAN membership */

	/* Multicast Forwarding Database */
	struct list_head mdb_entries;
	struct list_head pgid_entries;
	/* lock for mdb_entries and pgid_entries */
	struct mutex mdb_lock;

	/* Statistics  */
	struct lan9645x_stats *stats;

	/* vcap */
	struct vcap_control *vcap_ctrl;

	/* Stream table for FRER and HSR/PRP */
	struct lan9645x_stream *stream;

	/* HSR/PRP */
	struct lan9645x_hsr_prp hsr;

	/* Port mirroring */
	struct lan9645x_mirror *mirror;
	/* Lower 16 bits is the egress mirror port mask, and top 16 bits is
	 * BIT(to) -  the mirrored port.
	 * Egress traffic on port 'to' must be mirroroed to ports in the lower
	 * 16bit mask.
	 * This us used by the tag driver, for egress mirroring on standalone
	 * ports, where we bypass the forwarding engine.
	 */
	u32 emirror_map;

	/* TC / QOS Policer resource management */
	DECLARE_BITMAP(pol_idx_mask, LAN9645X_NUM_POL_POOL);
	DECLARE_BITMAP(sfi_idx_mask, LAN9645X_PSFP_NUM_SFI);
	DECLARE_BITMAP(sgi_idx_mask, LAN9645X_PSFP_NUM_SGI);
	struct mutex qos_lock; /* Global QOS: dscp, qos policers */

	/* TC chain_id to isdx management */
	struct list_head link_isdx;
	struct mutex link_isdx_lock;

	/* PTP */
	bool ptp;
	struct lan9645x_phc phc[LAN9645X_PHC_COUNT];
	struct mutex ptp_clock_lock; /* lock for phc */
	spinlock_t ptp_ts_id_lock; /* lock for ts_id */
	struct mutex ptp_lock; /* lock for ptp interface state */
	u16 ptp_skbs;
	int ptp_ext_irq;
	int ptp_irq;

	/* QOS DSCP map */
	struct lan9645x_ig_dscp i_dscp_map[LAN9645X_DSCP_COUNT];
};

struct lan9645x_port_qos {
	u8 i_default_prio;
	u8 i_default_dpl;
	u8 i_default_pcp;
	u8 i_default_dei;

	struct {
		u8 prio;
		u8 dpl;
	} i_map[LAN9645X_PCP_COUNT][LAN9645X_DEI_COUNT];
	struct {
		bool tag_map_enable;
		bool dscp_map_enable;
	} i_mode;

	u8 e_default_pcp;
	u8 e_default_dei;
	struct {
		u8 pcp;
		u8 dei;
	} e_map[LAN9645X_PRIO_COUNT][LAN9645X_DPL_COUNT];
	enum {
		E_MODE_CLASSIFIED = 0,
		E_MODE_PORT_PCP_DEI = 1,
		E_MODE_MAPPED = 2,
		E_MODE_QOS_DP = 3,
	} e_mode;

	u8 pfc_enable;
};

struct lan9645x_port {
	struct lan9645x *lan9645x;

	u16 pvid;
	u16 untagged_vid;
	u8 chip_port;
	u8 stp_state;
	bool vlan_aware;
	bool learn_ena;
	bool mcast_ena;

	phy_interface_t phy_mode;
	struct phylink_pcs phylink_pcs;
	struct phy *serdes;
	struct fwnode_handle *fwnode;

	int speed; /* internal speed value LAN9645X_SPEED_* */
	struct list_head path_delays;
	u32 rx_delay;

	struct net_device *bond; /* LAG upper device */
	enum netdev_lag_hash hash_type;
	bool lag_tx_active;

	struct net_device *hsr; /* HSR/PRP upper device */

	struct mutex qos_lock; /* Port QOS config */
	struct lan9645x_port_qos qos;

	/* PTP */
	struct sk_buff_head tx_skbs;
	struct sk_buff_head rx_skbs;
	u16 ts_id;
	u8 ptp_tx_cmd;
	bool ptp_rx_cmd;

	bool cut_thru_ena;
};

struct lan9645x_path_delay {
	struct list_head list;
	u32 rx_delay;
	u32 tx_delay;
	u32 speed;
};

static inline struct phylink *lan9645x_get_phylink(struct lan9645x *lan9645x,
						   int port)
{
	return dsa_to_port(lan9645x->ds, port)->pl;
}

/* PFC_CFG.FC_LINK_SPEED encoding */
static inline int lan9645x_speed_fc_enc(int speed)
{
	switch (speed) {
	case LAN9645X_SPEED_10:
		return 3;
	case LAN9645X_SPEED_100:
		return 2;
	case LAN9645X_SPEED_1000:
		return 1;
	case LAN9645X_SPEED_2500:
		return 0;
	default:
		WARN_ON_ONCE(1);
		return 1;
	}
}

/* Watermark encode. See QSYS:RES_CTRL[*]:RES_CFG.WM_HIGH for details.
 * Returns lowest encoded number which will fit request/ is larger than request.
 * Or the maximum representable value, if request is too large.
 */
static inline u32 lan9645x_wm_enc(u32 value)
{
#define GWM_MULTIPLIER_BIT BIT(8)
#define LAN9645X_BUFFER_CELL_SZ 64
	value = DIV_ROUND_UP(value, LAN9645X_BUFFER_CELL_SZ);

	if (value >= GWM_MULTIPLIER_BIT) {
		value = DIV_ROUND_UP(value, 16);
		if (value >= GWM_MULTIPLIER_BIT)
			value = (GWM_MULTIPLIER_BIT - 1);
		value |= GWM_MULTIPLIER_BIT;
	}

	return value;
}

static inline struct lan9645x_port *
lan9645x_port_from_netdev(struct net_device *dev)
{
	struct lan9645x *lan9645x;
	struct dsa_port *dp;

	dp = dsa_port_from_netdev(dev);

	if (IS_ERR_OR_NULL(dp))
		return NULL;

	lan9645x = dp->ds->priv;

	return lan9645x->ports[dp->index];
}

static inline struct lan9645x *lan9645x_from_netdev(struct net_device *dev)
{
	struct lan9645x_port *p = lan9645x_port_from_netdev(dev);

	if (IS_ERR_OR_NULL(p))
		return NULL;

	return p->lan9645x;
}

static inline struct lan9645x_port *lan9645x_to_port(struct lan9645x *lan9645x,
						     int port)
{
	if (WARN_ON(!(port >= 0 && port < lan9645x->num_phys_ports)))
		return NULL;

	return lan9645x->ports[port];
}

static inline struct net_device *lan9645x_port_to_ndev(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;

	if (!dsa_is_user_port(lan9645x->ds, p->chip_port))
		return NULL;

	return dsa_to_port(lan9645x->ds, p->chip_port)->user;
}

static inline struct net_device *
lan9645x_chipport_to_ndev(struct lan9645x *lan9645x, int port)
{
	return lan9645x_port_to_ndev(lan9645x_to_port(lan9645x, port));
}

static inline bool lan9645x_port_is_bridged(struct lan9645x_port *p)
{
	if (!p)
		return false;

	return !!(p->lan9645x->bridge_mask & BIT(p->chip_port));
}

static inline bool lan9645x_port_is_hsr(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;

	return lan9645x->hsr.enabled && lan9645x->hsr.type == LAN9645X_HSR &&
		(lan9645x->hsr.port_a == p->chip_port ||
		 lan9645x->hsr.port_b == p->chip_port);
}

static inline u32 __lan_rel_addr(int gbase, int ginst, int gcnt,
				 int gwidth, int raddr, int rinst,
				 int rcnt, int rwidth)
{
	WARN_ON(ginst >= gcnt);
	WARN_ON(rinst >= rcnt);
	return gbase + ginst * gwidth + raddr + rinst * rwidth;
}

/* Get register address relative to target instance */
static inline u32 lan_rel_addr(enum lan9645x_target t, int tinst, int tcnt,
			       int gbase, int ginst, int gcnt, int gwidth,
			       int raddr, int rinst, int rcnt, int rwidth)
{
	WARN_ON(tinst >= tcnt);
	return __lan_rel_addr(gbase, ginst, gcnt, gwidth, raddr, rinst,
			      rcnt, rwidth);
}

static inline u32 lan_rd(struct lan9645x *lan9645x, enum lan9645x_target t,
			 int tinst, int tcnt, int gbase, int ginst,
			 int gcnt, int gwidth, int raddr, int rinst,
			 int rcnt, int rwidth)
{
	u32 addr, val = 0;

	addr = lan_rel_addr(t, tinst, tcnt, gbase, ginst, gcnt, gwidth,
			    raddr, rinst, rcnt, rwidth);

	WARN_ON_ONCE(regmap_read(lan9645x->rmap[t + tinst], addr, &val));

	return val;
}

static inline int lan_bulk_rd(void *val, size_t val_count,
			      struct lan9645x *lan9645x,
			      enum lan9645x_target t, int tinst, int tcnt,
			      int gbase, int ginst, int gcnt, int gwidth,
			      int raddr, int rinst, int rcnt, int rwidth)
{
	u32 addr;

	addr = lan_rel_addr(t, tinst, tcnt, gbase, ginst, gcnt, gwidth,
			    raddr, rinst, rcnt, rwidth);

	return regmap_bulk_read(lan9645x->rmap[t + tinst], addr, val,
				val_count);
}

static inline struct regmap *lan_rmap(struct lan9645x *lan9645x,
				      enum lan9645x_target t, int tinst,
				      int tcnt, int gbase, int ginst,
				      int gcnt, int gwidth, int raddr,
				      int rinst, int rcnt, int rwidth)
{
	return lan9645x->rmap[t + tinst];
}

static inline void lan_wr(u32 val, struct lan9645x *lan9645x,
			  enum lan9645x_target t, int tinst, int tcnt,
			  int gbase, int ginst, int gcnt, int gwidth,
			  int raddr, int rinst, int rcnt, int rwidth)
{
	u32 addr;

	addr = lan_rel_addr(t, tinst, tcnt, gbase, ginst, gcnt, gwidth,
			    raddr, rinst, rcnt, rwidth);

	WARN_ON_ONCE(regmap_write(lan9645x->rmap[t + tinst], addr, val));
}

static inline void lan_rmw(u32 val, u32 mask, struct lan9645x *lan9645x,
			   enum lan9645x_target t, int tinst, int tcnt,
			   int gbase, int ginst, int gcnt, int gwidth,
			   int raddr, int rinst, int rcnt, int rwidth)
{
	u32 addr;

	addr = lan_rel_addr(t, tinst, tcnt, gbase, ginst, gcnt, gwidth,
			    raddr, rinst, rcnt, rwidth);

	WARN_ON_ONCE(regmap_update_bits(lan9645x->rmap[t + tinst],
					addr, mask, val));
}

/* lan9645x_npi.c */
void lan9645x_npi_port_init(struct lan9645x *lan9645x,
			    struct dsa_port *cpu_port);
void lan9645x_npi_port_deinit(struct lan9645x *lan9645x, int port);

/* lan9645x_phylink.c */
void lan9645x_phylink_get_caps(struct lan9645x *lan9645x, int port,
			       struct phylink_config *config);
void lan9645x_phylink_mac_config(struct lan9645x *lan9645x, int port,
				 unsigned int mode,
				 const struct phylink_link_state *state);
void lan9645x_phylink_mac_link_up(struct lan9645x *lan9645x, int port,
				  unsigned int link_an_mode,
				  phy_interface_t interface,
				  struct phy_device *phydev, int speed,
				  int duplex, bool tx_pause, bool rx_pause);
void lan9645x_phylink_mac_link_down(struct lan9645x *lan9645x, int port,
				    unsigned int link_an_mode,
				    phy_interface_t interface);
void lan9645x_phylink_port_down(struct lan9645x *lan9645x, int port);
struct phylink_pcs *lan9645x_phylink_mac_select_pcs(struct lan9645x *lan9645x,
						    int port,
						    phy_interface_t iface);
void lan9645x_pcs_aneg_restart(struct phylink_pcs *pcs);
int lan9645x_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			phy_interface_t interface,
			const unsigned long *advertising,
			bool permit_pause_to_mac);
void lan9645x_pcs_get_state(struct phylink_pcs *pcs,
			    struct phylink_link_state *state);

/* lan9645x_main.c */
bool lan9645x_port_is_bridged(struct lan9645x_port *p);
u16 lan9645x_vlan_unaware_pvid(struct lan9645x *lan9645x,
			       struct net_device *bridge);
void lan9645x_port_set_learning(struct lan9645x *lan9645x, int port,
				bool enabled);
void lan9645x_update_fwd_mask(struct lan9645x *lan9645x, bool joining);

/* MAC table: lan9645x_mac.c */
int lan9645x_mact_flush(struct lan9645x *lan9645x, int port);
int lan9645x_mact_learn(struct lan9645x *lan9645x, int port,
			const unsigned char *addr, u16 vid,
			enum macaccess_entry_type type);
int lan9645x_mact_forget(struct lan9645x *lan9645x,
			 const unsigned char mac[ETH_ALEN], unsigned int vid,
			 enum macaccess_entry_type type);
int lan9645x_mact_read(struct lan9645x *lan9645x, int port, int row, int bucket,
		       struct lan9645x_mact_entry *entry);
void lan9645x_mac_init(struct lan9645x *lan9645x);
void lan9645x_mac_deinit(struct lan9645x *lan9645x);
irqreturn_t lan9645x_mac_irq_handler(int virq, void *args);
int lan9645x_mact_dsa_dump(struct lan9645x *lan9645x, int port,
			   dsa_fdb_dump_cb_t *cb, void *data);
int lan9645x_mact_entry_del(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid);
int lan9645x_mact_entry_add(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid);
void lan9645x_migrate_lag_fdb(struct lan9645x *lan9645x,
			      struct net_device *bond, int old_lag_id,
			      int new_lag_id);

/* VLAN lan9645x_vlan.c */
void lan9645x_vlan_init(struct lan9645x *lan9645x);
void lan9645x_vlan_port_set_vlan_aware(struct lan9645x_port *p,
				       bool vlan_aware);
void lan9645x_vlan_port_set_vid(struct lan9645x_port *p, u16 vid, bool pvid,
				bool untagged);
void lan9645x_vlan_port_apply(struct lan9645x_port *p);
void lan9645x_vlan_port_rew_host(struct lan9645x_port *p);
void lan9645x_vlan_port_add_vlan(struct lan9645x_port *p, u16 vid, bool pvid,
				 bool untagged);
void lan9645x_vlan_port_del_vlan(struct lan9645x_port *p, u16 vid);
void lan9645x_vlan_cpu_set_vlan(struct lan9645x *lan9645x, u16 vid);
void lan9645x_vlan_cpu_clear_vlan(struct lan9645x *lan9645x, u16 vid);
void lan9645x_vlan_set_mask(struct lan9645x *lan9645x, u16 vid);
void lan9645x_vlan_set_hostmode(struct lan9645x_port *p);
int lan9645x_port_vlan_prepare(struct lan9645x_port *p, u16 vid, bool pvid,
			       bool untagged, struct netlink_ext_ack *extack);

/* LAG: Link aggregation group lan9645x_lag.c */
u32 lan9645x_lag_dev_get_mask(struct lan9645x *lan9645x,
			      struct net_device *bond);
int lan9645x_lag_dev_get_id(struct lan9645x *lan9645x, struct net_device *bond);
void lan9645x_lag_port_set_pgids(struct lan9645x *lan9645x, int port,
				 bool leaving, u32 bond_mask);
int lan9645x_lag_apply_hash_type(struct lan9645x *lan9645x,
				 struct netdev_lag_upper_info *info,
				 struct netlink_ext_ack *extack);
int lan9645x_lag_join_prepare(struct lan9645x *lan9645x,
			      struct netdev_lag_upper_info *info,
			      struct netlink_ext_ack *extack);
int lan9645x_lag_reconfigure(struct lan9645x *lan9645x, struct net_device *bond,
			     int port, bool leaving);

/* Multicast Database lan9645x_mdb.c */
int lan9645x_mdb_port_add(struct lan9645x *lan9645x, int port,
			  const struct switchdev_obj_port_mdb *mdb,
			  struct net_device *bridge);
int lan9645x_mdb_port_del(struct lan9645x *lan9645x, int port,
			  const struct switchdev_obj_port_mdb *mdb,
			  struct net_device *bridge);
void lan9645x_mdb_init(struct lan9645x *lan9645x);
void lan9645x_mdb_deinit(struct lan9645x *lan9645x);

/* VCAP */
int lan9645x_vcap_init(struct lan9645x *lan9645x);
void lan9645x_vcap_deinit(struct lan9645x *lan9645x);
#if defined(CONFIG_DEBUG_FS)
int lan9645x_vcap_port_info(struct net_device *dev, struct vcap_admin *admin,
			    struct vcap_output_print *out);
#else
static inline int lan9645x_vcap_port_info(struct net_device *dev,
					  struct vcap_admin *admin,
					  struct vcap_output_print *out)
{
	return 0;
}
#endif
int lan9645x_vcap_get_port_keyset(struct net_device *ndev,
				  struct vcap_admin *admin, int cid,
				  u16 l3_proto,
				  struct vcap_keyset_list *keysetlist);
const char *lan9645x_vcap_keyset_name(struct lan9645x *lan9645x,
				      enum vcap_keyfield_set keyset);
const char *lan9645x_vcap_keyset_name_short(struct lan9645x *lan9645x,
					    enum vcap_keyfield_set keyset);

/* Stream table, ISDX management HSR/PRP and FRER */
int lan9645x_stream_isdx_alloc(struct lan9645x *lan9645x);
void lan9645x_stream_isdx_free(struct lan9645x *lan9645x, u16 isdx);
int lan9645x_streamt_read(struct lan9645x *lan9645x, u16 isdx,
			  struct lan9645x_streamt_entry *entry);
int lan9645x_streamt_write(struct lan9645x *lan9645x, u16 isdx,
			   struct lan9645x_streamt_entry *entry);
int lan9645x_streamt_del(struct lan9645x *lan9645x, u16 isdx);
int lan9645x_streamt_init(struct lan9645x *lan9645x);
void lan9645x_streamt_deinit(struct lan9645x *lan9645x);

/* HSR and PRP management lan9645x_hsr.c */
int lan9645x_hsr_prp_init(struct lan9645x *lan9645x);
void lan9645x_hsr_prp_deinit(struct lan9645x *lan9645x);
int lan9645x_hsr_prp_pair_add(struct lan9645x *lan9645x, struct lan9645x_port *lrea,
			      struct lan9645x_port *lreb,
			      struct net_device *lrea_dev, struct net_device *hsr,
			      enum lan9645x_hsr_type type);
int lan9645x_hsr_prp_pair_del(struct lan9645x *lan9645x, int port,
			      struct net_device *hsr);
u32 lan9645x_hsr_prp_dev_get_mask(struct lan9645x *lan9645x,
				  struct net_device *hsr);
enum lan9645x_hsr_type lan9645x_hsr_prp_ver_to_type(enum hsr_version ver);
int lan9645x_hsr_prp_prepare(struct lan9645x *lan9645x, int port,
			     struct net_device *hsr, enum lan9645x_hsr_type type,
			     struct netlink_ext_ack *extack);
int lan9645x_hsr2type(struct net_device *hsr, enum lan9645x_hsr_type *type);

/* Mirroring */
int lan9645x_mirror_port_add(struct lan9645x *lan9645x, int from, int to,
			     bool ingress, struct netlink_ext_ack *extack);
void lan9645x_mirror_port_del(struct lan9645x *lan9645x, int from,
			      bool ingress);
void lan9645x_mirror_put(struct lan9645x *lan9645x);
struct lan9645x_mirror *lan9645x_mirror_get(struct lan9645x *lan9645x, int to,
					    struct netlink_ext_ack *extack);

/* Port Policer */
int lan9645x_police_port_add(struct lan9645x *lan9645x, int port,
			     struct lan9645x_policer *pol);
void lan9645x_police_port_del(struct lan9645x *lan9645x, int port);
int lan9645x_police_add(struct lan9645x_port *p,
			struct lan9645x_policer *pol, int pol_idx);
void lan9645x_police_del(struct lan9645x *lan9645x, u16 pol_idx);
void lan9645x_police_port_init(struct lan9645x_port *p);

/* TC matchall lan9645x_tc_matchall.c */
int lan9645x_tc_matchall_goto_add(struct lan9645x_port *p,
				  struct tc_cls_matchall_offload *f);
int lan9645x_tc_matchall_goto_del(struct lan9645x_port *p,
				  struct tc_cls_matchall_offload *f);

/* QOS quality of service lan9645x_qos.c */
int lan9645x_qos_polix_alloc(struct lan9645x *lan9645x);
void lan9645x_qos_polix_free(struct lan9645x *lan9645x, u16 polix);
int lan9645x_qos_init(struct lan9645x *lan9645x);
int __lan9645x_qos_portconf_set(struct lan9645x_port *p,
				struct lan9645x_port_qos *cfg);
int lan9645x_qos_portconf_set(struct lan9645x_port *p,
			      struct lan9645x_port_qos *cfg);
void __lan9645x_qos_portconf_get(struct lan9645x_port *p,
				 struct lan9645x_port_qos *cfg);
void lan9645x_qos_portconf_get(struct lan9645x_port *p,
			       struct lan9645x_port_qos *cfg);
int __lan9645x_qos_dscp_conf_set(struct lan9645x *lan9645x, u8 dscp,
				 struct lan9645x_ig_dscp *cfg);
int __lan9645x_qos_dscp_conf_get(struct lan9645x *lan9645x,
				 u8 dscp,
				 struct lan9645x_ig_dscp *cfg);

/* DCB integration lan9645x_dcb.c */
int lan9645x_dcb_port_get_default_prio(struct lan9645x *lan9645x, int port);
int lan9645x_dcb_port_set_default_prio(struct lan9645x *lan9645x, int port,
				       u8 prio);
int lan9645x_dcb_port_set_apptrust(struct lan9645x *lan9645x, int port,
				   const u8 *sel, int nsel);
int lan9645x_dcb_port_get_apptrust(struct lan9645x *lan9645x, int port, u8 *sel,
				   int *nsel);
int lan9645x_dcb_port_get_dscp_prio(struct lan9645x *lan9645x, int port,
				    u8 dscp);
int lan9645x_dcb_add_dscp_prio(struct lan9645x *lan9645x,
			       u8 dscp, u8 prio);
int lan9645x_dcb_del_dscp_prio(struct lan9645x *lan9645x,
			       u8 dscp, u8 prio);
int lan9645x_dcb_get_pcp_dei_prio(struct lan9645x *lan9645x, int port,
				  u8 pcp, u8 dei);
int lan9645x_dcb_add_pcp_dei_prio(struct lan9645x *lan9645x, int port,
				  u8 pcp, u8 dei, u8 prio);
int lan9645x_dcb_del_pcp_dei_prio(struct lan9645x *lan9645x, int port,
				  u8 pcp, u8 dei, u8 prio);
int lan9645x_dcb_getpfc(struct lan9645x *lan9645x, int port,
			struct ieee_pfc *pfc);
int lan9645x_dcb_setpfc(struct lan9645x *lan9645x, int port, u8 pfc_enable);

/* TC flower lan9645x_tc_flower.c */
int lan9645x_tc_flower_add(struct lan9645x_port *p, struct flow_cls_offload *f,
			   bool ingress);
int lan9645x_tc_flower_del(struct lan9645x_port *p, struct flow_cls_offload *f,
			   bool ingress);
int lan9645x_tc_flower_stats(struct lan9645x_port *p,
			     struct flow_cls_offload *f);

/* Credit Based Shaping CBS: lan9645x_cbs.c */
int lan9645x_cbs_del(struct lan9645x *lan9645x, int port,
		     struct tc_cbs_qopt_offload *qopt);
int lan9645x_cbs_add(struct lan9645x *lan9645x, int port,
		     struct tc_cbs_qopt_offload *qopt);

/* lan9645x_mqprio.c */
int lan9645x_mqprio_set(struct lan9645x *lan9645x, int port,
			struct tc_mqprio_qopt_offload *mqprio);

/* token based filter: lan9645x_tbf.c */
int lan9645x_tbf_add(struct lan9645x *lan9645x, int port,
		     struct tc_tbf_qopt_offload *qopt);
int lan9645x_tbf_del(struct lan9645x *lan9645x, int port,
		     struct tc_tbf_qopt_offload *qopt);

/* lan9645x_ets.c */
int lan9645x_ets_del(struct lan9645x *lan9645x, int port,
		     struct tc_ets_qopt_offload *qopt);
int lan9645x_ets_add(struct lan9645x *lan9645x, int port,
		     struct tc_ets_qopt_offload *qopt);

/* lan9645x_cut_thru.c */
void lan9645x_cut_through_fwd(struct lan9645x *lan9645x);

/* lan9645x_eee.c */
int lan9645x_eee_mac_get(struct lan9645x *lan9645x, int port,
			 struct ethtool_keee *e);
int lan9645x_eee_mac_set(struct lan9645x *lan9645x, int port,
			 struct ethtool_keee *e);

/* lan9645x_ptp.c */
int lan9645x_port_hwtstamp_get(struct dsa_switch *ds, int port,
			       struct ifreq *ifr);
int lan9645x_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct ifreq *ifr);
void lan9645x_txtstamp(struct dsa_switch *ds, int port, struct sk_buff *skb);
bool lan9645x_rxtstamp_defer(struct dsa_switch *ds, int port,
			     struct sk_buff *skb, unsigned int type);
int lan9645x_get_ts_info(struct dsa_switch *ds, int port,
			 struct kernel_ethtool_ts_info *info);
int lan9645x_ptp_init(struct lan9645x *lan9645x);
void lan9645x_ptp_deinit(struct lan9645x *lan9645x);
irqreturn_t lan9645x_ptp_irq_handler(int irq, void *args);
irqreturn_t lan9645x_ptp_ext_irq_handler(int irq, void *args);
int lan9645x_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts);
u32 lan9645x_ptp_get_period_ps(void);

/* lan9645x_tas.c */
int lan9645x_taprio_add(struct lan9645x *lan9645x, int port,
			struct tc_taprio_qopt_offload *qopt);
int lan9645x_taprio_del(struct lan9645x *lan9645x, int port);
void lan9645x_taprio_init(struct lan9645x *lan9645x);
void lan9645x_taprio_deinit(struct lan9645x *lan9645x);
int lan9645x_taprio_speed_set(struct lan9645x_port *port, int speed);
void lan9645x_new_base_time(struct lan9645x *lan9645x, const u32 cycle_time,
			    const ktime_t org_base_time,
			    ktime_t *new_base_time);

/* PSFP Stream Filter configuration */
struct lan9645x_psfp_sf_cfg {
	bool block_oversize_ena; /* StreamBlockedDueToOversizeFrameEnable */
	bool block_oversize; /* StreamBlockedDueToOversizeFrame */
	bool force_block; /* Block all frames matching filter */
	u32 max_sdu; /* Maximum SDU size (zero disables SDU check) */
};

/* PSFP Gate Control Entry configuration */
struct lan9645x_psfp_gce_cfg {
	bool gate_state;   /* StreamGateState (true = enabled) */
	u32 interval; /* TimeInterval (nsec) */
	s32 ipv;           /* IPV (-1 disables IPV) */
	s32 maxoctets;     /* IntervalOctetMax (-1 disables check) */
};

/* PSFP Stream Gate configuration */
struct lan9645x_psfp_sg_cfg {
	bool gate_state;  /* PSFPAdminGateStates: Initial gate state (true = enabled) */
	s32 ipv;          /* PSFPAdminIPV  (-1 disables IPV) */
	u64 basetime;     /* PSFPAdminBaseTime */
	u32 cycletime;    /* PSFPAdminCycleTime */
	u32 cycletimeext; /* PSFPAdminCycleTimeExtension */
	u32 num_entries;  /* PSFPAdminControlListLength */
	struct lan9645x_psfp_gce_cfg gce[LAN9645X_PSFP_NUM_GCE];
};

int lan9645x_sfi_get(struct lan9645x *lan9645x, u32 *sfi_ix);
int lan9645x_sfi_put(struct lan9645x *lan9645x, u32 sfi_ix);
int lan9645x_sgi_get(struct lan9645x *lan9645x, u32 *sgi_ix);
int lan9645x_sgi_put(struct lan9645x *lan9645x, u32 sgi_ix);
int lan9645x_psfp_sf_set(struct lan9645x *lan9645x, const u32 sfi_ix,
			 const struct lan9645x_psfp_sf_cfg *const c);

int lan9645x_psfp_sg_set(struct lan9645x *lan9645x, const u32 sgi_ix,
			 const struct lan9645x_psfp_sg_cfg *const sg);

#endif /* __LAN9645X_MAIN_H__ */
