// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include "lan966x_main.h"
#include "../mchp_ui_qos.h"

static struct genl_family lan966x_qos_fp_port_genl_family;

static struct nla_policy lan966x_qos_fp_port_genl_policy[MCHP_QOS_FP_PORT_ATTR_END] = {
	[MCHP_QOS_FP_PORT_ATTR_NONE] = { .type = NLA_UNSPEC },
	[MCHP_QOS_FP_PORT_ATTR_CONF] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_qos_fp_port_conf) },
	[MCHP_QOS_FP_PORT_ATTR_STATUS] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_qos_fp_port_status) },
};

static int lan966x_qos_fp_port_genl_conf_set(struct sk_buff *skb,
					     struct genl_info *info)
{
	struct mchp_qos_fp_port_conf nl_conf;
	struct lan966x_fp_port_conf conf = {};
	struct net *net = sock_net(skb->sk);
	struct lan966x_port *port;
	struct net_device *dev;
	u32 ifindex;

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]) {
		pr_err("ATTR_IDX is missing\n");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]);

	dev = __dev_get_by_index(net, ifindex);
	if (dev == NULL)
		return -EINVAL;

	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_CONF]) {
		pr_err("LAN966X_QOS_PORT_ATTR_CONF is missing\n");
		return -EINVAL;
	}

	nla_memcpy(&nl_conf, info->attrs[MCHP_QOS_FP_PORT_ATTR_CONF],
		   nla_len(info->attrs[MCHP_QOS_FP_PORT_ATTR_CONF]));

	conf.admin_status = nl_conf.admin_status;
	conf.enable_tx = !!nl_conf.enable_tx;
	conf.verify_disable_tx = !!nl_conf.verify_disable_tx;
	conf.verify_time = nl_conf.verify_time;
	conf.add_frag_size = nl_conf.add_frag_size;

	port = netdev_priv(dev);
	return lan966x_fp_set(port, &conf);
}

static int lan966x_qos_fp_port_genl_conf_get(struct sk_buff *skb,
					     struct genl_info *info)
{
	struct mchp_qos_fp_port_conf nl_conf = {};
	struct lan966x_fp_port_conf conf = {};
	struct net *net = sock_net(skb->sk);
	struct lan966x_port *port;
	struct net_device *dev;
	struct sk_buff *msg;
	u32 ifindex;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]) {
		pr_err("ATTR_IDX is missing\n");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]);

	dev = __dev_get_by_index(net, ifindex);
	if (dev == NULL)
		return -EINVAL;

	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	port = netdev_priv(dev);
	err = lan966x_fp_get(port, &conf);
	if (err)
		return -EINVAL;

	nl_conf.admin_status = conf.admin_status;
	nl_conf.enable_tx = conf.enable_tx;
	nl_conf.verify_disable_tx = conf.verify_disable_tx;
	nl_conf.verify_time = conf.verify_time;
	nl_conf.add_frag_size = conf.add_frag_size;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		pr_err("Allocate netlink msg failed\n");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &lan966x_qos_fp_port_genl_family, 0,
			  MCHP_QOS_FP_PORT_GENL_CONF_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_QOS_FP_PORT_ATTR_CONF, sizeof(nl_conf),
		    &nl_conf)) {
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

static int lan966x_qos_fp_port_genl_status_get(struct sk_buff *skb,
					       struct genl_info *info)
{
	struct mchp_qos_fp_port_status nl_status = {};
	struct mchp_qos_fp_port_status status = {};
	struct net *net = sock_net(skb->sk);
	struct lan966x_port *port;
	struct net_device *dev;
	struct sk_buff *msg;
	u32 ifindex;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]) {
		pr_err("ATTR_IDX is missing\n");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]);

	dev = __dev_get_by_index(net, ifindex);
	if (dev == NULL)
		return -EINVAL;

	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	port = netdev_priv(dev);
	err = lan966x_fp_status(port, &status);
	if (err)
		return -EINVAL;

	nl_status.hold_advance = status.hold_advance;
	nl_status.release_advance = status.release_advance;
	nl_status.preemption_active = status.preemption_active;
	nl_status.hold_request = status.hold_request;
	nl_status.status_verify = status.status_verify;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		pr_err("Allocate netlink msg failed\n");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &lan966x_qos_fp_port_genl_family, 0,
			  MCHP_QOS_FP_PORT_GENL_STATUS_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_QOS_FP_PORT_ATTR_STATUS, sizeof(nl_status),
		    &nl_status)) {
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

static struct genl_ops lan966x_qos_fp_port_genl_ops[] = {
	{
		.cmd    = MCHP_QOS_FP_PORT_GENL_CONF_SET,
		.doit   = lan966x_qos_fp_port_genl_conf_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_QOS_FP_PORT_GENL_CONF_GET,
		.doit   = lan966x_qos_fp_port_genl_conf_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_QOS_FP_PORT_GENL_STATUS_GET,
		.doit   = lan966x_qos_fp_port_genl_status_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	}
};

static struct genl_family lan966x_qos_fp_port_genl_family = {
	.name		= MCHP_FP_NETLINK,
	.hdrsize	= 0,
	.version	= 1,
	.maxattr	= MCHP_QOS_FP_PORT_ATTR_MAX,
	.policy		= lan966x_qos_fp_port_genl_policy,
	.ops		= lan966x_qos_fp_port_genl_ops,
	.n_ops		= ARRAY_SIZE(lan966x_qos_fp_port_genl_ops),
	.resv_start_op	= MCHP_QOS_FP_PORT_GENL_STATUS_GET + 1,
};

int lan966x_netlink_fp_init(void)
{
	int err;

	err = genl_register_family(&lan966x_qos_fp_port_genl_family);
	if (err)
		pr_err("genl_register_family failed\n");

	return err;
}

void lan966x_netlink_fp_uninit(void)
{
	genl_unregister_family(&lan966x_qos_fp_port_genl_family);
}
