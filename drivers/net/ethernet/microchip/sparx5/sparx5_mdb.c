// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/if_bridge.h>
#include <net/switchdev.h>

#include "sparx5_main_regs.h"
#include "sparx5_main.h"
#include "sparx5_mrp.h"
#include "sparx5_tc.h"

int sparx5_mdb_entries_clear(struct sparx5 *sparx5)
{
	struct sparx5_mdb_entry *mdb_entry;

	mutex_lock(&sparx5->mdb_lock);
	list_for_each_entry(mdb_entry, &sparx5->mdb_entries, list)
		sparx5_mact_forget(sparx5, mdb_entry->addr, mdb_entry->vid);
	mutex_unlock(&sparx5->mdb_lock);

	return 0;
}

int sparx5_mdb_entries_restore(struct sparx5 *sparx5)
{
	struct sparx5_mdb_entry *mdb_entry;

	mutex_lock(&sparx5->mdb_lock);
	list_for_each_entry(mdb_entry, &sparx5->mdb_entries, list)
		sparx5_mact_learn(sparx5, mdb_entry->pgid_idx, mdb_entry->addr,
				  mdb_entry->vid);
	mutex_unlock(&sparx5->mdb_lock);

	return 0;
}

static int sparx5_mdb_entry_alloc(struct sparx5 *sparx5,
				  const unsigned char *addr,
				  u16 vid,
				  struct sparx5_mdb_entry **entry_out)
{
	struct sparx5_mdb_entry *entry;
	u16 pgid_idx;
	int err;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	err = sparx5_pgid_alloc_mcast(sparx5, &pgid_idx);
	if (err) {
		kfree(entry);
		return err;
	}

	memcpy(entry->addr, addr, ETH_ALEN);
	entry->vid = vid;
	entry->pgid_idx = pgid_idx;

	mutex_lock(&sparx5->mdb_lock);
	list_add_tail(&entry->list, &sparx5->mdb_entries);
	mutex_unlock(&sparx5->mdb_lock);

	*entry_out = entry;
	return 0;
}

static void sparx5_mdb_entry_free(struct sparx5 *sparx5,
				  const unsigned char *addr,
				  u16 vid)
{
	struct sparx5_mdb_entry *entry, *tmp;

	mutex_lock(&sparx5->mdb_lock);
	list_for_each_entry_safe(entry, tmp, &sparx5->mdb_entries, list) {
		if ((vid == 0 || entry->vid == vid) &&
		    ether_addr_equal(addr, entry->addr)) {
			list_del(&entry->list);

			sparx5_pgid_free(sparx5, entry->pgid_idx);
			kfree(entry);
			goto out;
		}
	}

out:
	mutex_unlock(&sparx5->mdb_lock);
}

static struct sparx5_mdb_entry *sparx5_mdb_entry_get(struct sparx5 *sparx5,
						     const unsigned char *addr,
						     u16 vid)
{
	struct sparx5_mdb_entry *e, *found = NULL;

	mutex_lock(&sparx5->mdb_lock);
	list_for_each_entry(e, &sparx5->mdb_entries, list) {
		if (ether_addr_equal(e->addr, addr) && e->vid == vid) {
			found = e;
			goto out;
		}
	}

out:
	mutex_unlock(&sparx5->mdb_lock);
	return found;
}

int sparx5_handle_mdb_add(struct net_device *dev, struct notifier_block *nb,
			  const struct switchdev_obj_port_mdb *v)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *spx5 = port->sparx5;
	const struct sparx5_consts *consts;
	struct sparx5_mdb_entry *entry;
	bool is_host, is_new;
	int err, i;
	u16 vid;

	consts = &spx5->data->consts;

	if (!sparx5_netdevice_check(dev))
		return -EOPNOTSUPP;

	is_host = netif_is_bridge_master(v->obj.orig_dev);

	/* When VLAN unaware the vlan value is not parsed and we receive vid 0.
	 * Fall back to bridge vid 1.
	 */
	if (!br_vlan_enabled(spx5->hw_bridge_dev))
		vid = 1;
	else
		vid = v->vid;

	is_new = false;
	entry = sparx5_mdb_entry_get(spx5, v->addr, vid);
	if (!entry) {
		err = sparx5_mdb_entry_alloc(spx5, v->addr, vid, &entry);
		is_new = true;
		if (err)
			return err;
	}

	mutex_lock(&spx5->mdb_lock);

	/* Add any mrouter ports to the new entry */
	if (is_new && ether_addr_is_ip_mcast(v->addr))
		for (i = 0; i < consts->chip_ports; i++)
			if (spx5->ports[i] && spx5->ports[i]->is_mrouter)
				sparx5_pgid_update_mask(spx5->ports[i],
							entry->pgid_idx,
							true);

	if (is_host && !entry->cpu_copy) {
		sparx5_pgid_cpu_copy_ena(spx5, entry->pgid_idx, true);
		entry->cpu_copy = true;
	} else if (!is_host) {
		sparx5_pgid_update_mask(port, entry->pgid_idx, true);
		set_bit(port->portno, entry->port_mask);
	}
	mutex_unlock(&spx5->mdb_lock);

	sparx5_mact_learn(spx5, entry->pgid_idx, entry->addr, entry->vid);

	return 0;
}

int sparx5_handle_mdb_del(struct net_device *dev,
			  struct notifier_block *nb,
			  const struct switchdev_obj_port_mdb *v)
{
	struct sparx5_port *port = netdev_priv(dev);
	struct sparx5 *spx5 = port->sparx5;
	struct sparx5_mdb_entry *entry;
	bool is_host;
	u16 vid;

	if (!sparx5_netdevice_check(dev))
		return -EOPNOTSUPP;

	is_host = netif_is_bridge_master(v->obj.orig_dev);

	if (!br_vlan_enabled(spx5->hw_bridge_dev))
		vid = 1;
	else
		vid = v->vid;

	entry = sparx5_mdb_entry_get(spx5, v->addr, vid);
	if (!entry)
		return 0;

	mutex_lock(&spx5->mdb_lock);
	if (is_host && entry->cpu_copy) {
		sparx5_pgid_cpu_copy_ena(spx5, entry->pgid_idx, false);
		entry->cpu_copy = false;
	} else if (!is_host) {
		clear_bit(port->portno, entry->port_mask);

		/* Port not mrouter port or addr is L2 mcast, remove port from mask. */
		if (!port->is_mrouter || !ether_addr_is_ip_mcast(v->addr))
			sparx5_pgid_update_mask(port, entry->pgid_idx, false);
	}
	mutex_unlock(&spx5->mdb_lock);

	if (bitmap_empty(entry->port_mask, SPX5_PORTS) && !entry->cpu_copy) {
		 /* Clear pgid in case mrouter ports exists
		  * that are not part of the group.
		  */
		sparx5_pgid_clear(spx5, entry->pgid_idx);
		sparx5_mact_forget(spx5, entry->addr, entry->vid);
		sparx5_mdb_entry_free(spx5, entry->addr, entry->vid);
	}
	return 0;
}
