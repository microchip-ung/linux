// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"
#include "linux/err.h"
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/rtnetlink.h>

#include "lan9645x_netlink_frer.h"

static struct mchp_frer_stream_cfg cs_def_cfg;
static struct mchp_frer_stream_cfg ms_def_cfg;

static int lan9645x_frer_cs_cfg_update(struct lan9645x_nl_frer *frer,
				       const u16 cs_id)
{
	struct mchp_frer_stream_cfg *c = &frer->cs_cfg[cs_id];
	bool vector = (c->alg == MCHP_FRER_REC_ALG_VECTOR);
	u8 hlen = (c->hlen < 2) ? 1 : (c->hlen - 1);
	u16 rt = (c->reset_time) ? c->reset_time : 1;

	lan_wr(QSYS_FRER_CFG_CMP_TAKE_NO_SEQUENCE_SET(c->take_no_seq) |
	       QSYS_FRER_CFG_CMP_VECTOR_ALGORITHM_SET(vector) |
	       QSYS_FRER_CFG_CMP_HISTORY_LENGTH_SET(hlen) |
	       QSYS_FRER_CFG_CMP_RESET_TICKS_SET(rt) |
	       QSYS_FRER_CFG_CMP_RESET_SET(1) |
	       QSYS_FRER_CFG_CMP_ENABLE_SET(c->enable),
	       frer->lan9645x, QSYS_FRER_CFG_CMP(cs_id));
	return 0;
}

int lan9645x_frer_cs_cfg_set(struct lan9645x_nl_frer *frer, const u16 cs_id,
			     const struct mchp_frer_stream_cfg *const cfg)
{
	struct lan9645x *lan9645x = frer->lan9645x;

	dev_dbg(lan9645x->dev, "cs_id %u e %d a %s h %d r %d t %d\n",
		cs_id, cfg->enable,
		(cfg->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M",
		cfg->hlen, cfg->reset_time, cfg->take_no_seq);

	if (cs_id >= ARRAY_SIZE(frer->cs_cfg)) {
		dev_err(lan9645x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(frer->cs_cfg) - 1);
		return -EINVAL;
	}

	if (cfg->alg != MCHP_FRER_REC_ALG_VECTOR &&
	    cfg->alg != MCHP_FRER_REC_ALG_MATCH) {
		dev_err(lan9645x->dev, "Invalid alg (%d). Use %d for vector and %d for match\n",
			cfg->alg, MCHP_FRER_REC_ALG_VECTOR,
			MCHP_FRER_REC_ALG_MATCH);
		return -EINVAL;
	}

	if (cfg->hlen < LAN9645X_FRER_HLEN_MIN ||
	    cfg->hlen > LAN9645X_FRER_HLEN_MAX) {
		dev_err(lan9645x->dev, "Invalid hlen (%d). Use %d..%d\n",
			cfg->hlen, LAN9645X_FRER_HLEN_MIN, LAN9645X_FRER_HLEN_MAX);
		return -EINVAL;
	}

	if (cfg->reset_time < LAN9645X_FRER_RESET_MIN ||
	    cfg->reset_time > LAN9645X_FRER_RESET_MAX) {
		dev_err(lan9645x->dev, "Invalid reset_time (%d). Use %d..%d\n",
			cfg->reset_time, LAN9645X_FRER_RESET_MIN,
			LAN9645X_FRER_RESET_MAX);
		return -EINVAL;
	}

	frer->cs_cfg[cs_id] = *cfg;
	return lan9645x_frer_cs_cfg_update(frer, cs_id);
}

int lan9645x_frer_cs_cfg_get(struct lan9645x_nl_frer *frer,
			     const u16 cs_id,
			     struct mchp_frer_stream_cfg *const cfg)
{
	struct lan9645x *lan9645x = frer->lan9645x;

	ASSERT_RTNL();

	dev_dbg(lan9645x->dev, "cs_id %u\n", cs_id);

	if (cs_id >= ARRAY_SIZE(frer->cs_cfg)) {
		dev_err(lan9645x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(frer->cs_cfg) - 1);
		return -EINVAL;
	}

	*cfg = frer->cs_cfg[cs_id];
	return 0;
}

static inline void lan9645x_upd_cnt(u64 *cnt, u32 *prev, u32 val)
{
	*cnt += val - *prev;
	*prev = val;
}

static void lan9645x_frer_update_cs_stats(struct lan9645x_nl_frer *frer,
					  const u16 cs_id)
{
	struct mchp_frer_cnt *cnt = &frer->cs_cnt[cs_id];
	struct lan9645x_frer_prev_cnt *prev = &frer->cs_prev_cnt[cs_id];
	struct lan9645x *lan9645x = frer->lan9645x;

	/* WARN_ON(!mutex_is_locked(&lan9645x->stats_lock)); */

	lan9645x_upd_cnt(&cnt->out_of_order_packets, &prev->out_of_order_packets,
			 lan_rd(lan9645x, QSYS_CNT_CMP_OO(cs_id)));
	lan9645x_upd_cnt(&cnt->rogue_packets, &prev->rogue_packets,
			 lan_rd(lan9645x, QSYS_CNT_CMP_RG(cs_id)));
	lan9645x_upd_cnt(&cnt->passed_packets, &prev->passed_packets,
			 lan_rd(lan9645x, QSYS_CNT_CMP_PS(cs_id)));
	lan9645x_upd_cnt(&cnt->discarded_packets, &prev->discarded_packets,
			 lan_rd(lan9645x, QSYS_CNT_CMP_DC(cs_id)));
	lan9645x_upd_cnt(&cnt->lost_packets, &prev->lost_packets,
			 lan_rd(lan9645x, QSYS_CNT_CMP_LS(cs_id)));
	lan9645x_upd_cnt(&cnt->tagless_packets, &prev->tagless_packets,
			 lan_rd(lan9645x, QSYS_CNT_CMP_TL(cs_id)));
	lan9645x_upd_cnt(&cnt->resets, &prev->resets,
			 lan_rd(lan9645x, QSYS_CNT_CMP_RS(cs_id)));
}

int lan9645x_frer_cs_cnt_get(struct lan9645x_nl_frer *frer,
			     const u16 cs_id,
			     struct mchp_frer_cnt *const cnt)
{
	struct lan9645x *lan9645x = frer->lan9645x;

	ASSERT_RTNL();

	dev_dbg(lan9645x->dev, "cs_id %u\n", cs_id);

	if (cs_id >= ARRAY_SIZE(frer->cs_cfg)) {
		dev_err(lan9645x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(frer->cs_cfg) - 1);
		return -EINVAL;
	}

	/* mutex_lock(&lan9645x->stats_lock); */
	lan9645x_frer_update_cs_stats(frer, cs_id);
	*cnt = frer->cs_cnt[cs_id];
	/* mutex_unlock(&lan9645x->stats_lock); */
	return 0;
}

int lan9645x_frer_cs_cnt_clear(struct lan9645x_nl_frer *frer,
			       const u16 cs_id)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	struct mchp_frer_cnt *cnt;

	dev_dbg(lan9645x->dev, "cs_id %u\n", cs_id);

	if (cs_id >= ARRAY_SIZE(frer->cs_cfg)) {
		dev_err(lan9645x->dev, "Invalid cs_id (%u). Use 0..%u\n",
			cs_id, (u32)ARRAY_SIZE(frer->cs_cfg) - 1);
		return -EINVAL;
	}

	/* mutex_lock(&lan9645x->stats_lock); */
	cnt = &frer->cs_cnt[cs_id];
	/* memset(cnt, 0x00, sizeof(*cnt)); */
	cnt->out_of_order_packets = 0;
	cnt->rogue_packets = 0;
	cnt->passed_packets = 0;
	cnt->discarded_packets = 0;
	cnt->lost_packets = 0;
	cnt->tagless_packets = 0;
	cnt->resets = 0;
	/* mutex_unlock(&lan9645x->stats_lock); */
	return 0;
}

static int get_port_mask(struct net_device *dev1, struct net_device *dev2,
			 u16 *port_mask)
{
	struct lan9645x_port *p1, *p2 = NULL;

	p1 = lan9645x_port_from_netdev(dev1);
	if (IS_ERR_OR_NULL(p1))
		return -ENOTSUPP;

	*port_mask |= BIT(p1->chip_port);

	if (dev2) {
		p2 = lan9645x_port_from_netdev(dev2);
		if (IS_ERR_OR_NULL(p2))
			return -ENOTSUPP;
		*port_mask |= BIT(p2->chip_port);
	}

	return  0;
}

int lan9645x_frer_ms_alloc(struct lan9645x_nl_frer *frer,
			   struct net_device *dev1,
			   struct net_device *dev2,
			   u16 *const ms_id)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	u16 port_mask = 0;
	int err, i;

	ASSERT_RTNL();

	err = get_port_mask(dev1, dev2, &port_mask);
	if (err)
		return err;

	dev_dbg(lan9645x->dev, "ms_id=%u port_mask 0x%x\n", *ms_id, port_mask);

	for (i = 0; i < ARRAY_SIZE(frer->ms_adm); i++) {
		if (!frer->ms_adm[i].port_mask)
			break;  /* Found a free entry */
	}

	if (i >= ARRAY_SIZE(frer->ms_adm))
		return -ENOSPC; /* No more free entries */

	if (hweight16(port_mask) > MCHP_FRER_MAX_PORTS) {
		dev_err(lan9645x->dev, "More than %d ports\n",
			MCHP_FRER_MAX_PORTS);
		return -EINVAL;
	}

	frer->ms_adm[i].port_mask = port_mask; /* Mark as in use */
	*ms_id = i * MCHP_FRER_MAX_PORTS;
	dev_dbg(lan9645x->dev, "ms_id=%u port_mask 0x%x\n", *ms_id, port_mask);
	return 0;
}

static int lan9645x_frer_ms_cfg_update(struct lan9645x_nl_frer *frer, const u16 ms_id)
{
	struct mchp_frer_stream_cfg *c = &frer->ms_cfg[ms_id];
	bool vector = (c->alg == MCHP_FRER_REC_ALG_VECTOR);
	u16 rt = (c->reset_time) ? c->reset_time : 1;
	u8 hlen = (c->hlen < 2) ? 1 : (c->hlen - 1);
	struct lan9645x *lan9645x = frer->lan9645x;

	lan_wr(QSYS_FRER_CFG_MBM_TAKE_NO_SEQUENCE_SET(c->take_no_seq) |
	       QSYS_FRER_CFG_MBM_VECTOR_ALGORITHM_SET(vector) |
	       QSYS_FRER_CFG_MBM_HISTORY_LENGTH_SET(hlen) |
	       QSYS_FRER_CFG_MBM_RESET_TICKS_SET(rt) |
	       QSYS_FRER_CFG_MBM_RESET_SET(1) |
	       QSYS_FRER_CFG_MBM_ENABLE_SET(c->enable) |
	       QSYS_FRER_CFG_MBM_COMPOUND_HANDLE_SET(c->cs_id),
	       lan9645x, QSYS_FRER_CFG_MBM(ms_id));
	return 0;
}

int lan9645x_frer_ms_free(struct lan9645x_nl_frer *frer,
			  const u16 ms_id)
{
	u16 alloc_ix = ms_id / MCHP_FRER_MAX_PORTS;
	struct lan9645x *lan9645x = frer->lan9645x;
	int i;

	dev_dbg(lan9645x->dev, "ms_id %u\n", ms_id);

	if (ms_id % MCHP_FRER_MAX_PORTS ||
	    ms_id >= ARRAY_SIZE(frer->ms_cfg)) {
		dev_err(lan9645x->dev, "Invalid ms_id (%d)\n", ms_id);
		return -EINVAL;
	}

	if (!frer->ms_adm[alloc_ix].port_mask) {
		dev_err(lan9645x->dev, "Unused ms_id (%d)\n", ms_id);
		return -EINVAL;
	}

	frer->ms_adm[alloc_ix].port_mask = 0; /* Mark as free */

	/* Set involved member streams to default values */
	for (i = 0; i < MCHP_FRER_MAX_PORTS; i++) {
		frer->ms_cfg[ms_id + i] = ms_def_cfg;
		lan9645x_frer_ms_cfg_update(frer, ms_id + i);
	}

	/* Unmap all member ports */
	lan_wr(QSYS_FRER_FIRST_FRER_FIRST_MEMBER_SET(0),
	       lan9645x, QSYS_FRER_FIRST(alloc_ix));
	for (i = 0; i < 4; i++) {
		lan_wr(QSYS_FRER_PORT_FRER_EGR_PORT_SET(0xf),
		       lan9645x, QSYS_FRER_PORT(alloc_ix, i));
	}
	return 0;
}

static int lan9645x_frer_show(struct seq_file *m, void *unused)
{
	struct lan9645x_nl_frer *frer = m->private;
	struct lan9645x *lan9645x = frer->lan9645x;
	struct mchp_frer_stream_cfg *s;
	struct mchp_frer_iflow_cfg *f;
	u8 val;
	int i;

	rtnl_lock();
	seq_puts(m, "ISDX config:\n");
	for (i = 0; i < ARRAY_SIZE(frer->iflow_cfg); i++) {
		f = &frer->iflow_cfg[i].frer;
		if (f->ms_enable || f->generation || f->pop || f->split_mask) {
			seq_printf(m, "isdx %d me %d m %u g %d p %d sm 0x%x\n",
				   i, f->ms_enable, f->ms_id, f->generation,
				   f->pop, f->split_mask);
		}
	}
	seq_puts(m, "MS allocation:\n");
	for (i = 0; i < ARRAY_SIZE(frer->ms_adm); i++) {
		val = frer->ms_adm[i].port_mask;
		if (val) {
			seq_printf(m, "ms_id %d pm 0x%x\n",
				   i * MCHP_FRER_MAX_PORTS, val);
		}
	}
	seq_puts(m, "MS config:\n");
	for (i = 0; i < ARRAY_SIZE(frer->ms_cfg); i++) {
		s = &frer->ms_cfg[i];
		if (!s->enable)
			continue;
		seq_printf(m, "ms_id %d alg %s hl %u rt %u tns %d cs %u\n",
			   i,
			   (s->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M",
			   s->hlen, s->reset_time, s->take_no_seq, s->cs_id);
	}
	seq_puts(m, "CS config:\n");
	for (i = 0; i < ARRAY_SIZE(frer->cs_cfg); i++) {
		s = &frer->cs_cfg[i];
		if (!s->enable)
			continue;
		seq_printf(m, "cs_id %d alg %s hl %u rt %u tns %d\n",
			   i,
			   (s->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M",
			   s->hlen, s->reset_time, s->take_no_seq);
	}
	seq_puts(m, "VLAN config:\n");
	for (i = 0; i < ARRAY_SIZE(lan9645x->vlan_flags); i++) {
		if (!lan9645x->vlan_flags[i])
			continue;
		seq_printf(m, "vid %d flags 0x%x fd %d ld %d\n",
			   i, lan9645x->vlan_flags[i],
			   !!(lan9645x->vlan_flags[i] & LAN9645X_VLAN_FLOOD_DIS),
			   !!(lan9645x->vlan_flags[i] &
			      LAN9645X_VLAN_LEARN_DISABLED));
	}
	rtnl_unlock();
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan9645x_frer);

int lan9645x_frer_init(struct lan9645x_nl_frer *frer)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	u64 val;
	int port;

	/* Set FRER TicksPerSecond to 1000 */
	/* Formular: val = ClockFrequency / (TicsPerSecond * 8 * 512) */
	val = 1000000000000;
	do_div(val, lan9645x_ptp_get_period_ps());
	do_div(val, 1000 * 8 * 512);
	lan_wr(QSYS_FRER_CFG_WATCHDOG_PRESCALER_SET(val),
	       lan9645x, QSYS_FRER_CFG);

	/* Enable R-tag parsing and 48-bit R-tagging for all ports */
	lan9645x_for_each_chipport(lan9645x, port) {
		/* TODO: this breaks hsr/prp. Enable this dynamically on ports where frer is used */
		lan_rmw(ANA_PORT_MODE_REDTAG_PARSE_CFG_SET(1),
			ANA_PORT_MODE_REDTAG_PARSE_CFG,
			lan9645x, ANA_PORT_MODE(port));

		lan_rmw(DEV_PORT_MISC_RTAG48_ENA_SET(1),
			DEV_PORT_MISC_RTAG48_ENA,
			lan9645x, DEV_PORT_MISC(port));
	}

	/* Get default cstream register content */
	val = lan_rd(lan9645x, QSYS_FRER_CFG_CMP(0));
	cs_def_cfg.enable = QSYS_FRER_CFG_CMP_ENABLE_GET(val);
	cs_def_cfg.alg = QSYS_FRER_CFG_CMP_VECTOR_ALGORITHM_GET(val) ?
		MCHP_FRER_REC_ALG_VECTOR :
		MCHP_FRER_REC_ALG_MATCH;
	cs_def_cfg.hlen = QSYS_FRER_CFG_CMP_HISTORY_LENGTH_GET(val) + 1;
	cs_def_cfg.reset_time = QSYS_FRER_CFG_CMP_RESET_TICKS_GET(val);
	cs_def_cfg.take_no_seq = QSYS_FRER_CFG_CMP_TAKE_NO_SEQUENCE_GET(val);

	/* Sync configurstion with default values */
	for (port = 0; port < ARRAY_SIZE(frer->cs_cfg); port++)
		frer->cs_cfg[port] = cs_def_cfg;

	/* Get default mstream register content */
	val = lan_rd(lan9645x, QSYS_FRER_CFG_MBM(0));
	val = lan_rd(lan9645x, QSYS_FRER_CFG_CMP(0));
	ms_def_cfg.enable = QSYS_FRER_CFG_CMP_ENABLE_GET(val);
	ms_def_cfg.alg = QSYS_FRER_CFG_CMP_VECTOR_ALGORITHM_GET(val) ?
		MCHP_FRER_REC_ALG_VECTOR :
		MCHP_FRER_REC_ALG_MATCH;
	ms_def_cfg.hlen = QSYS_FRER_CFG_CMP_HISTORY_LENGTH_GET(val) + 1;
	ms_def_cfg.reset_time = QSYS_FRER_CFG_CMP_RESET_TICKS_GET(val);
	ms_def_cfg.take_no_seq = QSYS_FRER_CFG_CMP_TAKE_NO_SEQUENCE_GET(val);
	ms_def_cfg.cs_id = 0;

	/* Sync configurstion with default values */
	for (port = 0; port < ARRAY_SIZE(frer->ms_cfg); port++)
		frer->ms_cfg[port] = ms_def_cfg;

	/* Always apply split mask even if destination port set is empty */
	lan_rmw(ANA_AGENCTRL_APPLY_SPLIT_MASK_SET(1),
		ANA_AGENCTRL_APPLY_SPLIT_MASK,
		lan9645x, ANA_AGENCTRL);

	/* Enable FRER */
	lan_wr(QSYS_MISC_DROP_CFG_FRER_ENA_SET(1),
	       lan9645x, QSYS_MISC_DROP_CFG);

	debugfs_create_file("frer_show", 0444, lan9645x->debugfs_root, frer,
			    &lan9645x_frer_fops);
	return 0;
}

/* Check if ms_id is within limits and if port is part of member stream */
/* Returns port index or negative error code */
static int lan9645x_frer_ms_check(struct lan9645x_nl_frer *frer,
				  struct lan9645x_port *port,
				  const u16 ms_id)
{
	u16 alloc_ix = ms_id / MCHP_FRER_MAX_PORTS;
	struct lan9645x *lan9645x = frer->lan9645x;
	unsigned long port_mask;
	u8 chip_port;
	int ix = 0;

	ASSERT_RTNL();

	if (alloc_ix >= ARRAY_SIZE(frer->ms_adm) ||
	    ms_id % MCHP_FRER_MAX_PORTS ||
	    ms_id >= ARRAY_SIZE(frer->ms_cfg)) {
		dev_err(lan9645x->dev, "Invalid ms_id (%d). Use even numbers from 0 to %u\n",
			ms_id, (u32)ARRAY_SIZE(frer->ms_adm) - 2);
		return -EINVAL;
	}

	port_mask = frer->ms_adm[alloc_ix].port_mask;

	/* Check if port is part of member stream */
	if (!(port_mask & BIT(port->chip_port))) {
		dev_err(lan9645x->dev, "Port is not member of ms_id %d\n",
			ms_id);
		return -EINVAL;
	}

	for_each_set_bit(chip_port, &port_mask, 8) {
		if (chip_port == port->chip_port)
			break;
		ix++;
	}

	if (ix >= MCHP_FRER_MAX_PORTS) {
		dev_err(lan9645x->dev, "Invalid port_mask 0x%02lx\n", port_mask);
		return -EINVAL;
	}

	return ix;
}

int lan9645x_frer_ms_cfg_get(struct lan9645x_nl_frer *frer,
			     struct net_device *dev, const u16 ms_id,
			     struct mchp_frer_stream_cfg *const cfg)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	struct lan9645x_port *p;
	int i;

	ASSERT_RTNL();
	dev_dbg(lan9645x->dev, "dev %s ms_id %u\n", dev->name, ms_id);

	p = lan9645x_port_from_netdev(dev);
	if (!p)
		return -ENOTSUPP;

	i = lan9645x_frer_ms_check(frer, p, ms_id);
	if (i < 0)
		return i;

	*cfg = frer->ms_cfg[ms_id + i];
	return 0;
}

int lan9645x_frer_ms_cfg_set(struct lan9645x_nl_frer *frer,
			     struct net_device *dev, const u16 ms_id,
			     struct mchp_frer_stream_cfg *const cfg)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	struct lan9645x_port *p;
	int i;

	ASSERT_RTNL();
	dev_dbg(lan9645x->dev,
		"dev %s ms_id %u e %d a %s h %d r %d t %d cs %d\n", dev->name,
		ms_id, cfg->enable,
		(cfg->alg == MCHP_FRER_REC_ALG_VECTOR) ? "V" : "M", cfg->hlen,
		cfg->reset_time, cfg->take_no_seq, cfg->cs_id);

	p = lan9645x_port_from_netdev(dev);
	if (!p)
		return -ENOTSUPP;

	if (cfg->alg != MCHP_FRER_REC_ALG_VECTOR &&
	    cfg->alg != MCHP_FRER_REC_ALG_MATCH) {
		dev_err(lan9645x->dev, "Invalid alg (%d). Use %d for vector and %d for match\n",
			cfg->alg, MCHP_FRER_REC_ALG_VECTOR,
			MCHP_FRER_REC_ALG_MATCH);
		return -EINVAL;
	}

	if (cfg->hlen < LAN9645X_FRER_HLEN_MIN ||
	    cfg->hlen > LAN9645X_FRER_HLEN_MAX) {
		dev_err(lan9645x->dev, "Invalid hlen (%d). Use %d..%d\n",
			cfg->hlen, LAN9645X_FRER_HLEN_MIN, LAN9645X_FRER_HLEN_MAX);
		return -EINVAL;
	}

	if (cfg->reset_time < LAN9645X_FRER_RESET_MIN ||
	    cfg->reset_time > LAN9645X_FRER_RESET_MAX) {
		dev_err(lan9645x->dev, "Invalid reset_time (%d). Use %d..%d\n",
			cfg->reset_time, LAN9645X_FRER_RESET_MIN,
			LAN9645X_FRER_RESET_MAX);
		return -EINVAL;
	}

	if (cfg->cs_id >= LAN9645X_FRER_NUM_CSI) {
		dev_err(lan9645x->dev, "Invalid cs_id (%d). Use 0..%d\n",
			cfg->cs_id, LAN9645X_FRER_NUM_CSI - 1);
		return -EINVAL;
	}

	i = lan9645x_frer_ms_check(frer, p, ms_id);
	if (i < 0)
		return i;

	frer->ms_cfg[ms_id + i] = *cfg;
	return lan9645x_frer_ms_cfg_update(frer, ms_id + i);
}

/* Update FRER member stream counters */
static void lan9645x_frer_update_ms_stats(struct lan9645x_nl_frer *frer,
					  const u16 ms_id)
{
	struct lan9645x_frer_prev_cnt *prev = &frer->ms_prev_cnt[ms_id];
	struct mchp_frer_cnt *cnt = &frer->ms_cnt[ms_id];
	struct lan9645x *lan9645x = frer->lan9645x;

	/* WARN_ON(!mutex_is_locked(&lan9645x->stats_lock)); */

	lan9645x_upd_cnt(&cnt->out_of_order_packets, &prev->out_of_order_packets,
			 lan_rd(lan9645x, QSYS_CNT_MBM_OO(ms_id)));
	lan9645x_upd_cnt(&cnt->rogue_packets, &prev->rogue_packets,
			 lan_rd(lan9645x, QSYS_CNT_MBM_RG(ms_id)));
	lan9645x_upd_cnt(&cnt->passed_packets, &prev->passed_packets,
			 lan_rd(lan9645x, QSYS_CNT_MBM_PS(ms_id)));
	lan9645x_upd_cnt(&cnt->discarded_packets, &prev->discarded_packets,
			 lan_rd(lan9645x, QSYS_CNT_MBM_DC(ms_id)));
	lan9645x_upd_cnt(&cnt->lost_packets, &prev->lost_packets,
			 lan_rd(lan9645x, QSYS_CNT_MBM_LS(ms_id)));
	lan9645x_upd_cnt(&cnt->tagless_packets, &prev->tagless_packets,
			 lan_rd(lan9645x, QSYS_CNT_MBM_TL(ms_id)));
	lan9645x_upd_cnt(&cnt->resets, &prev->resets,
			 lan_rd(lan9645x, QSYS_CNT_MBM_RS(ms_id)));
}

int lan9645x_frer_ms_cnt_get(struct lan9645x_nl_frer *frer,
			     struct net_device *dev, const u16 ms_id,
			     struct mchp_frer_cnt *const cnt)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	struct lan9645x_port *p;
	u16 id;
	int i;

	dev_dbg(lan9645x->dev, "dev %s ms_id %u\n", dev->name, ms_id);

	p = lan9645x_port_from_netdev(dev);
	if (!p)
		return -ENOTSUPP;

	i = lan9645x_frer_ms_check(frer, p, ms_id);
	if (i < 0)
		return i;

	id = ms_id + i;
	/* mutex_lock(&lan9645x->stats_lock); */
	lan9645x_frer_update_ms_stats(frer, id);
	*cnt = frer->ms_cnt[id];
	/* mutex_unlock(&lan9645x->stats_lock); */
	return 0;
}

int lan9645x_frer_ms_cnt_clear(struct lan9645x_nl_frer *frer,
			       struct net_device *dev, const u16 ms_id)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	struct mchp_frer_cnt *cnt;
	struct lan9645x_port *p;
	u16 id;
	int i;

	dev_dbg(lan9645x->dev, "dev %s ms_id %u\n", dev->name, ms_id);

	p = lan9645x_port_from_netdev(dev);
	if (!p)
		return -ENOTSUPP;

	i = lan9645x_frer_ms_check(frer, p, ms_id);
	if (i < 0)
		return i;

	id = ms_id + i;
	/* mutex_lock(&lan9645x->stats_lock); */
	cnt = &frer->ms_cnt[id];
	cnt->out_of_order_packets = 0;
	cnt->rogue_packets = 0;
	cnt->passed_packets = 0;
	cnt->discarded_packets = 0;
	cnt->lost_packets = 0;
	cnt->tagless_packets = 0;
	cnt->resets = 0;
	/* mutex_unlock(&lan9645x->stats_lock); */
	return 0;
}

static int lan9645x_frer_iflow_update(struct lan9645x_nl_frer *frer, const u16 isdx)
{
	struct mchp_frer_iflow_cfg *c = &frer->iflow_cfg[isdx].frer;
	u16 alloc_ix = c->ms_id / MCHP_FRER_MAX_PORTS;
	struct lan9645x *lan9645x = frer->lan9645x;
	struct lan9645x_streamt_entry entry = {};
	unsigned long port_mask;
	u8 chip_port;
	int i, err;

	entry.split_mask = c->split_mask;
	entry.input_port_mask = GENMASK(CPU_PORT, 0);
	entry.stream_split = !!c->split_mask;

	entry.gen_seq_num = 0;
	entry.rtag_pop_ena = !!c->pop;
	entry.seq_gen_ena = !!c->generation;

	err = lan9645x_streamt_write(lan9645x, isdx, &entry);
	if (err)
		return err;

	if (c->ms_enable) {
		port_mask = frer->ms_adm[alloc_ix].port_mask;
		/* Map all member ports */
		lan_wr(QSYS_FRER_FIRST_FRER_FIRST_MEMBER_SET(c->ms_id),
		       lan9645x, QSYS_FRER_FIRST(isdx));
		i = 0;
		for_each_set_bit(chip_port, &port_mask, NUM_PHYS_PORTS) {
			lan_wr(QSYS_FRER_PORT_FRER_EGR_PORT_SET(chip_port),
			       lan9645x, QSYS_FRER_PORT(isdx, i));
			i++;
		}
		for (; i < 4; i++) { /* Disable FRER for the rest */
			lan_wr(QSYS_FRER_PORT_FRER_EGR_PORT_SET(0xf),
			       lan9645x, QSYS_FRER_PORT(isdx, i));
		}
	}

	return 0;
}

int lan9645x_iflow_cfg_set(struct lan9645x_nl_frer *frer,
			   struct net_device *dev1,
			   struct net_device *dev2,
			   const u16 isdx,
			   struct mchp_iflow_cfg *cfg)
{
	struct lan9645x *lan9645x = frer->lan9645x;
	int err;

	err = get_port_mask(dev1, dev2, &cfg->frer.split_mask);
	if (err)
		return err;

	dev_dbg(lan9645x->dev, "id %d me %d m %u g %d p %d sm 0x%x\n",
		isdx, cfg->frer.ms_enable, cfg->frer.ms_id, cfg->frer.generation,
		cfg->frer.pop, cfg->frer.split_mask);

	if (isdx < LAN9645X_FRER_FLOW_MIN ||
	    isdx >= ARRAY_SIZE(frer->iflow_cfg)) {
		dev_err(lan9645x->dev, "Invalid id (%u). Use %d..%u\n",
			isdx, LAN9645X_FRER_FLOW_MIN,
			(u32)ARRAY_SIZE(frer->iflow_cfg) - 1);
		return -EINVAL;
	}

	if (cfg->frer.ms_id >= LAN9645X_FRER_NUM_MSI) {
		dev_err(lan9645x->dev, "Invalid ms_id (%u). Use 0..%d\n",
			cfg->frer.ms_id, LAN9645X_FRER_NUM_MSI - 1);
		return -EINVAL;
	}

	if (cfg->frer.ms_enable && cfg->frer.generation) {
		dev_err(lan9645x->dev,
			"Cannot have member stream together with generation\n");
		return -EINVAL;
	}

	if (cfg->frer.generation && cfg->frer.pop) {
		dev_err(lan9645x->dev,
			"Cannot have generation together with pop\n");
		return -EINVAL;
	}

	if (hweight8(cfg->frer.split_mask) > MCHP_FRER_MAX_PORTS) {
		dev_err(lan9645x->dev, "Cannot have more than %d ports\n",
			MCHP_FRER_MAX_PORTS);
		return -EINVAL;
	}

	frer->iflow_cfg[isdx] = *cfg;
	return lan9645x_frer_iflow_update(frer, isdx);
}

int lan9645x_iflow_cfg_get(struct lan9645x_nl_frer *frer,
			   const u16 id,
			   struct mchp_iflow_cfg *const cfg)
{
	struct lan9645x *lan9645x  = frer->lan9645x;

	ASSERT_RTNL();
	dev_dbg(lan9645x->dev, "id %u\n", id);

	if (id < LAN9645X_FRER_FLOW_MIN ||
	    id >= ARRAY_SIZE(frer->iflow_cfg)) {
		dev_err(lan9645x->dev, "Invalid id (%u). Use %d..%u\n",
			id, LAN9645X_FRER_FLOW_MIN,
			(u32)ARRAY_SIZE(frer->iflow_cfg) - 1);
		return -EINVAL;
	}

	*cfg = frer->iflow_cfg[id];
	return 0;
}

int lan9645x_frer_vlan_cfg_get(struct lan9645x_nl_frer *frer,
			       const u16 vid,
			       struct mchp_frer_vlan_cfg *const cfg)
{
	struct lan9645x *lan9645x = frer->lan9645x;

	ASSERT_RTNL();
	dev_dbg(lan9645x->dev, "vid %u\n", vid);

	if (vid >= ARRAY_SIZE(lan9645x->vlan_flags)) {
		dev_err(lan9645x->dev, "Invalid vid (%u). Use 0..%u\n",
			vid, (u32)ARRAY_SIZE(lan9645x->vlan_flags) - 1);
		return -EINVAL;
	}

	cfg->flood_disable =
		!!(lan9645x->vlan_flags[vid] & LAN9645X_VLAN_FLOOD_DIS);
	cfg->learn_disable =
		!!(lan9645x->vlan_flags[vid] & LAN9645X_VLAN_LEARN_DISABLED);
	return 0;
}

int lan9645x_frer_vlan_cfg_set(struct lan9645x_nl_frer *frer,
			       const u16 vid,
			       const struct mchp_frer_vlan_cfg *const cfg)
{
	struct lan9645x *lan9645x = frer->lan9645x;

	ASSERT_RTNL();
	dev_dbg(lan9645x->dev, "vid %u fd %d ld %d\n",
		vid, cfg->flood_disable, cfg->learn_disable);

	if (vid >= ARRAY_SIZE(lan9645x->vlan_flags)) {
		dev_err(lan9645x->dev, "Invalid vid (%u). Use 0..%u\n",
			vid, (u32)ARRAY_SIZE(lan9645x->vlan_flags) - 1);
		return -EINVAL;
	}

	if (cfg->flood_disable)
		lan9645x->vlan_flags[vid] |= LAN9645X_VLAN_FLOOD_DIS;
	else
		lan9645x->vlan_flags[vid] &= ~LAN9645X_VLAN_FLOOD_DIS;

	if (cfg->learn_disable)
		lan9645x->vlan_flags[vid] |= LAN9645X_VLAN_LEARN_DISABLED;
	else
		lan9645x->vlan_flags[vid] &= ~LAN9645X_VLAN_LEARN_DISABLED;

	lan9645x_vlan_set_mask(lan9645x, vid);
	return 0;
}
