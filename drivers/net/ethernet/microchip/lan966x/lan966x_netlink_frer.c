// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2020 Microchip Technology Inc. */

#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include "lan966x_main.h"

static struct lan966x *local_lan966x;
static struct genl_family lan966x_frer_genl_family;

static struct nla_policy lan966x_frer_genl_policy[MCHP_FRER_ATTR_END] = {
	[MCHP_FRER_ATTR_NONE] = { .type = NLA_UNSPEC },
	[MCHP_FRER_ATTR_ID] = { .type = NLA_U32 },
	[MCHP_FRER_ATTR_DEV1] = { .type = NLA_U32 },
	[MCHP_FRER_ATTR_DEV2] = { .type = NLA_U32 },
	[MCHP_FRER_ATTR_STREAM_CFG] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_frer_stream_cfg) },
	[MCHP_FRER_ATTR_STREAM_CNT] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_frer_cnt) },
	[MCHP_FRER_ATTR_IFLOW_CFG] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_frer_iflow_cfg) },
	[MCHP_FRER_ATTR_VLAN_CFG] = { .type = NLA_BINARY,
		.len = sizeof(struct mchp_frer_vlan_cfg) },
};

static int lan966x_frer_genl_cs_cfg_set(struct sk_buff *skb,
					struct genl_info *info)
{
	struct mchp_frer_stream_cfg cfg = {};
	u32 cs_id;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_STREAM_CFG]) {
		pr_err("ATTR_STREAM_CFG is missing\n");
		return -EINVAL;
	}

	cs_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);
	nla_memcpy(&cfg, info->attrs[MCHP_FRER_ATTR_STREAM_CFG],
		   nla_len(info->attrs[MCHP_FRER_ATTR_STREAM_CFG]));

	return lan966x_frer_cs_cfg_set(local_lan966x, cs_id, &cfg);
}

static int lan966x_frer_genl_cs_cfg_get(struct sk_buff *skb,
					struct genl_info *info)
{
	struct mchp_frer_stream_cfg cfg = {};
	struct sk_buff *msg;
	u32 cs_id;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	cs_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	err = lan966x_frer_cs_cfg_get(local_lan966x, cs_id, &cfg);
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
			  &lan966x_frer_genl_family, 0,
			  MCHP_FRER_GENL_CS_CFG_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_FRER_ATTR_STREAM_CFG, sizeof(cfg), &cfg)) {
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

static int lan966x_frer_genl_cs_cnt_get(struct sk_buff *skb,
					struct genl_info *info)
{
	struct mchp_frer_cnt cnt = {};
	struct sk_buff *msg;
	u32 cs_id;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	cs_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	err = lan966x_frer_cs_cnt_get(local_lan966x, cs_id, &cnt);
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
			  &lan966x_frer_genl_family, 0,
			  MCHP_FRER_GENL_CS_CNT_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_FRER_ATTR_STREAM_CNT, sizeof(cnt), &cnt)) {
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

static int lan966x_frer_genl_cs_cnt_clr(struct sk_buff *skb,
					struct genl_info *info)
{
	u32 cs_id;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	cs_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	return lan966x_frer_cs_cnt_clear(local_lan966x, cs_id);
}

static int lan966x_frer_genl_ms_alloc(struct sk_buff *skb,
				      struct genl_info *info)
{
	struct net *net = sock_net(skb->sk);
	struct lan966x_port *port;
	struct net_device *dev;
	struct sk_buff *msg;
	u8 port_mask = 0;
	u32 ifindex;
	u16 ms_id;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_FRER_ATTR_DEV1]) {
		pr_err("ATTR_DEV1 is missing\n");
		return -EINVAL;
	}

	ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV1]);
	dev = __dev_get_by_index(net, ifindex);
	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	port = netdev_priv(dev);
	port_mask |= BIT(port->chip_port);

	if (info->attrs[MCHP_FRER_ATTR_DEV2]) { /* Optional second device */

		ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV2]);
		if (ifindex) {
			dev = __dev_get_by_index(net, ifindex);
			if (!lan966x_netdevice_check(dev))
				return -EOPNOTSUPP;

			port = netdev_priv(dev);
			port_mask |= BIT(port->chip_port);
		}
	}

	err = lan966x_frer_ms_alloc(local_lan966x, port_mask, &ms_id);
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
			  &lan966x_frer_genl_family, 0,
			  MCHP_FRER_GENL_MS_ALLOC);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put_u32(msg, MCHP_FRER_ATTR_ID, ms_id)) {
		pr_err("Failed nla_put_u32\n");
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

static int lan966x_frer_genl_ms_free(struct sk_buff *skb,
				     struct genl_info *info)
{
	u32 ms_id;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	ms_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	return lan966x_frer_ms_free(local_lan966x, ms_id);
}

static int lan966x_frer_genl_ms_cfg_set(struct sk_buff *skb,
					struct genl_info *info)
{
	struct mchp_frer_stream_cfg cfg = {};
	struct net *net = sock_net(skb->sk);
	struct lan966x_port *port;
	struct net_device *dev;
	u32 ifindex;
	u32 ms_id;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_DEV1]) {
		pr_err("ATTR_DEV1 is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_STREAM_CFG]) {
		pr_err("ATTR_STREAM_CFG is missing\n");
		return -EINVAL;
	}

	ms_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV1]);
	dev = __dev_get_by_index(net, ifindex);
	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	port = netdev_priv(dev);

	nla_memcpy(&cfg, info->attrs[MCHP_FRER_ATTR_STREAM_CFG],
		   nla_len(info->attrs[MCHP_FRER_ATTR_STREAM_CFG]));

	return lan966x_frer_ms_cfg_set(port, ms_id, &cfg);
}

static int lan966x_frer_genl_ms_cfg_get(struct sk_buff *skb,
					struct genl_info *info)
{
	struct mchp_frer_stream_cfg cfg = {};
	struct net *net = sock_net(skb->sk);
	struct lan966x_port *port;
	struct net_device *dev;
	struct sk_buff *msg;
	u32 ifindex;
	u32 ms_id;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_DEV1]) {
		pr_err("ATTR_DEV1 is missing\n");
		return -EINVAL;
	}

	ms_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV1]);
	dev = __dev_get_by_index(net, ifindex);
	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	port = netdev_priv(dev);

	err = lan966x_frer_ms_cfg_get(port, ms_id, &cfg);
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
			  &lan966x_frer_genl_family, 0,
			  MCHP_FRER_GENL_MS_CFG_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_FRER_ATTR_STREAM_CFG, sizeof(cfg), &cfg)) {
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

static int lan966x_frer_genl_ms_cnt_get(struct sk_buff *skb,
					struct genl_info *info)
{
	struct net *net = sock_net(skb->sk);
	struct mchp_frer_cnt cnt = {};
	struct lan966x_port *port;
	struct net_device *dev;
	struct sk_buff *msg;
	u32 ifindex;
	u32 ms_id;
	void *hdr;
	int err;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_DEV1]) {
		pr_err("ATTR_DEV1 is missing\n");
		return -EINVAL;
	}

	ms_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV1]);
	dev = __dev_get_by_index(net, ifindex);
	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	port = netdev_priv(dev);

	err = lan966x_frer_ms_cnt_get(port, ms_id, &cnt);
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
			  &lan966x_frer_genl_family, 0,
			  MCHP_FRER_GENL_MS_CNT_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_FRER_ATTR_STREAM_CNT, sizeof(cnt), &cnt)) {
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
	return 0;
}

static int lan966x_frer_genl_ms_cnt_clr(struct sk_buff *skb,
					struct genl_info *info)
{
	struct net *net = sock_net(skb->sk);
	struct lan966x_port *port;
	struct net_device *dev;
	u32 ifindex;
	u32 ms_id;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_DEV1]) {
		pr_err("ATTR_DEV1 is missing\n");
		return -EINVAL;
	}

	ms_id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV1]);
	dev = __dev_get_by_index(net, ifindex);
	if (!lan966x_netdevice_check(dev))
		return -EOPNOTSUPP;

	port = netdev_priv(dev);

	return lan966x_frer_ms_cnt_clear(port, ms_id);
}

static int lan966x_frer_genl_iflow_cfg_set(struct sk_buff *skb,
					   struct genl_info *info)
{
	struct net *net = sock_net(skb->sk);
	struct mchp_iflow_cfg cfg = {};
	struct lan966x_port *port;
	struct net_device *dev;
	u8 split_mask = 0; /* Transferred via DEV1 and DEV2 */
	u32 ifindex;
	u32 id;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_IFLOW_CFG]) {
		pr_err("ATTR_IFLOW_CFG is missing\n");
		return -EINVAL;
	}

	id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);
	nla_memcpy(&cfg, info->attrs[MCHP_FRER_ATTR_IFLOW_CFG],
		   nla_len(info->attrs[MCHP_FRER_ATTR_IFLOW_CFG]));

	/* Get split_mask via DEV1 and DEV2 */
	if (info->attrs[MCHP_FRER_ATTR_DEV1]) {

		ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV1]);
		if (ifindex) {
			dev = __dev_get_by_index(net, ifindex);
			if (!lan966x_netdevice_check(dev))
				return -EOPNOTSUPP;

			port = netdev_priv(dev);
			split_mask |= BIT(port->chip_port);
		}
	}

	if (info->attrs[MCHP_FRER_ATTR_DEV2]) {

		ifindex = nla_get_u32(info->attrs[MCHP_FRER_ATTR_DEV2]);
		if (ifindex) {
			dev = __dev_get_by_index(net, ifindex);
			if (!lan966x_netdevice_check(dev))
				return -EOPNOTSUPP;

			port = netdev_priv(dev);
			split_mask |= BIT(port->chip_port);
		}
	}

	cfg.frer.split_mask = split_mask;

	return lan966x_iflow_cfg_set(local_lan966x, id, &cfg);
}

static int lan966x_frer_genl_iflow_cfg_get(struct sk_buff *skb,
					   struct genl_info *info)
{
	u32 ifindex[MCHP_FRER_MAX_PORTS] = {};
	struct mchp_iflow_cfg cfg = {};
	unsigned long split_mask;
	struct sk_buff *msg;
	u8 chip_port;
	int err, i;
	void *hdr;
	u32 id;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	id = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	err = lan966x_iflow_cfg_get(local_lan966x, id, &cfg);
	if (err) {
		goto invalid_info;
	}

	/* Extract split_mask into ifindex[] */
	split_mask = cfg.frer.split_mask;
	i = 0;
	for_each_set_bit(chip_port, &split_mask, 8) {
		ifindex[i] = local_lan966x->ports[chip_port]->dev->ifindex;
		if (++i >= MCHP_FRER_MAX_PORTS)
			break;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		pr_err("Allocate netlink msg failed\n");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &lan966x_frer_genl_family, 0,
			  MCHP_FRER_GENL_IFLOW_CFG_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_FRER_ATTR_IFLOW_CFG, sizeof(cfg), &cfg)) {
		pr_err("Failed nla_put\n");
		err = -EMSGSIZE;
		goto nla_put_failure;
	}

	/* Transfer split_mask (ifindex[]) via DEV1 and DEV2 */
	if (nla_put_u32(msg, MCHP_FRER_ATTR_DEV1, ifindex[0])) {
		pr_err("Failed nla_put_u32\n");
		err = -EMSGSIZE;
		goto nla_put_failure;
	}

	if (nla_put_u32(msg, MCHP_FRER_ATTR_DEV2, ifindex[1])) {
		pr_err("Failed nla_put_u32\n");
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

static int lan966x_frer_genl_vlan_cfg_set(struct sk_buff *skb,
					  struct genl_info *info)
{
	struct mchp_frer_vlan_cfg cfg = {};
	u32 vid;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	if (!info->attrs[MCHP_FRER_ATTR_VLAN_CFG]) {
		pr_err("ATTR_VLAN_CFG is missing\n");
		return -EINVAL;
	}

	vid = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);
	nla_memcpy(&cfg, info->attrs[MCHP_FRER_ATTR_VLAN_CFG],
		   nla_len(info->attrs[MCHP_FRER_ATTR_VLAN_CFG]));

	return lan966x_frer_vlan_cfg_set(local_lan966x, vid, &cfg);
}

static int lan966x_frer_genl_vlan_cfg_get(struct sk_buff *skb,
					  struct genl_info *info)
{
	struct mchp_frer_vlan_cfg cfg = {};
	struct sk_buff *msg;
	void *hdr;
	int err;
	u32 vid;

	if (!info->attrs[MCHP_FRER_ATTR_ID]) {
		pr_err("ATTR_ID is missing\n");
		return -EINVAL;
	}

	vid = nla_get_u32(info->attrs[MCHP_FRER_ATTR_ID]);

	err = lan966x_frer_vlan_cfg_get(local_lan966x, vid, &cfg);
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
			  &lan966x_frer_genl_family, 0,
			  MCHP_FRER_GENL_VLAN_CFG_GET);
	if (!hdr) {
		pr_err("Create msg hdr failed \n");
		err = -EMSGSIZE;
		goto err_msg_free;
	}

	if (nla_put(msg, MCHP_FRER_ATTR_VLAN_CFG, sizeof(cfg), &cfg)) {
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

static struct genl_ops lan966x_frer_genl_ops[] = {
	{
		.cmd    = MCHP_FRER_GENL_CS_CFG_SET,
		.doit   = lan966x_frer_genl_cs_cfg_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_CS_CFG_GET,
		.doit   = lan966x_frer_genl_cs_cfg_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_CS_CNT_GET,
		.doit   = lan966x_frer_genl_cs_cnt_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_CS_CNT_CLR,
		.doit   = lan966x_frer_genl_cs_cnt_clr,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_MS_ALLOC,
		.doit   = lan966x_frer_genl_ms_alloc,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_MS_FREE,
		.doit   = lan966x_frer_genl_ms_free,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_MS_CFG_SET,
		.doit   = lan966x_frer_genl_ms_cfg_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_MS_CFG_GET,
		.doit   = lan966x_frer_genl_ms_cfg_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_MS_CNT_GET,
		.doit   = lan966x_frer_genl_ms_cnt_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_MS_CNT_CLR,
		.doit   = lan966x_frer_genl_ms_cnt_clr,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_IFLOW_CFG_SET,
		.doit   = lan966x_frer_genl_iflow_cfg_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_IFLOW_CFG_GET,
		.doit   = lan966x_frer_genl_iflow_cfg_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_VLAN_CFG_SET,
		.doit   = lan966x_frer_genl_vlan_cfg_set,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
	{
		.cmd    = MCHP_FRER_GENL_VLAN_CFG_GET,
		.doit   = lan966x_frer_genl_vlan_cfg_get,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags  = GENL_ADMIN_PERM,
	},
};

static struct genl_family lan966x_frer_genl_family = {
	.name		= MCHP_FRER_NETLINK,
	.hdrsize	= 0,
	.version	= 1,
	.maxattr	= MCHP_FRER_ATTR_MAX,
	.policy		= lan966x_frer_genl_policy,
	.ops		= lan966x_frer_genl_ops,
	.n_ops		= ARRAY_SIZE(lan966x_frer_genl_ops),
	.resv_start_op	= MCHP_FRER_GENL_VLAN_CFG_GET + 1,
};

int lan966x_netlink_frer_init(struct lan966x *lan966x)
{
	int err;

	local_lan966x = lan966x;
	err = genl_register_family(&lan966x_frer_genl_family);
	if (err)
		pr_err("genl_register_family failed\n");

	return err;
}

void lan966x_netlink_frer_uninit(void)
{
	local_lan966x = NULL;
	genl_unregister_family(&lan966x_frer_genl_family);
}
