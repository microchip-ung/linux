// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"
#include "lan9645x_vcap_utils.h"
#include "vcap_ag_api.h"
#include "vcap_api.h"
#include "vcap_api_client.h"

int lan9645x_vcap_add_key_mac(struct vcap_rule *rule,
			      enum vcap_key_field mac_field, unsigned char *mac)
{
	struct vcap_u48_key smac;

	/* VCAP fields must be little endian, since we write smac.value[0] first
	 * in stream.
	 */
	vcap_netbytes_copy(smac.value, mac, ETH_ALEN);
	memset(smac.mask, 0xff, ETH_ALEN);

	return vcap_rule_add_key_u48(rule, mac_field, &smac);
}

int lan9645x_vcap_rule_val_add(struct vcap_rule *rule, u16 l3_proto)
{
	int err;

	err = vcap_val_rule(rule, l3_proto);
	if (err)
		return err;

	err = vcap_add_rule(rule);
	if (err)
		return err;

	return err;
}

void lan9645x_dmac_enable(struct lan9645x_port *port, int lookup, bool enable)
{
	struct lan9645x *lan9645x = port->lan9645x;
	u32 value;

	if (enable) {
		value = lan_rd(lan9645x, ANA_VCAP_CFG(port->chip_port));
		value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
		value |= BIT(lookup);

		lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
			ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
			lan9645x, ANA_VCAP_CFG(port->chip_port));
	} else {
		value = lan_rd(lan9645x, ANA_VCAP_CFG(port->chip_port));
		value = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(value);
		value &= ~BIT(lookup);

		lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(value),
			ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
			lan9645x, ANA_VCAP_CFG(port->chip_port));
	}
}
