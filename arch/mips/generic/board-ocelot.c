// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi MIPS SoC support
 *
 * Copyright (c) 2017 Microsemi Corporation
 */
#include <asm/machine.h>
#include <asm/prom.h>
#include <asm/fw/fw.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/clocksource.h>

extern char __appended_dtb[];
extern __initdata const void *fdt;
extern __initdata const struct mips_machine *mach;
extern __initdata const void *mach_match_data;

/* module data */
static unsigned long vcoreiii_memmap_start;
static size_t vcoreiii_memmap_size;
static const struct plat_props *props;

struct plat_props {
	phys_addr_t uart_addr;
};

static const struct plat_props ocelot_props = {
	.uart_addr = 0x70100000,
};

static const struct plat_props luton_props = {
	.uart_addr = 0x70100000,
};

static const struct plat_props serval_props = {
	.uart_addr = 0x70100000,
};

static const struct plat_props jr2_props = {
	.uart_addr = 0x70100000,
};

static void __init ocelot_earlyprintk_init(void)
{
	if (props) {
		void __iomem *uart_base;
		uart_base = ioremap(props->uart_addr, 0x20);
		setup_8250_early_printk_port((unsigned long)uart_base, 2, 50000);
	}
}

static void __init ocelot_late_init(void)
{
	ocelot_earlyprintk_init();
}

static __init int vcfw_address_fixup(void *fdt)
{
	int vcfw_off;
	int len;

	vcfw_off = fdt_node_offset_by_compatible(fdt, -1, "mscc,vcfw_uio");
	if (vcfw_off < 0) {
		pr_info("VCFW DT node is not available\n");
	} else if (vcoreiii_memmap_start == 0) {
		fdt_nop_node(fdt, vcfw_off);
		pr_info("Remove VCFW DT node: no memory map\n");
	} else {
		const __be32 *origreg;
		u32 value[2];
		__be32 newreg[2];

		origreg = fdt_getprop(fdt, vcfw_off, "reg", &len);
		if (len == sizeof(value)) {
			be32_to_cpu_array(value, origreg, 2);
			pr_debug("%s: addr: 0x%x, size: %u\n", __func__, value[0], value[1]);
			if (value[0] == vcoreiii_memmap_start && value[1] == vcoreiii_memmap_size) {
				return 0;
			}
			value[0] = vcoreiii_memmap_start;
			value[1] = vcoreiii_memmap_size;
			pr_debug("%s: new: addr: 0x%x, size: %u\n", __func__, value[0], value[1]);
			cpu_to_be32_array(newreg, value, 2);
			fdt_setprop(fdt, vcfw_off, "reg", newreg, sizeof(newreg));
		}
	}

	return 0;
}

static const struct mips_fdt_fixup ocelot_fdt_fixups[] __initconst = {
	{ vcfw_address_fixup, "update address and size for FW ramload" },
	{},
};

static __init const void *ocelot_fixup_fdt(const void *fdt,
					   const void *match_data)
{
	static unsigned char fdt_buf[16 << 10] __initdata;

	/* This has to be done so late because ioremap needs to work */
	late_time_init = ocelot_late_init;
	props = match_data;

	apply_mips_fdt_fixups(fdt_buf, sizeof(fdt_buf), fdt, ocelot_fdt_fixups);
	return fdt_buf;
}

static __init bool mscc_mips_detect(void)
{
	/* TODO: Do a real MIPS vcore check */
#if defined(CONFIG_LEGACY_BOARD_OCELOT) || defined(CONFIG_MSCC_OCELOT) \
	|| defined(CONFIG_MSCC_LUTON) || defined(CONFIG_MSCC_JAGUAR2)
	return 1;
#else
	return 0;
#endif
}

static const struct of_device_id mscc_of_match[] __initconst = {
	{
		.compatible = "mscc,ocelot",
		.data	    = &ocelot_props,
	},{
		.compatible = "mscc,luton",
		.data	    = &luton_props,
	},{
		.compatible = "mscc,serval",
		.data	    = &serval_props,
	},{
		.compatible = "mscc,jr2",
		.data	    = &jr2_props,
	},{
		.compatible = "mscc,servalt",
		.data	    = &jr2_props,
	},{
	}
};

MIPS_MACHINE(ocelot) = {
	.fixup_fdt = ocelot_fixup_fdt,
	.matches = mscc_of_match,
	.detect = mscc_mips_detect,
#if defined(CONFIG_MIPS_RAW_APPENDED_DTB)
	.fdt = __appended_dtb,
#endif
};

/*
 * MIPS VCOREIII common initialization
 * handling the command line and the environment
 * variables passed from the bootloader
 */
void __init prom_init(void)
{
	fw_init_cmdline();

#ifdef CONFIG_BLK_DEV_INITRD
	/* Read the initrd address from the firmware environment */
	initrd_start = fw_getenvl("initrd_start");
	if (initrd_start) {
		//strcat(arcs_cmdline, " root=/dev/ram0");
		initrd_start = KSEG0ADDR(initrd_start);
		initrd_end = initrd_start + fw_getenvl("initrd_size");
	}
#endif

	if ((strstr(arcs_cmdline, "mem=")) == NULL) {
		unsigned long memsize = fw_getenvl("memsize");
		/* Some bootloaders disagree on size in bytes/Mbytes */
		if (memsize > 0 && memsize < SZ_1M) {
			memsize *= SZ_1M;
		}
		if (memsize) {
			unsigned long mmapsize = fw_getenvl("memmap"); /* Note: Always in bytes */
			if (mmapsize) {
				/* Reserve the 'memmap' part off the memory end */
				memsize -= mmapsize;
				/* Update the vcfw driver with this */
				vcoreiii_memmap_start = (unsigned long)phys_to_virt(memsize);
				vcoreiii_memmap_size = mmapsize;
				pr_debug("%s: vcfw start: 0x%lx, size: %zu\n", __func__, vcoreiii_memmap_start, vcoreiii_memmap_size);
			}
			/* Add directly as memory region */
			memblock_add(0x00000000, memsize);
			pr_debug("%s: add memory: %lu\n", __func__, memsize);
		} else {
			/* Reasonable default */
			strcat(arcs_cmdline, " mem=128M");
		}
	}
	if ((strstr(arcs_cmdline, "console=")) == NULL)
		strcat(arcs_cmdline, " console=ttyS0,115200");
	plat_get_fdt();
	BUG_ON(!fdt);
}

void __init plat_mem_setup(void)
{
	if (mach && mach->fixup_fdt)
		fdt = mach->fixup_fdt(fdt, mach_match_data);

	__dt_setup_arch((void *)fdt);
	strlcat(boot_command_line, arcs_cmdline, COMMAND_LINE_SIZE);
}

static int __init ocelot_time_init(struct device_node *timer)
{
	return 0;
}

TIMER_OF_DECLARE(ocelot_timer, "mchp,mips-timer", ocelot_time_init);
