// SPDX-License-Identifier: GPL-2.0+
/* Microchip AFI netlink API
 *
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Lets userspace drive the AFI fast-injection (DTI) engine: start a periodic
 * frame injection on a port at a given rate, stop it, and read back what is
 * configured. The generic logic lives in the AFI library; this layer only
 * marshals netlink attributes and resolves a net device to a registrant's AFI
 * controller and chip port.
 */

#include <linux/etherdevice.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>

#include "afi_api.h"
#include "afi_netlink.h"

/* Upper bound on the injected frame size (jumbo). */
#define AFI_NL_FRAME_MAX	10240

struct afi_nl {
	struct list_head list;
	struct afi_control *afi;
	void *priv;
	const struct afi_nl_ops *ops;
};

static LIST_HEAD(afi_nl_list);
static DEFINE_MUTEX(afi_nl_lock);
static struct genl_family afi_genl_family;

static bool afi_nl_has_registrants(void)
{
	return !list_empty(&afi_nl_list);
}

/* Resolve an ifindex to its AFI controller and chip port. On success returns
 * the net device with a held reference (caller must dev_put) and fills @afi
 * and @port_no.
 *
 * The registrant list is protected by afi_nl_lock. afi_netlink_register() and
 * afi_netlink_unregister() never hold afi_nl_lock across genl_(un)register_
 * family(), so taking it here under genl's cb_lock does not invert against
 * those paths. rtnl (taken in pre_doit) still serializes the command path's
 * DTI table accesses.
 */
static struct net_device *afi_nl_resolve(struct net *net, u32 ifindex,
					 struct afi_control **afi, u32 *port_no)
{
	struct net_device *ndev;
	struct afi_nl *anl;
	bool found = false;

	ASSERT_RTNL();

	ndev = dev_get_by_index(net, ifindex);
	if (!ndev)
		return NULL;

	mutex_lock(&afi_nl_lock);
	list_for_each_entry(anl, &afi_nl_list, list) {
		int port = anl->ops->port_of(anl->priv, ndev);

		if (port >= 0) {
			*afi = anl->afi;
			*port_no = port;
			found = true;
			break;
		}
	}
	mutex_unlock(&afi_nl_lock);

	if (!found) {
		dev_put(ndev);
		return NULL;
	}

	return ndev;
}

static int afi_genl_fast_start(struct sk_buff *skb, struct genl_info *info)
{
	struct afi_fast_inj_alloc_cfg acfg = {};
	struct afi_fast_inj_start_cfg scfg = {};
	u32 ifindex, port_no, fastid, frame_len;
	struct afi_fast_inj_frm_cfg fcfg = {};
	struct net_device *ndev;
	struct afi_control *afi;
	struct sk_buff *frm;
	struct sk_buff *msg;
	void *frame, *hdr;
	int err;

	if (!info->attrs[AFI_NL_ATTR_IFINDEX] ||
	    !info->attrs[AFI_NL_ATTR_FRAME])
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[AFI_NL_ATTR_IFINDEX]);
	frame = nla_data(info->attrs[AFI_NL_ATTR_FRAME]);
	frame_len = nla_len(info->attrs[AFI_NL_ATTR_FRAME]);

	if (frame_len < ETH_ZLEN)
		return -EINVAL;

	scfg.bps = info->attrs[AFI_NL_ATTR_BPS] ?
		nla_get_u64(info->attrs[AFI_NL_ATTR_BPS]) : 0;
	scfg.seq_cnt = info->attrs[AFI_NL_ATTR_SEQ_CNT] ?
		nla_get_u32(info->attrs[AFI_NL_ATTR_SEQ_CNT]) : 0;
	acfg.prio = info->attrs[AFI_NL_ATTR_PRIO] ?
		nla_get_u8(info->attrs[AFI_NL_ATTR_PRIO]) : 0;
	acfg.frm_cnt = 1;
	fcfg.frm_size = info->attrs[AFI_NL_ATTR_FRM_SIZE] ?
		nla_get_u32(info->attrs[AFI_NL_ATTR_FRM_SIZE]) : frame_len;

	ndev = afi_nl_resolve(genl_info_net(info), ifindex, &afi, &port_no);
	if (!ndev)
		return -ENODEV;

	if (!afi->ops->frm_inject || !afi->ops->dti_start) {
		err = -EOPNOTSUPP;
		goto out_dev;
	}

	acfg.port_no = port_no;

	/* rtnl is held by pre_doit. */
	err = afi_fast_inj_alloc(afi, &acfg, &fastid);
	if (err)
		goto out_dev;

	frm = netdev_alloc_skb(ndev, frame_len);
	if (!frm) {
		err = -ENOMEM;
		goto out_free;
	}
	skb_put_data(frm, frame, frame_len);

	/* Transmit the frame with the AFI marker so the engine can hijack it.
	 * On error frm_inject does not consume the skb.
	 */
	err = afi->ops->frm_inject(afi, port_no, acfg.prio, frm);
	if (err) {
		kfree_skb(frm);
		err = -EBUSY;
		goto out_free;
	}

	err = afi_fast_inj_frm_hijack(afi, fastid, &fcfg);
	if (err)
		goto out_free;

	err = afi_fast_inj_start(afi, fastid, &scfg);
	if (err)
		goto out_free;

	dev_put(ndev);

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		err = -ENOMEM;
		goto out_stop;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &afi_genl_family, 0, AFI_NL_CMD_FAST_START);
	if (!hdr) {
		nlmsg_free(msg);
		err = -EMSGSIZE;
		goto out_stop;
	}

	if (nla_put_u32(msg, AFI_NL_ATTR_FASTID, fastid) ||
	    nla_put_u64_64bit(msg, AFI_NL_ATTR_BPS_ACTUAL, scfg.bps_actual,
			      AFI_NL_ATTR_PAD) ||
	    nla_put_u32(msg, AFI_NL_ATTR_SEQ_ACTUAL, scfg.seq_actual)) {
		genlmsg_cancel(msg, hdr);
		nlmsg_free(msg);
		err = -EMSGSIZE;
		goto out_stop;
	}

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

out_stop:
	/* The flow is already started and ndev released; stop it so it can be
	 * freed (afi_fast_inj_free() rejects a started flow).
	 */
	afi_fast_inj_stop(afi, fastid);
	afi_fast_inj_free(afi, fastid);
	return err;
out_free:
	afi_fast_inj_free(afi, fastid);
out_dev:
	dev_put(ndev);
	return err;
}

static int afi_genl_fast_stop(struct sk_buff *skb, struct genl_info *info)
{
	u32 ifindex, port_no, fastid;
	struct net_device *ndev;
	struct afi_control *afi;
	int err;

	if (!info->attrs[AFI_NL_ATTR_IFINDEX] ||
	    !info->attrs[AFI_NL_ATTR_FASTID])
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[AFI_NL_ATTR_IFINDEX]);
	fastid = nla_get_u32(info->attrs[AFI_NL_ATTR_FASTID]);

	ndev = afi_nl_resolve(genl_info_net(info), ifindex, &afi, &port_no);
	if (!ndev)
		return -ENODEV;

	/* rtnl is held by pre_doit. */
	if (afi_fast_inj_started(afi, fastid))
		afi_fast_inj_stop(afi, fastid);
	err = afi_fast_inj_free(afi, fastid);

	dev_put(ndev);
	return err;
}

/* Fill one flow's status into a netlink message. Both the doit and the dump
 * path hold rtnl.
 */
static int afi_genl_fill_flow(struct sk_buff *msg, u32 portid, u32 seq,
			      int flags, struct afi_control *afi, u32 fastid)
{
	struct afi_dti *dti = &afi->dti_tbl[fastid];
	u32 seq_cnt = 0;
	void *hdr;

	hdr = genlmsg_put(msg, portid, seq, &afi_genl_family, flags,
			  AFI_NL_CMD_FAST_GET);
	if (!hdr)
		return -EMSGSIZE;

	if (afi->ops->dti_seq_cnt_get)
		afi->ops->dti_seq_cnt_get(afi, fastid, &seq_cnt);

	if (nla_put_u32(msg, AFI_NL_ATTR_FASTID, fastid) ||
	    nla_put_u32(msg, AFI_NL_ATTR_PORT, dti->port_no) ||
	    nla_put_u8(msg, AFI_NL_ATTR_PRIO, dti->prio) ||
	    nla_put_u8(msg, AFI_NL_ATTR_STATE, dti->state) ||
	    nla_put_u32(msg, AFI_NL_ATTR_SEQ_CNT, seq_cnt) ||
	    nla_put_u64_64bit(msg, AFI_NL_ATTR_BPS_ACTUAL,
			      dti->start_cfg.bps_actual, AFI_NL_ATTR_PAD)) {
		genlmsg_cancel(msg, hdr);
		return -EMSGSIZE;
	}

	genlmsg_end(msg, hdr);
	return 0;
}

static int afi_genl_fast_get(struct sk_buff *skb, struct genl_info *info)
{
	u32 ifindex, port_no, fastid;
	struct net_device *ndev;
	struct afi_control *afi;
	struct sk_buff *msg;
	int err;

	if (!info->attrs[AFI_NL_ATTR_IFINDEX] ||
	    !info->attrs[AFI_NL_ATTR_FASTID])
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[AFI_NL_ATTR_IFINDEX]);
	fastid = nla_get_u32(info->attrs[AFI_NL_ATTR_FASTID]);

	ndev = afi_nl_resolve(genl_info_net(info), ifindex, &afi, &port_no);
	if (!ndev)
		return -ENODEV;

	if (fastid >= afi->consts->dti_cnt ||
	    afi->dti_tbl[fastid].state == AFI_ENTRY_STATE_FREE) {
		dev_put(ndev);
		return -ENOENT;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		dev_put(ndev);
		return -ENOMEM;
	}

	/* rtnl is held by pre_doit. */
	err = afi_genl_fill_flow(msg, info->snd_portid, info->snd_seq, 0,
				 afi, fastid);

	dev_put(ndev);

	if (err) {
		nlmsg_free(msg);
		return err;
	}

	return genlmsg_reply(msg, info);
}

/* Dump all allocated flows across all registrants. cb->args[0] is the number
 * of flows already emitted (a flat index across the registrant list).
 *
 * rtnl serializes against the doit path's DTI table accesses; afi_nl_lock
 * guards the registrant list (see afi_nl_resolve()).
 */
static int afi_genl_fast_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	long target = cb->args[0];
	struct afi_nl *anl;
	long idx = 0;
	int err = 0;

	rtnl_lock();
	mutex_lock(&afi_nl_lock);
	list_for_each_entry(anl, &afi_nl_list, list) {
		struct afi_control *afi = anl->afi;
		u32 i;

		for (i = 0; i < afi->consts->dti_cnt; i++) {
			if (afi->dti_tbl[i].state == AFI_ENTRY_STATE_FREE)
				continue;

			if (idx++ < target)
				continue;

			err = afi_genl_fill_flow(skb,
						 NETLINK_CB(cb->skb).portid,
						 cb->nlh->nlmsg_seq,
						 NLM_F_MULTI, afi, i);
			if (err)
				goto out;

			cb->args[0] = idx;
		}
	}
out:
	mutex_unlock(&afi_nl_lock);
	rtnl_unlock();

	return skb->len;
}

static const struct nla_policy afi_genl_policy[AFI_NL_ATTR_MAX + 1] = {
	[AFI_NL_ATTR_IFINDEX]	= { .type = NLA_U32 },
	[AFI_NL_ATTR_FRAME]	= { .type = NLA_BINARY, .len = AFI_NL_FRAME_MAX },
	[AFI_NL_ATTR_BPS]	= { .type = NLA_U64 },
	[AFI_NL_ATTR_SEQ_CNT]	= { .type = NLA_U32 },
	[AFI_NL_ATTR_PRIO]	= { .type = NLA_U8 },
	[AFI_NL_ATTR_FRM_SIZE]	= { .type = NLA_U32 },
	[AFI_NL_ATTR_FASTID]	= { .type = NLA_U32 },
};

/* Serialize the command path (and thus the per-flow DTI table accesses) with
 * rtnl. The registrant list itself is protected by afi_nl_lock, which is never
 * held across genl_(un)register_family().
 */
static int afi_genl_pre_doit(const struct genl_split_ops *ops,
			     struct sk_buff *skb, struct genl_info *info)
{
	rtnl_lock();
	return 0;
}

static void afi_genl_post_doit(const struct genl_split_ops *ops,
			       struct sk_buff *skb, struct genl_info *info)
{
	rtnl_unlock();
}

static const struct genl_ops afi_genl_ops[] = {
	{
		.cmd	= AFI_NL_CMD_FAST_START,
		.doit	= afi_genl_fast_start,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= AFI_NL_CMD_FAST_STOP,
		.doit	= afi_genl_fast_stop,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= AFI_NL_CMD_FAST_GET,
		.doit	= afi_genl_fast_get,
		.dumpit	= afi_genl_fast_dump,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
	},
};

static struct genl_family afi_genl_family = {
	.name		= AFI_NETLINK_NAME,
	.hdrsize	= 0,
	.version	= AFI_NETLINK_VERSION,
	.maxattr	= AFI_NL_ATTR_MAX,
	.policy		= afi_genl_policy,
	.parallel_ops	= true,
	.pre_doit	= afi_genl_pre_doit,
	.post_doit	= afi_genl_post_doit,
	.ops		= afi_genl_ops,
	.n_ops		= ARRAY_SIZE(afi_genl_ops),
	.resv_start_op	= AFI_NL_CMD_FAST_GET + 1,
};

int afi_netlink_register(struct afi_control *afi, void *priv,
			 const struct afi_nl_ops *ops)
{
	struct afi_nl *anl;
	bool first;
	int err = 0;

	if (!afi || !ops || !ops->port_of)
		return -EINVAL;

	anl = kzalloc(sizeof(*anl), GFP_KERNEL);
	if (!anl)
		return -ENOMEM;

	anl->afi = afi;
	anl->priv = priv;
	anl->ops = ops;

	mutex_lock(&afi_nl_lock);
	first = !afi_nl_has_registrants();
	list_add(&anl->list, &afi_nl_list);
	mutex_unlock(&afi_nl_lock);

	/* Register the family outside afi_nl_lock: genl_register_family() takes
	 * genl's cb_lock, and the command path takes afi_nl_lock under cb_lock,
	 * so holding afi_nl_lock here would invert the two. No command can be
	 * dispatched until the family is registered, so the list entry being
	 * visible early is harmless.
	 */
	if (first) {
		err = genl_register_family(&afi_genl_family);
		if (err) {
			mutex_lock(&afi_nl_lock);
			list_del(&anl->list);
			mutex_unlock(&afi_nl_lock);
			kfree(anl);
			pr_err("AFI netlink family registration failed: %d\n",
			       err);
		}
	}

	return err;
}
EXPORT_SYMBOL_GPL(afi_netlink_register);

void afi_netlink_unregister(struct afi_control *afi)
{
	struct afi_nl *anl, *tmp;
	bool last;

	mutex_lock(&afi_nl_lock);
	list_for_each_entry_safe(anl, tmp, &afi_nl_list, list) {
		if (anl->afi != afi)
			continue;

		list_del(&anl->list);
		kfree(anl);
		break;
	}
	last = !afi_nl_has_registrants();
	mutex_unlock(&afi_nl_lock);

	/* Drop the family outside afi_nl_lock, for the same ordering reason as
	 * afi_netlink_register(). genl_unregister_family() drains in-flight
	 * commands.
	 */
	if (last)
		genl_unregister_family(&afi_genl_family);
}
EXPORT_SYMBOL_GPL(afi_netlink_unregister);
