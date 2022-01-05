/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN966X_CFM_H_
#define _LAN966X_CFM_H_

#include <net/switchdev.h>

#include "lan966x_main.h"

void lan966x_cfm_init(struct lan966x *lan966x);
void lan966x_cfm_uninit(struct lan966x *lan966x);
void lan966x_cfm_event(struct lan966x *lan966x);

int lan966x_handle_cfm_mep_add(struct net_device *dev,
			       struct notifier_block *nb,
			       const struct switchdev_obj_cfm_mep *config);
int lan966x_handle_cfm_cc_peer_mep_add(struct net_device *dev,
				       struct notifier_block *nb,
				       const struct switchdev_obj_cfm_cc_peer_mep *config);
int lan966x_handle_cfm_mep_config_add(struct net_device *dev,
				      struct notifier_block *nb,
				      const struct switchdev_obj_cfm_mep_config_set *config);
int lan966x_handle_cfm_cc_config_add(struct net_device *dev,
				     struct notifier_block *nb,
				     const struct switchdev_obj_cfm_cc_config_set *config);
int lan966x_handle_cfm_cc_rdi_add(struct net_device *dev,
				  struct notifier_block *nb,
				  const struct switchdev_obj_cfm_cc_rdi_set *config);
int lan966x_handle_cfm_cc_ccm_tx_add(struct net_device *dev,
				     struct notifier_block *nb,
				     const struct switchdev_obj_cfm_cc_ccm_tx *config);

int lan966x_handle_cfm_mep_del(struct net_device *dev,
			   struct notifier_block *nb,
			   const struct switchdev_obj_cfm_mep *config);
int lan966x_handle_cfm_cc_peer_mep_del(struct net_device *dev,
			   struct notifier_block *nb,
			   const struct switchdev_obj_cfm_cc_peer_mep *config);

int lan966x_handle_cfm_mep_status_get(struct net_device *dev,
				      struct switchdev_cfm_mep_status *status);
int lan966x_handle_cfm_cc_peer_status_get(struct net_device *dev,
					  struct switchdev_cfm_cc_peer_status *status);

int lan966x_handle_cfm_mip_add(struct net_device *dev,
			       struct notifier_block *nb,
			       const struct switchdev_obj_cfm_mip *config);
int lan966x_handle_cfm_mip_config_add(struct net_device *dev,
				      struct notifier_block *nb,
				      const struct switchdev_obj_cfm_mip_config_set *config);
int lan966x_handle_cfm_mip_del(struct net_device *dev,
			       struct notifier_block *nb,
			       const struct switchdev_obj_cfm_mip *config);

int lan966x_handle_cfm_interrupt(struct lan966x *lan966x);
#endif
