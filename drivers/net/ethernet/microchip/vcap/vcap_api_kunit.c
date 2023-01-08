/*
 * Copyright (C) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Microchip VCAP API kunit test suite
 */

#include <kunit/test.h>
#include "vcap_api.h"
#include "vcap_api_client.h"

#define TEST_BUF_CNT 100
#define TEST_BUF_SZ  350
#define STREAMWSIZE 64

/* First we have the test infrastructure that emulates the platform
 * implementation
 */
extern const struct vcap_info test_vcaps[];
extern const struct vcap_statistics test_vcap_stats;

static struct vcap_cache_data test_hw_cache;
static u32 test_hw_counter_id = 0;

static u32 test_init_start = 0;
static u32 test_init_count = 0;

static int test_cache_erase_count = 0;

static u32 test_updateaddr[STREAMWSIZE] = {};
static int test_updateaddridx = 0;

static int test_move_addr = 0;
static int test_move_offset = 0;
static int test_move_count = 0;

static char test_pr_buffer[TEST_BUF_CNT][TEST_BUF_SZ];
static int test_pr_bufferidx = 0;

static struct net_device netdev = {};

/* Callback used by the VCAP Library */
static enum vcap_keyfield_set test_val_keyset(struct net_device *ndev, struct vcap_admin *admin,
					      struct vcap_rule *rule,
					      struct vcap_keyset_list *kslist,
					      u16 l3_proto)
{
	int idx;

	if (kslist->cnt > 0) {
		switch (admin->vtype) {
		case VCAP_TYPE_IS0:
			for (idx = 0; idx < kslist->cnt; idx++) {
				if (kslist->keysets[idx] == VCAP_KFS_ETAG)
					return kslist->keysets[idx];
				if (kslist->keysets[idx] == VCAP_KFS_PURE_5TUPLE_IP4)
					return kslist->keysets[idx];
				if (kslist->keysets[idx] == VCAP_KFS_NORMAL_5TUPLE_IP4)
					return kslist->keysets[idx];
				if (kslist->keysets[idx] == VCAP_KFS_NORMAL_7TUPLE)
					return kslist->keysets[idx];
			}
			break;
		case VCAP_TYPE_IS2:
			for (idx = 0; idx < kslist->cnt; idx++) {
				if (kslist->keysets[idx] == VCAP_KFS_MAC_ETYPE)
					return kslist->keysets[idx];
				if (kslist->keysets[idx] == VCAP_KFS_ARP)
					return kslist->keysets[idx];
				if (kslist->keysets[idx] == VCAP_KFS_IP_7TUPLE)
					return kslist->keysets[idx];
			}
			break;
		default:
			pr_info("%s:%d: no validation for VCAP %d\n",
				__func__, __LINE__, admin->vtype);
			break;
		}
	}
	return -EINVAL;
}

/* Callback used by the VCAP Library */
static void test_add_def_fields(struct net_device *ndev, struct vcap_admin *admin,
				   struct vcap_rule *rule)
{
	/* This should be determined by the chain id really */
	if (admin->vinst == 0 || admin->vinst == 2)
		vcap_rule_add_key_bit(rule, VCAP_KF_FIRST, VCAP_BIT_1);
	else
		vcap_rule_add_key_bit(rule, VCAP_KF_FIRST, VCAP_BIT_0);
}

/* Callback used by the VCAP Library */
static void test_cache_erase(struct vcap_admin *admin)
{
	if (test_cache_erase_count) {
		memset(admin->cache.keystream, 0, test_cache_erase_count);
		memset(admin->cache.maskstream, 0, test_cache_erase_count);
		memset(admin->cache.actionstream, 0, test_cache_erase_count);
		test_cache_erase_count = 0;
	}
}

/* Callback used by the VCAP Library */
static void test_cache_init(struct net_device *ndev, struct vcap_admin *admin,
			    u32 start, u32 count)
{
	test_init_start = start;
	test_init_count = count;
}

/* Callback used by the VCAP Library */
static void test_cache_read(struct net_device *ndev, struct vcap_admin *admin,
			     enum vcap_selection sel, u32 start, u32 count)
{
	u32 *keystr, *mskstr, *actstr;
	int idx;

	pr_debug("%s:%d: %d %d\n", __func__, __LINE__, start, count);
	switch (sel) {
	case VCAP_SEL_ENTRY:
		keystr = &admin->cache.keystream[start];
		mskstr = &admin->cache.maskstream[start];
		for (idx = 0; idx < count; ++idx) {
			pr_debug("%s:%d: keydata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, keystr[idx]);
		}
		for (idx = 0; idx < count; ++idx) {
			/* Invert the mask before decoding starts */
			mskstr[idx] = ~mskstr[idx];
			pr_debug("%s:%d: mskdata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, mskstr[idx]);
		}
		break;
	case VCAP_SEL_ACTION:
		actstr = &admin->cache.actionstream[start];
		for (idx = 0; idx < count; ++idx) {
			pr_debug("%s:%d: actdata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, actstr[idx]);
		}
		break;
	case VCAP_SEL_COUNTER:
		pr_debug("%s:%d\n", __func__, __LINE__);
		test_hw_counter_id = start;
		admin->cache.counter = test_hw_cache.counter;
		admin->cache.sticky = test_hw_cache.sticky;
		break;
	case VCAP_SEL_ALL:
		pr_debug("%s:%d\n", __func__, __LINE__);
		break;
	}
}

/* Callback used by the VCAP Library */
static void test_cache_write(struct net_device *ndev, struct vcap_admin *admin,
			     enum vcap_selection sel, u32 start, u32 count)
{
	u32 *keystr, *mskstr, *actstr;
	int idx;

	switch (sel) {
	case VCAP_SEL_ENTRY:
		keystr = &admin->cache.keystream[start];
		mskstr = &admin->cache.maskstream[start];
		for (idx = 0; idx < count; ++idx) {
			pr_debug("%s:%d: keydata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, keystr[idx]);
		}
		for (idx = 0; idx < count; ++idx) {
			/* Invert the mask before encoding starts */
			mskstr[idx] = ~mskstr[idx];
			pr_debug("%s:%d: mskdata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, mskstr[idx]);
		}
		break;
	case VCAP_SEL_ACTION:
		actstr = &admin->cache.actionstream[start];
		for (idx = 0; idx < count; ++idx) {
			pr_debug("%s:%d: actdata[%02d]: 0x%08x\n", __func__,
				 __LINE__, start + idx, actstr[idx]);
		}
		break;
	case VCAP_SEL_COUNTER:
		pr_debug("%s:%d\n", __func__, __LINE__);
		test_hw_counter_id = start;
		test_hw_cache.counter = admin->cache.counter;
		test_hw_cache.sticky = admin->cache.sticky;
		break;
	case VCAP_SEL_ALL:
		pr_err("%s:%d: cannot write all streams at once\n",
		       __func__, __LINE__);
		break;
	}
}

/* Callback used by the VCAP Library */
static void test_cache_update(struct net_device *ndev, struct vcap_admin *admin,
		       enum vcap_command cmd,
		       enum vcap_selection sel, u32 addr)
{
	char *cmdstr, *selstr;

	switch (cmd) {
	case VCAP_CMD_WRITE: cmdstr = "write"; break;
	case VCAP_CMD_READ: cmdstr = "read"; break;
	case VCAP_CMD_MOVE_DOWN: cmdstr = "move_down"; break;
	case VCAP_CMD_MOVE_UP: cmdstr = "move_up"; break;
	case VCAP_CMD_INITIALIZE: cmdstr = "init"; break;
	}
	switch (sel) {
	case VCAP_SEL_ENTRY: selstr = "entry"; break;
	case VCAP_SEL_ACTION: selstr = "action"; break;
	case VCAP_SEL_COUNTER: selstr = "counter"; break;
	case VCAP_SEL_ALL: selstr = "all"; break;
	}
	pr_debug("%s:%d: %s %s: addr: %d\n", __func__, __LINE__, cmdstr, selstr, addr);
	if (test_updateaddridx < ARRAY_SIZE(test_updateaddr))
		test_updateaddr[test_updateaddridx] = addr;
	else
		pr_err("%s:%d: overflow: %d\n", __func__, __LINE__, test_updateaddridx);
	test_updateaddridx++;
}

static void test_cache_move(struct net_device *ndev, struct vcap_admin *admin, u32 addr, int offset,
		     int count)
{
	test_move_addr = addr;
	test_move_offset = offset;
	test_move_count = count;
}

/* callback used by the show_admin function */
int test_pf(void *client, const char *fmt, ...)
{
	va_list args;
	int cnt;

	if (test_pr_bufferidx < TEST_BUF_CNT) {
		va_start(args, fmt);
		cnt = vscnprintf(test_pr_buffer[test_pr_bufferidx], TEST_BUF_SZ, fmt, args);
		va_end(args);
	} else {
		pr_err("%s:%d: overflow: %d\n", __func__, __LINE__, test_pr_bufferidx);
	}
	test_pr_bufferidx++;
	return cnt;
}

/* Provide port information via a callback interface */
static int vcap_test_port_info(struct net_device *ndev, enum vcap_type vtype,
		   int (*pf)(void *out, int arg, const char *fmt, ...),
		   void *out, int arg)
{
	return 0;
}

struct vcap_operations test_callbacks = {
	.validate_keyset = test_val_keyset,
	.add_default_fields = test_add_def_fields,
	.cache_erase = test_cache_erase,
	.cache_write = test_cache_write,
	.cache_read = test_cache_read,
	.init = test_cache_init,
	.update = test_cache_update,
	.move = test_cache_move,
	.port_info = vcap_test_port_info,
};

struct vcap_control test_vctrl = {
	.vcaps = test_vcaps,
	.stats = &test_vcap_stats,
	.ops = &test_callbacks,
};

/* Define the test cases. */

static void vcap_test_api_init(struct vcap_admin *admin)
{
	/* Initialize the shared objects */
	INIT_LIST_HEAD(&test_vctrl.list);
	INIT_LIST_HEAD(&admin->list);
	INIT_LIST_HEAD(&admin->rules);
	list_add_tail(&admin->list, &test_vctrl.list);
	memset(test_updateaddr, 0, sizeof(test_updateaddr));
	test_updateaddridx = 0;
	vcap_api_set_client(&test_vctrl);
}

static void vcap_api_set_bit_1_test(struct kunit *test)
{
	struct vcap_stream_iter iter = {
		.offset = 35,
		.sw_width = 52,
		.reg_idx = 1,
		.reg_bitpos = 20,
		.tg = 0
	};
	u32 stream[2] = {0};

	vcap_set_bit(stream, &iter, 1);

	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)BIT(20), stream[1]);
}

static void vcap_api_set_bit_0_test(struct kunit *test)
{
	struct vcap_stream_iter iter = {
		.offset = 35,
		.sw_width = 52,
		.reg_idx = 2,
		.reg_bitpos = 11,
		.tg = 0
	};
	u32 stream[3] = {~0, ~0, ~0};

	vcap_set_bit(stream, &iter, 0);

	KUNIT_EXPECT_EQ(test, (u32)~0, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)~0, stream[1]);
	KUNIT_EXPECT_EQ(test, (u32)~BIT(11), stream[2]);
}

static void vcap_api_iterator_init_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 2, .value = 2, },
		{ .offset = 156, .width = 1, .value = 0, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	struct vcap_typegroup typegroups2[] = {
		{ .offset = 0, .width = 3, .value = 4, },
		{ .offset = 49, .width = 2, .value = 0, },
		{ .offset = 98, .width = 2, .value = 0, },
	};

	vcap_iter_init(&iter, 52, typegroups, 86);

	KUNIT_EXPECT_EQ(test, 52, iter.sw_width);
	KUNIT_EXPECT_EQ(test, 86 + 2, iter.offset);
	KUNIT_EXPECT_EQ(test, 3, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 4, iter.reg_bitpos);

	vcap_iter_init(&iter, 49, typegroups2, 134);

	KUNIT_EXPECT_EQ(test, 49, iter.sw_width);
	KUNIT_EXPECT_EQ(test, 134 + 7, iter.offset);
	KUNIT_EXPECT_EQ(test, 5, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 11, iter.reg_bitpos);
}

static void vcap_api_iterator_next_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 4, .value = 8, },
		{ .offset = 49, .width = 1, .value = 0, },
		{ .offset = 98, .width = 2, .value = 0, },
		{ .offset = 147, .width = 3, .value = 0, },
		{ .offset = 196, .width = 2, .value = 0, },
		{ .offset = 245, .width = 1, .value = 0, },
	};
	int idx;

	vcap_iter_init(&iter, 49, typegroups, 86);

	KUNIT_EXPECT_EQ(test, 49, iter.sw_width);
	KUNIT_EXPECT_EQ(test, 86 + 5, iter.offset);
	KUNIT_EXPECT_EQ(test, 3, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 10, iter.reg_bitpos);

	vcap_iter_next(&iter);

	KUNIT_EXPECT_EQ(test, 91 + 1, iter.offset);
	KUNIT_EXPECT_EQ(test, 3, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 11, iter.reg_bitpos);

	for (idx = 0; idx < 6; idx++)
		vcap_iter_next(&iter);

	KUNIT_EXPECT_EQ(test, 92 + 6 + 2, iter.offset);
	KUNIT_EXPECT_EQ(test, 4, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 2, iter.reg_bitpos);
}

static void vcap_api_encode_typegroups_test(struct kunit *test)
{
	u32 stream[12] = {0};
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 4, .value = 8, },
		{ .offset = 49, .width = 1, .value = 1, },
		{ .offset = 98, .width = 2, .value = 3, },
		{ .offset = 147, .width = 3, .value = 5, },
		{ .offset = 196, .width = 2, .value = 2, },
		{ .offset = 245, .width = 5, .value = 27, },
		{ .offset = 0, .width = 0, .value = 0, },
	};

	vcap_encode_typegroups(stream, 49, typegroups, false);

	KUNIT_EXPECT_EQ(test, (u32)0x8, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[1]);
	KUNIT_EXPECT_EQ(test, (u32)0x1, stream[2]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x3, stream[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[5]);
	KUNIT_EXPECT_EQ(test, (u32)0x5, stream[6]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[7]);
	KUNIT_EXPECT_EQ(test, (u32)0x2, stream[8]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[9]);
	KUNIT_EXPECT_EQ(test, (u32)27, stream[10]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[11]);
}

static void vcap_api_encode_bit_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	u32 stream[4] = {0};
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 4, .value = 8, },
		{ .offset = 49, .width = 1, .value = 1, },
		{ .offset = 98, .width = 2, .value = 3, },
		{ .offset = 147, .width = 3, .value = 5, },
		{ .offset = 196, .width = 2, .value = 2, },
		{ .offset = 245, .width = 1, .value = 0, },
	};

	vcap_iter_init(&iter, 49, typegroups, 44);

	KUNIT_EXPECT_EQ(test, 48, iter.offset);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 16, iter.reg_bitpos);

	vcap_encode_bit(stream, &iter, 1);

	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)BIT(16), stream[1]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[2]);
}

static void vcap_api_encode_field_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	u32 stream[16] = {0};
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 4, .value = 8, },
		{ .offset = 49, .width = 1, .value = 1, },
		{ .offset = 98, .width = 2, .value = 3, },
		{ .offset = 147, .width = 3, .value = 5, },
		{ .offset = 196, .width = 2, .value = 2, },
		{ .offset = 245, .width = 5, .value = 27, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	struct vcap_field rf = {
		.type = VCAP_FIELD_U32,
		.offset = 86,
		.width = 4,
	};
	u8 value[] = {0x5};

	vcap_iter_init(&iter, 49, typegroups, rf.offset);

	KUNIT_EXPECT_EQ(test, 91, iter.offset);
	KUNIT_EXPECT_EQ(test, 3, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 10, iter.reg_bitpos);

	vcap_encode_field(stream, &iter, rf.width, value);

	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[1]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[2]);
	KUNIT_EXPECT_EQ(test, (u32)(0x5 << 10), stream[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[4]);

	vcap_encode_typegroups(stream, 49, typegroups, false);

	KUNIT_EXPECT_EQ(test, (u32)0x8, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[1]);
	KUNIT_EXPECT_EQ(test, (u32)0x1, stream[2]);
	KUNIT_EXPECT_EQ(test, (u32)(0x5 << 10), stream[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x3, stream[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[5]);
	KUNIT_EXPECT_EQ(test, (u32)0x5, stream[6]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[7]);
	KUNIT_EXPECT_EQ(test, (u32)0x2, stream[8]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[9]);
	KUNIT_EXPECT_EQ(test, (u32)27, stream[10]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[11]);
}

/* In this testcase the subword is smaller than a register */
static void vcap_api_encode_short_field_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	int sw_width = 21;
	u32 stream[6] = {0};
	struct vcap_typegroup tgt[] = {
		{ .offset = 0, .width = 3, .value = 7, },
		{ .offset = 21, .width = 2, .value = 3, },
		{ .offset = 42, .width = 1, .value = 1, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	struct vcap_field rf = {
		.type = VCAP_FIELD_U32,
		.offset = 25,
		.width = 4,
	};
	u8 value[] = {0x5};

	vcap_iter_init(&iter, sw_width, tgt, rf.offset);

	KUNIT_EXPECT_EQ(test, 1, iter.regs_per_sw);
	KUNIT_EXPECT_EQ(test, 21, iter.sw_width);
	KUNIT_EXPECT_EQ(test, 25 + 3 + 2, iter.offset);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 25 + 3 + 2 - sw_width, iter.reg_bitpos);

	vcap_encode_field(stream, &iter, rf.width, value);

	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)(0x5 << (25 + 3 + 2 - sw_width)), stream[1]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[2]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, stream[5]);

	vcap_encode_typegroups(stream, sw_width, tgt, false);

	KUNIT_EXPECT_EQ(test, (u32)7, stream[0]);
	KUNIT_EXPECT_EQ(test, (u32)((0x5 << (25 + 3 + 2 - sw_width)) + 3), stream[1]);
	KUNIT_EXPECT_EQ(test, (u32)1, stream[2]);
	KUNIT_EXPECT_EQ(test, (u32)0, stream[3]);
	KUNIT_EXPECT_EQ(test, (u32)0, stream[4]);
	KUNIT_EXPECT_EQ(test, (u32)0, stream[5]);
}

static void vcap_api_encode_keyfield_test(struct kunit *test)
{
	u32 keywords[16] = {0};
	u32 maskwords[16] = {0};
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.cache = {
			.keystream = keywords,
			.maskstream = maskwords,
			.actionstream = keywords,
		},
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
		.data = {
			.keyset = VCAP_KFS_MAC_ETYPE,
		},
	};
	struct vcap_client_keyfield ckf = {
		.ctrl.list = {},
		.ctrl.key = VCAP_KF_ISDX,
		.ctrl.type = VCAP_FIELD_U32,
		.data.u32.value = 0xeef014a1,
		.data.u32.mask = 0xfff,
	};
	struct vcap_field rf = {
		.type = VCAP_FIELD_U32,
		.offset = 56,
		.width = 12,
	};
	struct vcap_typegroup tgt[] = {
		{ .offset = 0, .width = 2, .value = 2, },
		{ .offset = 156, .width = 1, .value = 1, },
		{ .offset = 0, .width = 0, .value = 0, },
	};

	vcap_test_api_init(&admin);
	vcap_encode_keyfield(&rule, &ckf, &rf, tgt);

	/* Key */
	KUNIT_EXPECT_EQ(test, (u32)0x0, keywords[0]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, keywords[1]);
	KUNIT_EXPECT_EQ(test, (u32)(0x04a1 << 6), keywords[2]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, keywords[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, keywords[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, keywords[5]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, keywords[6]);

	/* Mask */
	KUNIT_EXPECT_EQ(test, (u32)0x0, maskwords[0]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, maskwords[1]);
	KUNIT_EXPECT_EQ(test, (u32)(0x0fff << 6), maskwords[2]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, maskwords[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, maskwords[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, maskwords[5]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, maskwords[6]);
}

static void vcap_api_encode_max_keyfield_test(struct kunit *test)
{
	int idx;
	u32 keywords[6] = {0};
	u32 maskwords[6] = {0};
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		/* IS2 sw_width = 52 bit */
		.cache = {
			.keystream = keywords,
			.maskstream = maskwords,
			.actionstream = keywords,
		},
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
		.data = {
			.keyset = VCAP_KFS_IP_7TUPLE,
		},
	};
	struct vcap_client_keyfield ckf = {
		.ctrl.list = {},
		.ctrl.key = VCAP_KF_L3_IP6_DIP,
		.ctrl.type = VCAP_FIELD_U128,
		.data.u128.value = {0xa1, 0xa2, 0xa3, 0xa4, 0, 0, 0x43, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x8e, },
		.data.u128.mask =  {0xff, 0xff, 0xff, 0xff, 0, 0, 0xff, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff },
	};
	struct vcap_field rf = {
		.type = VCAP_FIELD_U128,
		.offset = 0,
		.width = 128,
	};
	struct vcap_typegroup tgt[] = {
		{ .offset = 0, .width = 2, .value = 2, },
		{ .offset = 156, .width = 1, .value = 1, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	u32 keyres[] = {
		0x928e8a84,
		0x000c0002,
		0x00000010,
		0x00000000,
		0x0239e000,
		0x00000000,
	};
	u32 mskres[] = {
		0xfffffffc,
		0x000c0003,
		0x0000003f,
		0x00000000,
		0x03fffc00,
		0x00000000,
	};

	vcap_encode_keyfield(&rule, &ckf, &rf, tgt);

	/* Key */
	for (idx = 0; idx < ARRAY_SIZE(keyres); ++idx) {
		KUNIT_EXPECT_EQ(test, keyres[idx], keywords[idx]);
	}
	/* Mask */
	for (idx = 0; idx < ARRAY_SIZE(mskres); ++idx) {
		KUNIT_EXPECT_EQ(test, mskres[idx], maskwords[idx]);
	}
}

static void vcap_api_encode_actionfield_test(struct kunit *test)
{
	u32 actwords[16] = {0};
	int sw_width = 21;
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_ES2, /* act_width = 21 */
		.cache = {
			.actionstream = actwords,
		},
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
		.data = {
			.actionset = VCAP_AFS_BASE_TYPE,
		},
	};
	struct vcap_client_actionfield caf = {
		.ctrl.list = {},
		.ctrl.action = VCAP_AF_POLICE_IDX,
		.ctrl.type = VCAP_FIELD_U32,
		.data.u32.value = 0x67908032,
	};
	struct vcap_field rf = {
		.type = VCAP_FIELD_U32,
		.offset = 35,
		.width = 6,
	};
	struct vcap_typegroup tgt[] = {
		{ .offset = 0, .width = 2, .value = 2, },
		{ .offset = 21, .width = 1, .value = 1, },
		{ .offset = 42, .width = 1, .value = 0, },
		{ .offset = 0, .width = 0, .value = 0, },
	};

	vcap_encode_actionfield(&rule, &caf, &rf, tgt);

	/* Action */
	KUNIT_EXPECT_EQ(test, (u32)0x0, actwords[0]);
	KUNIT_EXPECT_EQ(test, (u32)((0x32 << (35 + 2 + 1 - sw_width)) & 0x1fffff) , actwords[1]);
	KUNIT_EXPECT_EQ(test, (u32)((0x32 >> ((2 * sw_width) - 38 - 1))) , actwords[2]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, actwords[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, actwords[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, actwords[5]);
	KUNIT_EXPECT_EQ(test, (u32)0x0, actwords[6]);
}

static void vcap_api_keyfield_typegroup_test(struct kunit *test)
{
	const struct vcap_typegroup *tg;

	tg = vcap_keyfield_typegroup(VCAP_TYPE_IS2, VCAP_KFS_MAC_ETYPE);
	KUNIT_EXPECT_PTR_NE(test, NULL, tg);
	KUNIT_EXPECT_EQ(test, 0, tg[0].offset);
	KUNIT_EXPECT_EQ(test, 2, tg[0].width);
	KUNIT_EXPECT_EQ(test, 2, tg[0].value);
	KUNIT_EXPECT_EQ(test, 156, tg[1].offset);
	KUNIT_EXPECT_EQ(test, 1, tg[1].width);
	KUNIT_EXPECT_EQ(test, 0, tg[1].value);
	KUNIT_EXPECT_EQ(test, 0, tg[2].offset);
	KUNIT_EXPECT_EQ(test, 0, tg[2].width);
	KUNIT_EXPECT_EQ(test, 0, tg[2].value);

	tg = vcap_keyfield_typegroup(VCAP_TYPE_ES2, VCAP_KFS_LL_FULL);
	KUNIT_EXPECT_PTR_EQ(test, NULL, tg);
}

static void vcap_api_actionfield_typegroup_test(struct kunit *test)
{
	const struct vcap_typegroup *tg;

	tg = vcap_actionfield_typegroup(VCAP_TYPE_IS0, VCAP_AFS_FULL);
	KUNIT_EXPECT_PTR_NE(test, NULL, tg);
	KUNIT_EXPECT_EQ(test, 0, tg[0].offset);
	KUNIT_EXPECT_EQ(test, 3, tg[0].width);
	KUNIT_EXPECT_EQ(test, 4, tg[0].value);
	KUNIT_EXPECT_EQ(test, 110, tg[1].offset);
	KUNIT_EXPECT_EQ(test, 2, tg[1].width);
	KUNIT_EXPECT_EQ(test, 0, tg[1].value);
	KUNIT_EXPECT_EQ(test, 220, tg[2].offset);
	KUNIT_EXPECT_EQ(test, 2, tg[2].width);
	KUNIT_EXPECT_EQ(test, 0, tg[2].value);
	KUNIT_EXPECT_EQ(test, 0, tg[3].offset);
	KUNIT_EXPECT_EQ(test, 0, tg[3].width);
	KUNIT_EXPECT_EQ(test, 0, tg[3].value);

	tg = vcap_actionfield_typegroup(VCAP_TYPE_IS2, VCAP_AFS_SMAC_SIP);
	KUNIT_EXPECT_PTR_EQ(test, NULL, tg);
}

static void vcap_api_vcap_keyfields_test(struct kunit *test)
{
	const struct vcap_field *ft;

	ft = vcap_keyfields(VCAP_TYPE_IS2, VCAP_KFS_MAC_ETYPE);
	KUNIT_EXPECT_PTR_NE(test, NULL, ft);

	/* Keyset that is not available and within the maximum keyset enum value */
	ft = vcap_keyfields(VCAP_TYPE_ES2, VCAP_KFS_PURE_5TUPLE_IP4);
	KUNIT_EXPECT_PTR_EQ(test, NULL, ft);

	/* Keyset that is not available and beyond the maximum keyset enum value */
	ft = vcap_keyfields(VCAP_TYPE_ES2, VCAP_KFS_LL_FULL);
	KUNIT_EXPECT_PTR_EQ(test, NULL, ft);
}

static void vcap_api_vcap_actionfields_test(struct kunit *test)
{
	const struct vcap_field *ft;

	ft = vcap_actionfields(VCAP_TYPE_IS0, VCAP_AFS_FULL);
	KUNIT_EXPECT_PTR_NE(test, NULL, ft);

	ft = vcap_actionfields(VCAP_TYPE_IS2, VCAP_AFS_FULL);
	KUNIT_EXPECT_PTR_EQ(test, NULL, ft);

	ft = vcap_actionfields(VCAP_TYPE_IS2, VCAP_AFS_SMAC_SIP);
	KUNIT_EXPECT_PTR_EQ(test, NULL, ft);
}

static void vcap_api_encode_rule_keyset_test(struct kunit *test)
{
	u32 keywords[16] = {0};
	u32 maskwords[16] = {0};
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.cache = {
			.keystream = keywords,
			.maskstream = maskwords,
		},
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
		.data = {
			.keyset = VCAP_KFS_MAC_ETYPE,
		},
	};
	struct vcap_client_keyfield ckf[] = {
		{
			.ctrl.key = VCAP_KF_TYPE,
			.ctrl.type = VCAP_FIELD_U32,
			.data.u32.value = 0x00,
			.data.u32.mask = 0x0f,
		},
		{
			.ctrl.key = VCAP_KF_FIRST,
			.ctrl.type = VCAP_FIELD_BIT,
			.data.u1.value = 0x01,
			.data.u1.mask = 0x01,
		},
		{
			.ctrl.key = VCAP_KF_IGR_PORT_MASK_L3,
			.ctrl.type = VCAP_FIELD_BIT,
			.data.u1.value = 0x00,
			.data.u1.mask = 0x01,
		},
		{
			.ctrl.key = VCAP_KF_IGR_PORT_MASK_RNG,
			.ctrl.type = VCAP_FIELD_U32,
			.data.u32.value = 0x00,
			.data.u32.mask = 0x0f,
		},
		{
			.ctrl.key = VCAP_KF_IGR_PORT_MASK,
			.ctrl.type = VCAP_FIELD_U72,
			.data.u72.value = {0x0, 0x00, 0x00, 0x00},
			.data.u72.mask = {0xfd, 0xff, 0xff, 0xff},
		},
		{
			.ctrl.key = VCAP_KF_L2_DMAC,
			.ctrl.type = VCAP_FIELD_U48,
			/* Opposite endianness */
			.data.u48.value = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
			.data.u48.mask = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
		},
		{
			.ctrl.key = VCAP_KF_ETYPE_LEN,
			.ctrl.type = VCAP_FIELD_BIT,
			.data.u1.value = 0x01,
			.data.u1.mask = 0x01,
		},
		{
			.ctrl.key = VCAP_KF_ETYPE,
			.ctrl.type = VCAP_FIELD_U32,
			.data.u32.value = 0xaabb,
			.data.u32.mask = 0xffff,
		},
	};
	int idx;
	int ret;

	/* Empty entry list */
	INIT_LIST_HEAD(&rule.data.keyfields);
	ret = vcap_encode_rule_keyset(&rule);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	for (idx = 0; idx < ARRAY_SIZE(ckf); idx++) {
		list_add_tail(&ckf[idx].ctrl.list, &rule.data.keyfields);
	}
	ret = vcap_encode_rule_keyset(&rule);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* The key and mask values below are from an actual Sparx5 rule config */
	/* Key */
	KUNIT_EXPECT_EQ(test, (u32)0x00000042, keywords[0]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[1]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[2]);
	KUNIT_EXPECT_EQ(test, (u32)0x00020100, keywords[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x60504030, keywords[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[5]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[6]);
	KUNIT_EXPECT_EQ(test, (u32)0x0002aaee, keywords[7]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[8]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[9]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[10]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, keywords[11]);

	/* Mask: they will be inverted when applied to the register */
	KUNIT_EXPECT_EQ(test, (u32)~0x00b07f80, maskwords[0]);
	KUNIT_EXPECT_EQ(test, (u32)~0xfff00000, maskwords[1]);
	KUNIT_EXPECT_EQ(test, (u32)~0xfffffffc, maskwords[2]);
	KUNIT_EXPECT_EQ(test, (u32)~0xfff000ff, maskwords[3]);
	KUNIT_EXPECT_EQ(test, (u32)~0x00000000, maskwords[4]);
	KUNIT_EXPECT_EQ(test, (u32)~0xfffffff0, maskwords[5]);
	KUNIT_EXPECT_EQ(test, (u32)~0xfffffffe, maskwords[6]);
	KUNIT_EXPECT_EQ(test, (u32)~0xfffc0001, maskwords[7]);
	KUNIT_EXPECT_EQ(test, (u32)~0xffffffff, maskwords[8]);
	KUNIT_EXPECT_EQ(test, (u32)~0xffffffff, maskwords[9]);
	KUNIT_EXPECT_EQ(test, (u32)~0xffffffff, maskwords[10]);
	KUNIT_EXPECT_EQ(test, (u32)~0xffffffff, maskwords[11]);
}

static void vcap_api_encode_rule_actionset_test(struct kunit *test)
{
	u32 actwords[16] = {0};
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.cache = {
			.actionstream = actwords,
		},
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
		.data = {
			.actionset = VCAP_AFS_BASE_TYPE,
		},
	};
	struct vcap_client_actionfield caf[] = {
		{
			.ctrl.action = VCAP_AF_MATCH_ID,
			.ctrl.type = VCAP_FIELD_U32,
			.data.u32.value = 0x01,
		},
		{
			.ctrl.action = VCAP_AF_MATCH_ID_MASK,
			.ctrl.type = VCAP_FIELD_U32,
			.data.u32.value = 0x01,
		},
		{
			.ctrl.action = VCAP_AF_CNT_ID,
			.ctrl.type = VCAP_FIELD_U32,
			.data.u32.value = 0x64,
		},
	};
	int idx;
	int ret;

	/* Empty entry list */
	INIT_LIST_HEAD(&rule.data.actionfields);
	ret = vcap_encode_rule_actionset(&rule);
	/* We allow rules with no actions */
	KUNIT_EXPECT_EQ(test, 0, ret);

	for (idx = 0; idx < ARRAY_SIZE(caf); idx++) {
		list_add_tail(&caf[idx].ctrl.list, &rule.data.actionfields);
	}
	ret = vcap_encode_rule_actionset(&rule);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* The action values below are from an actual Sparx5 rule config */
	KUNIT_EXPECT_EQ(test, (u32)0x00000002, actwords[0]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[1]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[2]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[3]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[4]);
	KUNIT_EXPECT_EQ(test, (u32)0x00100000, actwords[5]);
	KUNIT_EXPECT_EQ(test, (u32)0x06400010, actwords[6]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[7]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[8]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[9]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[10]);
	KUNIT_EXPECT_EQ(test, (u32)0x00000000, actwords[11]);
}

static void vcap_api_get_bit_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	u32 stream[2] = {BIT(31), BIT(1)};
	bool bit;

	/* Start at bit position 29 (not including TG bits) */
	/* 52 bits per subword, X6 rule entry typegroups */
	vcap_iter_init(&iter, 52, test_vcaps[VCAP_TYPE_IS2].keyfield_set_typegroups[6], 29);
	/* Skip over 2 initial typegroup bits at bitpos 0 and 1 */
	KUNIT_EXPECT_EQ(test, 31, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 0, iter.reg_idx);
	bit = vcap_get_bit(stream, &iter);
	KUNIT_EXPECT_EQ(test, true, bit);
	vcap_iter_next(&iter);
	KUNIT_EXPECT_EQ(test, 0, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	bit = vcap_get_bit(stream, &iter);
	KUNIT_EXPECT_EQ(test, false, bit);
	vcap_iter_next(&iter);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	bit = vcap_get_bit(stream, &iter);
	KUNIT_EXPECT_EQ(test, true, bit);
}

static void vcap_api_decode_field_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	int sw_width = 49; /* Subword width 49 bits */
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 4, .value = 8, },
		{ .offset = 49, .width = 1, .value = 1, },
		{ .offset = 98, .width = 2, .value = 3, },
		{ .offset = 147, .width = 3, .value = 5, },
		{ .offset = 196, .width = 2, .value = 2, },
		{ .offset = 245, .width = 5, .value = 27, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	struct vcap_field rf1 = {
		.type = VCAP_FIELD_U32,
		.offset = 86,
		.width = 4,
	};
	u32 stream1[16] = {0, 0, 0, 0x5 << 10, 0};
	u8 value1 = 0;
	struct vcap_field rf2 = {
		.type = VCAP_FIELD_U32,
		.offset = 40,
		.width = 12,
	};
	u32 stream2[16] = {0, 756 << 12, 756 >> 4};
	u32 value2 = 0;

	vcap_iter_init(&iter, sw_width, typegroups, rf1.offset);
	KUNIT_EXPECT_EQ(test, 10, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 3, iter.reg_idx);
	vcap_decode_field(stream1, &iter, rf1.width, &value1);
	KUNIT_EXPECT_EQ(test, 14, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 3, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 5, value1);

	vcap_iter_init(&iter, sw_width, typegroups, rf2.offset);
	KUNIT_EXPECT_EQ(test, 12, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	vcap_decode_field(stream2, &iter, rf2.width, (u8 *)&value2);
	KUNIT_EXPECT_EQ(test, 8, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 2, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 756, value2);
}

static void vcap_api_decode_long_field_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	int sw_width = 52; /* Subword width 52 bits */
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 2, .value = 2, },
		{ .offset = 156, .width = 1, .value = 1, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	u32 keystream[] = {
		0x928e8a84,
		0x000c0002,
		0x00000010,
		0x00000000,
		0x0239e000,
		0x00000000,
	};
	u32 mskstream[] = {
		0xfffffffc,
		0x000c0003,
		0x0000003f,
		0x00000000,
		0x03fffc00,
		0x00000000,
	};
	struct vcap_field rf = {
		.type = VCAP_FIELD_U128,
		.offset = 0,
		.width = 128,
	};
	u8 value[16] = {0};
	u8 exp_keyvalue[] = {0xa1, 0xa2, 0xa3, 0xa4, 0, 0, 0x43, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x8e};
	u8 exp_mskvalue[] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0xff, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
	int res;

	vcap_iter_init(&iter, sw_width, typegroups, rf.offset);
	vcap_decode_field(keystream, &iter, rf.width, value);
	res = memcmp(exp_keyvalue, value, sizeof(value));
	KUNIT_EXPECT_EQ(test, 0, res);

	vcap_iter_init(&iter, sw_width, typegroups, rf.offset);
	vcap_decode_field(mskstream, &iter, rf.width, value);
	res = memcmp(exp_mskvalue, value, sizeof(value));
	KUNIT_EXPECT_EQ(test, 0, res);
}

/* In this testcase the subword is smaller than a register */
static void vcap_api_decode_short_field_test(struct kunit *test)
{
	struct vcap_stream_iter iter;
	int sw_width = 21; /* Subword width 21 bits */
	struct vcap_typegroup typegroups[] = {
		{ .offset = 0, .width = 3, .value = 7, },
		{ .offset = 21, .width = 2, .value = 3, },
		{ .offset = 42, .width = 1, .value = 1, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	struct vcap_field rf1 = {
		.type = VCAP_FIELD_U32,
		.offset = 25,
		.width = 3,
	};
	u32 stream1[6] = {0, 5 << 9};
	u8 value1 = 0;
	struct vcap_field rf2 = {
		.type = VCAP_FIELD_U32,
		.offset = 15,
		.width = 12,
	};
	u32 stream2[16] = {931 << 18, 931 >> 1, 0};
	u32 value2 = 0;

	vcap_iter_init(&iter, sw_width, typegroups, rf1.offset);
	KUNIT_EXPECT_EQ(test, 9, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	vcap_decode_field(stream1, &iter, rf1.width, &value1);
	KUNIT_EXPECT_EQ(test, 12, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 5, value1);

	vcap_iter_init(&iter, sw_width, typegroups, rf2.offset);
	KUNIT_EXPECT_EQ(test, 18, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 0, iter.reg_idx);
	vcap_decode_field(stream2, &iter, rf2.width, (u8 *)&value2);
	KUNIT_EXPECT_EQ(test, 11, iter.reg_bitpos);
	KUNIT_EXPECT_EQ(test, 1, iter.reg_idx);
	KUNIT_EXPECT_EQ(test, 931, value2);
}

static void vcap_api_decode_keyfield_typegroup_test(struct kunit *test)
{
	int sw_width = 49; /* Subword width 49 bits */
	struct vcap_typegroup typegroups[] = { /* 12 32bit words in all */
		{ .offset = 0, .width = 4, .value = 8, },
		{ .offset = 49, .width = 1, .value = 1, },
		{ .offset = 98, .width = 2, .value = 3, },
		{ .offset = 147, .width = 3, .value = 5, },
		{ .offset = 196, .width = 2, .value = 2, },
		{ .offset = 245, .width = 5, .value = 27, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	u32 stream1[12] = {0};  /* Empty */
	u32 stream2[12] = {8, 0, 1, 0, 3, 0, 7, 0, 2, 0, 27}; /* One error */
	u32 stream3[12] = {8, 0, 1, 0, 3, 0, 5, 0, 2, 0, 27}; /* Valid */
	u32 maskstream1[12] = {0};  /* Empty */
	u32 maskstream2[12] = {15, 0, 1, 0, 3, 0, 6, 0, 3, 0, 31}; /* One error */
	u32 maskstream3[12] = {15, 0, 1, 0, 3, 0, 7, 0, 3, 0, 31}; /* Valid */
	int res;

	res = vcap_verify_typegroups(stream1, sw_width, typegroups, false, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(stream2, sw_width, typegroups, false, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(stream3, sw_width, typegroups, false, 0);
	KUNIT_EXPECT_EQ(test, 0, res);

	/* Only test 3 typegroups */
	res = vcap_verify_typegroups(stream2, sw_width, typegroups, false, 3);
	KUNIT_EXPECT_EQ(test, 0, res);

	res = vcap_verify_typegroups(maskstream1, sw_width, typegroups, true, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(maskstream2, sw_width, typegroups, true, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(maskstream3, sw_width, typegroups, true, 0);
	KUNIT_EXPECT_EQ(test, 0, res);

	/* Only test 3 typegroups */
	res = vcap_verify_typegroups(maskstream2, sw_width, typegroups, true, 3);
	KUNIT_EXPECT_EQ(test, 0, res);
}

/* In this testcase the subword is smaller than a register */
static void vcap_api_decode_short_keyfield_typegroup_test(struct kunit *test)
{
	int sw_width = 21; /* Subword width 21 bits */
	struct vcap_typegroup typegroups[] = {  /* 8 registers */
		{ .offset = 0, .width = 3, .value = 7, },
		{ .offset = 21, .width = 2, .value = 3, },
		{ .offset = 42, .width = 5, .value = 27, },
		{ .offset = 63, .width = 3, .value = 0, },
		{ .offset = 84, .width = 4, .value = 13, },
		{ .offset = 105, .width = 2, .value = 3, },
		{ .offset = 126, .width = 1, .value = 0, },
		{ .offset = 0, .width = 0, .value = 0, },
	};
	u32 stream1[8] = {0};  /* Empty */
	u32 stream2[8] = {7, 3, 27, 3, 13, 3, 0}; /* One error */
	u32 stream3[8] = {7, 3, 27, 0, 13, 3, 0}; /* Valid */
	u32 maskstream1[8] = {0};  /* Empty */
	u32 maskstream2[8] = {7, 3, 31, 7, 14, 3, 1}; /* One error */
	u32 maskstream3[8] = {7, 3, 31, 7, 15, 3, 1}; /* Valid */
	int res;

	res = vcap_verify_typegroups(stream1, sw_width, typegroups, false, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(stream2, sw_width, typegroups, false, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(stream3, sw_width, typegroups, false, 0);
	KUNIT_EXPECT_EQ(test, 0, res);

	/* Only test 3 typegroups */
	res = vcap_verify_typegroups(stream2, sw_width, typegroups, false, 3);
	KUNIT_EXPECT_EQ(test, 0, res);

	res = vcap_verify_typegroups(maskstream1, sw_width, typegroups, true, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(maskstream2, sw_width, typegroups, true, 0);
	KUNIT_EXPECT_NE(test, 0, res);

	res = vcap_verify_typegroups(maskstream3, sw_width, typegroups, true, 0);
	KUNIT_EXPECT_EQ(test, 0, res);

	/* Only test 3 typegroups */
	res = vcap_verify_typegroups(maskstream2, sw_width, typegroups, true, 4);
	KUNIT_EXPECT_EQ(test, 0, res);
}

static void vcap_api_decode_keystream_test(struct kunit *test)
{
	/* The key and mask values below are from an actual Sparx5 rule config */
	u32 keywords[16] = { 0x00000042, 0x00000000, 0x00000000, 0x00020100,
		0x60504030, 0x00000000, 0x00000000, 0x0002aaee, 0x00000000,
		0x00000000, 0x00000000, 0x00000000 };
	/* Mask: inverted when applied to the cache */
	u32 maskwords[16] = { ~0x00b07f80, ~0xfff00000, ~0xfffffffc,
		~0xfff000ff, ~0x00000000, ~0xfffffff0, ~0xfffffffe, ~0xfffc0001,
		~0xffffffff, ~0xffffffff, ~0xffffffff, ~0xffffffff };
	int sw_count;
	bool res;
	enum vcap_keyfield_set keyset;

	sw_count = vcap_find_keystream_typegroup_sw(VCAP_TYPE_IS2, keywords, false, 0);
	KUNIT_EXPECT_EQ(test, 6, sw_count);
	sw_count = vcap_find_keystream_typegroup_sw(VCAP_TYPE_IS2, maskwords, true, 0);
	KUNIT_EXPECT_EQ(test, 6, sw_count);

	/* Not the correct keyset */
	res = vcap_verify_keystream_keyset(VCAP_TYPE_IS2, keywords, maskwords, VCAP_KFS_ARP);
	KUNIT_EXPECT_EQ(test, false, res);
	/* Keyset not available in S2  */
	res = vcap_verify_keystream_keyset(VCAP_TYPE_IS2, keywords, maskwords, VCAP_KFS_VID);
	KUNIT_EXPECT_EQ(test, false, res);
	res = vcap_verify_keystream_keyset(VCAP_TYPE_IS2, keywords, maskwords, VCAP_KFS_MAC_ETYPE);
	KUNIT_EXPECT_EQ(test, true, res);

	keyset = vcap_find_keystream_keyset(VCAP_TYPE_IS2, keywords, maskwords, false, 0);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_MAC_ETYPE, keyset);
	keyset = vcap_find_keystream_keyset(VCAP_TYPE_IS2, maskwords, maskwords, true, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, keyset);
}

static void vcap_api_decode_actionstream_test(struct kunit *test)
{
	u32 actwords[16] = { 0x00000002, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00100000, 0x06400010, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000 };
	u32 empwords[16] = { 0 };
	int sw_count;
	bool res;
	enum vcap_actionfield_set actionset;

	sw_count = vcap_find_actionstream_typegroup_sw(VCAP_TYPE_IS2, actwords, 0);
	KUNIT_EXPECT_EQ(test, 3, sw_count);

	/* Correct actionfield set */
	res = vcap_verify_actionstream_actionset(VCAP_TYPE_IS2, actwords, VCAP_AFS_BASE_TYPE);
	KUNIT_EXPECT_EQ(test, true, res);
	/* Actionset not available in S0 */
	res = vcap_verify_actionstream_actionset(VCAP_TYPE_IS0, actwords, VCAP_AFS_BASE_TYPE);
	KUNIT_EXPECT_EQ(test, false, res);
	/* Actionset not available in S0 and beyond the list */
	res = vcap_verify_actionstream_actionset(VCAP_TYPE_IS0, actwords, VCAP_AFS_VID);
	KUNIT_EXPECT_EQ(test, false, res);

	actionset = vcap_find_actionstream_actionset(VCAP_TYPE_IS2, actwords, 0);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, actionset);
	actionset = vcap_find_actionstream_actionset(VCAP_TYPE_IS2, empwords, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, actionset);
}

static void vcap_api_decode_bitarray_test(struct kunit *test)
{
	u8 empty[16] = {0};
	u8 nonempty[] = {0, 0, 0, 0, 0, 0, 0, 8};
	u8 bitvalue[] = {0x01};
	u8 vlanvalue[] = { 0xca, 0x6};
	u8 dmacvalue[] = {0xa0, 0x36, 0x9f, 0x67, 0xc1, 0x34};
	char *expstr [] = {
		"1",
		"0110.1100.1010",
		"0011.0100.1100.0001.0110.0111.1001.1111.0011.0110.1010.0000",
		"1.1111.0011.0110.1010.0000",
	};
	bool ret;
	char buffer[300];
	char expect[300];

	ret = vcap_bitarray_zero(8 * sizeof(empty), empty);
	KUNIT_EXPECT_EQ(test, true, ret);
	ret = vcap_bitarray_zero(8 * sizeof(nonempty), nonempty);
	KUNIT_EXPECT_EQ(test, false, ret);
	ret = vcap_bitarray_zero(8 * (sizeof(nonempty) - 1) + 3, nonempty);
	KUNIT_EXPECT_EQ(test, true, ret);
	ret = vcap_bitarray_zero(8 * (sizeof(nonempty) - 1) + 4, nonempty);
	KUNIT_EXPECT_EQ(test, false, ret);
	ret = vcap_bitarray_zero(7 * sizeof(nonempty) + 5, nonempty);
	KUNIT_EXPECT_EQ(test, false, ret);

	vcap_bitarray_tostring(buffer, 1, bitvalue);
	strcpy(expect, expstr[0]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	vcap_bitarray_tostring(buffer, 12, vlanvalue);
	strcpy(expect, expstr[1]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	vcap_bitarray_tostring(buffer, 48, dmacvalue);
	strcpy(expect, expstr[2]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	vcap_bitarray_tostring(buffer, 21, dmacvalue);
	strcpy(expect, expstr[3]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
}

static void vcap_api_alloc_rule_keyfield_test(struct kunit *test)
{
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
	};
	const struct vcap_field firstfield = {
		.type = VCAP_FIELD_BIT,
		.offset = 2,
		.width = 1,
	};
	struct vcap_field vlanfield = {
		.type = VCAP_FIELD_U32,
		.offset = 40,
		.width = 12,
	};
	struct vcap_field longfield = {
		.type = VCAP_FIELD_U48,
		.offset = 86,
		.width = 43,
	};
	u8 firstvalue[] = {0x1};
	u8 firstmask[] = {0x1};
	u8 vlanvalue[] = {0xae, 0x9, 0, 0};
	u8 vlanmask[] = {0xff, 0xf, 0, 0};
	u8 longvalue[] = {0xab, 0xcd, 0xef, 0x89, 0x56, 0xff};
	u8 longmask[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	char *expstr [] = {
		"1/1",
		"1001.1010.1110/1111.1111.1111",
		"111.0101.0110.1000.1001.1110.1111.1100.1101.1010.1011/111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111",
	};
	u8 longexpvalue[] = {0xab, 0xcd, 0xef, 0x89, 0x56, 0x7};
	u8 longexpmask[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0x7};
	char buffer[300];
	char expect[300];
	int ret, idx;
	struct vcap_client_keyfield *kf;

	INIT_LIST_HEAD(&rule.data.keyfields);

	vcap_rule_alloc_keyfield(&rule, &firstfield, VCAP_KF_FIRST, firstvalue, firstmask, buffer);
	strcpy(expect, expstr[0]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	ret = list_empty(&rule.data.keyfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	kf = list_first_entry(&rule.data.keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_FIRST, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.value);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.mask);

	vcap_rule_alloc_keyfield(&rule, &vlanfield, VCAP_KF_VID0, vlanvalue, vlanmask, buffer);
	strcpy(expect, expstr[1]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_VID0, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x9ae, kf->data.u32.value);
	KUNIT_EXPECT_EQ(test, 0xfff, kf->data.u32.mask);

	vcap_rule_alloc_keyfield(&rule, &longfield, VCAP_KF_L2_SMAC, longvalue, longmask, buffer);
	strcpy(expect, expstr[2]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_L2_SMAC, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U48, kf->ctrl.type);
	for (idx = 0; idx < 6; ++idx) {
		KUNIT_EXPECT_EQ(test, longexpvalue[idx], kf->data.u48.value[idx]);
		KUNIT_EXPECT_EQ(test, longexpmask[idx], kf->data.u48.mask[idx]);
	}
}

static void vcap_api_decode_rule_keyset_test(struct kunit *test)
{
	/* The key and mask values below are from an actual Sparx5 rule config */
	u32 keywords[16] = { 0x00000042, 0x00000000, 0x00000000, 0x00020100,
		0x60504030, 0x00000000, 0x00000000, 0x0002aaee, 0x00000000,
		0x00000000, 0x00000000, 0x00000000 };
	/* Mask: inverted when applied to the cache */
	u32 maskwords[16] = { ~0x00b07f80, ~0xfff00000, ~0xfffffffc,
		~0xfff000ff, ~0x00000000, ~0xfffffff0, ~0xfffffffe, ~0xfffc0001,
		~0xffffffff, ~0xffffffff, ~0xffffffff, ~0xffffffff };
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.cache = {
			.keystream = keywords,
			.maskstream = maskwords,
		},
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
	};
	u8 exp_dmac_value[] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x6};
	u8 exp_dmac_mask[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	struct vcap_client_keyfield *kf;
	int ret, idx;

	INIT_LIST_HEAD(&rule.data.keyfields);
	ret = vcap_decode_rule_keyset(&rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule.data.keyfields);
	KUNIT_EXPECT_EQ(test, false, ret);

	kf = list_first_entry(&rule.data.keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_ETYPE, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0xaabb, kf->data.u32.value);
	KUNIT_EXPECT_EQ(test, 0xffff, kf->data.u32.mask);

	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_ETYPE_LEN, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.value);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.mask);

	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_FIRST, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.value);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.mask);

	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_IGR_PORT_MASK, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, kf->data.u32.value);
	KUNIT_EXPECT_EQ(test, 0xfffffffd, kf->data.u32.mask);

	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_IGR_PORT_MASK_L3, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, kf->data.u1.value);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.mask);

	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_IGR_PORT_MASK_RNG, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, kf->data.u32.value);
	KUNIT_EXPECT_EQ(test, 0xf, kf->data.u32.mask);

	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_L2_DMAC, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U48, kf->ctrl.type);
	for (idx = 0; idx < 6; ++idx) {
		KUNIT_EXPECT_EQ(test, exp_dmac_value[idx], kf->data.u48.value[idx]);
		KUNIT_EXPECT_EQ(test, exp_dmac_mask[idx], kf->data.u48.mask[idx]);
	}

	kf = list_next_entry(kf, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_TYPE, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, kf->data.u32.value);
	KUNIT_EXPECT_EQ(test, 0xf, kf->data.u32.mask);

	ret = list_is_last(&kf->ctrl.list, &rule.data.keyfields);
	KUNIT_EXPECT_EQ(test, true, ret);
}

static void vcap_api_alloc_rule_actionfield_test(struct kunit *test)
{
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
	};
	const struct vcap_field polfield = {
		/* VCAP_AF_POLICE_ENA */
		.type = VCAP_FIELD_BIT,
		.offset = 16,
		.width = 1,
	};
	const struct vcap_field polidxfield = {
		/* VCAP_AF_POLICE_IDX */
		.type = VCAP_FIELD_U32,
		.offset = 17,
		.width = 6,
	};
	const struct vcap_field portfield = {
		/* VCAP_AF_PORT_MASK */
		.type = VCAP_FIELD_U72,
		.offset = 30,
		.width = 68,
	};
	u8 polval[] = {0};
	u8 polidxval[] = {62, 0, 0, 0};
	u8 portval[] = {0xe7, 0xa2, 0x5b, 0x1a, 0xf3, 0x34, 0x90, 0x5e, 0xff};
	u8 exp_portval[] = {0xe7, 0xa2, 0x5b, 0x1a, 0xf3, 0x34, 0x90, 0x5e, 0xf};
	char *expstr [] = {
		"0",
		"11.1110",
		"1111.0101.1110.1001.0000.0011.0100.1111.0011.0001.1010.0101.1011.1010.0010.1110.0111",
	};
	char buffer[300];
	char expect[300];
	int ret, idx;
	struct vcap_client_actionfield *af;

	INIT_LIST_HEAD(&rule.data.actionfields);

	vcap_rule_alloc_actionfield(&rule, &polfield, VCAP_AF_POLICE_ENA, polval, buffer);
	strcpy(expect, expstr[0]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	ret = list_empty(&rule.data.actionfields);
	KUNIT_EXPECT_EQ(test, false, ret);

	af = list_first_entry(&rule.data.actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_POLICE_ENA, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, af->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, af->data.u1.value);

	vcap_rule_alloc_actionfield(&rule, &polidxfield, VCAP_AF_POLICE_IDX, polidxval, buffer);
	strcpy(expect, expstr[1]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	af = list_next_entry(af, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_POLICE_IDX, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, af->ctrl.type);
	KUNIT_EXPECT_EQ(test, 62, af->data.u32.value);

	vcap_rule_alloc_actionfield(&rule, &portfield, VCAP_AF_PORT_MASK, portval, buffer);
	strcpy(expect, expstr[2]);
	KUNIT_EXPECT_STREQ(test, expect, buffer);
	af = list_next_entry(af, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_PORT_MASK, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U72, af->ctrl.type);
	for (idx = 0; idx < 9; ++idx) {
		KUNIT_EXPECT_EQ(test, exp_portval[idx], af->data.u72.value[idx]);
	}

}

static void vcap_api_decode_rule_actionset_test(struct kunit *test)
{
	/* The action values below are from an actual Sparx5 rule config */
	u32 actwords[16] = { 0x00000002, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00100000, 0x06400010, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000 };
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.cache = {
			.actionstream = actwords,
		},
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
	};
	struct vcap_client_actionfield *kaf;
	int ret;

	INIT_LIST_HEAD(&rule.data.actionfields);
	ret = vcap_decode_rule_actionset(&rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule.data.actionfields);
	KUNIT_EXPECT_EQ(test, false, ret);

	kaf = list_first_entry(&rule.data.actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_PTR_NE(test, NULL, kaf);
	KUNIT_EXPECT_EQ(test, VCAP_AF_CNT_ID, kaf->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kaf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 100, kaf->data.u32.value);

	kaf = list_next_entry(kaf, ctrl.list);
	KUNIT_EXPECT_PTR_NE(test, NULL, kaf);
	KUNIT_EXPECT_EQ(test, VCAP_AF_MATCH_ID, kaf->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kaf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 1, kaf->data.u32.value);

	kaf = list_next_entry(kaf, ctrl.list);
	KUNIT_EXPECT_PTR_NE(test, NULL, kaf);
	KUNIT_EXPECT_EQ(test, VCAP_AF_MATCH_ID_MASK, kaf->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kaf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 1, kaf->data.u32.value);

	ret = list_is_last(&kaf->ctrl.list, &rule.data.actionfields);
	KUNIT_EXPECT_EQ(test, true, ret);
}

static void vcap_api_rule_add_keyvalue_test(struct kunit *test)
{
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
	};
	struct vcap_rule_internal ri = {
		.admin = &admin,
		.data = {
			.keyset = VCAP_KFS_NO_VALUE,
		},
	};
	struct vcap_rule *rule = (struct vcap_rule *)&ri;
	struct vcap_client_keyfield *kf;
	int ret;
	struct vcap_u64_key payload = {
		.value = {0x17, 0x26, 0x35, 0x44, 0x63, 0x62, 0x71},
		.mask = {0xf1, 0xf2, 0xf3, 0xf4, 0x4f, 0x3f, 0x2f, 0x1f},
	};
	int idx;

	INIT_LIST_HEAD(&rule->keyfields);
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_FIRST, VCAP_BIT_0);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->keyfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	kf = list_first_entry(&rule->keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_FIRST, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, kf->data.u1.value);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.mask);

	INIT_LIST_HEAD(&rule->keyfields);
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_FIRST, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->keyfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	kf = list_first_entry(&rule->keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_FIRST, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.value);
	KUNIT_EXPECT_EQ(test, 0x1, kf->data.u1.mask);

	INIT_LIST_HEAD(&rule->keyfields);
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_FIRST, VCAP_BIT_ANY);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->keyfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	kf = list_first_entry(&rule->keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_FIRST, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, kf->data.u1.value);
	KUNIT_EXPECT_EQ(test, 0x0, kf->data.u1.mask);

	INIT_LIST_HEAD(&rule->keyfields);
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_TYPE, 0x98765432, 0xff00ffab);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->keyfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	kf = list_first_entry(&rule->keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_TYPE, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, kf->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x98765432, kf->data.u32.value);
	KUNIT_EXPECT_EQ(test, 0xff00ffab, kf->data.u32.mask);

	INIT_LIST_HEAD(&rule->keyfields);
	ret = vcap_rule_add_key_u64(rule, VCAP_KF_L4_PAYLOAD, &payload);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->keyfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	kf = list_first_entry(&rule->keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_KF_L4_PAYLOAD, kf->ctrl.key);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U64, kf->ctrl.type);
	for (idx = 0; idx < ARRAY_SIZE(payload.value); ++idx)
		KUNIT_EXPECT_EQ(test, payload.value[idx], kf->data.u64.value[idx]);
	for (idx = 0; idx < ARRAY_SIZE(payload.mask); ++idx)
		KUNIT_EXPECT_EQ(test, payload.mask[idx], kf->data.u64.mask[idx]);
}

static void vcap_api_rule_add_actionvalue_test(struct kunit *test)
{
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
	};
	struct vcap_rule_internal ri = {
		.admin = &admin,
		.data = {
			.actionset = VCAP_AFS_NO_VALUE,
		},
	};
	struct vcap_rule *rule = (struct vcap_rule *)&ri;
	struct vcap_client_actionfield *af;
	int ret;
	struct vcap_u72_action portmask = {
		.value = {0x17, 0x26, 0x35, 0x44, 0x63, 0x62, 0x71, 0x8f},
	};
	int idx;

	INIT_LIST_HEAD(&rule->actionfields);
	ret = vcap_rule_add_action_bit(rule, VCAP_AF_POLICE_ENA, VCAP_BIT_0);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->actionfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	af = list_first_entry(&rule->actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_POLICE_ENA, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, af->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, af->data.u1.value);

	INIT_LIST_HEAD(&rule->actionfields);
	ret = vcap_rule_add_action_bit(rule, VCAP_AF_POLICE_ENA, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->actionfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	af = list_first_entry(&rule->actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_POLICE_ENA, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, af->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x1, af->data.u1.value);

	INIT_LIST_HEAD(&rule->actionfields);
	ret = vcap_rule_add_action_bit(rule, VCAP_AF_POLICE_ENA, VCAP_BIT_ANY);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->actionfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	af = list_first_entry(&rule->actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_POLICE_ENA, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_BIT, af->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x0, af->data.u1.value);

	INIT_LIST_HEAD(&rule->actionfields);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_TYPE, 0x98765432);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->actionfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	af = list_first_entry(&rule->actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_TYPE, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U32, af->ctrl.type);
	KUNIT_EXPECT_EQ(test, 0x98765432, af->data.u32.value);

	INIT_LIST_HEAD(&rule->actionfields);
	ret = vcap_rule_add_action_u72(rule, VCAP_AF_PORT_MASK, &portmask);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = list_empty(&rule->actionfields);
	KUNIT_EXPECT_EQ(test, false, ret);
	af = list_first_entry(&rule->actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_EQ(test, VCAP_AF_PORT_MASK, af->ctrl.action);
	KUNIT_EXPECT_EQ(test, VCAP_FIELD_U72, af->ctrl.type);
	for (idx = 0; idx < ARRAY_SIZE(portmask.value); ++idx)
		KUNIT_EXPECT_EQ(test, portmask.value[idx], af->data.u64.value[idx]);
}

static void vcap_api_rule_find_keyset_test(struct kunit *test)
{
	struct vcap_keyset_match match = {0};
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
	};
	struct vcap_rule_internal ri = {
		.admin = &admin,
	};
	struct vcap_client_keyfield ckf_1[] = {
		{
			.ctrl.key = VCAP_KF_TYPE,
		}, {
			.ctrl.key = VCAP_KF_FIRST,
		}, {
			.ctrl.key = VCAP_KF_IGR_PORT_MASK_L3,
		}, {
			.ctrl.key = VCAP_KF_IGR_PORT_MASK_RNG,
		}, {
			.ctrl.key = VCAP_KF_IGR_PORT_MASK,
		}, {
			.ctrl.key = VCAP_KF_L2_DMAC,
		}, {
			.ctrl.key = VCAP_KF_ETYPE_LEN,
		}, {
			.ctrl.key = VCAP_KF_ETYPE,
		},
	};
	struct vcap_client_keyfield ckf_2[] = {
		{
			.ctrl.key = VCAP_KF_TYPE,
		}, {
			.ctrl.key = VCAP_KF_FIRST,
		}, {
			.ctrl.key = VCAP_KF_ARP_OPCODE,
		}, {
			.ctrl.key = VCAP_KF_L3_IP4_SIP,
		}, {
			.ctrl.key = VCAP_KF_L3_IP4_DIP,
		}, {
			.ctrl.key = VCAP_KF_PCP,
		}, {
			.ctrl.key = VCAP_KF_ETYPE_LEN, /* Not with ARP */
		}, {
			.ctrl.key = VCAP_KF_ETYPE, /* Not with ARP */
		},
	};
	struct vcap_client_keyfield ckf_3[] = {
		{
			.ctrl.key = VCAP_KF_TYPE,
		}, {
			.ctrl.key = VCAP_KF_FIRST,
		}, {
			.ctrl.key = VCAP_KF_DEI,
		}, {
			.ctrl.key = VCAP_KF_PCP,
		}, {
			.ctrl.key = VCAP_KF_XVID,
		}, {
			.ctrl.key = VCAP_KF_ISDX,
		}, {
			.ctrl.key = VCAP_KF_L2_MC,
		}, {
			.ctrl.key = VCAP_KF_L2_BC,
		},
	};
	int idx;
	bool ret;
	enum vcap_keyfield_set keysets[10] = {0};
	enum vcap_key_field unmatched[10];

	vcap_api_set_client(&test_vctrl);
	INIT_LIST_HEAD(&ri.data.keyfields);
	for (idx = 0; idx < ARRAY_SIZE(ckf_1); idx++) {
		list_add_tail(&ckf_1[idx].ctrl.list, &ri.data.keyfields);
	}
	match.matches.keysets = keysets;
	match.matches.max = ARRAY_SIZE(keysets);
	match.unmatched_keys.keys = unmatched;
	match.unmatched_keys.max = ARRAY_SIZE(unmatched);
	ret = vcap_rule_find_keysets(&ri.data, &match);
	KUNIT_EXPECT_EQ(test, true, ret);
	KUNIT_EXPECT_EQ(test, 1, match.matches.cnt);

	INIT_LIST_HEAD(&ri.data.keyfields);
	for (idx = 0; idx < ARRAY_SIZE(ckf_2); idx++) {
		list_add_tail(&ckf_2[idx].ctrl.list, &ri.data.keyfields);
	}
	ret = vcap_rule_find_keysets(&ri.data, &match);
	KUNIT_EXPECT_EQ(test, false, ret);
	KUNIT_EXPECT_EQ(test, 0, match.matches.cnt);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_ARP, match.best_match);
	KUNIT_EXPECT_EQ(test, 2, match.unmatched_keys.cnt);

	INIT_LIST_HEAD(&ri.data.keyfields);
	for (idx = 0; idx < ARRAY_SIZE(ckf_3); idx++) {
		list_add_tail(&ckf_3[idx].ctrl.list, &ri.data.keyfields);
	}
	ret = vcap_rule_find_keysets(&ri.data, &match);
	KUNIT_EXPECT_EQ(test, true, ret);
	KUNIT_EXPECT_EQ(test, 5, match.matches.cnt);
}

static void vcap_api_rule_find_actionset_test(struct kunit *test)
{
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
	};
	struct vcap_rule_internal rule = {
		.admin = &admin,
	};
	struct vcap_client_actionfield caf_1[] = {
		{
			.ctrl.action = VCAP_AF_DSCP_ENA,
		}, {
			.ctrl.action = VCAP_AF_COSID_ENA,
		}, {
			.ctrl.action = VCAP_AF_QOS_ENA,
		}, {
			.ctrl.action = VCAP_AF_DP_ENA,
		}, {
			.ctrl.action = VCAP_AF_PCP_VAL,
		}, {
			.ctrl.action = VCAP_AF_MAP_KEY,
		}, {
			.ctrl.action = VCAP_AF_VLAN_POP_CNT,
		}, {
			.ctrl.action = VCAP_AF_MASK_MODE, /* only in FULL set */
		},
	};
	struct vcap_client_actionfield caf_2[] = {
		{
			.ctrl.action = VCAP_AF_TYPE,
		}, {
			.ctrl.action = VCAP_AF_LOG_MSG_INTERVAL, /* S2 only */
		}, {
			.ctrl.action = VCAP_AF_NXT_KEY_TYPE,
		}, {
			.ctrl.action = VCAP_AF_MPLS_MIP_ENA,
		}, {
			.ctrl.action = VCAP_AF_DP_ENA,
		}, {
			.ctrl.action = VCAP_AF_QOS_ENA,
		}, {
			.ctrl.action = VCAP_AF_COSID_ENA,
		}, {
			.ctrl.action = VCAP_AF_CPU_ENA,
		},
	};
	struct vcap_client_actionfield caf_3[] = {
		{
			.ctrl.action = VCAP_AF_TYPE,
		}, {
			.ctrl.action = VCAP_AF_COSID_ENA,
		}, {
			.ctrl.action = VCAP_AF_COSID_VAL,
		}, {
			.ctrl.action = VCAP_AF_QOS_ENA,
		}, {
			.ctrl.action = VCAP_AF_QOS_VAL,
		}, {
			.ctrl.action = VCAP_AF_DP_ENA,
		}, {
			.ctrl.action = VCAP_AF_DP_VAL,
		}, {
			.ctrl.action = VCAP_AF_MAP_LOOKUP_SEL,
		},
	};
	int idx;
	bool ret;
	enum vcap_actionfield_set actionsets[20] = {0};
	int count;

	vcap_api_set_client(&test_vctrl);
	INIT_LIST_HEAD(&rule.data.actionfields);
	for (idx = 0; idx < ARRAY_SIZE(caf_1); idx++) {
		list_add_tail(&caf_1[idx].ctrl.list, &rule.data.actionfields);
	}
	ret = vcap_rule_find_actionsets(&rule, ARRAY_SIZE(actionsets), actionsets, &count);
	KUNIT_EXPECT_EQ(test, true, ret);
	KUNIT_EXPECT_EQ(test, 1, count);

	INIT_LIST_HEAD(&rule.data.actionfields);
	for (idx = 0; idx < ARRAY_SIZE(caf_2); idx++) {
		list_add_tail(&caf_2[idx].ctrl.list, &rule.data.actionfields);
	}
	ret = vcap_rule_find_actionsets(&rule, ARRAY_SIZE(actionsets), actionsets, &count);
	KUNIT_EXPECT_EQ(test, false, ret);
	KUNIT_EXPECT_EQ(test, 0, count);

	INIT_LIST_HEAD(&rule.data.actionfields);
	for (idx = 0; idx < ARRAY_SIZE(caf_3); idx++) {
		list_add_tail(&caf_3[idx].ctrl.list, &rule.data.actionfields);
	}
	ret = vcap_rule_find_actionsets(&rule, ARRAY_SIZE(actionsets), actionsets, &count);
	KUNIT_EXPECT_EQ(test, true, ret);
	KUNIT_EXPECT_EQ(test, 4, count);
}

static void vcap_api_encode_rule_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[32] = {};
	static u32 mskdata[32] = {};
	static u32 actdata[32] = {};

	struct vcap_admin is2_admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule *rule = 0;
	struct vcap_rule_internal *ri = 0;
	int vcap_chain_id = 10005;
	enum vcap_user user = VCAP_USER_VCAP_UTIL;
	u16 priority = 10;
	int id = 100;
	int ret;
	struct vcap_u48_key smac = {
		.value = { 0x88, 0x75, 0x32, 0x34, 0x9e, 0xb1 },
		.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
	};
	struct vcap_u48_key dmac = {
		.value = { 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 },
		.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
	};
	u32 port_mask_rng_value = 0x05;
	u32 port_mask_rng_mask = 0x0f;
	u32 igr_port_mask_value = 0xffabcd01;
	u32 igr_port_mask_mask = ~0;
	struct vcap_u64_key payload = {
		.value = { 0x81, 0, 0, 0, 0x20, 0, 0, 0x90},
		.mask = { 0xff, 0, 0, 0, 0, 0, 0, 0xff},
	};
	struct vcap_u72_action port_mask_act = {
		.value = {0x89, 0x45, 0x32, 0xf3, 0x15, 0x01, 0x67, 0x14, 0x45},
	};
	u32 expwriteaddr[] = {792, 793, 794, 795, 796, 797, 792, 0}; /* 2nd last is counter */
	int idx;

	vcap_test_api_init(&is2_admin);

	/* Allocate the rule */
	rule = vcap_alloc_rule(&netdev, vcap_chain_id, user, priority, id);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	ri = (struct vcap_rule_internal *)rule;

	/* Add rule keys */
	ret = vcap_rule_add_key_u48(rule, VCAP_KF_L2_DMAC, &dmac);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u48(rule, VCAP_KF_L2_SMAC, &smac);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_ETYPE_LEN, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	/* Cannot add the same field twice */
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_ETYPE_LEN, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_IGR_PORT_MASK_L3, VCAP_BIT_ANY);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_IGR_PORT_MASK_RNG, port_mask_rng_value, port_mask_rng_mask);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_IGR_PORT_MASK, igr_port_mask_value, igr_port_mask_mask);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u64(rule, VCAP_KF_L2_PAYLOAD_ETYPE, &payload);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add rule actions */
	ret = vcap_rule_add_action_bit(rule, VCAP_AF_POLICE_ENA, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_CNT_ID, id);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_MATCH_ID, 1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_MATCH_ID_MASK, 1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u72(rule, VCAP_AF_PORT_MASK, &port_mask_act);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Validation with validate keyset callback */
	ret = vcap_val_rule(rule, ETH_P_ALL);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_MAC_ETYPE, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);
	KUNIT_EXPECT_EQ(test, 2, ri->keyset_sw_regs);
	KUNIT_EXPECT_EQ(test, 4, ri->actionset_sw_regs);

	/* Add rule with write callback */
	ret = vcap_add_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 792, is2_admin.last_used_addr);

	/* Check that the rule has been added */
	ret = list_empty(&is2_admin.rules);
	KUNIT_EXPECT_EQ(test, false, ret);
	KUNIT_EXPECT_EQ(test, 0, ret);
	vcap_free_rule(rule);

	/* Check that the rule has been freed: tricky to access since this
	 * memory should not be accessible anymore
	 */
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	ret = list_empty(&rule->keyfields);
	KUNIT_EXPECT_EQ(test, true, ret);
	ret = list_empty(&rule->actionfields);
	KUNIT_EXPECT_EQ(test, true, ret);

	for (idx = 0; idx < ARRAY_SIZE(expwriteaddr); ++idx)
		KUNIT_EXPECT_EQ(test, expwriteaddr[idx], test_updateaddr[idx]);
}

static void vcap_api_decode_rule_test(struct kunit *test)
{
	u32 keydata[] = {
		0x40450042, 0x000feaf3, 0x00000003, 0x00050600,
		0x10203040, 0x00075880, 0x633c6864, 0x00040003,
		0x00000020, 0x00000008, 0x00000240, 0x00000000,
	};
	u32 mskdata[] = {
		0x0030ff80, 0xfff00000, 0xfffffffc, 0xfff000ff,
		0x00000000, 0xfff00000, 0x00000000, 0xfff3fffc,
		0xffffffc0, 0xffffffff, 0xfffffc03, 0xffffffff,
	};
	u32 actdata[] = {
		0x00040002, 0xf3324589, 0x14670115, 0x00000005,
		0x00000000, 0x00100000, 0x06400010, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
	};
	struct vcap_admin is2_admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 794,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule_internal admin_rule = {
		.admin = &is2_admin,
		.data = {
			.id = 100,
			.keyset = VCAP_KFS_MAC_ETYPE,
			.actionset = VCAP_AFS_BASE_TYPE,
		},
		.size = 6,
		.keyset_sw_regs = 2,
		.actionset_sw_regs = 4,
		.addr = 794,
	};
	struct vcap_rule *rule;
	struct vcap_client_keyfield *ckf;
	struct vcap_client_actionfield *caf;
	const struct vcap_field *keyfields;
	const struct vcap_field *actfields;
	struct vcap_client_keyfield expkey[] = {
		{
			.ctrl = {
				.key = VCAP_KF_ETYPE_LEN,
				.type = VCAP_FIELD_BIT,
			},
			.data.u1 = {
				.value = 1,
				.mask = 1,
			},
		},{
			.ctrl = {
				.key = VCAP_KF_FIRST,
				.type = VCAP_FIELD_BIT,
			},
			.data.u1 = {
				.value = 1,
				.mask = 1,
			},
		},{
			.ctrl = {
				.key = VCAP_KF_IGR_PORT_MASK,
				.type = VCAP_FIELD_U32,
			},
			.data.u32 = {
				.value = 0xffabcd01,
				.mask = ~0,
			},
		},{
			.ctrl = {
				.key = VCAP_KF_IGR_PORT_MASK_RNG,
				.type = VCAP_FIELD_U32,
			},
			.data.u32 = {
				.value = 0x05,
				.mask = 0x0f,
			},
		},{
			.ctrl = {
				.key = VCAP_KF_L2_DMAC,
				.type = VCAP_FIELD_U48,
			},
			.data.u48 = {
				.value = { 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 },
				.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
			},
		},{
			.ctrl = {
				.key = VCAP_KF_L2_PAYLOAD_ETYPE,
				.type = VCAP_FIELD_U64,
			},
			.data.u64 = {
				.value = { 0x81, 0, 0, 0, 0x20, 0, 0, 0x90},
				.mask = { 0xff, 0, 0, 0, 0, 0, 0, 0xff},
			},
		},{
			.ctrl = {
				.key = VCAP_KF_L2_SMAC,
				.type = VCAP_FIELD_U48,
			},
			.data.u48 = {
				.value = { 0x88, 0x75, 0x32, 0x34, 0x9e, 0xb1 },
				.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
			},
		},{
			.ctrl = {
				.key = VCAP_KF_TYPE,
				.type = VCAP_FIELD_U32,
			},
			.data.u32 = {
				.value = 0,
				.mask = 0xf,
			},
		},{
		},
	};
	struct vcap_client_actionfield expact[] = {
		{
			.ctrl = {
				.action = VCAP_AF_CNT_ID,
				.type = VCAP_FIELD_U32,
			},
			.data.u32 = {
				.value = 100,
			},
		},{
			.ctrl = {
				.action = VCAP_AF_MATCH_ID,
				.type = VCAP_FIELD_U32,
			},
			.data.u32 = {
				.value = 1,
			},
		},{
			.ctrl = {
				.action = VCAP_AF_MATCH_ID_MASK,
				.type = VCAP_FIELD_U32,
			},
			.data.u32 = {
				.value = 1,
			},
		},{
			.ctrl = {
				.action = VCAP_AF_POLICE_ENA,
				.type = VCAP_FIELD_BIT,
			},
			.data.u32 = {
				.value = 1,
			},
		},{
			.ctrl = {
				.action = VCAP_AF_PORT_MASK,
				.type = VCAP_FIELD_U72,
			},
			.data.u72 = {
				/* The field is only 68 bits wide so the MS 4
				 * bits in the original data is not present when
				 * reading back
				 */
				.value = {0x89, 0x45, 0x32, 0xf3, 0x15, 0x01, 0x67, 0x14, 0x05},
			},
		},{
		},
	};
	int idx, ret, field_size;

	vcap_test_api_init(&is2_admin);
	list_add_tail(&admin_rule.list, &is2_admin.rules);

	rule = vcap_get_rule(&netdev, 100);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ckf = list_first_entry(&rule->keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_PTR_NE(test, NULL, ckf);
	keyfields = test_vcaps[is2_admin.vtype].keyfield_set_map[rule->keyset];
	idx = 0;
	list_for_each_entry(ckf, &rule->keyfields, ctrl.list) {
		pr_debug("%s:%d: key: %d, type: %d: %s\n",
			__func__, __LINE__,
			ckf->ctrl.key,
			ckf->ctrl.type,
			test_vctrl.stats->keyfield_names[ckf->ctrl.key]);
		KUNIT_EXPECT_EQ(test, expkey[idx].ctrl.key, ckf->ctrl.key);
		KUNIT_EXPECT_EQ(test, expkey[idx].ctrl.type, ckf->ctrl.type);
		field_size = keyfield_size_table[ckf->ctrl.type];
		ret = memcmp(&expkey[idx].data, &ckf->data, field_size);
		if (ret) {
			print_hex_dump(KERN_INFO, "exp: ", DUMP_PREFIX_OFFSET, 16, 1, &expkey[idx].data, field_size, true);
			print_hex_dump(KERN_INFO, "act: ", DUMP_PREFIX_OFFSET, 16, 1, &ckf->data, field_size, true);
		}
		KUNIT_EXPECT_EQ(test, 0, ret);
		idx++;
	}

	caf = list_first_entry(&rule->actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_PTR_NE(test, NULL, caf);
	actfields = test_vcaps[is2_admin.vtype].actionfield_set_map[rule->actionset];
	idx = 0;
	list_for_each_entry(caf, &rule->actionfields, ctrl.list) {
		pr_debug("%s:%d: action: %d, type: %d: %s\n",
			__func__, __LINE__,
			caf->ctrl.action,
			caf->ctrl.type,
			test_vctrl.stats->actionfield_names[caf->ctrl.action]);
		if (idx >= ARRAY_SIZE(expact))
			continue;
		KUNIT_EXPECT_EQ(test, expact[idx].ctrl.action, caf->ctrl.action);
		KUNIT_EXPECT_EQ(test, expact[idx].ctrl.type, caf->ctrl.type);
		field_size = actionfield_size_table[caf->ctrl.type];
		ret = memcmp(&expact[idx].data, &caf->data, field_size);
		if (ret) {
			print_hex_dump(KERN_INFO, "exp: ", DUMP_PREFIX_OFFSET, 16, 1, &expact[idx].data, field_size, true);
			print_hex_dump(KERN_INFO, "act: ", DUMP_PREFIX_OFFSET, 16, 1, &caf->data, field_size, true);
		}
		KUNIT_EXPECT_EQ(test, 0, ret);
		idx++;
	}

	/* Free the rule again */
	vcap_free_rule(rule);

	/* Check that the rule has been freed: tricky to access since this
	 * memory should not be accessible anymore
	 */
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	ret = list_empty(&rule->keyfields);
	KUNIT_EXPECT_EQ(test, true, ret);
	ret = list_empty(&rule->actionfields);
	KUNIT_EXPECT_EQ(test, true, ret);
}

static void vcap_api_addr_keyset_test(struct kunit *test)
{
	u32 keydata[12] = {
		0x40450042, 0x000feaf3, 0x00000003, 0x00050600,
		0x10203040, 0x00075880, 0x633c6864, 0x00040003,
		0x00000020, 0x00000008, 0x00000240, 0x00000000,
	};
	u32 mskdata[12] = {
		0x0030ff80, 0xfff00000, 0xfffffffc, 0xfff000ff,
		0x00000000, 0xfff00000, 0x00000000, 0xfff3fffc,
		0xffffffc0, 0xffffffff, 0xfffffc03, 0xffffffff,
	};
	u32 actdata[12] = {};
	struct vcap_admin is2_admin = {
		.vtype = VCAP_TYPE_IS2,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	int ret, idx, addr;

	vcap_test_api_init(&is2_admin);
	list_add_tail(&is2_admin.list, &test_vctrl.list);

	/* Go from higher to lower addresses searching for a keyset */
	for (idx = ARRAY_SIZE(keydata) - 1, addr = 799; idx > 0; --idx, --addr) {
		is2_admin.cache.keystream = &keydata[idx];
		is2_admin.cache.maskstream = &mskdata[idx];
		ret = vcap_addr_keyset(&netdev, &is2_admin,  addr);
		KUNIT_EXPECT_EQ(test, -EINVAL, ret);
	}

	/* Finally we hit the start of the rule */
	is2_admin.cache.keystream = &keydata[idx];
	is2_admin.cache.maskstream = &mskdata[idx];
	ret = vcap_addr_keyset(&netdev, &is2_admin,  addr);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_MAC_ETYPE, ret);
}

static const char *test_explog[] = {
	"name: kunit_s2_vcap\n",
	"rows: 256\n",
	"sw_count: 12\n",
	"sw_width: 52\n",
	"sticky_width: 1\n",
	"act_width: 110\n",
	"default_cnt: 73\n",
	"require_cnt_dis: 0\n",
	"version: 1\n",
	"vtype: 4\n",
	"vinst: 0\n",
	"first_cid: 10000\n",
	"last_cid: 19999\n",
	"lookups: 4\n",
	"first_valid_addr: 0\n",
	"last_valid_addr: 3071\n",
	"last_used_addr: 794\n",
	"rule: 100, addr: [794,799], counter[0]: 0, hit: 0\n",
	"  id: 100\n",
	"  vcap_chain_id: 0\n",
	"  user: 0\n",
	"  priority: 0\n",
	"  keyset: VCAP_KFS_MAC_ETYPE\n",
	"  actionset: VCAP_AFS_BASE_TYPE\n",
	"  sort_key: 0x00000000\n",
	"  keyset_sw: 6\n",
	"  actionset_sw: 3\n",
	"  keyset_sw_regs: 2\n",
	"  actionset_sw_regs: 4\n",
	"  size: 6\n",
	"  addr: 794\n",
	"  keyfields:\n",
	"    ETYPE_LEN: bit: 1/1\n",
	"    FIRST: bit: 1/1\n",
	"    IGR_PORT_MASK: u32 (4289449217): 1111.1111.1010.1011.1100.1101.0000.0001/1111.1111.1111.1111.1111.1111.1111.1111\n",
	"    IGR_PORT_MASK_RNG: u32 (5): 0101/1111\n",
	"    L2_DMAC: u48: 0000.0001.0000.0010.0000.0011.0000.0100.0000.0101.0000.0110/1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111\n",
	"    L2_PAYLOAD_ETYPE: u64: 1001.0000.0000.0000.0000.0000.0010.0000.0000.0000.0000.0000.0000.0000.1000.0001/1111.1111.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.1111.1111\n",
	"    L2_SMAC: u48: 1011.0001.1001.1110.0011.0100.0011.0010.0111.0101.1000.1000/1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111\n",
	"    TYPE: u32 (0): 0000/1111\n",
	"  actionfields:\n",
	"    CNT_ID: u32 (100): 0000.0110.0100\n",
	"    MATCH_ID: u32 (1): 0000.0000.0000.0001\n",
	"    MATCH_ID_MASK: u32 (1): 0000.0000.0000.0001\n",
	"    POLICE_ENA: bit: 1\n",
	"    PORT_MASK: u72: 0101.0001.0100.0110.0111.0000.0001.0001.0101.1111.0011.0011.0010.0100.0101.1000.1001\n",
	"  counter: 0\n",
	"  counter_sticky: 0\n",
};

static void vcap_api_show_admin_test(struct kunit *test)
{
	u32 keydata[] = {
		0x40450042, 0x000feaf3, 0x00000003, 0x00050600,
		0x10203040, 0x00075880, 0x633c6864, 0x00040003,
		0x00000020, 0x00000008, 0x00000240, 0x00000000,
	};
	u32 mskdata[] = {
		0x0030ff80, 0xfff00000, 0xfffffffc, 0xfff000ff,
		0x00000000, 0xfff00000, 0x00000000, 0xfff3fffc,
		0xffffffc0, 0xffffffff, 0xfffffc03, 0xffffffff,
	};
	u32 actdata[] = {
		0x00040002, 0xf3324589, 0x14670115, 0x00000005,
		0x00000000, 0x00100000, 0x06400010, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
	};
	struct vcap_admin is2_admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 794,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule_internal admin_rule = {
		.admin = &is2_admin,
		.data = {
			.id = 100,
			.keyset = VCAP_KFS_MAC_ETYPE,
			.actionset = VCAP_AFS_BASE_TYPE,
		},
		.size = 6,
		.keyset_sw_regs = 2,
		.actionset_sw_regs = 4,
		.addr = 794,
	};
	int ret, idx;

	vcap_test_api_init(&is2_admin);
	list_add_tail(&admin_rule.list, &is2_admin.rules);


	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &is2_admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = 0; idx < test_pr_bufferidx; ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, test_explog[idx], test_pr_buffer[idx]);
	}
}

static void vcap_api_set_rule_counter_test(struct kunit *test)
{
	struct vcap_admin is2_admin = {
		.cache = {
			.counter = 100,
			.sticky = true,
		},
	};
	struct vcap_rule_internal ri = {
		.data = {
			.id = 1001,
		},
		.addr = 600,
		.admin = &is2_admin,
		.counter_id = 1002,
	};
	struct vcap_rule_internal ri2 = {
		.data = {
			.id = 2001,
		},
		.addr = 700,
		.admin = &is2_admin,
		.counter_id = 2002,
	};
	struct vcap_counter ctr = { .value = 0, .sticky = false};
	struct vcap_counter ctr2 = { .value = 101, .sticky = true};
	int ret;

	vcap_test_api_init(&is2_admin);
	list_add_tail(&ri.list, &is2_admin.rules);
	list_add_tail(&ri2.list, &is2_admin.rules);

	pr_info("%s:%d\n", __func__, __LINE__);
	ret = vcap_rule_set_counter(1001, &ctr);
	pr_info("%s:%d\n", __func__, __LINE__);
	KUNIT_EXPECT_EQ(test, 0, ret);

	KUNIT_EXPECT_EQ(test, 1002, test_hw_counter_id);
	KUNIT_EXPECT_EQ(test, 0, test_hw_cache.counter);
	KUNIT_EXPECT_EQ(test, false, test_hw_cache.sticky);
	KUNIT_EXPECT_EQ(test, 600, test_updateaddr[0]);

	ret = vcap_rule_set_counter(2001, &ctr2);
	KUNIT_EXPECT_EQ(test, 0, ret);

	KUNIT_EXPECT_EQ(test, 2002, test_hw_counter_id);
	KUNIT_EXPECT_EQ(test, 101, test_hw_cache.counter);
	KUNIT_EXPECT_EQ(test, true, test_hw_cache.sticky);
	KUNIT_EXPECT_EQ(test, 700, test_updateaddr[1]);
}

static void vcap_api_get_rule_counter_test(struct kunit *test)
{
	struct vcap_admin is2_admin = {
		.cache = {
			.counter = 100,
			.sticky = true,
		},
	};
	struct vcap_rule_internal ri = {
		.data = {
			.id = 1010,
		},
		.addr = 400,
		.admin = &is2_admin,
		.counter_id = 1011,
	};
	struct vcap_rule_internal ri2 = {
		.data = {
			.id = 2011,
		},
		.addr = 300,
		.admin = &is2_admin,
		.counter_id = 2012,
	};
	struct vcap_counter ctr = {};
	struct vcap_counter ctr2 = {};
	int ret;

	vcap_test_api_init(&is2_admin);
	test_hw_cache.counter = 55;
	test_hw_cache.sticky = true;

	list_add_tail(&ri.list, &is2_admin.rules);
	list_add_tail(&ri2.list, &is2_admin.rules);

	ret = vcap_rule_get_counter(1010, &ctr);
	KUNIT_EXPECT_EQ(test, 0, ret);

	KUNIT_EXPECT_EQ(test, 1011, test_hw_counter_id);
	KUNIT_EXPECT_EQ(test, 55, ctr.value);
	KUNIT_EXPECT_EQ(test, true, ctr.sticky);
	KUNIT_EXPECT_EQ(test, 400, test_updateaddr[0]);

	test_hw_cache.counter = 22;
	test_hw_cache.sticky = false;

	ret = vcap_rule_get_counter(2011, &ctr2);
	KUNIT_EXPECT_EQ(test, 0, ret);

	KUNIT_EXPECT_EQ(test, 2012, test_hw_counter_id);
	KUNIT_EXPECT_EQ(test, 22, ctr2.value);
	KUNIT_EXPECT_EQ(test, false, ctr2.sticky);
	KUNIT_EXPECT_EQ(test, 300, test_updateaddr[1]);
}

static struct vcap_rule *
test_vcap_xn_rule_creator(struct kunit *test, int cid, enum vcap_user user,
		       u16 priority,
		       int id, int size, int expected_addr)
{
	struct vcap_rule *rule = 0;
	struct vcap_rule_internal *ri = 0;
	enum vcap_keyfield_set keyset = VCAP_KFS_NO_VALUE;
	enum vcap_actionfield_set actionset = VCAP_AFS_NO_VALUE;
	int ret;

	/* init before testing */
	memset(test_updateaddr, 0, sizeof(test_updateaddr));
	test_updateaddridx = 0;
	test_move_addr = 0;
	test_move_offset = 0;
	test_move_count = 0;

	switch (size) {
	case 2:
		keyset = VCAP_KFS_ETAG;
		actionset = VCAP_AFS_CLASS_REDUCED;
		break;
	case 3:
		keyset = VCAP_KFS_PURE_5TUPLE_IP4;
		actionset = VCAP_AFS_CLASSIFICATION;
		break;
	case 6:
		keyset = VCAP_KFS_NORMAL_5TUPLE_IP4;
		actionset = VCAP_AFS_CLASSIFICATION;
		break;
	case 12:
		keyset = VCAP_KFS_NORMAL_7TUPLE;
		actionset = VCAP_AFS_FULL;
		break;
	default:
		break;
	}

	/* Check that a valid size was used */
	KUNIT_ASSERT_NE(test, VCAP_KFS_NO_VALUE, keyset);

	/* Allocate the rule */
	rule = vcap_alloc_rule(&netdev, cid, user, priority, id);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Override rule keyset */
	ret = vcap_set_rule_set_keyset(rule, keyset);

	/* Add rule actions : there must be at least one action */
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_COSID_VAL, 0);

	/* Override rule actionset */
	ret = vcap_set_rule_set_actionset(rule, actionset);

	ret = vcap_val_rule(rule, ETH_P_ALL);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, keyset, rule->keyset);
	KUNIT_EXPECT_EQ(test, actionset, rule->actionset);
	KUNIT_EXPECT_EQ(test, size, ri->size);

	/* Add rule with write callback */
	ret = vcap_add_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, expected_addr, ri->addr);
	return rule;
}

static void vcap_api_rule_insert_in_order_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[32] = {};
	static u32 mskdata[32] = {};
	static u32 actdata[32] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};

	vcap_test_api_init(&admin);

	/* Create rules with different sizes and check that they are placed
	 * at the correct address in the VCAP according to size
	 */
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 10, 500, 12, 780);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 20, 400, 6, 774);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 30, 300, 3, 771);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 40, 200, 2, 768);
}

static void vcap_api_rule_insert_reverse_order_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[32] = {};
	static u32 mskdata[32] = {};
	static u32 actdata[32] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule_internal *elem;
	u32 exp_addr[] = {780, 774, 771, 768, 767};
	int idx;

	vcap_test_api_init(&admin);

	/* Create rules with different sizes and check that they are placed
	 * at the correct address in the VCAP according to size
	 */
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 20, 200, 2, 798);
	KUNIT_EXPECT_EQ(test, 0, test_move_offset);
	KUNIT_EXPECT_EQ(test, 0, test_move_count);
	KUNIT_EXPECT_EQ(test, 0, test_move_addr);

	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 30, 300, 3, 795);
	KUNIT_EXPECT_EQ(test, 6, test_move_offset);
	KUNIT_EXPECT_EQ(test, 3, test_move_count);
	KUNIT_EXPECT_EQ(test, 798, test_move_addr);

	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 40, 400, 6, 792);
	KUNIT_EXPECT_EQ(test, 6, test_move_offset);
	KUNIT_EXPECT_EQ(test, 6, test_move_count);
	KUNIT_EXPECT_EQ(test, 792, test_move_addr);

	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 50, 500, 12, 780);
	KUNIT_EXPECT_EQ(test, 18, test_move_offset);
	KUNIT_EXPECT_EQ(test, 12, test_move_count);
	KUNIT_EXPECT_EQ(test, 786, test_move_addr);

	idx = 0;
	list_for_each_entry(elem, &admin.rules, list) {
		KUNIT_EXPECT_EQ(test, exp_addr[idx], elem->addr);
		++idx;
	}
	KUNIT_EXPECT_EQ(test, 768, admin.last_used_addr);
}

static void vcap_api_rule_remove_at_end_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[32] = {};
	static u32 mskdata[32] = {};
	static u32 actdata[32] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	int ret;

	vcap_test_api_init(&admin);
	test_move_addr = 0;
	test_move_offset = 0;
	test_move_count = 0;
	test_init_start = 0;
	test_init_count = 0;

	/* Create rules with different sizes and check that they are placed
	 * at the correct address in the VCAP according to size
	 */
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 10, 500, 12, 780);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 20, 400, 6, 774);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 30, 300, 3, 771);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 40, 200, 2, 768);

	/* Remove rules again from the end */
	ret = vcap_del_rule(&netdev, 200);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 0, test_move_addr);
	KUNIT_EXPECT_EQ(test, 0, test_move_offset);
	KUNIT_EXPECT_EQ(test, 0, test_move_count);
	KUNIT_EXPECT_EQ(test, 768, test_init_start);
	KUNIT_EXPECT_EQ(test, 2, test_init_count);
	KUNIT_EXPECT_EQ(test, 771, admin.last_used_addr);

	ret = vcap_del_rule(&netdev, 300);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, 0, test_move_addr);
	KUNIT_EXPECT_EQ(test, 0, test_move_offset);
	KUNIT_EXPECT_EQ(test, 0, test_move_count);
	KUNIT_EXPECT_EQ(test, 771, test_init_start);
	KUNIT_EXPECT_EQ(test, 3, test_init_count);
	KUNIT_EXPECT_EQ(test, 774, admin.last_used_addr);

	ret = vcap_del_rule(&netdev, 400);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, 0, test_move_addr);
	KUNIT_EXPECT_EQ(test, 0, test_move_offset);
	KUNIT_EXPECT_EQ(test, 0, test_move_count);
	KUNIT_EXPECT_EQ(test, 774, test_init_start);
	KUNIT_EXPECT_EQ(test, 6, test_init_count);
	KUNIT_EXPECT_EQ(test, 780, admin.last_used_addr);

	ret = vcap_del_rule(&netdev, 500);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, 0, test_move_addr);
	KUNIT_EXPECT_EQ(test, 0, test_move_offset);
	KUNIT_EXPECT_EQ(test, 0, test_move_count);
	KUNIT_EXPECT_EQ(test, 780, test_init_start);
	KUNIT_EXPECT_EQ(test, 12, test_init_count);
	KUNIT_EXPECT_EQ(test, 3071, admin.last_used_addr);
}

static void test_init_rule_deletion(void)
{
	test_move_addr = 0;
	test_move_offset = 0;
	test_move_count = 0;
	test_init_start = 0;
	test_init_count = 0;
}

static void vcap_api_rule_remove_in_middle_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[32] = {};
	static u32 mskdata[32] = {};
	static u32 actdata[32] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.last_valid_addr = 800 - 1,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	int ret;

	vcap_test_api_init(&admin);

	/* Create rules with different sizes and check that they are placed
	 * at the correct address in the VCAP according to size
	 */
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 10, 500, 12, 780);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 20, 400, 6, 774);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 30, 300, 3, 771);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 40, 200, 2, 768);

	/* Remove rules in the middle */
	test_init_rule_deletion();
	ret = vcap_del_rule(&netdev, 400);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 768, test_move_addr);
	KUNIT_EXPECT_EQ(test, -6, test_move_offset);
	KUNIT_EXPECT_EQ(test, 6, test_move_count);
	KUNIT_EXPECT_EQ(test, 768, test_init_start);
	KUNIT_EXPECT_EQ(test, 6, test_init_count);
	KUNIT_EXPECT_EQ(test, 774, admin.last_used_addr);

	test_init_rule_deletion();
	ret = vcap_del_rule(&netdev, 300);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 774, test_move_addr);
	KUNIT_EXPECT_EQ(test, -4, test_move_offset);
	KUNIT_EXPECT_EQ(test, 2, test_move_count);
	KUNIT_EXPECT_EQ(test, 774, test_init_start);
	KUNIT_EXPECT_EQ(test, 4, test_init_count);
	KUNIT_EXPECT_EQ(test, 778, admin.last_used_addr);

	test_init_rule_deletion();
	ret = vcap_del_rule(&netdev, 500);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 778, test_move_addr);
	KUNIT_EXPECT_EQ(test, -20, test_move_offset);
	KUNIT_EXPECT_EQ(test, 2, test_move_count);
	KUNIT_EXPECT_EQ(test, 778, test_init_start);
	KUNIT_EXPECT_EQ(test, 20, test_init_count);
	KUNIT_EXPECT_EQ(test, 798, admin.last_used_addr);

	test_init_rule_deletion();
	ret = vcap_del_rule(&netdev, 200);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 0, test_move_addr);
	KUNIT_EXPECT_EQ(test, 0, test_move_offset);
	KUNIT_EXPECT_EQ(test, 0, test_move_count);
	KUNIT_EXPECT_EQ(test, 798, test_init_start);
	KUNIT_EXPECT_EQ(test, 2, test_init_count);
	KUNIT_EXPECT_EQ(test, 799, admin.last_used_addr);
}

static void vcap_api_rule_remove_in_front_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[32] = {};
	static u32 mskdata[32] = {};
	static u32 actdata[32] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.last_valid_addr = 800 - 1,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	int ret;

	vcap_test_api_init(&admin);

	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 10, 500, 12, 780);
	KUNIT_EXPECT_EQ(test, 780, admin.last_used_addr);

	test_init_rule_deletion();
	ret = vcap_del_rule(&netdev, 500);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 0, test_move_addr);
	KUNIT_EXPECT_EQ(test, 0, test_move_offset);
	KUNIT_EXPECT_EQ(test, 0, test_move_count);
	KUNIT_EXPECT_EQ(test, 780, test_init_start);
	KUNIT_EXPECT_EQ(test, 12, test_init_count);
	KUNIT_EXPECT_EQ(test, 799, admin.last_used_addr);

	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 20, 400, 6, 792);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 30, 300, 3, 789);
	test_vcap_xn_rule_creator(test, 10000, VCAP_USER_QOS, 40, 200, 2, 786);

	test_init_rule_deletion();
	ret = vcap_del_rule(&netdev, 400);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 786, test_move_addr);
	KUNIT_EXPECT_EQ(test, -8, test_move_offset);
	KUNIT_EXPECT_EQ(test, 6, test_move_count);
	KUNIT_EXPECT_EQ(test, 786, test_init_start);
	KUNIT_EXPECT_EQ(test, 8, test_init_count);
	KUNIT_EXPECT_EQ(test, 794, admin.last_used_addr);
}

static struct vcap_rule *
test_is0_rule_creator(struct kunit *test, int cid, enum vcap_user user,
		       u16 priority,
		       int id, int size, int expected_addr)
{
	struct vcap_rule *rule = 0;
	struct vcap_rule_internal *ri = 0;
	enum vcap_keyfield_set keyset = VCAP_KFS_NO_VALUE;
	enum vcap_actionfield_set actionset = VCAP_AFS_NO_VALUE;
	int ret;
	enum vcap_key_field key;
	enum vcap_action_field action;
	u32 kval, mask, aval;

	/* init before testing */
	memset(test_updateaddr, 0, sizeof(test_updateaddr));
	test_updateaddridx = 0;
	test_move_addr = 0;
	test_move_offset = 0;
	test_move_count = 0;

	switch (size) {
	case 6:
		keyset = VCAP_KFS_NORMAL_5TUPLE_IP4;
		actionset = VCAP_AFS_CLASSIFICATION;
		key = VCAP_KF_PCP0;
		kval = 3;
		mask = 0x3;
		action = VCAP_AF_PIPELINE_PT;
		aval = 10;
		break;
	case 12:
		keyset = VCAP_KFS_NORMAL_7TUPLE;
		actionset = VCAP_AFS_FULL;
		key = VCAP_KF_L4_SPORT;
		kval = 23000;
		mask = 0xffff;
		action = VCAP_AF_MATCH_ID;
		aval = 40000;
		break;
	default:
		break;
	}

	/* Check that a valid size was used */
	KUNIT_ASSERT_NE(test, VCAP_KFS_NO_VALUE, keyset);

	/* Allocate the rule */
	rule = vcap_alloc_rule(&netdev, cid, user, priority, id);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Add common keys (between the two rule sizes) */
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_G_IDX, 3127, 0xfff);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_TPID0, 5, 0x7);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_rule_add_key_u32(rule, key, kval, mask);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Override rule keyset */
	ret = vcap_set_rule_set_keyset(rule, keyset);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add common actions (between the two rule sizes) */
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_CPU_Q, 2);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_PAG_VAL, 13);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add rule actions */
	ret = vcap_rule_add_action_u32(rule, action, aval);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Override rule actionset */
	ret = vcap_set_rule_set_actionset(rule, actionset);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_val_rule(rule, ETH_P_ALL);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, keyset, rule->keyset);
	KUNIT_EXPECT_EQ(test, actionset, rule->actionset);
	KUNIT_EXPECT_EQ(test, size, ri->size);

	/* Add rule with write callback */
	ret = vcap_add_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, expected_addr, ri->addr);
	return rule;
}

static struct vcap_rule *
test_is2_rule_creator(struct kunit *test, int cid, enum vcap_user user,
		       u16 priority,
		       int id, int size, int expected_addr)
{
	struct vcap_rule *rule = 0;
	struct vcap_rule_internal *ri = 0;
	enum vcap_keyfield_set keyset = VCAP_KFS_NO_VALUE;
	enum vcap_actionfield_set actionset = VCAP_AFS_NO_VALUE;
	int ret;
	enum vcap_key_field key;
	enum vcap_action_field action;
	u32 kval, mask, aval;

	/* init before testing */
	memset(test_updateaddr, 0, sizeof(test_updateaddr));
	test_updateaddridx = 0;
	test_move_addr = 0;
	test_move_offset = 0;
	test_move_count = 0;

	switch (size) {
	case 6:
		keyset = VCAP_KFS_ARP;
		actionset = VCAP_AFS_BASE_TYPE;
		key = VCAP_KF_ARP_OPCODE;
		kval = 2;
		mask = 0x3;
		action = VCAP_AF_PIPELINE_PT;
		aval = 10;
		break;
	case 12:
		keyset = VCAP_KFS_IP_7TUPLE;
		actionset = VCAP_AFS_BASE_TYPE;
		key = VCAP_KF_L4_DPORT;
		kval = 1024;
		mask = 0xffff;
		action = VCAP_AF_CPU_QU_NUM;
		aval = 3;
		break;
	default:
		break;
	}

	/* Check that a valid size was used */
	KUNIT_ASSERT_NE(test, VCAP_KFS_NO_VALUE, keyset);

	/* Allocate the rule */
	rule = vcap_alloc_rule(&netdev, cid, user, priority, id);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Add common keys (between the two rule sizes) */
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_PAG, 127, 0xff);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_PCP, 5, 0x7);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_rule_add_key_u32(rule, key, kval, mask);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Override rule keyset */
	ret = vcap_set_rule_set_keyset(rule, keyset);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add common actions (between the two rule sizes) */
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_MATCH_ID, 40000);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_LOG_MSG_INTERVAL, 13);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add rule actions */
	ret = vcap_rule_add_action_u32(rule, action, aval);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Override rule actionset */
	ret = vcap_set_rule_set_actionset(rule, actionset);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_val_rule(rule, ETH_P_ALL);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, keyset, rule->keyset);
	KUNIT_EXPECT_EQ(test, actionset, rule->actionset);
	KUNIT_EXPECT_EQ(test, size, ri->size);

	/* Add rule with write callback */
	ret = vcap_add_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, expected_addr, ri->addr);
	return rule;
}

#ifdef VCAP_DUMP
static void test_dump_cache(const char *name, struct vcap_cache_data *cache, int count)
{
	int idx;

	for (idx = 0; idx < count; ++idx) {
		pr_info("%s: keydata[%02d] = %#8x\n", name, idx, cache->keystream[idx]);
	}
	for (idx = 0; idx < count; ++idx) {
		pr_info("%s: mskdata[%02d] = %#8x\n", name, idx, cache->maskstream[idx]);
	}
	for (idx = 0; idx < count; ++idx) {
		pr_info("%s: actdata[%02d] = %#8x\n", name, idx, cache->actionstream[idx]);
	}
}
#endif

static void vcap_api_modify_key_values_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[12] = {};
	static u32 mskdata[12] = {};
	static u32 actdata[12] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 800,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;
	u32 orig_exp_key[] = { 0x3fce, 0x0, 0x0, 0x5, 0x0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,};
	u32 orig_exp_msk[] = { 0xffff8000, 0xffffffff, 0xffffffff, 0xfffffff8, 0xffffffff, 0xffffff3f, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,};
	u32 orig_exp_act[] = { 0xa2, 0x0, 0x0, 0x0, 0x0, 0xc4000000, 0x9, 0x0, 0x0, 0x6800, 0x0, 0x0,};
	u32 mod_exp_key[] = { 0x3fce, 0x0, 0x0, 0x5, 0x0, 0x40, 0x60800000, 0x2040, 0x0, 0x0, 0x0, 0x0, };
	u32 mod_exp_msk[] = { 0xffff8000, 0xffffffff, 0xffffffff, 0xfffffff8, 0xffffffff, 0xffffff3f, 0x1ffffe, 0xfff00000, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff, };
	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    ARP_OPCODE: u32 (1): 01/11\n",
		"    FIRST: bit: 1/1\n",
		"    L3_IP4_SIP: u32 (16909060): 0000.0001.0000.0010.0000.0011.0000.0100/1111.1111.1111.1111.1111.1111.1111.1111\n",
		"    PAG: u32 (127): 0111.1111/1111.1111\n",
		"    PCP: u32 (5): 101/111\n",
		"    TYPE: u32 (3): 0011/1111\n",
		"  actionfields:\n",
		"    LOG_MSG_INTERVAL: u32 (13): 1101\n",
		"    MATCH_ID: u32 (40000): 1001.1100.0100.0000\n",
		"    PIPELINE_PT: u32 (10): 0.1010\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is2_rule_creator(test, 10000, VCAP_USER_QOS, 20, 400, 6, 792);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Verify the VCAP data */
	for (idx = 0; idx < ARRAY_SIZE(orig_exp_key); ++idx) {
		KUNIT_EXPECT_EQ(test, orig_exp_key[idx], keydata[idx]);
		KUNIT_EXPECT_EQ(test, orig_exp_msk[idx], mskdata[idx]);
		KUNIT_EXPECT_EQ(test, orig_exp_act[idx], actdata[idx]);
	}

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 400);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Change the value of the key field */
	ret = vcap_rule_mod_key_u32(rule, VCAP_KF_L3_TOS, 0x25, 0xff);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	/* Try modifying a key field not in this keyset */
	ret = vcap_rule_mod_key_u32(rule, VCAP_KF_ARP_OPCODE, 0x1, 0x3);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Change the value of the key field not currently in the rule */
	ret = vcap_rule_mod_key_u32(rule, VCAP_KF_L3_IP4_SIP, 0x01020304, 0xffffffff);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_ARP, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	/* Verify the modified VCAP data */
	for (idx = 0; idx < ARRAY_SIZE(orig_exp_key); ++idx) {
		KUNIT_EXPECT_EQ(test, mod_exp_key[idx], keydata[idx]);
		KUNIT_EXPECT_EQ(test, mod_exp_msk[idx], mskdata[idx]);
		KUNIT_EXPECT_EQ(test, orig_exp_act[idx], actdata[idx]);
	}

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_action_values_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[12] = {};
	static u32 mskdata[12] = {};
	static u32 actdata[12] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 800,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;
	u32 orig_exp_key[] = { 0x3fce, 0x0, 0x0, 0x5, 0x0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,};
	u32 orig_exp_msk[] = { 0xffff8000, 0xffffffff, 0xffffffff, 0xfffffff8, 0xffffffff, 0xffffff3f, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,};
	u32 orig_exp_act[] = { 0xa2, 0x0, 0x0, 0x0, 0x0, 0xc4000000, 0x9, 0x0, 0x0, 0x6800, 0x0, 0x0,};
	u32 mod_exp_act[] = { 0xc0001e2, 0x0, 0x0, 0x0, 0x0, 0xc4000000, 0x9, 0x0, 0x0, 0x6800, 0x0, 0x0,};
	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    ARP_OPCODE: u32 (2): 10/11\n",
		"    FIRST: bit: 1/1\n",
		"    PAG: u32 (127): 0111.1111/1111.1111\n",
		"    PCP: u32 (5): 101/111\n",
		"    TYPE: u32 (3): 0011/1111\n",
		"  actionfields:\n",
		"    DLB_OFFSET: u32 (3): 011\n",
		"    LOG_MSG_INTERVAL: u32 (13): 1101\n",
		"    MATCH_ID: u32 (40000): 1001.1100.0100.0000\n",
		"    PIPELINE_PT: u32 (30): 1.1110\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is2_rule_creator(test, 10000, VCAP_USER_QOS, 20, 400, 6, 792);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Verify the VCAP data */
	for (idx = 0; idx < ARRAY_SIZE(orig_exp_key); ++idx) {
		KUNIT_EXPECT_EQ(test, orig_exp_key[idx], keydata[idx]);
		KUNIT_EXPECT_EQ(test, orig_exp_msk[idx], mskdata[idx]);
		KUNIT_EXPECT_EQ(test, orig_exp_act[idx], actdata[idx]);
	}

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 400);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Change the value of the action field */
	ret = vcap_rule_mod_action_u32(rule, VCAP_AF_PIPELINE_PT, 30);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Change the value of an action field not currently in the rule */
	ret = vcap_rule_mod_action_u32(rule, VCAP_AF_DLB_OFFSET, 3);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_ARP, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	/* test_dump_cache(__func__, &admin.cache, ARRAY_SIZE(keydata)); */

	/* Verify the modified VCAP data */
	for (idx = 0; idx < ARRAY_SIZE(orig_exp_key); ++idx) {
		KUNIT_EXPECT_EQ(test, orig_exp_key[idx], keydata[idx]);
		KUNIT_EXPECT_EQ(test, orig_exp_msk[idx], mskdata[idx]);
		KUNIT_EXPECT_EQ(test, mod_exp_act[idx], actdata[idx]);
	}

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_add_key_field_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[24] = {};
	static u32 mskdata[24] = {};
	static u32 actdata[24] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 800,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_u48_key dmac = {
		.value = { 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 },
		.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
	};

	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;

	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    FIRST: bit: 1/1\n",
		"    L2_DMAC: u48: 0000.0001.0000.0010.0000.0011.0000.0100.0000.0101.0000.0110/1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111\n",
		"    L4_DPORT: u32 (1024): 0000.0100.0000.0000/1111.1111.1111.1111\n",
		"    L4_URG: bit: 1/1\n",
		"    PAG: u32 (127): 0111.1111/1111.1111\n",
		"    PCP: u32 (5): 101/111\n",
		"    TYPE: u32 (1): 01/11\n",
		"    XVID: u32 (1209): 0.0100.1011.1001/0.1111.1111.1111\n",
		"  actionfields:\n",
		"    CPU_QU_NUM: u32 (3): 011\n",
		"    LOG_MSG_INTERVAL: u32 (13): 1101\n",
		"    MATCH_ID: u32 (40000): 1001.1100.0100.0000\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is2_rule_creator(test, 10000, VCAP_USER_QOS, 20, 500, 12, 780);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 500);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* add new fields */
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_XVID, 1209, 0xfff);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u48(rule, VCAP_KF_L2_DMAC, &dmac);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_L4_URG, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Try adding a key field that does not belong in the keyset */
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_ETYPE, 0x8181, 0xffff);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_IP_7TUPLE, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 12, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_add_action_field_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[24] = {};
	static u32 mskdata[24] = {};
	static u32 actdata[24] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 800,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_u48_action mac = {
		.value = { 0x12, 0x13, 0x22, 0x23, 0xae, 0xde },
	};

	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;

	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    FIRST: bit: 1/1\n",
		"    L4_DPORT: u32 (1024): 0000.0100.0000.0000/1111.1111.1111.1111\n",
		"    PAG: u32 (127): 0111.1111/1111.1111\n",
		"    PCP: u32 (5): 101/111\n",
		"    TYPE: u32 (1): 01/11\n",
		"  actionfields:\n",
		"    ACL_MAC: u48: 1101.1110.1010.1110.0010.0011.0010.0010.0001.0011.0001.0010\n",
		"    CPU_QU_NUM: u32 (3): 011\n",
		"    EGR_ACL_ENA: bit: 1\n",
		"    LOG_MSG_INTERVAL: u32 (13): 1101\n",
		"    MATCH_ID: u32 (40000): 1001.1100.0100.0000\n",
		"    POLICE_IDX: u32 (56): 11.1000\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is2_rule_creator(test, 10000, VCAP_USER_QOS, 20, 500, 12, 780);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 500);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* add new fields */
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_POLICE_IDX, 56);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u48(rule, VCAP_AF_ACL_MAC, &mac);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_bit(rule, VCAP_AF_EGR_ACL_ENA, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Try adding an action field that does not belong in the actionset */
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_NXT_IDX, 0x100);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_IP_7TUPLE, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 12, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_remove_key_field_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[24] = {};
	static u32 mskdata[24] = {};
	static u32 actdata[24] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 800,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};

	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;

	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    FIRST: bit: 1/1\n",
		"    L4_DPORT: u32 (1024): 0000.0100.0000.0000/1111.1111.1111.1111\n",
		"    PAG: u32 (127): 0111.1111/1111.1111\n",
		"    TYPE: u32 (1): 01/11\n",
		"  actionfields:\n",
		"    CPU_QU_NUM: u32 (3): 011\n",
		"    LOG_MSG_INTERVAL: u32 (13): 1101\n",
		"    MATCH_ID: u32 (40000): 1001.1100.0100.0000\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is2_rule_creator(test, 10000, VCAP_USER_QOS, 20, 500, 12, 780);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 500);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* remove non-existing field */
	ret = vcap_rule_rem_key(rule, VCAP_KF_XVID);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	/* remove existing field */
	ret = vcap_rule_rem_key(rule, VCAP_KF_PCP);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* allow the cache to be erased */
	test_cache_erase_count = sizeof(keydata);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_IP_7TUPLE, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 12, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_remove_action_field_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[24] = {};
	static u32 mskdata[24] = {};
	static u32 actdata[24] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 800,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};

	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;

	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    FIRST: bit: 1/1\n",
		"    L4_DPORT: u32 (1024): 0000.0100.0000.0000/1111.1111.1111.1111\n",
		"    PAG: u32 (127): 0111.1111/1111.1111\n",
		"    PCP: u32 (5): 101/111\n",
		"    TYPE: u32 (1): 01/11\n",
		"  actionfields:\n",
		"    CPU_QU_NUM: u32 (3): 011\n",
		"    LOG_MSG_INTERVAL: u32 (13): 1101\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is2_rule_creator(test, 10000, VCAP_USER_QOS, 20, 500, 12, 780);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 500);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* remove non-existing action */
	ret = vcap_rule_rem_action(rule, VCAP_AF_TCP_UDP_SPORT);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	/* remove existing action */
	ret = vcap_rule_rem_action(rule, VCAP_AF_MATCH_ID);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* allow the cache to be erased */
	test_cache_erase_count = sizeof(keydata);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_IP_7TUPLE, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 12, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_change_keyset_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[24] = {};
	static u32 mskdata[24] = {};
	static u32 actdata[24] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 800,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};

	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;

	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    ARP_OPCODE: u32 (2): 10/11\n",
		"    FIRST: bit: 1/1\n",
		"    PAG: u32 (127): 0111.1111/1111.1111\n",
		"    PCP: u32 (5): 101/111\n",
		"    TYPE: u32 (3): 0011/1111\n",
		"  actionfields:\n",
		"    LOG_MSG_INTERVAL: u32 (13): 1101\n",
		"    MATCH_ID: u32 (40000): 1001.1100.0100.0000\n",
		"    PIPELINE_PT: u32 (10): 0.1010\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is2_rule_creator(test, 10000, VCAP_USER_QOS, 20, 500, 6, 792);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 500);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	ret = vcap_set_rule_set_keyset(rule, VCAP_KFS_IP4_OTHER);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* allow the cache to be erased */
	test_cache_erase_count = sizeof(keydata);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_IP4_OTHER, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	/* Set the keyset back to its expected value */
	ret = vcap_set_rule_set_keyset(rule, VCAP_KFS_ARP);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* allow the cache to be erased */
	test_cache_erase_count = sizeof(keydata);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_ARP, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_change_actionset_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[64] = {};
	static u32 mskdata[64] = {};
	static u32 actdata[64] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 20000,
		.last_cid = 29999,
		.lookups = 4,
		.last_valid_addr = 2000,
		.first_valid_addr = 0,
		.last_used_addr = 1500,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};

	struct vcap_rule *rule;
	struct vcap_rule_internal *ri;
	int idx, ret;

	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    FIRST: bit: 1/1\n",
		"    G_IDX: u32 (3127): 1100.0011.0111/1111.1111.1111\n",
		"    PCP0: u32 (3): 011/011\n",
		"    TPID0: u32 (5): 101/111\n",
		"    TYPE: u32 (2): 10/11\n",
		"  actionfields:\n",
		"    CPU_Q: u32 (2): 010\n",
		"    PAG_VAL: u32 (13): 0000.1101\n",
		"    PIPELINE_PT: u32 (10): 0.1010\n",
		"    TYPE: bit: 1\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	rule = test_is0_rule_creator(test, 20000, VCAP_USER_QOS, 20, 200, 6, 1494);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 200);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	ret = vcap_set_rule_set_actionset(rule, VCAP_AFS_CLASS_REDUCED);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* allow the cache to be erased */
	test_cache_erase_count = sizeof(keydata);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_NORMAL_5TUPLE_IP4, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_CLASS_REDUCED, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	/* Set the keyset back to its expected value */
	ret = vcap_set_rule_set_actionset(rule, VCAP_AFS_CLASSIFICATION);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* allow the cache to be erased */
	test_cache_erase_count = sizeof(keydata);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_NORMAL_5TUPLE_IP4, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_CLASSIFICATION, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_all_keysizes_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[64] = {};
	static u32 mskdata[64] = {};
	static u32 actdata[64] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 20000,
		.last_cid = 29999,
		.lookups = 4,
		.last_valid_addr = 2000,
		.first_valid_addr = 0,
		.last_used_addr = 1500,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule *rule = 0;
	struct vcap_rule_internal *ri = 0;
	struct vcap_u48_key dmac = {
		.value = {0xe1, 0xf2, 0x33, 0x44, 0xa5, 0xb6},
		.mask = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
	};
	struct vcap_u72_key ports = {
		.value = {9, 8, 7, 6, 5, },
		.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, },
	};
	struct vcap_u128_key ip6dip = {
		.value = {0xa1, 0xa2, 0xa3, 0xa4, 0, 0, 0x43, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x8e },
		.mask =  {0xff, 0xff, 0xff, 0xff, 0, 0, 0xff, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff },
	};
	struct vcap_u48_key dmac2 = {
		.value = {0xf6, 0x12, 0xc5, 0x81, 0x3f, 0xff},
		.mask = {0xf0, 0xff, 0x0f, 0xff, 0x0f, 0xff},
	};
	struct vcap_u72_key ports2 = {
		.value = {0xa, 0xc, 0xff, },
		.mask = { 0xff, 0xff, 0xff, },
	};
	struct vcap_u128_key ip6dip2 = {
		.value = {0x02, 0x34, 0xf4, 0xb7, 0x76, 0x65, 0, 0, 0, 0, 0, 0, 0, 0, 0xed, 0x56 },
		.mask  = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff },
	};
	int idx, ret;
	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    FIRST: bit: 1/1\n",
		"    G_IDX: u32 (2489): 1001.1011.1001/1111.1111.1111\n",
		"    IGR_PORT_MASK: u72: 0.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.1111.1111.0000.1100.0000.1010/0.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.1111.1111.1111.1111.1111.1111\n",
		"    L2_BC: bit: 1/1\n",
		"    L2_DMAC: u48: 1111.1111.0011.1111.1000.0001.1100.0101.0001.0010.1111.0110/1111.1111.0000.1111.1111.1111.0000.1111.1111.1111.1111.0000\n",
		"    L3_IP6_DIP: u128: 0101.0110.1110.1101.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0110.0101.0111.0110.1011.0111.1111.0100.0011.0100.0000.0010/1111.1111.1111.1111.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.0000.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111.1111\n",
		"    TYPE: bit: 0/1\n",
		"  actionfields:\n",
		"    PAG_VAL: u32 (15): 0000.1111\n",
		"    TYPE: bit: 1\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	/* init before testing */
	memset(test_updateaddr, 0, sizeof(test_updateaddr));
	test_updateaddridx = 0;
	test_move_addr = 0;
	test_move_offset = 0;
	test_move_count = 0;

	/* Allocate the rule */
	rule = vcap_alloc_rule(&netdev, admin.first_cid, VCAP_USER_PTP, 10, 100);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Add all field type sizes for this vcap */
	ret = vcap_rule_add_key_bit(rule, VCAP_KF_L2_BC, VCAP_BIT_0);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_G_IDX, 3127, 0xfff);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u48(rule, VCAP_KF_L2_DMAC, &dmac);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u72(rule, VCAP_KF_IGR_PORT_MASK, &ports);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_key_u128(rule, VCAP_KF_L3_IP6_DIP, &ip6dip);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add actions */
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_PAG_VAL, 15);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_val_rule(rule, ETH_P_ALL);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_NORMAL_7TUPLE, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_CLASSIFICATION, rule->actionset);
	KUNIT_EXPECT_EQ(test, 12, ri->size);

	/* Add rule with write callback */
	ret = vcap_add_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 1488, ri->addr);

	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 100);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Modify all field type sizes for this vcap */
	ret = vcap_rule_mod_key_bit(rule, VCAP_KF_L2_BC, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_mod_key_u32(rule, VCAP_KF_G_IDX, 2489, 0xfff);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_mod_key_u48(rule, VCAP_KF_L2_DMAC, &dmac2);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_mod_key_u72(rule, VCAP_KF_IGR_PORT_MASK, &ports2);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_mod_key_u128(rule, VCAP_KF_L3_IP6_DIP, &ip6dip2);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_NORMAL_7TUPLE, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_CLASSIFICATION, rule->actionset);
	KUNIT_EXPECT_EQ(test, 12, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_modify_all_actionsizes_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[64] = {};
	static u32 mskdata[64] = {};
	static u32 actdata[64] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS0,
		.first_cid = 20000,
		.last_cid = 29999,
		.lookups = 4,
		.last_valid_addr = 2000,
		.first_valid_addr = 0,
		.last_used_addr = 1500,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule *rule = 0;
	struct vcap_rule_internal *ri = 0;
	struct vcap_u72_action ports = {
		.value = {9, 8, 7, 6, 5, },
	};
	struct vcap_u72_action ports2 = {
		.value = {0},
	};
	int idx, ret;
	int show_offset = 31;
	const char *show_text[] = {
		"  keyfields:\n",
		"    FIRST: bit: 1/1\n",
		"    G_IDX: u32 (3127): 1100.0011.0111/1111.1111.1111\n",
		"    TYPE: u32 (2): 10/11\n",
		"  actionfields:\n",
		"    NXT_IDX: u32 (2000): 0111.1101.0000\n",
		"    PAG_VAL: u32 (20): 0001.0100\n",
		"  counter: 0\n",
		"  counter_sticky: 0\n",
	};

	vcap_test_api_init(&admin);

	/* init before testing */
	memset(test_updateaddr, 0, sizeof(test_updateaddr));
	test_updateaddridx = 0;
	test_move_addr = 0;
	test_move_offset = 0;
	test_move_count = 0;

	/* Allocate the rule */
	rule = vcap_alloc_rule(&netdev, admin.first_cid, VCAP_USER_PTP, 10, 200);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Add key */
	ret = vcap_rule_add_key_u32(rule, VCAP_KF_G_IDX, 3127, 0xfff);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add different field type sizes for this vcap */
	/* Actions must be non-zero or they will not exist! */
	ret = vcap_rule_add_action_bit(rule, VCAP_AF_NXT_NORMALIZE, VCAP_BIT_1);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_NXT_IDX, 3000); KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u32(rule, VCAP_AF_PAG_VAL, 15);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_add_action_u72(rule, VCAP_AF_PORT_MASK, &ports);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_val_rule(rule, ETH_P_ALL);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_NORMAL_5TUPLE_IP4, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_FULL, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	/* Add rule with write callback */
	ret = vcap_add_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 1494, ri->addr);

	vcap_free_rule(rule);

	/* Get the rule again */
	rule = vcap_get_rule(&netdev, 200);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ri = (struct vcap_rule_internal *)rule;

	/* Modify all field type sizes for this vcap */
	/* Setting an action to zero effectively removes it! */
	ret = vcap_rule_mod_action_bit(rule, VCAP_AF_NXT_NORMALIZE, VCAP_BIT_0);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_mod_action_u32(rule, VCAP_AF_NXT_IDX, 2000);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_mod_action_u32(rule, VCAP_AF_PAG_VAL, 20);
	KUNIT_EXPECT_EQ(test, 0, ret);
	ret = vcap_rule_mod_action_u72(rule, VCAP_AF_PORT_MASK, &ports2);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = vcap_mod_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_NORMAL_5TUPLE_IP4, rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_FULL, rule->actionset);
	KUNIT_EXPECT_EQ(test, 6, ri->size);

	test_pr_bufferidx = 0;
	ret = vcap_show_admin(test_pf, 0, &admin);
	KUNIT_EXPECT_EQ(test, 0, ret);
	for (idx = show_offset; idx < test_pr_bufferidx && ((idx - show_offset) < ARRAY_SIZE(show_text)); ++idx) {
		/* pr_info("log[%02d]: %s", idx, test_pr_buffer[idx]); */
		KUNIT_EXPECT_STREQ(test, show_text[idx - show_offset], test_pr_buffer[idx]);
	}
}

static void vcap_api_copy_to_w32be_test(struct kunit *test)
{
	int idx;
	u8 inbuf1[9] = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8};
	u8 exp_outbuf1[9] = { 0xd8, 0xd4, 0xd5, 0xd6, 0xd7, 0xd0, 0xd1, 0xd2, 0xd3};
	u8 outbuf1[9];
	u8 inbuf2[16] = {0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef};
	u8 exp_outbuf2[16] = {0xec, 0xed, 0xee, 0xef, 0xe8, 0xe9, 0xea, 0xeb, 0xe4, 0xe5, 0xe6, 0xe7, 0xe0, 0xe1, 0xe2, 0xe3};
	u8 outbuf2[16];
	u8 inbuf3[6] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
	u8 exp_outbuf3[6] = {0xa4, 0xa5, 0xa0, 0xa1, 0xa2, 0xa3};
	u8 outbuf3[6];

	vcap_copy_to_w32be(outbuf1, inbuf1, sizeof(outbuf1));
	for (idx = 0; idx < sizeof(outbuf1); ++idx) {
		KUNIT_EXPECT_EQ(test, exp_outbuf1[idx], outbuf1[idx]);
	}
	vcap_copy_to_w32be(outbuf2, inbuf2, sizeof(outbuf2));
	for (idx = 0; idx < sizeof(outbuf2); ++idx) {
		KUNIT_EXPECT_EQ(test, exp_outbuf2[idx], outbuf2[idx]);
	}
	vcap_copy_to_w32be(outbuf3, inbuf3, sizeof(outbuf3));
	for (idx = 0; idx < sizeof(outbuf3); ++idx) {
		KUNIT_EXPECT_EQ(test, exp_outbuf3[idx], outbuf3[idx]);
	}
}

static void vcap_api_copy_from_w32be_test(struct kunit *test)
{
	int idx;
	u8 exp_outbuf1[9] = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8};
	u8 inbuf1[9] = { 0xd8, 0xd4, 0xd5, 0xd6, 0xd7, 0xd0, 0xd1, 0xd2, 0xd3};
	u8 outbuf1[9];
	u8 exp_outbuf2[16] = {0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef};
	u8 inbuf2[16] = {0xec, 0xed, 0xee, 0xef, 0xe8, 0xe9, 0xea, 0xeb, 0xe4, 0xe5, 0xe6, 0xe7, 0xe0, 0xe1, 0xe2, 0xe3};
	u8 outbuf2[16];
	u8 exp_outbuf3[6] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
	u8 inbuf3[6] = {0xa4, 0xa5, 0xa0, 0xa1, 0xa2, 0xa3};
	u8 outbuf3[6];

	vcap_copy_from_w32be(outbuf1, inbuf1, sizeof(outbuf1), sizeof(outbuf1) * 8);
	for (idx = 0; idx < sizeof(outbuf1); ++idx) {
		KUNIT_EXPECT_EQ(test, exp_outbuf1[idx], outbuf1[idx]);
	}
	vcap_copy_from_w32be(outbuf2, inbuf2, sizeof(outbuf2), sizeof(outbuf2) * 8);
	for (idx = 0; idx < sizeof(outbuf2); ++idx) {
		KUNIT_EXPECT_EQ(test, exp_outbuf2[idx], outbuf2[idx]);
	}
	vcap_copy_from_w32be(outbuf3, inbuf3, sizeof(outbuf3), sizeof(outbuf3) * 8);
	for (idx = 0; idx < sizeof(outbuf3); ++idx) {
		KUNIT_EXPECT_EQ(test, exp_outbuf3[idx], outbuf3[idx]);
	}
}

static void vcap_api_w32be_encode_rule_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[32] = {};
	static u32 mskdata[32] = {};
	static u32 actdata[32] = {};

	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 800,
		.w32be = true,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule *rule = 0;
	struct vcap_rule_internal *ri = 0;
	int vcap_chain_id = 10005;
	enum vcap_user user = VCAP_USER_VCAP_UTIL;
	u16 priority = 10;
	int id = 100;
	int ret;
	struct vcap_u128_key dip = {
		.value = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
		.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
	};
	/* VCAP_KFS_IP_7TUPLE, sw_width 52
	 * tg: offset 0, width 3, value 4
	 * tg: offset 156, width 1, value 0
	 * tg: offset 312, width 2, value 0
	 * VCAP_KF_L3_IP6_DIP: offset 227, width 128
	 * We cannot use fields with a width not a modulo of 8!
	 */
	u32 exp_keydata[] = {
		0x2c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
		0x86000000,     /* >> 3: 010c.. *1/ */
		0x78706,        /* >> 3: f0e0 .. d *1/ */
		0x58504840,     /* >> 3: b0a0908 *1/ */
		0x2820,         /* >> 3: 504 *1/ */
		0x2000e0c,      /* >> 1: 1000706 (tg bit at 312) *1/ */
		0x604,          /* >> 1: 0302 *1/ */
		0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	};
	u32 exp_mskdata[] = {
		0xffffffc0, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
		0xffffffff, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff, 0x7,
		0xfff00000, 0x0, 0xfffe0000, 0xffffffff, 0xffffffff, 0xffffffff,
		0xffffffff, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff,
		0xffffffff, 0xffffffff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	};
	struct vcap_u48_action mac = {
		.value = {0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5},
	};
	/* VCAP_AFS_BASE_TYPE, act_width 110
	 * tg: offset 0, width 2, value 2
	 * tg: offset 110, width 1, value 0
	 * tg: offset 220, width 1, value 0
	 * VCAP_AF_ACL_MAC: offset 208, width 48
	 */
	u32 exp_actdata[] = {
		0x2, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
		0x3c80,
		0xe2e1e0e4,  /* middle bytes */
		0xe3,
		0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
		0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	};
	int idx;

	vcap_test_api_init(&admin);

	/* Allocate the rule */
	rule = vcap_alloc_rule(&netdev, vcap_chain_id, user, priority, id);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);
	ri = (struct vcap_rule_internal *)rule;

	vcap_set_rule_set_keyset(rule, VCAP_KFS_IP_7TUPLE);
	vcap_set_rule_set_actionset(rule, VCAP_AFS_BASE_TYPE);

	/* Add rule keys */
	ret = vcap_rule_add_key_u128(rule, VCAP_KF_L3_IP6_DIP, &dip);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Add rule actions */
	ret = vcap_rule_add_action_u48(rule, VCAP_AF_ACL_MAC, &mac);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Validation with validate keyset callback */
	ret = vcap_val_rule(rule, ETH_P_ALL);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, VCAP_KFS_IP_7TUPLE , rule->keyset);
	KUNIT_EXPECT_EQ(test, VCAP_AFS_BASE_TYPE, rule->actionset);
	KUNIT_EXPECT_EQ(test, 12, ri->size);
	KUNIT_EXPECT_EQ(test, 2, ri->keyset_sw_regs);
	KUNIT_EXPECT_EQ(test, 4, ri->actionset_sw_regs);

	/* Add rule with write callback */
	ret = vcap_add_rule(rule);
	KUNIT_EXPECT_EQ(test, 0, ret);
	vcap_free_rule(rule);

	/* test_dump_cache(__func__, &admin.cache, ARRAY_SIZE(keydata)); */
	for (idx = 0; idx < ARRAY_SIZE(exp_keydata); ++idx) {
		KUNIT_EXPECT_EQ(test, (u32)exp_keydata[idx], keydata[idx]);
		KUNIT_EXPECT_EQ(test, (u32)exp_mskdata[idx], mskdata[idx]);
		KUNIT_EXPECT_EQ(test, exp_actdata[idx], actdata[idx]);
	}
}

static void vcap_api_w32be_decode_rule_test(struct kunit *test)
{
	/* Data used by VCAP Library callback */
	static u32 keydata[] = {
		0x2c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
		0x1fd00000, 0x0, 0xfbfdfe00, 0xdfa7d, 0x3fb000, 0x0, 0x0, 0x0, 0x0, 0x0,
		0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0
	};
	static u32 mskdata[] = {
		0xffffffc0, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
		0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
		0xfffffffc, 0xffffffff, 0x1fffffff, 0xfff00000, 0x0, 0xfff00000, 0xffc00000,
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x0, 0x0, 0x0, 0x0,
		0x0, 0x0, 0x0, 0x0
	};
	static u32 actdata[] = {
		0x2, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3480, 0xa2a1a0a4, 0xa3, 0x0, 0x0,
		0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
		0x0, 0x0, 0x0, 0x0, 0x0,
	};
	struct vcap_admin admin = {
		.vtype = VCAP_TYPE_IS2,
		.first_cid = 10000,
		.last_cid = 19999,
		.lookups = 4,
		.last_valid_addr = 3071,
		.first_valid_addr = 0,
		.last_used_addr = 794,
		.w32be = true,
		.cache = {
			.keystream = keydata,
			.maskstream = mskdata,
			.actionstream = actdata,
		},
	};
	struct vcap_rule_internal admin_rule = {
		.admin = &admin,
		.data = {
			.id = 100,
			.keyset = VCAP_KFS_IP_7TUPLE,
			.actionset = VCAP_AFS_BASE_TYPE,
		},
		.size = 12,
		.keyset_sw_regs = 2,
		.actionset_sw_regs = 4,
		.addr = 794,
	};
	struct vcap_rule *rule = 0;
	int ret;
	struct vcap_client_keyfield *ckf;
	struct vcap_client_actionfield *caf;
	const struct vcap_field *keyfields;
	const struct vcap_field *actfields;
	struct vcap_u128_key sip = {
		 /* fe80::3efd:feff:fec0:6fd */
		.value = { 0xfd, 0x06, 0xc0, 0xfe, 0xff, 0xfe, 0xfd, 0x3e, 0, 0, 0, 0, 0, 0, 0x80, 0xfe },
		.mask = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
	};
	struct vcap_u48_action mac = {
		.value = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5},
	};
	int idx, field_size;

	vcap_test_api_init(&admin);
	list_add_tail(&admin_rule.list, &admin.rules);

	rule = vcap_get_rule(&netdev, 100);
	KUNIT_EXPECT_PTR_NE(test, NULL, rule);

	ckf = list_first_entry(&rule->keyfields, struct vcap_client_keyfield, ctrl.list);
	KUNIT_EXPECT_PTR_NE(test, NULL, ckf);
	keyfields = test_vcaps[admin.vtype].keyfield_set_map[rule->keyset];
	idx = 0;
	list_for_each_entry(ckf, &rule->keyfields, ctrl.list) {
		pr_debug("%s:%d: key: %d, type: %d: %s\n",
			__func__, __LINE__,
			ckf->ctrl.key,
			ckf->ctrl.type,
			test_vctrl.stats->keyfield_names[ckf->ctrl.key]);
		if (ckf->ctrl.key == VCAP_KF_L3_IP6_SIP) {
			field_size = keyfield_size_table[ckf->ctrl.type];
			ret = memcmp(&sip, &ckf->data, field_size);
			if (ret) {
				print_hex_dump(KERN_INFO, "exp: ", DUMP_PREFIX_OFFSET, 16, 1, &sip, field_size, true);
				print_hex_dump(KERN_INFO, "act: ", DUMP_PREFIX_OFFSET, 16, 1, &ckf->data, field_size, true);
			}
			KUNIT_EXPECT_EQ(test, 0, ret);
		}
		idx++;
	}
	caf = list_first_entry(&rule->actionfields, struct vcap_client_actionfield, ctrl.list);
	KUNIT_EXPECT_PTR_NE(test, NULL, caf);
	actfields = test_vcaps[admin.vtype].actionfield_set_map[rule->actionset];
	idx = 0;
	list_for_each_entry(caf, &rule->actionfields, ctrl.list) {
		pr_debug("%s:%d: action: %d, type: %d: %s\n",
			__func__, __LINE__,
			caf->ctrl.action,
			caf->ctrl.type,
			test_vctrl.stats->actionfield_names[caf->ctrl.action]);
		if (caf->ctrl.action == VCAP_AF_ACL_MAC) {
			field_size = actionfield_size_table[caf->ctrl.type];
			ret = memcmp(&mac, &caf->data, field_size);
			if (ret) {
				print_hex_dump(KERN_INFO, "exp: ", DUMP_PREFIX_OFFSET, 16, 1, &mac, field_size, true);
				print_hex_dump(KERN_INFO, "act: ", DUMP_PREFIX_OFFSET, 16, 1, &ckf->data, field_size, true);
			}
			KUNIT_EXPECT_EQ(test, 0, ret);
		}
		idx++;
	}
	vcap_free_rule(rule);
}

static struct kunit_case vcap_api_encoding_test_cases[] = {
	KUNIT_CASE(vcap_api_set_bit_1_test),
	KUNIT_CASE(vcap_api_set_bit_0_test),
	KUNIT_CASE(vcap_api_iterator_init_test),
	KUNIT_CASE(vcap_api_iterator_next_test),
	KUNIT_CASE(vcap_api_encode_typegroups_test),
	KUNIT_CASE(vcap_api_encode_bit_test),
	KUNIT_CASE(vcap_api_encode_field_test),
	KUNIT_CASE(vcap_api_encode_short_field_test),
	KUNIT_CASE(vcap_api_encode_keyfield_test),
	KUNIT_CASE(vcap_api_encode_max_keyfield_test),
	KUNIT_CASE(vcap_api_encode_actionfield_test),
	KUNIT_CASE(vcap_api_keyfield_typegroup_test),
	KUNIT_CASE(vcap_api_actionfield_typegroup_test),
	KUNIT_CASE(vcap_api_vcap_keyfields_test),
	KUNIT_CASE(vcap_api_vcap_actionfields_test),
	KUNIT_CASE(vcap_api_encode_rule_keyset_test),
	KUNIT_CASE(vcap_api_encode_rule_actionset_test),
	{}
};

static struct kunit_case vcap_api_decoding_test_cases[] = {
	KUNIT_CASE(vcap_api_get_bit_test),
	KUNIT_CASE(vcap_api_decode_field_test),
	KUNIT_CASE(vcap_api_decode_long_field_test),
	KUNIT_CASE(vcap_api_decode_short_field_test),
	KUNIT_CASE(vcap_api_decode_keyfield_typegroup_test),
	KUNIT_CASE(vcap_api_decode_short_keyfield_typegroup_test),
	KUNIT_CASE(vcap_api_decode_keystream_test),
	KUNIT_CASE(vcap_api_decode_actionstream_test),
	KUNIT_CASE(vcap_api_decode_bitarray_test),
	KUNIT_CASE(vcap_api_alloc_rule_keyfield_test),
	KUNIT_CASE(vcap_api_decode_rule_keyset_test),
	KUNIT_CASE(vcap_api_alloc_rule_actionfield_test),
	KUNIT_CASE(vcap_api_decode_rule_actionset_test),
	{}
};

static struct kunit_case vcap_api_rule_value_test_cases[] = {
	KUNIT_CASE(vcap_api_rule_add_keyvalue_test),
	KUNIT_CASE(vcap_api_rule_add_actionvalue_test),
	{}
};

static struct kunit_case vcap_api_full_rule_test_cases[] = {
	KUNIT_CASE(vcap_api_rule_find_keyset_test),
	KUNIT_CASE(vcap_api_rule_find_actionset_test),
	KUNIT_CASE(vcap_api_encode_rule_test),
	KUNIT_CASE(vcap_api_decode_rule_test),
	{}
};

static struct kunit_case vcap_api_debugfs_test_cases[] = {
	KUNIT_CASE(vcap_api_addr_keyset_test),
	KUNIT_CASE(vcap_api_show_admin_test),
	{}
};

static struct kunit_case vcap_api_rule_counter_test_cases[] = {
	KUNIT_CASE(vcap_api_set_rule_counter_test),
	KUNIT_CASE(vcap_api_get_rule_counter_test),
	{}
};

static struct kunit_case vcap_api_rule_insert_test_cases[] = {
	KUNIT_CASE(vcap_api_rule_insert_in_order_test),
	KUNIT_CASE(vcap_api_rule_insert_reverse_order_test),
	{}
};

static struct kunit_case vcap_api_rule_remove_test_cases[] = {
	KUNIT_CASE(vcap_api_rule_remove_at_end_test),
	KUNIT_CASE(vcap_api_rule_remove_in_middle_test),
	KUNIT_CASE(vcap_api_rule_remove_in_front_test),
	{}
};

static struct kunit_case vcap_api_modify_rule_test_cases[] = {
	KUNIT_CASE(vcap_api_modify_key_values_test),
	KUNIT_CASE(vcap_api_modify_action_values_test),
	KUNIT_CASE(vcap_api_modify_add_key_field_test),
	KUNIT_CASE(vcap_api_modify_add_action_field_test),
	KUNIT_CASE(vcap_api_modify_remove_key_field_test),
	KUNIT_CASE(vcap_api_modify_remove_action_field_test),
	KUNIT_CASE(vcap_api_modify_change_keyset_test),
	KUNIT_CASE(vcap_api_modify_change_actionset_test),
	KUNIT_CASE(vcap_api_modify_all_keysizes_test),
	KUNIT_CASE(vcap_api_modify_all_actionsizes_test),
	{}
};

static struct kunit_case vcap_api_w32be_rule_test_cases[] = {
	KUNIT_CASE(vcap_api_copy_to_w32be_test),
	KUNIT_CASE(vcap_api_copy_from_w32be_test),
	KUNIT_CASE(vcap_api_w32be_encode_rule_test),
	KUNIT_CASE(vcap_api_w32be_decode_rule_test),
	{}
};

static struct kunit_suite vcap_api_encoding_test_suite = {
        .name = "VCAP_API_Encoding_Testsuite",
        .test_cases = vcap_api_encoding_test_cases,
};
static struct kunit_suite vcap_api_decoding_test_suite = {
        .name = "VCAP_API_Decoding_Testsuite",
        .test_cases = vcap_api_decoding_test_cases,
};
static struct kunit_suite vcap_api_rule_value_test_suite = {
        .name = "VCAP_API_Rule_Value_Testsuite",
        .test_cases = vcap_api_rule_value_test_cases,
};
static struct kunit_suite vcap_api_full_rule_test_suite = {
        .name = "VCAP_API_Full_Rule_Testsuite",
        .test_cases = vcap_api_full_rule_test_cases,
};
static struct kunit_suite vcap_api_debugfs_test_suite = {
        .name = "VCAP_API_DebugFS_Testsuite",
        .test_cases = vcap_api_debugfs_test_cases,
};
static struct kunit_suite vcap_api_rule_counter_test_suite = {
        .name = "VCAP_API_Rule_Counter_Testsuite",
        .test_cases = vcap_api_rule_counter_test_cases,
};
static struct kunit_suite vcap_api_rule_insert_test_suite = {
        .name = "VCAP_API_Rule_Insert_Testsuite",
        .test_cases = vcap_api_rule_insert_test_cases,
};
static struct kunit_suite vcap_api_rule_remove_test_suite = {
        .name = "VCAP_API_Rule_Remove_Testsuite",
        .test_cases = vcap_api_rule_remove_test_cases,
};
static struct kunit_suite vcap_api_modify_rule_test_suite = {
        .name = "VCAP_API_Modify_Rule_Testsuite",
        .test_cases = vcap_api_modify_rule_test_cases,
};
static struct kunit_suite vcap_api_w32be_rule_test_suite = {
        .name = "VCAP_API_W32BE_Rule_Testsuite",
        .test_cases = vcap_api_w32be_rule_test_cases,
};

kunit_test_suite(vcap_api_w32be_rule_test_suite);
kunit_test_suite(vcap_api_modify_rule_test_suite);
kunit_test_suite(vcap_api_rule_remove_test_suite);
kunit_test_suite(vcap_api_rule_insert_test_suite);
kunit_test_suite(vcap_api_rule_counter_test_suite);
kunit_test_suite(vcap_api_debugfs_test_suite);
kunit_test_suite(vcap_api_full_rule_test_suite);
kunit_test_suite(vcap_api_rule_value_test_suite);
kunit_test_suite(vcap_api_decoding_test_suite);
kunit_test_suite(vcap_api_encoding_test_suite);
