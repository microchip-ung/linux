// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/err.h>
#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "lan9645x_netlink_fp.h"

#undef MCHP_FP_NETLINK
#define MCHP_FP_NETLINK "mchp_fp_9645x"

static struct lan9645x_nl_fp *nl_fp;

static struct genl_family lan9645x_qos_fp_port_genl_family;

static struct nla_policy lan9645x_qos_fp_port_genl_policy[MCHP_QOS_FP_PORT_ATTR_END] = {
	[MCHP_QOS_FP_PORT_ATTR_NONE] = { .type = NLA_UNSPEC },
	[MCHP_QOS_FP_PORT_ATTR_CONF] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_qos_fp_port_conf) },
	[MCHP_QOS_FP_PORT_ATTR_STATUS] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_qos_fp_port_status) },
};

static struct lan9645x_port *to_switch_port(struct sk_buff *skb,
					    struct genl_info *info)
{
	struct net *net = sock_net(skb->sk);
	struct lan9645x_port *p;
	struct net_device *dev;
	u32 ifindex;

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]);

	dev = __dev_get_by_index(net, ifindex);
	if (dev == NULL)
		return ERR_PTR(-EINVAL);

	p = lan9645x_port_from_netdev(dev);
	if (IS_ERR_OR_NULL(p))
		return ERR_PTR(-ENOTSUPP);

	return p;
}

static bool port_link_status(struct lan9645x_port *p)
{
	struct net_device *dev = lan9645x_port_to_ndev(p);

	return dev != NULL && netif_carrier_ok(dev);
}

static int lan9645x_qos_fp_port_genl_conf_set(struct sk_buff *skb,
					      struct genl_info *info)
{
	struct mchp_qos_fp_port_conf nl_conf;
	struct lan9645x_fp_port_conf conf = {};
	struct lan9645x_port *p;
	int err;

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]) {
		pr_err("ATTR_IDX is missing\n");
		return -EINVAL;
	}

	p = to_switch_port(skb, info);
	if (IS_ERR(p))
		return PTR_ERR(p);

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_CONF]) {
		pr_err("LAN9645X_QOS_PORT_ATTR_CONF is missing\n");
		return -EINVAL;
	}

	nla_memcpy(&nl_conf, info->attrs[MCHP_QOS_FP_PORT_ATTR_CONF],
		   nla_len(info->attrs[MCHP_QOS_FP_PORT_ATTR_CONF]));

	conf.admin_status = nl_conf.admin_status;
	conf.enable_tx = !!nl_conf.enable_tx;
	conf.verify_disable_tx = !!nl_conf.verify_disable_tx;
	conf.verify_time = nl_conf.verify_time;
	conf.add_frag_size = nl_conf.add_frag_size;

	rtnl_lock();
	err = lan9645x_fp_set(p, &conf, port_link_status(p));
	rtnl_unlock();

	return err;
}

static int lan9645x_qos_fp_port_genl_conf_get(struct sk_buff *skb,
					     struct genl_info *info)
{
	struct mchp_qos_fp_port_conf nl_conf = {};
	struct lan9645x_fp_port_conf conf = {};
	struct lan9645x_port *p;
	struct sk_buff *msg;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]) {
		pr_err("ATTR_IDX is missing\n");
		return -EINVAL;
	}

	p = to_switch_port(skb, info);
	if (IS_ERR(p))
		return PTR_ERR(p);

	rtnl_lock();
	err = lan9645x_fp_get(p, &conf);
	rtnl_unlock();
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
			  &lan9645x_qos_fp_port_genl_family, 0,
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

static int lan9645x_qos_fp_port_genl_status_get(struct sk_buff *skb,
					       struct genl_info *info)
{
	struct mchp_qos_fp_port_status nl_status = {};
	struct lan9645x_fp_port_status status = {};
	struct lan9645x_port *p;
	struct sk_buff *msg;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_QOS_FP_PORT_ATTR_IDX]) {
		pr_err("ATTR_IDX is missing\n");
		return -EINVAL;
	}

	p = to_switch_port(skb, info);
	if (IS_ERR(p))
		return PTR_ERR(p);

	rtnl_lock();
	err = lan9645x_fp_status(p, &status);
	rtnl_unlock();
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
			  &lan9645x_qos_fp_port_genl_family, 0,
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

static struct genl_ops lan9645x_qos_fp_port_genl_ops[] = {
	{
		.cmd    = MCHP_QOS_FP_PORT_GENL_CONF_SET,
		.doit   = lan9645x_qos_fp_port_genl_conf_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_QOS_FP_PORT_GENL_CONF_GET,
		.doit   = lan9645x_qos_fp_port_genl_conf_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_QOS_FP_PORT_GENL_STATUS_GET,
		.doit   = lan9645x_qos_fp_port_genl_status_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	}
};

static struct genl_family lan9645x_qos_fp_port_genl_family = {
	.name		= MCHP_FP_NETLINK,
	.hdrsize	= 0,
	.version	= 1,
	.maxattr	= MCHP_QOS_FP_PORT_ATTR_MAX,
	.policy		= lan9645x_qos_fp_port_genl_policy,
	.parallel_ops	= true,
	.ops		= lan9645x_qos_fp_port_genl_ops,
	.n_ops		= ARRAY_SIZE(lan9645x_qos_fp_port_genl_ops),
	.resv_start_op	= MCHP_QOS_FP_PORT_GENL_STATUS_GET + 1,
};

int lan9645x_netlink_fp_init(struct lan9645x* lan9645x)
{
	int err;

	nl_fp = devm_kzalloc(lan9645x->dev, sizeof(*nl_fp), GFP_KERNEL);
	if (!nl_fp)
		return -ENOMEM;

	nl_fp->lan9645x = lan9645x;

	err = genl_register_family(&lan9645x_qos_fp_port_genl_family);
	if (err) {
		dev_err(lan9645x->dev, "genl_register_family '%s' failed",
			lan9645x_qos_fp_port_genl_family.name);
		goto err_out;
	}

	dev_info(lan9645x->dev, "Registered netlink family '%s'",
		 lan9645x_qos_fp_port_genl_family.name);

	return 0;

err_out:
	nl_fp = NULL;
	return err;
}

void lan9645x_netlink_fp_uninit(void)
{
	genl_unregister_family(&lan9645x_qos_fp_port_genl_family);
	nl_fp = NULL;
}
