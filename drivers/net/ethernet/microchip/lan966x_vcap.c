/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/iopoll.h>
#include "lan966x_main.h"


/*******************************************************************************
 * Functional overview
 ******************************************************************************/
/*
  A VCAP consists of a number of addressable entries called subwords.

  A subword consists of a key (subdivided in entry and mask), an action and a
  counter.
  All these fields have different bit-widths depending on the actual TCAM.
  For example is the counter in ES0 only 1 bit wide while the action in IS2 is
  120 bits wide.

  The subwords are manipulated via VCAP commands (see LAN966X_VCAP_CMD_XX below).
  Only a single subword can be read or written at a time whereas multiple
  subwords can be moved around or cleared (initialized) at the same time.

  Some entries are too large to be contained in one subword and are therefore
  distributed between several subwords. We name entries as X1, X2 or X4, where
  the number denotes the number of subwords.

  ES0 has only X1 entries while IS1 and IS2 has all three.

  An X1 entry with one subword is stored in the VCAP like this:
  +---------+
  |   adr   |
  +---------+
  ^ MSB     ^ LSB

  An X2 entry with two subwords is stored in the VCAP like this:
  +---------+---------+
  | adr + 1 |   adr   |
  +---------+---------+
  ^ MSB               ^ LSB

  An X4 entry four subwords is stored in the VCAP like this:
  +---------+---------+---------+---------+
  | adr + 3 | adr + 2 | adr + 1 |   adr   |
  +---------+---------+---------+---------+
  ^ MSB                                   ^ LSB

  The least significant bit (LSB) in the entry is located as shown above.

  The short version of how it works is like this:
  1) The TCAM is presented with some data or sideband information from a frame.
  2) A search for a matching key is initated, starting with the highest address.
  3) If there is a match, the action is executed, the counter is incremented and
     the search stops.

  As the search always starts with the highest address and stops if there is a
  match, one must be careful about how the entries are located.

  If a very general key is located at a higher address than a more specific key,
  the latter will never be hit.

  If the TCAM has other than X1 entries, the TCAM in subdevided in three size
  areas, where each area is allowed to be empty.

  If there are any X4 entries, they are all located at the highest addresses.

  If there are any X2 entries, they are all located at the highest addresses that
  are not already occupied by eventual X4 entries.

  If there are any X1 entries, they are all located at the highest addresses that
  are not already occupied by eventual X2 and X4 entries.

  Each of these size areas are subdivided into user areas, where users with
  lowest enum value is located at the bottom (highest priority) of the size area.
  See definition of users in LAN966x_VCAP_USER_XX in the header file.

  Each user specifies a priority for an entry that is added and entries with
  lowest priority value is located at the bottom (highest priority) within each
  user area.

  The content of a TCAM with 64 subwords can be depicted with rows and columns:

         +---------------------------------------+
  row  0 |                                       |  3..0
         +---------------------------------------+
  row  1 |                                       |  7..4
         +---------------------------------------+
  row  2 |                                       | 11..8
         +---------------------------------------+
  row  3 |                                       | 15..12
         +---------------------------------------+
  row  4 |                                       | 19..16
         +---------------------------------------+
  row  5 |                                       | 23..20
         +---------------------------------------+
  row  6 |                                       | 27..24
         +---------------------------------------+
  row  7 |                                       | 31..28
         +---------------------------------------+
  row  8 |                                       | 35..32
         +---------------------------------------+
  row  9 |                                       | 39..36
         +---------------------------------------+
  row 10 |                                       | 43..40
         +---------+-----------------------------+
  row 11 | (8) X1  |                             | 47..44
         +---------+---------+---------+---------+
  row 12 | (5)    X2         | (6) X1  | (7) X1  | 51..48
         +-------------------+---------+---------+
  row 13 | (3)    X2         | (4)    X2         | 55..52
         +-------------------+-------------------+
  row 14 | (2)              X4                   | 59..56
         +---------------------------------------+
  row 15 | (1)              X4                   | 63..60
         +---------------------------------------+
            col 3     col 2     col 1     col 0      ^ addresses in TCAM

  The numbers in parentheses indicate the search order for matches.

  X4 entries must be located on row boundaries and X2 entries on row/2 boundaries
  as shown above. X1 entries can be located anywhere.

  By placing the largest entries at the bottom it is possible to avoid unused
  holes in the TCAM.
  All entries are allowed to be moved four addresses up or down.
  X2 and X1 entries are allowed to be moved two addresses up or down.
  X1 entries are allowed to be moved one address up or down.

  If the sequence was the opposite, with X1 at the bottom, it would not be
  possible to insert a new X1 entry due to the boundary limitations in X2 and X4
  entries.

  Each TCAM is represented by a struct lan966x_vcap_admin, which consist of:
  *) list_head.
  *) mutex for list protection.
  *) last_valid_addr, which is 63 in the example above and never changes.
  *) last_used_addr, which is 47 in the example above.
     last_used_addr is initially set to (last_valid_addr - 1) which indicates
     that there are no entries yet.
     When adding a new entry, last_used_addr is decremented by the size of the
     new entry.
     When deleting an existing entry, last_used_addr is incremented by the size
     of the deleted entry.

  Entries are stored in a linked list which is sorted in the same way as the
  entries are located in the TCAM (see struct lan966x_vcap_rule_entry):

  +-->list_head<-------------------------------------------------------------+
  |                                                                          |
  +-->(1)X4<-->(2)X4<-->(3)X2<-->(4)X2<-->(5)X2<-->(6)X1<-->(7)X1<-->(8)X1<--+

  The sort_key is defined like this:
  #define SORT_KEY_INIT(max_size, size, user, prio)				\
	  (u32)((((max_size) - (size)) << 24) | ((user) << 16) | (prio))

  The size is reversed (4 - size) so that X4 ~ 0, X2 ~ 2 and X1 ~ 3.

  Note that the reversed size has highest precedence, then user and finally prio.
  This means that X4 entries always has a numerical lower sort_key than X2 and
  X1 entries.
  The sort_key is stored together with the entry for faster traversal.

  To insert a new entry:
  1) Calculate the sort_key of the new entry.
  2) If list is empty, insert new entry first in list, execute the insert
     operations mentioned below and we goto 6).
  3) Set insertion address = last_valid_addr.
  4) Traverse list
       if new.sort_key <= current.sort_key, insert new entry before current
       entry, execute the insert operations below and goto 6).
       else set insertion address -= current.size.
  5) If we didn't found an insertion address in 4), insert new entry last in
     list, execute the insert operations below and goto 6).
  6) Set last_used_addr -= new.size.

  Inserting an X4 entry consists of the following operations:
  1) Find the insertion address. E.g. address [59..56].
  2) Move all entries [59..47] up to [55..43] to make room for the new entry.
  3) Write the new entry in address [59..56].

  Inserting an X2 entry consists of the following operations:
  1) Find the insertion address. E.g. address [51..50].
  2) Move all entries [51..47] up to [49..45] to make room for the new entry.
  3) Write the new entry in address [51..50].

  Inserting an X1 entry consists of the following operations:
  1) Find the insertion address. E.g. address 48.
  2) Move all entries [48..47] up to [47..46] to make room for the new entry.
  3) Write the new entry in address 48.

  To delete an existing entry:
  1) Set delete address = last_valid_addr.
  2) Traverse list
       if deleted.[user, prio, cookie] == current.[user, prio, cookie], delete
       entry from list, execute the delete operations below and goto 3).
       else set delete address -= current.size
  3) Set last_used_addr += deleted.size.

  Deleting an X4 entry consists of the following operations:
  1) Find the entry to delete. E.g. address [59..56].
  2) Move all entries [55..47] down to [59..51] to overwrite the entry.
  3) Clear unused addresses [50..47].

  Deleting an X2 entry consists of the following operations:
  1) Find the entry to delete. E.g. address [51..50].
  2) Move all entries [49..47] down to [51..49] to overwrite the entry.
  3) Clear unused addresses [48..47].

  Deleting an X1 entry consists of the following operations:
  1) Find the entry to delete. E.g. address 48.
  2) Move entriy 47 down to 48 to overwrite the entry.
  3) Clear unused address 47.

*/
/*******************************************************************************
 * Bit manupulation
 ******************************************************************************/

#define LAN966X_BITS_TO_U32(x)          (((x) + 31) / 32)
#define LAN966X_BITMASK(x)              ((1 << (x)) - 1)
#define LAN966X_EXTRACT_BITFIELD(x,o,w) (((x) >> (o)) & LAN966X_BITMASK(w))
#define LAN966X_ENCODE_BITFIELD(x,o,w)  (((x) & LAN966X_BITMASK(w)) << (o))

/* Bit manipulation utilities that operates on arrays of u32 */
#define LAN966X_BIT_MASK(offset) (((u32)1) << ((offset) % 32))
#define LAN966X_BIT_WORD(offset) ((offset) / 32)

/* Set or clear one bit */
static inline void lan966x_set_bit(volatile u32 *addr, u32 offset, bool value)
{
	u32 mask = LAN966X_BIT_MASK(offset);
	u32 *p = ((u32*)addr) + LAN966X_BIT_WORD(offset);
	if (value)
		*p |= mask;
	else
		*p &= ~mask;
}

/* Set or clear one or more bits from a u32 */
static void lan966x_set_bits(volatile u32 *addr, u32 offset, u32 len, u32 value)
{
	if (len > 32) {
		pr_err("illegal length: %u", len);
		return;
	}

	while (len--) {
		lan966x_set_bit(addr, offset++, value & 1);
		value >>= 1;
	}
}

/* Get one bit */
static inline bool lan966x_get_bit(const volatile u32 *addr, u32 offset)
{
	return !!(1 & (addr[LAN966X_BIT_WORD(offset)] >> (offset & 31)));
}

/* Get one or more bits into a u32 */
static u32 lan966x_get_bits(const volatile u32 *addr, u32 offset, u32 len)
{
	u32 value = 0;

	if (len > 32) {
		pr_err("illegal length: %u", len);
		return 0;
	}

	offset += len; /* work backwards */
	while (len--) {
		value <<= 1;
		if (lan966x_get_bit(addr, --offset))
			value |= 1;
	}
	return value;
}

/* Returns true if at least one bit is set in the interval */
static bool lan966x_bits_set(const volatile u32 *addr, u32 offset, u32 len)
{
	while (len--) {
		if (lan966x_get_bit(addr, offset++)) {
			return true;
		}
	}
	return false;
}

/*******************************************************************************
 * VCAP control
 ******************************************************************************/

/* VCAP data selection */
#define LAN966X_VCAP_SEL_ENTRY   0x01 /* Select entry */
#define LAN966X_VCAP_SEL_ACTION  0x02 /* Select action */
#define LAN966X_VCAP_SEL_COUNTER 0x04 /* Select counter */
#define LAN966X_VCAP_SEL_ALL     0xff /* Select all */

/* VCAP commands */
#define LAN966X_VCAP_CMD_WRITE     0 /* Write from cache to VCAP */
#define LAN966X_VCAP_CMD_READ      1 /* Read from VCAP to cache */
#define LAN966X_VCAP_CMD_MOVE_UP   2 /* Move up to lower addr and prio */
#define LAN966X_VCAP_CMD_MOVE_DOWN 3 /* Move down ti higher addr and prio */
#define LAN966X_VCAP_CMD_INIT      4 /* Set to unused */

/* TG values */
#define LAN966X_VCAP_TG_NONE 0x00
#define LAN966X_VCAP_TG_X1   0x01
#define LAN966X_VCAP_TG_X2   0x02
#define LAN966X_VCAP_TG_X4   0x04

/* VCAP rule index */
struct lan966x_vcap_idx {
	u32 row;           /* TCAM row */
	u32 col;           /* TCAM column */
	u32 sw_per_entry;  /* Subwords per entry */
};

struct lan966x_vcap_info {
	enum lan966x_vcap vcap;
	struct lan966x_vcap_data data;
	u32 cmd;
	u32 sel;
	u32 addr;
	u32 mv_size;
	u32 mv_pos;
	u32 key_tg;
	u32 act_tg;
	u32 cnt;
	size_t ll; /* current line length */
	bool is_action;
};

struct lan966x_vcap_cmd_cb {
	struct lan966x *lan966x;
	u32 instance;
};

static u32 lan966x_vcap_read_update_ctrl(const struct lan966x_vcap_cmd_cb *cb)
{
	return lan_rd(cb->lan966x, VCAP_UPDATE_CTRL(cb->instance));
}

static int lan966x_vcap_cmd(struct lan966x *lan966x,
			    struct lan966x_vcap_info *info)
{
	const struct lan966x_vcap_attrs *va = lan966x_vcap_attrs_get(info->vcap);
	const struct lan966x_vcap_cmd_cb cb = {
		.lan966x = lan966x,
		.instance = va->instance
	};
	u32 value, tgt = va->instance;

	lan_wr(VCAP_MV_CFG_MV_NUM_POS_SET(info->mv_pos) |
	       VCAP_MV_CFG_MV_SIZE_SET(info->mv_size),
	       lan966x, VCAP_MV_CFG(tgt));

	lan_wr(VCAP_UPDATE_CTRL_UPDATE_CMD_SET(info->cmd) |
	       VCAP_UPDATE_CTRL_UPDATE_ENTRY_DIS_SET(
		       info->sel & LAN966X_VCAP_SEL_ENTRY ? 0 : 1) |
	       VCAP_UPDATE_CTRL_UPDATE_ACTION_DIS_SET(
		       info->sel & LAN966X_VCAP_SEL_ACTION ? 0 : 1) |
	       VCAP_UPDATE_CTRL_UPDATE_CNT_DIS_SET(
		       info->sel & LAN966X_VCAP_SEL_COUNTER ? 0 : 1) |
	       VCAP_UPDATE_CTRL_UPDATE_ADDR_SET(info->addr) |
	       VCAP_UPDATE_CTRL_CLEAR_CACHE_SET(
		       info->cmd == LAN966X_VCAP_CMD_INIT ? 1 : 0) |
	       VCAP_UPDATE_CTRL_UPDATE_SHOT,
	       lan966x,
	       VCAP_UPDATE_CTRL(tgt));

	return readx_poll_timeout(lan966x_vcap_read_update_ctrl, &cb,
				  value,
				  VCAP_UPDATE_CTRL_UPDATE_SHOT_GET(value) == 0,
				  10, 100000);
}

static inline u32 lan966x_vcap_tg_count(u32 tg)
{
    return tg;
}

//#define ENTRY_CMD_DEBUG
static int lan966x_vcap_entry_cmd(struct lan966x *lan966x,
				  struct lan966x_vcap_info *info)
{
	const struct lan966x_vcap_attrs *va = lan966x_vcap_attrs_get(info->vcap);
	const struct lan966x_vcap_tgs_attrs *ta =
		lan966x_vcap_key_tgs_attrs_get(info->vcap, va->sw_count);
	u32 key_sw_cnt = 0, key_reg_cnt = 0, key_tgw = 0, key_offs = 0;
	u32 act_sw_cnt = 0, act_reg_cnt = 0, act_tgw = 0, act_offs = 0;
	u32 i, j, sw_cnt, val, msk, w, tg, tgw, tgt = va->instance;
	u32 addr_old = info->addr;
	u32 cnt_sw_cnt = info->sel & LAN966X_VCAP_SEL_COUNTER ? 1 : 0;
	int rc;

	if (info->sel & LAN966X_VCAP_SEL_ENTRY) {
		key_sw_cnt = lan966x_vcap_tg_count(info->key_tg);
		key_reg_cnt = LAN966X_BITS_TO_U32(va->sw_width);
		key_tgw = ta->tg_width;
	}

	if (info->sel & LAN966X_VCAP_SEL_ACTION) {
		act_sw_cnt = lan966x_vcap_tg_count(info->act_tg);
		act_reg_cnt = LAN966X_BITS_TO_U32(va->act_width);
		act_tgw = (info->vcap == LAN966X_VCAP_IS2 ? 2 : 0);
	}

	sw_cnt = max3(key_sw_cnt, act_sw_cnt, cnt_sw_cnt);
#ifdef ENTRY_CMD_DEBUG
	dev_dbg(lan966x->dev, "cmd: %u, sw_cnt: %u, key_sw_cnt: %u, act_sw_cnt: %u",
		info->cmd, sw_cnt, key_sw_cnt, act_sw_cnt);
#endif
	for (i = 0; i < sw_cnt; i++, info->addr++) {
		if (info->cmd == LAN966X_VCAP_CMD_READ) {
			/* Read from cache */
			rc = lan966x_vcap_cmd(lan966x, info);
			if (rc)
				return rc;
		}

		/* Key */
		for (j = 0; j < key_reg_cnt && i < key_sw_cnt; j++) {
			if (info->cmd == LAN966X_VCAP_CMD_READ && i == 0 && j == 0) {
				/* Read TG for first word in base address */
				val = lan_rd(lan966x, VCAP_ENTRY_DAT(tgt, j));
				msk = lan_rd(lan966x, VCAP_MASK_DAT(tgt, j));
				if ((val & 1) == 1 && (msk & 1) == 1) {
					/* Match-off means that entry is
					   disabled */
					info->key_tg = LAN966X_VCAP_TG_NONE;
				} else if (key_tgw) {
					/* IS1/IS2 key, width 3/2/1 */
					tgw = ((info->addr % 4) == 0 ? 3 :
					       (info->addr % 2) == 0 ? 2 : 1);
					tg = (LAN966X_EXTRACT_BITFIELD(val, 0, tgw) & ~msk);
					info->key_tg = ((tg & LAN966X_VCAP_TG_X1) ? LAN966X_VCAP_TG_X1 :
							(tg & LAN966X_VCAP_TG_X2) ? LAN966X_VCAP_TG_X2 :
							(tg & LAN966X_VCAP_TG_X4) ? LAN966X_VCAP_TG_X4 :
							LAN966X_VCAP_TG_NONE);
				} else {
					/* ES0 key, width 0 */
					info->key_tg = LAN966X_VCAP_TG_X1;
				}
			}

			/* Calculate data and TG width */
			tg = info->key_tg;
			tgw = ((j != 0 || ta->tg_width == 0 || tg == LAN966X_VCAP_TG_NONE) ? 0 :
			       (tg > LAN966X_VCAP_TG_X2 && (info->addr % 4) == 0) ? 3 :
			       (tg > LAN966X_VCAP_TG_X1 && (info->addr % 2) == 0) ? 2 : 1);
			w = (va->sw_width % 32);
			w = ((j == (key_reg_cnt - 1) && w != 0 ? w : 32) - tgw);

			/* Read/write key */
			if (info->cmd == LAN966X_VCAP_CMD_READ) {
				val = lan_rd(lan966x, VCAP_ENTRY_DAT(tgt, j));
				msk = lan_rd(lan966x, VCAP_MASK_DAT(tgt, j));
				lan966x_set_bits(info->data.entry, key_offs, w, val >> tgw);
				lan966x_set_bits(info->data.mask, key_offs, w, ~msk >> tgw);
			} else {
				val = ((lan966x_get_bits(info->data.entry, key_offs, w) << tgw) +
				       LAN966X_ENCODE_BITFIELD(tg, 0, tgw));
				msk = ((lan966x_get_bits(info->data.mask, key_offs, w) << tgw) +
				       LAN966X_ENCODE_BITFIELD(0xff, 0, tgw));
				msk = ~msk;
				lan_wr(val, lan966x, VCAP_ENTRY_DAT(tgt, j));
				lan_wr(msk, lan966x, VCAP_MASK_DAT(tgt, j));
			}
#ifdef ENTRY_CMD_DEBUG
			dev_dbg(lan966x->dev, "addr: %u, j: %u, val/msk: 0x%08x/%08x",
				info->addr, j, val, msk);
			dev_dbg(lan966x->dev, "tg: %u, tgw: %u, w: %u",
				tg, tgw, w);
#endif
			key_offs += w;
		}

		/* Action */
		for (j = 0; j < act_reg_cnt && i < act_sw_cnt; j++) {
			if (info->cmd == LAN966X_VCAP_CMD_READ && i == 0 && j == 0) {
				/* Read TG for first word in base address */
				if (act_tgw) {
					/* IS2 action, width 2/1 */
					val = lan_rd(lan966x,
						     VCAP_ACTION_DAT(tgt, j));
					tgw = ((info->addr % 2) == 0 ? 2 : 1);
					tg = LAN966X_EXTRACT_BITFIELD(val, 0, tgw);
					info->act_tg = ((tg & LAN966X_VCAP_TG_X1) ? LAN966X_VCAP_TG_X1 :
							(tg & LAN966X_VCAP_TG_X2) ? LAN966X_VCAP_TG_X2 :
							LAN966X_VCAP_TG_NONE);
				} else {
					/* IS1/ES0 action, width 0 */
					info->act_tg = LAN966X_VCAP_TG_X1;
				}
			}

			/* Calculate data and TG width */
			tg = info->act_tg;
			tgw = ((j != 0 || act_tgw == 0 || tg == LAN966X_VCAP_TG_NONE) ? 0 :
			       (tg > LAN966X_VCAP_TG_X1 && (info->addr % 2) == 0) ? 2 : 1);
			w = (va->act_width % 32);
			w = ((j == (act_reg_cnt - 1) && w != 0 ? w : 32) - tgw);

			/* Read/write action */
			if (info->cmd == LAN966X_VCAP_CMD_READ) {
				val = lan_rd(lan966x, VCAP_ACTION_DAT(tgt, j));
				lan966x_set_bits(info->data.action, act_offs, w, val >> tgw);
			} else {
				val = ((lan966x_get_bits(info->data.action, act_offs, w) << tgw) +
				       LAN966X_ENCODE_BITFIELD(tg, 0, tgw));
				lan_wr(val, lan966x, VCAP_ACTION_DAT(tgt, j));
			}
#ifdef ENTRY_CMD_DEBUG
			dev_dbg(lan966x->dev, "addr: %u, j: %u, val: 0x%08x",
				info->addr, j, val);
			dev_dbg(lan966x->dev, "tg: %u, tgw: %u, w: %u",
				tg, tgw, w);
#endif
			act_offs += w;
		}

		/* Counter */
		if ((info->sel & LAN966X_VCAP_SEL_COUNTER) && i == 0) {
			if (info->cmd == LAN966X_VCAP_CMD_READ)
				info->cnt = lan_rd(lan966x,
						   VCAP_CNT_DAT(tgt, 0));
			else
				lan_wr(info->cnt, lan966x, VCAP_CNT_DAT(tgt, 0));
		}

		if (info->cmd == LAN966X_VCAP_CMD_WRITE) {
			/* Write to cache */
			rc = lan966x_vcap_cmd(lan966x, info);
			if (rc)
				return rc;
		}
	}
	info->addr = addr_old; /* Restore original address */
	return 0;
}

/*******************************************************************************
 * VCAP control
 ******************************************************************************/
/**
 * lan966x_vcap_lookup_get - Get the lookup for a rule
 * @lan966x: switch device
 * @vcap: VCAP to use.
 * @rule: rule to examine.
 *
 * Returns lookup number on success or negative error code on failure.
 */
static int lan966x_vcap_lookup_get(struct lan966x *lan966x,
				   enum lan966x_vcap vcap,
				   const struct lan966x_vcap_rule *rule)
{
	const struct lan966x_vcap_u8 *lookup = NULL;
	const enum lan966x_vcap_bit *first = NULL;
	int l = 0;

	switch (vcap) {
	case LAN966X_VCAP_IS1:
		switch (rule->is1.key.key) {
		case LAN966X_VCAP_IS1_KEY_S1_NORMAL:
			lookup = &rule->is1.key.s1_normal.lookup;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4:
			lookup = &rule->is1.key.s1_5tuple_ip4.lookup;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6:
			lookup = &rule->is1.key.s1_normal_ip6.lookup;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_7TUPLE:
			lookup = &rule->is1.key.s1_7tuple.lookup;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP6:
			lookup = &rule->is1.key.s1_5tuple_ip6.lookup;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_DBL_VID:
			lookup = &rule->is1.key.s1_dbl_vid.lookup;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_RT:
			first = &rule->is1.key.s1_rt.first;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_DMAC_VID:
			lookup = &rule->is1.key.s1_dmac_vid.lookup;
			break;
		default:
			dev_err(lan966x->dev, "ERROR: Invalid key!\n");
			return -EINVAL;
		}
		break;
	case LAN966X_VCAP_IS2:
		switch (rule->is2.key.key) {
		case LAN966X_VCAP_IS2_KEY_MAC_ETYPE:
			first = &rule->is2.key.mac_etype.first;
			break;
		case LAN966X_VCAP_IS2_KEY_MAC_LLC:
			first = &rule->is2.key.mac_llc.first;
			break;
		case LAN966X_VCAP_IS2_KEY_MAC_SNAP:
			first = &rule->is2.key.mac_snap.first;
			break;
		case LAN966X_VCAP_IS2_KEY_ARP:
			first = &rule->is2.key.arp.first;
			break;
		case LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP:
			first = &rule->is2.key.ip4_tcp_udp.first;
			break;
		case LAN966X_VCAP_IS2_KEY_IP4_OTHER:
			first = &rule->is2.key.ip4_other.first;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_STD:
			first = &rule->is2.key.ip6_std.first;
			break;
		case LAN966X_VCAP_IS2_KEY_OAM:
			first = &rule->is2.key.oam.first;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP:
			first = &rule->is2.key.ip6_tcp_udp.first;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_OTHER:
			first = &rule->is2.key.ip6_other.first;
			break;
		case LAN966X_VCAP_IS2_KEY_CUSTOM:
			first = &rule->is2.key.custom.first;
			break;
		case LAN966X_VCAP_IS2_KEY_SMAC_SIP4:
		case LAN966X_VCAP_IS2_KEY_SMAC_SIP6:
			l = 2; /* only one lookup but shown as third lookup */
			break;
		default:
			dev_err(lan966x->dev, "ERROR: Invalid key!\n");
			return -EINVAL;
		}
		break;
	case LAN966X_VCAP_ES0:
		l = 0; /* only one lookup - show as first lookup */
		break;
	default:
		dev_err(lan966x->dev, "ERROR: Invalid VCAP!\n");
		return -EINVAL;
	}

	if (lookup) {
		if ((lookup->mask & LAN966X_VCAP_LOOKUP_MASK) !=
		    LAN966X_VCAP_LOOKUP_MASK) {
			dev_err(lan966x->dev,
				"ERROR: 'lookup mask' must be 0x%x\n",
				LAN966X_VCAP_LOOKUP_MASK);
			return -EINVAL;
		}
		if (lookup->value >= LAN966X_VCAP_NUM_LOOKUPS_MAX) {
			dev_err(lan966x->dev,
				"ERROR: 'lookup value' must be less than %d\n",
				LAN966X_VCAP_NUM_LOOKUPS_MAX);
			return -EINVAL;
		}
		l = lookup->value;
	} else if (first) {
		switch (*first) {
		case LAN966X_VCAP_BIT_ANY:
			dev_err(lan966x->dev,
				"ERROR: 'first' must be specified\n");
			return -EINVAL;
		case LAN966X_VCAP_BIT_1:
			l = 0; /* lookup(0) */
			break;
		case LAN966X_VCAP_BIT_0:
			l = 1; /* lookup(1) */
			break;
		}
	}

	return l;
}

/**
 * lan966x_vcap_pack - pack a rule into binary data
 * @vcap: VCAP to use.
 * @rule: rule to pack.
 * @data: Where to store the binary data.
 *
 * Returns 0 on success or error code on failure.
 */
static int lan966x_vcap_pack(enum lan966x_vcap vcap,
			     const struct lan966x_vcap_rule *rule,
			     struct lan966x_vcap_data *data)
{
	int err;

	switch (vcap) {
	case LAN966X_VCAP_ES0:
		err = lan966x_vcap_es0_key_pack(&rule->es0.key, data);
		if (err)
			return err;

		return lan966x_vcap_es0_action_pack(&rule->es0.action, data);
	case LAN966X_VCAP_IS1:
		err = lan966x_vcap_is1_key_pack(&rule->is1.key, data);
		if (err)
			return err;

		return lan966x_vcap_is1_action_pack(&rule->is1.action, data);
	case LAN966X_VCAP_IS2:
		err = lan966x_vcap_is2_key_pack(&rule->is2.key, data);
		if (err)
			return err;

		return lan966x_vcap_is2_action_pack(&rule->is2.action, data);
	default:
		return -EINVAL;
	}
}

/**
 * lan966x_vcap_rule_size_get - get number of subwords for VCAP rule
 * @vcap: VCAP to use.
 * @rule: rule to examine.
 * @key_sw: Number of subwords in key. Set to NULL if no interest.
 * @act_sw: Number of subwords in action. Set to NULL if no interest.
 *
 * Returns 0 on success or error code on failure.
 */
static int lan966x_vcap_rule_size_get(enum lan966x_vcap vcap,
				      const struct lan966x_vcap_rule *rule,
				      u32 *key_sw, u32 *act_sw)
{
	int k, a;

	switch (vcap) {
	case LAN966X_VCAP_ES0:
		k = rule->es0.key.key;
		a = rule->es0.action.action;
		break;
	case LAN966X_VCAP_IS1:
		k = rule->is1.key.key;
		a = rule->is1.action.action;
		break;
	case LAN966X_VCAP_IS2:
		k = rule->is2.key.key;
		a = rule->is2.action.action;
		break;
	default:
		return -EINVAL;
	}

	if (key_sw) {
		const struct lan966x_vcap_key_attrs *ka =
			lan966x_vcap_key_attrs_get(vcap, k);
		if (ka)
			*key_sw = ka->sw_per_entry;
		else
			return -EINVAL;
	}

	if (act_sw) {
		const struct lan966x_vcap_action_attrs *aa =
			lan966x_vcap_action_attrs_get(vcap, a);
		if (aa)
			*act_sw = aa->sw_per_action;
		else
			return -EINVAL;
	}

	return 0;
}

/**
 * lan966x_vcap_hw_init - initialize (disable) a number of addresses in VCAP
 * @lan966x: switch device
 * @vcap: VCAP to use.
 * @addr: start address to initialize.
 * @size: number of addresses to initialize.
 *
 * The address specifies the lowest numerical address.
 * E.g.: Set addr = 8 and size = 4 to initialize addr 8-11.
 */
static int lan966x_vcap_hw_init(struct lan966x *lan966x,
				enum lan966x_vcap vcap,
				u32 addr,
				u32 size)
{
	struct lan966x_vcap_info info = {};

	dev_dbg(lan966x->dev, "HW_INIT: vcap %d addr %u size %u\n",
		vcap, addr, size);

	if (size < 1) {
		dev_err(lan966x->dev, "size (%u) must be greater than 1\n",
			size);
		return -EINVAL;
	}

	info.vcap = vcap;
	info.cmd = LAN966X_VCAP_CMD_INIT;
	info.sel = LAN966X_VCAP_SEL_ALL;
	info.addr = addr;
	info.mv_size = size - 1;
	return lan966x_vcap_cmd(lan966x, &info);
}

/**
 * lan966x_vcap_hw_move - move a number of subwords in VCAP
 * @lan966x: switch device
 * @vcap: VCAP to use.
 * @low: lowest numerical address to move.
 * @high: highest numerical address to move.
 * @distance: distance to move.
 * @up: if true move to a lower numerical address (decreasing priority).
 *
 * Set high = low to move only one address.
 */
static int lan966x_vcap_hw_move(struct lan966x *lan966x,
				enum lan966x_vcap vcap,
				u32 low,
				u32 high,
				u32 distance,
				bool up)
{
	struct lan966x_vcap_info info = {};

	dev_dbg(lan966x->dev, "HW_MOVE: vcap %d low %u high %u distance %u %s\n",
		vcap, low, high, distance, up ? "up" : "down");

	if (low > high) {
		dev_err(lan966x->dev, "low (%u) > high (%u)\n", low, high);
		return -EINVAL;
	}

	info.vcap = vcap;
	info.cmd = (up ? LAN966X_VCAP_CMD_MOVE_UP : LAN966X_VCAP_CMD_MOVE_DOWN);
	info.sel = LAN966X_VCAP_SEL_ALL;
	info.addr = low;
	info.mv_size = high - low;
	info.mv_pos = distance - 1;
	return lan966x_vcap_cmd(lan966x, &info);
}

/**
 * lan966x_vcap_hw_write - write VCAP entry
 * @lan966x: switch device
 * @vcap: VCAP to use.
 * @addr: start address to initialize.
 * @num_addr: number of addresses to initialize.
 *
 */
static int lan966x_vcap_hw_write(struct lan966x *lan966x,
				 enum lan966x_vcap vcap,
				 u32 addr,
				 const struct lan966x_vcap_rule *rule)
{
	struct lan966x_vcap_info info = {};
	u32 key_sw, act_sw;
	int err;

	dev_dbg(lan966x->dev, "HW_WRITE: vcap %d addr %u\n", vcap, addr);

	err = lan966x_vcap_rule_size_get(vcap, rule, &key_sw, &act_sw);
	if (err < 0) {
		dev_err(lan966x->dev, "Error from rule_size_get!\n");
		return err;
	}

	info.vcap = vcap;
	info.cmd = LAN966X_VCAP_CMD_WRITE;
	info.sel = LAN966X_VCAP_SEL_ALL;
	info.addr = addr;
	info.cnt = 0;
	info.key_tg = key_sw;
	info.act_tg = act_sw;

	err = lan966x_vcap_pack(vcap, rule, &info.data);
	if (err < 0) {
		dev_err(lan966x->dev, "Error from pack!\n");
		return err;
	}

	return lan966x_vcap_entry_cmd(lan966x, &info);
}

static int lan966x_vcap_hw_get(struct lan966x *lan966x,
			       enum lan966x_vcap vcap,
			       u32 addr,
			       u32 *counter,
			       bool clear)
{
	struct lan966x_vcap_info info = {};
	int err;

	dev_dbg(lan966x->dev, "HW_GET: vcap %d addr %u clear %d\n",
		vcap, addr, clear);

	info.vcap = vcap;
	info.cmd = LAN966X_VCAP_CMD_READ;
	info.sel = LAN966X_VCAP_SEL_COUNTER;
	info.addr = addr;

	err = lan966x_vcap_entry_cmd(lan966x, &info);
	if (err)
		return err;

	if (counter)
		*counter = info.cnt;

	if (clear) {
		info.cmd = LAN966X_VCAP_CMD_WRITE;
		info.cnt = 0;
		err = lan966x_vcap_entry_cmd(lan966x, &info);
	}
	return err;
}
/**
 * lan966x_vcap_lookup - lookup a specific entry
 * @lan966x: switch device
 * @vcap: VCAP to look into.
 * @user: entry user.
 * @prio: entry priority.
 * @cookie: entry cookie.
 * @addr: address in TCAM. Set to NULL if no interest.
 *
 * Must be called with locked mutex.
 * Returns pointer to the entry or NULL if not found.
 */
static struct lan966x_vcap_rule_entry *lan966x_vcap_lookup(
	struct lan966x *lan966x,
	enum lan966x_vcap vcap,
	enum lan966x_vcap_user user,
	u16 prio, unsigned long cookie,
	u32 *addr)
{
	struct lan966x_vcap_admin *a = &lan966x->vcap[vcap];
	struct lan966x_vcap_rule_entry *entry = NULL;
	struct lan966x_vcap_rule_entry *e;
	u32 tmp_addr = a->last_valid_addr;

	if (!list_empty(&a->list)) {
		list_for_each_entry(e, &a->list, list) {
			if ((e->user == user) &&
			    (e->prio == prio) &&
			    (e->cookie == cookie)) {
				entry = e;
				break;
			}
			tmp_addr -= e->size;
		}
	}

	/* tmp_addr designates the address of the highest numerical subword.
	 * Return the address of lowest numerical subword. */
	if (addr && entry)
		*addr = tmp_addr - entry->size + 1;

	return entry;
}
/* Entries are sorted with increasing values of sort_key.
 * I.e. Lowest numerical sort_key is first in list.
 * In order to locate largest keys first in list we negate the key size with
 * (max_size - size). Now X4 are first in list, then X2 and finally X1.
 */
#define SORT_KEY_INIT(max_size, size, user, prio)				\
	(u32)((((max_size) - (size)) << 24) | ((user) << 16) | (prio))

int lan966x_vcap_add(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     const struct lan966x_vcap_rule *rule)
{
	const struct lan966x_vcap_attrs *va = lan966x_vcap_attrs_get(vcap);
	struct lan966x_vcap_rule_entry *new_entry;
	struct lan966x_vcap_rule_entry *e;
	struct lan966x_vcap_admin *a;
	u32 addr, key_sw, act_sw, size;
	struct list_head *list;
	int lookup, err;

	dev_dbg(lan966x->dev, "------------ User %d ----------!\n", user);
	if (va == NULL) {
		dev_err(lan966x->dev, "ERROR: Invalid VCAP!\n");
		return -EINVAL;
	}

	if (rule == NULL) {
		dev_err(lan966x->dev, "ERROR: Missing rule!\n");
		return -EINVAL;
	}

	lookup = lan966x_vcap_lookup_get(lan966x, vcap, rule);
	if (lookup < 0)
		return lookup;

	a = &lan966x->vcap[vcap];
	mutex_lock(&a->lock);

	/* Entry must not exist */
	e = lan966x_vcap_lookup(lan966x, vcap, user, prio, cookie, NULL);
	if (e) {
		dev_dbg(lan966x->dev, "Entry exist!\n");
		err = -EEXIST;
		goto out;
	}

	err = lan966x_vcap_rule_size_get(vcap, rule, &key_sw, &act_sw);
	if (err < 0)
		goto out;

	size = max(key_sw, act_sw);

	/* Check if there is enough free space left in TCAM */
	if (a->last_used_addr < size) {
		dev_dbg(lan966x->dev, "No more space!\n");
		err = -ENOSPC;
		goto out;
	}

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (new_entry == NULL) {
		err = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&new_entry->list);
	new_entry->size = size;
	new_entry->user = user;
	new_entry->prio = prio;
	new_entry->cookie = cookie;
	new_entry->sort_key = SORT_KEY_INIT(va->sw_count, size, user, prio);
	new_entry->rule = *rule;

	addr = a->last_valid_addr;
	list = &a->list; /* The default list_head to insert into */

	dev_dbg(lan966x->dev, "BEGIN: vcap %d lua %u size %u user %d prio %u\n",
		vcap, a->last_used_addr, size, user, prio);

	if (!list_empty(&a->list)) {
		list_for_each_entry(e, &a->list, list) {
			if (new_entry->sort_key <= e->sort_key) {
				dev_dbg(lan966x->dev,
					"INSERT: 0x%08x before 0x%08x, addr %u size %u\n",
					new_entry->sort_key, e->sort_key,
					addr - size + 1, size);
				 /* Change list to current entry! */
				list = &e->list;
				break;
			} else {
				dev_dbg(lan966x->dev,
					"FOUND: 0x%08x, addr %u size %u\n",
					e->sort_key, addr - e->size + 1,
					e->size);
				addr -= e->size;
			}
		}
	}

	if (list == &a->list) {
		dev_dbg(lan966x->dev,
			"INSERT: 0x%08x at end, addr %u size %u\n",
			new_entry->sort_key, addr - size + 1, size);
	}

	list_add_tail(&new_entry->list, list);

	if (addr >= a->last_used_addr) {
		/* There are entries at the insertion point and up
		 * Move them up as many addresses as we occupy */
		err = lan966x_vcap_hw_move(lan966x, vcap, a->last_used_addr,
					   addr, size, true);
	}

	a->last_used_addr -= size;

	/* addr is pointing to the last (numerically highest) subword.
	 * Modify it to point to the first (numerically lowest) subword.
	 */
	addr = addr - size + 1;

	dev_dbg(lan966x->dev, "END: lua %u, addr %u size %u\n",
		a->last_used_addr, addr, size);

	err = lan966x_vcap_hw_write(lan966x, vcap, addr, rule);
	if (err)
		goto out;

	/* update rule counter */
	a->num_rules[lookup]++;

out:
	mutex_unlock(&a->lock);
	return err;
}

int lan966x_vcap_del(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     struct lan966x_vcap_rule *rule)
{
	struct lan966x_vcap_rule_entry *e;
	struct lan966x_vcap_admin *a;
	int err = 0;
	u32 addr;

	if (vcap >= LAN966X_VCAP_LAST) {
		dev_err(lan966x->dev, "ERROR: Invalid VCAP!\n");
		return -EINVAL;
	}

	a = &lan966x->vcap[vcap];
	mutex_lock(&a->lock);

	/* Entry must exist */
	e = lan966x_vcap_lookup(lan966x, vcap, user, prio, cookie, &addr);
	if (!e) {
		dev_dbg(lan966x->dev, "Entry not found!\n");
		err = -ENOENT;
		goto out;
	}

	if (rule)
		*rule = e->rule;

	dev_dbg(lan966x->dev,
		"DEL: vcap %d lua %u addr %u size %u user %d prio %u cookie %lu sort_key 0x%08x\n",
		vcap, a->last_used_addr, addr, e->size, e->user, e->prio,
		e->cookie, e->sort_key);

	/* Release reserved stream filter instance */
	if (e->rule.sfi) {
		err = lan966x_sfi_ix_release(lan966x, e->rule.sfi_ix);
		if (err)
			goto delete;
	}

	/* Release reserved stream gate instance */
	if (e->rule.sgi_user != LAN966X_RES_POOL_FREE) {
		err = lan966x_sgi_ix_release(lan966x, e->rule.sgi_user,
					     e->rule.sgi_id);
		if (err)
			goto delete;
	}

	/* Release reserved policer */
	if (e->rule.pol_user != LAN966X_RES_POOL_FREE) {
		err = lan966x_pol_ix_release(lan966x, e->rule.pol_user,
					     e->rule.pol_id);
		if (err)
			goto delete;
	}

	/* Delete VCAP mirroring */
	if (e->rule.mirroring)
		lan966x_mirror_vcap_del(lan966x);

	if (addr > a->last_used_addr) {
		/* There are entries above us
		 * Move them down as many addresses as we occupy */
		err = lan966x_vcap_hw_move(lan966x, vcap, a->last_used_addr,
					   addr - 1, e->size, false);
		if (err)
			goto delete;
	}

	/* Initialize unused VCAP entries */
	err = lan966x_vcap_hw_init(lan966x, vcap, a->last_used_addr,
				   e->size);

	if (err)
		goto delete;

	a->last_used_addr += e->size;

	err = lan966x_vcap_lookup_get(lan966x, vcap, &e->rule);
	if (err < 0)
		goto delete;

	/* update rule counter */
	if (a->num_rules[err]) {
		a->num_rules[err]--;
	} else {
		dev_err(lan966x->dev, "ERROR: Invalid counter value!\n");
	}
	err = 0;

delete:
	list_del(&e->list);
	kfree(e);

out:
	mutex_unlock(&a->lock);
	return err;
}

int lan966x_vcap_mod(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     const struct lan966x_vcap_rule *rule)
{
	struct lan966x_vcap_rule_entry *e;
	struct lan966x_vcap_admin *a;
	u32 addr, key_sw, act_sw;
	int err = 0;

	if (vcap >= LAN966X_VCAP_LAST) {
		dev_err(lan966x->dev, "ERROR: Invalid VCAP!\n");
		return -EINVAL;
	}

	if (rule == NULL) {
		dev_err(lan966x->dev, "ERROR: Missing rule!\n");
		return -EINVAL;
	}

	a = &lan966x->vcap[vcap];
	mutex_lock(&a->lock);

	/* Entry must exist */
	e = lan966x_vcap_lookup(lan966x, vcap, user, prio, cookie, &addr);
	if (!e) {
		dev_dbg(lan966x->dev, "Entry not found!\n");
		err = -ENOENT;
		goto out;
	}

	/* Check that number of subwords is unchanged */
	err = lan966x_vcap_rule_size_get(vcap, rule, &key_sw, &act_sw);
	if (err < 0)
		goto out;

	if (e->size != max(key_sw, act_sw)) {
		err = -EINVAL;
		goto out;
	}

	dev_dbg(lan966x->dev, "MODIFY: vcap %d addr %u size %u\n",
		vcap, addr, e->size);

	/* update rule counter for existing rule */
	err = lan966x_vcap_lookup_get(lan966x, vcap, &e->rule);
	if (err < 0)
		goto out;

	if (a->num_rules[err]) {
		a->num_rules[err]--;
	} else {
		dev_err(lan966x->dev, "ERROR: Invalid counter value!\n");
	}

	/* update rule counter for new rule */
	err = lan966x_vcap_lookup_get(lan966x, vcap, rule);
	if (err < 0)
		goto out;

	a->num_rules[err]++;

	e->rule = *rule;
	err = lan966x_vcap_hw_write(lan966x, vcap, addr, rule);

out:
	mutex_unlock(&a->lock);
	return err;
}

int lan966x_vcap_get(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     struct lan966x_vcap_rule *rule,
		     u32 *hits)
{
	struct lan966x_vcap_rule_entry *e;
	struct lan966x_vcap_admin *a;
	int err = 0;
	u32 addr;

	if (vcap >= LAN966X_VCAP_LAST) {
		dev_err(lan966x->dev, "ERROR: Invalid VCAP!\n");
		return -EINVAL;
	}

	a = &lan966x->vcap[vcap];
	mutex_lock(&a->lock);

	/* Entry must exist */
	e = lan966x_vcap_lookup(lan966x, vcap, user, prio, cookie, &addr);
	if (!e) {
		dev_dbg(lan966x->dev, "Entry not found!\n");
		err = -ENOENT;
		goto out;
	}

	dev_dbg(lan966x->dev, "GET: vcap %d addr %u size %u\n",
		vcap, addr, e->size);

	if (rule)
		*rule = e->rule;

	if (hits) {
		err = lan966x_vcap_hw_get(lan966x, vcap, addr, hits, true);
		if (err) {
			dev_err(lan966x->dev,
				"ERROR: lan966x_vcap_hw_get()!\n");
			goto out;
		}
	}
out:
	mutex_unlock(&a->lock);
	return err;
}

/*******************************************************************************
 * VCAP configuration
 ******************************************************************************/
int lan966x_vcap_igr_port_mask_set(struct lan966x *lan966x,
				   enum lan966x_vcap vcap,
				   struct lan966x_vcap_rule *r,
				   const struct lan966x_vcap_u16 *m)
{
	switch (vcap) {
	case LAN966X_VCAP_IS1:
		switch (r->is1.key.key) {
		case LAN966X_VCAP_IS1_KEY_S1_NORMAL:
			r->is1.key.s1_normal.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4:
			r->is1.key.s1_5tuple_ip4.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6:
			r->is1.key.s1_normal_ip6.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_7TUPLE:
			r->is1.key.s1_7tuple.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP6:
			r->is1.key.s1_5tuple_ip6.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_DBL_VID:
			r->is1.key.s1_dbl_vid.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_DMAC_VID:
			r->is1.key.s1_dmac_vid.igr_port_mask = *m;
			break;
		default:
			/* LAN966X_VCAP_IS1_KEY_S1_RT has no port mask */
			dev_err(lan966x->dev, "Invalid IS1 key %d", r->is1.key.key);
			return -EINVAL;
		}
		break;
	case LAN966X_VCAP_IS2:
		switch (r->is2.key.key) {
		case LAN966X_VCAP_IS2_KEY_MAC_ETYPE:
			r->is2.key.mac_etype.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_MAC_LLC:
			r->is2.key.mac_llc.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_MAC_SNAP:
			r->is2.key.mac_snap.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_ARP:
			r->is2.key.arp.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP:
			r->is2.key.ip4_tcp_udp.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_IP4_OTHER:
			r->is2.key.ip4_other.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_STD:
			r->is2.key.ip6_std.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_OAM:
			r->is2.key.oam.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP:
			r->is2.key.ip6_tcp_udp.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_OTHER:
			r->is2.key.ip6_other.igr_port_mask = *m;
			break;
		case LAN966X_VCAP_IS2_KEY_CUSTOM:
			r->is2.key.custom.igr_port_mask = *m;
			break;
		default:
			/* LAN966X_VCAP_IS2_KEY_SMAC_SIP4/SIP6 has no port mask */
			dev_err(lan966x->dev, "Invalid IS2 key %d", r->is2.key.key);
			return -EINVAL;
		}
		break;
	default:
		dev_err(lan966x->dev, "Invalid VCAP %d", vcap);
		return -EINVAL;
	}

	return 0;
}

int lan966x_vcap_igr_port_mask_get(struct lan966x *lan966x,
				   enum lan966x_vcap vcap,
				   const struct lan966x_vcap_rule *r,
				   struct lan966x_vcap_u16 *m)
{
	switch (vcap) {
	case LAN966X_VCAP_IS1:
		switch (r->is1.key.key) {
		case LAN966X_VCAP_IS1_KEY_S1_NORMAL:
			*m = r->is1.key.s1_normal.igr_port_mask;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4:
			*m = r->is1.key.s1_5tuple_ip4.igr_port_mask;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6:
			*m = r->is1.key.s1_normal_ip6.igr_port_mask;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_7TUPLE:
			*m = r->is1.key.s1_7tuple.igr_port_mask;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP6:
			*m = r->is1.key.s1_5tuple_ip6.igr_port_mask;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_DBL_VID:
			*m = r->is1.key.s1_dbl_vid.igr_port_mask;
			break;
		case LAN966X_VCAP_IS1_KEY_S1_DMAC_VID:
			*m = r->is1.key.s1_dmac_vid.igr_port_mask;
			break;
		default:
			/* LAN966X_VCAP_IS1_KEY_S1_RT has no port mask */
			dev_err(lan966x->dev, "Invalid IS1 key %d", r->is1.key.key);
			return -EINVAL;
		}
		break;
	case LAN966X_VCAP_IS2:
		switch (r->is2.key.key) {
		case LAN966X_VCAP_IS2_KEY_MAC_ETYPE:
			*m = r->is2.key.mac_etype.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_MAC_LLC:
			*m = r->is2.key.mac_llc.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_MAC_SNAP:
			*m = r->is2.key.mac_snap.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_ARP:
			*m = r->is2.key.arp.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP:
			*m = r->is2.key.ip4_tcp_udp.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_IP4_OTHER:
			*m = r->is2.key.ip4_other.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_STD:
			*m = r->is2.key.ip6_std.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_OAM:
			*m = r->is2.key.oam.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP:
			*m = r->is2.key.ip6_tcp_udp.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_IP6_OTHER:
			*m = r->is2.key.ip6_other.igr_port_mask;
			break;
		case LAN966X_VCAP_IS2_KEY_CUSTOM:
			*m = r->is2.key.custom.igr_port_mask;
			break;
		default:
			/* LAN966X_VCAP_IS2_KEY_SMAC_SIP4/SIP6 has no port mask */
			dev_err(lan966x->dev, "Invalid IS2 key %d", r->is2.key.key);
			return -EINVAL;
		}
		break;
	default:
		dev_err(lan966x->dev, "Invalid VCAP %d", vcap);
		return -EINVAL;
	}

	return 0;
}

int lan966x_vcap_num_rules_get(const struct lan966x *lan966x,
			       enum lan966x_vcap vcap,
			       u8 lookup)
{
	if (vcap >= LAN966X_VCAP_LAST) {
		dev_err(lan966x->dev, "ERROR: Invalid VCAP!\n");
		return -EINVAL;
	}

	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_MAX) {
		dev_err(lan966x->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	return lan966x->vcap[vcap].num_rules[lookup];
}

int lan966x_vcap_is1_port_smac_set(struct lan966x_port *port,
				   u8 lookup,
				   bool smac)
{
	u32 reg, fld;

	netdev_dbg(port->dev, "smac %d\n", smac);

	if (port->lan966x->vcap[LAN966X_VCAP_IS1].num_rules[lookup]) {
		netdev_err(port->dev, "ERROR: IS1 not empty!\n");
		return -EBUSY;
	}

	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	reg = lan_rd(port->lan966x, ANA_VCAP_CFG(port->chip_port));
	fld = ANA_VCAP_CFG_S1_SMAC_ENA_GET(reg);

	if (smac)
		fld |= BIT(lookup);
	else
		fld &= ~BIT(lookup);

	lan_rmw(ANA_VCAP_CFG_S1_SMAC_ENA_SET(fld),
		ANA_VCAP_CFG_S1_SMAC_ENA,
		port->lan966x, ANA_VCAP_CFG(port->chip_port));

	port->is1[lookup].smac = smac;
	return 0;
}

int lan966x_vcap_is1_port_smac_get(const struct lan966x_port *port,
				   u8 lookup,
				   bool *smac)
{
	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	*smac = port->is1[lookup].smac;
	return 0;
}

int lan966x_vcap_is1_port_dmac_dip_set(struct lan966x_port *port,
				       u8 lookup,
				       bool dmac_dip)
{
	u32 reg, fld;

	netdev_dbg(port->dev, "dmac_dip %d\n", dmac_dip);

	if (port->lan966x->vcap[LAN966X_VCAP_IS1].num_rules[lookup]) {
		netdev_err(port->dev, "ERROR: IS1 not empty!\n");
		return -EBUSY;
	}

	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	reg = lan_rd(port->lan966x, ANA_VCAP_CFG(port->chip_port));
	fld = ANA_VCAP_CFG_S1_DMAC_DIP_ENA_GET(reg);

	if (dmac_dip)
		fld |= BIT(lookup);
	else
		fld &= ~BIT(lookup);

	lan_rmw(ANA_VCAP_CFG_S1_DMAC_DIP_ENA_SET(fld),
		ANA_VCAP_CFG_S1_DMAC_DIP_ENA,
		port->lan966x, ANA_VCAP_CFG(port->chip_port));

	port->is1[lookup].dmac_dip = dmac_dip;
	return 0;
}

int lan966x_vcap_is1_port_dmac_dip_get(const struct lan966x_port *port,
				       u8 lookup,
				       bool *dmac_dip)
{
	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	*dmac_dip = port->is1[lookup].dmac_dip;
	return 0;
}

static int lan966x_vcap_is1_port_key_ipv4_set(struct lan966x_port *port,
					      u8 lookup,
					      enum lan966x_vcap_is1_key key)
{
	u32 val;

	netdev_dbg(port->dev, "lookup %d key %s\n", lookup,
		   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS1, key)->name);

	if (port->lan966x->vcap[LAN966X_VCAP_IS1].num_rules[lookup]) {
		netdev_err(port->dev, "ERROR: IS1 not empty!\n");
		return -EBUSY;
	}

	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	switch (key) {
	case LAN966X_VCAP_IS1_KEY_S1_NORMAL:
		val = 0;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_7TUPLE:
		val = 1;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4:
		val = 2;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DBL_VID:
		val = 3;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DMAC_VID:
		val = 4;
		break;
	default:
		netdev_err(port->dev, "ERROR: Invalid key!\n");
		return -EINVAL;
	}

	lan_rmw(ANA_VCAP_S1_CFG_KEY_IP4_CFG_SET(val),
		ANA_VCAP_S1_CFG_KEY_IP4_CFG,
		port->lan966x, ANA_VCAP_S1_CFG(port->chip_port, lookup));

	port->is1[lookup].key_ip4 = key;
	return 0;
}

static int lan966x_vcap_is1_port_key_ipv4_get(const struct lan966x_port *port,
					      u8 lookup,
					      enum lan966x_vcap_is1_key *key)
{
	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	*key = port->is1[lookup].key_ip4;
	return 0;
}

static int lan966x_vcap_is1_port_key_ipv6_set(struct lan966x_port *port,
					      u8 lookup,
					      enum lan966x_vcap_is1_key key)
{
	u32 val;

	netdev_dbg(port->dev, "lookup %d key %s\n", lookup,
		   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS1, key)->name);

	if (port->lan966x->vcap[LAN966X_VCAP_IS1].num_rules[lookup]) {
		netdev_err(port->dev, "ERROR: IS1 not empty!\n");
		return -EBUSY;
	}

	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	switch (key) {
	case LAN966X_VCAP_IS1_KEY_S1_NORMAL:
		val = 0;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_7TUPLE:
		val = 1;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP4:
		val = 2;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_NORMAL_IP6:
		val = 3;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_5TUPLE_IP6:
		val = 4;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DBL_VID:
		val = 5;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DMAC_VID:
		val = 6;
		break;
	default:
		netdev_err(port->dev, "ERROR: Invalid key!\n");
		return -EINVAL;
	}

	lan_rmw(ANA_VCAP_S1_CFG_KEY_IP6_CFG_SET(val),
		ANA_VCAP_S1_CFG_KEY_IP6_CFG,
		port->lan966x, ANA_VCAP_S1_CFG(port->chip_port, lookup));

	port->is1[lookup].key_ip6 = key;
	return 0;
}

static int lan966x_vcap_is1_port_key_ipv6_get(const struct lan966x_port *port,
					      u8 lookup,
					      enum lan966x_vcap_is1_key *key)
{
	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	*key = port->is1[lookup].key_ip6;
	return 0;
}

static int lan966x_vcap_is1_port_key_other_set(struct lan966x_port *port,
					       u8 lookup,
					       enum lan966x_vcap_is1_key key)
{
	u32 val;

	netdev_dbg(port->dev, "lookup %d key %s\n", lookup,
		   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS1, key)->name);

	if (port->lan966x->vcap[LAN966X_VCAP_IS1].num_rules[lookup]) {
		netdev_err(port->dev, "ERROR: IS1 not empty!\n");
		return -EBUSY;
	}

	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	switch (key) {
	case LAN966X_VCAP_IS1_KEY_S1_NORMAL:
		val = 0;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_7TUPLE:
		val = 1;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DBL_VID:
		val = 2;
		break;
	case LAN966X_VCAP_IS1_KEY_S1_DMAC_VID:
		val = 3;
		break;
	default:
		netdev_err(port->dev, "ERROR: Invalid key!\n");
		return -EINVAL;
	}

	lan_rmw(ANA_VCAP_S1_CFG_KEY_OTHER_CFG_SET(val),
		ANA_VCAP_S1_CFG_KEY_OTHER_CFG,
		port->lan966x, ANA_VCAP_S1_CFG(port->chip_port, lookup));

	port->is1[lookup].key_other = key;
	return 0;
}

static int lan966x_vcap_is1_port_key_other_get(const struct lan966x_port *port,
					       u8 lookup,
					       enum lan966x_vcap_is1_key *key)
{
	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS1) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	*key = port->is1[lookup].key_other;
	return 0;
}

int lan966x_vcap_is1_port_key_set(struct lan966x_port *port,
				  u8 lookup,
				  enum lan966x_vcap_is1_frame_type frame_type,
				  enum lan966x_vcap_is1_key key)
{
	int err;

	switch (frame_type) {
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV4:
		return lan966x_vcap_is1_port_key_ipv4_set(port,
							  lookup,
							  key);
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV6:
		return lan966x_vcap_is1_port_key_ipv6_set(port,
							  lookup,
							  key);
	case LAN966X_VCAP_IS1_FRAME_TYPE_OTHER:
		return lan966x_vcap_is1_port_key_other_set(port,
							   lookup,
							   key);
	case LAN966X_VCAP_IS1_FRAME_TYPE_ALL:
		err = lan966x_vcap_is1_port_key_ipv4_set(port,
							 lookup,
							 key);
		if (err)
			return err;

		err = lan966x_vcap_is1_port_key_ipv6_set(port,
							 lookup,
							 key);
		if (err)
			return err;

		return lan966x_vcap_is1_port_key_other_set(port,
							   lookup,
							   key);
	default:
		return -EINVAL;
	}
}

int lan966x_vcap_is1_port_key_get(const struct lan966x_port *port,
				  u8 lookup,
				  enum lan966x_vcap_is1_frame_type frame_type,
				  enum lan966x_vcap_is1_key *key)
{
	switch (frame_type) {
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV4:
		return lan966x_vcap_is1_port_key_ipv4_get(port,
							  lookup,
							  key);
	case LAN966X_VCAP_IS1_FRAME_TYPE_IPV6:
		return lan966x_vcap_is1_port_key_ipv6_get(port,
							  lookup,
							  key);
	case LAN966X_VCAP_IS1_FRAME_TYPE_OTHER:
	case LAN966X_VCAP_IS1_FRAME_TYPE_ALL:
		return lan966x_vcap_is1_port_key_other_get(port,
							   lookup,
							   key);
	default:
		return -EINVAL;
	}
}

int lan966x_vcap_is2_port_key_ipv6_set(struct lan966x_port *port,
				       u8 lookup,
				       enum lan966x_vcap_is2_key key)
{
	u32 val, cfg;

	netdev_dbg(port->dev, "lookup %d key %s\n", lookup,
		   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS2, key)->name);

	if (port->lan966x->vcap[LAN966X_VCAP_IS2].num_rules[lookup]) {
		netdev_err(port->dev, "ERROR: IS2 not empty!\n");
		return -EBUSY;
	}

	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS2) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	val = lan_rd(port->lan966x, ANA_VCAP_S2_CFG(port->chip_port));
	cfg = ANA_VCAP_S2_CFG_IP6_CFG_GET(val);

	switch (key) {
	case LAN966X_VCAP_IS2_KEY_MAC_ETYPE:
		val = 3;
		break;
	case LAN966X_VCAP_IS2_KEY_IP4_TCP_UDP:
		val = 2;
		break;
	case LAN966X_VCAP_IS2_KEY_IP6_STD:
		val = 1;
		break;
	case LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP:
		val = 0;
		break;
	default:
		netdev_err(port->dev, "ERROR: Invalid key!\n");
		return -EINVAL;
	}

	cfg &= ~(0x3 << (2 * lookup)); /* clear old value */
	cfg |= val << (2 * lookup); /* set new value */

	lan_rmw(ANA_VCAP_S2_CFG_IP6_CFG_SET(cfg),
		ANA_VCAP_S2_CFG_IP6_CFG,
		port->lan966x, ANA_VCAP_S2_CFG(port->chip_port));

	port->is2[lookup].key_ip6 = key;
	return 0;
}

int lan966x_vcap_is2_port_key_ipv6_get(const struct lan966x_port *port,
				       u8 lookup,
				       enum lan966x_vcap_is2_key *key)
{
	if (lookup >= LAN966X_VCAP_NUM_LOOKUPS_IS2) {
		netdev_err(port->dev, "ERROR: Invalid lookup!\n");
		return -EINVAL;
	}

	*key = port->is2[lookup].key_ip6;
	return 0;
}

/*******************************************************************************
 * Utilities used by pack functions
 ******************************************************************************/
void lan966x_vcap_key_set(struct lan966x_vcap_data *data, u32 offset,
			  u32 width, u32 value, u32 mask)
{
	if (width > 32)
		pr_err("illegal width: %u, offset: %u", width, offset);

	/* Avoid 'match-off' by setting entry = value & mask */
	lan966x_set_bits(data->entry, offset, width, value & mask);
	lan966x_set_bits(data->mask, offset, width, mask);
}

void lan966x_vcap_key_bit_set(struct lan966x_vcap_data *data, u32 offset,
			      enum lan966x_vcap_bit val)
{
	lan966x_vcap_key_set(data, offset, 1, val == LAN966X_VCAP_BIT_1 ? 1 : 0,
			     val == LAN966X_VCAP_BIT_ANY ? 0 : 1);
}

void lan966x_vcap_key_bytes_set(struct lan966x_vcap_data *data,
				u32 offset, const u8 *val, const u8 *msk,
				u32 count)
{
	u32 i, j, n = 0, value = 0, mask = 0;

	/* Data wider than 32 bits are split up in chunks of maximum 32 bits.
	 * The 32 LSB of the data are written to the 32 MSB of the TCAM.
	 */
	offset += (count * 8);
	for (i = 0; i < count; i++) {
		j = (count - i - 1);
		value += (val[j] << n);
		mask += (msk[j] << n);
		n += 8;
		if (n == 32 || (i + 1) == count) {
			offset -= n;
			lan966x_vcap_key_set(data, offset, n, value, mask);
			n = 0;
			value = 0;
			mask = 0;
		}
	}
}

void lan966x_vcap_action_set(struct lan966x_vcap_data *data, u32 offset,
			     u32 width, u32 value)
{
	if (width > 32)
		pr_err("illegal width: %u, offset: %u", width, offset);

	lan966x_set_bits(data->action, offset, width, value);
}

void lan966x_vcap_action_bit_set(struct lan966x_vcap_data *data, u32 offset,
				 u32 value)
{
	lan966x_vcap_action_set(data, offset, 1, value ? 1 : 0);
}

/*******************************************************************************
 * debugfs functions
 ******************************************************************************/
/* A local version of seq_printf() that returns the number of chars printed */
#define LAN966X_DBG_LINE_LENGTH 80
static size_t dbg_printf(void *m, const char *fmt, ...)
{
	struct seq_file *f = m;
	size_t cnt = f->count;
	va_list args;

	va_start(args, fmt);
	seq_vprintf(f, fmt, args);
	va_end(args);
	return f->count - cnt;
}

static int lan966x_vcap_find_key(struct seq_file *m,
				 const struct lan966x_vcap_tgs_attrs *ta,
				 struct lan966x_vcap_info *info)
{
	const struct lan966x_vcap_key_attrs *ka;
	u32 type_id;
	int key = 0;

	if (ta == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap key tgs attributes!\n");
		return -1;
	}

	ka = lan966x_vcap_key_attrs_get(info->vcap, key);
	if (ka == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap key attributes!\n");
		return -1;
	}
	while (ka) {
		if ((ka->sw_per_entry == info->key_tg) && /* same size */
		    (ka->type_width == ta->type_width)) { /* and type width */
			if (ka->type_width == 0) { /* no type = 1 key */
				return key;
			} else { /* more types = compare type_id */
				type_id = LAN966X_EXTRACT_BITFIELD(
					info->data.entry[0],
					0, ka->type_width);
				if (type_id == ka->type_id) {
					return key;
				}
			}
		}
		ka = lan966x_vcap_key_attrs_get(info->vcap, ++key);
	}
	return -1;
}

static size_t lan966x_vcap_show_field_val(struct seq_file *m,
					  const u32 *addr,
					  u32 offset, u32 len)
{
	size_t cnt = 0;
	u32 val;
	int i;

	if (len == 0) {
		cnt += dbg_printf(m, "invalid length %u!", len);
	} else if (len <= 32) {
		cnt = dbg_printf(m, "0x%x",
				 lan966x_get_bits(addr, offset, len));
	} else if (len == 48) { /* assume MAC address */
		for (i = 1; i >= 0; i--) {
			val = lan966x_get_bits(addr, offset + (8 * i), 8);
			cnt += dbg_printf(m, "%02x:", val);
		}
		for (i = 5; i >= 2; i--) {
			val = lan966x_get_bits(addr, offset + (8 * i), 8);
			cnt += dbg_printf(m, "%02x%s", val, (i > 2) ? ":" : "");
		}
	} else if ((len % 32) == 0) { /* e.g 64, 112 or 128 */
		for (i = ((len / 32) - 1); i >= 0; i--) {
			val = lan966x_get_bits(addr, offset + (32 * i), 32);
			cnt += dbg_printf(m, "%08x%s", val, (i > 0) ? ":" : "");
		}
	} else {
		cnt += dbg_printf(m, "invalid length %u!", len);
	}

	return cnt;
}

static void lan966x_vcap_show_field(struct seq_file *m,
				    struct lan966x_vcap_info *info,
				    const char *name, u32 offset, u32 len)
{
	if (info->is_action) {
		if (!lan966x_bits_set(info->data.action, offset, len))
			return; /* No bits set in action value */
	} else {
		if (!lan966x_bits_set(info->data.mask, offset, len))
			return; /* No bits set in entry mask */
	}

	if (info->ll)
		info->ll += dbg_printf(m, ", ");

	if (info->ll > LAN966X_DBG_LINE_LENGTH) {
		dbg_printf(m, "\n  ");
		info->ll = 0;
	}

	info->ll += dbg_printf(m, "%s ", name);
	if (info->is_action) {
		info->ll += lan966x_vcap_show_field_val(m, info->data.action,
							offset, len);
	} else {
		info->ll += lan966x_vcap_show_field_val(m, info->data.entry,
							offset, len);
		info->ll += dbg_printf(m, "/");
		info->ll += lan966x_vcap_show_field_val(m, info->data.mask,
							offset, len);
	}
}

static void lan966x_vcap_show_entry(struct seq_file *m,
				    struct lan966x_vcap_info *info)
{
	const struct lan966x_vcap_tgs_attrs *kta;
	const struct lan966x_vcap_key_attrs *ka;
	const struct lan966x_vcap_field_attrs *fa;
	int key, field;

	kta = lan966x_vcap_key_tgs_attrs_get(info->vcap, info->key_tg);
	key = lan966x_vcap_find_key(m, kta, info);
	ka = lan966x_vcap_key_attrs_get(info->vcap, key);
	if (ka == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap key attributes!\n");
		return;
	}

	dbg_printf(m,
		   " key %s, size %s (fields with zero masks are not shown)\n  ",
		   ka->name, kta->name);

	info->ll = 0;
	info->is_action = false;
	field = 0;
	fa = lan966x_vcap_key_field_attrs_get(info->vcap, key, field);
	while (fa) {
		lan966x_vcap_show_field(m, info, fa->name, fa->offset,
					fa->length);

		fa = lan966x_vcap_key_field_attrs_get(info->vcap, key, ++field);
	}
	dbg_printf(m, "\n");
}

static int lan966x_vcap_find_action(struct seq_file *m,
				    const struct lan966x_vcap_tgs_attrs *ta,
				    struct lan966x_vcap_info *info)
{
	const struct lan966x_vcap_action_attrs *aa;
	u32 type_id;
	int action = 0;

	if (ta == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap action tgs attributes!\n");
		return -1;
	}

	aa = lan966x_vcap_action_attrs_get(info->vcap, action);
	if (aa == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap action attributes!\n");
		return -1;
	}
	while (aa) {
		if ((aa->sw_per_action == info->act_tg) && /* same size */
		    (aa->type_width == ta->type_width)) { /* and type width */
			if (aa->type_width == 0) { /* no type = 1 action */
				return action;
			} else { /* more types = compare type_id */
				type_id = LAN966X_EXTRACT_BITFIELD(
					info->data.action[0],
					0, aa->type_width);
				if (type_id == aa->type_id) {
					return action;
				}
			}
		}
		aa = lan966x_vcap_action_attrs_get(info->vcap, ++action);
	}
	return -1;
}

static void lan966x_vcap_show_action(struct seq_file *m,
				     struct lan966x_vcap_info *info)
{
	const struct lan966x_vcap_tgs_attrs *ata;
	const struct lan966x_vcap_action_attrs *aa;
	const struct lan966x_vcap_field_attrs *fa;
	int action, field;

	ata = lan966x_vcap_action_tgs_attrs_get(info->vcap, info->act_tg);
	action = lan966x_vcap_find_action(m, ata, info);
	aa = lan966x_vcap_action_attrs_get(info->vcap, action);
	if (aa == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap action attributes!\n");
		return;
	}

	dbg_printf(m,
		   " action %s, size %s (fields with zero values are not shown)\n  ",
		   aa->name, ata->name);

	info->ll = 0;
	info->is_action = true;
	field = 0;
	fa = lan966x_vcap_action_field_attrs_get(info->vcap, action, field);
	while (fa) {
		lan966x_vcap_show_field(m, info, fa->name, fa->offset,
					fa->length);

		fa = lan966x_vcap_action_field_attrs_get(info->vcap, action,
							 ++field);
	}
	dbg_printf(m, "\n");
}

static int lan966x_vcap_show(struct seq_file *m, enum lan966x_vcap vcap)
{
	const struct lan966x_vcap_attrs *va = lan966x_vcap_attrs_get(vcap);
	struct lan966x *lan966x = m->private;
	struct lan966x_vcap_info info = {};
	u32 row, col, sw_per_entry, val, tgt = va->instance;
	int i, j;

	if (va == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap attributes!\n");
		return -EINVAL;
	}
	if (lan966x == NULL) {
		dbg_printf(m, "ERROR: Unable to get lan966x data!\n");
		return -EINVAL;
	}

	dbg_printf(m, "%-16s: %s\n", "name", va->name);
	dbg_printf(m, "%-16s: %u\n", "instance", tgt);
	dbg_printf(m, "%-16s: %u\n", "rows", va->rows);
	dbg_printf(m, "%-16s: %u\n", "sw_count", va->sw_count);
	dbg_printf(m, "%-16s: %u\n", "sw_width", va->sw_width);
	dbg_printf(m, "%-16s: %u\n", "sticky_width", va->sticky_width);
	dbg_printf(m, "%-16s: %u\n", "act_width", va->act_width);
	dbg_printf(m, "%-16s: %u\n", "default_cnt", va->default_cnt);

	val = lan_rd(lan966x, VCAP_VER(tgt));
	if (val != 1) {
		dbg_printf(m, "ERROR: Invalid version (%u)!\n", val);
		return -EINVAL;
	}

	val = lan_rd(lan966x, VCAP_ENTRY_SWCNT(tgt));
	if (val != va->sw_count) {
		dbg_printf(m, "ERROR: sw_count %u != %u!\n", va->sw_count, val);
		return -EINVAL;
	}

	val = lan_rd(lan966x, VCAP_ENTRY_WIDTH(tgt));
	if (val != va->sw_width) {
		dbg_printf(m, "ERROR: sw_width %u != %u!\n", va->sw_width, val);
		return -EINVAL;
	}

	val = lan_rd(lan966x, VCAP_ACTION_DEF_CNT(tgt));
	if (val != va->default_cnt) {
		dbg_printf(m, "ERROR: default_cnt %u != %u!\n",
			   va->default_cnt, val);
		return -EINVAL;
	}

	val = lan_rd(lan966x, VCAP_ACTION_WIDTH(tgt));
	if (val != va->act_width) {
		dbg_printf(m, "ERROR: act_width %u != %u!\n",
			   va->act_width, val);
		return -EINVAL;
	}

	val = lan_rd(lan966x, VCAP_CNT_WIDTH(tgt));
	if (val != va->sticky_width) {
		dbg_printf(m, "ERROR: sticky_width %u != %u!\n",
			   va->sticky_width, val);
		return -EINVAL;
	}

	dbg_printf(m, "\n");

	info.vcap = vcap;
	for (i = (va->rows + va->default_cnt - 1); i >= 0; i--) {
		if (i >= va->rows) {
			/* Default action */
			continue;
		}

		/* Read each subword until a valid TG is found,
		   then read the whole entry */
		row = va->rows - i - 1;
		for (j = (va->sw_count - 1); j >= 0; j--) {
			info.cmd = LAN966X_VCAP_CMD_READ;
			info.sel = LAN966X_VCAP_SEL_ALL;
			info.addr = i * va->sw_count + j;
			info.key_tg = LAN966X_VCAP_TG_X1;
			info.act_tg = LAN966X_VCAP_TG_X1;
			if (lan966x_vcap_entry_cmd(lan966x, &info) ||
			    info.key_tg == LAN966X_VCAP_TG_NONE ||
			    lan966x_vcap_entry_cmd(lan966x, &info)) {
				continue;
			}

			sw_per_entry = lan966x_vcap_tg_count(info.key_tg);
			col = va->sw_count - j - sw_per_entry;
			dbg_printf(m, "row %u, col %u, addr %u, hits %u:\n",
				   row, col, info.addr, info.cnt);

			lan966x_vcap_show_entry(m, &info);
			lan966x_vcap_show_action(m, &info);
			dbg_printf(m, "\n");
		}
	}
	return 0;
}

static int lan966x_vcap_admin_show(struct seq_file *m, enum lan966x_vcap vcap)
{
	const struct lan966x_vcap_attrs *va = lan966x_vcap_attrs_get(vcap);
	struct lan966x *lan966x = m->private;
	struct lan966x_vcap_rule_entry *e;
	struct lan966x_vcap_admin *a;
	char buf[32];
	u32 addr;
	int i;

	if (va == NULL) {
		dbg_printf(m, "ERROR: Unable to get vcap attributes!\n");
		return -EINVAL;
	}
	if (lan966x == NULL) {
		dbg_printf(m, "ERROR: Unable to get lan966x data!\n");
		return -EINVAL;
	}

	a = &lan966x->vcap[vcap];
	dbg_printf(m, "%-20s: %s\n", "name", va->name);
	dbg_printf(m, "%-20s: %u\n", "last_valid_addr", a->last_valid_addr);
	dbg_printf(m, "%-20s: %u\n", "last_used_addr", a->last_used_addr);
	for (i = 0; i < LAN966X_VCAP_NUM_LOOKUPS_MAX; i++) {
		sprintf(buf, "num rules lookup[%d]", i);
		dbg_printf(m, "%-20s: %u\n", buf, a->num_rules[i]);
	}
	dbg_printf(m, "\n");

	mutex_lock(&a->lock);
	if (list_empty(&a->list)) {
		dbg_printf(m, "No entries in list!\n");
	} else {
		i = 0;
		addr = a->last_valid_addr;
		list_for_each_entry(e, &a->list, list) {
			dbg_printf(m, "%d: addr %u size %u user %d prio 0x%x cookie 0x%lx sort_key 0x%08x\n",
				   i++, addr - e->size + 1, e->size, e->user,
				   e->prio, e->cookie, e->sort_key);
			addr -= e->size;
		}
	}
	mutex_unlock(&a->lock);

	return 0;
}

static int lan966x_vcap_es0_show(struct seq_file *m, void *unused)
{
	return lan966x_vcap_show(m, LAN966X_VCAP_ES0);
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_es0);

static int lan966x_vcap_admin_es0_show(struct seq_file *m, void *unused)
{
	return lan966x_vcap_admin_show(m, LAN966X_VCAP_ES0);
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_admin_es0);

static int lan966x_vcap_is1_show(struct seq_file *m, void *unused)
{
	return lan966x_vcap_show(m, LAN966X_VCAP_IS1);
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_is1);

static int lan966x_vcap_admin_is1_show(struct seq_file *m, void *unused)
{
	return lan966x_vcap_admin_show(m, LAN966X_VCAP_IS1);
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_admin_is1);

static int lan966x_vcap_port_is1_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	struct lan966x_port *port;
	int i, j;

	dbg_printf(m, "Dev, Lookup, Parm: Value\n");
	for (i = 0; i < lan966x->num_phys_ports; i++) {
		port = lan966x->ports[i];
		if (!port)
			continue;
		for (j = 0; j < LAN966X_VCAP_NUM_LOOKUPS_IS1; j++) {
			dbg_printf(m, "%s, %d, smac      : %s\n",
				   port->dev->name, j,
				   port->is1[j].smac ? "true" : "false");
			dbg_printf(m, "%s, %d, dmac_dip  : %s\n",
				   port->dev->name, j,
				   port->is1[j].dmac_dip ? "true" : "false");
			dbg_printf(m, "%s, %d, key_ip6   : %s\n",
				   port->dev->name, j,
				   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS1,
							      port->is1[j].key_ip6)->name);
			dbg_printf(m, "%s, %d, key_ip4   : %s\n",
				   port->dev->name, j,
				   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS1,
							      port->is1[j].key_ip4)->name);
			dbg_printf(m, "%s, %d, key_other : %s\n",
				   port->dev->name, j,
				   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS1,
							      port->is1[j].key_ip6)->name);
		}
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_port_is1);

static int lan966x_vcap_is2_show(struct seq_file *m, void *unused)
{
	return lan966x_vcap_show(m, LAN966X_VCAP_IS2);
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_is2);

static int lan966x_vcap_admin_is2_show(struct seq_file *m, void *unused)
{
	return lan966x_vcap_admin_show(m, LAN966X_VCAP_IS2);
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_admin_is2);

static int lan966x_vcap_port_is2_show(struct seq_file *m, void *unused)
{
	struct lan966x *lan966x = m->private;
	struct lan966x_port *port;
	int i, j;

	dbg_printf(m, "Dev, Lookup, Parm: Value\n");
	for (i = 0; i < lan966x->num_phys_ports; i++) {
		port = lan966x->ports[i];
		if (!port)
			continue;
		for (j = 0; j < LAN966X_VCAP_NUM_LOOKUPS_IS2; j++) {
			dbg_printf(m, "%s, %d, key_ip6   : %s\n",
				   port->dev->name, j,
				   lan966x_vcap_key_attrs_get(LAN966X_VCAP_IS2,
							      port->is2[j].key_ip6)->name);
		}
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan966x_vcap_port_is2);

/*******************************************************************************
 * Initialization
 ******************************************************************************/
static int lan966x_vcap_init_vcap(struct lan966x *lan966x,
				  enum lan966x_vcap vcap)
{
	const struct lan966x_vcap_attrs *va = lan966x_vcap_attrs_get(vcap);
	struct lan966x_vcap_info info = {0};
	struct lan966x_vcap_admin *a;
	int err;

	if (va == NULL)
		return -EINVAL;

	/* Initialize admin */
	a = &lan966x->vcap[vcap];
	INIT_LIST_HEAD(&a->list);
	mutex_init(&a->lock);
	a->last_used_addr = va->rows * va->sw_count; /* = nothing used */
	a->last_valid_addr = a->last_used_addr - 1;

	/* Initialize entries */
	info.vcap = vcap;
	info.cmd = LAN966X_VCAP_CMD_INIT;
	info.sel = LAN966X_VCAP_SEL_ENTRY;
	info.mv_size = (va->rows * va->sw_count);
	err = lan966x_vcap_cmd(lan966x, &info);
	if (err)
		return err;

	/* Initialize actions and counters */
	info.sel = (LAN966X_VCAP_SEL_ACTION | LAN966X_VCAP_SEL_COUNTER);
	info.mv_size = ((va->rows + va->default_cnt) * va->sw_count);
	err = lan966x_vcap_cmd(lan966x, &info);
	if (err)
		return err;

	/* Enable core */
	lan_wr(VCAP_CORE_MAP_CORE_MAP_SET(1),
	       lan966x, VCAP_CORE_MAP(va->instance));

	return 0;
}

void lan966x_vcap_init(struct lan966x *lan966x)
{
	int i, err;

/* Sanity check of subword sizes versus allocation */
#if LAN966X_VCAP_MAX_ENTRY_WIDTH < \
	LAN966X_BITS_TO_U32(LAN966X_VCAP_MAX_SW_WIDTH * 4)
#error "Increase LAN966X_VCAP_MAX_ENTRY_WIDTH!"
#endif

#if LAN966X_VCAP_MAX_ACTION_WIDTH < \
	LAN966X_BITS_TO_U32(LAN966X_VCAP_MAX_ACT_WIDTH * 4)
#error "Increase LAN966X_VCAP_MAX_ACTION_WIDTH!"
#endif

#if LAN966X_VCAP_MAX_COUNTER_WIDTH < \
	LAN966X_BITS_TO_U32(LAN966X_VCAP_MAX_STICKY_WIDTH * 4)
#error "Increase LAN966X_VCAP_MAX_COUNTER_WIDTH!"
#endif

	debugfs_create_file("vcap_show_es0", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_es0_fops);

	debugfs_create_file("vcap_show_admin_es0", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_admin_es0_fops);

	debugfs_create_file("vcap_show_is1", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_is1_fops);

	debugfs_create_file("vcap_show_admin_is1", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_admin_is1_fops);

	debugfs_create_file("vcap_show_port_is1", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_port_is1_fops);

	debugfs_create_file("vcap_show_is2", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_is2_fops);

	debugfs_create_file("vcap_show_admin_is2", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_admin_is2_fops);

	debugfs_create_file("vcap_show_port_is2", 0444, lan966x->debugfs_root,
			    lan966x, &lan966x_vcap_port_is2_fops);

	/* Initialize all VCAPs */
	for (i = 0; i < LAN966X_VCAP_LAST; i++) {
		err = lan966x_vcap_init_vcap(lan966x, i);
		if (err)
			dev_err(lan966x->dev, "ERROR initialize VCAP %d!\n", i);
	}
}

static void lan966x_vcap_uninit_vcap(struct lan966x *lan966x,
				     enum lan966x_vcap vcap)
{
	struct lan966x_vcap_admin *a = &lan966x->vcap[vcap];
	struct lan966x_vcap_rule_entry *e, *tmp;

	/* Delete and free entries */
	if (!list_empty(&a->list)) {
		list_for_each_entry_safe(e, tmp, &a->list, list) {
			list_del(&e->list);
			kfree(e);
		}
	}

	mutex_destroy(&a->lock);
}

void lan966x_vcap_uninit(struct lan966x *lan966x)
{
	int i;

	/* debugfs is removed in lan966x_main.c by debugfs_remove_recursive().
	 * memory is allocated with devm_kzalloc and need not to be freed here.
	 */
	for (i = 0; i < LAN966X_VCAP_LAST; i++)
		lan966x_vcap_uninit_vcap(lan966x, i);
}

void lan966x_vcap_port_enable(struct lan966x *lan966x,
			      struct lan966x_port *port)
{
	u8 lookup;

	/* Enable and initialize IS1 */
	lan_wr(ANA_VCAP_CFG_S1_ENA_SET(1),
	       lan966x, ANA_VCAP_CFG(port->chip_port));

	for (lookup = 0; lookup < LAN966X_VCAP_NUM_LOOKUPS_IS1; lookup++) {
		lan966x_vcap_is1_port_key_set(port,
					      lookup,
					      LAN966X_VCAP_IS1_FRAME_TYPE_ALL,
					      LAN966X_VCAP_IS1_KEY_S1_7TUPLE);
	}

	/* Enable and initialize IS2
	 * Note that ISDX_ENA is cleared here for both lookups.
	 */
	lan_wr(ANA_VCAP_S2_CFG_ENA_SET(1),
	       lan966x, ANA_VCAP_S2_CFG(port->chip_port));

	for (lookup = 0; lookup < LAN966X_VCAP_NUM_LOOKUPS_IS2; lookup++) {
		lan966x_vcap_is2_port_key_ipv6_set(port,
						   lookup,
						   LAN966X_VCAP_IS2_KEY_IP6_TCP_UDP);
	}

	/* Enable ES0 */
	lan_rmw(REW_PORT_CFG_ES0_EN_SET(1),
		REW_PORT_CFG_ES0_EN,
		lan966x, REW_PORT_CFG(port->chip_port));
}
