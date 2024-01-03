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

#ifndef _UFDMA_SERVAL_REGS_H_
#define _UFDMA_SERVAL_REGS_H_

#define VTSS_BIT(x)                       (1U << (x))
#define VTSS_BITMASK(x)                   ((1U << (x)) - 1)
#define VTSS_EXTRACT_BITFIELD(x, o, w)    (((x) >> (o)) & VTSS_BITMASK(w))
#define VTSS_ENCODE_BITFIELD(x, o, w)     (((x) & VTSS_BITMASK(w)) << (o))
#define VTSS_ENCODE_BITMASK(o, w)         (VTSS_BITMASK(w) << (o))
#define VTSS_IOREG(t, o)                  (((t) >> 2) + (o))
#define VTSS_IOREG_IX(t, o, g, gw, r, ro) VTSS_IOREG(t, (o) + ((g) * (gw)) + (ro) + (r))
#define VTSS_TO_CFG                       0x00000000
#define VTSS_TO_SYS                       0x01010000
#define VTSS_TO_REW                       0x01030000
#define VTSS_TO_DEVCPU_QS                 0x01080000
#define VTSS_TO_QSYS                      0x01800000
#define VTSS_TO_ANA                       (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x01900000 : 0x01880000)

#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(ri)                    VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x66 : 0x68) + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_DATAP(ri)                  VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x6a : 0x72) + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_DATAL(ri)                  VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x6e : 0x7c) + (ri))
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_DATAL_SW(x)              VTSS_ENCODE_BITFIELD(x, 24, 8)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_DATAL_DATAL(x)           VTSS_ENCODE_BITFIELD(x, 0, 16)
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_STAT(ri)                   VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x72 : 0x86) + (ri))
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKO(x)           VTSS_ENCODE_BITFIELD(x, 20, 12)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_PD                  VTSS_BIT(19)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_PD(x)               VTSS_EXTRACT_BITFIELD(x, 19, 1)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_ABORT               VTSS_BIT(18)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_ABORT(x)            VTSS_EXTRACT_BITFIELD(x, 18, 1)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_EOF                 VTSS_BIT(17)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_EOF(x)              VTSS_EXTRACT_BITFIELD(x, 17, 1)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_SOF                 VTSS_BIT(16)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_SOF(x)              VTSS_EXTRACT_BITFIELD(x, 16, 1)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKL(x)           VTSS_ENCODE_BITFIELD(x, 0, 16)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKL(x)           VTSS_EXTRACT_BITFIELD(x, 0, 16)
#define VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP_PREV(ri)               VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x76 : 0x90) + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_STAT                        VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x7a : 0x9a)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE                        VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x7b : 0x9b)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_ACTIVATE                    VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x7c : 0x9c)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_DISABLE                     VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x7d : 0x9d)
#define VTSS_X_ICPU_CFG_FDMA_FDMA_CH_DISABLE_CH_DISABLE(x)     VTSS_EXTRACT_BITFIELD(x, 0, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 4 : 10)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_FORCEDIS                    VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x7e : 0x9e)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_CNT(ri)                     VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x7f : 0x9f) + (ri))
#define VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR                        VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x89 : 0xc1)
#define VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR_CODE                   VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x8a : 0xc2)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP                       VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x8b : 0xc3)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP_ENA                   VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x8c : 0xc4)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM                       VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x8d : 0xc5)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM_ENA                   VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x8e : 0xc6)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG                       VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x8f : 0xc7)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG_ENA                   VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x90 : 0xc8)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_ENA                       VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x91 : 0xc9)
#define VTSS_ICPU_CFG_FDMA_FDMA_INTR_IDENT                     VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x92 : 0xca)
#define VTSS_ICPU_CFG_FDMA_FDMA_CH_CFG(ri)                     VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x93 : 0xcb) + (ri))
#define VTSS_F_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO(x)            VTSS_ENCODE_BITFIELD(x, 2, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 2 : 4)
#define VTSS_M_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO               VTSS_ENCODE_BITMASK(2, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 2 : 4)
#define VTSS_ICPU_CFG_FDMA_FDMA_GCFG                           VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x97 : 0xd5)
#define VTSS_F_ICPU_CFG_FDMA_FDMA_GCFG_INJ_RF_WM(x)            VTSS_ENCODE_BITFIELD(x, 7, 5)
#define VTSS_M_ICPU_CFG_FDMA_FDMA_GCFG_INJ_RF_WM               VTSS_ENCODE_BITMASK(7, 5)
#define VTSS_ICPU_CFG_FDMA_FDMA_GSTAT                          VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x98 : 0xd6)
#define VTSS_ICPU_CFG_FDMA_FDMA_IDLECNT                        VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x99 : 0xd7)
#define VTSS_ICPU_CFG_FDMA_FDMA_CONST                          VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x9a : 0xd8)
#define VTSS_DEVCPU_QS_XTR_XTR_GRP_CFG(ri)                     VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x0 + (ri))
#define VTSS_F_DEVCPU_QS_XTR_XTR_GRP_CFG_MODE(x)               VTSS_ENCODE_BITFIELD(x, 2, 2)
#define VTSS_M_DEVCPU_QS_XTR_XTR_GRP_CFG_MODE                  VTSS_ENCODE_BITMASK(2, 2)
#define VTSS_DEVCPU_QS_XTR_XTR_FRM_PRUNING(ri)                 VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x4 + (ri))
#define VTSS_DEVCPU_QS_XTR_XTR_FLUSH                           VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x6)
#define VTSS_F_DEVCPU_QS_XTR_XTR_FLUSH_FLUSH(x)                VTSS_ENCODE_BITFIELD(x, 0, 2)
#define VTSS_DEVCPU_QS_XTR_XTR_DATA_PRESENT                    VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x7)
#define VTSS_DEVCPU_QS_INJ_INJ_GRP_CFG(ri)                     VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x9 + (ri))
#define VTSS_F_DEVCPU_QS_INJ_INJ_GRP_CFG_MODE(x)               VTSS_ENCODE_BITFIELD(x, 2, 2)
#define VTSS_M_DEVCPU_QS_INJ_INJ_GRP_CFG_MODE                  VTSS_ENCODE_BITMASK(2, 2)
#define VTSS_DEVCPU_QS_INJ_INJ_CTRL(ri)                        VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xd + (ri))
#define VTSS_F_DEVCPU_QS_INJ_INJ_CTRL_GAP_SIZE(x)              VTSS_ENCODE_BITFIELD(x, 21, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 2 : 4)
#define VTSS_DEVCPU_QS_INJ_INJ_STATUS                          VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xf)
#define VTSS_DEVCPU_QS_INJ_INJ_ERR(ri)                         VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x10 + (ri))
#define VTSS_QSYS_SYSTEM_CPU_GROUP_MAP                         VTSS_IOREG(VTSS_TO_QSYS, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x56b6 : 0x44b6)
#define VTSS_F_QSYS_SYSTEM_CPU_GROUP_MAP_CPU_GROUP_MAP(x)      VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_QSYS_SYSTEM_SWITCH_PORT_MODE(ri)                  VTSS_IOREG(VTSS_TO_QSYS, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x568d : 0x448d) + (ri))
#define VTSS_F_QSYS_SYSTEM_SWITCH_PORT_MODE_PORT_ENA           VTSS_BIT(state->cil.chip_arch == CHIP_ARCH_SERVAL ? 13 : 14)
#define VTSS_SYS_SYSTEM_PORT_MODE(ri)                          VTSS_IOREG(VTSS_TO_SYS, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x149 :0x145) + (ri))
#define VTSS_F_SYS_SYSTEM_PORT_MODE_INCL_XTR_HDR(x)            VTSS_ENCODE_BITFIELD(x, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 2 : 1, 2)
#define VTSS_M_SYS_SYSTEM_PORT_MODE_INCL_XTR_HDR               VTSS_ENCODE_BITMASK(state->cil.chip_arch == CHIP_ARCH_SERVAL ? 2 : 1, 2)
#define VTSS_F_SYS_SYSTEM_PORT_MODE_INCL_INJ_HDR(x)            VTSS_ENCODE_BITFIELD(x, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 4 : 3, 2)
#define VTSS_M_SYS_SYSTEM_PORT_MODE_INCL_INJ_HDR               VTSS_ENCODE_BITMASK(state->cil.chip_arch == CHIP_ARCH_SERVAL ? 4 : 3, 2)
#define VTSS_ANA_PORT_VLAN_CFG(gi)                             VTSS_IOREG_IX(VTSS_TO_ANA, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x3000 : 0x1c00, gi, 64, 0, 0)
#define VTSS_F_ANA_PORT_VLAN_CFG_VLAN_AWARE_ENA                VTSS_BIT(20)
#define VTSS_F_ANA_PORT_VLAN_CFG_VLAN_POP_CNT(x)               VTSS_ENCODE_BITFIELD(x, 18, 2)
#define VTSS_F_ANA_PORT_VLAN_CFG_VLAN_VID(x)                   VTSS_ENCODE_BITFIELD(x, 0, 12)
#define VTSS_ANA_PORT_PORT_CFG(gi)                             VTSS_IOREG_IX(VTSS_TO_ANA, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x3000 : 0x1c00, gi, 64, 0, 28)
#define VTSS_ANA_PORT_QOS_CFG(gi)                              VTSS_IOREG_IX(VTSS_TO_ANA, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x3000 : 0x1c00, gi, 64, 0,  2)
#define VTSS_F_ANA_PORT_QOS_CFG_QOS_PCP_ENA                    VTSS_BIT(3)
#define VTSS_F_ANA_PORT_PORT_CFG_RECV_ENA                      VTSS_BIT(6)
#define VTSS_ANA_PORT_QOS_PCP_DEI_MAP_CFG(gi, ri)              VTSS_IOREG_IX(VTSS_TO_ANA, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x3000 : 0x1c00, gi, 64, ri, 8)
#define VTSS_F_ANA_PORT_QOS_PCP_DEI_MAP_CFG_QOS_PCP_DEI_VAL(x) VTSS_ENCODE_BITFIELD(x, 0, 3)
#define VTSS_REW_PORT_PORT_CFG(gi)                             VTSS_IOREG_IX(VTSS_TO_REW, 0x0, gi, 32, 0, 2)
#define VTSS_F_REW_PORT_PORT_CFG_AGE_DIS                       VTSS_BIT(0)
#define VTSS_ICPU_CFG_INTR_INTR_RAW                            VTSS_IOREG(VTSS_TO_CFG, 0x1c)
#define VTSS_ICPU_CFG_INTR_INTR_STICKY                         VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x1f : 0x20)
#define VTSS_ICPU_CFG_INTR_INTR_BYPASS                         VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x20 : 0x21)
#define VTSS_ICPU_CFG_INTR_INTR_ENA                            VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x21 : 0x22)
#define VTSS_ICPU_CFG_INTR_INTR_IDENT                          VTSS_IOREG(VTSS_TO_CFG, state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x24 : 0x25)
#define VTSS_ICPU_CFG_INTR_DST_INTR_MAP(ri)                    VTSS_IOREG(VTSS_TO_CFG, (state->cil.chip_arch == CHIP_ARCH_SERVAL ? 0x25  : 0x26) + (ri))

#endif /* _UFDMA_SERVAL_REGS_H_ */
