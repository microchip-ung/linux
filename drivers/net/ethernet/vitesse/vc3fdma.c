/* Copyright (c) 2019 Microsemi Corporation

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

/*
 *  Driver for the Switch FDMA (and register IO)
 */

//#undef DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/seq_file.h>
#include <net/genetlink.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/reboot.h>
#include <linux/of.h>

#include <linux/debugfs.h>
#include <linux/proc_fs.h>

#include "vtss_ifh.h"
#include "ufdma/include/vtss_ufdma_api.h"

struct fdma_chip {
    vtss_ufdma_platform_driver_t *driver;
    u8 ifh_id;
};

/* fwd */
static struct genl_family packet_generic_netlink_family;

static u8 ifh_encap [] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x88, 0x80, 0x00, 0x00 /* IFH_ID */};

#define PROTO_B0               12
#define PROTO_B1               13
#define IFH_ID_OFF             (sizeof(ifh_encap) - 1)

#define DRV_NAME               "vc3fdma"
#define DRV_VERSION            "01.27"
#define DRV_RELDATE            "2023/01/25"

#define ETH_VLAN_TAGSZ         4    // Size of a 802.1Q VLAN tag
#define RX_MTU_DEFAULT         (ETH_FRAME_LEN + ETH_FCS_LEN + (2 * ETH_VLAN_TAGSZ))
#define RX_MTU_MIN                64
#define RX_MTU_MAX             16384
#define IF_BUFSIZE_JUMBO       10400
#define RX_BUF_CNT_DEFAULT     1024
#define NAPI_BUDGET            ((RX_BUF_CNT_DEFAULT / 2) > NAPI_POLL_WEIGHT ? (NAPI_POLL_WEIGHT / 2) : (RX_BUF_CNT_DEFAULT / 2))
#define DCACHE_LINE_SIZE_BYTES 32 /* Data Cache Line size. Currently, it's the same on all supported platforms */
#define ZC_CDEV_NAME           "vc3fdma_zc"
#define RX_MULTI_DCB_SUPPORT   1 /* 0 or 1 */
static int do_debug;

#define T_D(_fmt_, ...)        do { if(do_debug) printk(KERN_DEBUG "%s#%d. " _fmt_ "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); } while(0)
#define T_I(_fmt_, ...)        printk(KERN_INFO  "%s#%d. " _fmt_ "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define T_E(_fmt_, ...)        printk(KERN_ERR   "%s#%d. " _fmt_ "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

/****************************************************************************/
// VC3FDMA_ALIGNED_SIZE()
// Use this macro to align a block of memory to a given alignment.
// Only use powers of two for _align_.
/****************************************************************************/
#define VC3FDMA_ALIGNED_SIZE(_addr_, _align_) (((u32)(_addr_) + ((u32)(_align_) - 1)) & ~((u32)(_align_) - 1))

/****************************************************************************/
// VC3FDMA_CACHE_ALIGNED_SIZE()
// Cache-align a block of memory.
/****************************************************************************/
#define VC3FDMA_CACHE_ALIGNED_SIZE(_size_) VC3FDMA_ALIGNED_SIZE(_size_, DCACHE_LINE_SIZE_BYTES)

static struct vc3fdma_private *vc3fdma_inst;

/****************************************************************************/
// Structure common between kernel and user space.
/****************************************************************************/
struct zc_frm_dscr {
    u8 *frm_ptr;   // Pointer to actual frame data. NULL if end of list.
    u32 len;       // Frame length including IFH and FCS.
    u64 timestamp; // Timestamp (in usecs since boot).
    u8 *head_ptr;  // Pointer to head-of data (corresponding to skb->head). Not used by user-space application, and hence put a little aside.
    u64 dummy;
};

/****************************************************************************/
/****************************************************************************/
struct zc_circ_buf {
    struct zc_frm_dscr *items;   // Items
    u32                head;     // Points to the next location to store an item
    u32                tail;     // Points to the next location to retrieve an item
    u32                count;    // Current number of items
    u32                capacity; // Total number of possible items
};

/****************************************************************************/
// rx_cfg
// Changeable by application through netlink.
/****************************************************************************/
struct rx_cfg {
    // Current Rx MTU including FCS, but excluding Rx IFH.
    // Notice that the size of each Rx buffer does not change when this member
    // is changed (through a netlink call).
    // If requested MTU is greater than the current buffer size - given by
    // RX_MTU_DEFAULT - then RX_MULTI_DCB_SUPPORT must be set to 1 during
    // compilationof this module. If a frame spans multiple DCBs, it will be
    // copied to a fresh SKB during RX_callback().
    // Also notice that the MTU set here does not affect the MTU of Linux'
    // IP stack. Frames are forwarded to the IP stack according to the MTU set
    // here, but the IP stack may or may not drop it due to its own MTU.
    // Range of valid values is [RX_MTU_MIN; RX_MTU_MAX] bytes.
    // See vc3fdma_change_mtu() for Tx MTU.
    u32 mtu;
};

/* Information that need to be kept for each board. */
struct vc3fdma_private {
    struct net_device      *netdev;
    const struct fdma_chip *chip;
    void __iomem           *map_origin1;
    void __iomem           *map_origin2;
    struct resource        origin1, origin2;

    spinlock_t            lock;          // uFDMA lock
    struct napi_struct    napi;
    struct proc_dir_entry *proc_fs_dump_file;

    // Rx config
    struct rx_cfg      rx_cfg;

    // Rx state
    int                rx_work_done;           // Number of frames received during a driver->poll() operation
    u32                rx_mtu_cur;             // Current frame size (incl. FCS, but excl. Rx IFH) put aside for one Rx buffer
    u32                rx_buf_cnt_cur;         // Current total number of Rx buffers.
    u32                rx_frame_part_size;     // SKB_DATA_ALIGN()ed size of the whole data part of one frame (uFDMA buffer state, NET_SKB_PAD, and frame data itself).
    u32                rx_size_of_one_list;    // The number of bytes that one of the user- or krnl-lists take.
    u32                rx_data_size_bytes;     // The number of bytes needed in order to map the physical memory to user-space (when doing zero-copy). See also ZC_IOCTL_RX_MMAP_SIZE_GET.
    u8                 *rx_data;               // All frame data and housekeeping is allocated in one big chunk (size = #rx_data_size_bytes).
    struct zc_frm_dscr *rx_user_frm_list;      // Pointer to the first element of the user-list (used in zero-copy)
    struct zc_frm_dscr *rx_krnl_frm_list;      // Pointer to the first element of the kernel frame list.
    u32                rx_bufs_owned_by_fdma;  // Number of Rx buffers currently assigned to the FDMA.
    u32                rx_bufs_owned_by_appl;  // Number of Rx buffers currently owned by application (socket and/or zero-copy interface)

    // Debug counters
    u32 irq_ena_self_cnt;
    u32 irq_ena_napi_cnt;
    u8  irq_ena_last_was_napi;
    u32 rx_work_done_max;       // Max number of Rx_callback() calls per call to driver->poll().
    u32 netif_rx_cnt;
    u32 netif_tx_cnt;
    u32 netif_tx_drop_cnt;

    // Zero-copy state
    struct {
        dev_t                  major_minor;
        struct device          *device;
        struct class           *class;
        struct file_operations fops;
        struct cdev            cdev;
        bool                   dev_open;
        bool                   mmapped;
        u32                    rx_mtu;                 // Rx MTU currently configured through IOCTLs
        u32                    rx_buf_cnt;             // Rx buffer count currently configured through IOCTLs
        struct zc_circ_buf     rx_user_frms;           // Circular buffer of frames to user-space - in user-space virtual addresses.
        struct zc_circ_buf     rx_krnl_frms;           // Circular buffer of frames not yet processed by user-space - in kernel-space logical (virtual) addresses
        u8                     *user_space_data_start; // Contains the user-space virtual address corresponding to priv->rx_data.
        wait_queue_head_t      rx_wait_queue;
    } zc;

    // Restart handler info
    struct notifier_block restart_nb;
};

// Pre-declarations
static void rx_recycle(struct sk_buff *skb);
static void rx_no_recycle(struct sk_buff *skb);

/****************************************************************************/
// zc_circ_buf_init()
/****************************************************************************/
static inline void zc_circ_buf_init(struct zc_circ_buf *cb, struct zc_frm_dscr *items, u32 capacity)
{
    cb->items    = items;
    cb->head     = 0;
    cb->tail     = 0;
    cb->count    = 0;
    cb->capacity = capacity;

    // For the sake of user-space "circular" buffer (which isn't circular at all),
    // always reset the whole buffer.
    memset(items, 0, capacity * sizeof(*items));
}

/****************************************************************************/
// zc_circ_buf_add()
/****************************************************************************/
static inline void zc_circ_buf_add(struct zc_circ_buf *cb, struct zc_frm_dscr *item)
{
    if (cb->count == cb->capacity) {
        T_E("Circular buffer (%px) is full", cb);
        return;
    }

    cb->items[cb->head] = *item;
    if (++cb->head == cb->capacity) {
        cb->head = 0;
    }

    cb->count++;
}

/****************************************************************************/
// zc_circ_buf_get()
/****************************************************************************/
static inline struct zc_frm_dscr *zc_circ_buf_get(struct zc_circ_buf *cb)
{
    struct zc_frm_dscr *item;

    if (cb->count) {
        item = &cb->items[cb->tail];

        if (++cb->tail == cb->capacity) {
            cb->tail = 0;
        }

        cb->count--;
    } else {
        item = NULL;
    }

    return item;
}

/****************************************************************************/
// zc_circ_buf_peek()
/****************************************************************************/
static inline struct zc_frm_dscr *zc_circ_buf_peek(struct zc_circ_buf *cb)
{
    return cb->count ? &cb->items[cb->tail] : NULL;
}

/****************************************************************************/
// zc_circ_buf_empty()
/****************************************************************************/
static inline bool zc_circ_buf_empty(struct zc_circ_buf *cb)
{
    return cb->count == 0;
}

/****************************************************************************/
// zc_circ_buf_copy()
/****************************************************************************/
static inline u32 zc_circ_buf_copy(struct zc_circ_buf *dst, struct zc_circ_buf *src, u8 *dst_offset, u8 *src_offset)
{
    u32 i, tail = src->tail;

    for (i = 0; i < src->count; i++) {
        struct zc_frm_dscr item;

        // This is pretty involved, but basically, we subtract the #src_offset from
        // the items in the #src circular buffer and add #dst_offset before
        // adding the item to the #dst circular buffer.
        // This is for transforming kernel-space pointers to user-space pointers.
        item = src->items[tail];
        item.frm_ptr -= (unsigned long)src_offset;
        item.frm_ptr += (unsigned long)dst_offset;
        zc_circ_buf_add(dst, &item);

        if (++tail == src->capacity) {
            tail = 0;
        }
    }

    return src->count;
}

/****************************************************************************/
// rx_buffer_add_to_ufdma()
// priv->lock already taken.
/****************************************************************************/
static int rx_buffer_add_to_ufdma(u8 *data, gfp_t flags)
{
    struct vc3fdma_private       *priv   = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    struct sk_buff               *skb;
    struct skb_shared_info       *shinfo;
    vtss_ufdma_buf_dscr_t        buf_dscr;
    int                          rc;

    // The second argument tells how much this SKB (excluding size of skb itself
    // and skb_shinfo) uses. If the rcvbuf size of vtss.ifh is exceeded (see
    // sk->sk_rcvbuf, SO_RCVBUF, SK_RMEM_MAX, and rmem_default),
    // netif_skb_receive() (in fact .../net/packet/af_packet.c#packet_rcv#2074)
    // will toss the frame and release the SKB right away. Since we have a fixed
    // amount of frame buffers that get reused, we don't want to utilize this
    // functionality, but let all extracted frames go to the vtss.ifh interface.
    // This means that we must set a small value for this one and let the
    // application set a large value for SO_RCVBUF.
    if ((skb = __build_skb(data, 1000)) == NULL) {
        T_E("Unable to allocate Rx SKB");
        return -ENOMEM;
    }

    // Inspired by .../net/netlink/af_netlink.c
    // __build_skb() clears the whole SKB except for
    // tail, end, head, data, truesize, and users.
    // It then sets users.counter to 1 and truesize to sizeof(sk_buff).
    // We override that here.
    skb->dev        = priv->netdev;
    skb->head       = data;
    skb->data       = data;
    skb_reset_tail_pointer(skb); // Make tail point to data (use a function, because on some platforms, it's an offset while on others, it's a pointer).
    skb->end        = skb->tail + priv->rx_frame_part_size;

    // Reserve headroom for uFDMA's per-frame state.
    skb_reserve(skb, driver->props.buf_state_size_bytes);

    // Reserve headroom for NET_SKB_PAD
    skb_reserve(skb, NET_SKB_PAD);

    // Initialize the shared info part as well */
    shinfo = skb_shinfo(skb);

    memset(shinfo, 0, offsetof(struct skb_shared_info, dataref));
    atomic_set(&shinfo->dataref, 0); // Not owned by anyone yet.
    shinfo->free = rx_recycle;
    //kmemcheck_annotate_variable(shinfo->destructor_arg);

    // Time to create and pass the structure to the uFDMA
    buf_dscr.buf_state      = skb->head;
    buf_dscr.buf            = skb->data;
    buf_dscr.buf_size_bytes = priv->rx_mtu_cur + driver->props.rx_ifh_size_bytes;
    buf_dscr.context        = skb; // So that we can get back to the SKB in RX_callback()

    if ((rc = driver->rx_buf_add(driver, &buf_dscr)) != 0) {
        T_E("uFDMA error: %s", driver->error_txt(driver, rc));
        atomic_set(&shinfo->dataref, 1);
        shinfo->free = rx_no_recycle;
        consume_skb(skb);
        return -EIO;
    }

    priv->rx_bufs_owned_by_fdma++;

    return 0;
}

/****************************************************************************/
// rx_no_recycle()
/****************************************************************************/
static void rx_no_recycle(struct sk_buff *skb)
{
    // Don't add the frame data back to the FDMA, and don't count it
}

#if 0
#define L() do {                 \
    T_E("A");                    \
    spin_lock_bh(&priv->lock);   \
    T_E("B");                    \
    } while (0)

#define U() do {                 \
    T_E("C");                    \
    spin_unlock_bh(&priv->lock); \
    T_E("D");                    \
    } while (0)
#else
#define L() do {                 \
    spin_lock_bh(&priv->lock);   \
    } while (0)

#define U() do {                 \
    spin_unlock_bh(&priv->lock); \
    } while (0)
#endif

/****************************************************************************/
// rx_recycle()
// Called when netif_receive_skb() and all its listeners have
// handled the frame.
// We can now re-feed the buffer back to the FDMA.
/****************************************************************************/
static void rx_recycle(struct sk_buff *skb)
{
    struct vc3fdma_private *priv = vc3fdma_inst;

    if (!priv || !priv->chip || !priv->chip->driver) {
        // We've probably been uninitialized and someone
        // (the IP stack?) held a frame while this happened.
        // Just let the SKB framework free the SKB itself.
        return;
    }

    L();

    priv->rx_bufs_owned_by_appl--;

    // Since the SKB framework will always free the SKB itself once
    // this function returns, we have to create a new SKB and copy
    // the data part of the old one into that.
    (void)rx_buffer_add_to_ufdma(skb->head, GFP_ATOMIC);

    U();
}

/****************************************************************************/
// rx_data_size_get()
// Given a MTU and a buffer count, compute the total number of bytes required
// to be allocated in one big chunk, so that a possible zero-copy application
// can map the whole region in one go, so that we avoid cache aliasing issues
// (that we know exist on the MIPS), because the dcache size (32 Kbytes)
// divided by the Linux kernel page size (4 Kbytes) is greater than the number
// of cache ways (4). Or said in another way: Each way is 8 Kbytes which is
// greater than the page size (PAGE_SIZE) of 4 Kbytes.
// You can see for yourself by making sure that the printk()s in
// .../arch/mips/mm/c-r4k.c/probe_pcache() print something on boot.
//
// The function takes a number of pointers that gets filled with various
// sub-sizes, so that the actual allocator function may assign the invidual
// areas correctly.
//
// The function returns 0 on error.
/****************************************************************************/
static u32 rx_data_size_get(u32 rx_mtu, u32 rx_buf_cnt, u32 *size_of_one_list, u32 *size_of_one_frame_excl_shared_skb, u32 *total_size_of_one_frame)
{
    struct vc3fdma_private       *priv   = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    unsigned int                 frame_buffer_alloc_size;
    u32                          size_of_skb_shared_info, total_size;
    int                          rc;

    // We allocate *all* the frame buffers in one big chunk.
    // If the requested size is too high, kmalloc() might fail,
    // but the alternative would be to allocate per frame, which
    // has proven not so nice due to cache aliasing when mapping a frame
    // from kernel- to user-space.
    // We have to use kmalloc() and not vmalloc(), because
    // we want the address space to be contiguous in both virtual
    // and physical address space, and because part of the memory
    // will be handed over to the FDMA, so an easy conversion between
    // physical and kernel addresses must exist.
    //
    // The layout is as follows:
    //
    //  =----------------------=
    //  | rx_user_frm_list     |
    //  | (rx_buf_cnt + 1      |
    //  | entries).            |
    //  =----------------------=
    //  | rx_krnl_frm_list     |
    //  | (rx_buf_cnt + 1      |
    //  | entries).            |
    //  =----------------------=
    //  | Frame buffer 0       |
    //  =----------------------=
    //  | Frame buffer 1       |
    //  =----------------------=
    //          ...
    //  =----------------------=
    //  | Frame buffer N-1     |
    //  | (rx_buf_cnt such)    |
    //  =----------------------=
    //
    //
    // Where the layout of one frame buffer is as follows:
    //
    //  =----------------------=
    //  | buf_state            |
    //  | (housekeeping        |
    //  | needed by uFDMA)     |
    //  =----------------------=
    //  | NET_SKB_PAD          |
    //  | bytes of             |
    //  | headroom.            |
    //  =----------------------=
    //  | Actual frame         |
    //  | buffer (based on     |
    //  | IFH size and rx_mtu) |
    //  =----------------------=
    //  | skb_shared_info      |
    //  | (it's actually the   |
    //  | work of alloc_skb()  |
    //  | we're doing).        |
    //  =----------------------=
    //
    // In front of the frame buffers, we allocate two lists of zc_frm_dscr items.
    // These lists are only used if zero-copy is enabled.
    // The first list of rx_buf_cnt + 1 entries is handed over to user-space
    // whenever she calls poll(). It contains user-space virtual addresses of
    // where unhandled frames can be found along with a length and a timestamp.
    // The application must serve these frames in order, overwrite processed entries
    // with NULL, and stop whenever a NULL pointer is reached. Calling poll() again
    // will cause this driver to look into the second and third lists (which get
    // updated upon every frame arrival) and mark all those frames that user-space
    // has handled as handled from a zero-copy perspective (frame handling may still
    // be in progress on the socket side). The user-list will then get re-populated
    // and NULL-terminated with newly arrived frames, and poll() will return if
    // non-empty.
    // Only the user-list is maintained with user-space virtual addresses, but since
    // we have one large allocation for all frame buffers, there is a one-to-one
    // correspondance between user- and kernel-space addresses.
    // On the allocation-side, we must ensure that there is room for one extra
    // entry for NULL-termination (in case all rx_buf_cnt buffers are available to
    // user-space), and that each list starts on a cache line boundary and that the
    // last byte of each list doesn't interfere with other data. The additional item
    // is only needed in the user-space list.
    // So, we need one extra item than buffers and each pointer is
    // sizeof(struct zc_frm_dscr) long.
    // We need to make sure we can cache-line-align it, hence adding the size of
    // a cache line.
    // Also, for simplicity, we allocate these two lists whether or not the zero-copy
    // chardev is open.
    *size_of_one_list = sizeof(struct zc_frm_dscr) * (rx_buf_cnt + 1) + DCACHE_LINE_SIZE_BYTES;

    // The uFDMA requires some housekeeping memory per frame. This can be any-aligned and is
    // given by driver->props.buf_state_size_bytes.
    //
    // The SKB framework would set up NET_SKB_PAD bytes of headroom for every allocated
    // data-area belonging to an SKB, but since we handle all the allocation,
    // we do it ourselves. This driver actually uses a bit of this to prepend the
    // ifh_encap[] header to the data.
    //
    // Also, the uFDMA may need more memory for the actual frame data than the simple
    // sum of IFH size and MTU.
    // This is because of cache alignment (avoid invalidating/flushing other users' data,
    // which could occur if the frame buffer wasn't completely within its own cache lines).
    // The uFDMA provides a function for returning the required size.
    if ((rc = priv->chip->driver->rx_buf_alloc_size_get(driver, rx_mtu + driver->props.rx_ifh_size_bytes, &frame_buffer_alloc_size))) {
        T_E("uFDMA error: %s", driver->error_txt(driver, rc));
        return 0; // Signifies error
    }

    // The buffer state, NET_SKB_PAD bytes of headroom, and frame buffer itself
    // must be SKB_DATA_ALIGN()-aligned in size.
    *size_of_one_frame_excl_shared_skb = SKB_DATA_ALIGN(driver->props.buf_state_size_bytes + NET_SKB_PAD + frame_buffer_alloc_size);

    // And so must the skb_shared_info. This part of the frame buffer is shared
    // amongst all SKB clones, and holds the reference count so that we know when
    // we can give the buffer back to the uFDMA.
    size_of_skb_shared_info = SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

    *total_size_of_one_frame = *size_of_one_frame_excl_shared_skb + size_of_skb_shared_info; // If it's individual parts are SKB_DATA_ALIGN()ed, so will the sum.

    // Finally, make the whole structure a multiple of the page size.
    // If we didn't, the requested_size wouldn't match the expected_size in zc_mmap().
    total_size = 2 * (*size_of_one_list) + rx_buf_cnt * (*total_size_of_one_frame);
    total_size = VC3FDMA_ALIGNED_SIZE(total_size, PAGE_SIZE);

    return total_size;
}

/****************************************************************************/
// rx_data_size_total_get()
/****************************************************************************/
static u32 rx_data_size_total_get(u32 rx_mtu, u32 rx_buf_cnt)
{
    u32 dummy1, dummy2, dummy3;

    return rx_data_size_get(rx_mtu, rx_buf_cnt, &dummy1, &dummy2, &dummy3);
}

/****************************************************************************/
// rx_buffers_add()
// priv->lock already taken.
/****************************************************************************/
static int rx_buffers_add(u32 rx_mtu_new, u32 rx_buf_cnt_new)
{
    struct vc3fdma_private *priv = vc3fdma_inst;
    int                    rc;
    u32                    i;
    u32                    size_of_one_list, size_of_one_frame_excl_shared_skb, total_size_of_one_frame, total_size;
    u8                     *ptr;

    // All the good stuff is documented in rx_data_size_get()
    if ((total_size = rx_data_size_get(rx_mtu_new, rx_buf_cnt_new, &size_of_one_list, &size_of_one_frame_excl_shared_skb, &total_size_of_one_frame)) == 0) {
        return -ENOMEM;
    }

    T_D("rx_mtu = %u, rx_buf_cnt = %u, size_of_one_list = %u, size_of_one_frame_excl_shared_skb = %u, total_size_of_one_frame = %u => total_size = %u", rx_mtu_new, rx_buf_cnt_new, size_of_one_list, size_of_one_frame_excl_shared_skb, total_size_of_one_frame, total_size);

    // Free any previous data memory (RBNTBD: Make sure all bufs are released by listeners first).
    if (priv->rx_bufs_owned_by_appl) {
        T_E("Some (%u) of the old Rx buffers are currently owned by the application. Try again later", priv->rx_bufs_owned_by_appl);
        return -EAGAIN;
    }

    kfree(priv->rx_data);

    priv->rx_bufs_owned_by_fdma  = 0;
    priv->rx_bufs_owned_by_appl  = 0;
    priv->rx_mtu_cur             = rx_mtu_new;
    priv->rx_buf_cnt_cur         = rx_buf_cnt_new;
    priv->rx_size_of_one_list    = size_of_one_list;
    priv->rx_frame_part_size     = size_of_one_frame_excl_shared_skb;
    priv->rx_data_size_bytes     = total_size;

    if ((priv->rx_data = kzalloc(priv->rx_data_size_bytes, GFP_KERNEL | GFP_DMA)) == NULL) {
        T_E("Unable to allocate %u bytes", priv->rx_data_size_bytes);
        return -ENOMEM;
    }

    priv->rx_user_frm_list = (struct zc_frm_dscr *)VC3FDMA_CACHE_ALIGNED_SIZE(priv->rx_data + 0 * size_of_one_list);
    priv->rx_krnl_frm_list = (struct zc_frm_dscr *)VC3FDMA_CACHE_ALIGNED_SIZE(priv->rx_data + 1 * size_of_one_list);
    ptr = priv->rx_data + 2 * size_of_one_list;

    if ((void *)priv->rx_user_frm_list != (void *)priv->rx_data) {
        T_E("Something terrible will happen if user space enables zero-copy (priv->rx_user_frm_list = %px, priv->rx_data = %px)", priv->rx_user_frm_list, priv->rx_data);
    }

    T_D("Adding %u %u-byte buffers (total %u bytes, got priv->rx_data = %px)", rx_buf_cnt_new, rx_mtu_new, priv->rx_data_size_bytes, priv->rx_data);

    // Now add the buffers to the uFDMA.
    for (i = 0; i < rx_buf_cnt_new; i++) {
        if ((rc = rx_buffer_add_to_ufdma(ptr, GFP_KERNEL)) != 0) {
            return rc;
        }

        ptr += total_size_of_one_frame;
    }

    return 0;
}

/****************************************************************************/
// rx_buffers_refresh()
// This is called in User Context only.
/****************************************************************************/
static int rx_buffers_refresh(u32 rx_mtu_new, u32 rx_buf_cnt_new)
{
    struct vc3fdma_private       *priv = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver;
    int                          rc;

    if (!priv) {
        return -EINVAL;
    }

    if (rx_mtu_new == priv->rx_mtu_cur && rx_buf_cnt_new == priv->rx_buf_cnt_cur) {
        // The requested MTU and buffer count is already what we're running with.
        return 0;
    }

    L();

    driver = priv->chip->driver;
    if ((rc = driver->rx_free(priv->chip->driver)) != 0) {
        T_E("ufdma::rx_free() failed: %s", driver->error_txt(driver, rc));
        // Fall through
    }

    // Add Rx buffers
    rc = rx_buffers_add(rx_mtu_new, rx_buf_cnt_new);

    U();

    return rc;
}

/****************************************************************************/
// Netlink support functions.
// Netlink is used to configure Rx throttling and uFDMA trace from user space.
// "NLA" stands for "NetLink Attribute"
/****************************************************************************/

/****************************************************************************/
// VC3FDMA_NLA_U32_GET()
/****************************************************************************/
#define VC3FDMA_NLA_U32_GET(_attr_, _val_)                        \
    do {                                                          \
        if (_attr_ != NULL) {                                     \
            if (nla_len(_attr_) != 4) {                           \
                T_E("Expected 4, got %u bytes", nla_len(_attr_)); \
                return -EINVAL;                                   \
            }                                                     \
                                                                  \
            _val_ = nla_get_u32(_attr_);                          \
        }                                                         \
    } while (0)

/****************************************************************************/
// VC3FDMA_NLA_U32_PUT()
/****************************************************************************/
#define VC3FDMA_NLA_U32_PUT(_attr_, _val_)             \
    if ((rc = nla_put_u32(msg, _attr_, _val_)) != 0) { \
        T_E("nla_put_u32() failed");                   \
        goto do_exit;                                  \
    }

/****************************************************************************/
// Parameters used in the throttling interface between user- and kernel space.
// Keep in sync with the user-space definitions
/****************************************************************************/
enum {
    VTSS_PACKET_ATTR_NONE,                            /**< Must come first                                                           */
    VTSS_PACKET_ATTR_RX_THROTTLE_TICK_PERIOD_MSEC,    /**< Number of milliseconds between two throttle ticks, 0 to disable, max 1000 */
    VTSS_PACKET_ATTR_RX_THROTTLE_QU_CFG,              /**< Config for one queue consists of the following four parameters            */
    VTSS_PACKET_ATTR_RX_THROTTLE_QU_NUMBER,           /**< Must-be-present attribute identifying queue number                        */
    VTSS_PACKET_ATTR_RX_THROTTLE_FRM_LIMIT_PER_TICK,  /**< Max number of frames extracted between two ticks w/o suspension           */
    VTSS_PACKET_ATTR_RX_THROTTLE_BYTE_LIMIT_PER_TICK, /**< Max number of bytes extracted between two ticks w/o suspension            */
    VTSS_PACKET_ATTR_RX_THROTTLE_SUSPEND_TICK_CNT,    /**< Number of ticks to suspend when suspending                                */
    VTSS_PACKET_ATTR_TRACE_LAYER,                     /**< AIL (0) or CIL (1)                                                        */
    VTSS_PACKET_ATTR_TRACE_GROUP,                     /**< Default (0), Rx (1), Tx (2)                                               */
    VTSS_PACKET_ATTR_TRACE_LEVEL,                     /**< None (0), Error (1), Info (2), Debug (3)                                  */
    VTSS_PACKET_ATTR_RX_CFG_MTU,                      /**< Rx MTU [RX_MTU_MIN; RX_MTU_MAX]                                           */
    VTSS_PACKET_ATTR_END,                             /**< Must come last                                                            */
};

#define VTSS_PACKET_ATTR_MAX (VTSS_PACKET_ATTR_END + 1)

static int               rx_throttle_tick_period_msec;
static struct timer_list rx_throttle_timer;
static spinlock_t        rx_throttle_timer_lock; // Protects rx_throttle_timer operations

/****************************************************************************/
// rx_throttle_timer_tick_add()
// This can be invoked from both user and softirq context.
// Either way, a spin lock protects it.
// It is guaranteed that the timer is currently not in the
// kernel's timer list and that the new timeout is non-zero.
/****************************************************************************/
static void rx_throttle_timer_tick_add(void (*func)(struct timer_list *t))
{
    timer_setup(&rx_throttle_timer, func, 0);
    mod_timer(&rx_throttle_timer, jiffies + (rx_throttle_tick_period_msec * HZ) / 1000);
}

/****************************************************************************/
// rx_throttle_time_out()
// This is invoked from softirq context.
/****************************************************************************/
static void rx_throttle_time_out(struct timer_list *t)
{
    struct vc3fdma_private       *priv = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    int                          rc;

    spin_lock_bh(&rx_throttle_timer_lock);
    if (rx_throttle_tick_period_msec == 0) {
        // We're going to disable this feature.
        // Don't even call the driver, since it might
        // be gone already.
        goto do_exit;
    }

    if (!driver) {
        T_E("No uFDMA driver");
        goto do_exit; // Don't re-add ourselves
    }

    if ((rc = driver->throttle_tick(driver) != 0)) {
        T_E("ufdma::throttle_tick() failed: %s", driver->error_txt(driver, rc));
        goto do_exit; // Don't re-add ourselves
    }

    // Re-add ourselves.
    rx_throttle_timer_tick_add(rx_throttle_time_out);

do_exit:
    spin_unlock_bh(&rx_throttle_timer_lock);
}

/****************************************************************************/
// rx_throttle_timer_tick_change()
// This is invoked from user context
/****************************************************************************/
static void rx_throttle_timer_tick_change(u32 new_timeout)
{
    spin_lock_bh(&rx_throttle_timer_lock);
    rx_throttle_tick_period_msec = new_timeout;

    if (timer_pending(&rx_throttle_timer)) {
        // The timer is currently active. If
        // the new timeout is 0, we should disable
        // the timer ASAP.
        if (new_timeout == 0) {
            del_timer(&rx_throttle_timer);
        } else {
            // The new timeout is non-zero and the timer
            // is currently running.
            // Let it timeout, and it will automatically
            // use the new timeout once that happens.
        }
    } else if (new_timeout != 0) {
        // The timer is currently not active. Start it.
        rx_throttle_timer_tick_add(rx_throttle_time_out);
    }

    spin_unlock_bh(&rx_throttle_timer_lock);
}

/****************************************************************************/
// Functions working on one or more of the attributes above.
// Keep in sync with the user-space definitions
/****************************************************************************/
enum {
    VTSS_PACKET_GENL_NOOP,                /** Must come first                                     */
    VTSS_PACKET_GENL_RX_THROTTLE_CFG_GET, /**< Get current throttle configuration from the kernel */
    VTSS_PACKET_GENL_RX_THROTTLE_CFG_SET, /**< Change throttle configuration in the kernel        */
    VTSS_PACKET_GENL_TRACE_CFG_SET,       /**< Set uFDMA trace level settings                     */
    VTSS_PACKET_GENL_RX_CFG_GET,          /**< Get Rx config                                      */
    VTSS_PACKET_GENL_RX_CFG_SET,          /**< Set Rx config                                      */
    VTSS_PACKET_GENL_STATI_CLEAR,         /**< Clear statistics                                   */
    // Add new operations here
};

/****************************************************************************/
// Throttle policies (basically: what kind of data comes with a given attribute)
/****************************************************************************/
static const struct nla_policy packet_policy[VTSS_PACKET_ATTR_END] = {
    [VTSS_PACKET_ATTR_NONE]                            = {.type = NLA_UNSPEC},
    [VTSS_PACKET_ATTR_RX_THROTTLE_TICK_PERIOD_MSEC]    = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_RX_THROTTLE_QU_CFG]              = {.type = NLA_NESTED},
    [VTSS_PACKET_ATTR_RX_THROTTLE_QU_NUMBER]           = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_RX_THROTTLE_FRM_LIMIT_PER_TICK]  = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_RX_THROTTLE_BYTE_LIMIT_PER_TICK] = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_RX_THROTTLE_SUSPEND_TICK_CNT]    = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_TRACE_LAYER]                     = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_TRACE_GROUP]                     = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_TRACE_LEVEL]                     = {.type = NLA_U32},
    [VTSS_PACKET_ATTR_RX_CFG_MTU]                      = {.type = NLA_U32},
};

/****************************************************************************/
// packet_cmd_noop()
// No-op netlink operation
/****************************************************************************/
static int packet_cmd_noop(struct sk_buff *skb, struct genl_info *info)
{
    return 0;
}

/****************************************************************************/
// rx_throttle_cmd_cfg_get()
// Return current rx throttle configuration to user-space
/****************************************************************************/
static int rx_throttle_cmd_cfg_get(struct sk_buff *skb, struct genl_info *info)
{
    struct vc3fdma_private       *priv   = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    struct nlattr                *row;
    vtss_ufdma_throttle_conf_t   throttle_cfg;
    int                          rc;
    unsigned int                 qu;
    struct sk_buff               *msg;
    void                         *hdr;

    T_I("Getting throttle");

    if (!driver) {
        T_E("No uFDMA driver");
        return -EIO;
    }

    if ((rc = driver->throttle_conf_get(driver, &throttle_cfg) != 0)) {
        T_E("ufdma::throttle_conf_get() failed: %s", driver->error_txt(driver, rc));
        return -EIO;
    }

    if ((msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL)) == NULL) {
        return -ENOMEM;
    }

    if ((hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq, &packet_generic_netlink_family, 0, VTSS_PACKET_GENL_RX_THROTTLE_CFG_GET)) == NULL) {
        T_E("genlmsg_put() failed");
        rc = -EMSGSIZE;
        goto do_exit;
    }

    VC3FDMA_NLA_U32_PUT(VTSS_PACKET_ATTR_RX_THROTTLE_TICK_PERIOD_MSEC, rx_throttle_tick_period_msec);

    for (qu = 0; qu < ARRAY_SIZE(throttle_cfg.frm_limit_per_tick); qu++) {
        if ((row = nla_nest_start(msg, VTSS_PACKET_ATTR_RX_THROTTLE_QU_CFG)) == NULL) {
            T_E("nla_nest_start() failed");
            rc = -EMSGSIZE;
            goto do_exit;
        }

        VC3FDMA_NLA_U32_PUT(VTSS_PACKET_ATTR_RX_THROTTLE_QU_NUMBER,           qu);
        VC3FDMA_NLA_U32_PUT(VTSS_PACKET_ATTR_RX_THROTTLE_FRM_LIMIT_PER_TICK,  throttle_cfg.frm_limit_per_tick[qu]);
        VC3FDMA_NLA_U32_PUT(VTSS_PACKET_ATTR_RX_THROTTLE_BYTE_LIMIT_PER_TICK, throttle_cfg.byte_limit_per_tick[qu]);
        VC3FDMA_NLA_U32_PUT(VTSS_PACKET_ATTR_RX_THROTTLE_SUSPEND_TICK_CNT,    throttle_cfg.suspend_tick_cnt[qu]);

        nla_nest_end(msg, row);
    }

    genlmsg_end(msg, hdr);
    return genlmsg_unicast(genl_info_net(info), msg, info->snd_portid);

do_exit:
    nlmsg_free(msg);

    return rc;
}

/****************************************************************************/
// rx_throttle_cmd_cfg_set()
// Change rx throttle configuration based on what we got from user-space.
/****************************************************************************/
static int rx_throttle_cmd_cfg_set(struct sk_buff *skb, struct genl_info *info)
{
    struct vc3fdma_private       *priv   = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    vtss_ufdma_throttle_conf_t   throttle_cfg;
    struct nlattr                *attr;
    int                          rc, remaining;
    u32                          tick_period = 0;
    bool                         tick_period_seen = false;

    // info->attrs[] doesn't work if the same attribute appears multiple times.
    // This might be the case in ours, because we have a table with per-queue
    // config. Therefore, we use info->genlhdr to traverse the raw data.
    // It must be possible for the user to only set part of the configuration,
    // so we don't check that everything is configured.

    T_I("Setting throttle");

    if (!driver) {
        T_E("No uFDMA driver");
        return -EIO;
    }

    if ((rc = driver->throttle_conf_get(driver, &throttle_cfg) != 0)) {
        T_E("ufdma::throttle_conf_get() failed: %s", driver->error_txt(driver, rc));
        return -EIO;
    }

    attr      = genlmsg_data(info->genlhdr);
    remaining = genlmsg_len(info->genlhdr);

    while (nla_ok(attr, remaining)) {
        switch (nla_type(attr)) {
        case VTSS_PACKET_ATTR_RX_THROTTLE_TICK_PERIOD_MSEC:
            VC3FDMA_NLA_U32_GET(attr, tick_period);

            if (tick_period > 1000) {
                return -EINVAL;
            }

            tick_period_seen = true;
            break;

        case VTSS_PACKET_ATTR_RX_THROTTLE_QU_CFG: {
            struct nlattr *qu_attrs[VTSS_PACKET_ATTR_MAX];
            u32           qu = ARRAY_SIZE(throttle_cfg.frm_limit_per_tick);

            if ((rc = nla_parse_nested(qu_attrs, VTSS_PACKET_ATTR_END, attr, packet_policy, NULL)) != 0) {
                T_E("Failed to parse queue config attributes (err = %d)", rc);
                return rc;
            }

            // All parameters need not be there, but the queue number must.
            VC3FDMA_NLA_U32_GET(qu_attrs[VTSS_PACKET_ATTR_RX_THROTTLE_QU_NUMBER], qu);

            if (qu >= ARRAY_SIZE(throttle_cfg.frm_limit_per_tick)) {
                T_E("Got queue number %u, but only %u queues are configurable", qu, ARRAY_SIZE(throttle_cfg.frm_limit_per_tick));
                return -EINVAL;
            }

            VC3FDMA_NLA_U32_GET(qu_attrs[VTSS_PACKET_ATTR_RX_THROTTLE_FRM_LIMIT_PER_TICK],  throttle_cfg.frm_limit_per_tick[qu]);
            VC3FDMA_NLA_U32_GET(qu_attrs[VTSS_PACKET_ATTR_RX_THROTTLE_BYTE_LIMIT_PER_TICK], throttle_cfg.byte_limit_per_tick[qu]);
            VC3FDMA_NLA_U32_GET(qu_attrs[VTSS_PACKET_ATTR_RX_THROTTLE_SUSPEND_TICK_CNT],    throttle_cfg.suspend_tick_cnt[qu]);
            break;
        }

        default:
            T_E("Unknown attribute %hu", nla_type(attr));
            return -EINVAL;
        }

        attr = nla_next(attr, &remaining);
    }

    if (tick_period_seen && tick_period != rx_throttle_tick_period_msec) {
        rx_throttle_timer_tick_change(tick_period);
    }

    if (tick_period == 0) {
        // Disable throttling in the driver.
        memset(&throttle_cfg, 0, sizeof(throttle_cfg));
    }

    if ((rc = driver->throttle_conf_set(driver, &throttle_cfg) != 0)) {
        T_E("ufdma::throttle_conf_set() failed: %s", driver->error_txt(driver, rc));
        return -EIO;
    }

    return 0;
}

/****************************************************************************/
// trace_cmd_cfg_set()
// Change trace configuration based on what we got from user-space.
/****************************************************************************/
static int trace_cmd_cfg_set(struct sk_buff *skb, struct genl_info *info)
{
    struct vc3fdma_private       *priv   = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    vtss_ufdma_trace_layer_t     layer;
    vtss_ufdma_trace_group_t     group;
    vtss_ufdma_trace_level_t     level;
    int                          rc;

    T_I("Setting trace");

    if (!driver) {
        T_E("No uFDMA driver");
        return -EIO;
    }

    // All three trace attributes must be present.
    if (!info->attrs[VTSS_PACKET_ATTR_TRACE_LAYER]) {
        T_E("Trace layer attribute not present");
        return -EINVAL;
    }

    if (!info->attrs[VTSS_PACKET_ATTR_TRACE_GROUP]) {
        T_E("Trace group attribute not present");
        return -EINVAL;
    }

    if (!info->attrs[VTSS_PACKET_ATTR_TRACE_LEVEL]) {
        T_E("Trace level attribute not present");
        return -EINVAL;
    }

    VC3FDMA_NLA_U32_GET(info->attrs[VTSS_PACKET_ATTR_TRACE_LAYER], layer);
    VC3FDMA_NLA_U32_GET(info->attrs[VTSS_PACKET_ATTR_TRACE_GROUP], group);
    VC3FDMA_NLA_U32_GET(info->attrs[VTSS_PACKET_ATTR_TRACE_LEVEL], level);

    T_I("Setting trace: %u:%u:%u", layer, group, level);

    if ((rc = driver->trace_level_set(driver, layer, group, level)) != 0) {
         T_E("ufdma::trace_level_set() failed: %s", driver->error_txt(driver, rc));
         return -EINVAL;
    }

    return 0;
}

/****************************************************************************/
// rx_cmd_cfg_get()
// Change trace configuration based on what we got from user-space.
/****************************************************************************/
static int rx_cmd_cfg_get(struct sk_buff *skb, struct genl_info *info)
{
    struct vc3fdma_private *priv = vc3fdma_inst;
    struct sk_buff         *msg;
    void                   *hdr;
    int                    rc;

    T_I("Getting Rx cfg");

    if (!priv) {
        T_E("No vc3fdma device");
        return -EIO;
    }

    if ((msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL)) == NULL) {
        return -ENOMEM;
    }

    if ((hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq, &packet_generic_netlink_family, 0, VTSS_PACKET_GENL_RX_CFG_GET)) == NULL) {
        T_E("genlmsg_put() failed");
        rc = -EMSGSIZE;
        goto do_exit;
    }

    VC3FDMA_NLA_U32_PUT(VTSS_PACKET_ATTR_RX_CFG_MTU, priv->rx_cfg.mtu);
    T_I("Exited with mtu = %u", priv->rx_cfg.mtu);

    genlmsg_end(msg, hdr);
    return genlmsg_unicast(genl_info_net(info), msg, info->snd_portid);

do_exit:
    nlmsg_free(msg);
    return rc;
}

/****************************************************************************/
// rx_cmd_cfg_set()
// Change trace configuration based on what we got from user-space.
/****************************************************************************/
static int rx_cmd_cfg_set(struct sk_buff *skb, struct genl_info *info)
{
    struct vc3fdma_private *priv   = vc3fdma_inst;
    u32                    new_mtu = 0;

    T_I("Setting Rx cfg");

    if (!priv) {
        T_E("No vc3fdma device");
        return -EIO;
    }

    if (info->attrs[VTSS_PACKET_ATTR_RX_CFG_MTU]) {
        VC3FDMA_NLA_U32_GET(info->attrs[VTSS_PACKET_ATTR_RX_CFG_MTU], new_mtu);
    }

    if (new_mtu < RX_MTU_MIN || new_mtu > RX_MTU_MAX) {
        return -EINVAL;
    }

    if (!RX_MULTI_DCB_SUPPORT && new_mtu > RX_MTU_MAX) {
        T_E("vc3fdma not compiled with multi-Rx-DCB support, so can't set MTU to %u (max is %u)", new_mtu, RX_MTU_DEFAULT);
        return -EINVAL;
    }

    // The following takes effect immediately
    priv->rx_cfg.mtu = new_mtu;

    return 0;
}

/****************************************************************************/
// stati_clear()
// Clear statistics
/****************************************************************************/
static int stati_clear(struct sk_buff *skb, struct genl_info *info)
{
    struct vc3fdma_private       *priv = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver;

    T_I("Clearing statistics");

    if (!priv || !priv->chip) {
        T_E("No vc3fdma instance");
        return -EIO;
    }

    driver = priv->chip->driver;
    if (!driver) {
        T_E("No uFDMA driver");
        return -EIO;
    }

    L();
    driver->stati_clr(driver);

    priv->irq_ena_self_cnt  = 0;
    priv->irq_ena_napi_cnt  = 0;
    priv->netif_rx_cnt      = 0;
    priv->netif_tx_cnt      = 0;
    priv->netif_tx_drop_cnt = 0;
    priv->rx_work_done_max  = 0;

    U();

    return 0;
}

/****************************************************************************/
// Structure used to define the operations applicable to the rx throttle netlink family.
/****************************************************************************/
static const struct genl_ops packet_generic_netlink_operations[] = {
    {
        .cmd    = VTSS_PACKET_GENL_NOOP,
        .doit   = packet_cmd_noop,
        // No access control
    },
    {
        .cmd    = VTSS_PACKET_GENL_RX_THROTTLE_CFG_GET,
        .doit   = rx_throttle_cmd_cfg_get,
        .flags  = GENL_ADMIN_PERM
    },
    {
        .cmd    = VTSS_PACKET_GENL_RX_THROTTLE_CFG_SET,
        .doit   = rx_throttle_cmd_cfg_set,
        .flags  = GENL_ADMIN_PERM
    },
    {
        .cmd    = VTSS_PACKET_GENL_TRACE_CFG_SET,
        .doit   = trace_cmd_cfg_set,
        .flags  = GENL_ADMIN_PERM
    },
    {
        .cmd    = VTSS_PACKET_GENL_RX_CFG_GET,
        .doit   = rx_cmd_cfg_get,
        .flags  = GENL_ADMIN_PERM
    },
    {
        .cmd    = VTSS_PACKET_GENL_RX_CFG_SET,
        .doit   = rx_cmd_cfg_set,
        .flags  = GENL_ADMIN_PERM
    },
    {
        .cmd    = VTSS_PACKET_GENL_STATI_CLEAR,
        .doit   = stati_clear,
        .flags  = GENL_ADMIN_PERM
    },
};

/****************************************************************************/
// Structure used to register the throttle family into the netlink system.
/****************************************************************************/
static struct genl_family packet_generic_netlink_family = {
    .hdrsize = 0,
    .name    = "vtss_packet", // Cannot be longer than 15 chars (excl. NULL-termination)
    .version = 1,
    .maxattr = VTSS_PACKET_ATTR_MAX,
    .policy  = packet_policy,
    .ops     = packet_generic_netlink_operations,
    .n_ops   = ARRAY_SIZE(packet_generic_netlink_operations),
};


/****************************************************************************/
// packet_generic_netlink_init()
/****************************************************************************/
static void packet_generic_netlink_init(void)
{
    int rc;

    if ((rc = genl_register_family(&packet_generic_netlink_family)) != 0) {
        T_E("genl_register_family() failed with error = %d", rc);
        return;
    }

    spin_lock_init(&rx_throttle_timer_lock);
}

/****************************************************************************/
// packet_generic_netlink_uninit()
/****************************************************************************/
static void packet_generic_netlink_uninit(void)
{
    genl_unregister_family(&packet_generic_netlink_family);

    // Stop a possible kernel timer.
    rx_throttle_timer_tick_change(0);
}

/****************************************************************************/
// RX_buf_dscr_free()
/****************************************************************************/
static void RX_buf_dscr_free(struct vc3fdma_private *priv, vtss_ufdma_buf_dscr_t *buf_dscr, bool recycle)
{
    while (buf_dscr) {
        struct sk_buff         *skb    = buf_dscr->context;
        struct skb_shared_info *shinfo = skb_shinfo(skb);
        vtss_ufdma_buf_dscr_t  *buf_dscr_next = buf_dscr->next;

        atomic_set(&shinfo->dataref, 1);
        shinfo->free = recycle ? rx_recycle : rx_no_recycle;

        // Regarding ref-counting:
        // If we recycle, rx_recycle() will be called back. This function will
        // decrease appl and increase FDMA ref count, so in order to balance it
        // out, we need to do the opposite operation here.
        // If we don't recycle, rx_no_recycle() will be called back. This
        // function will not touch any of the ref counters. However, we have one
        // less frame owned by the FDMA.
        // Hence, always decrease FDMA ref count, but increase appl ref count
        // only when recycling.
        priv->rx_bufs_owned_by_fdma--;

        if (recycle) {
            priv->rx_bufs_owned_by_appl++;
        }

        kfree_skb(skb);
        buf_dscr = buf_dscr_next;
    }
}

/****************************************************************************/
//
// uFDMA support functions
//
/****************************************************************************/

/****************************************************************************/
// RX_callback()
// priv->lock taken by vc3fdma_poll()
/****************************************************************************/
static void RX_callback(vtss_ufdma_platform_driver_t *unused, vtss_ufdma_buf_dscr_t *buf_dscr)
{
    struct sk_buff               *skb  = buf_dscr->context;
    struct net_device            *dev  = skb->dev;
    struct vc3fdma_private       *priv = netdev_priv(dev);
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    bool                         jumbo = buf_dscr->next != NULL;
    u32                          frm_len;

    if (buf_dscr->result) {
        // This happens during driver->uninit() or driver->rx_free()
        // Prevent the rx_recycle() function from being invoked, because
        // we just want to free it, where the rx_recycle() function would
        // re-add it to the uFDMA.
        RX_buf_dscr_free(priv, buf_dscr, false /* free for good */);
        return;
    }

    // Compute frame length including FCS, but excluding IFH
    frm_len = buf_dscr->frm_length_bytes - driver->props.rx_ifh_size_bytes;
    if (priv->rx_cfg.mtu < frm_len) {
        T_D("Dropping frame of %u bytes (incl. FCS, excl. IFH). Max MTU = %u bytes", frm_len, priv->rx_cfg.mtu);
        RX_buf_dscr_free(priv, buf_dscr, true /* Back to uFDMA */);
        dev->stats.rx_dropped++;
        return;
    }

    if (jumbo) {
        // Multi-DCB frame received. We need to allocate a new buffer and copy
        // the fragments into it, because shinfo->frag_list is not
        // used/supported by raw socket recvmsg() (af_packet.c/datagram.c) on
        // received frames.
        unsigned char *ptr;

        // Allocate buffer with room for both IFH encapsulation and total frame
        // length. This one sets shinfo->dataref to 1.
        skb = alloc_skb(NET_SKB_PAD + sizeof(ifh_encap) + buf_dscr->frm_length_bytes, GFP_ATOMIC);

        if (skb) {
            vtss_ufdma_buf_dscr_t *buf_dscr_iter;

            // Reserve room for NET_SKB_PAD and IFH encapsulation.
            skb_reserve(skb, NET_SKB_PAD + sizeof(ifh_encap));

            // Adjust the SKB to the size of the actual frame data
            skb_put(skb, buf_dscr->frm_length_bytes);

            // Copy frame data to the SKB
            ptr = skb->data;

            buf_dscr_iter = buf_dscr;
            while (buf_dscr_iter) {
                memcpy(ptr, buf_dscr_iter->buf, buf_dscr_iter->fragment_size_bytes);
                ptr += buf_dscr_iter->fragment_size_bytes;
                buf_dscr_iter = buf_dscr_iter->next;
            }
        } else {
            dev->stats.rx_dropped++;
        }

        // Send the now copied buf_dscr back to the uFDMA
        RX_buf_dscr_free(priv, buf_dscr, true /* back to uFDMA */);

        if (!skb) {
            return;
        }
    } else {
        atomic_set(&skb_shinfo(skb)->dataref, 1);
        skb_put(skb, buf_dscr->frm_length_bytes);
    }

    if (unlikely(skb_headroom(skb) < sizeof(ifh_encap))) {
        T_E("Not enough headroom in SKB (need %u, only have %u)", sizeof(ifh_encap), skb_headroom(skb));
        RX_buf_dscr_free(priv, buf_dscr, true /* back to uFDMA */);

        if (jumbo) {
            // This is the Jumbo SKB allocated in this function.
            kfree_skb(skb);
        }

        dev->stats.rx_errors++;
        return;
    }

    // Add IFH ethernet encapsulation header
    memcpy(skb_push(skb, sizeof(ifh_encap)), ifh_encap, sizeof(ifh_encap));

#ifdef DEBUG
    print_hex_dump(KERN_DEBUG, "Rx ", DUMP_PREFIX_ADDRESS, 16, 1, skb->data, skb->len, true);
#endif

    if (priv->zc.mmapped) {
        if (jumbo) {
            T_E("Zero-copy doesn't currently support multi-DCB frames");
        } else {
            struct zc_frm_dscr item;
            item.frm_ptr   = skb->data;
            item.len       = skb->len;
            item.timestamp = buf_dscr->timestamp;
            item.head_ptr  = skb->head;

            zc_circ_buf_add(&priv->zc.rx_krnl_frms, &item);

            atomic_inc(&skb_shinfo(skb)->dataref);

            // Wake-up poll() sleepers.
            wake_up(&priv->zc.rx_wait_queue);
        }
    }

    skb->protocol = eth_type_trans(skb, dev);
    dev->stats.rx_packets++;
    dev->stats.rx_bytes += skb->len;

    if (!jumbo) {
        // SKB we pass to application comes from uFDMA pool, so ref-cnt it.
        priv->rx_bufs_owned_by_fdma--;
        priv->rx_bufs_owned_by_appl++;
    }

    // The following call always returns NET_RX_SUCCESS.
    netif_receive_skb(skb);
    priv->netif_rx_cnt++;
    priv->rx_work_done++;
}

/****************************************************************************/
// TX_callback()
/****************************************************************************/
static void TX_callback(vtss_ufdma_platform_driver_t *unused, vtss_ufdma_buf_dscr_t *buf_dscr)
{
    struct sk_buff *skb = buf_dscr->context;
    kfree(buf_dscr->buf_state);
    consume_skb(skb);
}

/****************************************************************************/
// CX_cache_flush()
/****************************************************************************/
static void CX_cache_flush(void *virt_addr, unsigned int bytes)
{
//    T_E("virt_addr = %px, bytes = %u", virt_addr, bytes);
    dma_cache_wback((unsigned long)virt_addr, bytes);
}

/****************************************************************************/
// CX_cache_invalidate()
/****************************************************************************/
static void CX_cache_invalidate(void *virt_addr, unsigned int bytes)
{
//    T_E("virt_addr = %px, bytes = %u", virt_addr, bytes);
    dma_cache_inv((unsigned long)virt_addr, bytes);
}

/****************************************************************************/
// CX_virt_to_phys()
/****************************************************************************/
static void *CX_virt_to_phys(void *virt_addr)
{
    return (void *)virt_to_phys(virt_addr);
}

/****************************************************************************/
// CX_trace_printf()
/****************************************************************************/
static char CX_lvl_to_char(vtss_ufdma_trace_level_t level)
{
    switch (level) {
    case VTSS_UFDMA_TRACE_LEVEL_ERROR:
        return 'E';

    case VTSS_UFDMA_TRACE_LEVEL_INFO:
        return 'I';

    case VTSS_UFDMA_TRACE_LEVEL_DEBUG:
        return 'D';

    default:
        return '?';
    }
}

/****************************************************************************/
// CX_trace_printf()
// We "force" the trace out by printing with KERN_ERR level.
// This still allows for disabling it with "dmesg -n 2" or similar.
/****************************************************************************/
static void CX_trace_printf(vtss_ufdma_trace_layer_t layer, vtss_ufdma_trace_group_t group, vtss_ufdma_trace_level_t level, const char *file, const int line, const char *function, const char *fmt, ...)
{
    va_list args;
    char full_fmt[256];

    snprintf(full_fmt, sizeof(full_fmt), KERN_ERR "%c %s#%d: %s\n", CX_lvl_to_char(level), function, line, fmt);
    full_fmt[sizeof(full_fmt) - 1] = '\0';

    va_start(args, fmt);
    vprintk_emit(0, -1, NULL, full_fmt, args);
    va_end(args);
}

/****************************************************************************/
// CX_trace_printf()
// We "force" the trace out by printing with KERN_ERR level.
// This still allows for disabling it with "dmesg -n 2" or similar.
/****************************************************************************/
static void CX_trace_hex_dump(vtss_ufdma_trace_layer_t layer, vtss_ufdma_trace_group_t group, vtss_ufdma_trace_level_t level, const char *file, const int line, const char *function, const unsigned char *byte_p, int byte_cnt)
{
    char loghead[64];

    snprintf(loghead, sizeof(loghead), "%c %s#%d: ", CX_lvl_to_char(level), function, line);
    loghead[sizeof(loghead) - 1] = '\0';
    print_hex_dump(KERN_ERR, loghead, DUMP_PREFIX_ADDRESS, 16, 1, byte_p, byte_cnt, true);
}

/****************************************************************************/
// CX_timestamp()
/****************************************************************************/
static unsigned long long CX_timestamp(void)
{
    return DIV64_U64_ROUND_UP(ktime_get_ns(), 1000); /* In usec */
}

/****************************************************************************/
// get_reg()
// Register helper
/****************************************************************************/
static void __iomem *get_reg(unsigned int chip_no, unsigned int addr)
{
    struct vc3fdma_private *priv = vc3fdma_inst;

    // Relative register to abs u32 address
    addr = priv->origin1.start + (addr * 4);

    if (addr >= priv->origin1.start && addr < priv->origin1.end) {
        return priv->map_origin1 + (addr - priv->origin1.start);
    } else if (addr >= priv->origin2.start && addr < priv->origin2.end) {
        return priv->map_origin2 + (addr - priv->origin2.start);
    }

    return NULL;
}

/****************************************************************************/
// CX_reg_read()
/****************************************************************************/
static unsigned int CX_reg_read(unsigned int chip_no, unsigned int addr)
{
    void __iomem *regptr = get_reg(chip_no, addr);

    if (regptr) {
        return readl(regptr);
    }

    T_E("Illegal address referenced: address = %d:0x%08x", chip_no, addr);
    BUG();
    return -1;
}

/****************************************************************************/
// CX_reg_write()
/****************************************************************************/
static void CX_reg_write(unsigned int chip_no, unsigned int addr, unsigned int value)
{
    void __iomem *regptr = get_reg(chip_no, addr);

    if (regptr) {
        writel(value, regptr);
        return;
    }

    T_E("Illegal address referenced: address = %d:0x%08x", chip_no, addr);
    BUG();
}

/****************************************************************************/
// vc3fdma_poll()
// Invoked in softirq context.
/****************************************************************************/
static int vc3fdma_poll(struct napi_struct *napi, int budget)
{
    struct vc3fdma_private       *priv = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;

    // Ensure uFDMA driver calls are serialized
    L();

    priv->rx_work_done = 0;

    // This call indirectly invokes RX_callback() and TX_callback()
    // driver->poll(driver, budget);
    driver->poll(driver, 0);

    if (priv->rx_work_done > priv->rx_work_done_max) {
        priv->rx_work_done_max = priv->rx_work_done;
    }

    U();

    if (priv->rx_work_done < budget) {
        priv->irq_ena_self_cnt++;
        priv->irq_ena_last_was_napi = 0;
        napi_complete(napi);
        enable_irq(priv->netdev->irq);
    } else {
        priv->irq_ena_napi_cnt++;
        priv->irq_ena_last_was_napi = 1;
    }

    // The NAPI driver expects to return no more than budget
    return priv->rx_work_done < budget ? priv->rx_work_done : budget;
}

/****************************************************************************/
// vc3fdma_dma_interrupt()
// This is the only function in this module invoked in IRQ context.
// The FDMA interrupt is disabled, and only re-enabled once all Rx and Tx
// is handled (vc3fdma_poll()).
/****************************************************************************/
static irqreturn_t vc3fdma_dma_interrupt(int irq, void *dev_id)
{
    struct net_device      *dev  = dev_id;
    struct vc3fdma_private *priv = netdev_priv(dev);

    disable_irq_nosync(dev->irq);
    napi_schedule(&priv->napi);

    return IRQ_HANDLED;
}

/****************************************************************************/
// vc3fdma_ufdma_init()
/****************************************************************************/
static int vc3fdma_ufdma_init(struct net_device *dev)
{
    struct vc3fdma_private       *priv   = netdev_priv(dev);
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    vtss_ufdma_init_conf_t       init_conf;
    int                          rc;

    dev_info(&dev->dev, "Opening device\n");

    // Initialize uFDMA
    memset(&init_conf, 0, sizeof(init_conf));

    init_conf.rx_callback          = RX_callback;
    init_conf.tx_callback          = TX_callback;
    init_conf.cache_flush          = CX_cache_flush;
    init_conf.cache_invalidate     = CX_cache_invalidate;
    init_conf.virt_to_phys         = CX_virt_to_phys;
    init_conf.trace_printf         = CX_trace_printf;
    init_conf.trace_hex_dump       = CX_trace_hex_dump;
    init_conf.timestamp            = CX_timestamp;
    init_conf.reg_read             = CX_reg_read;
    init_conf.reg_write            = CX_reg_write;
    init_conf.rx_multi_dcb_support = RX_MULTI_DCB_SUPPORT;
#if defined(CONFIG_CPU_BIG_ENDIAN)
    init_conf.big_endian           = true;
#endif

    if ((driver->state = kmalloc(driver->props.ufdma_state_size_bytes, GFP_KERNEL)) == NULL) {
        T_E("Out of memory trying to allocate %u bytes", driver->props.ufdma_state_size_bytes);
        return -ENOMEM;
    }

    // Initialize uFDMA
    memset(driver->state, 0, driver->props.ufdma_state_size_bytes);
    if ((rc = driver->init(driver, &init_conf))) {
        // Don't call driver->error_txt() on init()-fail.
        T_E("uFDMA error: %d", rc);
        return -EIO;
    }

    // Add some Rx buffers to the uFDMA.
    return rx_buffers_refresh(RX_MTU_DEFAULT, RX_BUF_CNT_DEFAULT);
}

/****************************************************************************/
// vc3fdma_open()
/****************************************************************************/
static int vc3fdma_open(struct net_device *dev)
{
    struct vc3fdma_private *priv = netdev_priv(dev);
    int                    ret;

    if ((ret = vc3fdma_ufdma_init(dev))) {
        return ret;
    }

    // Initialize
    netif_start_queue(dev);
    if (dev->irq) {
        napi_enable(&priv->napi);
        /* Install the interrupt handler */
        ret = request_irq(dev->irq, vc3fdma_dma_interrupt, 0, DRV_NAME, dev);
        if (ret < 0) {
            T_E("Unable to get Rx DMA IRQ %d", dev->irq);
        }
    }

    return ret;
}

/****************************************************************************/
// vc3fdma_ufdma_uninit()
/****************************************************************************/
static void vc3fdma_ufdma_uninit(struct net_device *dev)
{
    struct vc3fdma_private       *priv   = netdev_priv(dev);
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;

    dev_info(&dev->dev, "Closing device\n");

    L();
    driver->uninit(driver);
    U();

    kfree(driver->state);
    kfree(priv->rx_data);
    priv->rx_data = NULL;

    driver = NULL; // Prevent rx_recycle() from recycling any Rx buffers after we've been uninitialized
}

/****************************************************************************/
// vc3fdma_close()
/****************************************************************************/
static int vc3fdma_close(struct net_device *dev)
{
    if (dev->irq) {
        struct vc3fdma_private *priv = netdev_priv(dev);
        disable_irq(dev->irq);
        napi_disable(&priv->napi);
        free_irq(dev->irq, dev);
    }

    vc3fdma_ufdma_uninit(dev);

    return 0;
}

/****************************************************************************/
// vc3fdma_send_packet()
/****************************************************************************/
static int vc3fdma_send_packet(struct sk_buff *skb, struct net_device *dev)
{
    struct vc3fdma_private       *priv = netdev_priv(dev);
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;
    vtss_ufdma_buf_dscr_t        tbd;
    int                          rc, pad, old_len;

    T_D("%s: Transmit %d bytes @ %px", dev->name, skb->len, skb->data);

    // Check for proper encapsulation
    if (skb->data[PROTO_B0]   != ifh_encap[PROTO_B0] ||
        skb->data[PROTO_B1]   != ifh_encap[PROTO_B1] ||
        skb->data[IFH_ID_OFF] != ifh_encap[IFH_ID_OFF]) {
        T_D("Wrong encapsulation - dropping %d bytes @ %px (%02x:%02x:%02x)", skb->len, skb->data, skb->data[PROTO_B0], skb->data[PROTO_B1], skb->data[IFH_ID_OFF]);
        dev->stats.tx_dropped++;
        kfree_skb(skb);
        return NETDEV_TX_OK;
    }

    // Transmit - first loose encap, leave SKB pointing at the IFH
    skb_pull_inline(skb, sizeof(ifh_encap));

    memset(&tbd, 0, sizeof(tbd));

    // skb->len contains the frame size including IFH, but excluding FCS.
    // The FDMA driver needs the size including the FCS. The FDMA driver itself
    // takes care of expanding the data it sends to H/W to a minimum-sized
    // Ethernet frame, so no need to check for that here.
    tbd.buf_size_bytes = skb->len + ETH_FCS_LEN;
    tbd.context = skb;

    // What we need to do, however, is to pad the frame with zeros if it's not
    // long enough. Since the FDMA driver itself expands the size to a minimum-
    // sized Ethernet frame, we must make sure to clear out any data it sends.

    // Generally: A frame with length w/o FCS < 60 must be padded to 60 bytes.
    // However, because the resulting frame may have its VLAN tag stripped by
    // the chip, we may also have to pad into the FCS, because that may - on
    // some chips - become payload when the VLAN tag is stripped.
    // Therefore, a frame with length w/o FCS < 64 bytes must be padded with
    // zeros up to 64 bytes. If no VLAN tag is auto-stripped by the chip, the
    // side-effect of this FCS padding is to simply write zeros to an FCS that
    // will be updated by the chip itself before Tx.
    pad = ETH_ZLEN /* 60 */ + 4 /* possible VLAN tag */ + driver->props.tx_ifh_size_bytes - skb->len;
    if (pad > 0) {
        old_len = skb->len;
        if (skb_tailroom(skb) < pad) {
            // Not room for immediate padding. Add 'pad' bytes of tailroom.
            if (pskb_expand_head(skb, 0, pad, GFP_ATOMIC)) {
                dev->stats.tx_dropped++;
                kfree_skb(skb);
                return NETDEV_TX_OK;
            }
        }

        skb_put(skb, pad);
        memset(skb->data + old_len, 0, pad);
    }

    // Cannot assign this until now, because pskb_expand_head() might have
    // changed skb->data.
    tbd.buf = skb->data;

    L();
    if ((tbd.buf_state = kmalloc(driver->props.buf_state_size_bytes, GFP_ATOMIC)) != NULL) {
        // Start Tx
        if ((rc = driver->tx(driver, &tbd))) {
            T_E("uFDMA transmit error: %s", driver->error_txt(driver, rc));
            dev->stats.tx_dropped++;
            priv->netif_tx_drop_cnt++;
            kfree(tbd.buf_state);
            kfree_skb(skb);
        } else {
            dev->stats.tx_packets++;
            priv->netif_tx_cnt++;
            dev->stats.tx_bytes += skb->len;
        }
    } else {
        net_err_ratelimited("%s: Tx: Unable to allocate %u bytes descriptor\n", dev->name, driver->props.buf_state_size_bytes);
        dev->stats.tx_dropped++;
        kfree_skb(skb);
    }

    U();

    return NETDEV_TX_OK;
}

/****************************************************************************/
// vc3fdma_change_mtu()
// Only affects Tx. See rx_cfg for Rx MTU.
/****************************************************************************/
static int vc3fdma_change_mtu(struct net_device *dev, int new_mtu)
{
    struct vc3fdma_private       *priv   = netdev_priv(dev);
    vtss_ufdma_platform_driver_t *driver = priv->chip->driver;

    if (new_mtu < (68 + ETH_FCS_LEN + driver->props.tx_ifh_size_bytes) || new_mtu > IF_BUFSIZE_JUMBO) {
        return -EINVAL;
    }

    dev->mtu = new_mtu;
    return 0;
}

/****************************************************************************/
/****************************************************************************/
static const struct net_device_ops vc3fdma_netdev_ops = {
    .ndo_open            = vc3fdma_open,
    .ndo_stop            = vc3fdma_close,
    .ndo_start_xmit      = vc3fdma_send_packet,
    .ndo_change_mtu      = vc3fdma_change_mtu,
    .ndo_validate_addr   = eth_validate_addr,
    .ndo_set_mac_address = eth_mac_addr,
};

/****************************************************************************/
// show_ufdma()
/****************************************************************************/
static int show_ufdma(struct seq_file *m, void *v)
{
    struct vc3fdma_private       *priv = vc3fdma_inst;
    vtss_ufdma_platform_driver_t *driver;
    vtss_ufdma_debug_info_t      info;

    if (priv && priv->chip) {
        driver = priv->chip->driver;
    } else {
        driver = NULL;
    }

    memset(&info, 0, sizeof(info));
    info.layer = VTSS_UFDMA_DEBUG_LAYER_ALL;
    info.group = VTSS_UFDMA_DEBUG_GROUP_ALL;
    info.full = 0;
    info.ref = m;

    seq_printf(m, "Driver: " DRV_NAME "-" DRV_VERSION " " DRV_RELDATE "\n\n");

    seq_printf(m, "uFDMA driver:\n=============\n\n");
    if (driver) {
        seq_printf(m, "TX IFH size      : %4d bytes\n", driver->props.tx_ifh_size_bytes);
        seq_printf(m, "RX IFH size      : %4d bytes\n", driver->props.rx_ifh_size_bytes);
        seq_printf(m, "Buffer state size: %4d bytes\n", driver->props.buf_state_size_bytes);
        seq_printf(m, "State size       : %4d bytes\n", driver->props.ufdma_state_size_bytes);
    } else {
        seq_printf(m, "No uFDMA driver\n\n");
    }
    seq_printf(m, "\n");

    seq_printf(m, "uFDMA state:\n============\n\n");
    if (driver && priv->netdev->flags & IFF_UP) {
        driver->debug_print(driver, &info, (void *)seq_printf);
        seq_printf(m, "Network Driver state:\n=====================\n\n");
        seq_printf(m, "IRQ ena count made by us:   %10u\n", priv->irq_ena_self_cnt);
        seq_printf(m, "IRQ ena count made by NAPI: %10u\n", priv->irq_ena_napi_cnt);
        seq_printf(m, "Max. Rx count per poll:     %10u\n", priv->rx_work_done_max);
        seq_printf(m, "Net I/F Rx count:           %10u\n", priv->netif_rx_cnt);
        seq_printf(m, "Net I/F Tx count:           %10u\n", priv->netif_tx_cnt);
        seq_printf(m, "Net I/F Tx drop count:      %10u\n", priv->netif_tx_drop_cnt);
        seq_printf(m, "Last IRQ ena %s\n", priv->irq_ena_last_was_napi ? "supposed to be made by NAPI": "made by us");
        seq_printf(m, "Zero-copy chardev open: %s\n", priv->zc.dev_open ? "Yes" : "No");
        seq_printf(m, "Zero-copy chardev memory-mapped: %s\n", priv->zc.mmapped ? "Yes" : "No");
        seq_printf(m, "Zero-copy list in kernel-space: %px\n", priv->rx_user_frm_list);
        seq_printf(m, "Zero-copy list in user-space:   %px\n", priv->zc.user_space_data_start);
        seq_printf(m, "Rx MTU: %u\n", priv->rx_cfg.mtu);
        seq_printf(m, "Rx buffers owned by FDMA: %u\n", priv->rx_bufs_owned_by_fdma);
        seq_printf(m, "Rx buffers owned by appl: %u\n", priv->rx_bufs_owned_by_appl);
    } else {
        seq_printf(m, "Driver is inactive (interface down)\n");
    }

    return 0;
}

/****************************************************************************/
// ufdma_open()
/****************************************************************************/
static int ufdma_open(struct inode *inode, struct file *file)
{
    return single_open(file, show_ufdma, NULL);
}

/****************************************************************************/
/****************************************************************************/
static const struct proc_ops ufdma_fops = {
    .proc_open       = ufdma_open,
    .proc_read       = seq_read,
    .proc_lseek      = seq_lseek,
    .proc_release    = single_release,
};

/****************************************************************************/
// vc3fdma_create()
/****************************************************************************/
static struct net_device *vc3fdma_create(struct platform_device *pdev)
{
    struct net_device      *dev;
    struct vc3fdma_private *priv;
    struct resource        *res;

    if ((dev = alloc_etherdev(sizeof(struct vc3fdma_private))) == NULL) {
        return NULL;
    }

    dev->netdev_ops = &vc3fdma_netdev_ops;
    priv = netdev_priv(dev);
    memset(priv, 0, sizeof(*priv));
    priv->netdev = dev; // Backlink
    priv->chip = device_get_match_data(&pdev->dev);
    vc3fdma_inst = priv; // Static pointer, ugly!
    ifh_encap[IFH_ID_OFF] = priv->chip->ifh_id; // Shared static data - ugly

    // This particular device adds no MAC header - must be part of data
    dev->hard_header_len = dev->min_header_len = 0;

    // Set initial, bogus MAC address
    eth_hw_addr_random(dev);
    memset(&dev->broadcast[0], 0xff, 6);

    // Memory regions
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        dev_err(&pdev->dev, "Unable to map origin1\n");
        free_netdev(dev);
        return NULL;
    }

    priv->map_origin1 = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    priv->origin1 = *res;
    res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    if (!res) {
        dev_err(&pdev->dev, "Unable to map origin2\n");
        free_netdev(dev);
        return NULL;
    }

    priv->map_origin2 = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    priv->origin2 = *res;
    dev_dbg(&pdev->dev, "Mapped switch registers 0x%08x 0x%08x\n", (u32)priv->map_origin1, (u32)priv->map_origin2);

    // Set arbitrarily high for direct injection (not rx)
    dev->mtu = IF_BUFSIZE_JUMBO;
    priv->rx_cfg.mtu = RX_MTU_DEFAULT;

    priv->proc_fs_dump_file = proc_create(DRV_NAME, S_IRUGO,  NULL, &ufdma_fops);

    spin_lock_init(&priv->lock);

    return dev;
}

/****************************************************************************/
// zc_open()
/****************************************************************************/
static int zc_open(struct inode *inode, struct file *filp)
{
    struct vc3fdma_private *priv = vc3fdma_inst;

    if (!priv) {
        return -EINVAL;
    }

    if (RX_MULTI_DCB_SUPPORT) {
        T_E("Zero-copy with Rx multi-DCB support is not working");
        return -EINVAL;
    }

    if (priv->zc.dev_open) {
        return -EBUSY;
    }

    // Default members.
    priv->zc.dev_open   = true;
    priv->zc.mmapped    = false;

    // Until told otherwise (with a IOCTL), inherit the buffer count and size
    // from the normal non-zerocopy driver.
    priv->zc.rx_mtu     = priv->rx_mtu_cur;
    priv->zc.rx_buf_cnt = priv->rx_buf_cnt_cur;

    return 0;
}

/****************************************************************************/
// zc_close()
/****************************************************************************/
static int zc_close(struct inode *inode, struct file *file)
{
    struct vc3fdma_private *priv = vc3fdma_inst;

    priv->zc.dev_open = false;
    priv->zc.mmapped  = false; // RBNTBD: Anything to do if mmapped before we close?

    return rx_buffers_refresh(RX_MTU_DEFAULT, RX_BUF_CNT_DEFAULT);
}

#define ZC_IOCTL_MAGIC            'Z'
#define ZC_IOCTL_RX_BUF_CNT_GET   _IOR(ZC_IOCTL_MAGIC, 0xA0, u32)
#define ZC_IOCTL_RX_BUF_CNT_SET   _IOW(ZC_IOCTL_MAGIC, 0xA1, u32)
#define ZC_IOCTL_RX_MTU_GET       _IOR(ZC_IOCTL_MAGIC, 0xA2, u32)
#define ZC_IOCTL_RX_MTU_SET       _IOW(ZC_IOCTL_MAGIC, 0xA3, u32)
#define ZC_IOCTL_RX_MMAP_SIZE_GET _IOR(ZC_IOCTL_MAGIC, 0xA4, u32)

/****************************************************************************/
// zc_ioctl()
/****************************************************************************/
static long zc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int                    rc = 0;
    u32 __user             *p32;
    u32                    tmp;
    struct vc3fdma_private *priv = vc3fdma_inst;

    if (_IOC_TYPE(cmd) != ZC_IOCTL_MAGIC) {
        return -ENOTTY;
    }

    // Check if the user data is read/writeable
    if (_IOC_DIR(cmd) & _IOC_READ) {
        rc = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
    } else if (_IOC_DIR(cmd) & _IOC_WRITE) {
        rc = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
    }

    if (rc) {
        return -EFAULT;
    }

    p32 = (u32 __user *)arg;

    switch (cmd) {
    case ZC_IOCTL_RX_BUF_CNT_GET:
        rc = __put_user(priv->zc.rx_buf_cnt, p32);
        break;

    case ZC_IOCTL_RX_BUF_CNT_SET:
         // We cannot be memory mapped while changing the number of Rx buffers.
        if (priv->zc.mmapped) {
            return -EBUSY;
        }

        rc = __get_user(tmp, p32);

        if (rc == 0) {
            if (tmp == 0) {
                return -EINVAL;
            }

            priv->zc.rx_buf_cnt = tmp;
        }

        break;

    case ZC_IOCTL_RX_MTU_GET:
        rc = __put_user(priv->zc.rx_mtu, p32);
        break;

    case ZC_IOCTL_RX_MTU_SET:
         // We cannot be memory mapped while changing the number of Rx buffers.
        if (priv->zc.mmapped) {
            return -EBUSY;
        }

        rc = __get_user(tmp, p32);

        if (rc == 0) {
            if (tmp < 60 + 2 * ETH_VLAN_TAGSZ + ETH_FCS_LEN || tmp > IF_BUFSIZE_JUMBO) {
                return -EINVAL;
            }

            priv->zc.rx_mtu = tmp;
        }

        break;

    case ZC_IOCTL_RX_MMAP_SIZE_GET:
        // Based on what we currently have been set up for in rx_mtu and rx_buf_cnt,
        // compute the total number of bytes that we intend to allocate once mmap()
        // gets called. This is in order to provide the application with a size
        // argument in its call to mmap().
        // We also need to take into account, that on MIPS, we have cache aliases.
        // This means that in order for the virtual address at user space to hit
        // the same cache lines of kernel space, the lower 15 bits of the two
        // spaces' virtual addresses must be equal. In order to achieve this, we
        // ask user space to map 2^15 + whatever is actually needed, and adjust the
        // actual size and the - by the kernel- suggested user address in zc_mmap().
        if ((tmp = rx_data_size_total_get(priv->zc.rx_mtu, priv->zc.rx_buf_cnt)) == 0) {
            return -EINVAL;
        }

        rc = __put_user(tmp + 32768, p32);
        break;

    default:
        return -ENOTTY;
    }

    return rc;
}

/****************************************************************************/
// zc_mmap()
/****************************************************************************/
static int zc_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct vc3fdma_private *priv = vc3fdma_inst;
    u32                    expected_size;
    unsigned long          requested_size = vma->vm_end - vma->vm_start, orig_vm_start = vma->vm_start, ua, ka_15lsbits;
    int                    rc;

    if (priv->zc.mmapped) {
        // Already mapped.
        return -EBUSY;
    }

    if (vma->vm_pgoff != 0) {
        // Only map to offset 0.
        return -EINVAL;
    }

    if ((expected_size = rx_data_size_total_get(priv->zc.rx_mtu, priv->zc.rx_buf_cnt)) == 0) {
        T_E("Huh?");
        return -EINVAL;
    }

    // See comment in zc_ioctl() above as to why we add 32 KBytes.
    if (requested_size != expected_size + 32768) {
        return -EINVAL;
    }

    // To avoid cache alias, run through the user space addresses until
    // the 15 LSbits are equal to priv->rx_data's 15 LSbits.
    ka_15lsbits = (unsigned long)priv->rx_data & 0x7FFF;
    for (ua = vma->vm_start;; ua++) {
        if ((ua & 0x7FFF) == ka_15lsbits) {
            break;
        }
    }

    if (ua + expected_size > vma->vm_end) {
        T_E("Internal error: vm_start = %px, vm_end = %px, rx_data = %px, size = %u", (void *)vma->vm_start, (void *)vma->vm_end, priv->rx_data, expected_size);
        return -EINVAL;
    }

    // Correct vma
    vma->vm_start = ua;
    vma->vm_end   = ua + expected_size;

    // PFN = Page Frame Number
    if ((rc = remap_pfn_range(vma, vma->vm_start, (unsigned long)priv->rx_data >> PAGE_SHIFT, expected_size, vma->vm_page_prot)) < 0) {
        return rc;
    }

    zc_circ_buf_init(&priv->zc.rx_user_frms, priv->rx_user_frm_list, priv->rx_buf_cnt_cur);
    zc_circ_buf_init(&priv->zc.rx_krnl_frms, priv->rx_krnl_frm_list, priv->rx_buf_cnt_cur);

    init_waitqueue_head(&priv->zc.rx_wait_queue);
    priv->zc.user_space_data_start = (u8 *)vma->vm_start;

    // When setting this to true (atomically), RX_callback() may start filling
    // the zero-copy lists.
    priv->zc.mmapped = true;

    T_I("Mapped priv->rx_data = %px to vma->vm_start = %px (original vma->vm_start = %px), size = %u (requested size = %lu)", priv->rx_data, (void *)vma->vm_start, (void *)orig_vm_start, expected_size, requested_size);

    return 0;
}

/****************************************************************************/
// zc_poll()
/****************************************************************************/
static unsigned int zc_poll(struct file *filp, struct poll_table_struct *pts)
{
    struct vc3fdma_private *priv = vc3fdma_inst;
    struct zc_frm_dscr     *item;
    unsigned int           mask = 0;

    if (!priv->zc.mmapped) {
        return POLLERR;
    }

    // Either these two:
    CX_cache_flush(priv->zc.user_space_data_start, priv->rx_size_of_one_list);
    CX_cache_invalidate(priv->rx_user_frm_list, priv->rx_size_of_one_list);

    // or this:
    // CX_cache_flush(priv->rx_user_frm_list, priv->rx_size_of_one_list);

    L();

    // First see if the user-space process has consumed any of its frames
    // We can't rely on the zc_circ_buf_empty() function, because user
    // space cannot change the properties (like 'count') of the circular buffer,
    // only the items held inside the buffer.
    if ((item = zc_circ_buf_peek(&priv->zc.rx_user_frms)) != NULL && item->frm_ptr != NULL) {
        // Nope. Still frames to go.
        mask = POLLIN | POLLRDNORM;
        goto do_exit;
    }

    // Then go through all frames that were previously handed to user-space
    // and see if it has processed them (written NULL into the individual entries).
    // There is a one-to-one correspondence between priv->rx_user_frm_list[0]
    // and priv->zc.rx_krnl_frms[priv->zc.rx_krnl_frms.tail], which is the very first
    // frame handed to user space upon previous invokation of poll.
    while (!zc_circ_buf_empty(&priv->zc.rx_user_frms)) {
        if (zc_circ_buf_get(&priv->zc.rx_user_frms)->frm_ptr) {
            // User space has not yet consumed this frame, so we
            // break and (re-)add whatever is left.
            // It doesn't matter that we leave the user-space
            // circular buffer in a limbo state here, because
            // we re-initialize it in just a second.
            break;
        }

        // User has written NULL to this entry, so it is consumed.

        // Update our kernel lists while freeing (recycling) the frame data.
        if ((item = zc_circ_buf_get(&priv->zc.rx_krnl_frms)) != NULL) {
            // item->head_ptr corresponds to skb->head, so that we can
            // obtain the shared info, and decrement its reference count,
            // and free it if reaches 0.
            // The shared info is priv->rx_frame_part_size away from the beginning
            // of the data area for this frame.
            struct skb_shared_info *shinfo = (struct skb_shared_info *)(item->head_ptr + priv->rx_frame_part_size);
            if (atomic_dec_return(&shinfo->dataref) == 0) {
                // Free it if the free-ptr matches rx_recycle (it could be rx_no_recycle().
                if (shinfo->free == rx_recycle) {
                    (void)rx_buffer_add_to_ufdma(item->head_ptr, GFP_KERNEL /* not in interrupt context */);
                    priv->rx_bufs_owned_by_appl--;
                }
            }
        } else {
            T_E("Mismatch between kernel and user space circular buffers");
            // Don't break out of here.
        }
    }

    // Now build up a new list for user space. Since user space always looks
    // at the same address for new pointers, we gotta re-initialize the
    // circular buffer that represents that address. Remember that the user-space
    // list is just a list, and not circular.
    zc_circ_buf_init(&priv->zc.rx_user_frms, priv->rx_user_frm_list, priv->rx_buf_cnt_cur);

    // Iterate through the current list of received - yet unprocessed - kernel frames and add them to the user-space buffer.
    if (zc_circ_buf_copy(&priv->zc.rx_user_frms, &priv->zc.rx_krnl_frms, priv->zc.user_space_data_start, priv->rx_data)) {
        mask = POLLIN | POLLRDNORM;
    }

do_exit:
    CX_cache_flush(priv->rx_user_frm_list, priv->rx_size_of_one_list);

    // Perhaps also this
    CX_cache_invalidate(priv->zc.user_space_data_start, priv->rx_size_of_one_list);

    if (!mask) {
        // Install a wait queue on this poll (method should have been called something like "add_filp_as_listener_to_events_on_rx_wait_queue()".
        poll_wait(filp, &priv->zc.rx_wait_queue, pts);
    } else {
#ifdef DEBUG
        print_hex_dump(KERN_ERR, "poll ", DUMP_PREFIX_ADDRESS, 16, 1, priv->rx_user_frm_list, 128, true);
#endif
    }

    U();

    return mask;
}

/****************************************************************************/
// zc_error()
/****************************************************************************/
static void zc_error(const char *str, int rc)
{
    T_E("%s: %d. %s character device will not be available", str, rc, ZC_CDEV_NAME);
}

/****************************************************************************/
// zc_destroy()
/****************************************************************************/
static void zc_destroy(void)
{
    struct vc3fdma_private *priv = vc3fdma_inst;

    if (priv->zc.cdev.owner == THIS_MODULE) {
        cdev_del(&priv->zc.cdev);
    }

    if (priv->zc.device) {
        device_destroy(priv->zc.class, priv->zc.major_minor);
    }

    if (priv->zc.class) {
        class_destroy(priv->zc.class);
    }

    unregister_chrdev_region(priv->zc.major_minor, 1);

    memset(&priv->zc, 0, sizeof(priv->zc));
}

/****************************************************************************/
// zc_create()
/****************************************************************************/
static void zc_create(struct vc3fdma_private *priv)
{
    int rc;

    // Create the zero-copy character device
    memset(&priv->zc, 0, sizeof(priv->zc));

    // Set up the file operations
    priv->zc.fops.owner          = THIS_MODULE;
    priv->zc.fops.unlocked_ioctl = zc_ioctl;
    priv->zc.fops.mmap           = zc_mmap;
    priv->zc.fops.poll           = zc_poll;
    priv->zc.fops.open           = zc_open;
    priv->zc.fops.release        = zc_close;

    // Get a major device number.
    if ((rc = alloc_chrdev_region(&priv->zc.major_minor, 0, 1, ZC_CDEV_NAME)) < 0) {
        // Even though the cdev will not be available, we can still be used
        // through the normal socket interface.
        zc_error("alloc_chrdev_region()", rc);
        return;
    }

    if (IS_ERR(priv->zc.class = class_create(THIS_MODULE, "vc3fdma_zc_class"))) {
        zc_error("class_create()", -1);
        goto do_exit;
    }

    if (IS_ERR(priv->zc.device = device_create(priv->zc.class, NULL, priv->zc.major_minor, NULL, ZC_CDEV_NAME))) {
        zc_error("device_create()", -1);
        goto do_exit;
    }

    // Initialize the cdev structure
    cdev_init(&priv->zc.cdev, &priv->zc.fops);
    priv->zc.cdev.owner = THIS_MODULE;

    // And add it
    if ((rc = cdev_add(&priv->zc.cdev, priv->zc.major_minor, 1)) < 0) {
        zc_error("cdev_add()", rc);
        priv->zc.cdev.owner = NULL; // Prevent it from being cdev_del()eted in zc_destroy()
        goto do_exit;
    }

    return;

do_exit:
    zc_destroy();
}

/****************************************************************************/
// vc3fdma_restart()
// Invoked when we are about to reboot.
/****************************************************************************/
static int vc3fdma_restart(struct notifier_block *nb, unsigned long action, void *data)
{
    struct vc3fdma_private       *priv = container_of(nb, struct vc3fdma_private, restart_nb);
    vtss_ufdma_platform_driver_t *driver;

    if (priv && priv->chip && priv->chip->driver) {
        driver = priv->chip->driver;
    } else {
        return NOTIFY_DONE;
    }

    L();

    T_I("Uninitializing driver due to soon reboot");

    if (driver && driver->uninit) {
        driver->uninit(driver);
    }

    // Don't unlock, because that may cause a pending interrupt to come through.
    // U();

    return NOTIFY_DONE;
}

/****************************************************************************/
// restart_handler_install()
/****************************************************************************/
static void restart_handler_install(struct vc3fdma_private *priv)
{
    int rc;

    /* Use high priority to ensure getting called. Platform reset
     * driver (ocelot-reset) runs at priority 192.
     */
    priv->restart_nb.notifier_call = vc3fdma_restart;
    priv->restart_nb.priority      = 204;
    if ((rc = register_restart_handler(&priv->restart_nb))) {
        T_E("Unable to register a restart handler: %d", rc);
        // Keep going
    }
}

/****************************************************************************/
// restart_handler_uninstall()
/****************************************************************************/
static void restart_handler_uninstall(struct vc3fdma_private *priv)
{
    int rc;
    if ((rc = unregister_restart_handler(&priv->restart_nb))) {
        T_E("Unable to unregister a restart handler: %d", rc);
    }
}

/****************************************************************************/
// vc3fdma_probe()
/****************************************************************************/
static int vc3fdma_probe(struct platform_device *pdev)
{
    struct net_device      *dev;
    struct vc3fdma_private *priv;
    int                    rc;

    if ((dev = vc3fdma_create(pdev)) == NULL) {
        return -ENOMEM;
    }

    // We're special aka. "vtss.ifh"
    strcpy(dev->name, "vtss.ifh");

    // Hook the devices together netdev <-> pdev
    SET_NETDEV_DEV(dev, &pdev->dev);
    platform_set_drvdata(pdev, dev);

    if ((dev->irq = platform_get_irq(pdev, 0)) <= 0) {
        dev_warn(&dev->dev, "interrupt resource missing\n");
        free_netdev(dev);
        return -ENXIO;
    }

    if ((rc = register_netdev(dev)) < 0) {
        T_E("Cannot register net device: %d", rc);
        free_netdev(dev);
        return rc;
    }

    packet_generic_netlink_init();

    // Turn on device handling
    priv = netdev_priv(dev);
    netif_napi_add_weight(dev, &priv->napi, vc3fdma_poll, NAPI_BUDGET);

    // Create the zc character device.
    // If the creation function fails, we should *not* return
    // an error code from this function, because this driver
    // would still work, but without zero-copy functionality.
    zc_create(priv);

    // Register a restart handler, to allow us to gracefully shut down prior to
    // a - potentially - two-step reboot, where the switch core is reset first
    // and the CPU - or perhaps the CPU system - is reset next.
    restart_handler_install(priv);

    printk(KERN_INFO "%s: " DRV_NAME "-" DRV_VERSION " " DRV_RELDATE "\n", dev->name);

    return 0;
}

/****************************************************************************/
// vc3fdma_remove()
/****************************************************************************/
static int vc3fdma_remove(struct platform_device *pdev)
{
    struct net_device      *dev  = platform_get_drvdata(pdev);
    struct vc3fdma_private *priv = netdev_priv(dev);

    // Unregister the restart handler.
    restart_handler_uninstall(priv);

    if (priv->proc_fs_dump_file) {
        proc_remove(priv->proc_fs_dump_file);
    }

    // Remove zero-copy character device.
    zc_destroy();

    packet_generic_netlink_uninit();

    unregister_netdev(dev);
    free_netdev(dev);
    vc3fdma_inst = NULL;

    return 0;
}

/****************************************************************************/
/****************************************************************************/

static const struct fdma_chip luton_chip = {
        .driver = &vtss_ufdma_platform_driver_luton26,
        .ifh_id = IFH_ID_LUTON,
};

static const struct fdma_chip serval_chip = {
        .driver =  &vtss_ufdma_platform_driver_serval,
        .ifh_id = IFH_ID_SERVAL1,
};

static const struct fdma_chip ocelot_chip = {
        .driver =  &vtss_ufdma_platform_driver_ocelot,
        .ifh_id = IFH_ID_OCELOT,
};

static const struct fdma_chip servalt_chip = {
        .driver =  &vtss_ufdma_platform_driver_servalt,
        .ifh_id = IFH_ID_SERVALT,
};

static const struct fdma_chip jaguar2_chip = {
        .driver =  &vtss_ufdma_platform_driver_jaguar2c,
        .ifh_id = IFH_ID_JAGUAR2,
};

static const struct of_device_id mscc_fdma_id_table[] = {
    {
        .compatible = "mscc,luton-fdma",
        .data = &luton_chip,
    },
    {
        .compatible = "mscc,serval-fdma",
        .data = &serval_chip,
    },
    {
        .compatible = "mscc,ocelot-fdma",
        .data = &ocelot_chip,
    },
    {
        .compatible = "mscc,servalt-fdma",
        .data = &servalt_chip,
    },
    {
        .compatible = "mscc,jaguar2-fdma",
        .data = &jaguar2_chip,
    },
    {}
};
MODULE_DEVICE_TABLE(of, mscc_fdma_id_table);

static struct platform_driver vc3fdma_driver = {
    .remove = vc3fdma_remove,
    .driver = {
        .name = DRV_NAME,
        .of_match_table = mscc_fdma_id_table,
    },
};

module_platform_driver_probe(vc3fdma_driver, vc3fdma_probe);

MODULE_AUTHOR("Lars Povlsen <lpovlsen@vitesse.com>");
MODULE_DESCRIPTION("VCore-III FDMA ethernet interface driver");
MODULE_LICENSE("GPL");

module_param(do_debug, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(do_debug, "Enable debug trace");
