/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip VCAP API
 *
 * Copyright (c) 2022 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/gfp.h>
#include <linux/slab.h>

#ifdef CONFIG_VCAP_KUNIT_TEST
#include "vcap_api_kunit.h"
#endif
#include "vcap_api.h"
#include "vcap_api_client.h"

#define VCAP_ALIGN 32

static struct vcap_control *vctrl;

struct vcap_rule_internal {
	struct vcap_rule data; /* provided by the client */
	struct list_head list; /* for insertion in the vcap admin list of rules */
	struct vcap_admin *admin;
	struct net_device *ndev;  /* the interface that the rule applies to */
	u32 sort_key;  /* defines the position in the VCAP */
	int keyset_sw;  /* subwords in a keyset */
	int actionset_sw;  /* subwords in an actionset */
	int keyset_sw_regs;  /* registers in a subword in an keyset */
	int actionset_sw_regs;  /* registers in a subword in an actionset */
	int size; /* the size of the rule: max(entry, action) */
	u32 addr; /* address in the VCAP at insertion */
	u32 counter_id; /* counter id (if a dedicated counter is available) */
	struct vcap_counter counter; /* last read counter value */
};

struct vcap_rule_move {
	int addr; /* address to move */
	int offset; /* change in address */
	int count; /* blocksize of addresses to move */
};

struct vcap_stream_iter {
	u32 offset;
	u32 sw_width;
	u32 regs_per_sw;
	u32 reg_idx;
	u32 reg_bitpos;
	const struct vcap_typegroup *tg;
};

static int keyfield_size_table[] = {
	[VCAP_FIELD_BIT]  = sizeof(struct vcap_u1_key),
	[VCAP_FIELD_U32]  = sizeof(struct vcap_u32_key),
	[VCAP_FIELD_U48]  = sizeof(struct vcap_u48_key),
	[VCAP_FIELD_U56]  = sizeof(struct vcap_u56_key),
	[VCAP_FIELD_U64]  = sizeof(struct vcap_u64_key),
	[VCAP_FIELD_U72]  = sizeof(struct vcap_u72_key),
	[VCAP_FIELD_U112] = sizeof(struct vcap_u112_key),
	[VCAP_FIELD_U128] = sizeof(struct vcap_u128_key),
};

static int actionfield_size_table[] = {
	[VCAP_FIELD_BIT]  = sizeof(struct vcap_u1_action),
	[VCAP_FIELD_U32]  = sizeof(struct vcap_u32_action),
	[VCAP_FIELD_U48]  = sizeof(struct vcap_u48_action),
	[VCAP_FIELD_U56]  = sizeof(struct vcap_u56_action),
	[VCAP_FIELD_U64]  = sizeof(struct vcap_u64_action),
	[VCAP_FIELD_U72]  = sizeof(struct vcap_u72_action),
	[VCAP_FIELD_U112] = sizeof(struct vcap_u112_action),
	[VCAP_FIELD_U128] = sizeof(struct vcap_u128_action),
};

static int vcap_api_check(struct vcap_control *ctrl)
{
	if (!ctrl) {
		pr_err("%s:%d: vcap control is missing\n", __func__, __LINE__);
		return -EINVAL;
	}
	if (!ctrl->ops || !ctrl->ops->validate_keyset ||
	    !ctrl->ops->add_default_fields || !ctrl->ops->cache_erase ||
	    !ctrl->ops->cache_write || !ctrl->ops->cache_read ||
	    !ctrl->ops->init || !ctrl->ops->update || !ctrl->ops->move ||
	    !ctrl->ops->port_info) {
		pr_err("%s:%d: client operations are missing\n",
		       __func__, __LINE__);
		return -ENOENT;
	}
	return 0;
}

/* client interface */
int vcap_api_set_client(struct vcap_control *ctrl)
{
	int ret = vcap_api_check(ctrl);

	if (ret)
		return ret;
	vctrl = ctrl;
	return ret;

}

static void vcap_erase_cache(struct vcap_rule_internal *ri)
{
	vctrl->ops->cache_erase(ri->admin);
}

static bool vcap_get_bit(u32 *stream, struct vcap_stream_iter *itr)
{
	u32 mask = BIT(itr->reg_bitpos);
	u32 *p = &stream[itr->reg_idx];
	return !!(*p & mask);
}

static void vcap_set_bit(u32 *stream, struct vcap_stream_iter *itr, bool value)
{
	u32 mask = BIT(itr->reg_bitpos);
	u32 *p = &stream[itr->reg_idx];
	if (value)
		*p |= mask;
	else
		*p &= ~mask;
}

static void vcap_iter_update(struct vcap_stream_iter *itr)
{
	int sw_idx, sw_bitpos;

	/* Calculate the subword index and bitposition for current bit */
	sw_idx = itr->offset / itr->sw_width;
	sw_bitpos = itr->offset % itr->sw_width;
	/* Calculate the register index and bitposition for current bit */
	itr->reg_idx = (sw_idx * itr->regs_per_sw) + (sw_bitpos / 32);
	itr->reg_bitpos = sw_bitpos % 32;
}

static void vcap_iter_skip_tg(struct vcap_stream_iter *itr)
{
	/* Compensate the field offset for preceeding typegroups */
	while (itr->tg->width && itr->offset >= itr->tg->offset) {
		itr->offset += itr->tg->width;
		itr->tg++; /* next typegroup */
	}
}

static void vcap_iter_next(struct vcap_stream_iter *itr)
{
	itr->offset++;
	vcap_iter_skip_tg(itr);
	vcap_iter_update(itr);
}

static void vcap_iter_set(struct vcap_stream_iter *itr, int sw_width,
			  const struct vcap_typegroup *tg, u32 offset)
{
	memset(itr, 0, sizeof(*itr));
	itr->offset = offset;
	itr->sw_width = sw_width;
	itr->regs_per_sw = DIV_ROUND_UP(sw_width, 32);
	itr->tg = tg;
}

static void vcap_iter_init(struct vcap_stream_iter *itr, int sw_width,
			   const struct vcap_typegroup *tg, u32 offset)
{
	vcap_iter_set(itr, sw_width, tg, offset);
	vcap_iter_skip_tg(itr);
	vcap_iter_update(itr);
}

static void vcap_encode_bit(u32 *stream, struct vcap_stream_iter *itr, bool val)
{
	/* When intersected by a type group field, stream the type group bits
	 * before continuing with the value bit
	 */
	while (itr->tg->width &&
	       (itr->offset >= itr->tg->offset &&
	        itr->offset < itr->tg->offset + itr->tg->width)) {
		int tg_bitpos = itr->tg->offset - itr->offset;

		vcap_set_bit(stream, itr, (itr->tg->value >> tg_bitpos) & 0x1);
		itr->offset++;
		vcap_iter_update(itr);
	}
	vcap_set_bit((u32 *)stream, itr, val);
}

static void vcap_encode_typegroups(u32 *stream, int sw_width,
				   const struct vcap_typegroup *tg,
				   bool mask)
{
	struct vcap_stream_iter iter;
	int idx;

	/* Mask bits must be set to zeros (inverted later when writing to the
	 * mask cache register), so that the mask typegroup bits consist of
	 * match-1 or match-0, or both
	 */
	vcap_iter_set(&iter, sw_width, tg, 0);
	while (iter.tg->width) {
		/* Set position to current typegroup bit */
		iter.offset = iter.tg->offset;
		vcap_iter_update(&iter);
		for (idx = 0; idx < iter.tg->width; idx++) {
			/* Iterate over current typegroup bits. Mask typegroup
			 * bits are always set */
			if (mask)
				vcap_set_bit(stream, &iter, 0x1);
			else
				vcap_set_bit(stream, &iter,
					     (iter.tg->value >> idx) & 0x1);
			iter.offset++;
			vcap_iter_update(&iter);
		}
		iter.tg++; /* next typegroup */
	}
}

static void vcap_decode_field(u32 *stream, struct vcap_stream_iter *itr,
			      int width, u8 *value)
{
	int idx;

	/* Loop over the field value bits and get the field bits and
	 * set them in the output value byte array
	 */
	for (idx = 0; idx < width; idx++) {
		u8 bidx = idx & 0x7;

		/* Decode one field value bit */
		if (vcap_get_bit(stream, itr))
			*value |= 1 << bidx;
		vcap_iter_next(itr);
		if (bidx == 7)
			value++;
	}
}

static void vcap_encode_field(u32 *stream, struct vcap_stream_iter *itr,
			      int width, const u8 *value)
{
	int idx;

	/* Loop over the field value bits and add the value bits one by one to
	 * the output stream.
	 */
	for (idx = 0; idx < width; idx++) {
		u8 bidx = idx & 0x7;

		/* Encode one field value bit */
		vcap_encode_bit(stream, itr, (value[idx / 8] >> bidx) & 0x1);
		vcap_iter_next(itr);
	}
}

static void vcap_encode_keyfield(struct vcap_rule_internal *ri,
			      const struct vcap_client_keyfield *kf,
			      const struct vcap_field *rf,
			      const struct vcap_typegroup *tgt)
{
	struct vcap_stream_iter iter;
	int sw_width = vctrl->vcaps[ri->admin->vtype].sw_width;
	struct vcap_cache_data *cache = &ri->admin->cache;
	const u8 *value, *mask;

	/* Encode the fields for the key and the mask in their respective
	 * streams, respecting the subword width.
	 */
	switch (kf->ctrl.type) {
	case VCAP_FIELD_BIT:
		value = &kf->data.u1.value;
		mask = &kf->data.u1.mask;
		break;
	case VCAP_FIELD_U32:
		value = (const u8 *)&kf->data.u32.value;
		mask = (const u8 *)&kf->data.u32.mask;
		break;
	case VCAP_FIELD_U48:
		value = kf->data.u48.value;
		mask = kf->data.u48.mask;
		break;
	case VCAP_FIELD_U56:
		value = kf->data.u56.value;
		mask = kf->data.u56.mask;
		break;
	case VCAP_FIELD_U64:
		value = kf->data.u64.value;
		mask = kf->data.u64.mask;
		break;
	case VCAP_FIELD_U72:
		value = kf->data.u72.value;
		mask = kf->data.u72.mask;
		break;
	case VCAP_FIELD_U112:
		value = kf->data.u112.value;
		mask = kf->data.u112.mask;
		break;
	case VCAP_FIELD_U128:
		value = kf->data.u128.value;
		mask = kf->data.u128.mask;
		break;
	}
	vcap_iter_init(&iter, sw_width, tgt, rf->offset);
	vcap_encode_field(cache->keystream, &iter, rf->width, value);
	vcap_iter_init(&iter, sw_width, tgt, rf->offset);
	vcap_encode_field(cache->maskstream, &iter, rf->width, mask);
}

static void vcap_encode_keyfield_typegroups(struct vcap_rule_internal *ri,
					 const struct vcap_typegroup *tgt)
{
	struct vcap_cache_data *cache = &ri->admin->cache;
	int sw_width = vctrl->vcaps[ri->admin->vtype].sw_width;

	/* Encode the typegroup bits for the key and the mask in their streams,
	 * respecting the subword width.
	 */
	vcap_encode_typegroups(cache->keystream, sw_width, tgt, false);
	vcap_encode_typegroups(cache->maskstream, sw_width, tgt, true);
}

static void vcap_encode_actionfield(struct vcap_rule_internal *ri,
			       const struct vcap_client_actionfield *af,
			       const struct vcap_field *rf,
			       const struct vcap_typegroup *tgt)
{
	struct vcap_cache_data *cache = &ri->admin->cache;
	int act_width = vctrl->vcaps[ri->admin->vtype].act_width;
	struct vcap_stream_iter iter;
	const u8 *value;

	/* Encode the action field in the stream, respecting the subword width */
	switch (af->ctrl.type) {
	case VCAP_FIELD_BIT:
		value = &af->data.u1.value;
		break;
	case VCAP_FIELD_U32:
		value = (const u8 *)&af->data.u32.value;
		break;
	case VCAP_FIELD_U48:
		value = af->data.u48.value;
		break;
	case VCAP_FIELD_U56:
		value = af->data.u56.value;
		break;
	case VCAP_FIELD_U64:
		value = af->data.u64.value;
		break;
	case VCAP_FIELD_U72:
		value = af->data.u72.value;
		break;
	case VCAP_FIELD_U112:
		value = af->data.u112.value;
		break;
	case VCAP_FIELD_U128:
		value = af->data.u128.value;
		break;
	}
	vcap_iter_init(&iter, act_width, tgt, rf->offset);
	vcap_encode_field(cache->actionstream, &iter, rf->width, value);
}

static void vcap_encode_actionfield_typegroups(struct vcap_rule_internal *ri,
					  const struct vcap_typegroup *tgt)
{
	struct vcap_cache_data *cache = &ri->admin->cache;
	int sw_width = vctrl->vcaps[ri->admin->vtype].act_width;

	/* Encode the typegroup bits for the actionstream respecting the subword
	 * width.
	 */
	vcap_encode_typegroups(cache->actionstream, sw_width, tgt, false);
}

/* Get a vcap instance from a rule */
struct vcap_admin *vcap_rule_get_admin(struct vcap_rule *rule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;

	return ri->admin;
}

/* Return the number of keyfields in the keyset */
int vcap_keyfield_count(enum vcap_type vt, enum vcap_keyfield_set keyset)
{
	/* Check that the keyset exists in the vcap keyset list */
	if (keyset >= vctrl->vcaps[vt].keyfield_set_size)
		return 0;
	return vctrl->vcaps[vt].keyfield_set_map_size[keyset];
}

/* Return the list of keyfields for the keyset */
const struct vcap_field *vcap_keyfields(enum vcap_type vt,
					enum vcap_keyfield_set keyset)
{
	/* Check that the keyset exists in the vcap keyset list */
	if (keyset >= vctrl->vcaps[vt].keyfield_set_size)
		return 0;
	return vctrl->vcaps[vt].keyfield_set_map[keyset];
}

/* Return the keyset information for the keyset */
const struct vcap_set *vcap_keyfieldset(enum vcap_type vt,
					enum vcap_keyfield_set keyset)
{
	const struct vcap_set *kset;

	/* Check that the keyset exists in the vcap keyset list */
	if (keyset >= vctrl->vcaps[vt].keyfield_set_size)
		return 0;
	kset = &vctrl->vcaps[vt].keyfield_set[keyset];
	if (kset->sw_per_item == 0 || kset->sw_per_item > vctrl->vcaps[vt].sw_count)
		return 0;
	return kset;
}

/* Return the typegroup table for the matching keyset (using subword size) */
static const struct vcap_typegroup *vcap_keyfield_typegroup(enum vcap_type vt,
							 enum vcap_keyfield_set keyset)
{
	const struct vcap_set *kset = vcap_keyfieldset(vt, keyset);

	/* Check that the keyset is valid */
	if (!kset)
		return 0;
	return vctrl->vcaps[vt].keyfield_set_typegroups[kset->sw_per_item];
}

/* Return the number of actionfields in the actionset */
int vcap_actionfield_count(enum vcap_type vt,
			   enum vcap_actionfield_set actionset)
{
	/* Check that the actionset exists in the vcap actionset list */
	if (actionset >= vctrl->vcaps[vt].actionfield_set_size)
		return 0;
	return vctrl->vcaps[vt].actionfield_set_map_size[actionset];
}

/* Return the list of actionfields for the actionset */
const struct vcap_field *vcap_actionfields(enum vcap_type vt,
					   enum vcap_actionfield_set actionset)
{
	/* Check that the actionset exists in the vcap actionset list */
	if (actionset >= vctrl->vcaps[vt].actionfield_set_size)
		return 0;
	return vctrl->vcaps[vt].actionfield_set_map[actionset];
}

/* Return the actionset information for the actionset */
const struct vcap_set *vcap_actionfieldset(enum vcap_type vt,
					   enum vcap_actionfield_set actionset)
{
	const struct vcap_set *aset;

	/* Check that the actionset exists in the vcap actionset list */
	if (actionset >= vctrl->vcaps[vt].actionfield_set_size)
		return 0;
	aset = &vctrl->vcaps[vt].actionfield_set[actionset];
	if (aset->sw_per_item == 0 || aset->sw_per_item > vctrl->vcaps[vt].sw_count)
		return 0;
	return aset;
}

/* Return the typegroup table for the matching actionset (using subword size) */
static const struct vcap_typegroup *vcap_actionfield_typegroup(enum vcap_type vt,
							       enum vcap_actionfield_set actionset)
{
	const struct vcap_set *aset = vcap_actionfieldset(vt, actionset);

	/* Check that the actionset is valid */
	if (!aset)
		return 0;
	return vctrl->vcaps[vt].actionfield_set_typegroups[aset->sw_per_item];
}

/* Verify that the typegroup bits have the correct values */
static int vcap_verify_typegroups(u32 *stream, int sw_width,
				  const struct vcap_typegroup *tgt, bool mask,
				  int sw_max)
{
	struct vcap_stream_iter iter;
	int sw_cnt, idx;

	vcap_iter_set(&iter, sw_width, tgt, 0);
	sw_cnt = 0;
	while (iter.tg->width) {
		u32 value = 0;
		u32 tg_value = iter.tg->value;

		if (mask)
			tg_value = (1 << iter.tg->width) - 1;
		/* Set position to current typegroup bit */
		iter.offset = iter.tg->offset;
		vcap_iter_update(&iter);
		for (idx = 0; idx < iter.tg->width; idx++) {
			/* Decode one typegroup bit */
			if (vcap_get_bit(stream, &iter))
				value |= 1 << idx;
			iter.offset++;
			vcap_iter_update(&iter);
		}
		if (value != tg_value)
			return -EINVAL;
		iter.tg++; /* next typegroup */
		sw_cnt++;
		/* Stop checking more typegroups */
		if (sw_max && sw_cnt >= sw_max)
			break;
	}
	return 0;
}

/* Find the subword width of the key typegroup that matches the stream data */
static int vcap_find_keystream_typegroup_sw(enum vcap_type vt, u32 *stream,
					    bool mask, int sw_max)
{
	const struct vcap_typegroup **tgt;
	int sw_idx, res;

	tgt = vctrl->vcaps[vt].keyfield_set_typegroups;
	/* Try the longest subword match first */
	for (sw_idx = vctrl->vcaps[vt].sw_count; sw_idx >= 0; sw_idx--) {
		if (tgt[sw_idx]) {
			res = vcap_verify_typegroups(stream, vctrl->vcaps[vt].sw_width,
						     tgt[sw_idx], mask, sw_max);
			if (res == 0)
				return sw_idx;
		}
	}
	return -EINVAL;
}

static bool vcap_bitarray_zero(int width, u8 *value)
{
	int idx;
	int max = DIV_ROUND_UP(width, 8);
	int rwidth = width;
	u8 total = 0;
	u8 bmask = 0xff;

	for (idx = 0; idx < max; ++idx, rwidth -= 8) {
		if (rwidth && rwidth < 8)
			bmask = (1 << rwidth) - 1;
		total += value[idx] & bmask;
	}
	return total == 0;
}

/* Verify that the type id in the stream matches the type id of the keyset */
static bool vcap_verify_keystream_keyset(enum vcap_type vt,
					 u32 *keystream,
					 u32 *mskstream,
					 enum vcap_keyfield_set keyset)
{
	const struct vcap_info *vcap = &vctrl->vcaps[vt];
	const struct vcap_field *typefld;
	const struct vcap_field *fields;
	const struct vcap_typegroup *tgt;
	struct vcap_stream_iter iter;
	const struct vcap_set *info;
	u32 value = 0;
	u32 mask = 0;

	if (vcap_keyfield_count(vt, keyset) == 0)
		return false;
	info = vcap_keyfieldset(vt, keyset);
	/* Check that the keyset is valid */
	if (!info)
		return false;
	/* a type_id of value -1 means that there is no type field */
	if (info->type_id == (u8)-1)
		return true;
	/* Get a valid typegroup for the specific keyset */
	tgt = vcap_keyfield_typegroup(vt, keyset);
	if (tgt == 0)
		return 0;
	fields = vcap_keyfields(vt, keyset);
	if (!fields)
		return false;
	typefld = &fields[VCAP_KF_TYPE];
	vcap_iter_init(&iter, vcap->sw_width, tgt, typefld->offset);
	vcap_decode_field(mskstream, &iter, typefld->width, (u8 *)&mask);
	/* no type info if there are no mask bits */
	if (vcap_bitarray_zero(typefld->width, (u8 *)&mask))
		return false;
	/* Get the value of the type field in the stream and compare to the
	 * one define in the vcap keyset
	 */
	vcap_iter_init(&iter, vcap->sw_width, tgt, typefld->offset);
	vcap_decode_field(keystream, &iter, typefld->width, (u8 *)&value);
	return (value == info->type_id);
}

/* Verify that the typegroup information, subword count, keyset and type id
 * are in sync and correct, return the keyset */
static enum vcap_keyfield_set vcap_find_keystream_keyset(enum vcap_type vt,
							 u32 *keystream,
							 u32 *mskstream,
							 bool mask, int sw_max)
{
	int sw_count = vcap_find_keystream_typegroup_sw(vt, keystream, mask,
							sw_max);
	const struct vcap_set *keyfield_set = vctrl->vcaps[vt].keyfield_set;
	bool res;
	int idx;

	if (sw_count < 0)
		return sw_count;

	for (idx = 0; idx < vctrl->vcaps[vt].keyfield_set_size; ++idx) {
		if (keyfield_set[idx].sw_per_item == sw_count) {
			res = vcap_verify_keystream_keyset(vt, keystream,
							   mskstream, idx);
			if (res)
				return idx;
		}
	}
	return -EINVAL;
}

/* Find the subword width of the action typegroup that matches the stream data */
static int vcap_find_actionstream_typegroup_sw(enum vcap_type vt, u32 *stream,
					       int sw_max)
{
	const struct vcap_typegroup **tgt;
	int sw_idx, res;

	tgt = vctrl->vcaps[vt].actionfield_set_typegroups;
	/* Try the longest subword match first */
	for (sw_idx = vctrl->vcaps[vt].sw_count; sw_idx >= 0; sw_idx--) {
		if (tgt[sw_idx]) {
			res = vcap_verify_typegroups(stream, vctrl->vcaps[vt].act_width,
						     tgt[sw_idx], false, sw_max);
			if (res == 0)
				return sw_idx;
		}
	}
	return -EINVAL;
}

/* Verify that the type id in the stream matches the type id of the actionset */
static bool vcap_verify_actionstream_actionset(enum vcap_type vt, u32 *stream,
					 enum vcap_actionfield_set actionset)
{
	const struct vcap_info *vcap = &vctrl->vcaps[vt];
	const struct vcap_field *typefld;
	const struct vcap_field *fields;
	const struct vcap_typegroup *tgt;
	struct vcap_stream_iter iter;
	const struct vcap_set *info;
	u32 value = 0;

	if (vcap_actionfield_count(vt, actionset) == 0)
		return false;
	info = vcap_actionfieldset(vt, actionset);
	/* Check that the actionset is valid */
	if (!info)
		return false;
	/* a type_id of value -1 means that there is no type field */
	if (info->type_id == (u8)-1)
		return true;
	/* Get a valid typegroup for the specific actionset */
	tgt = vcap_actionfield_typegroup(vt, actionset);
	if (tgt == 0)
		return 0;
	fields = vcap_actionfields(vt, actionset);
	if (!fields)
		return false;
	/* Get the value of the type field in the stream and compare to the
	 * one define in the vcap actionset
	 */
	typefld = &fields[VCAP_AF_TYPE];
	vcap_iter_init(&iter, vcap->act_width, tgt, typefld->offset);
	vcap_decode_field(stream, &iter, typefld->width, (u8 *)&value);
	return (value == info->type_id);
}

/* Verify that the typegroup information, subword count, keyset and type id
 * are in sync and correct, return the actionset */
static enum vcap_actionfield_set vcap_find_actionstream_actionset(enum vcap_type vt,
							       u32 *stream,
							       int sw_max)
{
	int sw_count = vcap_find_actionstream_typegroup_sw(vt, stream, sw_max);
	const struct vcap_set *actionfield_set = vctrl->vcaps[vt].actionfield_set;
	bool res;
	int idx;

	if (sw_count < 0)
		return sw_count;

	for (idx = 0; idx < vctrl->vcaps[vt].actionfield_set_size; ++idx) {
		if (actionfield_set[idx].sw_per_item == sw_count) {
			res = vcap_verify_actionstream_actionset(vt, stream, idx);
			if (res)
				return idx;
		}
	}
	return -EINVAL;
}

static int vcap_encode_rule_keyset(struct vcap_rule_internal *ri)
{
	const struct vcap_client_keyfield *ckf;
	const struct vcap_typegroup *tg_table;
	const struct vcap_field *kf_table;
	int keyset_size;

	/* Get a valid set of fields for the specific keyset */
	kf_table = vcap_keyfields(ri->admin->vtype, ri->data.keyset);
	if (kf_table == 0) {
		pr_err("%s:%d: no fields available for this keyset: %d\n",
		       __func__, __LINE__, ri->data.keyset);
		return -EINVAL;
	}
	/* Get a valid typegroup for the specific keyset */
	tg_table = vcap_keyfield_typegroup(ri->admin->vtype, ri->data.keyset);
	if (tg_table == 0) {
		pr_err("%s:%d: no typegroups available for this keyset: %d\n",
		       __func__, __LINE__, ri->data.keyset);
		return -EINVAL;
	}
	/* Get a valid size for the specific keyset */
	keyset_size = vcap_keyfield_count(ri->admin->vtype, ri->data.keyset);
	if (keyset_size == 0) {
		pr_err("%s:%d: zero field count for this keyset: %d\n",
		       __func__, __LINE__, ri->data.keyset);
		pr_err("%s:%d\n", __func__, __LINE__);
		return -EINVAL;
	}
	/* Iterate over the keyfields (key, mask) in the rule
	 * and encode these bits
	 */
	if (list_empty(&ri->data.keyfields)) {
		pr_err("%s:%d: no keyfields in the rule\n", __func__, __LINE__);
		return -EINVAL;
	}
	list_for_each_entry(ckf, &ri->data.keyfields, ctrl.list) {
		/* Check that the client entry exists in the keyset */
		if (ckf->ctrl.key >= keyset_size) {
			pr_err("%s:%d: key %d is not in vcap\n",
			       __func__, __LINE__, ckf->ctrl.key);
			return -EINVAL;
		}
		vcap_encode_keyfield(ri, ckf, &(kf_table[ckf->ctrl.key]), tg_table);
	}
	/* Add typegroup bits to the key/mask bitstreams */
	vcap_encode_keyfield_typegroups(ri, tg_table);
	return 0;
}

static int vcap_encode_rule_actionset(struct vcap_rule_internal *ri)
{
	const struct vcap_client_actionfield *caf;
	const struct vcap_typegroup *tg_table;
	const struct vcap_field *af_table;
	int actionset_size;

	/* Get a valid set of actionset fields for the specific actionset */
	af_table = vcap_actionfields(ri->admin->vtype, ri->data.actionset);
	if (af_table == 0) {
		pr_err("%s:%d: no fields available for this actionset: %d\n",
		       __func__, __LINE__, ri->data.actionset);
		return -EINVAL;
	}
	/* Get a valid typegroup for the specific actionset */
	tg_table = vcap_actionfield_typegroup(ri->admin->vtype, ri->data.actionset);
	if (tg_table == 0) {
		pr_err("%s:%d: no typegroups available for this actionset: %d\n",
		       __func__, __LINE__, ri->data.actionset);
		return -EINVAL;
	}
	/* Get a valid actionset size for the specific actionset */
	actionset_size = vcap_actionfield_count(ri->admin->vtype, ri->data.actionset);
	if (actionset_size == 0) {
		pr_err("%s:%d: zero field count for this actionset: %d\n",
		       __func__, __LINE__, ri->data.actionset);
		return -EINVAL;
	}
	/* Iterate over the actionfields in the rule
	 * and encode these bits
	 */
	if (list_empty(&ri->data.actionfields)) {
		pr_warn("%s:%d: no actionfields in the rule\n", __func__, __LINE__);
	}
	list_for_each_entry(caf, &ri->data.actionfields, ctrl.list) {
		/* Check that the client action exists in the actionset */
		if (caf->ctrl.action >= actionset_size) {
			pr_err("%s:%d: action %d is not in vcap\n",
			       __func__, __LINE__, caf->ctrl.action);
			return -EINVAL;
		}
		vcap_encode_actionfield(ri, caf, &(af_table[caf->ctrl.action]),
					tg_table);
	}
	/* Add typegroup bits to the entry bitstreams */
	vcap_encode_actionfield_typegroups(ri, tg_table);
	return 0;
}

static int vcap_encode_rule(struct vcap_rule_internal *ri)
{
	int err;

	err = vcap_encode_rule_keyset(ri);
	if (err)
		return err;
	err = vcap_encode_rule_actionset(ri);
	if (err)
		return err;
	/* Iterate over counters, to reset them */
	return 0;
}

static char *vcap_bitarray_tostring(char *buffer, int width, u8 *value)
{
	int idx, cidx = 0;
	value += DIV_ROUND_UP(width, 8) - 1;

	for (idx = width - 1; idx >= 0; --idx, cidx++) {
		u8 bidx = idx & 0x7;

		buffer[cidx] = '0';
		if ((*value >> bidx) & 0x1)
			buffer[cidx] = '1';
		if (idx && ((idx & 0x3) == 0))
			buffer[++cidx] = '.';
		if (bidx == 0)
			--value;
	}
	buffer[cidx] = 0;
	return buffer + cidx;
}

static void vcap_apply_width(u8 *dst, int width, int bytes)
{
	u8 bmask;
	int idx;

	for (idx = 0; idx < bytes; idx++) {
		if (width > 0)
			if (width < 8)
				bmask = (1 << width) - 1;
			else
				bmask = ~0;
		else
			bmask = 0;
		dst[idx] &= bmask;
		width -= 8;
	}
}

/*
 * This is the transformation shown with a 16 byte value
 * 1514:1312:1110:0908:0706:0504:0302:0100    1514:1312:1110:0908:0706:0504:0302:0100
 * ff  :    :    :    :    :    :    :     ->     :    :    :    :    :    :ff  :
 *   ff:    :    :    :    :    :    :     ->     :    :    :    :    :    :  ff:
 *     :ff  :    :    :    :    :    :     ->     :    :    :    :    :    :    :ff
 *     :  ff:    :    :    :    :    :     ->     :    :    :    :    :    :    :  ff
 *     :    :ff  :    :    :    :    :     ->     :    :    :    :ff  :    :    :
 *     :    :  ff:    :    :    :    :     ->     :    :    :    :  ff:    :    :
 *     :    :    :ff  :    :    :    :     ->     :    :    :    :    :ff  :    :
 *     :    :    :  ff:    :    :    :     ->     :    :    :    :    :  ff:    :
 *     :    :    :    :ff  :    :    :     ->     :    :ff  :    :    :    :    :
 *     :    :    :    :  ff:    :    :     ->     :    :  ff:    :    :    :    :
 *     :    :    :    :    :ff  :    :     ->     :    :    :ff  :    :    :    :
 *     :    :    :    :    :  ff:    :     ->     :    :    :  ff:    :    :    :
 *     :    :    :    :    :    :ff  :     -> ff  :    :    :    :    :    :    :
 *     :    :    :    :    :    :  ff:     ->   ff:    :    :    :    :    :    :
 *     :    :    :    :    :    :    :ff   ->     :ff  :    :    :    :    :    :
 *     :    :    :    :    :    :    :  ff ->     :  ff:    :    :    :    :    :
 */
static void vcap_copy_to_w32be(uint8_t *dst, uint8_t *src, int size)
{
	int idx, nidx;
	int first_byte_index = 0;

	for (idx = 0; idx < size; ++idx) {
		first_byte_index = size - (((idx >> 2) + 1) << 2);
		if (first_byte_index < 0)
			first_byte_index = 0;
		nidx = idx + first_byte_index - (idx & ~0x3);
		dst[nidx] = src[idx];
	}
}

static void vcap_copy_from_w32be(uint8_t *dst, uint8_t *src, int size, int width)
{
	int idx, ridx, wstart, nidx;
	int tail_bytes = (((size + 4) >> 2) << 2) - size;

	for (idx = 0, ridx = size - 1; idx < size; ++idx, --ridx) {
		wstart = (idx >> 2) << 2;
		nidx = wstart + 3 - (idx & 0x3);
		if (nidx >= size)
			nidx -= tail_bytes;
		dst[nidx] = src[ridx];
	}
	vcap_apply_width(dst, width, size);
}

static void vcap_copy_key_bit_field(struct vcap_u1_key *field,
				    u8 *value, u8 *mask)
{
	field->value = (*value) & 0x1;
	field->mask = (*mask) & 0x1;
}

static void vcap_copy_limited_keyfield(u8 *dstvalue, u8 *dstmask,
				       u8 *srcvalue, u8 *srcmask,
				       int width, int bytes)
{
	memcpy(dstvalue, srcvalue, bytes);
	vcap_apply_width(dstvalue, width, bytes);
	memcpy(dstmask, srcmask, bytes);
	vcap_apply_width(dstmask, width, bytes);
}

static void vcap_copy_to_client_keyfield(struct vcap_rule_internal *ri,
					 struct vcap_client_keyfield *field,
					 u8 *value, u8 *mask, u16 width)
{
	int field_size = keyfield_size_table[field->ctrl.type] / 2;
	if (ri->admin->w32be) {
		switch (field->ctrl.type) {
		case VCAP_FIELD_BIT:
			vcap_copy_key_bit_field(&field->data.u1, value, mask);
			break;
		case VCAP_FIELD_U32:
			vcap_copy_limited_keyfield((u8 *)&field->data.u32.value,
						   (u8 *)&field->data.u32.mask,
						   value, mask,
						   width, field_size);
			break;
		case VCAP_FIELD_U48:
			vcap_copy_from_w32be(field->data.u48.value, value,
					     field_size, width);
			vcap_copy_from_w32be(field->data.u48.mask,  mask,
					     field_size, width);
			break;
		case VCAP_FIELD_U56:
			vcap_copy_from_w32be(field->data.u56.value, value,
					     field_size, width);
			vcap_copy_from_w32be(field->data.u56.mask,  mask,
					     field_size, width);
			break;
		case VCAP_FIELD_U64:
			vcap_copy_from_w32be(field->data.u64.value, value,
					     field_size, width);
			vcap_copy_from_w32be(field->data.u64.mask,  mask,
					     field_size, width);
			break;
		case VCAP_FIELD_U72:
			vcap_copy_from_w32be(field->data.u72.value, value,
					     field_size, width);
			vcap_copy_from_w32be(field->data.u72.mask,  mask,
					     field_size, width);
			break;
		case VCAP_FIELD_U112:
			vcap_copy_from_w32be(field->data.u112.value, value,
					     field_size, width);
			vcap_copy_from_w32be(field->data.u112.mask,  mask,
					     field_size, width);
			break;
		case VCAP_FIELD_U128:
			vcap_copy_from_w32be(field->data.u128.value, value,
					     field_size, width);
			vcap_copy_from_w32be(field->data.u128.mask,  mask,
					     field_size, width);
			break;
		};
	} else {
		switch (field->ctrl.type) {
		case VCAP_FIELD_BIT:
			vcap_copy_key_bit_field(&field->data.u1, value, mask);
			break;
		case VCAP_FIELD_U32:
			vcap_copy_limited_keyfield((u8 *)&field->data.u32.value,
						   (u8 *)&field->data.u32.mask,
						   value, mask,
						   width, field_size);
			break;
		case VCAP_FIELD_U48:
			vcap_copy_limited_keyfield(field->data.u48.value,
						   field->data.u48.mask,
						   value, mask,
						   width, field_size);
			break;
		case VCAP_FIELD_U56:
			vcap_copy_limited_keyfield(field->data.u56.value,
						   field->data.u56.mask,
						   value, mask,
						   width, field_size);
			break;
		case VCAP_FIELD_U64:
			vcap_copy_limited_keyfield(field->data.u64.value,
						   field->data.u64.mask,
						   value, mask,
						   width, field_size);
			break;
		case VCAP_FIELD_U72:
			vcap_copy_limited_keyfield(field->data.u72.value,
						   field->data.u72.mask,
						   value, mask,
						   width, field_size);
			break;
		case VCAP_FIELD_U112:
			vcap_copy_limited_keyfield(field->data.u112.value,
						   field->data.u112.mask,
						   value, mask,
						   width, field_size);
			break;
		case VCAP_FIELD_U128:
			vcap_copy_limited_keyfield(field->data.u128.value,
						   field->data.u128.mask,
						   value, mask,
						   width, field_size);
			break;
		};
	}
}

/* Store (key, value) data in a element in a list for the client */
static void vcap_rule_alloc_keyfield(struct vcap_rule_internal *ri,
				     const struct vcap_field *keyfield,
				     enum vcap_key_field key,
				     u8 *value, u8 *mask, char *buffer)
{
	bool no_mask = vcap_bitarray_zero(keyfield->width, mask);
	struct vcap_client_keyfield *field;

	if (no_mask) {
		if (buffer)
			*buffer = 0;
		return;
	}
	if (buffer) {
		buffer = vcap_bitarray_tostring(buffer, keyfield->width, value);
		*buffer++ = '/';
		buffer = vcap_bitarray_tostring(buffer, keyfield->width, mask);
		*buffer++ = 0;
	}
	field = kzalloc(sizeof(struct vcap_client_keyfield), GFP_KERNEL);
	if (!field)
		return;
	INIT_LIST_HEAD(&field->ctrl.list);
	field->ctrl.key = key;
	field->ctrl.type = keyfield->type;
	vcap_copy_to_client_keyfield(ri, field, value, mask, keyfield->width);
	list_add_tail(&field->ctrl.list, &ri->data.keyfields);
}

static void vcap_copy_action_bit_field(struct vcap_u1_action *field, u8 *value)
{
	field->value = (*value) & 0x1;
}

static void vcap_copy_limited_actionfield(u8 *dstvalue, u8 *srcvalue,
					  int width, int bytes)
{
	memcpy(dstvalue, srcvalue, bytes);
	vcap_apply_width(dstvalue, width, bytes);
}

static void vcap_copy_to_client_actionfield(struct vcap_rule_internal *ri,
					    struct vcap_client_actionfield *field,
					    u8 *value, u16 width)
{
	int field_size = actionfield_size_table[field->ctrl.type];

	if (ri->admin->w32be) {
		switch (field->ctrl.type) {
		case VCAP_FIELD_BIT:
			vcap_copy_action_bit_field(&field->data.u1, value);
			break;
		case VCAP_FIELD_U32:
			vcap_copy_limited_actionfield((u8 *)&field->data.u32.value,
						      value,
						      width, field_size);
			break;
		case VCAP_FIELD_U48:
			vcap_copy_from_w32be(field->data.u48.value, value,
					     field_size, width);
			break;
		case VCAP_FIELD_U56:
			vcap_copy_from_w32be(field->data.u56.value, value,
					     field_size, width);
			break;
		case VCAP_FIELD_U64:
			vcap_copy_from_w32be(field->data.u64.value, value,
					     field_size, width);
			break;
		case VCAP_FIELD_U72:
			vcap_copy_from_w32be(field->data.u72.value, value,
					     field_size, width);
			break;
		case VCAP_FIELD_U112:
			vcap_copy_from_w32be(field->data.u112.value, value,
					     field_size, width);
			break;
		case VCAP_FIELD_U128:
			vcap_copy_from_w32be(field->data.u128.value, value,
					     field_size, width);
			break;
		};
	} else {
		switch (field->ctrl.type) {
		case VCAP_FIELD_BIT:
			vcap_copy_action_bit_field(&field->data.u1, value);
			break;
		case VCAP_FIELD_U32:
			vcap_copy_limited_actionfield((u8 *)&field->data.u32.value,
						      value,
						      width, field_size);
			break;
		case VCAP_FIELD_U48:
			vcap_copy_limited_actionfield(field->data.u48.value,
						      value,
						      width, field_size);
			break;
		case VCAP_FIELD_U56:
			vcap_copy_limited_actionfield(field->data.u56.value,
						      value,
						      width, field_size);
			break;
		case VCAP_FIELD_U64:
			vcap_copy_limited_actionfield(field->data.u64.value,
						      value,
						      width, field_size);
			break;
		case VCAP_FIELD_U72:
			vcap_copy_limited_actionfield(field->data.u72.value,
						      value,
						      width, field_size);
			break;
		case VCAP_FIELD_U112:
			vcap_copy_limited_actionfield(field->data.u112.value,
						      value,
						      width, field_size);
			break;
		case VCAP_FIELD_U128:
			vcap_copy_limited_actionfield(field->data.u128.value,
						      value,
						      width, field_size);
			break;
		};
	}
}

/* Store action value in an element in a list for the client */
static void vcap_rule_alloc_actionfield(struct vcap_rule_internal *ri,
				     const struct vcap_field *actionfield,
				     enum vcap_action_field action,
				     u8 *value, char *buffer)
{
	struct vcap_client_actionfield *field;

	buffer = vcap_bitarray_tostring(buffer, actionfield->width, value);
	*buffer++ = 0;
	field = kzalloc(sizeof(struct vcap_client_actionfield), GFP_KERNEL);
	if (!field)
		return;
	INIT_LIST_HEAD(&field->ctrl.list);
	field->ctrl.action = action;
	field->ctrl.type = actionfield->type;
	vcap_copy_to_client_actionfield(ri, field, value, actionfield->width);
	list_add_tail(&field->ctrl.list, &ri->data.actionfields);
}

/* Update the keyset for the rule */
int vcap_set_rule_set_keyset(struct vcap_rule *rule, enum vcap_keyfield_set keyset)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	const struct vcap_set *kset = vcap_keyfieldset(ri->admin->vtype, keyset);

	/* Check that the keyset is valid */
	if (!kset)
		return -EINVAL;
	ri->keyset_sw = kset->sw_per_item;
	ri->keyset_sw_regs = DIV_ROUND_UP(vctrl->vcaps[ri->admin->vtype].sw_width, 32);
	ri->data.keyset = keyset;
	return 0;
}

static int vcap_decode_rule_keyset(struct vcap_rule_internal *ri)
{
	struct vcap_stream_iter kiter, miter;
	struct vcap_admin *admin = ri->admin;
	const struct vcap_field *keyfield;
	enum vcap_type vt = admin->vtype;
	const struct vcap_typegroup *tgt;
	enum vcap_keyfield_set keyset;
	int keyfield_count;
	char buffer[400];
	u32 *maskstream;
	u32 *keystream;
	u8 value[16];
	u8 mask[16];
	int idx, res;
	bool no_mask;

	keystream = admin->cache.keystream;
	maskstream = admin->cache.maskstream;
	res = vcap_find_keystream_keyset(vt, keystream, maskstream, false, 0);
	if (res < 0) {
		pr_err("%s:%d: could not find valid keyset: %d\n",
		       __func__, __LINE__, res);
		return -EINVAL;
	}
	keyset = res;
	pr_debug("%s:%d: keyset: %d\n", __func__, __LINE__, keyset);
	keyfield_count = vcap_keyfield_count(vt, keyset);
	keyfield = vcap_keyfields(vt, keyset);
	tgt = vcap_keyfield_typegroup(vt, keyset);
	/* Start decoding the streams. Note: fields are not ordered by their offset */
	for (idx = 0; idx < keyfield_count; ++idx) {
		if (keyfield[idx].width > 0) {
			/* First get the mask */
			memset(mask, 0, DIV_ROUND_UP(keyfield[idx].width, 8));
			vcap_iter_init(&miter, vctrl->vcaps[vt].sw_width, tgt, keyfield[idx].offset);
			vcap_decode_field(maskstream, &miter, keyfield[idx].width, mask);
			/* Skip if no mask bits are set */
			no_mask = vcap_bitarray_zero(keyfield[idx].width, mask);
			if (no_mask)
				continue;
			/* Get the key */
			memset(value, 0, DIV_ROUND_UP(keyfield[idx].width, 8));
			vcap_iter_init(&kiter, vctrl->vcaps[vt].sw_width, tgt, keyfield[idx].offset);
			vcap_decode_field(keystream, &kiter, keyfield[idx].width, value);
			vcap_rule_alloc_keyfield(ri, &keyfield[idx], idx, value, mask, buffer);
			pr_debug("%s:%d: %s, type: %d: width: %d: %s\n",
				__func__, __LINE__,
				vctrl->stats->keyfield_names[idx],
				keyfield[idx].type,
				keyfield[idx].width,
				buffer);
		}
	}
	return vcap_set_rule_set_keyset((struct vcap_rule *)ri, keyset);
}

/* Update the actionset for the rule */
int vcap_set_rule_set_actionset(struct vcap_rule *rule,
				enum vcap_actionfield_set actionset)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	const struct vcap_set *aset = vcap_actionfieldset(ri->admin->vtype,
							  actionset);

	/* Check that the actionset is valid */
	if (!aset)
		return -EINVAL;
	ri->actionset_sw = aset->sw_per_item;
	ri->actionset_sw_regs = DIV_ROUND_UP(vctrl->vcaps[ri->admin->vtype].act_width, 32);
	ri->data.actionset = actionset;
	return 0;
}

static int vcap_decode_rule_actionset(struct vcap_rule_internal *ri)
{
	struct vcap_admin *admin = ri->admin;
	const struct vcap_field *actionfield;
	enum vcap_actionfield_set actionset;
	enum vcap_type vt = admin->vtype;
	const struct vcap_typegroup *tgt;
	struct vcap_stream_iter iter;
	int actfield_count;
	char buffer[400];
	u32 *actstream;
	u8 value[16];
	int idx, res;
	bool no_bits;

	actstream = admin->cache.actionstream;
	res = vcap_find_actionstream_actionset(vt, actstream, 0);
	if (res < 0) {
		pr_err("%s:%d: could not find valid actionset: %d\n",
		       __func__, __LINE__, res);
		return -EINVAL;
	}
	actionset = res;
	pr_debug("%s:%d: actionset: %d\n", __func__, __LINE__, actionset);
	actfield_count = vcap_actionfield_count(vt, actionset);
	actionfield = vcap_actionfields(vt, actionset);
	tgt = vcap_actionfield_typegroup(vt, actionset);
	/* Start decoding the stream. Note: fields are not ordered by their offset */
	for (idx = 0; idx < actfield_count; ++idx) {
		if (actionfield[idx].width > 0) {
			/* Get the action */
			memset(value, 0, DIV_ROUND_UP(actionfield[idx].width, 8));
			vcap_iter_init(&iter, vctrl->vcaps[vt].act_width, tgt, actionfield[idx].offset);
			vcap_decode_field(actstream, &iter, actionfield[idx].width, value);
			/* Skip if no bits are set */
			no_bits = vcap_bitarray_zero(actionfield[idx].width, value);
			if (no_bits && idx != VCAP_AF_TYPE)
				continue;
			vcap_rule_alloc_actionfield(ri, &actionfield[idx], idx, value, buffer);
			pr_debug("%s:%d: %s, type: %d: width: %d: %s\n",
				__func__, __LINE__,
				vctrl->stats->actionfield_names[idx],
				actionfield[idx].type,
				actionfield[idx].width,
				buffer);
		}
	}
	return vcap_set_rule_set_actionset((struct vcap_rule *)ri, actionset);
}

static void vcap_decode_rule_counter(struct vcap_rule_internal *ri)
{
	struct vcap_admin *admin = ri->admin;

	ri->counter.value = admin->cache.counter;
	ri->counter.sticky = admin->cache.sticky;
}

/* Read key data from a VCAP address and discover if there is a rule keyset here */
static int vcap_addr_keyset(struct net_device *ndev, struct vcap_admin *admin, int addr)
{
	enum vcap_type vt = admin->vtype;
	int keyset_sw_regs, idx;
	u32 key = 0, mask = 0;

	keyset_sw_regs = DIV_ROUND_UP(vctrl->vcaps[vt].sw_width, 32);
	vctrl->ops->update(ndev, admin, VCAP_CMD_READ, VCAP_SEL_ALL, addr);
	vctrl->ops->cache_read(ndev, admin, VCAP_SEL_ENTRY, 0,
			       keyset_sw_regs);
	/* Skip uninitialized key/mask entries */
	for (idx = 0; idx < keyset_sw_regs; ++idx) {
		key |= ~admin->cache.keystream[idx];
		mask |= admin->cache.maskstream[idx];
	}
	if (key == 0 && mask == 0)
		return -EINVAL;
	return vcap_find_keystream_keyset(vt, admin->cache.keystream,
					  admin->cache.maskstream, false, 0);
}

/* Make a shallow copy of the rule without the fields */
static struct vcap_rule_internal *vcap_dup_rule(struct vcap_rule_internal *ri)
{
	struct vcap_rule_internal *duprule;

	/* Allocate the client part */
	duprule = kzalloc(sizeof(*duprule), GFP_KERNEL);
	if (duprule) {
		*duprule = *ri;
		/* Not inserted in the VCAP */
		INIT_LIST_HEAD(&duprule->list);
		/* No elements in these lists */
		INIT_LIST_HEAD(&duprule->data.keyfields);
		INIT_LIST_HEAD(&duprule->data.actionfields);
	} else {
		return ERR_PTR(-ENOMEM);
	}
	return duprule;
}

static int vcap_read_rule(struct vcap_rule_internal *ri)
{
	struct vcap_admin *admin = ri->admin;
	int sw_idx, ent_idx = 0, act_idx = 0;
	u32 addr = ri->addr;

	if (!ri->size || !ri->keyset_sw_regs || !ri->actionset_sw_regs) {
		pr_err("%s:%d: rule is empty\n", __func__, __LINE__);
		return -EINVAL;
	}
	vcap_erase_cache(ri);
	/* Use the values in the streams to read the VCAP cache */
	for (sw_idx = 0; sw_idx < ri->size; sw_idx++, addr++) {
		vctrl->ops->update(ri->ndev, admin, VCAP_CMD_READ,
				  VCAP_SEL_ALL, addr);
		vctrl->ops->cache_read(ri->ndev, admin,
				      VCAP_SEL_ENTRY, ent_idx,
				      ri->keyset_sw_regs);
		vctrl->ops->cache_read(ri->ndev, admin,
				      VCAP_SEL_ACTION, act_idx,
				      ri->actionset_sw_regs);
		if (sw_idx == 0)
			vctrl->ops->cache_read(ri->ndev, admin,
					      VCAP_SEL_COUNTER, ri->counter_id, 0);
		ent_idx += ri->keyset_sw_regs;
		act_idx += ri->actionset_sw_regs;
	}
	return 0;
}

static int vcap_read_counter(struct vcap_rule_internal *ri,
			     struct vcap_counter *ctr)
{
	struct vcap_admin *admin = ri->admin;

	if (!ctr) {
		pr_err("%s:%d: counter is missing\n", __func__, __LINE__);
		return -EINVAL;
	}
	vctrl->ops->update(ri->ndev, admin, VCAP_CMD_READ, VCAP_SEL_COUNTER, ri->addr);
	vctrl->ops->cache_read(ri->ndev, admin, VCAP_SEL_COUNTER, ri->counter_id, 0);
	ctr->value = admin->cache.counter;
	ctr->sticky = admin->cache.sticky;
	return 0;
}

/* Provide all rules via a callback interface */
int vcap_rule_iter(int (*callback)(void *, struct vcap_rule *), void *arg)
{
	struct vcap_rule_internal *ri;
	struct vcap_admin *admin;
	int ret;

	ret = vcap_api_check(vctrl);
	if (ret)
		return ret;
	list_for_each_entry(admin, &vctrl->list, list) {
		list_for_each_entry(ri, &admin->rules, list) {
			int ret = callback(arg, &ri->data);
			if (ret)
				return ret;
		}
	}
	return 0;
}


/* Find a rule with a provided rule id */
static struct vcap_rule_internal *vcap_lookup_rule(u32 id)
{
	struct vcap_rule_internal *ri;
	struct vcap_admin *admin;

	/* Look for the rule id in all vcaps */
	list_for_each_entry(admin, &vctrl->list, list) {
		list_for_each_entry(ri, &admin->rules, list) {
			if (ri->data.id == id) {
				return ri;
			}
		}
	}
	return NULL;
}

/* Find the first rule id with a provided cookie */
int vcap_lookup_rule_by_cookie(u64 cookie)
{
	struct vcap_rule_internal *ri;
	struct vcap_admin *admin;
	u32 min_id = ~0;

	/* Look for the rule id in all vcaps */
	list_for_each_entry(admin, &vctrl->list, list)
		list_for_each_entry(ri, &admin->rules, list)
			if (ri->data.cookie == cookie && ri->data.id < min_id)
				min_id = ri->data.id;
	if (min_id == ~0)
		return -ENOENT;
	return min_id;
}

/* Give client a copy of the rule with ownership */
struct vcap_rule *vcap_get_rule(struct net_device *ndev, u32 id)
{
	struct vcap_rule_internal *elem;
	struct vcap_rule_internal *ri;
	int ret;

	if (!ndev) {
		pr_err("%s:%d: netdev is missing\n", __func__, __LINE__);
		return ERR_PTR(-ENODEV);
	}
	ret = vcap_api_check(vctrl);
	if (ret)
		return ERR_PTR(ret);
	elem = vcap_lookup_rule(id);
	if (!elem) {
		pr_err("%s:%d: could not find rule: %u\n", __func__, __LINE__, id);
		ri = 0;
		goto out;
	}
	mutex_lock(&elem->admin->lock);
	ri = vcap_dup_rule(elem);
	if (IS_ERR(ri)) {
		pr_err("%s:%d: could not allocate rule for vcap type: %d, instance: %d\n",
		       __func__, __LINE__, elem->admin->vtype, elem->admin->vinst);
		ri = ERR_PTR(-ENOMEM);
		goto unlock;
	}
	/* Read data from VCAP */
	ret = vcap_read_rule(ri);
	if (ret) {
		pr_err("%s:%d: could not read rule: %u\n", __func__, __LINE__, id);
		ri = ERR_PTR(ret);
		goto unlock;
	}
	/* Decode key and mask stream data and add fields to the rule */
	ret = vcap_decode_rule_keyset(ri);
	if (ret) {
		pr_err("%s:%d: could not decode rule %u keys\n", __func__, __LINE__, id);
		ri = ERR_PTR(ret);
		goto unlock;
	}
	ret = vcap_decode_rule_actionset(ri);
	if (ret) {
		pr_err("%s:%d: could not decode rule %u actions\n", __func__, __LINE__, id);
		ri = ERR_PTR(ret);
		goto unlock;
	}
	vcap_decode_rule_counter(ri);
unlock:
	mutex_unlock(&ri->admin->lock);
out:
	return (struct vcap_rule *)ri;
}

static int vcap_write_rule(struct vcap_rule_internal *ri)
{
	struct vcap_admin *admin = ri->admin;
	int sw_idx, ent_idx = 0, act_idx = 0;
	u32 addr = ri->addr;

	if (!ri->size || !ri->keyset_sw_regs || !ri->actionset_sw_regs) {
		pr_err("%s:%d: rule is empty\n", __func__, __LINE__);
		return -EINVAL;
	}
	/* Use the values in the streams to write the VCAP cache */
	for (sw_idx = 0; sw_idx < ri->size; sw_idx++, addr++) {
		vctrl->ops->cache_write(ri->ndev, admin,
					VCAP_SEL_ENTRY, ent_idx,
					ri->keyset_sw_regs);
		vctrl->ops->cache_write(ri->ndev, admin,
					VCAP_SEL_ACTION, act_idx,
					ri->actionset_sw_regs);
		vctrl->ops->update(ri->ndev, admin, VCAP_CMD_WRITE,
				   VCAP_SEL_ALL, addr);
		ent_idx += ri->keyset_sw_regs;
		act_idx += ri->actionset_sw_regs;
	}
	return 0;
}

static int vcap_write_counter(struct vcap_rule_internal *ri,
			      struct vcap_counter *ctr)
{
	struct vcap_admin *admin = ri->admin;

	if (!ctr) {
		pr_err("%s:%d: counter is missing\n", __func__, __LINE__);
		return -EINVAL;
	}
	admin->cache.counter = ctr->value;
	admin->cache.sticky = ctr->sticky;
	vctrl->ops->cache_write(ri->ndev, admin, VCAP_SEL_COUNTER, ri->counter_id, 0);
	vctrl->ops->update(ri->ndev, admin, VCAP_CMD_WRITE, VCAP_SEL_COUNTER, ri->addr);
	return 0;
}

/* Add a keyset to a keyset list */
bool vcap_keyset_list_add(struct vcap_keyset_list *keysetlist,
			  enum vcap_keyfield_set keyset)
{
	int idx;

	if (keysetlist->cnt < keysetlist->max) {
		/* Avoid duplicates */
		for (idx = 0; idx < keysetlist->cnt; ++idx)
			if (keysetlist->keysets[idx] == keyset)
				return keysetlist->cnt < keysetlist->max;
		keysetlist->keysets[keysetlist->cnt++] = keyset;
	}
	return keysetlist->cnt < keysetlist->max;
}

/* Add a key to a key list */
bool vcap_key_list_add(struct vcap_key_list *keylist, enum vcap_key_field key)
{
	int idx;

	if (keylist->cnt < keylist->max) {
		/* Avoid duplicates */
		for (idx = 0; idx < keylist->cnt; ++idx)
			if (keylist->keys[idx] == key)
				return keylist->cnt < keylist->max;
		keylist->keys[keylist->cnt++] = key;
	}
	return keylist->cnt < keylist->max;
}

/* Match a list of keys against the keysets available in a vcap type */
bool vcap_rule_match_keysets(enum vcap_type vtype,
			     struct vcap_key_list *keylist,
			     struct vcap_keyset_match *match)
{
	const struct vcap_field **keysetmap = vctrl->vcaps[vtype].keyfield_set_map;
	int *max_fields = vctrl->vcaps[vtype].keyfield_set_map_size;
	enum vcap_keyfield_set best_match = VCAP_KFS_NO_VALUE;
	int set_size = vctrl->vcaps[vtype].keyfield_set_size;
	int idx, jdx, ldx, max, found, max_keys = 0;
	const struct vcap_field *fields;

	match->matches.cnt = 0;
	match->unmatched_keys.cnt = 0;
	for (idx = 0; idx < set_size; ++idx) {
		/* Iterate the keysets of the VCAP */
		if (keysetmap[idx]) {
			fields = keysetmap[idx];
			max = max_fields[idx];
			found = 0;
			/* Iterate the keyfields of the keyset */
			for (jdx = 0; jdx < max; ++jdx) {
				if (fields[jdx].width == 0)
					continue;
				/* Count the matching keyfields */
				for (ldx = 0; ldx < keylist->cnt; ++ldx) {
					if (keylist->keys[ldx] == jdx) {
						found++;
						pr_debug("%s:%d: %s: found: %d/%d %s\n",
							 __func__, __LINE__,
							 vctrl->stats->keyfield_set_names[idx],
							 found, keylist->cnt,
							 vctrl->stats->keyfield_names[jdx]);
					}
				}
			}
			if (found > max_keys) {
				max_keys = found;
				best_match = idx;
			}
			/* Save the keyset if all fields were found */
			if (found == keylist->cnt)
				if (!vcap_keyset_list_add(&match->matches, idx))
					/* Return when the quota is filled */
					break;
		}
	}
	if (match->matches.cnt == 0) {
		/* Provide the best matching keyset */
		match->best_match = best_match;
		if (match->unmatched_keys.max > 0) {
			/* Provide the unmatched keys */
			fields = keysetmap[best_match];
			max = max_fields[best_match];
			/* Iterate the requested keys */
			for (ldx = 0; ldx < keylist->cnt; ++ldx) {
				found = 0;
				/* Iterate the keyfields of the keyset */
				for (jdx = 0; jdx < max; ++jdx) {
					if (fields[jdx].width == 0)
						continue;
					if (keylist->keys[ldx] == jdx) {
						found = 1;
						break;
					}
				}
				if (found == 0)
					vcap_key_list_add(&match->unmatched_keys,
							  keylist->keys[ldx]);
			}
		}
	}
	return match->matches.cnt > 0;
}

static bool _vcap_rule_find_keysets(struct vcap_rule_internal *ri,
				   struct vcap_keyset_match *match)
{
	const struct vcap_client_keyfield *ckf;
	struct vcap_key_list keylist = {0};
	enum vcap_key_field vkeys[30];

	keylist.max = ARRAY_SIZE(vkeys);
	keylist.keys = vkeys;

	/* Collect the keys from the rule in a keylist */
	list_for_each_entry(ckf, &ri->data.keyfields, ctrl.list) {
		pr_debug("%s:%d: add [%d] %s\n",
			 __func__, __LINE__, ckf->ctrl.key,
			 vctrl->stats->keyfield_names[ckf->ctrl.key]);
		if (!vcap_key_list_add(&keylist, ckf->ctrl.key))
			/* bail out when the list is full */
			break;
	}
	pr_debug("%s:%d: look for: %d keys\n", __func__, __LINE__, keylist.cnt);
	return vcap_rule_match_keysets(ri->admin->vtype, &keylist, match);
}

/* Return keyset information that matches the keys in the rule */
bool vcap_rule_find_keysets(struct vcap_rule *rule,
			   struct vcap_keyset_match *match)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;

	return _vcap_rule_find_keysets(ri, match);
}

static bool vcap_rule_find_actionsets(struct vcap_rule_internal *ri, int count,
				      enum vcap_actionfield_set *result,
				      int *res_count)
{
	enum vcap_type vt = ri->admin->vtype;
	const struct vcap_field **map = vctrl->vcaps[vt].actionfield_set_map;
	int *max_fields = vctrl->vcaps[vt].actionfield_set_map_size;
	int idx, jdx, kdx, max, found, rule_num_keys = 0;
	int max_sets = vctrl->vcaps[vt].actionfield_set_size;
	const struct vcap_client_actionfield *caf;
	const struct vcap_field *fields;

	/* First find the number of actionfields in the rule */
	list_for_each_entry(caf, &ri->data.actionfields, ctrl.list) {
		pr_debug("%s:%d: search: %d\n", __func__, __LINE__, caf->ctrl.action);
		++rule_num_keys;
	}
	pr_debug("%s:%d: look for: %d actions\n", __func__, __LINE__, rule_num_keys);
	kdx = 0;
	*res_count = 0;
	for (idx = 0; idx < max_sets; ++idx) {
		/* Iterate the actionsets of the VCAP */
		if (map[idx]) {
			fields = map[idx];
			max = max_fields[idx];
			found = 0;
			/* Iterate the actionfields of the actionset */
			for (jdx = 0; jdx < max; ++jdx) {
				if (fields[jdx].width == 0)
					continue;
				pr_debug("%s:%d: [%d] %s, type: %d: width: %d\n",
					__func__, __LINE__, jdx,
					vctrl->stats->actionfield_names[jdx],
					fields[jdx].type,
					fields[jdx].width);
				/* Count the matching fields */
				list_for_each_entry(caf, &ri->data.actionfields, ctrl.list) {
					if (caf->ctrl.action == jdx) {
						found++;
						pr_debug("%s:%d: found: action: %d, total %d\n",
							__func__, __LINE__,
							jdx, found);
					}
				}
			}
			/* Save the actionset if all fields were found */
			if (found == rule_num_keys) {
				result[kdx++] = idx;
				pr_debug("%s:%d: done: actionset: %d, have: %d\n", __func__, __LINE__, idx, kdx);
				/* Return when the quota is filled */
				if (kdx == count)
					break;
			}
		}
	}
	*res_count = kdx;
	return kdx > 0;
}

/* Find vcap type instance count */
int vcap_admin_type_count(enum vcap_type vt)
{
	struct vcap_admin *admin;
	int res = 0;

	list_for_each_entry(admin, &vctrl->list, list)
		if (vt == admin->vtype)
			++res;
	return res;
}

/* Convert a chain id to a VCAP lookup index */
int vcap_chain_id_to_lookup(struct vcap_admin *admin, int cur_cid)
{
	int lookup_first = admin->vinst * admin->lookups_per_instance;
	int lookup_last = lookup_first + admin->lookups_per_instance;
	int cid_next = admin->first_cid + VCAP_CID_LOOKUP_SIZE;
	int cid = admin->first_cid;
	int lookup;

	for (lookup = lookup_first; lookup < lookup_last; ++lookup,
	     cid += VCAP_CID_LOOKUP_SIZE, cid_next += VCAP_CID_LOOKUP_SIZE)
		if (cur_cid >= cid && cur_cid < cid_next)
			return lookup;
	return 0;
}

/* Get number of rules in a vcap instance lookup chain id range */
int vcap_admin_rule_count(struct vcap_admin *admin, int cid)
{
	int min_cid = DIV_ROUND_DOWN_ULL(cid, VCAP_CID_LOOKUP_SIZE);
	int max_cid = min_cid + VCAP_CID_LOOKUP_SIZE - 1;
	struct vcap_rule_internal *elem;
	int count = 0;

	list_for_each_entry(elem, &admin->rules, list)
		if (elem->data.vcap_chain_id >= min_cid &&
		    elem->data.vcap_chain_id < max_cid)
			++count;
	return count;
}

static int vcap_lookup_to_chain_id(struct vcap_admin *admin, int lookup)
{
	int lookup_first = admin->vinst * admin->lookups_per_instance;
	int lookup_next = lookup_first + admin->lookups_per_instance;

	if (lookup >= lookup_first && lookup < lookup_next)
		return admin->first_cid + (lookup - lookup_first) *
			VCAP_CID_LOOKUP_SIZE;
	return 0;
}

/* Find a vcap instance and chain id using vcap type and lookup index */
struct vcap_admin *vcap_find_admin_with_lookup(enum vcap_type vt,
					       int lookup, int *cid)
{
	struct vcap_admin *admin;
	int chain;

	list_for_each_entry(admin, &vctrl->list, list) {
		if (vt == admin->vtype) {
			chain = vcap_lookup_to_chain_id(admin, lookup);
			if (chain) {
				if (cid)
					*cid = chain;
				return admin;
			}
		}
	}
	return 0;
}

/* Lookup a vcap instance using chain id */
static struct vcap_admin *_vcap_find_admin(int cid)
{
	struct vcap_admin *admin;

	list_for_each_entry(admin, &vctrl->list, list) {
		if (cid >= admin->first_cid && cid <= admin->last_cid)
			return admin;
	}
	return 0;
}

/* Lookup a vcap instance using chain id */
struct vcap_admin *vcap_find_admin(int cid)
{
	int ret = vcap_api_check(vctrl);

	if (ret)
		return NULL;
	return _vcap_find_admin(cid);
}

/* Check if there is room for a new rule */
static int vcap_rule_space(struct vcap_admin *admin, int size)
{
	if (admin->last_used_addr - size < admin->first_valid_addr) {
		pr_err("%s:%d: No room for rule size: %u, %u\n",
		       __func__, __LINE__, size, admin->first_valid_addr);
		return -ENOSPC;
	}
	return 0;
}

/* Add the keyset typefield to the list of rule keyfields */
static int vcap_add_type_keyfield(struct vcap_rule *rule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	enum vcap_keyfield_set keyset = rule->keyset;
	enum vcap_type vt = ri->admin->vtype;
	const struct vcap_field *fields;
	const struct vcap_set *kset;
	int ret = -EINVAL;

	kset = vcap_keyfieldset(vt, keyset);
	if (!kset)
		return ret;
	if (kset->type_id == (u8)-1)  /* No type field is needed */
		return 0;

	fields = vcap_keyfields(vt, keyset);
	if (!fields)
		return -EINVAL;
	if (fields[VCAP_KF_TYPE].width > 1) {
		ret = vcap_rule_add_key_u32(rule, VCAP_KF_TYPE, kset->type_id, 0xff);
	} else {
		if (kset->type_id)
			ret = vcap_rule_add_key_bit(rule, VCAP_KF_TYPE, VCAP_BIT_1);
		else
			ret = vcap_rule_add_key_bit(rule, VCAP_KF_TYPE, VCAP_BIT_0);
	}
	return 0;
}

/* Add the actionset typefield to the list of rule actionfields */
static int vcap_add_type_actionfield(struct vcap_rule *rule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	enum vcap_actionfield_set actionset = rule->actionset;
	enum vcap_type vt = ri->admin->vtype;
	const struct vcap_field *fields;
	const struct vcap_set *aset;
	int ret = -EINVAL;

	aset = vcap_actionfieldset(vt, actionset);
	if (!aset)
		return ret;
	if (aset->type_id == (u8)-1)  /* No type field is needed */
		return 0;

	fields = vcap_actionfields(vt, actionset);
	if (!fields)
		return -EINVAL;
	if (fields[VCAP_AF_TYPE].width > 1) {
		ret = vcap_rule_add_action_u32(rule, VCAP_AF_TYPE, aset->type_id);
	} else {
		if (aset->type_id)
			ret = vcap_rule_add_action_bit(rule, VCAP_AF_TYPE, VCAP_BIT_1);
		else
			ret = vcap_rule_add_action_bit(rule, VCAP_AF_TYPE, VCAP_BIT_0);
	}
	return 0;
}

/* Find keyfield info in any of available keysets in the vcap */
static const struct vcap_field *vcap_find_keyfield_info(struct vcap_admin *admin,
							enum vcap_key_field key)
{
	int max_sets = vctrl->vcaps[admin->vtype].keyfield_set_size;
	const struct vcap_field **map = vctrl->vcaps[admin->vtype].keyfield_set_map;
	int *max_fields = vctrl->vcaps[admin->vtype].keyfield_set_map_size;
	const struct vcap_field *fields;
	int idx, jdx, max;

	for (idx = 0; idx < max_sets; ++idx) {
		/* Iterate the keysets of the VCAP */
		if (map[idx]) {
			fields = map[idx];
			max = max_fields[idx];
			/* Iterate the keyfields of the keyset */
			for (jdx = 0; jdx < max; ++jdx) {
				if (fields[jdx].width == 0)
					continue;
				if (key == jdx)
					return &fields[jdx];
			}
		}
	}
	return NULL;
}

/* Find actionfield info in any of available actionsets in the vcap */
static const struct vcap_field *vcap_find_actionfield_info(struct vcap_admin *admin,
							   enum vcap_action_field action)
{
	int max_sets = vctrl->vcaps[admin->vtype].actionfield_set_size;
	const struct vcap_field **map = vctrl->vcaps[admin->vtype].actionfield_set_map;
	int *max_fields = vctrl->vcaps[admin->vtype].actionfield_set_map_size;
	const struct vcap_field *fields;
	int idx, jdx, max;

	for (idx = 0; idx < max_sets; ++idx) {
		/* Iterate the actionsets of the VCAP */
		if (map[idx]) {
			fields = map[idx];
			max = max_fields[idx];
			/* Iterate the actionfields of the actionset */
			for (jdx = 0; jdx < max; ++jdx) {
				if (fields[jdx].width == 0)
					continue;
				if (action == jdx)
					return &fields[jdx];
			}
		}
	}
	return NULL;
}

static void vcap_show_keyset_match(struct vcap_rule_internal *ri,
			      struct vcap_keyset_match *match)
{
	int idx;

	pr_info("%s:%d: best match: [%d], %s, missing: %d\n",
		__func__, __LINE__,
		match->best_match,
		vctrl->stats->keyfield_set_names[match->best_match],
		match->unmatched_keys.cnt);
	for (idx = 0; idx < match->unmatched_keys.cnt; ++idx)
		pr_info("%s:%d: missing: [%d] %s\n", __func__, __LINE__,
			match->unmatched_keys.keys[idx],
			vctrl->stats->keyfield_names[match->unmatched_keys.keys[idx]]);
}

/* Validate a rule with respect to available port keys */
int vcap_val_rule(struct vcap_rule *rule, u16 l3_proto)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	enum vcap_actionfield_set actionsets[1];
	struct vcap_keyset_match match = {0};
	enum vcap_keyfield_set keysets[10];
	enum vcap_key_field keys[10];
	int ret;

	ret = vcap_api_check(vctrl);
	if (ret)
		return ret;
	if (!ri->admin) {
		ri->data.exterr = VCAP_ERR_NO_ADMIN;
		pr_err("%s:%d: vcap instance is missing\n", __func__, __LINE__);
		return -EINVAL;
	}
	if (!ri->ndev) {
		ri->data.exterr = VCAP_ERR_NO_NETDEV;
		pr_err("%s:%d: netdev is missing\n", __func__, __LINE__);
		return -EINVAL;
	}
	match.matches.keysets = keysets;
	match.matches.max = ARRAY_SIZE(keysets);
	if (ri->data.keyset == VCAP_KFS_NO_VALUE) {
		/* Iterate over rule keyfields and select a keyset that fits */
		match.unmatched_keys.keys = keys;
		match.unmatched_keys.max = ARRAY_SIZE(keys);
		if (!_vcap_rule_find_keysets(ri, &match)) {
			pr_err("%s:%d: no keysets matched the rule keys\n", __func__, __LINE__);
			vcap_show_keyset_match(ri, &match);
			ri->data.exterr = VCAP_ERR_NO_KEYSET_MATCH;
			return -EINVAL;
		}
	} else {
		/* prepare for keyset validation */
		keysets[0] = ri->data.keyset;
		match.matches.cnt = 1;
	}
	/* Pick a keyset that is supported in the port lookups */
	ret = vctrl->ops->validate_keyset(ri->ndev, ri->admin, rule,
					     &match.matches, l3_proto);
	if (ret < 0) {
		pr_err("%s:%d: keyset validation failed: %d\n",
		       __func__, __LINE__, ret);
		ri->data.exterr = VCAP_ERR_NO_PORT_KEYSET_MATCH;
		return ret;
	}

	/* use the keyset that is supported in the port lookups */
	ret = vcap_set_rule_set_keyset(rule, ret);
	if (ret < 0) {
		pr_err("%s:%d: keyset was not updated: %d\n",
		       __func__, __LINE__, ret);
		return ret;
	}
	if (ri->data.actionset == VCAP_AFS_NO_VALUE) {
		int actionset_count;

		/* Iterate over rule actionfields and select a actionset that fits */
		if (!vcap_rule_find_actionsets(ri, ARRAY_SIZE(actionsets), actionsets,
					       &actionset_count)) {
			pr_err("%s:%d: no actionsets matched the rule actions\n", __func__, __LINE__);
			ri->data.exterr = VCAP_ERR_NO_ACTIONSET_MATCH;
			return -EINVAL;
		}
		ret = vcap_set_rule_set_actionset(rule, actionsets[0]);
		if (ret < 0) {
			pr_err("%s:%d: actionset was not updated: %d\n",
			       __func__, __LINE__, ret);
			return ret;
		}
	}
	vcap_add_type_keyfield(rule);
	vcap_add_type_actionfield(rule);
	/* Add default fields to this rule */
	vctrl->ops->add_default_fields(ri->ndev, ri->admin, rule);

	/* Rule size is the maximum of the entry and action subword count */
	ri->size = max(ri->keyset_sw, ri->actionset_sw);

	/* Finally check if there is room for the rule in the VCAP */
	return vcap_rule_space(ri->admin, ri->size);
}

/*
 * Entries are sorted with increasing values of sort_key.
 * I.e. Lowest numerical sort_key is first in list.
 * In order to locate largest keys first in list we negate the key size with
 * (max_size - size).
 */
static u32 vcap_sort_key(u32 max_size, u32 size, u8 user, u16 prio)
{
	return ((max_size - size) << 24) | (user << 16) | prio;
}

/* calculate the address of the next rule after this (lower address and prio) */
static u32 vcap_next_rule_addr(u32 addr, struct vcap_rule_internal *ri)
{
	return ((addr - ri->size) /  ri->size) * ri->size;
}

/* Assign a unique rule id and autogenerate one if id == 0 */
static u32 vcap_set_rule_id(struct vcap_rule_internal *ri)
{
	if (ri->data.id == 0) {
		u32 next_id = vctrl->rule_id + 1;
		for (next_id = vctrl->rule_id + 1; next_id < ~0; ++next_id) {
			if (!vcap_lookup_rule(next_id)) {
				ri->data.id = next_id;
				vctrl->rule_id = next_id;
				break;
			}
		}
	}
	return ri->data.id;
}

/* Set a rule counter id (for certain vcaps only) */
void vcap_rule_set_counter_id(struct vcap_rule *rule, u32 counter_id)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;

	ri->counter_id = counter_id;
}

/* Insert a rule (duplicate) in the VCAP list. Return indication if the rule
 * block needs to be moved to make room for the new rule
 */
static int vcap_insert_rule(struct vcap_rule_internal *ri,
			    struct vcap_rule_move *move)
{
	int sw_count = vctrl->vcaps[ri->admin->vtype].sw_count;
	struct vcap_rule_internal *duprule;
	struct vcap_admin *admin = ri->admin;
	struct vcap_rule_internal *elem;
	u32 addr;

	/* Calculate a sort key based on rule size, user and priority
	 * Insert the new rule in the list of rule based on the sort key
	 * If the rule is inserted between existing rules then move these
	 * rules to make room for the new rule and update their start address.
	 */
	ri->sort_key = vcap_sort_key(sw_count, ri->size, ri->data.user, ri->data.priority);

	/* TODO: Lock when modifying the list */
	list_for_each_entry(elem, &admin->rules, list) {
		if (ri->sort_key < elem->sort_key) {
			pr_debug("%s:%d: insert: %#08x (%u, %u) before %#08x (%u, %u)\n",
				__func__, __LINE__,
				ri->sort_key, ri->size, ri->addr,
				elem->sort_key, elem->size, elem->addr);
			break;
		}
	}
	if (list_entry_is_head(elem, &admin->rules, list)) {
		ri->addr = vcap_next_rule_addr(admin->last_used_addr, ri);
		admin->last_used_addr = ri->addr;
		/* Add a shallow copy of the rule to the VCAP list */
		duprule = vcap_dup_rule(ri);
		if (IS_ERR(duprule))
			return PTR_ERR(duprule);
		list_add_tail(&duprule->list, &admin->rules);
		pr_debug("%s:%d: appending: %#08x (%u, %u)\n",
			__func__, __LINE__, duprule->sort_key, duprule->size, duprule->addr);
	} else {
		/* Reuse the space of the current rule */
		addr = elem->addr + elem->size;
		addr = ri->addr = vcap_next_rule_addr(addr, ri);
		/* Add a shallow copy of the rule to the VCAP list */
		duprule = vcap_dup_rule(ri);
		if (IS_ERR(duprule))
			return PTR_ERR(duprule);
		/* Add before the current entry in elem */
		list_add_tail(&duprule->list, &elem->list);
		pr_debug("%s:%d: inserting: %#08x (%u, %u)\n",
			__func__, __LINE__, duprule->sort_key, duprule->size, duprule->addr);
		/* Update the current rule */
		addr = elem->addr = vcap_next_rule_addr(addr, elem);
		/* Update the remaining rules in the list */
		list_for_each_entry_continue(elem, &admin->rules, list) {
			/* Set new adresses */
			addr = elem->addr = vcap_next_rule_addr(addr, elem);
		}
		move->addr = admin->last_used_addr;
		move->count = ri->addr - addr;
		move->offset = admin->last_used_addr - addr;
		admin->last_used_addr = addr;
	}
	return 0;
}

static int vcap_move_rules(struct vcap_rule_internal *ri,
			    struct vcap_rule_move *move)
{
	struct vcap_admin *admin = ri->admin;
	struct vcap_rule_internal *elem;
	/* If the rule is inserted between existing rules then move these
	 * rules to make room for the new rule and update their start address.
	 */
	pr_debug("%s:%d, move offset: %d, count: %d\n", __func__, __LINE__,
		move->offset, move->count);
	list_for_each_entry(elem, &admin->rules, list) {
		pr_debug("%s:%d: %#08x (%u, %u)\n",
			__func__, __LINE__,
			elem->sort_key, elem->size, elem->addr);
	}
	vctrl->ops->move(ri->ndev, admin, move->addr,
			 move->offset, move->count);
	return 0;
}

/* Encode and write a validated rule to the VCAP */
int vcap_add_rule(struct vcap_rule *rule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	struct vcap_rule_move move = {0};
	struct vcap_counter ctr = {0};
	int ret;

	ret = vcap_api_check(vctrl);
	if (ret)
		return ret;
	/* Insert the new rule in the list of vcap rules */
	mutex_lock(&ri->admin->lock);
	ret = vcap_insert_rule(ri, &move);
	if (ret < 0) {
		pr_err("%s:%d: could not insert rule in vcap list: %d\n", __func__, __LINE__, ret);
		goto out;
	}
	if (move.count > 0) {
		ret = vcap_move_rules(ri, &move);
		if (ret) {
			pr_err("%s:%d: rule move error: %d\n", __func__, __LINE__, ret);
			goto out;
		}
	}
	/* Encode the bitstreams to the VCAP cache */
	ret = vcap_encode_rule(ri);
	if (ret) {
		pr_err("%s:%d: rule encoding error: %d\n", __func__, __LINE__, ret);
		goto out;
	}

	/* Write the bitstreams to the VCAP cache */
	ret = vcap_write_rule(ri);
	if (ret)
		pr_err("%s:%d: rule write error: %d\n", __func__, __LINE__, ret);
	/* Set the counter to zero */
	ret = vcap_write_counter(ri, &ctr);
out:
	mutex_unlock(&ri->admin->lock);
	return ret;
}

/* Allocate a new rule with the provided arguments */
struct vcap_rule *vcap_alloc_rule(struct net_device *ndev, int vcap_chain_id,
				  enum vcap_user user, u16 priority,
				  u32 id)
{
	struct vcap_rule_internal *ri;
	struct vcap_admin *admin;
	int maxsize;
	int err;

	if (!ndev) {
		pr_err("%s:%d: netdev is missing\n", __func__, __LINE__);
		return ERR_PTR(-ENODEV);
	}
	err = vcap_api_check(vctrl);
	if (err)
		return ERR_PTR(err);
	/* Get the VCAP instance */
	admin = _vcap_find_admin(vcap_chain_id);
	if (!admin) {
		pr_err("%s:%d: no vcap admin: cid: %d\n",
		       __func__, __LINE__, vcap_chain_id);
		return ERR_PTR(-ENOENT);
	}
	/* Sanity check that this VCAP is supported on this platform */
	if (vctrl->vcaps[admin->vtype].rows == 0) {
		pr_err("%s:%d: vcap type is not available: %d\n",
		       __func__, __LINE__, admin->vtype);
		return ERR_PTR(-EINVAL);
	}
	/* Check if a rule with this id already exists */
	if (vcap_lookup_rule(id)) {
		pr_err("%s:%d: rule already exists: %u\n",
		       __func__, __LINE__, id);
		return ERR_PTR(-EEXIST);
	}
	/* Check if there is room for the rule in the block(s) of the VCAP */
	maxsize = vctrl->vcaps[admin->vtype].sw_count; /* worst case rule size */
	if (vcap_rule_space(admin, maxsize))
		return ERR_PTR(-ENOSPC);
	/* Create a container for the rule and return it */
	ri = kzalloc(sizeof(*ri), GFP_KERNEL);
	if (ri) {
		ri->data.vcap_chain_id = vcap_chain_id;
		ri->data.user = user;
		ri->data.priority = priority;
		ri->data.id = id;
		ri->data.keyset = VCAP_KFS_NO_VALUE;
		ri->data.actionset = VCAP_AFS_NO_VALUE;
		INIT_LIST_HEAD(&ri->list);
		INIT_LIST_HEAD(&ri->data.keyfields);
		INIT_LIST_HEAD(&ri->data.actionfields);
		ri->ndev = ndev;
		ri->admin = admin; /* refer to the vcap instance */
		if (vcap_set_rule_id(ri) == 0) {
			goto out_free;
		}
		vcap_erase_cache(ri);
	} else {
		pr_err("%s:%d: could not allocate rule for vcap type: %d, instance: %d\n",
		       __func__, __LINE__, admin->vtype, admin->vinst);
		return ERR_PTR(-ENOMEM);
	}
	return (struct vcap_rule *)ri;

out_free:
	kfree(ri);
	pr_err("%s:%d: could not assign a rule id\n", __func__, __LINE__);
	return ERR_PTR(-EINVAL);
}

/* Free mem of a rule owned by client after the rule as been added to the VCAP */
void vcap_free_rule(struct vcap_rule *rule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	struct vcap_client_keyfield *ckf, *next_ckf;
	struct vcap_client_actionfield *caf, *next_caf;

	/* Deallocate the list of keys and actions */
	list_for_each_entry_safe(ckf, next_ckf, &ri->data.keyfields, ctrl.list) {
		list_del(&ckf->ctrl.list);
		kfree(ckf);
	}
	list_for_each_entry_safe(caf, next_caf, &ri->data.actionfields, ctrl.list) {
		list_del(&caf->ctrl.list);
		kfree(caf);
	}
	/* Deallocate the rule */
	kfree(rule);
}

/* Update existing rule and transfer ownership of rule to VCAP library */
int vcap_mod_rule(struct vcap_rule *rule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	struct vcap_admin *admin = ri->admin;
	struct vcap_rule_internal *elem;
	struct vcap_counter ctr = {0};
	int ret;

	ret = vcap_api_check(vctrl);
	if (ret)
		return ret;
	mutex_lock(&ri->admin->lock);
	list_for_each_entry(elem, &admin->rules, list) {
		if (ri->data.id == elem->data.id) {
			ret = 0;
			break;
		}
	}
	if (ret < 0) {
		pr_err("%s:%d: could not find rule: %u\n", __func__, __LINE__,
		       rule->id);
		goto out;
	}
	if (elem->data.vcap_chain_id != ri->data.vcap_chain_id ||
	    elem->data.user != ri->data.user ||
	    elem->data.priority != ri->data.priority ||
	    elem->data.keyset != ri->data.keyset ||
	    elem->data.actionset != ri->data.actionset) {
		pr_err("%s:%d: rule %u was modified beyond the fields\n",
		       __func__, __LINE__, rule->id);
		ret = -EINVAL;
		goto out;
	}
	/* Encode the bitstreams to the VCAP cache */
	vcap_erase_cache(ri);
	ret = vcap_encode_rule(ri);
	if (ret) {
		pr_err("%s:%d: rule encoding error: %d\n", __func__, __LINE__,
		       ret);
		goto out;
	}

	/* Write the bitstreams to the VCAP cache */
	ret = vcap_write_rule(ri);
	if (ret)
		pr_err("%s:%d: rule write error: %d\n", __func__, __LINE__, ret);
	/* Set the counter to zero */
	ret = vcap_write_counter(ri, &ctr);
out:
	mutex_unlock(&ri->admin->lock);
	return ret;
}

/* Return the alignment offset for a new rule address */
static int vcap_valid_rule_move(struct vcap_rule_internal *ri, int offset)
{
	return (ri->addr + offset) % ri->size;
}

/* Update the rule address with an offset */
static void vcap_adjust_rule_addr(struct vcap_rule_internal *ri, int offset)
{
	ri->addr += offset;
	pr_debug("%s:%d: %#08x (%u, %u)\n", __func__, __LINE__,
		ri->sort_key, ri->size, ri->addr);
}

/* Delete rule in the VCAP Library */
int vcap_del_rule(struct net_device *ndev, u32 id)
{
	struct vcap_rule_internal *ri, *elem;
	struct vcap_rule_move move;
	struct vcap_admin *admin;
	int gap = 0, offset = 0;
	int err;

	if (!ndev) {
		pr_err("%s:%d: netdev is missing\n", __func__, __LINE__);
		return -ENODEV;
	}
	err = vcap_api_check(vctrl);
	if (err)
		return err;
	/* Look for the rule id in all vcaps */
	ri = vcap_lookup_rule(id);
	if (!ri) {
		pr_err("%s:%d: could not find rule: %u\n", __func__, __LINE__, id);
		return -EINVAL;
	}
	admin = ri->admin;
	pr_debug("%s:%d: deleting: %#08x (%u, %u): last_used_addr: %u\n",
		 __func__, __LINE__, ri->sort_key, ri->size, ri->addr, admin->last_used_addr);
	/* delete the rule in the cache */
	if (ri->addr > admin->last_used_addr) {
		/* Entries needs to be moved to fill the gap */
		elem = ri;
		if (list_is_first(&elem->list, &admin->rules)) {
			/* Move to the beginning of the VCAP */
			offset = admin->last_valid_addr + 1 - elem->addr - ri->size;
			pr_debug("%s:%d: initial rule gap: %d, offset: %d\n",
				 __func__, __LINE__, gap, offset);
		}
		/* locate gaps between odd size rules and adjust the move */
		list_for_each_entry_continue(elem, &admin->rules, list) {
			gap += vcap_valid_rule_move(elem, ri->size);
		}
		pr_debug("%s:%d: rule gap: %d\n", __func__, __LINE__, gap);
		elem = ri;
		list_for_each_entry_continue(elem, &admin->rules, list) {
			vcap_adjust_rule_addr(elem, ri->size + gap + offset);
		}
		move.addr = admin->last_used_addr;
		move.offset = -(ri->size + gap + offset);
		move.count = ri->addr - admin->last_used_addr - gap;
		pr_debug("%s:%d: final offset: %d\n", __func__, __LINE__, offset);
		vcap_move_rules(ri, &move);
	}
	list_del(&ri->list);

	pr_debug("%s:%d, after removal:\n", __func__, __LINE__);
	list_for_each_entry(elem, &admin->rules, list) {
		pr_debug("%s:%d: %#08x (%u, %u)\n",
			 __func__, __LINE__,
			 elem->sort_key, elem->size, elem->addr);
	}
	vctrl->ops->init(ri->ndev, admin, admin->last_used_addr,
			 ri->size + gap + offset);
	if (list_empty(&admin->rules)) {
		admin->last_used_addr = admin->last_valid_addr;
	} else {
		/* update the address range end marker from the last rule in the list */
		elem = list_last_entry(&admin->rules, struct vcap_rule_internal, list);
		admin->last_used_addr = elem->addr;
	}
	kfree(ri);
	return 0;
}

/* Delete all rules in the VCAP instance */
int vcap_del_rules(struct vcap_admin *admin)
{
	struct vcap_rule_internal *ri, *next_ri;
	int ret = vcap_api_check(vctrl);

	if (ret)
		return ret;
	list_for_each_entry_safe(ri, next_ri, &admin->rules, list) {
		pr_debug("%s:%d: addr: %d\n", __func__, __LINE__, ri->addr);
		vctrl->ops->init(ri->ndev, admin, ri->addr, ri->size);
		list_del(&ri->list);
		kfree(ri);
	}
	admin->last_used_addr = admin->last_valid_addr;
	return 0;
}


static void vcap_copy_from_client_keyfield(struct vcap_rule *rule,
					struct vcap_client_keyfield *field,
					struct vcap_client_keyfield_data *data)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	int size;

	if (!ri->admin->w32be) {
		memcpy(&field->data, data, sizeof(field->data));
		return;
	}
	size = keyfield_size_table[field->ctrl.type] / 2;
	switch (field->ctrl.type) {
	case VCAP_FIELD_BIT:
	case VCAP_FIELD_U32:
		memcpy(&field->data, data, sizeof(field->data));
		break;
	case VCAP_FIELD_U48:
		vcap_copy_to_w32be(field->data.u48.value, data->u48.value, size);
		vcap_copy_to_w32be(field->data.u48.mask,  data->u48.mask, size);
		break;
	case VCAP_FIELD_U56:
		vcap_copy_to_w32be(field->data.u56.value, data->u56.value, size);
		vcap_copy_to_w32be(field->data.u56.mask,  data->u56.mask, size);
		break;
	case VCAP_FIELD_U64:
		vcap_copy_to_w32be(field->data.u64.value, data->u64.value, size);
		vcap_copy_to_w32be(field->data.u64.mask,  data->u64.mask, size);
		break;
	case VCAP_FIELD_U72:
		vcap_copy_to_w32be(field->data.u72.value, data->u72.value, size);
		vcap_copy_to_w32be(field->data.u72.mask,  data->u72.mask, size);
		break;
	case VCAP_FIELD_U112:
		vcap_copy_to_w32be(field->data.u112.value, data->u112.value, size);
		vcap_copy_to_w32be(field->data.u112.mask,  data->u112.mask, size);
		break;
	case VCAP_FIELD_U128:
		vcap_copy_to_w32be(field->data.u128.value, data->u128.value, size);
		vcap_copy_to_w32be(field->data.u128.mask,  data->u128.mask, size);
		break;
	};
}

/* Find a client key field in a rule */
struct vcap_client_keyfield *
vcap_find_keyfield(struct vcap_rule *rule, enum vcap_key_field key)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	struct vcap_client_keyfield *ckf;

	list_for_each_entry(ckf, &ri->data.keyfields, ctrl.list)
		if (ckf->ctrl.key == key)
			return ckf;
	return 0;
}

static bool vcap_keyfield_unique(struct vcap_rule *rule, enum vcap_key_field key)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	const struct vcap_client_keyfield *ckf;

	list_for_each_entry(ckf, &ri->data.keyfields, ctrl.list)
		if (ckf->ctrl.key == key)
			return false;
	return true;
}

/* Find information on a key field in a rule */
const struct vcap_field *vcap_lookup_keyfield(struct vcap_rule *rule,
					      enum vcap_key_field key)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	enum vcap_keyfield_set keyset = rule->keyset;
	enum vcap_type vt = ri->admin->vtype;
	const struct vcap_field *fields;

	if (keyset == VCAP_KFS_NO_VALUE)
		return 0;
	fields = vcap_keyfields(vt, keyset);
	if (!fields)
		return 0;
	return &fields[key];
}

static bool vcap_keyfield_match_keyset(struct vcap_rule *rule,
				       enum vcap_key_field key)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	enum vcap_keyfield_set keyset = rule->keyset;
	enum vcap_type vt = ri->admin->vtype;
	const struct vcap_field *fields;

	/* the field is accepted if the rule has no keyset yet */
	if (keyset == VCAP_KFS_NO_VALUE)
		return true;
	fields = vcap_keyfields(vt, keyset);
	if (!fields)
		return false;
	/* if there is a width there is a way */
	return fields[key].width > 0;
}

int vcap_rule_add_key(struct vcap_rule *rule,
		      enum vcap_key_field key,
		      enum vcap_field_type ftype,
		      struct vcap_client_keyfield_data *data)
{
	struct vcap_client_keyfield *field;

	if (!vcap_keyfield_unique(rule, key)) {
		pr_warn("%s:%d: key [%d] %s is already in the rule\n",
		       __func__, __LINE__, key,
		       vctrl->stats->keyfield_names[key]);
		return -EINVAL;
	}
	if (!vcap_keyfield_match_keyset(rule, key)) {
		pr_err("%s:%d: key [%d] %s does not belong in the rule keyset\n",
		       __func__, __LINE__, key,
		       vctrl->stats->keyfield_names[key]);
		return -EINVAL;
	}
	field = kzalloc(sizeof(struct vcap_client_keyfield), GFP_KERNEL);
	if (!field)
		return -ENOMEM;
	field->ctrl.key = key;
	field->ctrl.type = ftype;
	vcap_copy_from_client_keyfield(rule, field, data);
	list_add_tail(&field->ctrl.list, &rule->keyfields);
	return 0;
}

int vcap_rule_mod_key(struct vcap_rule *rule,
		      enum vcap_key_field key,
		      enum vcap_field_type ftype,
		      struct vcap_client_keyfield_data *data)
{
	struct vcap_client_keyfield *field;

	field = vcap_find_keyfield(rule, key);
	if (!field)
		return vcap_rule_add_key(rule, key, ftype, data);
	vcap_copy_from_client_keyfield(rule, field, data);
	return 0;
}

static void vcap_rule_set_key_bitsize(struct vcap_u1_key *u1, enum vcap_bit val)
{
	switch (val) {
	case VCAP_BIT_0:
		u1->value = 0;
		u1->mask = 1;
		break;
	case VCAP_BIT_1:
		u1->value = 1;
		u1->mask = 1;
		break;
	case VCAP_BIT_ANY:
		u1->value = 0;
		u1->mask = 0;
		break;
	}
}

/* Add a bit key with value and mask to the rule */
int vcap_rule_add_key_bit(struct vcap_rule *rule, enum vcap_key_field key,
			  enum vcap_bit val)
{
	struct vcap_client_keyfield_data data;

	vcap_rule_set_key_bitsize(&data.u1, val);
	return vcap_rule_add_key(rule, key, VCAP_FIELD_BIT, &data);
}

/* Add a 32 bit key field with value and mask to the rule */
int vcap_rule_add_key_u32(struct vcap_rule *rule, enum vcap_key_field key,
			     u32 value, u32 mask)
{
	struct vcap_client_keyfield_data data;

	data.u32.value = value;
	data.u32.mask = mask;
	return vcap_rule_add_key(rule, key, VCAP_FIELD_U32, &data);
}

/* Add a 48 bit key with value and mask to the rule */
int vcap_rule_add_key_u48(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u48_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u48, fieldval, sizeof(data.u48));
	return vcap_rule_add_key(rule, key, VCAP_FIELD_U48, &data);
}

/* Add a 56 bit key with value and mask to the rule */
int vcap_rule_add_key_u56(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u56_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u56, fieldval, sizeof(data.u56));
	return vcap_rule_add_key(rule, key, VCAP_FIELD_U56, &data);
}

/* Add a 64 bit key with value and mask to the rule */
int vcap_rule_add_key_u64(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u64_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u64, fieldval, sizeof(data.u64));
	return vcap_rule_add_key(rule, key, VCAP_FIELD_U64, &data);
}

/* Add a 72 bit key with value and mask to the rule */
int vcap_rule_add_key_u72(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u72_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u72, fieldval, sizeof(data.u72));
	return vcap_rule_add_key(rule, key, VCAP_FIELD_U72, &data);
}

/* Add a 112 bit key with value and mask to the rule */
int vcap_rule_add_key_u112(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u112_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u112, fieldval, sizeof(data.u112));
	return vcap_rule_add_key(rule, key, VCAP_FIELD_U112, &data);
}

/* Add a 128 bit key with value and mask to the rule */
int vcap_rule_add_key_u128(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u128_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u128, fieldval, sizeof(data.u128));
	return vcap_rule_add_key(rule, key, VCAP_FIELD_U128, &data);
}

/* Modify a bit key with value and mask in the rule */
int vcap_rule_mod_key_bit(struct vcap_rule *rule, enum vcap_key_field key,
			  enum vcap_bit val)
{
	struct vcap_client_keyfield_data data;

	vcap_rule_set_key_bitsize(&data.u1, val);
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_BIT, &data);
}

/* Modify a 32 bit key field with value and mask in the rule */
int vcap_rule_mod_key_u32(struct vcap_rule *rule, enum vcap_key_field key,
			     u32 value, u32 mask)
{
	struct vcap_client_keyfield_data data;

	data.u32.value = value;
	data.u32.mask = mask;
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_U32, &data);
}

/* Modify a 48 bit key with value and mask in the rule */
int vcap_rule_mod_key_u48(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u48_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u48, fieldval, sizeof(data.u48));
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_U48, &data);
}

/* Modify a 56 bit key with value and mask in the rule */
int vcap_rule_mod_key_u56(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u56_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u56, fieldval, sizeof(data.u56));
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_U56, &data);
}

/* Modify a 64 bit key with value and mask in the rule */
int vcap_rule_mod_key_u64(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u64_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u64, fieldval, sizeof(data.u64));
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_U64, &data);
}

/* Modify a 72 bit key with value and mask in the rule */
int vcap_rule_mod_key_u72(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u72_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u72, fieldval, sizeof(data.u72));
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_U72, &data);
}

/* Modify a 112 bit key with value and mask in the rule */
int vcap_rule_mod_key_u112(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u112_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u112, fieldval, sizeof(data.u112));
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_U112, &data);
}

/* Modify a 128 bit key with value and mask in the rule */
int vcap_rule_mod_key_u128(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u128_key *fieldval)
{
	struct vcap_client_keyfield_data data;

	memcpy(&data.u128, fieldval, sizeof(data.u128));
	return vcap_rule_mod_key(rule, key, VCAP_FIELD_U128, &data);
}

/* Remove a key field with value and mask in the rule */
int vcap_rule_rem_key(struct vcap_rule *rule, enum vcap_key_field key)
{
	struct vcap_client_keyfield *field;

	field = vcap_find_keyfield(rule, key);
	if (!field) {
		pr_err("%s:%d: key %d is not in the rule\n",
		       __func__, __LINE__, key);
		return -EINVAL;
	}
	/* Deallocate the key field */
	list_del(&field->ctrl.list);
	kfree(field);
	return 0;
}

static void vcap_copy_actionfield_to_w32be(struct vcap_rule *rule,
					   struct vcap_client_actionfield *field,
					   struct vcap_client_actionfield_data *data)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	int size;

	if (!ri->admin->w32be) {
		memcpy(&field->data, data, sizeof(field->data));
		return;
	}
	size = actionfield_size_table[field->ctrl.type];
	switch (field->ctrl.type) {
	case VCAP_FIELD_BIT:
	case VCAP_FIELD_U32:
		memcpy(&field->data, data, sizeof(field->data));
		break;
	case VCAP_FIELD_U48:
		vcap_copy_to_w32be(field->data.u48.value, data->u48.value, size);
		break;
	case VCAP_FIELD_U56:
		vcap_copy_to_w32be(field->data.u56.value, data->u56.value, size);
		break;
	case VCAP_FIELD_U64:
		vcap_copy_to_w32be(field->data.u64.value, data->u64.value, size);
		break;
	case VCAP_FIELD_U72:
		vcap_copy_to_w32be(field->data.u72.value, data->u72.value, size);
		break;
	case VCAP_FIELD_U112:
		vcap_copy_to_w32be(field->data.u112.value, data->u112.value, size);
		break;
	case VCAP_FIELD_U128:
		vcap_copy_to_w32be(field->data.u128.value, data->u128.value, size);
		break;
	};
}

/* Find a client action field in a rule */
struct vcap_client_actionfield *
vcap_find_actionfield(struct vcap_rule *rule, enum vcap_action_field act)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	struct vcap_client_actionfield *caf;

	list_for_each_entry(caf, &ri->data.actionfields, ctrl.list)
		if (caf->ctrl.action == act)
			return caf;
	return 0;
}

static bool vcap_actionfield_unique(struct vcap_rule *rule, enum vcap_action_field act)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	const struct vcap_client_actionfield *caf;

	list_for_each_entry(caf, &ri->data.actionfields, ctrl.list)
		if (caf->ctrl.action == act)
			return false;
	return true;
}

/* Find information on a action field in a rule */
const struct vcap_field *vcap_lookup_actionfield(struct vcap_rule *rule,
					      enum vcap_action_field action)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	enum vcap_actionfield_set actionset = rule->actionset;
	enum vcap_type vt = ri->admin->vtype;
	const struct vcap_field *fields;

	if (actionset == VCAP_AFS_NO_VALUE)
		return 0;
	fields = vcap_actionfields(vt, actionset);
	if (!fields)
		return 0;
	return &fields[action];
}

static bool vcap_actionfield_match_actionset(struct vcap_rule *rule,
					     enum vcap_action_field action)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	enum vcap_actionfield_set actionset = rule->actionset;
	enum vcap_type vt = ri->admin->vtype;
	const struct vcap_field *fields;

	/* the field is accepted if the rule has no keyset yet */
	if (actionset == VCAP_AFS_NO_VALUE)
		return true;
	fields = vcap_actionfields(vt, actionset);
	if (!fields)
		return false;
	/* if there is a width there is a way */
	return fields[action].width > 0;
}

int vcap_rule_add_action(struct vcap_rule *rule,
			 enum vcap_action_field action,
			 enum vcap_field_type ftype,
			 struct vcap_client_actionfield_data *data)
{
	struct vcap_client_actionfield *field;

	if (!vcap_actionfield_unique(rule, action)) {
		pr_warn("%s:%d: action %d is already in the rule\n",
		       __func__, __LINE__, action);
		return -EINVAL;
	}
	if (!vcap_actionfield_match_actionset(rule, action)) {
		pr_err("%s:%d: action %d does not belong in the rule actionset\n",
		       __func__, __LINE__, action);
		return -EINVAL;
	}
	field = kzalloc(sizeof(struct vcap_client_actionfield), GFP_KERNEL);
	if (!field)
		return -ENOMEM;
	field->ctrl.action = action;
	field->ctrl.type = ftype;
	vcap_copy_actionfield_to_w32be(rule, field, data);
	list_add_tail(&field->ctrl.list, &rule->actionfields);
	return 0;
}

int vcap_rule_mod_action(struct vcap_rule *rule,
			 enum vcap_action_field action,
			 enum vcap_field_type ftype,
			 struct vcap_client_actionfield_data *data)
{
	struct vcap_client_actionfield *field;

	field = vcap_find_actionfield(rule, action);
	if (!field)
		return vcap_rule_add_action(rule, action, ftype, data);
	vcap_copy_actionfield_to_w32be(rule, field, data);
	return 0;
}

static void vcap_rule_set_action_bitsize(struct vcap_u1_action *u1,
					 enum vcap_bit val)
{
	switch (val) {
	case VCAP_BIT_0:
		u1->value = 0;
		break;
	case VCAP_BIT_1:
		u1->value = 1;
		break;
	case VCAP_BIT_ANY:
		u1->value = 0;
		break;
	}
}

/* Add a bit action with value to the rule */
int vcap_rule_add_action_bit(struct vcap_rule *rule, enum vcap_action_field action,
			     enum vcap_bit val)
{
	struct vcap_client_actionfield_data data;

	vcap_rule_set_action_bitsize(&data.u1, val);
	return vcap_rule_add_action(rule, action, VCAP_FIELD_BIT, &data);
}

/* Add a 32 bit action field with value to the rule */
int vcap_rule_add_action_u32(struct vcap_rule *rule, enum vcap_action_field action,
			     u32 value)
{
	struct vcap_client_actionfield_data data;

	data.u32.value = value;
	return vcap_rule_add_action(rule, action, VCAP_FIELD_U32, &data);
}

/* Add a 48 bit action with value to the rule */
int vcap_rule_add_action_u48(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u48_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u48, fieldval, sizeof(data.u48));
	return vcap_rule_add_action(rule, action, VCAP_FIELD_U48, &data);
}

/* Add a 56 bit action with value to the rule */
int vcap_rule_add_action_u56(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u56_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u56, fieldval, sizeof(data.u56));
	return vcap_rule_add_action(rule, action, VCAP_FIELD_U56, &data);
}

/* Add a 64 bit action with value to the rule */
int vcap_rule_add_action_u64(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u64_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u64, fieldval, sizeof(data.u64));
	return vcap_rule_add_action(rule, action, VCAP_FIELD_U64, &data);
}

/* Add a 72 bit action with value to the rule */
int vcap_rule_add_action_u72(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u72_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u72, fieldval, sizeof(data.u72));
	return vcap_rule_add_action(rule, action, VCAP_FIELD_U72, &data);
}

/* Add a 112 bit action with value to the rule */
int vcap_rule_add_action_u112(struct vcap_rule *rule, enum vcap_action_field action,
			      struct vcap_u112_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u112, fieldval, sizeof(data.u112));
	return vcap_rule_add_action(rule, action, VCAP_FIELD_U112, &data);
}

/* Add a 128 bit action with value to the rule */
int vcap_rule_add_action_u128(struct vcap_rule *rule, enum vcap_action_field action,
			      struct vcap_u128_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u128, fieldval, sizeof(data.u128));
	return vcap_rule_add_action(rule, action, VCAP_FIELD_U128, &data);
}

/* Modify a bit action with value in the rule */
int vcap_rule_mod_action_bit(struct vcap_rule *rule, enum vcap_action_field action,
			     enum vcap_bit val)
{
	struct vcap_client_actionfield_data data;

	vcap_rule_set_action_bitsize(&data.u1, val);
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_BIT, &data);
}

/* Modify a 32 bit action field with value in the rule */
int vcap_rule_mod_action_u32(struct vcap_rule *rule, enum vcap_action_field action,
			     u32 value)
{
	struct vcap_client_actionfield_data data;

	data.u32.value = value;
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_U32, &data);
}

/* Modify a 48 bit action with value in the rule */
int vcap_rule_mod_action_u48(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u48_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u48, fieldval, sizeof(data.u48));
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_U48, &data);
}

/* Modify a 56 bit action with value in the rule */
int vcap_rule_mod_action_u56(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u56_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u56, fieldval, sizeof(data.u56));
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_U56, &data);
}

/* Modify a 64 bit action with value in the rule */
int vcap_rule_mod_action_u64(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u64_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u64, fieldval, sizeof(data.u64));
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_U64, &data);
}

/* Modify a 72 bit action with value in the rule */
int vcap_rule_mod_action_u72(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u72_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u72, fieldval, sizeof(data.u72));
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_U72, &data);
}

/* Modify a 112 bit action with value in the rule */
int vcap_rule_mod_action_u112(struct vcap_rule *rule, enum vcap_action_field action,
			      struct vcap_u112_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u112, fieldval, sizeof(data.u112));
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_U112, &data);
}

/* Modify a 128 bit action with value in the rule */
int vcap_rule_mod_action_u128(struct vcap_rule *rule, enum vcap_action_field action,
			      struct vcap_u128_action *fieldval)
{
	struct vcap_client_actionfield_data data;

	memcpy(&data.u128, fieldval, sizeof(data.u128));
	return vcap_rule_mod_action(rule, action, VCAP_FIELD_U128, &data);
}

/* Remove an action field with value in the rule */
int vcap_rule_rem_action(struct vcap_rule *rule, enum vcap_action_field action)
{
	struct vcap_client_actionfield *field;

	field = vcap_find_actionfield(rule, action);
	if (!field) {
		pr_err("%s:%d: action %d is not in the rule\n",
		       __func__, __LINE__, action);
		return -EINVAL;
	}
	/* Deallocate the action field */
	list_del(&field->ctrl.list);
	kfree(field);
	return 0;
}

int vcap_rule_set_counter(u32 id, struct vcap_counter *ctr)
{
	struct vcap_rule_internal *ri;
	int ret;

	ret = vcap_api_check(vctrl);
	if (ret)
		return ret;
	/* Look for the rule id in all vcaps */
	ri = vcap_lookup_rule(id);
	if (!ri) {
		pr_err("%s:%d: could not find rule: %u\n", __func__, __LINE__, id);
		return -EINVAL;
	}
	return vcap_write_counter(ri, ctr);
}

int vcap_rule_get_counter(u32 id, struct vcap_counter *ctr)
{
	struct vcap_rule_internal *ri;
	int ret;

	ret = vcap_api_check(vctrl);
	if (ret)
		return ret;
	/* Look for the rule id in all vcaps */
	ri = vcap_lookup_rule(id);
	if (!ri) {
		pr_err("%s:%d: could not find rule: %u\n", __func__, __LINE__, id);
		return -EINVAL;
	}
	return vcap_read_counter(ri, ctr);
}

/* Drop keys in a keylist and any keys that are not supported by the keyset */
int vcap_filter_rule_keys(struct vcap_rule *rule,
			  enum vcap_key_field keylist[], int length,
			  bool drop_unsupported)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	struct vcap_client_keyfield *ckf, *next_ckf;
	const struct vcap_field *fields;
	enum vcap_key_field key;
	int err = 0;
	int idx;

	if (length > 0) {
		err = -EEXIST;
		list_for_each_entry_safe(ckf, next_ckf,
					 &ri->data.keyfields, ctrl.list) {
			key = ckf->ctrl.key;
			for (idx = 0; idx < length; ++idx)
				if (key == keylist[idx]) {
					list_del(&ckf->ctrl.list);
					kfree(ckf);
					idx++;
					err = 0;
				}
		}
	}
	if (drop_unsupported) {
		err = -EEXIST;
		fields = vcap_keyfields(ri->admin->vtype, rule->keyset);
		if (!fields)
			return err;
		list_for_each_entry_safe(ckf, next_ckf,
					 &ri->data.keyfields, ctrl.list) {
			key = ckf->ctrl.key;
			if (fields[key].width > 0) {
				list_del(&ckf->ctrl.list);
				kfree(ckf);
				err = 0;
			}
		}
	}
	return err;
}

/* Make a full copy of an existing rule with a new rule id */
struct vcap_rule *vcap_copy_rule(struct vcap_rule *erule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)erule;
	struct vcap_client_actionfield *caf;
	struct vcap_client_keyfield *ckf;
	struct vcap_rule *rule;
	int err;

	err = vcap_api_check(vctrl);
	if (err)
		return ERR_PTR(err);
	rule = vcap_alloc_rule(ri->ndev, ri->data.vcap_chain_id,
			       ri->data.user, ri->data.priority, 0);
	if (IS_ERR(rule))
		return rule;

	list_for_each_entry(ckf, &ri->data.keyfields, ctrl.list) {
		/* Add a key duplicate in the new rule */
		err = vcap_rule_add_key(rule,
					ckf->ctrl.key,
					ckf->ctrl.type,
					&ckf->data);
		if (err)
			goto err;
	}
	list_for_each_entry(caf, &ri->data.actionfields, ctrl.list) {
		/* Add a action duplicate in the new rule */
		err = vcap_rule_add_action(rule,
					caf->ctrl.action,
					caf->ctrl.type,
					&caf->data);
		if (err)
			goto err;
	}
	return rule;
err:
	vcap_free_rule(rule);
	return ERR_PTR(err);
}

int vcap_rule_get_address(struct net_device *ndev, u32 id,
			  struct vcap_address *addr)
{
	struct vcap_rule_internal *ri;
	int ret = vcap_api_check(vctrl);

	if (ret)
		return ret;

	/* Look for the rule id in all vcaps */
	ri = vcap_lookup_rule(id);
	if (!ri || !addr) {
		pr_err("%s:%d: could not find rule: %u\n", __func__, __LINE__, id);
		return -EINVAL;
	}
	addr->start = ri->addr;
	addr->size = ri->size;
	return 0;
}

static void vcap_show_admin_rule_keyfield(int (*pf)(void *ct, const char *f, ...),
					  void *ct,
					  const struct vcap_field *vfield,
					  struct vcap_client_keyfield *field)
{
	char fbuffer[400] = {0}, *buffer = fbuffer;
	u8 *value = 0, *mask = 0;

	switch (field->ctrl.type)
	{
	case VCAP_FIELD_BIT:
		buffer += sprintf(buffer, "bit");
		value = (u8 *)&field->data.u1.value;
		mask = (u8 *)&field->data.u1.mask;
		break;
	case VCAP_FIELD_U32:
		buffer += sprintf(buffer, "u32 (%u)", field->data.u32.value);
		value = (u8 *)&field->data.u32.value;
		mask = (u8 *)&field->data.u32.mask;
		break;
	case VCAP_FIELD_U48:
		buffer += sprintf(buffer, "u48");
		value = field->data.u48.value;
		mask = field->data.u48.mask;
		break;
	case VCAP_FIELD_U56:
		buffer += sprintf(buffer, "u56");
		value = field->data.u56.value;
		mask = field->data.u56.mask;
		break;
	case VCAP_FIELD_U64:
		buffer += sprintf(buffer, "u64");
		value = field->data.u64.value;
		mask = field->data.u64.mask;
		break;
	case VCAP_FIELD_U72:
		buffer += sprintf(buffer, "u72");
		value = field->data.u72.value;
		mask = field->data.u72.mask;
		break;
	case VCAP_FIELD_U112:
		buffer += sprintf(buffer, "u112");
		value = field->data.u112.value;
		mask = field->data.u112.mask;
		break;
	case VCAP_FIELD_U128:
		buffer += sprintf(buffer, "u128");
		value = field->data.u128.value;
		mask = field->data.u128.mask;
		break;
	}
	buffer += sprintf(buffer, ": ");
	if (value) {
		buffer = vcap_bitarray_tostring(buffer, vfield->width, value);
		*buffer++ = '/';
		buffer = vcap_bitarray_tostring(buffer, vfield->width, mask);
		*buffer++ = 0;
	}
	pf(ct, "    %s: %s\n", vctrl->stats->keyfield_names[field->ctrl.key],
	   fbuffer);
}

static void vcap_show_admin_rule_actionfield(int (*pf)(void *ct, const char *f, ...),
					     void *ct,
					     const struct vcap_field *vfield,
					     struct vcap_client_actionfield *field)
{
	char fbuffer[200] = {0}, *buffer = fbuffer;
	u8 *value = 0;

	switch (field->ctrl.type)
	{
	case VCAP_FIELD_BIT:
		buffer += sprintf(buffer, "bit");
		value = (u8 *)&field->data.u1.value;
		break;
	case VCAP_FIELD_U32:
		buffer += sprintf(buffer, "u32 (%u)", field->data.u32.value);
		value = (u8 *)&field->data.u32.value;
		break;
	case VCAP_FIELD_U48:
		buffer += sprintf(buffer, "u48");
		value = field->data.u48.value;
		break;
	case VCAP_FIELD_U56:
		buffer += sprintf(buffer, "u56");
		value = field->data.u56.value;
		break;
	case VCAP_FIELD_U64:
		buffer += sprintf(buffer, "u64");
		value = field->data.u64.value;
		break;
	case VCAP_FIELD_U72:
		buffer += sprintf(buffer, "u72");
		value = field->data.u72.value;
		break;
	case VCAP_FIELD_U112:
		buffer += sprintf(buffer, "u112");
		value = field->data.u112.value;
		break;
	case VCAP_FIELD_U128:
		buffer += sprintf(buffer, "u128");
		value = field->data.u128.value;
		break;
	}
	buffer += sprintf(buffer, ": ");
	if (value) {
		buffer = vcap_bitarray_tostring(buffer, vfield->width, value);
		*buffer++ = 0;
	}
	pf(ct, "    %s: %s\n", vctrl->stats->actionfield_names[field->ctrl.action],
	   fbuffer);
}

static void vcap_show_admin_rule_keys(int (*pf)(void *ct, const char *f, ...),
				      void *ct,
				      struct vcap_admin *admin,
				      struct vcap_rule_internal *ri)
{
	struct vcap_client_keyfield *ckf;
	const struct vcap_field *keyfields;

	pf(ct, "  keyfields:\n");
	keyfields = vctrl->vcaps[admin->vtype].keyfield_set_map[ri->data.keyset];
	list_for_each_entry(ckf, &ri->data.keyfields, ctrl.list) {
		vcap_show_admin_rule_keyfield(pf, ct, &keyfields[ckf->ctrl.key], ckf);
	}
}

static void vcap_show_admin_rule_actions(int (*pf)(void *ct, const char *f, ...),
					 void *ct,
					 struct vcap_admin *admin,
					 struct vcap_rule_internal *ri)
{
	struct vcap_client_actionfield *caf;
	const struct vcap_field *actfields;

	pf(ct, "  actionfields:\n");
	actfields = vctrl->vcaps[admin->vtype].actionfield_set_map[ri->data.actionset];
	list_for_each_entry(caf, &ri->data.actionfields, ctrl.list) {
		vcap_show_admin_rule_actionfield(pf, ct, &actfields[caf->ctrl.action], caf);
	}
}

void vcap_show_rule(int (*pf)(void *ct, const char *f, ...),
		    void *ct,
		    struct vcap_admin *admin,
		    struct vcap_rule *rule)
{
	struct vcap_rule_internal *ri = (struct vcap_rule_internal *)rule;
	struct vcap_client_actionfield *caf;
	struct vcap_client_keyfield *ckf;
	const struct vcap_field *field;

	pf(ct, "  id: %u\n", ri->data.id);
	pf(ct, "  vcap_chain_id: %d\n", ri->data.vcap_chain_id);
	pf(ct, "  size: X%d\n", ri->size);
	if (ri->data.keyset == VCAP_KFS_NO_VALUE)
		pf(ct, "  keyset: no value\n");
	else
		pf(ct, "  keyset [%d]: %s\n", ri->data.keyset,
		   vctrl->stats->keyfield_set_names[ri->data.keyset]);
	list_for_each_entry(ckf, &ri->data.keyfields, ctrl.list) {
		field = vcap_find_keyfield_info(admin, ckf->ctrl.key);
		if (field)
			vcap_show_admin_rule_keyfield(pf, ct, field, ckf);
	}
	if (ri->data.actionset == VCAP_AFS_NO_VALUE)
		pf(ct, "  actionset: no value\n");
	else
		pf(ct, "  actionset[%d]: %s\n",  ri->data.actionset,
		   vctrl->stats->actionfield_set_names[ri->data.actionset]);
	list_for_each_entry(caf, &ri->data.actionfields, ctrl.list) {
		field = vcap_find_actionfield_info(admin, caf->ctrl.action);
		if (field) {
			vcap_show_admin_rule_actionfield(pf, ct, field, caf);
		}
	}
}

static void vcap_show_admin_rule(int (*pf)(void *ct, const char *f, ...),
				 void *ct,
				 struct vcap_admin *admin,
				 struct vcap_rule_internal *ri)
{

	pf(ct, "rule: %u, addr: [%d,%d], counter[%d]: %d, hit: %d\n",
	   ri->data.id, ri->addr, ri->addr + ri->size - 1, ri->counter_id,
	   ri->counter.value, ri->counter.sticky);
	pf(ct, "  id: %u\n", ri->data.id);
	pf(ct, "  vcap_chain_id: %d\n", ri->data.vcap_chain_id);
	pf(ct, "  user: %d\n", ri->data.user);
	pf(ct, "  priority: %d\n", ri->data.priority);
	pf(ct, "  keyset: %s\n", vctrl->stats->keyfield_set_names[ri->data.keyset]);
	pf(ct, "  actionset: %s\n",  vctrl->stats->actionfield_set_names[ri->data.actionset]);
	pf(ct, "  sort_key: 0x%08x\n", ri->sort_key);
	pf(ct, "  keyset_sw: %d\n", ri->keyset_sw);
	pf(ct, "  actionset_sw: %d\n", ri->actionset_sw);
	pf(ct, "  keyset_sw_regs: %d\n", ri->keyset_sw_regs);
	pf(ct, "  actionset_sw_regs: %d\n", ri->actionset_sw_regs);
	pf(ct, "  size: %d\n", ri->size);
	pf(ct, "  addr: %d\n", ri->addr);
	vcap_show_admin_rule_keys(pf, ct, admin, ri);
	vcap_show_admin_rule_actions(pf, ct, admin, ri);
	pf(ct, "  counter: %d\n", ri->counter.value);
	pf(ct, "  counter_sticky: %d\n", ri->counter.sticky);
}

int vcap_show_admin_info(int (*pf)(void *ct, const char *fmt, ...), void *ct,
			 struct vcap_admin *admin)
{
	const struct vcap_info *vcap = &vctrl->vcaps[admin->vtype];
	int ret = 0;

	pf(ct, "name: %s\n", vcap->name);
	pf(ct, "rows: %d\n", vcap->rows);
	pf(ct, "sw_count: %d\n", vcap->sw_count);
	pf(ct, "sw_width: %d\n", vcap->sw_width);
	pf(ct, "sticky_width: %d\n", vcap->sticky_width);
	pf(ct, "act_width: %d\n", vcap->act_width);
	pf(ct, "default_cnt: %d\n", vcap->default_cnt);
	pf(ct, "require_cnt_dis: %d\n", vcap->require_cnt_dis);
	pf(ct, "version: %d\n", vcap->version);
	pf(ct, "vtype: %d\n", admin->vtype);
	pf(ct, "vinst: %d\n", admin->vinst);
	pf(ct, "first_cid: %d\n", admin->first_cid);
	pf(ct, "last_cid: %d\n", admin->last_cid);
	pf(ct, "lookups: %d\n", admin->lookups);
	pf(ct, "first_valid_addr: %d\n", admin->first_valid_addr);
	pf(ct, "last_valid_addr: %d\n", admin->last_valid_addr);
	pf(ct, "last_used_addr: %d\n", admin->last_used_addr);
	return ret;
}

int vcap_show_admin(int (*pf)(void *ct, const char *fmt, ...), void *ct,
		    struct vcap_admin *admin)
{
	struct vcap_rule_internal *elem, *ri;
	int ret;

	ret = vcap_show_admin_info(pf, ct, admin);
	mutex_lock(&admin->lock);
	list_for_each_entry(elem, &admin->rules, list) {
		ri = vcap_dup_rule(elem);
		if (IS_ERR(ri))
			goto free_rule;
		/* Read data from VCAP */
		ret = vcap_read_rule(ri);
		if (ret)
			goto free_rule;
		/* Decode key and mask stream data and add fields to the rule */
		ret = vcap_decode_rule_keyset(ri);
		if (ret)
			goto free_rule;
		ret = vcap_decode_rule_actionset(ri);
		if (ret)
			goto free_rule;
		vcap_decode_rule_counter(ri);
		vcap_show_admin_rule(pf, ct, admin, ri);
free_rule:
		vcap_free_rule((struct vcap_rule *)ri);
	}
	mutex_unlock(&admin->lock);
	return ret;
}

int vcap_show_admin_raw(int (*pf)(void *ct, const char *fmt, ...), void *ct,
			struct vcap_admin *admin)
{
	enum vcap_type vt = admin->vtype;
	struct vcap_rule_internal *ri;
	const struct vcap_set *info;
	int keyset;
	int addr;
	int ret;

	if (list_empty(&admin->rules))
		return 0;

	ret = vcap_api_check(vctrl);
	if (ret)
		return ret;

	ri = list_first_entry(&admin->rules, struct vcap_rule_internal, list);

	/* Go from higher to lower addresses searching for a keyset */
	for (addr = admin->last_valid_addr; addr >= admin->first_valid_addr;
	     --addr) {
		keyset = vcap_addr_keyset(ri->ndev, admin,  addr);
		if (keyset >= 0) {
			info = vcap_keyfieldset(vt, keyset);
			if (info) {
				if (addr % info->sw_per_item)
					pr_info("%s:%d: addr: %d X%d error rule, keyset: %s (%d)\n",
						__func__, __LINE__,
						addr,
						info->sw_per_item,
						vctrl->stats->keyfield_set_names[keyset],
						keyset);
				else
					pf(ct, "  addr: %d, X%d rule, keyset: %s (%d)\n",
					   addr,
					   info->sw_per_item,
					   vctrl->stats->keyfield_set_names[keyset],
					   keyset);
			}
		}
	}
	return 0;
}
#ifdef CONFIG_VCAP_KUNIT_TEST
#include "vcap_api_kunit.c"
#endif
