/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN966X_ETHTOOL_H
#define _LAN966X_ETHTOOL_H

#include "linux/ethtool.h"

extern const struct ethtool_ops lan966x_ethtool_ops;
extern const struct dcbnl_rtnl_ops lan966x_dcbnl_ops;

#endif /* _LAN966X_ETHTOOL_H */
