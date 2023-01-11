// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver VCAP Library
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 * The Sparx5 Chip Register Model can be browsed at this location:
 * https://github.com/microchip-ung/sparx-5_reginfo
 */

#ifndef __SPARX5_VCAP_IMPL_H__
#define __SPARX5_VCAP_IMPL_H__

#include <linux/types.h>

struct sparx5;
struct vcap_admin;
struct net_device;
struct vcap_keyset_list;
enum vcap_keyfield_set;
enum vcap_key_field;

#define SPARX5_VCAP_CID_IS0_L0 VCAP_CID_INGRESS_L0 /* IS0/CLM lookup 0 */
#define SPARX5_VCAP_CID_IS0_L1 VCAP_CID_INGRESS_L1 /* IS0/CLM lookup 1 */
#define SPARX5_VCAP_CID_IS0_L2 VCAP_CID_INGRESS_L2 /* IS0/CLM lookup 2 */
#define SPARX5_VCAP_CID_IS0_L3 VCAP_CID_INGRESS_L3 /* IS0/CLM lookup 3 */
#define SPARX5_VCAP_CID_IS0_L4 VCAP_CID_INGRESS_L4 /* IS0/CLM lookup 4 */
#define SPARX5_VCAP_CID_IS0_L5 VCAP_CID_INGRESS_L5 /* IS0/CLM lookup 5 */
#define SPARX5_VCAP_CID_IS0_MAX (VCAP_CID_INGRESS_L5 + VCAP_CID_LOOKUP_SIZE - 1) /* IS0/CLM Max */

#define SPARX5_VCAP_CID_IS2_L0 VCAP_CID_INGRESS_STAGE2_L0 /* IS2 lookup 0 */
#define SPARX5_VCAP_CID_IS2_L1 VCAP_CID_INGRESS_STAGE2_L1 /* IS2 lookup 1 */
#define SPARX5_VCAP_CID_IS2_L2 VCAP_CID_INGRESS_STAGE2_L2 /* IS2 lookup 2 */
#define SPARX5_VCAP_CID_IS2_L3 VCAP_CID_INGRESS_STAGE2_L3 /* IS2 lookup 3 */
#define SPARX5_VCAP_CID_IS2_MAX (VCAP_CID_INGRESS_STAGE2_L3 + VCAP_CID_LOOKUP_SIZE - 1) /* IS2 Max */

#define SPARX5_VCAP_CID_ES0_L0 VCAP_CID_EGRESS_L0 /* ES0 lookup 0 */
#define SPARX5_VCAP_CID_ES0_MAX (VCAP_CID_EGRESS_L1 - 1) /* ES0 Max */

#define SPARX5_VCAP_CID_ES2_L0 VCAP_CID_EGRESS_STAGE2_L0 /* ES2 lookup 0 */
#define SPARX5_VCAP_CID_ES2_L1 VCAP_CID_EGRESS_STAGE2_L1 /* ES2 lookup 1 */
#define SPARX5_VCAP_CID_ES2_MAX (VCAP_CID_EGRESS_STAGE2_L1 + VCAP_CID_LOOKUP_SIZE - 1) /* ES2 Max */

/* Controls how PORT_MASK is applied */
enum SPX5_PORT_MASK_MODE {
	SPX5_PMM_OR_DSTMASK,
	SPX5_PMM_AND_VLANMASK,
	SPX5_PMM_REPLACE_PGID,
	SPX5_PMM_REPLACE_ALL,
	SPX5_PMM_REDIR_PGID,
	SPX5_PMM_OR_PGID_MASK,
};

/* Get the keyset name from the Sparx5 VCAP model */
const char *sparx5_vcap_keyset_name(struct net_device *ndev,
				    enum vcap_keyfield_set keyset);
/* Get the key name from the Sparx5 VCAP model */
const char *sparx5_vcap_key_name(struct net_device *ndev,
				 enum vcap_key_field key);
/* Get the port keyset for the vcap lookup */
int sparx5_vcap_get_port_keyset(struct net_device *ndev,
				struct vcap_admin *admin,
				int cid,
				u16 l3_proto,
				struct vcap_keyset_list *keysetlist);
/* Set the port keyset for the vcap lookup */
void sparx5_vcap_set_port_keyset(struct net_device *ndev,
				 struct vcap_admin *admin,
				 int cid,
				 u16 l3proto,
				 u8 l4proto,
				 enum vcap_keyfield_set keyset);
/* Output information about port keysets and port keyset sticky bits */
int sparx5_vcap_port_info(struct sparx5 *sparx5,
				 struct vcap_admin *admin,
				 int (*pf)(void *out, int arg, const char *f, ...),
				 void *out, int arg);
#endif /* __SPARX5_VCAP_IMPL_H__ */
