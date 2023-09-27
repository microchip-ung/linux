// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include "lan966x_main.h"

#define MCHP_PMAC_NETLINK "mchp_pmac_nl"

enum mchp_pmac_attr {
	MCHP_PMAC_ATTR_NONE,
	MCHP_PMAC_ATTR_IFINDEX,
	MCHP_PMAC_ATTR_MAC,
	MCHP_PMAC_ATTR_VLAN,
	MCHP_PMAC_ATTR_OUI,
	MCHP_PMAC_ATTR_ENTRIES,
	MCHP_PMAC_ATTR_ENTRY,
	MCHP_PMAC_ATTR_ENTRY_INDEX,
	MCHP_PMAC_ATTR_ENTRY_IFINDEXES,
	MCHP_PMAC_ATTR_ENTRY_VLAN,

	/* This must be the last entry */
	MCHP_PMAC_ATTR_END,
};

#define MCHP_PMAC_ATTR_MAX (MCHP_PMAC_ATTR_END - 1)

enum mchp_pmac_genl {
	MCHP_PMAC_GENL_ADD,
	MCHP_PMAC_GENL_DEL,
	MCHP_PMAC_GENL_GET,
	MCHP_PMAC_GENL_PURGE,
};

static struct genl_family lan966x_pmac_genl_family;
static struct lan966x *local_lan966x;

static struct nla_policy lan966x_pmac_genl_policy[MCHP_PMAC_ATTR_END] = {
	[MCHP_PMAC_ATTR_NONE] = { .type = NLA_UNSPEC },
	[MCHP_PMAC_ATTR_MAC] = { .type = NLA_BINARY, .len = ETH_ALEN },
	[MCHP_PMAC_ATTR_IFINDEX] = { .type = NLA_U32 },
	[MCHP_PMAC_ATTR_VLAN] = { .type = NLA_U16 },
	[MCHP_PMAC_ATTR_OUI] = { .type = NLA_U32 },
	[MCHP_PMAC_ATTR_ENTRIES] = { .type = NLA_NESTED },
	[MCHP_PMAC_ATTR_ENTRY] = { .type = NLA_NESTED },
	[MCHP_PMAC_ATTR_ENTRY_INDEX] = { .type = NLA_U16 },
	[MCHP_PMAC_ATTR_ENTRY_IFINDEXES] = { .type = NLA_NESTED },
	[MCHP_PMAC_ATTR_ENTRY_VLAN] = { .type = NLA_U16 },
};

static int lan966x_pmac_genl_parse(struct sk_buff *skb,
				   struct genl_info *info,
				   struct lan966x_port **port,
				   u8 *mac, u16 *vlan)
{
	struct net *net = sock_net(skb->sk);
	struct net_device *dev;
	u32 ifindex;

	if (!info->attrs[MCHP_PMAC_ATTR_IFINDEX]) {
		pr_err("ATTR_IFINDEX is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_PMAC_ATTR_MAC]) {
		pr_err("ATTR_MAC is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_PMAC_ATTR_VLAN]) {
		pr_err("ATTR_VLAN is missing\n");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_PMAC_ATTR_IFINDEX]);
	dev = __dev_get_by_index(net, ifindex);
	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;
	*port = netdev_priv(dev);

	*vlan = nla_get_u16(info->attrs[MCHP_PMAC_ATTR_VLAN]);
	nla_memcpy(mac, info->attrs[MCHP_PMAC_ATTR_MAC], ETH_ALEN);

	return 0;
}

static int lan966x_pmac_genl_add(struct sk_buff *skb,
				 struct genl_info *info)
{
	struct lan966x_port *port = NULL;
	u8 mac[ETH_ALEN];
	u16 vlan;
	int ret;

	ret = lan966x_pmac_genl_parse(skb, info, &port, mac, &vlan);
	if (ret < 0)
		return ret;

	return lan966x_pmac_add(port, mac, vlan);
}

static int lan966x_pmac_genl_del(struct sk_buff *skb,
				 struct genl_info *info)
{
	struct lan966x_port *port = NULL;
	u8 mac[ETH_ALEN];
	u16 vlan;
	int ret;

	ret = lan966x_pmac_genl_parse(skb, info, &port, mac, &vlan);
	if (ret < 0)
		return ret;

	return lan966x_pmac_del(port, mac, vlan);
}

static int lan966x_pmac_genl_get(struct sk_buff *skb,
				 struct genl_info *info)
{
	struct lan966x *lan966x = local_lan966x;
	struct lan966x_pmac *pmac = &lan966x->pmac;
	struct lan966x_pmac_entry *pmac_entry;
	struct nlattr *start_entries;
	struct sk_buff *msg;
	void *hdr;
	int err;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		pr_err("Allocate netlink msg failed\n");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &lan966x_pmac_genl_family, 0,
			  MCHP_PMAC_GENL_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (pmac->oui < 0)
		goto done;

	nla_put_u32(msg, MCHP_PMAC_ATTR_OUI, pmac->oui);

	start_entries = nla_nest_start(msg, MCHP_PMAC_ATTR_ENTRIES);
	if (!start_entries)
		goto nla_put_failure;

	list_for_each_entry(pmac_entry, &pmac->pmac_entries, list) {
		struct nlattr *start_ifindexes;
		struct nlattr *start_entry;

		start_entry = nla_nest_start(msg, MCHP_PMAC_ATTR_ENTRY);
		if (!start_entry)
			goto nla_put_failure;

		nla_put_u16(msg, MCHP_PMAC_ATTR_ENTRY_INDEX, pmac_entry->index);
		nla_put_u16(msg, MCHP_PMAC_ATTR_ENTRY_VLAN, pmac_entry->vlan->vlan);

		start_ifindexes = nla_nest_start(msg, MCHP_PMAC_ATTR_ENTRY_IFINDEXES);
		if (!start_ifindexes)
			goto nla_put_failure;

		for (int i = 0; i < NUM_PHYS_PORTS; ++i) {
			if (!(pmac_entry->ports & BIT(i)))
				continue;

			nla_put_u32(msg, MCHP_PMAC_ATTR_IFINDEX,
				    lan966x->ports[i]->dev->ifindex);
		}
		nla_nest_end(msg, start_ifindexes);

		nla_nest_end(msg, start_entry);
	}
	nla_nest_end(msg, start_entries);

done:
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	genlmsg_cancel(msg, hdr);

err_msg_free:
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int lan966x_pmac_genl_purge(struct sk_buff *skb,
				   struct genl_info *info)
{
	return lan966x_pmac_purge(local_lan966x);
}

static struct genl_ops lan966x_pmac_genl_ops[] = {
	{
		.cmd    = MCHP_PMAC_GENL_ADD,
		.doit   = lan966x_pmac_genl_add,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_PMAC_GENL_DEL,
		.doit   = lan966x_pmac_genl_del,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_PMAC_GENL_GET,
		.doit   = lan966x_pmac_genl_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_PMAC_GENL_PURGE,
		.doit   = lan966x_pmac_genl_purge,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	}
};

static struct genl_family lan966x_pmac_genl_family = {
	.name		= MCHP_PMAC_NETLINK,
	.hdrsize	= 0,
	.version	= 1,
	.maxattr	= MCHP_PMAC_ATTR_MAX,
	.policy		= lan966x_pmac_genl_policy,
	.ops		= lan966x_pmac_genl_ops,
	.n_ops		= ARRAY_SIZE(lan966x_pmac_genl_ops),
	.resv_start_op	= MCHP_PMAC_GENL_PURGE + 1,
};

int lan966x_netlink_pmac_init(struct lan966x *lan966x)
{
	int err;

	local_lan966x = lan966x;
	err = genl_register_family(&lan966x_pmac_genl_family);
	if (err)
		pr_err("genl_register_family failed\n");

	return err;
}

void lan966x_netlink_pmac_uninit(void)
{
	genl_unregister_family(&lan966x_pmac_genl_family);
	local_lan966x = NULL;
}
