// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi sparx5 switch driver
 *
 * Copyright (c) 2019 Microsemi Corporation
 */

#ifndef _MSCC_SPARX5_H_
#define _MSCC_SPARX5_H_

/* DEVCPU_GCB at default, offset 0x01010000 */
#define DEVCPU_GCB_CHIP_REGS_ID_OFF                                  0x1010000
#define DEVCPU_GCB_CHIP_REGS_GPR_OFF                                 0x1010004
#define DEVCPU_GCB_CHIP_REGS_SOFT_RST_OFF                            0x1010008
#define DEVCPU_GCB_CHIP_REGS_HW_CFG_OFF                              0x101000c
#define DEVCPU_GCB_CHIP_REGS_HW_STAT_OFF                             0x1010010
#define DEVCPU_GCB_CHIP_REGS_HW_SGPIO_SD_CFG_OFF                     0x1010014
#define DEVCPU_GCB_CHIP_REGS_HW_SGPIO_TO_SD_MAP_CFG_OFF              0x1010018
#define DEVCPU_GCB_CHIP_REGS_HW_SGPIO_TO_SERDES_SD_MAP_CFG_OFF       0x101011c
#define DEVCPU_GCB_CHIP_REGS_FEA_STAT_OFF                            0x10101a0
#define DEVCPU_GCB_CHIP_REGS_FEA_DIS_OFF                             0x10101a4
#define DEVCPU_GCB_SI_REGS_IF_CTRL_OFF                               0x10101a8
#define DEVCPU_GCB_SI_REGS_IF_CFGSTAT_OFF                            0x10101ac
#define DEVCPU_GCB_SI_SS_MAP_SPI_MASTER_SS0_MASK_OFF                 0x10101b0
#define DEVCPU_GCB_SI_SS_MAP_SPI_MASTER_SS1_MASK_OFF                 0x10101b4
#define DEVCPU_GCB_SI_SS_MAP_SPI_MASTER_SS2_MASK_OFF                 0x10101b8
#define DEVCPU_GCB_SI_SS_MAP_SPI_MASTER_SS3_MASK_OFF                 0x10101bc
#define DEVCPU_GCB_SI_SS_MAP_SLV_OFF                                 0x10101c0
#define DEVCPU_GCB_SW_REGS_INTR_OFF                                  0x10101c4
#define DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF                          0x10101c8
#define DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_LSB_OFF                      0x10101cc
#define DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_MSB_OFF                      0x10101d0
#define DEVCPU_GCB_VCORE_ACCESS_VA_DATA_OFF                          0x10101d4
#define DEVCPU_GCB_VCORE_ACCESS_VA_DATA_INCR_OFF                     0x10101d8
#define DEVCPU_GCB_VCORE_ACCESS_VA_DATA_INERT_OFF                    0x10101dc
#define DEVCPU_GCB_GPIO_OUT_SET_OFF                                  0x10101e0
#define DEVCPU_GCB_GPIO_OUT_SET1_OFF                                 0x10101e4
#define DEVCPU_GCB_GPIO_OUT_CLR_OFF                                  0x10101e8
#define DEVCPU_GCB_GPIO_OUT_CLR1_OFF                                 0x10101ec
#define DEVCPU_GCB_GPIO_OUT_OFF                                      0x10101f0
#define DEVCPU_GCB_GPIO_OUT1_OFF                                     0x10101f4
#define DEVCPU_GCB_GPIO_IN_OFF                                       0x10101f8
#define DEVCPU_GCB_GPIO_IN1_OFF                                      0x10101fc
#define DEVCPU_GCB_GPIO_OE_OFF                                       0x1010200
#define DEVCPU_GCB_GPIO_OE1_OFF                                      0x1010204
#define DEVCPU_GCB_GPIO_INTR_OFF                                     0x1010208
#define DEVCPU_GCB_GPIO_INTR1_OFF                                    0x101020c
#define DEVCPU_GCB_GPIO_INTR_ENA_OFF                                 0x1010210
#define DEVCPU_GCB_GPIO_INTR_ENA1_OFF                                0x1010214
#define DEVCPU_GCB_GPIO_INTR_IDENT_OFF                               0x1010218
#define DEVCPU_GCB_GPIO_INTR_IDENT1_OFF                              0x101021c
#define DEVCPU_GCB_GPIO_ALT_OFF                                      0x1010220
#define DEVCPU_GCB_GPIO_ALT1_OFF                                     0x1010228
#define DEVCPU_GCB_GPIO_SD_MAP_OFF                                   0x1010230
#define DEVCPU_GCB_MIIM_MII_STATUS_OFF                               0x10102b0
#define DEVCPU_GCB_MIIM_MII_CFG_7226_OFF                             0x10102b4
#define DEVCPU_GCB_MIIM_MII_CMD_OFF                                  0x10102b8
#define DEVCPU_GCB_MIIM_MII_DATA_OFF                                 0x10102bc
#define DEVCPU_GCB_MIIM_MII_CFG_OFF                                  0x10102c0
#define DEVCPU_GCB_MIIM_MII_SCAN_0_OFF                               0x10102c4
#define DEVCPU_GCB_MIIM_MII_SCAN_1_OFF                               0x10102c8
#define PERF_MII_SCAN_RES_OFF                                        0x10102cc
#define PERF_MII_SCAN_RES_VLD_OFF                                    0x10102d0
#define PERF_MII_SCAN_RES_STICKY_OFF                                 0x1010340
#define DEVCPU_GCB_ROSC_CFG_OFF                                      0x1010350
#define DEVCPU_GCB_ROSC_MEASURE_CFG_OFF                              0x1010364
#define DEVCPU_GCB_ROSC_FREQ_CNT_OFF                                 0x1010368
#define DEVCPU_GCB_SIO_CTRL_INPUT_DATA_OFF                           0x101036c
#define DEVCPU_GCB_SIO_CTRL_CFG_OFF                                  0x101037c
#define DEVCPU_GCB_SIO_CTRL_CLOCK_OFF                                0x1010380
#define DEVCPU_GCB_SIO_CTRL_PORT_CFG_OFF                             0x1010384
#define DEVCPU_GCB_SIO_CTRL_PORT_ENA_OFF                             0x1010404
#define DEVCPU_GCB_SIO_CTRL_PWM_CFG_OFF                              0x1010408
#define DEVCPU_GCB_SIO_CTRL_INTR_POL_OFF                             0x1010414
#define DEVCPU_GCB_SIO_CTRL_INTR_RAW_OFF                             0x1010424
#define DEVCPU_GCB_SIO_CTRL_INTR_TRIGGER0_OFF                        0x1010434
#define DEVCPU_GCB_SIO_CTRL_INTR_TRIGGER1_OFF                        0x1010444
#define DEVCPU_GCB_SIO_CTRL_INTR_OFF                                 0x1010454
#define DEVCPU_GCB_SIO_CTRL_INTR_ENA_OFF                             0x1010464
#define DEVCPU_GCB_SIO_CTRL_INTR_IDENT_OFF                           0x1010474
#define DEVCPU_GCB_FAN_CTRL_CFG_OFF                                  0x10106b4
#define DEVCPU_GCB_FAN_CTRL_PWM_FREQ_OFF                             0x10106b8
#define DEVCPU_GCB_FAN_CTRL_CNT_OFF                                  0x10106bc
#define DEVCPU_GCB_MEMITGR_CTRL_OFF                                  0x10106c0
#define DEVCPU_GCB_MEMITGR_STAT_OFF                                  0x10106c4
#define DEVCPU_GCB_MEMITGR_INFO_OFF                                  0x10106c8
#define DEVCPU_GCB_MEMITGR_IDX_OFF                                   0x10106cc
#define DEVCPU_GCB_MEMITGR_DIV_OFF                                   0x10106d0
#define DEVCPU_GCB_MEMITGR_DBG_OFF                                   0x10106d4
#define DEVCPU_GCB_ETHERACCESS_VRAP_ACCESS_STAT_OFF                  0x10106d8

/* FDMA at amba_top, offset 0x00080000 */
#define FDMA_CH_ACTIVATE_OFF                                         0x080008
#define FDMA_CH_RELOAD_OFF                                           0x08000c
#define FDMA_CH_DISABLE_OFF                                          0x080010
#define FDMA_CH_FORCEDIS_OFF                                         0x080014
#define FDMA_CH_DB_DISCARD_OFF                                       0x080018
#define FDMA_CH_CNT_OFF                                              0x08001c
#define FDMA_DCB_LLP_OFF                                             0x08003c
#define FDMA_DCB_LLP1_OFF                                            0x08005c
#define FDMA_DCB_LLP_PREV_OFF                                        0x08007c
#define FDMA_DCB_LLP_PREV1_OFF                                       0x08009c
#define FDMA_CH_ACTIVE_OFF                                           0x0800bc
#define FDMA_CH_PENDING_OFF                                          0x0800c0
#define FDMA_CH_IDLE_OFF                                             0x0800c4
#define FDMA_CH_STATUS_OFF                                           0x0800c8
#define FDMA_CH_CFG_OFF                                              0x0800e8
#define FDMA_CH_TRANSLATE_OFF                                        0x080108
#define FDMA_CH_INJ_TOKEN_CNT_OFF                                    0x080128
#define FDMA_CH_INJ_TOKEN_TICK_RLD_OFF                               0x080140
#define FDMA_CH_INJ_TOKEN_TICK_CNT_OFF                               0x080158
#define FDMA_INJ_CFG_OFF                                             0x080170
#define FDMA_XTR_CFG_OFF                                             0x080174
#define FDMA_PORT_CFG_OFF                                            0x080178
#define FDMA_PORT_CTRL_OFF                                           0x080180
#define FDMA_INTR_DCB_OFF                                            0x080188
#define FDMA_INTR_DCB_ENA_OFF                                        0x08018c
#define FDMA_INTR_DB_OFF                                             0x080190
#define FDMA_INTR_DB_ENA_OFF                                         0x080194
#define FDMA_INTR_ERR_OFF                                            0x080198
#define FDMA_INTR_ENA_OFF                                            0x08019c
#define FDMA_INTR_IDENT_OFF                                          0x0801a0
#define FDMA_ERRORS_OFF                                              0x0801a4
#define FDMA_ERRORS_2_OFF                                            0x0801a8
#define FDMA_IDLECNT_OFF                                             0x0801ac
#define FDMA_CTRL_OFF                                                0x0801b0
#define FDMA_HA_PORT_STAT_OFF                                        0x080000

#endif /* _MSCC_SPARX5_H_ */

