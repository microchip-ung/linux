/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#ifndef _LAN966X_MAIN_H_
#define _LAN966X_MAIN_H_

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/kthread.h>
#include <linux/net_tstamp.h>
#include <linux/phy.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/jiffies.h>
#include <linux/debugfs.h>
#include <linux/dmapool.h>
#include <linux/dmaengine.h>
#include <linux/spinlock.h>
#include <linux/phylink.h>
#include <net/switchdev.h>
#ifdef CONFIG_BRIDGE_MRP
#include <uapi/linux/mrp_bridge.h>
#endif

#include "lan966x_board.h"
#include "lan966x_afi.h"
#include "lan966x_tc.h"
#include "lan966x_qos.h"
#include "vcap_api_client.h"

#if defined(ASIC)
#include "lan966x_regs.h"
#elif defined(SUNRISE)
#include "lan966x_regs_sr.h"
#else
#include "lan966x_regs_ad.h"
#endif

#define IFH_LEN		7

#define LAN966X_BUFFER_CELL_SZ		64
#if defined(ASIC)
#define LAN966X_BUFFER_MEMORY		(160 * 1024)
#define LAN966X_BUFFER_REFERENCE        1280
#else
#define LAN966X_BUFFER_MEMORY		(32 * 1024)
#define LAN966X_BUFFER_REFERENCE        255
#endif
#define LAN966X_BUFFER_MIN_SZ		60

#define LAN966X_STATS_CHECK_DELAY	(2 * HZ)
#define LAN966X_MACT_PULL_DELAY		(2 * HZ)
#define LAN966X_PTP_QUEUE_SZ		128

#define PGID_AGGR    64
#define PGID_SRC     80
#define PGID_ENTRIES 89

#define PORT_PVID    4095

/* Reserved PGIDs */
#define PGID_MRP     (PGID_AGGR - 7)
#define PGID_CPU     (PGID_AGGR - 6)
#define PGID_UC      (PGID_AGGR - 5)
#define PGID_BC      (PGID_AGGR - 4)
#define PGID_MC      (PGID_AGGR - 3)
#define PGID_MCIPV4  (PGID_AGGR - 2)
#define PGID_MCIPV6  (PGID_AGGR - 1)

#define for_each_unicast_dest_pgid(lan966x, pgid)		\
	for ((pgid) = 0;					\
	     (pgid) < (lan966x)->num_phys_ports;		\
	     (pgid)++)

#define for_each_aggr_pgid(lan966x, pgid)			\
	for ((pgid) = PGID_AGGR;				\
	     (pgid) < PGID_SRC;					\
	     (pgid)++)

#define LAN966X_SPEED_2500 1
#define LAN966X_SPEED_1000 1
#define LAN966X_SPEED_100  2
#define LAN966X_SPEED_10   3

#define IFH_REW_OP_NOOP			0x0
#define IFH_REW_OP_RESIDENT_PTP		0x1
#define IFH_REW_OP_ONE_STEP_PTP		0x3
#define IFH_REW_OP_TWO_STEP_PTP		0x4
#define IFH_REW_OP_ORIGIN_TIMESTAMP_SEQ	0x7
#define IFH_REW_OP_PTP_AFI_NONE		0xC

#define OAM_TYPE_CCM 1
#define OAM_TYPE_TST 2
#define OAM_TYPE_ITST 3
#define OAM_TYPE_BCN 4
#define OAM_TYPE_ADV 5
#define OAM_VOE_CNT 10

#define CPU_PORT 8
#define LAN966X_NUM_PHYS_PORTS	8
#define LAN966X_MAX_PORTS 10

#define LAN966X_MACT_COLUMNS		4

#define MACACCESS_CMD_IDLE		0
#define MACACCESS_CMD_LEARN		1
#define MACACCESS_CMD_FORGET		2
#define MACACCESS_CMD_AGE		3
#define MACACCESS_CMD_GET_NEXT		4
#define MACACCESS_CMD_INIT		5
#define MACACCESS_CMD_READ		6
#define MACACCESS_CMD_WRITE		7
#define MACACCESS_CMD_SYNC_GET_NEXT	8

#define VLANACCESS_CMD_IDLE		0
#define VLANACCESS_CMD_READ		1
#define VLANACCESS_CMD_WRITE		2
#define VLANACCESS_CMD_INIT		3

#define XTR_EOF_0			0x00000080U
#define XTR_EOF_1			0x01000080U
#define XTR_EOF_2			0x02000080U
#define XTR_EOF_3			0x03000080U
#define XTR_PRUNED			0x04000080U
#define XTR_ABORT			0x05000080U
#define XTR_ESCAPE			0x06000080U
#define XTR_NOT_READY			0x07000080U
#define XTR_VALID_BYTES(x)		(4 - (((x) >> 24) & 3))

#define SGL_MAX				3
#define FDMA_TX_REQUEST_MAX		5
#define FDMA_RX_REQUEST_MAX		5
#define FDMA_XTR_BUFFER_COUNT		SGL_MAX
#define FDMA_XTR_BUFFER_SIZE		2048
#define FDMA_BUFFER_ALIGN		128

#define FDMA_RX_DCB_MAX_DBS		3
#define FDMA_TX_DCB_MAX_DBS		1
#define FDMA_DCB_INFO_DATAL(x)		((x) & GENMASK(15, 0))
#define FDMA_DCB_INFO_TOKEN		BIT(17)
#define FDMA_DCB_INFO_INTR		BIT(18)
#define FDMA_DCB_INFO_SW(x)		(((x) << 24) & GENMASK(31, 24))

#define FDMA_DCB_STATUS_BLOCKL(x)	((x) & GENMASK(15, 0))
#define FDMA_DCB_STATUS_SOF		BIT(16)
#define FDMA_DCB_STATUS_EOF		BIT(17)
#define FDMA_DCB_STATUS_INTR		BIT(18)
#define FDMA_DCB_STATUS_DONE		BIT(19)
#define FDMA_DCB_STATUS_BLOCKO(x)	(((x) << 20) & GENMASK(31, 20))
#define FDMA_DCB_INVALID_DATA		0x1

#define FDMA_BUFFER_ALIGN		128
#define FDMA_BUFFER_MASK		127
#define XTR_BUFFER_SIZE			(XTR_CHUNK_SIZE*12)
#define FDMA_XTR_CHANNEL		6
#define FDMA_INJ_CHANNEL		0
#define FDMA_DCB_MAX			512
#define FDMA_WEIGHT			64
#define VCORE_ACCESS_TIMEOUT_MS		5
#define FDMA_DISABLE_TIMEOUT_MS		5

#if defined(ASIC)
#define MULTIPLIER_BIT BIT(8)
#else
#define MULTIPLIER_BIT BIT(5)
#endif

enum lan966x_prof_t {
	LAN966X_PROFILE_MAC_IRQ,

	LAN966X_PROFILE_MAX,
};

struct lan966x_prof_stat {
	char *name;
	int count;
	u64 last;
	u64 min;
	u64 max;
	u64 *samples;
	u32 samples_size;
};

struct frame_info {
	u32 len;
	u16 port; /* Bit mask */
	u16 vid;
	u32 timestamp;
	u32 ptp_seq_idx;
	u32 rew_op;
	u8 qos_class;
	u8 ipv;
	bool afi;
	bool rew_oam;
	u8 oam_type;
};

struct lan966x_port;

struct lan966x_multicast {
	struct list_head list;
	unsigned char addr[ETH_ALEN];
	u16 vid;
	u16 ports;
};

struct lan966x_mact_entry {
	struct list_head list;
	unsigned char mac[ETH_ALEN];
	u16 vid;
	u16 port;
	int row;
	int bucket;
};

struct lan966x_mact_raw_entry {
	u32 mach;
	u32 macl;
	u32 maca;
	bool process;
};

struct lan966x_mact_event_work {
	struct work_struct work;
	struct net_device *dev;
	unsigned char mac[ETH_ALEN];
	enum switchdev_notifier_type type;
	u16 vid;
};

struct lan966x_path_delay {
	struct list_head list;
	u32 rx_delay;
	u32 tx_delay;
	u32 speed;
};

struct lan966x_db_hw {
	u64 dataptr;
	u64 status;
};

struct lan966x_rx_dcb_hw {
	u64 nextptr;
	u64 info;
	struct lan966x_db_hw db[FDMA_RX_DCB_MAX_DBS];
};

struct lan966x_tx_dcb_hw {
	u64 nextptr;
	u64 info;
	struct lan966x_db_hw db[FDMA_TX_DCB_MAX_DBS];
};

struct lan966x_rx {
	struct lan966x *lan966x;

	/* This port is used only for napi to be registeed and allocate skb.
	 * Don't use it for something else. It points to port 0.*/
	struct lan966x_port *port;

	/* Pointer to the array of hardward dcbs. */
	struct lan966x_rx_dcb_hw *dcb_entries;
	/* Pointer to the last addres in the dcb_entries */
	struct lan966x_rx_dcb_hw *last_entry;

	/* For each DB, there is a SKB, and the skb data pointer is mapped in
	 * the DB. Once a frame is received the skb is given to the upper layers
	 * and a new skb is added to the dcb.
	 */
	struct sk_buff *skb[FDMA_DCB_MAX][FDMA_RX_DCB_MAX_DBS];

	/* Represents the db_index, it can have a value between 0 and
	 * FDMA_RX_DCB_MAX_DBS, once it reaches the value of FDMA_RX_DCB_MAX_DBS
	 * it means that the DCB can be reused
	 */
	int db_index;

	/* Represetns the index in the dcb_entries. It has a value between 0 and
	 * FDMA_DCB_MAX
	 */
	int dcb_index;

	/* Represets the dma address to the dcb_entries array */
	dma_addr_t dma;

	/* Represents the page order that is used to allocate the pages for the
	 * RX buffers. This value is calculated based on max mtu of the
	 * devices that is currently set.
	 */
	u8 page_order;

	u32 channel_id;
};

struct lan966x_tx_dcb_buf {
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	bool used;
	bool ptp;
};

struct lan966x_tx {
	struct lan966x *lan966x;

	/* This port is used only for napi to be registeed and allocate skb.
	 * Don't use it for something else. It points to port 0.*/
	struct lan966x_port *port;

	/* Pointer to the dcb list */
	struct lan966x_tx_dcb_hw *dcbs;
	u16 last_in_use;

	/* Represets the dma address to the first entry of the dcb entries.
	 * It is used to update the next pointer of the DCBs
	 */
	dma_addr_t dma;

	/* Circular list of dcbs that are given to the HW */
	struct lan966x_tx_dcb_buf *dcbs_buf;

	u32 channel_id;

	bool activated;
};

#ifdef CONFIG_BRIDGE_MRP
struct lan966x_mrp {
	struct list_head list;

	struct lan966x *lan966x;
	struct lan966x_port *p_port;
	struct lan966x_port *s_port;
	struct lan966x_port *i_port;

	enum br_mrp_ring_role_type ring_role;
	enum br_mrp_ring_state_type ring_state;
	enum br_mrp_in_role_type in_role;
	enum br_mrp_in_state_type in_state;
	bool mra_support;
	bool monitor;
	u32 ring_id;
	u32 in_id;

	u32 ring_interval;
	u32 in_interval;

	u8 ring_loc_idx;
	u8 in_loc_idx;

	u32 ring_transitions;
	u32 in_transitions;

	struct delayed_work ring_loc_work;
	struct delayed_work in_loc_rc_work;

	u32 interval;
	u32 max_miss;
};
#endif
#ifdef CONFIG_BRIDGE_CFM
#define MEP_AFI_ID_NONE 0xFFFFFFFF
struct lan966x_mep {
	struct hlist_node head;
	u32 instance;
	u32 voe_idx;
	u32 afi_id;
	struct lan966x_port *port;
};

struct lan966x_mip {
	struct hlist_node head;
	u32 instance;
	struct lan966x_port *port;
};
#endif

#define LAN966X_PTP_DOMAINS		3
#define LAN966X_PTP_PORT_DOMAIN		0
struct lan966x_ptp_domain {
	struct ptp_clock *clock;
	struct ptp_clock_info info;
	struct lan966x *lan966x;
	u32 index;
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

struct lan966x_stat_layout {
	u32 offset;
	char name[ETH_GSTRING_LEN];
};

/* The following numbers are indexes into lan966x_stats_layout[] */
#define SYS_COUNT_RX_OCT		  0
#define SYS_COUNT_RX_UC			  1
#define SYS_COUNT_RX_MC			  2
#define SYS_COUNT_RX_BC			  3
#define SYS_COUNT_RX_SHORT		  4
#define SYS_COUNT_RX_FRAG		  5
#define SYS_COUNT_RX_JABBER		  6
#define SYS_COUNT_RX_CRC		  7
#define SYS_COUNT_RX_SYMBOL_ERR		  8
#define SYS_COUNT_RX_SZ_64		  9
#define SYS_COUNT_RX_SZ_65_127		 10
#define SYS_COUNT_RX_SZ_128_255		 11
#define SYS_COUNT_RX_SZ_256_511		 12
#define SYS_COUNT_RX_SZ_512_1023	 13
#define SYS_COUNT_RX_SZ_1024_1526	 14
#define SYS_COUNT_RX_SZ_JUMBO		 15
#define SYS_COUNT_RX_PAUSE		 16
#define SYS_COUNT_RX_CONTROL		 17
#define SYS_COUNT_RX_LONG		 18
#define SYS_COUNT_RX_CAT_DROP		 19
#define SYS_COUNT_RX_RED_PRIO_0		 20
#define SYS_COUNT_RX_RED_PRIO_1		 21
#define SYS_COUNT_RX_RED_PRIO_2		 22
#define SYS_COUNT_RX_RED_PRIO_3		 23
#define SYS_COUNT_RX_RED_PRIO_4		 24
#define SYS_COUNT_RX_RED_PRIO_5		 25
#define SYS_COUNT_RX_RED_PRIO_6		 26
#define SYS_COUNT_RX_RED_PRIO_7		 27
#define SYS_COUNT_RX_YELLOW_PRIO_0	 28
#define SYS_COUNT_RX_YELLOW_PRIO_1	 29
#define SYS_COUNT_RX_YELLOW_PRIO_2	 30
#define SYS_COUNT_RX_YELLOW_PRIO_3	 31
#define SYS_COUNT_RX_YELLOW_PRIO_4	 32
#define SYS_COUNT_RX_YELLOW_PRIO_5	 33
#define SYS_COUNT_RX_YELLOW_PRIO_6	 34
#define SYS_COUNT_RX_YELLOW_PRIO_7	 35
#define SYS_COUNT_RX_GREEN_PRIO_0	 36
#define SYS_COUNT_RX_GREEN_PRIO_1	 37
#define SYS_COUNT_RX_GREEN_PRIO_2	 38
#define SYS_COUNT_RX_GREEN_PRIO_3	 39
#define SYS_COUNT_RX_GREEN_PRIO_4	 40
#define SYS_COUNT_RX_GREEN_PRIO_5	 41
#define SYS_COUNT_RX_GREEN_PRIO_6	 42
#define SYS_COUNT_RX_GREEN_PRIO_7	 43
#define SYS_COUNT_RX_ASSEMBLY_ERR	 44
#define SYS_COUNT_RX_SMD_ERR		 45
#define SYS_COUNT_RX_ASSEMBLY_OK	 46
#define SYS_COUNT_RX_MERGE_FRAG		 47
#define SYS_COUNT_RX_PMAC_OCT		 48
#define SYS_COUNT_RX_PMAC_UC		 49
#define SYS_COUNT_RX_PMAC_MC		 50
#define SYS_COUNT_RX_PMAC_BC		 51
#define SYS_COUNT_RX_PMAC_SHORT		 52
#define SYS_COUNT_RX_PMAC_FRAG		 53
#define SYS_COUNT_RX_PMAC_JABBER	 54
#define SYS_COUNT_RX_PMAC_CRC		 55
#define SYS_COUNT_RX_PMAC_SYMBOL_ERR	 56
#define SYS_COUNT_RX_PMAC_SZ_64		 57
#define SYS_COUNT_RX_PMAC_SZ_65_127	 58
#define SYS_COUNT_RX_PMAC_SZ_128_255	 59
#define SYS_COUNT_RX_PMAC_SZ_256_511	 60
#define SYS_COUNT_RX_PMAC_SZ_512_1023	 61
#define SYS_COUNT_RX_PMAC_SZ_1024_1526	 62
#define SYS_COUNT_RX_PMAC_SZ_JUMBO	 63
#define SYS_COUNT_RX_PMAC_PAUSE		 64
#define SYS_COUNT_RX_PMAC_CONTROL	 65
#define SYS_COUNT_RX_PMAC_LONG		 66

#define SYS_COUNT_TX_OCT		 67
#define SYS_COUNT_TX_UC			 68
#define SYS_COUNT_TX_MC			 69
#define SYS_COUNT_TX_BC			 70
#define SYS_COUNT_TX_COL		 71
#define SYS_COUNT_TX_DROP		 72
#define SYS_COUNT_TX_PAUSE		 73
#define SYS_COUNT_TX_SZ_64		 74
#define SYS_COUNT_TX_SZ_65_127		 75
#define SYS_COUNT_TX_SZ_128_255		 76
#define SYS_COUNT_TX_SZ_256_511		 77
#define SYS_COUNT_TX_SZ_512_1023	 78
#define SYS_COUNT_TX_SZ_1024_1526	 79
#define SYS_COUNT_TX_SZ_JUMBO		 80
#define SYS_COUNT_TX_YELLOW_PRIO_0	 81
#define SYS_COUNT_TX_YELLOW_PRIO_1	 82
#define SYS_COUNT_TX_YELLOW_PRIO_2	 83
#define SYS_COUNT_TX_YELLOW_PRIO_3	 84
#define SYS_COUNT_TX_YELLOW_PRIO_4	 85
#define SYS_COUNT_TX_YELLOW_PRIO_5	 86
#define SYS_COUNT_TX_YELLOW_PRIO_6	 87
#define SYS_COUNT_TX_YELLOW_PRIO_7	 88
#define SYS_COUNT_TX_GREEN_PRIO_0	 89
#define SYS_COUNT_TX_GREEN_PRIO_1	 90
#define SYS_COUNT_TX_GREEN_PRIO_2	 91
#define SYS_COUNT_TX_GREEN_PRIO_3	 92
#define SYS_COUNT_TX_GREEN_PRIO_4	 93
#define SYS_COUNT_TX_GREEN_PRIO_5	 94
#define SYS_COUNT_TX_GREEN_PRIO_6	 95
#define SYS_COUNT_TX_GREEN_PRIO_7	 96
#define SYS_COUNT_TX_AGED		 97
#define SYS_COUNT_TX_LLCT		 98
#define SYS_COUNT_TX_CT			 99
#define SYS_COUNT_TX_MM_HOLD		100
#define SYS_COUNT_TX_MERGE_FRAG		101
#define SYS_COUNT_TX_PMAC_OCT		102
#define SYS_COUNT_TX_PMAC_UC		103
#define SYS_COUNT_TX_PMAC_MC		104
#define SYS_COUNT_TX_PMAC_BC		105
#define SYS_COUNT_TX_PMAC_PAUSE		106
#define SYS_COUNT_TX_PMAC_SZ_64		107
#define SYS_COUNT_TX_PMAC_SZ_65_127	108
#define SYS_COUNT_TX_PMAC_SZ_128_255	109
#define SYS_COUNT_TX_PMAC_SZ_256_511	110
#define SYS_COUNT_TX_PMAC_SZ_512_1023	111
#define SYS_COUNT_TX_PMAC_SZ_1024_1526	112
#define SYS_COUNT_TX_PMAC_SZ_JUMBO	113

#define SYS_COUNT_DR_LOCAL		114
#define SYS_COUNT_DR_TAIL		115
#define SYS_COUNT_DR_YELLOW_PRIO_0	116
#define SYS_COUNT_DR_YELLOW_PRIO_1	117
#define SYS_COUNT_DR_YELLOW_PRIO_2	118
#define SYS_COUNT_DR_YELLOW_PRIO_3	119
#define SYS_COUNT_DR_YELLOW_PRIO_4	120
#define SYS_COUNT_DR_YELLOW_PRIO_5	121
#define SYS_COUNT_DR_YELLOW_PRIO_6	122
#define SYS_COUNT_DR_YELLOW_PRIO_7	123
#define SYS_COUNT_DR_GREEN_PRIO_0	124
#define SYS_COUNT_DR_GREEN_PRIO_1	125
#define SYS_COUNT_DR_GREEN_PRIO_2	126
#define SYS_COUNT_DR_GREEN_PRIO_3	127
#define SYS_COUNT_DR_GREEN_PRIO_4	128
#define SYS_COUNT_DR_GREEN_PRIO_5	129
#define SYS_COUNT_DR_GREEN_PRIO_6	130
#define SYS_COUNT_DR_GREEN_PRIO_7	131

#define LAN966X_VLAN_SRC_CHK        0x01
#define LAN966X_VLAN_MIRROR         0x02
#define LAN966X_VLAN_LEARN_DISABLED 0x04
#define LAN966X_VLAN_PRIV_VLAN      0x08
#define LAN966X_VLAN_FLOOD_DIS      0x10
#define LAN966X_VLAN_SEC_FWD_ENA    0x20
/* VLAN_PGID_CPU_DIS is set via vlan_mask */

struct lan966x_data {
	u8 hw_offload;
	u8 internal_phy;
};

struct lan966x {
	struct device *dev;

	u8 num_phys_ports;
	struct lan966x_port **ports;

	void __iomem *regs[NUM_TARGETS];

	u8 base_mac[ETH_ALEN];

	struct net_device *hw_bridge_dev;
	u16 bridge_mask;
	u16 bridge_fwd_mask;

	u16 vlan_mask[VLAN_N_VID];
	u8 vlan_flags[VLAN_N_VID]; /* LAN966X_VLAN_XXXX */

	struct lan966x_afi afi;

	struct list_head multicast;

	int shared_queue_sz;

	/* stats */
	const struct lan966x_stat_layout *stats_layout;
	u32 num_stats;

	/* workqueue for reading stats */
	struct mutex stats_lock;
	u64 *stats;
	struct delayed_work stats_work;
	struct workqueue_struct *stats_queue;

	/* notifier blocks */
	struct notifier_block netdevice_nb;
	struct notifier_block switchdev_nb;
	struct notifier_block switchdev_blocking_nb;

	/* ptp */
	struct lan966x_ptp_domain ptp_domain[LAN966X_PTP_DOMAINS];
	struct hwtstamp_config hwtstamp_config;
	spinlock_t ptp_clock_lock;
	struct mutex ptp_lock;
	u8 ptp_poll : 1;
	u8 ptp_sync_poll : 1;

	/* SW MAC table */
	struct list_head mact_entries;
	spinlock_t mact_lock;
	struct delayed_work mact_work;
	struct workqueue_struct *mact_queue;

	struct task_struct *recv_task;

	/* interrupts */
	int ptp_irq;
	int ptp_sync_irq;
	int xtr_irq;
	int ana_irq;
	int fdma_irq;

#ifdef CONFIG_BRIDGE_MRP
	struct list_head mrp_list;
	/* It is used as a bit mask where a bit set represents that the loc
	 * period is used
	 */
	u8 loc_period_mask;

#endif
#ifdef CONFIG_BRIDGE_CFM
	struct hlist_head mep_list;
	struct hlist_head mip_list;

	/* IS1 rule ID for RAPS frames */
	int raps_is1_rule_id;
#endif
	/* Ana IRQ is used by multiple targets */
	u8 ana_poll : 1;

	/* Configution options for FDMA */
	u8 use_dma : 1;
	u8 use_napi : 1;
	u8 hw_offload : 1;
	const struct fdma_config *config;
	struct dma_chan *rxdma;
	struct dma_chan *txdma;
	struct dma_pool *rx_pool;
	struct list_head free_tx_reqs;
	struct list_head free_rx_reqs;
	struct list_head tx_reqs;
	struct list_head rx_reqs;
	u32 rx_req_fill_level;
	u32 tx_req_interval;
	spinlock_t tx_lock;

	/* QoS configuration and state */
	struct lan966x_qos_conf qos;

	/* PSFP configuration and state */
	struct lan966x_psfp_conf psfp;

	/* FRER configuration and state */
	struct lan966x_frer_conf frer;

	/* Mirror administration */
	struct lan966x_port *mirror_monitor; /* Monitor port for mirroring */
	u32 mirror_mask[2]; /* Mirror port mask, egress[0], ingress[1] */
	u32 mirror_count; /* Total count of egress, ingress and vcap mirroring */

	/* Common root for debugfs */
	struct dentry *debugfs_root;

	/* Profile */
	struct lan966x_prof_stat prof_stat[LAN966X_PROFILE_MAX];

	/* Count extern ports - number of ports that are not part of the HW
	 * switch
	 */
	int ext_port;

	struct lan966x_rx rx;
	struct lan966x_tx tx;
	struct napi_struct napi;

	/* Use internal phy */
	u8 internal_phy;

	/* VCAP api */
	struct vcap_control *vcap_ctrl;
};

#ifdef CONFIG_BRIDGE_MRP
struct lan966x_port_mrp {
	u32 ring_test_flow;
	u32 in_test_flow;
	struct lan966x_mrp *mrp;

	enum br_mrp_port_role_type role;
	enum br_mrp_port_state_type state;

	bool ring_loc_interrupt;
	bool in_loc_interrupt;
};
#endif

struct lan966x_port_config {
	phy_interface_t portmode;
	phy_interface_t phy_mode;
	const unsigned long *advertising;
	int speed;
	int duplex;
	u32 pause;
	bool inband;
	bool autoneg;
};

struct lan966x_port {
	struct net_device *dev;
	struct lan966x  *lan966x;

	u8 chip_port;

	struct phylink_config phylink_config;
	struct phylink_pcs phylink_pcs;
	struct lan966x_port_config config;
	struct phylink *phylink;
	struct phy *serdes;
	struct fwnode_handle *fwnode;

	void __iomem *regs;

	/* Ingress default VLAN (pvid) */
	u16 pvid;

	/* Egress default VLAN (vid) */
	u16 vid;

	u8 vlan_aware;

	/* TC administration */
	struct lan966x_port_tc tc;

	/* QOS port configuration */
	struct mchp_qos_port_conf qos_port_conf;

	/* Frame preemption configuration */
	struct lan966x_fp_port_conf fp;

	u8 ptp_cmd;
	u8 ptp_trans;
	struct sk_buff_head tx_skbs;
	u8 ts_id;

	/* The flag is set when promisc mode is set. And it is used when an
	 * interface is added/remove to/from a bridge. Because when an port
	 * is under a bridge it can't be in promisc mode.
	 */
	bool promisc_mode;

	bool mrouter_port;
	bool igmp_snooping_enabled;

	/* List of delays added to port, based on speed and interface */
	struct list_head path_delays;
	u32 rx_delay;

	/* Link aggregation */
	struct net_device *bond;
	bool lag_tx_active;

#ifdef CONFIG_BRIDGE_MRP
	struct lan966x_port_mrp mrp;

	/* IS1 rule ID for MRP frames */
	int mrp_is1_p_port_rule_id;
	int mrp_is1_s_port_rule_id;
	int mrp_is1_i_port_rule_id;
#endif
#ifdef CONFIG_BRIDGE_CFM
	/* IS1 rule ID for RAPS frames */
	int raps_is1_rule_id;
#endif
};

extern const struct phylink_mac_ops lan966x_phylink_mac_ops;
extern const struct phylink_pcs_ops lan966x_phylink_pcs_ops;

#ifdef CONFIG_DEBUG_KERNEL
#define lan_rd_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 readl(lan966x->regs[id + (tinst)] +		\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)))

#define lan_wr_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	(WARN_ON((tinst) >= tcnt),			\
	 WARN_ON((ginst) >= gcnt),			\
	 WARN_ON((rinst) >= rcnt),			\
	 writel(val, lan966x->regs[id + (tinst)] +	\
	        gbase + ((ginst) * gwidth) +		\
	        raddr + ((rinst) * rwidth)))

#define lan_rmw_(val, mask, lan966x, id, tinst, tcnt,	\
		 gbase, ginst, gcnt, gwidth,		\
		 raddr, rinst, rcnt, rwidth) do {	\
	u32 _v_;					\
	WARN_ON((tinst) >= tcnt);			\
	WARN_ON((ginst) >= gcnt);			\
	WARN_ON((rinst) >= rcnt);			\
	_v_ = readl(lan966x->regs[id + (tinst)] +	\
		    gbase + ((ginst) * gwidth) +	\
		    raddr + ((rinst) * rwidth));	\
	_v_ = ((_v_ & ~(mask)) | ((val) & (mask)));	\
	writel(_v_, lan966x->regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)); } while (0)
#else
#define lan_rd_(lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	readl(lan966x->regs[id + (tinst)] +		\
	      gbase + ((ginst) * gwidth) +		\
	      raddr + ((rinst) * rwidth))

#define lan_wr_(val, lan966x, id, tinst, tcnt,		\
		gbase, ginst, gcnt, gwidth,		\
		raddr, rinst, rcnt, rwidth)		\
	writel(val, lan966x->regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth))

#define lan_rmw_(val, mask, lan966x, id, tinst, tcnt,	\
		 gbase, ginst, gcnt, gwidth,		\
		 raddr, rinst, rcnt, rwidth) do {	\
	u32 _v_;					\
	_v_ = readl(lan966x->regs[id + (tinst)] +	\
		    gbase + ((ginst) * gwidth) +	\
		    raddr + ((rinst) * rwidth));	\
	_v_ = ((_v_ & ~(mask)) | ((val) & (mask)));	\
	writel(_v_, lan966x->regs[id + (tinst)] +	\
	       gbase + ((ginst) * gwidth) +		\
	       raddr + ((rinst) * rwidth)); } while (0)
#endif

#define lan_wr(...) lan_wr_(__VA_ARGS__)
#define lan_rd(...) lan_rd_(__VA_ARGS__)
#define lan_rmw(...) lan_rmw_(__VA_ARGS__)

/* Add a possibly wrapping 32 bit value to a 64 bit counter */
static inline void lan966x_add_cnt(u64 *cnt, u32 val)
{
	if (val < (*cnt & U32_MAX))
		*cnt += (u64)1 << 32; /* value has wrapped */

	*cnt = (*cnt & ~(u64)U32_MAX) + val;
}

/* lan966x_main.c */
void lan966x_hw_lock(struct lan966x *lan966x);
void lan966x_hw_unlock(struct lan966x *lan966x);

u32 lan966x_clk_period_ps(struct lan966x *lan966x);

void lan966x_update_stats(struct lan966x *lan966x);
bool lan966x_netdevice_check(const struct net_device *dev);

int lan966x_vlan_vid_add(struct net_device *dev, u16 vid, bool pvid,
			 bool untagged);
int lan966x_vlan_vid_del(struct net_device *dev, u16 vid);
void lan966x_vlan_port_apply(struct lan966x *lan966x,
			     struct lan966x_port *port);
int lan966x_vlant_set_mask(struct lan966x *lan966x, u16 vid);

int lan966x_mact_learn(struct lan966x *lan966x, int port,
		       const unsigned char mac[ETH_ALEN], unsigned int vid,
		       enum macaccess_entry_type type);
int lan966x_mact_forget(struct lan966x *lan966x,
			const unsigned char mac[ETH_ALEN], unsigned int vid,
			enum macaccess_entry_type type);
int lan966x_mc_sync(struct net_device *dev, const unsigned char *addr);
int lan966x_mc_unsync(struct net_device *dev, const unsigned char *addr);

int lan966x_del_mact_entry(struct lan966x *lan966x, const unsigned char *addr,
			    u16 vid);
int lan966x_add_mact_entry(struct lan966x *lan966x, struct lan966x_port *port,
			   const unsigned char *addr, u16 vid);

void lan966x_set_promisc(struct lan966x_port *port, bool enable,
			 bool change_master);

int lan966x_port_xmit_impl(struct sk_buff *skb, struct frame_info *info,
			   struct net_device *dev);
int lan966x_ptp_extts_handle(struct lan966x *lan966x, int irq);

void lan966x_port_config_down(struct lan966x_port *port);
void lan966x_port_config_up(struct lan966x_port *port);
void lan966x_port_status_get(struct lan966x_port *port,
			     struct phylink_link_state *state);
int lan966x_port_pcs_set(struct lan966x_port *port,
			 struct lan966x_port_config *config);
void lan966x_port_init(struct lan966x_port *port);

/* mirror functions */
int lan966x_mirror_port_add(const struct lan966x_port *port, bool ingress,
			    struct lan966x_port *monitor_port);
int lan966x_mirror_port_del(const struct lan966x_port *port, bool ingress);
int lan966x_mirror_vcap_add(const struct lan966x_port *port,
			    struct lan966x_port *monitor_port);
void lan966x_mirror_vcap_del(struct lan966x *lan966x);

/* profile functions */
void lan966x_prof_init_dbgfs(struct lan966x *lan966x);
void lan966x_prof_remove_dbgfs(struct lan966x *lan966x);
void lan966x_prof_sample_begin(struct lan966x_prof_stat *stat);
void lan966x_prof_sample_end(struct lan966x_prof_stat *stat);

/* lan966x_netlink_fp.c */
int lan966x_netlink_fp_init(void);
void lan966x_netlink_fp_uninit(void);

/* lan966x_netlink_psfp.c */
int lan966x_netlink_psfp_init(struct lan966x *lan966x);
void lan966x_netlink_psfp_uninit(void);

/* lan966x_netlink_frer.c */
int lan966x_netlink_frer_init(struct lan966x *lan966x);
void lan966x_netlink_frer_uninit(void);

/* lan966x_netlink_vcap.c */
int lan966x_netlink_vcap_init(struct lan966x *lan966x);
void lan966x_netlink_vcap_uninit(void);

/* lan966x_netlink_qos.c */
int lan966x_netlink_qos_init(struct lan966x *lan966x);
void lan966x_netlink_qos_uninit(void);

/* lan966x_switchdev.c */
int lan966x_register_notifier_blocks(struct lan966x *lan966x);
void lan966x_unregister_notifier_blocks(struct lan966x *lan966x);

/* lan966x_proc.c - debug purpose*/
extern unsigned long rx_counters;
void lan966x_proc_register_dbg(struct lan966x *lan966x);
void lan966x_proc_unregister_dbg(void);
void lan966x_debugfs_init(struct lan966x *lan966x);

#endif /* _LAN966X_MAIN_H_ */
