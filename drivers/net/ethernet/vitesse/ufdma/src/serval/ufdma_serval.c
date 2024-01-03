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

#include "vtss_ufdma_api.h"    /* Public header file            */
#include "../ail/ufdma.h"      /* Internal header file          */
#include "ufdma_serval_regs.h" /* For chip register definitions */
#ifdef __KERNEL__
#include <linux/string.h>      /* For memset()                  */
#else
#include <string.h>            /* For memset()                  */
#endif

#define SRVL_RX_IFH_SIZE_BYTES 16
#define SRVL_TX_IFH_SIZE_BYTES 16
#define SRVL_RX_CH              0
#define SRVL_TX_CH              2
#define SRVL_RX_GRP            SRVL_RX_CH /* One-to-one correspondence between extraction group and channel number */
#define SRVL_TX_GRP             0         /* Use injection group 0 */
#define SRVL_RX_PRIO            3
#define SRVL_TX_PRIO            3
#define SRVL_CHIP_PORT_CPU     11         /* First CPU port */

/**
 * CIL_debug_print()
 */
static int CIL_debug_print(ufdma_state_t *state, vtss_ufdma_debug_info_t *info, int (*pr)(void *ref, const char *fmt, ...))
{
    u32  i, val, irq;
    void *ref    = info->ref;
    BOOL pr_full = (info->full != 0);
    BOOL pr_rx   = (info->group == VTSS_UFDMA_DEBUG_GROUP_ALL || info->group == VTSS_UFDMA_DEBUG_GROUP_RX);
    BOOL pr_tx   = (info->group == VTSS_UFDMA_DEBUG_GROUP_ALL || info->group == VTSS_UFDMA_DEBUG_GROUP_TX);

    if (pr_rx) {
        pr(ref, "Rx channel: %u\n", SRVL_RX_CH);
    }

    if (pr_tx) {
        pr(ref, "Tx channel: %u\n", SRVL_TX_CH);
    }

    pr(ref, "\nICPU_CFG:FDMA registers:\n");
    for (i = 0; i < 4; i++) {
        if (!pr_full) {
            if (i == SRVL_RX_CH) {
                if (!pr_rx) {
                    continue;
                }
            } else if (i == SRVL_TX_CH) {
                if (!pr_tx) {
                    continue;
                }
            } else {
                continue;
            }
        }

        pr(ref, " FDMA_DCB_LLP[%u]      = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(i)));
        pr(ref, " FDMA_DCB_DATAP[%u]    = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_DCB_DATAP(i)));
        pr(ref, " FDMA_DCB_DATAL[%u]    = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_DCB_DATAL(i)));
        pr(ref, " FDMA_DCB_STAT[%u]     = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_DCB_STAT(i)));
        pr(ref, " FDMA_DCB_LLP_PREV[%u] = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP_PREV(i)));
    }

    for (i = 0; i < 4; i++) {
        if (!pr_full) {
            if (i == SRVL_RX_CH) {
                if (!pr_rx) {
                    continue;
                }
            } else if (i == SRVL_TX_CH) {
                if (!pr_tx) {
                    continue;
                }
            } else {
                continue;
            }
        }

        pr(ref, " FDMA_CH_CNT[%u]       = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_CNT(i)));
        pr(ref, " FDMA_CH_CFG[%u]       = 0x%08x\n", i, REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_CFG(i)));
    }

    pr(ref, " FDMA_CH_STAT         = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_STAT));
    pr(ref, " FDMA_CH_SAFE         = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE));
    pr(ref, " FDMA_CH_ACTIVATE     = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_ACTIVATE));
    pr(ref, " FDMA_CH_DISABLE      = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_DISABLE));
    pr(ref, " FDMA_CH_FORCEDIS     = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_FORCEDIS));
    pr(ref, " FDMA_EVT_ERR         = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR));
    pr(ref, " FDMA_EVT_ERR_CODE    = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR_CODE));
    pr(ref, " FDMA_INTR_LLP        = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP));
    pr(ref, " FDMA_INTR_LLP_ENA    = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP_ENA));
    pr(ref, " FDMA_INTR_FRM        = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM));
    pr(ref, " FDMA_INTR_FRM_ENA    = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM_ENA));
    pr(ref, " FDMA_INTR_SIG        = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG));
    pr(ref, " FDMA_INTR_SIG_ENA    = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG_ENA));
    pr(ref, " FDMA_INTR_ENA        = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_ENA));
    pr(ref, " FDMA_INTR_IDENT      = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_IDENT));
    pr(ref, " FDMA_GCFG            = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_GCFG));
    pr(ref, " FDMA_GSTAT           = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_GSTAT));
    pr(ref, " FDMA_IDLECNT         = 0x%08x\n",   REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_IDLECNT));
    pr(ref, " FDMA_CONST           = 0x%08x\n\n", REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CONST));

    if (pr_rx) {
        pr(ref, "Rx group: %u\n", SRVL_RX_GRP);
    }

    if (pr_tx) {
        pr(ref, "Tx group: %u\n", SRVL_TX_GRP);
    }

    if (pr_rx) {
        pr(ref, "\nDEVCPU_QS:XTR registers:\n");
        for (i = 0; i < 2; i++) {
            if (!pr_full) {
                if (i != SRVL_RX_GRP) {
                    continue;
                }
            }

            pr(ref, " XTR_GRP_CFG[%u]       = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_XTR_XTR_GRP_CFG(i)));
            pr(ref, " XTR_FRM_PRUNING[%u]   = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_XTR_XTR_FRM_PRUNING(i)));
        }

        pr(ref, " XTR_FLUSH            = 0x%08x\n", REG_RD(VTSS_DEVCPU_QS_XTR_XTR_FLUSH));
        pr(ref, " XTR_DATA_PRESENT     = 0x%08x\n", REG_RD(VTSS_DEVCPU_QS_XTR_XTR_DATA_PRESENT));
    }

    if (pr_tx) {
        pr(ref, "\nDEVCPU_QS:INJ registers:\n");
        for (i = 0; i < 2; i++) {
            if (!pr_full) {
                if (i != SRVL_TX_GRP) {
                    continue;
                }
            }

            pr(ref, " INJ_GRP_CFG[%u]       = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_INJ_INJ_GRP_CFG(i)));
            pr(ref, " INJ_CTRL[%u]          = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_INJ_INJ_CTRL(i)));
            pr(ref, " INJ_ERR[%u]           = 0x%08x\n", i, REG_RD(VTSS_DEVCPU_QS_INJ_INJ_ERR(i)));
        }

        pr(ref, " INJ_STATUS           = 0x%08x\n", REG_RD(VTSS_DEVCPU_QS_INJ_INJ_STATUS));
    }

    if (pr_rx) {
        // For throttling
        pr(ref, "\nQSYS:SYSTEM registers\n");
        pr(ref, " CPU_GROUP_MAP        = 0x%08x\n", REG_RD(VTSS_QSYS_SYSTEM_CPU_GROUP_MAP));
    }

    irq = state->cil.chip_arch == CHIP_ARCH_SERVAL ? 15 : 16;
    pr(ref, "\nFDMA IRQ: %d supposed to go into DST_INTR_MAP[0]\n", irq);
    pr(ref, "ICPU_CFG:INTR registers:\n");
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR_RAW);
    pr(ref, " INTR_RAW             = 0x%08x (bit %u = %u)\n", val, irq, (val >> irq) & 1);
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR_STICKY);
    pr(ref, " INTR_STICKY          = 0x%08x (bit %u = %u)\n", val, irq, (val >> irq) & 1);
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR_BYPASS);
    pr(ref, " INTR_BYPASS          = 0x%08x (bit %u = %u)\n", val, irq, (val >> irq) & 1);
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR_ENA);
    pr(ref, " INTR_ENA             = 0x%08x (bit %u = %u)\n", val, irq, (val >> irq) & 1);
    val = REG_RD(VTSS_ICPU_CFG_INTR_INTR_IDENT);
    pr(ref, " INTR_IDENT           = 0x%08x (bit %u = %u)\n", val, irq, (val >> irq) & 1);
    for (i = 0; i < 4; i++) {
        val = REG_RD(VTSS_ICPU_CFG_INTR_DST_INTR_MAP(i));
        pr(ref, " DST_INTR_MAP[%u]      = 0x%08x (bit %u = %u)\n", i, val, irq, (val >> irq) & 1);
    }

    pr(ref, "\n");
    return UFDMA_RC_OK;
}

/**
 * CIL_hw_dcb_next()
 */
static void *CIL_hw_dcb_next(ufdma_state_t *state, ufdma_dcb_t *dcb)
{
    return (void *)dcb->hw_dcb.v2.llp;
}

/**
 * CIL_rx_qu_suspend_set()
 */
static int CIL_rx_qu_suspend_set(ufdma_state_t *state, u32 rx_qu, BOOL suspend)
{
    // On Serval there are two CPU ports (physical #11 and #12). When running only on
    // the internal CPU, we only use #11. However, some applications may
    // want to divide the Rx queues among them and forward some queues to
    // #11 and others to #12, which can then be read by the external CPU.
    // If enabling throttling, the internal CPU will suspend a given queue by
    // moving that queue from port #11 to port #12, which might have some
    // unexpected side-effects on an external CPU reading port #12.
    u32 mask;

    // First make sure that port #12 is flushing all data it receives.
    mask = VTSS_F_DEVCPU_QS_XTR_XTR_FLUSH_FLUSH(VTSS_BIT(1));
    REG_WRM(VTSS_DEVCPU_QS_XTR_XTR_FLUSH, mask, mask);

    // Then either redirect the queue to port #11 (ourselves) or port #12 (suspend)
    mask = VTSS_F_QSYS_SYSTEM_CPU_GROUP_MAP_CPU_GROUP_MAP(VTSS_BIT(rx_qu));
    REG_WRM(VTSS_QSYS_SYSTEM_CPU_GROUP_MAP, suspend ? mask : 0, mask);

    return UFDMA_RC_OK;
}

/**
 * CIL_poll()
 */
static int CIL_poll(ufdma_state_t *state, BOOL rx, BOOL tx, unsigned int rx_cnt_max)
{
    u32 rx_mask    = VTSS_BIT(SRVL_RX_CH);
    u32 tx_mask    = VTSS_BIT(SRVL_TX_CH);
    u32 ch_mask    = (rx ? rx_mask : 0) | (tx ? tx_mask : 0);
    u32 intr_ident = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_IDENT) & ch_mask; // Which enabled channel is interrupting?
    u32 intr_err   = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR)    & ch_mask; // Who is signaling error events?
    u32 intr_llp   = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP)   & ch_mask; // Who is signaling LLP interrupts?
    u32 intr_frm   = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM)   & ch_mask; // Who is signaling FRM interrupts?
    u32 intr_sig   = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG)   & ch_mask; // Who is signaling SIG interrupts?
    u32 err_mask   = intr_err & intr_ident;

    // We don't expect any errors. Write a message and clear them if they occur.
    if (err_mask != 0) {
        UFDMA_E("intr_err (%u) & intr_ident (%u) != 0", intr_err, intr_ident);

        // Force the channel to disabled state.
        REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_CH_FORCEDIS, err_mask);

        // Clear the sticky error
        REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR, err_mask);
    }

    // We also don't expect any SIG interrupts (for now). Write a message and clear them if they occur.
    if ((intr_sig & intr_ident) != 0) {
        UFDMA_E("intr_sig (%u) & intr_ident (%u) != 0", intr_sig, intr_ident);
        REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG, intr_sig & intr_ident);
    }

    // Clear interrupts.
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP, intr_llp & intr_ident);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM, intr_frm & intr_ident);

    UFDMA_AIL_FUNC_RC(state, reorder_barrier);

    if (rx) {
        if (intr_err & rx_mask) {
            // Gotta restart the channel in case of an error.
            // The channel is already force-disabled.
            // Clear the LLP pointer in H/W.
            REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_RX_CH), 0);

            // All the DCBs currently handed to H/W need to be moved
            // back to S/W to get re-initialized, in case H/W has filled
            // them partly.
            UFDMA_AIL_FUNC_RC(state, rx_buf_recycle_all);
        } else {
            // rx_frm() will also check if we need to restart the channel afterwards.
            UFDMA_AIL_FUNC_RC(state, rx_frm, 0, rx_cnt_max);
        }
    }

    if (tx) {
        UFDMA_AIL_FUNC_RC(state, tx_done);
    }

    return UFDMA_RC_OK;
}

/**
 * CIL_interrupts_enable()
 */
static void CIL_interrupts_enable(ufdma_state_t *state)
{
    u32 ch_mask = VTSS_BIT(SRVL_RX_CH) | VTSS_BIT(SRVL_TX_CH);

    // Clear and enable FRM interrupts
    REG_WR( VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM,     ch_mask);
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM_ENA, ch_mask, ch_mask);

    // Clear and enable LLP interrupts
    REG_WR( VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP,     ch_mask);
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP_ENA, ch_mask, ch_mask);

    // Enable global interrupts from DMA controller
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_INTR_ENA,     ch_mask, ch_mask);
}

/**
 * CIL_interrupts_disable()
 */
static void CIL_interrupts_disable(ufdma_state_t *state, BOOL graceful)
{
    u32 ch_mask = VTSS_BIT(SRVL_RX_CH) | VTSS_BIT(SRVL_TX_CH);

    // Disable all channels
    if (graceful) {
        u32 val;

        REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_CH_DISABLE, ch_mask);
        do {
            val = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE);
        } while ((val & ch_mask) != ch_mask);
    }

    // Ensure we also force disable it, now that it's safe to change LLP (in
    // case we were invoked with graceful == TRUE)
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_CH_FORCEDIS, ch_mask);

    // And clear LLP pointers
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_RX_CH), 0);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_TX_CH), 0);

    // Disable global interrupts from DMA controller
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_INTR_ENA,     ~ch_mask, ch_mask);

    // Disable and clear SIG interrupts
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG_ENA, ~ch_mask, ch_mask);
    REG_WR( VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG,      ch_mask);

    // Disable and clear FRM interrupts
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM_ENA, ~ch_mask, ch_mask);
    REG_WR( VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM,      ch_mask);

    // Disable and clear LLP interrupts
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP_ENA, ~ch_mask, ch_mask);
    REG_WR( VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP,      ch_mask);
}

/**
 * CIL_enable_ch()
 */
static void CIL_enable_ch(ufdma_state_t *state, u32 ch)
{
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_CH_ACTIVATE, VTSS_BIT(ch), VTSS_BIT(ch));
}

/**
 * CIL_rx_dcb_init()
 */
static int CIL_rx_dcb_init(ufdma_state_t *state, ufdma_dcb_t *dcb, u32 buf_size_bytes_aligned)
{
    ufdma_hw_dcb_v2_t *hw_dcb = &dcb->hw_dcb.v2;

    // #buf_size_bytes_aligned contains a value that is guaranteed to be a multiple of
    // our rx_burst_size_bytes, which is needed because DATAL must have 32-bit words,
    // that is, bit 1:0 must be 0.

    // Initialize the DCB area
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, (u32)VIRT_TO_PHYS(dcb->buf_dscr.buf),                                                                                     &hw_dcb->datap);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, (VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_DATAL_SW(SRVL_RX_CH)) | VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_DATAL_DATAL(buf_size_bytes_aligned), &hw_dcb->datal);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, dcb->next ? (u32)VIRT_TO_PHYS(&dcb->next->hw_dcb.v2) : 0,                                                                 &hw_dcb->llp);
    hw_dcb->stat  = 0; // Important to reset this one, because it a.o. eventually will hold the number of bytes received into this DCB's buffer, which is used to differentiate between a used and an unused DCB.

    // Get the DCB written to main memory
    DCACHE_FLUSH(hw_dcb, sizeof(*hw_dcb));

    return UFDMA_RC_OK;
}

/**
 * CIL_tx_dcb_init()
 */
static int CIL_tx_dcb_init(ufdma_state_t *state, ufdma_dcb_t *dcb, ufdma_dcb_t *dcb_prev)
{
    ufdma_hw_dcb_v2_t *hw_dcb = &dcb->hw_dcb.v2;
    u32               datap, min_len, len;

    // Auto-adjust the length to form a minimum-sized Ethernet frame.
    len = dcb->buf_dscr.buf_size_bytes;
    min_len = state->self->props.tx_ifh_size_bytes + 64;
    if (len < min_len) {
        UFDMA_IG(TX, "Auto-adjusting frame size from %u to %u bytes", len, min_len);
        len = min_len;
    }

    datap = (u32)VIRT_TO_PHYS(dcb->buf_dscr.buf);

    hw_dcb->llp = 0; // We only have one frame in #dcb, but below, we might end up appending it to #dcb_prev.
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, datap & ~0x3,                                                     &hw_dcb->datap);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus, VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_DATAL_DATAL(0xFFFF) /* whatever */, &hw_dcb->datal);
    UFDMA_AIL_FUNC_RC(state, cpu_to_bus,
                      VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKO(datap & 0x3) |
                      VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_EOF                 |
                      VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_SOF                 |
                      VTSS_F_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKL(len),                                      &hw_dcb->stat);

    // Get the DCB written to main memory
    DCACHE_FLUSH(hw_dcb, sizeof(*hw_dcb));

    if (dcb_prev) {
        // We also need to link the previous DCB to this one.
        ufdma_hw_dcb_v2_t *hw_dcb_prev = &dcb_prev->hw_dcb.v2;

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
    u32 val;

    if (REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_RX_CH)) != 0) {
        // The FDMA is not yet done.
        *restarted = FALSE;
        return UFDMA_RC_OK;
    }

    // Pause the channel gracefully
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_CH_DISABLE, VTSS_BIT(SRVL_RX_CH));

    do {
        val = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE);
    } while ((val & VTSS_BIT(SRVL_RX_CH)) == 0);

    // Feed #head into the H/W.
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_RX_CH), (u32)VIRT_TO_PHYS(&head->hw_dcb.v2));

    // Make sure the above lines are executed before enabling
    // the channel - hence the reorder barrier.
    UFDMA_AIL_FUNC_RC(state, reorder_barrier);

    // Re-activate the channel.
    CIL_enable_ch(state, SRVL_RX_CH);

    *restarted = TRUE;
    return UFDMA_RC_OK;
}

/**
 * CIL_tx_start()
 */
static int CIL_tx_start(ufdma_state_t *state, ufdma_dcb_t *head)
{
    // Channel must be inactive
    u32 ch_safe = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE);

    if ((ch_safe & VTSS_BIT(SRVL_TX_CH)) == 0) {
        UFDMA_EG(TX, "Tx channel (%u) not safe: 0x%08x", SRVL_TX_CH, ch_safe);
        // Fall through and write it anyway.
    }

    // Hand the DCB list to the channel
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_TX_CH), (u32)VIRT_TO_PHYS(&head->hw_dcb.v2));

    // And start the channel
    CIL_enable_ch(state, SRVL_TX_CH);

    return UFDMA_RC_OK;
}

/**
 * CIL_rx_reinit()
 */
static int CIL_rx_reinit(ufdma_state_t *state)
{
    u32 val, rx_mask = VTSS_BIT(SRVL_RX_CH);

    // Stop the channel gracefully
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_CH_DISABLE, VTSS_BIT(SRVL_RX_CH));

    do {
        val = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE);
    } while ((val & VTSS_BIT(SRVL_RX_CH)) == 0);

    // Set the channel's LLP field to NULL
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_RX_CH), 0);

    // Make sure, we don't get a spurious interrupt after we leave
    // this function, by clearing the interrupts.
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP, rx_mask);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM, rx_mask);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR,  rx_mask);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG, rx_mask);

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
    u32 val, tx_mask = VTSS_BIT(SRVL_TX_CH);

    // Stop the channel gracefully
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_CH_DISABLE, VTSS_BIT(SRVL_TX_CH));

    do {
        val = REG_RD(VTSS_ICPU_CFG_FDMA_FDMA_CH_SAFE);
    } while ((val & VTSS_BIT(SRVL_TX_CH)) == 0);

    // Set the channel's LLP field to NULL
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_DCB_LLP(SRVL_TX_CH), 0);

    // Make sure, we don't get a spurious interrupt after we leave
    // this function, by clearing the interrupts.
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_LLP, tx_mask);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_FRM, tx_mask);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_EVT_ERR,  tx_mask);
    REG_WR(VTSS_ICPU_CFG_FDMA_FDMA_INTR_SIG, tx_mask);

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
    ufdma_hw_dcb_v2_t *hw_dcb = &dcb->hw_dcb.v2;
    u32               stat;

    memset(status, 0, sizeof(*status));

    // Before dereferencing the DCB, we must invalidate the cache line(s) occupied by it.
    DCACHE_INVALIDATE(hw_dcb, sizeof(*hw_dcb));

    // Get the status in the CPU's endianness.
    UFDMA_AIL_FUNC_RC(state, bus_to_cpu, hw_dcb->stat, &stat);

    status->sof = VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_SOF(stat);
    status->eof = VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_EOF(stat);

    if (is_rx) {
        // If stat.blockl is 0, we've consumed all the frames, and we're done.
        status->fragment_size_bytes = VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_BLOCKL(stat);
        status->pruned              = VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_PD(stat);
        status->aborted             = VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_ABORT(stat);
    } else {
        status->tx_done             = VTSS_X_ICPU_CFG_FDMA_FDMA_DCB_STAT_PD(stat);
    }

    return UFDMA_RC_OK;
}

/**
 * CIL_rx_qu_mask_get()
 */
static int CIL_rx_qu_mask_get(ufdma_state_t *state, ufdma_dcb_t *dcb, u32 *rx_qu_mask)
{
    // The physical queues that #dcb's frame would have been forwarded to is
    // given by IFH.CPUQ. The actual queue is the most significant of these.
    *rx_qu_mask = (((dcb->buf_dscr.buf[12] << 8) | (dcb->buf_dscr.buf[13])) >> 4) & 0xFF;

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

    return UFDMA_RC_OK;
}

/**
 * CIL_init()
 */
static int CIL_init(vtss_ufdma_platform_driver_t *self, vtss_ufdma_init_conf_t *init_conf, ufdma_chip_arch_t chip_arch)
{
    ufdma_state_t *state;
    u32           pcp, dei;

    // Call AIL. This will a.o. check #self and #init_conf and
    // install the remaining public functions.
    UFDMA_RC(vtss_ufdma_init(self, init_conf, FALSE, &state));

    // Are we instantiated as Jaguar2AB, Jaguar2C or ServalT?
    state->cil.chip_arch = chip_arch;

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
    state->cil.rx_buf_size_bytes_min = 64 + SRVL_RX_IFH_SIZE_BYTES; // Frames must be at least 64 bytes
    state->cil.rx_buf_size_bytes_max = 16384;
    state->cil.tx_buf_size_bytes_max = 16384;
    state->cil.rx_burst_size_bytes   = 4;

    CIL_interrupts_disable(state, FALSE);

    /*
     * Rx initialization
     */

    // Configure the extraction group for FDMA-based operation
    REG_WRM(VTSS_DEVCPU_QS_XTR_XTR_GRP_CFG(SRVL_RX_GRP), VTSS_F_DEVCPU_QS_XTR_XTR_GRP_CFG_MODE(2), VTSS_M_DEVCPU_QS_XTR_XTR_GRP_CFG_MODE);

    // Set channel priority
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_CH_CFG(SRVL_RX_CH), VTSS_F_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO(SRVL_RX_PRIO), VTSS_M_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO);

    // Enable IFH insertion upon Rx
    REG_WRM(VTSS_SYS_SYSTEM_PORT_MODE(SRVL_CHIP_PORT_CPU), VTSS_F_SYS_SYSTEM_PORT_MODE_INCL_XTR_HDR(1), VTSS_M_SYS_SYSTEM_PORT_MODE_INCL_XTR_HDR);

    /*
     * Tx initialization
     */

    // Setup CPU port as VLAN-aware to support switching of frames based on tags embedded in transmitted frames
    REG_WR(VTSS_ANA_PORT_VLAN_CFG(SRVL_CHIP_PORT_CPU), VTSS_F_ANA_PORT_VLAN_CFG_VLAN_AWARE_ENA | VTSS_F_ANA_PORT_VLAN_CFG_VLAN_POP_CNT(1) | VTSS_F_ANA_PORT_VLAN_CFG_VLAN_VID(1));

    // Disable learning (only RECV_ENA must be set)
    REG_WR(VTSS_ANA_PORT_PORT_CFG(SRVL_CHIP_PORT_CPU), VTSS_F_ANA_PORT_PORT_CFG_RECV_ENA);

    // Configure the injection group for FDMA-based operation
    REG_WRM(VTSS_DEVCPU_QS_INJ_INJ_GRP_CFG(SRVL_TX_GRP), VTSS_F_DEVCPU_QS_INJ_INJ_GRP_CFG_MODE(2), VTSS_M_DEVCPU_QS_INJ_INJ_GRP_CFG_MODE);

    // According to the datasheet, injection buffer watermark must be set to 13.
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_GCFG, VTSS_F_ICPU_CFG_FDMA_FDMA_GCFG_INJ_RF_WM(0xD), VTSS_M_ICPU_CFG_FDMA_FDMA_GCFG_INJ_RF_WM);

    // According to datasheet, we must insert a small delay after every end-of-frame when injecting to QS.
    REG_WR(VTSS_DEVCPU_QS_INJ_INJ_CTRL(SRVL_TX_GRP), VTSS_F_DEVCPU_QS_INJ_INJ_CTRL_GAP_SIZE(1));

    // Set channel priority
    REG_WRM(VTSS_ICPU_CFG_FDMA_FDMA_CH_CFG(SRVL_TX_CH), VTSS_F_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO(SRVL_TX_PRIO), VTSS_M_ICPU_CFG_FDMA_FDMA_CH_CFG_CH_PRIO);

    // Enable IFH parsing upon Tx
    REG_WRM(VTSS_SYS_SYSTEM_PORT_MODE(SRVL_CHIP_PORT_CPU), VTSS_F_SYS_SYSTEM_PORT_MODE_INCL_INJ_HDR(1), VTSS_M_SYS_SYSTEM_PORT_MODE_INCL_INJ_HDR);

    // Disable aging of Rx CPU queues to allow the frames to stay there longer than on normal front ports.
    REG_WRM(VTSS_REW_PORT_PORT_CFG(SRVL_CHIP_PORT_CPU), VTSS_F_REW_PORT_PORT_CFG_AGE_DIS, VTSS_F_REW_PORT_PORT_CFG_AGE_DIS);

    // Setup CPU port 0 and 1 to allow for classification of transmission of
    // switched frames into a user-module-specifiable QoS class.
    // For the two CPU ports, we set a one-to-one mapping between a VLAN tag's
    // PCP and a QoS class. When transmitting switched frames, the PCP value
    // of the VLAN tag (which is always inserted to get it switched on a given
    // VID), then controls the priority.
    // Enable looking into PCP bits
    REG_WRM(VTSS_ANA_PORT_QOS_CFG(SRVL_CHIP_PORT_CPU), VTSS_F_ANA_PORT_QOS_CFG_QOS_PCP_ENA, VTSS_F_ANA_PORT_QOS_CFG_QOS_PCP_ENA);

    // Set-up the one-to-one mapping
    for (pcp = 0; pcp < 8; pcp++) {
        for (dei = 0; dei < 2; dei++) {
            REG_WR(VTSS_ANA_PORT_QOS_PCP_DEI_MAP_CFG(SRVL_CHIP_PORT_CPU, (8 * dei + pcp)), VTSS_F_ANA_PORT_QOS_PCP_DEI_MAP_CFG_QOS_PCP_DEI_VAL(pcp));
        }
    }

    /**
     * Common Rx/Tx initialization
     */

    // Enable CPU port
    REG_WRM(VTSS_QSYS_SYSTEM_SWITCH_PORT_MODE(SRVL_CHIP_PORT_CPU), VTSS_F_QSYS_SYSTEM_SWITCH_PORT_MODE_PORT_ENA, VTSS_F_QSYS_SYSTEM_SWITCH_PORT_MODE_PORT_ENA);

    // Clear and enable LLP, FRM and global interrupts.
    CIL_interrupts_enable(state);

    return UFDMA_RC_OK;
}

/**
 * CIL_init_serval()
 */
static int CIL_init_serval(vtss_ufdma_platform_driver_t *self, vtss_ufdma_init_conf_t *init_conf)
{
    return CIL_init(self, init_conf, CHIP_ARCH_SERVAL);
}

/**
 * CIL_init_ocelot()
 */
static int CIL_init_ocelot(vtss_ufdma_platform_driver_t *self, vtss_ufdma_init_conf_t *init_conf)
{
    return CIL_init(self, init_conf, CHIP_ARCH_OCELOT);
}

/**
 * Driver structure for Serval
 */
vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_serval = {
    .props = {
        .rx_ifh_size_bytes      = SRVL_RX_IFH_SIZE_BYTES,
        .tx_ifh_size_bytes      = SRVL_TX_IFH_SIZE_BYTES,
        .buf_state_size_bytes   = sizeof(ufdma_dcb_t) + UFDMA_DCACHE_LINE_SIZE_BYTES, // We add Cache Line size because we want to be able to align the H/W DCB to a cache line no matter which pointer the user comes with.
        .ufdma_state_size_bytes = 8 * ((sizeof(ufdma_state_t) + 7) / 8)               // Make sure we can make an 8-byte aligned pointer out of the state that the user comes with.
    },

    // The only driver function that needs be installed
    // to start with is the init() function.
    // The reamaining entry points are AIL functions that are
    // installed by the AIL layer once the CIL_init_serval()
    // function calls the AIL init() function.
    .init = CIL_init_serval,
};

/**
 * Driver structure for Ocelot
 */
vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_ocelot = {
    .props = {
        .rx_ifh_size_bytes      = SRVL_RX_IFH_SIZE_BYTES,
        .tx_ifh_size_bytes      = SRVL_TX_IFH_SIZE_BYTES,
        .buf_state_size_bytes   = sizeof(ufdma_dcb_t) + UFDMA_DCACHE_LINE_SIZE_BYTES, // We add Cache Line size because we want to be able to align the H/W DCB to a cache line no matter which pointer the user comes with.
        .ufdma_state_size_bytes = 8 * ((sizeof(ufdma_state_t) + 7) / 8)               // Make sure we can make an 8-byte aligned pointer out of the state that the user comes with.
    },

    // The only driver function that needs be installed
    // to start with is the init() function.
    // The reamaining entry points are AIL functions that are
    // installed by the AIL layer once the CIL_init_ocelot()
    // function calls the AIL init() function.
    .init = CIL_init_ocelot,
};
