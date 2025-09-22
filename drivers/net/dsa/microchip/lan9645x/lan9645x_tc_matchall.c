// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */
#include <linux/netlink.h>

#include "lan9645x_main.h"

int lan9645x_tc_matchall_goto_add(struct lan9645x_port *p,
				  struct tc_cls_matchall_offload *f)
{
	struct net_device *dev = lan9645x_port_to_ndev(p);
	struct netlink_ext_ack *extack = f->common.extack;
	struct lan9645x *lan9645x = p->lan9645x;
	struct flow_action_entry *act;
	int err, from_cid, to_cid;

	if (!(flow_offload_has_one_action(&f->rule->action) &&
	      f->rule->action.entries[0].id == FLOW_ACTION_GOTO)) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Only one GOTO action per filter is supported");
		return -EOPNOTSUPP;
	}

	act = &f->rule->action.entries[0];
	from_cid = f->common.chain_index;
	to_cid = act->chain_index;

	err = vcap_enable_lookups(lan9645x->vcap_ctrl, dev, from_cid, to_cid,
				  f->cookie, true);
	if (err == -EFAULT) {
		NL_SET_ERR_MSG_MOD(extack, "Unsupported goto chain");
		return -EOPNOTSUPP;
	}

	if (err == -EADDRINUSE) {
		NL_SET_ERR_MSG_MOD(extack, "VCAP already enabled");
		return -EOPNOTSUPP;
	}

	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Could not enable VCAP lookups");
		return err;
	}

	return 0;
}

int lan9645x_tc_matchall_goto_del(struct lan9645x_port *p,
				  struct tc_cls_matchall_offload *f)
{
	struct net_device *dev = lan9645x_port_to_ndev(p);
	struct netlink_ext_ack *extack = f->common.extack;
	struct lan9645x *lan9645x = p->lan9645x;
	int err;

	err = vcap_enable_lookups(lan9645x->vcap_ctrl, dev, 0, 0, f->cookie,
				  false);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Could not disable VCAP lookups");
		return err;
	}

	return 0;
}
