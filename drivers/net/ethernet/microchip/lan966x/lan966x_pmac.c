// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2019 Microchip Technology Inc. */

#include <linux/iopoll.h>
#include <linux/bitfield.h>

#include "lan966x_main.h"

static int lan966x_pmac_get_oui(u8 *mac)
{
	int res;

	res = mac[0];
	res <<= 8;
	res |= mac[1];
	res <<= 8;
	res |= mac[2];

	return res;
}

static int lan966x_pmac_get_device(u8 *mac)
{
	int res;

	res = mac[4] & GENMASK(3,0);
	res <<= 8;
	res |= mac[5];

	return res;
}

static struct lan966x_pmac_entry *lan966x_pmac_find_entry(struct lan966x_pmac *pmac,
							  u16 index)
{
	struct lan966x_pmac_entry *pmac_entry;

	list_for_each_entry(pmac_entry, &pmac->pmac_entries, list) {
		if (pmac_entry->index == index)
			return pmac_entry;
	}

	return NULL;
}

static struct lan966x_pmac_entry *lan966x_pmac_add_pmac_entry(struct lan966x_pmac *pmac,
							      u16 index)
{
	struct lan966x_pmac_entry *pmac_entry;

	pmac_entry = kzalloc(sizeof(*pmac_entry), GFP_KERNEL);
	if (!pmac_entry)
		return NULL;

	pmac_entry->index = index;
	list_add_tail(&pmac_entry->list, &pmac->pmac_entries);

	return pmac_entry;
}

static struct lan966x_pmac_pgid_entry *lan966x_pmac_get_pgid_entry(struct lan966x_pmac *pmac,
								   u8 ports)
{
	struct lan966x_pmac_pgid_entry *pgid_entry;

	/* Try to find an existing entry */
	list_for_each_entry(pgid_entry, &pmac->pgid_entries, list) {
		if (pgid_entry->ports == ports) {
			refcount_inc(&pgid_entry->refcount);
			return pgid_entry;
		}
	}

	/* Try to find an empty entry */
	for (int i = PGID_PMAC_START; i < PGID_PMAC_END; ++i) {
		bool used = false;

		list_for_each_entry(pgid_entry, &pmac->pgid_entries, list) {
			if (pgid_entry->index == i) {
				used = true;
				break;
			}
		}

		if (!used) {
			pgid_entry = kzalloc(sizeof(*pgid_entry), GFP_KERNEL);
			if (!pgid_entry)
				return NULL;

			pgid_entry->ports = ports;
			pgid_entry->index = i;
			refcount_set(&pgid_entry->refcount, 1);
			list_add_tail(&pgid_entry->list, &pmac->pgid_entries);

			return pgid_entry;
		}
	}

	return NULL;
}

static void lan966x_pmac_del_pgid_entry(struct lan966x_pmac_pgid_entry *pgid_entry)
{
	if (!refcount_dec_and_test(&pgid_entry->refcount))
		return;

	list_del(&pgid_entry->list);
	kfree(pgid_entry);
}

static struct lan966x_pmac_vlan_entry *lan966x_pmac_find_vlan_entry(struct lan966x_pmac *pmac,
								    u16 vlan)
{
	/* Try to find an existing entry */
	for (int i = 0; i < LAN966X_PMAC_VLAN_ENTRIES; ++i) {
		if (pmac->vlan_entries[i].vlan == vlan &&
		    pmac->vlan_entries[i].enabled) {
			refcount_inc(&pmac->vlan_entries[i].refcount);
			return &pmac->vlan_entries[i];
		}
	}

	return NULL;
}

static struct lan966x_pmac_vlan_entry *lan966x_pmac_get_vlan_entry(struct lan966x_pmac *pmac,
								   u16 vlan)
{
	struct lan966x_pmac_vlan_entry *vlan_entry = NULL;
	int i;

	/* Try to find an existing entry */
	vlan_entry = lan966x_pmac_find_vlan_entry(pmac, vlan);
	if (vlan_entry)
		return vlan_entry;;

	/* Try to allocate one */
	for (i = 0; i < LAN966X_PMAC_VLAN_ENTRIES; ++i) {
		if (!pmac->vlan_entries[i].enabled) {
			vlan_entry = &pmac->vlan_entries[i];
			break;
		}
	}

	if (!vlan_entry)
		return NULL;

	vlan_entry->enabled = true;
	vlan_entry->vlan = vlan;
	vlan_entry->index = i;
	refcount_set(&vlan_entry->refcount, 1);

	return vlan_entry;
}

static void lan966x_pmac_del_vlan_entry(struct lan966x *lan966x,
					struct lan966x_pmac_vlan_entry *vlan_entry)
{
	if (!refcount_dec_and_test(&vlan_entry->refcount))
		return;

	memset(vlan_entry, 0, sizeof(struct lan966x_pmac_vlan_entry));
	lan_wr(0, lan966x, ANA_PMAC_VLAN_CFG(vlan_entry->index));
}

int lan966x_pmac_add(struct lan966x_port *port, u8 *mac, u16 vlan)
{
	struct lan966x_pmac_pgid_entry *pgid_entry;
	struct lan966x_pmac_vlan_entry *vlan_entry;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_pmac_entry *pmac_entry;
	struct lan966x_pmac *pmac;
	int ret = 0;
	u16 index;

	pmac = &lan966x->pmac;
	/* Set and and initialize the oui on the first added entry */
	if (pmac->oui == -1) {
		pmac->oui = lan966x_pmac_get_oui(mac);
		lan_rmw(ANA_PMAC_CFG_PMAC_OUI_SET(pmac->oui) |
			ANA_PMAC_CFG_PMAC_ENA_SET(1),
			ANA_PMAC_CFG_PMAC_OUI |
			ANA_PMAC_CFG_PMAC_ENA,
			lan966x, ANA_PMAC_CFG);
	} else {
		/* Don't allow to add entries that don't match the oui */
		if (pmac->oui != lan966x_pmac_get_oui(mac))
			return -EINVAL;
	}

	/* Find entry in the vlan table */
	vlan_entry = lan966x_pmac_get_vlan_entry(pmac, vlan);
	if (!vlan_entry)
		return -ENOMEM;

	/* Find entry in the pmac table */
	index = lan966x_pmac_get_device(mac) +
		LAN966X_PMAC_ENTRIES_PER_VLAN * vlan_entry->index;

	pmac_entry = lan966x_pmac_find_entry(pmac, index);
	if (!pmac_entry) {
		pmac_entry = lan966x_pmac_add_pmac_entry(pmac, index);
		if (!pmac_entry) {
			ret = -ENOMEM;
			goto free_vlan_entry;
		}
	} else {
		/* If the pmac entry exists, then delete the reference to the
		 * PGID table because we need to allocate/reuse another one
		 * because the ports will be modified
		 */
		lan966x_pmac_del_pgid_entry(pmac_entry->pgid);
	}

	pmac_entry->ports |= BIT(port->chip_port);

	/* Find entry in the pgid table */
	pgid_entry = lan966x_pmac_get_pgid_entry(pmac, pmac_entry->ports);
	if (!pgid_entry) {
		ret = -ENOMEM;
		goto free_pmac_entry;
	}

	pmac_entry->pgid = pgid_entry;
	pmac_entry->vlan = vlan_entry;

	/* Now have allocated everything, just need to write it to the HW*/
	lan_wr(ANA_PMACTIDX_PMAC_INDEX_SET(pmac_entry->index),
	       lan966x, ANA_PMACTIDX);
	lan_wr(ANA_PMACACCESS_PMAC_VALID_SET(1) |
	       ANA_PMACACCESS_PMAC_DEST_IDX_SET(pgid_entry->index) |
	       ANA_PMACACCESS_PMAC_TBL_CMD_SET(PMACACCESS_CMD_WRITE),
	       lan966x, ANA_PMACACCESS);

	lan_rmw(ANA_PGID_PGID_SET(pgid_entry->ports),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(pgid_entry->index));

	lan_rmw(ANA_PMAC_VLAN_CFG_PMAC_VLAN_ENA_SET(1) |
		ANA_PMAC_VLAN_CFG_PMAC_VLAN_ID_SET(vlan_entry->vlan),
		ANA_PMAC_VLAN_CFG_PMAC_VLAN_ENA |
		ANA_PMAC_VLAN_CFG_PMAC_VLAN_ID,
		lan966x, ANA_PMAC_VLAN_CFG(vlan_entry->index));

	return ret;

free_pmac_entry:
	list_del(&pmac_entry->list);
	kfree(pmac_entry);

free_vlan_entry:
	lan966x_pmac_del_vlan_entry(lan966x, vlan_entry);

	return ret;
}

int lan966x_pmac_del(struct lan966x_port *port, u8 *mac, u16 vlan)
{
	struct lan966x_pmac_pgid_entry *pgid_entry;
	struct lan966x_pmac_vlan_entry *vlan_entry;
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_pmac_entry *pmac_entry;
	struct lan966x_pmac *pmac;
	u16 index;

	pmac = &lan966x->pmac;

	/* Don't allow to delete entries if it is not enabled or delete entries
	 * with wrong oui
	 */
	if (pmac->oui == -1 || pmac->oui != lan966x_pmac_get_oui(mac))
		return -EINVAL;

	/* If the vlan is not valid then there is nothing to delete */
	vlan_entry = lan966x_pmac_find_vlan_entry(pmac, vlan);
	if (!vlan_entry)
		return -EINVAL;

	/* If there is no entry then we don't have anything to delete */
	index = lan966x_pmac_get_device(mac) +
		LAN966X_PMAC_ENTRIES_PER_VLAN * vlan_entry->index;

	pmac_entry = lan966x_pmac_find_entry(pmac, index);
	if (!pmac_entry)
		return -EINVAL;

	/* If the deleted port is not part of the ports, then there is nothing
	 * to delete
	 */
	if (!(pmac_entry->ports & BIT(port->chip_port)))
		return -EINVAL;

	/* Delete the pmac entry from HW*/
	lan_wr(ANA_PMACTIDX_PMAC_INDEX_SET(pmac_entry->index),
	       lan966x, ANA_PMACTIDX);
	lan_wr(ANA_PMACACCESS_PMAC_TBL_CMD_SET(PMACACCESS_CMD_WRITE),
	       lan966x, ANA_PMACACCESS);

	/* Delete the pgid entry */
	lan966x_pmac_del_pgid_entry(pmac_entry->pgid);

	/* Delete the vlan entry */
	lan966x_pmac_del_vlan_entry(lan966x, vlan_entry);

	/* Check if there are any other entries for the pmac_entry */
	pmac_entry->ports &= ~BIT(port->chip_port);
	if (!pmac_entry->ports) {
		list_del(&pmac_entry->list);
		kfree(pmac_entry);
		goto check_oui;
	}

	/* Allocate a new PGID entry */
	pgid_entry = lan966x_pmac_get_pgid_entry(pmac, pmac_entry->ports);
	if (!pgid_entry) {
		list_del(&pmac_entry->list);
		kfree(pmac_entry);
		return -ENOMEM;
	}
	pmac_entry->pgid = pgid_entry;

	/* Write the PGID and the PMAC table, no need to write the vlan as it is
	 * already updated, otherwise there is a bug because is not possible to
	 * have entries in pmac without pointing to a vlan entry
	 */
	lan_rmw(ANA_PGID_PGID_SET(pgid_entry->ports),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(pgid_entry->index));

	lan_wr(ANA_PMACTIDX_PMAC_INDEX_SET(pmac_entry->index),
	       lan966x, ANA_PMACTIDX);
	lan_wr(ANA_PMACACCESS_PMAC_VALID_SET(1) |
	       ANA_PMACACCESS_PMAC_DEST_IDX_SET(pgid_entry->index) |
	       ANA_PMACACCESS_PMAC_TBL_CMD_SET(PMACACCESS_CMD_WRITE),
	       lan966x, ANA_PMACACCESS);

check_oui:
	if (list_empty(&pmac->pmac_entries)) {
		lan966x->pmac.oui = -1;
		lan_wr(0, lan966x, ANA_PMAC_CFG);
	}

	return 0;
}

int lan966x_pmac_purge(struct lan966x *lan966x)
{
	struct lan966x_pmac_pgid_entry *pgid_entry, *pgid_tmp;
	struct lan966x_pmac_entry *pmac_entry, *pmac_tmp;
	struct lan966x_pmac *pmac = &lan966x->pmac;

	/* Delete the vlans entries */
	for (int i = 0; i < LAN966X_PMAC_VLAN_ENTRIES; ++i) {
		lan_wr(0, lan966x, ANA_PMAC_VLAN_CFG(i));
		memset(&pmac->vlan_entries[i], 0, sizeof(struct lan966x_pmac_vlan_entry));
	}

	/* Delete the pmac entries */
	list_for_each_entry_safe(pmac_entry, pmac_tmp, &pmac->pmac_entries, list) {
		lan_wr(ANA_PMACTIDX_PMAC_INDEX_SET(pmac_entry->index),
		       lan966x, ANA_PMACTIDX);
		lan_wr(ANA_PMACACCESS_PMAC_TBL_CMD_SET(PMACACCESS_CMD_WRITE),
		       lan966x, ANA_PMACACCESS);

		list_del(&pmac_entry->list);
		kfree(pmac_entry);
	}

	/* Delet eth pgid entries */
	list_for_each_entry_safe(pgid_entry, pgid_tmp, &pmac->pgid_entries, list) {
		list_del(&pgid_entry->list);
		kfree(pgid_entry);
	}

	lan966x->pmac.oui = -1;
	lan_wr(0, lan966x, ANA_PMAC_CFG);

	return 0;
}

void lan966x_pmac_init(struct lan966x *lan966x)
{
	lan966x->pmac.oui = -1;
	INIT_LIST_HEAD(&lan966x->pmac.pgid_entries);
	INIT_LIST_HEAD(&lan966x->pmac.pmac_entries);
}

void lan966x_pmac_deinit(struct lan966x *lan966x)
{
	lan966x_pmac_purge(lan966x);
}
