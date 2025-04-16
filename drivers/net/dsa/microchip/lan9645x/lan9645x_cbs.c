// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

int lan9645x_cbs_add(struct lan9645x *lan9645x, int port,
		     struct tc_cbs_qopt_offload *qopt)
{
	u32 cir, cbs;
	u8 se_idx;

	se_idx = 0 + port * NUM_PRIO_QUEUES + qopt->queue;

	dev_dbg(lan9645x->dev,
		"port=%d se_idx=%u queue=%u idleslope=%d sendslope=%d locredit=%d hicredit=%d",
		port, se_idx, qopt->queue, qopt->idleslope, qopt->sendslope,
		qopt->locredit, qopt->hicredit);

	cir = qopt->idleslope;
	cbs = (qopt->idleslope - qopt->sendslope) *
		(qopt->hicredit - qopt->locredit) / -qopt->sendslope;

	/* Rate unit is 100 kbps */
	cir = DIV_ROUND_UP(cir, 100);
	/* Avoid using zero rate */
	cir = cir ?: 1;
	/* Burst unit is 4kB */
	cbs = DIV_ROUND_UP(cbs, 4096);
	/* Avoid using zero burst */
	cbs = cbs ?: 1;

	if (!FIELD_FIT(QSYS_CIR_CFG_CIR_RATE, cir) ||
	    !FIELD_FIT(QSYS_CIR_CFG_CIR_BURST, cbs))
		return -EINVAL;

	lan_rmw(QSYS_SE_CFG_SE_AVB_ENA_SET(1) |
		QSYS_SE_CFG_SE_FRM_MODE_SET(1),
		QSYS_SE_CFG_SE_AVB_ENA |
		QSYS_SE_CFG_SE_FRM_MODE,
		lan9645x, QSYS_SE_CFG(se_idx));

	lan_wr(QSYS_CIR_CFG_CIR_RATE_SET(cir) |
	       QSYS_CIR_CFG_CIR_BURST_SET(cbs),
	       lan9645x, QSYS_CIR_CFG(se_idx));

	return 0;
}

int lan9645x_cbs_del(struct lan9645x *lan9645x, int port,
		     struct tc_cbs_qopt_offload *qopt)
{
	u8 se_idx;

	se_idx = port * NUM_PRIO_QUEUES + qopt->queue;

	dev_dbg(lan9645x->dev, "port=%d se_idx=%u queue=%u", port, se_idx,
		qopt->queue);

	lan_rmw(QSYS_SE_CFG_SE_AVB_ENA_SET(1) |
		QSYS_SE_CFG_SE_FRM_MODE_SET(0),
		QSYS_SE_CFG_SE_AVB_ENA |
		QSYS_SE_CFG_SE_FRM_MODE, lan9645x,
		QSYS_SE_CFG(se_idx));

	lan_wr(QSYS_CIR_CFG_CIR_RATE_SET(0) |
	       QSYS_CIR_CFG_CIR_BURST_SET(0),
	       lan9645x, QSYS_CIR_CFG(se_idx));

	return 0;
}
