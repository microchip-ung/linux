/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN966X_VCAP_TYPES_H_
#define _LAN966X_VCAP_TYPES_H_

#include <linux/types.h>

enum lan966x_vcap_bit {
	LAN966X_VCAP_BIT_ANY,
	LAN966X_VCAP_BIT_0,
	LAN966X_VCAP_BIT_1
};

struct lan966x_vcap_u8 {
	u8 value;
	u8 mask;
};

struct lan966x_vcap_u16 {
	u16 value;
	u16 mask;
};

struct lan966x_vcap_u32 {
	u32 value;
	u32 mask;
};

struct lan966x_vcap_u48 {
	u8 value[6];
	u8 mask[6];
};

struct lan966x_vcap_u56 {
	u8 value[7];
	u8 mask[7];
};

struct lan966x_vcap_u64 {
	u8 value[8];
	u8 mask[8];
};

struct lan966x_vcap_u112 {
	u8 value[14];
	u8 mask[14];
};

struct lan966x_vcap_u128 {
	u8 value[16];
	u8 mask[16];
};

#define LAN966X_VCAP_MAX_ENTRY_WIDTH   12 /* Max entry width in 32bit words */
#define LAN966X_VCAP_MAX_ACTION_WIDTH  16 /* Max action width in 32bit words */
#define LAN966X_VCAP_MAX_COUNTER_WIDTH  4 /* Max counter width in 32bit words */

struct lan966x_vcap_data {
	u32 entry[LAN966X_VCAP_MAX_ENTRY_WIDTH];     /* ENTRY_DAT */
	u32 mask[LAN966X_VCAP_MAX_ENTRY_WIDTH];      /* MASK_DAT */
	u32 action[LAN966X_VCAP_MAX_ACTION_WIDTH];   /* ACTION_DAT */
	u32 counter[LAN966X_VCAP_MAX_COUNTER_WIDTH]; /* CNT_DAT */
	u32 tg;                                      /* TG_DAT */
};

#define LAN966X_VCAP_INCLUDE_FIELD_ATTRS

#endif /* _LAN966X_VCAP_TYPES_H_ */
