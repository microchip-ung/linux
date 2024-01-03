// SPDX-License-Identifier: GPL-2.0+
/* Microchip lan969x Switch driver
 *
 * Copyright (c) 2024 Microchip Technology Inc. and its subsidiaries.
 */

#include "lan969x.h"

#define LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI 10
#define LAN969X_DSM_CAL_TAXIS 5
#define LAN969X_DSM_CAL_LEN SPX5_DSM_CAL_LEN
#define LAN969X_DSM_CAL_SLOT_UNUSED LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI

enum lan969x_dsm_cal_dev {
	DSM_CAL_DEV_2G5,
	DSM_CAL_DEV_5G,
	DSM_CAL_DEV_10G,
	DSM_CAL_DEV_OTHER, /* 1G or less */
	DSM_CAL_DEV_MAX
};

/* Each entry in the following struct defines properties for a given speed
 * (10G, 5G, 2.5G, or 1G or less).
 */
struct lan969x_dsm_cal_dev_speed {
	/* Number of devices that requires this speed */
	u32 dev_cnt;

	/* List of devices that requires this speed. Only first 'dev_cnt' are
	 * valid.
	 */
	u32 devs[LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI];

	/* Number of slots required for one device running this speed */
	u32 slots_required;

	/* Number of slots between two slots for one device running this speed. */
	u32 slots_between_repeats;
};

static int lan969x_dsm_cal_idx_find_next_free(u32 taxi, u32 *calendar,
					      u32 cal_len, u32 *cal_idx,
					      u32 dev)
{
	if (*cal_idx >= cal_len) {
		pr_err("Taxi %u, dev %u: cal_idx (%u) >= cal_len (%u) on entry to function",
		       taxi, dev, *cal_idx, cal_len);
		return -EINVAL;
	}

	do {
		if (calendar[*cal_idx] == LAN969X_DSM_CAL_SLOT_UNUSED)
			return 0;

		(*cal_idx)++;
	} while (*cal_idx < cal_len);

	pr_err("Taxi %u, dev %u: No free entries found in calendar of length %u",
	       taxi, dev, cal_len);

	return -ENOENT;
}

static enum lan969x_dsm_cal_dev lan969x_dsm_cal_get_dev(int speed)
{
	return (speed == 10000 ? DSM_CAL_DEV_10G :
		speed == 5000  ? DSM_CAL_DEV_5G :
		speed == 2500  ? DSM_CAL_DEV_2G5 :
				 DSM_CAL_DEV_OTHER);
}

static int lan969x_dsm_cal_get_speed(enum lan969x_dsm_cal_dev dev)
{
	return (dev == DSM_CAL_DEV_10G ? 10000 :
		dev == DSM_CAL_DEV_5G  ? 5000 :
		dev == DSM_CAL_DEV_2G5 ? 2500 :
					 1000);
}

static void lan969x_dsm_cal_print(struct lan969x_dsm_cal_dev_speed *speeds)
{
	for (int idx = 0; idx < DSM_CAL_DEV_MAX; idx++) {
		struct lan969x_dsm_cal_dev_speed *speed = &speeds[idx];
		char buf[LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI * 4];
		int size = 0;

		buf[0] = '\0';
		for (u32 dev = 0; dev < speed->dev_cnt; dev++) {
			size += snprintf(buf + size, sizeof(buf) - size, " %u ",
					 speed->devs[dev]);
		}

		pr_debug("Speed = %5u, dev_cnt = %u, slots_required = %u, slots_between_repeats = %u, devs = %s",
			 lan969x_dsm_cal_get_speed(idx), speed->dev_cnt,
			 speed->slots_required, speed->slots_between_repeats,
			 buf);
	}
}

int lan969x_dsm_calendar_calc(struct sparx5 *sparx5, u32 taxi,
			      struct sparx5_calendar_data *data,
			      u32 *calendar_len)
{
	u32 required_bw  = 0, active_dev_cnt = 0, delay = 0, bw_per_slot = 0;
	struct lan969x_dsm_cal_dev_speed dev_speed[DSM_CAL_DEV_MAX] = {}, *d;
	const struct sparx5_consts *consts = &sparx5->data->consts;
	u32 cal_len, speed, idx, cal_idx, slots_required, taxi_bw;
	bool works;

	/* Maximum bandwidth for this taxi */
	taxi_bw = ((128 * 1000000) / sparx5_clk_period(sparx5->coreclock)) /
		  (1 + 1 / 20);

	memcpy(data->taxi_ports, sparx5->data->ops.get_taxi(taxi),
	       LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI * sizeof(u32));

	for (int i = 0; i < LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI; i++) {
		u32 portno = data->taxi_ports[i];

		if (portno < consts->chip_ports_all)
			data->taxi_speeds[i] = sparx5_cal_speed_to_value(sparx5_get_port_cal_speed(sparx5, portno));
		else
			data->taxi_speeds[i] = 0;
	}

	/* Determine the different port types (10G, 5G, 2.5G, <= 1G) in the
	 * this taxi map
	 */
	for (u32 dev = 0; dev < LAN969X_DSM_CAL_MAX_DEVS_PER_TAXI; dev++) {
		speed = data->taxi_speeds[dev];

		if (speed == 0)
			continue;

		required_bw += speed;

		idx = lan969x_dsm_cal_get_dev(speed);
		d = &dev_speed[idx];
		d->devs[d->dev_cnt++] = dev;
		active_dev_cnt++;
	}

	pr_debug("Required bandwitdh: %u, total taxi (%u) bandwidth: %u",
		 required_bw, taxi, taxi_bw);

	if (required_bw > taxi_bw) {
		pr_err("Required bandwitdh: %u is higher than total taxi (%u) bandwidth: %u",
		       required_bw, taxi, taxi_bw);
		return -EINVAL;
	}

	if (active_dev_cnt == 0) {
		*calendar_len = 1;
		data->schedule[0] = LAN969X_DSM_CAL_SLOT_UNUSED;
		return 0;
	}

	/* The calendar needs at least one slot per device. */
	cal_len = active_dev_cnt;

	/* And it needs to be at least one longer than the delay. */
	if (cal_len < delay)
		cal_len = delay + 1;

	/* Search for a calendar length that fits all active devices. */
	while (cal_len < LAN969X_DSM_CAL_LEN) {
		/* Use truncating division here. */
		bw_per_slot = taxi_bw / cal_len;

		slots_required = 0;
		works = true;
		for (idx = 0; idx < DSM_CAL_DEV_MAX; idx++) {
			d = &dev_speed[idx];

			if (d->dev_cnt == 0)
				continue;

			required_bw = lan969x_dsm_cal_get_speed(idx);

			d->slots_required =
				DIV_ROUND_UP(required_bw, bw_per_slot);

			if (d->slots_required) {
				d->slots_between_repeats = DIV_ROUND_UP(cal_len, d->slots_required);
				/* Delay and slots_between_repeats may not be the same. */
				if (d->slots_between_repeats == delay) {
					/* Calendar length doesn't work. */
					cal_len++;
					works = false;
					break;
				}

				slots_required +=
					d->dev_cnt * d->slots_required;
			} else {
				d->slots_between_repeats = 0;
			}
		}

		if (!works)
			continue;

		if (slots_required <= cal_len)
			break; /* Found a suitable calendar length. */

		/* Not good enough yet. */
		cal_len = slots_required;
	}

	if (cal_len > LAN969X_DSM_CAL_LEN) {
		pr_err("Invalid length: %u for taxi: %u", cal_len, taxi);
		return -EINVAL;
	}

	lan969x_dsm_cal_print(dev_speed);

	for (cal_idx = 0; cal_idx < cal_len; cal_idx++)
		data->schedule[cal_idx] = LAN969X_DSM_CAL_SLOT_UNUSED;

	/* Place the remaining devices. Start with the fastest. */
	for (idx = 0; idx < DSM_CAL_DEV_MAX; idx++) {
		d = &dev_speed[idx];
		for (u32 dev = 0; dev < d->dev_cnt; dev++) {
			cal_idx = 0;
			for (slots_required = 0;
			     slots_required < d->slots_required;
			     slots_required++) {
				lan969x_dsm_cal_idx_find_next_free(taxi,
								   data->schedule,
								   cal_len,
								   &cal_idx,
								   d->devs[dev]);
				data->schedule[cal_idx] = d->devs[dev];
				cal_idx += d->slots_between_repeats;
			}
		}
	}

	*calendar_len = cal_len;

	return 0;
}
