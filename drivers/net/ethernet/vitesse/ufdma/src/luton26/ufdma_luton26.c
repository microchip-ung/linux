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

#include "vtss_ufdma_api.h"     /* Public header file            */
#include "../ail/ufdma.h"       /* Internal header file          */
#include "ufdma_luton26_regs.h" /* For chip register definitions */
#ifdef __KERNEL__
#include <linux/string.h>       /* For memset()                  */
#else
#include <string.h>             /* For memset()                  */
#endif

#define LU26_RX_IFH_SIZE_BYTES  8
#define LU26_TX_IFH_SIZE_BYTES  8 /* TBD: ptp_action != 0 means that the network driver must insert a 4-byte timestamp between the IFH and the first byte of the DMAC */
#define LU26_RX_CH              0
#define LU26_TX_CH              4
#define LU26_RX_GRP             0
#define LU26_TX_GRP             0
#define LU26_RX_PRIO            7
#define LU26_TX_PRIO            7
#define LU26_CHIP_PORT_CPU     26

// Source and destination burst transaction lengths for extraction and injection, respectively.
#define RX_SRC_MSIZE 2
#define RX_DST_MSIZE 2
#define RX_FIFOMODE  0
#define TX_SRC_MSIZE 2
#define TX_DST_MSIZE 1
#define TX_FIFOMODE  0

/**
 * CIL_debug_print()
 */
static int CIL_debug_print(ufdma_state_t *state, vtss_ufdma_debug_info_t *info, int (*pr)(void *ref, const char *fmt, ...))
{
    u32  i, val, val2;
    void *ref    = info->ref;
    BOOL pr_full = (info->full != 0);
    BOOL pr_rx   = (info->group == VTSS_UFDMA_DEBUG_GROUP_ALL || info->group == VTSS_UFDMA_DEBUG_GROUP_RX);
    BOOL pr_tx   = (info->group == VTSS_UFDMA_DEBUG_GROUP_ALL || info->group == VTSS_UFDMA_DEBUG_GROUP_TX);

    if (pr_rx) {
        pr(ref, "Rx channel: %u\n", LU26_RX_CH);
    }

    if (pr_tx) {
        pr(ref, "Tx channel: %u\n", LU26_TX_CH);
    }

    pr(ref, "\nFDMA:CH[x] registers:\n");
    for (i = 0; i < 8; i++) {
        if (!pr_full) {
            if (i == LU26_RX_CH) {
                if (!pr_rx) {
                    continue;
                }
            } else if (i == LU26_TX_CH) {
                if (!pr_tx) {
                    continue;
                }
            } else {
                continue;
            }
        }

        pr(ref, " SAR[%u]                    = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_SAR(i)));
        pr(ref, " DAR[%u]                    = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_DAR(i)));
        pr(ref, " LLP[%u]                    = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_LLP(i)));
        pr(ref, " CTL0[%u]                   = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_CTL0(i)));
        pr(ref, " CTL1[%u]                   = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_CTL1(i)));
        pr(ref, " DSTAT[%u]                  = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_DSTAT(i)));
        pr(ref, " DSTATAR[%u]                = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_DSTATAR(i)));
        pr(ref, " CFG0[%u]                   = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_CFG0(i)));
        pr(ref, " CFG1[%u]                   = 0x%08x\n", i, REG_RD(VTSS_FDMA_CH_CFG1(i)));
    }

    pr(ref, "\nFDMA:INTR registers:\n");
    pr(ref, " RAW_TFR                   = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_RAW_TFR));
    pr(ref, " RAW_BLOCK                 = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_RAW_BLOCK));
    pr(ref, " RAW_ERR                   = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_RAW_ERR));
    pr(ref, " STATUS_TFR                = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_STATUS_TFR));
    pr(ref, " STATUS_BLOCK              = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_STATUS_BLOCK));
    pr(ref, " STATUS_ERR                = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_STATUS_ERR));
    pr(ref, " MASK_TFR                  = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_MASK_TFR));
    pr(ref, " MASK_BLOCK                = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_MASK_BLOCK));
    pr(ref, " MASK_ERR                  = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_MASK_ERR));
    pr(ref, " STATUSINT                 = 0x%08x\n", REG_RD(VTSS_FDMA_INTR_STATUSINT));

    pr(ref, "\nFDMA:MISC registers:\n");
    pr(ref, " DMA_CFG_REG               = 0x%08x\n", REG_RD(VTSS_FDMA_MISC_DMA_CFG_REG));
    pr(ref, " CH_EN_REG                 = 0x%08x\n", REG_RD(VTSS_FDMA_MISC_CH_EN_REG));
    pr(ref, " DMA_COMP_VERSION          = 0x%08x\n\n", REG_RD(VTSS_FDMA_MISC_DMA_COMP_VERSION));

    if (pr_rx) {
        pr(ref, "Rx group: %u\n", LU26_RX_GRP);
    }

    if (pr_tx) {
        pr(ref, "Tx group: %u\n", LU26_TX_GRP);
    }

    if (pr_rx) {
        pr(ref, "\nDEVCPU_QS:XTR registers:\n");
        for (i = 0; i < 2; i++) {
            if (!pr_full) {
                if (i != LU26_RX_GRP) {
                    continue;
                }
            }

            pr(ref, " XTR_FRM_PRUNING[%u]        = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_XTR_XTR_FRM_PRUNING(i)));
            pr(ref, " XTR_GRP_CFG[%u]            = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_XTR_XTR_GRP_CFG(i)));
            pr(ref, " XTR_MAP[%u]                = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_XTR_XTR_MAP(i)));
        }

        pr(ref, " XTR_FLUSH                 = 0x%08x\n", REG_RD(VTSS_DEVCPU_QS_XTR_XTR_QU_FLUSH));
        pr(ref, " XTR_DATA_PRESENT          = 0x%08x\n", REG_RD(VTSS_DEVCPU_QS_XTR_XTR_DATA_PRESENT));
        pr(ref, " XTR_QU_DBG                = 0x%08x\n", REG_RD(VTSS_DEVCPU_QS_XTR_XTR_QU_DBG));
    }

    if (pr_tx) {
        pr(ref, "\nDEVCPU_QS:INJ registers:\n");
        for (i = 0; i < 2; i++) {
            if (!pr_full) {
                if (i != LU26_TX_GRP) {
                    continue;
                }
            }

            pr(ref, " INJ_GRP_CFG[%u]            = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_INJ_INJ_GRP_CFG(i)));
            pr(ref, " INJ_CTRL[%u]               = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_INJ_INJ_CTRL(i)));
            pr(ref, " INJ_ERR[%u]                = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_INJ_INJ_ERR(i)));
        }

        pr(ref, " INJ_STATUS                = 0x%08x\n", REG_RD(VTSS_DEVCPU_QS_INJ_INJ_STATUS));
    }

    pr(ref, "\nICPU_CFG:GPDMA registers:\n");
    pr(ref, " FDMA_CFG                  = 0x%08x\n", REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_CFG));
    pr(ref, " FDMA_FRM_CNT              = 0x%08x\n", REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_FRM_CNT));
    pr(ref, " FDMA_BP_TO_INT            = 0x%08x\n", REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_BP_TO_INT));
    pr(ref, " FDMA_BP_TO_DIV            = 0x%08x\n", REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_BP_TO_DIV));

    for (i = 0; i < 8; i++) {
        if (!pr_full) {
            if (i == LU26_RX_CH) {
                if (!pr_rx) {
                    continue;
                }
            } else if (i == LU26_TX_CH) {
                if (!pr_tx) {
                    continue;
                }
            } else {
                continue;
            }
        }

        pr(ref, " FDMA_CH_CFG[%u]            = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_CH_CFG(i)));
    }

    if (pr_rx) {
        for (i = 0; i < 2; i++) {
            if (!pr_full) {
                if (i != LU26_RX_GRP) {
                    continue;
                }
            }

            pr(ref, " FDMA_XTR_CFG[%u]           = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_XTR_CFG(i)));
            pr(ref, " FDMA_XTR_STAT_LAST_DCB[%u] = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB(i)));
        }
    }

    if (pr_tx) {
        for (i = 0; i < 2; i++) {
            if (!pr_full) {
                if (i != LU26_TX_GRP) {
                    continue;
                }
            }

            pr(ref, " FDMA_INJ_CFG[%u]           = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_GPDMA_FDMA_INJ_CFG(i)));
        }
    }

    if (pr_rx) {
        // For throttling
        pr(ref, "\nSYS registers\n");
        pr(ref, " SYSTEM:PORT_MODE(27)      = 0x%08x\n", REG_RD(VTSS_SYS_SYSTEM_PORT_MODE(27)));
        pr(ref, " SCH:SCH_CPU               = 0x%08x\n", REG_RD(VTSS_SYS_SCH_SCH_CPU));
    }

#define FDMA_IRQ 10
    pr(ref, "\nFDMA IRQ: %d, supposed to go into ICPU_IRQ0\n", FDMA_IRQ);
    pr(ref, "ICPU_CFG:INTR registers:\n");
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR_RAW);
    pr(ref, " INTR_RAW             = 0x%08x (bit %u = %u)\n", val, FDMA_IRQ, (val >> FDMA_IRQ) & 1);
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR);
    pr(ref, " INTR                 = 0x%08x (bit %u = %u)\n", val, FDMA_IRQ, (val >> FDMA_IRQ) & 1);
    val = REG_RD(VTSS_ICPU_CFG_INTR_FDMA_INTR_CFG);
    val2 = val & 0x3;
    pr(ref, " FDMA_INTR_CFG        = 0x%08x (Interrupt destination is %s)\n", val, val2 == 0 ? "ICPU_IRQ0" : val2 == 1 ? "ICPU_IRQ1" : val2 == 2 ? "EXT_IRQ0" : "EXT_IRQ1");
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR_ENA);
    pr(ref, " INTR_ENA             = 0x%08x (bit %u = %u)\n", val, FDMA_IRQ, (val >> FDMA_IRQ) & 1);
    val = REG_RD(VTSS_ICPU_CFG_INTR_ICPU_IRQ0_IDENT);
    pr(ref, " ICPU_IRQ0_IDENT      = 0x%08x (bit %u = %u)\n", val, FDMA_IRQ, (val >> FDMA_IRQ) & 1);
#undef FDMA_IRQ

    pr(ref, "\n");
    return UFDMA_RC_OK;
}

/**
 * CIL_hw_dcb_next()
 */
static void *CIL_hw_dcb_next(ufdma_state_t *state, ufdma_dcb_t *dcb)
{
    return (void *)dcb->hw_dcb.v1.llp;
}

/**
 * CIL_rx_qu_suspend_set()
 */
static int CIL_rx_qu_suspend_set(ufdma_state_t *state, u32 rx_qu, BOOL suspend)
{
    // On Lu26 there are two CPU ports (physical #26 and #27). When running only on
    // the on-chip, internal CPU, we only use #26. However, some applications may
    // want to divide the Rx queues among them and forward some queues to
    // #26 and others to #27, which can then be read by the external CPU.
    // If enabling throttling, the internal CPU will suspend a given queue by
    // moving that queue from port #26 to port #27, which might have some
    // unexpected side-effects on an external CPU reading port #27.
    u32 mask = VTSS_F_SYS_SCH_SCH_CPU_SCH_CPU_MAP(VTSS_BIT(rx_qu));

    // First disable dequeing from port #27
    REG_WRM(VTSS_SYS_SYSTEM_PORT_MODE(27), VTSS_F_SYS_SYSTEM_PORT_MODE_DEQUEUE_DIS, VTSS_M_SYS_SYSTEM_PORT_MODE_DEQUEUE_DIS);

    // Then either redirect the queue to port #26 (ourselves) or port #27 (suspend)
    REG_WRM(VTSS_SYS_SCH_SCH_CPU, suspend ? mask : 0, mask);

    return UFDMA_RC_OK;
}

/**
 * CIL_poll()
 */
static int CIL_poll(ufdma_state_t *state, BOOL rx, BOOL tx, unsigned int rx_cnt_max)
{
    u32 rx_mask         = VTSS_BIT(LU26_RX_CH);
    u32 tx_mask         = VTSS_BIT(LU26_TX_CH);
    u32 ch_mask         = (rx ? rx_mask : 0) | (tx ? tx_mask : 0);
    u32 intr_err        = REG_RD(VTSS_FDMA_INTR_STATUS_ERR) & ch_mask;

    // We don't expect any errors. Write a message and clear them if they occur.
    if (intr_err != 0) {
        UFDMA_E("intr_err (%u) != 0", intr_err);
        REG_WR(VTSS_FDMA_INTR_CLEAR_ERR, VTSS_F_FDMA_INTR_CLEAR_ERR_CLEAR_ERR(intr_err));
    }

    // We first handle block interrupts, which are per-DCB reception interrupts.
    // We need to clear the interrupts before iterating through the DCBs. If we didn't,
    // we might end up in a situation where a frame arrives after we've looped through
    // the DCBs, but before we clear the interrupts, and the frame would be stuck in RAM
    // until the next frame arrives. About the same argumentation holds for injection interrupts.
    REG_WR(VTSS_FDMA_INTR_CLEAR_BLOCK, VTSS_F_FDMA_INTR_CLEAR_BLOCK_CLEAR_BLOCK(ch_mask));

    // The transfer done interrupts occur both when one frame has been injected and
    // in the (rare?) case where the OS cannot take off frames in the rate that the
    // FDMA receives them to RAM, causing the DCB list to exhaust and the channel
    // to stop.
    REG_WR(VTSS_FDMA_INTR_CLEAR_TFR, VTSS_F_FDMA_INTR_CLEAR_TFR_CLEAR_TFR(ch_mask));

    UFDMA_AIL_FUNC_RC(state, reorder_barrier);

    if (rx) {
        // rx_frm() will also check if we need to restart
        // the channel afterwards.
        UFDMA_AIL_FUNC_RC(state, rx_frm, 0, rx_cnt_max);
    }

    if (tx) {
        UFDMA_AIL_FUNC_RC(state, tx_done);
    }

    return UFDMA_RC_OK;
}

/**
 * CIL_interrupts_enable()
 * Rx:
 *  We enable block interrupts for extraction channels, so that we get an interrupt every time a DCB has been filled with
 *  data. If we used Transfer Done interrupts, we wouldn't get an interrupt until the list of DCBs was exhausted.
 *  We also, on the other hand enable transfer done interrupts. This is needed in order to restart a channel if it happens
 *  that the list of DCBs gets exhausted, which may occur if the FDMA transfers packets faster than the operating system
 *  can take them off.
 *
 * Tx:
 *  Since the same channel can be used to inject to different ports, and since we need to re-configure
 *  the DMA controller if the port changes, we must get a new interrupt for every frame that has been
 *  injected. Therefore we make sure that the DCB->llp is NULL for the last DCB of every
 *  frame, and that the interrupt enable flag in DCB->ctl1 is 1 for the EOF DCB, only.
 *  When the DMA controller has injected the EOF DCB, it will then invoke the transfer done interrupt
 *  so that we can notify the callback function that the frame has been transmitted, and possibly
 *  restart the controller if there're pending frames.
 */
static void CIL_interrupts_enable(ufdma_state_t *state)
{
    u32 rx_mask = VTSS_BIT(LU26_RX_CH);
    u32 tx_mask = VTSS_BIT(LU26_TX_CH);
    u32 ch_mask = rx_mask | tx_mask;

    // Clear and enable Block Done interrupts (raised when interrupt bit in DCB is set and DCB is completed; Rx only)
    REG_WR(VTSS_FDMA_INTR_CLEAR_BLOCK, VTSS_F_FDMA_INTR_CLEAR_BLOCK_CLEAR_BLOCK(rx_mask));
    REG_WR(VTSS_FDMA_INTR_MASK_BLOCK, VTSS_F_FDMA_INTR_MASK_BLOCK_INT_MASK_WE_BLOCK(rx_mask) | VTSS_F_FDMA_INTR_MASK_BLOCK_INT_MASK_BLOCK(rx_mask));

    // Clear and enable Transfer Done (LLP == NULL) interrupts from FDMA (both Rx and Tx)
    REG_WR(VTSS_FDMA_INTR_CLEAR_TFR, VTSS_F_FDMA_INTR_CLEAR_TFR_CLEAR_TFR(ch_mask));
    REG_WR(VTSS_FDMA_INTR_MASK_TFR,  VTSS_F_FDMA_INTR_MASK_TFR_INT_MASK_WE_TFR(ch_mask) | VTSS_F_FDMA_INTR_MASK_TFR_INT_MASK_TFR(ch_mask));

    // Clear and enable Error interrupts (both Rx and Tx)
    REG_WR(VTSS_FDMA_INTR_CLEAR_ERR, VTSS_F_FDMA_INTR_CLEAR_ERR_CLEAR_ERR(ch_mask));
    REG_WR(VTSS_FDMA_INTR_MASK_ERR, VTSS_F_FDMA_INTR_MASK_ERR_INT_MASK_WE_ERR(ch_mask) | VTSS_F_FDMA_INTR_MASK_ERR_INT_MASK_ERR(ch_mask));
}

/**
 * CIL_interrupts_disable()
 */
static void CIL_interrupts_disable(ufdma_state_t *state, BOOL graceful)
{
    u32 rx_mask = VTSS_BIT(LU26_RX_CH);
    u32 tx_mask = VTSS_BIT(LU26_TX_CH);
    u32 ch_mask = rx_mask | tx_mask;

    // Disable all channels
    REG_WR(VTSS_FDMA_MISC_CH_EN_REG, VTSS_F_FDMA_MISC_CH_EN_REG_CH_EN_WE(ch_mask)); // CH_EN_WE = mask, CH_EN = 0

    if (graceful) {
        u32 val;
        // Wait for it to disable itself.
        do {
            val = REG_RD(VTSS_FDMA_MISC_CH_EN_REG);
        } while ((VTSS_X_FDMA_MISC_CH_EN_REG_CH_EN(val) & ch_mask) != 0);
    }

    // Disable and clear Block Done interrupts (raised when interrupt bit in DCB is set and DCB is completed, Rx only)
    REG_WR(VTSS_FDMA_INTR_MASK_BLOCK, VTSS_F_FDMA_INTR_MASK_BLOCK_INT_MASK_WE_BLOCK(rx_mask));
    REG_WR(VTSS_FDMA_INTR_CLEAR_BLOCK, VTSS_F_FDMA_INTR_CLEAR_BLOCK_CLEAR_BLOCK(rx_mask));

    // Disable and clear Transfer Done (LLP == NULL) interrupts from FDMA (both Rx and Tx)
    REG_WR(VTSS_FDMA_INTR_MASK_TFR,  VTSS_F_FDMA_INTR_MASK_TFR_INT_MASK_WE_TFR(ch_mask));
    REG_WR(VTSS_FDMA_INTR_CLEAR_TFR, VTSS_F_FDMA_INTR_CLEAR_TFR_CLEAR_TFR(ch_mask));

    // Disable and clear Error interrupts (both Rx and Tx)
    REG_WR(VTSS_FDMA_INTR_MASK_ERR, VTSS_F_FDMA_INTR_MASK_ERR_INT_MASK_WE_ERR(ch_mask));
    REG_WR(VTSS_FDMA_INTR_CLEAR_ERR, VTSS_F_FDMA_INTR_CLEAR_ERR_CLEAR_ERR(ch_mask));
}

/**
 * CIL_enable_ch()
 */
static void CIL_enable_ch(ufdma_state_t *state, u32 ch)
{
    REG_WR(VTSS_FDMA_MISC_CH_EN_REG, VTSS_F_FDMA_MISC_CH_EN_REG_CH_EN_WE(VTSS_BIT(ch)) | VTSS_F_FDMA_MISC_CH_EN_REG_CH_EN(VTSS_BIT(ch))); // CH_EN_WE = CH_EN = 1
}

/**
 * CIL_rx_dcb_init()
 */
static int CIL_rx_dcb_init(ufdma_state_t *state, ufdma_dcb_t *dcb, u32 buf_size_bytes_aligned)
{
    ufdma_hw_dcb_v1_t *hw_dcb = &dcb->hw_dcb.v1;

    // #buf_size_bytes_aligned contains a value that is guaranteed to be a multiple of
    // our rx_burst_size_bytes, which is needed because DATAL must have 32-bit words,
    // that is, bit 1:0 must be 0.

    // Initialize the DCB area
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, VTSS_F_FDMA_SAR_CHUNK_SIZE(buf_size_bytes_aligned) | VTSS_F_FDMA_SAR_CH_ID(LU26_RX_CH), &hw_dcb->sar);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, (u32)VIRT_TO_PHYS(dcb->buf_dscr.buf),                                                   &hw_dcb->dar);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, dcb->next ? (u32)VIRT_TO_PHYS(&dcb->next->hw_dcb.v1) : 0,                               &hw_dcb->llp);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus,
                      VTSS_F_FDMA_CH_CTL0_LLP_SRC_EN              | VTSS_F_FDMA_CH_CTL0_LLP_DST_EN               |
                      VTSS_F_FDMA_CH_CTL0_SMS(1)                  | VTSS_F_FDMA_CH_CTL0_TT_FC(4)                 |
                      VTSS_F_FDMA_CH_CTL0_SRC_MSIZE(RX_SRC_MSIZE) | VTSS_F_FDMA_CH_CTL0_DEST_MSIZE(RX_DST_MSIZE) |
                      VTSS_F_FDMA_CH_CTL0_SINC(2)                 | VTSS_F_FDMA_CH_CTL0_SRC_TR_WIDTH(2)          |
                      VTSS_F_FDMA_CH_CTL0_DST_TR_WIDTH(2)         | VTSS_F_FDMA_CH_CTL0_INT_EN,                                  &hw_dcb->ctl0);
    hw_dcb->ctl1 = hw_dcb->stat = 0;

    // Get the DCB written to main memory
    DCACHE_FLUSH(hw_dcb, sizeof(*hw_dcb));

    return UFDMA_RC_OK;
}

/**
 * CIL_tx_dcb_init()
 */
static int CIL_tx_dcb_init(ufdma_state_t *state, ufdma_dcb_t *dcb, ufdma_dcb_t *dcb_prev)
{
    ufdma_hw_dcb_v1_t *hw_dcb = &dcb->hw_dcb.v1;
    u32               datap, datao, min_len, len, ifh_size_bytes;

    // Auto-adjust the length to form a minimum-sized Ethernet frame.
    // There is one caveat here: If IFH.PTP_ACTION != 0, the size of the IFH is 12 bytes
    // instead of 8 bytes. If the application used vtss_packet_tx_hdr_encode(), this
    // is already taken care of in the frame, but we still need to take this into account
    // when adjusting the frame to a minimum-sized Ethernet frame.
    ifh_size_bytes = state->self->props.tx_ifh_size_bytes;

    // ifh.ptp_action is located at bit [61; 62] of the IFH corresponding
    // to bit 5 and 6 of the first byte of the IFH.
    if (dcb->buf_dscr.buf[0] & 0x60) {
        // A four-byte timestamp has been pushed.
        ifh_size_bytes += 4;
    }

    len = dcb->buf_dscr.buf_size_bytes;
    min_len = ifh_size_bytes + 64;

    if (len < min_len) {
        UFDMA_IG(TX, "Auto-adjusting frame size from %u to %u bytes", len, min_len);
        len = min_len;
    }

    datap = (u32)VIRT_TO_PHYS(dcb->buf_dscr.buf);
    datao = (u32)datap & 0x3; // Offset

    hw_dcb->llp = 0; // We only have one frame in #dcb, but below, we might end up appending it to #dcb_prev.
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, datap & ~0x3,                                              &hw_dcb->sar);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus,
                      VTSS_F_FDMA_DAR_CHUNK_SIZE(len)             | VTSS_F_FDMA_DAR_INJ_GRP(LU26_TX_GRP)         |
                      VTSS_F_FDMA_DAR_SAR_OFFSET(datao)           | VTSS_F_FDMA_DAR_EOF(1)                       |
                      VTSS_F_FDMA_DAR_SOF(1),                                                       &hw_dcb->dar);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus,
                      VTSS_F_FDMA_CH_CTL0_LLP_SRC_EN              | VTSS_F_FDMA_CH_CTL0_LLP_DST_EN               |
                      VTSS_F_FDMA_CH_CTL0_DMS(1)                  | VTSS_F_FDMA_CH_CTL0_TT_FC(1)                 |
                      VTSS_F_FDMA_CH_CTL0_SRC_MSIZE(TX_SRC_MSIZE) | VTSS_F_FDMA_CH_CTL0_DEST_MSIZE(TX_DST_MSIZE) |
                      VTSS_F_FDMA_CH_CTL0_DINC(2)                 | VTSS_F_FDMA_CH_CTL0_SRC_TR_WIDTH(2)          |
                      VTSS_F_FDMA_CH_CTL0_DST_TR_WIDTH(2)         | VTSS_F_FDMA_CH_CTL0_INT_EN,     &hw_dcb->ctl0);

    // If the data area is non-32-bit-aligned (as is the case for Linux), then we need to check if we're gonna inject one more 32-bit word.
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, (len + datao + 3) / 4, &hw_dcb->ctl1); // BLOCK_TS
    hw_dcb->stat = 0;

    // Get the DCB written to main memory
    DCACHE_FLUSH(hw_dcb, sizeof(*hw_dcb));

    if (dcb_prev) {
        // We also need to link the previous DCB to this one.
        ufdma_hw_dcb_v1_t *hw_dcb_prev = &dcb_prev->hw_dcb.v1;

        hw_dcb_prev->llp = (u32)VIRT_TO_PHYS(hw_dcb);

        // And re-flush it.
        DCACHE_FLUSH(&hw_dcb_prev->llp, sizeof(hw_dcb_prev->llp));
    }

    return UFDMA_RC_OK;
}

/**
 * CIL_rx_start()
 */
static int CIL_rx_start(ufdma_state_t *state, ufdma_dcb_t *head, BOOL *restarted)
{
    if (state->rx_head_hw || (VTSS_X_FDMA_MISC_CH_EN_REG_CH_EN(REG_RD(VTSS_FDMA_MISC_CH_EN_REG)) & VTSS_BIT(LU26_RX_CH)) != 0) {
        // As long as we haven't yet used all DCBs or the channel is not disabled, don't restart.
        *restarted = FALSE;
        return UFDMA_RC_OK;
    }

    // Feed #head into the H/W
    REG_WR(VTSS_FDMA_CH_LLP(LU26_RX_CH), (u32)VIRT_TO_PHYS(&head->hw_dcb.v1));

    // Make sure the above lines are executed before enabling
    // the channel - hence the reorder barrier.
    UFDMA_AIL_FUNC_RC(state, reorder_barrier);

    // Re-activate the channel.
    CIL_enable_ch(state, LU26_RX_CH);

    *restarted = TRUE;
    return UFDMA_RC_OK;
}

/**
 * CIL_tx_start()
 */
static int CIL_tx_start(ufdma_state_t *state, ufdma_dcb_t *head)
{
    u32 ch_en = REG_RD(VTSS_FDMA_MISC_CH_EN_REG);

    // Channel must be inactive
    if ((ch_en & VTSS_BIT(LU26_TX_CH)) != 0) {
        UFDMA_EG(TX, "Tx channel (%u) still enabled: 0x%08x", LU26_TX_CH, ch_en);
        // Fall through and write it anyway
    }

    // Hand the DCB list to the channel
    REG_WR(VTSS_FDMA_CH_LLP(LU26_TX_CH), (u32)VIRT_TO_PHYS(&head->hw_dcb.v1));

    // Make sure the above lines are executed before enabling
    // the channel - hence the reorder barrier.
    UFDMA_AIL_FUNC_RC(state, reorder_barrier);

    // And start the channel
    CIL_enable_ch(state, LU26_TX_CH);

    return UFDMA_RC_OK;
}

/**
 * CIL_rx_reinit()
 */
static int CIL_rx_reinit(ufdma_state_t *state)
{
    u32 val, rx_mask = VTSS_BIT(LU26_RX_CH);

    // Stop the channel gracefully
    REG_WR(VTSS_FDMA_MISC_CH_EN_REG, VTSS_F_FDMA_MISC_CH_EN_REG_CH_EN_WE(rx_mask)); // CH_EN_WE = mask, CH_EN = 0

    // Wait for it to disable itself.
    do {
        val = REG_RD(VTSS_FDMA_MISC_CH_EN_REG);
    } while ((VTSS_X_FDMA_MISC_CH_EN_REG_CH_EN(val) & rx_mask) != 0);

    // Set the channel's LLP field to NULL
    REG_WR(VTSS_FDMA_CH_LLP(LU26_RX_CH), 0);

    // Make sure, we don't get a spurious interrupt after we leave
    // this function, by clearing the interrupts.
    REG_WR(VTSS_FDMA_INTR_CLEAR_BLOCK, VTSS_F_FDMA_INTR_CLEAR_BLOCK_CLEAR_BLOCK(rx_mask));
    REG_WR(VTSS_FDMA_INTR_CLEAR_TFR, VTSS_F_FDMA_INTR_CLEAR_TFR_CLEAR_TFR(rx_mask));
    REG_WR(VTSS_FDMA_INTR_CLEAR_ERR, VTSS_F_FDMA_INTR_CLEAR_ERR_CLEAR_ERR(rx_mask));

    // At this point, the FDMA Rx H/W doesn't have any references
    // to DCBs or frame data anymore. The channel is still
    // enabled interrupt-wise, but that's OK, because we expect
    // the user of the uFDMA to feed a new Rx buffer chain
    // into us shortly.
    return UFDMA_RC_OK;
}

/**
 * CIL_tx_reinit()
 */
static int CIL_tx_reinit(ufdma_state_t *state)
{
    u32 val, tx_mask = VTSS_BIT(LU26_TX_CH);

    // Stop the channel gracefully
    REG_WR(VTSS_FDMA_MISC_CH_EN_REG, VTSS_F_FDMA_MISC_CH_EN_REG_CH_EN_WE(tx_mask)); // CH_EN_WE = mask, CH_EN = 0

    // Wait for it to disable itself.
    do {
        val = REG_RD(VTSS_FDMA_MISC_CH_EN_REG);
    } while ((VTSS_X_FDMA_MISC_CH_EN_REG_CH_EN(val) & tx_mask) != 0);

    // Set the channel's LLP field to NULL
    REG_WR(VTSS_FDMA_CH_LLP(LU26_TX_CH), 0);

    // Make sure, we don't get a spurious interrupt after we leave
    // this function, by clearing the interrupts.
    REG_WR(VTSS_FDMA_INTR_CLEAR_TFR, VTSS_F_FDMA_INTR_CLEAR_TFR_CLEAR_TFR(tx_mask));
    REG_WR(VTSS_FDMA_INTR_CLEAR_ERR, VTSS_F_FDMA_INTR_CLEAR_ERR_CLEAR_ERR(tx_mask));

    // At this point, the FDMA Tx H/W doesn't have any references
    // to DCBs or frame data anymore. The channel is still
    // enabled interrupt-wise, but that's OK, because we expect
    // the user of the uFDMA to feed new frames to Tx into us
    // shortly.
    return UFDMA_RC_OK;
}

/**
 * CIL_dcb_status_decode()
 */
static int CIL_dcb_status_decode(ufdma_state_t *state, ufdma_dcb_t *dcb, ufdma_hw_dcb_status_t *status, BOOL is_rx)
{
    ufdma_hw_dcb_v1_t *hw_dcb = &dcb->hw_dcb.v1;
    u32               ctl1, dstat, dar, size_to_subtract;

    memset(status, 0, sizeof(*status));

    // Before dereferencing the DCB, we must invalidate the cache line(s) occupied by it.
    DCACHE_INVALIDATE(hw_dcb, sizeof(*hw_dcb));

    // Get the status in the CPU's endianness.
    UFDMA_AIL_FUNC_RC(state, bus_to_cpu, hw_dcb->ctl1, &ctl1);

    if (is_rx) {
        // The order that the GPDMA writes back status fields is: First write back hw_dcb->ctl1, then write back hw_dcb->dstat.
        // This means that we have to check DSTAT.VLD field before the CTL1.DONE bit.

        // Get the status information for this DCB. This information was automatically
        // fetched by the General Purpose DMA from the VTSS_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB[LU26_RX_GRP]
        // register after the transfer completed.
        // This means that we can use that register's bit definitions when referring to
        // bits within the dstat.
        UFDMA_AIL_FUNC_RC(state, bus_to_cpu, hw_dcb->stat, &dstat);

        // If DSTAT.VLD is not set, this DCB has not been filled by H/W, so
        // leave fragment_size_bytes set to 0.
        if (!VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_VLD(dstat)) {
            return UFDMA_RC_OK;
        }

        if (VTSS_X_FDMA_CH_CTL1_DONE(ctl1) == 0) {
            UFDMA_EG(RX, "dstat (0x%08x) indicates valid, but ctl1 (0x%08x) doesn't indicate done", dstat, ctl1);
            return UFDMA_RC_CIL;
        }

        status->sof     = VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_SOF(dstat);
        status->eof     = VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_EOF(dstat);
        status->aborted = VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_ABORT(dstat);
        status->pruned  = VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_PRUNED(dstat);

        // The following is the accummulated frame size, so if a frame spans
        // multiple DCBs, the fragment sizes of previous DCBs must be subtracted
        // from this number before it can be used for anything, because the AIL
        // expects the size of this particular DCB, and not the entire frame
        // until now.
        status->fragment_size_bytes = VTSS_X_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB_XTR_STAT_FRM_LEN(dstat);

        if (status->fragment_size_bytes && !status->sof) {
            // This is a non-first fragment. Adjust it's length.
            // The AIL layer saves the so far accummulated length in the first
            // pending DCB's length, so all we need to do is to subtract that
            // length from this fragment's length.
            if (state->rx_head_sw_pending) {
                size_to_subtract = state->rx_head_sw_pending->buf_dscr.frm_length_bytes;
                if (size_to_subtract < status->fragment_size_bytes) {
                    status->fragment_size_bytes -= size_to_subtract;
                } else {
                    UFDMA_EG(RX, "Got non-SOF fragment whose accummulated size is %u, but until now, %u bytes have been extracted", status->fragment_size_bytes, size_to_subtract);
                }
            } else {
                UFDMA_EG(RX, "Huh? Not SOF DCB but no fragments pending?!?");
            }
        }
    } else {
        UFDMA_AIL_FUNC_RC(state, cpu_to_bus, hw_dcb->dar, &dar);
        status->sof     = VTSS_X_FDMA_DAR_SOF(dar);
        status->eof     = VTSS_X_FDMA_DAR_EOF(dar);
        status->tx_done = VTSS_X_FDMA_CH_CTL1_DONE(ctl1);
    }

    return UFDMA_RC_OK;
}

/**
 * CIL_rx_qu_mask_get()
 */
static int CIL_rx_qu_mask_get(ufdma_state_t *state, ufdma_dcb_t *dcb, u32 *rx_qu_mask)
{
    // The physical queues that #dcb's frame would have been forwarded to is
    // given by IFH.cpu_queue_mask. The actual queue is the most significant of these.
    *rx_qu_mask = (((dcb->buf_dscr.buf[4] << 8) | (dcb->buf_dscr.buf[5])) >> 4) & 0xFF;

    return UFDMA_RC_OK;
}

/**
 * CIL_rx_frm_drop()
 */
static int CIL_rx_frm_drop(ufdma_state_t *state, ufdma_dcb_t *dcb, BOOL *drop)
{
    *drop = FALSE;
    return UFDMA_RC_OK;
}

/**
 * CIL_uninit()
 */
static int CIL_uninit(ufdma_state_t *state)
{
    CIL_interrupts_disable(state, TRUE);

    // Globally disable DMA controller
    REG_WR(VTSS_FDMA_MISC_DMA_CFG_REG, 0);

    // Globally disable FDMA's access to the queue system
    REG_WR(VTSS_ICPU_CFG_GPDMA_FDMA_CFG, 0);

    return UFDMA_RC_OK;
}

/**
 * CIL_init()
 */
static int CIL_init(vtss_ufdma_platform_driver_t *self, vtss_ufdma_init_conf_t *init_conf)
{
    ufdma_state_t *state;
    u32           val, pcp, dei;

    // Call AIL. This will a.o. check #self and #init_conf and
    // install the remaining public functions.
    UFDMA_RC(vtss_ufdma_init(self, init_conf, FALSE, &state));

    // Set-up our CIL functions and platform-specific variables
    state->cil.rx_dcb_init           = CIL_rx_dcb_init;
    state->cil.tx_dcb_init           = CIL_tx_dcb_init;
    state->cil.rx_start              = CIL_rx_start;
    state->cil.tx_start              = CIL_tx_start;
    state->cil.rx_reinit             = CIL_rx_reinit;
    state->cil.tx_reinit             = CIL_tx_reinit;
    state->cil.poll                  = CIL_poll;
    state->cil.dcb_status_decode     = CIL_dcb_status_decode;
    state->cil.rx_qu_mask_get        = CIL_rx_qu_mask_get;
    state->cil.rx_frm_drop           = CIL_rx_frm_drop;
    state->cil.uninit                = CIL_uninit;
    state->cil.debug_print           = CIL_debug_print;
    state->cil.hw_dcb_next           = CIL_hw_dcb_next;
    state->cil.rx_qu_suspend_set     = CIL_rx_qu_suspend_set;
    state->cil.rx_buf_size_bytes_min = 64 + LU26_RX_IFH_SIZE_BYTES; // Frames must be at least 64 bytes
    state->cil.rx_buf_size_bytes_max = 16384;
    state->cil.tx_buf_size_bytes_max = 16384;

    // The allocation length we configure in hw_dcb->sar must be a multiple of the GPDMA burst size.
    // The MSIZE enumeration table looks like:
    //   MSIZE == 0 =>   1 item
    //   MSIZE == 1 =>   4 items
    //   MSIZE == 2 =>   8 items
    //   MSIZE == 3 =>  16 items
    //   MSIZE == 4 =>  32 items
    //   MSIZE == 5 =>  64 items
    //   MSIZE == 6 => 128 items
    //   MSIZE == 7 => 256 items
    // We multiple the number of items by 4 to get it in bytes, since we always transfer 32-bit items.
#if RX_SRC_MSIZE == 0
    state->cil.rx_burst_size_bytes   = 4 * 1;
#else
    state->cil.rx_burst_size_bytes   = 4 * 2 * (1 << (RX_SRC_MSIZE));
#endif

    CIL_interrupts_disable(state, FALSE);

    /*
     * Common initialization (part 1)
     */

    // Enable FDMA's access to the queue system.
    REG_WR(VTSS_ICPU_CFG_GPDMA_FDMA_CFG, VTSS_F_ICPU_CFG_GPDMA_FDMA_CFG_FDMA_ENA);

    // Globally enable DMA controller
    REG_WR(VTSS_FDMA_MISC_DMA_CFG_REG, VTSS_F_FDMA_MISC_DMA_CFG_REG_DMA_EN);

    /*
     * Rx initialization
     */

    // By default, the API sets up the DEVCPU Queue System for byte swapping and status word just before
    // the last data to prepare for register-based frame reception.
    // The FDMA needs the status word *after* the last data, so we must change the register for that.
    // And it needs the data in little endian.
    REG_WRM(VTSS_DEVCPU_QS_XTR_XTR_GRP_CFG(LU26_RX_GRP), VTSS_F_DEVCPU_QS_XTR_XTR_GRP_CFG_STATUS_WORD_POS | VTSS_F_DEVCPU_QS_XTR_XTR_GRP_CFG_BYTE_SWAP, VTSS_M_DEVCPU_QS_XTR_XTR_GRP_CFG_STATUS_WORD_POS | VTSS_M_DEVCPU_QS_XTR_XTR_GRP_CFG_BYTE_SWAP);

    REG_WR(VTSS_ICPU_CFG_GPDMA_FDMA_CH_CFG(LU26_RX_CH), VTSS_F_ICPU_CFG_GPDMA_FDMA_CH_CFG_CH_ENA);

    // Burst is the same as set-up in extraction DCBs. Can be tweaked.
    REG_WR(VTSS_ICPU_CFG_GPDMA_FDMA_XTR_CFG(LU26_RX_GRP), VTSS_F_ICPU_CFG_GPDMA_FDMA_XTR_CFG_XTR_BURST_SIZE(RX_SRC_MSIZE));

    // We just need to enable LLP_SRC_EN and LLP_DST_EN here, since a new descriptor is loaded in just a second.
    // This new descriptor sets up the remaining fields.
    REG_WR(VTSS_FDMA_CH_CTL0(LU26_RX_CH), VTSS_F_FDMA_CH_CTL0_LLP_SRC_EN  | VTSS_F_FDMA_CH_CTL0_LLP_DST_EN);

    // HS_SEL_SRC=Hardware Initiated, HS_SEL_DST=1=Software Initiated, use programmed priority
    REG_WR(VTSS_FDMA_CH_CFG0(LU26_RX_CH), /* VTSS_F_FDMA_CH_CFG0_LOCK_CH | VTSS_F_FDMA_CH_CFG0_LOCK_CH_L(1) | */ VTSS_F_FDMA_CH_CFG0_HS_SEL_DST  | VTSS_F_FDMA_CH_CFG0_CH_PRIOR(LU26_RX_PRIO));

    // SRC_PER=channel, DS_UPD_EN=1, VTSS_BIT(2) = Reserved = must be 1.
    val = VTSS_F_FDMA_CH_CFG1_SRC_PER(LU26_RX_CH) | VTSS_F_FDMA_CH_CFG1_DS_UPD_EN | VTSS_BIT(2) | VTSS_F_FDMA_CH_CFG1_FCMODE;
#if RX_FIFOMODE
    val |= VTSS_F_FDMA_CH_CFG1_FIFOMODE;
#endif /* RX_FIFO_MODE */
    REG_WR(VTSS_FDMA_CH_CFG1(LU26_RX_CH), val);

    // With the DS_UPD_EN bit set in CFG1, we must also set the address from which the destination update status
    // is fetched by the DMA controller. The ICPU_CFG:GPDMA:FDMA_XTR_STAT_LAST_DCB[LU26_RX_GRP] provides the needed information
    // (VLD, SOF, EOF, PRUNED, ABORT, FRM_LEN).
    REG_WR(VTSS_FDMA_CH_DSTATAR(LU26_RX_CH), UFDMA_TO_PHYS(VTSS_ICPU_CFG_GPDMA_FDMA_XTR_STAT_LAST_DCB(LU26_RX_GRP)));

    // Disable aging of Rx CPU queues to allow the frames to stay there longer than
    // on normal front ports.
    REG_WRM(VTSS_REW_PORT_PORT_CFG(LU26_CHIP_PORT_CPU), VTSS_F_REW_PORT_PORT_CFG_AGE_DIS, VTSS_M_REW_PORT_PORT_CFG_AGE_DIS);

    // Disallow the CPU Rx queues to use shared memory.
    REG_WRM(VTSS_SYS_SYSTEM_EGR_NO_SHARING, VTSS_BIT(LU26_CHIP_PORT_CPU), VTSS_BIT(LU26_CHIP_PORT_CPU));

    // Don't make head-of-line-blocking of frames going to the CPU, as this may
    // cause pause-frames to be sent out on flow-control-enabled front ports.
    REG_WRM(VTSS_SYS_PAUSE_CFG_EGR_DROP_FORCE, VTSS_BIT(LU26_CHIP_PORT_CPU), VTSS_BIT(LU26_CHIP_PORT_CPU));

    /*
     * Tx initialization
     */

    // Setup back-pressure from the injection group in question to the DMA channel in question.
    REG_WR(VTSS_ICPU_CFG_GPDMA_FDMA_INJ_CFG(LU26_TX_GRP), VTSS_F_ICPU_CFG_GPDMA_FDMA_INJ_CFG_INJ_GRP_BP_ENA | VTSS_F_ICPU_CFG_GPDMA_FDMA_INJ_CFG_INJ_GRP_BP_MAP(LU26_TX_CH));

    // Enable linked list DCB operation.
    // We just need to enable LLP_SRC_EN and LLP_DST_EN here since the first descriptor overwrites the remaining fields anyway.
    REG_WR(VTSS_FDMA_CH_CTL0(LU26_TX_CH), VTSS_F_FDMA_CH_CTL0_LLP_SRC_EN | VTSS_F_FDMA_CH_CTL0_LLP_DST_EN);

    REG_WR(VTSS_FDMA_CH_CFG0(LU26_TX_CH), VTSS_F_FDMA_CH_CFG0_CH_PRIOR(LU26_TX_PRIO));

    // VTSS_BIT(2) = Reserved. Must be 1. DST_PER = handshake interface to obey, which is identical to the channel number (used for normal injection).
    // SRC_PER = handshake interface to obey, which is identical to the channel number (used for AFI injection).
    val = VTSS_F_FDMA_CH_CFG1_DST_PER(LU26_TX_CH) | VTSS_F_FDMA_CH_CFG1_SRC_PER(LU26_TX_CH) | VTSS_BIT(2) | VTSS_F_FDMA_CH_CFG1_FCMODE;
#if TX_FIFOMODE
    val |= VTSS_F_FDMA_CH_CFG1_FIFOMODE;
#endif /* TX_FIFOMODE */
    REG_WR(VTSS_FDMA_CH_CFG1(LU26_TX_CH), val);

    // Setup the usage of this DMA channel to be injection, and enable the channel.
    REG_WR(VTSS_ICPU_CFG_GPDMA_FDMA_CH_CFG(LU26_TX_CH), VTSS_F_ICPU_CFG_GPDMA_FDMA_CH_CFG_USAGE | VTSS_F_ICPU_CFG_GPDMA_FDMA_CH_CFG_CH_ENA); // USAGE == Injection and CH_ENA

    // Setup the CPU port as VLAN aware to support switching frames based on tags
    REG_WR(VTSS_ANA_PORT_VLAN_CFG(LU26_CHIP_PORT_CPU), VTSS_F_ANA_PORT_VLAN_CFG_VLAN_AWARE_ENA | VTSS_F_ANA_PORT_VLAN_CFG_VLAN_POP_CNT(1) | VTSS_F_ANA_PORT_VLAN_CFG_VLAN_VID(1));

    // Disable learning (only RECV_ENA must be set)
    REG_WR(VTSS_ANA_PORT_PORT_CFG(LU26_CHIP_PORT_CPU), VTSS_F_ANA_PORT_PORT_CFG_RECV_ENA);

    // Setup CPU port 0 and 1 to allow for classification of transmission of
    // switched frames into a user-module-specifiable QoS class.
    // For the two CPU ports, we set a one-to-one mapping between a VLAN tag's
    // PCP and a QoS class. When transmitting switched frames, the PCP value
    // of the VLAN tag (which is always inserted to get it switched on a given
    // VID), then controls the priority.
    // Enable looking into PCP bits
    REG_WR(VTSS_ANA_PORT_QOS_CFG(LU26_CHIP_PORT_CPU), VTSS_F_ANA_PORT_QOS_CFG_QOS_PCP_ENA);

    // Set-up the one-to-one mapping
    for (pcp = 0; pcp < 8; pcp++) {
        for (dei = 0; dei < 2; dei++) {
            REG_WR(VTSS_ANA_PORT_QOS_PCP_DEI_MAP_CFG(LU26_CHIP_PORT_CPU, (8 * dei + pcp)), VTSS_F_ANA_PORT_QOS_PCP_DEI_MAP_CFG_QOS_PCP_DEI_VAL(pcp));
        }
    }

    /*
     * Common initialization (part 2)
     */

    CIL_interrupts_enable(state);

    return UFDMA_RC_OK;
}

/**
 * Driver structure for Luton26
 */
vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_luton26 = {
    .props = {
        .rx_ifh_size_bytes      = LU26_RX_IFH_SIZE_BYTES,
        .tx_ifh_size_bytes      = LU26_TX_IFH_SIZE_BYTES,
        .buf_state_size_bytes   = sizeof(ufdma_dcb_t) + UFDMA_DCACHE_LINE_SIZE_BYTES, // We add Cache Line size because we want to be able to align the H/W DCB to a cache line no matter which pointer the user comes with.
        .ufdma_state_size_bytes = 8 * ((sizeof(ufdma_state_t) + 7) / 8)               // Make sure we can make an 8-byte aligned pointer out of the state that the user comes with.
    },

    // The only driver function that needs be installed
    // to start with is the init() function.
    // The reamaining entry points are AIL functions that are
    // installed by the AIL layer once the CIL_init()
    // function calls the AIL init() function.
    .init = CIL_init,
};
