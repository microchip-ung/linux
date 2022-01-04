/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/ {
	images {
		fdt@jaguar2_pcb110 {
			description = "MSCC Jaguar2 PCB110 Device Tree";
			data = /incbin/("boot/dts/mscc/jaguar2_pcb110.dtb");
			type = "flat_dt";
			arch = "mips";
			compression = "none";
			hash@0 {
				algo = "sha1";
			};
		};
		fdt@jaguar2_pcb111 {
			description = "MSCC Jaguar2 PCB111 Device Tree";
			data = /incbin/("boot/dts/mscc/jaguar2_pcb111.dtb");
			type = "flat_dt";
			arch = "mips";
			compression = "none";
			hash@0 {
				algo = "sha1";
			};
		};
		fdt@serval2_pcb112 {
			description = "MSCC Serval2 PCB112 Device Tree";
			data = /incbin/("boot/dts/mscc/serval2_pcb112.dtb");
			type = "flat_dt";
			arch = "mips";
			compression = "none";
			hash@0 {
				algo = "sha1";
			};
		};
	};

	configurations {
		pcb110 {
			description = "Jaguar2 Linux kernel";
			kernel = "kernel@0";
			fdt = "fdt@jaguar2_pcb110";
			ramdisk = "ramdisk";
		};
		pcb111 {
			description = "Jaguar2 Linux kernel";
			kernel = "kernel@0";
			fdt = "fdt@jaguar2_pcb111";
			ramdisk = "ramdisk";
		};
		pcb112 {
			description = "Serval2 Linux kernel";
			kernel = "kernel@0";
			fdt = "fdt@serval2_pcb112";
			ramdisk = "ramdisk";
		};
	};
};
