# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_NET_DSA_MICROCHIP_LAN9645X) += mchp-lan9645x.o

mchp-lan9645x-objs := lan9645x_main.o \
	lan9645x_phylink.o \
	lan9645x_npi.o \
	lan9645x_mac.o \
	lan9645x_vlan.o \
	lan9645x_lag.o \
	lan9645x_mdb.o \
	lan9645x_stats.o  \
	lan9645x_vcap_impl.o \
	lan9645x_vcap_ag_api.o \
	lan9645x_vcap_utils.o \
	lan9645x_stream.o \
	lan9645x_hsr_prp.o \
	lan9645x_mirror.o \
	lan9645x_police.o \
	lan9645x_tc_matchall.o \
	lan9645x_qos.o \
	lan9645x_tc_flower.o \
	lan9645x_cbs.o \
	lan9645x_mqprio.o \
	lan9645x_tbf.o lan9645x_ets.o \
	lan9645x_cut_thru.o \
	lan9645x_eee.o \
	lan9645x_netlink_qos.o \
	lan9645x_netlink_frer.o \
	lan9645x_netlink_frer_impl.o \
	lan9645x_ptp.o \
	lan9645x_dcb.o \
	lan9645x_taprio.o


mchp-lan9645x-$(CONFIG_DEBUG_FS) += lan9645x_vcap_debugfs.o

# Provide include files
ccflags-y += -I$(srctree)/drivers/net/ethernet/microchip/vcap
