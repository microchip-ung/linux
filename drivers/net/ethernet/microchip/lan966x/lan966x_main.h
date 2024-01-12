/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __LAN966X_MAIN_H__
#define __LAN966X_MAIN_H__

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/ptp_clock_kernel.h>
#include <net/pkt_cls.h>
#include <net/pkt_sched.h>
#include <net/switchdev.h>

#include "lan966x_regs.h"
#include "lan966x_ifh.h"

#include "lan966x_afi.h"
#include <linux/debugfs.h>
#include "lan966x_qos.h"
#include <uapi/linux/mrp_bridge.h>

#include <vcap_api.h>
#include <vcap_api_client.h>

#define TABLE_UPDATE_SLEEP_US		10
#define TABLE_UPDATE_TIMEOUT_US		100000

#define READL_SLEEP_US			10
#define READL_TIMEOUT_US		100000000

#define LAN966X_BUFFER_CELL_SZ		64
#define LAN966X_BUFFER_MEMORY		(160 * 1024)
#define LAN966X_BUFFER_MIN_SZ		60

#define LAN966X_HW_MTU(mtu)		((mtu) + ETH_HLEN + ETH_FCS_LEN)

#define PGID_AGGR			64
#define PGID_SRC			80
#define PGID_ENTRIES			89

#define UNAWARE_PVID			0
#define HOST_PVID			4095

/* Reserved amount for (SRC, PRIO) at index 8*SRC + PRIO */
#define QSYS_Q_RSRV			95

#define NUM_PHYS_PORTS			8
#define CPU_PORT			8
#define NUM_PRIO_QUEUES			8

/* Reserved PGIDs */
#define PGID_CPU			(PGID_AGGR - 6)
#define PGID_UC				(PGID_AGGR - 5)
#define PGID_BC				(PGID_AGGR - 4)
#define PGID_MC				(PGID_AGGR - 3)
#define PGID_MCIPV4			(PGID_AGGR - 2)
#define PGID_MCIPV6			(PGID_AGGR - 1)

#define PGID_PMAC_START			(CPU_PORT + 1)
#define PGID_PMAC_END			(50)

/* Non-reserved PGIDs, used for general purpose */
#define PGID_GP_START			(PGID_PMAC_END + 1)
#define PGID_GP_END			PGID_CPU

#define LAN966X_SPEED_NONE		0
#define LAN966X_SPEED_2500		1
#define LAN966X_SPEED_1000		1
#define LAN966X_SPEED_100		2
#define LAN966X_SPEED_10		3

#define LAN966X_PHC_COUNT		3
#define LAN966X_PHC_PORT		0
#define LAN966X_PHC_PINS_NUM		7

#define IFH_REW_OP_NOOP			0x0
#define IFH_REW_OP_ONE_STEP_PTP		0x3
#define IFH_REW_OP_TWO_STEP_PTP		0x4

#define FDMA_RX_DCB_MAX_DBS		1
#define FDMA_TX_DCB_MAX_DBS		1
#define FDMA_DCB_INFO_DATAL(x)		((x) & GENMASK(15, 0))

#define FDMA_DCB_STATUS_BLOCKL(x)	((x) & GENMASK(15, 0))
#define FDMA_DCB_STATUS_SOF		BIT(16)
#define FDMA_DCB_STATUS_EOF		BIT(17)
#define FDMA_DCB_STATUS_INTR		BIT(18)
#define FDMA_DCB_STATUS_DONE		BIT(19)
#define FDMA_DCB_STATUS_BLOCKO(x)	(((x) << 20) & GENMASK(31, 20))
#define FDMA_DCB_INVALID_DATA		0x1

#define FDMA_XTR_CHANNEL		6
#define FDMA_INJ_CHANNEL		0
#define FDMA_DCB_MAX			512

#define SE_IDX_QUEUE			0  /* 0-79 : Queue scheduler elements */
#define SE_IDX_PORT			80 /* 80-89 : Port schedular elements */

#define LAN966X_VCAP_CID_IS1_L0 VCAP_CID_INGRESS_L0 /* IS1 lookup 0 */
#define LAN966X_VCAP_CID_IS1_L1 VCAP_CID_INGRESS_L1 /* IS1 lookup 1 */
#define LAN966X_VCAP_CID_IS1_L2 VCAP_CID_INGRESS_L2 /* IS1 lookup 2 */
#define LAN966X_VCAP_CID_IS1_MAX (VCAP_CID_INGRESS_L3 - 1) /* IS1 Max */

#define LAN966X_VCAP_CID_IS2_L0 VCAP_CID_INGRESS_STAGE2_L0 /* IS2 lookup 0 */
#define LAN966X_VCAP_CID_IS2_L1 VCAP_CID_INGRESS_STAGE2_L1 /* IS2 lookup 1 */
#define LAN966X_VCAP_CID_IS2_MAX (VCAP_CID_INGRESS_STAGE2_L2 - 1) /* IS2 Max */

#define LAN966X_VCAP_CID_ES0_L0 VCAP_CID_EGRESS_L0 /* ES0 lookup 0 */
#define LAN966X_VCAP_CID_ES0_MAX (VCAP_CID_EGRESS_L1 - 1) /* ES0 Max */

#define LAN966X_VLAN_SRC_CHK        0x01
#define LAN966X_VLAN_MIRROR         0x02
#define LAN966X_VLAN_LEARN_DISABLED 0x04
#define LAN966X_VLAN_PRIV_VLAN      0x08
#define LAN966X_VLAN_FLOOD_DIS      0x10
#define LAN966X_VLAN_SEC_FWD_ENA    0x20

#define PGID_MRP  (PGID_AGGR - 7)

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

/* Controls how PORT_MASK is applied */
enum LAN966X_PORT_MASK_MODE {
	LAN966X_PMM_NO_ACTION,
	LAN966X_PMM_REPLACE,
	LAN966X_PMM_FORWARDING,
	LAN966X_PMM_REDIRECT,
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

enum vcap_is1_port_sel_rt {
	VCAP_IS1_PS_RT_NORMAL,
	VCAP_IS1_PS_RT_7TUPLE,
	VCAP_IS1_PS_RT_DBL_VID,
	VCAP_IS1_PS_RT_DMAC_VID,
	VCAP_IS1_PS_RT_FOLLOW_OTHER = 7,
};

struct lan966x_port;

struct lan966x_db {
	u64 dataptr;
	u64 status;
};

struct lan966x_rx_dcb {
	u64 nextptr;
	u64 info;
	struct lan966x_db db[FDMA_RX_DCB_MAX_DBS];
};

struct lan966x_tx_dcb {
	u64 nextptr;
	u64 info;
	struct lan966x_db db[FDMA_TX_DCB_MAX_DBS];
};

struct lan966x_rx {
	struct lan966x *lan966x;

	/* Pointer to the array of hardware dcbs. */
	struct lan966x_rx_dcb *dcbs;

	/* Pointer to the last address in the dcbs. */
	struct lan966x_rx_dcb *last_entry;

	/* For each DB, there is a page */
	struct page *page[FDMA_DCB_MAX][FDMA_RX_DCB_MAX_DBS];

	/* Represents the db_index, it can have a value between 0 and
	 * FDMA_RX_DCB_MAX_DBS, once it reaches the value of FDMA_RX_DCB_MAX_DBS
	 * it means that the DCB can be reused.
	 */
	int db_index;

	/* Represents the index in the dcbs. It has a value between 0 and
	 * FDMA_DCB_MAX
	 */
	int dcb_index;

	/* Represents the dma address to the dcbs array */
	dma_addr_t dma;

	/* Represents the page order that is used to allocate the pages for the
	 * RX buffers. This value is calculated based on max MTU of the devices.
	 */
	u8 page_order;

	u8 channel_id;
};

struct lan966x_tx_dcb_buf {
	struct net_device *dev;
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	bool used;
	bool ptp;
};

struct lan966x_tx {
	struct lan966x *lan966x;

	/* Pointer to the dcb list */
	struct lan966x_tx_dcb *dcbs;
	u16 last_in_use;

	/* Represents the DMA address to the first entry of the dcb entries. */
	dma_addr_t dma;

	/* Array of dcbs that are given to the HW */
	struct lan966x_tx_dcb_buf *dcbs_buf;

	u8 channel_id;

	bool activated;
};

struct lan966x_stat_layout {
	u32 offset;
	char name[ETH_GSTRING_LEN];
};

struct lan966x_phc {
	struct ptp_clock *clock;
	struct ptp_clock_info info;
	struct ptp_pin_desc pins[LAN966X_PHC_PINS_NUM];
	struct hwtstamp_config hwtstamp_config;
	struct lan966x *lan966x;
	u8 index;
};

struct lan966x_skb_cb {
	u8 rew_op;
	u16 ts_id;
	unsigned long jiffies;
};

#define LAN966X_PTP_TIMEOUT		msecs_to_jiffies(10)
#define LAN966X_SKB_CB(skb) \
	((struct lan966x_skb_cb *)((skb)->cb))

struct lan966x_tc_policer {
	/* kilobit per second */
	u32 rate;
	/* bytes */
	u32 burst;
};

struct lan966x_path_delay {
	struct list_head list;
	u32 rx_delay;
	u32 tx_delay;
	u32 speed;
};

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

#define LAN966X_PMAC_VLAN_ENTRIES		4
#define LAN966X_PMAC_ENTRIES_PER_VLAN		2048

#define PMACACCESS_CMD_IDLE			0
#define PMACACCESS_CMD_READ			1
#define PMACACCESS_CMD_WRITE			2
#define PMACACCESS_CMD_INIT			3

struct lan966x_pmac_pgid_entry {
	refcount_t refcount;
	struct list_head list;
	int index;
	u16 ports;
};

struct lan966x_pmac_vlan_entry {
	refcount_t refcount;
	u16 vlan;
	u8 index;
	bool enabled;
};

struct lan966x_pmac_entry {
	struct lan966x_pmac_pgid_entry *pgid;
	struct lan966x_pmac_vlan_entry *vlan;
	struct list_head list;
	u16 index;
	u16 ports;
};

struct lan966x_pmac {
	/* a negative value means that nothing is set */
	int oui;

	struct list_head pgid_entries;
	struct list_head pmac_entries;
	struct lan966x_pmac_vlan_entry vlan_entries[LAN966X_PMAC_VLAN_ENTRIES];
};

struct lan966x {
	struct device *dev;

	u8 num_phys_ports;
	struct lan966x_port **ports;

	void __iomem *regs[NUM_TARGETS];

	int shared_queue_sz;

	u8 base_mac[ETH_ALEN];

	spinlock_t tx_lock; /* lock for frame transmition */

	struct net_device *bridge;
	u16 bridge_mask;
	u16 bridge_fwd_mask;

	struct list_head mac_entries;
	spinlock_t mac_lock; /* lock for mac_entries list */

	u16 vlan_mask[VLAN_N_VID];
	DECLARE_BITMAP(cpu_vlan_mask, VLAN_N_VID);
	u8 vlan_flags[VLAN_N_VID];

	/* stats */
	const struct lan966x_stat_layout *stats_layout;
	u32 num_stats;

	/* workqueue for reading stats */
	struct mutex stats_lock;
	u64 *stats;
	struct delayed_work stats_work;
	struct workqueue_struct *stats_queue;

	/* interrupts */
	int xtr_irq;
	int ana_irq;
	int ptp_irq;
	int fdma_irq;
	int ptp_ext_irq;

	/* worqueue for fdb */
	struct workqueue_struct *fdb_work;
	struct list_head fdb_entries;

	/* mdb */
	struct list_head mdb_entries;
	struct list_head pgid_entries;

	/* ptp */
	bool ptp;
	struct lan966x_phc phc[LAN966X_PHC_COUNT];
	spinlock_t ptp_clock_lock; /* lock for phc */
	spinlock_t ptp_ts_id_lock; /* lock for ts_id */
	struct mutex ptp_lock; /* lock for ptp interface state */
	u16 ptp_skbs;

	/* fdma */
	bool fdma;
	struct net_device *fdma_ndev;
	struct lan966x_rx rx;
	struct lan966x_tx tx;
	struct napi_struct napi;

	/* Mirror */
	struct lan966x_port *mirror_monitor;
	u32 mirror_mask[2];
	u32 mirror_count;

	struct lan966x_afi afi;

	/* VCAP api */
	struct vcap_control *vcap_ctrl;

	/* Common root for debugfs */
	struct dentry *debugfs_root;

	/* QoS configuration and state */
	struct lan966x_qos_conf qos;

	/* PSFP configuration and state */
	struct lan966x_psfp_conf psfp;

	/* FRER configuration and state */
	struct lan966x_frer_conf frer;

	/* PMAC configuration */
	struct lan966x_pmac pmac;

	struct list_head mrp_list;
	u8 loc_period_mask;

	struct hlist_head mep_list;
	struct hlist_head mip_list;
	/* IS1 rule ID for RAPS frames */
	int raps_is1_rule_id;
};

struct lan966x_port_config {
	phy_interface_t portmode;
	const unsigned long *advertising;
	int speed;
	int duplex;
	u32 pause;
	bool inband;
	bool autoneg;
};

#define LAN966X_VCAP_LOOKUP_MAX (3+2+1) /* IS1, IS2, ES0 */
struct lan966x_port_tc {
	bool ingress_shared_block;
	unsigned long police_id;
	unsigned long ingress_mirror_id;
	unsigned long egress_mirror_id;
	struct flow_stats police_stat;
	struct flow_stats mirror_stat;

	u16 flower_template_proto[LAN966X_VCAP_LOOKUP_MAX];
	/* list of flower templates for this port */
	struct list_head templates;
};

struct lan966x_port_mrp {
	u32 ring_test_flow;
	u32 in_test_flow;
	struct lan966x_mrp *mrp;

	enum br_mrp_port_role_type role;
	enum br_mrp_port_state_type state;

	bool ring_loc_interrupt;
	bool in_loc_interrupt;

	u32 ring_id;
	u32 in_id;
};

struct lan966x_port {
	struct net_device *dev;
	struct lan966x *lan966x;

	u8 chip_port;
	u16 pvid;
	u16 vid;
	bool vlan_aware;

	bool learn_ena;
	bool mcast_ena;

	struct phylink_config phylink_config;
	struct phylink_pcs phylink_pcs;
	struct lan966x_port_config config;
	struct phylink *phylink;
	struct phy *serdes;
	struct fwnode_handle *fwnode;

	u8 ptp_cmd;
	u16 ts_id;
	struct sk_buff_head tx_skbs;

	struct net_device *bond;
	bool lag_tx_active;
	enum netdev_lag_hash hash_type;

	struct lan966x_port_tc tc;

	struct mchp_qos_port_conf qos_port_conf;
	struct lan966x_fp_port_conf fp;

	struct list_head path_delays;
	u32 rx_delay;

	struct lan966x_port_mrp mrp;
	int mrp_is1_p_port_rule_id;
	int mrp_is1_s_port_rule_id;
	int mrp_is1_i_port_rule_id;

	/* IS1 rule ID for RAPS frames */
	int raps_is1_rule_id;
};

extern const struct phylink_mac_ops lan966x_phylink_mac_ops;
extern const struct phylink_pcs_ops lan966x_phylink_pcs_ops;
extern const struct ethtool_ops lan966x_ethtool_ops;
extern struct notifier_block lan966x_switchdev_nb __read_mostly;
extern struct notifier_block lan966x_switchdev_blocking_nb __read_mostly;

void lan966x_add_cnt(u64 *cnt, u32 val);

bool lan966x_netdevice_check(const struct net_device *dev);

void lan966x_register_notifier_blocks(void);
void lan966x_unregister_notifier_blocks(void);

bool lan966x_hw_offload(struct lan966x *lan966x, u32 port, struct sk_buff *skb);

void lan966x_ifh_get_src_port(void *ifh, u64 *src_port);
void lan966x_ifh_get_timestamp(void *ifh, u64 *timestamp);

void lan966x_stats_get(struct net_device *dev,
		       struct rtnl_link_stats64 *stats);
int lan966x_stats_init(struct lan966x *lan966x);

void lan966x_port_config_down(struct lan966x_port *port);
void lan966x_port_config_up(struct lan966x_port *port);
void lan966x_port_status_get(struct lan966x_port *port,
			     struct phylink_link_state *state);
int lan966x_port_pcs_set(struct lan966x_port *port,
			 struct lan966x_port_config *config);
void lan966x_port_init(struct lan966x_port *port);

int lan966x_mac_ip_learn(struct lan966x *lan966x,
			 bool cpu_copy,
			 const unsigned char mac[ETH_ALEN],
			 unsigned int vid,
			 enum macaccess_entry_type type);
int lan966x_mac_learn(struct lan966x *lan966x, int port,
		      const unsigned char mac[ETH_ALEN],
		      unsigned int vid,
		      enum macaccess_entry_type type);
int lan966x_mac_forget(struct lan966x *lan966x,
		       const unsigned char mac[ETH_ALEN],
		       unsigned int vid,
		       enum macaccess_entry_type type);
int lan966x_mac_cpu_learn(struct lan966x *lan966x, const char *addr, u16 vid);
int lan966x_mac_cpu_forget(struct lan966x *lan966x, const char *addr, u16 vid);
void lan966x_mac_init(struct lan966x *lan966x);
void lan966x_mac_set_ageing(struct lan966x *lan966x,
			    u32 ageing);
int lan966x_mac_del_entry(struct lan966x *lan966x,
			  const unsigned char *addr,
			  u16 vid);
int lan966x_mac_add_entry(struct lan966x *lan966x,
			  struct lan966x_port *port,
			  const unsigned char *addr,
			  u16 vid);
void lan966x_mac_lag_replace_port_entry(struct lan966x *lan966x,
					struct lan966x_port *src,
					struct lan966x_port *dst);
void lan966x_mac_lag_remove_port_entry(struct lan966x *lan966x,
				       struct lan966x_port *src);
void lan966x_mac_purge_entries(struct lan966x *lan966x);
irqreturn_t lan966x_mac_irq_handler(struct lan966x *lan966x);

void lan966x_vlan_init(struct lan966x *lan966x);
void lan966x_vlan_port_apply(struct lan966x_port *port);
bool lan966x_vlan_cpu_member_cpu_vlan_mask(struct lan966x *lan966x, u16 vid);
void lan966x_vlan_port_set_vlan_aware(struct lan966x_port *port,
				      bool vlan_aware);
int lan966x_vlan_port_set_vid(struct lan966x_port *port,
			      u16 vid,
			      bool pvid,
			      bool untagged);
void lan966x_vlan_port_add_vlan(struct lan966x_port *port,
				u16 vid,
				bool pvid,
				bool untagged);
void lan966x_vlan_port_del_vlan(struct lan966x_port *port, u16 vid);
void lan966x_vlan_cpu_add_vlan(struct lan966x *lan966x, u16 vid);
void lan966x_vlan_cpu_del_vlan(struct lan966x *lan966x, u16 vid);
void lan966x_vlan_set_mask(struct lan966x *lan966x, u16 vid);

void lan966x_fdb_write_entries(struct lan966x *lan966x, u16 vid);
void lan966x_fdb_erase_entries(struct lan966x *lan966x, u16 vid);
int lan966x_fdb_init(struct lan966x *lan966x);
void lan966x_fdb_deinit(struct lan966x *lan966x);
void lan966x_fdb_flush_workqueue(struct lan966x *lan966x);
int lan966x_handle_fdb(struct net_device *dev,
		       struct net_device *orig_dev,
		       unsigned long event, const void *ctx,
		       const struct switchdev_notifier_fdb_info *fdb_info);

void lan966x_mdb_init(struct lan966x *lan966x);
void lan966x_mdb_deinit(struct lan966x *lan966x);
int lan966x_handle_port_mdb_add(struct lan966x_port *port,
				const struct switchdev_obj *obj);
int lan966x_handle_port_mdb_del(struct lan966x_port *port,
				const struct switchdev_obj *obj);
void lan966x_mdb_erase_entries(struct lan966x *lan966x, u16 vid);
void lan966x_mdb_write_entries(struct lan966x *lan966x, u16 vid);
void lan966x_mdb_clear_entries(struct lan966x *lan966x);
void lan966x_mdb_restore_entries(struct lan966x *lan966x);

int lan966x_ptp_init(struct lan966x *lan966x);
void lan966x_ptp_deinit(struct lan966x *lan966x);
int lan966x_ptp_hwtstamp_set(struct lan966x_port *port, struct ifreq *ifr);
int lan966x_ptp_hwtstamp_get(struct lan966x_port *port, struct ifreq *ifr);
void lan966x_ptp_rxtstamp(struct lan966x *lan966x, struct sk_buff *skb,
			  u64 timestamp);
int lan966x_ptp_txtstamp_request(struct lan966x_port *port,
				 struct sk_buff *skb);
void lan966x_ptp_txtstamp_release(struct lan966x_port *port,
				  struct sk_buff *skb);
irqreturn_t lan966x_ptp_irq_handler(int irq, void *args);
irqreturn_t lan966x_ptp_ext_irq_handler(int irq, void *args);
u32 lan966x_ptp_get_period_ps(void);
int lan966x_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts);
int lan966x_ptp_setup_traps(struct lan966x_port *port, struct ifreq *ifr);
int lan966x_ptp_del_traps(struct lan966x_port *port);

int lan966x_fdma_xmit(struct sk_buff *skb, __be32 *ifh, struct net_device *dev);
int lan966x_fdma_change_mtu(struct lan966x *lan966x);
void lan966x_fdma_netdev_init(struct lan966x *lan966x, struct net_device *dev);
void lan966x_fdma_netdev_deinit(struct lan966x *lan966x, struct net_device *dev);
int lan966x_fdma_init(struct lan966x *lan966x);
void lan966x_fdma_deinit(struct lan966x *lan966x);
irqreturn_t lan966x_fdma_irq_handler(int irq, void *args);

int lan966x_lag_port_join(struct lan966x_port *port,
			  struct net_device *brport_dev,
			  struct net_device *bond,
			  struct netlink_ext_ack *extack);
void lan966x_lag_port_leave(struct lan966x_port *port, struct net_device *bond);
int lan966x_lag_port_prechangeupper(struct net_device *dev,
				    struct netdev_notifier_changeupper_info *info);
int lan966x_lag_port_changelowerstate(struct net_device *dev,
				      struct netdev_notifier_changelowerstate_info *info);
int lan966x_lag_netdev_prechangeupper(struct net_device *dev,
				      struct netdev_notifier_changeupper_info *info);
int lan966x_lag_netdev_changeupper(struct net_device *dev,
				   struct netdev_notifier_changeupper_info *info);
bool lan966x_lag_first_port(struct net_device *lag, struct net_device *dev);
u32 lan966x_lag_get_mask(struct lan966x *lan966x, struct net_device *bond);

int lan966x_port_changeupper(struct net_device *dev,
			     struct net_device *brport_dev,
			     struct netdev_notifier_changeupper_info *info);
int lan966x_port_prechangeupper(struct net_device *dev,
				struct net_device *brport_dev,
				struct netdev_notifier_changeupper_info *info);
void lan966x_port_stp_state_set(struct lan966x_port *port, u8 state);
void lan966x_port_ageing_set(struct lan966x_port *port,
			     unsigned long ageing_clock_t);
void lan966x_update_fwd_mask(struct lan966x *lan966x);

int lan966x_tc_setup(struct net_device *dev, enum tc_setup_type type,
		     void *type_data);

int lan966x_mqprio_add(struct lan966x_port *port, u8 num_tc);
int lan966x_mqprio_del(struct lan966x_port *port);

void lan966x_taprio_init(struct lan966x *lan966x);
void lan966x_taprio_deinit(struct lan966x *lan966x);
int lan966x_taprio_add(struct lan966x_port *port,
		       struct tc_taprio_qopt_offload *qopt);
int lan966x_taprio_del(struct lan966x_port *port);
int lan966x_taprio_speed_set(struct lan966x_port *port, int speed);

int lan966x_tbf_add(struct lan966x_port *port,
		    struct tc_tbf_qopt_offload *qopt);
int lan966x_tbf_del(struct lan966x_port *port,
		    struct tc_tbf_qopt_offload *qopt);

int lan966x_cbs_add(struct lan966x_port *port,
		    struct tc_cbs_qopt_offload *qopt);
int lan966x_cbs_del(struct lan966x_port *port,
		    struct tc_cbs_qopt_offload *qopt);

int lan966x_ets_add(struct lan966x_port *port,
		    struct tc_ets_qopt_offload *qopt);
int lan966x_ets_del(struct lan966x_port *port,
		    struct tc_ets_qopt_offload *qopt);

int lan966x_tc_matchall(struct lan966x_port *port,
			struct tc_cls_matchall_offload *f,
			bool ingress);

int lan966x_police_port_add(struct lan966x_port *port,
			    struct flow_action *action,
			    struct flow_action_entry *act,
			    unsigned long police_id,
			    bool ingress,
			    struct netlink_ext_ack *extack);
int lan966x_police_port_del(struct lan966x_port *port,
			    unsigned long police_id,
			    struct netlink_ext_ack *extack);
void lan966x_police_port_stats(struct lan966x_port *port,
			       struct flow_stats *stats);

int lan966x_mirror_port_add(struct lan966x_port *port,
			    struct flow_action_entry *action,
			    unsigned long mirror_id,
			    bool ingress,
			    struct netlink_ext_ack *extack);
int lan966x_mirror_port_del(struct lan966x_port *port,
			    bool ingress,
			    struct netlink_ext_ack *extack);
void lan966x_mirror_port_stats(struct lan966x_port *port,
			       struct flow_stats *stats,
			       bool ingress);

int lan966x_qos_init(struct lan966x *lan966x);
void lan966x_qos_port_init(struct lan966x_port *port);

int lan966x_tc_flower(struct lan966x_port *port,
		      struct flow_cls_offload *f,
		      bool ingress);

int lan966x_goto_port_add(struct lan966x_port *port,
			  int from_cid, int to_cid,
			  unsigned long goto_id,
			  struct netlink_ext_ack *extack);
int lan966x_goto_port_del(struct lan966x_port *port,
			  unsigned long goto_id,
			  struct netlink_ext_ack *extack);

int lan966x_police_del(struct lan966x_port *port, u16 pol_idx);
int lan966x_police_add(struct lan966x_port *port,
		       struct lan966x_tc_policer *pol,
		       u16 pol_idx);
int lan966x_mirror_vcap_add(const struct lan966x_port *port,
			    struct lan966x_port *monitor_port);
void lan966x_mirror_vcap_del(struct lan966x *lan966x);

int lan966x_netlink_fp_init(void);
void lan966x_netlink_fp_uninit(void);
int lan966x_netlink_frer_init(struct lan966x *lan966x);
void lan966x_netlink_frer_uninit(void);
int lan966x_netlink_qos_init(struct lan966x *lan966x);
void lan966x_netlink_qos_uninit(void);
int lan966x_netlink_pmac_init(struct lan966x *lan966x);
void lan966x_netlink_pmac_uninit(void);

netdev_tx_t lan966x_xmit(struct lan966x_port *port,
			 struct sk_buff *skb,
			 __be32 ifh[IFH_LEN]);

void lan966x_ifh_set_bypass(void *ifh, u64 bypass);
void lan966x_ifh_set_port(void *ifh, u64 bypass);
void lan966x_ifh_set_rew_op(void *ifh, u64 rew_op);
void lan966x_ifh_set_timestamp(void *ifh, u64 timestamp);
void lan966x_ifh_set_afi(void *ifh, u64 afi);
void lan966x_ifh_set_rew_oam(void *ifh, u64 rew_oam);
void lan966x_ifh_set_oam_type(void *ifh, u64 oam_type);
void lan966x_ifh_set_seq_num(void *ifh, u64 seq_num);

int lan966x_pmac_add(struct lan966x_port *port, u8 *mac, u16 vlan);
int lan966x_pmac_del(struct lan966x_port *port, u8 *mac, u16 vlan);
int lan966x_pmac_purge(struct lan966x *lan966x);
void lan966x_pmac_init(struct lan966x *lan966x);
void lan966x_pmac_deinit(struct lan966x *lan966x);

int lan966x_vcap_init(struct lan966x *lan966x);
void lan966x_vcap_deinit(struct lan966x *lan966x);
#if defined(CONFIG_DEBUG_FS)
int lan966x_vcap_port_info(struct net_device *dev,
			   struct vcap_admin *admin,
			   struct vcap_output_print *out);
#else
static inline int lan966x_vcap_port_info(struct net_device *dev,
					 struct vcap_admin *admin,
					 struct vcap_output_print *out)
{
	return 0;
}
#endif
int lan966x_vcap_get_port_keyset(struct net_device *ndev,
				 struct vcap_admin *admin, int cid,
				 u16 l3_proto,
				 struct vcap_keyset_list *keysetlist);
const char *lan966x_vcap_keyset_name(struct net_device *ndev,
				     enum vcap_keyfield_set keyset);
void lan966x_vcap_set_port_keyset(struct net_device *ndev,
				  struct vcap_admin *admin, int cid,
				  u16 l3_proto, enum vcap_keyfield_set keyset,
				  struct vcap_keyset_list *orig);

static inline void __iomem *lan_addr(void __iomem *base[],
				     int id, int tinst, int tcnt,
				     int gbase, int ginst,
				     int gcnt, int gwidth,
				     int raddr, int rinst,
				     int rcnt, int rwidth)
{
	WARN_ON((tinst) >= tcnt);
	WARN_ON((ginst) >= gcnt);
	WARN_ON((rinst) >= rcnt);
	return base[id + (tinst)] +
		gbase + ((ginst) * gwidth) +
		raddr + ((rinst) * rwidth);
}

static inline u32 lan_rd(struct lan966x *lan966x, int id, int tinst, int tcnt,
			 int gbase, int ginst, int gcnt, int gwidth,
			 int raddr, int rinst, int rcnt, int rwidth)
{
	return readl(lan_addr(lan966x->regs, id, tinst, tcnt, gbase, ginst,
			      gcnt, gwidth, raddr, rinst, rcnt, rwidth));
}

static inline void lan_wr(u32 val, struct lan966x *lan966x,
			  int id, int tinst, int tcnt,
			  int gbase, int ginst, int gcnt, int gwidth,
			  int raddr, int rinst, int rcnt, int rwidth)
{
	writel(val, lan_addr(lan966x->regs, id, tinst, tcnt,
			     gbase, ginst, gcnt, gwidth,
			     raddr, rinst, rcnt, rwidth));
}

static inline void lan_rmw(u32 val, u32 mask, struct lan966x *lan966x,
			   int id, int tinst, int tcnt,
			   int gbase, int ginst, int gcnt, int gwidth,
			   int raddr, int rinst, int rcnt, int rwidth)
{
	u32 nval;

	nval = readl(lan_addr(lan966x->regs, id, tinst, tcnt, gbase, ginst,
			      gcnt, gwidth, raddr, rinst, rcnt, rwidth));
	nval = (nval & ~mask) | (val & mask);
	writel(nval, lan_addr(lan966x->regs, id, tinst, tcnt, gbase, ginst,
			      gcnt, gwidth, raddr, rinst, rcnt, rwidth));
}

#endif /* __LAN966X_MAIN_H__ */
