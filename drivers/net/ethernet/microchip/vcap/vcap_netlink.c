/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip VCAP API Netlink interface
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <net/genetlink.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "vcap_api_debugfs.h"
#include "vcap_netlink.h"
#include "vcap_api.h"
#include "vcap_api_private.h"

static struct net_device *priv_ndev;
static struct vcap_control *vctrl;
static struct genl_family vcap_genl_family;

#define VCAP_NETLINK_NAME "mchp_vcap_nl"
#define VCAP_NETLINK_VERSION 1

enum vcap_nl_attr {
	VCAP_NL_ATTR_NONE,
	VCAP_NL_ATTR_PLATFORM_NAME,
	VCAP_NL_ATTR_VCAPS,
	VCAP_NL_ATTR_VCAP_ITEM,
	VCAP_NL_ATTR_VCAP_TYPE,
	VCAP_NL_ATTR_VCAP_NAME,
	VCAP_NL_ATTR_VCAP_LOOKUP,
	VCAP_NL_ATTR_RULE_LIST,
	VCAP_NL_ATTR_RULE_PRIORITY,
	VCAP_NL_ATTR_RULE_ID,
	VCAP_NL_ATTR_RULE_ADDR,
	VCAP_NL_ATTR_RULE_SIZE,
	VCAP_NL_ATTR_RULE_COUNTER,
	VCAP_NL_ATTR_RULE_HIT,
	VCAP_NL_ATTR_KEYSETS,
	VCAP_NL_ATTR_KEYSET_ITEM,
	VCAP_NL_ATTR_KEYSET_ID,
	VCAP_NL_ATTR_KEYSET_NAME,
	VCAP_NL_ATTR_ACTIONSETS,
	VCAP_NL_ATTR_ACTIONSET_ITEM,
	VCAP_NL_ATTR_ACTIONSET_ID,
	VCAP_NL_ATTR_ACTIONSET_NAME,
	VCAP_NL_ATTR_KEYS,
	VCAP_NL_ATTR_KEY_ITEM,
	VCAP_NL_ATTR_KEY_ID,
	VCAP_NL_ATTR_KEY_NAME,
	VCAP_NL_ATTR_KEY_WIDTH,
	VCAP_NL_ATTR_KEY_TYPE,
	VCAP_NL_ATTR_ACTIONS,
	VCAP_NL_ATTR_ACTION_ITEM,
	VCAP_NL_ATTR_ACTION_ID,
	VCAP_NL_ATTR_ACTION_NAME,
	VCAP_NL_ATTR_ACTION_WIDTH,
	VCAP_NL_ATTR_ACTION_TYPE,
	VCAP_NL_ATTR_VALUE,
	VCAP_NL_ATTR_PORT_ITEM,
	VCAP_NL_ATTR_PORT_INFO,
	VCAP_NL_ATTR_VCAP_INSTANCE,
	VCAP_NL_ATTR_VCAP_INFO_ITEM,
	VCAP_NL_ATTR_VCAP_INFO,
	/* This must be the last entry */
	VCAP_NL_ATTR_END,
};

#define VCAP_NL_ATTR_MAX (VCAP_NL_ATTR_END - 1)

enum vcap_genl_cmd {
	VCAP_GENL_CMD_GET_PLATFORM_INFO,
	VCAP_GENL_CMD_GET_VCAP_INFO,
	VCAP_GENL_CMD_GET_VCAP_INSTANCE_INFO,
	VCAP_GENL_CMD_GET_KEYSET_INFO,
	VCAP_GENL_CMD_GET_ACTIONSET_INFO,
	VCAP_GENL_CMD_ADD_RULE,
	VCAP_GENL_CMD_GET_RULE,
	VCAP_GENL_CMD_MOD_RULE,
	VCAP_GENL_CMD_DEL_RULE,
	VCAP_GENL_CMD_LIST_RULES,
	VCAP_GENL_CMD_RESET_RULE_COUNTER,
	VCAP_GENL_CMD_GET_PORT_INFO,
};

struct nla_policy vcap_genl_policy[VCAP_NL_ATTR_END] = {
	[VCAP_NL_ATTR_NONE] = { .type = NLA_UNSPEC },
	[VCAP_NL_ATTR_PLATFORM_NAME] = { .type = NLA_STRING },
	[VCAP_NL_ATTR_VCAPS] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_VCAP_ITEM] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_VCAP_TYPE] = { .type = NLA_U8 },
	[VCAP_NL_ATTR_VCAP_NAME] = { .type = NLA_STRING },
	[VCAP_NL_ATTR_VCAP_LOOKUP] = { .type = NLA_U8 },
	[VCAP_NL_ATTR_RULE_LIST] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_RULE_PRIORITY] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_RULE_ID] = { .type = NLA_U32 },
	[VCAP_NL_ATTR_RULE_ADDR] = { .type = NLA_U32 },
	[VCAP_NL_ATTR_RULE_SIZE] = { .type = NLA_U8 },
	[VCAP_NL_ATTR_RULE_COUNTER] = { .type = NLA_U32 },
	[VCAP_NL_ATTR_RULE_HIT] = { .type = NLA_U8 },
	[VCAP_NL_ATTR_KEYSETS] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_KEYSET_ITEM] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_KEYSET_ID] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_KEYSET_NAME] = { .type = NLA_STRING },
	[VCAP_NL_ATTR_ACTIONSETS] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_ACTIONSET_ITEM] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_ACTIONSET_ID] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_ACTIONSET_NAME] = { .type = NLA_STRING },
	[VCAP_NL_ATTR_KEYS] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_KEY_ITEM] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_KEY_ID] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_KEY_NAME] = { .type = NLA_STRING },
	[VCAP_NL_ATTR_KEY_WIDTH] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_KEY_TYPE] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_ACTIONS] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_ACTION_ITEM] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_ACTION_ID] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_ACTION_NAME] = { .type = NLA_STRING },
	[VCAP_NL_ATTR_ACTION_WIDTH] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_ACTION_TYPE] = { .type = NLA_U16 },
	[VCAP_NL_ATTR_VALUE] = { .type = NLA_BINARY }, /* key/mask/action */
	[VCAP_NL_ATTR_PORT_ITEM] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_PORT_INFO] = { .type = NLA_STRING },
	[VCAP_NL_ATTR_VCAP_INSTANCE] = { .type = NLA_U8 },
	[VCAP_NL_ATTR_VCAP_INFO_ITEM] = { .type = NLA_NESTED },
	[VCAP_NL_ATTR_VCAP_INFO] = { .type = NLA_STRING },
};

static int vcap_put_keyfields(struct sk_buff *msg, struct vcap_admin *admin, struct vcap_rule *rule)
{
	struct nlattr *start_keys, *start_key;
	struct vcap_client_keyfield_data *key;
	struct vcap_client_keyfield *ckf;
	const struct vcap_field *fields;

	fields = vcap_keyfields(vctrl, admin->vtype, rule->keyset);
	if (!fields)
		return -1;

	start_keys = nla_nest_start(msg, VCAP_NL_ATTR_KEYS);

	/* Loop over keys and add them to the netlink message */
	list_for_each_entry(ckf, &rule->keyfields, ctrl.list) {
		key = &ckf->data;
		start_key = nla_nest_start(msg, VCAP_NL_ATTR_KEY_ITEM);
		nla_put_u16(msg, VCAP_NL_ATTR_KEY_ID, ckf->ctrl.key);
		pr_debug("%s:%d: field: %s\n", __func__, __LINE__,
			 vctrl->stats->keyfield_names[ckf->ctrl.key]);
		switch (ckf->ctrl.type) {
		case VCAP_FIELD_BIT:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u1), &key->u1);
			break;
		case VCAP_FIELD_U32:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u32), &key->u32);
			break;
		case VCAP_FIELD_U48:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u48), &key->u48);
			break;
		case VCAP_FIELD_U56:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u56), &key->u56);
			break;
		case VCAP_FIELD_U64:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u64), &key->u64);
			break;
		case VCAP_FIELD_U72:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u72), &key->u72);
			break;
		case VCAP_FIELD_U112:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u112), &key->u112);
			break;
		case VCAP_FIELD_U128:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(key->u128), &key->u128);
			break;
		}
		nla_nest_end(msg, start_key);
	}
	nla_nest_end(msg, start_keys);
	return 0;
}

static int vcap_put_actionfields(struct sk_buff *msg, struct vcap_admin *admin, struct vcap_rule *rule)
{
	struct vcap_control *vctrl = to_intrule(rule)->vctrl;
	struct nlattr *start_actions, *start_action;
	struct vcap_client_actionfield_data *action;
	struct vcap_client_actionfield *caf;
	const struct vcap_field *fields;

	fields = vcap_actionfields(vctrl, admin->vtype, rule->actionset);
	if (!fields)
		return -1;

	start_actions = nla_nest_start(msg, VCAP_NL_ATTR_ACTIONS);

	/* Loop over actions and add them to the netlink message */
	list_for_each_entry(caf, &rule->actionfields, ctrl.list) {
		action = &caf->data;
		start_action = nla_nest_start(msg, VCAP_NL_ATTR_ACTION_ITEM);
		nla_put_u16(msg, VCAP_NL_ATTR_ACTION_ID, caf->ctrl.action);
		pr_debug("%s:%d: field: %s\n", __func__, __LINE__,
			 vctrl->stats->actionfield_names[caf->ctrl.action]);
		switch (caf->ctrl.type) {
		case VCAP_FIELD_BIT:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u1), &action->u1);
			break;
		case VCAP_FIELD_U32:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u32), &action->u32);
			break;
		case VCAP_FIELD_U48:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u48), &action->u48);
			break;
		case VCAP_FIELD_U56:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u56), &action->u56);
			break;
		case VCAP_FIELD_U64:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u64), &action->u64);
			break;
		case VCAP_FIELD_U72:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u72), &action->u72);
			break;
		case VCAP_FIELD_U112:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u112), &action->u112);
			break;
		case VCAP_FIELD_U128:
			nla_put(msg, VCAP_NL_ATTR_VALUE, sizeof(action->u128), &action->u128);
			break;
		}
		nla_nest_end(msg, start_action);
	}
	nla_nest_end(msg, start_actions);
	return 0;
}

static void vcap_genl_copy_keyfield(struct vcap_client_keyfield_data *data,
				    struct nlattr *value,
				    enum vcap_field_type ftype)
{

	switch (ftype) {
	case VCAP_FIELD_BIT:
		nla_memcpy((void *)&data->u1, value, nla_len(value));
		break;
	case VCAP_FIELD_U32:
		nla_memcpy((void *)&data->u32, value, nla_len(value));
		break;
	case VCAP_FIELD_U48:
		nla_memcpy((void *)&data->u48, value, nla_len(value));
		break;
	case VCAP_FIELD_U56:
		nla_memcpy((void *)&data->u56, value, nla_len(value));
		break;
	case VCAP_FIELD_U64:
		nla_memcpy((void *)&data->u64, value, nla_len(value));
		break;
	case VCAP_FIELD_U72:
		nla_memcpy((void *)&data->u72, value, nla_len(value));
		break;
	case VCAP_FIELD_U112:
		nla_memcpy((void *)&data->u112, value, nla_len(value));
		break;
	case VCAP_FIELD_U128:
		nla_memcpy((void *)&data->u128, value, nla_len(value));
		break;
	}
}

static void vcap_genl_copy_actionfield(struct vcap_client_actionfield_data *data,
				       struct nlattr *value,
				       enum vcap_field_type ftype)
{

	switch (ftype) {
	case VCAP_FIELD_BIT:
		nla_memcpy((void *)&data->u1, value, nla_len(value));
		break;
	case VCAP_FIELD_U32:
		nla_memcpy((void *)&data->u32, value, nla_len(value));
		break;
	case VCAP_FIELD_U48:
		nla_memcpy((void *)&data->u48, value, nla_len(value));
		break;
	case VCAP_FIELD_U56:
		nla_memcpy((void *)&data->u56, value, nla_len(value));
		break;
	case VCAP_FIELD_U64:
		nla_memcpy((void *)&data->u64, value, nla_len(value));
		break;
	case VCAP_FIELD_U72:
		nla_memcpy((void *)&data->u72, value, nla_len(value));
		break;
	case VCAP_FIELD_U112:
		nla_memcpy((void *)&data->u112, value, nla_len(value));
		break;
	case VCAP_FIELD_U128:
		nla_memcpy((void *)&data->u128, value, nla_len(value));
		break;
	}
}

static int vcap_genl_add_rule_key_value_by_type(struct vcap_rule *rule,
						enum vcap_key_field key_id,
						enum vcap_field_type ftype,
						struct vcap_client_keyfield_data *data)
{
	switch (ftype) {
	case VCAP_FIELD_BIT:
		return vcap_rule_add_key_bit(rule, key_id, data->u1.value);
	case VCAP_FIELD_U32:
		return vcap_rule_add_key_u32(rule, key_id, data->u32.value,
					     data->u32.value);
	case VCAP_FIELD_U48:
		return vcap_rule_add_key_u48(rule, key_id, &data->u48);
	case VCAP_FIELD_U56:
		return vcap_rule_add_key_u56(rule, key_id, &data->u56);
	case VCAP_FIELD_U64:
		return vcap_rule_add_key_u64(rule, key_id, &data->u64);
	case VCAP_FIELD_U72:
		return vcap_rule_add_key_u72(rule, key_id, &data->u72);
	case VCAP_FIELD_U112:
		return vcap_rule_add_key_u112(rule, key_id, &data->u112);
	case VCAP_FIELD_U128:
		return vcap_rule_add_key_u128(rule, key_id, &data->u128);
	default:
		break;
	}

	return 0;
}

static int vcap_genl_add_rule_key_value(struct vcap_rule *rule,
					struct nlattr *value,
					enum vcap_key_field key_id,
					enum vcap_field_type ftype)
{
	struct vcap_client_keyfield_data data;

	vcap_genl_copy_keyfield(&data, value, ftype);
	return vcap_genl_add_rule_key_value_by_type(rule, key_id, ftype, &data);
}

static int vcap_genl_add_rule_key(struct vcap_rule *rule, struct nlattr *item,
				  const struct vcap_field *fields,
				  int field_count)
{
	struct nlattr *nattrs[VCAP_NL_ATTR_END];
	const struct vcap_field *fld;
	enum vcap_key_field key_id;
	int res = -1;

	if (nla_parse_nested(nattrs, VCAP_NL_ATTR_MAX, item, vcap_genl_policy, NULL)) {
		return res;
	}
	if (nattrs[VCAP_NL_ATTR_KEY_ID]) {
		key_id = nla_get_u16(nattrs[VCAP_NL_ATTR_KEY_ID]);
		if (key_id >= field_count) {
			return res;
		}
		fld = &fields[key_id];
		if (!fld->width) {
			return res;
		}
		if (nattrs[VCAP_NL_ATTR_VALUE]) {
			if (vcap_genl_add_rule_key_value(rule,
							 nattrs[VCAP_NL_ATTR_VALUE],
							 key_id,
							 fld->type)) {
				return -1;
			}
		}
		res = 0;
	}
	return res;
}

static int vcap_genl_add_rule_keys(struct vcap_rule *rule, struct nlattr *keys,
				   const struct vcap_field *fields,
				   int field_count)
{
	struct nlattr *cur;
	int rem;

	if (!keys) {
		return -1;
	}

	nla_for_each_nested(cur, keys, rem) {
		if (nla_type(cur) == VCAP_NL_ATTR_KEY_ITEM) {
			if (vcap_genl_add_rule_key(rule, cur, fields, field_count)) {
				return -1;
			}
		}
	}
	return 0;
}

static int vcap_genl_add_rule_action_value_by_type(
	struct vcap_rule *rule, enum vcap_action_field action_id,
	enum vcap_field_type ftype, struct vcap_client_actionfield_data *data)
{
	switch (ftype) {
	case VCAP_FIELD_BIT:
		return vcap_rule_add_action_bit(rule, action_id,
						data->u1.value);
	case VCAP_FIELD_U32:
		return vcap_rule_add_action_u32(rule, action_id,
						data->u32.value);
	case VCAP_FIELD_U48:
		return vcap_rule_add_action_u48(rule, action_id, &data->u48);
	case VCAP_FIELD_U56:
		return vcap_rule_add_action_u56(rule, action_id, &data->u56);
	case VCAP_FIELD_U64:
		return vcap_rule_add_action_u64(rule, action_id, &data->u64);
	case VCAP_FIELD_U72:
		return vcap_rule_add_action_u72(rule, action_id, &data->u72);
	case VCAP_FIELD_U112:
		return vcap_rule_add_action_u112(rule, action_id, &data->u112);
	case VCAP_FIELD_U128:
		return vcap_rule_add_action_u128(rule, action_id, &data->u128);
	default:
		break;
	}

	return 0;
}

static int vcap_genl_add_rule_action_value(struct vcap_rule *rule,
					struct nlattr *value,
					enum vcap_action_field action_id,
					enum vcap_field_type ftype)
{
	struct vcap_client_actionfield_data data;

	vcap_genl_copy_actionfield(&data, value, ftype);
	return vcap_genl_add_rule_action_value_by_type(rule, action_id, ftype, &data);
}

static int vcap_genl_add_rule_action(struct vcap_rule *rule, struct nlattr *item,
				     const struct vcap_field *fields,
				     int field_count)
{
	struct nlattr *nattrs[VCAP_NL_ATTR_END];
	const struct vcap_field *fld;
	enum vcap_action_field action_id;
	int res = -1;

	if (nla_parse_nested(nattrs, VCAP_NL_ATTR_MAX, item, vcap_genl_policy, NULL)) {
		return res;
	}
	if (nattrs[VCAP_NL_ATTR_ACTION_ID]) {
		action_id = nla_get_u16(nattrs[VCAP_NL_ATTR_ACTION_ID]);
		if (action_id >= field_count) {
			return res;
		}
		fld = &fields[action_id];
		if (!fld->width) {
			return res;
		}
		if (nattrs[VCAP_NL_ATTR_VALUE]) {
			if (vcap_genl_add_rule_action_value(rule, nattrs[VCAP_NL_ATTR_VALUE],
							 action_id,
							 fld->type)) {
				return -1;
			}
		}
		res = 0;
	}
	return res;
}

static int vcap_genl_add_rule_actions(struct vcap_rule *rule, struct nlattr *actions,
				      const struct vcap_field *fields,
				      int field_count)
{
	struct nlattr *cur;
	int rem;

	if (!actions) {
		return -1;
	}

	nla_for_each_nested(cur, actions, rem) {
		if (nla_type(cur) == VCAP_NL_ATTR_ACTION_ITEM) {
			if (vcap_genl_add_rule_action(rule, cur, fields, field_count)) {
				return -1;
			}
		}
	}
	return 0;
}

static int vcap_genl_mod_rule_action_by_type(
	struct vcap_rule *rule, enum vcap_action_field action_id,
	enum vcap_field_type ftype, struct vcap_client_actionfield_data *data)
{
	switch (ftype) {
	case VCAP_FIELD_BIT:
		return vcap_rule_mod_action_bit(rule, action_id,
						data->u1.value);
	case VCAP_FIELD_U32:
		return vcap_rule_mod_action_u32(rule, action_id,
						data->u32.value);
	case VCAP_FIELD_U48:
		return vcap_rule_mod_action_u48(rule, action_id, &data->u48);
	case VCAP_FIELD_U56:
		return vcap_rule_mod_action_u56(rule, action_id, &data->u56);
	case VCAP_FIELD_U64:
		return vcap_rule_mod_action_u64(rule, action_id, &data->u64);
	case VCAP_FIELD_U72:
		return vcap_rule_mod_action_u72(rule, action_id, &data->u72);
	case VCAP_FIELD_U112:
		return vcap_rule_mod_action_u112(rule, action_id, &data->u112);
	case VCAP_FIELD_U128:
		return vcap_rule_mod_action_u128(rule, action_id, &data->u128);
	default:
		break;
	}

	return 0;
}

static int vcap_genl_mod_rule_action_value(struct vcap_rule *rule,
					struct nlattr *value,
					enum vcap_action_field action_id,
					enum vcap_field_type ftype)
{
	struct vcap_client_actionfield_data data;

	vcap_genl_copy_actionfield(&data, value, ftype);
	return vcap_genl_mod_rule_action_by_type(rule, action_id, ftype, &data);
}

static int vcap_genl_mod_rule_action(struct vcap_rule *rule, struct nlattr *item,
				     const struct vcap_field *fields,
				     int field_count)
{
	struct nlattr *nattrs[VCAP_NL_ATTR_END];
	const struct vcap_field *fld;
	enum vcap_action_field action_id;
	int res = -1;

	if (nla_parse_nested(nattrs, VCAP_NL_ATTR_MAX, item, vcap_genl_policy, NULL)) {
		return res;
	}
	if (nattrs[VCAP_NL_ATTR_ACTION_ID]) {
		action_id = nla_get_u16(nattrs[VCAP_NL_ATTR_ACTION_ID]);
		if (action_id >= field_count) {
			return res;
		}
		fld = &fields[action_id];
		if (!fld->width) {
			return res;
		}
		if (nattrs[VCAP_NL_ATTR_VALUE]) {
			if (vcap_genl_mod_rule_action_value(rule, nattrs[VCAP_NL_ATTR_VALUE],
							 action_id,
							 fld->type)) {
				return -1;
			}
		}
		res = 0;
	}
	return res;
}

static int vcap_genl_mod_rule_actions(struct vcap_rule *rule, struct nlattr *actions,
				      const struct vcap_field *fields,
				      int field_count)
{
	struct nlattr *cur;
	int rem;

	if (!actions) {
		return 0;
	}

	nla_for_each_nested(cur, actions, rem) {
		if (nla_type(cur) == VCAP_NL_ATTR_ACTION_ITEM) {
			if (vcap_genl_mod_rule_action(rule, cur, fields, field_count)) {
				return -1;
			}
		}
	}
	return 0;
}

static int vcap_genl_mod_rule_key_by_type(
	struct vcap_rule *rule, enum vcap_key_field key_id,
	enum vcap_field_type ftype, struct vcap_client_keyfield_data *data)
{
	switch (ftype) {
	case VCAP_FIELD_BIT:
		return vcap_rule_mod_key_bit(rule, key_id, data->u1.value);
	case VCAP_FIELD_U32:
		return vcap_rule_mod_key_u32(rule, key_id, data->u32.value,
					     data->u32.value);
	case VCAP_FIELD_U48:
		return vcap_rule_mod_key_u48(rule, key_id, &data->u48);
	case VCAP_FIELD_U56:
		return vcap_rule_mod_key_u56(rule, key_id, &data->u56);
	case VCAP_FIELD_U64:
		return vcap_rule_mod_key_u64(rule, key_id, &data->u64);
	case VCAP_FIELD_U72:
		return vcap_rule_mod_key_u72(rule, key_id, &data->u72);
	case VCAP_FIELD_U112:
		return vcap_rule_mod_key_u112(rule, key_id, &data->u112);
	case VCAP_FIELD_U128:
		return vcap_rule_mod_key_u128(rule, key_id, &data->u128);
	default:
		break;
	}

	return 0;
}

static int vcap_genl_mod_rule_key_value(struct vcap_rule *rule,
					struct nlattr *value,
					enum vcap_key_field key_id,
					enum vcap_field_type ftype)
{
	struct vcap_client_keyfield_data data;

	vcap_genl_copy_keyfield(&data, value, ftype);
	return vcap_genl_mod_rule_key_by_type(rule, key_id, ftype, &data);
}

static int vcap_genl_mod_rule_key(struct vcap_rule *rule, struct nlattr *item,
				  const struct vcap_field *fields,
				  int field_count)
{
	struct nlattr *nattrs[VCAP_NL_ATTR_END];
	const struct vcap_field *fld;
	enum vcap_key_field key_id;
	int res = -1;

	if (nla_parse_nested(nattrs, VCAP_NL_ATTR_MAX, item, vcap_genl_policy, NULL)) {
		return res;
	}
	if (nattrs[VCAP_NL_ATTR_KEY_ID]) {
		key_id = nla_get_u16(nattrs[VCAP_NL_ATTR_KEY_ID]);
		if (key_id >= field_count) {
			return res;
		}
		fld = &fields[key_id];
		if (!fld->width) {
			return res;
		}
		if (nattrs[VCAP_NL_ATTR_VALUE]) {
			if (vcap_genl_mod_rule_key_value(rule,
							 nattrs[VCAP_NL_ATTR_VALUE],
							 key_id,
							 fld->type)) {
				return -1;
			}
		}
		res = 0;
	}
	return res;
}

static int vcap_genl_mod_rule_keys(struct vcap_rule *rule, struct nlattr *keys,
				   const struct vcap_field *fields,
				   int field_count)
{
	struct nlattr *cur;
	int rem;

	if (!keys) {
		return 0;
	}

	nla_for_each_nested(cur, keys, rem) {
		if (nla_type(cur) == VCAP_NL_ATTR_KEY_ITEM) {
			if (vcap_genl_mod_rule_key(rule, cur, fields, field_count)) {
				return -1;
			}
		}
	}
	return 0;
}

static int vcap_genl_rule_cb(void *arg, struct vcap_rule *rule)
{
	return nla_put_u32(arg, VCAP_NL_ATTR_RULE_ID, rule->id);
}

static int vcap_genl_get_vcap_key(enum vcap_type vt,
				  enum vcap_keyfield_set key,
				  const struct vcap_field *kf,
				  struct sk_buff *msg)
{
	struct nlattr *start_key;

	/* Check that the key is valid */
	if (kf->width == 0)
		return 0;

	start_key = nla_nest_start(msg, VCAP_NL_ATTR_KEY_ITEM);
	if (!start_key)
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_KEY_ID, key))
		return -EMSGSIZE;
	if (nla_put_string(msg, VCAP_NL_ATTR_KEY_NAME,
			   vctrl->stats->keyfield_names[key]))
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_KEY_TYPE, kf->type))
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_KEY_WIDTH, kf->width))
		return -EMSGSIZE;
	nla_nest_end(msg, start_key);
	return 0;
}

static int vcap_genl_get_vcap_keyset_keys(enum vcap_type vt,
					  enum vcap_keyfield_set keyset,
					  struct sk_buff *msg)
{
	int count = vcap_keyfield_count(vctrl, vt, keyset);
	const struct vcap_field *fields;
	enum vcap_keyfield_set key;
	struct nlattr *start_keys;
	int ret;

	if (count == 0)
		return 0;
	fields = vcap_keyfields(vctrl, vt, keyset);
	if (!fields)
		return 0;

	start_keys = nla_nest_start(msg, VCAP_NL_ATTR_KEYS);
	for (key = 0; key < count; ++key) {
		ret = vcap_genl_get_vcap_key(vt, key, &fields[key], msg);
		if (ret)
			return ret;
	}
	nla_nest_end(msg, start_keys);
	return 0;
}

static int vcap_genl_get_vcap_keyset(enum vcap_type vt,
				     enum vcap_keyfield_set keyset,
				     struct sk_buff *msg)
{
	struct nlattr *start_keyset;

	/* Check that the keyset is valid */
	if (vcap_keyfieldset(vctrl, vt, keyset) == 0)
		return 0;

	start_keyset = nla_nest_start(msg, VCAP_NL_ATTR_KEYSET_ITEM);
	if (!start_keyset)
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_KEYSET_ID, keyset))
		return -EMSGSIZE;
	if (nla_put_string(msg, VCAP_NL_ATTR_KEYSET_NAME, vctrl->stats->keyfield_set_names[keyset]))
		return -EMSGSIZE;
	nla_nest_end(msg, start_keyset);
	return 0;
}

static int vcap_genl_get_vcap_keysets(enum vcap_type vt, struct sk_buff *msg)
{
	struct nlattr *start_keysets;
	enum vcap_keyfield_set keyset;
	int ret;

	start_keysets = nla_nest_start(msg, VCAP_NL_ATTR_KEYSETS);
	for (keyset = 0; keyset < vctrl->vcaps[vt].keyfield_set_size; ++keyset) {
		ret = vcap_genl_get_vcap_keyset(vt, keyset, msg);
		if (ret)
			return ret;
	}
	nla_nest_end(msg, start_keysets);
	return 0;
}

static int vcap_genl_get_vcap_action(enum vcap_type vt,
				  enum vcap_actionfield_set action,
				  const struct vcap_field *af,
				  struct sk_buff *msg)
{
	struct nlattr *start_action;

	/* Check that the action is valid */
	if (af->width == 0)
		return 0;

	start_action = nla_nest_start(msg, VCAP_NL_ATTR_ACTION_ITEM);
	if (!start_action)
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_ACTION_ID, action))
		return -EMSGSIZE;
	if (nla_put_string(msg, VCAP_NL_ATTR_ACTION_NAME,
			   vctrl->stats->actionfield_names[action]))
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_ACTION_TYPE, af->type))
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_ACTION_WIDTH, af->width))
		return -EMSGSIZE;
	nla_nest_end(msg, start_action);
	return 0;
}

static int vcap_genl_get_vcap_actionset_actions(enum vcap_type vt,
						enum vcap_actionfield_set actionset,
						struct sk_buff *msg)
{
	int count = vcap_actionfield_count(vctrl, vt, actionset);
	const struct vcap_field *fields;
	enum vcap_actionfield_set action;
	struct nlattr *start_actions;
	int ret;

	if (count == 0)
		return 0;
	fields = vcap_actionfields(vctrl, vt, actionset);
	if (!fields)
		return 0;

	start_actions = nla_nest_start(msg, VCAP_NL_ATTR_ACTIONS);
	for (action = 0; action < count; ++action) {
		ret = vcap_genl_get_vcap_action(vt, action, &fields[action], msg);
		if (ret)
			return ret;
	}
	nla_nest_end(msg, start_actions);
	return 0;
}

static int vcap_genl_get_vcap_actionset(enum vcap_type vt,
				     enum vcap_actionfield_set actionset,
				     struct sk_buff *msg)
{
	struct nlattr *start_actionset;

	/* Check that the actionset is valid */
	if (vcap_actionfieldset(vctrl, vt, actionset) == 0)
		return 0;

	start_actionset = nla_nest_start(msg, VCAP_NL_ATTR_ACTIONSET_ITEM);
	if (!start_actionset)
		return -EMSGSIZE;
	if (nla_put_u16(msg, VCAP_NL_ATTR_ACTIONSET_ID, actionset))
		return -EMSGSIZE;
	if (nla_put_string(msg, VCAP_NL_ATTR_ACTIONSET_NAME, vctrl->stats->actionfield_set_names[actionset]))
		return -EMSGSIZE;
	nla_nest_end(msg, start_actionset);
	return 0;
}

static int vcap_genl_get_vcap_actionsets(enum vcap_type vt, struct sk_buff *msg)
{
	struct nlattr *start_actionsets;
	enum vcap_actionfield_set actionset;
	int ret;

	start_actionsets = nla_nest_start(msg, VCAP_NL_ATTR_ACTIONSETS);
	for (actionset = 0; actionset < vctrl->vcaps[vt].actionfield_set_size; ++actionset) {
		ret = vcap_genl_get_vcap_actionset(vt, actionset, msg);
		if (ret)
			return ret;
	}
	nla_nest_end(msg, start_actionsets);
	return 0;
}

int vcap_genl_port_printf(void *out, const char *fmt, ...)
{
	struct sk_buff *msg = out;
	char buffer[300];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	return nla_put_string(msg, VCAP_NL_ATTR_PORT_INFO, buffer);
}

static int vcap_genl_get_port_info(struct sk_buff *skb, struct genl_info *info)
{
	struct vcap_admin *admin_itr, *admin = NULL;
	struct vcap_output_print out;
	struct nlattr *start_list;
	enum vcap_type vtype;
	struct sk_buff *msg;
	void *hdr;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_VCAP_TYPE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vtype = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_TYPE]);
	if (vctrl->vcaps[vtype].rows == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is invalid");
		err = -EINVAL;
		goto invalid_info;
	}

	list_for_each_entry(admin_itr, &vctrl->list, list) {
		if (admin_itr->vinst)
			continue;
		if (admin_itr->vtype == vtype) {
			admin = admin_itr;
			break;
		}
	}

	/* There should always be an admin for a validated vtype. */
	if (!admin) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not find admin for given vtype");
		err = -EINVAL;
		goto invalid_info;
	}

	/* Create the response */
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -EINVAL;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_ADD_RULE);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	start_list = nla_nest_start(msg, VCAP_NL_ATTR_PORT_ITEM);
	if (!start_list)
		goto nla_put_failure;

	out.prf = (void *)vcap_genl_port_printf;
	out.dst = msg;

	if (vctrl->ops->port_info(priv_ndev, admin, &out))
		goto nla_put_failure;

	nla_nest_end(msg, start_list);

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int vcap_genl_reset_rule_counter(struct sk_buff *skb, struct genl_info *info)
{
	struct vcap_counter ctr = {0};
	struct vcap_rule *rule;
	struct sk_buff *msg;
	u32 rule_id;
	void *hdr;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_RULE_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "RULE_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	rule_id = nla_get_u32(info->attrs[VCAP_NL_ATTR_RULE_ID]);

	rule = vcap_get_rule(vctrl, rule_id);

	if (vcap_rule_set_counter(rule, &ctr)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not reset counter");
		err = -EINVAL;
		goto invalid_info;
	}

	/* Create the response */
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -EINVAL;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_ADD_RULE);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	/* Put the rule id to allow the client to check this */
	if (nla_put_u32(msg, VCAP_NL_ATTR_RULE_ID, rule_id))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int vcap_genl_list_rules(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *start_list;
	struct sk_buff *msg;
	void *hdr;
	int err;

	/* Create the response with the rule id */
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -EINVAL;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_ADD_RULE);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}
	start_list = nla_nest_start(msg, VCAP_NL_ATTR_RULE_LIST);
	if (!start_list)
		goto nla_put_failure;
	/* Add all rule ids via a callback interface */
	if (vcap_rule_iter(vctrl, vcap_genl_rule_cb, msg))
		goto nla_put_failure;
	nla_nest_end(msg, start_list);

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int vcap_genl_mod_rule(struct sk_buff *skb, struct genl_info *info)
{
	const struct vcap_field *fields;
	struct vcap_admin *admin;
	struct vcap_rule *rule;
	struct sk_buff *msg;
	int field_count;
	u32 rule_id;
	void *hdr;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_RULE_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "RULE_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	rule_id = nla_get_u32(info->attrs[VCAP_NL_ATTR_RULE_ID]);

	/* Get the rule specified by the rule id */
	rule = vcap_get_rule(vctrl, rule_id);
	if (!rule || IS_ERR(rule)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not get RULE_ID");
		err = -EINVAL;
		goto invalid_info;
	}
	admin = vcap_find_admin(vctrl, rule->vcap_chain_id);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not get the VCAP of this rule");
		err = -EINVAL;
		goto invalid_info;
	}

	/* Modify keys */
	fields = vcap_keyfields(vctrl, admin->vtype, rule->keyset);
	if (!fields) {
		NL_SET_ERR_MSG_MOD(info->extack, "No rule keys for this keyset");
		err = -EINVAL;
		goto err_rule_free;
	}
	field_count = vcap_keyfield_count(vctrl, admin->vtype, rule->keyset);

	if (vcap_genl_mod_rule_keys(rule, info->attrs[VCAP_NL_ATTR_KEYS],
				    fields, field_count)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not modify rule keys");
		err = -EINVAL;
		goto err_rule_free;
	}

	fields = vcap_actionfields(vctrl, admin->vtype, rule->actionset);
	if (!fields) {
		NL_SET_ERR_MSG_MOD(info->extack, "No rule actions for this actionset");
		err = -EINVAL;
		goto err_rule_free;
	}
	field_count = vcap_actionfield_count(vctrl, admin->vtype, rule->actionset);

	if (vcap_genl_mod_rule_actions(rule, info->attrs[VCAP_NL_ATTR_ACTIONS],
				    fields, field_count)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not modify rule actions");
		err = -EINVAL;
		goto err_rule_free;
	}

	if (vcap_mod_rule(rule)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not modify rule");
		err = -EINVAL;
		goto err_rule_free;
	}

	/* Create the response */
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -EINVAL;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_ADD_RULE);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	/* Put the rule id to allow the client to check this */
	if (nla_put_u32(msg, VCAP_NL_ATTR_RULE_ID, rule_id))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

err_rule_free:
	vcap_free_rule(rule);

invalid_info:
	return err;
}

static int vcap_genl_get_rule(struct sk_buff *skb, struct genl_info *info)
{
	struct vcap_counter counter;
	struct vcap_address address;
	struct vcap_admin *admin;
	struct sk_buff *msg;
	struct vcap_rule *rule;
	u32 rule_id;
	int vlookup;
	void *hdr;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_RULE_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "RULE_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	rule_id = nla_get_u32(info->attrs[VCAP_NL_ATTR_RULE_ID]);

	/* Get the rule specified by the rule id */
	rule = vcap_get_rule(vctrl, rule_id);
	if (rule == NULL || IS_ERR(rule)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not get this rule id");
		err = -EINVAL;
		goto invalid_info;
	}
	admin = vcap_find_admin(vctrl, rule->vcap_chain_id);

	if (vcap_rule_get_counter(rule, &counter)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not get the rule counter");
		err = -EINVAL;
		goto err_rule_free;
	}
	if (vcap_rule_get_address(vctrl, rule_id, &address)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not get the rule address");
		err = -EINVAL;
		goto err_rule_free;
	}
	vlookup = vcap_chain_id_to_lookup(admin, rule->vcap_chain_id);

	/* Create the response */
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -EINVAL;
		goto err_rule_free;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_ADD_RULE);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	/* Put the rule id to allow the client to check this */
	if (nla_put_u32(msg, VCAP_NL_ATTR_RULE_ID, rule_id))
		goto nla_put_failure;
	/* Put the vcap id to allow the client lookup keyset etc */
	if (nla_put_u8(msg, VCAP_NL_ATTR_VCAP_TYPE, admin->vtype))
		goto nla_put_failure;
	if (nla_put_u8(msg, VCAP_NL_ATTR_VCAP_LOOKUP, vlookup))
		goto nla_put_failure;
	if (nla_put_u16(msg, VCAP_NL_ATTR_RULE_PRIORITY, rule->priority))
		goto nla_put_failure;
	if (nla_put_u32(msg, VCAP_NL_ATTR_RULE_ADDR, address.start))
		goto nla_put_failure;
	if (nla_put_u8(msg, VCAP_NL_ATTR_RULE_SIZE, address.size))
		goto nla_put_failure;
	if (nla_put_u32(msg, VCAP_NL_ATTR_RULE_COUNTER, counter.value))
		goto nla_put_failure;
	if (nla_put_u8(msg, VCAP_NL_ATTR_RULE_HIT, counter.sticky))
		goto nla_put_failure;

	/* Put keyset id */
	if (nla_put_u16(msg, VCAP_NL_ATTR_KEYSET_ID, rule->keyset))
		goto nla_put_failure;
	/* Put actionset id */
	if (nla_put_u16(msg, VCAP_NL_ATTR_ACTIONSET_ID, rule->actionset))
		goto nla_put_failure;
	/* Put all the keys */
	if (vcap_put_keyfields(msg, admin, rule))
		goto nla_put_failure;
	/* Put all the actions */
	if (vcap_put_actionfields(msg, admin, rule))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

err_rule_free:
	vcap_free_rule(rule);

invalid_info:
	return err;
}

static int vcap_genl_del_rule(struct sk_buff *skb, struct genl_info *info)
{
	struct sk_buff *msg;
	u32 rule_id;
	void *hdr;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_RULE_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "RULE_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	rule_id = nla_get_u32(info->attrs[VCAP_NL_ATTR_RULE_ID]);

	/* Delete the rule specified by the rule id */
	if (vcap_del_rule(vctrl, priv_ndev, rule_id)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Rule could not be deleted");
		err = -EINVAL;
		goto invalid_info;
	}

	/* Create the response with the rule id */
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -EINVAL;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_ADD_RULE);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	if (nla_put_u32(msg, VCAP_NL_ATTR_RULE_ID, rule_id))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int vcap_genl_add_rule(struct sk_buff *skb, struct genl_info *info)
{
	enum vcap_keyfield_set keyset_id;
	enum vcap_actionfield_set actionset_id;
	const struct vcap_field *fields;
	struct vcap_admin *admin;
	struct vcap_rule *rule;
	struct sk_buff *msg;
	enum vcap_type vtype;
	int field_count;
	int vlookup;
	u16 priority;
	u32 rule_id;
	void *hdr;
	int cid;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_VCAP_TYPE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vtype = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_TYPE]);
	if (vctrl->vcaps[vtype].rows == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is invalid");
		err = -EINVAL;
		goto invalid_info;
	}
	if (!info->attrs[VCAP_NL_ATTR_VCAP_LOOKUP]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_LOOKUP is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vlookup = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_LOOKUP]);

	/* Check that the vcap information is valid, and get the chain id */
	admin = vcap_find_admin_with_lookup(vctrl, vtype, vlookup, &cid);
	if (!admin) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not find VCAP instance");
		err = -EINVAL;
		goto invalid_info;
	}

	if (!info->attrs[VCAP_NL_ATTR_RULE_PRIORITY]) {
		NL_SET_ERR_MSG_MOD(info->extack, "RULE_PRIORITY is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	priority = nla_get_u16(info->attrs[VCAP_NL_ATTR_RULE_PRIORITY]);

	if (!info->attrs[VCAP_NL_ATTR_RULE_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "RULE_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	rule_id = nla_get_u32(info->attrs[VCAP_NL_ATTR_RULE_ID]);

	if (!info->attrs[VCAP_NL_ATTR_KEYSET_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "KEYSET_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	keyset_id = nla_get_u16(info->attrs[VCAP_NL_ATTR_KEYSET_ID]);

	if (!info->attrs[VCAP_NL_ATTR_ACTIONSET_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "ACTIONSET_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	actionset_id = nla_get_u16(info->attrs[VCAP_NL_ATTR_ACTIONSET_ID]);

	rule = vcap_alloc_rule(vctrl, priv_ndev, cid, VCAP_USER_VCAP_UTIL,
			       priority, rule_id);

	if (!rule || IS_ERR(rule)) {
		err = PTR_ERR(rule);
		if (err == -EEXIST)
			NL_SET_ERR_MSG_MOD(info->extack, "Duplicate RULE_ID");
		if (err == -ENOSPC)
			NL_SET_ERR_MSG_MOD(info->extack, "No more space in VCAP");
		goto invalid_info;
	}

	/* Overrule keyset  */
	if (vcap_set_rule_set_keyset(rule, keyset_id)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Invalid KEYSET_ID");
		err = -EINVAL;
		goto err_rule_free;
	}

	/* Overrule actionset  */
	if (vcap_set_rule_set_actionset(rule, actionset_id)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Invalid ACTIONSET_ID");
		err = -EINVAL;
		goto err_rule_free;
	}

	/* Add keys */
	fields = vcap_keyfields(vctrl, vtype, keyset_id);
	if (!fields) {
		NL_SET_ERR_MSG_MOD(info->extack, "No rule keys for this keyset");
		err = -EINVAL;
		goto err_rule_free;
	}
	field_count = vcap_keyfield_count(vctrl, vtype, keyset_id);

	if (vcap_genl_add_rule_keys(rule, info->attrs[VCAP_NL_ATTR_KEYS],
				    fields, field_count)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add rule keys");
		err = -EINVAL;
		goto err_rule_free;
	}

	/* Add actions */
	fields = vcap_actionfields(vctrl, vtype, actionset_id);
	if (!fields) {
		NL_SET_ERR_MSG_MOD(info->extack, "No rule actions for this actionset");
		err = -EINVAL;
		goto err_rule_free;
	}
	field_count = vcap_actionfield_count(vctrl, vtype, actionset_id);

	if (vcap_genl_add_rule_actions(rule, info->attrs[VCAP_NL_ATTR_ACTIONS],
				    fields, field_count)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add rule actions");
		err = -EINVAL;
		goto err_rule_free;
	}

	/* Validate and add default fields */
	if (vcap_val_rule(rule, ETH_P_ALL)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Rule not valid");
		err = -EINVAL;
		goto err_rule_free;
	}

	/* Add the rule to the VCAP */
	if (vcap_add_rule(rule)) {
		NL_SET_ERR_MSG_MOD(info->extack, "Rule could not be added");
		err = -EINVAL;
		goto err_rule_free;
	}

	/* Create the response with the rule id */
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -EINVAL;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_ADD_RULE);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	if (nla_put_u32(msg, VCAP_NL_ATTR_RULE_ID, rule->id))
		goto nla_put_failure;

	vcap_free_rule(rule);
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

err_rule_free:
	vcap_free_rule(rule);

invalid_info:
	return err;
}

static int vcap_genl_get_actionset_info(struct sk_buff *skb,
					struct genl_info *info)
{
	enum vcap_actionfield_set actionset;
	struct sk_buff *msg;
	enum vcap_type vt;
	void *hdr;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_VCAP_TYPE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vt = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_TYPE]);
	if (vctrl->vcaps[vt].rows == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is invalid");
		err = -EINVAL;
		goto invalid_info;
	}
	if (!info->attrs[VCAP_NL_ATTR_ACTIONSET_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute ACTIONSET ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	actionset = nla_get_u16(info->attrs[VCAP_NL_ATTR_ACTIONSET_ID]);

	/* Check that the actionset is valid */
	if (vcap_actionfieldset(vctrl, vt, actionset) == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute ACTIONSET ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_GET_ACTIONSET_INFO);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	if (nla_put_u16(msg, VCAP_NL_ATTR_ACTIONSET_ID, actionset))
		goto nla_put_failure;
	if (nla_put_string(msg, VCAP_NL_ATTR_ACTIONSET_NAME, vctrl->stats->actionfield_set_names[actionset]))
		goto nla_put_failure;
	if (vcap_genl_get_vcap_actionset_actions(vt, actionset, msg))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int vcap_genl_get_keyset_info(struct sk_buff *skb,
				   struct genl_info *info)
{
	enum vcap_keyfield_set keyset;
	struct sk_buff *msg;
	enum vcap_type vt;
	void *hdr;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_VCAP_TYPE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vt = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_TYPE]);
	if (vctrl->vcaps[vt].rows == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is invalid");
		err = -EINVAL;
		goto invalid_info;
	}
	if (!info->attrs[VCAP_NL_ATTR_KEYSET_ID]) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute KEYSET ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	keyset = nla_get_u16(info->attrs[VCAP_NL_ATTR_KEYSET_ID]);

	/* Check that the keyset is valid */
	if (vcap_keyfieldset(vctrl, vt, keyset) == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "Attribute KEYSET ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_GET_KEYSET_INFO);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	if (nla_put_u16(msg, VCAP_NL_ATTR_KEYSET_ID, keyset))
		goto nla_put_failure;
	if (nla_put_string(msg, VCAP_NL_ATTR_KEYSET_NAME, vctrl->stats->keyfield_set_names[keyset]))
		goto nla_put_failure;
	if (vcap_genl_get_vcap_keyset_keys(vt, keyset, msg))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int vcap_genl_get_vcap_info(struct sk_buff *skb,
				   struct genl_info *info)
{
	struct sk_buff *msg;
	enum vcap_type vtype;
	void *hdr;
	int vcount;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_VCAP_TYPE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vtype = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_TYPE]);
	if (vctrl->vcaps[vtype].rows == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is invalid");
		err = -EINVAL;
		goto invalid_info;
	}
	vcount = vcap_admin_type_count(vctrl, vtype);

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_GET_VCAP_INFO);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	if (nla_put_u8(msg, VCAP_NL_ATTR_VCAP_TYPE, vtype))
		goto nla_put_failure;
	if (nla_put_string(msg, VCAP_NL_ATTR_VCAP_NAME, vctrl->vcaps[vtype].name))
		goto nla_put_failure;
	if (nla_put_u8(msg, VCAP_NL_ATTR_VCAP_INSTANCE, vcount))
		goto nla_put_failure;
	if (vcap_genl_get_vcap_keysets(vtype, msg))
		goto nla_put_failure;
	if (vcap_genl_get_vcap_actionsets(vtype, msg))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

int vcap_genl_vcap_printf(void *out, const char *fmt, ...)
{
	struct sk_buff *msg = out;
	char buffer[300];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	return nla_put_string(msg, VCAP_NL_ATTR_VCAP_INFO, buffer);
}

static int vcap_genl_vcap_info(enum vcap_type vtype, int vinst, struct sk_buff *msg)
{
	struct nlattr *start_vcap;
	struct vcap_admin *admin;
	struct vcap_output_print out = {
		.prf = (void *)vcap_genl_vcap_printf,
		.dst = msg,
	};

	list_for_each_entry(admin, &vctrl->list, list) {
		if (admin->vtype == vtype && admin->vinst == vinst) {
			start_vcap = nla_nest_start(msg, VCAP_NL_ATTR_VCAP_INFO_ITEM);
			vcap_show_admin_info(vctrl, admin, &out);
			nla_nest_end(msg, start_vcap);
			break;
		}
	}
	return 0;
}

static int vcap_genl_get_vcap_instance_info(struct sk_buff *skb,
					    struct genl_info *info)
{
	struct sk_buff *msg;
	enum vcap_type vtype;
	void *hdr;
	int vinst;
	int err;

	if (!info->attrs[VCAP_NL_ATTR_VCAP_TYPE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vtype = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_TYPE]);
	if (vctrl->vcaps[vtype].rows == 0) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_ID is invalid");
		err = -EINVAL;
		goto invalid_info;
	}
	if (!info->attrs[VCAP_NL_ATTR_VCAP_INSTANCE]) {
		NL_SET_ERR_MSG_MOD(info->extack, "VCAP_INSTANCE is missing");
		err = -EINVAL;
		goto invalid_info;
	}
	vinst = nla_get_u8(info->attrs[VCAP_NL_ATTR_VCAP_INSTANCE]);

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_GET_VCAP_INSTANCE_INFO);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	if (nla_put_u8(msg, VCAP_NL_ATTR_VCAP_TYPE, vtype))
		goto nla_put_failure;
	if (nla_put_u8(msg, VCAP_NL_ATTR_VCAP_INSTANCE, vinst))
		goto nla_put_failure;
	if (vcap_genl_vcap_info(vtype, vinst, msg))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static int vcap_genl_get_platform_info(struct sk_buff *skb,
				       struct genl_info *info)
{
	struct nlattr *start_vcaps;
	struct sk_buff *msg;
	enum vcap_type vtype;
	void *hdr;
	int err;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not create netlink response");
		err = -ENOMEM;
		goto invalid_info;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vcap_genl_family, 0,
			  VCAP_GENL_CMD_GET_PLATFORM_INFO);
	if (!hdr) {
		NL_SET_ERR_MSG_MOD(info->extack, "Could not add netlink header");
		goto err_msg_free;
	}

	if (nla_put_string(msg, VCAP_NL_ATTR_PLATFORM_NAME, vctrl->stats->name))
		goto nla_put_failure;
	start_vcaps = nla_nest_start(msg, VCAP_NL_ATTR_VCAPS);
	if (!start_vcaps)
		goto nla_put_failure;

	for (vtype = 0; vtype < VCAP_TYPE_MAX; ++vtype) {
		struct nlattr *start_vcap;

		if (vctrl->vcaps[vtype].rows) {
			start_vcap = nla_nest_start(msg, VCAP_NL_ATTR_VCAP_ITEM);
			if (!start_vcap)
				goto nla_put_failure;
			if (nla_put_string(msg, VCAP_NL_ATTR_VCAP_NAME, vctrl->vcaps[vtype].name))
				goto nla_put_failure;
			if (nla_put_u8(msg, VCAP_NL_ATTR_VCAP_TYPE, vtype))
				goto nla_put_failure;
			nla_nest_end(msg, start_vcap);
		}
	}

	nla_nest_end(msg, start_vcaps);

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

nla_put_failure:
	NL_SET_ERR_MSG_MOD(info->extack, "Could not add field to response");
	genlmsg_cancel(msg, hdr);

err_msg_free:
	err = -EMSGSIZE;
	nlmsg_free(msg);

invalid_info:
	return err;
}

static struct genl_ops vcap_genl_ops[] = {
	{
		.cmd      = VCAP_GENL_CMD_GET_PLATFORM_INFO,
		.doit     = vcap_genl_get_platform_info,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_GET_VCAP_INFO,
		.doit     = vcap_genl_get_vcap_info,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_GET_VCAP_INSTANCE_INFO,
		.doit     = vcap_genl_get_vcap_instance_info,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_GET_KEYSET_INFO,
		.doit     = vcap_genl_get_keyset_info,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_GET_ACTIONSET_INFO,
		.doit     = vcap_genl_get_actionset_info,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_ADD_RULE,
		.doit     = vcap_genl_add_rule,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_DEL_RULE,
		.doit     = vcap_genl_del_rule,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_GET_RULE,
		.doit     = vcap_genl_get_rule,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_MOD_RULE,
		.doit     = vcap_genl_mod_rule,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_LIST_RULES,
		.doit     = vcap_genl_list_rules,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_RESET_RULE_COUNTER,
		.doit     = vcap_genl_reset_rule_counter,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	}, {
		.cmd      = VCAP_GENL_CMD_GET_PORT_INFO,
		.doit     = vcap_genl_get_port_info,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags    = GENL_ADMIN_PERM,
	},
};

static struct genl_family vcap_genl_family = {
	.name		= VCAP_NETLINK_NAME,
	.hdrsize	= 0,
	.version	= VCAP_NETLINK_VERSION,
	.maxattr	= VCAP_NL_ATTR_MAX,
	.policy		= vcap_genl_policy,
	.ops		= vcap_genl_ops,
	.n_ops		= ARRAY_SIZE(vcap_genl_ops),
	.resv_start_op	= VCAP_GENL_CMD_GET_PORT_INFO + 1,
};

int vcap_netlink_init(struct vcap_control *ctrl, struct net_device *ndev)
{
	int err;

	vctrl = ctrl;
	priv_ndev = ndev;
	err = genl_register_family(&vcap_genl_family);
	if (err)
		pr_err("genl_register_family() failed\n");

	return err;
}

void vcap_netlink_uninit(void)
{
	vctrl = NULL;
	priv_ndev = NULL;
	genl_unregister_family(&vcap_genl_family);
}
