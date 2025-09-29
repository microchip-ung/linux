// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/dev_printk.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/types.h>

#include "core.h"
#include "ref.h"

/**
 * zl3073x_ref_state_fetch - get input reference state
 * @zldev: pointer to zl3073x_dev structure
 * @index: input reference index to fetch state for
 *
 * Function fetches state for the given input reference and stores it for
 * later user.
 *
 * Return: 0 on success, <0 on error
 */
int zl3073x_ref_state_fetch(struct zl3073x_dev *zldev, u8 index)
{
	struct zl3073x_ref *ref = &zldev->ref[index];
	int rc;

	/* If the input is differential then the configuration for N-pin
	 * reference is ignored and P-pin config is used for both.
	 */
	if (zl3073x_is_n_pin(index) && zl3073x_ref_is_diff(ref - 1)) {
		memcpy(ref, ref - 1, sizeof(*ref));

		return 0;
	}

	guard(mutex)(&zldev->multiop_lock);

	/* Read reference configuration */
	rc = zl3073x_mb_op(zldev, ZL_REG_REF_MB_SEM, ZL_REF_MB_SEM_RD,
			   ZL_REG_REF_MB_MASK, BIT(index));
	if (rc)
		return rc;

	/* Read ref_config register */
	rc = zl3073x_read_u8(zldev, ZL_REG_REF_CONFIG, &ref->config);
	if (rc)
		return rc;

	dev_dbg(zldev->dev, "REF%u is %s and configured as %s\n", index,
		str_enabled_disabled(zl3073x_ref_is_enabled(ref)),
		zl3073x_ref_is_diff(ref) ? "differential" : "single-ended");

	return rc;
}

const struct zl3073x_ref *
zl3073x_ref_state_get(struct zl3073x_dev *zldev, u8 index)
{
	return index < ZL3073X_NUM_REFS ? &zldev->ref[index] : NULL;
}
