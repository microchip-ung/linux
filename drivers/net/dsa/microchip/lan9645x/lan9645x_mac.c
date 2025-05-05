// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

#define LAN9645X_MAC_COLUMNS	4

#define CMD_IDLE		0
#define CMD_LEARN		1
#define CMD_FORGET		2
#define CMD_AGE			3
#define CMD_GET_NEXT		4
#define CMD_INIT		5
#define CMD_READ		6
#define CMD_WRITE		7
#define CMD_SYNC_GET_NEXT	8

#define LAN9645X_INVALID_ROW	(-1)

static bool lan9645x_mact_entry_equal(struct lan9645x_mact_entry *entry,
				      const unsigned char *mac, u16 vid)
{
	/* The hardware table is keyed on (vid,mac) */
	return entry->common.key.vid == vid &&
		ether_addr_equal(mac, entry->common.key.mac);
}

static struct lan9645x_mact_entry *
lan9645x_mact_entry_find(struct lan9645x *lan9645x, const unsigned char *mac,
			 u16 vid)
{
	struct lan9645x_mact_entry *entry;

	lockdep_assert_held(&lan9645x->mac_entry_lock);

	list_for_each_entry(entry, &lan9645x->mac_entries, list)
		if (lan9645x_mact_entry_equal(entry, mac, vid))
			return entry;

	return NULL;
}

static struct lan9645x_mact_entry *
lan9645x_mact_entry_lookup(struct lan9645x *lan9645x, const unsigned char *mac,
			   u16 vid)
{
	return lan9645x_mact_entry_find(lan9645x, mac, vid);
}

static struct net_device *lan9645x_to_mac_master(struct lan9645x *lan9645x,
						 int port)
{
	struct lan9645x_port *p;

	p = lan9645x_to_port(lan9645x, port);
	if (!p)
		return NULL;

	return p->bond ? p->bond : lan9645x_port_to_ndev(p);
}

static struct lan9645x_mact_entry *
lan9645x_mact_entry_alloc(struct lan9645x *lan9645x, const unsigned char *mac,
			  u16 vid, u8 pgid, enum macaccess_entry_type type)
{
	struct lan9645x_mact_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NULL;

	INIT_LIST_HEAD(&entry->list);
	ether_addr_copy(entry->common.key.mac, mac);
	entry->common.key.vid = vid;
	entry->common.pgid = pgid;
	entry->common.row = LAN9645X_INVALID_ROW;
	entry->common.type = type;
	entry->bond = lan9645x_to_mac_master(lan9645x, pgid);

	dev_dbg(lan9645x->dev,
		"mact_entry_alloc mac=%pM vid=%u pgid=%u type=%d",
		entry->common.key.mac, entry->common.key.vid,
		entry->common.pgid, entry->common.type);
	return entry;
}

static void lan9645x_mact_entry_dealloc(struct lan9645x *lan9645x,
					struct lan9645x_mact_entry *entry)
{
	if (!entry)
		return;

	dev_dbg(lan9645x->dev,
		"mact_entry_dealloc mac=%pM vid=%u pgid=%u type=%d",
		entry->common.key.mac, entry->common.key.vid,
		entry->common.pgid, entry->common.type);

	list_del(&entry->list);
	kfree(entry);
}

static int lan9645x_mac_wait_for_completion(struct lan9645x *lan9645x,
					    u32 *maca)
{
	u32 val = 0;
	int err;

	lockdep_assert_held(&lan9645x->mact_lock);

	err = lan9645x_rd_poll_timeout(lan9645x, ANA_MACACCESS, val,
				       ANA_MACACCESS_MAC_TABLE_CMD_GET(val) ==
				       CMD_IDLE);
	if (err)
		return err;

	if (maca)
		*maca = val;

	return 0;
}

static void lan9645x_mact_parse(u32 machi, u32 maclo, u32 maca,
				struct lan9645x_mact_common *rentry)
{
	u64 addr = ANA_MACHDATA_MACHDATA_GET(machi);

	addr = addr << 32 | maclo;
	u64_to_ether_addr(addr, rentry->key.mac);
	rentry->key.vid = ANA_MACHDATA_VID_GET(machi);
	rentry->pgid = ANA_MACACCESS_DEST_IDX_GET(maca);
	rentry->type = ANA_MACACCESS_ENTRYTYPE_GET(maca);
}

int lan9645x_mact_read(struct lan9645x *lan9645x, int port, int row, int bucket,
		       struct lan9645x_mact_entry *entry)
{
	u32 maca, maclo, machi;

	lockdep_assert_held(&lan9645x->mact_lock);

	/* Prepare mac table index */
	lan_rmw(ANA_MACTINDX_M_INDEX_SET(row) |
		ANA_MACTINDX_BUCKET_SET(bucket),
		ANA_MACTINDX_M_INDEX |
		ANA_MACTINDX_BUCKET,
		lan9645x, ANA_MACTINDX);

	/* Issue read in direct mode */
	lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_READ),
	       lan9645x,
	       ANA_MACACCESS);

	if (lan9645x_mac_wait_for_completion(lan9645x, &maca))
		return -ETIMEDOUT;

	/* Parse data */
	if (!(maca & ANA_MACACCESS_VALID))
		return -EINVAL;

	if (ANA_MACACCESS_DEST_IDX_GET(maca) != port)
		return -EINVAL;

	machi = lan_rd(lan9645x, ANA_MACHDATA);
	maclo = lan_rd(lan9645x, ANA_MACLDATA);

	lan9645x_mact_parse(machi, maclo, maca, &entry->common);

	return 0;
}

static void lan9645x_mac_select(struct lan9645x *lan9645x,
				const unsigned char *addr, u16 vid)
{
	u64 maddr = ether_addr_to_u64(addr);

	lockdep_assert_held(&lan9645x->mact_lock);

	lan_wr(ANA_MACHDATA_VID_SET(vid) |
	       ANA_MACHDATA_MACHDATA_SET(maddr >> 32),
	       lan9645x,
	       ANA_MACHDATA);

	lan_wr(maddr & GENMASK(31, 0),
	       lan9645x,
	       ANA_MACLDATA);
}

static int __lan9645x_mact_hw_lookup(struct lan9645x *lan9645x,
				     const unsigned char *mac, u16 vid)
{
	u32 maca;
	int err;

	lockdep_assert_held(&lan9645x->mact_lock);

	lan9645x_mac_select(lan9645x, mac, vid);

	/* (vid,mac) lookup */
	lan_wr(ANA_MACACCESS_VALID_SET(1) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_READ),
	       lan9645x, ANA_MACACCESS);

	err = lan9645x_mac_wait_for_completion(lan9645x, &maca);
	if (err)
		return err;

	return ANA_MACACCESS_VALID_GET(maca) &&
		ANA_MACACCESS_ENTRYTYPE_GET(maca) == ENTRYTYPE_NORMAL;
}

static int lan9645x_mact_hw_lookup(struct lan9645x *lan9645x,
				   const unsigned char *mac, u16 vid)
{
	int err;

	mutex_lock(&lan9645x->mact_lock);
	err = __lan9645x_mact_hw_lookup(lan9645x, mac, vid);
	mutex_unlock(&lan9645x->mact_lock);
	return err;
}

static int __lan9645x_mact_forget(struct lan9645x *lan9645x,
				  const unsigned char mac[ETH_ALEN],
				  unsigned int vid,
				  enum macaccess_entry_type type)
{
	lockdep_assert_held(&lan9645x->mact_lock);

	lan9645x_mac_select(lan9645x, mac, vid);

	lan_wr(ANA_MACACCESS_ENTRYTYPE_SET(type) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_FORGET),
	       lan9645x,
	       ANA_MACACCESS);

	return lan9645x_mac_wait_for_completion(lan9645x, NULL);
}

int lan9645x_mact_forget(struct lan9645x *lan9645x,
			 const unsigned char mac[ETH_ALEN], unsigned int vid,
			 enum macaccess_entry_type type)
{
	int ret;

	mutex_lock(&lan9645x->mact_lock);
	ret = __lan9645x_mact_forget(lan9645x, mac, vid, type);
	mutex_unlock(&lan9645x->mact_lock);

	return ret;
}

static bool lan9645x_mac_ports_use_cpu(const unsigned char *mac,
				       enum macaccess_entry_type type)
{
	u32 mc_ports;

	switch (type) {
	case ENTRYTYPE_MACV4:
		mc_ports = (mac[1] << 8) | mac[2];
		break;
	case ENTRYTYPE_MACV6:
		mc_ports = (mac[0] << 8) | mac[1];
		break;
	default:
		return false;
	}

	return !!(mc_ports & BIT(CPU_PORT));
}

static int __lan9645x_mact_learn(struct lan9645x *lan9645x, int port,
				 const unsigned char *addr, u16 vid,
				 enum macaccess_entry_type type)
{
	bool cpu_copy = lan9645x_mac_ports_use_cpu(addr, type);

	lockdep_assert_held(&lan9645x->mact_lock);

	lan9645x_mac_select(lan9645x, addr, vid);

	lan_wr(ANA_MACACCESS_VALID_SET(1) |
	       ANA_MACACCESS_DEST_IDX_SET(port) |
	       ANA_MACACCESS_MAC_CPU_COPY_SET(cpu_copy) |
	       ANA_MACACCESS_ENTRYTYPE_SET(type) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_LEARN),
	       lan9645x, ANA_MACACCESS);

	return lan9645x_mac_wait_for_completion(lan9645x, NULL);
}

int lan9645x_mact_learn(struct lan9645x *lan9645x, int port,
			const unsigned char *addr, u16 vid,
			enum macaccess_entry_type type)
{
	int ret;

	mutex_lock(&lan9645x->mact_lock);
	ret = __lan9645x_mact_learn(lan9645x, port, addr, vid, type);
	mutex_unlock(&lan9645x->mact_lock);

	return ret;
}

static void __lan9645x_mac_notifiers(struct lan9645x *lan9645x,
				     enum switchdev_notifier_type type,
				     int port, const unsigned char *mac,
				     u16 vid, struct net_device *ndev)
{
	struct switchdev_notifier_fdb_info info = { 0 };

	if (WARN_ON(port > lan9645x->num_phys_ports))
		return;

	/* When HW learns on LAG ports, the pgid used is the lag_id. So same
	 * (vid,mac) hitting another port in the bond will not change the mac
	 * table
	 */
	if (!ndev)
		return;

	info.addr = mac;
	info.vid = vid;
	info.offloaded = true;
	call_switchdev_notifiers(type, ndev, &info.info, NULL);
}

static void lan9645x_mac_notifiers(struct lan9645x *lan9645x,
				   enum switchdev_notifier_type type, int port,
				   const unsigned char *mac, u16 vid,
				   struct net_device *ndev)
{
	rtnl_lock();
	__lan9645x_mac_notifiers(lan9645x, type, port, mac, vid, ndev);
	rtnl_unlock();
}

int lan9645x_mact_flush(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_mact_entry *entry, *tmp;
	struct list_head deleted;
	int err = 0;

	ASSERT_RTNL();
	/* Forced aging (flush) triggers mact table changes irq, so if the port
	 * has a very large number of hw learned entries, the irq handler can get
	 * overwhelmed.
	 *
	 * On the contrary, the automatic aging scans has some ratelimiting so
	 * this is avoided even if the mac table is filled at line rate.
	 *
	 * For flushing, we can ease pressure on the irq handler, by manually
	 * forgetting our entries.
	 */

	INIT_LIST_HEAD(&deleted);

	mutex_lock(&lan9645x->mac_entry_lock);
	list_for_each_entry_safe(entry, tmp, &lan9645x->mac_entries, list) {
		if (entry->common.type == ENTRYTYPE_NORMAL &&
		    entry->common.pgid == port) {
			list_del(&entry->list);
			list_add_tail(&entry->list, &deleted);
		}
	}
	mutex_unlock(&lan9645x->mac_entry_lock);

	mutex_lock(&lan9645x->mact_lock);
	list_for_each_entry(entry, &deleted, list)
		__lan9645x_mact_forget(lan9645x, entry->common.key.mac,
				       entry->common.key.vid,
				       entry->common.type);
	mutex_unlock(&lan9645x->mact_lock);

	list_for_each_entry_safe(entry, tmp, &deleted, list) {
		__lan9645x_mac_notifiers(lan9645x, SWITCHDEV_FDB_DEL_TO_BRIDGE,
					 entry->common.pgid,
					 entry->common.key.mac,
					 entry->common.key.vid, entry->bond);
		lan9645x_mact_entry_dealloc(lan9645x, entry);
	}

	 /* For good measure, we flush explicitly. */

	mutex_lock(&lan9645x->mact_lock);
	/* MAC table entries with dst index maching port are aged on scan. */
	lan_wr(ANA_ANAGEFIL_PID_EN_SET(1) |
	       ANA_ANAGEFIL_PID_VAL_SET(port),
	       lan9645x, ANA_ANAGEFIL);

	/* Flushing requires two scans. First sets AGE_FLAG=1, second removes
	 * entries with AGE_FLAG=1.
	 */
	lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_AGE),
	       lan9645x,
	       ANA_MACACCESS);

	err = lan9645x_mac_wait_for_completion(lan9645x, NULL);
	if (err)
		goto mact_unlock;

	lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_AGE),
	       lan9645x,
	       ANA_MACACCESS);

	err = lan9645x_mac_wait_for_completion(lan9645x, NULL);

mact_unlock:
	lan_wr(0, lan9645x, ANA_ANAGEFIL);
	mutex_unlock(&lan9645x->mact_lock);
	return err;
}

int lan9645x_mact_entry_add(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid)
{
	struct lan9645x_mact_entry *entry;
	int ret = 0;

	ret = lan9645x_mact_hw_lookup(lan9645x, mac, vid);
	if (ret)
		return ret;

	/* Users can not move (vid,mac) to a different port, without removing
	 * the original entry first.
	 */
	mutex_lock(&lan9645x->mac_entry_lock);
	entry = lan9645x_mact_entry_lookup(lan9645x, mac, vid);
	if (entry) {
		mutex_unlock(&lan9645x->mac_entry_lock);
		goto mac_learn;
	}

	entry = lan9645x_mact_entry_alloc(lan9645x, mac, vid, pgid,
					  ENTRYTYPE_LOCKED);
	if (!entry) {
		mutex_unlock(&lan9645x->mac_entry_lock);
		return -ENOMEM;
	}

	list_add_tail(&entry->list, &lan9645x->mac_entries);
	mutex_unlock(&lan9645x->mac_entry_lock);

mac_learn:
	WARN_ON(entry->common.pgid != pgid);
	ret = lan9645x_mact_learn(lan9645x, pgid, mac, vid, ENTRYTYPE_LOCKED);
	if (ret) {
		mutex_lock(&lan9645x->mac_entry_lock);
		lan9645x_mact_entry_dealloc(lan9645x, entry);
		mutex_unlock(&lan9645x->mac_entry_lock);
	}
	return ret;
}

int lan9645x_mact_entry_del(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid)
{
	struct lan9645x_mact_entry *entry;

	mutex_lock(&lan9645x->mac_entry_lock);
	entry = lan9645x_mact_entry_lookup(lan9645x, mac, vid);
	if (entry) {
		WARN_ON(entry->common.pgid != pgid);
		lan9645x_mact_entry_dealloc(lan9645x, entry);
		mutex_unlock(&lan9645x->mac_entry_lock);
		goto forget;
	}
	mutex_unlock(&lan9645x->mac_entry_lock);
	return -ENOENT;

forget:
	return lan9645x_mact_forget(lan9645x, mac, vid, ENTRYTYPE_LOCKED);
}

static void lan9645x_mac_irq_process(struct lan9645x *lan9645x, int row,
				     struct lan9645x_mact_common *raw)
{
	struct lan9645x_mact_entry *entry, *tmp;
	struct list_head deleted;
	struct net_device *ndev;
	int col;

	INIT_LIST_HEAD(&deleted);

	/* The changes2sw bit can not reliably be used to detect which columns
	 * in a row was actually changed.
	 */

	mutex_lock(&lan9645x->mac_entry_lock);
	list_for_each_entry_safe(entry, tmp, &lan9645x->mac_entries, list) {
		bool found = false;

		/* This implicitly also filters on ENTRYTYE_NORMAL */
		if (entry->common.row != row)
			continue;

		for (col = 0; col < LAN9645X_MAC_COLUMNS; ++col) {
			/* All the valid entries are at the start of the row */
			if (!raw[col].valid)
				break;

			if (raw[col].processed ||
			    raw[col].pgid >= lan9645x->num_phys_ports ||
			    raw[col].type != ENTRYTYPE_NORMAL)
				continue;

			if (lan9645x_mact_entry_equal(entry, raw[col].key.mac,
						      raw[col].key.vid)) {
				/* (vid,mac) either got aged flag set, moved port
				 * or is collateral.
				 */
				if (entry->common.pgid == raw[col].pgid) {
					raw[col].processed = true;
					found = true;
					break;
				}
			}
		}

		if (!found) {
			/* aged out or moved */
			list_del(&entry->list);
			list_add_tail(&entry->list, &deleted);
		}
	}
	mutex_unlock(&lan9645x->mac_entry_lock);

	list_for_each_entry_safe(entry, tmp, &deleted, list) {
		lan9645x_mac_notifiers(lan9645x, SWITCHDEV_FDB_DEL_TO_BRIDGE,
				       entry->common.pgid,
				       entry->common.key.mac,
				       entry->common.key.vid, entry->bond);
		lan9645x_mact_entry_dealloc(lan9645x, entry);
	}

	/* Now go to the list of columns and see if any entry was not in the SW
	 * list, then that means that the entry is new so it needs to notify the
	 * bridge.
	 */
	for (col = 0; col < LAN9645X_MAC_COLUMNS; ++col) {
		if (!raw[col].valid)
			break;
		if (raw[col].processed ||
		    raw[col].pgid >= lan9645x->num_phys_ports ||
		    raw[col].type != ENTRYTYPE_NORMAL)
			continue;

		mutex_lock(&lan9645x->mac_entry_lock);
		entry = lan9645x_mact_entry_lookup(lan9645x, raw[col].key.mac,
						   raw[col].key.vid);
		if (entry) {
			WARN_ON(entry->common.pgid != raw[col].pgid);
			dev_info(lan9645x->dev,
				 "found SKIPPED not added mac=%pM vid=%u pgid=%u epgid=%u",
				 raw[col].key.mac, raw[col].key.vid,
				 raw[col].pgid, entry->common.pgid);
			mutex_unlock(&lan9645x->mac_entry_lock);
			continue;
		}

		entry = lan9645x_mact_entry_alloc(lan9645x, raw[col].key.mac,
						  raw[col].key.vid,
						  raw[col].pgid,
						  ENTRYTYPE_NORMAL);
		if (!entry) {
			mutex_unlock(&lan9645x->mac_entry_lock);
			return;
		}

		ndev = entry->bond;
		entry->common.row = row;
		list_add_tail(&entry->list, &lan9645x->mac_entries);
		mutex_unlock(&lan9645x->mac_entry_lock);

		WARN_ON(entry->common.pgid != raw[col].pgid);

		lan9645x_mac_notifiers(lan9645x, SWITCHDEV_FDB_ADD_TO_BRIDGE,
				       raw[col].pgid, raw[col].key.mac,
				       raw[col].key.vid, ndev);
	}
}

irqreturn_t lan9645x_mac_irq_handler(int virq, void *args)
{
	struct lan9645x_mact_common rentry[LAN9645X_MAC_COLUMNS] = { 0 };
	struct lan9645x *lan9645x = args;
	u32 mach, macl, maca = 0;
	int nrows = 0, rnds = 0;
	u64 t0 = ktime_get_ns();
	u32 index, column;
	bool stop = true;
	u32 val;

	if (!(ANA_ANAINTR_INTR_GET(lan_rd(lan9645x, ANA_ANAINTR))))
		return IRQ_HANDLED;

	mutex_lock(&lan9645x->mact_lock);
	/* Start the scan from 0, 0 */
	lan_wr(ANA_MACTINDX_M_INDEX_SET(0) | ANA_MACTINDX_BUCKET_SET(0),
	       lan9645x, ANA_MACTINDX);

	while (1) {
		lan_rmw(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_SYNC_GET_NEXT),
			ANA_MACACCESS_MAC_TABLE_CMD, lan9645x,
			ANA_MACACCESS);
		if (WARN_ON(lan9645x_mac_wait_for_completion(lan9645x,
							     &maca))) {
			break;
		}

		val = lan_rd(lan9645x, ANA_MACTINDX);
		index = ANA_MACTINDX_M_INDEX_GET(val);
		column = ANA_MACTINDX_BUCKET_GET(val);

		/* The SYNC-GET-NEXT returns all the entries(4) in a row in
		 * which is suffered a change. By change it means that new entry
		 * was added or an entry was removed because of ageing.
		 * It would return all the columns for that row. And after that
		 * it would return the next row.
		 * The stop conditions of the SYNC-GET-NEXT is when it reaches
		 * 'directly' to row 0 column 3. So if SYNC-GET-NEXT returns
		 * row 0 and column 0 then it is required to continue to read
		 * more even if it reaches row 0 and column 3.
		 */
		if (index == 0 && column == 0)
			stop = false;

		if (column == LAN9645X_MAC_COLUMNS - 1 && index == 0 && stop)
			break;

		rentry[column].valid = ANA_MACACCESS_VALID_GET(maca);
		rentry[column].processed = ANA_MACACCESS_ENTRYTYPE_GET(maca) !=
			ENTRYTYPE_NORMAL;
		if (rentry[column].valid && !rentry[column].processed) {
			mach = lan_rd(lan9645x, ANA_MACHDATA);
			macl = lan_rd(lan9645x, ANA_MACLDATA);
			lan9645x_mact_parse(mach, macl, maca, &rentry[column]);
		}

		/* Once all the columns are read process them */
		if (column == LAN9645X_MAC_COLUMNS - 1) {
			lan9645x_mac_irq_process(lan9645x, index, rentry);
			/* A row was processed so it is safe to assume that the
			 * next row/column can be the stop condition
			 */
			stop = true;
			nrows++;
		}
	}

	mutex_unlock(&lan9645x->mact_lock);

	lan_rmw(ANA_ANAINTR_INTR_SET(0), ANA_ANAINTR_INTR, lan9645x,
		ANA_ANAINTR);

	dev_dbg(lan9645x->dev,
		"ana irq nrows=%d rnds=%d struct_sz=%d idx,col=(%u,%u) nsec=%llu",
		nrows, rnds, sizeof(struct lan9645x_mact_common), index, column,
		ktime_get_ns() - t0);

	return IRQ_HANDLED;
}

void lan9645x_mac_init(struct lan9645x *lan9645x)
{
	/* Clear the MAC table */
	lan_wr(CMD_INIT, lan9645x, ANA_MACACCESS);
	lan9645x_mac_wait_for_completion(lan9645x, NULL);

	mutex_init(&lan9645x->mac_entry_lock);
	mutex_init(&lan9645x->mact_lock);
	mutex_init(&lan9645x->fwd_domain_lock);
	INIT_LIST_HEAD(&lan9645x->mac_entries);
}

void lan9645x_mac_deinit(struct lan9645x *lan9645x)
{
	mutex_destroy(&lan9645x->mac_entry_lock);
	mutex_destroy(&lan9645x->mact_lock);
	mutex_destroy(&lan9645x->fwd_domain_lock);
}

int lan9645x_mact_dsa_dump(struct lan9645x *lan9645x, int port,
			   dsa_fdb_dump_cb_t *cb, void *data)
{
	struct lan9645x_mact_entry entry = { 0 };
	u32 mach, macl, maca, type;
	u64 t0 = ktime_get_ns();
	int err = 0;
	u32 autoage;
	u32 cnt = 0;

	mach = 0;
	macl = 0;
	type = ENTRYTYPE_NORMAL;

	mutex_lock(&lan9645x->mact_lock);

	/* The aging filter works both for aging scans and GET_NEXT table scans */

	/* Disable automatic aging temporarily */
	autoage = lan_rd(lan9645x, ANA_AUTOAGE);
	lan_wr(autoage & ~ANA_AUTOAGE_AGE_PERIOD, lan9645x, ANA_AUTOAGE);

	/* Setup filter on our port */
	lan_wr(ANA_ANAGEFIL_PID_EN_SET(1) | ANA_ANAGEFIL_PID_VAL_SET(port),
	       lan9645x, ANA_ANAGEFIL);

	lan_wr(0, lan9645x, ANA_MACHDATA);
	lan_wr(0, lan9645x, ANA_MACLDATA);

	while (1) {
		/* NOTE: we rely on mach, macl and type being set correctly from
		 * previous round, vs the GET_NEXT semantics, so locking entire
		 * loop is important.
		 */
		lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_GET_NEXT) |
		       ANA_MACACCESS_ENTRYTYPE_SET(type),
		       lan9645x, ANA_MACACCESS);

		if (lan9645x_mac_wait_for_completion(lan9645x, &maca))
			break;

		if (ANA_MACACCESS_VALID_GET(maca) == 0)
			break;

		mach = lan_rd(lan9645x, ANA_MACHDATA);
		macl = lan_rd(lan9645x, ANA_MACLDATA);

		lan9645x_mact_parse(mach, macl, maca, &entry.common);

		if (ANA_MACACCESS_DEST_IDX_GET(maca) == port &&
		    (entry.common.type == ENTRYTYPE_NORMAL ||
		     entry.common.type == ENTRYTYPE_LOCKED)) {
			cnt++;
			err = cb(entry.common.key.mac, entry.common.key.vid,
				 entry.common.type == ENTRYTYPE_LOCKED, data);
			if (err)
				break;
		}
	}

	/* Remove aging filters and reenable aging */
	lan_wr(0, lan9645x, ANA_ANAGEFIL);
	lan_wr(autoage, lan9645x, ANA_AUTOAGE);

	mutex_unlock(&lan9645x->mact_lock);

	dev_dbg(lan9645x->dev, "dump elapsed=%llu", ktime_get_ns() - t0);

	return err;
}
