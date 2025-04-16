// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

int lan9645x_mqprio_set(struct lan9645x *lan9645x, int port,
			struct tc_mqprio_qopt_offload *mqprio)
{
	struct net_device *ndev = lan9645x_chipport_to_ndev(lan9645x, port);
	struct tc_mqprio_qopt *qopt = &mqprio->qopt;
	int num_tc = qopt->num_tc;
	int tc, err;

	dev_dbg(lan9645x->dev, "mqprio port=%d num_tc=%d hw=%u", port, num_tc,
		qopt->hw);

	if (!num_tc) {
		netdev_reset_tc(ndev);
		return 0;
	}

	if (num_tc != NUM_PRIO_QUEUES) {
		netdev_err(ndev, "Only %d traffic classes supported\n",
			   NUM_PRIO_QUEUES);
		return -EINVAL;
	}

	err = netdev_set_num_tc(ndev, num_tc);
	if (err)
		goto err_reset_tc;

	for (tc = 0; tc < num_tc; tc++) {
		err = netdev_set_tc_queue(ndev, tc, 1, tc);
		if (err)
			goto err_reset_tc;
	}

	return 0;

err_reset_tc:
	netdev_reset_tc(ndev);
	return err;
}
