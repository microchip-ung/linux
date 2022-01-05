// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <linux/phy/phy.h>
#include <linux/ptp_classify.h>
#include <net/addrconf.h>
#include <net/rtnetlink.h>
#include <net/netevent.h>
#include <net/switchdev.h>
#include <linux/dma-direct.h>
#include <linux/reset.h>

#include "lan966x_main.h"
#include "lan966x_ethtool.h"
#include "lan966x_ifh.h"
#include "lan966x_ptp.h"
#include "lan966x_vcap_impl.h"

#ifdef CONFIG_BRIDGE_MRP
#include "lan966x_mrp.h"
#endif
#ifdef CONFIG_BRIDGE_CFM
#include "lan966x_cfm.h"
#endif

#define SGL_MAX				3
#define FDMA_TX_REQUEST_MAX		5
#define FDMA_RX_REQUEST_MAX		5
#define FDMA_XTR_BUFFER_COUNT		SGL_MAX
#define FDMA_BUFFER_ALIGN		128

#define READL_SLEEP_US		10
#define READL_TIMEOUT_US	100000000

#define LAN966X_PTP_RULE_ID_OFFSET 2048

struct lan966x_tx_request {
	struct list_head node;
	struct lan966x *lan966x;
	dma_cookie_t cookie;
	unsigned int size;
	unsigned int blocks;
	struct scatterlist sgl[SGL_MAX];
	void *buffer[SGL_MAX];
};

struct lan966x_rx_request {
	struct list_head node;
	struct lan966x *lan966x;
	dma_cookie_t cookie;
	int idx;
	struct scatterlist sgl[SGL_MAX];
	void *buffer[SGL_MAX];
	int fill_level;
};

struct request_iterator {
	int idx;
	struct lan966x_rx_request *req;
};

static irqreturn_t lan966x_ptp_irq_handler(int irq, void *args);
static void lan966x_ptp_2step_save(struct lan966x_port *port,
				   struct frame_info *ifh,
				   struct skb_shared_info *shinfo,
				   struct sk_buff *skb);
static bool lan966x_prepare_rx_request(struct lan966x *lan966x);
static int lan966x_fdma_xmit(struct sk_buff *skb, struct frame_info *info,
			     struct net_device *dev);
#if defined(SUNRISE) || defined(ASIC)
static int lan966x_napi_xmit(struct sk_buff *skb, struct frame_info *info,
			     struct net_device *dev);
static int lan966x_napi_rx_alloc(struct lan966x_rx *rx);
static int lan966x_napi_tx_alloc(struct lan966x_tx *tx);
static void lan966x_napi_rx_clear_dbs(struct lan966x_rx *rx);
static void lan966x_napi_rx_disable(struct lan966x_rx *rx);
static void lan966x_napi_tx_disable(struct lan966x_tx *tx);
static void lan966x_napi_rx_activate(struct lan966x_rx *rx);
static void lan966x_napi_rx_get_dcb(struct lan966x_rx *rx,
				    void *dcb_hw,
				    dma_addr_t *dma);
static void lan966x_napi_tx_get_dcb(struct lan966x_tx *rx,
				    void *dcb_hw,
				    void *dcb_buf,
				    dma_addr_t *dma);
static void lan966x_napi_reload(struct lan966x *lan966x, int new_mtu);
static int lan966x_napi_channel_active(struct lan966x *lan966x);
#endif
static void lan966x_close_tx_request(struct lan966x *lan966x,
				     struct lan966x_tx_request *req);

static const struct lan966x_data lan966x_data_hw = {
	.hw_offload = 1,
	.internal_phy = 0,
};

static const struct lan966x_data lan966x_data_nohw = {
	.hw_offload = 0,
	.internal_phy = 0,
};

static const struct lan966x_data lan966x_data_internal = {
	.hw_offload = 1,
	.internal_phy = 1,
};

static const struct of_device_id mchp_lan966x_match[] = {
	{ .compatible = "mchp,lan966x-switch", .data = &lan966x_data_hw },
	{ .compatible = "mchp,lan966x-switch-nohw", .data = &lan966x_data_nohw },
	{ .compatible = "mchp,lan966x-switch-internal", .data = &lan966x_data_internal },
	{ }
};
MODULE_DEVICE_TABLE(of, mchp_lan966x_match);

#define TABLE_UPDATE_SLEEP_US 10
#define TABLE_UPDATE_TIMEOUT_US 100000

void lan966x_hw_lock(struct lan966x *lan966x)
{
#if defined(SUNRISE) || defined(ASIC)
	while (!lan_rd(lan966x, ORG_SEMA(0, 0)))
		cond_resched();
#else
	while (!lan_rd(lan966x, ORG_SEMA(0)))
		cond_resched();
#endif
}

void lan966x_hw_unlock(struct lan966x *lan966x)
{
	/* Any value can be written to release */
#if defined(SUNRISE) || defined(ASIC)
	lan_wr(0x1, lan966x, ORG_SEMA(0, 0));
#else
	lan_wr(0x1, lan966x, ORG_SEMA(0));
#endif
}

/**
 * lan966x_mirror_port_add - Add port mirroring
 * @port: The interface to mirror.
 * @ingress: false if egress, true if ingress.
 * @monitor_port: Port where traffic is sent to.
 *
 * Add port mirroring for either egress or ingress to a monitor port.
 * The monitor port must be the same in all calls of this function and
 * lan966x_mirror_vcap_add(), and can only be set the first time or when all
 * egress, ingress and vcap mirroring has been deleted.
 * The number of egress, ingress and vcap mirroring is counted in mirror_count.
 *
 * Returns:
 * 0 if ok.
 * -EEXIST if mirroring is already added for this port and direction.
 * -EBUSY if monitor port is assigned to another port.
 * -EINVAL if trying to mirror the monitor port.
 */
int lan966x_mirror_port_add(const struct lan966x_port *port, bool ingress,
			    struct lan966x_port *monitor_port)
{
	struct lan966x *lan966x = port->lan966x;

	if (lan966x->mirror_mask[ingress] & BIT(port->chip_port))
		return -EEXIST;

	if (lan966x->mirror_monitor && (lan966x->mirror_monitor != monitor_port))
		return -EBUSY;

	if (port == monitor_port)
		return -EINVAL;

	lan966x->mirror_mask[ingress] |= BIT(port->chip_port);

	lan966x->mirror_monitor = monitor_port;
	lan_wr(BIT(monitor_port->chip_port), lan966x, ANA_MIRRORPORTS);

	if (ingress) {
		lan_rmw(ANA_PORT_CFG_SRC_MIRROR_ENA_SET(1),
			ANA_PORT_CFG_SRC_MIRROR_ENA,
			lan966x, ANA_PORT_CFG(port->chip_port));
	} else {
		lan_wr(lan966x->mirror_mask[0], lan966x, ANA_EMIRRORPORTS);
	}

	lan966x->mirror_count++;
	return 0;
}

/**
 * lan966x_mirror_port_del - Delete port mirroring
 * @port: The interface to mirror.
 * @ingress: false if egress, true if ingress.
 *
 * Delete port mirroring for either egress or ingress and decrement mirror_count.
 * Release monitor port if mirror_count becomes zero.
 *
 * Returns:
 * 0 if ok.
 * -ENOENT if mirroring is not added for this port and direction.
 */
int lan966x_mirror_port_del(const struct lan966x_port *port, bool ingress)
{
	struct lan966x *lan966x = port->lan966x;

	if (!(lan966x->mirror_mask[ingress] & BIT(port->chip_port)))
		return -ENOENT;

	lan966x->mirror_mask[ingress] &= ~BIT(port->chip_port);

	if (ingress) {
		lan_rmw(ANA_PORT_CFG_SRC_MIRROR_ENA_SET(0),
			ANA_PORT_CFG_SRC_MIRROR_ENA,
			lan966x, ANA_PORT_CFG(port->chip_port));
	} else {
		lan_wr(lan966x->mirror_mask[0], lan966x, ANA_EMIRRORPORTS);
	}

	if (lan966x->mirror_count == 0)
		dev_err(lan966x->dev,"ERROR: mirror_count is zero\n");
	else
		lan966x->mirror_count--;

	if (lan966x->mirror_count == 0) {
		lan966x->mirror_monitor = NULL;
		lan_wr(0, lan966x, ANA_MIRRORPORTS);
	}
	return 0;
}

/**
 * lan966x_mirror_vcap_add - Add VCAP mirroring
 * @port: The interface to mirror.
 * @lan966x: The interface.
 * @monitor_port: Port where traffic is sent to.
 *
 * The monitor port must be the same in all calls of this function and
 * lan966x_mirror_port_add(), and can only be set the first time or when all
 * egress, ingress and vcap mirroring has been deleted.
 * The number of egress, ingress and vcap mirroring is counted in mirror_count.
 *
 * Returns:
 * 0 if ok.
 * -EBUSY if monitor port is assigned to another port.
 * -EINVAL if trying to mirror the monitor port.
 */
int lan966x_mirror_vcap_add(const struct lan966x_port *port,
			    struct lan966x_port *monitor_port)
{
	struct lan966x *lan966x = port->lan966x;

	if (lan966x->mirror_monitor && (lan966x->mirror_monitor != monitor_port))
		return -EBUSY;

	if (port == monitor_port)
		return -EINVAL;

	lan966x->mirror_monitor = monitor_port;
	lan_wr(BIT(monitor_port->chip_port), lan966x, ANA_MIRRORPORTS);

	lan966x->mirror_count++;
	return 0;
}

/**
 * lan966x_mirror_vcap_del - Delete VCAP mirroring
 * @lan966x: The interface.
 *
 * Decrement mirror_count and release monitor port if mirror_count becomes zero.
 */
void lan966x_mirror_vcap_del(struct lan966x *lan966x)
{

	if (lan966x->mirror_count == 0)
		dev_err(lan966x->dev,"ERROR: mirror_count is zero\n");
	else
		lan966x->mirror_count--;

	if (lan966x->mirror_count == 0) {
		lan966x->mirror_monitor = NULL;
		lan_wr(0, lan966x, ANA_MIRRORPORTS);
	}
}

static inline int lan966x_mact_get_status(struct lan966x *lan966x)
{
	return lan_rd(lan966x, ANA_MACACCESS);
}

static inline int lan966x_mact_wait_for_completion(struct lan966x *lan966x)
{
	u32 val;

	return readx_poll_timeout(lan966x_mact_get_status,
		lan966x, val,
		(ANA_MACACCESS_MAC_TABLE_CMD_GET(val)) ==
		MACACCESS_CMD_IDLE,
		TABLE_UPDATE_SLEEP_US, TABLE_UPDATE_TIMEOUT_US);
}

static inline int lan966x_vlant_get_status(struct lan966x *lan966x)
{
	return lan_rd(lan966x, ANA_VLANACCESS);
}

static inline int lan966x_vlant_wait_for_completion(struct lan966x *lan966x)
{
	u32 val;

	return readx_poll_timeout(lan966x_vlant_get_status,
		lan966x, val,
		(ANA_VLANACCESS_VLAN_TBL_CMD_GET(val)) ==
		VLANACCESS_CMD_IDLE,
		TABLE_UPDATE_SLEEP_US, TABLE_UPDATE_TIMEOUT_US);
}

static void lan966x_mact_select(struct lan966x *lan966x,
				const unsigned char mac[ETH_ALEN],
				unsigned int vid)
{
	u32 macl = 0, mach = 0;

	/* Set the MAC address to handle and the vlan associated in a format
	 * understood by the hardware.
	 */
	mach |= vid    << 16;
	mach |= mac[0] << 8;
	mach |= mac[1] << 0;
	macl |= mac[2] << 24;
	macl |= mac[3] << 16;
	macl |= mac[4] << 8;
	macl |= mac[5] << 0;

	lan_wr(macl, lan966x, ANA_MACLDATA);
	lan_wr(mach, lan966x, ANA_MACHDATA);
}

int lan966x_mact_learn(struct lan966x *lan966x, int port,
		       const unsigned char mac[ETH_ALEN], unsigned int vid,
		       enum macaccess_entry_type type)
{
	int ret = 0;

	lan966x_hw_lock(lan966x);

	lan966x_mact_select(lan966x, mac, vid);

	/* Issue a write command */
	lan_wr(ANA_MACACCESS_VALID_SET(1) |
	       ANA_MACACCESS_CHANGE2SW_SET(0) |
	       ANA_MACACCESS_DEST_IDX_SET(port) |
	       ANA_MACACCESS_ENTRYTYPE_SET(type) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(MACACCESS_CMD_LEARN),
	       lan966x, ANA_MACACCESS);

	ret = lan966x_mact_wait_for_completion(lan966x);

	lan966x_hw_unlock(lan966x);

	return ret;
}

static int lan966x_mact_lookup(struct lan966x *lan966x,
			       const unsigned char mac[ETH_ALEN],
			       unsigned int vid, enum macaccess_entry_type type)
{
	int ret;

	lan966x_hw_lock(lan966x);

	lan966x_mact_select(lan966x, mac, vid);

	/* Issue a read command */
	lan_wr(ANA_MACACCESS_ENTRYTYPE_SET(type) |
	       ANA_MACACCESS_VALID_SET(1) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(MACACCESS_CMD_READ),
	       lan966x, ANA_MACACCESS);

	ret = lan966x_mact_wait_for_completion(lan966x);
	if (ret)
		goto out;

	ret = ANA_MACACCESS_VALID_GET(lan_rd(lan966x, ANA_MACACCESS));

out:
	lan966x_hw_unlock(lan966x);

	return ret;
}

int lan966x_mact_forget(struct lan966x *lan966x,
			const unsigned char mac[ETH_ALEN], unsigned int vid,
			enum macaccess_entry_type type)
{
	int ret;

	lan966x_hw_lock(lan966x);

	lan966x_mact_select(lan966x, mac, vid);

	/* Issue a forget command */
	lan_wr(ANA_MACACCESS_ENTRYTYPE_SET(type) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(MACACCESS_CMD_FORGET),
	       lan966x, ANA_MACACCESS);

	ret = lan966x_mact_wait_for_completion(lan966x);

	lan966x_hw_unlock(lan966x);

	return ret;
}

int lan966x_vlant_set_mask(struct lan966x *lan966x, u16 vid)
{
	u8 flags = lan966x->vlan_flags[vid];
	u16 mask = lan966x->vlan_mask[vid];
	int ret;

	lan966x_hw_lock(lan966x);

	/* Set flags and the VID to configure */
	lan_wr(ANA_VLANTIDX_VLAN_PGID_CPU_DIS_SET(!(mask & BIT(CPU_PORT))) |
	       ANA_VLANTIDX_VLAN_SEC_FWD_ENA_SET(!!(flags & LAN966X_VLAN_SEC_FWD_ENA)) |
	       ANA_VLANTIDX_VLAN_FLOOD_DIS_SET(!!(flags & LAN966X_VLAN_FLOOD_DIS)) |
	       ANA_VLANTIDX_VLAN_PRIV_VLAN_SET(!!(flags & LAN966X_VLAN_PRIV_VLAN)) |
	       ANA_VLANTIDX_VLAN_LEARN_DISABLED_SET(!!(flags & LAN966X_VLAN_LEARN_DISABLED)) |
	       ANA_VLANTIDX_VLAN_MIRROR_SET(!!(flags & LAN966X_VLAN_MIRROR)) |
	       ANA_VLANTIDX_VLAN_SRC_CHK_SET(!!(flags & LAN966X_VLAN_SRC_CHK)) |
	       ANA_VLANTIDX_V_INDEX_SET(vid),
	       lan966x, ANA_VLANTIDX);

	/* Set the vlan port members mask */
	lan_wr(ANA_VLAN_PORT_MASK_VLAN_PORT_MASK_SET(mask),
	       lan966x, ANA_VLAN_PORT_MASK);

	/* Issue a write command */
	lan_wr(VLANACCESS_CMD_WRITE, lan966x, ANA_VLANACCESS);
	ret = lan966x_vlant_wait_for_completion(lan966x);

	lan966x_hw_unlock(lan966x);

	return ret;
}

static void lan966x_mact_init(struct lan966x *lan966x)
{
	lan966x_hw_lock(lan966x);

	/* Clear the MAC table */
	lan_wr(MACACCESS_CMD_INIT, lan966x, ANA_MACACCESS);
	lan966x_mact_wait_for_completion(lan966x);

	lan966x_hw_unlock(lan966x);
}

static void lan966x_vlan_init(struct lan966x *lan966x)
{
	u16 port, vid;

	/* Clear VLAN table, by default all ports are members of all VLANS */
	lan_wr(VLANACCESS_CMD_INIT, lan966x, ANA_VLANACCESS);
	lan966x_vlant_wait_for_completion(lan966x);

	for (vid = 1; vid < VLAN_N_VID; vid++) {
		lan966x->vlan_mask[vid] = 0;
		lan966x_vlant_set_mask(lan966x, vid);
	}

	lan966x->vlan_mask[PORT_PVID] = GENMASK(lan966x->num_phys_ports - 1, 0) |
					BIT(CPU_PORT);
	lan966x_vlant_set_mask(lan966x, PORT_PVID);

	/* Because VLAN filtering is enabled, we need VID 0 to get untagged
	 * traffic. It is added automatically if 8021q module is loaded,
	 * but we can't rely on it since module may be not loaded.
	 */
	lan966x->vlan_mask[0] = GENMASK(lan966x->num_phys_ports - 1, 0);
	lan966x_vlant_set_mask(lan966x, 0);

	/* Configure the CPU port to be vlan aware */
	lan_wr(ANA_VLAN_CFG_VLAN_VID_SET(0) |
	       ANA_VLAN_CFG_VLAN_AWARE_ENA_SET(1) |
	       ANA_VLAN_CFG_VLAN_POP_CNT_SET(1),
	       lan966x, ANA_VLAN_CFG(CPU_PORT));

	/* Set vlan ingress filter mask to all ports */
	lan_wr(GENMASK(lan966x->num_phys_ports, 0),
	       lan966x, ANA_VLANMASK);

	for (port = 0; port < lan966x->num_phys_ports; port++) {
		lan_wr(0, lan966x, REW_PORT_VLAN_CFG(port));
		lan_wr(0, lan966x, REW_TAG_CFG(port));
	}
}

void lan966x_vlan_port_apply(struct lan966x *lan966x,
			     struct lan966x_port *port)
{
	u32 val;

	/* Ingress clasification (ANA_PORT_VLAN_CFG) */
	/* Default vlan to casify for untagged frames (may be zero) */
	val = ANA_VLAN_CFG_VLAN_VID_SET(port->pvid);
	if (port->vlan_aware)
		val |= ANA_VLAN_CFG_VLAN_AWARE_ENA_SET(1) |
		       ANA_VLAN_CFG_VLAN_POP_CNT_SET(1);

	lan_rmw(DEV_MAC_TAGS_CFG_VLAN_AWR_ENA_SET(port->vlan_aware) |
		DEV_MAC_TAGS_CFG_VLAN_DBL_AWR_ENA_SET(port->vlan_aware),
		DEV_MAC_TAGS_CFG_VLAN_AWR_ENA |
		DEV_MAC_TAGS_CFG_VLAN_DBL_AWR_ENA,
		lan966x, DEV_MAC_TAGS_CFG(port->chip_port));

	lan_rmw(val,
		ANA_VLAN_CFG_VLAN_VID |
		ANA_VLAN_CFG_VLAN_AWARE_ENA |
		ANA_VLAN_CFG_VLAN_POP_CNT,
		lan966x, ANA_VLAN_CFG(port->chip_port));

	/* Drop frames with multicast source address */
	val = ANA_DROP_CFG_DROP_MC_SMAC_ENA_SET(1);
	if (port->vlan_aware && !port->pvid)
		/* If port is vlan-aware and tagged, drop untagged and priority
		 * tagged frames.
		 */
		val |= ANA_DROP_CFG_DROP_UNTAGGED_ENA_SET(1) |
		       ANA_DROP_CFG_DROP_PRIO_S_TAGGED_ENA_SET(1) |
		       ANA_DROP_CFG_DROP_PRIO_C_TAGGED_ENA_SET(1);

	lan_wr(val, lan966x, ANA_DROP_CFG(port->chip_port));

	/* Egress configuration (REW_TAG_CFG): VLAN tag type to 8021Q */
	val = REW_TAG_CFG_TAG_TPID_CFG_SET(0);
	if (port->vlan_aware) {
		if (port->vid)
			/* Tag all frames except when VID == DEFAULT_VLAN */
			val |= REW_TAG_CFG_TAG_CFG_SET(1);
		else
			val |= REW_TAG_CFG_TAG_CFG_SET(3);
	}

	/* Update only some bits in the register */
	lan_wr((lan_rd(lan966x, REW_TAG_CFG(port->chip_port)) &
	       ~(REW_TAG_CFG_TAG_TPID_CFG | REW_TAG_CFG_TAG_CFG)) |
	       val,
	       lan966x, REW_TAG_CFG(port->chip_port));

	/* Set default VLAN and tag type to 8021Q */
	val = REW_PORT_VLAN_CFG_PORT_TPID_SET(ETH_P_8021Q) |
	      REW_PORT_VLAN_CFG_PORT_VID_SET(port->vid);
	lan_rmw(val,
		REW_PORT_VLAN_CFG_PORT_TPID |
		REW_PORT_VLAN_CFG_PORT_VID,
		lan966x, REW_PORT_VLAN_CFG(port->chip_port));
}

static void lan966x_qos_port_apply(struct lan966x *lan966x,
				   struct lan966x_port *port)
{
	int pcp, dei, cos, dpl;
	u8 tag_cfg;

	/* Setup ingress 1:1 mapping between tag [PCP,DEI] and [PRIO,DPL].
	 * PCP determines the priority (0..7) of the frame and
	 * DEI determines the color (green og yellow) of the frame. */
	for (pcp = 0; pcp < 8; pcp++) {
		for (dei = 0; dei < 2; dei++) {
			lan_wr(ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL_SET(dei) |
			       ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL_SET(pcp),
			       lan966x,
			       ANA_PCP_DEI_CFG(port->chip_port, 8 * dei + pcp));
			port->qos_port_conf.i_pcp_dei_prio_dpl_map[pcp][dei].prio = pcp;
			port->qos_port_conf.i_pcp_dei_prio_dpl_map[pcp][dei].dpl = dei;
		}
	}

	port->qos_port_conf.i_default_prio = 0;
	port->qos_port_conf.i_default_dpl = 0;
	port->qos_port_conf.i_mode.tag_map_enable = false;
	port->qos_port_conf.i_mode.dscp_map_enable = false;
	port->qos_port_conf.i_default_pcp = 0;
	port->qos_port_conf.i_default_dei = 0;

	/* Setup egress 1:1 mapping between [PRIO,DPL] and [PCP,DEI].
	 * priority determines the PCP value (0..7) in the frame and
	 * DPL determines the DEI value (0..1) in the frame. */
	for (cos = 0; cos < 8; cos++) {
		for (dpl = 0; dpl < 2; dpl++) {
			lan_wr(REW_PCP_DEI_CFG_DEI_QOS_VAL_SET(dpl) |
			       REW_PCP_DEI_CFG_PCP_QOS_VAL_SET(cos),
			       lan966x,
			       REW_PCP_DEI_CFG(port->chip_port, 8 * dpl + cos));
			port->qos_port_conf.e_prio_dpl_pcp_dei_map[cos][dpl].pcp = cos;
			port->qos_port_conf.e_prio_dpl_pcp_dei_map[cos][dpl].dei = dpl;
		}
	}

	tag_cfg = 0; /* Classified [PCP,DEI] */
// 	tag_cfg = 1; /* Port based [PCP,DEI] */
//	tag_cfg = 2; /* [COS,DPL] via map to [PCP,DEI] */
//	tag_cfg = 3; /* [COS,DPL] 1:1 to [PCP,DEI] */
	lan_rmw(REW_TAG_CFG_TAG_PCP_CFG_SET(tag_cfg) |
		REW_TAG_CFG_TAG_DEI_CFG_SET(tag_cfg),
		REW_TAG_CFG_TAG_PCP_CFG |
		REW_TAG_CFG_TAG_DEI_CFG,
		lan966x, REW_TAG_CFG(port->chip_port));
	port->qos_port_conf.e_mode = MCHP_E_MODE_CLASSIFIED;

	port->qos_port_conf.e_default_pcp = 0;
	port->qos_port_conf.e_default_dei = 0;
}

static struct lan966x_mact_entry *alloc_mact_entry(struct lan966x *lan966x,
						   const unsigned char *mac,
						   u16 vid, u16 port_index)
{
	struct lan966x_mact_entry *mact_entry;

	mact_entry = devm_kzalloc(lan966x->dev,
				  sizeof(*mact_entry), GFP_ATOMIC);
	if (!mact_entry)
		return NULL;

	memcpy(mact_entry->mac, mac, ETH_ALEN);
	mact_entry->vid = vid;
	mact_entry->port = port_index;
	mact_entry->row = -1;
	mact_entry->bucket = -1;
	return mact_entry;
}

static struct lan966x_mact_entry *find_mact_entry(struct lan966x *lan966x,
						  const unsigned char *mac,
						  u16 vid, u16 port_index)
{
	struct lan966x_mact_entry *mact_entry;
	struct lan966x_mact_entry *res = NULL;
	unsigned long flags;

	spin_lock_irqsave(&lan966x->mact_lock, flags);
	list_for_each_entry(mact_entry, &lan966x->mact_entries, list) {
		if ((mact_entry->vid == vid) &&
		    ether_addr_equal(mac, mact_entry->mac) &&
		    mact_entry->port == port_index) {
			res = mact_entry;
			break;
		}
	}

	spin_unlock_irqrestore(&lan966x->mact_lock, flags);
	return res;
}

static void lan966x_fdb_call_notifiers(enum switchdev_notifier_type type,
				       const char *mac, u16 vid,
				       struct net_device *dev, bool offloaded)
{
	struct switchdev_notifier_fdb_info info = { 0 };

	info.addr = mac;
	info.vid = vid;
	info.offloaded = offloaded;
	call_switchdev_notifiers(type, dev, &info.info, NULL);
}

int lan966x_add_mact_entry(struct lan966x *lan966x, struct lan966x_port *port,
			   const unsigned char *addr, u16 vid)
{
	struct lan966x_mact_entry *mact_entry;
	unsigned long flags;
	bool exists_entry;
	int ret;

	ret = lan966x_mact_lookup(lan966x, addr, vid, ENTRYTYPE_NORMAL);
	if (ret)
		return 0;

	/* In case the entry already exists, don't add it again to SW,
	 * just update HW, but we need to look in the actual HW because
	 * it is possible for an entry to be learn by HW and before the
	 * mact thread to start the frame will reach CPU and the CPU will
	 * add the entry but without the extern_learn flag.
	 */
	exists_entry = find_mact_entry(lan966x, addr, vid, port->chip_port) != NULL;
	if (exists_entry)
		goto update_hw;

	/* Add the entry in SW MAC table not to get the notification when
	 * SW is pulling again
	 */
	mact_entry = alloc_mact_entry(lan966x, addr, vid, port->chip_port);
	if (!mact_entry)
		return -ENOMEM;
	spin_lock_irqsave(&lan966x->mact_lock, flags);
	list_add_tail(&mact_entry->list, &lan966x->mact_entries);
	spin_unlock_irqrestore(&lan966x->mact_lock, flags);

update_hw:
	ret = lan966x_mact_learn(lan966x, port->chip_port,
				 addr, vid, ENTRYTYPE_LOCKED);

	if (!exists_entry)
		lan966x_fdb_call_notifiers(SWITCHDEV_FDB_OFFLOADED, addr, vid,
					   port->dev, true);

	return ret;
}

int lan966x_del_mact_entry(struct lan966x *lan966x, const unsigned char *addr,
			    u16 vid)
{
	struct lan966x_mact_entry *mact_entry, *tmp;
	unsigned long flags;

	/* Delete the entry in SW MAC table not to get the notification when
	 * SW is pulling again
	 */
	spin_lock_irqsave(&lan966x->mact_lock, flags);
	list_for_each_entry_safe(mact_entry, tmp, &lan966x->mact_entries,
				 list) {
		if ((vid == 0 || mact_entry->vid == vid) &&
		    ether_addr_equal(addr, mact_entry->mac)) {
			list_del(&mact_entry->list);
			devm_kfree(lan966x->dev, mact_entry);

			lan966x_mact_forget(lan966x, addr, mact_entry->vid,
					    ENTRYTYPE_LOCKED);
		}
	}
	spin_unlock_irqrestore(&lan966x->mact_lock, flags);

	return 0;
}

void lan966x_update_stats(struct lan966x *lan966x)
{
	int i, j;

	mutex_lock(&lan966x->stats_lock);

	for (i = 0; i < LAN966X_MAX_PORTS; i++) {
		uint idx = i * lan966x->num_stats;

		lan_wr(SYS_STAT_CFG_STAT_VIEW_SET(i),
		       lan966x, SYS_STAT_CFG);

		for (j = 0; j < lan966x->num_stats; j++) {
			u32 offset = lan966x->stats_layout[j].offset;
			lan966x_add_cnt(&lan966x->stats[idx++],
					lan_rd(lan966x, SYS_CNT(offset)));
		}
	}

	mutex_unlock(&lan966x->stats_lock);
}

static void lan966x_check_stats_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan966x *lan966x = container_of(del_work, struct lan966x,
					       stats_work);

	lan966x_update_stats(lan966x);
	lan966x_qos_update_stats(lan966x);

	queue_delayed_work(lan966x->stats_queue, &lan966x->stats_work,
			   LAN966X_STATS_CHECK_DELAY);
}

static void lan966x_mac_notifiers(struct work_struct *work)
{
	struct lan966x_mact_event_work *mact_work =
		container_of(work, struct lan966x_mact_event_work, work);

	rtnl_lock();

	lan966x_fdb_call_notifiers(mact_work->type, mact_work->mac,
				   mact_work->vid, mact_work->dev,
				   true);

	rtnl_unlock();
	dev_put(mact_work->dev);
	kfree(work);
}

static void lan966x_mac_delay_notifiers(struct lan966x *lan966x,
					 enum switchdev_notifier_type type,
					 unsigned char *mac, u32 vid,
					 struct net_device *dev)
{
	struct lan966x_mact_event_work *work;
	struct lan966x_port *port;

	if (!dev)
		return;

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work)
		return;

	INIT_WORK(&work->work, lan966x_mac_notifiers);

	port = netdev_priv(dev);
	if (port->bond)
		dev = port->bond; /* pretend it's learned on the LAG device */

	work->dev = dev;
	ether_addr_copy((u8*)work->mac, mac);
	work->vid = vid;
	work->type = type;

	queue_work(system_wq, &work->work);
	dev_hold(dev);
}

static void lan966x_mac_irq_process(struct lan966x *lan966x, u32 row,
				    struct lan966x_mact_raw_entry *raw_entry)
{
	struct lan966x_mact_entry *mact_entry, *tmp;
	unsigned long flags;
	char mac[ETH_ALEN];
	u32 dest_idx;
	u32 column;
	u32 vid;

	spin_lock_irqsave(&lan966x->mact_lock, flags);
	list_for_each_entry_safe(mact_entry, tmp, &lan966x->mact_entries,
				 list) {
		bool founded = false;

		if (mact_entry->row != row)
			continue;

		for (column = 0; column < LAN966X_MACT_COLUMNS; ++column) {
			/* All the valid entries are at the start of the row,
			 * so when get one invalid entry it can just skip
			 */
			if (!ANA_MACACCESS_VALID_GET(raw_entry[column].maca))
				break;

			mac[0] = (raw_entry[column].mach >> 8)  & 0xff;
			mac[1] = (raw_entry[column].mach >> 0)  & 0xff;
			mac[2] = (raw_entry[column].macl >> 24) & 0xff;
			mac[3] = (raw_entry[column].macl >> 16) & 0xff;
			mac[4] = (raw_entry[column].macl >> 8)  & 0xff;
			mac[5] = (raw_entry[column].macl >> 0)  & 0xff;

			vid = (raw_entry[column].mach >> 16) & 0xfff;

			dest_idx  = ANA_MACACCESS_DEST_IDX_GET(raw_entry[column].maca);

			if (mact_entry->vid == vid &&
			    ether_addr_equal(mac, mact_entry->mac) &&
			    mact_entry->port == dest_idx) {
				raw_entry[column].process = true;
				founded = true;
				break;
			}
		}

		if (!founded) {
			lan966x_mac_delay_notifiers(lan966x,
						    SWITCHDEV_FDB_DEL_TO_BRIDGE,
						    mact_entry->mac, mact_entry->vid,
						    lan966x->ports[mact_entry->port]->dev);

			list_del(&mact_entry->list);
			devm_kfree(lan966x->dev, mact_entry);
		}
	}
	spin_unlock_irqrestore(&lan966x->mact_lock, flags);

	for (column = 0; column < LAN966X_MACT_COLUMNS; ++column) {
		if (!ANA_MACACCESS_VALID_GET(raw_entry[column].maca))
			break;

		if (raw_entry[column].process)
			continue;

		mac[0] = (raw_entry[column].mach >> 8)  & 0xff;
		mac[1] = (raw_entry[column].mach >> 0)  & 0xff;
		mac[2] = (raw_entry[column].macl >> 24) & 0xff;
		mac[3] = (raw_entry[column].macl >> 16) & 0xff;
		mac[4] = (raw_entry[column].macl >> 8)  & 0xff;
		mac[5] = (raw_entry[column].macl >> 0)  & 0xff;

		vid = (raw_entry[column].mach >> 16) & 0xfff;

		dest_idx  = ANA_MACACCESS_DEST_IDX_GET(raw_entry[column].maca);
		if (dest_idx > lan966x->num_phys_ports)
			break;

		mact_entry = alloc_mact_entry(lan966x, mac, vid,
					      dest_idx);
		if (!mact_entry)
			return;

		mact_entry->row = row;
		mact_entry->bucket = column;

		spin_lock_irqsave(&lan966x->mact_lock, flags);
		list_add_tail(&mact_entry->list,
			      &lan966x->mact_entries);
		spin_unlock_irqrestore(&lan966x->mact_lock, flags);

		lan966x_mac_delay_notifiers(lan966x,
					    SWITCHDEV_FDB_ADD_TO_BRIDGE,
					    mac, vid, lan966x->ports[dest_idx]->dev);
	}
}

static void lan966x_mac_irq_handler(struct lan966x *lan966x)
{
	struct lan966x_mact_raw_entry entry[LAN966X_MACT_COLUMNS] = { 0 };
	bool process_entry = false;
	u32 index, column;
	u32 val;

	/* Check if the mac table triggered this, if not just bail out */
	if (!(ANA_ANAINTR_INTR_GET(lan_rd(lan966x, ANA_ANAINTR))))
		return;

	lan966x_prof_sample_begin(&lan966x->prof_stat[LAN966X_PROFILE_MAC_IRQ]);

	/* Start the scan from 0, 0 */
	lan_wr(ANA_MACTINDX_M_INDEX_SET(0) |
	       ANA_MACTINDX_BUCKET_SET(0),
	       lan966x, ANA_MACTINDX);

	while (1) {
		lan_rmw(ANA_MACACCESS_MAC_TABLE_CMD_SET(MACACCESS_CMD_SYNC_GET_NEXT),
			ANA_MACACCESS_MAC_TABLE_CMD,
			lan966x, ANA_MACACCESS);

		lan966x_mact_wait_for_completion(lan966x);

		val = lan_rd(lan966x, ANA_MACTINDX);

		index = ANA_MACTINDX_M_INDEX_GET(val);
		column = ANA_MACTINDX_BUCKET_GET(val);

		/* The SYNC-GET-NEXT finishes at row 0 and column 3. But there
		 * could be an entry there. Therfore it is required to check if
		 * the initial index is 0, then there is a valid entry so it
		 * doesn't need to stop at column 3. Otherwise stop when it
		 * reaches row 0, column 3.
		 */
		if (index == 0) {
			if (column == LAN966X_MACT_COLUMNS - 1) {
				if (process_entry == true) {
					break;
				}

				process_entry = true;
			}
		} else {
			process_entry = true;
		}

		/* Just fill up the array */
		entry[column].mach = lan_rd(lan966x, ANA_MACHDATA);
		entry[column].macl = lan_rd(lan966x, ANA_MACLDATA);
		entry[column].maca = lan_rd(lan966x, ANA_MACACCESS);
		entry[column].process = false;

		/* Because the entries in the row can interchange between them,
		 * it is required to read initially all 4 columns and after that
		 * process them.
		 */
		if (column == LAN966X_MACT_COLUMNS - 1) {
			lan966x_mac_irq_process(lan966x, index, entry);
			continue;
		}
	}

	lan_rmw(ANA_ANAINTR_INTR_SET(0),
		ANA_ANAINTR_INTR,
		lan966x, ANA_ANAINTR);

	lan966x_prof_sample_end(&lan966x->prof_stat[LAN966X_PROFILE_MAC_IRQ]);
}

static void lan966x_mact_pull_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan966x *lan966x = container_of(del_work, struct lan966x,
					       mact_work);

	if (ANA_ANAINTR_INTR_GET(lan_rd(lan966x, ANA_ANAINTR)))
		lan966x_mac_irq_handler(lan966x);

	queue_delayed_work(lan966x->mact_queue, &lan966x->mact_work,
			   LAN966X_MACT_PULL_DELAY);
}

static void lan966x_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *stats)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	uint i, idx = port->chip_port * lan966x->num_stats;

	mutex_lock(&lan966x->stats_lock);

	stats->rx_bytes = lan966x->stats[idx + SYS_COUNT_RX_OCT] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_OCT];

	stats->rx_packets= lan966x->stats[idx + SYS_COUNT_RX_SHORT] +
		lan966x->stats[idx + SYS_COUNT_RX_FRAG] +
		lan966x->stats[idx + SYS_COUNT_RX_JABBER] +
		lan966x->stats[idx + SYS_COUNT_RX_CRC] +
		lan966x->stats[idx + SYS_COUNT_RX_SYMBOL_ERR] +
		lan966x->stats[idx + SYS_COUNT_RX_SZ_64] +
		lan966x->stats[idx + SYS_COUNT_RX_SZ_65_127] +
		lan966x->stats[idx + SYS_COUNT_RX_SZ_128_255] +
		lan966x->stats[idx + SYS_COUNT_RX_SZ_256_511] +
		lan966x->stats[idx + SYS_COUNT_RX_SZ_512_1023] +
		lan966x->stats[idx + SYS_COUNT_RX_SZ_1024_1526] +
		lan966x->stats[idx + SYS_COUNT_RX_SZ_JUMBO] +
		lan966x->stats[idx + SYS_COUNT_RX_LONG] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SHORT] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_FRAG] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_JABBER] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SZ_64] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SZ_65_127] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SZ_128_255] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SZ_256_511] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SZ_512_1023] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SZ_1024_1526] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_SZ_JUMBO];

	stats->multicast = lan966x->stats[idx + SYS_COUNT_RX_MC] +
		lan966x->stats[idx + SYS_COUNT_RX_PMAC_MC];

	stats->rx_errors = lan966x->stats[idx + SYS_COUNT_RX_SHORT] +
		lan966x->stats[idx + SYS_COUNT_RX_FRAG] +
		lan966x->stats[idx + SYS_COUNT_RX_JABBER] +
		lan966x->stats[idx + SYS_COUNT_RX_CRC] +
		lan966x->stats[idx + SYS_COUNT_RX_SYMBOL_ERR] +
		lan966x->stats[idx + SYS_COUNT_RX_LONG];

	stats->rx_dropped = dev->stats.rx_dropped +
		lan966x->stats[idx + SYS_COUNT_RX_LONG] +
		lan966x->stats[idx + SYS_COUNT_DR_LOCAL] +
		lan966x->stats[idx + SYS_COUNT_DR_TAIL];

	for (i = 0; i < LAN966X_NUM_TC; i++) {
		stats->rx_dropped +=
			(lan966x->stats[idx + SYS_COUNT_DR_YELLOW_PRIO_0 + i] +
			 lan966x->stats[idx + SYS_COUNT_DR_GREEN_PRIO_0 + i]);
	}

	/* Get Tx stats */
	stats->tx_bytes = lan966x->stats[idx + SYS_COUNT_TX_OCT] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_OCT];

	stats->tx_packets = lan966x->stats[idx + SYS_COUNT_TX_SZ_64] +
		lan966x->stats[idx + SYS_COUNT_TX_SZ_65_127] +
		lan966x->stats[idx + SYS_COUNT_TX_SZ_128_255] +
		lan966x->stats[idx + SYS_COUNT_TX_SZ_256_511] +
		lan966x->stats[idx + SYS_COUNT_TX_SZ_512_1023] +
		lan966x->stats[idx + SYS_COUNT_TX_SZ_1024_1526] +
		lan966x->stats[idx + SYS_COUNT_TX_SZ_JUMBO] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_SZ_64] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_SZ_65_127] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_SZ_128_255] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_SZ_256_511] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_SZ_512_1023] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_SZ_1024_1526] +
		lan966x->stats[idx + SYS_COUNT_TX_PMAC_SZ_JUMBO];

	stats->tx_dropped = lan966x->stats[idx + SYS_COUNT_TX_DROP] +
		lan966x->stats[idx + SYS_COUNT_TX_AGED];

	stats->collisions = lan966x->stats[idx + SYS_COUNT_TX_COL];

	mutex_unlock(&lan966x->stats_lock);
}

static int lan966x_port_set_mac_address(struct net_device *dev, void *p)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	const struct sockaddr *addr = p;

	/* Learn the new net device MAC address in the mac table. */
	lan966x_mact_learn(lan966x, PGID_CPU, addr->sa_data, port->pvid,
			   ENTRYTYPE_LOCKED);

	/* Then forget the previous one. */
	lan966x_mact_forget(lan966x, dev->dev_addr, port->pvid,
			    ENTRYTYPE_LOCKED);

	eth_hw_addr_set(dev, addr->sa_data);
	return 0;
}

static int lan966x_port_get_phys_port_name(struct net_device *dev,
					   char *buf, size_t len)
{
	struct lan966x_port *port = netdev_priv(dev);
	int ret;

	ret = snprintf(buf, len, "p%d", port->chip_port);
	if (ret >= len)
		return -EINVAL;

	return 0;
}

static int lan966x_port_open(struct net_device *dev)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	int err;

	/* Enable receiving frames on the port, and activate auto-learning of
	 * MAC addresses.
	 */
	lan_rmw(ANA_PORT_CFG_LEARNAUTO_SET(1) |
		ANA_PORT_CFG_RECV_ENA_SET(1) |
		ANA_PORT_CFG_PORTID_VAL_SET(port->chip_port),
		ANA_PORT_CFG_LEARNAUTO |
		ANA_PORT_CFG_RECV_ENA |
		ANA_PORT_CFG_PORTID_VAL,
		lan966x, ANA_PORT_CFG(port->chip_port));

	err = phylink_of_phy_connect(port->phylink, to_of_node(port->fwnode), 0);
	if (err) {
		netdev_err(dev, "Could not attach to PHY\n");
		return err;
	}

	phylink_start(port->phylink);

	return 0;
}

static int lan966x_port_stop(struct net_device *dev)
{
	struct lan966x_port *port = netdev_priv(dev);

	lan966x_port_config_down(port);
	phylink_stop(port->phylink);
	phylink_disconnect_phy(port->phylink);

	return 0;
}

static void lan966x_ifh_inject(u32 *ifh, size_t val, size_t pos, size_t length)
{
	int i;

	for (i = pos; i < pos + length; ++i) {
		if (val & BIT(i - pos))
			ifh[IFH_LEN - i / 32 - 1] |= BIT(i % 32);
		else
			ifh[IFH_LEN - i / 32 - 1] &= ~(BIT(i % 32));
	}
}

static void lan966x_gen_ifh(u32 *ifh, struct frame_info *info, struct lan966x *lan966x)
{
	u32 mep_cnt = lan966x->num_phys_ports;
	u32 chip_port, seq_num = 0;

	for (chip_port = 0; chip_port < lan966x->num_phys_ports; chip_port++)
		if (info->port & (1 << chip_port))
			break;
	/* The VOP and DLR requires a 32 bit counter. MRP and PT requires 16 bit sequence number
	 * The PTP sequence numbers are last after VOP, DLR and MRP
	 * In case of PTP frame the SEQ_NUM field is indexing every 16 bit field
	 * in the PTP_SEQ_NO configuration - see VML
	 */
	if (info->rew_oam) {
		switch (info->oam_type) {
		case OAM_TYPE_CCM:
			/* VOP sequence numbers */
			seq_num = chip_port;
			break;
		case OAM_TYPE_BCN:
		case OAM_TYPE_ADV:
			/* DLR sequence numbers - after VOP */
			seq_num = mep_cnt + chip_port;
			break;
		case OAM_TYPE_TST:
		case OAM_TYPE_ITST:
			/* MRP sequence numbers after VOP and DLR */
			seq_num = (mep_cnt*2) + (mep_cnt*2) + chip_port;
			break;
		default:
			/* Should never happen */
			seq_num = 0;
		}
	} else {
		if ((info->rew_op == IFH_REW_OP_ORIGIN_TIMESTAMP_SEQ) ||
		    (info->rew_op == IFH_REW_OP_PTP_AFI_NONE))
			/* PTP sequence numbers after VOP, DLR and MRP */
			seq_num = ((mep_cnt*2) + (mep_cnt*2) + mep_cnt) + info->ptp_seq_idx;
	}

	lan966x_ifh_inject(ifh, 1, IFH_POS_BYPASS, 1);
	lan966x_ifh_inject(ifh, info->port, IFH_POS_DSTS, IFH_WID_DSTS);
	lan966x_ifh_inject(ifh, info->rew_op, IFH_POS_REW_CMD, IFH_WID_REW_CMD);
	lan966x_ifh_inject(ifh, info->timestamp, IFH_POS_TIMESTAMP,
			   IFH_WID_TIMESTAMP);
	lan966x_ifh_inject(ifh, info->qos_class, IFH_POS_QOS_CLASS,
			   IFH_WID_QOS_CLASS);
	lan966x_ifh_inject(ifh, info->ipv, IFH_POS_IPV, IFH_WID_IPV);
	lan966x_ifh_inject(ifh, info->afi, IFH_POS_AFI, IFH_WID_AFI);
	lan966x_ifh_inject(ifh, info->rew_oam, IFH_POS_REW_OAM,
			   IFH_WID_REW_OAM);
	lan966x_ifh_inject(ifh, info->oam_type, IFH_POS_PDU_TYPE,
			   IFH_WID_PDU_TYPE);
	lan966x_ifh_inject(ifh, seq_num, IFH_POS_SEQ_NUM,
			   IFH_WID_SEQ_NUM);
	lan966x_ifh_inject(ifh, info->vid, IFH_POS_TCI, IFH_WID_TCI);
}

static inline int lan966x_ts_fifo_ready(struct lan966x *lan966x)
{
	return PTP_TWOSTEP_CTRL_VLD_GET(lan_rd(lan966x, PTP_TWOSTEP_CTRL));
}

static void lan966x_ptp_2step_save(struct lan966x_port *port,
				   struct frame_info *info,
				   struct skb_shared_info *shinfo,
				   struct sk_buff *skb)
{
	if (shinfo->tx_flags & SKBTX_HW_TSTAMP &&
	    info->rew_op == IFH_REW_OP_TWO_STEP_PTP) {

		/* When using twostep timestamping, the timestamp in ifh
		 * represents the id of the frame, which can be used to
		 * look in the fifo for the frame
		 */
		shinfo->tx_flags |= SKBTX_IN_PROGRESS;

		skb->cb[0] = port->ts_id;
		skb_queue_tail(&port->tx_skbs, skb);
		port->ts_id++;
		port->ts_id &= 0xff;
	}
}

static int lan966x_port_ifh_xmit(struct sk_buff *skb, struct frame_info *info,
				 struct net_device *dev)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	u32 ifh[IFH_LEN] = { 0 };
	u32 val;
	u8 grp = 0;
	u32 i, count, last;

	spin_lock(&lan966x->tx_lock);
	val = lan_rd(lan966x, QS_INJ_STATUS);
	if (!(val & QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))) ||
	    (val & QS_INJ_STATUS_WMARK_REACHED_SET(BIT(grp)))) {
		spin_unlock(&lan966x->tx_lock);
		return NETDEV_TX_BUSY;
	}

	/* Write start of frame */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_SOF_SET(1),
	       lan966x, QS_INJ_CTRL(grp));

	lan966x_gen_ifh(ifh, info, lan966x);

	/* Write IFH header */
	for (i = 0; i < IFH_LEN; ++i) {
		/* Wait until the fifo is ready */
		while (!(lan_rd(lan966x, QS_INJ_STATUS) &
			 QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))))
				;

		lan_wr((__force u32)cpu_to_be32(ifh[i]), lan966x,
		       QS_INJ_WR(grp));
	}

	/* Write frame */
	count = (skb->len + 3) / 4;
	last = skb->len % 4;
	for (i = 0; i < count; ++i) {
		/* Wait until the fifo is ready */
		while (!(lan_rd(lan966x, QS_INJ_STATUS) &
			 QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))))
				;

		lan_wr(((u32 *)skb->data)[i], lan966x, QS_INJ_WR(grp));
	}

	/* Add padding */
	while (i < (LAN966X_BUFFER_MIN_SZ / 4)) {
		/* Wait until the fifo is ready */
		while (!(lan_rd(lan966x, QS_INJ_STATUS) &
			 QS_INJ_STATUS_FIFO_RDY_SET(BIT(grp))))
				;

		lan_wr(0, lan966x, QS_INJ_WR(grp));
		++i;
	}

	skb_tx_timestamp(skb);
	lan966x_ptp_2step_save(port, info, shinfo, skb);

	/* Inidcate EOF and valid bytes in the last word */
	lan_wr(QS_INJ_CTRL_GAP_SIZE_SET(1) |
	       QS_INJ_CTRL_VLD_BYTES_SET(skb->len < LAN966X_BUFFER_MIN_SZ ?  0 : last) |
	       QS_INJ_CTRL_EOF_SET(1),
	       lan966x, QS_INJ_CTRL(grp));

	/* Add dummy CRC */
	lan_wr(0, lan966x, QS_INJ_WR(grp));

	spin_unlock(&lan966x->tx_lock);

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	if (shinfo->tx_flags & SKBTX_HW_TSTAMP &&
	    info->rew_op == IFH_REW_OP_TWO_STEP_PTP) {
		u32 val = 0;
		int err;

		if (!lan966x->ptp_poll)
			goto skip_polling;

		err = readx_poll_timeout_atomic(lan966x_ts_fifo_ready, lan966x,
						val, val, 10, 100000);
		if (err == -ETIMEDOUT) {
			pr_info("Ts fifo no valid value\n");
			goto skip_polling;
		}

		lan966x_ptp_irq_handler(0, lan966x);
skip_polling:
		return NETDEV_TX_OK;
	}

	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;

}

int lan966x_port_xmit_impl(struct sk_buff *skb, struct frame_info *info,
			   struct net_device *dev)
{
	struct lan966x_port *port = netdev_priv(dev);

#if defined(SUNRISE) || defined(ASIC)
	if (port->lan966x->use_napi)
		return lan966x_napi_xmit(skb, info, dev);
#endif
	if (port->lan966x->use_dma)
		return lan966x_fdma_xmit(skb, info, dev);

	return lan966x_port_ifh_xmit(skb, info, dev);
}

static int lan966x_ptp_classify(struct lan966x_port *port, struct sk_buff *skb)
{
	struct ptp_header *header;
	u8 msgtype;
	int type;

	if (port->ptp_cmd == IFH_REW_OP_NOOP)
		return IFH_REW_OP_NOOP;

	type = ptp_classify_raw(skb);
	if (type == PTP_CLASS_NONE)
		return IFH_REW_OP_NOOP;

	header = ptp_parse_header(skb, type);
	if (!header)
		return IFH_REW_OP_NOOP;

	if (port->ptp_cmd == IFH_REW_OP_TWO_STEP_PTP)
		return IFH_REW_OP_TWO_STEP_PTP;

	/* If it is sync and run 1 step then set the correct operation,
	 * otherwise run as 2 step
	 */
	msgtype = ptp_get_msgtype(header, type);
	if ((msgtype & 0xf) == 0)
		return IFH_REW_OP_ONE_STEP_PTP;

	return IFH_REW_OP_TWO_STEP_PTP;
}

static int lan966x_port_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	struct lan966x_port *port = netdev_priv(dev);
	struct frame_info info = {};

	info.port = BIT(port->chip_port);
	info.vid = skb_vlan_tag_get(skb);

	if (shinfo->tx_flags & SKBTX_HW_TSTAMP) {
		/* Timestamp on tx only PTP frames */
		info.rew_op = lan966x_ptp_classify(port, skb);
		if (info.rew_op == IFH_REW_OP_TWO_STEP_PTP)
			info.timestamp = port->ts_id;
	}

	/* Adjust frame priority to priority queue */
	info.qos_class = skb->priority >= 0x7 ? 0x7 : skb->priority;
	info.ipv = info.qos_class;

	return lan966x_port_xmit_impl(skb, &info, dev);
}

void lan966x_set_promisc(struct lan966x_port *port, bool enable,
			 bool change_master)
{
	struct lan966x *lan966x = port->lan966x;
	u32 val;

	val = lan_rd(lan966x, ANA_CPU_FWD_CFG(port->chip_port));
	if (enable)
		lan_wr(val | ANA_CPU_FWD_CFG_SRC_COPY_ENA_SET(1),
		       lan966x, ANA_CPU_FWD_CFG(port->chip_port));
	else
		lan_wr(val & ~(ANA_CPU_FWD_CFG_SRC_COPY_ENA_SET(1)),
		       lan966x, ANA_CPU_FWD_CFG(port->chip_port));

	if (!change_master)
		port->promisc_mode = enable;
}

static void lan966x_change_rx_flags(struct net_device *dev, int flags)
{
	struct lan966x_port *port = netdev_priv(dev);
#ifdef CONFIG_NET_SWITCHDEV
	struct lan966x *lan966x = port->lan966x;
#endif

	if (!(flags & IFF_PROMISC))
		return;

#ifdef CONFIG_NET_SWITCHDEV
	if (lan966x->bridge_mask & BIT(port->chip_port) &&
	    lan966x->hw_offload)
		return;
#endif

	if (dev->flags & IFF_PROMISC)
		lan966x_set_promisc(port, true, false);
	else
		lan966x_set_promisc(port, false, false);
}

static int lan966x_get_max_mtu(struct lan966x *lan966x)
{
	int max_mtu = 0;
	int i;

	for (i = 0; i < lan966x->num_phys_ports; ++i) {
		struct lan966x_port *port;
		int mtu;

		port = lan966x->ports[i];
		if (!port)
			continue;

		mtu = lan_rd(lan966x, DEV_MAC_MAXLEN_CFG(port->chip_port));
		if (mtu > max_mtu)
			max_mtu = mtu;
	}

	return max_mtu;
}

#if defined(SUNRISE) || defined(ASIC)
static void lan966x_napi_reload(struct lan966x *lan966x, int new_mtu)
{
	void **rx_dcb, **tx_dcb, **tx_dcb_buf;
	dma_addr_t rx_dma, tx_dma;
	unsigned long flags;
	u32 size;

	/* Redo the dcbs with the new page order */
	spin_lock_irqsave(&lan966x->tx_lock, flags);

	lan966x_napi_rx_disable(&lan966x->rx);
	lan966x_napi_tx_disable(&lan966x->tx);

	lan966x_napi_rx_get_dcb(&lan966x->rx, &rx_dcb, &rx_dma);
	lan966x_napi_tx_get_dcb(&lan966x->tx, &tx_dcb, &tx_dcb_buf, &tx_dma);

	lan966x_napi_rx_clear_dbs(&lan966x->rx);

	lan966x->rx.page_order = round_up(new_mtu, PAGE_SIZE) / PAGE_SIZE - 1;

	lan966x_napi_rx_alloc(&lan966x->rx);
	lan966x_napi_tx_alloc(&lan966x->tx);

	spin_unlock_irqrestore(&lan966x->tx_lock, flags);

	/* Now it is possible to do the cleanup of dcb */
	size = sizeof(struct lan966x_tx_dcb_hw) * FDMA_DCB_MAX;
	size = ALIGN(size, PAGE_SIZE);
	dma_free_coherent(lan966x->dev, size, tx_dcb, tx_dma);

	kfree(tx_dcb_buf);

	size = sizeof(struct lan966x_rx_dcb_hw) * FDMA_DCB_MAX;
	size = ALIGN(size, PAGE_SIZE);
	dma_free_coherent(lan966x->dev, size, rx_dcb, rx_dma);

	lan966x_napi_rx_activate(&lan966x->rx);
}
#endif

#define LAN966X_HW_MTU(mtu)	(mtu + ETH_HLEN + ETH_FCS_LEN)
static int lan966x_change_mtu(struct net_device *dev, int new_mtu)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	u32 val, delay;
	int max_mtu;

	dev->mtu = new_mtu;
	lan_wr(DEV_MAC_MAXLEN_CFG_MAX_LEN_SET(LAN966X_HW_MTU(new_mtu)),
	       lan966x, DEV_MAC_MAXLEN_CFG(port->chip_port));

	max_mtu = lan966x_get_max_mtu(lan966x);

	/* If there is no change then just apply the new value and exit */
	if (round_up(max_mtu, PAGE_SIZE) / PAGE_SIZE - 1 ==
	    lan966x->rx.page_order)
		return 0;

	/* Disable the CPU port */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(0),
		QSYS_SW_PORT_MODE_PORT_ENA,
		lan966x,  QSYS_SW_PORT_MODE(CPU_PORT));

	/* Flush the CPU queues */
	do {
		val = lan_rd(lan966x, QSYS_SW_STATUS(CPU_PORT));
		msleep(1);
		delay++;
		if (delay == 2000) {
			pr_err("Flush timeout chip port %u", CPU_PORT);
			break;
		}
	} while (QSYS_SW_STATUS_EQ_AVAIL_GET(val));

	/* Add a sleep in case there are frames between the queues and the CPU
	 * port
	 */
	msleep(10);

#if defined(SUNRISE) || defined(ASIC)
	lan966x_napi_reload(lan966x, LAN966X_HW_MTU(new_mtu));
#endif

	/* Enable back the CPU port */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1),
		QSYS_SW_PORT_MODE_PORT_ENA,
		lan966x,  QSYS_SW_PORT_MODE(CPU_PORT));

	return 0;
}

int lan966x_mc_unsync(struct net_device *dev, const unsigned char *addr)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;

	return lan966x_mact_forget(lan966x, addr, port->pvid, ENTRYTYPE_LOCKED);
}

int lan966x_mc_sync(struct net_device *dev, const unsigned char *addr)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;

	return lan966x_mact_learn(lan966x, PGID_CPU, addr, port->pvid,
				  ENTRYTYPE_LOCKED);
}

static void lan966x_set_rx_mode(struct net_device *dev)
{
	struct lan966x_port *lan966x_port = netdev_priv(dev);
	struct lan966x *lan966x = lan966x_port->lan966x;

	if (!(lan966x->bridge_mask & BIT(lan966x_port->chip_port)))
		__dev_mc_sync(dev, lan966x_mc_sync, lan966x_mc_unsync);
}

int lan966x_vlan_vid_add(struct net_device *dev, u16 vid, bool pvid,
			 bool untagged)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	int ret;

	/* Make the port a member of the VLAN */
	lan966x->vlan_mask[vid] |= BIT(port->chip_port);
	ret = lan966x_vlant_set_mask(lan966x, vid);
	if (ret)
		return ret;

	/* Default ingress vlan classification */
	if (pvid)
		port->pvid = vid;

	if (untagged && port->vid != vid) {
		if (port->vid) {
			dev_err(lan966x->dev,
				"Port already has a native VLAN: %d\n",
				port->vid);
			return -EBUSY;
		}
		port->vid = vid;
	}

	lan966x_vlan_port_apply(lan966x, port);

	return 0;
}

static int lan966x_vlan_rx_add_vid(struct net_device *dev, __be16 proto,
				   u16 vid)
{
	return lan966x_vlan_vid_add(dev, vid, false, false);
}

int lan966x_vlan_vid_del(struct net_device *dev, u16 vid)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	int ret;

	/* 8021q removes VID 0 on module unload for all interfaces
	 * with VLAN filtering feature. We need to keep it to receive
	 * untagged traffic
	 */
	if (vid == 0)
		return 0;

	/* Stop port from being a member of the vlan */
	lan966x->vlan_mask[vid] &= ~BIT(port->chip_port);
	ret = lan966x_vlant_set_mask(lan966x, vid);
	if (ret)
		return ret;

	/* Ingress */
	if (port->pvid == vid)
		port->pvid = 0;

	/* Egress */
	if (port->vid == vid)
		port->vid = 0;

	lan966x_vlan_port_apply(lan966x, port);

	return 0;
}

static int lan966x_vlan_rx_kill_vid(struct net_device *dev, __be16 proto,
				    u16 vid)
{
	return lan966x_vlan_vid_del(dev, vid);
}

static void lan966x_vlan_mode(struct lan966x_port *port,
			      netdev_features_t features)
{
	struct lan966x *lan966x = port->lan966x;
	u8 p = port->chip_port;
	u32 val;

	/* Filtering */
	val = lan_rd(lan966x, ANA_VLANMASK);
	if (features & NETIF_F_HW_VLAN_CTAG_FILTER)
		val |= BIT(p);
	else
		val &= ~BIT(p);
	lan_wr(val, lan966x, ANA_VLANMASK);
}

static int lan966x_set_features(struct net_device *dev,
				netdev_features_t features)
{
	struct lan966x_port *port = netdev_priv(dev);
	netdev_features_t changed = dev->features ^ features;

	if ((dev->features & NETIF_F_HW_TC) > (features & NETIF_F_HW_TC) &&
	    port->tc.offload_cnt) {
		netdev_err(dev,
			   "Cannot disable HW TC offload while offloads active\n");
		return -EBUSY;
	}

	if (changed & NETIF_F_HW_VLAN_CTAG_FILTER)
		lan966x_vlan_mode(port, features);

	return 0;
}

static int lan966x_get_port_parent_id(struct net_device *dev,
				      struct netdev_phys_item_id *ppid)
{
	struct lan966x_port *lan966x_port = netdev_priv(dev);
	struct lan966x *lan966x = lan966x_port->lan966x;

	ppid->id_len = sizeof(lan966x->base_mac);
	memcpy(&ppid->id, &lan966x->base_mac, ppid->id_len);

	return 0;
}

static int lan966x_hwtstamp_get(struct net_device *dev, struct ifreq *ifr)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;

	return copy_to_user(ifr->ifr_data, &lan966x->hwtstamp_config,
			    sizeof(lan966x->hwtstamp_config)) ? -EFAULT : 0;
}

#define LAN966X_PTP_TRAP_RULES_CNT	5
static struct vcap_rule *lan966x_ptp_add_l2_key(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 0;
	int chain_id = LAN966X_VCAP_CID_IS2_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return NULL;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_ETYPE, ETH_P_1588, ~0);
	if (err) {
		vcap_del_rule(port->dev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *lan966x_ptp_add_ipv4_event_key(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 1;
	int chain_id = LAN966X_VCAP_CID_IS2_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return NULL;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 319, ~0);
	if (err) {
		vcap_del_rule(port->dev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *lan966x_ptp_add_ipv4_general_key(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 2;
	int chain_id = LAN966X_VCAP_CID_IS2_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return NULL;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 320, ~0);
	if (err) {
		vcap_del_rule(port->dev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *lan966x_ptp_add_ipv6_event_key(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 3;
	int chain_id = LAN966X_VCAP_CID_IS2_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return NULL;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 319, ~0);
	if (err) {
		vcap_del_rule(port->dev, rule_id);
		return NULL;
	}

	return vrule;
}

static struct vcap_rule *lan966x_ptp_add_ipv6_general_key(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 4;
	int chain_id = LAN966X_VCAP_CID_IS2_L0;
	int prio = (port->chip_port << 8) + 1;
	struct vcap_rule *vrule;
	int err;

	vrule = vcap_alloc_rule(port->dev, chain_id, VCAP_USER_PTP, prio, rule_id);
	if (!vrule || IS_ERR(vrule))
		return NULL;

	err = vcap_rule_add_key_u32(vrule, VCAP_KF_L4_DPORT, 320, ~0);
	if (err) {
		vcap_del_rule(port->dev, rule_id);
		return NULL;
	}

	return vrule;
}

static int lan966x_ptp_add_trap(struct lan966x_port *port,
				struct vcap_rule* (*lan966x_add_ptp_key)(struct lan966x_port*),
				u16 proto)
{
	struct vcap_rule *vrule;
	int err;

	vrule = lan966x_add_ptp_key(port);
	if (!vrule)
		return -ENOMEM;

	err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_COPY_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, LAN966X_PMM_REPLACE);
	err |= vcap_val_rule(vrule, proto);
	if (err)
		goto free_rule;

	err = vcap_add_rule(vrule);

free_rule:
	/* Free the local copy of the rule */
	vcap_free_rule(vrule);
	return err;
}

static int lan966x_ptp_del(struct lan966x_port *port, int rule_id)
{
	return vcap_del_rule(port->dev, rule_id);
}

static int lan966x_ptp_del_l2(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 0;

	return lan966x_ptp_del(port, rule_id);
}

static int lan966x_ptp_del_ipv4_event(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 1;

	return lan966x_ptp_del(port, rule_id);
}

static int lan966x_ptp_del_ipv4_general(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 2;

	return lan966x_ptp_del(port, rule_id);
}

static int lan966x_ptp_del_ipv6_event(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 3;

	return lan966x_ptp_del(port, rule_id);
}

static int lan966x_ptp_del_ipv6_general(struct lan966x_port *port)
{
	int rule_id = LAN966X_PTP_RULE_ID_OFFSET +
		      port->chip_port * LAN966X_PTP_TRAP_RULES_CNT + 4;

	return lan966x_ptp_del(port, rule_id);
}

static int lan966x_ptp_add_l2_rule(struct lan966x_port *port)
{
	return lan966x_ptp_add_trap(port, lan966x_ptp_add_l2_key, ETH_P_ALL);
}

static int lan966x_ptp_del_l2_rule(struct lan966x_port *port)
{
	return lan966x_ptp_del_l2(port);
}

static int lan966x_ptp_add_ipv4_rules(struct lan966x_port *port)
{
	int err;

	err = lan966x_ptp_add_trap(port, lan966x_ptp_add_ipv4_event_key,
				   ETH_P_IP);
	if (err)
		return err;

	err = lan966x_ptp_add_trap(port, lan966x_ptp_add_ipv4_general_key,
				   ETH_P_IP);
	if (err)
		lan966x_ptp_del_ipv4_event(port);

	return err;
}

static int lan966x_ptp_del_ipv4_rules(struct lan966x_port *port)
{
	int err;

	err = lan966x_ptp_del_ipv4_event(port);
	err |= lan966x_ptp_del_ipv4_general(port);

	return err;
}

static int lan966x_ptp_add_ipv6_rules(struct lan966x_port *port)
{
	int err;

	err = lan966x_ptp_add_trap(port, lan966x_ptp_add_ipv6_event_key,
				   ETH_P_IPV6);
	if (err)
		return err;

	err = lan966x_ptp_add_trap(port, lan966x_ptp_add_ipv6_general_key,
				   ETH_P_IPV6);
	if (err)
		lan966x_ptp_del_ipv6_event(port);

	return err;
}

static int lan966x_ptp_del_ipv6_rules(struct lan966x_port *port)
{
	int err;

	err = lan966x_ptp_del_ipv6_event(port);
	err |= lan966x_ptp_del_ipv6_general(port);

	return err;
}

static int lan966x_setup_ptp_traps(struct lan966x_port *port,
				   bool l2, bool l4)
{
	int err;

	if (l2)
		err = lan966x_ptp_add_l2_rule(port);
	else
		err = lan966x_ptp_del_l2_rule(port);
	if (err)
		return err;

	if (l4) {
		err = lan966x_ptp_add_ipv4_rules(port);
		if (err)
			goto err_ipv4;

		err = lan966x_ptp_add_ipv6_rules(port);
		if (err)
			goto err_ipv6;
	} else {
		err = lan966x_ptp_del_ipv4_rules(port);
		err |= lan966x_ptp_del_ipv6_rules(port);
	}

	if (err)
		return err;

	return 0;
err_ipv6:
	lan966x_ptp_del_ipv6_rules(port);
err_ipv4:
	if (l2)
		lan966x_ptp_del_l2_rule(port);

	return err;
}

static int lan966x_hwtstamp_set(struct net_device *dev, struct ifreq *ifr)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	bool l2 = false, l4 = false;
	struct hwtstamp_config cfg;

	if (copy_from_user(&cfg, ifr->ifr_data, sizeof(cfg)))
		return -EFAULT;

	switch (cfg.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		l4 = true;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		l2 = true;
		break;
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		l2 = true;
		l4 = true;
		break;
	default:
		return -ERANGE;
	}

	/* In case ptp4l is killed with -9 then is not possible to remove the
	 * rules again.
	 */
	lan966x_setup_ptp_traps(port, l2, l4);

	/* In case the PHY is doing all the timestamping, just bail out after
	 * the traps were set
	 */
	if (phy_has_hwtstamp(dev->phydev))
		return 0;

	switch (cfg.tx_type) {
	case HWTSTAMP_TX_ON:
		port->ptp_cmd = IFH_REW_OP_TWO_STEP_PTP;
		break;
	case HWTSTAMP_TX_ONESTEP_SYNC:
		port->ptp_cmd = IFH_REW_OP_ONE_STEP_PTP;
		break;
	case HWTSTAMP_TX_OFF:
		port->ptp_cmd = IFH_REW_OP_NOOP;
		break;
	default:
		lan966x_setup_ptp_traps(port, false, false);
		return -ERANGE;
	}

	if (l2 && l4)
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
	else if (l2)
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
	else if (l4)
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
	else
		cfg.rx_filter = HWTSTAMP_FILTER_NONE;

	mutex_lock(&lan966x->ptp_lock);
	/* Commit back the result & save it */
	memcpy(&lan966x->hwtstamp_config, &cfg, sizeof(cfg));
	mutex_unlock(&lan966x->ptp_lock);

	return copy_to_user(ifr->ifr_data, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}

static int lan966x_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct lan966x_port *port = netdev_priv(dev);
	int ret = 0;

	if (phy_has_hwtstamp(dev->phydev)) {
		/* Don't return here immediately, because it might be required
		 * for the MAC to configure to copy all the PTP frames to CPU
		 */
		ret = phy_mii_ioctl(dev->phydev, ifr, cmd);
		if (ret)
			return ret;
	}

	switch (cmd) {
	case SIOCSHWTSTAMP:
		return lan966x_hwtstamp_set(dev, ifr);
	case SIOCGHWTSTAMP:
		return lan966x_hwtstamp_get(dev, ifr);
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		if (!phy_has_hwtstamp(dev->phydev))
			return phylink_mii_ioctl(port->phylink, ifr, cmd);
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static const struct net_device_ops lan966x_port_netdev_ops = {
	.ndo_open			= lan966x_port_open,
	.ndo_stop			= lan966x_port_stop,
	.ndo_start_xmit			= lan966x_port_xmit,
	.ndo_change_rx_flags		= lan966x_change_rx_flags,
	.ndo_change_mtu			= lan966x_change_mtu,
	.ndo_set_rx_mode		= lan966x_set_rx_mode,
	.ndo_get_phys_port_name		= lan966x_port_get_phys_port_name,
	.ndo_get_stats64		= lan966x_get_stats64,
	.ndo_set_mac_address		= lan966x_port_set_mac_address,
	.ndo_vlan_rx_add_vid		= lan966x_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid		= lan966x_vlan_rx_kill_vid,
	.ndo_set_features		= lan966x_set_features,
	.ndo_get_port_parent_id		= lan966x_get_port_parent_id,
	.ndo_setup_tc			= lan966x_setup_tc,
	.ndo_eth_ioctl			= lan966x_ioctl,
};

bool lan966x_netdevice_check(const struct net_device *dev)
{
	return dev && (dev->netdev_ops == &lan966x_port_netdev_ops);
}

static bool lan966x_snooping_enabled(struct lan966x *lan966x, u32 port)
{
	u32 val;

	/* It is enough to check only IGMP flag because also the MLD is
	 * controlled at the same time
	 */
	val = lan_rd(lan966x, ANA_CPU_FWD_CFG(port));
	if (ANA_CPU_FWD_CFG_IGMP_REDIR_ENA_GET(val))
		return true;

	return false;
}

static bool lan966x_hw_offload(struct lan966x *lan966x, u32 port,
			       struct sk_buff *skb)
{
	/* The IGMP and MLD frames are not forward by the HW if
	 * igmp snooping is enabled, therefor don't mark as
	 * offload to allow the SW to forward the frames accordingly.
	 */
	if (!lan966x_snooping_enabled(lan966x, port))
		return true;

	if (eth_type_vlan(skb->protocol)) {
		skb = skb_vlan_untag(skb);
		if (unlikely(!skb))
			return false;
	}

	if (skb->protocol == htons(ETH_P_IP) &&
	    ip_hdr(skb)->protocol == IPPROTO_IGMP)
		return false;

	if (skb->protocol == htons(ETH_P_IPV6) &&
	    ipv6_addr_is_multicast(&ipv6_hdr(skb)->daddr) &&
	    !ipv6_mc_check_mld(skb))
		return false;

	return true;
}

static int lan966x_ifh_extract(u32 *ifh, size_t pos, size_t length)
{
	int i;
	int val = 0;

	for (i = pos; i < pos + length; ++i)
		val |= ((ifh[IFH_LEN - i / 32 - 1] & BIT(i % 32)) >>
			(i % 32)) << (i - pos);

	return val;
}

static inline int lan966x_parse_ifh(u32 *ifh, struct frame_info *info)
{
	int i;

	/* The IFH is in network order, switch to CPU order */
	for (i = 0; i < IFH_LEN; i++)
		ifh[i] = ntohl((__force __be32)ifh[i]);

	info->len = lan966x_ifh_extract(ifh, IFH_POS_LEN, IFH_WID_LEN);
	info->port = lan966x_ifh_extract(ifh, IFH_POS_SRCPORT, IFH_WID_SRCPORT);

	info->vid = lan966x_ifh_extract(ifh, IFH_POS_TCI, IFH_WID_TCI);
	info->timestamp = lan966x_ifh_extract(ifh, IFH_POS_TIMESTAMP,
					      IFH_WID_TIMESTAMP);
	return 0;
}

static int lan966x_rx_frame_word(struct lan966x *lan966x, u8 grp, u32 *rval)
{
	u32 bytes_valid;
	u32 val;

	val = lan_rd(lan966x, QS_XTR_RD(grp));
	if (val == XTR_NOT_READY) {
		do {
			val = lan_rd(lan966x, QS_XTR_RD(grp));
		} while (val == XTR_NOT_READY);
	}

	switch (val) {
	case XTR_ABORT:
		return -EIO;
	case XTR_EOF_0:
	case XTR_EOF_1:
	case XTR_EOF_2:
	case XTR_EOF_3:
	case XTR_PRUNED:
		bytes_valid = XTR_VALID_BYTES(val);
		val = lan_rd(lan966x, QS_XTR_RD(grp));
		if (val == XTR_ESCAPE)
			*rval = lan_rd(lan966x, QS_XTR_RD(grp));
		else
			*rval = val;

		return bytes_valid;
	case XTR_ESCAPE:
		*rval = lan_rd(lan966x, QS_XTR_RD(grp));

		return 4;
	default:
		*rval = val;

		return 4;
	}
}

static inline int lan966x_data_ready(struct lan966x *lan966x)
{
	return lan_rd(lan966x, QS_XTR_DATA_PRESENT);
}

static irqreturn_t lan966x_xtr_irq_handler(int irq, void *args)
{
	struct lan966x *lan966x = args;
	int i = 0, grp = 0, err = 0;

	if (!(lan_rd(lan966x, QS_XTR_DATA_PRESENT) & BIT(grp)))
		return IRQ_NONE;

	do {
		struct skb_shared_hwtstamps *shhwtstamps;
		u32 ifh[IFH_LEN] = { 0 };
		struct net_device *dev;
		struct frame_info info;
		int sz, len, buf_len;
		struct timespec64 ts;
		struct sk_buff *skb;
		u64 full_ts_in_ns;
		u32 *buf;
		u32 val;

		for (i = 0; i < IFH_LEN; i++) {
			err = lan966x_rx_frame_word(lan966x, grp, &ifh[i]);
			if (err != 4)
				goto recover;
		}

		/* The error needs to be reseted.
		 * In case there is only 1 frame in the queue, then after the
		 * extraction of ifh and of the frame then the while condition
		 * will failed. Then it would check if it is an err but the err
		 * is 4, as set previously. In this case will try to read the
		 * rest of the frames from the queue. And in case only a part of
		 * the frame is in the queue, it would read only that. So next
		 * time when this function is called it would presume would read
		 * initially the ifh but actually will read the rest of the
		 * previous frame. Therfore reset here the error code, meaning
		 * that there is no error with reading the ifh. Then if there is
		 * an error reading the frame the error will be set and then the
		 * check is partially correct.
		 */
		err = 0;

		lan966x_parse_ifh(ifh, &info);
		WARN_ON(info.port >= lan966x->num_phys_ports);

		dev = lan966x->ports[info.port]->dev;
		skb = netdev_alloc_skb(dev, info.len);
		if (unlikely(!skb)) {
			netdev_err(dev, "Unable to allocate sk_buff\n");
			err = -ENOMEM;
			break;
		}
		buf_len = info.len - ETH_FCS_LEN;
		buf = (u32 *)skb_put(skb, buf_len);

		len = 0;
		do {
			sz = lan966x_rx_frame_word(lan966x, grp, &val);
			if (sz < 0) {
				kfree_skb(skb);
				goto recover;
			}

			*buf++ = val;
			len += sz;
		} while (len < buf_len);

		/* Read the FCS */
		sz = lan966x_rx_frame_word(lan966x, grp, &val);
		if (sz < 0) {
			kfree_skb(skb);
			goto recover;
		}

		/* Update the statistics if part of the FCS was read before */
		len -= ETH_FCS_LEN - sz;

		if (unlikely(dev->features & NETIF_F_RXFCS)) {
			buf = (u32 *)skb_put(skb, ETH_FCS_LEN);
			*buf = val;
		}

		if (sz < 0) {
			err = sz;
			break;
		}

		lan966x_ptp_gettime64(&lan966x->ptp_domain[LAN966X_PTP_PORT_DOMAIN].info,
				      &ts);
		info.timestamp = info.timestamp >> 2;
		if (ts.tv_nsec < info.timestamp)
			ts.tv_sec--;
		ts.tv_nsec = info.timestamp;
		full_ts_in_ns = ktime_set(ts.tv_sec, ts.tv_nsec);

		shhwtstamps = skb_hwtstamps(skb);
		shhwtstamps->hwtstamp = full_ts_in_ns;

		skb->protocol = eth_type_trans(skb, dev);

		/* Everything we see on an interface that is in the HW bridge
		 * has already been forwarded
		 */
		if (lan966x->bridge_mask & BIT(info.port) &&
		    lan966x->hw_offload) {
#ifdef CONFIG_NET_SWITCHDEV
			skb->offload_fwd_mark = 1;

			skb_reset_network_header(skb);
			if (!lan966x_hw_offload(lan966x, info.port, skb))
				skb->offload_fwd_mark = 0;
#endif
		}

		if (!skb_defer_rx_timestamp(skb))
			netif_rx(skb);
		dev->stats.rx_bytes += len;
		dev->stats.rx_packets++;

		rx_counters++;

recover:
		if (sz < 0 || err)
			lan_rd(lan966x, QS_XTR_RD(grp));

	} while (lan_rd(lan966x, QS_XTR_DATA_PRESENT) & BIT(grp));

	return IRQ_HANDLED;
}

static int lan966x_xtr_task(void *args)
{
	struct lan966x *lan966x = args;
	int grp = 0, err = 0;

	while (!kthread_should_stop()) {
		u32 val;

		lan966x_xtr_irq_handler(0, lan966x);

		err = readx_poll_timeout(lan966x_data_ready, lan966x, val,
					 val & BIT(grp), 10, 100000);
		if (err == -ETIMEDOUT)
			continue;
	}

	return 0;
}

static irqreturn_t lan966x_ptp_sync_irq_handler(int irq, void *args)
{
	struct lan966x *lan966x = args;

	if (!(lan_rd(lan966x, PTP_PIN_INTR) & BIT(TOD_INPUT)))
		return IRQ_NONE;

	return lan966x_ptp_extts_handle(lan966x, irq);
}

static irqreturn_t lan966x_ptp_irq_handler(int irq, void *args)
{
	int budget = LAN966X_PTP_QUEUE_SZ;
	struct lan966x *lan966x = args;

	while (budget--) {
		struct sk_buff *skb, *skb_tmp, *skb_match = NULL;
		struct skb_shared_hwtstamps shhwtstamps;
		struct lan966x_port *port;
		struct timespec64 ts;
		unsigned long flags;
		u32 val, id, txport;
		u32 delay;

		val = lan_rd(lan966x, PTP_TWOSTEP_CTRL);

		/* Check if a timestamp can be retrieved */
		if (!(val & PTP_TWOSTEP_CTRL_VLD))
			break;

		WARN_ON(val & PTP_TWOSTEP_CTRL_OVFL);

		if (!(val & PTP_TWOSTEP_CTRL_STAMP_TX))
			continue;

		/* Retrieve the ts Tx port */
		txport = PTP_TWOSTEP_CTRL_STAMP_PORT_GET(val);

		/* Retrieve its associated skb */
		port = lan966x->ports[txport];

		/* Retrive the delay */
		delay = lan_rd(lan966x, PTP_TWOSTEP_STAMP);
		delay = PTP_TWOSTEP_STAMP_STAMP_NSEC_GET(delay);

		/* Get next timestamp from fifo, which needs to be the
		 * rx timestamp which represents the id of the frame
		 */
		lan_rmw(PTP_TWOSTEP_CTRL_NXT_SET(1),
			PTP_TWOSTEP_CTRL_NXT,
			lan966x, PTP_TWOSTEP_CTRL);

		val = lan_rd(lan966x, PTP_TWOSTEP_CTRL);

		/* Check if a timestamp can be retried */
		if (!(val & PTP_TWOSTEP_CTRL_VLD))
			break;

		/* Read RX timestamping to get the ID */
		id = lan_rd(lan966x, PTP_TWOSTEP_STAMP);

		spin_lock_irqsave(&port->tx_skbs.lock, flags);
		skb_queue_walk_safe(&port->tx_skbs, skb, skb_tmp) {
			if (skb->cb[0] != id)
				continue;

			__skb_unlink(skb, &port->tx_skbs);
			skb_match = skb;
			break;
		}
		spin_unlock_irqrestore(&port->tx_skbs.lock, flags);

		/* Next ts */
		lan_rmw(PTP_TWOSTEP_CTRL_NXT_SET(1),
			PTP_TWOSTEP_CTRL_NXT,
			lan966x, PTP_TWOSTEP_CTRL);

		if (unlikely(!skb_match))
			continue;

		/* Get the h/w timestamp */
		lan966x_get_hwtimestamp(lan966x, &ts, delay);

		/* Set the timestamp into the skb */
		shhwtstamps.hwtstamp = ktime_set(ts.tv_sec, ts.tv_nsec);
		skb_tstamp_tx(skb_match, &shhwtstamps);

		dev_kfree_skb_any(skb_match);
	}

	return IRQ_HANDLED;
}

static irqreturn_t lan966x_ana_irq_handler(int irq, void *args)
{
	struct lan966x *lan966x = args;

#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	lan966x_mrp_ring_open(lan966x);
	lan966x_mrp_in_open(lan966x);
#endif
#if IS_ENABLED(CONFIG_BRIDGE_CFM)
	lan966x_handle_cfm_interrupt(lan966x);
#endif
	lan966x_mac_irq_handler(lan966x);

	return IRQ_HANDLED;
}

static int lan966x_probe_port(struct lan966x *lan966x, u8 port,
			      phy_interface_t phy_mode,
			      struct fwnode_handle *portnp)
{
	struct lan966x_port *lan966x_port;
	struct phylink *phylink;
	struct net_device *dev;
	int err;

	if (port >= lan966x->num_phys_ports)
		return -EINVAL;

	dev = alloc_etherdev_mqs(sizeof(struct lan966x_port), 8, 1); /* TODO: Use devicetree? */
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, lan966x->dev);
	lan966x_port = netdev_priv(dev);
	lan966x_port->dev = dev;
	lan966x_port->lan966x = lan966x;
	lan966x_port->chip_port = port;
	lan966x_port->pvid = PORT_PVID;
	lan966x->ports[port] = lan966x_port;

	dev->max_mtu = 9600;
	skb_queue_head_init(&lan966x_port->tx_skbs);

	dev->netdev_ops = &lan966x_port_netdev_ops;
	dev->ethtool_ops = &lan966x_ethtool_ops;
#ifdef CONFIG_DCB
	dev->dcbnl_ops = &lan966x_dcbnl_ops;
#endif
	dev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER | NETIF_F_RXFCS |
		NETIF_F_HW_TC;
	dev->features |= NETIF_F_HW_VLAN_CTAG_FILTER | NETIF_F_HW_TC |
		NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_TX;
	dev->priv_flags |= IFF_UNICAST_FLT;
	dev->needed_headroom = IFH_LEN * sizeof(u32);

	eth_hw_addr_gen(dev, lan966x->base_mac, port + 1);

	lan966x_mact_learn(lan966x, PGID_CPU, dev->dev_addr, lan966x_port->pvid,
			   ENTRYTYPE_LOCKED);

	/* Create a phylink for PHY management.  Also handles SFPs */
	lan966x_port->phylink_config.dev = &lan966x_port->dev->dev;
	lan966x_port->phylink_config.type = PHYLINK_NETDEV;
	lan966x_port->phylink_pcs.poll = true;
	lan966x_port->phylink_pcs.ops = &lan966x_phylink_pcs_ops;

	lan966x_port->phylink_config.mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
		MAC_10 | MAC_100 | MAC_1000FD | MAC_2500FD;

	phy_interface_set_rgmii(lan966x_port->phylink_config.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_MII,
		  lan966x_port->phylink_config.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_GMII,
		  lan966x_port->phylink_config.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_SGMII,
		  lan966x_port->phylink_config.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_QSGMII,
		  lan966x_port->phylink_config.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_QUSGMII,
		  lan966x_port->phylink_config.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_1000BASEX,
		  lan966x_port->phylink_config.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX,
		  lan966x_port->phylink_config.supported_interfaces);

	phylink = phylink_create(&lan966x_port->phylink_config,
				 portnp,
				 phy_mode,
				 &lan966x_phylink_mac_ops);
	if (IS_ERR(phylink))
		return PTR_ERR(phylink);

	lan966x_port->phylink = phylink;

	INIT_LIST_HEAD(&lan966x_port->tc.templates);

	err = register_netdev(dev);
	if (err) {
		dev_err(lan966x->dev, "register_netdev failed\n");
		goto err_register_netdev;
	}

	lan966x_vlan_port_apply(lan966x, lan966x_port);
	/* TODO: OLD_VCAP_API */
	/* lan966x_vcap_port_enable(lan966x, lan966x_port); */
	lan966x_qos_port_apply(lan966x, lan966x_port);

	return 0;

err_register_netdev:
	free_netdev(dev);
	return err;
}

static void lan966x_init(struct lan966x *lan966x)
{
	char queue_name[32];
	u32 port, i;

	/* Initialization is done already in PCI driver */

	/* MAC table initialization */
	lan966x_mact_init(lan966x);

	/* VLAN configuration */
	lan966x_vlan_init(lan966x);

	/* Flush queues */
	lan_wr(lan_rd(lan966x, QS_XTR_FLUSH) | GENMASK(1, 0),
	       lan966x, QS_XTR_FLUSH);

	/* Allow to drain */
	mdelay(1);

	/* All Queues normal */
	lan_wr(lan_rd(lan966x, QS_XTR_FLUSH) & ~(GENMASK(1, 0)),
	       lan966x, QS_XTR_FLUSH);

	/* Set MAC age time to default value, the entry is aged after
	 * 2 * AGE_PERIOD
	 */
	lan_wr(ANA_AUTOAGE_AGE_PERIOD_SET(BR_DEFAULT_AGEING_TIME / 2 / HZ),
	       lan966x, ANA_AUTOAGE);

	/* Disable learning for frames discarded by VLAN ingress filtering */
	lan_rmw(ANA_ADVLEARN_VLAN_CHK_SET(1),
		ANA_ADVLEARN_VLAN_CHK,
		lan966x, ANA_ADVLEARN);

	/* Setup frame ageing - "2 sec" - The unit is 6.5 us on lan966x */
	lan_wr(SYS_FRM_AGING_AGE_TX_ENA_SET(1) | (20000000 / 65),
	       lan966x, SYS_FRM_AGING);

	/* Map the 8 CPU extraction queues to CPU port */
	lan_wr(0, lan966x, QSYS_CPU_GROUP_MAP);

	/* Do byte-swap and expect status after last data word
	 * Extraction: Mode: manual extraction) | Byte_swap
	 */
	lan_wr(QS_XTR_GRP_CFG_MODE_SET(lan966x->use_dma || lan966x->use_napi ? 2 : 1) |
	       QS_XTR_GRP_CFG_BYTE_SWAP_SET(1),
	       lan966x, QS_XTR_GRP_CFG(0));

	/* Injection: Mode: manual injection | Byte_swap */
	lan_wr(QS_INJ_GRP_CFG_MODE_SET(lan966x->use_dma || lan966x->use_napi ? 2 : 1) |
	       QS_INJ_GRP_CFG_BYTE_SWAP_SET(1),
	       lan966x, QS_INJ_GRP_CFG(0));

	lan_rmw(QS_INJ_CTRL_GAP_SIZE_SET(0),
		QS_INJ_CTRL_GAP_SIZE,
		lan966x, QS_INJ_CTRL(0));

	/* Enable IFH insertion/parsing on CPU ports */
	lan_wr(SYS_PORT_MODE_INCL_INJ_HDR_SET(1) |
	       SYS_PORT_MODE_INCL_XTR_HDR_SET(1),
	       lan966x, SYS_PORT_MODE(CPU_PORT));

	/* Setup flooding PGIDs */
	lan_wr(ANA_FLOODING_IPMC_FLD_MC4_DATA_SET(PGID_MCIPV4) |
	       ANA_FLOODING_IPMC_FLD_MC4_CTRL_SET(PGID_MC) |
	       ANA_FLOODING_IPMC_FLD_MC6_DATA_SET(PGID_MCIPV6) |
	       ANA_FLOODING_IPMC_FLD_MC6_CTRL_SET(PGID_MC),
	       lan966x, ANA_FLOODING_IPMC);

	/* There are 8 priorities */
	for (i = 0; i < 8; ++i)
		lan_rmw(ANA_FLOODING_FLD_MULTICAST_SET(PGID_MC) |
			ANA_FLOODING_FLD_UNICAST_SET(PGID_UC) |
			ANA_FLOODING_FLD_BROADCAST_SET(PGID_BC),
			ANA_FLOODING_FLD_MULTICAST |
			ANA_FLOODING_FLD_UNICAST |
			ANA_FLOODING_FLD_BROADCAST,
			lan966x, ANA_FLOODING(i));

	for (port = 0; port < PGID_ENTRIES; ++port)
		/* Set all the entries to obey VLAN_VLAN */
		lan_rmw(ANA_PGID_CFG_OBEY_VLAN_SET(1),
			ANA_PGID_CFG_OBEY_VLAN,
			lan966x, ANA_PGID_CFG(port));

	for (port = 0; port < lan966x->num_phys_ports; port++) {
		/* Disable bridging by default */
		lan_rmw(ANA_PGID_PGID_SET(0x0),
			ANA_PGID_PGID,
			lan966x, ANA_PGID(port + PGID_SRC));

		/* Do not forward BPDU frames to the front ports and copy them
		 * to CPU
		 */
		lan_wr(0xffff, lan966x, ANA_CPU_FWD_BPDU_CFG(port));
	}

	/* Set source buffer size for each priority and each port to 1500 bytes */
	for (i = 0; i <= 95; ++i) {
		lan_wr(1500 / 64, lan966x, QSYS_RES_CFG(i));
		lan_wr(1500 / 64, lan966x, QSYS_RES_CFG(512 + i));
	}

	lan966x->bridge_mask = 0;

	/* Enable switching to/from cpu port */
	lan_wr(QSYS_SW_PORT_MODE_PORT_ENA_SET(1) |
	       QSYS_SW_PORT_MODE_SCH_NEXT_CFG_SET(1) |
	       QSYS_SW_PORT_MODE_INGRESS_DROP_MODE_SET(1),
	       lan966x, QSYS_SW_PORT_MODE(CPU_PORT));

	/* Configure and enable the CPU port */
	lan_rmw(ANA_PGID_PGID_SET(0),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(CPU_PORT));
	lan_rmw(ANA_PGID_PGID_SET(BIT(CPU_PORT)),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(PGID_CPU));

	/* This will be controlled by mrouter ports */
	lan_rmw(GENMASK(lan966x->num_phys_ports - 1, 0),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(PGID_MCIPV4));

	lan_rmw(GENMASK(lan966x->num_phys_ports - 1, 0),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(PGID_MCIPV6));

	/* Multicast to the CPU port and to other ports */
	lan_rmw(ANA_PGID_PGID_SET(BIT(CPU_PORT) | GENMASK(lan966x->num_phys_ports - 1, 0)),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(PGID_MC));

	/* Unicast to all other ports */
	lan_rmw(GENMASK(lan966x->num_phys_ports - 1, 0),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(PGID_UC));

	/* Broadcast to the CPU port and to other ports */
	lan_rmw(ANA_PGID_PGID_SET(BIT(CPU_PORT) | GENMASK(lan966x->num_phys_ports - 1, 0)),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(PGID_BC));

	/* Set the priority queues for different frame types */
	lan_wr(ANA_CPUQ_CFG_CPUQ_MLD_SET(5) |
	       ANA_CPUQ_CFG_CPUQ_IGMP_SET(5) |
	       ANA_CPUQ_CFG_CPUQ_IPMC_CTRL_SET(5) |
	       ANA_CPUQ_CFG_CPUQ_ALLBRIDGE_SET(6) |
	       ANA_CPUQ_CFG_CPUQ_LOCKED_PORTMOVE_SET(2) |
	       ANA_CPUQ_CFG_CPUQ_SRC_COPY_SET(2) |
	       ANA_CPUQ_CFG_CPUQ_MAC_COPY_SET(2) |
	       ANA_CPUQ_CFG_CPUQ_LRN_SET(2) |
	       ANA_CPUQ_CFG_CPUQ_MIRROR_SET(2) |
	       ANA_CPUQ_CFG_CPUQ_SFLOW_SET(2),
	       lan966x, ANA_CPUQ_CFG);
	for (i = 0; i < 16; ++i) {
		lan_wr(ANA_CPUQ_8021_CFG_CPUQ_GARP_VAL_SET(6) |
		       ANA_CPUQ_8021_CFG_CPUQ_BPDU_VAL_SET(6),
		       lan966x, ANA_CPUQ_8021_CFG(i));
	}

	lan_wr(REW_PORT_CFG_NO_REWRITE_SET(1), lan966x, REW_PORT_CFG(CPU_PORT));

	/* Init ptp */
	lan966x_timestamp_init(lan966x);

	/* Init netlink */
	lan966x_netlink_fp_init();
	lan966x_netlink_frer_init(lan966x);
	lan966x_netlink_qos_init(lan966x);

	/* Init stats worker */
	mutex_init(&lan966x->stats_lock);
	snprintf(queue_name, sizeof(queue_name), "%s-stats",
		 dev_name(lan966x->dev));
	lan966x->stats_queue = create_singlethread_workqueue(queue_name);
	INIT_DELAYED_WORK(&lan966x->stats_work, lan966x_check_stats_work);
	queue_delayed_work(lan966x->stats_queue, &lan966x->stats_work,
			   LAN966X_STATS_CHECK_DELAY);

	/* Init mact_sw struct */
	spin_lock_init(&lan966x->mact_lock);
	INIT_LIST_HEAD(&lan966x->mact_entries);

	/* If the HW can't get interrupts regarding the learn MAC addresses
	 * then continue with manual polling, otherwise just enable the
	 * interrupt.
	 */
	if (lan966x->ana_poll) {
		snprintf(queue_name, sizeof(queue_name), "%s-mact",
			 dev_name(lan966x->dev));
		lan966x->mact_queue = create_singlethread_workqueue(queue_name);
		INIT_DELAYED_WORK(&lan966x->mact_work, lan966x_mact_pull_work);
		queue_delayed_work(lan966x->mact_queue, &lan966x->mact_work,
				   LAN966X_MACT_PULL_DELAY);
	}

	lan_rmw(ANA_ANAINTR_INTR_ENA_SET(1),
		ANA_ANAINTR_INTR_ENA,
		lan966x, ANA_ANAINTR);

	/* Take out the 2 internal phys from reset */
#if defined(SUNRISE) || defined(ASIC)
	lan_rmw(CHIP_TOP_CUPHY_COMMON_CFG_RESET_N_SET(1),
	        CHIP_TOP_CUPHY_COMMON_CFG_RESET_N,
	        lan966x, CHIP_TOP_CUPHY_COMMON_CFG);

#endif
}

static const struct lan966x_stat_layout lan966x_stats_layout[] = {
	{ .name = "rx_octets", .offset = 0x00, },
	{ .name = "rx_unicast", .offset = 0x01, },
	{ .name = "rx_multicast", .offset = 0x02 },
	{ .name = "rx_broadcast", .offset = 0x03 },
	{ .name = "rx_short", .offset = 0x04 },
	{ .name = "rx_frag", .offset = 0x05 },
	{ .name = "rx_jabber", .offset = 0x06 },
	{ .name = "rx_crc", .offset = 0x07 },
	{ .name = "rx_symbol_err", .offset = 0x08 },
	{ .name = "rx_sz_64", .offset = 0x09 },
	{ .name = "rx_sz_65_127", .offset = 0x0a},
	{ .name = "rx_sz_128_255", .offset = 0x0b},
	{ .name = "rx_sz_256_511", .offset = 0x0c },
	{ .name = "rx_sz_512_1023", .offset = 0x0d },
	{ .name = "rx_sz_1024_1526", .offset = 0x0e },
	{ .name = "rx_sz_jumbo", .offset = 0x0f },
	{ .name = "rx_pause", .offset = 0x10 },
	{ .name = "rx_control", .offset = 0x11 },
	{ .name = "rx_long", .offset = 0x12 },
	{ .name = "rx_cat_drop", .offset = 0x13 },
	{ .name = "rx_red_prio_0", .offset = 0x14 },
	{ .name = "rx_red_prio_1", .offset = 0x15 },
	{ .name = "rx_red_prio_2", .offset = 0x16 },
	{ .name = "rx_red_prio_3", .offset = 0x17 },
	{ .name = "rx_red_prio_4", .offset = 0x18 },
	{ .name = "rx_red_prio_5", .offset = 0x19 },
	{ .name = "rx_red_prio_6", .offset = 0x1a },
	{ .name = "rx_red_prio_7", .offset = 0x1b },
	{ .name = "rx_yellow_prio_0", .offset = 0x1c },
	{ .name = "rx_yellow_prio_1", .offset = 0x1d },
	{ .name = "rx_yellow_prio_2", .offset = 0x1e },
	{ .name = "rx_yellow_prio_3", .offset = 0x1f },
	{ .name = "rx_yellow_prio_4", .offset = 0x20 },
	{ .name = "rx_yellow_prio_5", .offset = 0x21 },
	{ .name = "rx_yellow_prio_6", .offset = 0x22 },
	{ .name = "rx_yellow_prio_7", .offset = 0x23 },
	{ .name = "rx_green_prio_0", .offset = 0x24 },
	{ .name = "rx_green_prio_1", .offset = 0x25 },
	{ .name = "rx_green_prio_2", .offset = 0x26 },
	{ .name = "rx_green_prio_3", .offset = 0x27 },
	{ .name = "rx_green_prio_4", .offset = 0x28 },
	{ .name = "rx_green_prio_5", .offset = 0x29 },
	{ .name = "rx_green_prio_6", .offset = 0x2a },
	{ .name = "rx_green_prio_7", .offset = 0x2b },
	{ .name = "rx_assembly_err", .offset = 0x2c },
	{ .name = "rx_smd_err", .offset = 0x2d },
	{ .name = "rx_assembly_ok", .offset = 0x2e },
	{ .name = "rx_merge_frag", .offset = 0x2f },
	{ .name = "rx_pmac_octets", .offset = 0x30, },
	{ .name = "rx_pmac_unicast", .offset = 0x31, },
	{ .name = "rx_pmac_multicast", .offset = 0x32 },
	{ .name = "rx_pmac_broadcast", .offset = 0x33 },
	{ .name = "rx_pmac_short", .offset = 0x34 },
	{ .name = "rx_pmac_frag", .offset = 0x35 },
	{ .name = "rx_pmac_jabber", .offset = 0x36 },
	{ .name = "rx_pmac_crc", .offset = 0x37 },
	{ .name = "rx_pmac_symbol_err", .offset = 0x38 },
	{ .name = "rx_pmac_sz_64", .offset = 0x39 },
	{ .name = "rx_pmac_sz_65_127", .offset = 0x3a },
	{ .name = "rx_pmac_sz_128_255", .offset = 0x3b },
	{ .name = "rx_pmac_sz_256_511", .offset = 0x3c },
	{ .name = "rx_pmac_sz_512_1023", .offset = 0x3d },
	{ .name = "rx_pmac_sz_1024_1526", .offset = 0x3e },
	{ .name = "rx_pmac_sz_jumbo", .offset = 0x3f },
	{ .name = "rx_pmac_pause", .offset = 0x40 },
	{ .name = "rx_pmac_control", .offset = 0x41 },
	{ .name = "rx_pmac_long", .offset = 0x42 },

	{ .name = "tx_octets", .offset = 0x80, },
	{ .name = "tx_unicast", .offset = 0x81, },
	{ .name = "tx_multicast", .offset = 0x82 },
	{ .name = "tx_broadcast", .offset = 0x83 },
	{ .name = "tx_col", .offset = 0x84 },
	{ .name = "tx_drop", .offset = 0x85 },
	{ .name = "tx_pause", .offset = 0x86 },
	{ .name = "tx_sz_64", .offset = 0x87 },
	{ .name = "tx_sz_65_127", .offset = 0x88 },
	{ .name = "tx_sz_128_255", .offset = 0x89 },
	{ .name = "tx_sz_256_511", .offset = 0x8a },
	{ .name = "tx_sz_512_1023", .offset = 0x8b },
	{ .name = "tx_sz_1024_1526", .offset = 0x8c },
	{ .name = "tx_sz_jumbo", .offset = 0x8d },
	{ .name = "tx_yellow_prio_0", .offset = 0x8e },
	{ .name = "tx_yellow_prio_1", .offset = 0x8f },
	{ .name = "tx_yellow_prio_2", .offset = 0x90 },
	{ .name = "tx_yellow_prio_3", .offset = 0x91 },
	{ .name = "tx_yellow_prio_4", .offset = 0x92 },
	{ .name = "tx_yellow_prio_5", .offset = 0x93 },
	{ .name = "tx_yellow_prio_6", .offset = 0x94 },
	{ .name = "tx_yellow_prio_7", .offset = 0x95 },
	{ .name = "tx_green_prio_0", .offset = 0x96 },
	{ .name = "tx_green_prio_1", .offset = 0x97 },
	{ .name = "tx_green_prio_2", .offset = 0x98 },
	{ .name = "tx_green_prio_3", .offset = 0x99 },
	{ .name = "tx_green_prio_4", .offset = 0x9a },
	{ .name = "tx_green_prio_5", .offset = 0x9b },
	{ .name = "tx_green_prio_6", .offset = 0x9c },
	{ .name = "tx_green_prio_7", .offset = 0x9d },
	{ .name = "tx_aged", .offset = 0x9e },
	{ .name = "tx_llct", .offset = 0x9f },
	{ .name = "tx_ct", .offset = 0xa0 },
	{ .name = "tx_mm_hold", .offset = 0xa1 },
	{ .name = "tx_merge_frag", .offset = 0xa2 },
	{ .name = "tx_pmac_octets", .offset = 0xa3, },
	{ .name = "tx_pmac_unicast", .offset = 0xa4, },
	{ .name = "tx_pmac_multicast", .offset = 0xa5 },
	{ .name = "tx_pmac_broadcast", .offset = 0xa6 },
	{ .name = "tx_pmac_pause", .offset = 0xa7 },
	{ .name = "tx_pmac_sz_64", .offset = 0xa8 },
	{ .name = "tx_pmac_sz_65_127", .offset = 0xa9 },
	{ .name = "tx_pmac_sz_128_255", .offset = 0xaa },
	{ .name = "tx_pmac_sz_256_511", .offset = 0xab },
	{ .name = "tx_pmac_sz_512_1023", .offset = 0xac },
	{ .name = "tx_pmac_sz_1024_1526", .offset = 0xad },
	{ .name = "tx_pmac_sz_jumbo", .offset = 0xae },

	{ .name = "dr_local", .offset = 0x100 },
	{ .name = "dr_tail", .offset = 0x101 },
	{ .name = "dr_yellow_prio_0", .offset = 0x102 },
	{ .name = "dr_yellow_prio_1", .offset = 0x103 },
	{ .name = "dr_yellow_prio_2", .offset = 0x104 },
	{ .name = "dr_yellow_prio_3", .offset = 0x105 },
	{ .name = "dr_yellow_prio_4", .offset = 0x106 },
	{ .name = "dr_yellow_prio_5", .offset = 0x107 },
	{ .name = "dr_yellow_prio_6", .offset = 0x108 },
	{ .name = "dr_yellow_prio_7", .offset = 0x109 },
	{ .name = "dr_green_prio_0", .offset = 0x10a },
	{ .name = "dr_green_prio_1", .offset = 0x10b },
	{ .name = "dr_green_prio_2", .offset = 0x10c },
	{ .name = "dr_green_prio_3", .offset = 0x10d },
	{ .name = "dr_green_prio_4", .offset = 0x10e },
	{ .name = "dr_green_prio_5", .offset = 0x10f },
	{ .name = "dr_green_prio_6", .offset = 0x110 },
	{ .name = "dr_green_prio_7", .offset = 0x111 },
};

static void lan966x_init_rx_request(struct lan966x *lan966x,
				    struct lan966x_rx_request *req,
				    int size)
{
	struct scatterlist *sg;
	dma_addr_t phys;
	int idx;

	pr_debug("%s:%d %s: rx request: 0x%px\n",
		 __FILE__, __LINE__, __func__,
		 req);

	req->lan966x = lan966x;
	req->cookie = 0;
	sg_init_table(req->sgl, FDMA_XTR_BUFFER_COUNT);
	for_each_sg(req->sgl, sg, FDMA_XTR_BUFFER_COUNT, idx) {
		req->buffer[idx] = dma_pool_zalloc(lan966x->rx_pool, GFP_KERNEL,
						   &phys);
		sg_dma_address(sg) = phys;
		sg_dma_len(sg) = size;

		pr_debug("%s:%d %s: buffer[%02u]: 0x%llx\n",
			 __FILE__, __LINE__, __func__,
			 idx, (u64)phys);
	}
}

static void lan966x_init_iterator(struct request_iterator *iter, int idx,
				  struct lan966x_rx_request *req)
{
	iter->idx = idx;
	iter->req = req;

	if (idx >= req->fill_level) {
		iter->idx = idx % req->fill_level;
		iter->req = list_next_entry(iter->req, node);
	}

	pr_debug("%s:%d %s: [C%u,I%u]\n", __FILE__, __LINE__, __func__,
		 iter->req->cookie, iter->idx);
}

static struct lan966x_rx_request *next_block(struct request_iterator *iter)
{
	struct lan966x_rx_request *req = NULL;

	iter->idx++;
	if (iter->idx == iter->req->fill_level) {
		req = iter->req;
		iter->idx = 0;
		iter->req = list_next_entry(iter->req, node);
	}

	pr_debug("%s:%d %s: [C%u,I%u], req: %u\n", __FILE__, __LINE__, __func__,
		 iter->req->cookie, iter->idx, req != NULL);

	return req;
}

static bool lan966x_reached(struct request_iterator *iter,
			    struct request_iterator *max)
{
	pr_debug("%s:%d %s: %u\n", __FILE__, __LINE__, __func__,
		iter->req == max->req && iter->idx == max->idx);

	return iter->req == max->req && iter->idx == max->idx;
}

static void *lan966x_get_block_data(struct lan966x *lan966x,
				    struct request_iterator *iter)
{
	pr_debug("%s:%d %s: [C%u,I%u]: 0x%px\n",
		__FILE__, __LINE__, __func__,
		iter->req->cookie, iter->idx,
		iter->req->buffer[iter->idx]);

	return iter->req->buffer[iter->idx];
}

static struct sk_buff *lan966x_create_receive_skb(
	struct lan966x *lan966x, struct request_iterator *iter,
	struct request_iterator *max, int size, int block_bytes)
{
	struct skb_shared_hwtstamps *shhwtstamps;
	struct lan966x_rx_request *done_req;
	struct frame_info info = { 0 };
	struct sk_buff *skb = 0;
	struct timespec64 ts;
	void *data = NULL;
	u64 full_ts_in_ns;
	int block_size;
	u8 *skbdata;

	skb = dev_alloc_skb(block_bytes);
	if (!skb) {
		pr_err("%s:%d %s: no skb: %u bytes\n",
		       __FILE__, __LINE__, __func__, block_bytes);
		return NULL;
	}

	skbdata = skb->data;
	skb_put(skb, size);
	pr_debug("%s:%d %s: skb: len: %d, data: 0x%px\n",
		 __FILE__, __LINE__, __func__, skb->len, skb->data);

	while (!lan966x_reached(iter, max)) {
		data = lan966x_get_block_data(lan966x, iter);
		block_size = min(size, FDMA_XTR_BUFFER_SIZE);

		pr_debug("%s:%d %s: copy: len: %d, data: 0x%px\n",
			 __FILE__, __LINE__, __func__,
			 block_size, data);

		memcpy(skbdata, data, block_size);
		done_req = next_block(iter);
		if (done_req) {
			pr_debug("%s:%d %s: done: [C:%u]\n",
				 __FILE__, __LINE__, __func__,
				 done_req->cookie);
			list_move_tail(&done_req->node, &lan966x->free_rx_reqs);
		}
		skbdata += FDMA_XTR_BUFFER_SIZE;
		size -= block_size;
	}

	if (!data)
		pr_err("%s:%d %s: did not copy: [C:%u,I%u]\n",
			 __FILE__, __LINE__, __func__,
			 iter->req->cookie, iter->idx);

	lan966x_parse_ifh((u32*)skb->data, &info);
	skb->dev = lan966x->ports[info.port]->dev;
	skb_pull(skb, IFH_LEN * sizeof(u32));

	if (likely(!(skb->dev->features & NETIF_F_RXFCS)))
		skb_trim(skb, skb->len - ETH_FCS_LEN);

	lan966x_ptp_gettime64(&lan966x->ptp_domain[LAN966X_PTP_PORT_DOMAIN].info,
			      &ts);
	info.timestamp = info.timestamp >> 2;
	if (ts.tv_nsec < info.timestamp)
		ts.tv_sec--;
	ts.tv_nsec = info.timestamp;
	full_ts_in_ns = ktime_set(ts.tv_sec, ts.tv_nsec);

	shhwtstamps = skb_hwtstamps(skb);
	shhwtstamps->hwtstamp = full_ts_in_ns;

	/* Everything we see on an interface that is in the HW bridge
	 * has already been forwarded
	 */
	if (lan966x->bridge_mask & BIT(info.port) &&
	    lan966x->hw_offload) {
#ifdef CONFIG_NET_SWITCHDEV
		skb->offload_fwd_mark = 1;

		skb_reset_network_header(skb);
		if (!lan966x_hw_offload(lan966x, info.port, skb))
			skb->offload_fwd_mark = 0;
#endif
	}

	return skb;
}

static void lan966x_receive_cb(void *data,
			       const struct dmaengine_result *result)
{
	struct lan966x_rx_request *req = data;
	struct request_iterator next;
	struct request_iterator cur;
	struct lan966x *lan966x;
	struct sk_buff *skb;
	int used_blocks;
	int packet_size;
	int next_sof;

	pr_debug("%s:%d %s: result: %u, residue: %u\n",
		 __FILE__, __LINE__, __func__,
		 result->result, result->residue);

	if (!req) {
		pr_err("%s:%d %s: no request\n",
		       __FILE__, __LINE__, __func__);
		return;
	}

	lan966x = req->lan966x;

	/* Get the packet size (includes IFH and FCS) */
	packet_size = result->residue;
	used_blocks = DIV_ROUND_UP(packet_size, FDMA_XTR_BUFFER_SIZE);
	next_sof = req->idx + used_blocks;
	lan966x_init_iterator(&cur, req->idx, req);
	lan966x_init_iterator(&next, next_sof, req);

	pr_debug("%s:%d %s: from: [C%u,I%u] to: [C%u,I%u]  size: %u, blocks: %u\n",
		 __FILE__, __LINE__, __func__, cur.req->cookie, cur.idx,
		 next.req->cookie, next.idx, packet_size, used_blocks);

	if (req->idx == 0) {
		struct lan966x_rx_request *prev = list_prev_entry(req, node);
		static int cookie = 0;

		if (prev->idx != req->fill_level) {
			if (cookie != prev->cookie) {
				pr_err("%s:%d %s: going from: [C%u,I%u] to: [C%u,I%u]\n",
				       __FILE__, __LINE__, __func__,
				       prev->cookie,
				       prev->idx,
				       req->cookie,
				       req->idx);
				cookie = prev->cookie;
			}
		}
	}

	if (result->result != DMA_TRANS_NOERROR)
		goto err;

	if (used_blocks == 0)
		goto err;

	skb = lan966x_create_receive_skb(lan966x, &cur, &next, packet_size,
					 used_blocks * FDMA_XTR_BUFFER_SIZE);
	if (skb) {
		pr_debug("%s:%d %s: skb: len: %d, data: 0x%px\n",
			 __FILE__, __LINE__, __func__, skb->len, skb->data);

		skb->protocol = eth_type_trans(skb, skb->dev);
		pr_debug("%s:%d %s: skb: len: %d, data: 0x%px, used_blocks: %u\n",
			 __FILE__, __LINE__, __func__, skb->len, skb->data,
			 used_blocks);

		/* Save state for next packet */
		next.req->idx = next.idx;
	} else {
		pr_err("%s:%d %s: could not create skb: [C%u,I%u]"
		       "result: %u, size: %u\n", __FILE__, __LINE__, __func__,
		       cur.req->cookie, cur.idx, result->result,
		       result->residue);
		goto err;
	}

	/* Prepare more buffers */
	lan966x_prepare_rx_request(lan966x);

	rx_counters++;
	if (!skb_defer_rx_timestamp(skb))
		netif_rx(skb);
	return;

err:
	pr_err("%s:%d %s: error: %u, [C%u,I%u]\n",
	       __FILE__, __LINE__, __func__,
	       result->result, next.req->cookie, next.idx);
	/* Save state for next packet */
	req->idx = next.idx;
	/* TODO: count unreceived bytes/packet */
}

static bool lan966x_prepare_rx_request(struct lan966x *lan966x)
{
	struct dma_async_tx_descriptor *txd;
	struct lan966x_rx_request *req;

	while (true) {
		req = list_first_entry_or_null(&lan966x->free_rx_reqs,
					       struct lan966x_rx_request,
					       node);
		if (!req)
			return false;

		pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);

		req->cookie = 0;
		req->idx = 0;
		req->fill_level = lan966x->rx_req_fill_level;
		txd = dmaengine_prep_slave_sg(lan966x->rxdma, req->sgl,
					      req->fill_level,
					      DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);

		if (!txd) {
			dev_err(lan966x->dev, "Could not get RX Descriptor\n");
			goto error;
		}

		txd->callback_param = req;
		txd->callback_result = lan966x_receive_cb;
		req->cookie = dmaengine_submit(txd);

		if (req->cookie < DMA_MIN_COOKIE) {
			dev_err(lan966x->dev, "Submit failed\n");
			goto error;
		}

		pr_debug("%s:%d %s: Issue: txd: 0x%px, C%u, Submitted: %d\n",
			 __FILE__, __LINE__, __func__,
			 txd,
			 txd->cookie,
			 req->cookie);
		dma_async_issue_pending(lan966x->rxdma);
		list_move_tail(&req->node, &lan966x->rx_reqs);
	}

	return true;

error:
	pr_err("%s:%d %s: error\n", __FILE__, __LINE__, __func__);
	return false;
}

static void lan966x_transmit_cb(void *data,
				const struct dmaengine_result *result)
{
	struct lan966x_tx_request *req = data;
	struct dma_tx_state state;
	struct lan966x *lan966x;
	enum dma_status status;

	pr_debug("%s:%d %s: result: %u, residue: %u\n",
		 __FILE__, __LINE__, __func__, result->result, result->residue);

	if (!req) {
		pr_err("%s:%d %s: no request\n",
		       __FILE__, __LINE__, __func__);
		return;
	}

	lan966x = req->lan966x;
	spin_lock(&lan966x->tx_lock);

	if (result->result != DMA_TRANS_NOERROR) {
		pr_err("%s:%d %s: error: %u, [C%u]\n",
			__FILE__, __LINE__, __func__,
			result->result, req->cookie);
	} else {
		status = dmaengine_tx_status(lan966x->txdma, req->cookie, &state);
		pr_debug("%s:%d %s: status %d, state: last: %u, used: %u, "
			 "residue: %u\n",
			__FILE__, __LINE__, __func__,
			status, state.last, state.used, state.residue);
		/* TODO: req->size should cover the whole thing */
	}
	lan966x_close_tx_request(lan966x, req);
	spin_unlock(&lan966x->tx_lock);
}

static struct lan966x_tx_request *
lan966x_prepare_tx_request(struct lan966x *lan966x, struct sk_buff *skb,
			   struct frame_info *info)
{
	struct lan966x_tx_request *req;
	u32 ifh[IFH_LEN] = { 0 };
	struct scatterlist *sg;
	unsigned int size;
	dma_addr_t phys;
	void *buffer;
	int idx = 0;
	int fidx;
	int blocks;

	size = skb->len + IFH_LEN * 4 + 4; /* Includes the IFH and FCS */
	buffer = dma_alloc_coherent(lan966x->txdma->device->dev, size,
				    &phys, GFP_ATOMIC);
	if (!buffer)
		return NULL;

	lan966x_gen_ifh(ifh, info, lan966x);
	for (idx = 0; idx < IFH_LEN; ++idx)
		ifh[idx] = (__force u32)cpu_to_be32(ifh[idx]);

	idx = 0;

	blocks = skb_shinfo(skb)->nr_frags + 1;
	pr_debug("%s:%d %s: skb: frags: %d, size: %u, headsize: %u\n",
		__FILE__, __LINE__, __func__,
		skb_shinfo(skb)->nr_frags,
		skb->len,
		skb_headlen(skb));

	if (blocks > SGL_MAX) {
		pr_err("%s:%d %s: too many blocks\n",
		       __FILE__, __LINE__, __func__);
		return NULL;
	}

	req = list_first_entry_or_null(&lan966x->free_tx_reqs,
				       struct lan966x_tx_request, node);
	if (!req) {
		return NULL;
	}

	memset(req->sgl, 0, sizeof(req->sgl));
	memset(req->buffer, 0, sizeof(req->buffer));
	req->lan966x = lan966x;
	req->cookie = 0;
	req->size = skb->len + IFH_LEN * 4 + 4;
	req->blocks = blocks;
	sg_init_table(req->sgl, blocks);
	sg = req->sgl;

	memcpy(buffer, ifh, IFH_LEN * 4);
	memcpy(buffer + IFH_LEN * 4, skb->data, size - IFH_LEN * 4 + 4);
	sg_dma_address(sg) = phys;
	sg_dma_len(sg) = size;
	req->buffer[idx] = buffer;
	sg = sg_next(sg);

	/* Add SKB fragments if available */
	idx++;
	for (fidx = 0; fidx < skb_shinfo(skb)->nr_frags; fidx++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[fidx];

		size = skb_frag_size(frag);
		if (!size) {
			sg_dma_address(sg) = 0;
			sg_dma_len(sg) = 0;
			continue;
		}

		buffer = dma_alloc_coherent(lan966x->txdma->device->dev, size,
					    &phys, GFP_ATOMIC);

		memcpy(buffer, frag, size);
		sg_dma_address(sg) = (dma_addr_t)phys;
		sg_dma_len(sg) = size;
		req->buffer[idx] = buffer;
		idx++;
		sg = sg_next(sg);
	}
	list_move_tail(&req->node, &lan966x->tx_reqs);

	return req;
}

static void lan966x_close_tx_request(struct lan966x *lan966x,
				     struct lan966x_tx_request *req)
{
	struct scatterlist *sg;
	int idx;

	pr_debug("%s:%d %s: [C%u]\n",
		__FILE__, __LINE__, __func__, req->cookie);

	for_each_sg(req->sgl, sg, req->blocks, idx) {
		pr_debug("%s:%d %s: %u [C%u] %u 0x%px 0x%llx\n",
			__FILE__, __LINE__, __func__,
			idx,
			req->cookie,
			sg_dma_len(sg),
			req->buffer[idx],
			(u64)sg_dma_address(sg));

		dma_free_coherent(lan966x->txdma->device->dev,
				  sg_dma_len(sg),
				  req->buffer[idx],
				  sg_dma_address(sg));
	}

	list_move_tail(&req->node, &lan966x->free_tx_reqs);
}

static int lan966x_fdma_xmit(struct sk_buff *skb,
			     struct frame_info *info,
			     struct net_device *dev)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct dma_async_tx_descriptor *txd;
	struct lan966x_tx_request *req;
	int budget = 10;

	spin_lock(&lan966x->tx_lock);
	do {
		req = lan966x_prepare_tx_request(lan966x, skb, info);
	} while (req == NULL && budget--);

	if (!req) {
		spin_unlock(&lan966x->tx_lock);
		return NETDEV_TX_BUSY;
	}

	txd = dmaengine_prep_slave_sg(lan966x->txdma, req->sgl, req->blocks,
				      DMA_MEM_TO_DEV, 0);

	if (!txd) {
		dev_err(lan966x->dev, "Could not get TX Descriptor\n");
		goto error;
	}

	txd->callback_param = req;
	txd->callback_result = lan966x_transmit_cb;
	req->cookie = dmaengine_submit(txd);
	if (req->cookie < DMA_MIN_COOKIE) {
		dev_err(lan966x->dev, "Submit failed\n");
		goto error;
	}

	skb_tx_timestamp(skb);
	lan966x_ptp_2step_save(port, info, shinfo, skb);

	dma_async_issue_pending(lan966x->txdma);

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	if (shinfo->tx_flags & SKBTX_HW_TSTAMP &&
	    info->rew_op == IFH_REW_OP_TWO_STEP_PTP) {
		u32 val = 0;
		int err;

		if (!lan966x->ptp_poll)
			goto skip_polling;

		err = readx_poll_timeout_atomic(lan966x_ts_fifo_ready, lan966x,
						val, val, 10, 100000);
		if (err == -ETIMEDOUT) {
			pr_info("Ts fifo no valid value\n");
			goto skip_polling;
		}

		lan966x_ptp_irq_handler(0, lan966x);

skip_polling:
		spin_unlock(&lan966x->tx_lock);
		return NETDEV_TX_OK;
	}

	dev_kfree_skb_any(skb);

	spin_unlock(&lan966x->tx_lock);
	return NETDEV_TX_OK;

error:
	pr_err("%s:%d %s: error, close request\n", __FILE__, __LINE__, __func__);
	lan966x_close_tx_request(lan966x, req);

	spin_unlock(&lan966x->tx_lock);
	return -1;
}

static int lan966x_parse_delays(struct lan966x *lan966x, int port_index,
				struct fwnode_handle *port)
{
	struct lan966x_port *lan966x_port = lan966x->ports[port_index];
	struct fwnode_handle *delay;
	int err;

	INIT_LIST_HEAD(&lan966x_port->path_delays);

	fwnode_for_each_available_child_node(port, delay) {
		struct lan966x_path_delay *path_delay;
		s32 tx_delay;
		s32 rx_delay;
		u32 speed;

		err = fwnode_property_read_u32(delay, "speed", &speed);
		if (err)
			return err;

		err = fwnode_property_read_u32(delay, "rx_delay", &rx_delay);
		if (err)
			return err;

		err = fwnode_property_read_u32(delay, "tx_delay", &tx_delay);
		if (err)
			return err;

		path_delay = devm_kzalloc(&lan966x_port->dev->dev,
					  sizeof(*path_delay), GFP_KERNEL);
		if (!path_delay)
			return -ENOMEM;

		path_delay->rx_delay = rx_delay;
		path_delay->tx_delay = tx_delay;
		path_delay->speed = speed;
		list_add_tail(&path_delay->list, &lan966x_port->path_delays);
	}

	return 0;
}

#if defined(SUNRISE) || defined(ASIC)
static inline int lan966x_ram_init(struct lan966x *lan966x)
{
	return lan_rd(lan966x, SYS_RAM_INIT);
}

static inline int lan966x_reset_switch(struct lan966x *lan966x)
{
	struct reset_control *reset;
	int val = 0;
	int ret;

	reset = devm_reset_control_get_shared(lan966x->dev, "switch");
	if (IS_ERR(reset))
		dev_warn(lan966x->dev, "Could not obtain reset control: %ld\n",
			 PTR_ERR(reset));
	else
		reset_control_reset(reset);

	lan_wr(0x0, lan966x, SYS_RESET_CFG);
	lan_wr(0x2, lan966x, SYS_RAM_INIT);
	ret = readx_poll_timeout(lan966x_ram_init, lan966x,
				 val, (val & BIT(1)) == 0, READL_SLEEP_US,
				 READL_TIMEOUT_US);
	if (ret)
		return ret;

	lan_wr(0x1, lan966x, SYS_RESET_CFG);
	return 0;
}

static void lan966x_napi_rx_init(struct lan966x *lan966x,
				 struct lan966x_port *port,
				 struct lan966x_rx *rx, int channel)
{
	if (rx->port)
		return;

	rx->lan966x = lan966x;
	rx->port = port;
	rx->channel_id = channel;
}

static void lan966x_napi_tx_init(struct lan966x *lan966x,
				 struct lan966x_port *port,
				 struct lan966x_tx *tx, int channel)
{
	if (tx->port)
		return;

	tx->lan966x = lan966x;
	tx->port = port;
	tx->channel_id = channel;
	tx->last_in_use = -1;
}

static void lan966x_napi_rx_add_dcb(struct lan966x_rx *rx,
				    struct lan966x_rx_dcb_hw *dcb,
				    u64 nextptr)
{
	int i = 0;

	/* Reset also the status of the DB, just to be sure that we don't have
	 * any leftovers */
	for (i = 0; i < FDMA_RX_DCB_MAX_DBS; ++i) {
		struct lan966x_db_hw *db = &dcb->db[i];
		db->status = FDMA_DCB_STATUS_INTR;
	}

	dcb->nextptr = FDMA_DCB_INVALID_DATA;
	dcb->info = FDMA_DCB_INFO_DATAL(PAGE_SIZE << rx->page_order);

	rx->last_entry->nextptr = nextptr;
	rx->last_entry = dcb;
}

static void lan966x_napi_tx_add_dcb(struct lan966x_tx *tx,
				    struct lan966x_tx_dcb_hw *dcb)
{
	dcb->nextptr = FDMA_DCB_INVALID_DATA;
	dcb->info = 0;
}

static void lan966x_napi_rx_activate(struct lan966x_rx *rx)
{
	struct lan966x *lan966x = rx->lan966x;
	u32 mask;

	/* When activating a channel, first is required to write the first DCB
	 * address and then to activate it
	 */
	lan_wr(((u64)rx->dma) & GENMASK(31, 0), lan966x,
	       FDMA_DCB_LLP(rx->channel_id));
	lan_wr(((u64)rx->dma) >> 32, lan966x, FDMA_DCB_LLP1(rx->channel_id));

	lan_wr(FDMA_CH_CFG_CH_DCB_DB_CNT_SET(FDMA_RX_DCB_MAX_DBS) |
	       FDMA_CH_CFG_CH_INTR_DB_EOF_ONLY_SET(1) |
	       FDMA_CH_CFG_CH_INJ_PORT_SET(0) |
	       FDMA_CH_CFG_CH_MEM_SET(1),
	       lan966x, FDMA_CH_CFG(rx->channel_id));

	/* Start fdma */
	lan_rmw(FDMA_PORT_CTRL_XTR_STOP_SET(0),
		FDMA_PORT_CTRL_XTR_STOP,
		lan966x, FDMA_PORT_CTRL(0));

	/* Enable interrupts */
	mask = lan_rd(lan966x, FDMA_INTR_DB_ENA);
	mask = FDMA_INTR_DB_ENA_INTR_DB_ENA_GET(mask);
	mask |= BIT(rx->channel_id);
	lan_rmw(FDMA_INTR_DB_ENA_INTR_DB_ENA_SET(mask),
		FDMA_INTR_DB_ENA_INTR_DB_ENA,
		lan966x, FDMA_INTR_DB_ENA);

	/* Activate the channel */
	lan_rmw(BIT(rx->channel_id),
		FDMA_CH_ACTIVATE_CH_ACTIVATE,
		lan966x, FDMA_CH_ACTIVATE);
}

static void lan966x_napi_tx_activate(struct lan966x_tx *tx)
{
	struct lan966x *lan966x = tx->lan966x;
	u32 mask;

	/* When activating a channel, first is required to write the first DCB
	 * address and then to activate it
	 */
	lan_wr(((u64)tx->dma) & GENMASK(31, 0),
	       lan966x, FDMA_DCB_LLP(tx->channel_id));
	lan_wr(((u64)tx->dma) >> 32, lan966x, FDMA_DCB_LLP1(tx->channel_id));

	lan_wr(FDMA_CH_CFG_CH_DCB_DB_CNT_SET(FDMA_TX_DCB_MAX_DBS) |
	       FDMA_CH_CFG_CH_INTR_DB_EOF_ONLY_SET(1) |
	       FDMA_CH_CFG_CH_INJ_PORT_SET(0) |
	       FDMA_CH_CFG_CH_MEM_SET(1),
	       lan966x, FDMA_CH_CFG(tx->channel_id));

	/* Start fdma */
	lan_rmw(FDMA_PORT_CTRL_INJ_STOP_SET(0),
		FDMA_PORT_CTRL_INJ_STOP,
		lan966x, FDMA_PORT_CTRL(0));

	/* Enable interrupts */
	mask = lan_rd(lan966x, FDMA_INTR_DB_ENA);
	mask = FDMA_INTR_DB_ENA_INTR_DB_ENA_GET(mask);
	mask |= BIT(tx->channel_id);
	lan_rmw(FDMA_INTR_DB_ENA_INTR_DB_ENA_SET(mask),
		FDMA_INTR_DB_ENA_INTR_DB_ENA,
		lan966x, FDMA_INTR_DB_ENA);

	/* Activate the channel */
	lan_rmw(BIT(tx->channel_id),
		FDMA_CH_ACTIVATE_CH_ACTIVATE,
		lan966x, FDMA_CH_ACTIVATE);
}

static void lan966x_napi_rx_reload(struct lan966x_rx *rx)
{
	struct lan966x *lan966x = rx->lan966x;

	/* Write the registers to reload the channel */
	lan_rmw(BIT(rx->channel_id),
		FDMA_CH_RELOAD_CH_RELOAD,
		lan966x, FDMA_CH_RELOAD);
}

static void lan966x_napi_tx_reload(struct lan966x_tx *tx)
{
	struct lan966x *lan966x = tx->lan966x;

	/* Write the registers to reload the channel */
	lan_rmw(BIT(tx->channel_id),
		FDMA_CH_RELOAD_CH_RELOAD,
		lan966x, FDMA_CH_RELOAD);
}

static struct sk_buff *lan966x_napi_rx_alloc_skb(struct lan966x_rx *rx,
						 struct lan966x_db_hw *db_hw)
{
	struct lan966x *lan966x = rx->lan966x;
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	struct page *page;
	void *buff_addr;

	page = dev_alloc_pages(rx->page_order);
	if (unlikely(!page))
		return NULL;

	dma_addr = dma_map_page(lan966x->dev, page, 0,
				PAGE_SIZE << rx->page_order,
				DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(lan966x->dev, dma_addr))) {
		__free_pages(page, rx->page_order);
		return NULL;
	}

	buff_addr = page_address(page);
	skb = build_skb(buff_addr, PAGE_SIZE << rx->page_order);

	if (unlikely(!skb)) {
		dev_err_ratelimited(lan966x->dev,
				    "build_skb failed !\n");
		dma_unmap_single(lan966x->dev, dma_addr,
				 PAGE_SIZE << rx->page_order,
				 DMA_FROM_DEVICE);
		__free_pages(page, rx->page_order);
		return NULL;
	}

	db_hw->dataptr = dma_addr;
	return skb;
}

static struct sk_buff *lan966x_napi_rx_get_frame(struct lan966x_rx *rx)
{
	struct skb_shared_hwtstamps *shhwtstamps;
	struct lan966x *lan966x = rx->lan966x;
	struct lan966x_db_hw *db_hw;
	unsigned int packet_size;
	struct sk_buff *new_skb;
	struct frame_info info;
	struct timespec64 ts;
	struct sk_buff *skb;
	u64 full_ts_in_ns;

	/* A frame copy to the DMA by the FDMA only when the DONE bit is set */
	db_hw = &rx->dcb_entries[rx->dcb_index].db[rx->db_index];
	if (unlikely(!(db_hw->status & FDMA_DCB_STATUS_DONE)))
		return NULL;

	skb = rx->skb[rx->dcb_index][rx->db_index];
	/* Unmap the received skb data, this is required otherwise all the
	 * access to the skb->data will go through coherent memory and it is
	 * slow
	 */
	dma_unmap_single(lan966x->dev, (dma_addr_t)db_hw->dataptr,
			 FDMA_DCB_STATUS_BLOCKL(db_hw->status),
			 DMA_FROM_DEVICE);

	/* Allocate a new SKB here because the upper layers might be trying to
	 * release ownership of the buffer and then still using the skb
	 */
	new_skb = lan966x_napi_rx_alloc_skb(rx, db_hw);
	if (unlikely(!new_skb))
		return NULL;

	rx->skb[rx->dcb_index][rx->db_index] = new_skb;

	packet_size = FDMA_DCB_STATUS_BLOCKL(db_hw->status);
	skb_put(skb, packet_size);

	/* Now do the normal processing of the skb */
	lan966x_parse_ifh((u32*)skb->data, &info);
	skb->dev = lan966x->ports[info.port]->dev;
	skb_pull(skb, IFH_LEN * sizeof(u32));

	if (likely(!(skb->dev->features & NETIF_F_RXFCS)))
		skb_trim(skb, skb->len - ETH_FCS_LEN);

	skb->protocol = eth_type_trans(skb, skb->dev);

	/* This is an expensive operation, it would be great to if this can be
	 * moved out of this function, but if we extract continously for more
	 * than 1 second we might have issue with the second part of the
	 * timestamping. Maybe we can do this operation only when the nsec is
	 * rolling over. We reduce the number of calls.
	 */
	lan966x_ptp_gettime64(&lan966x->ptp_domain[LAN966X_PTP_PORT_DOMAIN].info,
			      &ts);
	info.timestamp = info.timestamp >> 2;
	if (ts.tv_nsec < info.timestamp)
		ts.tv_sec--;
	ts.tv_nsec = info.timestamp;
	full_ts_in_ns = ktime_set(ts.tv_sec, ts.tv_nsec);

	shhwtstamps = skb_hwtstamps(skb);
	shhwtstamps->hwtstamp = full_ts_in_ns;

	/* Everything we see on an interface that is in the HW bridge
	 * has already been forwarded
	 */
	if (lan966x->bridge_mask & BIT(info.port) &&
	    lan966x->hw_offload) {
#ifdef CONFIG_NET_SWITCHDEV
		skb->offload_fwd_mark = 1;

		skb_reset_network_header(skb);
		if (!lan966x_hw_offload(lan966x, info.port, skb))
			skb->offload_fwd_mark = 0;
#endif
	}

	skb->dev->stats.rx_bytes += skb->len;
	skb->dev->stats.rx_packets++;

	return skb;
}

static void lan966x_napi_rx_get_dcb(struct lan966x_rx *rx,
				    void *dcb_hw,
				    dma_addr_t *dma)
{
	*dma = rx->dma;
	dcb_hw = &rx->dcb_entries;
}

static void lan966x_napi_rx_disable(struct lan966x_rx *rx)
{
	struct lan966x *lan966x = rx->lan966x;
	u32 val;

	/* Disable the channel */
	lan_rmw(BIT(rx->channel_id),
		FDMA_CH_DISABLE_CH_DISABLE,
		lan966x, FDMA_CH_DISABLE);

	readx_poll_timeout_atomic(lan966x_napi_channel_active, lan966x,
				  val, !(val & BIT(rx->channel_id)),
				  READL_SLEEP_US, READL_TIMEOUT_US);

	lan_rmw(BIT(rx->channel_id),
		FDMA_CH_DB_DISCARD_DB_DISCARD,
		lan966x, FDMA_CH_DB_DISCARD);
}

static void lan966x_napi_rx_clear_dbs(struct lan966x_rx *rx)
{
	struct lan966x *lan966x = rx->lan966x;
	struct lan966x_rx_dcb_hw *dcb;
	int i, j;

	for (i = 0; i < FDMA_DCB_MAX; ++i) {
		dcb = &rx->dcb_entries[i];

		for (j = 0; j < FDMA_RX_DCB_MAX_DBS; ++j) {
			struct lan966x_db_hw *db_hw = &dcb->db[j];

			dma_unmap_single(lan966x->dev, (dma_addr_t)db_hw->dataptr,
					 PAGE_SIZE << rx->page_order,
					 DMA_FROM_DEVICE);
			kfree_skb(rx->skb[i][j]);
		}
	}
}

static int lan966x_napi_rx_alloc(struct lan966x_rx *rx)
{
	struct lan966x *lan966x = rx->lan966x;
	struct lan966x_rx_dcb_hw *dcb;
	int i, j;
	int size;

	/* calculate how many pages are needed to allocate the dcbs, they are
	 * basically a list
	 */
	size = sizeof(struct lan966x_rx_dcb_hw) * FDMA_DCB_MAX;
	size = ALIGN(size, PAGE_SIZE);

	rx->dcb_entries = dma_alloc_coherent(lan966x->dev, size, &rx->dma,
					     GFP_ATOMIC);
	rx->last_entry = rx->dcb_entries;
	rx->db_index = 0;
	rx->dcb_index = 0;

	/* Now for each dcb allocate the db */
	for (i = 0; i < FDMA_DCB_MAX; ++i) {
		dcb = &rx->dcb_entries[i];
		dcb->info = 0;

		/* For each db allocate a skb and map skb data pointer to the DB
		 * dataptr. In this way when the frame is received the skb->data
		 * will contain the frame, so it is not needed any memcpy
		 */
		for (j = 0; j < FDMA_RX_DCB_MAX_DBS; ++j) {
			struct lan966x_db_hw *db_hw = &dcb->db[j];
			struct sk_buff *skb;

			skb = lan966x_napi_rx_alloc_skb(rx, db_hw);
			if (!skb)
				return -ENOMEM;

			db_hw->status = 0;
			rx->skb[i][j] = skb;
		}

		lan966x_napi_rx_add_dcb(rx, dcb, rx->dma + sizeof(*dcb) * i);
	}

	return 0;
}

static void lan966x_napi_tx_get_dcb(struct lan966x_tx *tx,
				    void *dcb_hw,
				    void *dcb_buf,
				    dma_addr_t *dma)
{
	*dma = tx->dma;
	dcb_hw = &tx->dcbs;
	dcb_buf = &tx->dcbs_buf;
}

static int lan966x_napi_channel_active(struct lan966x *lan966x)
{
	return lan_rd(lan966x, FDMA_CH_ACTIVE);
}

static void lan966x_napi_tx_disable(struct lan966x_tx *tx)
{
	struct lan966x *lan966x = tx->lan966x;
	u32 val;

	/* Disable the channel */
	lan_rmw(BIT(tx->channel_id),
		FDMA_CH_DISABLE_CH_DISABLE,
		lan966x, FDMA_CH_DISABLE);

	readx_poll_timeout_atomic(lan966x_napi_channel_active, lan966x,
				  val, !(val & BIT(tx->channel_id)),
				  READL_SLEEP_US, READL_TIMEOUT_US);

	lan_rmw(BIT(tx->channel_id),
		FDMA_CH_DB_DISCARD_DB_DISCARD,
		lan966x, FDMA_CH_DB_DISCARD);

	tx->activated = false;
}

static int lan966x_napi_tx_alloc(struct lan966x_tx *tx)
{
	struct lan966x *lan966x = tx->lan966x;
	struct lan966x_tx_dcb_hw *dcb;
	int size;
	int i, j;

	tx->dcbs_buf = kcalloc(FDMA_DCB_MAX, sizeof(struct lan966x_tx_dcb_buf),
			       GFP_ATOMIC);
	if (!tx->dcbs_buf)
		return -ENOMEM;

	/* calculate how many pages are needed to allocate the dcbs, they are
	 * basically a list
	 */
	size = sizeof(struct lan966x_tx_dcb_hw) * FDMA_DCB_MAX;
	size = ALIGN(size, PAGE_SIZE);
	tx->dcbs = dma_alloc_coherent(lan966x->dev, size, &tx->dma, GFP_ATOMIC);

	/* Now for each dcb allocate the db */
	for (i = 0; i < FDMA_DCB_MAX; ++i) {
		dcb = &tx->dcbs[i];

		for (j = 0; j < FDMA_TX_DCB_MAX_DBS; ++j) {
			struct lan966x_db_hw *db_hw = &dcb->db[j];

			db_hw->dataptr = 0;
			db_hw->status = 0;
		}

		lan966x_napi_tx_add_dcb(tx, dcb);
	}

	return 0;
}

static int lan966x_get_next_dcb(struct lan966x_tx *tx)
{
	struct lan966x_tx_dcb_buf *dcb_buf;
	int i;

	for (i = 0; i < FDMA_DCB_MAX; ++i) {
		dcb_buf = &tx->dcbs_buf[i];
		if (!dcb_buf->used && i != tx->last_in_use)
			return i;
	}

	return -1;
}

static int lan966x_napi_xmit(struct sk_buff *skb, struct frame_info *info,
			     struct net_device *dev)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	struct lan966x_tx_dcb_hw *next_dcb_hw, *dcb_hw;
	struct lan966x_port *port = netdev_priv(dev);
	volatile struct lan966x_db_hw *next_db_hw;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_tx_dcb_buf *next_dcb_buf;
	struct lan966x_tx *tx = &lan966x->tx;
	u32 ifh[IFH_LEN] = { 0 };
	dma_addr_t dma_addr;
	unsigned long flags;
	int next_to_use;
	int i, err;

	if (skb_put_padto(skb, ETH_ZLEN))
		return NETDEV_TX_OK;

	spin_lock_irqsave(&lan966x->tx_lock, flags);

	/* Get next index */
	next_to_use = lan966x_get_next_dcb(tx);
	if (next_to_use < 0) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&lan966x->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}

	/* Skb processing */
	lan966x_gen_ifh(ifh, info, lan966x);
	for (i = 0; i < IFH_LEN; ++i)
		ifh[i] = (__force u32)cpu_to_be32(ifh[i]);

	skb_tx_timestamp(skb);
	if (skb_headroom(skb) < IFH_LEN * sizeof(u32)) {
		err = pskb_expand_head(skb,
				       IFH_LEN * sizeof(u32) - skb_headroom(skb),
				       0, GFP_ATOMIC);
		if (unlikely(err)) {
			dev_kfree_skb_any(skb);
			spin_unlock_irqrestore(&lan966x->tx_lock, flags);
			return NETDEV_TX_OK;
		}
	}

	skb_push(skb, IFH_LEN * sizeof(u32));
	memcpy(skb->data, ifh, IFH_LEN * sizeof(u32));
	skb_put(skb, 4);

	dma_addr = dma_map_single(lan966x->dev, skb->data, skb->len,
				  DMA_TO_DEVICE);
	if (dma_mapping_error(lan966x->dev, dma_addr)) {
		spin_unlock_irqrestore(&lan966x->tx_lock, flags);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Setup next dcb */
	next_dcb_hw = &tx->dcbs[next_to_use];
	next_dcb_hw->nextptr = FDMA_DCB_INVALID_DATA;

	next_db_hw = &next_dcb_hw->db[0];
	next_db_hw->dataptr = dma_addr;
	next_db_hw->status = FDMA_DCB_STATUS_SOF |
			     FDMA_DCB_STATUS_EOF |
			     FDMA_DCB_STATUS_INTR |
			     FDMA_DCB_STATUS_BLOCKO(0) |
			     FDMA_DCB_STATUS_BLOCKL(skb->len);

	/* Fill up the buffer */
	next_dcb_buf = &tx->dcbs_buf[next_to_use];
	next_dcb_buf->skb = skb;
	next_dcb_buf->dma_addr = dma_addr;
	next_dcb_buf->used = true;
	next_dcb_buf->ptp = false;

	if (shinfo->tx_flags & SKBTX_HW_TSTAMP &&
	    info->rew_op == IFH_REW_OP_TWO_STEP_PTP)
		next_dcb_buf->ptp = true;

	lan966x_ptp_2step_save(port, info, shinfo, skb);

	if (likely(lan966x->tx.activated)) {
		/* Connect current dcb to the next db */
		dcb_hw = &tx->dcbs[tx->last_in_use];
		dcb_hw->nextptr = tx->dma + (next_to_use * sizeof(struct lan966x_tx_dcb_hw));

		lan966x_napi_tx_reload(tx);
	} else {
		/* Because it is first time, then just activate */
		lan966x->tx.activated = true;
		lan966x_napi_tx_activate(tx);
	}

	/* Move to next dcb because this last in use */
	tx->last_in_use = next_to_use;

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	spin_unlock_irqrestore(&lan966x->tx_lock, flags);

	if (shinfo->tx_flags & SKBTX_HW_TSTAMP &&
	    info->rew_op == IFH_REW_OP_TWO_STEP_PTP) {
		u32 val = 0;
		int err;

		if (!lan966x->ptp_poll)
			goto skip_polling;

		err = readx_poll_timeout_atomic(lan966x_ts_fifo_ready, lan966x,
						val, val, 10, 100000);
		if (err == -ETIMEDOUT) {
			pr_info("Ts fifo no valid value\n");
			goto skip_polling;
		}

		lan966x_ptp_irq_handler(0, lan966x);

skip_polling:
		return NETDEV_TX_OK;
	}

	return NETDEV_TX_OK;
}

static void lan966x_wakeup_netdev(struct lan966x *lan966x)
{
	struct lan966x_port *port;
	int i;

	for (i = 0; i < lan966x->num_phys_ports; ++i) {
		port = lan966x->ports[i];
		if (!port)
			continue;

		if (netif_queue_stopped(port->dev))
			netif_wake_queue(port->dev);
	}
}

static void lan966x_tx_clear_buf(struct lan966x* lan966x, int weight)
{
	struct lan966x_tx *tx = &lan966x->tx;
	volatile struct lan966x_db_hw *db_hw;
	struct lan966x_tx_dcb_buf *dcb_buf;
	unsigned long flags;
	bool clear = false;
	int i;

	spin_lock_irqsave(&lan966x->tx_lock, flags);
	for (i = 0; i < FDMA_DCB_MAX; ++i) {
		dcb_buf = &tx->dcbs_buf[i];

		if (!dcb_buf->used)
			continue;

		db_hw = &tx->dcbs[i].db[0];
		if (!(db_hw->status & FDMA_DCB_STATUS_DONE))
			continue;

		dcb_buf->used = false;
		dma_unmap_single(lan966x->dev,
				 dcb_buf->dma_addr,
				 dcb_buf->skb->len,
				 DMA_TO_DEVICE);
		if (!dcb_buf->ptp)
			dev_kfree_skb_any(dcb_buf->skb);

		clear = true;
	}
	spin_unlock_irqrestore(&lan966x->tx_lock, flags);

	if (clear)
		lan966x_wakeup_netdev(lan966x);
}

/* This function is called by NAPI framework by the NET_RX_SOFTIRQ */
static int lan966x_napi_poll(struct napi_struct *napi, int weight)
{
	struct lan966x *lan966x = container_of(napi, struct lan966x, napi);
	struct lan966x_rx *rx = &lan966x->rx;
	struct list_head rx_list;
	int counter = 0;

	lan966x_tx_clear_buf(lan966x, weight);

	INIT_LIST_HEAD(&rx_list);

	while (counter < weight) {
		struct lan966x_rx_dcb_hw *old_dcb;
		struct sk_buff *skb;

		skb = lan966x_napi_rx_get_frame(rx);
		if (!skb)
			break;
		list_add_tail(&skb->list, &rx_list);

		rx->db_index++;
		rx_counters++;
		counter++;

		/* Check if the DCB can be reused */
		if (rx->db_index != FDMA_RX_DCB_MAX_DBS)
			continue;

		/* Now the DCB  can be reused, just advance the dcb_index
		 * pointer and set the nextptr in the DCB
		 */
		rx->db_index = 0;

		old_dcb = &rx->dcb_entries[rx->dcb_index];
		rx->dcb_index++;
		rx->dcb_index &= FDMA_DCB_MAX - 1;

		lan966x_napi_rx_add_dcb(rx, old_dcb,
					rx->dma + ((unsigned long)old_dcb - (unsigned long)rx->dcb_entries));
		lan966x_napi_rx_reload(rx);
	}

	if (counter < weight) {
		napi_complete_done(napi, counter);
		lan_wr(0xff, lan966x, FDMA_INTR_DB_ENA);
	}

	netif_receive_skb_list(&rx_list);

	return counter;
}

static void lan966x_napi_start(struct lan966x *lan966x)
{
	netif_napi_add(lan966x->rx.port->dev, &lan966x->napi, lan966x_napi_poll);
	napi_enable(&lan966x->napi);

	lan966x_napi_rx_activate(&lan966x->rx);
}

static irqreturn_t lan966x_fdma_irq_handler(int irq, void *args)
{
	struct lan966x *lan966x = args;
	u32 dcb = 0, db = 0, err = 0;

	dcb = lan_rd(lan966x, FDMA_INTR_DCB);
	db = lan_rd(lan966x, FDMA_INTR_DB);
	err = lan_rd(lan966x, FDMA_INTR_ERR);

	/* Clear interrupt */
	if (db) {
		lan_wr(0, lan966x, FDMA_INTR_DB_ENA);
		lan_wr(db, lan966x, FDMA_INTR_DB);

		napi_schedule(&lan966x->napi);
	}

	if (err) {
		u32 err_type = lan_rd(lan966x, FDMA_ERRORS);

		pr_err("%s:%d %s: ERR int: 0x%x\n",
		       __FILE__, __LINE__, __func__, err);
		pr_err("%s:%d %s: errtype: 0x%x\n",
		       __FILE__, __LINE__, __func__, err_type);

		lan_wr(err, lan966x, FDMA_INTR_ERR);
		lan_wr(err_type, lan966x, FDMA_ERRORS);
	}

	return IRQ_HANDLED;
}
#endif

static int mchp_lan966x_probe(struct platform_device *pdev)
{
	struct fwnode_handle *ports, *portnp;
	const struct lan966x_data *data;
	struct lan966x *lan966x;
	u8 mac_addr[ETH_ALEN];
	int err, i;

	struct {
		enum lan966x_target id;
		char *name;
	} res[] = {
#if defined(SUNRISE) || defined(ASIC)
		{ TARGET_CPU, "cpu" },
		{ TARGET_FDMA, "fdma" },
		{ TARGET_CHIP_TOP, "chip_top" },
#if defined(SUNRISE)
		{ TARGET_SUNRISE_TOP, "sunrise_top" },
#endif
#endif
		{ TARGET_ORG, "org" },
		{ TARGET_SYS, "sys" },
		{ TARGET_QS, "qs" },
		{ TARGET_QSYS, "qsys" },
		{ TARGET_ANA, "ana" },
		{ TARGET_REW, "rew" },
		{ TARGET_GCB, "gcb" },
		{ TARGET_PTP, "ptp" },
		{ TARGET_VCAP, "es0" },
		{ TARGET_VCAP + 1, "s1" },
		{ TARGET_VCAP + 2, "s2" },
		{ TARGET_AFI, "afi" },
		{ TARGET_MEP, "mep" },
	};

	lan966x = devm_kzalloc(&pdev->dev, sizeof(*lan966x), GFP_KERNEL);
	if (!lan966x)
		return -ENOMEM;

	lan966x->debugfs_root = debugfs_create_dir("lan966x", NULL);
	if (IS_ERR(lan966x->debugfs_root)) {
		dev_err(&pdev->dev, "Unable to create debugfs root\n");
		return PTR_ERR(lan966x->debugfs_root);
	}

	platform_set_drvdata(pdev, lan966x);
	lan966x->dev = &pdev->dev;

	data = device_get_match_data(&pdev->dev);
	lan966x->hw_offload = data->hw_offload;
	lan966x->internal_phy = data->internal_phy;

	lan966x_prof_init_dbgfs(lan966x);
	lan966x_debugfs_init(lan966x);

	for (i = 0; i < ARRAY_SIZE(res); i++) {
		struct resource *resource;

		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							res[i].name);
		if (!resource)
			return -ENODEV;

		lan966x->regs[res[i].id] = ioremap(resource->start,
						   resource_size(resource));
		if (IS_ERR(lan966x->regs[res[i].id])) {
			dev_info(&pdev->dev,
				"Unable to map Switch registers: %x\n", i);
		}
	}

#if defined(SUNRISE) || defined(ASIC)
	if (lan966x_reset_switch(lan966x)) {
		pr_info("Failed to reset the switch\n");
		return -EINVAL;
	}
#endif

	lan966x->txdma = dma_request_chan(lan966x->dev, "tx");
	lan966x->rxdma = dma_request_chan(lan966x->dev, "rx");
	if (IS_ERR(lan966x->txdma) || IS_ERR(lan966x->rxdma)) {
		if (!IS_ERR(lan966x->txdma))
			dma_release_channel(lan966x->txdma);
		if (!IS_ERR(lan966x->rxdma))
			dma_release_channel(lan966x->rxdma);

		lan966x->txdma = NULL;
		lan966x->rxdma = NULL;

		dev_info(lan966x->dev, "Use register extraction\n");
		lan966x->use_dma = false;
	} else {
		dev_info(lan966x->dev, "Use TX & RX DMA channels\n");
		lan966x->use_dma = true;
	}

	INIT_LIST_HEAD(&lan966x->free_tx_reqs);
	INIT_LIST_HEAD(&lan966x->free_rx_reqs);
	INIT_LIST_HEAD(&lan966x->rx_reqs);
	INIT_LIST_HEAD(&lan966x->tx_reqs);
	spin_lock_init(&lan966x->tx_lock);

	if (lan966x->use_dma) {
		lan966x->rx_req_fill_level = FDMA_XTR_BUFFER_COUNT;
		lan966x->tx_req_interval = 20;

		for (i = 0; i < FDMA_TX_REQUEST_MAX; ++i) {
			struct lan966x_tx_request *req =
				devm_kzalloc(lan966x->dev, sizeof(*req), GFP_KERNEL);

			if (!req) {
				dev_err(&pdev->dev,
					"Unable to allocate tx req\n");
				return -ENOMEM;
			}

			list_add(&req->node, &lan966x->free_tx_reqs);
		}

		lan966x->rx_pool = dmam_pool_create("lan966x-rx",
						    lan966x->rxdma->device->dev,
						    FDMA_XTR_BUFFER_SIZE,
						    FDMA_BUFFER_ALIGN, 0);
		if (!lan966x->rx_pool) {
			dev_err(&pdev->dev, "Unable to allocate rx pool\n");
			return -ENOMEM;
		}

		for (i = 0; i < FDMA_RX_REQUEST_MAX; ++i) {
			struct lan966x_rx_request *req =
				devm_kzalloc(lan966x->dev, sizeof(*req), GFP_KERNEL);

			if (!req) {
				dev_err(&pdev->dev,
					"Unable to allocate rx req\n");
				return -ENOMEM;
			}

			lan966x_init_rx_request(lan966x, req,
						FDMA_XTR_BUFFER_SIZE);
			list_add(&req->node, &lan966x->free_rx_reqs);
			lan966x_prepare_rx_request(lan966x);
		}
	}

	lan966x->use_napi = false;
	if (device_property_present(&pdev->dev, "mchp,use_napi"))
		lan966x->use_napi = true;

	if (!device_get_mac_address(&pdev->dev, mac_addr)) {
		ether_addr_copy(lan966x->base_mac, mac_addr);
	} else {
		pr_info("MAC addr was not set, use random MAC\n");
		eth_random_addr(lan966x->base_mac);
		lan966x->base_mac[5] &= 0xf0;
	}

	ports = device_get_named_child_node(&pdev->dev, "ethernet-ports");
	if (!ports) {
		dev_err(&pdev->dev, "no ethernet-ports child not found\n");
		return -ENODEV;
	}

	lan966x->num_phys_ports = LAN966X_NUM_PHYS_PORTS;
	lan966x->ports = devm_kcalloc(&pdev->dev, lan966x->num_phys_ports,
				      sizeof(struct lan966x_port *),
				      GFP_KERNEL);

	lan966x->stats_layout = lan966x_stats_layout;
	lan966x->num_stats = ARRAY_SIZE(lan966x_stats_layout);
	lan966x->stats = devm_kcalloc(&pdev->dev, LAN966X_MAX_PORTS *
				      lan966x->num_stats,
				      sizeof(u64), GFP_KERNEL);
	if (!lan966x->stats)
		return -ENOMEM;

	INIT_LIST_HEAD(&lan966x->multicast);

	/* There QS system has 32KB of memory */
	lan966x->shared_queue_sz = LAN966X_BUFFER_MEMORY;

	/* set irq */
	lan966x->xtr_irq = platform_get_irq_byname(pdev, "xtr");
	err = devm_request_threaded_irq(&pdev->dev, lan966x->xtr_irq, NULL,
					lan966x_xtr_irq_handler, IRQF_ONESHOT,
					"frame extraction", lan966x);
	if (err) {
		pr_info("Unable to use xtr irq, fallback to manual polling");
		lan966x->recv_task = kthread_run(lan966x_xtr_task,
						 lan966x, "frame extraction");
		if (lan966x->recv_task == ERR_PTR(-ENOMEM)) {
			dev_err(&pdev->dev, "recv thread not started\n");
			return -ENODEV;
		}
	}

	/* set ptp irq */
	lan966x->ptp_irq = platform_get_irq_byname(pdev, "ptp");
	err = devm_request_threaded_irq(&pdev->dev, lan966x->ptp_irq, NULL,
					lan966x_ptp_irq_handler, IRQF_ONESHOT,
					"ptp ready", lan966x);
	if (err) {
		pr_info("Unable to use ptp irq, fallback to manual polling");
		lan966x->ptp_poll = 1;
	}

	/* set ptp-sync irq */
	lan966x->ptp_sync_irq = platform_get_irq_byname(pdev, "ptp-sync");
	err = devm_request_threaded_irq(&pdev->dev, lan966x->ptp_sync_irq, NULL,
					lan966x_ptp_sync_irq_handler, IRQF_ONESHOT,
					"ptp sync", lan966x);
	if (err) {
		pr_info("Unable to use ptp-sync irq, fallback to manual polling");
		lan966x->ptp_sync_poll = 1;
	}

	/* set ana irq */
	lan966x->ana_irq = platform_get_irq_byname(pdev, "ana");
	err = devm_request_threaded_irq(&pdev->dev, lan966x->ana_irq, NULL,
					lan966x_ana_irq_handler, IRQF_ONESHOT,
					"ana irq", lan966x);
	if (err) {
		pr_info("Unable to use ana irq, fallback to manual polling");
		lan966x->ana_poll = 1;
	}

#if defined(SUNRISE) || defined(ASIC)
	if (!lan966x->use_dma) {
		lan966x->fdma_irq = platform_get_irq_byname(pdev, "fdma");
		err = devm_request_threaded_irq(&pdev->dev, lan966x->fdma_irq,
						lan966x_fdma_irq_handler, NULL,
						IRQF_SHARED,
						"fdma irq", lan966x);
		if (err)
			pr_info("Unable to use fdma irq, fallback to manual polling");
	}
#endif

	/* init switch */
	lan966x_init(lan966x);

	/* go over the child nodes */
	fwnode_for_each_available_child_node(ports, portnp) {
		phy_interface_t phy_mode;
		struct resource *res;
		struct phy *serdes;
		void __iomem *regs;
		char res_name[8];
		u32 port;

		if (fwnode_property_read_u32(portnp, "reg", &port))
			continue;

		snprintf(res_name, sizeof(res_name), "port%d", port);

		/* Map the resources */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   res_name);
		regs = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(regs))
			continue;

		lan966x->regs[TARGET_DEV + port] = regs;

		phy_mode = fwnode_get_phy_mode(portnp);
		err = lan966x_probe_port(lan966x, port, phy_mode, portnp);
		if (err)
			return err;

		/* Read needed configuration */
		lan966x->ports[port]->config.phy_mode = phy_mode;
		lan966x->ports[port]->config.portmode = phy_mode;
		lan966x->ports[port]->fwnode = fwnode_handle_get(portnp);

		serdes = devm_of_phy_get(lan966x->dev, to_of_node(portnp), NULL);
		if (!IS_ERR(serdes))
			lan966x->ports[port]->serdes = serdes;

		err = lan966x_parse_delays(lan966x, port, portnp);
		if (err)
			netdev_info(lan966x->ports[port]->dev,
				    "Unable to parse delays");

		lan966x_port_init(lan966x->ports[port]);

#if defined(SUNRISE) || defined(ASIC)
		if (lan966x->use_napi) {
			lan966x_napi_rx_init(lan966x, lan966x->ports[port],
					     &lan966x->rx, FDMA_XTR_CHANNEL);
			lan966x_napi_tx_init(lan966x, lan966x->ports[port],
					     &lan966x->tx, FDMA_INJ_CHANNEL);
		}
#endif
	}

#if defined(SUNRISE) || defined(ASIC)
	if (lan966x->use_napi) {
		lan966x_napi_rx_alloc(&lan966x->rx);
		lan966x_napi_tx_alloc(&lan966x->tx);

		lan966x_napi_start(lan966x);
	}
#endif

	err = lan966x_register_notifier_blocks(lan966x);
	if (err)
		return err;

	err = lan966x_qos_init(lan966x);
	if (err)
		return err;

	/* Init vcap */
	lan966x_vcap_init(lan966x);

#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	lan966x_mrp_init(lan966x);
#endif
#if IS_ENABLED(CONFIG_BRIDGE_CFM)
	lan966x_cfm_init(lan966x);
#endif

#if defined(SUNRISE)
	lan_rmw(CPU_ULPI_RST_ULPI_RST_SET(1),
		CPU_ULPI_RST_ULPI_RST,
		lan966x, CPU_ULPI_RST);

	lan_rmw(CPU_ULPI_RST_ULPI_RST_SET(0),
		CPU_ULPI_RST_ULPI_RST,
		lan966x, CPU_ULPI_RST);
#endif

	lan966x_proc_register_dbg(lan966x);

#if defined(SUNRISE) || defined(ADARO)
	if (lan_rd(lan966x, LAN966X_BUILD_ID_REG) != LAN966X_BUILD_ID)
		pr_info("HEADERS: %08x, FPGA: %08x\n", LAN966X_BUILD_ID,
			lan_rd(lan966x, LAN966X_BUILD_ID_REG));

	BUG_ON(lan_rd(lan966x, LAN966X_BUILD_ID_REG) != LAN966X_BUILD_ID);
#endif

	return 0;
}

static int mchp_lan966x_remove(struct platform_device *pdev)
{
	struct lan966x *lan966x = platform_get_drvdata(pdev);
	struct lan966x_port *port;
	int i;

	if (lan966x->recv_task)
		kthread_stop(lan966x->recv_task);
	else
		devm_free_irq(lan966x->dev, lan966x->xtr_irq, lan966x);

	if (lan966x->use_dma) {
		dma_release_channel(lan966x->rxdma);
		dma_release_channel(lan966x->txdma);
	}

	if (lan966x->ana_poll)
		cancel_delayed_work_sync(&lan966x->mact_work);
	else
		devm_free_irq(lan966x->dev, lan966x->ana_irq, lan966x);

	if (!lan966x->ptp_poll)
		devm_free_irq(lan966x->dev, lan966x->ptp_irq, lan966x);

	lan966x_prof_remove_dbgfs(lan966x);
	debugfs_remove_recursive(lan966x->debugfs_root);
	lan966x_proc_unregister_dbg();
	lan966x_unregister_notifier_blocks(lan966x);

#if IS_ENABLED(CONFIG_BRIDGE_MRP)
	lan966x_mrp_uninit(lan966x);
#endif
#if IS_ENABLED(CONFIG_BRIDGE_CFM)
	lan966x_cfm_uninit(lan966x);
#endif

	lan966x_timestamp_deinit(lan966x);
	lan966x_vcap_uninit(lan966x);
	lan966x_netlink_frer_uninit();
	lan966x_netlink_fp_uninit();
	lan966x_netlink_qos_uninit();

	destroy_workqueue(lan966x->mact_queue);

	destroy_workqueue(lan966x->stats_queue);
	mutex_destroy(&lan966x->stats_lock);

	for (i = 0; i < lan966x->num_phys_ports; i++) {
		port = lan966x->ports[i];

		if (!port)
			continue;

		skb_queue_purge(&port->tx_skbs);
	}

	return 0;
}

static struct platform_driver mchp_lan966x_driver = {
	.probe = mchp_lan966x_probe,
	.remove = mchp_lan966x_remove,
	.driver = {
		.name = "lan966x-switch",
		.of_match_table = mchp_lan966x_match,
	},
};
module_platform_driver(mchp_lan966x_driver);

MODULE_DESCRIPTION("Microchip LAN966X switch driver");
MODULE_AUTHOR("Horatiu Vultur <horatiu.vultur@microchip.com>");
MODULE_LICENSE("Dual MIT/GPL");
