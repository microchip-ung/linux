// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/dev_printk.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/types.h>

#include "core.h"
#include "synth.h"

/**
 * zl3073x_synth_state_fetch - get synth state
 * @zldev: pointer to zl3073x_dev structure
 * @index: synth index to fetch state for
 *
 * Function fetches information for the given synthesizer that are
 * invariant and stores them for later use.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_synth_state_fetch(struct zl3073x_dev *zldev, u8 index)
{
	struct zl3073x_synth *synth = &zldev->synth[index];
	int rc;

	/* Read synth control register */
	rc = zl3073x_read_u8(zldev, ZL_REG_SYNTH_CTRL(index), &synth->ctrl);
	if (rc)
		return rc;

	guard(mutex)(&zldev->multiop_lock);

	/* Read synth configuration */
	rc = zl3073x_mb_op(zldev, ZL_REG_SYNTH_MB_SEM, ZL_SYNTH_MB_SEM_RD,
			   ZL_REG_SYNTH_MB_MASK, BIT(index));
	if (rc)
		return rc;

	/* The output frequency is determined by the following formula:
	 * base * multiplier * numerator / denominator
	 *
	 * Read registers with these values
	 */
	rc = zl3073x_read_u16(zldev, ZL_REG_SYNTH_FREQ_BASE, &synth->freq_base);
	if (rc)
		return rc;

	rc = zl3073x_read_u32(zldev, ZL_REG_SYNTH_FREQ_MULT, &synth->freq_mult);
	if (rc)
		return rc;

	rc = zl3073x_read_u16(zldev, ZL_REG_SYNTH_FREQ_M, &synth->freq_m);
	if (rc)
		return rc;

	rc = zl3073x_read_u16(zldev, ZL_REG_SYNTH_FREQ_N, &synth->freq_n);
	if (rc)
		return rc;

	/* Check denominator for zero to avoid div by 0 */
	if (!synth->freq_n) {
		dev_err(zldev->dev,
			"Zero divisor for SYNTH%u retrieved from device\n",
			index);
		return -EINVAL;
	}

	dev_dbg(zldev->dev, "SYNTH%u frequency: %u Hz\n", index,
		zl3073x_synth_freq_get(synth));

	return rc;
}

const struct zl3073x_synth *zl3073x_synth_state_get(struct zl3073x_dev *zldev,
						    u8 index)
{
	return index < ZL3073X_NUM_SYNTHS ? &zldev->synth[index] : NULL;
}
