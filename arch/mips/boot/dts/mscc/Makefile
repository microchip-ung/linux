# SPDX-License-Identifier: GPL-2.0-only
dtb-$(CONFIG_MSCC_OCELOT)	+= ocelot_pcb123.dtb ocelot_pcb120.dtb
dtb-$(CONFIG_MSCC_LUTON)	+= luton_pcb090.dtb luton_pcb091.dtb
dtb-$(CONFIG_MSCC_SERVAL)	+= serval_pcb105.dtb serval_pcb106.dtb
dtb-$(CONFIG_MSCC_JAGUAR2)	+= jaguar2_pcb110.dtb jaguar2_pcb111.dtb
dtb-$(CONFIG_MSCC_JAGUAR2)	+= jaguar2_pcb118.dtb serval2_pcb112.dtb
dtb-$(CONFIG_MSCC_SERVALT)	+= servalt_pcb116.dtb

obj-$(CONFIG_BUILTIN_DTB)	+= $(addsuffix .o, $(dtb-y))
