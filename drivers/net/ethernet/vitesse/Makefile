# Copyright (c) 2015 Microsemi Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.


#
# Makefile for the network device drivers.
#

ccflags-$(CONFIG_VC3FDMA) := -I$(srctree)/drivers/net/ethernet/vitesse/ufdma/include

# We want all warnings to produce an error and lead to a failed build.
ccflags-$(CONFIG_VC3FDMA) += -Werror
# Level-1 warning flags
ccflags-$(CONFIG_VC3FDMA) += -Wextra -Wunused -Wno-unused-parameter -Wno-type-limits
ccflags-$(CONFIG_VC3FDMA) += -Wmissing-declarations
#ccflags-$(CONFIG_VC3FDMA) += -Wmissing-format-attribute
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wmissing-prototypes)
ccflags-$(CONFIG_VC3FDMA) += -Wold-style-definition
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wmissing-include-dirs)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wunused-but-set-variable)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wunused-const-variable)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wpacked-not-aligned)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-disable-warning, missing-field-initializers)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-disable-warning, sign-compare)
# Level-2 warning flags
ccflags-$(CONFIG_VC3FDMA) += -Waggregate-return
#ccflags-$(CONFIG_VC3FDMA) += -Wcast-align
ccflags-$(CONFIG_VC3FDMA) += -Wdisabled-optimization
#ccflags-$(CONFIG_VC3FDMA) += -Wnested-externs
#ccflags-$(CONFIG_VC3FDMA) += -Wshadow
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wlogical-op)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wmissing-field-initializers)
#ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wsign-compare)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wmaybe-uninitialized)
ccflags-$(CONFIG_VC3FDMA) += $(call cc-option, -Wunused-macros)

obj-$(CONFIG_VC3FDMA) += vtss_vc3fdma.o

vtss_vc3fdma-y := vc3fdma.o ufdma/src/ail/ufdma.o
vtss_vc3fdma-$(CONFIG_VC3FDMA) += ufdma/src/luton26/ufdma_luton26.o
vtss_vc3fdma-$(CONFIG_VC3FDMA) += ufdma/src/serval/ufdma_serval.o
vtss_vc3fdma-$(CONFIG_VC3FDMA) += ufdma/src/jaguar2/ufdma_jaguar2.o

obj-$(CONFIG_VC3MUX)  += vtss_if_mux.o

vtss_if_mux-y := vtss_if_mux_main.o vtss_if_mux_dev.o vtss_if_mux_netlink.o vtss_if_mux_genetlink.o

obj-$(CONFIG_DYING_GASP) += 	vtss_dying_gasp_main.o \
				vtss_dying_gasp_genetlink.o
