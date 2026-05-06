// SPDX-License-Identifier: GPL-2.0+
/* Microchip MRP API
 *
 * Copyright (c) 2024 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/export.h>
#include <linux/if_bridge.h>

#include "mrp_api.h"

#define MRP_LOC_PERIOD_CNT	7

static void mrp_alloc_loc_period(struct mrp_control *mrp_ctrl,
				 struct mrp_instance *mrp_inst)
{
	u8 i;

	/* Start from index 5, because the index 0-4 is used for CCM frames for
	 * period 3.3ms - 10ms - 100ms - 1s - 10s.
	 * This gives 5 timer for MRP that is also the needed max for
	 * two Interconnected MRPs (four timers) and one Ring MRP (one timer)
	 */
	for (i = 5; i < MRP_LOC_PERIOD_CNT; ++i) {
		if (BIT(i) & mrp_ctrl->loc_period_mask)
			continue;

		/* In MRP the LOC_PERIOD 0 means that there is no LOC period in
		 * used therefore add 1 to use the index 0 of the LOC period from
		 * MEP
		 */
		mrp_inst->ring_loc_idx = i + 1;
		mrp_inst->in_loc_idx = mrp_inst->ring_loc_idx + 1;
		break;
	}
}

static struct mrp_port *mrp_find_port(struct mrp_instance *mrp_inst,
				      enum br_mrp_port_role_type type)
{
	for (int i = 0; i < MRP_MAX_PORTS; ++i) {
		if (!mrp_inst->ports[i])
			continue;

		if (mrp_inst->ports[i]->role == type)
			return mrp_inst->ports[i];
	}

	return NULL;
}

static void mrp_ring_loc_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct mrp_instance *mrp_inst = container_of(del_work,
						     struct mrp_instance,
						     ring_loc_work);
	struct mrp_port *mrp_port;

	rtnl_lock();
	rcu_read_lock();
	mrp_port = mrp_find_port(mrp_inst, BR_MRP_PORT_ROLE_PRIMARY);
	if (mrp_port->ring_intr_status == MRP_INTERRUPT_STATUS_OPEN) {
		br_mrp_ring_port_open(mrp_port->dev, true);
		mrp_port->ring_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	if (mrp_port->ring_intr_status == MRP_INTERRUPT_STATUS_CLOSED) {
		br_mrp_ring_port_open(mrp_port->dev, false);
		mrp_port->ring_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	mrp_port = mrp_find_port(mrp_inst, BR_MRP_PORT_ROLE_SECONDARY);
	if (mrp_port->ring_intr_status == MRP_INTERRUPT_STATUS_OPEN) {
		br_mrp_ring_port_open(mrp_port->dev, true);
		mrp_port->ring_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	if (mrp_port->ring_intr_status == MRP_INTERRUPT_STATUS_CLOSED) {
		br_mrp_ring_port_open(mrp_port->dev, false);
		mrp_port->ring_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}
	rcu_read_unlock();
	rtnl_unlock();
}

static void mrp_in_loc_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct mrp_instance *mrp_inst = container_of(del_work,
						     struct mrp_instance,
						     in_loc_work);
	struct mrp_port *mrp_port;

	rtnl_lock();
	rcu_read_lock();
	mrp_port = mrp_find_port(mrp_inst, BR_MRP_PORT_ROLE_PRIMARY);
	if (mrp_port->in_intr_status == MRP_INTERRUPT_STATUS_OPEN) {
		br_mrp_in_port_open(mrp_port->dev, true);
		mrp_port->in_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	if (mrp_port->in_intr_status == MRP_INTERRUPT_STATUS_CLOSED) {
		br_mrp_in_port_open(mrp_port->dev, false);
		mrp_port->in_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	mrp_port = mrp_find_port(mrp_inst, BR_MRP_PORT_ROLE_SECONDARY);
	if (mrp_port->in_intr_status == MRP_INTERRUPT_STATUS_OPEN) {
		br_mrp_in_port_open(mrp_port->dev, true);
		mrp_port->in_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	if (mrp_port->in_intr_status == MRP_INTERRUPT_STATUS_CLOSED) {
		br_mrp_in_port_open(mrp_port->dev, false);
		mrp_port->in_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	mrp_port = mrp_find_port(mrp_inst, BR_MRP_PORT_ROLE_INTER);
	if (mrp_port->in_intr_status == MRP_INTERRUPT_STATUS_OPEN) {
		br_mrp_in_port_open(mrp_port->dev, true);
		mrp_port->in_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}

	if (mrp_port->in_intr_status == MRP_INTERRUPT_STATUS_CLOSED) {
		br_mrp_in_port_open(mrp_port->dev, false);
		mrp_port->in_intr_status = MRP_INTERRUPT_STATUS_NONE;
	}
	rcu_read_unlock();
	rtnl_unlock();
}

static enum mrp_interrupt_status mrp_port_get_ring_interrupt_status(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_get_ring_interrupt_status(mrp_port);
}

static enum mrp_interrupt_status mrp_port_get_in_interrupt_status(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_get_in_interrupt_status(mrp_port);
}

static int mrp_port_alloc_ring_test(struct mrp_port *mrp_port)
{
	struct afi_slow_inj_alloc_cfg cfg;
	struct mrp_control *mrp_ctrl;
	struct sk_buff *skb;
	int ret;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;

	ret = mrp_ctrl->ops->mrp_port_afi_cfg(mrp_port, &cfg);
	if (ret)
		return ret;

	ret = afi_slow_inj_alloc(mrp_ctrl->afi_ctrl,
				 &cfg,
				 &mrp_port->afi_ring_test_id);
	if (ret)
		return ret;

	skb = br_mrp_alloc_test(mrp_port->dev, mrp_port->role);
	if (!skb)
		return -ENOMEM;

	ret = mrp_ctrl->ops->mrp_port_hijack_test(mrp_port, skb);
	if (ret) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	return afi_slow_inj_frm_hijack(mrp_ctrl->afi_ctrl,
				       mrp_port->afi_ring_test_id);
}

static int mrp_port_free_ring_test(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return afi_slow_inj_free(mrp_ctrl->afi_ctrl,
				 mrp_port->afi_ring_test_id);
}

static int __mrp_port_start_ring_test(struct mrp_port *mrp_port,
				      u32 interval,
				      u32 max)
{
	struct afi_slow_inj_start_cfg cfg;
	struct mrp_control *mrp_ctrl;
	int ret;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;

	if (afi_slow_inj_started(mrp_ctrl->afi_ctrl,
				 mrp_port->afi_ring_test_id))
		return 0;

	ret = mrp_ctrl->ops->mrp_port_disable_ring_interrupt(mrp_port);
	if (ret)
		return ret;

	cfg.fph = (u64)3600 * (u64)1000 * (u64)1000 / interval;

	/* Start injecting the frames on both ports */
	/* There are cases when SW just changes the interval of the test frames
	 * without stopping the injection. In this case for the AFI to use the
	 * new interval it needs first to stop and then to start again
	 */
	afi_slow_inj_stop(mrp_ctrl->afi_ctrl, mrp_port->afi_ring_test_id);
	afi_slow_inj_start(mrp_ctrl->afi_ctrl,
			   mrp_port->afi_ring_test_id,
			   &cfg);

	mrp_port->ring_interval = interval;

	ret = mrp_ctrl->ops->mrp_port_process_ring_test(mrp_port, true);
	if (ret)
		return ret;

	return mrp_ctrl->ops->mrp_port_enable_ring_interrupt(mrp_port, max);
}

static int __mrp_port_stop_ring_test(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return afi_slow_inj_stop(mrp_ctrl->afi_ctrl,
				 mrp_port->afi_ring_test_id);
}

static int mrp_port_terminate_ring_test(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_terminate_ring_test(mrp_port);
}

static int mrp_port_redirect_ring_test(struct mrp_port *mrp_port, bool redirect)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_redirect_ring_test(mrp_port, redirect);
}

static int mrp_port_forward_ring_test(struct mrp_port *mrp_port,
				      struct mrp_port *mrp_partner_port,
				      bool forward)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_forward_ring_test(mrp_port,
							 mrp_partner_port,
							 forward);
}

static int mrp_port_rewrite_ring_test(struct mrp_port *mrp_port, bool rewrite)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_rewrite_ring_test(mrp_port, rewrite);
}

static int mrp_port_process_ring_test(struct mrp_port *mrp_port, bool process)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_process_ring_test(mrp_port, process);
}

static int mrp_port_alloc_in_test(struct mrp_port *mrp_port)
{
	struct afi_slow_inj_alloc_cfg cfg;
	struct mrp_control *mrp_ctrl;
	struct sk_buff *skb;
	int ret;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;

	ret = mrp_ctrl->ops->mrp_port_afi_cfg(mrp_port, &cfg);
	if (ret)
		return ret;

	ret = afi_slow_inj_alloc(mrp_ctrl->afi_ctrl,
				 &cfg,
				 &mrp_port->afi_in_test_id);
	if (ret)
		return ret;

	skb = br_mrp_alloc_in_test(mrp_port->dev, mrp_port->role);
	if (!skb)
		return -ENOMEM;

	ret = mrp_ctrl->ops->mrp_port_hijack_test(mrp_port, skb);
	if (ret) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	return afi_slow_inj_frm_hijack(mrp_ctrl->afi_ctrl,
				       mrp_port->afi_in_test_id);
}

static int mrp_port_free_in_test(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return afi_slow_inj_free(mrp_ctrl->afi_ctrl,
				 mrp_port->afi_in_test_id);
}

static int __mrp_port_start_in_test(struct mrp_port *mrp_port,
				    u32 interval,
				    u32 max)
{
	struct afi_slow_inj_start_cfg cfg;
	struct mrp_control *mrp_ctrl;
	int ret;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;

	if (afi_slow_inj_started(mrp_ctrl->afi_ctrl,
				 mrp_port->afi_in_test_id))
		return 0;

	ret = mrp_ctrl->ops->mrp_port_disable_in_interrupt(mrp_port);
	if (ret)
		return ret;

	cfg.fph = (u64)3600 * (u64)1000 * (u64)1000 / interval;

	/* Start injecting the frames on both ports */
	/* There are cases when SW just changes the interval of the test frames
	 * without stopping the injection. In this case for the AFI to use the
	 * new interval it needs first to stop and then to start again
	 */
	afi_slow_inj_stop(mrp_ctrl->afi_ctrl, mrp_port->afi_in_test_id);
	afi_slow_inj_start(mrp_ctrl->afi_ctrl,
			   mrp_port->afi_in_test_id,
			   &cfg);

	mrp_port->in_interval = interval;

	ret = mrp_ctrl->ops->mrp_port_process_in_test(mrp_port, true);
	if (ret)
		return ret;

	return mrp_ctrl->ops->mrp_port_enable_in_interrupt(mrp_port, max);
}

static int __mrp_port_stop_in_test(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return afi_slow_inj_stop(mrp_ctrl->afi_ctrl,
				 mrp_port->afi_in_test_id);
}

static int mrp_port_terminate_in_test(struct mrp_port *mrp_port)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_terminate_in_test(mrp_port);
}

static int mrp_port_forward_rem_in_test(struct mrp_port *mrp_port,
					struct mrp_port *mrp_partner_port,
					bool forward)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_forward_rem_in_test(mrp_port,
							   mrp_partner_port,
							   forward);
}

static int mrp_port_forward_in_test(struct mrp_port *mrp_port,
				    struct mrp_port *mrp_partner_port_1,
				    struct mrp_port *mrp_partner_port_2,
				    bool forward)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_forward_in_test(mrp_port,
						       mrp_partner_port_1,
						       mrp_partner_port_2,
						       forward);
}

static int mrp_port_rewrite_in_test(struct mrp_port *mrp_port, bool rewrite)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_rewrite_in_test(mrp_port, rewrite);
}

static int mrp_port_process_in_test(struct mrp_port *mrp_port, bool process)
{
	struct mrp_control *mrp_ctrl;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_process_in_test(mrp_port, process);
}

static struct mrp_instance *mrp_find_instance(struct mrp_control *mrp_ctrl,
					      u32 ring_id)
{
	struct mrp_instance *mrp_inst;

	list_for_each_entry(mrp_inst, &mrp_ctrl->mrps_list, list) {
		if (mrp_inst->ring_id == ring_id)
			return mrp_inst;
	}

	return NULL;
}

static struct mrp_instance *mrp_create_instance(struct mrp_control *mrp_ctrl,
						u32 ring_id)
{
	struct mrp_instance *mrp_inst;

	/* No entries founded create one */
	mrp_inst = kzalloc(sizeof(*mrp_inst), GFP_KERNEL);
	if (!mrp_inst)
		return NULL;

	mrp_inst->ring_id = ring_id;
	mrp_inst->mrp_ctrl = mrp_ctrl;
	mrp_inst->mra_support = false;
	mrp_inst->monitor = false;
	mrp_alloc_loc_period(mrp_ctrl, mrp_inst);
	INIT_DELAYED_WORK(&mrp_inst->ring_loc_work, mrp_ring_loc_work);
	INIT_DELAYED_WORK(&mrp_inst->in_loc_work, mrp_in_loc_work);

	list_add_tail(&mrp_inst->list, &mrp_ctrl->mrps_list);

	return mrp_inst;
}

static void mrp_remove_instance(struct mrp_instance *mrp_inst)
{
	list_del(&mrp_inst->list);
	kfree(mrp_inst);
}

static bool mrp_is_empty_instance(struct mrp_instance *mrp_inst)
{
	for (int i = 0; i < MRP_MAX_PORTS; ++i)
		if (mrp_inst->ports[i])
			return false;

	return true;
}

static void mrp_instance_add_port(struct mrp_instance *mrp_inst,
				  struct mrp_port *mrp_port)
{
	int i;

	for (i = 0; i < MRP_MAX_PORTS; ++i)
		if (!mrp_inst->ports[i])
			break;

	mrp_inst->ports[i] = mrp_port;

	mrp_port_alloc_ring_test(mrp_port);
	mrp_port_alloc_in_test(mrp_port);
}

static void mrp_instance_del_port(struct mrp_instance *mrp_inst,
				  struct mrp_port *mrp_port)
{
	/* Remove the port from the instance */
	for (int i = 0; i < MRP_MAX_PORTS; ++i) {
		if (mrp_inst->ports[i] == mrp_port) {
			mrp_port_free_ring_test(mrp_port);
			mrp_port_free_in_test(mrp_port);

			mrp_inst->ports[i] = NULL;
			break;
		}
	}
}

static struct mrp_port *mrp_instance_find_ring_port_partner(struct mrp_instance *mrp_inst,
							    struct mrp_port *mrp_port)
{
	for (int i = 0; i < MRP_MAX_PORTS; ++i) {
		struct mrp_port *p = mrp_inst->ports[i];

		if (p && p != mrp_port) {
			if (p->role == BR_MRP_PORT_ROLE_PRIMARY &&
			    mrp_port->role == BR_MRP_PORT_ROLE_SECONDARY)
				return p;

			if (p->role == BR_MRP_PORT_ROLE_SECONDARY &&
			    mrp_port->role == BR_MRP_PORT_ROLE_PRIMARY)
				return p;
		}
	}

	return NULL;
}

static void mrp_instance_find_port_partners(struct mrp_instance *mrp_inst,
					    struct mrp_port *mrp_port,
					    struct mrp_port **partners)
{
	for (int i = 0; i < MRP_MAX_PORTS; ++i) {
		struct mrp_port *p = mrp_inst->ports[i];

		if (p && p != mrp_port) {
			if (!partners[0])
				partners[0] = p;
			if (!partners[1])
				partners[1] = p;
		}
	}
}

int mrp_init(struct mrp_control *mrp)
{
	INIT_LIST_HEAD(&mrp->mrps_list);
	mrp->loc_period_mask = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(mrp_init);

int mrp_deinit(struct mrp_control *mrp)
{
	return 0;
}
EXPORT_SYMBOL_GPL(mrp_deinit);

static void *mrp_port_priv_from_netdev(struct mrp_control *mrp_ctrl,
				       struct net_device *dev)
{
	if (!mrp_ctrl->ops->mrp_port_priv_from_netdev)
		return netdev_priv(dev);

	return mrp_ctrl->ops->mrp_port_priv_from_netdev(dev);
}

struct mrp_port *mrp_add_port(struct mrp_control *mrp_ctrl,
			      const struct switchdev_obj_mrp *mrp,
			      struct net_device *dev)
{
	struct mrp_instance *mrp_inst;
	struct mrp_port *mrp_port;

	mrp_inst = mrp_find_instance(mrp_ctrl, mrp->ring_id);
	if (!mrp_inst) {
		/* If this is the first port added to the instance, then create
		 * the instance
		 */
		mrp_inst = mrp_create_instance(mrp_ctrl, mrp->ring_id);
		if (!mrp_inst)
			return ERR_PTR(-ENOMEM);
	}

	mrp_port = kzalloc(sizeof(*mrp_port), GFP_KERNEL);
	if (!mrp_port)
		goto out;

	mrp_port->priv = mrp_port_priv_from_netdev(mrp_ctrl, dev);
	mrp_port->dev = dev;
	mrp_port->mrp_inst = mrp_inst;
	mrp_port->ring_intr_status = MRP_INTERRUPT_STATUS_NONE;
	mrp_port->afi_ring_test_id = -1;

	mrp_ctrl->ops->mrp_port_init(mrp_port, mrp->prio);
	mrp_ctrl->ops->mrp_port_update_mac(mrp_port);
	mrp_ctrl->ops->mrp_port_redirect_control(mrp_port);

	mrp_instance_add_port(mrp_inst, mrp_port);

	return mrp_port;

out:
	mrp_remove_instance(mrp_inst);

	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_GPL(mrp_add_port);

int mrp_del_port(struct mrp_control *mrp_ctrl,
		 const struct switchdev_obj_mrp *mrp,
		 struct mrp_port *mrp_port)
{
	struct mrp_instance *mrp_inst = NULL, *tmp;

	/* Don't create again the instance if already is deleted. Because we
	 * will just delete it later. In that case just return OK
	 */
	list_for_each_entry(tmp, &mrp_ctrl->mrps_list, list) {
		if (tmp->ring_id == mrp->ring_id) {
			mrp_inst = tmp;
			break;
		}
	}

	if (!mrp_inst)
		return 0;

	mrp_ctrl->ops->mrp_port_uninit(mrp_port);
	mrp_instance_del_port(mrp_inst, mrp_port);
	kfree(mrp_port);

	/* If there are no more ports to the instance, then remove the instance
	 */
	if (mrp_is_empty_instance(mrp_inst))
		mrp_remove_instance(mrp_inst);

	return 0;
}
EXPORT_SYMBOL_GPL(mrp_del_port);

struct mrp_port *mrp_add_in_port(struct mrp_control *mrp_ctrl,
				 const struct switchdev_obj_in_role_mrp *mrp,
				 struct net_device *dev)
{
	struct mrp_instance *mrp_inst;
	struct mrp_port *mrp_port;

	mrp_inst = mrp_find_instance(mrp_ctrl, mrp->ring_id);
	if (!mrp_inst)
		return ERR_PTR(-EINVAL);

	mrp_port = kzalloc(sizeof(*mrp_port), GFP_KERNEL);
	if (!mrp_port)
		goto out;

	mrp_inst->in_id = mrp->in_id;

	mrp_port->priv = mrp_port_priv_from_netdev(mrp_ctrl, dev);
	mrp_port->dev = dev;
	mrp_port->mrp_inst = mrp_inst;
	mrp_port->in_intr_status = MRP_INTERRUPT_STATUS_NONE;
	mrp_port->afi_in_test_id = -1;

	mrp_ctrl->ops->mrp_port_init(mrp_port, 0);
	mrp_ctrl->ops->mrp_port_update_mac(mrp_port);
	mrp_ctrl->ops->mrp_port_redirect_control(mrp_port);
	mrp_port_set_port_role(mrp_port, BR_MRP_PORT_ROLE_INTER);

	mrp_instance_add_port(mrp_inst, mrp_port);

	return mrp_port;

out:
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_GPL(mrp_add_in_port);

int mrp_del_in_port(struct mrp_control *mrp_ctrl,
		    const struct switchdev_obj_in_role_mrp *mrp,
		    struct mrp_port *mrp_port)
{
	struct mrp_instance *mrp_inst = NULL, *tmp;

	/* Don't create again the instance if already is deleted. Because we
	 * will just delete it later. In that case just return OK
	 */
	list_for_each_entry(tmp, &mrp_ctrl->mrps_list, list) {
		if (tmp->ring_id == mrp->ring_id && tmp->in_id == mrp->in_id) {
			mrp_inst = tmp;
			break;
		}
	}

	if (!mrp_inst)
		return 0;

	mrp_ctrl->ops->mrp_port_uninit(mrp_port);
	mrp_instance_del_port(mrp_inst, mrp_port);
	kfree(mrp_port);

	return 0;
}
EXPORT_SYMBOL_GPL(mrp_del_in_port);

int mrp_ring_interrupt(struct mrp_control *mrp_ctrl)
{
	struct mrp_instance *mrp_inst;
	bool is_interrupt = false;
	struct mrp_port *mrp_port;

	list_for_each_entry(mrp_inst, &mrp_ctrl->mrps_list, list) {
		for (int i = 0; i < MRP_MAX_PORTS; ++i) {
			mrp_port = mrp_inst->ports[i];

			if (!mrp_port)
				continue;

			if (mrp_port->role == BR_MRP_PORT_ROLE_INTER)
				continue;

			mrp_port->ring_intr_status = mrp_port_get_ring_interrupt_status(mrp_port);
			if (mrp_port->ring_intr_status != MRP_INTERRUPT_STATUS_NONE)
				is_interrupt = true;
		}

		if (is_interrupt) {
			queue_delayed_work(system_wq,
					   &mrp_inst->ring_loc_work,
					   0);
		}
		is_interrupt = false;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mrp_ring_interrupt);

int mrp_in_interrupt(struct mrp_control *mrp_ctrl)
{
	struct mrp_instance *mrp_inst;
	bool is_interrupt = false;
	struct mrp_port *mrp_port;

	list_for_each_entry(mrp_inst, &mrp_ctrl->mrps_list, list) {
		for (int i = 0; i < MRP_MAX_PORTS; ++i) {
			mrp_port = mrp_inst->ports[i];

			if (!mrp_port)
				continue;

			mrp_port->in_intr_status = mrp_port_get_in_interrupt_status(mrp_port);
			if (mrp_port->in_intr_status != MRP_INTERRUPT_STATUS_NONE)
				is_interrupt = true;
		}

		if (is_interrupt) {
			queue_delayed_work(system_wq,
					   &mrp_inst->in_loc_work,
					   0);
		}
		is_interrupt = false;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mrp_in_interrupt);

int mrp_port_set_ring_state(struct mrp_port *mrp_port,
			    const struct switchdev_obj_ring_state_mrp *mrp)
{
	struct mrp_control *mrp_ctrl;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->ring_id != mrp->ring_id)
		return 0;

	/* If the port is the interconnected port, then there is nothing to do
	 * here.
	 */
	if (mrp_port->role == BR_MRP_PORT_ROLE_INTER)
		return 0;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;

	if (mrp_port->mrp_inst->ring_state == mrp->ring_state)
		return mrp_ctrl->ops->mrp_port_set_ring_state(mrp_port,
							      mrp_port->mrp_inst->ring_transitions,
							      mrp->ring_state);

	if (mrp_port->mrp_inst->ring_state == BR_MRP_RING_STATE_CLOSED &&
	    mrp->ring_state == BR_MRP_RING_STATE_OPEN)
		mrp_port->mrp_inst->ring_transitions++;

	mrp_port->mrp_inst->ring_state = mrp->ring_state;

	return mrp_ctrl->ops->mrp_port_set_ring_state(mrp_port,
						      mrp_port->mrp_inst->ring_transitions,
						      mrp->ring_state);
}
EXPORT_SYMBOL_GPL(mrp_port_set_ring_state);

int mrp_port_set_ring_role(struct mrp_port *mrp_port,
			   const struct switchdev_obj_ring_role_mrp *mrp)
{
	struct mrp_port *mrp_port_partner;
	struct mrp_instance *mrp_inst;
	int ret;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->ring_id != mrp->ring_id)
		return 0;

	/* If the port is the interconnected port, then there is nothing to do
	 * here.
	 */
	if (mrp_port->role == BR_MRP_PORT_ROLE_INTER)
		return 0;

	if (mrp->ring_role == BR_MRP_RING_ROLE_MRA)
		mrp_port->mrp_inst->mra_support = true;

	mrp_inst = mrp_port->mrp_inst;
	mrp_port_partner = mrp_instance_find_ring_port_partner(mrp_inst,
							       mrp_port);

	switch (mrp->ring_role) {
	case BR_MRP_RING_ROLE_MRM:
		ret = mrp_port_terminate_ring_test(mrp_port);
		if (ret)
			return ret;

		ret = mrp_port_rewrite_ring_test(mrp_port, true);
		if (ret)
			return ret;

		break;
	case BR_MRP_RING_ROLE_MRA:
		if (mrp_inst->mra_support)
			mrp_port_redirect_ring_test(mrp_port, true);

		break;
	case BR_MRP_RING_ROLE_MRC:
		mrp_port_forward_ring_test(mrp_port, mrp_port_partner, true);
		break;
	case BR_MRP_RING_ROLE_DISABLED:
		mrp_port_redirect_ring_test(mrp_port, false);
		mrp_port_rewrite_ring_test(mrp_port, false);
		mrp_port_process_ring_test(mrp_port, false);
		cancel_delayed_work(&mrp_inst->ring_loc_work);
		break;
	default:
		break;
	}

	mrp_inst->ring_role = mrp->ring_role;

	return 0;
}
EXPORT_SYMBOL_GPL(mrp_port_set_ring_role);

int mrp_port_start_ring_test(struct mrp_port *mrp_port,
			     const struct switchdev_obj_ring_test_mrp *mrp)
{
	struct mrp_port *mrp_port_partner;
	struct mrp_instance *mrp_inst;
	int ret = 0;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->ring_id != mrp->ring_id)
		return 0;

	/* If the port is the interconnected port, then there is nothing to do
	 * here.
	 */
	if (mrp_port->role == BR_MRP_PORT_ROLE_INTER)
		return 0;

	mrp_port->ring_max_miss = mrp->max_miss;
	mrp_inst = mrp_port->mrp_inst;
	mrp_inst->monitor = mrp->monitor;

	if (mrp_inst->ring_role == BR_MRP_RING_ROLE_MRA)
		mrp_inst->mrp_ctrl->ops->mrp_port_update_mrm_mac(mrp_port,
								 mrp->best_mac);

	switch (mrp_inst->ring_role) {
	case BR_MRP_RING_ROLE_MRM:
		ret = __mrp_port_start_ring_test(mrp_port,
						 mrp->interval,
						 mrp->max_miss);
		break;
	case BR_MRP_RING_ROLE_MRA:
		if (!mrp_inst->monitor) {
			ret = mrp_port_rewrite_ring_test(mrp_port, true);
			if (ret)
				return ret;

			mrp_port_partner = mrp_instance_find_ring_port_partner(mrp_inst,
									       mrp_port);
			ret = mrp_port_forward_ring_test(mrp_port,
							 mrp_port_partner,
							 false);

			ret = __mrp_port_start_ring_test(mrp_port,
							 mrp->interval,
							 mrp->max_miss);
		} else {
			__mrp_port_stop_ring_test(mrp_port);
			mrp_port_rewrite_ring_test(mrp_port, false);

			mrp_port_partner = mrp_instance_find_ring_port_partner(mrp_inst,
									       mrp_port);
			ret = mrp_port_forward_ring_test(mrp_port,
							 mrp_port_partner,
							 true);
		}
		break;
	default:
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mrp_port_start_ring_test);

int mrp_port_stop_ring_test(struct mrp_port *mrp_port,
			    const struct switchdev_obj_ring_test_mrp *mrp)
{
	struct mrp_instance *mrp_inst;
	int ret = 0;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->ring_id != mrp->ring_id)
		return 0;

	/* If the port is the interconnected port, then there is nothing to do
	 * here.
	 */
	if (mrp_port->role == BR_MRP_PORT_ROLE_INTER)
		return 0;

	mrp_inst = mrp_port->mrp_inst;
	switch (mrp_inst->ring_role) {
	case BR_MRP_RING_ROLE_MRM:
		ret = __mrp_port_stop_ring_test(mrp_port);
		break;
	case BR_MRP_RING_ROLE_MRA:
		if (!mrp_inst->monitor)
			ret = __mrp_port_stop_ring_test(mrp_port);
		break;
	default:
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mrp_port_stop_ring_test);

int mrp_port_set_in_state(struct mrp_port *mrp_port,
			  const struct switchdev_obj_in_state_mrp *mrp)
{
	struct mrp_control *mrp_ctrl;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->in_id != mrp->in_id)
		return 0;

	/* If the port is not the interconnected port, then there is nothing to
	 * do here.
	 */
	if (mrp_port->role != BR_MRP_PORT_ROLE_INTER)
		return 0;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;

	if (mrp_port->mrp_inst->in_state == mrp->in_state)
		return mrp_ctrl->ops->mrp_port_set_in_state(mrp_port,
							    mrp_port->mrp_inst->in_transitions,
							    mrp->in_state);

	if (mrp_port->mrp_inst->in_state == BR_MRP_IN_STATE_CLOSED &&
	    mrp->in_state == BR_MRP_IN_STATE_OPEN)
		mrp_port->mrp_inst->in_transitions++;

	mrp_port->mrp_inst->in_state = mrp->in_state;

	return mrp_ctrl->ops->mrp_port_set_in_state(mrp_port,
						    mrp_port->mrp_inst->in_transitions,
						    mrp->in_state);
}
EXPORT_SYMBOL_GPL(mrp_port_set_in_state);

int mrp_port_set_in_role(struct mrp_port *mrp_port,
			 const struct switchdev_obj_in_role_mrp *mrp)
{
	struct mrp_port *mrp_port_partners[2];
	struct mrp_instance *mrp_inst;
	int ret;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->ring_id != mrp->ring_id ||
	    mrp_port->mrp_inst->in_id != mrp->in_id)
		return 0;

	/* If the port is not the interconnected port, then there is nothing to
	 * do here.
	 */
	if (mrp_port->role != BR_MRP_PORT_ROLE_INTER)
		return 0;

	mrp_inst = mrp_port->mrp_inst;
	mrp_instance_find_port_partners(mrp_inst, mrp_port, mrp_port_partners);

	switch (mrp->in_role) {
	case BR_MRP_IN_ROLE_MIM:
		/* Rewrite all the interconnect frames that are going out on all
		 * the ports, regardless of the port role
		 */
		ret = mrp_port_rewrite_in_test(mrp_port, true);
		if (ret)
			return ret;

		ret = mrp_port_rewrite_in_test(mrp_port_partners[0], true);
		if (ret)
			return ret;

		ret = mrp_port_rewrite_in_test(mrp_port_partners[1], true);
		if (ret)
			return ret;

		/* It is required to terminate our own frames on all the ports
		 * of the MRP instance
		 */
		ret = mrp_port_terminate_in_test(mrp_port);
		if (ret)
			return ret;

		ret = mrp_port_terminate_in_test(mrp_port_partners[0]);
		if (ret)
			return ret;

		ret = mrp_port_terminate_in_test(mrp_port_partners[1]);
		if (ret)
			return ret;

		/* It is required to forward the other interconnect frames on
		 * the other ring port. If the port is interconnect port then
		 * there is no interconnect frames from a remote end coming on
		 * this port, so there is nothing to do.
		 */
		ret = mrp_port_forward_rem_in_test(mrp_port_partners[0],
						   mrp_port_partners[1],
						   true);
		if (ret)
			return ret;

		ret = mrp_port_forward_rem_in_test(mrp_port_partners[1],
						   mrp_port_partners[0],
						   true);
		if (ret)
			return ret;

		break;
	case BR_MRP_IN_ROLE_MIC:
		/* It is required to forward the in test on all the ports, so
		 * first it is required to get the other ring ports and
		 * configure the interconnect port to forward the frame to ring
		 * ports and configure the ring ports to forward to the other
		 * ring port and interconnect port
		 */
		ret = mrp_port_forward_in_test(mrp_port,
					       mrp_port_partners[0],
					       mrp_port_partners[1],
					       true);
		if (ret)
			return ret;

		ret = mrp_port_forward_in_test(mrp_port_partners[0],
					       mrp_port,
					       mrp_port_partners[1],
					       true);
		if (ret)
			return ret;

		ret = mrp_port_forward_in_test(mrp_port_partners[1],
					       mrp_port_partners[0],
					       mrp_port,
					       true);
		if (ret)
			return ret;

		break;
	case BR_MRP_IN_ROLE_DISABLED:
		mrp_port_rewrite_in_test(mrp_port, false);
		mrp_port_process_in_test(mrp_port, false);

		cancel_delayed_work(&mrp_inst->in_loc_work);
		break;
	default:
		break;
	}

	mrp_inst->in_role = mrp->in_role;

	return 0;
}
EXPORT_SYMBOL_GPL(mrp_port_set_in_role);

int mrp_port_start_in_test(struct mrp_port *mrp_port,
			   const struct switchdev_obj_in_test_mrp *mrp)
{
	struct mrp_instance *mrp_inst;
	int ret = 0;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->in_id != mrp->in_id)
		return 0;

	/* It is required to allow to start the transmit of interconnect test
	 * frames on all the ports of the MRP instance regardless of the port
	 */
	mrp_inst = mrp_port->mrp_inst;
	mrp_port->in_max_miss = mrp->max_miss;

	switch (mrp_inst->in_role) {
	case BR_MRP_IN_ROLE_MIM:
		ret = __mrp_port_start_in_test(mrp_port,
					       mrp->interval,
					       mrp->max_miss);
		break;
	default:
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mrp_port_start_in_test);

int mrp_port_stop_in_test(struct mrp_port *mrp_port,
			  const struct switchdev_obj_in_test_mrp *mrp)
{
	struct mrp_instance *mrp_inst;
	int ret = 0;

	if (!mrp_port)
		return 0;

	if (mrp_port->mrp_inst->in_id != mrp->in_id)
		return 0;

	/* As allow to start to transmit the interconnect frames on all the
	 * ports then it should be allow to stop the transmision on the ports
	 */
	mrp_inst = mrp_port->mrp_inst;
	switch (mrp_inst->in_role) {
	case BR_MRP_IN_ROLE_MIM:
		ret = __mrp_port_stop_in_test(mrp_port);
		break;
	default:
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mrp_port_stop_in_test);

int mrp_port_set_port_role(struct mrp_port *mrp_port,
			   enum br_mrp_port_role_type port_role)
{
	struct mrp_control *mrp_ctrl;

	mrp_port->role = port_role;

	mrp_ctrl = mrp_port->mrp_inst->mrp_ctrl;
	return mrp_ctrl->ops->mrp_port_set_port_role(mrp_port, port_role);
}
EXPORT_SYMBOL_GPL(mrp_port_set_port_role);
