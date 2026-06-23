// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include <linux/ptp_classify.h>
#include <linux/debugfs.h>
#include <linux/dsa/lan9645x.h>

#include "lan9645x_main.h"

/* Define the maximum number of logs to be stored. If the logs count is bigger
 * than this we then no more logs are stored. The number of logs is calculated
 * with presumption that we send one frame per second for Sync and one frame per
 * second for Delay. And then we can store the logs for 1h. (60 sec * 60 min *
 * 2 types of frames)
 */
#define LAN9645X_PTP_LOGS_COUNT_MAX		7200

struct sync_header {
	struct ptp_header hdr;
	__be16 sec_msb;
	__be32 sec_lsb;
	__be32 nsec;
} __packed;

struct lan9645x_ptp_log_entry {
	struct list_head list;
	u64 sec_a; /* This represents T1, T4 timestamps */
	u32 nsec_a;
	u64 sec_b; /* This represents T2 and T3 timestamps */
	u32 nsec_b;
	u64 correction;
	u32 sub_ns;
	u16 seq;
	char direction;
	bool complete;
};

static int lan9645x_ptp_log_show(struct seq_file *m, void *unused)
{
	struct lan9645x *lan9645x = m->private;
	struct lan9645x_ptp_log_entry *e;

	mutex_lock(&lan9645x->ptp_logs_lock);

	list_for_each_entry(e, &lan9645x->ptp_logs, list) {
		if (!e->complete)
			continue;

		seq_printf(m, "%c,%05d,%010lld,%09d,%010lld,%09d,%c%010lld,%03d\n",
			   e->direction, e->seq,
			   e->sec_a, e->nsec_a,
			   e->sec_b, e->nsec_b,
			   '+', e->correction, e->sub_ns);
	}

	mutex_unlock(&lan9645x->ptp_logs_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan9645x_ptp_log);

static int lan9645x_ptp_enable_log_show(struct seq_file *m, void *unused)
{
	struct lan9645x *lan9645x = m->private;

	mutex_lock(&lan9645x->ptp_logs_lock);

	seq_printf(m, "%d\n", lan9645x->ptp_enable_logs);

	mutex_unlock(&lan9645x->ptp_logs_lock);

	return 0;
}

static ssize_t lan9645x_ptp_enable_log_write(struct file *file,
					     const char __user *userbuf,
					     size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct lan9645x_ptp_log_entry *l, *tmp;
	struct lan9645x *lan9645x = m->private;
	u32 input;
	int ret;

	mutex_lock(&lan9645x->ptp_logs_lock);

	ret = kstrtou32_from_user(userbuf, count, 10, &input);
	if (ret)
		goto unlock;

	lan9645x->ptp_enable_logs = input;

	if (!input) {
		list_for_each_entry_safe(l, tmp, &lan9645x->ptp_logs, list) {
			list_del(&l->list);
			kfree(l);
		}
		lan9645x->ptp_logs_count = 0;
	}

	ret = count;
unlock:
	mutex_unlock(&lan9645x->ptp_logs_lock);

	return ret;
}
DEFINE_SHOW_STORE_ATTRIBUTE(lan9645x_ptp_enable_log);

static void lan9645x_ptp_add_t1(struct lan9645x *lan9645x,
				u16 seq, u64 sec, u32 nsec, u32 sub_ns,
				u64 correction)
{
	struct lan9645x_ptp_log_entry *e;

	if (lan9645x->ptp_logs_count > LAN9645X_PTP_LOGS_COUNT_MAX)
		return;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return;

	e->direction = 'X';
	e->seq = seq;
	e->sub_ns = sub_ns;
	e->correction = correction >> 16;
	e->sec_a = sec;
	e->nsec_a = nsec;

	list_add_tail(&e->list, &lan9645x->ptp_logs);
	++lan9645x->ptp_logs_count;
}

static void lan9645x_ptp_add_t2(struct lan9645x *lan9645x,
				u16 seq, u64 sec, u32 nsec, u32 sub_ns,
				u64 correction)
{
	struct lan9645x_ptp_log_entry *e;

	list_for_each_entry(e, &lan9645x->ptp_logs, list) {
		if (e->seq == seq && e->direction == 'X') {
			e->sec_b = sec;
			e->nsec_b = nsec;
			e->correction += correction >> 16;
			e->sub_ns += sub_ns;
			e->complete = true;
			break;
		}
	}
}

static void lan9645x_ptp_add_t3(struct lan9645x *lan9645x,
				u16 seq, u64 sec, u32 nsec, u32 sub_ns,
				u64 correction)
{
	struct lan9645x_ptp_log_entry *e;

	if (lan9645x->ptp_logs_count > LAN9645X_PTP_LOGS_COUNT_MAX)
		return;

	/* This is called in atomic context: tx PTP irq handler */
	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return;

	e->direction = 'Y';
	e->seq = seq;
	e->sub_ns = sub_ns;
	e->correction = correction >> 16;
	e->sec_b = sec;
	e->nsec_b = nsec;

	list_add_tail(&e->list, &lan9645x->ptp_logs);
	++lan9645x->ptp_logs_count;
}

static void lan9645x_ptp_add_t4(struct lan9645x *lan9645x,
				u16 seq, u64 sec, u32 nsec, u32 sub_ns,
				u64 correction)
{
	struct lan9645x_ptp_log_entry *e;

	list_for_each_entry(e, &lan9645x->ptp_logs, list) {
		if (e->seq == seq && e->direction == 'Y') {
			e->sec_a = sec;
			e->nsec_a = nsec;
			e->correction += correction >> 16;
			e->sub_ns += sub_ns;
			break;
		}
	}
}

static void lan9645x_ptp_update_t4(struct lan9645x *lan9645x,
				   u16 seq, u64 correction)
{
	struct lan9645x_ptp_log_entry *e;

	list_for_each_entry(e, &lan9645x->ptp_logs, list) {
		if (e->seq == seq && e->direction == 'Y') {
			e->correction += correction >> 16;
			e->complete = true;
			break;
		}
	}
}

static unsigned long lan9645x_cb_to_ptp_class(u8 rew_op, u8 pdu_type)
{
	if (rew_op == IFH_REW_OP_NOOP)
		return PTP_CLASS_NONE;

	switch (pdu_type) {
	case IFH_PDU_TYPE_IPV4:
		return PTP_CLASS_IPV4;
	case IFH_PDU_TYPE_IPV6:
		return PTP_CLASS_IPV6;
	default:
		return PTP_CLASS_L2;
	}
}

void lan9645x_ptp_log_tx(struct lan9645x *lan9645x, struct sk_buff *skb,
			 struct timespec64 ts, u32 sub_ns)
{
	struct lan9645x_skb_cb *cb = LAN9645X_SKB_CB(skb);
	struct ptp_header *ptp_header;
	unsigned long type;
	u16 seq;

	type = lan9645x_cb_to_ptp_class(cb->rew_op, cb->pdu_type);
	if (type == PTP_CLASS_NONE)
		return;

	mutex_lock(&lan9645x->ptp_logs_lock);
	if (!lan9645x->ptp_enable_logs)
		goto out;

	ptp_header = ptp_parse_header(skb, type);

	switch (ptp_get_msgtype(ptp_header, type)) {
	case PTP_MSGTYPE_DELAY_REQ: /* Delay req */
		seq = ntohs(ptp_header->sequence_id);
		lan9645x_ptp_add_t3(lan9645x, seq, ts.tv_sec, ts.tv_nsec,
				    sub_ns, 0);
		break;
	default:
		break;
	}

out:
	mutex_unlock(&lan9645x->ptp_logs_lock);
}

void lan9645x_ptp_log_rx(struct lan9645x *lan9645x, struct sk_buff *skb,
			 struct timespec64 ts, u8 sub_ns)
{
	struct ptp_header *ptp_header;
	struct sync_header *sync_header;
	unsigned long type;
	u64 origin_sec;
	u16 seq;

	mutex_lock(&lan9645x->ptp_logs_lock);
	if (!lan9645x->ptp_enable_logs)
		goto out;

	__skb_push(skb, ETH_HLEN);

	/* We log only PTP frames over L2 */
	type = PTP_CLASS_L2;
	ptp_header = ptp_parse_header(skb, type);

	switch (ptp_get_msgtype(ptp_header, type)) {
	case 0x0: /* Sync */
		seq = ntohs(ptp_header->sequence_id);

		sync_header = (struct sync_header *)ptp_header;
		origin_sec = ntohs(sync_header->sec_msb);
		origin_sec = origin_sec << 32;
		origin_sec |= ntohl(sync_header->sec_lsb);

		lan9645x_ptp_add_t1(lan9645x, seq, origin_sec,
				    ntohl(sync_header->nsec), 0,
				    be64_to_cpu(ptp_header->correction));

		lan9645x_ptp_add_t2(lan9645x, seq, ts.tv_sec,
				    ts.tv_nsec, sub_ns, 0);
		break;
	case 0x1: /* Delay req */
		seq = ntohs(ptp_header->sequence_id);
		lan9645x_ptp_add_t4(lan9645x, seq, ts.tv_sec, ts.tv_nsec,
				    sub_ns, 0);

		break;
	case 0x9: /* Delay resp */
		seq = ntohs(ptp_header->sequence_id);
		lan9645x_ptp_update_t4(lan9645x, seq,
				       be64_to_cpu(ptp_header->correction));
		break;
	default:
		break;
	}

	__skb_pull(skb, ETH_HLEN);
out:
	mutex_unlock(&lan9645x->ptp_logs_lock);
}

int lan9645x_ptp_log_init(struct lan9645x *lan9645x)
{
	struct dentry *dir;

	mutex_init(&lan9645x->ptp_logs_lock);
	INIT_LIST_HEAD(&lan9645x->ptp_logs);
	lan9645x->ptp_logs_count = 0;

	dir = debugfs_create_dir("ptp", lan9645x->debugfs_root);
	if (PTR_ERR_OR_ZERO(dir))
		return 0;

	debugfs_create_file("enable_log", 0666, dir, lan9645x,
			    &lan9645x_ptp_enable_log_fops);
	debugfs_create_file("log", 0444, dir, lan9645x,
			    &lan9645x_ptp_log_fops);

	return 0;
}

void lan9645x_ptp_log_deinit(struct lan9645x *lan9645x)
{
	struct lan9645x_ptp_log_entry *l, *tmp;

	mutex_lock(&lan9645x->ptp_logs_lock);
	list_for_each_entry_safe(l, tmp, &lan9645x->ptp_logs, list) {
		list_del(&l->list);
		kfree(l);
	}
	lan9645x->ptp_logs_count = 0;
	mutex_unlock(&lan9645x->ptp_logs_lock);
	mutex_destroy(&lan9645x->ptp_logs_lock);
}
