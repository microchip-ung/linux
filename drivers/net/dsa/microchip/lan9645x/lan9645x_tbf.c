// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

int lan9645x_tbf_add(struct lan9645x *lan9645x, int port,
		     struct tc_tbf_qopt_offload *qopt)
{
	bool root = qopt->parent == TC_H_ROOT;
	u32 queue = 0;
	u32 cir, cbs;
	u32 se_idx;

	if (!root) {
		queue = TC_H_MIN(qopt->parent) - 1;
		if (queue >= NUM_PRIO_QUEUES)
			return -EOPNOTSUPP;
		se_idx = LAN9645X_QSCHD_IDX(port, queue);
	} else {
		se_idx = LAN9645X_PSCHD_IDX(port);
	}

	dev_dbg(lan9645x->dev, "port=%d queue=%u se_idx=%u", port, queue,
		se_idx);

	cir = div_u64(qopt->replace_params.rate.rate_bytes_ps, 1000) * 8;
	cbs = qopt->replace_params.max_size;

	/* Rate unit is 100 kbps */
	cir = DIV_ROUND_UP(cir, 100);
	/* Avoid using zero rate */
	cir = cir ?: 1;
	/* Burst unit is 4kB */
	cbs = DIV_ROUND_UP(cbs, 4096);
	/* Avoid using zero burst */
	cbs = cbs ?: 1;

	/* Check that actually the result can be written */
	if (!FIELD_FIT(QSYS_CIR_CFG_CIR_RATE, cir) ||
	    !FIELD_FIT(QSYS_CIR_CFG_CIR_BURST, cbs))
		return -EINVAL;

	lan_rmw(QSYS_SE_CFG_SE_AVB_ENA_SET(0) |
		QSYS_SE_CFG_SE_FRM_MODE_SET(1),
		QSYS_SE_CFG_SE_AVB_ENA |
		QSYS_SE_CFG_SE_FRM_MODE, lan9645x,
		QSYS_SE_CFG(se_idx));

	lan_wr(QSYS_CIR_CFG_CIR_RATE_SET(cir) |
	       QSYS_CIR_CFG_CIR_BURST_SET(cbs),
	       lan9645x, QSYS_CIR_CFG(se_idx));

	return 0;
}

int lan9645x_tbf_del(struct lan9645x *lan9645x, int port,
		     struct tc_tbf_qopt_offload *qopt)
{
	bool root = qopt->parent == TC_H_ROOT;
	u32 queue = 0;
	u32 se_idx;

	if (!root) {
		queue = TC_H_MIN(qopt->parent) - 1;
		if (queue >= NUM_PRIO_QUEUES)
			return -EOPNOTSUPP;
		se_idx = LAN9645X_QSCHD_IDX(port, queue);
	} else {
		se_idx = LAN9645X_PSCHD_IDX(port);
	}

	dev_dbg(lan9645x->dev, "port=%d queue=%u se_idx=%u", port, queue,
		se_idx);

	lan_rmw(QSYS_SE_CFG_SE_AVB_ENA_SET(0) |
		QSYS_SE_CFG_SE_FRM_MODE_SET(0),
		QSYS_SE_CFG_SE_AVB_ENA |
		QSYS_SE_CFG_SE_FRM_MODE, lan9645x,
		QSYS_SE_CFG(se_idx));

	lan_wr(QSYS_CIR_CFG_CIR_RATE_SET(0) |
	       QSYS_CIR_CFG_CIR_BURST_SET(0),
	       lan9645x, QSYS_CIR_CFG(se_idx));

	return 0;
}
