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
 * \brief Micro Frame Direct Memory Access API
 */

#ifndef _VTSS_UFDMA_API_H_
#define _VTSS_UFDMA_API_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Structure containing platform-specific uFDMA properties.
 */
typedef struct {
    /**
     * Number of bytes in IFH (Internal Frame Header) of received frames.
     */
    unsigned int rx_ifh_size_bytes;

    /**
     * Number of bytes in IFH of transmitted frames.
     */
    unsigned int tx_ifh_size_bytes;

    /**
     * Number of bytes needed by the uFDMA internally to keep state
     * and control of H/W for Rx or Tx of one frame.
     * This number of bytes must be allocated for every frame
     * to be received or transmitted.
     */
    unsigned int buf_state_size_bytes;

    /**
     * Number of bytes needed by the uFDMA driver to keep internal
     * state. A pointer to a buffer of this size must be allocated
     * once and passed to the uFDMA in all calls.
     */
    unsigned int ufdma_state_size_bytes;
} vtss_ufdma_platform_props_t;

/**
 * Structure describing one frame buffer.
 */
typedef struct vtss_ufdma_buf_dscr_s {
    /**
     * [IN]
     * Frame buffer. Must be 32-bit aligned for Rx-buffers and any-aligned for Tx-buffers.
     * First byte points to the location of the first byte of the IFH.
     * The actual frame is located vtss_ufdma_platform_props_t::rx_ifh_size_bytes
     * after the IFH if this is a receive buffer and
     * vtss_ufdma_platform_props_t::tx_ifh_size_bytes after the IFH
     * if this buffer is a transmit buffer.
     *
     * The buffer must be allocated as a contiguous chunk of DMAable memory.
     *
     * For Rx buffers, the size of the allocated area may have to be larger
     * than the number of bytes that the frame at most must contain.
     * Use vtss_ufdma_platform_driver_t::rx_buf_alloc_size_get() to get the
     * size that you must allocate given a wanted MTU.
     */
    unsigned char *buf;

    /**
     * [IN]
     * Buffer length.
     *
     * For Rx buffers, this must be less than or equal to the size actually allocated.
     * See #buf and vtss_ufdma_platform_driver_t::rx_buf_alloc_size_get() for details.
     * The size must be the wanted CPU MTU + size-of-Rx-IFH.
     *
     * For Tx buffers, the size must be the size of the frame (including FCS) + size-of-Tx-IFH.
     * The upper limit on the frame length is defined by the chip configuration.
     * The uFDMA driver adjusts the size to be at least a minimum Ethernet frame.
     * Notice that if auto-adjusting occurs, the bytes at the end of the buffer will be
     * sent as is. To avoid compromising security, it is recommended that the network driver
     * therefore always allocates a minimum-sized Ethernet frame and sets unused bytes of
     * the frame to all-zeros.
     */
    unsigned int buf_size_bytes;

    /**
     * [IN]
     * A user-controlled parameter that will follow this buffer
     * and get passed back to the Rx-callback or Tx-done-callback function.
     * It is not used by the uFDMA.
     */
    void *context;

    /**
     * [IN]
     * Internal state required by the uFDMA to control
     * this frame. The size of this chunk of memory can be
     * found in vtss_ufdma_platform_props_t::buf_state_size_bytes.
     * Also this piece of memory must be allocated as a
     * contiguous chunk of DMAable memory.
     * The buffer can be any-aligned (uFDMA driver auto-adjusts) to
     * fit its internal structure, and buf_state_size_bytes is long
     * enough to accommodate for this).
     *
     * Some network drivers may want to allocate this along with the
     * frame buffer itself (#buf). In this case, it is recommended
     * that #buf comes first and #buf_state comes after (#buf must
     * be 32-bit aligned, whereas #buf_state can be any-aligned).
     */
    void *buf_state;

    /**
     * [OUT]
     * Only used in Rx buffers.
     *
     * If rx_multi_dcb_support is FALSE:
     *   On rx_callback(), it will contain the actual length of the frame
     *   including Rx-IFH and FCS. It will always be <= #buf_size_bytes.
     *
     * If rx_multi_dcb_support is TRUE:
     *   On rx_callback(), the first Rx buffer will contain the total length
     *   of the frame including Rx-IFH and FCS.
     *   It is undefined for subsequent Rx buffers belonging to the same frame.
     *   See also #fragment_size_bytes
     */
    unsigned int frm_length_bytes;

    /**
     * [OUT]
     * Only used in Rx buffers.
     *
     * If rx_multi_dcb_support is FALSE, this will always equal
     * #frm_length_bytes.
     * If rx_multi_dcb_support is TRUE, this will equal #frm_length_bytes if
     * there's only one fragment. Otherwise it will contain the number of bytes
     * valid in this Rx buffer.
     */
    unsigned int fragment_size_bytes;

    /**
     * [OUT]
     * Only used in Rx buffers.
     * On rx_callback(), it will contain the Rx queue that
     * this frame was received on.
     */
    unsigned int rx_qu;

    /**
     * [OUT]
     * Only used in Rx buffers.
     * On rx_callback(), it will contain the chip number that
     * this frame was received on. Only non-zero on multi-
     * chip platforms.
     */
    unsigned int chip_no;

    /**
     * [OUT]
     * Result of frame transfer operation.
     * During normal, successful operation, it will contain
     * 0, but if e.g. the FDMA is being uninitialized
     * (through a call to vtss_ufdma_platform_driver_t::uninit(),
     * vtss_ufdma_platform_driver_t::rx_free(), or
     * vtss_ufdma_platform_driver_t::tx_free()), it will contain
     * a non-zero number, indicating that the buffer was not transmitted/received.
     */
    int result;

    /**
     * [OUT]
     * Rx or Tx done timestamp.
     *
     * If the network driver provided a timestamp function in the
     * vtss_ufdma_init_conf_t::timestamp() pointer, this field will hold
     * the 64-bit value returned by that function call. Otherwise, it
     * will be 0.
     */
    unsigned long long timestamp;

    /**
     * [OUT]
     * Only used in Rx buffers.
     * If a frame cannot be contained in one single buffer, this one indicates
     * that there are more fragments to come. It's NULL when the end of frame is
     * reached.
     *
     * It can only be non-NULL if vtss_ufdma_init_conf_t::rx_multi_dcb_support is
     * TRUE.
     */
    struct vtss_ufdma_buf_dscr_s *next;

} vtss_ufdma_buf_dscr_t;

/**
 * uFDMA trace layers
 */
typedef enum {
    VTSS_UFDMA_TRACE_LAYER_AIL, /**< Application Interface Layer */
    VTSS_UFDMA_TRACE_LAYER_CIL, /**< Chip Interface Layer        */
    VTSS_UFDMA_TRACE_LAYER_ALL  /**< Must come last              */
} vtss_ufdma_trace_layer_t;

/**
 * uFDMA trace groups
 */
typedef enum {
    VTSS_UFDMA_TRACE_GROUP_DEFAULT, /**< Default trace group */
    VTSS_UFDMA_TRACE_GROUP_RX,      /**< Rx-specific trace   */
    VTSS_UFDMA_TRACE_GROUP_TX,      /**< Tx-specific trace   */
    VTSS_UFDMA_TRACE_GROUP_ALL      /**< Must come last      */
} vtss_ufdma_trace_group_t;

/**
 * uFDMA trace levels
 */
typedef enum {
    VTSS_UFDMA_TRACE_LEVEL_NONE,  /**< Don't print anything          */
    VTSS_UFDMA_TRACE_LEVEL_ERROR, /**< Print errors only (default)   */
    VTSS_UFDMA_TRACE_LEVEL_INFO,  /**< Print errors and info         */
    VTSS_UFDMA_TRACE_LEVEL_DEBUG, /**< Print errors, info, and debug */
    VTSS_UFDMA_TRACE_LEVEL_LAST   /**< Must come last. Do not use.   */
} vtss_ufdma_trace_level_t;

/**
 * Forward declaration of platform driver structure.
 */
typedef struct vtss_ufdma_platform_driver_s vtss_ufdma_platform_driver_t;

/**
 * Structure describing the external functions needed to operate the uFDMA.
 * It is used to set up call-out functions needed by the uFDMA.
 */
typedef struct {
    /**
     * Callback function invoked by the uFDMA whenever a frame is received.
     * The function will be called in the same context as
     * the vtss_ufdma_platform_driver_t::rx_poll() function is called.
     *
     * Whenever it gets invoked, the \p rx_buf_dscr is a structure
     * built up on the stack and must not be utilized by the called
     * back function once it returns.
     * The pointers referenced within it are not used by the uFDMA
     * after the call returns.
     */
    void (*rx_callback)(vtss_ufdma_platform_driver_t *self, vtss_ufdma_buf_dscr_t *rx_buf_dscr);

    /**
     * Callback function invoked by the uFDMA whenever a frame
     * has been transmitted (called from vtss_ufdma_platform_driver_t::tx_poll()).
     */
    void (*tx_callback)(vtss_ufdma_platform_driver_t *self, vtss_ufdma_buf_dscr_t *tx_buf_dscr);

    /**
     * Function invoked by the uFDMA to flush dcache lines to RAM.
     * It expects \p bytes bytes to be flushed starting at virtual
     * address \p virt_addr.
     */
    void (*cache_flush)(void *virt_addr, unsigned int bytes);

    /**
     * Function invoked by the uFDMA to invalidate dcache lines.
     * It expects \p bytes bytes to be invalidated starting at virtual
     * address \p virt_addr.
     */
    void (*cache_invalidate)(void *virt_addr, unsigned int bytes);

    /**
     * Function invoked by the uFDMA to convert a virtual address to
     * a physical address.
     */
    void *(*virt_to_phys)(void *virt_addr);

    /**
     * Function invoked by the uFDMA to print trace info.
     * The uFDMA may invoke the function from all contexts that the
     * uFDMA's functions are invoked by.
     * If NULL, no trace will be generated.
     */
    void (*trace_printf)(vtss_ufdma_trace_layer_t layer, vtss_ufdma_trace_group_t group, vtss_ufdma_trace_level_t level, const char *file, const int line, const char *function, const char *fmt, ...) __attribute__ ((format (printf, 7, 8)));

    /**
     * Function invoked by the uFDMA to print a hex dump trace.
     * The uFDMA may invoke the function from all contexts that the
     * uFDMA's functions are invoked by.
     * If NULL, no trace will be generated.
     */
    void (*trace_hex_dump)(vtss_ufdma_trace_layer_t layer, vtss_ufdma_trace_group_t group, vtss_ufdma_trace_level_t level, const char *file, const int line, const char *function, const unsigned char *byte_p, int byte_cnt);

    /**
     * If Rx of frames must be time-stamped as close to
     * reception of the frame as possible, set this
     * function pointer to a non-NULL value, and implement
     * a function that returns a 64-bit value representing
     * the current time. This value will be stored in the
     * timestamp field of the buffer descriptor returned
     * in the rx_callback().
     */
    unsigned long long (*timestamp)(void);

    /**
     * Register read function.
     * The uFDMA driver uses this function to read physical
     * chip registers.
     * The UFDMA driver invokes the function with a 32-bit address
     * and expects the function to return the value of that
     * address. It's impossible to pass error codes from the
     * network driver to the uFDMA.
     * For future use, it also invokes the function with a chip
     * number. On single-chip solutions, the \p chip_no will always
     * be 0.
     */
    unsigned int (*reg_read)(unsigned int chip_no, unsigned int addr);

    /**
     * Register write function.
     * The uFDMA driver uses this function to write physical
     * chip registers.
     * The UFDMA driver invokes the function with a 32-bit address
     * and expects the function to write \p value to that address.
     * It's impossible to pass error codes from the network driver
     * to the uFDMA.
     * For future use, it also invokes the function with a chip
     * number. On single-chip solutions, the \p chip_no will always
     * be 0.
     */
    void (*reg_write)(unsigned int chip_no, unsigned int addr, unsigned int value);

    /**
     * Endianness.
     * When storing memory pointers in the FDMA H/W, knowledge
     * about the CPU's endianness is required.
     * Set this variable to 1 to enable the code for big-endian.
     */
    unsigned char big_endian;

    /**
     * Multi-DCB-Rx-support.
     * If the user of this driver supports receiving multiple Rx buffers forming
     * a single frame, then it may want to set this to TRUE. This allows for
     * receiving frames larger than MTU.
     */
    unsigned char rx_multi_dcb_support;

    /**
     * User-defined property. The uFDMA will not use this one.
     * It can be retrieved by the use of the context_get() function.
     */
    void *context;

} vtss_ufdma_init_conf_t;

/**
 * uFDMA debug layers
 */
typedef enum {
    VTSS_UFDMA_DEBUG_LAYER_ALL, /**< Both layers                  */
    VTSS_UFDMA_DEBUG_LAYER_AIL, /**< Application Interface Layer  */
    VTSS_UFDMA_DEBUG_LAYER_CIL, /**< Chip Interface Layer         */
    VTSS_UFDMA_DEBUG_LAYER_LAST /**< Must come last. Do not use.  */
} vtss_ufdma_debug_layer_t;

/**
 * uFDMA debug groups
 */
typedef enum {
    VTSS_UFDMA_DEBUG_GROUP_ALL,  /**< All groups                  */
    VTSS_UFDMA_DEBUG_GROUP_RX,   /**< Frame Rx group              */
    VTSS_UFDMA_DEBUG_GROUP_TX,   /**< Frame Tx group              */
    VTSS_UFDMA_DEBUG_GROUP_LAST  /*/< Must come last. Do not use. */
} vtss_ufdma_debug_group_t;

/**
 * uFDMA debug info request
 */
typedef struct {
    vtss_ufdma_debug_layer_t layer; /**< Layer                    */
    vtss_ufdma_debug_group_t group; /**< Group                    */
    unsigned int             full;  /**< 0 to limit information   */
    void                     *ref;  /**< Print function reference */
} vtss_ufdma_debug_info_t;

/**
 * In order to survive e.g. broadcast storms, the uFDMA driver incorporates
 * a poor man's policing/throttling scheme.
 *
 * The idea is to check upon every frame reception whether the CPU Rx queue
 * on which the frame was received has exceeded its limit, and if so,
 * suspend reception from the queue for a period of time.
 *
 * The uFDMA driver has no notion of time, so this requires a little help from
 * the network driver.
 * To take advantage of the feature, the application must first call
 * vtss_ufdma_platform_driver_t::throttle_conf_set() with an appropriate configuration
 * and then call vtss_ufdma_platform_driver_t::throttle_tick() on a regular,
 * network-driver-defined basis, e.g. 10 times per second.
 *
 * The throttle tick takes care of re-opening the queue after the suspension
 * period elapses.
 *
 * This feature is controlled per Rx queue, and if
 * vtss_ufdma_throttle_conf_t::frm_limit_per_tick[rx_qu] is 0 and
 * vtss_ufdma_throttle_conf_t::byte_limit_per_tick[rx_qu] is 0 this
 * feature is disabled for that queue.
 *
 * If either vtss_ufdma_throttle_conf_t::frm_limit_per_tick[rx_qu] or
 * vtss_ufdma_throttle_conf_t::byte_limit_per_tick[rx_qu] is non-zero,
 * the feature is enabled, and in case a queue gets suspended, it will be
 * suspended for the remainder of the tick period PLUS whatever is
 * specified with vtss_ufdma_throttle_conf_t::suspend_tick_cnt.
 *
 * Notice that once an Rx queue gets disabled, that Rx queue will no longer
 * be a source of interrupts. The feature will only affect rx queues for which
 * it is enabled.
 *
 * Once disabling an Rx queue, the remaining frames in that queue will be
 * read out, after which the queue will be silent. When re-enabling, it
 * will be fresh frames that come in.
 *
 * On some platforms, the trick to disable a queue involves directing the
 * queue to the second CPU port. If your application uses two CPU ports,
 * then throttling will have unexpected side-effects.
 *
 * There is no need to call vtss_ufdma_platform_driver_t::throttle_tick()
 * unless throttling is enabled for at least one Rx queue.
 *
 * If throttling is enabled for at least one queue, but you fail to call
 * vtss_ufdma_platform_driver_t::throttle_tick(), you risk that an Rx
 * queue will get disabled and never re-enabled again.
 */
typedef struct {
    /**
     * Controls - per Rx queue - the maximum number of frames
     * received between two calls to vtss_ufdma_platform_driver_t::throttle_tick()
     * without suspending reception from that queue.
     *
     * If 0, frame count throttling is disabled for that Rx queue.
     */
    unsigned int frm_limit_per_tick[8];

    /**
     * Controls - per Rx queue - the maximum number of bytes
     * received between two calls to vtss_ufdma_platform_driver_t::throttle_tick()
     * without suspending reception from that queue.
     * The number of bytes includes the size of the Rx IFH, the frame itself and
     * FCS.
     *
     * If 0, byte count throttling is disabled for that Rx queue.
     */
    unsigned int byte_limit_per_tick[8];

    /**
     * Controls - per Rx queue - the number of invocations of
     * vtss_ufdma_platform_driver_t::throttle_tick() that must happen before
     * an Rx queue that has been disabled, gets re-enabled.
     *
     * For instance,
     *   a value of 0 means: re-enable the Rx queue on the next tick.
     *   a value of 1 means: re-enable the Rx queue two ticks from when it was suspended.
     */
    unsigned int suspend_tick_cnt[8];
} vtss_ufdma_throttle_conf_t;

/**
 * All FDMA operations are made through the functions defined in this structure.
 *
 * The network driver must ensure that no two functions can be called
 * simultaneously.
 */
typedef struct vtss_ufdma_platform_driver_s {
    /**
     * Platform properties.
     *
     * These are statically assigned at compile time and
     * available at all times to the upper layer.
     *
     * The purpose of these properties is for the upper layer
     * to be able to allocate state and frame buffers of the
     * correct sizes.
     */
    const vtss_ufdma_platform_props_t props;

    /**
     * Internal state of the uFDMA.
     *
     * This area must be dynamically allocated and set by
     * the upper layer prior to calling any of the functions
     * within this structure. The required size can be found
     * in \p props.ufdma_state_size_bytes.
     * The allocated area does not need to be DMAable and can
     * have any alignment.
     *
     * \p state must be reset to all-zeros prior to the
     * call into \p init().
     */
    void *state;

    /**
     * Initialize uFDMA H/W and its state.
     *
     * \param self      [IN] Reference to this structure (kind of 'this' pointer)
     * \param init_conf [IN] Operational parameters.
     *
     * \return 0 on success, anything else on error. Unlike other functions, you
     * cannot use vtss_ufdma_platform_driver_t::error_txt() to obtain a textual
     * representation of the error code if this function fails.
     */
    int (*init)(struct vtss_ufdma_platform_driver_s *self, vtss_ufdma_init_conf_t *init_conf);

    /**
     * Get number of bytes to allocate when allocating Rx buffers.
     *
     * Rx buffers are a bit special in that they may require to be somewhat
     * larger than the CPU-MTU. The reason for this is two-fold:
     *   1) The end of the buffer must not share cache line with other data
     *      because the uFDMA driver may invalidate or flush the cache line
     *      which may cause the other data to become corrupt.
     *   2) The H/W may put constraints on the buffer size due to its
     *      ability to burst, so that the size of the buffer must be a multiple
     *      of the platform's burst size.
     *
     * These facts mean that when you allocate an Rx buffer, you must first
     * call this function to figure out how much to actually allocate given a
     * wanted max-frame-size including Rx IFH. Then allocate that amount of memory
     * but write the max-frame-size including Rx IFH into vtss_ufdma_buf_dscr_t::buf_size_bytes.
     *
     * Example:
     *   You want the CPU-MTU to be 1518 + 2 * 4 bytes (max Ethernet frame plus 2 VLAN tags).
     *     wanted_size = 1518 + 2 * 4 + self.props.rx_ifh_size_bytes;
     *     self.rx_buf_alloc_size_get(self, wanted_size, &alloc_size);
     *     rx_buf_dscr.buf = Allocate(alloc_size); // This must be 32-bit aligned
     *     rx_buf_dscr.buf_size_bytes = wanted_size;
     *
     * \param self              [IN]  Reference to this structure (kind of 'this' pointer)
     * \param wanted_size_bytes [IN]  Max. size of frame incl. Rx IFH
     * \param needed_size_bytes [OUT] Number of bytes you must allocate for this buffer.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*rx_buf_alloc_size_get)(struct vtss_ufdma_platform_driver_s *self, unsigned int wanted_size_bytes, unsigned int *needed_size_bytes);

    /**
     * Add one Rx buffer to the uFDMA.
     *
     * Initially, call this function as many times as you need Rx buffers.
     * Subsequently, it is recommended to add one (new) Rx buffer for every
     * frame received.
     *
     * The location, length, and state of the Rx buffer is described in the
     * \p rx_buf_dscr structure passed to this function. The structure itself
     * may be allocated on the stack, but the pointers within it should be
     * allocated according to the description of vtss_ufdma_buf_dscr_t.
     *
     * Once the uFDMA has received a frame into the buffer, it will invoke
     * the function specified with vtss_ufdma_init_conf_t::rx_callback.
     *
     * \param self        [IN] Reference to this structure (kind of 'this' pointer)
     * \param rx_buf_dscr [IN] Description of the Rx buffer.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*rx_buf_add)(struct vtss_ufdma_platform_driver_s *self, vtss_ufdma_buf_dscr_t *rx_buf_dscr);

    /**
     * Transmit a frame using the uFDMA.
     *
     * This call is an asynchronous call in the sense that the frame is not
     * necessarily transmitted when this function returns.
     * The uFDMA will invoke vtss_ufdma_init_conf_t::tx_callback once
     * the frame is really transmitted.
     *
     * The location, length, and state of the Tx buffer is described in the
     * \p tx_buf_dscr structure passed to this function. The structure itself
     * may be allocated on the stack, but the pointers within it should be
     * allocated according to the description of vtss_ufdma_buf_dscr_t.
     *
     * \param self [IN] Reference to this structure (kind of 'this' pointer)
     * \param tx_buf_dscr [IN] Description of the Tx buffer.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*tx)(struct vtss_ufdma_platform_driver_s *self, vtss_ufdma_buf_dscr_t *tx_buf_dscr);

    /**
     * Invoke to see if any frame transfer (Rx/Tx) is done.
     *
     * The result is zero or more calls to vtss_ufdma_init_conf_t::rx_callback()
     * and vtss_ufdma_init_conf_t::tx_callback().
     *
     * The maximum number of calls to vtss_ufdma_init_conf_t::rx_callback() can
     * be controlled with the \p rx_cnt_max parameter. Set this to 0 to call
     * as many times as there are frames ready.
     *
     * If running interrupt-driven, it is recommended to use this function
     * rather than its cousins, #rx_poll() and #tx_poll(), because it is
     * a tiny bit faster. It's impossible to see from outside whether the
     * FDMA interrupt occurred due to a frame reception or a frame transmission
     * done event.
     *
     * If running polled (for instance during high-load periods), the network
     * driver may - at its own pace - call either #poll(), #rx_poll(), or #tx_poll()
     * depending on its needs.
     *
     * \param self       [IN] Reference to this structure (kind of 'this' pointer)
     * \param rx_cnt_max [IN] Maximum number of times to call rx_callback(). If set to 0, all available frames will be returned.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*poll)(struct vtss_ufdma_platform_driver_s *self, unsigned int rx_cnt_max);

    /**
     * Invoke to see if any frame is received and ready to be handed to the network driver.
     *
     * Normally, the network driver should only use this function when running
     * in polled mode. Use #poll() when running interrupt-driven.
     *
     * The result of the invokation is zero or more calls of the rx_callback() function,
     * but at most \p rx_cnt_max, where 0 means until no more frames ready.
     *
     * \param self       [IN] Reference to this structure (kind of 'this' pointer)
     * \param rx_cnt_max [IN] Maximum number of times to call rx_callback(). If set to 0, all available frames will be returned.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*rx_poll)(struct vtss_ufdma_platform_driver_s *self, unsigned int rx_cnt_max);

    /**
     * Invoke to see if any frame transmission is complete.
     *
     * Normally, the network driver should only use this function when running
     * in polled mode. Use #poll() when running interrupt-driven.
     *
     * The result of the invokation is zero or more calls of the tx_callback() function.
     *
     * \param self [IN] Reference to this structure (kind of 'this' pointer)
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*tx_poll)(struct vtss_ufdma_platform_driver_s *self);

    /**
     * Uninitialize uFDMA H/W, cancel all Tx bufs currently owned by the uFDMA, and
     * release all receive buffers to the caller.
     *
     * The Tx and Rx bufs are returned by uFDMA calls to the rx_callback() and
     * tx_callback() functions.
     *
     * See also rx_free() and tx_free().
     *
     * \param self [IN] Reference to this structure (kind of 'this' pointer)
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*uninit)(struct vtss_ufdma_platform_driver_s *self);

    /**
     * Free all Rx bufs currently owned by the uFDMA.
     *
     * The Rx bufs are returned by uFDMA calls to the rx_callback() function.
     *
     * After the call, the H/W will still be active, but you need to add new
     * buffers to re-activate frame reception.
     *
     * See also uninit() and tx_free().
     *
     * \param self [IN] Reference to this structure (kind of 'this' pointer)
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*rx_free)(struct vtss_ufdma_platform_driver_s *self);

    /**
     * Free all Tx bufs currently owned by the uFDMA.
     *
     * The Tx bufs are returned by uFDMA calls to the tx_callback() function.
     *
     * After the call, the H/W will still be able to receive frames for transmission.
     * This function's only purpose is to let go of all buffers currently in possesion
     * of the uFDMA.
     *
     * See also uninit() and rx_free().
     *
     * \param self [IN] Reference to this structure (kind of 'this' pointer)
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*tx_free)(struct vtss_ufdma_platform_driver_s *self);

    /**
     * Retrieve the context that was provided in the structure passed to the call to init().
     *
     * This function returns the value passed to init() in vtss:ufdma_init_conf_t::context.
     *
     * This value is completely user-defined, so the uFDMA will not use it for anything.
     *
     * \param self    [IN]  Reference to this structure (kind of 'this' pointer)
     * \param context [OUT] Pointer to a location that will receive the value of context passed to init().
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*context_get)(struct vtss_ufdma_platform_driver_s *self, void **context);

    /**
     * Print debug info
     *
     * Print Chip Interface Layer's (CIL), Application Interface Layer's (AIL)
     * or both's debug info using the \p pr function.
     *
     * The statistics shown may be cleared with a call to stati_clr().
     *
     * \param self [IN] Reference to this structure (kind of 'this' pointer)
     * \param info [IN] Controls which layer and group to print.
     * \param pr   [IN] printf()-like function used to print the debug info. 'ref' is implementation-defined.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*debug_print)(struct vtss_ufdma_platform_driver_s *self, vtss_ufdma_debug_info_t *info, int (*pr)(void *ref, const char *fmt, ...) __attribute__ ((format (printf, 2, 3))));

    /**
     * Clear internal statistics
     *
     * Clear the statistics kept internally and showable with debug_print().
     *
     * \param self [IN] Reference to this structure (kind of 'this' pointer)
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*stati_clr)(struct vtss_ufdma_platform_driver_s *self);

    /**
     * Set trace level
     *
     * The trace level can be controlled by layer (CIL, AIL) and
     * group.
     * To set for all layers, use VTSS_UFDMA_TRACE_LAYER_ALL.
     * To set for all groups, use VTSS_UFDMA_TRACE_GROUP_ALL.
     *
     * \param self  [IN] Reference to this structure (kind of 'this' pointer)
     * \param layer [IN] Trace layer
     * \param group [IN] Trace group
     * \param level [IN] New trace level
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*trace_level_set)(struct vtss_ufdma_platform_driver_s *self, vtss_ufdma_trace_layer_t layer, vtss_ufdma_trace_group_t group, vtss_ufdma_trace_level_t level);

    /**
     * Function to convert error code to a textual representation.
     *
     * \param self       [IN] Reference to this structure (kind of 'this' pointer)
     * \param error_code [IN] Error code to convert to a string.
     *
     * \return A const string representing the error code.
     */
    const char *(*error_txt)(struct vtss_ufdma_platform_driver_s *self, int error_code);

    /**
     * Get current throttle configuration.
     *
     * Returns the current throttling configuration.
     * The current throttling status can be seen with a call to debug_print().
     *
     * \param self          [IN]  Reference to this structure (kind of 'this' pointer)
     * \param throttle_conf [OUT] Pointer to structure that receives the current throttling configuration.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*throttle_conf_get)(struct vtss_ufdma_platform_driver_s *self, vtss_ufdma_throttle_conf_t *throttle_conf);

    /**
     * Configure throttling.
     *
     * See vtss_ufdma_throttle_conf_t for a description of the throttle feature
     * and the use of this function.
     *
     * \param self          [IN]: Reference to this structure (kind of 'this' pointer)
     * \param throttle_conf [IN]: New throttling configuration.
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*throttle_conf_set)(struct vtss_ufdma_platform_driver_s *self, vtss_ufdma_throttle_conf_t *throttle_conf);

    /**
     * Provide throttle tick.
     *
     * See vtss_ufdma_throttle_conf_t for a description of the throttle feature
     * and the use of this function.
     *
     * \param self [IN]: Reference to this structure (kind of 'this' pointer)
     *
     * \return 0 on success, anything else on error. Use vtss_ufdma_platform_driver_t::error_txt()
     * to convert to a textual representation.
     */
    int (*throttle_tick)(struct vtss_ufdma_platform_driver_s *self);

} vtss_ufdma_platform_driver_t;

// Select one of the following drivers corresponding to the platform on which you are running.

/**
 * Driver structure for Luton26-based platforms.
 */
extern vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_luton26;

/**
 * Driver structure for Serval-based platforms.
 */
extern vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_serval;

/**
 * Driver structure for Ocelot-based platforms.
 */
extern vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_ocelot;

/**
 * Driver structure for Jaguar2A- and Jaguar2B-based platforms.
 */
extern vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_jaguar2ab;

/**
 * Driver structure for Jaguar2C-based platforms.
 */
extern vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_jaguar2c;

/**
 * Driver structure for ServalT-based platforms.
 */
extern vtss_ufdma_platform_driver_t vtss_ufdma_platform_driver_servalt;

#ifdef __cplusplus
}
#endif
#endif /* _VTSS_UFDMA_API_H_ */
