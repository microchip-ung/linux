// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Microchip Technology Inc.
 */

#ifndef __LINUX_MFD_LAN9645X_H
#define __LINUX_MFD_LAN9645X_H

#include <linux/kernel.h>

#define LAN9645X_SPI_MAX_PADDING_BYTES 15
#define LAN9645X_SPI_DEFAULT_PADDING_BYTES 15

struct lan9645x_ddata {
	struct device *dev;
	struct regmap *gcb;

	int spi_padding_bytes;

	struct regmap *regs;
	struct dentry *debugfs_root;
};

int lan9645x_spi_chip_reset(struct device *dev);

/* The IF_CTRL registers hold a 4-bit value, which controls the encoding of
 * data values using SPI.
 *
 * BIT(0): 0: Little-endian byte order, 1:Big-endian byte order
 * BIT(1): 0: MSB-first bit order, 1:LSB-first bit order
 * BIT[2:3] = must be 0.
 *
 * The default value is 0x1 (Big-endian, MSB).
 *
 * It is possible to write to these registers no matter the initial configuration
 * of the SPI-controller, using the following scheme:
 *
 * a) copy the 4-bit value into bits 3:0, 11:8, 19:16, and 27:24.
 * b) reverse the 4-bit value and copy into bits 7:4, 15:12, 23:20, and 31:28
 *
 * desired value    write value     effect
 *     0x0          0x00000000      LE byte order, MSB-first bit order
 *     0x1          0x81818181      BE byte order, MSB-first bit order
 *     0x2          0x42424242      LE byte order, LSB-first bit order
 *     0x3          0xc3c3c3c3      BE byte order, LSB-first bit order
 */

#define LAN9645X_SPI_BYTE_ORDER_LE 0x00000000
#define LAN9645X_SPI_BYTE_ORDER_BE 0x81818181

#ifdef __LITTLE_ENDIAN
#define LAN9645X_SPI_BYTE_ORDER LAN9645X_SPI_BYTE_ORDER_LE
#else
#define LAN9645X_SPI_BYTE_ORDER LAN9645X_SPI_BYTE_ORDER_BE
#endif

#endif /*  __LINUX_MFD_LAN9645X_H */
