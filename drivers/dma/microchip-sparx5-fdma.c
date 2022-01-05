// SPDX-License-Identifier: (GPL-2.0 OR MIT)
//
// Microsemi SoC Sparx5 FDMA driver
//
// Copyright (c) 2019 Microsemi

#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <asm/cacheflush.h>

#include "dmaengine.h"

#define DEVICE_NAME "sparx5-fdma"

enum sparx5_top {
    SPARX5_DEFAULT   = 0x000000000, /* Default instance in CML model */
    SPARX5_AMBA_TOP  = 0x010000000, /* amba_top instance in CML model */
};

#define SPARX5_REGIONS                            2
#define GET_REGION(off)                            ((off) & GENMASK(31, 28))
#define GET_REGION_INDEX(off)                      ((off) >> 28)
#define GET_ADDRESS(off)                           ((off) & GENMASK(27, 0))

#define AMBA_TOP_SPACE                             0x600000000
#define VCORE_CSR_SPACE                            0x610000000
#define PCIE_TARGET_OFFSET                         0x900000000
#define FDMA_ATU_TARGET_SPACE                      0x0

/* Register offsets */

/* ASM:CFG:PORT_CFG[67] */
#define ASM_CFG_PORT_R_OFF(ridx)                   (0x0060841c + (ridx*4))

/* CPU:CPU_REGS:GPR[32] */
#define CPU_REGS_GPR_R_OFF(ridx)                   (0x10000000 + (ridx*4))
/* CPU:CPU_REGS:PROC_CTRL */
#define CPU_REGS_PROC_CTRL_OFF                     0x100000b0

/* DEVCPU_GCB:CHIP_REGS:CHIP_ID */
#define DEVCPU_GCB_CHIP_REGS_ID_OFF                0x01010000
/* DEVCPU_GCB:VCORE_ACCESS:VA_CTRL */
#define DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF        0x010101c8
/* DEVCPU_GCB:VCORE_ACCESS:VA_ADDR_LSB */
#define DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_LSB_OFF    0x010101cc
/* DEVCPU_GCB:VCORE_ACCESS:VA_ADDR_MSB */
#define DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_MSB_OFF    0x010101d0
/* DEVCPU_GCB:VCORE_ACCESS:VA_DATA */
#define DEVCPU_GCB_VCORE_ACCESS_VA_DATA_OFF        0x010101d4
/* DEVCPU_GCB:VCORE_ACCESS:VA_DATA_INERT */
#define DEVCPU_GCB_VCORE_ACCESS_VA_DATA_INERT_OFF  0x010101dc

/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_R_OFF(ridx)          (0x01030000 + (ridx*4))
/* DEVCPU_QS:INJ:INJ_GRP_CFG[2] */
#define DEVCPU_QS_INJ_GRP_CFG_R_OFF(ridx)          (0x01030024 + (ridx*4))
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_R_OFF(ridx)             (0x01030034 + (ridx*4))

/* DSM:CFG:DEV_TX_STOP_WM_CFG[67] */
#define DSM_CFG_DEV_TX_STOP_WM_R_OFF(ridx)         (0x00504564 + (ridx*4))
/* DSM:CFG:BUF_CFG[67] */
#define DSM_CFG_BUF_R_OFF(ridx)                    (0x00504014 + (ridx*4))

/* REW:COMMON:IFH_CTRL_CPUVD */
#define REW_COMMON_IFH_CTRL_CPUVD_OFF              0x0165e9d4

/* QFWD:SYSTEM:FRAME_COPY_CFG[12] */
#define QFWD_SYSTEM_FRAME_COPY_CFG_R_OFF(ridx)     (0x010b011c + (ridx*4))
#define QFWD_SYSTEM_FRAME_COPY_CFG_MAX             12

/* FDMA:FDMA:FDMA_CH_ACTIVATE */
#define FDMA_CH_ACTIVATE_OFF                       0x10080008
/* FDMA:FDMA:FDMA_CH_RELOAD */
#define FDMA_CH_RELOAD_OFF                         0x1008000c
/* FDMA:FDMA:FDMA_CH_DISABLE */
#define FDMA_CH_DISABLE_OFF                        0x10080010
/* FDMA:FDMA:FDMA_CH_FORCEDIS */
#define FDMA_CH_FORCEDIS_OFF                       0x10080014
/* FDMA:FDMA:FDMA_CH_DB_DISCARD */
#define FDMA_CH_DB_DISCARD_OFF                     0x10080018
/* FDMA:FDMA:FDMA_CH_CNT[8] */
#define FDMA_CH_CNT_R_OFF(ridx)                    (0x1008001c + (ridx*4))
/* FDMA:FDMA:FDMA_DCB_LLP[8] */
#define FDMA_DCB_LLP_R_OFF(ridx)                   (0x1008003c + (ridx*4))
/* FDMA:FDMA:FDMA_DCB_LLP1[8] */
#define FDMA_DCB_LLP1_R_OFF(ridx)                  (0x1008005c + (ridx*4))
/* FDMA:FDMA:FDMA_DCB_LLP_PREV[8] */
#define FDMA_DCB_LLP_PREV_R_OFF(ridx)              (0x1008007c + (ridx*4))
/* FDMA:FDMA:FDMA_DCB_LLP_PREV1[8] */
#define FDMA_DCB_LLP_PREV1_R_OFF(ridx)             (0x1008009c + (ridx*4))
/* FDMA:FDMA:FDMA_CH_ACTIVE */
#define FDMA_CH_ACTIVE_OFF                         0x100800bc
/* FDMA:FDMA:FDMA_CH_PENDING */
#define FDMA_CH_PENDING_OFF                        0x100800c0
/* FDMA:FDMA:FDMA_CH_IDLE */
#define FDMA_CH_IDLE_OFF                           0x100800c4
/* FDMA:FDMA:FDMA_CH_STATUS[8] */
#define FDMA_CH_STATUS_R_OFF(ridx)                 (0x100800c8 + (ridx*4))
/* FDMA:FDMA:FDMA_CH_CFG[8] */
#define FDMA_CH_CFG_R_OFF(ridx)                    (0x100800e8 + (ridx*4))
/* FDMA:FDMA:FDMA_CH_TRANSLATE[8] */
#define FDMA_CH_TRANSLATE_R_OFF(ridx)              (0x10080108 + (ridx*4))
/* FDMA:FDMA:FDMA_CH_INJ_TOKEN_CNT[6] */
#define FDMA_CH_INJ_TOKEN_CNT_R_OFF(ridx)          (0x10080128 + (ridx*4))
/* FDMA:FDMA:FDMA_CH_INJ_TOKEN_TICK_RLD[6] */
#define FDMA_CH_INJ_TOKEN_TICK_RLD_R_OFF(ridx)     (0x10080140 + (ridx*4))
/* FDMA:FDMA:FDMA_CH_INJ_TOKEN_TICK_CNT[6] */
#define FDMA_CH_INJ_TOKEN_TICK_CNT_R_OFF(ridx)     (0x10080158 + (ridx*4))
/* FDMA:FDMA:FDMA_INJ_CFG */
#define FDMA_INJ_CFG_OFF                           0x10080170
/* FDMA:FDMA:FDMA_XTR_CFG */
#define FDMA_XTR_CFG_OFF                           0x10080174
/* FDMA:FDMA:FDMA_PORT_CFG[2] */
#define FDMA_PORT_CFG_R_OFF(ridx)                  (0x10080178 + (ridx*4))
/* FDMA:FDMA:FDMA_PORT_CTRL[2] */
#define FDMA_PORT_CTRL_R_OFF(ridx)                 (0x10080180 + (ridx*4))
/* FDMA:FDMA:FDMA_INTR_DCB */
#define FDMA_INTR_DCB_OFF                          0x10080188
/* FDMA:FDMA:FDMA_INTR_DCB_ENA */
#define FDMA_INTR_DCB_ENA_OFF                      0x1008018c
/* FDMA:FDMA:FDMA_INTR_DB */
#define FDMA_INTR_DB_OFF                           0x10080190
/* FDMA:FDMA:FDMA_INTR_DB_ENA */
#define FDMA_INTR_DB_ENA_OFF                       0x10080194
/* FDMA:FDMA:FDMA_INTR_ERR */
#define FDMA_INTR_ERR_OFF                          0x10080198
/* FDMA:FDMA:FDMA_INTR_ENA */
#define FDMA_INTR_ENA_OFF                          0x1008019c
/* FDMA:FDMA:FDMA_INTR_IDENT */
#define FDMA_INTR_IDENT_OFF                        0x100801a0
/* FDMA:FDMA:FDMA_ERRORS */
#define FDMA_ERRORS_OFF                            0x100801a4
/* FDMA:FDMA:FDMA_ERRORS_2 */
#define FDMA_ERRORS_2_OFF                          0x100801a8
/* FDMA:FDMA:FDMA_IDLECNT */
#define FDMA_IDLECNT_OFF                           0x100801ac
/* FDMA:FDMA:FDMA_CTRL */
#define FDMA_CTRL_OFF                              0x100801b0

/* PCIE_DM_EP:PF0_ATU_CAP:IATU_REGION_CTRL_2_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_REGION_CTRL_2_OFF_OUTBOUND_0_OFF          0x10700004
/* PCIE_DM_EP:PF0_ATU_CAP:IATU_LWR_BASE_ADDR_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_LWR_BASE_ADDR_OFF_OUTBOUND_0_OFF          0x10700008
/* PCIE_DM_EP:PF0_ATU_CAP:IATU_UPPER_BASE_ADDR_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_UPPER_BASE_ADDR_OFF_OUTBOUND_0_OFF        0x1070000c
/* PCIE_DM_EP:PF0_ATU_CAP:IATU_LIMIT_ADDR_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_LIMIT_ADDR_OFF_OUTBOUND_0_OFF             0x10700010
/* PCIE_DM_EP:PF0_ATU_CAP:IATU_LWR_TARGET_ADDR_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_LWR_TARGET_ADDR_OFF_OUTBOUND_0_OFF        0x10700014
/* PCIE_DM_EP:PF0_ATU_CAP:IATU_UPPER_TARGET_ADDR_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_UPPER_TARGET_ADDR_OFF_OUTBOUND_0_OFF      0x10700018
/* PCIE_DM_EP:PF0_ATU_CAP:IATU_UPPR_LIMIT_ADDR_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_UPPR_LIMIT_ADDR_OFF_OUTBOUND_0_OFF        0x10700020

/* Accessors */

/* DEVCPU_GCB:CHIP_REGS:CHIP_ID */
#define GCB_CHIP_ID_PART_ID_GET(x)               (((x) & GENMASK(27, 12)) >> 12)

/* ASM:CFG:PORT_CFG[67] */
#define ASM_CFG_PORT_NO_PREAMBLE_ENA             BIT(9)
#define ASM_CFG_PORT_INJ_FORMAT(x)               (((x) << 2) & GENMASK(3, 2))
#define ASM_CFG_PORT_INJ_FORMAT_M                GENMASK(3, 2)
#define ASM_CFG_PORT_INJ_FORMAT_X(x)             (((x) & GENMASK(3, 2)) >> 2)
#define ASM_CFG_PORT_VSTAX2_AWR_ENA              BIT(1)

/* CPU:CPU_REGS:PROC_CTRL */
#define CPU_REGS_PROC_CTRL_ACP_DISABLE           BIT(0)
#define CPU_REGS_PROC_CTRL_L2_FLUSH_REQ          BIT(1)
#define CPU_REGS_PROC_CTRL_ACP_AWCACHE           BIT(3)
#define CPU_REGS_PROC_CTRL_ACP_ARCACHE           BIT(2)
#define CPU_REGS_PROC_CTRL_ACP_CACHE_FORCE_ENA   BIT(4)

/* DEVCPU_GCB:VCORE_ACCESS:VA_CTRL */
#define DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_ERR_M    GENMASK(3, 2)
#define DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_ERR_X(x) (((x) & GENMASK(3, 2)) >> 2)
/* DEVCPU_GCB:VCORE_ACCESS:VA_CTRL */
#define DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_BUSY_RD  BIT(1)
/* DEVCPU_GCB:VCORE_ACCESS:VA_CTRL */
#define DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_BUSY     BIT(0)

/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_MODE(x)            (((x) << 2) & GENMASK(3, 2))
#define DEVCPU_QS_XTR_GRP_CFG_MODE_M             GENMASK(3, 2)
#define DEVCPU_QS_XTR_GRP_CFG_MODE_X(x)          (((x) & GENMASK(3, 2)) >> 2)
/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_STATUS_WORD_POS    BIT(1)
/* DEVCPU_QS:XTR:XTR_GRP_CFG[2] */
#define DEVCPU_QS_XTR_GRP_CFG_BYTE_SWAP          BIT(0)
/* DEVCPU_QS:INJ:INJ_GRP_CFG[2] */
#define DEVCPU_QS_INJ_GRP_CFG_MODE(x)            (((x) << 2) & GENMASK(3, 2))
#define DEVCPU_QS_INJ_GRP_CFG_MODE_M             GENMASK(3, 2)
#define DEVCPU_QS_INJ_GRP_CFG_MODE_X(x)          (((x) & GENMASK(3, 2)) >> 2)
/* DEVCPU_QS:INJ:INJ_GRP_CFG[2] */
#define DEVCPU_QS_INJ_GRP_CFG_BYTE_SWAP          BIT(0)
/* DEVCPU_QS:INJ:INJ_CTRL[2] */
#define DEVCPU_QS_INJ_CTRL_GAP_SIZE(x)           (((x) << 21) & GENMASK(24, 21))
#define DEVCPU_QS_INJ_CTRL_GAP_SIZE_M            GENMASK(24, 21)
#define DEVCPU_QS_INJ_CTRL_GAP_SIZE_X(x)         (((x) & GENMASK(24, 21)) >> 21)

/* DSM:CFG:DEV_TX_STOP_WM_CFG[67] */
#define DSM_CFG_DEV_TX_STOP_WM(x)                (((x) << 1) & GENMASK(7, 1))
#define DSM_CFG_DEV_TX_STOP_WM_M                 GENMASK(7, 1)
#define DSM_CFG_DEV_TX_STOP_WM_X(x)              (((x) & GENMASK(7, 1)) >> 1)
/* DSM:CFG:BUF_CFG[67] */
#define DSM_CFG_BUF_UNDERFLOW_WATCHDOG_DIS       BIT(11)

/* QFWD:SYSTEM:FRAME_COPY_CFG[12] */
#define QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL(x)   (((x) << 6) & GENMASK(12, 6))
#define QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL_M    GENMASK(12, 6)
#define QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL_X(x) (((x) & GENMASK(12, 6)) >> 6)

/* FDMA:FDMA:FDMA_CH_CFG[8] */
#define FDMA_CH_CFG_INTR_DB_EOF_ONLY             BIT(6)
/* FDMA:FDMA:FDMA_CH_CFG[8] */
#define FDMA_CH_CFG_INJ_PORT                     BIT(5)
/* FDMA:FDMA:FDMA_CH_CFG[8] */
#define FDMA_CH_CFG_DCB_DB_CNT(x)                (((x) << 1) & GENMASK(4, 1))
#define FDMA_CH_CFG_DCB_DB_CNT_M                 GENMASK(4, 1)
#define FDMA_CH_CFG_DCB_DB_CNT_X(x)              (((x) & GENMASK(4, 1)) >> 1)
/* FDMA:FDMA:FDMA_CH_CFG[8] */
#define FDMA_CH_CFG_MEM                          BIT(0)
/* FDMA:FDMA:FDMA_PORT_CTRL[2] */
#define FDMA_PORT_CTRL_INJ_STOP                  BIT(4)
/* FDMA:FDMA:FDMA_PORT_CTRL[2] */
#define FDMA_PORT_CTRL_INJ_STOP_FORCE            BIT(3)
/* FDMA:FDMA:FDMA_PORT_CTRL[2] */
#define FDMA_PORT_CTRL_XTR_STOP                  BIT(2)
/* FDMA:FDMA:FDMA_PORT_CTRL[2] */
#define FDMA_PORT_CTRL_XTR_BUF_IS_EMPTY          BIT(1)
/* FDMA:FDMA:FDMA_PORT_CTRL[2] */
#define FDMA_PORT_CTRL_XTR_BUF_RST               BIT(0)
/* FDMA:FDMA:FDMA_CTRL */
#define FDMA_CTRL_NRESET                         BIT(0)


/* PCIE_DM_EP:PF0_ATU_CAP:IATU_REGION_CTRL_2_OFF_OUTBOUND_0 */
#define PCIE_DM_EP_PF0_ATU_REGION_CTRL_2_OFF_OUTBOUND_0_EN BIT(31)

/* DCB structure */

#define FDMA_DCB_MAX_DBS                         15
#define FDMA_DCB_INFO_DATAL(x)                   ((x) & GENMASK(15, 0))
#define FDMA_DCB_INFO_TOKEN                      BIT(17)
#define FDMA_DCB_INFO_INTR                       BIT(18)
#define FDMA_DCB_INFO_SW(x)                      (((x) << 24) & GENMASK(31, 24))

#define FDMA_DCB_STATUS_BLOCKL(x)                ((x) & GENMASK(15, 0))
#define FDMA_DCB_STATUS_SOF                      BIT(16)
#define FDMA_DCB_STATUS_EOF                      BIT(17)
#define FDMA_DCB_STATUS_INTR                     BIT(18)
#define FDMA_DCB_STATUS_DONE                     BIT(19)
#define FDMA_DCB_STATUS_BLOCKO(x)                (((x) << 20) & GENMASK(31, 20))
#define FDMA_DCB_INVALID_DATA                    0x1


/*
*
*   +---------------------------+
*   |         Next Ptr          |
*   +---------------------------+
*   |   Reserved  |    Info     |
*   +---------------------------+
*   |         Data0 Ptr         |
*   +---------------------------+
*   |   Reserved  |    Status0  |
*   +---------------------------+
*   |         Data1 Ptr         |
*   +---------------------------+
*   |   Reserved  |    Status1  |
*   +---------------------------+
*   |         Data2 Ptr         |
*   +---------------------------+
*   |   Reserved  |    Status2  |
*   |-------------|-------------|
*   |                           |
*   |                           |
*   |                           |
*   |                           |
*   |                           |
*   |---------------------------|
*   |         Data14 Ptr        |
*   +-------------|-------------+
*   |   Reserved  |    Status14 |
*   +-------------|-------------+
*/

#define FDMA_BUFFER_ALIGN                        128
#define FDMA_BUFFER_MASK                         127
#define XTR_BUFFER_SIZE                          (XTR_CHUNK_SIZE*12)
#define FDMA_XTR_CHANNEL                         6
#define FDMA_DCB_MAX                             21
#define VCORE_ACCESS_TIMEOUT_MS                  5
#define FDMA_DISABLE_TIMEOUT_MS                  5

/*
 * FDMA writing buffer data via PCIe
 *                                               Switch Core
 *  +--------------+            +---------------------------------------------+
 *  |     Host     |            |                         0 => 0x900000000    |
 *  |     RAM      |            |                        +-----------------+  |
 *  |              |            |      DCB Config ------->FDMA Translation |  |
 *  |              |            |                        +--------|--------+  |
 *  |              |            |                                 |           |
 *  |              |            |                                 |           |
 *  |              |            |    0x900000000 => 0         +---v---+       |
 *  +--------------+ PCIe Write | +-------------------+       | FDMA  |       |
 *  |   Buffer     |<--------------  ATU (outbound)   |       +---|---+       |
 *  +--------------+            | +---------^---------+           |           |
 *  |              |            |           |                     |           |
 *  |              |            |           |                     |           |
 *  +--------------+            |           |                     |           |
 *                              |           |             +-------v------+    |
 *                              |           |             | 0x900000000  |    |
 *                              |           +--------------              |    |
 *                              |                         |   PCIe EP    |    |
 *                              |                         +--------------+    |
 *                              +---------------------------------------------+
 */

/* Debugging flags */
// #define FDMA_ACCESS_LOG
// #define FDMA_DEBUG_TX
// #define FDMA_DEBUG_RX

/* Structures */

struct microchip_sparx5_fdma;

enum microchip_sparx5_fdma_channel_state {
	DCS_IDLE = 0,
	DCS_ACTIVE,
	DCS_RUNNING,
	DCS_STOPPING,
	DCS_ERROR
};

enum microchip_sparx5_fdma_dcb_state {
	DCBS_IDLE = 0,    /* Not yet used or ready to be reused */
	DCBS_QUEUED,      /* In queue for transfer */
	DCBS_ISSUED,      /* In transfer in progress */
	DCBS_ERROR,       /* Transfer failed */
	DCBS_COMPLETE,    /* Transfer successful */
};

struct microchip_sparx5_fdma_data {
	u64 dataptr;
	u64 status;
};

struct microchip_sparx5_fdma_dcb_hw {
	u64 nextptr;
	u64 info;
	struct microchip_sparx5_fdma_data block[FDMA_DCB_MAX_DBS];
};

struct microchip_sparx5_fdma_block_info {
	int size;
};

struct microchip_sparx5_fdma_dcb {
	struct  microchip_sparx5_fdma_dcb_hw hw;
	struct dma_async_tx_descriptor txd;
	dma_addr_t phys;
	enum microchip_sparx5_fdma_dcb_state state;
	int valid_blocks;
	struct microchip_sparx5_fdma_dcb *first_dcb;
	int is_last_dcb;
	struct microchip_sparx5_fdma_block_info binfo[FDMA_DCB_MAX_DBS];
	u32 residue;
	struct list_head node;
};

struct microchip_sparx5_fdma_region {
	phys_addr_t phys_addr;
	void __iomem *io_addr;
	resource_size_t size;
	phys_addr_t vcore_addr;
};

struct microchip_sparx5_stats {
	int free_dcbs;
	int free_dcbs_low_mark;
};

struct microchip_sparx5_fdma_channel {
	struct dma_chan chan;
	enum microchip_sparx5_fdma_channel_state state;
	struct dma_tx_state tx_state;
	struct list_head free_dcbs;
	struct list_head queued_dcbs;
	u64 dbirq_pattern;
	struct tasklet_struct tasklet;
	struct microchip_sparx5_fdma *drv;
	struct microchip_sparx5_fdma_dcb *next_dcb;
	int next_idx;
	struct microchip_sparx5_stats stats;
	spinlock_t lock;
};

struct microchip_sparx5_fdma {
	struct dma_device dma;  /* Must be first member due to xlate function */
	struct microchip_sparx5_fdma_region regions[SPARX5_REGIONS];
	struct dma_pool *dcb_pool;
	int irq;
	bool using_pcie;
	struct dentry *rootfs;

	unsigned int nr_pchans;
	struct microchip_sparx5_fdma_channel chans[0];
};

/* Implementation */

/* New encoding as of 27-may-2021
 *  0x05.. = TSN switches (Industrial)
 *  0x75.. = Non TSN switches (Enterprise)
 *  0x..46 = 64G switch capacity
 *  0x..49 = 90G switch capacity
 *  0x..52 = 128G switch capacity
 *  0x..56 = 160G switch capacity
 *  0x..58 = 200G switch capacity
 */
static u16 sparx5_partid[] = {
	0x0500, /* ST00195CCA11  VSC7500-V/5CC
	         * ST00195CCA12 VSC7500TSN-V/5CC
	         * ST00195CCA14 VSC7500-V/5CC
	         * ST00195CCA15 VSC7500TSN-V/5CC */
	0x0546, /* ST00195CCB11 VSC7546-V/5CC
	         * ST00195CCB12 VSC7546TSN-V/5CC
	         * ST00195CCB14 VSC7546-V/5CC
	         * ST00195CCB15 VSC7546TSN-V/5CC
	         * ST00195CCVB1 VSC7546TSN-V/5CC-VAO */
	0x0549, /* ST00195CCC11 VSC7549-V/5CC
	         * ST00195CCC12 VSC7549TSN-V/5CC
	         * ST00195CCC14 VSC7549-V/5CC
	         * ST00195CCC15 VSC7549TSN-V/5CC
	         * ST00195CCVC1 VSC7549TSN-V/5CC-VAO */
	0x0552, /* ST00195CCD11 VSC7552-V/5CC
	         * ST00195CCD12 VSC7552TSN-V/5CC
	         * ST00195CCD14 VSC7552-V/5CC
	         * ST00195CCD15 VSC7552TSN-V/5CC
	         * ST00195CCVD1 VSC7552TSN-V/5CC-VAO */
	0x0556, /* ST00195CCE11 VSC7556-V/5CC
	         * ST00195CCE12 VSC7556TSN-V/5CC
	         * ST00195CCE14 VSC7556-V/5CC
	         * ST00195CCE15 VSC7556TSN-V/5CC
	         * ST00195CCVE1 VSC7556TSN-V/5CC-VAO */
	0x0558, /* ST00195CCF11 VSC7558-V/5CC
	         * ST00195CCF12 VSC7558TSN-V/5CC
	         * ST00195CCF14 VSC7558-V/5CC
	         * ST00195CCF15 VSC7558TSN-V/5CC
	         * ST00195CCVF1 VSC7558TSN-V/5CC-VAO */
	0x7546, /* SparX-5-64 */
	0x7549, /* SparX-5-90 */
	0x7552, /* SparX-5-128 */
	0x7556, /* SparX-5-160 */
	0x7558, /* SparX-5-200 (is also TSN, unfused default value) */
	0x052e, /* SparX-5-64  fused with hex encoded BCD digits */
	0x0531, /* SparX-5-90  fused with hex encoded BCD digits */
	0x0534, /* SparX-5-128 fused with hex encoded BCD digits */
	0x0538, /* SparX-5-160 fused with hex encoded BCD digits */
	0x053a, /* SparX-5-200 fused with hex encoded BCD digits */
};

struct prof_sample {
	u64 min;
	u64 max;
	u64 sum;
};

struct prof_stat {
	int samples;
	char *name;
	int count;
	u64 last;
	struct prof_sample sample;
	struct prof_sample result;
};


static struct prof_stat profstats[2];

static void prof_sample_init(struct prof_stat *stat, int samples, char *name)
{
	stat->samples = samples;
	stat->count = stat->samples;
	stat->name = name;
	stat->sample.min = ~0;
	stat->sample.max = 0;
	stat->sample.sum = 0;
	stat->result.min = 0;
	stat->result.max = 0;
	stat->result.sum = 0;
}

static void prof_sample_begin(struct prof_stat *stat)
{
	stat->last = ktime_get_ns();
}

static void prof_sample_reset(struct prof_stat *stat)
{
	stat->count = stat->samples;
	stat->sample.min = ~0;
	stat->sample.max = 0;
	stat->sample.sum = 0;
}

static void prof_sample_end(struct prof_stat *stat)
{
	u64 diff = ktime_get_ns() - stat->last;
	stat->sample.sum += diff;
	if (diff > stat->sample.max) {
		stat->sample.max = diff;
	}
	if (diff < stat->sample.min) {
		stat->sample.min = diff;
	}
	if (--stat->count == 0) {
		memcpy(&stat->result, &stat->sample, sizeof(struct prof_sample));
		prof_sample_reset(stat);
	}
}

#if defined(FDMA_DEBUG_TX) || defined(FDMA_DEBUG_RX)
static void microchip_sparx5_dump_byte_array(const char *name, const u8 *buf, size_t len)
{
	char prefix[64];
	if (!buf) {
		return;
	}
	snprintf(prefix, sizeof(prefix), "%.50s[%zu]: ", name, len);
	print_hex_dump(KERN_NOTICE, prefix, DUMP_PREFIX_OFFSET, 16, 1, buf, len,
		       false);
}
#endif

static void microchip_sparx5_fdma_writel(struct microchip_sparx5_fdma *priv, u32 reg, u32 data)
{
	void __iomem *addr =
		priv->regions[GET_REGION_INDEX(reg)].io_addr + GET_ADDRESS(reg);
#ifdef FDMA_ACCESS_LOG
	pr_debug("%s:%d %s: addr 0x%llx, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		priv->regions[GET_REGION_INDEX(reg)].phys_addr +
		GET_ADDRESS(reg),
		data);
#endif
	writel(data, addr);
}

static void microchip_sparx5_fdma_writell(struct microchip_sparx5_fdma *priv, u32 regls, u32 regms,
			    u64 data)
{
	microchip_sparx5_fdma_writel(priv, regls, data & GENMASK(31,0));
	microchip_sparx5_fdma_writel(priv, regms, data >> 32);
}

static u32 microchip_sparx5_fdma_readl(struct microchip_sparx5_fdma *priv, u32 reg)
{
	void __iomem *addr =
		priv->regions[GET_REGION_INDEX(reg)].io_addr + GET_ADDRESS(reg);
	u32 data = readl(addr);
#ifdef FDMA_ACCESS_LOG
	pr_debug("%s:%d %s: addr 0x%llx, data 0x%08x\n",
		__FILE__, __LINE__, __func__,
		priv->regions[GET_REGION_INDEX(reg)].phys_addr +
		GET_ADDRESS(reg),
		data);
#endif
	return data;
}

static u64 microchip_sparx5_fdma_readll(struct microchip_sparx5_fdma *priv, u32 regls, u32 regms)
{
	u32 datals = microchip_sparx5_fdma_readl(priv, regls);
	u64 datams = microchip_sparx5_fdma_readl(priv, regms);
	return (datams << 32) | datals;
}

static u32 microchip_sparx5_vcore_readl(struct microchip_sparx5_fdma *priv, u64 vcore_addr) {
	unsigned long deadline;
	u32 value;
	u32 status;

#ifdef FDMA_ACCESS_LOG
	pr_debug("%s:%d %s: VCore Access Read Begin at 0x%llx",
		__FILE__, __LINE__, __func__, vcore_addr);
#endif
	/* Wait for not busy */
	deadline = jiffies + msecs_to_jiffies(VCORE_ACCESS_TIMEOUT_MS);
	do {
		status = microchip_sparx5_fdma_readl(priv,
				       DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF);
	} while (time_before(jiffies, deadline) &&
		 (status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_BUSY));
	/* Set VA_SIZE = 0 (32 bit access) */
	microchip_sparx5_fdma_writel(priv, DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF, 0);
	/* Set LS Word */
	microchip_sparx5_fdma_writel(priv, DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_LSB_OFF,
		       (u32)vcore_addr);
	/* Set MS Word */
	microchip_sparx5_fdma_writel(priv, DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_MSB_OFF,
		       (u32)(vcore_addr >> 32));
	/* Read VA_DATA to trigger access */
	microchip_sparx5_fdma_readl(priv, DEVCPU_GCB_VCORE_ACCESS_VA_DATA_OFF);
	/* Wait while VA_BUSY is set, handle timeouts */
	deadline = jiffies + msecs_to_jiffies(VCORE_ACCESS_TIMEOUT_MS);
	do {
		status = microchip_sparx5_fdma_readl(priv,
				       DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF);
	} while (time_before(jiffies, deadline) &&
		 (status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_BUSY) &&
		 !(status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_ERR_M));
	if (status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_ERR_M) {
		pr_err("%s:%d %s: Error reading VCore Access at 0x%llx",
			__FILE__, __LINE__, __func__, vcore_addr);
		return -EINVAL;
	}
	/* Read data without triggering a new read (using INERT) */
	value = microchip_sparx5_fdma_readl(priv, DEVCPU_GCB_VCORE_ACCESS_VA_DATA_INERT_OFF);
#ifdef FDMA_ACCESS_LOG
	pr_debug("%s:%d %s: VCore Access Read at 0x%llx, value 0x%08x",
		__FILE__, __LINE__, __func__, vcore_addr, value);
#endif
	return value;
}

static u64 microchip_sparx5_fdma_vcore_readll(struct microchip_sparx5_fdma *priv, u64 vcore_addr)
{
	u32 datals = microchip_sparx5_vcore_readl(priv, vcore_addr);
	u64 datams = microchip_sparx5_vcore_readl(priv, vcore_addr + 4);
	return (datams << 32) | datals;
}

static void microchip_sparx5_fdma_vcore_writel(struct microchip_sparx5_fdma *priv, u64 vcore_addr,
				u32 value) {
	unsigned long deadline;
	u32 status;

#ifdef FDMA_ACCESS_LOG
	pr_debug("%s:%d %s: VCore Access Write Begin at 0x%llx",
		__FILE__, __LINE__, __func__, vcore_addr);
#endif
	/* Wait for not busy */
	deadline = jiffies + msecs_to_jiffies(VCORE_ACCESS_TIMEOUT_MS);
	do {
		status = microchip_sparx5_fdma_readl(priv,
				       DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF);
	} while (time_before(jiffies, deadline) &&
		 (status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_BUSY));
	/* Set VA_SIZE = 0 (32 bit access) */
	microchip_sparx5_fdma_writel(priv, DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF, 0);
	/* Set LS Word */
	microchip_sparx5_fdma_writel(priv, DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_LSB_OFF,
		       (u32)vcore_addr);
	/* Set MS Word */
	microchip_sparx5_fdma_writel(priv, DEVCPU_GCB_VCORE_ACCESS_VA_ADDR_MSB_OFF,
		       (u32)(vcore_addr >> 32));
	/* Write VA_DATA to trigger write operation */
	microchip_sparx5_fdma_writel(priv, DEVCPU_GCB_VCORE_ACCESS_VA_DATA_OFF, value);
	/* Wait while VA_BUSY is set, handle timeouts */
	deadline = jiffies + msecs_to_jiffies(VCORE_ACCESS_TIMEOUT_MS);
	do {
		status = microchip_sparx5_fdma_readl(priv,
				       DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_OFF);
	} while (time_before(jiffies, deadline) &&
		 (status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_BUSY) &&
		 !(status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_ERR_M));
	if (status & DEVCPU_GCB_VCORE_ACCESS_VA_CTRL_ERR_M) {
		pr_err("%s:%d %s: Error writing VCore Access at 0x%llx",
			__FILE__, __LINE__, __func__, vcore_addr);
	}
#ifdef FDMA_ACCESS_LOG
	pr_debug("%s:%d %s: VCore Access Write at 0x%llx, value 0x%08x",
		__FILE__, __LINE__, __func__, vcore_addr, value);
#endif
}

/* Using Vcore access to read VCore address space */
static u32 microchip_sparx5_vcore_read_reg(struct microchip_sparx5_fdma *priv, u32 reg) {
	u64 vcore_addr =
		priv->regions[GET_REGION_INDEX(reg)].vcore_addr +
		GET_ADDRESS(reg);
	return microchip_sparx5_vcore_readl(priv, vcore_addr);
}

/* Using Vcore access to write VCore address space */
static void microchip_sparx5_fdma_vcore_write_reg(struct microchip_sparx5_fdma *priv,
				    u32 reg, u32 value) {
	u64 vcore_addr =
		priv->regions[GET_REGION_INDEX(reg)].vcore_addr +
		GET_ADDRESS(reg);
	microchip_sparx5_fdma_vcore_writel(priv, vcore_addr, value);
}

static int microchip_sparx5_fdma_cpuport_config(struct microchip_sparx5_fdma *priv, int channel)
{
	u32 value = 0;
	u32 mask;
	unsigned int cpuport = channel % 2;
	/* Sparx5 specific mapping to chipport */
	unsigned int chipport = cpuport + 65;
	int idx;

	pr_debug("%s:%d %s, Channel: %d, cpuport: %d, chipport: %d\n",
		__FILE__, __LINE__, __func__, channel, cpuport, chipport);

	/* Channel:
	* INJ_PORT is set to cpuport
	* DCB_DB_CNT = 15 for number of DCBs
	* INTR_DB_EOF_ONLY = 1 for intr on EOF only
	* CFG_MEM = 1 (is no longer used)
	*/
	microchip_sparx5_fdma_writel(priv, FDMA_CH_CFG_R_OFF(channel),
		       /* Use all data blocks */
		       FDMA_CH_CFG_DCB_DB_CNT(FDMA_DCB_MAX_DBS) |
		       FDMA_CH_CFG_INTR_DB_EOF_ONLY |
		       (cpuport ? FDMA_CH_CFG_INJ_PORT : 0) |
		       FDMA_CH_CFG_MEM);

	/* CPU Port: Enable FDMA Injection (mode 2) */
	value = microchip_sparx5_fdma_readl(priv, DEVCPU_QS_INJ_GRP_CFG_R_OFF(cpuport));
	value &= ~DEVCPU_QS_INJ_GRP_CFG_MODE_M;
	value |= DEVCPU_QS_INJ_GRP_CFG_MODE(2);
	value |= DEVCPU_QS_INJ_GRP_CFG_BYTE_SWAP;
	microchip_sparx5_fdma_writel(priv, DEVCPU_QS_INJ_GRP_CFG_R_OFF(cpuport), value);

	/* CPU Port: Set gap using IFH (gap 0) */
	microchip_sparx5_fdma_writel(priv, DEVCPU_QS_INJ_CTRL_R_OFF(cpuport),
		DEVCPU_QS_INJ_CTRL_GAP_SIZE(0));

	/* CPU Port: Enable FDMA Extraction (mode 2) */
	value = microchip_sparx5_fdma_readl(priv, DEVCPU_QS_XTR_GRP_CFG_R_OFF(cpuport));
	value &= ~DEVCPU_QS_XTR_GRP_CFG_MODE_M;
	value |= DEVCPU_QS_XTR_GRP_CFG_MODE(2);
	value |= DEVCPU_QS_XTR_GRP_CFG_BYTE_SWAP;
	microchip_sparx5_fdma_writel(priv, DEVCPU_QS_XTR_GRP_CFG_R_OFF(cpuport), value);

	/* INJ CPU Port Format: IFH with no prefix, no preamble, no VSTAX2 aware */
	value = microchip_sparx5_fdma_readl(priv, ASM_CFG_PORT_R_OFF(chipport));
	value &= ~ASM_CFG_PORT_VSTAX2_AWR_ENA;
	value |= ASM_CFG_PORT_INJ_FORMAT(1);
	value |= ASM_CFG_PORT_NO_PREAMBLE_ENA;
	microchip_sparx5_fdma_writel(priv, ASM_CFG_PORT_R_OFF(chipport), value);

	/* XTR CPU Port Format: IFH with prefix */
	value = microchip_sparx5_fdma_readl(priv, REW_COMMON_IFH_CTRL_CPUVD_OFF);
	mask = ~(0x3 << (cpuport << 1));
	value &= mask;
	value |= 2 << (cpuport << 1);
	microchip_sparx5_fdma_writel(priv, REW_COMMON_IFH_CTRL_CPUVD_OFF, value);

	/* Set Disassembler Stop Watermark level */
	value = microchip_sparx5_fdma_readl(priv, DSM_CFG_DEV_TX_STOP_WM_R_OFF(chipport));
	value &= ~DSM_CFG_DEV_TX_STOP_WM_M;
	value |= DSM_CFG_DEV_TX_STOP_WM(100);  /* Watermark at 100 packets */
	microchip_sparx5_fdma_writel(priv, DSM_CFG_DEV_TX_STOP_WM_R_OFF(chipport), value);

	/* Disable Disassembler buffer underrun watchdog (to avoid truncated
	 * packets in XTR)
	 */
	value = microchip_sparx5_fdma_readl(priv, DSM_CFG_BUF_R_OFF(chipport));
	value |= DSM_CFG_BUF_UNDERFLOW_WATCHDOG_DIS;
	microchip_sparx5_fdma_writel(priv, DSM_CFG_BUF_R_OFF(chipport), value);

	/* Copy frames in all fwd queues to the FDMA chipport */
	for (idx = 0; idx < QFWD_SYSTEM_FRAME_COPY_CFG_MAX; ++idx) {
		value = microchip_sparx5_fdma_readl(priv, QFWD_SYSTEM_FRAME_COPY_CFG_R_OFF(idx));
		value &= ~QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL_M;
		value |= QFWD_SYSTEM_FRAME_COPY_CFG_FRMC_PORT_VAL(chipport);
		microchip_sparx5_fdma_writel(priv, QFWD_SYSTEM_FRAME_COPY_CFG_R_OFF(idx), value);
	}

	return 0;
}

static inline struct microchip_sparx5_fdma *to_microchip_sparx5_fdma(struct dma_device *dd)
{
	return container_of(dd, struct microchip_sparx5_fdma, dma);
}

static inline struct microchip_sparx5_fdma_channel *to_microchip_sparx5_fdma_channel(
	struct dma_chan *c)
{
	return container_of(c, struct microchip_sparx5_fdma_channel, chan);
}

static void microchip_sparx5_fdma_free_dcb_list(struct dma_chan *chan)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	struct microchip_sparx5_fdma_channel *fdma_chan = to_microchip_sparx5_fdma_channel(chan);
	struct microchip_sparx5_fdma_dcb *iter;

	list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
		pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx:"
			" state: %u, C%u\n",
			__FILE__, __LINE__, __func__,
			fdma_chan->chan.chan_id,
			iter->phys,
			iter->state,
			iter->txd.cookie);
		dma_pool_free(priv->dcb_pool, iter, iter->phys);
	}
}

static int microchip_sparx5_fdma_wait_for_xtr_buffer_empty(struct microchip_sparx5_fdma *priv,
						  int channel)
{
	unsigned long deadline;
	int cpuport = channel % 2;
	int empty;

	pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);
	/* Wait here until the extraction buffer is empty */
	deadline = jiffies + msecs_to_jiffies(FDMA_DISABLE_TIMEOUT_MS);
	do {
		empty = microchip_sparx5_fdma_readl(priv,
			FDMA_PORT_CTRL_R_OFF(cpuport)) &
			FDMA_PORT_CTRL_XTR_BUF_IS_EMPTY;
		pr_debug("%s:%d %s: empty: %u\n",
			__FILE__, __LINE__, __func__, empty);
	} while (time_before(jiffies, deadline) && (empty == 0)) ;

	return empty;
}

static void microchip_sparx5_fdma_start(struct dma_chan *chan)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	int cpuport = chan->chan_id % 2;
	u32 control;

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);
	priv->chans[chan->chan_id].state = DCS_ACTIVE;
	if (priv->using_pcie) {
		/* Adding translation (in MB) to address the PCIe controller */
		microchip_sparx5_fdma_writel(priv, FDMA_CH_TRANSLATE_R_OFF(chan->chan_id),
				    (PCIE_TARGET_OFFSET >> 20));
	}
	if (chan->chan_id >= FDMA_XTR_CHANNEL) {
		/* Start extraction */
		control = microchip_sparx5_fdma_readl(priv,
					FDMA_PORT_CTRL_R_OFF(cpuport));
		control &= ~FDMA_PORT_CTRL_XTR_STOP;
		microchip_sparx5_fdma_writel(priv, FDMA_PORT_CTRL_R_OFF(cpuport), control);
	} else {
		/* Start injection */
		control = microchip_sparx5_fdma_readl(priv,
					FDMA_PORT_CTRL_R_OFF(cpuport));
		control &= ~FDMA_PORT_CTRL_INJ_STOP;
		microchip_sparx5_fdma_writel(priv, FDMA_PORT_CTRL_R_OFF(cpuport), control);
	}

}

static int microchip_sparx5_fdma_stop(struct dma_chan *chan)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	int cpuport = chan->chan_id % 2;
	int stopped = 0;
	u32 control, empty;

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);
	priv->chans[chan->chan_id].state = DCS_STOPPING;
	if (chan->chan_id >= FDMA_XTR_CHANNEL) {
		empty = microchip_sparx5_fdma_wait_for_xtr_buffer_empty(priv, chan->chan_id);
		if (!empty) {
			return stopped;
		}
		/* Stop extraction */
		control = microchip_sparx5_fdma_readl(priv,
					FDMA_PORT_CTRL_R_OFF(cpuport));
		control |= FDMA_PORT_CTRL_XTR_STOP;
		microchip_sparx5_fdma_writel(priv, FDMA_PORT_CTRL_R_OFF(cpuport), control);
		stopped = microchip_sparx5_fdma_wait_for_xtr_buffer_empty(priv, chan->chan_id);
	} else {
		/* Stop injection */
		control = microchip_sparx5_fdma_readl(priv,
					FDMA_PORT_CTRL_R_OFF(cpuport));
		control |= FDMA_PORT_CTRL_INJ_STOP;
		microchip_sparx5_fdma_writel(priv, FDMA_PORT_CTRL_R_OFF(cpuport), control);
		stopped = 1;
	}
	return stopped;

}

static int microchip_sparx5_fdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	u32 chan_mask;

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);

	microchip_sparx5_fdma_cpuport_config(priv, chan->chan_id);
	microchip_sparx5_fdma_start(chan);

	dma_cookie_init(chan);
	/* Enable DB interrupts */
	chan_mask = microchip_sparx5_fdma_readl(priv, FDMA_INTR_DB_ENA_OFF)
		| BIT(chan->chan_id);
	microchip_sparx5_fdma_writel(priv, FDMA_INTR_DB_ENA_OFF, chan_mask);
	return 0;
}

static void microchip_sparx5_fdma_free_chan_resources(struct dma_chan *chan)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	u32 chan_mask;

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);

	microchip_sparx5_fdma_stop(chan);

	/* Disable the channel */
	microchip_sparx5_fdma_writel(priv, FDMA_CH_DISABLE_OFF, ~BIT(chan->chan_id));

	/* Disable the channels DB interrupt */
	chan_mask = microchip_sparx5_fdma_readl(priv, FDMA_INTR_DB_ENA_OFF) &
		~BIT(chan->chan_id);
	microchip_sparx5_fdma_writel(priv, FDMA_INTR_DB_ENA_OFF, chan_mask);
}


static dma_cookie_t microchip_sparx5_fdma_tx_submit(struct dma_async_tx_descriptor *txd)
{
	if (txd) {
		if (txd->cookie >= DMA_MIN_COOKIE) {
			pr_debug("%s:%d %s: Channel: %u, reuse cookie: %u",
				 __FILE__, __LINE__, __func__,
				 txd->chan->chan_id, txd->cookie);
			return txd->cookie;
		}
		dma_cookie_assign(txd);
		pr_debug("%s:%d %s: Channel: %u, new cookie: %u",
			 __FILE__, __LINE__, __func__,
			 txd->chan->chan_id, txd->cookie);
		return txd->cookie;
	} else {
		pr_err("%s:%d %s: Invalid TXD",
		       __FILE__, __LINE__, __func__);
		return 0;
	}
}

static bool microchip_sparx5_fdma_get_dcb(struct microchip_sparx5_fdma *priv,
	struct dma_chan *chan,
	int sg_len,
	struct microchip_sparx5_fdma_dcb **res,
	int *residx)
{
	struct microchip_sparx5_fdma_channel *fdma_chan = to_microchip_sparx5_fdma_channel(chan);
	struct microchip_sparx5_fdma_dcb *dcb;
	int idx, jdx;

	if (list_empty(&fdma_chan->free_dcbs)) {
		return false;
	}
	dcb = list_first_entry(&fdma_chan->free_dcbs,
			       struct microchip_sparx5_fdma_dcb, node);
	/* Initialize the new DCB */
	memset(dcb, 0, sizeof(struct  microchip_sparx5_fdma_dcb_hw));
	memset(dcb->binfo, 0,
	       sizeof(struct  microchip_sparx5_fdma_block_info));
	dcb->state = DCBS_QUEUED;
	dcb->valid_blocks = 0;
	dcb->residue = 0;
	dcb->is_last_dcb = 0;
	/* No next DCB */
	dcb->hw.nextptr = FDMA_DCB_INVALID_DATA;
	for (jdx = 0; jdx < FDMA_DCB_MAX_DBS; ++jdx) {
		dcb->hw.block[jdx].dataptr =
			FDMA_DCB_INVALID_DATA;
	}
	dma_async_tx_descriptor_init(&dcb->txd, chan);
	dcb->txd.tx_submit = microchip_sparx5_fdma_tx_submit;
	dcb->txd.phys = dcb->phys;
	/* Move item into the channel */
	list_move_tail(&dcb->node, &fdma_chan->queued_dcbs);
	/* Update free dcb statistics */
	fdma_chan->stats.free_dcbs--;
	if (fdma_chan->stats.free_dcbs < fdma_chan->stats.free_dcbs_low_mark) {
		fdma_chan->stats.free_dcbs_low_mark =
			fdma_chan->stats.free_dcbs;
	}
	idx = 0;
	pr_debug("%s:%d %s: Channel: %u, new DCB: 0x%llx\n",
		 __FILE__, __LINE__, __func__,
		 chan->chan_id, dcb->phys);
	*res = dcb;
	*residx = idx;
	return true;
}

static void microchip_sparx5_fdma_add_datablock(struct microchip_sparx5_fdma *priv,
	struct microchip_sparx5_fdma_channel *fdma_chan,
	struct microchip_sparx5_fdma_dcb *dcb,
	enum dma_transfer_direction direction,
	struct scatterlist *sg,
	int sg_len,
	int sidx,
	int idx)
{
	u64 len, off;
	dma_addr_t db_phys = 0;
	/* Interrupt on DB status update */
	u32 status_flags;

	len = dcb->binfo[idx].size = sg_dma_len(sg);
	db_phys = sg_dma_address(sg);
	off = db_phys & 0x7;
	db_phys &= ~0x7;
	/* Adapt the DB Interrupt to the current load */
	status_flags = (fdma_chan->dbirq_pattern >> idx) & 0x1 ?
		FDMA_DCB_STATUS_INTR : 0;
	if (direction == DMA_MEM_TO_DEV) {
		if (sidx == 0) {
			status_flags |= FDMA_DCB_STATUS_SOF;
		}
		if (sg_is_last(sg)) {
			status_flags |= FDMA_DCB_STATUS_EOF;
		}
		dcb->hw.block[idx].dataptr = db_phys;
		dcb->hw.block[idx].status =
			FDMA_DCB_STATUS_BLOCKL(len) |
			status_flags |
			FDMA_DCB_STATUS_BLOCKO(off);
	} else {
		/* Length is a multipla of 128 */
		dcb->hw.info = FDMA_DCB_INFO_DATAL(
			len & ~FDMA_BUFFER_MASK);
		dcb->hw.block[idx].dataptr = db_phys;
		dcb->hw.block[idx].status = status_flags;
	}
	dcb->valid_blocks++;
	pr_debug("%s:%d %s: DCB: 0x%llx, Block[%02d], "
		 "dataptr: 0x%09llx, offset: 0x%llu, bytes: %llu\n",
		 __FILE__, __LINE__, __func__,
		 dcb->phys,
		 idx, dcb->hw.block[idx].dataptr,
		 off, len);
}

static void microchip_sparx5_fdma_xtr_eof(struct microchip_sparx5_fdma *priv,
	struct microchip_sparx5_fdma_channel *fdma_chan,
	struct microchip_sparx5_fdma_dcb *first,
	struct microchip_sparx5_fdma_dcb *iter,
	int idx,
	int packet_size,
	u64 status)
{
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_NOERROR,
		.residue = 0
	};
	struct dma_async_tx_descriptor *txd = &first->txd;

	packet_size += FDMA_DCB_STATUS_BLOCKL(status);
	if (first != iter) {
		first->residue = 0;
	}
	iter->residue -= FDMA_DCB_STATUS_BLOCKL(status);
	fdma_chan->tx_state.residue = iter->residue;
	pr_debug("%s:%d %s: Channel: %d, notify client:"
		 " txd: 0x%px, [C%u,I%u], packet size: %u\n",
		 __FILE__, __LINE__, __func__,
		 fdma_chan->chan.chan_id,
		 txd,
		 txd->cookie,
		 idx,
		 packet_size);
	dma_result.residue = packet_size;
	prof_sample_begin(&profstats[0]);
	dmaengine_desc_get_callback_invoke(
		txd, &dma_result);
	prof_sample_end(&profstats[0]);
	if (first != iter) {
		do {
			/* Last block in this DCB has been transferred  */
			fdma_chan->tx_state.last = txd->cookie;
			dma_cookie_complete(txd);
			pr_debug("%s:%d %s: Channel: %d,"
				 " completed DCB: 0x%llx"
				 " [C%u,I%u]\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 first->phys,
				 fdma_chan->tx_state.last, idx);
			iter->state = DCBS_IDLE;
			spin_lock(&fdma_chan->lock);
			list_move_tail(&first->node, &fdma_chan->free_dcbs);
			fdma_chan->stats.free_dcbs++;
			spin_unlock(&fdma_chan->lock);
			first = list_first_entry(&fdma_chan->queued_dcbs,
						 struct microchip_sparx5_fdma_dcb, node);
		} while (first != iter) ;
	}
	idx++;
	if (idx == iter->valid_blocks) {
		/* Last block in this DCB has been transferred  */
		fdma_chan->tx_state.last = txd->cookie;
		dma_cookie_complete(txd);
		pr_debug("%s:%d %s: Channel: %d,"
			 " completed DCB: 0x%llx"
			 " [C%u,I%u]\n",
			 __FILE__, __LINE__, __func__,
			 fdma_chan->chan.chan_id,
			 iter->phys,
			 fdma_chan->tx_state.last, idx);
		iter->state = DCBS_IDLE;
		spin_lock(&fdma_chan->lock);
		list_move_tail(&iter->node, &fdma_chan->free_dcbs);
		fdma_chan->stats.free_dcbs++;
		spin_unlock(&fdma_chan->lock);
		iter = list_first_entry(&fdma_chan->queued_dcbs,
					struct microchip_sparx5_fdma_dcb, node);
		idx = 0;
	}
	fdma_chan->next_dcb = iter;
	fdma_chan->next_idx = idx;
}


static void microchip_sparx5_fdma_xtr_tasklet(unsigned long data)
{
	struct microchip_sparx5_fdma_channel *fdma_chan =
		(struct microchip_sparx5_fdma_channel *)data;
	struct microchip_sparx5_fdma *priv = fdma_chan->drv;
	struct microchip_sparx5_fdma_dcb *iter;
	int idx;
	bool more = true;
	u32 packet_size = 0;
	u64 status;
	u64 pktstatus;
	struct microchip_sparx5_fdma_dcb *first = NULL;

	prof_sample_begin(&profstats[1]);
	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	iter = fdma_chan->next_dcb;
	idx = fdma_chan->next_idx;
	status = iter->hw.block[idx].status;
	more = !!(status & FDMA_DCB_STATUS_DONE);
	pktstatus = status & (FDMA_DCB_STATUS_SOF | FDMA_DCB_STATUS_EOF | FDMA_DCB_STATUS_DONE);
	while (more) {
		pr_debug("%s:%d %s: Channel: %d, [C%u,I%u], status: 0x%llx\n",
			 __FILE__, __LINE__, __func__,
			 fdma_chan->chan.chan_id,
			 iter->txd.cookie,
			 idx,
			 status);
		if (pktstatus ==
		    (FDMA_DCB_STATUS_SOF | FDMA_DCB_STATUS_EOF | FDMA_DCB_STATUS_DONE)) {
			microchip_sparx5_fdma_xtr_eof(priv, fdma_chan, iter, iter, idx, 0, status);
			iter = fdma_chan->next_dcb;
			idx = fdma_chan->next_idx;
		} else if (pktstatus & FDMA_DCB_STATUS_SOF) {
			packet_size = FDMA_DCB_STATUS_BLOCKL(status);
			first = iter;
			pr_debug("%s:%d %s: Channel: %d, SOF:"
				 " txd: 0x%px, [C%u,I%u], packet size: %u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 &iter->txd,
				 iter->txd.cookie,
				 idx,
				 packet_size);
			idx++;
			if (idx == iter->valid_blocks) {
				iter = list_next_entry(iter, node);
				idx = 0;
			}
		} else if ((pktstatus & FDMA_DCB_STATUS_EOF) && first) {
			microchip_sparx5_fdma_xtr_eof(priv, fdma_chan, first, iter, idx, packet_size, status);
			iter = fdma_chan->next_dcb;
			idx = fdma_chan->next_idx;
		} else {
			packet_size += FDMA_DCB_STATUS_BLOCKL(status);
			pr_debug("%s:%d %s: Channel: %d, middle block: packet size: %u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 packet_size);
			idx++;
			if (idx == iter->valid_blocks) {
				iter = list_next_entry(iter, node);
				idx = 0;
			}
		}
		status = iter->hw.block[idx].status;
		more = !!(status & FDMA_DCB_STATUS_DONE);
		pktstatus = status & (FDMA_DCB_STATUS_SOF | FDMA_DCB_STATUS_EOF | FDMA_DCB_STATUS_DONE);
	}
	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
	prof_sample_end(&profstats[1]);
}

static void microchip_sparx5_fdma_inj_tasklet(unsigned long data)
{
	struct microchip_sparx5_fdma_channel *fdma_chan =
		(struct microchip_sparx5_fdma_channel *)data;
	struct microchip_sparx5_fdma_dcb *request = 0;
	struct microchip_sparx5_fdma_dcb *iter, *first = 0, *prev = 0, *tmp;
	int idx;
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_ABORTED,
		.residue = 0
	};
	u32 packet_size = 0;

	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	spin_lock(&fdma_chan->lock);
	list_for_each_entry_safe(iter, tmp,
				 &fdma_chan->queued_dcbs, node) {
		if (prev && prev->state == DCBS_COMPLETE &&
		    prev->hw.nextptr != FDMA_DCB_INVALID_DATA) {
			/* The previous completed DCB can be freed */
			pr_debug("%s:%d %s: Channel: %d, previous completed DCB:"
				 " 0x%llx move to free list\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id, prev->phys);
			prev->state = DCBS_IDLE;
			list_move_tail(&prev->node, &fdma_chan->free_dcbs);
			fdma_chan->stats.free_dcbs++;
		}
		prev = iter;
		if (iter->state != DCBS_ISSUED) {
			continue;
		}
		for (idx = 0; idx < iter->valid_blocks; ++idx) {
			u64 status = iter->hw.block[idx].status;
			struct microchip_sparx5_fdma_block_info *blk = &iter->binfo[idx];

			pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx,"
				 " Block[%02d], dataptr: 0x%09llx, status:"
				 " 0x%09llx: bytes: %u, C%u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 iter->phys,
				 idx,
				 iter->hw.block[idx].dataptr,
				 status,
				 blk->size,
				 iter->txd.cookie);
			if (iter->txd.cookie > DMA_MIN_COOKIE) {
				/* Requests have valid cookies */
				if (!request) {
					/* First in queue is the used TXD */
					fdma_chan->tx_state.used = iter->txd.cookie;
				}
				request = iter;
			}
			if (!request) {
				continue;
			}
			if (blk->size == 0) {
				continue;
			}
			if (!(status & FDMA_DCB_STATUS_DONE)) {
				break;
			}
			if (status & FDMA_DCB_STATUS_SOF) {
				first = iter;
			}
			if (!first) {
				continue;
			}
			/* Update packet size and request residue with this
			 * block
			 */
			packet_size += FDMA_DCB_STATUS_BLOCKL(status);
			request->residue -= FDMA_DCB_STATUS_BLOCKL(status);
			if (!(status & FDMA_DCB_STATUS_EOF)) {
				continue;
			}
			if (idx == iter->valid_blocks - 1) {
				/* Last block in this DCB has been transferred  */
				pr_debug("%s:%d %s: Channel: %d, completed DCB:"
					 " 0x%llx\n",
					 __FILE__, __LINE__, __func__,
					 fdma_chan->chan.chan_id, iter->phys);
				iter->state = DCBS_COMPLETE;
				/* Last DCB in this request has been transferred  */
				if (iter->is_last_dcb) {
					fdma_chan->tx_state.last =
						iter->first_dcb->txd.cookie;
					dma_cookie_complete(&iter->first_dcb->txd);
					pr_debug("%s:%d %s: Channel: %d,"
						 " completed cookie: %u\n",
						 __FILE__, __LINE__, __func__,
						 fdma_chan->chan.chan_id,
						 fdma_chan->tx_state.last);
				}
			}
			fdma_chan->tx_state.residue = request->residue;
			dma_result.residue = request->residue;
			dma_result.result = DMA_TRANS_NOERROR;
			pr_debug("%s:%d %s: Channel: %d, notify client:"
				 " txd: 0x%px, residue: %u, packet size: %u\n",
				 __FILE__, __LINE__, __func__,
				 fdma_chan->chan.chan_id,
				 &request->txd,
				 request->residue,
				 packet_size);
			if (fdma_chan->chan.chan_id >= FDMA_XTR_CHANNEL) {
				dma_result.residue = packet_size;
			}
			prof_sample_begin(&profstats[0]);
			spin_unlock(&fdma_chan->lock);
			dmaengine_desc_get_callback_invoke(
				&request->txd, &dma_result);
			spin_lock(&fdma_chan->lock);
			prof_sample_end(&profstats[0]);
			packet_size = 0;
			/* Mark data block as transferred */
			blk->size = 0;
		}
	}
	spin_unlock(&fdma_chan->lock);
	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
}

static struct dma_async_tx_descriptor *
microchip_sparx5_fdma_prep_slave_sg(struct dma_chan *chan,
	struct scatterlist *sgl,
	unsigned int sg_len,
	enum dma_transfer_direction direction,
	unsigned long flags,
	void *context)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	struct microchip_sparx5_fdma_channel *fdma_chan = to_microchip_sparx5_fdma_channel(chan);
	struct scatterlist *sg;
	struct microchip_sparx5_fdma_dcb *first;
	struct microchip_sparx5_fdma_dcb *dcb;
	int sidx; /* Segment index */
	int idx;  /* Block index */
	u32 residue;
	unsigned long lflags;

	if (!sgl) {
		return NULL;
	}

	if (!is_slave_direction(direction)) {
		dev_err(&chan->dev->device, "Invalid DMA direction\n");
		return NULL;
	}
	if (fdma_chan->state == DCS_STOPPING) {
		pr_debug("%s:%d %s, Stopping channel %d\n",
			__FILE__, __LINE__, __func__, chan->chan_id);
		return NULL;
	}

	spin_lock_irqsave(&fdma_chan->lock, lflags);
	first = 0;
	dcb = 0;
	idx = 0;
	residue = 0;
	for_each_sg(sgl, sg, sg_len, sidx) {
		/* One DCB has room for FDMA_DCB_MAX_DDBS blocks */
		if (idx == 0) {
			if (!microchip_sparx5_fdma_get_dcb(priv, chan, sg_len,
						  &dcb, &idx)) {
				pr_err("%s:%d %s: no more DCBs\n",
				       __FILE__, __LINE__, __func__);
				goto unlock;
			}
		}
		microchip_sparx5_fdma_add_datablock(priv, fdma_chan, dcb, direction,
						sg, sg_len, sidx, idx);
		residue += dcb->binfo[idx].size;
		pr_debug("%s:%d %s, Channel %d, residue: %u, block[%02u]: %u\n",
			__FILE__, __LINE__, __func__,
			chan->chan_id, residue, idx, dcb->binfo[idx].size);
		if (!first) {
			first = dcb;
			pr_debug("%s:%d %s, Channel %d, dcb: 0x%llx, txd:"
				 " 0x%px, block: %02u\n",
				 __FILE__, __LINE__, __func__,
				 chan->chan_id, dcb->phys, &dcb->txd, idx);
		}
		++idx;
		idx = idx % FDMA_DCB_MAX_DBS;
	}
	dcb->is_last_dcb = 1;
	first->residue += residue;
	dcb->first_dcb = first;
	spin_unlock_irqrestore(&fdma_chan->lock, lflags);
	pr_debug("%s:%d %s, Channel %d, len: %d, dir: %s: txd: 0x%px\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id,
		sg_len,
		direction == DMA_MEM_TO_DEV ? "to device" : "from device",
		&first->txd);

	return &first->txd;
unlock:
	spin_unlock_irqrestore(&fdma_chan->lock, lflags);
	return NULL;
}

static enum dma_status microchip_sparx5_fdma_tx_status(struct dma_chan *chan,
	dma_cookie_t cookie,
	struct dma_tx_state *txstate)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	struct microchip_sparx5_fdma_channel *fdma_chan;
	struct microchip_sparx5_fdma_dcb *iter;
	enum dma_status status;
	int found = 0;
	unsigned int residue = 0;
	int idx;

	pr_debug("%s:%d %s, cookie: %u\n",
		__FILE__, __LINE__, __func__, cookie);

	status = dma_cookie_status(chan, cookie, txstate);

	fdma_chan = &priv->chans[chan->chan_id];

	if (fdma_chan->state == DCS_ERROR) {
		return DMA_ERROR;
	}
	if (status != DMA_IN_PROGRESS) {
		return status;
	}
	list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
		pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx:"
			" state: %u, cookie: %u\n",
			__FILE__, __LINE__, __func__,
			fdma_chan->chan.chan_id,
			iter->txd.phys,
			iter->state,
			iter->txd.cookie);
		if (iter->txd.cookie == cookie) {
			found = 1;
			for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
				residue += iter->binfo[idx].size;
			}
		} else if (iter->txd.cookie > DMA_MIN_COOKIE) {
			found = 0;
		} else if (found) {
			for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
				residue += iter->binfo[idx].size;
			}
		}
	}
	txstate->residue = residue;
	return status;
}

static void microchip_sparx5_fdma_issue_pending(struct dma_chan *chan)
{
	u32 channel_bit = BIT(chan->chan_id);
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	struct microchip_sparx5_fdma_dcb *dcb, *first = 0;
	struct microchip_sparx5_fdma_channel *fdma_chan;
	int idx = 0;
	int queued = 0;
	unsigned long flags;

	prof_sample_begin(&profstats[1]);
	pr_debug("%s:%d %s, Channel: %d\n",
		 __FILE__, __LINE__, __func__,
		 chan->chan_id);
	fdma_chan = &priv->chans[chan->chan_id];
	spin_lock_irqsave(&fdma_chan->lock, flags);
	list_for_each_entry(dcb, &fdma_chan->queued_dcbs, node) {
		if (dcb->state == DCBS_QUEUED) {
			if (first == 0) {
				first = dcb;
			}
			++queued;
		}
		++idx;
	}
	spin_unlock_irqrestore(&fdma_chan->lock, flags);
	if (!first) {
		pr_err("%s:%d %s, Channel: %d, nothing queued\n",
		       __FILE__, __LINE__, __func__,
		       chan->chan_id);
		return;
	}
	pr_debug("%s:%d %s, Channel: %d, state: %u, len: %d, queued: %d\n",
		 __FILE__, __LINE__, __func__,
		 chan->chan_id, fdma_chan->state, idx, queued);

	switch (fdma_chan->state) {
	case DCS_ACTIVE: {
		struct microchip_sparx5_fdma_dcb *prev = 0;
		int idx = 0;

		pr_debug("%s:%d %s, Activate channel %d, DCB: 0x%llx\n",
			 __FILE__, __LINE__, __func__, chan->chan_id,
			 first->phys);
		spin_lock_irqsave(&fdma_chan->lock, flags);
		list_for_each_entry(dcb, &fdma_chan->queued_dcbs, node) {
			if (dcb->state == DCBS_QUEUED) {
				dcb->state = DCBS_ISSUED;
				pr_debug("%s:%d %s, Channel: %d: Issued: [%02d]:"
					 " DCB: 0x%llx\n",
					 __FILE__, __LINE__, __func__,
					 chan->chan_id,
					 idx,
					 dcb->phys);
				if (prev) {
					pr_debug("%s:%d %s, Channel: %d:"
						 " chain[%02d]:"
						 " DCB: 0x%llx -> 0x%llx\n",
						 __FILE__, __LINE__, __func__,
						 chan->chan_id,
						 idx,
						 prev->phys, dcb->phys);
					prev->hw.nextptr = dcb->phys; /* Valid */
				}
			}
			prev = dcb;
			++idx;
		}
		spin_unlock_irqrestore(&fdma_chan->lock, flags);
		fdma_chan->next_dcb = first;
		fdma_chan->next_idx = 0;
		/* Write the DCB address */
		microchip_sparx5_fdma_writell(priv,
				     FDMA_DCB_LLP_R_OFF(chan->chan_id),
				     FDMA_DCB_LLP1_R_OFF(chan->chan_id),
				     first->phys);
		/* Activate the channel */
		microchip_sparx5_fdma_writel(priv, FDMA_CH_ACTIVATE_OFF, channel_bit);
		fdma_chan->state = DCS_RUNNING;
		break;
	}
	case DCS_RUNNING: {
		struct microchip_sparx5_fdma_dcb *prev = 0;
		int idx = 0;

		spin_lock_irqsave(&fdma_chan->lock, flags);
		list_for_each_entry(dcb, &fdma_chan->queued_dcbs, node) {
			if (dcb->state == DCBS_QUEUED) {
				dcb->state = DCBS_ISSUED;
				if (prev) {
					prev->hw.nextptr = dcb->phys; /* Valid */
					pr_debug("%s:%d %s, Channel: %d:"
						 " chain[%02d]:"
						 " DCB: 0x%llx -> 0x%llx\n",
						 __FILE__, __LINE__, __func__,
						 chan->chan_id,
						 idx,
						 prev->phys, dcb->phys);
				}
			}
			prev = dcb;
			++idx;
		}
		spin_unlock_irqrestore(&fdma_chan->lock, flags);
		pr_debug("%s:%d %s, Reload channel %d\n",
			 __FILE__, __LINE__, __func__, chan->chan_id);
		microchip_sparx5_fdma_writel(priv, FDMA_CH_RELOAD_OFF, channel_bit);
		break;
	}
	case DCS_STOPPING:
		pr_debug("%s:%d %s, Stopping channel %d\n",
			 __FILE__, __LINE__, __func__, chan->chan_id);
		break;
	case DCS_IDLE:
		/* When is a reload needed ? */
		pr_debug("%s:%d %s, Queue channel %d, DCB: 0x%llx\n",
			 __FILE__, __LINE__, __func__, chan->chan_id,
			 dcb->phys);
		break;
	case DCS_ERROR:
		pr_err("%s:%d %s, Errored channel %d,\n",
		       __FILE__, __LINE__, __func__, chan->chan_id);
		break;
	}
	prof_sample_end(&profstats[1]);
}

static void microchip_sparx5_fdma_notify_clients_abort(struct dma_chan *chan)
{
	struct microchip_sparx5_fdma_channel *fdma_chan = to_microchip_sparx5_fdma_channel(chan);
	struct microchip_sparx5_fdma_dcb *iter;
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_ABORTED,
		.residue = 0
	};
	int idx;

	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
		pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx:"
			" state: %u, cookie: %u\n",
			__FILE__, __LINE__, __func__,
			fdma_chan->chan.chan_id,
			iter->phys,
			iter->state,
			iter->txd.cookie);
		for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
			iter->binfo[idx].size = 0;
		}
		if (iter->txd.cookie > DMA_MIN_COOKIE) {
			/* Requests have valid cookies */
			fdma_chan->tx_state.used = iter->txd.cookie;
			fdma_chan->tx_state.last = iter->txd.cookie;
			fdma_chan->tx_state.residue = 0;
			iter->state = DCBS_COMPLETE;
			dma_cookie_complete(&iter->txd);
			pr_debug("%s:%d %s: Channel: %d, notify client abort:"
				" txd: 0x%px\n",
				__FILE__, __LINE__, __func__,
				fdma_chan->chan.chan_id,
				&iter->txd);
			dmaengine_desc_get_callback_invoke(&iter->txd,
							   &dma_result);
		}
	}
	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
}

static void microchip_sparx5_fdma_sync(struct dma_chan *chan)
{
	struct microchip_sparx5_fdma *priv = to_microchip_sparx5_fdma(chan->device);
	u32 channel_mask = BIT(chan->chan_id);
	u32 status;
	unsigned long deadline;

	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);
	/* Wait here until the FDMA has stopped */
	deadline = jiffies + msecs_to_jiffies(FDMA_DISABLE_TIMEOUT_MS);
	do {
		status = microchip_sparx5_fdma_readl(priv, FDMA_CH_ACTIVE_OFF);
		status |= microchip_sparx5_fdma_readl(priv, FDMA_CH_PENDING_OFF);
		pr_debug("%s:%d %s: Channel: %u, status: %u\n",
			__FILE__, __LINE__, __func__,
			chan->chan_id, status);
	} while (time_before(jiffies, deadline) && (status & channel_mask)) ;

	/* Notify the client that the queued transfers have been aborted */
	microchip_sparx5_fdma_notify_clients_abort(chan);
}

static int microchip_sparx5_fdma_terminate(struct dma_chan *chan)
{
	pr_debug("%s:%d %s: Channel: %u\n", __FILE__, __LINE__, __func__,
		chan->chan_id);
	return 0;
}

static void microchip_sparx5_fdma_notify_clients_error(
	struct microchip_sparx5_fdma_channel *fdma_chan)
{
	struct microchip_sparx5_fdma_dcb *iter;
	struct dmaengine_result dma_result = {
		.result = DMA_TRANS_ABORTED,
		.residue = 0
	};
	int idx;

	pr_debug("%s:%d %s: Channel: %d, begin\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);

	dma_result.result = DMA_TRANS_WRITE_FAILED;
	if (fdma_chan->chan.chan_id >= FDMA_XTR_CHANNEL) {
		dma_result.result = DMA_TRANS_READ_FAILED;
	}
	list_for_each_entry(iter, &fdma_chan->queued_dcbs, node) {
		pr_debug("%s:%d %s: Channel: %d, DCB: 0x%llx:"
			" state: %u, cookie: %u\n",
			__FILE__, __LINE__, __func__,
			fdma_chan->chan.chan_id,
			iter->phys,
			iter->state,
			iter->txd.cookie);
		for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
			iter->binfo[idx].size = 0;
		}
		if (iter->txd.cookie > DMA_MIN_COOKIE) {
			/* Requests have valid cookies */
			fdma_chan->tx_state.used = iter->txd.cookie;
			fdma_chan->tx_state.last = iter->txd.cookie;
			fdma_chan->tx_state.residue = 0;
			iter->state = DCBS_COMPLETE;
			dma_cookie_complete(&iter->txd);
			pr_debug("%s:%d %s: Channel: %d, notify client abort:"
				" txd: 0x%px\n",
				__FILE__, __LINE__, __func__,
				fdma_chan->chan.chan_id,
				&iter->txd);
			dmaengine_desc_get_callback_invoke(&iter->txd,
							   &dma_result);
		}
	}
	pr_debug("%s:%d %s: Channel: %d, end\n",
		__FILE__, __LINE__, __func__,
		fdma_chan->chan.chan_id);
}

static irqreturn_t microchip_sparx5_fdma_interrupt(int irq, void *dev_id)
{
	struct microchip_sparx5_fdma *priv = dev_id;
	u32 dcb = 0, db = 0, err = 0;

	pr_debug("%s:%d %s: begin\n",
		__FILE__, __LINE__, __func__);
	dcb = microchip_sparx5_fdma_readl(priv, FDMA_INTR_DCB_OFF);
	db = microchip_sparx5_fdma_readl(priv, FDMA_INTR_DB_OFF);
	err = microchip_sparx5_fdma_readl(priv, FDMA_INTR_ERR_OFF);
	/* Clear interrupt */
	if (dcb) {
		microchip_sparx5_fdma_writel(priv, FDMA_INTR_DCB_OFF, dcb);
		pr_debug("%s:%d %s: DCB int: 0x%x\n",
			__FILE__, __LINE__, __func__, dcb);
	}
	if (db) {
		microchip_sparx5_fdma_writel(priv, FDMA_INTR_DB_OFF, db);
		pr_debug("%s:%d %s: DB int: 0x%x\n",
			__FILE__, __LINE__, __func__, db);
		while (db) {
			u32 chan = __fls(db);
			struct microchip_sparx5_fdma_channel *fdma_chan =
				&priv->chans[chan];
			tasklet_schedule(&fdma_chan->tasklet);
			db &= ~(BIT(chan));
		}
	}
	if (err) {
		u32 err_type = microchip_sparx5_fdma_readl(priv, FDMA_ERRORS_OFF);

		pr_err("%s:%d %s: ERR int: 0x%x\n",
			__FILE__, __LINE__, __func__, err);
		pr_err("%s:%d %s: errtype: 0x%x\n",
			__FILE__, __LINE__, __func__,
			err_type);
		microchip_sparx5_fdma_writel(priv, FDMA_INTR_ERR_OFF, err);
		microchip_sparx5_fdma_writel(priv, FDMA_ERRORS_OFF, err_type);
		while (err) {
			u32 chan = __fls(err);
			struct microchip_sparx5_fdma_channel *fdma_chan =
				&priv->chans[chan];
			microchip_sparx5_fdma_notify_clients_error(fdma_chan);
			err &= ~(BIT(chan));
		}
	}

	pr_debug("%s:%d %s: end\n",
		__FILE__, __LINE__, __func__);

	return IRQ_HANDLED;
}

static void microchip_sparx5_fdma_disable_acp_caching(struct microchip_sparx5_fdma *priv)
{
	/*
	 * Force ACP caching but disable read/write allocation
	 * This avoid cache skew where secure entries are allocated by the
	 * FDMA and not seen by the host.
	 */
	u32 proc_ctrl = microchip_sparx5_fdma_readl(priv, CPU_REGS_PROC_CTRL_OFF);
	proc_ctrl |= CPU_REGS_PROC_CTRL_ACP_CACHE_FORCE_ENA;
	proc_ctrl &= ~(CPU_REGS_PROC_CTRL_ACP_AWCACHE
		       | CPU_REGS_PROC_CTRL_ACP_ARCACHE);
	microchip_sparx5_fdma_writel(priv, CPU_REGS_PROC_CTRL_OFF, proc_ctrl);
	pr_debug("%s:%d %s: Setting to: 0x%x\n",
		 __FILE__, __LINE__, __func__, proc_ctrl);
}

static int microchip_sparx5_buffer_access_test(struct microchip_sparx5_fdma *priv,
				      char *message,
				      int size,
				      u32 *buffer,
				      dma_addr_t phys)
{
	u32 value1 = 0xdeadbeef;
	u32 value2;

	if (priv->using_pcie) {
		phys += PCIE_TARGET_OFFSET;
	}
	*buffer = value1;

	value2 = microchip_sparx5_vcore_readl(priv, phys);
	if (value1 != value2) {
		snprintf(message, size, "Read to uncached buffer via vcore:"
			 " 0x%px, phys: 0x%09llx, "
			 " 0x%x != 0x%x\n",
			 buffer, phys,
			 value1, value2);
		return -1;
	}
	value1 = 0x12345678;
	microchip_sparx5_fdma_vcore_writel(priv, phys, value1);
	value2 = *buffer;
	if (value1 != value2) {
		snprintf(message, size, "Write to uncached buffer via vcore:"
			 " 0x%px, phys: 0x%09llx, "
			 " 0x%x != 0x%x\n",
			 buffer, phys,
			 value1, value2);
		return -1;
	}
	return 0;
}


static int microchip_sparx5_access_test(struct platform_device *pdev,
			       struct microchip_sparx5_fdma *priv)
{
	/* Check VCore Access functionality by reading chip id */
	u32 test0 = 0x12345678;
	u32 test1 = 0xabcdef;
	u32 value = 0;
	u32 vcore_chip_id;
	u32 part_id;
	char message[160];
	u64 phys;
	u32 *buffer;
	int res;
	bool found = false;
	int idx;

	vcore_chip_id = microchip_sparx5_fdma_readl(priv, DEVCPU_GCB_CHIP_REGS_ID_OFF);
	part_id = GCB_CHIP_ID_PART_ID_GET(vcore_chip_id);
	dev_info(&pdev->dev, "Chip Part ID %#04x\n", part_id);
	for (idx = 0; idx < ARRAY_SIZE(sparx5_partid); idx++)
		if (sparx5_partid[idx] == part_id)
			found = true;
	if (!found) {
		snprintf(message, 160, "Chip ID error: %#08x\n", vcore_chip_id);
		goto error;
	}
	/* Write value to General CPU reg 0 */
	microchip_sparx5_fdma_vcore_write_reg(priv, CPU_REGS_GPR_R_OFF(0), test0);
	/* Write value to General CPU reg 1 */
	microchip_sparx5_fdma_vcore_write_reg(priv, CPU_REGS_GPR_R_OFF(1), test1);
	/* Read General CPU reg 0 */
	value = microchip_sparx5_vcore_read_reg(priv, CPU_REGS_GPR_R_OFF(0));
	if (test0 != value) {
		snprintf(message, 160, "VCore access error: AMBA_TOP space\n");
		goto error;
	}
	/* Read General CPU reg 1 */
	value = microchip_sparx5_vcore_read_reg(priv, CPU_REGS_GPR_R_OFF(1));
	if (test1 != value) {
		snprintf(message, 160, "VCore access error: AMBA_TOP space\n");
		goto error;
	}
	microchip_sparx5_fdma_disable_acp_caching(priv);
	buffer = dma_pool_zalloc(priv->dcb_pool, GFP_ATOMIC, &phys);
	res = microchip_sparx5_buffer_access_test(priv, message, 160, buffer, phys);
	dma_pool_free(priv->dcb_pool, buffer, phys);
	if (res) {
		goto error;
	}
	snprintf(message, 160, "Successfully accessed host via vcore access\n");
	dev_info(&pdev->dev, message);
	return 0;
error:
	dev_err(&pdev->dev, message);
	return -ENODEV;
}

static void microchip_sparx5_setup_pcie_atu(struct microchip_sparx5_fdma *priv)
{
	/* DMA Address Translation Configuration
	 * Enable outbound ATU for region 0:
	 * Set enable bit
	 * Set base address = PCI_TARGET_SPACE (the vcore side)
	 * Set limit address =  PCI_TARGET_SPACE + 4GB (the vcore side)
	 * Set target address = 0 (the host side)
	 */
	microchip_sparx5_fdma_vcore_write_reg(priv,
		PCIE_DM_EP_PF0_ATU_REGION_CTRL_2_OFF_OUTBOUND_0_OFF,
		PCIE_DM_EP_PF0_ATU_REGION_CTRL_2_OFF_OUTBOUND_0_EN);

	microchip_sparx5_fdma_vcore_write_reg(priv,
		PCIE_DM_EP_PF0_ATU_LWR_BASE_ADDR_OFF_OUTBOUND_0_OFF,
		(u32)PCIE_TARGET_OFFSET);
	microchip_sparx5_fdma_vcore_write_reg(priv,
		PCIE_DM_EP_PF0_ATU_UPPER_BASE_ADDR_OFF_OUTBOUND_0_OFF,
		(u32)(PCIE_TARGET_OFFSET >> 32));
	microchip_sparx5_fdma_vcore_write_reg(priv,
		PCIE_DM_EP_PF0_ATU_LIMIT_ADDR_OFF_OUTBOUND_0_OFF,
		~0x0);
	microchip_sparx5_fdma_vcore_write_reg(priv,
		PCIE_DM_EP_PF0_ATU_UPPR_LIMIT_ADDR_OFF_OUTBOUND_0_OFF,
		(u32)(PCIE_TARGET_OFFSET >> 32));
	microchip_sparx5_fdma_vcore_write_reg(priv,
		PCIE_DM_EP_PF0_ATU_LWR_TARGET_ADDR_OFF_OUTBOUND_0_OFF,
		0);
	microchip_sparx5_fdma_vcore_write_reg(priv,
		PCIE_DM_EP_PF0_ATU_UPPER_TARGET_ADDR_OFF_OUTBOUND_0_OFF,
		0);
}

static void microchip_sparx5_reset_fdma(struct microchip_sparx5_fdma *priv)
{
	pr_debug("%s:%d %s\n", __FILE__, __LINE__, __func__);

	/* Stop any active injection or extraction */
	microchip_sparx5_fdma_writel(priv, FDMA_PORT_CTRL_R_OFF(0),
				     FDMA_PORT_CTRL_XTR_STOP |
				     FDMA_PORT_CTRL_INJ_STOP);
	microchip_sparx5_fdma_writel(priv, FDMA_PORT_CTRL_R_OFF(1),
				     FDMA_PORT_CTRL_XTR_STOP |
				     FDMA_PORT_CTRL_INJ_STOP);

	/* Disable all channels and their interrupts */
	microchip_sparx5_fdma_writel(priv, FDMA_CH_FORCEDIS_OFF, 0xff);
	microchip_sparx5_fdma_writel(priv, FDMA_INTR_DB_ENA_OFF, 0);
	microchip_sparx5_fdma_writel(priv, FDMA_INTR_ERR_OFF, ~0);
	microchip_sparx5_fdma_writel(priv, FDMA_ERRORS_OFF, ~0);
	microchip_sparx5_fdma_writel(priv, FDMA_ERRORS_2_OFF, ~0);

	/* Reset the FDMA */
	microchip_sparx5_fdma_writel(priv, FDMA_CTRL_OFF, 0);
	microchip_sparx5_fdma_writel(priv, FDMA_CTRL_OFF, FDMA_CTRL_NRESET);

	/* Use the Address Translation for PCIe */
	if (priv->using_pcie) {
		microchip_sparx5_setup_pcie_atu(priv);
	}
	/* Workaround for FDMA secure mode operations */
	microchip_sparx5_fdma_disable_acp_caching(priv);
}

static bool microchip_sparx5_init_region(struct platform_device *pdev,
	struct microchip_sparx5_fdma *priv, struct resource *res,
	unsigned int top, phys_addr_t vcore_addr)
{
	struct microchip_sparx5_fdma_region *region = 0;

	if (!res->start || !res->end) {
		return 0;
	}
	region = &priv->regions[GET_REGION_INDEX(top)];
	region->phys_addr = res->start;
	region->size = res->end - res->start + 1;
	region->io_addr = ioremap(region->phys_addr, region->size);
	region->vcore_addr = vcore_addr;
	pr_debug("%s:%d %s: IO Mapping %u: 0x%llx -> 0x%px .. 0x%px,"
		" vcore: 0x%llx\n",
		__FILE__, __LINE__, __func__,
		GET_REGION_INDEX(top),
		region->phys_addr,
		region->io_addr,
		region->io_addr + region->size - 1,
		region->vcore_addr);
	return region->phys_addr != region->vcore_addr;
}

static u64 microchip_sparx5_fdma_show_dcb_via_vcore(struct seq_file *s,
					    struct microchip_sparx5_fdma *priv,
					    u64 phys,
					    bool show_data)
{
	u64 vcore_addr = phys;
	int idx;
	u64 nextptr, info;
	u64 data_addr;
	u64 data_info;

	nextptr = microchip_sparx5_fdma_vcore_readll(priv, vcore_addr);
	vcore_addr += 8;
	info = microchip_sparx5_fdma_vcore_readll(priv, vcore_addr);
	vcore_addr += 8;
	seq_printf(s, "DCB: 0x%09llx nextptr: 0x%09llx info: 0x%09llx\n",
		   phys, nextptr, info);
	for (idx = 0; idx < FDMA_DCB_MAX_DBS; ++idx) {
		data_addr = microchip_sparx5_fdma_vcore_readll(priv, vcore_addr);
		vcore_addr += 8;
		if (data_addr != FDMA_DCB_INVALID_DATA) {
			data_info = microchip_sparx5_fdma_vcore_readll(priv, vcore_addr);
			vcore_addr += 8;
			seq_printf(s, "      [%02d] dataptr: 0x%09llx"
				   " status: 0x%09llx\n",
				   idx, data_addr, data_info);
		} else {
			vcore_addr += 8;
			data_addr = 0;
		}
		if (show_data && data_addr) {
			int wmax = ((data_info + 4) & 0xffff) >> 2;
			int wcount;
			vcore_addr = data_addr;
			for (wcount = 0; wcount < wmax; wcount++) {
				u32 val = microchip_sparx5_vcore_readl(priv, vcore_addr);
				seq_printf(s, "      0x%llx: %02x %02x %02x %02x\n",
					   vcore_addr,
					   val & 0xff,
					   (val >> 8) & 0xff,
					   (val >> 16) & 0xff,
					   (val >> 24) & 0xff);
				vcore_addr += 4;
			}
		}
	}
	return nextptr;
}

static void proc_microchip_sparx5_fdma_stats(struct seq_file *s, struct prof_stat *stat)
{
	seq_printf(s, "%s min ns: %llu\n", stat->name, stat->result.min);
	seq_printf(s, "%s max ns: %llu\n", stat->name, stat->result.max);
	seq_printf(s, "%s avg ns: %llu\n", stat->name,
		   stat->result.sum / stat->samples);
}

static int microchip_sparx5_fdma_seqfile_dcbqueue(struct seq_file *s, void *offset)
{
	struct microchip_sparx5_fdma *priv = dev_get_drvdata(s->private);
	struct microchip_sparx5_fdma_channel *fdma_chan;
	u32 chan;
	static struct {
		char *name;
	} states[] = {
		{"Idle"},      /* CBS_IDLE = 0 */
		{"Queued"},    /* DCBS_QUEUED */
		{"Issued"},    /* DCBS_ISSUED */
		{"Error"},     /* DCBS_ERROR */
		{"Complete"}   /* DCBS_COMPLETE */
	};

	seq_printf(s, "\nChannel Status:\n");
	for (chan = 0; chan < priv->nr_pchans; ++chan) {
		struct microchip_sparx5_fdma_dcb *dcb;
		int idx = 0;

		fdma_chan = &priv->chans[chan];
		if (fdma_chan->state == DCS_IDLE) {
			continue;
		}
		seq_printf(s, "  Channel %u\n", chan);
		list_for_each_entry(dcb, &fdma_chan->queued_dcbs, node) {
			seq_printf(s, "    {%02d} %8.8s C%u ",
				   idx, states[dcb->state].name, dcb->txd.cookie);
			microchip_sparx5_fdma_show_dcb_via_vcore(s,
							priv,
							dcb->phys,
							false);
			++idx;
		}
	}
	return 0;
}

static int microchip_sparx5_fdma_seqfile_dcblinks(struct seq_file *s, void *offset)
{
	struct microchip_sparx5_fdma *priv = dev_get_drvdata(s->private);
	struct microchip_sparx5_fdma_channel *fdma_chan;
	u32 chan;

	for (chan = 0; chan < priv->nr_pchans; ++chan) {
		u64 dcb_phys;

		fdma_chan = &priv->chans[chan];
		if (fdma_chan->state == DCS_IDLE) {
			continue;
		}
		dcb_phys = microchip_sparx5_fdma_readll(priv,
					       FDMA_DCB_LLP_PREV_R_OFF(chan),
					       FDMA_DCB_LLP_PREV1_R_OFF(chan));
		seq_printf(s, "\nDCB Links: channel: %u\n", chan);
		while (dcb_phys > FDMA_DCB_INVALID_DATA) {
			dcb_phys = microchip_sparx5_fdma_show_dcb_via_vcore(s,
								   priv,
								   dcb_phys,
								   false);
		}
	}
	return 0;
}

static int microchip_sparx5_fdma_seqfile_stats(struct seq_file *s, void *offset)
{
	struct microchip_sparx5_fdma *priv = dev_get_drvdata(s->private);
	u32 chan;

	for (chan = 0; chan < priv->nr_pchans; ++chan) {
		struct microchip_sparx5_fdma_channel *fdma_chan = &priv->chans[chan];
		if (fdma_chan->state == DCS_IDLE) {
			continue;
		}
		seq_printf(s, "Channel %u: Total dcbs: %d\n", chan, FDMA_DCB_MAX);
		seq_printf(s, "Channel %u: Free dcbs: %d\n", chan,
			   fdma_chan->stats.free_dcbs);
		seq_printf(s, "Channel %u: Free dcbs low mark: %d\n", chan,
			   fdma_chan->stats.free_dcbs_low_mark);
	}
	proc_microchip_sparx5_fdma_stats(s, &profstats[0]);
	proc_microchip_sparx5_fdma_stats(s, &profstats[1]);
	return 0;
}

static void microchip_sparx5_fdma_debugfs(struct microchip_sparx5_fdma *priv)
{
	u32 chan;

	prof_sample_init(&profstats[0], 20000, "rx callback");
	prof_sample_init(&profstats[1], 20000, "issue pending");

	priv->rootfs = debugfs_create_dir("microchip_sparx5_fdma", NULL);
	for (chan = 0; chan < priv->nr_pchans; ++chan) {
		struct microchip_sparx5_fdma_channel *fdma_chan;
		char buffer[15];

		fdma_chan = &priv->chans[chan];
		snprintf(buffer, 14, "channel%u_irq", chan);
		debugfs_create_x64(buffer, 0666, priv->rootfs,
				   &fdma_chan->dbirq_pattern);
	}
	debugfs_create_devm_seqfile(priv->dma.dev, "dcbqueue", priv->rootfs,
				    microchip_sparx5_fdma_seqfile_dcbqueue);
	debugfs_create_devm_seqfile(priv->dma.dev, "dcblinks", priv->rootfs,
				    microchip_sparx5_fdma_seqfile_dcblinks);
	debugfs_create_devm_seqfile(priv->dma.dev, "stats", priv->rootfs,
				    microchip_sparx5_fdma_seqfile_stats);
}

static int microchip_sparx5_fdma_probe(struct platform_device *pdev)
{
	struct device_node *devnode = pdev->dev.of_node;
	struct microchip_sparx5_fdma *priv;
	int ret = -ENODEV, nr_channels, idx, jdx;
	struct resource *res;

	/* Get Frame DMA info from Device Tree */
	if (!devnode) {
		dev_err(&pdev->dev, "Did not find frame dma device tree node\n");
		return ret;
	}
	ret = of_property_read_u32(devnode, "dma-channels", &nr_channels);
	if (ret) {
		dev_err(&pdev->dev, "Cannot get dma-channels\n");
		goto out_freetreenode;
	}

	dev_info(&pdev->dev, "%d dma-channels\n", nr_channels);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv) + nr_channels *
			sizeof(struct microchip_sparx5_fdma_channel), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto out_freetreenode;
	}
	priv->nr_pchans = nr_channels;
	platform_set_drvdata(pdev, priv);

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);

	/* Use slave mode DMA */
	dma_cap_set(DMA_SLAVE, priv->dma.cap_mask);
	priv->dma.dev = &pdev->dev;

	/* Create a pool of consistent memory blocks for hardware descriptors */
	priv->dcb_pool = dmam_pool_create("microchip-fdma-dcb", priv->dma.dev,
		sizeof(struct microchip_sparx5_fdma_dcb),
		FDMA_BUFFER_ALIGN,
		0);
	if (!priv->dcb_pool) {
		dev_err(&pdev->dev, "Unable to allocate DMA descriptor pool\n");
		ret = -ENOMEM;
		goto out_free;
	}

	/* Create the channel list */
	INIT_LIST_HEAD(&priv->dma.channels);
	for (idx = 0; idx < nr_channels; idx++) {
		struct microchip_sparx5_fdma_channel *fdma_chan = &priv->chans[idx];

		fdma_chan->chan.device = &priv->dma;
		fdma_chan->state = DCS_IDLE;
		INIT_LIST_HEAD(&fdma_chan->queued_dcbs);
		fdma_chan->dbirq_pattern = 0x7fff;
		fdma_chan->drv = priv;
		if (idx >= FDMA_XTR_CHANNEL) {
			tasklet_init(&fdma_chan->tasklet,
				     microchip_sparx5_fdma_xtr_tasklet,
				     (unsigned long)fdma_chan);
		} else {
			tasklet_init(&fdma_chan->tasklet,
				     microchip_sparx5_fdma_inj_tasklet,
				     (unsigned long)fdma_chan);
		}

		spin_lock_init(&fdma_chan->lock);
		list_add_tail(&fdma_chan->chan.device_node, &priv->dma.channels);

		INIT_LIST_HEAD(&fdma_chan->free_dcbs);
		for (jdx = 0; jdx < FDMA_DCB_MAX; ++jdx) {
			struct microchip_sparx5_fdma_dcb *dcb;
			dma_addr_t dcb_phys;

			dcb = dma_pool_zalloc(priv->dcb_pool, GFP_ATOMIC, &dcb_phys);
			if (dcb) {
				dcb->phys = dcb_phys;
				list_add(&dcb->node, &fdma_chan->free_dcbs);
			}
		}
		fdma_chan->stats.free_dcbs = FDMA_DCB_MAX;
		fdma_chan->stats.free_dcbs_low_mark = FDMA_DCB_MAX;
	}

	/* Provide DMA Engine device interface */
	priv->dma.dev = &pdev->dev;
	priv->dma.device_alloc_chan_resources =
		microchip_sparx5_fdma_alloc_chan_resources;
	priv->dma.device_free_chan_resources =
		microchip_sparx5_fdma_free_chan_resources;
	priv->dma.device_prep_slave_sg = microchip_sparx5_fdma_prep_slave_sg;
	priv->dma.device_tx_status = microchip_sparx5_fdma_tx_status;
	priv->dma.device_issue_pending = microchip_sparx5_fdma_issue_pending;
	priv->dma.device_terminate_all = microchip_sparx5_fdma_terminate;
	priv->dma.device_synchronize = microchip_sparx5_fdma_sync;
	priv->dma.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
	priv->dma.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
	priv->dma.directions = BIT(DMA_MEM_TO_MEM);
	priv->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;

	/* Register DMA Engine device */
	ret = dma_async_device_register(&priv->dma);
	if (ret) {
		dev_err(&pdev->dev, "Could not register DMA engine device\n");
		goto out_free;
	}

	/* Register DMA controller (uses "dmas" and "dma-names" in DT) */
	ret = of_dma_controller_register(devnode, of_dma_xlate_by_chan_id, priv);
	if (ret) {
		dev_err(&pdev->dev, "Could not register DMA controller\n");
		goto out_unregister;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Cannot get IO resource 0\n");
		return -EINVAL;
	}
	priv->using_pcie = microchip_sparx5_init_region(pdev, priv, res, SPARX5_DEFAULT,
					       VCORE_CSR_SPACE);
	dev_info(&pdev->dev, "using_pcie: %d\n", priv->using_pcie);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(&pdev->dev, "Cannot get IO resource 1\n");
		return -EINVAL;
	}
	microchip_sparx5_init_region(pdev, priv, res, SPARX5_AMBA_TOP, AMBA_TOP_SPACE);

	microchip_sparx5_reset_fdma(priv);

	ret = microchip_sparx5_access_test(pdev, priv);
	if (ret) {
		goto out_free;
	}

	priv->irq = platform_get_irq(pdev, 0);
	pr_debug("%s:%d %s: IRQ: %d\n", __FILE__, __LINE__, __func__, priv->irq);
	ret = devm_request_irq(&pdev->dev, priv->irq, microchip_sparx5_fdma_interrupt, 0,
		dev_name(&pdev->dev), priv);
	if (ret) {
		dev_err(&pdev->dev, "Could not request IRQ\n");
		goto out_unregister;
	}

	/* Provide debugfs access to data */
	microchip_sparx5_fdma_debugfs(priv);

	return 0;


out_unregister:
	dma_async_device_unregister(&priv->dma);
out_free:
	kfree(priv);
out_freetreenode:
	of_node_put(devnode);
	return ret;
}

static int microchip_sparx5_fdma_remove(struct platform_device *pdev)
{
	struct microchip_sparx5_fdma *priv = platform_get_drvdata(pdev);
	struct microchip_sparx5_fdma_channel *fdma_chan;
	struct microchip_sparx5_fdma_dcb *iter;
	int idx, chan;

	pr_debug("%s:%d %s: begin\n", __FILE__, __LINE__, __func__);

	debugfs_remove_recursive(priv->rootfs);
	for (chan = 0; chan < priv->nr_pchans; ++chan) {
		fdma_chan = &priv->chans[chan];
		microchip_sparx5_fdma_free_chan_resources(&fdma_chan->chan);
		microchip_sparx5_fdma_sync(&fdma_chan->chan);
		microchip_sparx5_fdma_free_dcb_list(&fdma_chan->chan);
		list_for_each_entry(iter, &fdma_chan->free_dcbs, node) {
			pr_debug("%s:%d %s: DCB: 0x%llx: state: %u, C%u\n",
				 __FILE__, __LINE__, __func__,
				 iter->phys,
				 iter->state,
				 iter->txd.cookie);
			dma_pool_free(priv->dcb_pool, iter, iter->phys);
		}
	}

	/* Free interrupt */
	devm_free_irq(priv->dma.dev, priv->irq, priv);
	for (idx = 0; idx < SPARX5_REGIONS; ++idx) {
		iounmap(priv->regions[idx].io_addr);
	}
	kfree(priv);
	pr_debug("%s:%d %s: end\n", __FILE__, __LINE__, __func__);
	return 0;
}

static const struct of_device_id microchip_sparx5_fdma_match[] = {
	{ .compatible = "microchip,vsc7558-fdma", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, microchip_sparx5_fdma_match);

static struct platform_driver microchip_sparx5_fdma_driver = {
	.probe = microchip_sparx5_fdma_probe,
	.remove = microchip_sparx5_fdma_remove,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = of_match_ptr(microchip_sparx5_fdma_match),
	},
};

static int __init microchip_sparx5_fdma_init(void)
{
	return platform_driver_register(&microchip_sparx5_fdma_driver);
}

static void __exit microchip_sparx5_fdma_exit(void)
{
	platform_driver_unregister(&microchip_sparx5_fdma_driver);
}

void unload_sparx5_fdma_driver(void)
{
	pr_debug("%s:%d %s: begin\n", __FILE__, __LINE__, __func__);
	platform_driver_unregister(&microchip_sparx5_fdma_driver);
	pr_debug("%s:%d %s: end\n", __FILE__, __LINE__, __func__);
}
EXPORT_SYMBOL(unload_sparx5_fdma_driver);


subsys_initcall(microchip_sparx5_fdma_init);
module_exit(microchip_sparx5_fdma_exit);

MODULE_AUTHOR("Steen Hegelund <steen.hegelund@microchip.com>");
MODULE_DESCRIPTION("Microsemi Sparx5 FDMA driver");
MODULE_LICENSE("Dual MIT/GPL");
