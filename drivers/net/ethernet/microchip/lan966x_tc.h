/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2020 Microchip Technology Inc. */

#ifndef _LAN966X_TC_H_
#define _LAN966X_TC_H_

#include <linux/types.h>
#include <linux/netdevice.h>
#include <net/pkt_cls.h>

struct lan966x;
struct lan966x_port;

/* Temporary definition until if_ether.h is updated */
#define ETH_P_RTAG     0xF1C1          /* Redundancy Tag (IEEE 802.1CB) */
#define ETH_P_ELMI     0x88EE          /* MEF 16 E-LMI */


/* TC index for Always Open Stream Gate */
#define LAN966X_TC_AOSG U32_MAX

/*******************************************************************************
 * tc flower VCAP definitions
 ******************************************************************************/
#define LAN966X_VCAP_LOOKUP_MAX (3+2+1) /* IS1, IS2, ES0 */

/**
 * struct lan966x_port_tc - Per port tc data
 *
 * @block_shared: Array where index 0 is egress and index 1 is ingress.
 *                Lookup with a bool called ingress will work.
 *                True if port is associated with a shared block.
 * @offload_cnt: Count the number of offloaded qdiscs and filters
 * @police_id: Saved police id (cookie)
 * @police_stats: Saved policer statistics.
 * @mirror_stats: Saved mirror statistics, egress[0], ingress[1].
 * @flower_tmplt_proto: Protocol assigned by template. 0 = no template.
 * @flower_ref_cnt: Reference count for each chain
 */
struct lan966x_port_tc {
	bool block_shared[2];
	unsigned long offload_cnt;
	unsigned long police_id;
	struct flow_stats police_stats;
	struct flow_stats mirror_stats[2];
	 /* protocol assigned template per vcap lookup */
	u16 flower_template_proto[LAN966X_VCAP_LOOKUP_MAX];
	/* list of flower templates for this port */
	struct list_head templates;
};

/*******************************************************************************
 * API
 ******************************************************************************/
/**
 * lan966x_tc_flower - Handle all flower filter commands
 * @port: The interface.
 * @f: Offload info.
 * @ingress: true if ingress rule.
 *
 * Returns:
 * 0 if ok.
 * Negative error code on failure.
 */
int lan966x_tc_flower(struct lan966x_port *port,
		      struct flow_cls_offload *fco,
		      bool ingress);

/**
 * lan966x_tc_matchall - Handle all matchall filter commands
 * @port: The interface.
 * @f: Offload info.
 * @ingress: true if ingress rule.
 *
 * Returns:
 * 0 if ok.
 * Negative error code on failure.
 */
int lan966x_tc_matchall(struct lan966x_port *port,
			struct tc_cls_matchall_offload *fco,
			bool ingress);

/**
 * lan966x_setup_tc - Common entry point for tc
 * @dev: The netdevice.
 * @type: Type of operation.
 * @type_data: Operation dependent data.
 *
 * Returns:
 * 0 if ok.
 * Negative error code on failure.
 */
int lan966x_setup_tc(struct net_device *dev, enum tc_setup_type type,
		     void *type_data);

#endif /* _LAN966X_TC_H_ */
