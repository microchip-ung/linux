/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip Sparx5 Switch driver debug filesystem support
 *
 * Copyright (c) 2023 Microchip Technology Inc. and its subsidiaries.
 */

#ifndef __LAN969X_H__
#define __LAN969X_H__

#include "../sparx5_main.h"

/* lan969x.c */
extern const struct sparx5_match_data lan969x_desc;
extern const unsigned int lan969x_raddr[RADDR_LAST];
extern const unsigned int lan969x_rcnt[RCNT_LAST];
extern const unsigned int lan969x_gaddr[GADDR_LAST];
extern const unsigned int lan969x_gcnt[GCNT_LAST];
extern const unsigned int lan969x_gsize[GSIZE_LAST];
extern const unsigned int lan969x_fpos[FPOS_LAST];

#endif
