// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include "lan966x_vcap.h"

static struct lan966x *local_lan966x;
static struct genl_family lan966x_vcap_genl_family;

enum lan966x_vcap_attr {
	LAN966X_VCAP_ATTR_NONE,
	LAN966X_VCAP_ATTR_VCAP,
	LAN966X_VCAP_ATTR_PRIO,
	LAN966X_VCAP_ATTR_HANDLE,
	LAN966X_VCAP_ATTR_RULE,
	LAN966X_VCAP_ATTR_COUNTER,

	/* This must be the last entry */
	LAN966X_VCAP_ATTR_END,
};

#define LAN966X_VCAP_ATTR_MAX (LAN966X_VCAP_ATTR_END - 1)

enum lan966x_vcap_genl {
	LAN966X_VCAP_GENL_ADD,
	LAN966X_VCAP_GENL_MOD,
	LAN966X_VCAP_GENL_GET,
	LAN966X_VCAP_GENL_DEL,
};

static struct nla_policy lan966x_vcap_genl_policy[LAN966X_VCAP_ATTR_END] = {
	[LAN966X_VCAP_ATTR_NONE] = { .type = NLA_UNSPEC },
	[LAN966X_VCAP_ATTR_VCAP] = { .type = NLA_U8 },
	[LAN966X_VCAP_ATTR_PRIO] = { .type = NLA_U16 },
	[LAN966X_VCAP_ATTR_HANDLE] = { .type = NLA_U64 },
	[LAN966X_VCAP_ATTR_RULE] = { .type = NLA_BINARY,
		.len = sizeof(struct lan966x_vcap_rule) },
	[LAN966X_VCAP_ATTR_COUNTER] = { .type = NLA_U32 },
};

static int lan966x_vcap_genl_set(struct sk_buff *skb,
				 struct genl_info *info)
{
	struct lan966x_vcap_rule rule = {};
	enum lan966x_vcap vcap;
	unsigned long handle;
	u16 prio;
	int err;

	if (!info->attrs[LAN966X_VCAP_ATTR_VCAP]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute VCAP is missing");
		return -EINVAL;
	}
	if (!info->attrs[LAN966X_VCAP_ATTR_PRIO]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute PRIO is missing");
		return -EINVAL;
	}
	if (!info->attrs[LAN966X_VCAP_ATTR_HANDLE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute HANDLE is missing");
		return -EINVAL;
	}
	if (!info->attrs[LAN966X_VCAP_ATTR_RULE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute RULE is missing");
		return -EINVAL;
	}

	vcap = nla_get_u8(info->attrs[LAN966X_VCAP_ATTR_VCAP]);
	prio = nla_get_u16(info->attrs[LAN966X_VCAP_ATTR_PRIO]);
	handle = nla_get_u64(info->attrs[LAN966X_VCAP_ATTR_HANDLE]);
	nla_memcpy(&rule, info->attrs[LAN966X_VCAP_ATTR_RULE],
		   nla_len(info->attrs[LAN966X_VCAP_ATTR_RULE]));

	if (info->genlhdr->cmd == LAN966X_VCAP_GENL_ADD) {
		err = lan966x_vcap_add(local_lan966x,
				       vcap,
				       LAN966X_VCAP_USER_VCAP_UTIL,
				       prio,
				       handle,
				       &rule);
		if (err)
			NL_SET_ERR_MSG_MOD(info->extack,
					   "lan966x_vcap_add() failed");
	} else {
		err = lan966x_vcap_mod(local_lan966x,
				       vcap,
				       LAN966X_VCAP_USER_VCAP_UTIL,
				       prio,
				       handle,
				       &rule);
		if (err)
			NL_SET_ERR_MSG_MOD(info->extack,
					   "lan966x_vcap_mod() failed");
	}
	return err;
}

static int lan966x_vcap_genl_get(struct sk_buff *skb,
				 struct genl_info *info)
{
	struct lan966x_vcap_rule rule = {};
	enum lan966x_vcap vcap;
	unsigned long handle;
	struct sk_buff *msg;
	u32 hits;
	void *hdr;
	u16 prio;
	int err;

	if (!info->attrs[LAN966X_VCAP_ATTR_VCAP]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute VCAP is missing");
		return -EINVAL;
	}
	if (!info->attrs[LAN966X_VCAP_ATTR_PRIO]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute PRIO is missing");
		return -EINVAL;
	}
	if (!info->attrs[LAN966X_VCAP_ATTR_HANDLE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute HANDLE is missing");
		return -EINVAL;
	}

	vcap = nla_get_u8(info->attrs[LAN966X_VCAP_ATTR_VCAP]);
	prio = nla_get_u16(info->attrs[LAN966X_VCAP_ATTR_PRIO]);
	handle = nla_get_u64(info->attrs[LAN966X_VCAP_ATTR_HANDLE]);

	err = lan966x_vcap_get(local_lan966x,
			       vcap,
			       LAN966X_VCAP_USER_VCAP_UTIL,
			       prio,
			       handle,
			       &rule,
			       &hits);
	if (err) {
		NL_SET_ERR_MSG_MOD(info->extack, "lan966x_vcap_get() failed");
		goto invalid_info;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "genlmsg_new() failed");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &lan966x_vcap_genl_family, 0,
			  LAN966X_VCAP_GENL_GET);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "genlmsg_put() failed");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, LAN966X_VCAP_ATTR_RULE, sizeof(rule), &rule)) {
		NL_SET_ERR_MSG_MOD(info->extack, "nla_put(RULE) failed");
		err = -EMSGSIZE;
		goto nla_put_failure;
	}

	if (nla_put_u32(msg, LAN966X_VCAP_ATTR_COUNTER, hits)) {
		NL_SET_ERR_MSG_MOD(info->extack, "nla_put_32(COUNTER) failed");
		err = -EMSGSIZE;
		goto nla_put_failure;
	}

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	genlmsg_cancel(msg, hdr);

err_msg_free:
	nlmsg_free(msg);

invalid_info:
	return err;

}

static int lan966x_vcap_genl_del(struct sk_buff *skb,
				 struct genl_info *info)
{
	enum lan966x_vcap vcap;
	unsigned long handle;
	u16 prio;
	int err;

	if (!info->attrs[LAN966X_VCAP_ATTR_VCAP]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute VCAP is missing");
		return -EINVAL;
	}
	if (!info->attrs[LAN966X_VCAP_ATTR_PRIO]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute PRIO is missing");
		return -EINVAL;
	}
	if (!info->attrs[LAN966X_VCAP_ATTR_HANDLE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute HANDLE is missing");
		return -EINVAL;
	}

	vcap = nla_get_u8(info->attrs[LAN966X_VCAP_ATTR_VCAP]);
	prio = nla_get_u16(info->attrs[LAN966X_VCAP_ATTR_PRIO]);
	handle = nla_get_u64(info->attrs[LAN966X_VCAP_ATTR_HANDLE]);

	err = lan966x_vcap_del(local_lan966x,
			       vcap,
			       LAN966X_VCAP_USER_VCAP_UTIL,
			       prio,
			       handle,
			       NULL);
	if (err)
		NL_SET_ERR_MSG_MOD(info->extack,
				   "lan966x_vcap_del() failed");
	return err;
}

static struct genl_ops lan966x_vcap_genl_ops[] = {
	{
		.cmd     = LAN966X_VCAP_GENL_ADD,
		.doit    = lan966x_vcap_genl_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags   = GENL_ADMIN_PERM,
	},
	{
		.cmd     = LAN966X_VCAP_GENL_MOD,
		.doit    = lan966x_vcap_genl_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags   = GENL_ADMIN_PERM,
	},
	{
		.cmd     = LAN966X_VCAP_GENL_GET,
		.doit    = lan966x_vcap_genl_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags   = GENL_ADMIN_PERM,
	},
	{
		.cmd     = LAN966X_VCAP_GENL_DEL,
		.doit    = lan966x_vcap_genl_del,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags   = GENL_ADMIN_PERM,
	},
};

static struct genl_family lan966x_vcap_genl_family = {
	.name		= "lan966x_vcap_nl",
	.hdrsize	= 0,
	.version	= 1,
	.maxattr	= LAN966X_VCAP_ATTR_MAX,
	.policy		= lan966x_vcap_genl_policy,
	.ops		= lan966x_vcap_genl_ops,
	.n_ops		= ARRAY_SIZE(lan966x_vcap_genl_ops),
	.resv_start_op	= LAN966X_VCAP_GENL_DEL + 1,
};

int lan966x_netlink_vcap_init(struct lan966x *lan966x)
{
	int err;

	local_lan966x = lan966x;
	err = genl_register_family(&lan966x_vcap_genl_family);
	if (err)
		pr_err("genl_register_family() failed\n");

	return err;
}

void lan966x_netlink_vcap_uninit(void)
{
	local_lan966x = NULL;
	genl_unregister_family(&lan966x_vcap_genl_family);
}
