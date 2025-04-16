// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

#define DWRR_COST_BIT_WIDTH	BIT(5)

static u32 lan9645x_ets_hw_cost(u32 w_min, u32 weight)
{
	u32 res;

	/* Round half up: Multiply with 16 before division,
	 * add 8 and divide result with 16 again
	 */
	res = (((DWRR_COST_BIT_WIDTH << 4) * w_min / weight) + 8) >> 4;
	return max_t(u32, 1, res) - 1;
}

int lan9645x_ets_del(struct lan9645x *lan9645x, int port,
		     struct tc_ets_qopt_offload *qopt)
{
	u32 se_idx;
	int i;

	se_idx = LAN9645X_PSCHD_IDX(port);

	for (i = 0; i < NUM_PRIO_QUEUES; ++i)
		lan_wr(0, lan9645x, QSYS_SE_DWRR_CFG(se_idx, i));

	lan_rmw(QSYS_SE_CFG_SE_DWRR_CNT_SET(0) |
		QSYS_SE_CFG_SE_RR_ENA_SET(0),
		QSYS_SE_CFG_SE_DWRR_CNT |
		QSYS_SE_CFG_SE_RR_ENA, lan9645x,
		QSYS_SE_CFG(se_idx));

	return 0;
}

int lan9645x_ets_add(struct lan9645x *lan9645x, int port,
		     struct tc_ets_qopt_offload *qopt)
{
	struct tc_ets_qopt_offload_replace_params *params;
	u32 w_min = 100;
	u8 count = 0;
	u32 se_idx;
	u8 i;

	/* Check the input */
	if (qopt->parent != TC_H_ROOT)
		return -EINVAL;

	params = &qopt->replace_params;
	if (params->bands != NUM_PRIO_QUEUES)
		return -EINVAL;

	for (i = 0; i < params->bands; ++i) {
		/* In the switch the DWRR is always on the lowest consecutive
		 * priorities. Due to this, the first priority must map to the
		 * first DWRR band.
		 */
		if (params->priomap[i] != (7 - i))
			return -EINVAL;

		if (params->quanta[i] && params->weights[i] == 0)
			return -EINVAL;
	}

	se_idx = LAN9645X_PSCHD_IDX(port);

	dev_dbg(lan9645x->dev, "port=%d se_idx=%u", port, se_idx);

	/* Find minimum weight */
	for (i = 0; i < params->bands; ++i) {
		if (params->quanta[i] == 0)
			continue;

		w_min = min(w_min, params->weights[i]);
	}

	for (i = 0; i < params->bands; ++i) {
		if (params->quanta[i] == 0)
			continue;

		++count;

		lan_wr(lan9645x_ets_hw_cost(w_min, params->weights[i]),
		       lan9645x, QSYS_SE_DWRR_CFG(se_idx, 7 - i));
	}

	lan_rmw(QSYS_SE_CFG_SE_DWRR_CNT_SET(count) |
		QSYS_SE_CFG_SE_RR_ENA_SET(0),
		QSYS_SE_CFG_SE_DWRR_CNT |
		QSYS_SE_CFG_SE_RR_ENA,
		lan9645x, QSYS_SE_CFG(se_idx));

	return 0;
}
