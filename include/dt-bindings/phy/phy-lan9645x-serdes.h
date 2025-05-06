// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef __PHY_LAN9645X_SERDES_H__
#define __PHY_LAN9645X_SERDES_H__

#define LAN9645X_CU(x) ((0 << 12) | ((x) & 0x0fff))
#define LAN9645X_SERDES6G(x) ((1 << 12) | ((x) & 0x0fff))
#define LAN9645X_RGMII(x) ((2 << 12) | ((x) & 0x0fff))

#endif
