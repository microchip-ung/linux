// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "lan9645x_netlink_qos.h"

static struct lan9645x_netlink_qos *nl_qos;
static struct genl_family lan9645x_qos_genl_family;

#undef MCHP_QOS_NETLINK
#define MCHP_QOS_NETLINK "mchp_qos_9645x"

static struct nla_policy lan9645x_qos_genl_policy[MCHP_QOS_ATTR_END] = {
	[MCHP_QOS_ATTR_NONE] = { .type = NLA_UNSPEC },
	[MCHP_QOS_ATTR_DEV] = { .type = NLA_U32 },
	[MCHP_QOS_ATTR_PORT_CFG] = {
		.type = NLA_BINARY,
		.len = sizeof(struct mchp_qos_port_conf),
	},
	[MCHP_QOS_ATTR_DSCP] = { .type = NLA_U32 },
	[MCHP_QOS_ATTR_DSCP_PRIO_DPL] = {
		.type = NLA_BINARY,
		.len = sizeof(struct mchp_qos_dscp_prio_dpl),
	},
};

static int lan9645x_qos_genl_port_cfg_set(struct sk_buff *skb,
					  struct genl_info *info)
{
	struct mchp_qos_port_conf cfg = {};
	struct net *net = sock_net(skb->sk);
	struct net_device *dev;
	u32 ifindex;
	int err;

	if (!info->attrs[MCHP_QOS_ATTR_DEV]) {
		dev_err(nl_qos->lan9645x->dev, "ATTR_DEV is missing");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_QOS_ATTR_PORT_CFG]) {
		dev_err(nl_qos->lan9645x->dev, "ATTR_PORT_CFG is missing");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_ATTR_DEV]);
	dev = __dev_get_by_index(net, ifindex);

	nla_memcpy(&cfg, info->attrs[MCHP_QOS_ATTR_PORT_CFG],
		   nla_len(info->attrs[MCHP_QOS_ATTR_PORT_CFG]));

	rtnl_lock();
	err = lan9645x_qos_port_conf_set(nl_qos, dev, &cfg);
	rtnl_unlock();
	return err;
}

static int lan9645x_qos_genl_port_cfg_get(struct sk_buff *skb,
					  struct genl_info *info)
{
	struct mchp_qos_port_conf cfg = {};
	struct net *net = sock_net(skb->sk);
	struct net_device *dev;
	struct sk_buff *msg;
	u32 ifindex;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_QOS_ATTR_DEV]) {
		dev_err(nl_qos->lan9645x->dev, "ATTR_DEV is missing");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_QOS_ATTR_DEV]);
	dev = __dev_get_by_index(net, ifindex);

	rtnl_lock();
	err = lan9645x_qos_port_conf_get(nl_qos, dev, &cfg);
	rtnl_unlock();
	if (err)
		goto invalid_info;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		dev_err(nl_qos->lan9645x->dev, "Allocate netlink msg failed");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &lan9645x_qos_genl_family, 0,
			  MCHP_QOS_GENL_PORT_CFG_GET);
	if (!hdr) {
		dev_err(nl_qos->lan9645x->dev, "Create msg hdr failed");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_QOS_ATTR_PORT_CFG, sizeof(cfg), &cfg)) {
		dev_err(nl_qos->lan9645x->dev, "Failed nla_put");
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

static int lan9645x_qos_genl_dscp_prio_dpl_set(struct sk_buff *skb,
					       struct genl_info *info)
{
	struct mchp_qos_dscp_prio_dpl cfg = {};
	u32 dscp;
	int err;

	if (!info->attrs[MCHP_QOS_ATTR_DSCP]) {
		dev_err(nl_qos->lan9645x->dev, "ATTR_DEV is missing");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_QOS_ATTR_DSCP_PRIO_DPL]) {
		dev_err(nl_qos->lan9645x->dev, "ATTR_DSCP_PRIO_DPL is missing");
		return -EINVAL;
	}

	dscp = nla_get_u32(info->attrs[MCHP_QOS_ATTR_DSCP]);

	nla_memcpy(&cfg, info->attrs[MCHP_QOS_ATTR_DSCP_PRIO_DPL],
		   nla_len(info->attrs[MCHP_QOS_ATTR_DSCP_PRIO_DPL]));

	rtnl_lock();
	err = lan9645x_qos_dscp_prio_dpl_set(nl_qos, dscp, &cfg);
	rtnl_unlock();
	return err;
}

static int lan9645x_qos_genl_dscp_prio_dpl_get(struct sk_buff *skb,
					       struct genl_info *info)
{
	struct mchp_qos_dscp_prio_dpl cfg = {};
	struct sk_buff *msg;
	void *hdr;
	u32 dscp;
	int err;

	if (!info->attrs[MCHP_QOS_ATTR_DSCP]) {
		dev_err(nl_qos->lan9645x->dev, "ATTR_DSCP is missing");
		return -EINVAL;
	}

	dscp = nla_get_u32(info->attrs[MCHP_QOS_ATTR_DSCP]);

	rtnl_lock();
	err = lan9645x_qos_dscp_prio_dpl_get(nl_qos, dscp, &cfg);
	rtnl_unlock();
	if (err)
		goto invalid_info;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		dev_err(nl_qos->lan9645x->dev, "Allocate netlink msg failed");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &lan9645x_qos_genl_family, 0,
			  MCHP_QOS_GENL_DSCP_PRIO_DPL_GET);
	if (!hdr) {
		dev_err(nl_qos->lan9645x->dev, "Create msg hdr failed");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_QOS_ATTR_DSCP_PRIO_DPL, sizeof(cfg), &cfg)) {
		dev_err(nl_qos->lan9645x->dev, "Failed nla_put");
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

static struct genl_ops lan9645x_qos_genl_ops[] = {
	{
		.cmd = MCHP_QOS_GENL_PORT_CFG_SET,
		.doit = lan9645x_qos_genl_port_cfg_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MCHP_QOS_GENL_PORT_CFG_GET,
		.doit = lan9645x_qos_genl_port_cfg_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MCHP_QOS_GENL_DSCP_PRIO_DPL_SET,
		.doit = lan9645x_qos_genl_dscp_prio_dpl_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = MCHP_QOS_GENL_DSCP_PRIO_DPL_GET,
		.doit = lan9645x_qos_genl_dscp_prio_dpl_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = GENL_ADMIN_PERM,
	},
};

static struct genl_family lan9645x_qos_genl_family = {
	.name = MCHP_QOS_NETLINK,
	.hdrsize = 0,
	.version = 1,
	.maxattr = MCHP_QOS_ATTR_MAX,
	.policy = lan9645x_qos_genl_policy,
	.ops = lan9645x_qos_genl_ops,
	.n_ops = ARRAY_SIZE(lan9645x_qos_genl_ops),
	.resv_start_op = MCHP_QOS_GENL_DSCP_PRIO_DPL_GET + 1,
};

int lan9645x_netlink_qos_init(struct lan9645x *lan9645x)
{
	struct mchp_prio_dpl_pcp_dei *e_pcp_dei;
	struct mchp_pcp_dei_prio_dpl *i_qos_dpl;
	struct mchp_qos_port_conf *pqos_map;
	u8 qos, dpl, pcp, dei;
	int err, p;
	u32 cfg;

	nl_qos = devm_kzalloc(lan9645x->dev, sizeof(*nl_qos), GFP_KERNEL);
	if (!nl_qos)
		return -ENOMEM;

	nl_qos->lan9645x = lan9645x;

	lan9645x_for_each_chipport(lan9645x, p)
	{
		pqos_map = &nl_qos->qos_map[p];

		pqos_map->i_mode.tag_map_enable = true;
		pqos_map->i_mode.dscp_map_enable = false;

		for (pcp = 0; pcp < 8; pcp++) {
			for (dei = 0; dei < 2; dei++) {
				cfg = lan_rd(lan9645x, ANA_PCP_DEI_CFG(p, 8 * dei + pcp));
				i_qos_dpl = &pqos_map->i_pcp_dei_prio_dpl_map[pcp][dei];
				i_qos_dpl->prio = ANA_PCP_DEI_CFG_QOS_PCP_DEI_VAL_GET(cfg);
				i_qos_dpl->dpl = ANA_PCP_DEI_CFG_DP_PCP_DEI_VAL_GET(cfg);
			}
		}

		for (qos = 0; qos < 8; qos++) {
			for (dpl = 0; dpl < 2; dpl++) {
				cfg = lan_rd(lan9645x, REW_PCP_DEI_CFG(p, 8 * dpl + qos));
				e_pcp_dei = &pqos_map->e_prio_dpl_pcp_dei_map[qos][dpl];
				e_pcp_dei->pcp = REW_PCP_DEI_CFG_PCP_QOS_VAL_GET(cfg);
				e_pcp_dei->dei = REW_PCP_DEI_CFG_DEI_QOS_VAL_GET(cfg);
			}
		}

		nl_qos->qos_map[p].e_mode = MCHP_E_MODE_MAPPED;
	}

	err = genl_register_family(&lan9645x_qos_genl_family);
	if (err) {
		dev_err(lan9645x->dev, "genl_register_family '%s' failed",
			lan9645x_qos_genl_family.name);
		return err;
	}

	dev_info(lan9645x->dev, "Registered netlink family '%s'",
		 lan9645x_qos_genl_family.name);

	return 0;
}

void lan9645x_netlink_qos_uninit(void)
{
	nl_qos = NULL;
	genl_unregister_family(&lan9645x_qos_genl_family);
}
