// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

u32 lan9645x_emirror_get_dst(struct dsa_port *dp)
{
	struct lan9645x *lan9645x = dp->ds->priv;
	u32 emm = lan9645x->emirror_map;

	return BIT(dp->index) & emm ? (emm >> 16) : 0x0;
}

struct lan9645x_mirror *lan9645x_mirror_get(struct lan9645x *lan9645x, int to,
					    struct netlink_ext_ack *extack)
{
	struct lan9645x_mirror *m = lan9645x->mirror;

	if (m) {
		if (m->to != to) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Mirroring already configured towards different egress port");
			return ERR_PTR(-EBUSY);
		}

		refcount_inc(&m->refcount);
		return m;
	}

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);

	m->to = to;
	refcount_set(&m->refcount, 1);
	lan9645x->mirror = m;
	lan9645x->emirror_map = BIT(to) << 16;

	lan_wr(BIT(to), lan9645x, ANA_MIRRORPORTS);

	return m;
}

void lan9645x_mirror_put(struct lan9645x *lan9645x)
{
	struct lan9645x_mirror *m = lan9645x->mirror;

	if (!refcount_dec_and_test(&m->refcount))
		return;

	lan_wr(0, lan9645x, ANA_MIRRORPORTS);
	lan9645x->mirror = NULL;
	lan9645x->emirror_map = 0x0;
	kfree(m);
}

int lan9645x_mirror_port_add(struct lan9645x *lan9645x, int from, int to,
			     bool ingress, struct netlink_ext_ack *extack)
{
	struct lan9645x_mirror *m = lan9645x_mirror_get(lan9645x, to, extack);

	ASSERT_RTNL();

	if (IS_ERR(m))
		return PTR_ERR(m);

	if (ingress) {
		lan_rmw(ANA_PORT_CFG_SRC_MIRROR_ENA_SET(1),
			ANA_PORT_CFG_SRC_MIRROR_ENA, lan9645x,
			ANA_PORT_CFG(from));
	} else {
		lan9645x->emirror_map |= BIT(from);
		lan_rmw(BIT(from), BIT(from), lan9645x, ANA_EMIRRORPORTS);
	}

	return 0;
}

void lan9645x_mirror_port_del(struct lan9645x *lan9645x, int from, bool ingress)
{
	ASSERT_RTNL();

	if (ingress) {
		lan_rmw(ANA_PORT_CFG_SRC_MIRROR_ENA_SET(0),
			ANA_PORT_CFG_SRC_MIRROR_ENA, lan9645x,
			ANA_PORT_CFG(from));
	} else {
		lan9645x->emirror_map &= ~BIT(from);
		lan_rmw(0, BIT(from), lan9645x, ANA_EMIRRORPORTS);
	}

	lan9645x_mirror_put(lan9645x);
}
