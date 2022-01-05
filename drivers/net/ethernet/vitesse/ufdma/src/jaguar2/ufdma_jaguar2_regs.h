/* Copyright (c) 2015 Microsemi Corporation

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/
#ifndef _UFDMA_JAGUAR2_REGS_H_
#define _UFDMA_JAGUAR2_REGS_H_

#define VTSS_BIT(x)                       (1U << (x))
#define VTSS_BITMASK(x)                   ((1U << (x)) - 1)
#define VTSS_EXTRACT_BITFIELD(x, o, w)    (((x) >> (o)) & VTSS_BITMASK(w))
#define VTSS_ENCODE_BITFIELD(x, o, w)     (((x) & VTSS_BITMASK(w)) << (o))
#define VTSS_ENCODE_BITMASK(o, w)         (VTSS_BITMASK(w) << (o))
#define VTSS_IOREG(t, o)                  (((t) >> 2) + (o))
#define VTSS_IOREG_IX(t, o, g, gw, r, ro) VTSS_IOREG(t, (o) + ((g) * (gw)) + (ro) + (r))
// ServalT register offsets are 8 lower than Jaguar2 register offsets for register groups coming after the INTR reggrp.
#define VTSS_IOREG_ICPU_CFG(o)            (((VTSS_TO_CFG) >> 2) + (o) - (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 8 : 0))
#define VTSS_TO_CFG                       0x00000000
#define VTSS_TO_DEVCPU_QS                 0x01020000
#define VTSS_TO_ASM                       (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x01120000 : 0x01410000)
#define VTSS_TO_DSM                       (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x01160000 : 0x01450000)
#define VTSS_TO_QFWD                      0x017d0000
#define VTSS_TO_HSCH                      0x01880000
#define VTSS_TO_REW                       0x01b00000
#define VTSS_TO_ANA_CL                    0x01d00000
#define VTSS_TO_ANA_AC                    0x01f00000

#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(ri)                             VTSS_IOREG_ICPU_CFG(0x70 + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_DATAP(ri)                           VTSS_IOREG_ICPU_CFG(0x7a + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_DATAL(ri)                           VTSS_IOREG_ICPU_CFG(0x84 + (ri))
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_DATAL_SW(x)                       VTSS_ENCODE_BITFIELD(x, 24, 8)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_DATAL_DATAL(x)                    VTSS_ENCODE_BITFIELD(x, 0, 16)
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_STAT(ri)                            VTSS_IOREG_ICPU_CFG(0x8e + (ri))
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKO(x)                    VTSS_ENCODE_BITFIELD(x, 20, 12)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_PD(x)                        VTSS_EXTRACT_BITFIELD(x, 19, 1)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_ABORT(x)                     VTSS_EXTRACT_BITFIELD(x, 18, 1)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_EOF(x)                       VTSS_ENCODE_BITFIELD(!!(x), 17, 1)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_EOF(x)                       VTSS_EXTRACT_BITFIELD(x, 17, 1)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_SOF(x)                       VTSS_ENCODE_BITFIELD(!!(x), 16, 1)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_SOF(x)                       VTSS_EXTRACT_BITFIELD(x, 16, 1)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKL(x)                    VTSS_ENCODE_BITFIELD(x, 0, 16)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKL(x)                    VTSS_EXTRACT_BITFIELD(x, 0, 16)
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP_PREV(ri)                        VTSS_IOREG_ICPU_CFG(0x98 + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_STAT                                 VTSS_IOREG_ICPU_CFG(0xa2)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE                                 VTSS_IOREG_ICPU_CFG(0xa3)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_ACTIVATE                             VTSS_IOREG_ICPU_CFG(0xa4)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_DISABLE                              VTSS_IOREG_ICPU_CFG(0xa5)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_CH_DISABLE_CH_DISABLE(x)              VTSS_EXTRACT_BITFIELD(x, 0, 10)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_FORCEDIS                             VTSS_IOREG_ICPU_CFG(0xa6)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_CNT(ri)                              VTSS_IOREG_ICPU_CFG(0xa7 + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR                                 VTSS_IOREG_ICPU_CFG(0xc9)
#define VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR_CODE                            VTSS_IOREG_ICPU_CFG(0xca)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP                                VTSS_IOREG_ICPU_CFG(0xcb)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP_ENA                            VTSS_IOREG_ICPU_CFG(0xcc)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM                                VTSS_IOREG_ICPU_CFG(0xcd)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM_ENA                            VTSS_IOREG_ICPU_CFG(0xce)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG                                VTSS_IOREG_ICPU_CFG(0xcf)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG_ENA                            VTSS_IOREG_ICPU_CFG(0xd0)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_ENA                                VTSS_IOREG_ICPU_CFG(0xd1)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_IDENT                              VTSS_IOREG_ICPU_CFG(0xd2)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_CFG(ri)                              VTSS_IOREG_ICPU_CFG(0xd3 + (ri))
#define VTSS_F_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO(x)                     VTSS_ENCODE_BITFIELD(x, 2, 4)
#define VTSS_M_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO                        VTSS_ENCODE_BITMASK(2, 4)
#define VTSS_ICPU_CFG_FDMA_FDMA_GCFG                                    VTSS_IOREG_ICPU_CFG(0xdd)
#define VTSS_ICPU_CFG_FDMA_FDMA_GSTAT                                   VTSS_IOREG_ICPU_CFG(0xde)
#define VTSS_ICPU_CFG_FDMA_FDMA_IDLECNT                                 VTSS_IOREG_ICPU_CFG(0xdf)
#define VTSS_ICPU_CFG_FDMA_FDMA_CONST                                   VTSS_IOREG_ICPU_CFG(0xe0)
#define VTSS_DEVCPU_QS_XTR_XTR_GRP_CFG(ri)                              VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x0 + (ri))
#define VTSS_F_DEVCPU_QS_XTR_XTR_GRP_CFG_MODE(x)                        VTSS_ENCODE_BITFIELD(x, 2, 2)
#define VTSS_M_DEVCPU_QS_XTR_XTR_GRP_CFG_MODE                           VTSS_ENCODE_BITMASK(2, 2)
#define VTSS_DEVCPU_QS_XTR_XTR_FRM_PRUNING(ri)                          VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x4 + (ri))
#define VTSS_DEVCPU_QS_XTR_XTR_FLUSH                                    VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x6)
#define VTSS_F_DEVCPU_QS_XTR_XTR_FLUSH_FLUSH(x)                         VTSS_ENCODE_BITFIELD(x, 0, 2)
#define VTSS_DEVCPU_QS_XTR_XTR_DATA_PRESENT                             VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x7)
#define VTSS_DEVCPU_QS_INJ_INJ_GRP_CFG(ri)                              VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x9 + (ri))
#define VTSS_F_DEVCPU_QS_INJ_INJ_GRP_CFG_MODE(x)                        VTSS_ENCODE_BITFIELD(x, 2, 2)
#define VTSS_M_DEVCPU_QS_INJ_INJ_GRP_CFG_MODE                           VTSS_ENCODE_BITMASK(2, 2)
#define VTSS_DEVCPU_QS_INJ_INJ_CTRL(ri)                                 VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xd + (ri))
#define VTSS_F_DEVCPU_QS_INJ_INJ_CTRL_GAP_SIZE(x)                       VTSS_ENCODE_BITFIELD(x, 21, 4)
#define VTSS_M_DEVCPU_QS_INJ_INJ_CTRL_GAP_SIZE                          VTSS_ENCODE_BITMASK(21, 4)
#define VTSS_DEVCPU_QS_INJ_INJ_STATUS                                   VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xf)
#define VTSS_DEVCPU_QS_INJ_INJ_ERR(ri)                                  VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x10 + (ri))
#define VTSS_QFWD_SYSTEM_FRAME_COPY_CFG(ri)                             VTSS_IOREG(VTSS_TO_QFWD, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x01120 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x0003a : 0x0111f) + (ri))
#define VTSS_F_QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL(x)              VTSS_ENCODE_BITFIELD(x, 6, 6)
#define VTSS_M_QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL                 VTSS_ENCODE_BITMASK(6, 6)
#define VTSS_X_QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL(x)              VTSS_EXTRACT_BITFIELD(x, 6, 6)
#define VTSS_REW_COMMON_PORT_CTRL(ri)                                   VTSS_IOREG(VTSS_TO_REW,  (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x03f83 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x14f01 : 0x10751) + (ri))
#define VTSS_F_REW_COMMON_PORT_CTRL_KEEP_IFH_SEL(x)                     VTSS_ENCODE_BITFIELD(x, 4, 2)
#define VTSS_M_REW_COMMON_PORT_CTRL_KEEP_IFH_SEL                        VTSS_ENCODE_BITMASK(4, 2)
#define VTSS_ASM_CFG_PORT_CFG(ri)                                       VTSS_IOREG(VTSS_TO_ASM, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x2dd : 0xdb1) + (ri))
#define VTSS_F_ASM_CFG_PORT_CFG_INJ_FORMAT_CFG(x)                       VTSS_ENCODE_BITFIELD(x, 1, 2)
#define VTSS_M_ASM_CFG_PORT_CFG_INJ_FORMAT_CFG                          VTSS_ENCODE_BITMASK(1, 2)
#define VTSS_F_ASM_CFG_PORT_CFG_NO_PREAMBLE_ENA(x)                      VTSS_ENCODE_BITFIELD(!!(x), 8, 1)
#define VTSS_M_ASM_CFG_PORT_CFG_NO_PREAMBLE_ENA                         VTSS_BIT(8)
#define VTSS_F_ASM_CFG_PORT_CFG_VSTAX2_AWR_ENA(x)                       VTSS_ENCODE_BITFIELD(!!(x), 0, 1)
#define VTSS_M_ASM_CFG_PORT_CFG_VSTAX2_AWR_ENA                          VTSS_BIT(0)
#define VTSS_ANA_CL_PORT_STACKING_CTRL(gi)                              VTSS_IOREG_IX(VTSS_TO_ANA_CL, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x2800 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x9000 : 0x8000), gi, 64, 0, 4)
#define VTSS_F_ANA_CL_PORT_STACKING_CTRL_STACKING_AWARE_ENA(x)          VTSS_ENCODE_BITFIELD(!!(x), 2, 1)
#define VTSS_M_ANA_CL_PORT_STACKING_CTRL_STACKING_AWARE_ENA             VTSS_BIT(2)
#define VTSS_F_ANA_CL_PORT_STACKING_CTRL_STACKING_HEADER_DISCARD_ENA(x) VTSS_ENCODE_BITFIELD(!!(x), 0, 1)
#define VTSS_M_ANA_CL_PORT_STACKING_CTRL_STACKING_HEADER_DISCARD_ENA    VTSS_BIT(0)
#define VTSS_ANA_CL_PORT_VLAN_CTRL(gi)                                  VTSS_IOREG_IX(VTSS_TO_ANA_CL, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x2800 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x9000 : 0x8000), gi, 64, 0, 6)
#define VTSS_F_ANA_CL_PORT_VLAN_CTRL_PORT_VID(x)                        VTSS_ENCODE_BITFIELD(x, 0, 12)
#define VTSS_M_ANA_CL_PORT_VLAN_CTRL_PORT_VID                           VTSS_ENCODE_BITMASK(0, 12)
#define VTSS_F_ANA_CL_PORT_VLAN_CTRL_VLAN_AWARE_ENA(x)                  VTSS_ENCODE_BITFIELD(!!(x), 19, 1)
#define VTSS_M_ANA_CL_PORT_VLAN_CTRL_VLAN_AWARE_ENA                     VTSS_BIT(19)
#define VTSS_F_ANA_CL_PORT_VLAN_CTRL_VLAN_POP_CNT(x)                    VTSS_ENCODE_BITFIELD(x, 17, 2)
#define VTSS_M_ANA_CL_PORT_VLAN_CTRL_VLAN_POP_CNT                       VTSS_ENCODE_BITMASK(17, 2)
#define VTSS_ANA_CL_PORT_QOS_CFG(gi)                                    VTSS_IOREG_IX(VTSS_TO_ANA_CL, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x2800 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x9000 : 0x8000), gi, 64, 0, 40)
#define VTSS_F_ANA_CL_PORT_QOS_CFG_PCP_DEI_DP_ENA(x)                    VTSS_ENCODE_BITFIELD(!!(x), 8, 1)
#define VTSS_M_ANA_CL_PORT_QOS_CFG_PCP_DEI_DP_ENA                       VTSS_BIT(8)
#define VTSS_F_ANA_CL_PORT_QOS_CFG_PCP_DEI_QOS_ENA(x)                   VTSS_ENCODE_BITFIELD(!!(x), 7, 1)
#define VTSS_M_ANA_CL_PORT_QOS_CFG_PCP_DEI_QOS_ENA                      VTSS_BIT(7)
#define VTSS_ANA_CL_PORT_PCP_DEI_MAP_CFG(gi, ri)                        VTSS_IOREG_IX(VTSS_TO_ANA_CL, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x2800 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x9000 : 0x8000), gi, 64, ri, 24)
#define VTSS_F_ANA_CL_PORT_PCP_DEI_MAP_CFG_PCP_DEI_QOS_VAL(x)           VTSS_ENCODE_BITFIELD(x, 0, 3)
#define VTSS_HSCH_HSCH_MISC_PORT_MODE(ri)                               VTSS_IOREG(VTSS_TO_HSCH, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x0db4 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x6b84 : 0x7f82) + (ri))
#define VTSS_F_HSCH_HSCH_MISC_PORT_MODE_AGE_DIS(x)                      VTSS_ENCODE_BITFIELD(!!(x), 3, 1)
#define VTSS_M_HSCH_HSCH_MISC_PORT_MODE_AGE_DIS                         VTSS_BIT(3)
#define VTSS_DSM_CFG_BUF_CFG(ri)                                        VTSS_IOREG(VTSS_TO_DSM, 0x3 + (ri))
#define VTSS_F_DSM_CFG_BUF_CFG_AGING_ENA(x)                             VTSS_ENCODE_BITFIELD(!!(x), 0, 1)
#define VTSS_M_DSM_CFG_BUF_CFG_AGING_ENA                                VTSS_BIT(0)
#define VTSS_ICPU_CFG_INTR_INTR_RAW                                     VTSS_IOREG(VTSS_TO_CFG, 0x1c)
#define VTSS_ICPU_CFG_INTR_INTR_STICKY                                  VTSS_IOREG(VTSS_TO_CFG, 0x20)
#define VTSS_ICPU_CFG_INTR_INTR_BYPASS                                  VTSS_IOREG(VTSS_TO_CFG, 0x21)
#define VTSS_ICPU_CFG_INTR_INTR_ENA                                     VTSS_IOREG(VTSS_TO_CFG, 0x22)
#define VTSS_ICPU_CFG_INTR_INTR_IDENT                                   VTSS_IOREG(VTSS_TO_CFG, 0x25)
#define VTSS_ICPU_CFG_INTR_DST_INTR_MAP(ri)                             VTSS_IOREG(VTSS_TO_CFG, 0x26 + (ri))
#define VTSS_ANA_AC_PS_COMMON_SFLOW_CFG                                 VTSS_IOREG(VTSS_TO_ANA_AC, (state->cil.chip_arch == CHIP_ARCH_SERVAL_T ? 0x5702 : state->cil.chip_arch == CHIP_ARCH_JAGUAR_2C ? 0x2536c : 0x2136c))
#define VTSS_X_ANA_AC_PS_COMMON_SFLOW_CFG_SFLOW_CPU_QU(x)               VTSS_EXTRACT_BITFIELD(x, 12, 3)

#endif /* _UFDMA_JAGUAR2_REGS_H_ */
