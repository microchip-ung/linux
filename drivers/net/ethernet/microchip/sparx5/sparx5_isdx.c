// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2025 Microchip Technology Inc. and its subsidiaries.
 */

#include "sparx5_main.h"

#define SPX5_ISDX_CNT 4096

static struct sparx5_pool_entry sparx5_isdx_pool[SPX5_ISDX_CNT];

int sparx5_isdx_get(struct sparx5 *sparx5, u32 *isdx)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;

	return sparx5_pool_get(sparx5_isdx_pool, consts->isdx_cnt, isdx);
}

int sparx5_isdx_put(struct sparx5 *sparx5, u32 isdx)
{
	const struct sparx5_consts *consts = &sparx5->data->consts;

	return sparx5_pool_put(sparx5_isdx_pool, consts->isdx_cnt, isdx);
}
