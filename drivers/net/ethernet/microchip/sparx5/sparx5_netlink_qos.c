// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include "sparx5_main_regs.h"
#include "sparx5_main.h"

static struct sparx5 *local_sparx5;
static struct genl_family sparx5_qos_genl_family;

static struct nla_policy sparx5_qos_genl_policy[MCHP_QOS_ATTR_END] = {
	[MCHP_QOS_ATTR_NONE] = { .type = NLA_UNSPEC },
	[MCHP_QOS_ATTR_DEV] = { .type = NLA_U32 },
	[MCHP_QOS_ATTR_PORT_CFG] = { .type = NLA_BINARY,
					.len = sizeof(struct mchp_qos_port_conf) },
	[MCHP_QOS_ATTR_DSCP] = { .type = NLA_U32 },
	[MCHP_QOS_ATTR_DSCP_PRIO_DPL] = { .type = NLA_BINARY,
					     .len = sizeof(struct mchp_qos_dscp_prio_dpl) },
};

static int sparx5_qos_genl_port_cfg_set(struct sk_buff *skb,
										struct genl_info *info)
{

	struct mchp_qos_port_conf cfg = {};
	struct net *net = sock_net(skb->sk);
	struct sparx5_port *port;
	struct net_device *dev;
	u32 ifindex;

	if (!info->attrs[MCHP_QOS_ATTR_DEV]) {
		pr_err("ATTR_DEV is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_QOS_ATTR_PORT_CFG]) {
		pr_err("ATTR_PORT_CFG is missing\n");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_ATTR_DEV]);
	dev = __dev_get_by_index(net, ifindex);
	if (!sparx5_netdevice_check(dev))
		return -EOPNOTSUPP;
	port = netdev_priv(dev);

	nla_memcpy(&cfg, info->attrs[MCHP_QOS_ATTR_PORT_CFG],
		   nla_len(info->attrs[MCHP_QOS_ATTR_PORT_CFG]));

	return sparx5_qos_port_conf_set(port, &cfg);
}

static int sparx5_qos_genl_port_cfg_get(struct sk_buff *skb,
					 struct genl_info *info)
{
	struct mchp_qos_port_conf cfg = {};
	struct net *net = sock_net(skb->sk);
	struct sparx5_port *port;
	struct net_device *dev;
	struct sk_buff *msg;
	u32 ifindex;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_QOS_ATTR_DEV]) {
		pr_err("ATTR_DEV is missing\n");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_ATTR_DEV]);
	dev = __dev_get_by_index(net, ifindex);
	if (!sparx5_netdevice_check(dev))
		return -EOPNOTSUPP;
	port = netdev_priv(dev);

	err = sparx5_qos_port_conf_get(port, &cfg);
	if (err) {
		goto invalid_info;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		pr_err("Allocate netlink msg failed\n");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &sparx5_qos_genl_family, 0,
			  MCHP_QOS_GENL_PORT_CFG_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_QOS_ATTR_PORT_CFG, sizeof(cfg), &cfg)) {
		pr_err("Failed nla_put\n");
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

static int sparx5_qos_genl_dscp_prio_dpl_set(struct sk_buff *skb,
					      struct genl_info *info)
{
	printk(KERN_INFO "sparx5_qos_genl_dscp_prio_dpl_set no implemented\n");
	return -1;
}

static int sparx5_qos_genl_dscp_prio_dpl_get(struct sk_buff *skb,
					      struct genl_info *info)
{
	printk(KERN_INFO "sparx5_qos_genl_dscp_prio_dpl_get no implemented\n");
	return -1;
}

static struct genl_ops sparx5_qos_genl_ops[] = {
	{
		.cmd    = MCHP_QOS_GENL_PORT_CFG_SET,
		.doit   = sparx5_qos_genl_port_cfg_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_QOS_GENL_PORT_CFG_GET,
		.doit   = sparx5_qos_genl_port_cfg_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_QOS_GENL_DSCP_PRIO_DPL_SET,
		.doit   = sparx5_qos_genl_dscp_prio_dpl_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_QOS_GENL_DSCP_PRIO_DPL_GET,
		.doit   = sparx5_qos_genl_dscp_prio_dpl_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
};

static struct genl_family sparx5_qos_genl_family = {
	.name		= MCHP_QOS_NETLINK,
	.hdrsize	= 0,
	.version	= 1,
	.maxattr	= MCHP_QOS_ATTR_MAX,
	.policy		= sparx5_qos_genl_policy,
	.ops		= sparx5_qos_genl_ops,
	.n_ops		= ARRAY_SIZE(sparx5_qos_genl_ops),
	.resv_start_op	= MCHP_QOS_GENL_DSCP_PRIO_DPL_GET + 1,
};

int sparx5_netlink_qos_init(struct sparx5 *sparx5)
{
	int err;

	local_sparx5 = sparx5;
	err = genl_register_family(&sparx5_qos_genl_family);
	if (err)
		pr_err("genl_register_family failed\n");

	return err;
}

void sparx5_netlink_qos_uninit(void)
{
	local_sparx5 = NULL;
	genl_unregister_family(&sparx5_qos_genl_family);
}
