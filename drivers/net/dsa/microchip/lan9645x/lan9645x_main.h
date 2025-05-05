/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef __LAN9645X_MAIN_H__
#define __LAN9645X_MAIN_H__

#include <linux/dsa/lan9645x.h>
#include <linux/regmap.h>
#include <net/dsa.h>

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

/* Reserved amount for (SRC, PRIO) at index 8*SRC + PRIO
 * See QSYS:RES_CTRL[*]:RES_CFG description
 */
#define QSYS_Q_RSRV			95

#define LAN9645X_ISDX_MAX 128
#define LAN9645X_ESDX_MAX 128
#define LAN9645X_SFID_MAX 256

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
#define VLAN_MAX			(HOST_PVID - 1)

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
	    pgid: 6, /* 0-63 GP pgds. */
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

	/* Forwarding Database */
	struct list_head mac_entries;
	struct mutex mact_lock; /* lock access to mact_table */
	struct mutex mac_entry_lock; /* lock for mac_entries list */
	struct net_device *bridge; /* Only support single bridge */
	u16 bridge_mask; /* Mask for bridged ports */
	u16 bridge_fwd_mask; /* Mask for forwarding bridged ports */
	struct mutex fwd_domain_lock; /* lock forwarding configuration */
	int ana_irq; /* mac table hw changes irq */

	/* VLAN */
	u16 vlan_mask[VLAN_N_VID]; /* Port mask per vlan */
	u8 vlan_flags[VLAN_N_VID];
	DECLARE_BITMAP(cpu_vlan_mask, VLAN_N_VID); /* CPU port VLAN membership */
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

#endif /* __LAN9645X_MAIN_H__ */
