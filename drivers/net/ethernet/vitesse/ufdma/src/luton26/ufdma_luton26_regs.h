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

#ifndef _UFDMA_LUTON26_REGS_H_
#define _UFDMA_LUTON26_REGS_H_

#define VTSS_BIT(x)                       (1U << (x))
#define VTSS_BITMASK(x)                   ((1U << (x)) - 1)
#define VTSS_EXTRACT_BITFIELD(x, o, w)    (((x) >> (o)) & VTSS_BITMASK(w))
#define VTSS_ENCODE_BITFIELD(x, o, w)     (((x) & VTSS_BITMASK(w)) << (o))
#define VTSS_ENCODE_BITMASK(o, w)         (VTSS_BITMASK(w) << (o))
#define VTSS_IOREG(t, o)                  (((t) >> 2) + (o))
#define VTSS_IOREG_IX(t, o, g, gw, r, ro) VTSS_IOREG(t, (o) + ((g) * (gw)) + (ro) + (r))
#define VTSS_TO_SYS                       0x00010000
#define VTSS_TO_ANA                       0x00020000
#define VTSS_TO_REW                       0x00030000
#define VTSS_TO_DEVCPU_QS                 0x00080000
#define VTSS_TO_CFG                       0x10000000
#define VTSS_TO_DMAC                      0x10110800

// All register accesses within this file happen through the callout->reg_read/write(), which
// requires a relative 32-bit address, but in one occassion we need to present a physical
// address to the H/W, so here is the macro that does that.
#define UFDMA_TO_PHYS(r) (((r) << 2) + 0x60000000)

#define VTSS_FDMA_CH_SAR(gi)                                             VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 0)
#define VTSS_FDMA_CH_DAR(gi)                                             VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 2)
#define VTSS_FDMA_CH_LLP(gi)                                             VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 4)
#define VTSS_FDMA_CH_CTL0(gi)                                            VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 6)
#define VTSS_F_FDMA_CH_CTL0_LLP_SRC_EN                                   VTSS_BIT(28)
#define VTSS_F_FDMA_CH_CTL0_LLP_DST_EN                                   VTSS_BIT(27)
#define VTSS_F_FDMA_CH_CTL0_SMS(x)                                       VTSS_ENCODE_BITFIELD(x, 25, 2)
#define VTSS_F_FDMA_CH_CTL0_DMS(x)                                       VTSS_ENCODE_BITFIELD(x, 23, 2)
#define VTSS_F_FDMA_CH_CTL0_TT_FC(x)                                     VTSS_ENCODE_BITFIELD(x, 20, 3)
#define VTSS_F_FDMA_CH_CTL0_SRC_MSIZE(x)                                 VTSS_ENCODE_BITFIELD(x, 14, 3)
#define VTSS_F_FDMA_CH_CTL0_DEST_MSIZE(x)                                VTSS_ENCODE_BITFIELD(x, 11, 3)
#define VTSS_F_FDMA_CH_CTL0_SINC(x)                                      VTSS_ENCODE_BITFIELD(x, 9, 2)
#define VTSS_F_FDMA_CH_CTL0_DINC(x)                                      VTSS_ENCODE_BITFIELD(x, 7, 2)
#define VTSS_F_FDMA_CH_CTL0_SRC_TR_WIDTH(x)                              VTSS_ENCODE_BITFIELD(x, 4, 3)
#define VTSS_F_FDMA_CH_CTL0_DST_TR_WIDTH(x)                              VTSS_ENCODE_BITFIELD(x, 1, 3)
#define VTSS_F_FDMA_CH_CTL0_INT_EN                                       VTSS_BIT(0)
#define VTSS_FDMA_CH_CTL1(gi)                                            VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 7)
#define VTSS_X_FDMA_CH_CTL1_DONE(x)                                      VTSS_EXTRACT_BITFIELD(x, 12, 1)
#define VTSS_FDMA_CH_DSTAT(gi)                                           VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 10)
#define VTSS_FDMA_CH_DSTATAR(gi)                                         VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 14)
#define VTSS_FDMA_CH_CFG0(gi)                                            VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 16)
#define VTSS_F_FDMA_CH_CFG0_HS_SEL_DST                                   VTSS_BIT(10)
#define VTSS_F_FDMA_CH_CFG0_CH_PRIOR(x)                                  VTSS_ENCODE_BITFIELD(x, 5, 3)
#define VTSS_FDMA_CH_CFG1(gi)                                            VTSS_IOREG_IX(VTSS_TO_DMAC, 0x0, gi, 22, 0, 17)
#define VTSS_F_FDMA_CH_CFG1_DST_PER(x)                                   VTSS_ENCODE_BITFIELD(x, 11, 4)
#define VTSS_F_FDMA_CH_CFG1_SRC_PER(x)                                   VTSS_ENCODE_BITFIELD(x, 7, 4)
#define VTSS_F_FDMA_CH_CFG1_DS_UPD_EN                                    VTSS_BIT(5)
#define VTSS_F_FDMA_CH_CFG1_FIFOMODE                                     VTSS_BIT(1)
#define VTSS_F_FDMA_CH_CFG1_FCMODE                                       VTSS_BIT(0)
#define VTSS_FDMA_INTR_RAW_TFR                                           VTSS_IOREG(VTSS_TO_DMAC, 0xb0)
#define VTSS_FDMA_INTR_RAW_BLOCK                                         VTSS_IOREG(VTSS_TO_DMAC, 0xb2)
#define VTSS_FDMA_INTR_RAW_ERR                                           VTSS_IOREG(VTSS_TO_DMAC, 0xb8)
#define VTSS_FDMA_INTR_STATUS_TFR                                        VTSS_IOREG(VTSS_TO_DMAC, 0xba)
#define VTSS_FDMA_INTR_STATUS_BLOCK                                      VTSS_IOREG(VTSS_TO_DMAC, 0xbc)
#define VTSS_FDMA_INTR_STATUS_ERR                                        VTSS_IOREG(VTSS_TO_DMAC, 0xc2)
#define VTSS_FDMA_INTR_MASK_TFR                                          VTSS_IOREG(VTSS_TO_DMAC, 0xc4)
#define VTSS_F_FDMA_INTR_MASK_TFR_INT_MASK_WE_TFR(x)                     VTSS_ENCODE_BITFIELD(x, 8, 8)
#define VTSS_F_FDMA_INTR_MASK_TFR_INT_MASK_TFR(x)                        VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_FDMA_INTR_MASK_BLOCK                                        VTSS_IOREG(VTSS_TO_DMAC, 0xc6)
#define VTSS_F_FDMA_INTR_MASK_BLOCK_INT_MASK_WE_BLOCK(x)                 VTSS_ENCODE_BITFIELD(x, 8, 8)
#define VTSS_F_FDMA_INTR_MASK_BLOCK_INT_MASK_BLOCK(x)                    VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_FDMA_INTR_MASK_ERR                                          VTSS_IOREG(VTSS_TO_DMAC, 0xcc)
#define VTSS_F_FDMA_INTR_MASK_ERR_INT_MASK_WE_ERR(x)                     VTSS_ENCODE_BITFIELD(x, 8, 8)
#define VTSS_F_FDMA_INTR_MASK_ERR_INT_MASK_ERR(x)                        VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_FDMA_INTR_CLEAR_TFR                                         VTSS_IOREG(VTSS_TO_DMAC, 0xce)
#define VTSS_F_FDMA_INTR_CLEAR_TFR_CLEAR_TFR(x)                          VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_FDMA_INTR_CLEAR_BLOCK                                       VTSS_IOREG(VTSS_TO_DMAC, 0xd0)
#define VTSS_F_FDMA_INTR_CLEAR_BLOCK_CLEAR_BLOCK(x)                      VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_FDMA_INTR_CLEAR_ERR                                         VTSS_IOREG(VTSS_TO_DMAC, 0xd6)
#define VTSS_F_FDMA_INTR_CLEAR_ERR_CLEAR_ERR(x)                          VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_FDMA_INTR_STATUSINT                                         VTSS_IOREG(VTSS_TO_DMAC, 0xd8)
#define VTSS_FDMA_MISC_DMA_CFG_REG                                       VTSS_IOREG(VTSS_TO_DMAC, 0xe6)
#define VTSS_F_FDMA_MISC_DMA_CFG_REG_DMA_EN                              VTSS_BIT(0)
#define VTSS_FDMA_MISC_CH_EN_REG                                         VTSS_IOREG(VTSS_TO_DMAC, 0xe8)
#define VTSS_F_FDMA_MISC_CH_EN_REG_CH_EN_WE(x)                           VTSS_ENCODE_BITFIELD(x, 8, 8)
#define VTSS_F_FDMA_MISC_CH_EN_REG_CH_EN(x)                              VTSS_ENCODE_BITFIELD(x, 0, 8)
#define VTSS_X_FDMA_MISC_CH_EN_REG_CH_EN(x)                              VTSS_EXTRACT_BITFIELD(x, 0, 8)
#define VTSS_FDMA_MISC_DMA_COMP_VERSION                                  VTSS_IOREG(VTSS_TO_DMAC, 0xff)
#define VTSS_DEVCPU_QS_XTR_XTR_FRM_PRUNING(ri)                           VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x0 + (ri))
#define VTSS_DEVCPU_QS_XTR_XTR_GRP_CFG(ri)                               VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x2 + (ri))
#define VTSS_F_DEVCPU_QS_XTR_XTR_GRP_CFG_STATUS_WORD_POS                 VTSS_BIT(1)
#define VTSS_M_DEVCPU_QS_XTR_XTR_GRP_CFG_STATUS_WORD_POS                 VTSS_BIT(1)
#define VTSS_F_DEVCPU_QS_XTR_XTR_GRP_CFG_BYTE_SWAP                       VTSS_BIT(0)
#define VTSS_M_DEVCPU_QS_XTR_XTR_GRP_CFG_BYTE_SWAP                       VTSS_BIT(0)
#define VTSS_DEVCPU_QS_XTR_XTR_MAP(ri)                                   VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x4 + (ri))
#define VTSS_DEVCPU_QS_XTR_XTR_QU_FLUSH                                  VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xa)
#define VTSS_DEVCPU_QS_XTR_XTR_DATA_PRESENT                              VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xb)
#define VTSS_DEVCPU_QS_XTR_XTR_QU_DBG                                    VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xc)
#define VTSS_DEVCPU_QS_INJ_INJ_GRP_CFG(ri)                               VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0xd + (ri))
#define VTSS_DEVCPU_QS_INJ_INJ_CTRL(ri)                                  VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x11 + (ri))
#define VTSS_DEVCPU_QS_INJ_INJ_STATUS                                    VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x13)
#define VTSS_DEVCPU_QS_INJ_INJ_ERR(ri)                                   VTSS_IOREG(VTSS_TO_DEVCPU_QS, 0x14 + (ri))
#define VTSS_ICPU_CFG_GPDMA_FDMA_CFG                                     VTSS_IOREG(VTSS_TO_CFG, 0x4f)
#define VTSS_F_ICPU_CFG_GPDMA_FDMA_CFG_FDMA_ENA                          VTSS_BIT(0)
#define VTSS_ICPU_CFG_GPDMA_FDMA_CH_CFG(ri)                              VTSS_IOREG(VTSS_TO_CFG, 0x51 + (ri))
#define VTSS_F_ICPU_CFG_GPDMA_FDMA_CH_CFG_USAGE                          VTSS_BIT(1)
#define VTSS_F_ICPU_CFG_GPDMA_FDMA_CH_CFG_CH_ENA                         VTSS_BIT(0)
#define VTSS_ICPU_CFG_GPDMA_FDMA_INJ_CFG(ri)                             VTSS_IOREG(VTSS_TO_CFG, 0x59 + (ri))
#define VTSS_F_ICPU_CFG_GPDMA_FDMA_INJ_CFG_INJ_GRP_BP_ENA                VTSS_BIT(3)
#define VTSS_F_ICPU_CFG_GPDMA_FDMA_INJ_CFG_INJ_GRP_BP_MAP(x)             VTSS_ENCODE_BITFIELD(x, 0, 3)
#define VTSS_ICPU_CFG_GPDMA_FDMA_XTR_CFG(ri)                             VTSS_IOREG(VTSS_TO_CFG, 0x5b + (ri))
#define VTSS_F_ICPU_CFG_GPDMA_FDMA_XTR_CFG_XTR_BURST_SIZE(x)             VTSS_ENCODE_BITFIELD(x, 0, 3)
#define VTSS_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB(ri)                   VTSS_IOREG(VTSS_TO_CFG, 0x5d + (ri))
#define VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_FRM_LEN(x) VTSS_EXTRACT_BITFIELD(x, 16, 16)
#define VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_ABORT(x)   VTSS_EXTRACT_BITFIELD(x, 4, 1)
#define VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_PRUNED(x)  VTSS_EXTRACT_BITFIELD(x, 3, 1)
#define VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_EOF(x)     VTSS_EXTRACT_BITFIELD(x, 2, 1)
#define VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_SOF(x)     VTSS_EXTRACT_BITFIELD(x, 1, 1)
#define VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_VLD(x)     VTSS_EXTRACT_BITFIELD(x, 0, 1)
#define VTSS_ICPU_CFG_GPDMA_FDMA_FRM_CNT                                 VTSS_IOREG(VTSS_TO_CFG, 0x5f)
#define VTSS_ICPU_CFG_GPDMA_FDMA_BP_TO_INT                               VTSS_IOREG(VTSS_TO_CFG, 0x60)
#define VTSS_ICPU_CFG_GPDMA_FDMA_BP_TO_DIV                               VTSS_IOREG(VTSS_TO_CFG, 0x61)
#define VTSS_F_FDMA_DAR_CHUNK_SIZE(x)                                    VTSS_ENCODE_BITFIELD((x),  16, 16)
#define VTSS_F_FDMA_DAR_INJ_GRP(x)                                       VTSS_ENCODE_BITFIELD((x),   8,  3)
#define VTSS_F_FDMA_DAR_SAR_OFFSET(x)                                    VTSS_ENCODE_BITFIELD((x),   4,  2)
#define VTSS_F_FDMA_DAR_EOF(x)                                           VTSS_ENCODE_BITFIELD((x),   3,  1)
#define VTSS_X_FDMA_DAR_EOF(x)                                           VTSS_EXTRACT_BITFIELD((x),  3,  1)
#define VTSS_F_FDMA_DAR_SOF(x)                                           VTSS_ENCODE_BITFIELD((x),   2,  1)
#define VTSS_X_FDMA_DAR_SOF(x)                                           VTSS_EXTRACT_BITFIELD((x),  2,  1)
#define VTSS_ANA_PORT_VLAN_CFG(gi)                                       VTSS_IOREG_IX(VTSS_TO_ANA, 0x0, gi, 32, 0,  0)
#define VTSS_F_ANA_PORT_VLAN_CFG_VLAN_AWARE_ENA                          VTSS_BIT(20)
#define VTSS_F_ANA_PORT_VLAN_CFG_VLAN_POP_CNT(x)                         VTSS_ENCODE_BITFIELD(x, 18, 2)
#define VTSS_F_ANA_PORT_VLAN_CFG_VLAN_VID(x)                             VTSS_ENCODE_BITFIELD(x, 0, 12)
#define VTSS_ANA_PORT_QOS_CFG(gi)                                        VTSS_IOREG_IX(VTSS_TO_ANA, 0x0, gi, 32, 0,  2)
#define VTSS_F_ANA_PORT_QOS_CFG_QOS_PCP_ENA                              VTSS_BIT(3)
#define VTSS_ANA_PORT_QOS_PCP_DEI_MAP_CFG(gi, ri)                        VTSS_IOREG_IX(VTSS_TO_ANA, 0x0, gi, 32, ri, 4)
#define VTSS_F_ANA_PORT_QOS_PCP_DEI_MAP_CFG_QOS_PCP_DEI_VAL(x)           VTSS_ENCODE_BITFIELD(x, 0, 3)
#define VTSS_ANA_PORT_PORT_CFG(gi)                                       VTSS_IOREG_IX(VTSS_TO_ANA, 0x0, gi, 32, 0, 24)
#define VTSS_F_ANA_PORT_PORT_CFG_RECV_ENA                                VTSS_BIT(5)
#define VTSS_REW_PORT_PORT_CFG(gi)                                       VTSS_IOREG_IX(VTSS_TO_REW, 0x0, gi, 32, 0,  2)
#define VTSS_F_REW_PORT_PORT_CFG_AGE_DIS                                 VTSS_BIT(1)
#define VTSS_M_REW_PORT_PORT_CFG_AGE_DIS                                 VTSS_BIT(1)
#define VTSS_SYS_SYSTEM_EGR_NO_SHARING                                   VTSS_IOREG(VTSS_TO_SYS, 0x20de)
#define VTSS_SYS_PAUSE_CFG_EGR_DROP_FORCE                                VTSS_IOREG(VTSS_TO_SYS, 0x21a1)
#define VTSS_SYS_SYSTEM_PORT_MODE(ri)                                    VTSS_IOREG(VTSS_TO_SYS, 0x206f + (ri))
#define VTSS_F_SYS_SYSTEM_PORT_MODE_DEQUEUE_DIS                          VTSS_BIT(1)
#define VTSS_M_SYS_SYSTEM_PORT_MODE_DEQUEUE_DIS                          VTSS_BIT(1)
#define VTSS_SYS_SCH_SCH_CPU                                             VTSS_IOREG(VTSS_TO_SYS, 0x2168)
#define VTSS_F_SYS_SCH_SCH_CPU_SCH_CPU_MAP(x)                            VTSS_ENCODE_BITFIELD(x, 2, 8)
#define VTSS_ICPU_CFG_INTR_INTR_RAW                                      VTSS_IOREG(VTSS_TO_CFG, 0x25)
#define VTSS_ICPU_CFG_INTR_INTR                                          VTSS_IOREG(VTSS_TO_CFG, 0x21)
#define VTSS_ICPU_CFG_INTR_FDMA_INTR_CFG                                 VTSS_IOREG(VTSS_TO_CFG, 0x3b)
#define VTSS_ICPU_CFG_INTR_INTR_ENA                                      VTSS_IOREG(VTSS_TO_CFG, 0x22)
#define VTSS_ICPU_CFG_INTR_ICPU_IRQ0_IDENT                               VTSS_IOREG(VTSS_TO_CFG, 0x27)

// The FDMA overrides the usage of the DCB->sar field in the extraction case
#define VTSS_F_FDMA_SAR_CHUNK_SIZE(x)                                    VTSS_ENCODE_BITFIELD((x), 16, 16)
#define VTSS_F_FDMA_SAR_CH_ID(x)                                         VTSS_ENCODE_BITFIELD((x),  2,  3) /* This is actually a reserved field in the DCB, but we can safely use it to store the channel ID for later retrieval with vcoreiii_fdma_xtr_ch_from_list() */

#endif /* _UFDMA_LUTON26_REGS_H_ */
