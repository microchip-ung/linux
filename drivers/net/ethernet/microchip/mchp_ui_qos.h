/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

/* This file must be copied to the qos_utils/src folder before building the QOS tools */

#ifndef _QOS_H_
#define _QOS_H_

#define MCHP_QOS_NETLINK "mchp_qos_nl"

enum mchp_qos_attr {
	MCHP_QOS_ATTR_NONE,
	MCHP_QOS_ATTR_DEV,
	MCHP_QOS_ATTR_PORT_CFG,
	MCHP_QOS_ATTR_DSCP,
	MCHP_QOS_ATTR_DSCP_PRIO_DPL,

	/* This must be the last entry */
	MCHP_QOS_ATTR_END,
};

#define MCHP_QOS_ATTR_MAX (MCHP_QOS_ATTR_END - 1)

enum mchp_qos_genl {
	MCHP_QOS_GENL_PORT_CFG_SET,
	MCHP_QOS_GENL_PORT_CFG_GET,
	MCHP_QOS_GENL_DSCP_PRIO_DPL_SET,
	MCHP_QOS_GENL_DSCP_PRIO_DPL_GET,
};

/* QOS port configuration */
#define DEI_COUNT 2
#define DPL_COUNT 2
#define PCP_COUNT 8
#define PRIO_COUNT 8

struct mchp_qos_i_mode {
	bool tag_map_enable;
	bool dscp_map_enable;
};

enum mchp_qos_e_mode{
	MCHP_E_MODE_CLASSIFIED = 0, /* Use ingress classified PCP,DEI as TAG PCP,DEI */
	MCHP_E_MODE_DEFAULT    = 2, /* Use default PCP,DEI as TAG PCP,DEI */
	MCHP_E_MODE_MAPPED     = 3  /* Use mapped (SKB)Priority and DPL as TAG PCP,DEI */
};

struct mchp_pcp_dei_prio_dpl {
	u8 prio;
	u8 dpl;
};

struct mchp_prio_dpl_pcp_dei {
	u8 pcp;
	u8 dei;
};

struct mchp_qos_port_conf {
	u8 i_default_prio;
	u8 i_default_dpl;
	u8 i_default_pcp;
	u8 i_default_dei;
	struct mchp_pcp_dei_prio_dpl i_pcp_dei_prio_dpl_map[PCP_COUNT][DEI_COUNT];
	struct mchp_qos_i_mode i_mode;

	u8 e_default_pcp;
	u8 e_default_dei;
	struct mchp_prio_dpl_pcp_dei e_prio_dpl_pcp_dei_map[PRIO_COUNT][DPL_COUNT];
	enum mchp_qos_e_mode e_mode;

	bool dwrr_enable;
	u8 dwrr_count;
	u8 dwrr_queue_pct[PRIO_COUNT];

	u8 pfc_enable;
};

/* QOS DSCP configuration */
struct mchp_qos_dscp_prio_dpl {
	bool trust; /* Only trusted DSCP values are used for QOS class and DP level classification  */
	u8 prio;
	u8 dpl;
};



#define MCHP_FRER_NETLINK "mchp_frer_nl"

enum mchp_frer_attr {
	MCHP_FRER_ATTR_NONE,
	MCHP_FRER_ATTR_ID,
	MCHP_FRER_ATTR_DEV1,
	MCHP_FRER_ATTR_DEV2,
	MCHP_FRER_ATTR_STREAM_CFG,
	MCHP_FRER_ATTR_STREAM_CNT,
	MCHP_FRER_ATTR_IFLOW_CFG,
	MCHP_FRER_ATTR_VLAN_CFG,

	/* This must be the last entry */
	MCHP_FRER_ATTR_END,
};

#define MCHP_FRER_ATTR_MAX (MCHP_FRER_ATTR_END - 1)

enum mchp_frer_genl {
	MCHP_FRER_GENL_CS_CFG_SET,
	MCHP_FRER_GENL_CS_CFG_GET,
	MCHP_FRER_GENL_CS_CNT_GET,
	MCHP_FRER_GENL_CS_CNT_CLR,
	MCHP_FRER_GENL_MS_ALLOC,
	MCHP_FRER_GENL_MS_FREE,
	MCHP_FRER_GENL_MS_CFG_SET,
	MCHP_FRER_GENL_MS_CFG_GET,
	MCHP_FRER_GENL_MS_CNT_GET,
	MCHP_FRER_GENL_MS_CNT_CLR,
	MCHP_FRER_GENL_IFLOW_CFG_SET,
	MCHP_FRER_GENL_IFLOW_CFG_GET,
	MCHP_FRER_GENL_VLAN_CFG_SET,
	MCHP_FRER_GENL_VLAN_CFG_GET,
};

#define MCHP_FRER_MAX_PORTS     2 /* Max # of ports for split and mstreams */

/* FRER recovery counters */
struct mchp_frer_cnt {
	u64 out_of_order_packets; /* frerCpsSeqRcvyOutOfOrderPackets */
	u64 rogue_packets;        /* frerCpsSeqRcvyRoguePackets */
	u64 passed_packets;       /* frerCpsSeqRcvyPassedPackets */
	u64 discarded_packets;    /* frerCpsSeqRcvyDiscardedPackets */
	u64 lost_packets;         /* frerCpsSeqRcvyLostPackets */
	u64 tagless_packets;      /* frerCpsSeqRcvyTaglessPackets */
	u64 resets;               /* frerCpsSeqRcvyResets */
};

/* FRER recovery algorithm */
enum mchp_frer_rec_alg {
	MCHP_FRER_REC_ALG_VECTOR, /* Vector recovery */
	MCHP_FRER_REC_ALG_MATCH,  /* Match recovery */
};

/* FRER member/compound stream configuration */
struct mchp_frer_stream_cfg {
	bool                      enable;      /* Enable/disable recovery */
	enum mchp_frer_rec_alg alg;         /* frerSeqRcvyAlgorithm */
	u8                        hlen;        /* frerSeqRcvyHistoryLength */
	u16                       reset_time;  /* frerSeqRcvyResetMSec */
	bool                      take_no_seq; /* frerSeqRcvyTakeNoSequence */
	u16                       cs_id; /* Compound stream ID (mstream only) */
};

/* FRER ingress flow configuration */
struct mchp_frer_iflow_cfg {
	bool ms_enable;  /* Enable member stream */
	u16 ms_id;       /* Member stream base ID */
	bool generation; /* Enable/disable sequence generation */
	bool pop;        /* Pop R-tag */
	u8 split_mask;   /* Egress ports to split into */
};

/* Ingress flow configuration */
struct mchp_iflow_cfg {
	struct mchp_frer_iflow_cfg frer; /* FRER iflow configuration */
};

/* VLAN configuration */
struct mchp_frer_vlan_cfg {
	bool flood_disable; /* Disable flooding */
	bool learn_disable; /* Disable learning */
};


#define MCHP_PSFP_NETLINK	"mchp_psfp_nl"

enum mchp_psfp_attr {
	MCHP_PSFP_ATTR_NONE,
	MCHP_PSFP_SF_ATTR_CONF,
	MCHP_PSFP_SF_ATTR_STATUS,
	MCHP_PSFP_SF_ATTR_SFI,
	MCHP_PSFP_GCE_ATTR_CONF,
	MCHP_PSFP_GCE_ATTR_SGI,
	MCHP_PSFP_GCE_ATTR_GCI,
	MCHP_PSFP_SG_ATTR_CONF,
	MCHP_PSFP_SG_ATTR_STATUS,
	MCHP_PSFP_SG_ATTR_SGI,
	MCHP_PSFP_FM_ATTR_CONF,
	MCHP_PSFP_FM_ATTR_FMI,

	/* This must be the last entry */
	MCHP_PSFP_ATTR_END,
};

#define MCHP_PSFP_ATTR_MAX		MCHP_PSFP_ATTR_END - 1

enum mchp_psfp_genl {
	MCHP_PSFP_SF_GENL_CONF_SET,
	MCHP_PSFP_SF_GENL_CONF_GET,
	MCHP_PSFP_SF_GENL_STATUS_GET,
	MCHP_PSFP_GCE_GENL_CONF_SET,
	MCHP_PSFP_GCE_GENL_CONF_GET,
	MCHP_PSFP_GCE_GENL_STATUS_GET,
	MCHP_PSFP_SG_GENL_CONF_SET,
	MCHP_PSFP_SG_GENL_CONF_GET,
	MCHP_PSFP_SG_GENL_STATUS_GET,
	MCHP_PSFP_FM_GENL_CONF_SET,
	MCHP_PSFP_FM_GENL_CONF_GET,
};

/* PSFP Stream Filter configuration */
struct mchp_psfp_sf_conf {
	bool enable;     /* Enable filter */
	uint16_t max_sdu;     /* Maximum SDU size (zero disables SDU check) */
	bool block_oversize_enable; /* StreamBlockedDueToOversizeFrameEnable */
	bool block_oversize;        /* StreamBlockedDueToOversizeFrame */
};

/* PSFP Stream Filter counters */
struct mchp_psfp_sf_counters {
	uint64_t matching_frames_count;
	uint64_t passing_frames_count;
	uint64_t not_passing_frames_count;
	uint64_t passing_sdu_count;
	uint64_t not_passing_sdu_count;
	uint64_t red_frames_count;
};

/* PSFP Gate Control Entry configuration/status */
struct mchp_psfp_gce {
	bool gate_open;    /* StreamGateState */
	bool ipv_enable;   /* enable IPV */
	uint8_t ipv;            /* IPV */
	uint32_t time_interval; /* TimeInterval (nsec) */
	uint32_t octet_max;     /* IntervalOctetMax (zero disables check) */
};

/* PSFP Gate Control List configuration */
struct mchp_psfp_gcl_conf {
	int64_t base_time;  /* PSFPAdminBaseTime/PSFPOperBaseTime */
	uint32_t cycle_time;     /* PSFPAdminCycleTime/PSFPOperCycleTime */
	uint32_t cycle_time_ext; /* PSFPAdminCycleTimeExtension/
			       PSFPOperCycleTimeExtension */
	uint32_t gcl_length;     /* PSFPAdminControlListLength/
			       PSFPOperControlListLength */
};

/* PSFP Stream Gate configuration */
struct mchp_psfp_sg_conf{
	bool enable;               /* PSFPGateEnabled: Enable/disable Gate */
	bool gate_open;            /* PSFPAdminGateStates: Initial gate state */
	bool ipv_enable;           /* Enable PSFPAdminIPV */
	uint8_t ipv;                    /* PSFPAdminIPV */
	bool close_invalid_rx_enable; /* PSFPGateClosedDueToInvalidRxEnable */
	bool close_invalid_rx;        /* PSFPGateClosedDueToInvalidRx */
	bool close_octets_exceeded_enable; /* PSFPGateClosedDueToOctetsExceededEnable */
	bool close_octets_exceeded; /* PSFPGateClosedDueOctetsExceeded */
	bool config_change;         /* PSFPConfigChange: Apply config */
	struct mchp_psfp_gcl_conf admin; /* PSFPAdmin* */
};

/* PSFP Stream Gate status */
struct mchp_psfp_sg_status {
	bool gate_open;             /* PSFPOperGateStates */
	bool ipv_enable;            /* PSFPOperIPV enabled */
	uint8_t ipv;                /* PSFPOperIPV */
	int64_t config_change_time; /* PSFPConfigChangeTime */
	int64_t current_time;       /* PSFPCurrentTime */
	bool config_pending;        /* PSFPConfigPending */
	struct mchp_psfp_gcl_conf oper; /* PSFPOper* */
};

/* PSFP Flow Meter configuration */
struct mchp_psfp_fm_conf {
	bool enable; /* Enable flow meter */
	uint32_t cir; /* kbit/s */
	uint32_t cbs; /* octets */
	uint32_t eir; /* kbit/s */
	uint32_t ebs; /* octets */
	bool cf;
	bool drop_on_yellow;
	bool mark_red_enable;
	bool mark_red;
};


#define MCHP_FP_NETLINK "mchp_fp_nl"

enum mchp_qos_fp_port_attr {
	MCHP_QOS_FP_PORT_ATTR_NONE,
	MCHP_QOS_FP_PORT_ATTR_CONF,
	MCHP_QOS_FP_PORT_ATTR_STATUS,
	MCHP_QOS_FP_PORT_ATTR_IDX,

	/* This must be the last entry */
	MCHP_QOS_FP_PORT_ATTR_END,
};

#define MCHP_QOS_FP_PORT_ATTR_MAX		MCHP_QOS_FP_PORT_ATTR_END - 1

enum mchp_qos_fp_port_genl {
	MCHP_QOS_FP_PORT_GENL_CONF_SET,
	MCHP_QOS_FP_PORT_GENL_CONF_GET,
	MCHP_QOS_FP_PORT_GENL_STATUS_GET,
};

struct mchp_qos_fp_port_conf {
	uint8_t  admin_status;      // IEEE802.1Qbu: framePreemptionStatusTable
	uint8_t  enable_tx;         // IEEE802.3br: aMACMergeEnableTx
	uint8_t  verify_disable_tx; // IEEE802.3br: aMACMergeVerifyDisableTx
	uint8_t  verify_time;       // IEEE802.3br: aMACMergeVerifyTime [msec]
	uint8_t  add_frag_size;     // IEEE802.3br: aMACMergeAddFragSize
};

enum mchp_mm_status_verify {
	MCHP_MM_STATUS_VERIFY_INITIAL,   /**< INIT_VERIFICATION */
	MCHP_MM_STATUS_VERIFY_IDLE,      /**< VERIFICATION_IDLE */
	MCHP_MM_STATUS_VERIFY_SEND,      /**< SEND_VERIFY */
	MCHP_MM_STATUS_VERIFY_WAIT,      /**< WAIT_FOR_RESPONSE */
	MCHP_MM_STATUS_VERIFY_SUCCEEDED, /**< VERIFIED */
	MCHP_MM_STATUS_VERIFY_FAILED,    /**< VERIFY_FAIL */
	MCHP_MM_STATUS_VERIFY_DISABLED   /**< Verification process is disabled */
};

struct mchp_qos_fp_port_status {
	uint32_t hold_advance;      // TBD: IEEE802.1Qbu: holdAdvance [nsec]
	uint32_t release_advance;   // TBD: IEEE802.1Qbu: releaseAdvance [nsec]
	uint8_t preemption_active; // IEEE802.1Qbu: preemptionActive, IEEE802.3br: aMACMergeStatusTx
	uint8_t hold_request;      // TBD: IEEE802.1Qbu: holdRequest
	enum mchp_mm_status_verify status_verify;     // IEEE802.3br: aMACMergeStatusVerify
};

#endif /* _QOS_H_ */
