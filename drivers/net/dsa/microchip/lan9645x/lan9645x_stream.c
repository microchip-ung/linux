// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#include "lan9645x_main.h"
#include "lan9645x_stats.h"

/* The STREAM_TBL_CMD must be IDLE before a new command can be issued. The INIT
 * command run for approximately 50 us whereas the other commands execute
 * immediately. When an operation has completed, STREAM_TBL_CMD changes to IDLE.
 */
#define STREAMACCESS_CMD_IDLE		0
#define STREAMACCESS_CMD_READ		1
#define STREAMACCESS_CMD_WRITE		2
#define STREAMACCESS_CMD_INIT		3

#define CMD_IS_IDLE(val)						\
	(ANA_STREAMACCESS_STREAM_TBL_CMD_GET((val)) == STREAMACCESS_CMD_IDLE)

static int lan9645x_streamt_wait_for_completion(struct lan9645x *lan9645x)
{
	u32 val;

	return lan9645x_rd_poll_timeout(lan9645x, ANA_STREAMACCESS, val,
					CMD_IS_IDLE(val));
}

static int __lan9645x_stream_isdx_alloc(struct lan9645x *lan9645x)
{
	int isdx;

	lockdep_assert_held(&lan9645x->stream->lock);

	isdx = find_first_zero_bit(lan9645x->stream->isdx_mask,
				   LAN9645X_ISDX_MAX);
	if (isdx >= LAN9645X_ISDX_MAX)
		return -ENOSPC;

	set_bit(isdx, lan9645x->stream->isdx_mask);

	return isdx;
}

int lan9645x_stream_isdx_alloc(struct lan9645x *lan9645x)
{
	int ret;

	mutex_lock(&lan9645x->stream->lock);
	ret = __lan9645x_stream_isdx_alloc(lan9645x);
	mutex_unlock(&lan9645x->stream->lock);

	return ret;
}

static void __lan9645x_stream_isdx_free(struct lan9645x *lan9645x, u16 isdx)
{
	lockdep_assert_held(&lan9645x->stream->lock);

	clear_bit(isdx, lan9645x->stream->isdx_mask);
	lan9645x_stats_clear_counters(lan9645x, LAN9645X_STAT_ISDX, isdx);
}

void lan9645x_stream_isdx_free(struct lan9645x *lan9645x, u16 isdx)
{
	mutex_lock(&lan9645x->stream->lock);
	__lan9645x_stream_isdx_free(lan9645x, isdx);
	mutex_unlock(&lan9645x->stream->lock);
}

static int __lan9645x_streamt_read(struct lan9645x *lan9645x, u16 isdx,
				   struct lan9645x_streamt_entry *entry)
{
	u32 tidx, access;
	int err;

	lockdep_assert_held(&lan9645x->stream->lock);

	lan_wr(ANA_STREAMTIDX_S_INDEX_SET(isdx), lan9645x, ANA_STREAMTIDX);
	lan_wr(STREAMACCESS_CMD_READ, lan9645x, ANA_STREAMACCESS);

	err = lan9645x_streamt_wait_for_completion(lan9645x);
	if (err) {
		dev_err(lan9645x->dev, "stream table access err: %d", err);
		return err;
	}

	tidx = lan_rd(lan9645x, ANA_STREAMTIDX);
	access = lan_rd(lan9645x, ANA_STREAMACCESS);

	entry->split_mask = lan_rd(lan9645x, ANA_SPLIT_MASK);
	entry->input_port_mask = lan_rd(lan9645x, ANA_INPUT_PORT_MASK);
	entry->time_last_seen = lan_rd(lan9645x, ANA_STREAM_TIME);

	entry->isdx = isdx;
	entry->seq_gen_err_status = ANA_STREAMTIDX_SEQ_GEN_ERR_STATUS_GET(tidx);
	entry->stream_split = ANA_STREAMTIDX_STREAM_SPLIT_GET(tidx);

	entry->gen_seq_num = ANA_STREAMACCESS_GEN_SEQ_NUM_GET(access);
	entry->rtag_pop_ena = ANA_STREAMACCESS_RTAG_POP_ENA_GET(access);
	entry->seq_gen_ena = ANA_STREAMACCESS_SEQ_GEN_ENA_GET(access);

	return 0;
}

int lan9645x_streamt_read(struct lan9645x *lan9645x, u16 isdx,
			  struct lan9645x_streamt_entry *entry)
{
	int ret;

	mutex_lock(&lan9645x->stream->lock);
	ret = __lan9645x_streamt_read(lan9645x, isdx, entry);
	mutex_unlock(&lan9645x->stream->lock);

	return ret;
}

static int __lan9645x_streamt_write(struct lan9645x *lan9645x, u16 isdx,
				    struct lan9645x_streamt_entry *entry)
{
	int err;

	lockdep_assert_held(&lan9645x->stream->lock);

	lan_wr(entry->split_mask, lan9645x, ANA_SPLIT_MASK);
	lan_wr(entry->input_port_mask, lan9645x, ANA_INPUT_PORT_MASK);

	lan_wr(ANA_STREAMTIDX_S_INDEX_SET(isdx) |
	       ANA_STREAMTIDX_STREAM_SPLIT_SET(entry->stream_split),
	       lan9645x, ANA_STREAMTIDX);

	lan_wr(ANA_STREAMACCESS_GEN_SEQ_NUM_SET(entry->gen_seq_num) |
	       ANA_STREAMACCESS_RTAG_POP_ENA_SET(entry->rtag_pop_ena) |
	       ANA_STREAMACCESS_SEQ_GEN_ENA_SET(entry->seq_gen_ena) |
	       ANA_STREAMACCESS_STREAM_TBL_CMD_SET(STREAMACCESS_CMD_WRITE),
	       lan9645x, ANA_STREAMACCESS);

	err = lan9645x_streamt_wait_for_completion(lan9645x);
	if (err) {
		dev_err(lan9645x->dev, "stream table write err: %d", err);
		return err;
	}

	return 0;
}

int lan9645x_streamt_write(struct lan9645x *lan9645x, u16 isdx,
			   struct lan9645x_streamt_entry *entry)
{
	int ret;

	mutex_lock(&lan9645x->stream->lock);
	ret = __lan9645x_streamt_write(lan9645x, isdx, entry);
	mutex_unlock(&lan9645x->stream->lock);

	return ret;
}

int lan9645x_streamt_del(struct lan9645x *lan9645x, u16 isdx)
{
	struct lan9645x_streamt_entry entry = { 0 };

	/* TODO: this is not really needed, since without ISDX classification,
	 * entries are not used. New writes just have to make sure to set all
	 * fields.
	 */

	/* Default table values are 0. */
	return lan9645x_streamt_write(lan9645x, isdx, &entry);
}

int lan9645x_streamt_init(struct lan9645x *lan9645x)
{
	struct lan9645x_stream *stream;
	int err;

	stream = devm_kzalloc(lan9645x->dev, sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;

	lan_wr(STREAMACCESS_CMD_INIT, lan9645x, ANA_STREAMACCESS);
	err = lan9645x_streamt_wait_for_completion(lan9645x);
	if (err)
		return err;

	mutex_init(&stream->lock);

	lan9645x->stream = stream;

	/* Set current time used by ISDX.
	 * Ticks per second = system freq / (prescaler * 4096)
	 *
	 * ASIC: 165.625 Mhz
	 * FPGA: 66.125 Mhz
	 *
	 * System freq: X Mhz
	 * Desired period: y ms
	 *
	 * Then prescaler = X * y * 1000 / 4096
	 *
	 * Datasheet recommends period of 10-100ms is sufficient to allow SW to
	 * read stream table entries every few seconds or so.
	 *
	 * 50 ms ~ p=2021 (ASIC)
	 */
	lan_rmw(ANA_TIME_CFG_PRESCALER_SET(2021),
		ANA_TIME_CFG_PRESCALER,
		lan9645x, ANA_TIME_CFG);

	/* NOTE: isdx 0 is not generally usable and is interpreted as no-isdx. */
	set_bit(0, lan9645x->stream->isdx_mask);

	return 0;
}

void lan9645x_streamt_deinit(struct lan9645x *lan9645x)
{
	mutex_destroy(&lan9645x->stream->lock);
}
