/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#if defined(TARGET_SUNRISE) && !defined(TARGET_ADARO)
#define SUNRISE
#elif !defined(TARGET_SUNRISE) && defined(TARGET_ADARO)
#define ADARO
#elif !defined(TARGET_SUNRISE) && !defined(TARGET_SUNRISE)
#define ASIC
#else
#error
#endif
