// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/gfp_types.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/netlink.h>
#include <linux/refcount.h>
#include <linux/refcount_types.h>
#include <linux/types.h>
#include <net/tc_act/tc_gate.h>
#include <net/flow_offload.h>
#include <net/flow_dissector.h>
#include <linux/err.h>

#include "lan9645x_main.h"
#include "lan9645x_vcap_utils.h"
#include "lan9645x_stats.h"
#include "vcap_api_client.h"
#include "vcap_tc.h"

#define LAN9645X_MAX_RULE_SIZE 5 /* allows X1, X2 and X4 rules */
#define LAN9645X_MAX_TYPES 9 /* IS2 X2 has 9 different types */

#define ETH_P_FRER	0xF1C1          /* FRER Redundancy Tag (IEEE 802.1CB) */
#define ETH_P_ELMI	0x88EE          /* MEF 16 E-LMI */

struct lan9645x_polymorphic_rule {
	enum vcap_keyfield_set kset;
	u8 tid;
	u8 tid_mask;
	u8 level;
};

struct lan9645x_multi_rules {
	struct lan9645x_polymorphic_rule rule[LAN9645X_MAX_RULE_SIZE]
					     [LAN9645X_MAX_TYPES];
};

/* ISDXes are used by several features such as:
 *
 * - HSR/PRP (stream table and traffic classification using vcap)
 * - FRER (stream table)
 * - TC flower for linking rule processing across VCAP intances
 *
 * If multiple features try to use the same ISDX, the feature will break. We
 * need proper resource management of the ISDX indices, and tc flower can not
 * just use chain offsets as ISDXes to link rules.
 *
 * This struct is used to track the map between chain_id's and allocated isdx's.
 *
 * Note this is not necessary for ESDX's, as ES0 is the sole user of these.
 */
struct lan9645x_link_isdx {
	u32 chain_id;
	refcount_t refc;
	struct list_head list;
	u16 isdx;
};

struct lan9645x_act_state {
	struct lan9645x_mirror *m; /* FLOW_ACTINO_MIRRED */
	u32 redir_ports; /* FLOW_ACTOIN_REDIRECT */
	int pol_idx; /* FLOW_ACTION_POLICE */
	int goto_isdx; /* FLOW_ACTION_GOTO */
	int vlan_push; /* FLOW_ACTION_VLAN_PUSH */
	int target_isdx;
};

static enum vcap_bit __vcap2bit(u32 val)
{
	return !!val ? VCAP_BIT_1 : VCAP_BIT_0;
}

static struct lan9645x_link_isdx *
__lan9645x_tc_link_isdx_find(struct lan9645x *lan9645x, u16 isdx)
{
	struct lan9645x_link_isdx *pos;

	lockdep_assert_held(&lan9645x->link_isdx_lock);

	list_for_each_entry(pos, &lan9645x->link_isdx, list) {
		if (pos->isdx == isdx)
			return pos;
	}
	return NULL;
}

static struct lan9645x_link_isdx *
__lan9645x_tc_link_cid_find(struct lan9645x *lan9645x, u32 to_cid)
{
	struct lan9645x_link_isdx *pos;
	struct vcap_admin *admin;

	lockdep_assert_held(&lan9645x->link_isdx_lock);

	admin = vcap_find_admin(lan9645x->vcap_ctrl, to_cid);
	if (!admin || admin->vtype != VCAP_TYPE_ES0)
		return NULL;

	to_cid = to_cid % VCAP_CID_LOOKUP_SIZE;
	if (!to_cid) /* Chain is a lookup, no links required */
		return NULL;

	list_for_each_entry(pos, &lan9645x->link_isdx, list) {
		if (pos->chain_id == to_cid)
			return pos;
	}

	return NULL;
}

static int lan9645x_tc_link_cid_get(struct lan9645x *lan9645x, u32 to_cid)
{
	struct lan9645x_link_isdx *link;
	int res, clamped;

	mutex_lock(&lan9645x->link_isdx_lock);
	clamped = to_cid % VCAP_CID_LOOKUP_SIZE;

	link = __lan9645x_tc_link_cid_find(lan9645x, to_cid);
	if (link) {
		dev_dbg(lan9645x->dev, "existing to_cid=%u chain_id=%u isdx=%u",
			to_cid, link->chain_id, link->isdx);
		refcount_inc(&link->refc);
		res = link->isdx;
		goto unlock;
	}

	res = lan9645x_stream_isdx_alloc(lan9645x);
	if (res < 0)
		goto unlock;

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link) {
		lan9645x_stream_isdx_free(lan9645x, res);
		res = -ENOMEM;
		goto unlock;
	}

	link->chain_id = clamped;
	link->isdx = res;
	dev_dbg(lan9645x->dev, "alloc to_cid=%u chain_id=%u isdx=%u",
		to_cid, link->chain_id, link->isdx);

	INIT_LIST_HEAD(&link->list);
	refcount_set(&link->refc, 1);
	list_add_tail(&link->list, &lan9645x->link_isdx);

unlock:
	mutex_unlock(&lan9645x->link_isdx_lock);
	dev_dbg(lan9645x->dev, "Allocated isdx=%d cid=%u to_cid=%u\n", res,
		to_cid, to_cid % VCAP_CID_LOOKUP_SIZE);
	return res;
}

static void __lan9645x_tc_link_put(struct lan9645x *lan9645x,
				   struct lan9645x_link_isdx *link)
{
	lockdep_assert_held(&lan9645x->link_isdx_lock);

	if (!link)
		return;

	dev_dbg(lan9645x->dev, "PUT to_cid=%u isdx=%u", link->chain_id, link->isdx);

	if (!refcount_dec_and_test(&link->refc))
		return;

	dev_dbg(lan9645x->dev, "DELETED to_cid=%u isdx=%u", link->chain_id, link->isdx);

	lan9645x_stream_isdx_free(lan9645x, link->isdx);
	list_del(&link->list);
	kfree(link);
}

static void lan9645x_tc_link_cid_put(struct lan9645x *lan9645x, u32 to_cid)
{
	struct lan9645x_link_isdx *link;

	mutex_lock(&lan9645x->link_isdx_lock);
	link = __lan9645x_tc_link_cid_find(lan9645x, to_cid);
	__lan9645x_tc_link_put(lan9645x, link);
	mutex_unlock(&lan9645x->link_isdx_lock);
}

static void lan9645x_tc_link_isdx_put(struct lan9645x *lan9645x, u16 isdx)
{
	struct lan9645x_link_isdx *link;

	/* We must be able to put by isdx, when deleting a rule with the goto
	 * chain target_cid. At delete time the goto action data is not
	 * available.
	 */

	mutex_lock(&lan9645x->link_isdx_lock);
	link = __lan9645x_tc_link_isdx_find(lan9645x, isdx);
	__lan9645x_tc_link_put(lan9645x, link);
	mutex_unlock(&lan9645x->link_isdx_lock);
}

int lan9645x_tc_flower_stats(struct lan9645x_port *p,
			     struct flow_cls_offload *f)
{
	struct vcap_counter count = {};
	struct vcap_admin *admin;
	int err;

	admin = vcap_find_admin(p->lan9645x->vcap_ctrl,
				f->common.chain_index);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(f->common.extack, "Invalid chain");
		return -EINVAL;
	}

	dev_dbg(p->lan9645x->dev, "pkts=%llu lastused=%llu", f->stats.pkts,
		f->stats.lastused);

	err = vcap_get_rule_count_by_cookie(p->lan9645x->vcap_ctrl, &count,
					    f->cookie);
	if (err)
		return err;

	flow_stats_update(&f->stats, 0x0, count.value, 0, 0,
			  FLOW_ACTION_HW_STATS_IMMEDIATE);

	dev_dbg(p->lan9645x->dev, "pkts=%llu lastused=%llu", f->stats.pkts,
		f->stats.lastused);

	return err;
}

static int
lan9645x_tc_flower_handler_control_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct netlink_ext_ack *extack = st->fco->common.extack;
	struct flow_match_control match;
	int err = 0;

	flow_rule_match_control(st->frule, &match);
	if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L3_FRAGMENT,
					    __vcap2bit(match.key->flags &
						       FLOW_DIS_IS_FRAGMENT));
		if (err)
			goto bad_frag_out;
	}

	if (match.mask->flags & FLOW_DIS_FIRST_FRAG) {
		err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_L3_FRAG_OFS_GT0,
					    __vcap2bit(!(match.key->flags &
						       FLOW_DIS_FIRST_FRAG)));
		if (err)
			goto bad_frag_out;
	}

	if (!flow_rule_is_supp_control_flags(FLOW_DIS_IS_FRAGMENT |
						     FLOW_DIS_FIRST_FRAG,
					     match.mask->flags, extack))
		return -EOPNOTSUPP;

	st->used_keys |= BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL);

	return err;

bad_frag_out:
	NL_SET_ERR_MSG_MOD(extack, "ip_frag parse error");
	return err;
}

static bool lan9645x_tc_is_known_etype(struct vcap_tc_flower_parse_usage *st,
				       u16 etype)
{
	switch (st->admin->vtype) {
	case VCAP_TYPE_IS1:
		switch (etype) {
		case ETH_P_ALL:
		case ETH_P_ARP:
		case ETH_P_IP:
		case ETH_P_IPV6:
		case ETH_P_FRER:
		case ETH_P_SNAP:
			return true;
		}
		break;
	case VCAP_TYPE_IS2:
		switch (etype) {
		case ETH_P_ALL:
		case ETH_P_ARP:
		case ETH_P_IP:
		case ETH_P_IPV6:
		case ETH_P_FRER:
		case ETH_P_SNAP:
		case ETH_P_802_2:
		case ETH_P_SLOW:
		case ETH_P_CFM:
		case ETH_P_ELMI:
			return true;
		}
		break;
	case VCAP_TYPE_ES0:
		return true;
	default:
		NL_SET_ERR_MSG_MOD(st->fco->common.extack,
				   "VCAP type not supported");
		return false;
	}

	return false;
}

static int
lan9645x_tc_flower_handler_basic_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct flow_match_basic match;
	int err = 0;

	flow_rule_match_basic(st->frule, &match);
	if (match.mask->n_proto) {
		st->l3_proto = be16_to_cpu(match.key->n_proto);
		if (!lan9645x_tc_is_known_etype(st, st->l3_proto)) {
			err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_ETYPE,
						    st->l3_proto, ~0);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IP) {
			err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_IP4_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l3_proto == ETH_P_IPV6 &&
			   st->admin->vtype == VCAP_TYPE_IS1) {
			/* Don't set any keys in this case */
		} else if (st->l3_proto == ETH_P_ALL) {
			/* Nothing to do */
		} else if (st->l3_proto == ETH_P_SNAP &&
			   st->admin->vtype == VCAP_TYPE_IS1) {
			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_ETYPE_LEN_IS, VCAP_BIT_0);
			if (err)
				goto out;

			err = vcap_rule_add_key_bit(st->vrule,
						    VCAP_KF_IP_SNAP_IS, VCAP_BIT_1);
			if (err)
				goto out;
		} else if (false) {
		} else if (st->l3_proto == ETH_P_FRER) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				/* This key set to 1 in hw when frame has FRER
				 * tag, HSR tag or PRP rct
				 */
				vcap_rule_add_key_bit(st->vrule,
						      VCAP_KF_R_TAGGED_IS,
						      VCAP_BIT_1);
			}
		} else {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				err = vcap_rule_add_key_bit(st->vrule,
							    VCAP_KF_ETYPE_LEN_IS,
							    VCAP_BIT_1);
				if (err)
					goto out;

				err = vcap_rule_add_key_u32(st->vrule,
							    VCAP_KF_ETYPE,
							    st->l3_proto, ~0);
				if (err)
					goto out;
			}
		}
	}
	if (match.mask->ip_proto) {
		st->l4_proto = match.key->ip_proto;

		if (st->l4_proto == IPPROTO_TCP) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				err = vcap_rule_add_key_bit(st->vrule,
							    VCAP_KF_TCP_UDP_IS,
							    VCAP_BIT_1);
				if (err)
					goto out;
			}

			/* Note see l3_proto == ETH_P_IPV6 workaround in
			 * *_handler_ipv6_usage
			 */
			err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_TCP_IS,
						    VCAP_BIT_1);
			if (err)
				goto out;
		} else if (st->l4_proto == IPPROTO_UDP) {
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				err = vcap_rule_add_key_bit(st->vrule,
							    VCAP_KF_TCP_UDP_IS,
							    VCAP_BIT_1);
				if (err)
					goto out;
			}

			err = vcap_rule_add_key_bit(st->vrule, VCAP_KF_TCP_IS,
						    VCAP_BIT_0);
			if (err)
				goto out;
		} else {
			err = vcap_rule_add_key_u32(st->vrule,
						    VCAP_KF_L3_IP_PROTO,
						    st->l4_proto, ~0);
			if (err)
				goto out;
		}
	}

	st->used_keys |= BIT_ULL(FLOW_DISSECTOR_KEY_BASIC);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_proto parse error");
	return err;
}

static int lan9645x_tc_flower_action_check(struct vcap_control *vctrl,
					   struct flow_cls_offload *fco,
					   bool ingress)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(fco);
	struct flow_action_entry *actent, *last_actent = NULL;
	struct flow_action *act = &rule->action;
	u64 action_mask = 0;
	int idx;

	if (!flow_action_has_entries(act)) {
		NL_SET_ERR_MSG_MOD(fco->common.extack, "No actions");
		return -EINVAL;
	}

	if (!flow_action_basic_hw_stats_check(act, fco->common.extack))
		return -EOPNOTSUPP;

	flow_action_for_each(idx, actent, act) {
		if (action_mask & BIT(actent->id) && actent->id != FLOW_ACTION_VLAN_PUSH) {
			/* TODO: would be helpful to be able show action name violation here... */
			NL_SET_ERR_MSG_MOD(fco->common.extack,
					   "More actions of the same type");
			return -EINVAL;
		}
		action_mask |= BIT(actent->id);
		last_actent = actent; /* Save last action for later check */
	}

	/* Check that last action is a goto
	 * The last chain/lookup does not need to have goto action
	 */
	if (last_actent->id == FLOW_ACTION_GOTO) {
		/* Check if the destination chain is in one of the VCAPs */
		if (!vcap_is_next_lookup(vctrl, fco->common.chain_index,
					 last_actent->chain_index)) {
			NL_SET_ERR_MSG_MOD(fco->common.extack,
					   "Invalid goto chain");
			return -EINVAL;
		}
	} else if (!vcap_is_last_chain(vctrl, fco->common.chain_index,
				       ingress)) {
		NL_SET_ERR_MSG_MOD(fco->common.extack,
				   "Last action must be 'goto'");
		return -EINVAL;
	}

	/* Catch unsupported combinations of actions */
	if (action_mask & BIT(FLOW_ACTION_TRAP) &&
	    action_mask & BIT(FLOW_ACTION_ACCEPT)) {
		NL_SET_ERR_MSG_MOD(fco->common.extack,
				   "Cannot combine pass and trap action");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int lan9645x_tc_add_goto_link_target(struct lan9645x *lan9645x,
					    struct lan9645x_act_state *s,
					    struct vcap_admin *admin,
					    struct vcap_rule *vrule,
					    int target_cid,
					    struct netlink_ext_ack *extack)
{
	int link_val = target_cid % VCAP_CID_LOOKUP_SIZE;
	int isdx;

	if (!link_val)
		return 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		NL_SET_ERR_MSG_FMT(extack,
				   "VCAP IS1 only allows lookup chains. Off lookup chain %d is invalid",
				   target_cid);
		return -EINVAL;
	case VCAP_TYPE_IS2:
		/* Add IS2 specific PAG key (for chaining rules from IS1) */
		return vcap_rule_add_key_u32(vrule, VCAP_KF_LOOKUP_PAG,
					     link_val, ~0);
	case VCAP_TYPE_ES0:
		isdx = lan9645x_tc_link_cid_get(lan9645x, target_cid);
		if (isdx < 0)
			return isdx;

		s->target_isdx = isdx;

		return vcap_rule_add_key_u32(vrule, VCAP_KF_ISDX_CLS, isdx, ~0);
	default:
		break;
	}
	return 0;
}

static bool keyfields_offset_eq(struct vcap_rule *vrule,
				struct lan9645x_polymorphic_rule *cur,
				struct lan9645x_polymorphic_rule *nb,
				const struct vcap_info *vinfo)
{
	const struct vcap_field *cur_fields, *nb_fields;
	const struct vcap_client_keyfield *ckf;
	const struct vcap_field **kfs_map;
	const struct vcap_set *kfs_info;
	enum vcap_key_field kf;

	kfs_map = vinfo->keyfield_set_map;
	kfs_info = vinfo->keyfield_set;

	/* This function only makes sense for rules of same size in same vcap. */
	if (kfs_info[cur->kset].sw_per_item != kfs_info[cur->kset].sw_per_item)
		return false;

	cur_fields =  kfs_map[cur->kset];
	nb_fields = kfs_map[nb->kset];

	list_for_each_entry(ckf, &vrule->keyfields, ctrl.list) {
		kf = ckf->ctrl.key;

		if (cur_fields[kf].offset != nb_fields[kf].offset)
			return false;
	}

	return true;
}

static void lan9645x_tc_flower_merge_type_of_size(struct lan9645x *lan9645x,
						  struct vcap_admin *admin,
						  struct vcap_rule *vrule,
						  struct lan9645x_polymorphic_rule *rules,
						  int rsize, int num_types)
{
	struct lan9645x_polymorphic_rule *cur, *nb = NULL;
	const struct vcap_info *vinfo;
	int j, level, cur_idx, nb_idx;
	bool do_nxt_lvl;

	vinfo = &lan9645x->vcap_ctrl->vcaps[admin->vtype];

	/* Generally, rules of same size but different types can be merged if
	 *
	 * 1) They both contain all keyfields selected by user in vrule
	 * 2) All keyfields in 1) are at the same offset in the HW layout.
	 *
	 * However, since the type_id/type_id_mask keyfield in the VCAP is not
	 * a bitmask, with 1 bit for each type, but just sequential numbers,
	 * we have to be careful that our match-any masks only match exactly
	 * the types requested.
	 */

	for (level = 0; level < LAN9645X_MAX_TYPES; level++) {
		do_nxt_lvl = false;

		for (j = 0; j < num_types; j++) {
			cur_idx = (1 << level) * 2 * j;
			nb_idx = (1 << level) * (2 * j + 1);

			if (cur_idx >= num_types)
				break;

			cur = &rules[cur_idx];
			if (cur->kset == VCAP_KFS_NO_VALUE ||
			    cur->level != level)
				continue;

			if (nb_idx >= num_types && cur_idx == 0)
				return;

			if (nb_idx >= num_types) {
				/* It is safe to use mask-any for bits beyond the
				 * total number of types, so we do a 'merge' in
				 * this case also
				 */
				cur->level = level + 1;
				cur->tid_mask <<= 1;
				cur->tid = cur->tid &
						   cur->tid_mask;
				do_nxt_lvl = true;
				break;
			}

			nb = &rules[nb_idx];
			if (nb->kset == VCAP_KFS_NO_VALUE ||
			    nb->level != level)
				continue;

			if (keyfields_offset_eq(vrule, cur, nb, vinfo)) {
				cur->level = level + 1;
				cur->tid_mask <<= 1;
				cur->tid = cur->tid &
						   cur->tid_mask;
				do_nxt_lvl = true;
				nb->kset = VCAP_KFS_NO_VALUE;
			}
		}

		if (!do_nxt_lvl)
			break;
	}
}

static int lan9645x_vcap_num_rules_of_sz(struct vcap_control *vctrl,
					 struct vcap_admin *admin, int rsize)
{
	const struct vcap_info *vinfo;
	int num_types = 0;
	int rtype;

	vinfo = &vctrl->vcaps[admin->vtype];

	for (rtype = 0; rtype < vinfo->keyfield_set_size; rtype++) {
		if (vinfo->keyfield_set[rtype].sw_per_item == rsize)
			num_types++;
	}

	return num_types;
}

static int lan9645x_vcap_num_matched_of_sz(struct vcap_control *vctrl,
					   struct vcap_admin *admin, int rsize,
					   struct vcap_keyset_list *matches)
{
	const struct vcap_info *vinfo;
	enum vcap_keyfield_set kset;
	int i, num_types = 0;

	vinfo = &vctrl->vcaps[admin->vtype];

	for (i = 0; i < matches->cnt; i++) {
		kset = matches->keysets[i];
		if (vinfo->keyfield_set[kset].sw_per_item == rsize)
			num_types++;
	}
	return num_types;
}

static void lan9645x_tc_flower_merge_types(struct lan9645x *lan9645x,
					   struct vcap_admin *admin,
					   struct vcap_rule *vrule,
					   struct lan9645x_multi_rules *multi,
					   struct vcap_keyset_list *matches)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	int num_types_total, num_types_matched, rsize;
	const struct vcap_info *vinfo;
	/* multi now contains, for each keyset type size, all the keyset types
	 * which contain all the keyfields passed from tc dissectors.
	 *
	 * To cover all traffic, we must make a a vcap entry for each of these.
	 * However, since we can match the type_id in the entries with a mask,
	 * it is possible to create polymorphic rule entries.
	 *
	 * E.g. a single entry where the type_id field matches multiple types.
	 * This trick saves space in the VCAP, but there are two challenges:
	 *
	 * 1) The type_id fields are NOT a mask, but just a number. This makes it
	 *    tricky to use the MATCH_ANY bits in the key masks.
	 * 2) Even if the same field is present in two types, the bit offset may
	 *    not be the same. We can not merge two such rules, as we would end
	 *    up matching garbage.
	 *
	 * Calculating the optimal set of polymorphic rules, in the sense of
	 * minimal count, is difficult in general due to 1).
	 *
	 * Therefore, we use a heuristic which is non-optional, but works ok in
	 * practice.
	 *
	 * TODO: We can improve this further by distinguishing between whether a
	 *
	 * - rule type_id/keyset is active per port key selection
	 * - rule type_id/keyset is in intersection of keysets per port key
	 *   selection and keysets with keyfields chosen by the user
	 *
	 * At the moment we skip merging when it would be safe. E.g. in IS2
	 * rsize=2 we have types 6 and 7 (IP6_STD and OAM).
	 *
	 * Since IP6_STD is not active per port key selection, it always has
	 * multi->rule[2][6].kset == KFS_NO_VALUE
	 * which means we skip merging types 6 and 7.
	 *
	 * But it is safe to do so, since hardware never picks type 6 when it is
	 * disabled in the port key selection.
	 *
	 * We could check this by introducing additional state to
	 * struct lan9645x_polymorphic_rule
	 */

	vinfo = &lan9645x->vcap_ctrl->vcaps[admin->vtype];

	for (rsize = 1; rsize < LAN9645X_MAX_RULE_SIZE; ++rsize) {
		num_types_total =
			lan9645x_vcap_num_rules_of_sz(vctrl, admin, rsize);
		if (num_types_total <= 1)
			continue;

		num_types_matched = lan9645x_vcap_num_matched_of_sz(vctrl,
								    admin,
								    rsize,
								    matches);
		if (num_types_matched <= 1)
			continue;

		dev_dbg(lan9645x->dev,
			"rsize=%d types_total=%d types_matched=%d", rsize,
			num_types_total, num_types_matched);

		lan9645x_tc_flower_merge_type_of_size(lan9645x, admin, vrule,
						      multi->rule[rsize], rsize,
						      num_types_total);
	}
}

/* Collect all port keysets and apply the first of them, possibly wildcarded */
static int lan9645x_tc_select_protocol_keyset(struct lan9645x_port *p,
					      struct vcap_rule *vrule,
					      struct vcap_admin *admin,
					      struct vcap_tc_flower_parse_usage *st,
					      struct lan9645x_multi_rules *multi)
{
	struct vcap_keyset_list portkeysetlist = {};
	enum vcap_keyfield_set portkeysets[10] = {};
	struct vcap_keyset_list matches = {};
	enum vcap_keyfield_set keysets[10];
	int idx, jdx, err = 0, count = 0;
	const struct vcap_set *kinfo;
	struct vcap_control *vctrl;
	struct net_device *ndev;
	struct lan9645x *lan9645x;
	bool found, empty;
	u8 tidx, ks_sz;

	lan9645x = p->lan9645x;
	ndev = lan9645x_port_to_ndev(p);
	vctrl = lan9645x->vcap_ctrl;

	/* Given the keyfields in the vrule, find keyset types which contain all
	 * these keyfields. Only an enum match is made. If more than one type
	 * match, the position of the keyfields in the different types may
	 * differ. Such types can not be merged!
	 */
	matches.keysets = keysets;
	matches.max = ARRAY_SIZE(keysets);
	if (!vcap_rule_find_keysets(vrule, &matches)) {
		dev_dbg(lan9645x->dev, "Could not find any keysets matching rule keys! Did you add key for another platform?");
		return -EINVAL;
	}

	/* Find the keyset types for the VCAP instance determined by chain_id,
	 * which are possible  given the port keyselection configuration
	 * currently in use.
	 */
	portkeysetlist.max = ARRAY_SIZE(portkeysets);
	portkeysetlist.keysets = portkeysets;
	err = lan9645x_vcap_get_port_keyset(ndev, admin, vrule->vcap_chain_id,
					    st->l3_proto, &portkeysetlist);
	if (err)
		return err;

	empty = true;
	/* Find the intersection of the two sets of keyset */
	for (idx = 0; idx < portkeysetlist.cnt; ++idx) {
		kinfo = vcap_keyfieldset(vctrl, admin->vtype,
					 portkeysetlist.keysets[idx]);
		if (!kinfo)
			continue;

		/* Some entries have no type_id, because there is only 1 type of
		 * the given size
		 */
		tidx = kinfo->type_id == (u8)-1 ? 0 : kinfo->type_id;
		ks_sz = kinfo->sw_per_item;

		found = false;
		for (jdx = 0; jdx < matches.cnt; ++jdx) {
			if (portkeysetlist.keysets[idx] ==
			    matches.keysets[jdx]) {
				/* kinfo is in intersection */
				found = true;
				multi->rule[ks_sz][tidx].kset = matches.keysets[jdx];
				multi->rule[ks_sz][tidx].tid = kinfo->type_id;
				multi->rule[ks_sz][tidx].tid_mask = 0xff;
				++count;
				break;
			}
		}
		empty &= !found;

		if (!found && !st->l4_proto) {
			dev_dbg(lan9645x->dev,
				"port keyset not matched: %s. Covering too little traffic.\n",
				lan9645x_vcap_keyset_name_short(lan9645x,
								portkeysetlist.keysets[idx]));
		}
	}

	if (empty)
		return -ENOENT;

	lan9645x_tc_flower_merge_types(lan9645x, admin, vrule, multi, &matches);

	return 0;
}

static int
lan9645x_tc_flower_handler_ipv6_usage(struct vcap_tc_flower_parse_usage *st)
{
	int err = 0;

	if (st->l3_proto == ETH_P_IPV6) {
		struct flow_match_ipv6_addrs mt;
		struct vcap_u128_key sip;
		struct vcap_u128_key dip;

		flow_rule_match_ipv6_addrs(st->frule, &mt);
		/* Check if address masks are non-zero */
		if (!ipv6_addr_any(&mt.mask->src)) {
			vcap_netbytes_copy(sip.value, mt.key->src.s6_addr, 16);
			vcap_netbytes_copy(sip.mask, mt.mask->src.s6_addr, 16);
			err = vcap_rule_add_key_u128(st->vrule,
						     VCAP_KF_L3_IP6_SIP, &sip);
			if (err)
				goto out;

			/* IS1: With ipv6 addresses, we have to hit: NORMAL_IPV6
			 * or 5TUPLE_IPV6. These keysets do not support TCP_IS
			 * key, which might have been added earlier by the
			 * basic_usage() dissector. We remove it, and add the
			 * l4 proto in the L3_IP_PROTO key instead.
			 */
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				if (vcap_contains_key(st->vrule,
						      VCAP_KF_TCP_IS))
					vcap_rule_rem_key(st->vrule,
							  VCAP_KF_TCP_IS);

				err = vcap_rule_add_key_u32(st->vrule,
							    VCAP_KF_L3_IP_PROTO,
							    st->l4_proto, ~0);
				if (err)
					goto out;
			}
		}
		if (!ipv6_addr_any(&mt.mask->dst)) {
			vcap_netbytes_copy(dip.value, mt.key->dst.s6_addr, 16);
			vcap_netbytes_copy(dip.mask, mt.mask->dst.s6_addr, 16);
			err = vcap_rule_add_key_u128(st->vrule,
						     VCAP_KF_L3_IP6_DIP, &dip);
			if (err)
				goto out;

			/* IS1: With ipv6 addresses, we have to hit: NORMAL_IPV6
			 * or 5TUPLE_IPV6. These keysets do not support TCP_IS
			 * key, which might have been added earlier by the
			 * basic_usage() dissector. We remove it, and add the
			 * l4 proto in the L3_IP_PROTO key instead.
			 */
			if (st->admin->vtype == VCAP_TYPE_IS1) {
				if (vcap_contains_key(st->vrule, VCAP_KF_TCP_IS))
					vcap_rule_rem_key(st->vrule, VCAP_KF_TCP_IS);

				if (!vcap_contains_key(st->vrule, VCAP_KF_L3_IP_PROTO)) {
					err = vcap_rule_add_key_u32(st->vrule,
								    VCAP_KF_L3_IP_PROTO,
								    st->l4_proto,
								    ~0);
					if (err)
						goto out;
				}
			}
		}
	}
	st->used_keys |= BIT_ULL(FLOW_DISSECTOR_KEY_IPV6_ADDRS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ipv6_addr parse error");
	return err;
}

static int
lan9645x_tc_flower_handler_portnum_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct flow_match_ports match;
	enum vcap_key_field key;
	u16 value, mask;
	int err = 0;

	if (st->admin->vtype == VCAP_TYPE_IS1)
		key = VCAP_KF_ETYPE;
	else
		key = VCAP_KF_L4_DPORT;

	flow_rule_match_ports(st->frule, &match);
	if (match.mask->src) {
		value = be16_to_cpu(match.key->src);
		mask = be16_to_cpu(match.mask->src);
		err = vcap_rule_add_key_u32(st->vrule, VCAP_KF_L4_SPORT, value,
					    mask);
		if (err)
			goto out;
	}
	if (match.mask->dst) {
		value = be16_to_cpu(match.key->dst);
		mask = be16_to_cpu(match.mask->dst);
		err = vcap_rule_add_key_u32(st->vrule, key, value, mask);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_PORTS);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "port parse error");
	return err;
}

static int
lan9645x_tc_flower_handler_ip_usage(struct vcap_tc_flower_parse_usage *st)
{
	struct flow_match_ip match;
	enum vcap_key_field key;
	int err = 0;

	flow_rule_match_ip(st->frule, &match);

	if (st->admin->vtype == VCAP_TYPE_IS1)
		key = VCAP_KF_L3_DSCP;
	else
		key = VCAP_KF_L3_TOS;

	if (match.mask->tos) {
		err = vcap_rule_add_key_u32(st->vrule, key, match.key->tos,
					    match.mask->tos);
		if (err)
			goto out;
	}
	st->used_keys |= BIT(FLOW_DISSECTOR_KEY_IP);
	return err;
out:
	NL_SET_ERR_MSG_MOD(st->fco->common.extack, "ip_tos parse error");
	return err;
}

static int
lan9645x_tc_flower_handler_vlan_usage(struct vcap_tc_flower_parse_usage *st)
{
	enum vcap_key_field vid_key = VCAP_KF_8021Q_VID_CLS;
	enum vcap_key_field pcp_key = VCAP_KF_8021Q_PCP_CLS;

	if (st->admin->vtype == VCAP_TYPE_IS1) {
		vid_key = VCAP_KF_8021Q_VID0;
		pcp_key = VCAP_KF_8021Q_PCP0;
	}

	return vcap_tc_flower_handler_vlan_usage(st, vid_key, pcp_key);
}

static int
lan9645x_tc_flower_handler_cvlan_usage(struct vcap_tc_flower_parse_usage *st)
{
	if (st->admin->vtype != VCAP_TYPE_IS1) {
		NL_SET_ERR_MSG_MOD(st->fco->common.extack,
				   "cvlan not supported in this VCAP");
		return -EINVAL;
	}

	return vcap_tc_flower_handler_cvlan_usage(st);
}

static int
(*lan9645x_tc_flower_handlers_usage[])(struct vcap_tc_flower_parse_usage *st) = {
	[FLOW_DISSECTOR_KEY_ETH_ADDRS] = vcap_tc_flower_handler_ethaddr_usage,
	[FLOW_DISSECTOR_KEY_IPV4_ADDRS] = vcap_tc_flower_handler_ipv4_usage,
	[FLOW_DISSECTOR_KEY_IPV6_ADDRS] = lan9645x_tc_flower_handler_ipv6_usage,
	[FLOW_DISSECTOR_KEY_CONTROL] = lan9645x_tc_flower_handler_control_usage,
	[FLOW_DISSECTOR_KEY_PORTS] = lan9645x_tc_flower_handler_portnum_usage,
	[FLOW_DISSECTOR_KEY_BASIC] = lan9645x_tc_flower_handler_basic_usage,
	[FLOW_DISSECTOR_KEY_CVLAN] = lan9645x_tc_flower_handler_cvlan_usage,
	[FLOW_DISSECTOR_KEY_VLAN] = lan9645x_tc_flower_handler_vlan_usage,
	[FLOW_DISSECTOR_KEY_TCP] = vcap_tc_flower_handler_tcp_usage,
	[FLOW_DISSECTOR_KEY_ARP] = vcap_tc_flower_handler_arp_usage,
	[FLOW_DISSECTOR_KEY_IP] = lan9645x_tc_flower_handler_ip_usage,
};

static int
lan9645x_tc_flower_use_dissectors(struct vcap_tc_flower_parse_usage *st,
				  struct vcap_admin *admin,
				  struct vcap_rule *vrule)
{
	int idx, err = 0;

	for (idx = 0; idx < ARRAY_SIZE(lan9645x_tc_flower_handlers_usage);
	     ++idx) {
		if (!flow_rule_match_key(st->frule, idx))
			continue;
		if (!lan9645x_tc_flower_handlers_usage[idx])
			continue;
		err = lan9645x_tc_flower_handlers_usage[idx](st);
		if (err)
			return err;
	}

	if (st->frule->match.dissector->used_keys ^ st->used_keys) {
		NL_SET_ERR_MSG_MOD(st->fco->common.extack,
				   "Unsupported match item");
		return -ENOENT;
	}

	return err;
}

static int lan9645x_tc_add_rule_counter(struct vcap_admin *admin,
					struct vcap_rule *vrule)
{
	int err;

	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		err = vcap_rule_mod_action_u32(vrule, VCAP_AF_ESDX, vrule->id);
		if (!err)
			vcap_rule_set_counter_id(vrule, vrule->id);
		return err;
	default:
		return 0;
	}
}

static void lan9645x_tc_clear_rule_counter(struct lan9645x *lan9645x,
					   struct vcap_admin *admin, u32 rid)
{
	switch (admin->vtype) {
	case VCAP_TYPE_ES0:
		lan9645x_stats_clear_counters(lan9645x, LAN9645X_STAT_ESDX, rid);
		return;
	default:
		return;
	}
}

static int lan9645x_tc_add_rule_copy(struct lan9645x_port *p,
				     struct flow_cls_offload *fco,
				     struct vcap_rule *erule,
				     struct lan9645x_polymorphic_rule *prule)
{
	enum vcap_key_field keylist[] = {
		VCAP_KF_IF_IGR_PORT_MASK,
		VCAP_KF_IF_IGR_PORT_MASK_SEL,
		VCAP_KF_IF_IGR_PORT_MASK_RNG,
		VCAP_KF_LOOKUP_FIRST_IS,
		VCAP_KF_TYPE,
	};
	enum vcap_keyfield_set keyset;
	struct lan9645x *lan9645x;
	struct vcap_admin *admin;
	struct vcap_rule *vrule;
	const char *ksname;
	int err;

	lan9645x = p->lan9645x;
	admin = vcap_find_admin(lan9645x->vcap_ctrl, erule->vcap_chain_id);
	keyset = prule->kset;
	ksname = lan9645x_vcap_keyset_name_short(lan9645x, keyset);

	/* Add an extra rule with a special user and the new keyset */
	erule->user = VCAP_USER_TC_EXTRA;
	dev_dbg(lan9645x->dev,
		"extra filter: keyset: %s type_key=%#x mask=%#x rsize=%u",
		ksname, prule->tid, prule->tid_mask,
		vcap_keyfieldset(lan9645x->vcap_ctrl, admin->vtype, keyset)
			->sw_per_item);

	vrule = vcap_copy_rule(erule);
	if (IS_ERR(vrule))
		return PTR_ERR(vrule);

	/* Link the new rule to the existing rule with the cookie */
	vrule->cookie = erule->cookie;

	err = lan9645x_tc_add_rule_counter(admin, vrule);
	if (err) {
		dev_dbg(lan9645x->dev, "could not add counter: %u", vrule->id);
		goto out;
	}

	vcap_filter_rule_keys(vrule, keylist, ARRAY_SIZE(keylist), true);
	err = vcap_set_rule_set_keyset(vrule, keyset);
	if (err) {
		dev_err(lan9645x->dev, "could not set keyset %s in rule: %u",
			ksname, vrule->id);
		goto out;
	}

	if (prule->tid != (u8)-1) {
		/* Do not invert mask here, it already has correct format:
		 *  1 means fixed bit
		 *  0 means match-any
		 */
		err = vcap_rule_mod_key_u32(vrule, VCAP_KF_TYPE, prule->tid,
					    prule->tid_mask);
		if (err) {
			dev_err(lan9645x->dev,
				"could not wildcard rule type id in rule: %u",
				vrule->id);
			goto out;
		}
	}

	err = vcap_val_rule(vrule, ETH_P_ALL);
	if (err) {
		dev_err(lan9645x->dev, "could not validate rule: %u\n",
			vrule->id);
		vcap_set_tc_exterr(fco, vrule);
		goto out;
	}
	err = vcap_add_rule(vrule);
	if (err) {
		dev_err(lan9645x->dev, "could not add rule: %u\n", vrule->id);
		goto out;
	}
out:
	vcap_free_rule(vrule);
	return err;
}

static int lan9645x_tc_add_remaining_rules(struct lan9645x_port *p,
					   struct flow_cls_offload *fco,
					   struct vcap_rule *erule,
					   struct vcap_admin *admin,
					   struct lan9645x_multi_rules *multi)
{
	struct lan9645x_polymorphic_rule *poly_rule;
	int rsize, rtype, err = 0;

	for (rsize = 0; rsize < LAN9645X_MAX_RULE_SIZE; ++rsize) {
		for (rtype = 0; rtype < LAN9645X_MAX_TYPES; ++rtype) {
			poly_rule = &multi->rule[rsize][rtype];
			if (!poly_rule->kset)
				continue;

			err = lan9645x_tc_add_rule_copy(p, fco, erule,
							poly_rule);
			if (err)
				break;
		}
	}

	return err;
}

static int lan9645x_tc_add_goto_link(struct lan9645x *lan9645x,
				     struct lan9645x_act_state *s,
				     struct vcap_admin *admin,
				     struct vcap_rule *vrule,
				     struct flow_cls_offload *f, int to_cid)
{
	struct vcap_control *vctrl = lan9645x->vcap_ctrl;
	struct vcap_admin *to_admin = vcap_find_admin(vctrl, to_cid);
	int diff, isdx, err = 0;

	if (!to_admin) {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Unknown destination chain");
		return -EINVAL;
	}

	diff = vcap_chain_offset(vctrl, f->common.chain_index, to_cid);
	if (!diff)
		return 0;

	if (diff < 0)
		return -EINVAL;

	/* Between IS1 and IS2 the PAG value is used */
	if (admin->vtype == VCAP_TYPE_IS1 && to_admin->vtype == VCAP_TYPE_IS2) {
		/* This works for IS1->IS2 */
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_VAL, diff);
		if (err)
			return err;

		err = vcap_rule_add_action_u32(vrule, VCAP_AF_PAG_OVERRIDE_MASK,
					       0xff);
		if (err)
			return err;
	} else if (admin->vtype == VCAP_TYPE_IS1 &&
		   to_admin->vtype == VCAP_TYPE_ES0) {
		isdx = lan9645x_tc_link_cid_get(lan9645x, to_cid);
		if (isdx < 0)
			return -ENOSPC;

		s->goto_isdx = isdx;

		err = vcap_rule_add_action_u32(vrule, VCAP_AF_ISDX_ADD_VAL,
					       isdx);
		if (err)
			return err;

		err = vcap_rule_add_action_bit(vrule, VCAP_AF_ISDX_REPLACE_ENA,
					       VCAP_BIT_1);
		if (err)
			return err;

	} else {
		NL_SET_ERR_MSG_MOD(f->common.extack,
				   "Unsupported chain destination");
		return -EOPNOTSUPP;
	}

	return err;
}

/* Add the actionset that is the default for the VCAP type */
static int lan9645x_tc_set_actionset(struct vcap_admin *admin,
				     struct vcap_rule *vrule)
{
	enum vcap_actionfield_set aset;

	/* Do not overwrite any current actionset */
	if (vrule->actionset != VCAP_AFS_NO_VALUE)
		return 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		aset = VCAP_AFS_S1;
		break;
	case VCAP_TYPE_IS2:
		aset = VCAP_AFS_BASE_TYPE;
		break;
	case VCAP_TYPE_ES0:
		aset = VCAP_AFS_VID;
		break;
	default:
		return -EINVAL;
	}

	return vcap_set_rule_set_actionset(vrule, aset);
}

static int lan9645x_tc_free_rule_resources(struct lan9645x_port *p,
					   struct flow_cls_offload *f,
					   int rule_id)
{
	struct lan9645x *lan9645x = p->lan9645x;
	struct vcap_client_actionfield *afield;
	struct vcap_control *vctrl;
	struct vcap_rule *vrule;
	int ret = 0;

	vctrl = lan9645x->vcap_ctrl;

	vrule = vcap_get_rule(vctrl, rule_id);
	if (IS_ERR_OR_NULL(vrule))
		return -EINVAL;

	/* Did rule contain goto ES0 isdx offset */
	afield = vcap_find_actionfield(vrule, VCAP_AF_ISDX_ADD_VAL);
	if (afield && afield->ctrl.type == VCAP_FIELD_U32 &&
	    afield->data.u32.value) {
		dev_dbg(lan9645x->dev, "rule %u: remove ES0 isdx=%u link",
			vrule->id, afield->data.u32.value);
		lan9645x_tc_link_isdx_put(lan9645x, afield->data.u32.value);
	}

	/* Check for enabled mirroring in this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_MIRROR_ENA);
	if (afield && afield->ctrl.type == VCAP_FIELD_BIT &&
	    afield->data.u1.value) {
		dev_dbg(lan9645x->dev, "rule %u: remove mirroring", vrule->id);
		lan9645x_mirror_put(lan9645x);
	}

	/* Check for an enabled policer for this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_POLICE_IDX);
	if (afield && afield->ctrl.type == VCAP_FIELD_U32 &&
	    afield->data.u32.value) {
		dev_dbg(lan9645x->dev, "rule %u: remove vcap policer pol_idx=%u",
			vrule->id, afield->data.u32.value);
		lan9645x_police_del(lan9645x, afield->data.u32.value);
		lan9645x_qos_polix_free(lan9645x, afield->data.u32.value);
	}

	/* Check for an enabled stream filter in this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_SFID_VAL);
	if (afield && afield->ctrl.type == VCAP_FIELD_U32 &&
	    afield->data.u32.value) {
		dev_dbg(lan9645x->dev, "rule %u: remove stream filter=%u",
			vrule->id, afield->data.u32.value);
		lan9645x_sfi_put(lan9645x, afield->data.u32.value);
	}

	/* Check for an enabled stream gate in this rule */
	afield = vcap_find_actionfield(vrule, VCAP_AF_SGID_VAL);
	if (afield && afield->ctrl.type == VCAP_FIELD_U32 &&
	    afield->data.u32.value) {
		dev_dbg(lan9645x->dev, "rule %u: remove stream gate=%u",
			vrule->id, afield->data.u32.value);
		lan9645x_sgi_put(lan9645x, afield->data.u32.value);
	}

	vcap_free_rule(vrule);
	return ret;
}

int lan9645x_tc_flower_del(struct lan9645x_port *p, struct flow_cls_offload *f,
			   bool ingress)
{
	struct vcap_control *vctrl;
	int err = -ENOENT, rule_id;
	struct vcap_admin *admin;
	struct net_device *ndev;
	int count = 0;

	ndev = lan9645x_port_to_ndev(p);
	vctrl = p->lan9645x->vcap_ctrl;

	admin = vcap_find_admin(vctrl, f->common.chain_index);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(f->common.extack, "Invalid chain");
		return -EINVAL;
	}

	/* This is a noop unless this is an ES0 rule with an isdx link. */
	lan9645x_tc_link_cid_put(p->lan9645x, f->common.chain_index);

	while (true) {
		/* Filters for shared blocks use the same cookie, so we need to
		 * filter on cookie and net_device.
		 */
		rule_id =
			vcap_lookup_rule_by_cookie_ndev(vctrl, f->cookie, ndev);
		if (rule_id <= 0)
			break;
		if (!count++) {
			/* Auxiallary resources are shared between all vcap
			 * rules (per net_device) required by a flower filter.
			 * Therefore, we only delete auxiliary resources for the
			 * one vcap rule.
			 */

			/* TODO: It would be better if we could free rule
			 * auxiliary resources after we delete the rule from the
			 * VCAP. If rule to be deleted is being used, there is a
			 * potential problem for frames which hit the rule after
			 * auxiliary resources are freed, but before rule is
			 * removed from vcap. Fixing this would require some
			 * restructuring of the VCAP lib.
			 */
			err = lan9645x_tc_free_rule_resources(p, f, rule_id);
			if (err) {
				dev_err(p->lan9645x->dev,
					"could not get rule %d for free auxiliary resources",
					rule_id);
			}
		}

		lan9645x_tc_clear_rule_counter(p->lan9645x, admin, rule_id);

		err = vcap_del_rule(vctrl, ndev, rule_id);
		if (err) {
			NL_SET_ERR_MSG_MOD(f->common.extack,
					   "Cannot delete rule");
			break;
		}
	}

	return err;
}

static bool lan9645x_vcap_is1_supported_flow_action(enum flow_action_id fact)
{
	switch (fact) {
	case FLOW_ACTION_POLICE:
	case FLOW_ACTION_VLAN_MANGLE:
	case FLOW_ACTION_GATE:
	case FLOW_ACTION_PRIORITY:
	case FLOW_ACTION_ACCEPT:
	case FLOW_ACTION_GOTO:
		return true;
	default:
		return false;
	}
}

static bool lan9645x_vcap_is2_supported_flow_action(enum flow_action_id fact)
{
	switch (fact) {
	case FLOW_ACTION_TRAP:
	case FLOW_ACTION_DROP:
	case FLOW_ACTION_MIRRED:
	case FLOW_ACTION_REDIRECT:
	case FLOW_ACTION_POLICE:
	case FLOW_ACTION_ACCEPT:
	case FLOW_ACTION_GOTO:
		return true;
	default:
		return false;
	}
}

static bool lan9645x_vcap_es0_supported_flow_action(enum flow_action_id fact)
{
	switch (fact) {
	case FLOW_ACTION_VLAN_MANGLE:
	case FLOW_ACTION_VLAN_POP:
	case FLOW_ACTION_VLAN_PUSH:
	case FLOW_ACTION_ACCEPT:
	case FLOW_ACTION_GOTO:
		return true;
	default:
		return false;
	}
}

static bool lan9645x_vcap_supported_flow_action(enum vcap_type vcap,
						enum flow_action_id fact)
{
	switch (vcap) {
	case VCAP_TYPE_IS1:
		return lan9645x_vcap_is1_supported_flow_action(fact);
	case VCAP_TYPE_IS2:
		return lan9645x_vcap_is2_supported_flow_action(fact);
	case VCAP_TYPE_ES0:
		return lan9645x_vcap_es0_supported_flow_action(fact);
	default:
		return false;
	}
}

static int lan9645x_tc_set_default_actionset(struct vcap_admin *admin,
					     struct vcap_rule *vrule, int cid)
{
	int err = 0;

	switch (admin->vtype) {
	case VCAP_TYPE_IS1:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_S1);
		break;
	case VCAP_TYPE_IS2:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_BASE_TYPE);
		break;
	case VCAP_TYPE_ES0:
		err = vcap_set_rule_set_actionset(vrule, VCAP_AFS_VID);
		break;
	default:
		break;
	}
	return err;
}

enum lan9645x_acl_id {
	LAN9645X_ACL_ID_NO_VALUE = 0,
	LAN9645X_ACL_ID_TRAPPED = 1,
	LAN9645X_ACL_ID_UNUSED1 = 2,
	LAN9645X_ACL_ID_UNUSED2 = 3,
	LAN9645X_ACL_ID_UNUSED3 = 4,
	LAN9645X_ACL_ID_UNUSED4 = 5,
	LAN9645X_ACL_ID_UNUSED5 = 6,
	LAN9645X_ACL_ID_UNUSED6 = 7,
};

static u16 lan9645x_tc_acl_id_encode(int lookup, enum lan9645x_acl_id acl)
{
	/* ACL_ID is 6 bits. The values obtained from lookup 1 and lookup 2
	 * are added, so we allocate 3 bits per lookup.
	 *
	 * [0:2] Lookup 0
	 * [3:5] Lookup 1
	 *
	 * The ACL_ID is copied to the extraction header, which enables us
	 * to communicate from the VCAP (IS2) to the tag_driver.
	 */
	acl = acl & 0x7;
	switch (lookup) {
	case 0:
		return acl;
	case 1:
		return acl << 3;
	default:
		return 0;
	}
}

static int lan9645x_tc_flower_parse_act_es0(struct vcap_rule *vrule,
					    struct flow_action_entry *act)
{
	int err;

	switch (be16_to_cpu(act->vlan.proto)) {
	case ETH_P_8021Q:
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_TPID_SEL,
					       0); /* 0x8100 */
		break;
	case ETH_P_8021AD:
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_TPID_SEL,
					       1); /* 0x88a8 */
		break;
	default:
		return -EINVAL;
	}

	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_OUTER_TAG,
					1); /* Push ES0 tag A */
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_TAG_A_VID_SEL,
					VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_VID_A_VAL,
					act->vlan.vid);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_PCP_SEL, 1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PCP_A_VAL,
					act->vlan.prio);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_DEI_SEL, 0);

	return err;
}

static int lan9645x_tc_flower_parse_act_is1(struct vcap_rule *vrule,
					    struct flow_action_entry *act)
{
	int err;

	if (be16_to_cpu(act->vlan.proto) != ETH_P_8021Q)
		return -EINVAL;

	err = vcap_rule_add_action_bit(vrule, VCAP_AF_VID_REPLACE_ENA,
				       VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_VID_VAL, act->vlan.vid);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_PCP_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PCP_VAL, act->vlan.prio);

	/* TODO: if we use PCP/DEI -> DP/QOS mapping, we need to change QOS,
	 * otherwise rewriter lacks information about the overwritten PCP value.
	 */
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, act->vlan.prio);

	return err;
}

static int lan9645x_tc_parse_trap(struct vcap_admin *admin,
				  struct vcap_rule *vrule, int chain_index)
{
	int err, acl_id, lookup;

	lookup = vcap_chain_id_to_lookup(admin, chain_index);
	acl_id = lan9645x_tc_acl_id_encode(lookup, LAN9645X_ACL_ID_TRAPPED);
	err = vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_COPY_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, PERMIT_MASK);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_ACL_ID, acl_id);
	return err ? -EINVAL : 0;
}

static int lan9645x_tc_parse_drop(struct vcap_rule *vrule)
{
	int err;

	err = vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE, PERMIT_MASK);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_DIS_MODE,
					VCAP_BIT_1);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_CPU_DIS, VCAP_BIT_1);

	return err ? -EINVAL : 0;
}

static int lan9645x_tc_handle_vlan_push(struct lan9645x_act_state *s,
					struct vcap_admin *admin,
					struct vcap_rule *vrule,
					struct lan9645x_port *p,
					struct flow_action_entry *act,
					struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = p->lan9645x;
	int tpid_sel, err = 0;

	if (admin->vtype != VCAP_TYPE_ES0) {
		NL_SET_ERR_MSG_MOD(extack, "Cannot use vlan pop on non es0");
		return -EOPNOTSUPP;
	}

	switch (be16_to_cpu(act->vlan.proto)) {
	case ETH_P_8021Q:
		tpid_sel = 0; /* 0x8100 */
		break;
	case ETH_P_8021AD:
		tpid_sel = 1; /* 0x88a8 */
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack, "Invalid vlan proto");
		return -EINVAL;
	}

	s->vlan_push++;

	/* In software TC will push tags in the order listed by
	 * the actions. Outer tag first. I.e:
	 *
	 * tc ...
	 * action vlan push id X protocol 802.1ad
	 * action vlan push id Y protocol 802.1q
	 *
	 * ....88a8xxxx8100yyyyETYPE
	 */
	switch (s->vlan_push) {
	case 1:
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_OUTER_TAG, 1);
		err |= vcap_rule_add_action_bit(vrule, VCAP_AF_TAG_A_VID_SEL, VCAP_BIT_1);
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_VID_A_VAL, act->vlan.vid);
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_PCP_SEL, 1);
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PCP_A_VAL, act->vlan.prio);
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_DEI_SEL, 0);
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_A_TPID_SEL, tpid_sel);

		/* IF VLAN-AWARE and 1 vlan push action */
		dev_dbg(lan9645x->dev, "vlan_aware=%d port=%u", p->vlan_aware,
			p->chip_port);
		if (p->vlan_aware) {
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_INNER_TAG, 1);
			err |= vcap_rule_add_action_bit(vrule, VCAP_AF_TAG_B_VID_SEL, VCAP_BIT_0);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_B_PCP_SEL, 0);
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_B_DEI_SEL, 0);
			/* TPID based on IFH. If
			 *
			 * IFH.tag_type = 0 =>0x8100
			 * else => REW.PORT[i].PORT_VLAN_CFG.PORT_TPID
			 *
			 * which is 0x88a8 by default.
			 */
			err |= vcap_rule_add_action_u32(vrule, VCAP_AF_TAG_B_TPID_SEL, 3);
		}
		break;
	case 2:
		/* err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PUSH_INNER_TAG, 1); */
		err |= vcap_rule_mod_action_bit(vrule, VCAP_AF_TAG_B_VID_SEL,
						VCAP_BIT_1);
		err |= vcap_rule_mod_action_u32(vrule, VCAP_AF_TAG_B_PCP_SEL,
						1);
		err |= vcap_rule_mod_action_u32(vrule, VCAP_AF_TAG_B_DEI_SEL,
						0);
		err |= vcap_rule_mod_action_u32(vrule, VCAP_AF_TAG_B_TPID_SEL,
						tpid_sel);

		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_VID_B_VAL,
						act->vlan.vid);
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_PCP_B_VAL,
						act->vlan.prio);
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack, "Can not push more than two VLANS");
		return -EINVAL;
	}
	return err ? -EINVAL : err;
}

static int lan9645x_tc_handle_redirect(struct lan9645x_act_state *s,
				       struct flow_action_entry *act,
				       struct netlink_ext_ack *ext)
{
	struct lan9645x_port *redirp = NULL;

	redirp = lan9645x_port_from_netdev(act->dev);
	if (!redirp) {
		NL_SET_ERR_MSG_MOD(ext, "Redirect device is not a switch port");
		return -EINVAL;
	}

	s->redir_ports |= BIT(redirp->chip_port);
	return 0;
}

static int lan9645x_tc_handle_priority(struct vcap_rule *vrule,
				       struct flow_action_entry *act,
				       struct netlink_ext_ack *extack)
{
	int err = 0;

	if (act->priority > 7) {
		NL_SET_ERR_MSG_MOD(extack, "Invalid skbedit priority");
		return -EINVAL;
	}

	err = vcap_rule_add_action_bit(vrule, VCAP_AF_QOS_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_QOS_VAL, act->priority);
	return err ? -EINVAL : 0;
}

static int lan9645x_tc_handle_vlan_mangle(struct vcap_admin *admin,
					  struct vcap_rule *vrule,
					  struct flow_action_entry *act)
{
	if (admin->vtype == VCAP_TYPE_ES0)
		return lan9645x_tc_flower_parse_act_es0(vrule, act);
	else if (admin->vtype == VCAP_TYPE_IS1)
		return lan9645x_tc_flower_parse_act_is1(vrule, act);
	else
		return -EINVAL;
}

static int lan9645x_tc_handle_police(struct lan9645x_act_state *s,
				     struct vcap_admin *admin,
				     struct vcap_rule *vrule,
				     struct lan9645x_port *p,
				     struct flow_action_entry *act,
				     struct netlink_ext_ack *extack, int cid)
{
	struct lan9645x *lan9645x = p->lan9645x;
	struct lan9645x_policer pol = {};
	int err = 0;

	if (admin->vtype == VCAP_TYPE_IS2 &&
	    vcap_chain_id_to_lookup(admin, cid) != 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Police action is only supported in first lookup of IS2");
		return -EOPNOTSUPP;
	}

	if (!act->police.rate_bytes_ps && !act->police.burst) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Police action only support byte based rate policing.");
		return -EOPNOTSUPP;
	}

	s->pol_idx = lan9645x_qos_polix_alloc(lan9645x);
	if (s->pol_idx < 0) {
		NL_SET_ERR_MSG_MOD(extack, "Out of policer resources");
		return -ENOSPC;
	}

	dev_dbg(lan9645x->dev,
		"policer idx alloc id=%d port=%u rate_bts_ps=%llu burst=%u rate_pkt_ps=%llu burst_pkt=%llu exceed.act_id=%d exceed.extval=%u notexceed.act_id=%d notexceed.extval=%u ",
		s->pol_idx, p->chip_port, act->police.rate_bytes_ps,
		act->police.burst, act->police.rate_pkt_ps,
		act->police.burst_pkt, act->police.exceed.act_id,
		act->police.exceed.extval, act->police.notexceed.act_id,
		act->police.notexceed.extval);

	err = vcap_rule_add_action_bit(vrule, VCAP_AF_POLICE_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_POLICE_IDX, s->pol_idx);
	if (err)
		return -EINVAL;

	pol.rate = div_u64(act->police.rate_bytes_ps, 1000) * 8;
	pol.burst = act->police.burst;
	if (lan9645x_police_add(p, &pol, s->pol_idx)) {
		NL_SET_ERR_MSG_MOD(extack, "Cannot set policer");
		return -ENOSPC;
	}

	return 0;
}

static int lan9645x_tc_handle_mirred(struct lan9645x_act_state *s,
				     struct vcap_rule *vrule,
				     struct lan9645x_port *p,
				     struct flow_action_entry *act,
				     struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = p->lan9645x;
	struct lan9645x_port *mirror_to;

	mirror_to = lan9645x_port_from_netdev(act->dev);
	if (!mirror_to) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Mirror action not supported on non-switch port");
		return -EOPNOTSUPP;
	}

	if (p == mirror_to) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot mirror the mirror monitor port");
		return -EINVAL;
	}

	if (!s->m) {
		s->m = lan9645x_mirror_get(lan9645x, mirror_to->chip_port,
					   extack);
		if (IS_ERR(s->m))
			return PTR_ERR(s->m);
	}

	return vcap_rule_add_action_bit(vrule, VCAP_AF_MIRROR_ENA, VCAP_BIT_1);
}

static int lan9645x_tc_handle_gate(struct lan9645x_act_state *s,
				   struct vcap_admin *admin,
				   struct vcap_rule *vrule,
				   struct lan9645x_port *p,
				   struct flow_action_entry *act,
				   struct netlink_ext_ack *extack)
{
	struct lan9645x_psfp_sg_cfg sg = {0};
	struct lan9645x_psfp_sf_cfg sf = {0};
	u32 sfi_ix, sgi_ix;
	int err;

	if (admin->vtype != VCAP_TYPE_IS1) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot use gate on non is1");
		return -EOPNOTSUPP;
	}

	if (act->gate.prio < -1 || act->gate.prio > LAN9645X_PSFP_SG_MAX_IPV) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Invalid initial priority");
		return -EINVAL;
	}

	if (act->gate.cycletime < LAN9645X_PSFP_SG_MIN_CYCLE_TIME_NS ||
	    act->gate.cycletime > LAN9645X_PSFP_SG_MAX_CYCLE_TIME_NS) {
		NL_SET_ERR_MSG_MOD(extack, "Invalid cycle time");
		return -EINVAL;
	}

	if (act->gate.cycletimeext > LAN9645X_PSFP_SG_MAX_CYCLE_TIME_NS) {
		NL_SET_ERR_MSG_MOD(extack, "Invalid cycle time ext");
		return -EINVAL;
	}

	if (act->gate.num_entries == 0 ||
	    act->gate.num_entries >= LAN9645X_PSFP_NUM_GCE) {
		NL_SET_ERR_MSG_MOD(extack, "Invalid number of entries");
		return -EINVAL;
	}

	sg.gate_state = true;
	sg.ipv = act->gate.prio;
	sg.basetime = act->gate.basetime;
	sg.cycletime = act->gate.cycletime;
	sg.cycletimeext = act->gate.cycletimeext;
	sg.num_entries = act->gate.num_entries;

	for (int i = 0; i < act->gate.num_entries; i++) {
		if (act->gate.entries[i].interval <
			    LAN9645X_PSFP_SG_MIN_CYCLE_TIME_NS ||
		    act->gate.entries[i].interval >
			    LAN9645X_PSFP_SG_MAX_CYCLE_TIME_NS) {
			NL_SET_ERR_MSG_MOD(extack, "Invalid interval");
			return -EINVAL;
		}
		if (act->gate.entries[i].ipv < -1 ||
		    act->gate.entries[i].ipv > LAN9645X_PSFP_SG_MAX_IPV) {
			NL_SET_ERR_MSG_MOD(extack, "Invalid internal priority");
			return -EINVAL;
		}
		if (act->gate.entries[i].maxoctets < -1) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Invalid max octets");
			return -EINVAL;
		}

		sg.gce[i].gate_state = (act->gate.entries[i].gate_state != 0);
		sg.gce[i].interval = act->gate.entries[i].interval;
		sg.gce[i].ipv = act->gate.entries[i].ipv;
		sg.gce[i].maxoctets = act->gate.entries[i].maxoctets;
	}

	err = lan9645x_sfi_get(p->lan9645x, &sfi_ix);
	if (err < 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot reserve stream filter");
		return err;
	}

	err = lan9645x_sgi_get(p->lan9645x, &sgi_ix);
	if (err < 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot reserve stream gate");
		return err;
	}

	err = lan9645x_psfp_sg_set(p->lan9645x, sgi_ix, &sg);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Cannot set stream gate");
		return err;
	}

	err = lan9645x_psfp_sf_set(p->lan9645x, sfi_ix, &sf);
	if (err < 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot set stream filter");
		return err;
	}

	err = vcap_rule_add_action_bit(vrule, VCAP_AF_SGID_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_SGID_VAL, sgi_ix);
	err |= vcap_rule_add_action_bit(vrule, VCAP_AF_SFID_ENA, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(vrule, VCAP_AF_SFID_VAL, sfi_ix);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot set sgid and sfid");

		return err;
	}

	return 0;
}

static int lan9645x_tc_parse_actions(struct lan9645x_act_state *s,
				     struct lan9645x_port *p,
				     struct vcap_admin *admin,
				     struct vcap_rule *vrule,
				     struct flow_cls_offload *f)
{
	struct lan9645x *lan9645x = p->lan9645x;
	struct netlink_ext_ack *extack;
	struct flow_action_entry *act;
	struct flow_rule *frule;
	int idx, err, fcid;

	extack = f->common.extack;
	frule = flow_cls_offload_flow_rule(f);
	fcid = f->common.chain_index;

	flow_action_for_each(idx, act, &frule->action) {
		if (!lan9645x_vcap_supported_flow_action(admin->vtype,
							 act->id)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Unsupported TC action for this VCAP");
			return -EOPNOTSUPP;
		}

		switch (act->id) {
		case FLOW_ACTION_TRAP:
			err = lan9645x_tc_parse_trap(admin, vrule, fcid);
			if (err)
				return err;
			break;
		case FLOW_ACTION_DROP:
			err = lan9645x_tc_parse_drop(vrule);
			if (err)
				return err;
		break;
		case FLOW_ACTION_VLAN_PUSH:
			err = lan9645x_tc_handle_vlan_push(s, admin, vrule, p,
							   act, extack);
			if (err)
				return err;
			break;
		case FLOW_ACTION_ACCEPT:
			lan9645x_tc_set_default_actionset(admin, vrule, fcid);
			/* TODO: strictly speaking, this handling is appropriate
			 * for  FLOW_ACTION_CONTINUE, not ACCEPT, assuming the
			 * next action is a goto.
			 *
			 * pass/accept is supposed to end processing entirely.
			 * If we are in is1, we can try to link to a vcap "blackhole"
			 * PAG/ISDX.
			 *
			 * From IS2 we can not stop ES0 processing, nor processing
			 * of later lookups?
			 */
			break;
		case FLOW_ACTION_GOTO:
			err = lan9645x_tc_set_actionset(admin, vrule);
			if (err)
				return err;

			err = lan9645x_tc_add_goto_link(lan9645x, s, admin,
							vrule, f,
							act->chain_index);
			if (err)
				return err;
			break;
		case FLOW_ACTION_REDIRECT:
			err = lan9645x_tc_handle_redirect(s, act, extack);
			if (err)
				return err;
			break;
		case FLOW_ACTION_PRIORITY:
			err = lan9645x_tc_handle_priority(vrule, act, extack);
			if (err) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Cannot set skkedit priority");
				return err;
			}
			break;
		case FLOW_ACTION_VLAN_MANGLE:
			err = lan9645x_tc_handle_vlan_mangle(admin, vrule, act);
			if (err) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Cannot set vlan mangle");
				return err;
			}
			break;
		case FLOW_ACTION_VLAN_POP:
			/* Force untag */
			err = vcap_rule_add_action_u32(vrule,
						       VCAP_AF_PUSH_OUTER_TAG,
						       3);
			if (err)
				return err;
			break;
		case FLOW_ACTION_POLICE:
			err = lan9645x_tc_handle_police(s, admin, vrule, p, act,
							extack, fcid);
			if (err)
				return err;
			break;
		case FLOW_ACTION_MIRRED:
			err = lan9645x_tc_handle_mirred(s, vrule, p, act,
							extack);
			if (err)
				return err;
			break;
		case FLOW_ACTION_GATE:
			err = lan9645x_tc_handle_gate(s, admin, vrule, p, act,
						      extack);
			if (err)
				return err;
			break;
		default:
			NL_SET_ERR_MSG_MOD(extack, "Unsupported TC action");
			return -EOPNOTSUPP;
		}
	}

	/* Apply parsed *_REDIRECT ports */
	if (s->redir_ports) {
		err = vcap_rule_add_action_u32(vrule, VCAP_AF_PORT_MASK,
					       s->redir_ports);
		err |= vcap_rule_add_action_u32(vrule, VCAP_AF_MASK_MODE,
						REDIRECT);
		if (err)
			return -EINVAL;
	}

	return 0;
}

static void lan9645x_tc_action_state_cleanup(struct lan9645x *lan9645x,
					     struct lan9645x_act_state *s)
{
	if (!s)
		return;

	dev_dbg(lan9645x->dev,
		"goto_isdx=%d target_isdx=%d mirror=%p pol_idx=%d",
		s->goto_isdx, s->target_isdx, s->m, s->pol_idx);

	if (s->goto_isdx)
		lan9645x_tc_link_isdx_put(lan9645x, s->goto_isdx);

	if (s->target_isdx)
		lan9645x_tc_link_isdx_put(lan9645x, s->target_isdx);

	if (s->m)
		lan9645x_mirror_put(lan9645x);

	if (s->pol_idx) {
		lan9645x_police_del(lan9645x, s->pol_idx);
		lan9645x_qos_polix_free(lan9645x, s->pol_idx);
	}
}

int lan9645x_tc_flower_add(struct lan9645x_port *p, struct flow_cls_offload *f,
			   bool ingress)
{
	struct vcap_tc_flower_parse_usage state = {
		.fco = f,
		.l3_proto = ETH_P_ALL,
	};
	struct lan9645x *lan9645x = p->lan9645x;
	struct lan9645x_multi_rules multi = {};
	struct lan9645x_act_state act_s = { };
	struct netlink_ext_ack *extack;
	struct vcap_admin *admin;
	struct vcap_rule *vrule;
	struct net_device *ndev;
	int err, lookup, fcid;

	err = lan9645x_tc_flower_action_check(lan9645x->vcap_ctrl, f, ingress);
	if (err)
		return err;

	fcid = f->common.chain_index;
	extack = f->common.extack;

	/* TODO: handle the empty flower with goto as matchall goto */
	admin = vcap_find_admin(lan9645x->vcap_ctrl, fcid);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(extack, "Invalid chain");
		return -EINVAL;
	}

	lookup = vcap_chain_id_to_lookup(admin, fcid);
	state.admin = admin;
	ndev = lan9645x_port_to_ndev(p);

	vrule = vcap_alloc_rule(lan9645x->vcap_ctrl, ndev, fcid,
				VCAP_USER_TC, f->common.prio, 0);
	if (IS_ERR(vrule))
		return PTR_ERR(vrule);

	vrule->cookie = f->cookie;
	state.vrule = vrule;
	state.frule = flow_cls_offload_flow_rule(f);

	/* Parse dissectors/keys given by user */
	err = lan9645x_tc_flower_use_dissectors(&state, admin, vrule);
	if (err)
		goto out;

	/* Setup rule link keyfields if requested. These keys enable later
	 * flower rules to use action goto CHAIN_ID, to link vcap processing to
	 * this rule.
	 */
	err = lan9645x_tc_add_goto_link_target(lan9645x, &act_s, admin, vrule,
					       fcid, extack);
	if (err)
		goto out;

	/* Parse actions given by user and translate to vcap actions */
	err = lan9645x_tc_parse_actions(&act_s, p, admin, vrule, f);
	if (err)
		goto out;

	err = lan9645x_tc_select_protocol_keyset(p, vrule, admin, &state,
						 &multi);
	if (err) {
		dev_dbg(lan9645x->dev, "err: %d", err);
		NL_SET_ERR_MSG_MOD(extack,
				   "No matching port keyset for filter protocol and keys");
		goto out;
	}

	/* TODO: vrule may not have keyset yet. Do we need to run through all
	 * used keysets in multi?
	 */
	lan9645x_dmac_enable(p, lookup, !!(vrule->keyset == VCAP_KFS_NORMAL_DMAC));

	err = lan9645x_tc_add_rule_counter(admin, vrule);
	if (err) {
		vcap_set_tc_exterr(f, vrule);
		goto out;
	}

	err = lan9645x_tc_add_remaining_rules(p, f, vrule, admin, &multi);

out:
	if (err)
		/* Cleanup any resources allocated during action parsing */
		lan9645x_tc_action_state_cleanup(lan9645x, &act_s);
	vcap_free_rule(vrule);
	return err;
}
