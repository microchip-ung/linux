/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN966X_PTP_H_
#define _LAN966X_PTP_H_

#include "lan966x_main.h"

#define TOD_ACC_PIN		0x5

#if defined(SUNRISE) || defined(ASIC)
#define TOD_INPUT		0x1
#else
#define TOD_INPUT		0x0
#endif

#define ADJ_UNITS_PR_NS		10
#define PSEC_PER_SEC		1000000000000LL

enum {
	PTP_PIN_ACTION_IDLE = 0,
	PTP_PIN_ACTION_LOAD,
	PTP_PIN_ACTION_SAVE,
	PTP_PIN_ACTION_CLOCK,
	PTP_PIN_ACTION_DELTA,
	PTP_PIN_ACTION_TOD
};

int lan966x_timestamp_init(struct lan966x *lan966x);
void lan966x_timestamp_deinit(struct lan966x *lan966x);

int lan966x_ptp_gettime64(struct ptp_clock_info *ptp, struct timespec64 *ts);
void lan966x_get_hwtimestamp(struct lan966x *lan966x, struct timespec64 *ts,
			     u32 nsec);

#endif /* _LAN966X_PTP_H_ */
