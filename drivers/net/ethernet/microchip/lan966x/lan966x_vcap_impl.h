// SPDX-License-Identifier: GPL-2.0+
/* Microchip Lan966x Switch driver VCAP Library
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 */

#ifndef __LAN966X_VCAP_IMPL_H__
#define __LAN966X_VCAP_IMPL_H__

#include "lan966x_main.h"

#include <vcap_api.h>
#include <vcap_api_client.h>

#define LAN966X_VCAP_CID_IS1_L0 VCAP_CID_INGRESS_L0 /* IS1 lookup 0 */
#define LAN966X_VCAP_CID_IS1_L1 VCAP_CID_INGRESS_L1 /* IS1 lookup 1 */
#define LAN966X_VCAP_CID_IS1_L2 VCAP_CID_INGRESS_L2 /* IS1 lookup 2 */
#define LAN966X_VCAP_CID_IS1_MAX (VCAP_CID_INGRESS_L3 - 1) /* IS1 Max */

#define LAN966X_VCAP_CID_IS2_L0 VCAP_CID_INGRESS_STAGE2_L0 /* IS2 lookup 0 */
#define LAN966X_VCAP_CID_IS2_L1 VCAP_CID_INGRESS_STAGE2_L1 /* IS2 lookup 1 */
#define LAN966X_VCAP_CID_IS2_MAX (VCAP_CID_INGRESS_STAGE2_L2 - 1) /* IS2 Max */

#define LAN966X_VCAP_CID_ES0_L0 VCAP_CID_EGRESS_L0 /* ES0 lookup 0 */
#define LAN966X_VCAP_CID_ES0_MAX (VCAP_CID_EGRESS_L1 - 1) /* ES0 Max */

/* Controls how PORT_MASK is applied */
enum LAN966X_PORT_MASK_MODE {
	LAN966X_PMM_NO_ACTION,
	LAN966X_PMM_REPLACE,
	LAN966X_PMM_FORWARDING,
	LAN966X_PMM_REDIRECT,
};

int lan966x_vcap_init(struct lan966x *lan966x);
void lan966x_vcap_uninit(struct lan966x *lan966x);

/* Get the keyset name from the LAN966X VCAP model */
const char *lan966x_vcap_keyset_name(struct net_device *ndev,
				    enum vcap_keyfield_set keyset);
/* Get the key name from the LAN966X VCAP model */
const char *lan966x_vcap_key_name(struct net_device *ndev,
				 enum vcap_key_field key);
/* Get the port keyset for the vcap lookup */
int lan966x_vcap_get_port_keyset(struct net_device *ndev,
				struct vcap_admin *admin,
				int cid,
				u16 l3_proto,
				struct vcap_keyset_list *keysetlist);
/* Set the port keyset for the vcap lookup */
void lan966x_vcap_set_port_keyset(struct net_device *ndev,
				 struct vcap_admin *admin,
				 int cid,
				 u16 l3proto,
				 u8 l4proto,
				 enum vcap_keyfield_set keyset);
/* Convert chain id to vcap lookup id */
int lan966x_vcap_cid_to_lookup(struct vcap_admin *admin, int cid);
#endif /* __LAN966X_VCAP_IMPL_H__ */
