# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Microchip Sparx5 network device drivers.
#

obj-$(CONFIG_SPARX5_SWITCH) += sparx5-switch.o

sparx5-switch-y  := sparx5_main.o \
		    sparx5_packet.o \
		    sparx5_netdev.o \
		    sparx5_phylink.o \
		    sparx5_port.o \
		    sparx5_mactable.o \
		    sparx5_vlan.o \
		    sparx5_switchdev.o \
		    sparx5_calendar.o \
		    sparx5_ethtool.o \
		    sparx5_fdma.o \
		    sparx5_ptp.o \
		    sparx5_pgid.o \
		    sparx5_tc.o \
		    sparx5_qos.o \
		    sparx5_vcap_impl.o \
		    sparx5_vcap_ag_api.o \
		    sparx5_tc_flower.o \
		    sparx5_tc_matchall.o \
		    sparx5_pool.o \
		    sparx5_sdlb.o \
		    sparx5_police.o \
		    sparx5_psfp.o \
		    sparx5_mirror.o \
		    sparx5_netlink_qos.o \
		    sparx5_netlink_fp.o \
		    sparx5_regs.o \
		    sparx5_router.o \
		    sparx5_afi.o \
		    sparx5_mrp.o \
		    sparx5_mtu.o \
		    sparx5_xdp.o

sparx5-switch-$(CONFIG_SPARX5_DCB) += sparx5_dcb.o

sparx5-switch-$(CONFIG_DEBUG_FS) += sparx5_vcap_debugfs.o \
				    sparx5_qos_debugfs.o \
				    sparx5_debugfs.o

# Allow debugging
ccflags-$(CONFIG_SPARX5_DEBUG) += -DDEBUG
sparx5-switch-$(CONFIG_SPARX5_DEBUG) += sparx5_tc_dbg.o

obj-$(CONFIG_SPARX5_SWITCH_APPL) += sparx5-appl.o
sparx5-appl-y := sparx5_main_appl.o

obj-$(CONFIG_LAN969X_SWITCH) += lan969x-switch.o

lan969x-switch-y := lan969x/lan969x_regs.o \
		    lan969x/lan969x.o \
		    lan969x/lan969x_vcap_impl.o \
		    lan969x/lan969x_vcap_ag_api.o \
		    lan969x/lan969x_fdma.o \
		    lan969x/lan969x_calendar.o \
		    lan969x/lan969x_redbox.o

lan969x-switch-$(CONFIG_MFD_LAN969X_PCI) += lan969x/lan969x_fdma_pci.o

# Provide include files
ccflags-y += -I$(srctree)/drivers/net/ethernet/microchip/vcap
ccflags-y += -I$(srctree)/drivers/net/ethernet/microchip/fdma
ccflags-y += -I$(srctree)/drivers/net/ethernet/microchip/afi
ccflags-y += -I$(srctree)/drivers/net/ethernet/microchip/mrp
