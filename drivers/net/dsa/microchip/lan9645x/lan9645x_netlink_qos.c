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

static void qos_cfg_to_nl(struct mchp_qos_port_conf *dst,
			  struct lan9645x_port_qos *src)
{
	dst->i_default_prio = src->i_default_prio;
	dst->i_default_dpl = src->i_default_dpl;
	dst->i_default_pcp = src->i_default_pcp;
	dst->i_default_dei = src->i_default_dei;
	dst->i_mode.dscp_map_enable = src->i_mode.dscp_map_enable;
	dst->i_mode.tag_map_enable = src->i_mode.tag_map_enable;

	dst->e_default_pcp = src->e_default_pcp;
	dst->e_default_dei = src->e_default_dei;

	switch (src->e_mode) {
	case E_MODE_PORT_PCP_DEI:
		dst->e_mode = MCHP_E_MODE_DEFAULT;
		break;
	case E_MODE_MAPPED:
		dst->e_mode = MCHP_E_MODE_MAPPED;
		break;
	default:
		dst->e_mode = MCHP_E_MODE_CLASSIFIED;
		break;
	}

	dst->pfc_enable = src->pfc_enable;

	for (int prio = 0; prio < 8; prio++) {
		for (int dpl = 0; dpl < 2; dpl++) {
			dst->e_prio_dpl_pcp_dei_map[prio][dpl].pcp =
				src->e_map[prio][dpl].pcp;
			dst->e_prio_dpl_pcp_dei_map[prio][dpl].dei =
				src->e_map[prio][dpl].dei;
		}
	}

	for (int pcp = 0; pcp < 8; pcp++) {
		for (int dei = 0; dei < 2; dei++) {
			dst->i_pcp_dei_prio_dpl_map[pcp][dei].prio =
				src->i_map[pcp][dei].prio;
			dst->i_pcp_dei_prio_dpl_map[pcp][dei].dpl =
				src->i_map[pcp][dei].dpl;
		}
	}
}

static void nl_to_qos_cfg(struct lan9645x_port_qos *dst,
			  struct mchp_qos_port_conf *src)
{
	dst->i_default_prio = src->i_default_prio;
	dst->i_default_dpl = src->i_default_dpl;
	dst->i_default_pcp = src->i_default_pcp;
	dst->i_default_dei = src->i_default_dei;
	dst->i_mode.dscp_map_enable = src->i_mode.dscp_map_enable;
	dst->i_mode.tag_map_enable = src->i_mode.tag_map_enable;

	dst->e_default_pcp = src->e_default_pcp;
	dst->e_default_dei = src->e_default_dei;

	switch (src->e_mode) {
	case MCHP_E_MODE_DEFAULT:
		dst->e_mode = E_MODE_PORT_PCP_DEI;
		break;
	case MCHP_E_MODE_MAPPED:
		dst->e_mode = E_MODE_MAPPED;
		break;
	default:
		dst->e_mode = E_MODE_CLASSIFIED;
		break;
	}

	dst->pfc_enable = src->pfc_enable;

	for (int prio = 0; prio < 8; prio++) {
		for (int dpl = 0; dpl < 2; dpl++) {
			dst->e_map[prio][dpl].pcp =
				src->e_prio_dpl_pcp_dei_map[prio][dpl].pcp;
			dst->e_map[prio][dpl].dei =
				src->e_prio_dpl_pcp_dei_map[prio][dpl].dei;
		}
	}

	for (int pcp = 0; pcp < 8; pcp++) {
		for (int dei = 0; dei < 2; dei++) {
			dst->i_map[pcp][dei].prio =
				src->i_pcp_dei_prio_dpl_map[pcp][dei].prio;
			dst->i_map[pcp][dei].dpl =
				src->i_pcp_dei_prio_dpl_map[pcp][dei].dpl;
		}
	}
}

static void nl2dscp_cfg(struct lan9645x_ig_dscp *dst,
			struct mchp_qos_dscp_prio_dpl *src)
{
	dst->prio = src->prio;
	dst->dpl = src->dpl;
	dst->trust = src->trust;
}

static void dscp_cfg2nl(struct mchp_qos_dscp_prio_dpl *dst,
			struct lan9645x_ig_dscp *src)
{
	dst->prio = src->prio;
	dst->dpl = src->dpl;
	dst->trust = src->trust;
}

static int lan9645x_qos_port_conf_set(struct lan9645x_netlink_qos *q,
				      struct net_device *dev,
				      struct mchp_qos_port_conf *cfg)
{
	struct lan9645x_port_qos icfg = {};
	struct lan9645x *lan9645x;
	struct lan9645x_port *p;

	ASSERT_RTNL();

	lan9645x = q->lan9645x;

	p = lan9645x_port_from_netdev(dev);
	if (IS_ERR_OR_NULL(p))
		return -ENOTSUPP;

	dev_dbg(lan9645x->dev, "port=%d", p->chip_port);

	nl_to_qos_cfg(&icfg, cfg);

	return lan9645x_qos_portconf_set(p, &icfg);
}

static int lan9645x_qos_port_conf_get(struct lan9645x_netlink_qos *q,
				      struct net_device *dev,
				      struct mchp_qos_port_conf *cfg)
{
	struct lan9645x_port_qos icfg = {};
	struct lan9645x_port *p;

	ASSERT_RTNL();

	p = lan9645x_port_from_netdev(dev);
	if (IS_ERR_OR_NULL(p))
		return -ENOTSUPP;

	dev_dbg(q->lan9645x->dev, "port=%d", p->chip_port);

	lan9645x_qos_portconf_get(p, &icfg);
	qos_cfg_to_nl(cfg, &icfg);

	return 0;
}

static int lan9645x_qos_dscp_prio_dpl_set(struct lan9645x_netlink_qos *q,
					  u8 dscp,
					  struct mchp_qos_dscp_prio_dpl *cfg)
{
	struct lan9645x *lan9645x = q->lan9645x;
	struct lan9645x_ig_dscp dcfg;
	int err;

	/* TODO: add e_dscp_map subcmd to qos-utils */
	ASSERT_RTNL();

	if (dscp >= LAN9645X_DSCP_COUNT)
		return -ERANGE;

	dev_dbg(q->lan9645x->dev, "dscp=%u dpl=%u prio=%u trust=%u", dscp,
		cfg->dpl, cfg->prio, cfg->trust);

	nl2dscp_cfg(&dcfg, cfg);

	mutex_lock(&lan9645x->qos_lock);
	err = __lan9645x_qos_dscp_conf_set(lan9645x, dscp, &dcfg);
	mutex_unlock(&lan9645x->qos_lock);

	return err;
}

static int lan9645x_qos_dscp_prio_dpl_get(struct lan9645x_netlink_qos *q,
					  u8 dscp,
					  struct mchp_qos_dscp_prio_dpl *cfg)
{
	struct lan9645x *lan9645x = q->lan9645x;
	struct lan9645x_ig_dscp dcfg;
	int err;

	ASSERT_RTNL();

	if (dscp >= LAN9645X_DSCP_COUNT)
		return -ERANGE;

	nl2dscp_cfg(&dcfg, cfg);

	mutex_lock(&lan9645x->qos_lock);
	err = __lan9645x_qos_dscp_conf_get(lan9645x, dscp, &dcfg);
	mutex_unlock(&lan9645x->qos_lock);
	dscp_cfg2nl(cfg, &dcfg);

	return err;
}

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
	.parallel_ops = true,
	.ops = lan9645x_qos_genl_ops,
	.n_ops = ARRAY_SIZE(lan9645x_qos_genl_ops),
	.resv_start_op = MCHP_QOS_GENL_DSCP_PRIO_DPL_GET + 1,
};

int lan9645x_netlink_qos_init(struct lan9645x *lan9645x)
{
	int err;

	nl_qos = devm_kzalloc(lan9645x->dev, sizeof(*nl_qos), GFP_KERNEL);
	if (!nl_qos)
		return -ENOMEM;

	nl_qos->lan9645x = lan9645x;

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
