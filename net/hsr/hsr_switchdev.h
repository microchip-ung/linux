/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HSR_SWITCHDEV_H
#define __HSR_SWITCHDEV_H

#include "hsr_main.h"

#ifdef CONFIG_NET_SWITCHDEV
int hsr_node_switchdev_add(struct hsr_priv *hsr, struct hsr_node *node);
int hsr_node_switchdev_del(struct hsr_priv *hsr, struct hsr_node *node);
#else
static int hsr_node_switchdev_add(struct hsr_priv *hsr, struct hsr_node *node)
{
	return 0;
}

static int hsr_node_switchdev_del(struct hsr_priv *hsr, struct hsr_node *node)
{
	return 0;
}
#endif /* CONFIG_NET_SWITCHDEV */

#endif /* __HSR_SWITCHDEV_H */
