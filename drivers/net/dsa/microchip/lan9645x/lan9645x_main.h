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
#include <uapi/linux/mrp_bridge.h>

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
#define LAN9645X_TAS_NUM_GCL	900

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
#define LAN9645X_SFID_MAX 128

#define HSR_NETID 0x0
#define PRP_NETID 0x5
#define PRP_LANID_A 0x0
#define PRP_LANID_B 0x1
/* Reserved VLAN IDs.
 *
 * We use these to enable isolated VLAN-unaware standalone ports and
 * per-bridge isolation for VLAN-unaware bridges. Because the MAC table is
 * keyed on (mac, vid), distinct reserved VIDs give each unaware bridge
 * its own MAC table namespace.
 *
 * Standalone: untagged RX frames are classified to HOST_PVID.
 *
 * VLAN-unaware bridge n: untagged RX frames are classified to
 * VLAN_N_VID - n - 1 (counting down from 4094), where n is the
 * dsa_bridge.num. Frames forward within the bridge's PGID SRC mask,
 * and (mac, per-bridge-vid) entries isolate the MAC table across
 * bridges.
 *
 * VIDs 4000..4095 are reserved from userspace by VLAN_RSV_RANGE_START.
 */
#define HOST_PVID			0
#define VLAN_HSR_PRP			4095
#define VLAN_RSV_RANGE_START		4000
#define VLAN_MAX			(VLAN_RSV_RANGE_START - 1)

#define VLAN_N_VID 4096

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

/* PGID_MRP is a blackhole PGID */
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

/* This represents the base rule ID for the PTP rules that are added in the
 * VCAP to trap frames to CPU. This number needs to be bigger than the maximum
 * number of entries that can exist in the VCAP.
 */
#define LAN9645X_VCAP_PTP_RULE_ID		1000000
#define LAN9645X_VCAP_L2_PTP_TRAP		(LAN9645X_VCAP_PTP_RULE_ID + 0)
#define LAN9645X_VCAP_IPV4_EV_PTP_TRAP		(LAN9645X_VCAP_PTP_RULE_ID + 1)
#define LAN9645X_VCAP_IPV4_GEN_PTP_TRAP		(LAN9645X_VCAP_PTP_RULE_ID + 2)
#define LAN9645X_VCAP_IPV6_EV_PTP_TRAP		(LAN9645X_VCAP_PTP_RULE_ID + 3)
#define LAN9645X_VCAP_IPV6_GEN_PTP_TRAP		(LAN9645X_VCAP_PTP_RULE_ID + 4)

#define LAN9645X_VCAP_L2_PTP_REW_CMD		(LAN9645X_VCAP_PTP_RULE_ID + 5)
#define LAN9645X_VCAP_IPV4_PTP_REW_CMD		(LAN9645X_VCAP_PTP_RULE_ID + 6)
#define LAN9645X_VCAP_IPV6_PTP_REW_CMD		(LAN9645X_VCAP_PTP_RULE_ID + 7)
/* One-step Sync rules: separate from the event rules above so that Sync
 * gets IFH_REW_OP_ONE_STEP_PTP while other event types get TWO_STEP.
 */
#define LAN9645X_VCAP_L2_PTP_SYNC_REW_CMD	(LAN9645X_VCAP_PTP_RULE_ID + 8)
#define LAN9645X_VCAP_IPV4_PTP_SYNC_REW_CMD	(LAN9645X_VCAP_PTP_RULE_ID + 9)
#define LAN9645X_VCAP_IPV6_PTP_SYNC_REW_CMD	(LAN9645X_VCAP_PTP_RULE_ID + 10)
#define LAN9645X_VCAP_IS2_HSR_FWD1		(LAN9645X_VCAP_PTP_RULE_ID + 11)
#define LAN9645X_VCAP_IS2_HSR_FWD2		(LAN9645X_VCAP_PTP_RULE_ID + 12)

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

#define LAN9645X_NUM_BUM_POL 3

enum lan9645x_bum_mode {
	LAN9645X_BUM_MODE_DIS = 0,
	LAN9645X_BUM_MODE_CPU,
	LAN9645X_BUM_MODE_FPORTS,
	LAN9645X_BUM_MODE_CPU_AND_FPORTS,
};

enum lan9645x_bum_type {
	LAN9645X_BUM_UC = 0,
	LAN9645X_BUM_BC = 1,
	LAN9645X_BUM_MC = 2,

	__LAN9645X_BUM_NUM,
};

struct lan9645x_bum_pol {
	struct lan9645x *lan9645x;
	enum lan9645x_bum_type type;
	int unit;
	/* Frame-rate is 2**rate * UNIT */
	int rate;
	enum lan9645x_bum_mode mode;
	bool cpu_redir_ena;
	bool known_ena;
	bool unknown_ena;

	/* Only for MC bum policer */
	bool ipmc_known_ena;
	bool ipmc_unknown_ena;
};

struct lan9645x_bum_ctrl {
	struct lan9645x *lan9645x;
	int burst;
	struct lan9645x_bum_pol policers[LAN9645X_NUM_BUM_POL];
	struct list_head debugfs_list;
	/* Lock bum_ctrl and bum reg IO */
	struct mutex bum_lock;
};

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

/* Shadow of registers VLANTIDX + VLAN_PORT_MASK per VID.
 * portmask:  VLAN member ports      VLAN_PORT_MASK
 * untagged:  egress-untagged ports
 * src_chk:   VLAN_SRC_CHK           ingress filter: drop if port not in VLAN
 * mir:       VLAN_MIRROR            mirror frames on this VLAN
 * lrn_dis:   VLAN_LEARN_DISABLED
 * prv_vlan:  VLAN_PRIV_VLAN         private VLAN (see ISOLATED_PORTS)
 * fld_dis:   VLAN_FLOOD_DIS         disable unknown-DMAC flooding (incl. BC/MC)
 * s_fwd_ena: VLAN_SEC_FWD_ENA       secure forwarding (known SMAC only)
 */
struct lan9645x_vlan {
	u32 portmask: 10, /* ports 0-8 + CPU_PORT */
	    untagged: 9, /* ports 0-8 */
	    src_chk: 1,
	    mir: 1,
	    lrn_dis: 1,
	    prv_vlan: 1,
	    fld_dis: 1,
	    s_fwd_ena: 1;
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
	u8 lanid_err;
	bool rtag_pop_ena;
	bool seq_gen_ena;
	bool stream_split;
	bool seq_gen_err_status;
};

struct lan9645x_stream {
	/* Lock for stream table access and ISDX allocation.
	 *
	 * If this lock must be held at the same time as stats->hw_lock, then
	 * you must first lock stream->lock, then stats->hw_lock.
	 */
	struct mutex lock;
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
	u16 isdx; /* Allocated ISDX for tx stream */
	int port_a;
	int port_b;
	int shadow_ports[2];
	bool enabled;
	enum lan9645x_hsr_type type; /* HSR or PRP */
	struct list_head nodes;
	u16 ptp_ports;
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
	struct kernel_hwtstamp_config hwtstamp_config;
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

	/* Lock manual frame injection */
	struct mutex tx_lock;

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
	struct mutex fwd_domain_lock; /* lock forwarding configuration */

	/* VLAN entries */
	struct lan9645x_vlan vlans[VLAN_N_VID];

	/* Multicast Forwarding Database */
	struct list_head mdb_entries;
	struct list_head pgid_entries;
	/* lock for mdb_entries and pgid_entries */
	struct mutex mdb_lock;
	u32 mrouter_mask;
	u16 mc_flood_mask;
	u16 mc_disabled_mask;

	/* Statistics  */
	struct lan9645x_stats *stats;

	/* vcap */
	struct vcap_control *vcap_ctrl;

	/* Lock for ESDX allocation.
	 *
	 * If this lock must be held at the same time as stats->hw_lock, then
	 * you must first lock esdx_lock, then stats->hw_lock.
	 */
	struct mutex esdx_lock;
	/* Track allocated ESDX indices in hw */
	DECLARE_BITMAP(esdx_mask, LAN9645X_ESDX_MAX);

	/* Stream table for FRER and HSR/PRP */
	struct lan9645x_stream *stream;

	/* HSR/PRP */
	struct lan9645x_hsr_prp hsr;

	/* Port mirroring */
	struct lan9645x_mirror *mirror;

	/* TC / QOS Policer resource management */
	DECLARE_BITMAP(pol_idx_mask, LAN9645X_NUM_POL_POOL);
	DECLARE_BITMAP(sfi_idx_mask, LAN9645X_PSFP_NUM_SFI);
	DECLARE_BITMAP(sgi_idx_mask, LAN9645X_PSFP_NUM_SGI);
	DECLARE_BITMAP(tas_gcl_bitmap, LAN9645X_TAS_NUM_GCL);
	struct mutex qos_lock; /* Global QOS: dscp, qos policers */
	/* Lock SFI/SGI allocation, and tables SG_ACCESS/SFID_ACCESS
	 *
	 * If this lock must be held at the same time as stats->hw_lock, then
	 * you must first lock psfp_lock, then stats->hw_lock.
	 * */
	struct mutex psfp_lock;

	/* Polling FP verify status */
	struct delayed_work fp_work;
	struct workqueue_struct *queue;

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
	struct mutex ptp_logs_lock;
	bool ptp_enable_logs;
	struct list_head ptp_logs;
	u16 ptp_logs_count;

	/* QOS DSCP map */
	struct lan9645x_ig_dscp i_dscp_map[LAN9645X_DSCP_COUNT];

	/* BUM policers */
	struct lan9645x_bum_ctrl *bum;

	int num_port_dis;
	bool dd_dis;
	bool tsn_dis;

	struct afi_control *afi_ctrl;

	struct mrp_control *mrp_ctrl;
	int ana_irq;
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

struct lan9645x_fp_port_conf {
	u8 admin_status;        /* IEEE802.1Qbu: framePreemptionStatusTable */
	bool enable_tx;         /* IEEE802.3br: aMACMergeEnableTx */
	bool verify_disable_tx; /* IEEE802.3br: aMACMergeVerifyDisableTx */
	u8 verify_time;         /* IEEE802.3br: aMACMergeVerifyTime [msec] */
	u8 add_frag_size;       /* IEEE802.3br: aMACMergeAddFragSize */
};

struct lan9645x_fp_port_status {
	u32 hold_advance;      // TBD: IEEE802.1Qbu: holdAdvance [nsec]
	u32 release_advance;   // TBD: IEEE802.1Qbu: releaseAdvance [nsec]
	u8 preemption_active;  // IEEE802.1Qbu: preemptionActive, IEEE802.3br: aMACMergeStatusTx
	u8 hold_request;       // TBD: IEEE802.1Qbu: holdRequest
	int status_verify;     // IEEE802.3br: aMACMergeStatusVerify
};

struct lan9645x_port {
	struct lan9645x *lan9645x;
	const char *name;

	u16 pvid;
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
	u8 duplex;
	struct list_head path_delays;
	u32 rx_delay;

	struct net_device *bond; /* LAG upper device */
	enum netdev_lag_hash hash_type;
	bool lag_tx_active;

	struct net_device *bridge;
	int bridge_num;

	struct net_device *hsr; /* HSR/PRP upper device */

	struct mutex qos_lock; /* Port QOS config */
	struct lan9645x_port_qos qos;

	/* Time-Aware Shaper (TAS / taprio) */
	struct {
		struct tc_taprio_qopt_offload *taprio; /* Stored schedule */
		int list_base;    /* Pre-computed TAS list base index */
		int active_list;  /* Which list is OPERATING, or -1 */
		/* Per-list GCL tracking for deferred freeing. Entries are
		 * only freed when HW confirms the list is in ADMIN state.
		 */
		struct {
			int gcl_base;
			int gcl_count;
		} lists[2];
	} tas;

	/* Frame preemption */
	struct lan9645x_fp_port_conf fp;
	struct mutex fp_lock; /* Lock port FP config */

	/* PTP */
	struct sk_buff_head tx_skbs;
	struct sk_buff_head rx_skbs;
	u16 ts_id;
	u8 ptp_tx_cmd;
	bool ptp_rx_cmd;

	bool cut_thru_ena;

	bool pcs_lost_sync;

	struct mrp_port *mrp_port;
	int mrp_is1_p_port_rule_id;
};

struct lan9645x_path_delay {
	struct list_head list;
	u32 rx_delay;
	u32 tx_delay;
	u32 speed;
};


extern const struct phylink_pcs_ops lan9645x_phylink_pcs_ops;
extern const struct phylink_mac_ops lan9645x_phylink_mac_ops;

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
	struct dsa_port *dp;

	dp = dsa_to_port(lan9645x->ds, p->chip_port);
	if (dp && dp->type == DSA_PORT_TYPE_USER)
		return dp->user;

	return NULL;
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

	return p->bridge;
}

static inline bool lan9645x_port_is_used(struct lan9645x *lan9645x, int port)
{
	struct dsa_port *dp;

	dp = dsa_to_port(lan9645x->ds, port);
	if (!dp)
		return false;

	return dp->type != DSA_PORT_TYPE_UNUSED;
}

static inline bool lan9645x_port_is_hsr(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;

	return lan9645x->hsr.enabled && lan9645x->hsr.type == LAN9645X_HSR &&
		(lan9645x->hsr.port_a == p->chip_port ||
		 lan9645x->hsr.port_b == p->chip_port);
}

static inline struct lan9645x_port *
lan9645x_port_shadow_of(struct lan9645x_port *p)
{
	struct lan9645x_hsr_prp *hsr = &p->lan9645x->hsr;

	if (!hsr->enabled)
		return NULL;

	if (hsr->shadow_ports[0] == p->chip_port)
		return lan9645x_to_port(p->lan9645x, hsr->port_a);

	if (hsr->shadow_ports[1] == p->chip_port)
		return lan9645x_to_port(p->lan9645x, hsr->port_b);

	return NULL;
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
			    unsigned int neg_mode,
			    struct phylink_link_state *state);

/* lan9645x_main.c */
bool lan9645x_port_is_bridged(struct lan9645x_port *p);
void lan9645x_update_fwd_mask(struct lan9645x *lan9645x, bool joining);
void lan9645x_port_pgid_set(struct lan9645x *lan9645x, u16 pgid,
			    int chip_port, bool enabled);
void __lan9645x_pgid_mc_update(struct lan9645x *lan9645x);
void lan9645x_port_stp_state_set(struct lan9645x *lan9645x, int port, u8 state);

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
int lan9645x_mact_dsa_dump(struct lan9645x *lan9645x, int port,
			   dsa_fdb_dump_cb_t *cb, void *data);
int lan9645x_mact_entry_del(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid);
int lan9645x_mact_entry_add(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid);
void lan9645x_migrate_lag_fdb(struct lan9645x *lan9645x,
			      struct net_device *bond, int old_lag_id,
			      int new_lag_id);
int lan9645x_mac_bc_flood_add(struct lan9645x *lan9645x, u16 vid);
int lan9645x_mac_bc_flood_del(struct lan9645x *lan9645x, u16 vid);

/* VLAN lan9645x_vlan.c */
int lan9645x_vlan_init(struct lan9645x *lan9645x);
u16 lan9645x_vlan_unaware_pvid(int bridge_num);
void lan9645x_vlan_port_apply(struct lan9645x_port *p);
int lan9645x_vlan_port_add_vlan(struct lan9645x_port *p, u16 vid, bool pvid,
				bool untagged,
				struct netlink_ext_ack *extack);
int lan9645x_vlan_port_del_vlan(struct lan9645x_port *p, u16 vid);
int lan9645x_vlan_hw_wr(struct lan9645x *lan9645x, u16 vid);
int lan9645x_vlan_set_port_mask(struct lan9645x *lan9645x, u16 vid,
				u16 new_mask);
void lan9645x_vlan_set_hostmode(struct lan9645x_port *p);
void lan9645x_vlan_add_unaware_pvid(struct lan9645x_port *p);
void lan9645x_vlan_del_unaware_pvid(struct lan9645x_port *p);

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
			  int bridge_num);
int lan9645x_mdb_port_del(struct lan9645x *lan9645x, int port,
			  const struct switchdev_obj_port_mdb *mdb,
			  int bridge_num);
void lan9645x_mdb_init(struct lan9645x *lan9645x);
void lan9645x_mdb_deinit(struct lan9645x *lan9645x);
int lan9645x_mdb_port_mrouter_set(struct lan9645x *lan9645x, int port,
				  bool enable);

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
int lan9645x_hsr_prp_dan_node_add(struct lan9645x *lan9645x, int port,
				  struct net_device *hsr,
				  const unsigned char *smac);
void lan9645x_hsr_prp_dan_node_del(struct lan9645x *lan9645x, int port,
				   struct net_device *hsr,
				   const unsigned char *smac);

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
int lan9645x_eee_mac_set(struct lan9645x *lan9645x, int port,
			 struct ethtool_keee *e);

/* lan9645x_ptp.c */
int lan9645x_port_hwtstamp_get(struct dsa_switch *ds, int port,
                               struct kernel_hwtstamp_config *config);
int lan9645x_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config,
			       struct netlink_ext_ack *extack);
void lan9645x_txtstamp(struct dsa_switch *ds, int port, struct sk_buff *skb);
bool lan9645x_rxtstamp_defer(struct dsa_switch *ds, int port,
			     struct sk_buff *skb, unsigned int type);
bool lan9645x_rxtstamp_all_defer(struct dsa_switch *ds, int port,
				 struct sk_buff *skb, unsigned int type);
int lan9645x_get_ts_info(struct dsa_switch *ds, int port,
			 struct kernel_ethtool_ts_info *info);
int lan9645x_ptp_init(struct lan9645x *lan9645x);
void lan9645x_ptp_deinit(struct lan9645x *lan9645x);
irqreturn_t lan9645x_ptp_irq_handler(int irq, void *args);
irqreturn_t lan9645x_ptp_ext_irq_handler(int irq, void *args);
int lan9645x_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts);
u32 lan9645x_ptp_get_period_ps(void);
void lan9645x_ptp_improvements(struct lan9645x *lan9645x,
			       struct lan9645x_port *p,
			       phy_interface_t interface, int speed, int duplex);

/* lan9645x_ptp_logs.c */
void lan9645x_ptp_log_tx(struct lan9645x *lan9645x, struct sk_buff *skb,
			 struct timespec64 ts, u32 sub_ns);
void lan9645x_ptp_log_rx(struct lan9645x *lan9645x, struct sk_buff *skb,
			 struct timespec64 ts, u8 sub_ns);
int lan9645x_ptp_log_init(struct lan9645x *lan9645x);
void lan9645x_ptp_log_deinit(struct lan9645x *lan9645x);

/* lan9645x_taprio.c */
int lan9645x_taprio_add(struct lan9645x *lan9645x, int port,
			struct tc_taprio_qopt_offload *qopt);
int lan9645x_taprio_del(struct lan9645x *lan9645x, int port);
void lan9645x_taprio_init(struct lan9645x *lan9645x);
void lan9645x_taprio_deinit(struct lan9645x *lan9645x);
int lan9645x_taprio_speed_set(struct lan9645x_port *port, int speed);
void lan9645x_taprio_guard_bands_recalc(struct lan9645x_port *port);
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

int lan9645x_sfi_put(struct lan9645x *lan9645x, u32 sfi_ix);
int lan9645x_sgi_put(struct lan9645x *lan9645x, u32 sgi_ix);
int lan9645x_psfp_tc_action_set(struct lan9645x *lan9645x,
				struct lan9645x_psfp_sf_cfg *sf_cfg,
				struct lan9645x_psfp_sg_cfg *sg_cfg,
				struct netlink_ext_ack *extack,
				u32 *sfi_ix, u32 *sgi_ix);

/* lan9645x_fp.c */
int lan9645x_fp_status(struct lan9645x_port *p,
		       struct lan9645x_fp_port_status *s);
int lan9645x_fp_set(struct lan9645x_port *p,
		    struct lan9645x_fp_port_conf *c, bool link);
int lan9645x_fp_get(struct lan9645x_port *p,
		    struct lan9645x_fp_port_conf *c);
int lan9645x_fp_init(struct lan9645x *lan9645x);
void lan9645x_fp_link_change(struct lan9645x_port *p, bool link);
void lan9645x_fp_change_preemptable_tcs(struct lan9645x_port *p,
					unsigned long preemptible_tcs);
int lan9645x_fp_ethtool_get_mm(struct lan9645x *lan9645x, int port,
			       struct ethtool_mm_state *state);
int lan9645x_fp_ethtool_set_mm(struct lan9645x *lan9645x, int port,
			       struct ethtool_mm_cfg *cfg,
			       struct netlink_ext_ack *extack);

/* BUM policers lan9645x_bum_.c */
int lan9645x_bum_init(struct lan9645x *lan9645x);
void lan9645x_bum_deinit(struct lan9645x *lan9645x);

/* Manual frame injection lan9645x_manual_inj.c */
netdev_tx_t lan9645x_inj_xmit(struct lan9645x_port *port,
			      struct sk_buff *skb,
			      __be32 ifh[LAN9645X_IFH_LEN_U32]);

/* Automatic Frame Injection, lan9645x_afi.c */
int lan9645x_afi_init(struct lan9645x *lan9645x);
void lan9645x_afi_deinit(struct lan9645x *lan9645x);

/* PTP over HSR, lan9645x_ptp_hsr.c */
int lan9645x_ptp_hsr_init(struct lan9645x *lan9645x);
int lan9645x_ptp_hsr_setup(struct lan9645x *lan9645x, int port,
			   struct kernel_hwtstamp_config *cfg);
struct sk_buff *lan9645x_ptp_hsr_tx_irq_skb_match(struct lan9645x_port *port);
void lan9645x_ptp_hsr_flush_tx_skbs(struct lan9645x_port *port);
int lan9645x_port_xmit_redundancy_src(struct dsa_switch *ds, int port,
				      struct sk_buff *skb);
void lan9645x_port_set_rcv_redundancy_info(struct dsa_switch *ds, int port,
					   struct sk_buff *skb);

#endif /* __LAN9645X_MAIN_H__ */
