// SPDX-License-Identifier: GPL-2.0+

#include <linux/bitfield.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/types.h>

#include "fdma_pci.h"

/* In order for the FDMA device to dma to/from main memory, the DMA addresses
 * will have to be mapped inside the PCIe Outbound (OB) address space. Mappings
 * can be set up by configuring up to six regions in the PCIe ATU.
 *
 * This implementation takes a simple approach of dividing the regions in six
 * equally sized chunks.
 *
 * +-------------+------------+------------+------------+
 * | Index       | Region 0   | Region 1   | Region 5   |
 * +-------------+------------+------------+------------+
 * | Base addr   | 0x10000000 | 0x12a80000 | 0x1d480000 |
 * | Limit addr  | 0x12a7ffff | 0x154fffff | 0x1fefffff |
 * | Target addr | dma addr   | dma addr   | dma addr   |
 * +-------------+------------+------------+------------+
 *
 * Base addr is the start address of the region within the OB address space.
 * Limit addr is the end address of the region within the OB address space.
 * Target addr is the address to be translated to within the DMA address space.
 */

#define FDMA_PCI_ATU_REGION_ALIGN BIT(18) /* Align on 64KB boundary */
#define FDMA_PCI_ATU_OB_START    0x10000000
#define FDMA_PCI_ATU_OB_END      0x1fffffff
#define FDMA_PCI_ATU_REGION_SIZE round_down((FDMA_PCI_ATU_OB_END - FDMA_PCI_ATU_OB_START) \
	/ FDMA_PCI_ATU_REGION_MAX, FDMA_PCI_ATU_REGION_ALIGN)

#define FDMA_PCI_ATU_ADDR            0x300000
#define FDMA_PCI_ATU_IDX_SIZE        0x200
#define FDMA_PCI_ATU_ENA             0x4
#define FDMA_PCI_ATU_ENA_BIT         BIT(31)
#define FDMA_PCI_ATU_LWR_BASE_ADDR   0x8
#define FDMA_PCI_ATU_UPP_BASE_ADDR   0xc
#define FDMA_PCI_ATU_LIMIT_ADDR      0x10
#define FDMA_PCI_ATU_LWR_TARGET_ADDR 0x14
#define FDMA_PCI_ATU_UPP_TARGET_ADDR 0x18

static void *fdma_pci_io_addr_get(void __iomem *addr, int offset, int idx)
{
	return addr + FDMA_PCI_ATU_ADDR + FDMA_PCI_ATU_IDX_SIZE * idx + offset;
}

/* Configure the mapping in the ATU */
static void fdma_pci_configure_atu(struct fdma_pci_atu_region *region)
{
	struct fdma_pci_atu *atu = region->atu;
	int idx = region->idx;

	writel(FDMA_PCI_ATU_ENA_BIT,
	       fdma_pci_io_addr_get(atu->addr, FDMA_PCI_ATU_ENA, idx));

	writel(lower_32_bits(region->base_addr),
	       fdma_pci_io_addr_get(atu->addr, FDMA_PCI_ATU_LWR_BASE_ADDR,
				    idx));

	writel(upper_32_bits(region->base_addr),
	       fdma_pci_io_addr_get(atu->addr, FDMA_PCI_ATU_UPP_BASE_ADDR,
				    idx));

	writel(region->limit_addr,
	       fdma_pci_io_addr_get(atu->addr, FDMA_PCI_ATU_LIMIT_ADDR, idx));

	writel(lower_32_bits(region->target_addr),
	       fdma_pci_io_addr_get(atu->addr, FDMA_PCI_ATU_LWR_TARGET_ADDR,
				    idx));

	writel(upper_32_bits(region->target_addr),
	       fdma_pci_io_addr_get(atu->addr, FDMA_PCI_ATU_UPP_TARGET_ADDR,
				    idx));
}

/* Get a free region */
static struct fdma_pci_atu_region *
fdma_pci_atu_region_get_free(struct fdma_pci_atu *atu)
{
	struct fdma_pci_atu_region *regions = atu->regions;
	int i;

	for (i = 0; i < FDMA_PCI_ATU_REGION_MAX; i++) {
		if (regions[i].target_addr)
			continue;

		pr_debug("%s:%u Using free region %u", __func__, __LINE__, i);

		return &regions[i];
	}

	return ERR_PTR(-ENOMEM);
}

/* Unmap a region */
int fdma_pci_atu_region_unmap(struct fdma_pci_atu_region *region)
{
	struct fdma_pci_atu_region *regions = region->atu->regions;
	int i;

	for (i = 0; i < FDMA_PCI_ATU_REGION_MAX; i++) {
		if (regions[i].base_addr != region->base_addr)
			continue;

		pr_debug("%s:%u Unmapping region %u", __func__, __LINE__, i);

		region->target_addr = 0;

		fdma_pci_configure_atu(region);

		return 0;
	}

	return -ENOENT;
}

/* Map a region */
struct fdma_pci_atu_region *
fdma_pci_atu_region_map(struct fdma_pci_atu *atu, u64 target_addr, int size)
{
	struct fdma_pci_atu_region *region;

	if (!atu)
		return ERR_PTR(-EINVAL);

	if (size > FDMA_PCI_ATU_REGION_SIZE)
		return ERR_PTR(-E2BIG);

	region = fdma_pci_atu_region_get_free(atu);
	if (IS_ERR(region))
		return region;

	region->target_addr = target_addr;

	fdma_pci_configure_atu(region);

	return region;
}

/* Convert a target addr to the equivalent base addr */
u64 fdma_pci_atu_get_mapped_addr(struct fdma_pci_atu_region *region, u64 addr)
{
	return region->base_addr + (addr - region->target_addr);
}

/* Divide the OB address space in equally sized regions */
void fdma_pci_atu_init(struct fdma_pci_atu *atu, void __iomem *addr)
{
	struct fdma_pci_atu_region *regions = atu->regions;
	int i;

	atu->addr = addr;

	for (i = 0; i < FDMA_PCI_ATU_REGION_MAX; i++) {
		regions[i].base_addr =
			FDMA_PCI_ATU_OB_START + (i * FDMA_PCI_ATU_REGION_SIZE);
		regions[i].limit_addr =
			regions[i].base_addr + FDMA_PCI_ATU_REGION_SIZE - 1;
		regions[i].idx = i;
		regions[i].atu = atu;
	}
}
