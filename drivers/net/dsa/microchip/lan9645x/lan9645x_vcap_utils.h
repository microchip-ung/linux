/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef _LAN9645X_VCAP_UTILS_H_
#define _LAN9645X_VCAP_UTILS_H_

#include "lan9645x_main.h"
#include "vcap_api.h"

/* Lookup encoding for VCAP_S2_CFG.S2_*_DIS, except S2_IP6_CFG */
#define S2_LOOKUP1 BIT(0)
#define S2_LOOKUP2 BIT(1)

enum s2_ip6_cfg {
	IP6_TCP_UDP_OR_OTHER = 0,
	IP6_STD = 1,
	IP4_TCP_UDP_OR_OTHER = 2,
	IP6_MAC_ETYPE = 3,
};

/* This field contains two separate fields. Add them manually for convenience. */
#define ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1                  GENMASK(3, 2)
#define ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP2                  GENMASK(5, 4)
#define ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1_SET(x)\
	FIELD_PREP(ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1, x)
#define ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP2_SET(x)\
	FIELD_PREP(ANA_VCAP_S2_CFG_IP6_CFG_LOOKUP1, x)

enum is2_mask_mode {
	NO_ACTION = 0,
	PERMIT_MASK = 1, /* DMAC lookup AND'ed with portmask */
	POLICY = 2,      /* Replace DMAC lookup with portmask */
	REDIRECT = 3,    /* Replace SRC, AGGR, VLAN, DMAC lookup with portmask */
};

int lan9645x_vcap_rule_val_add(struct vcap_rule *rule, u16 l3_proto);
int lan9645x_vcap_add_key_mac(struct vcap_rule *rule,
			      enum vcap_key_field mac_field,
			      unsigned char *mac);
void lan9645x_dmac_enable(struct lan9645x_port *port, int lookup, bool enable);

#endif
