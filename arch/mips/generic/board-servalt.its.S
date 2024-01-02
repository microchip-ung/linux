/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/ {
	images {
		fdt@servalt_pcb116 {
			description = "MSCC Servalt PCB116 Device Tree";
			data = /incbin/("boot/dts/mscc/servalt_pcb116.dtb");
			type = "flat_dt";
			arch = "mips";
			compression = "none";
			hash@0 {
				algo = "sha1";
			};
		};
	};

	configurations {
		pcb116 {
			description = "Servalt Linux kernel";
			kernel = "kernel@0";
			fdt = "fdt@servalt_pcb116";
			ramdisk = "ramdisk";
		};
	};
};
