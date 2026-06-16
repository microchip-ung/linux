/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip AFI netlink API
 *
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 */

#ifndef __AFI_NETLINK__
#define __AFI_NETLINK__

#include <linux/netdevice.h>

#include "afi_api.h"

/* Generic netlink ABI (shared with userspace). */
#define AFI_NETLINK_NAME	"mchp_afi"
#define AFI_NETLINK_VERSION	1

/* Netlink attributes. */
enum afi_nl_attr {
	AFI_NL_ATTR_NONE,
	AFI_NL_ATTR_PAD,	/* u64 alignment padding */
	AFI_NL_ATTR_IFINDEX,	/* u32: target net device */
	AFI_NL_ATTR_FRAME,	/* binary: frame to inject */
	AFI_NL_ATTR_BPS,	/* u64: requested rate, 0 => line rate */
	AFI_NL_ATTR_SEQ_CNT,	/* u32: frames to inject, 0 => forever */
	AFI_NL_ATTR_PRIO,	/* u8: egress priority */
	AFI_NL_ATTR_FRM_SIZE,	/* u32: frame size for rate calc (optional) */
	AFI_NL_ATTR_FASTID,	/* u32: flow id */
	AFI_NL_ATTR_BPS_ACTUAL,	/* u64: actual rate */
	AFI_NL_ATTR_STATE,	/* u8: flow state (enum afi_entry_state) */
	AFI_NL_ATTR_PORT,	/* u32: chip port */
	AFI_NL_ATTR_SEQ_ACTUAL,	/* u32: actual frames injected (>= requested) */

	__AFI_NL_ATTR_END,
	AFI_NL_ATTR_MAX = __AFI_NL_ATTR_END - 1,
};

/* Netlink commands. */
enum afi_nl_cmd {
	AFI_NL_CMD_NONE,
	AFI_NL_CMD_FAST_START,	/* start fast injection; returns fastid + bps */
	AFI_NL_CMD_FAST_STOP,	/* stop and free a fast injection */
	AFI_NL_CMD_FAST_GET,	/* get one flow (doit) / list all (dumpit) */

	__AFI_NL_CMD_END,
	AFI_NL_CMD_MAX = __AFI_NL_CMD_END - 1,
};

/* Per-registrant chip callbacks. */
struct afi_nl_ops {
	/* Return the chip port for ndev, or negative if ndev is not a port of
	 * this registrant.
	 */
	int (*port_of)(void *priv, struct net_device *ndev);
};

/**
 * \brief Register an AFI controller with the netlink API.
 *
 * The generic netlink family is registered on the first call and unregistered
 * when the last controller is removed.
 *
 * \param afi  [IN] AFI controller.
 * \param priv [IN] Opaque value passed back to ops callbacks.
 * \param ops  [IN] Chip callbacks.
 *
 * \return Return code.
 **/
int afi_netlink_register(struct afi_control *afi, void *priv,
			 const struct afi_nl_ops *ops);

/**
 * \brief Unregister an AFI controller from the netlink API.
 *
 * \param afi [IN] AFI controller.
 **/
void afi_netlink_unregister(struct afi_control *afi);

#endif /* __AFI_NETLINK__ */
