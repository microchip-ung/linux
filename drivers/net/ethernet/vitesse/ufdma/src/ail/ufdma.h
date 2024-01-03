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

/**
 * \file
 * \brief Internal header file for the uFDMA.
 */

#ifndef _VTSS_UFDMA_H_
#define _VTSS_UFDMA_H_

#include "vtss_ufdma_api.h" /* Our own public API */

/******************************************************************************/
//
// Typedefs
//
/******************************************************************************/
typedef unsigned char      BOOL;
typedef unsigned char      u8;
typedef unsigned int       u32; // We know we run on a 32-bit MIPS, so an integer is 32 bits.
typedef unsigned long long u64;
#define FALSE 0
#define TRUE  1

#ifndef NULL
#define NULL 0
#endif

/******************************************************************************/
//
// Various useful macros
//
/******************************************************************************/

/**
 * Check if the result of #expr is UFDMA_RC_OK. If so, continue execution,
 * otherwise return the result.
 */
#define UFDMA_RC(expr) { int __rc__ = (expr); if (__rc__ != UFDMA_RC_OK) return __rc__; }

/**
 * Get the number of elements of an array
 */
#define ARRSZ(_x_) (sizeof(_x_) / sizeof((_x_)[0]))

/**
 * Chip registers access macros.
 * #state is implicit.
 */
#define REG_RD(addr)                state->ail.reg_rd(state, addr)
#define REG_WR(addr, value)         state->ail.reg_wr(state, addr, value)
#define REG_WRM(addr, value, mask) {                  \
    u32 __v__ = REG_RD(addr);                         \
    __v__ = ((__v__ & ~(mask)) | ((value) & (mask))); \
    REG_WR(addr, __v__);                              \
}

#define REG_WRM_SET(addr, mask)         REG_WRM(addr, mask, mask)
#define REG_WRM_CLR(addr, mask)         REG_WRM(addr, 0,    mask)
#define REG_WRM_CTL(addr, _cond_, mask) REG_WRM(addr, (_cond_) ? mask : 0, mask)

/**
 * Convert a virtual to a physical address.
 * #state is implicit.
 */
#define VIRT_TO_PHYS(virt) state->callout.virt_to_phys(virt)

/**
 * Flush cache.
 * #state is implicit.
 */
#define DCACHE_FLUSH(addr, len) state->callout.cache_flush(addr, len)

/**
 * Invalidate cache.
 * #state is implicit.
 */
#define DCACHE_INVALIDATE(addr, len) state->callout.cache_invalidate(addr, len)

/**
 * Get a timestamp from the network driver.
 * #state is implicit.
 */
#define TIMESTAMP() (state->callout.timestamp ? state->callout.timestamp() : 0)

/******************************************************************************/
//
// Trace
//
/******************************************************************************/

// Trace group configuration
typedef struct {
    vtss_ufdma_trace_level_t level[VTSS_UFDMA_TRACE_LAYER_ALL]; /**< Trace level per layer */
} ufdma_trace_conf_t;

// Default trace layer to CIL.
#ifndef UFDMA_TRACE_LAYER
#define UFDMA_TRACE_LAYER VTSS_UFDMA_TRACE_LAYER_CIL
#endif

// For files with multiple trace groups
// #state is implicit
#define UFDMA_T(_grp, _lvl, ...) {                                                  \
    if (state->trace_conf[_grp].level[UFDMA_TRACE_LAYER] >= _lvl &&                 \
        state->callout.trace_printf != NULL) {                                      \
        state->callout.trace_printf(UFDMA_TRACE_LAYER, _grp, _lvl, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__); \
    }                                                                               \
}

#define UFDMA_EG(_grp, ...) UFDMA_T(VTSS_UFDMA_TRACE_GROUP_##_grp, VTSS_UFDMA_TRACE_LEVEL_ERROR, __VA_ARGS__)
#define UFDMA_IG(_grp, ...) UFDMA_T(VTSS_UFDMA_TRACE_GROUP_##_grp, VTSS_UFDMA_TRACE_LEVEL_INFO,  __VA_ARGS__)
#define UFDMA_DG(_grp, ...) UFDMA_T(VTSS_UFDMA_TRACE_GROUP_##_grp, VTSS_UFDMA_TRACE_LEVEL_DEBUG, __VA_ARGS__)

#define UFDMA_HEX(_grp, _lvl, _byte_p, _byte_cnt) {                                          \
    if (state->trace_conf[_grp].level[UFDMA_TRACE_LAYER] >= _lvl &&                          \
        state->callout.trace_hex_dump != NULL) {                                             \
        state->callout.trace_hex_dump(UFDMA_TRACE_LAYER, _grp, _lvl, __FILE__, __LINE__, __FUNCTION__, _byte_p, _byte_cnt); \
    }                                                                                        \
}

#define UFDMA_EG_HEX(_grp, _byte_p, _byte_cnt) UFDMA_HEX(VTSS_UFDMA_TRACE_GROUP_##_grp, VTSS_UFDMA_TRACE_LEVEL_ERROR, _byte_p, _byte_cnt)
#define UFDMA_IG_HEX(_grp, _byte_p, _byte_cnt) UFDMA_HEX(VTSS_UFDMA_TRACE_GROUP_##_grp, VTSS_UFDMA_TRACE_LEVEL_INFO,  _byte_p, _byte_cnt)
#define UFDMA_DG_HEX(_grp, _byte_p, _byte_cnt) UFDMA_HEX(VTSS_UFDMA_TRACE_GROUP_##_grp, VTSS_UFDMA_TRACE_LEVEL_DEBUG, _byte_p, _byte_cnt)

// Macros for traces into the default trace group
#define UFDMA_E(...) UFDMA_EG(DEFAULT, ##__VA_ARGS__)
#define UFDMA_I(...) UFDMA_IG(DEFAULT, ##__VA_ARGS__)
#define UFDMA_D(...) UFDMA_DG(DEFAULT, ##__VA_ARGS__)

#define UFDMA_E_HEX(_byte_p, _byte_cnt) UFDMA_EG_HEX(DEFAULT, _byte_p, _byte_cnt)
#define UFDMA_I_HEX(_byte_p, _byte_cnt) UFDMA_IG_HEX(DEFAULT, _byte_p, _byte_cnt)
#define UFDMA_D_HEX(_byte_p, _byte_cnt) UFDMA_DG_HEX(DEFAULT, _byte_p, _byte_cnt)

#define UFDMA_DCACHE_LINE_SIZE_BYTES 32 /* Data Cache Line size. Currently, it's the same on all supported platforms */

/****************************************************************************/
// UFDMA_ALIGNED_SIZE()
// Use this macro to align a block of memory to a given alignment.
// Only use powers of two for _align_.
/****************************************************************************/
#define UFDMA_ALIGNED_SIZE(_addr_, _align_) (((u32)(_addr_) + ((u32)(_align_) - 1)) & ~((u32)(_align_) - 1))

/****************************************************************************/
// UFDMA_CACHE_ALIGNED_SIZE()
// Get the size of a DCB, given it must be cache-aligned.
// The macro may also be used to cache-align a block of memory.
/****************************************************************************/
#define UFDMA_CACHE_ALIGNED_SIZE(_size_) UFDMA_ALIGNED_SIZE(_size_, UFDMA_DCACHE_LINE_SIZE_BYTES)

/**
 * If AIL function exists, result of this macro is the result
 * of invoking the AIL function, otherwise it is UFDMA_RC_NO_SUCH_AIL_FUNC.
 */
#define UFDMA_AIL_FUNC(state, ail_func, ...) ((state)->ail.ail_func == NULL ? UFDMA_RC_NO_SUCH_AIL_FUNC : state->ail.ail_func(state, ##__VA_ARGS__))

/**
 * If AIL function exists, returns if AIL function fails, otherwise continues.
 * If AIL function doesn't exist, UFDMA_RC_NO_SUCH_AIL_FUNC is returned.
 * '##' means: Delete preceding comma if no args.
 */
#define UFDMA_AIL_FUNC_RC(state, ail_func, ...) UFDMA_RC(UFDMA_AIL_FUNC(state, ail_func, ##__VA_ARGS__))

/******************************************************************************/
//
// Error codes
//
/******************************************************************************/
enum {
    UFDMA_RC_OK = 0,              // Everything is OK (not an error)
    UFDMA_RC_SELF,                // Invalid self pointer
    UFDMA_RC_STATE,               // Invalid self->state pointer
    UFDMA_RC_ARG,                 // Invalid argument passed to function (other than the above)
    UFDMA_RC_INIT_CONF,           // Invalid value passed in vtss_fdma_init_conf_t structure
    UFDMA_RC_NO_SUCH_CIL_FUNC,    // No such CIL function (internal error)
    UFDMA_RC_NO_SUCH_AIL_FUNC,    // No such AIL function (internal error)
    UFDMA_RC_ALREADY_INITIALIZED, // uFDMA is already initialized
    UFDMA_RC_NOT_INITIALIZED,     // uFDMA is not initialized.
    UFDMA_RC_BUF_SIZE,            // Invalid buffer size. Must accommodate a minimum Ethernet frame + IFH.
    UFDMA_RC_BUF_STATE,           // Invalid buf_dscr->buf_state
    UFDMA_RC_RX_BUF_ALIGNMENT,    // Rx buffer must be 32-bit aligned.
    UFDMA_RC_UNINIT,              // Buffers are being returned because the uFDMA has been uninitialized.
    UFDMA_RC_CIL,                 // Unspecified CIL error
}; // Leave anonymous

/******************************************************************************/
//
// DCB layout
//
/******************************************************************************/

// This is the H/W DCB layout for version 1 FDMAs, that is,
// Jaguar-1 and Luton26.
typedef struct {
    u32 sar;
    u32 dar;
    u32 llp;
    u32 ctl0;
    u32 ctl1;
    u32 stat;
} ufdma_hw_dcb_v1_t;

// This is the H/W DCB layout for version 2 FDMAs, that is,
// Serval and later.
typedef struct {
    u32 llp;
    u32 datap;
    u32 datal;
    u32 stat;
} ufdma_hw_dcb_v2_t;

// We cannot use #ifdefs in this driver, because we want to support all
// platforms with one single driver. This means that the size put aside
// for the H/W DCBs become the largest of the two supported H/W versions.
typedef union {
    ufdma_hw_dcb_v1_t v1;
    ufdma_hw_dcb_v2_t v2;
} ufdma_hw_dcb_t;

// The S/W DCB consists of the cache-aligned H/W DCB
// and the network driver's data plus a next field
// to chain the DCBs together.
typedef struct ufdma_dcb_s {
    // H/W DCB. Must come first and must be cache-aligned.
    ufdma_hw_dcb_t hw_dcb;

    // Make sure that we don't invalidate what comes after the H/W DCB
    // when invalidating the cache.
    u8 unused[UFDMA_DCACHE_LINE_SIZE_BYTES - sizeof(ufdma_hw_dcb_t)];

    // Copy of the user's buffer structure.
    vtss_ufdma_buf_dscr_t buf_dscr;

    // We chain them together.
    struct ufdma_dcb_s *next;
} ufdma_dcb_t;

/******************************************************************************/
//
// Internal state
//
/******************************************************************************/

/**
 * Statistics
 */
typedef struct {
    // Rx
    u32 rx_poll_calls;          /**< Number of calls to vtss_ufdma_platform_driver_t::rx_poll()                                  */
    u32 rx_buf_add_calls;       /**< Number of calls to vtss_ufdma_platform_driver_t::rx_buf_add() (tells how many buffers are added to the FDMA in total) */
    u32 rx_callback_calls;      /**< Number of received frames                                                                   */
    u64 rx_callback_bytes;      /**< Number of received bytes (incl. IFH and FCS)                                                */
    u32 rx_oversize_drops;      /**< Number of rx-dropped frames because they are larger than the buffer size.                   */
    u32 rx_abort_drops;         /**< Number of rx-dropped DCBs because the abort bit was set                                     */
    u32 rx_pruned_drops;        /**< Number of rx-dropped DCBs because the pruned bit was set                                    */
    u32 rx_suspended_drops;     /**< Number of rx-dropped frames because the Rx queue was suspended (tail of H/W being read out) */
    u32 rx_cil_drops;           /**< Number of rx-dropped frames because the CIL layer thinks so.                                */
    u32 rx_multi_dcb_drops;     /**< Number of rx-dropped frames because they span multiple Rx buffers                           */
    u32 rx_multi_dcb_frms;      /**< Number of multi-DCB-frames forwarded through rx_callback()                                  */
    u32 rx_frms[8];             /**< Per-Rx queue frame count                                                                    */
    u64 rx_bytes[8];            /**< Per-Rx queue byte count                                                                     */

    // Tx
    u32 tx_poll_calls;          /**< Number of calls to vtss_ufdma_platform_driver_t::tx_poll()                                  */
    u32 tx_calls;               /**< Number of calls to vtss_ufdma_platform_driver_t::tx()                                       */
    u32 tx_done_callback_calls; /**< Number of times the tx_callback() has been called                                           */
    u64 tx_bytes;               /**< Number of transmitted bytes                                                                 */

    // Common
    u32 poll_calls;             /**< Number of calls to vtss_ufdma_platform_driver_t::poll()                                     */

} ufdma_statistics_t;

/**
 * Throttle state
 */
typedef struct {
    /**
     * Everytime the uFDMA driver has received a frame from a given
     * Rx queue (successfully as well as unsuccessfully), it will
     * increment this variable by one.
     * The counter is cleared upon every call to
     * vtss_ufdma_platform_driver_t::throttle_tick().
     */
    u32 frm_cnt[8];

    /**
     * Everytime the uFDMA driver has received a frame from a given
     * Rx queue (successfully as well as unsuccessfully), it
     * will increment this variable by the number of bytes received.
     * The counter is cleared upon every call to
     * vtss_ufdma_platform_driver_t::throttle_tick().
     */
    u32 byte_cnt[8];

    /**
     * Counts the number of times vtss_ufdma_platform_driver_t::throttle_tick()
     * will have yet to be invoked in order to re-open that queue for reception.
     */
    u32 ticks_left[8];

    /**
     * Counts the number of times Rx queues have been turned off.
     */
    u32 suspend_cnt[8];

    /**
     * Holds the maximum number of frames seen in between two calls to
     * vtss_ufdma_platform_driver_t::throttle_tick().
     */
    u32 statistics_max_frames_per_tick[8];

    /**
     * Holds the maximum number of bytes seen in between two calls to
     * vtss_ufdma_platform_driver_t::throttle_tick().
     */
    u32 statistics_max_bytes_per_tick[8];

    /**
     * Number of times vtss_ufdma_platform_driver_t::throttle_tick() has been invoked.
     * The real thing won't start until it's greater than 1 (not just greater than 0).
     */
    u64 tick_cnt;
} ufdma_throttle_state_t;

/**
 * Structure containing H/W's status of a DCB
 */
typedef struct {
    /**
     * When TRUE, this DCB contains the start-of-frame.
     * Valid for both Rx and Tx DCBs.
     */
    BOOL sof;

    /**
     * When TRUE, this DCB contains the end-of-frame
     * Valid for both Rx and Tx DCBs.
     */
    BOOL eof;

    /**
     * This will contain the number of bytes that
     * the FDMA has stored in this DCB's data area.
     * If #sof == #eof == TRUE, it will contain the full
     * frame length including IFH and FCS.
     *
     * The DCB has not been used by H/W if this field is zero
     * upon decoding.
     * Valid for Rx DCBs.
     */
    u32 fragment_size_bytes;

    /**
     * TRUE when H/W has pruned the frame to a certain
     * number of bytes. FALSE otherwise.
     * Valid for Rx DCBs.
     */
    BOOL pruned;

    /**
     * TRUE if this frame was aborted for one or another reason.
     * Valid for Rx DCBs.
     */
    BOOL aborted;

    /**
     * TRUE when H/W has indeed injected this DCB.
     * Valid for Tx DCBs
     */
    BOOL tx_done;

} ufdma_hw_dcb_status_t;


/**
 * Chip architecture.
 * Used for chips that use the same code-base, but have different register offsets.
 */
typedef enum {
    CHIP_ARCH_JAGUAR_2AB,
    CHIP_ARCH_JAGUAR_2C,
    CHIP_ARCH_SERVAL_T,
    CHIP_ARCH_SERVAL,
    CHIP_ARCH_OCELOT
} ufdma_chip_arch_t;

/**
 * Forward declaration
 */
struct ufdma_state_s;

/**
 * Functions and variable-size entities defined in CIL and used by the AIL layer.
 */
typedef struct {
    // CIL function for initializing an Rx H/W DCB and link it to dcb->next H/W-wise
    int (*rx_dcb_init)(struct ufdma_state_s *state, ufdma_dcb_t *dcb, u32 buf_size_bytes_aligned);

    // CIL function for initializing a Tx H/W DCB and link #dcb_prev to it H/W-wise
    int (*tx_dcb_init)(struct ufdma_state_s *state, ufdma_dcb_t *dcb, ufdma_dcb_t *dcb_prev);

    // CIL function for (re-)starting the FDMA Rx channel
    int (*rx_start)(struct ufdma_state_s *state, ufdma_dcb_t *head, BOOL *restarted);

    // CIL function for starting the FDMA Tx channel
    int (*tx_start)(struct ufdma_state_s *state, ufdma_dcb_t *head);

    // CIL function for re-initializing the FDMA Rx channel gracefully
    int (*rx_reinit)(struct ufdma_state_s *state);

    // CIL function for re-initializing the FDMA Tx channel gracefully
    int (*tx_reinit)(struct ufdma_state_s *state);

    // CIL function for checking whether frames are received or transmitted
    int (*poll)(struct ufdma_state_s *state, BOOL rx, BOOL tx, unsigned int rx_cnt_max);

    // CIL function for decoding a H/W DCB's status
    int (*dcb_status_decode)(struct ufdma_state_s *state, ufdma_dcb_t *dcb, ufdma_hw_dcb_status_t *status, BOOL is_rx);

    // CIL function for obtaining the Rx queue mask for a given DCB.
    int (*rx_qu_mask_get)(struct ufdma_state_s *state, ufdma_dcb_t *dcb, u32 *rx_qu_mask);

    // CIL function for figuring out whether the CIL layer wants to drop a
    // particular frame.
    int (*rx_frm_drop)(struct ufdma_state_s *state, ufdma_dcb_t *dcb, BOOL *drop);

    // CIL function for gracefully stopping and disabling the FDMA H/W
    int (*uninit)(struct ufdma_state_s *state);

    // CIL function for printing state
    int (*debug_print)(struct ufdma_state_s *state, vtss_ufdma_debug_info_t *info, int (*pr)(void *ref, const char *fmt, ...) __attribute__ ((format (printf, 2, 3))));

    // CIL function for getting a DCB's H/W LLP pointer (for debugging purposes)
    void *(*hw_dcb_next)(struct ufdma_state_s *state, ufdma_dcb_t *dcb);

    // CIL function for suspending or resuming a particular Rx queue.
    int (*rx_qu_suspend_set)(struct ufdma_state_s *state, u32 rx_qu, BOOL suspend);

    // Minimum and maximum buffer sizes (may differ between Rx and Tx)
    // tx_buf_size_bytes_min is not defined, because it only depends
    // on the Tx IFH size, since the uFDMA auto-adjusts the length to
    // a minimum Ethernet frame.
    u32 rx_buf_size_bytes_min;
    u32 rx_buf_size_bytes_max;
    u32 tx_buf_size_bytes_max;

    // H/W's burst size (needed to calculate size of Rx buffer allocation)
    u32 rx_burst_size_bytes;

    // Jaguar2AB, Jaguar2C, and ServalT use exactly the same codebase.
    // The only thing that differs is register offsets, which is why
    // the platform driver must be instantiated differently for the three.
    ufdma_chip_arch_t chip_arch;
} ufdma_cil_t;

/**
 * Functions defined in AIL and used by the CIL layer.
 */
typedef struct {
    /**
     * AIL function invoked upon Rx of frame.
     */
    int (*rx_frm)(struct ufdma_state_s *state, u32 chip_no, unsigned int rx_cnt_max);

    /**
     * AIL function invoked by the CIL layer upon
     * Rx error. Must move all H/W DCBs back to S/W while
     * re-initializing them. Finally, it must restart the
     * Rx channel.
     */
    int (*rx_buf_recycle_all)(struct ufdma_state_s *state);

    /**
     * AIL function invoked upon Tx done of frame.
     */
    int (*tx_done)(struct ufdma_state_s *state);

    /**
     * AIL function for converting from the CPU's
     * endianness to the bus's endianness. The
     * first may change, while the latter is fixed.
     */
    int (*cpu_to_bus)(struct ufdma_state_s *state, u32 cpu, u32 *bus);

    /**
     * AIL function for converting from the bus's
     * endianness to the CPU's endianness. The
     * first is fixed, while the latter may change.
     */
    int (*bus_to_cpu)(struct ufdma_state_s *state, u32 bus, u32 *cpu);

    /**
     * Function needed by the CIL to prevent re-ordering
     * of statements. We use a function pointer in order
     * to force the compiler not to optimize the statement
     * away. The function is implemented by AIL.
     */
    int (*reorder_barrier)(struct ufdma_state_s *state);

    /**
     * AIL function for reading a particular register.
     */
    u32 (*reg_rd)(struct ufdma_state_s *state, u32 addr);

    /**
     * AIL function for writing a value to a particular register.
     */
    void (*reg_wr)(struct ufdma_state_s *state, u32 addr, u32 val);

} ufdma_ail_t;

/**
 * uFDMA's internal state.
 */
typedef struct ufdma_state_s {
    /**
     * Reference to the platform driver that we are part of.
     */
    vtss_ufdma_platform_driver_t *self;

    /**
     * Trace configuration.
     */
    ufdma_trace_conf_t trace_conf[VTSS_UFDMA_TRACE_GROUP_ALL];

    /**
     * Various functions needed by the uFDMA and implemented
     * by the instantiator of the uFDMA.
     */
    vtss_ufdma_init_conf_t callout;

    /**
     * TRUE if initialized, FALSE if not.
     */
    BOOL initialized;

    /**
     * List of Rx fragments of a multi-DCB frame. First in list points to start
     * of frame. If non-NULL, end-of-frame not yet reached.
     * Only used if rx_multi_dcb_support is TRUE.
     */
    ufdma_dcb_t *rx_head_sw_pending;

    /**
     * List of Rx buffers not added to H/W.
     * NULL if none.
     */
    ufdma_dcb_t *rx_head_sw;

    /**
     * List of Rx buffers currently handed over to H/W.
     */
    ufdma_dcb_t *rx_head_hw;

    /**
     * List of Tx buffers to be handed over to H/W.
     */
    ufdma_dcb_t *tx_head_sw;

    /**
     * Tail of Tx buffers to be handed over to H/W.
     * Used to be able to quickly add new frames
     * at the end of the list.
     */
    ufdma_dcb_t *tx_tail_sw;

    /**
     * List of Tx buffers currently handed over to H/W.
     */
    ufdma_dcb_t *tx_head_hw;

    /**
     * Platform-specific functions used by AIL.
     */
    ufdma_cil_t cil;

    /**
     * Platform-independent functions used by CIL
     */
    ufdma_ail_t ail;

    /**
     * Statistics
     */
    ufdma_statistics_t stati;

    /**
     * Current throttling configuration
     */
    vtss_ufdma_throttle_conf_t throttle_conf;

    /**
     * Current throttling state
     */
    ufdma_throttle_state_t throttle_state;

} ufdma_state_t;

/******************************************************************************/
//
// The only AIL function needed to be public
//
/******************************************************************************/

/**
 * vtss_ufdma_init()
 * Called by CIL layer during call of CIL layer's init-function.
 */
int vtss_ufdma_init(vtss_ufdma_platform_driver_t *self, vtss_ufdma_init_conf_t *init_conf, BOOL dual_chip, ufdma_state_t **pass_out_state);

#endif /* _VTSS_UFDMA_H_ */
