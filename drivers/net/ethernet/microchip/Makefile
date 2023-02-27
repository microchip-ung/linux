# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Microchip network device drivers.
#

obj-$(CONFIG_ENC28J60) += enc28j60.o
obj-$(CONFIG_ENCX24J600) += encx24j600.o encx24j600-regmap.o
obj-$(CONFIG_LAN743X) += lan743x.o

lan743x-objs := lan743x_main.o lan743x_ethtool.o lan743x_ptp.o

obj-$(CONFIG_LAN966X_SWITCH) += lan966x/
obj-$(CONFIG_LAN966X_SWITCH_APPL) += lan966x/
obj-$(CONFIG_SPARX5_SWITCH) += sparx5/

obj-$(CONFIG_VCAP) += vcap/
obj-$(CONFIG_VCAP_KUNIT_TEST) += vcap/

obj-$(CONFIG_LAN966X_INTERNAL_SWITCH) += lan966x_common.o
lan966x_common-y := lan966x_main.o lan966x_switchdev.o\
			lan966x_ethtool.o lan966x_afi.o\
			lan966x_proc.o lan966x_prof.o\
			lan966x_vcap_impl.o lan966x_vcap_ag_api.o\
			lan966x_tc.o lan966x_tc_flower.o lan966x_tc_matchall.o\
			lan966x_tc_dbg.o lan966x_vcap_utils.o\
			lan966x_ptp.o lan966x_qos.o\
			lan966x_netlink_fp.o lan966x_netlink_frer.o\
			lan966x_mrp.o lan966x_cfm.o\
			lan966x_netlink_qos.o lan966x_phylink.o\
			lan966x_port.o lan966x_debugfs.o lan966x_dcbtool.o

obj-$(CONFIG_LAN966X_INTERNAL_SWITCH_AD) += lan966x_common_ad.o
lan966x_common_ad-y := lan966x_main_ad.o lan966x_switchdev_ad.o\
			lan966x_ethtool_ad.o lan966x_afi_ad.o\
			lan966x_proc_ad.o lan966x_prof_ad.o\
			lan966x_vcap_ad.o lan966x_vcap_utils_ad.o lan966x_vcap_ag_api_ad.o\
			lan966x_ptp_ad.o lan966x_qos_ad.o\
			lan966x_tc_ad.o lan966x_tc_dbg_ad.o\
			lan966x_tc_flower_ad.o lan966x_tc_matchall_ad.o\
			lan966x_tc_flower_is1_ad.o lan966x_tc_flower_is2_ad.o\
			lan966x_tc_flower_es0_ad.o\
			lan966x_netlink_fp_ad.o lan966x_netlink_frer_ad.o\
			lan966x_netlink_vcap_ad.o\
			lan966x_mrp_ad.o lan966x_cfm_ad.o\
			lan966x_netlink_qos_ad.o lan966x_phylink_ad.o\
			lan966x_port_ad.o lan966x_debugfs_ad.o lan966x_dcbtool_ad.o

obj-$(CONFIG_LAN966X_INTERNAL_SWITCH_SR) += lan966x_common_sr.o
lan966x_common_sr-y := lan966x_main_sr.o lan966x_switchdev_sr.o\
			lan966x_ethtool_sr.o lan966x_afi_sr.o\
			lan966x_proc_sr.o lan966x_prof_sr.o\
			lan966x_vcap_sr.o lan966x_vcap_utils_sr.o lan966x_vcap_ag_api_sr.o\
			lan966x_ptp_sr.o lan966x_qos_sr.o\
			lan966x_tc_sr.o lan966x_tc_dbg_sr.o\
			lan966x_tc_flower_sr.o lan966x_tc_matchall_sr.o\
			lan966x_tc_flower_is1_sr.o lan966x_tc_flower_is2_sr.o\
			lan966x_tc_flower_es0_sr.o\
			lan966x_netlink_fp_sr.o	lan966x_netlink_frer_sr.o\
			lan966x_netlink_vcap_sr.o\
			lan966x_mrp_sr.o lan966x_cfm_sr.o \
			lan966x_netlink_qos_sr.o lan966x_phylink_sr.o\
			lan966x_port_sr.o lan966x_debugfs_sr.o lan966x_dcbtool_sr.o

obj-$(CONFIG_LAN966X_INTERNAL_SWITCH_VTSS) += lan966x_common_vtss.o
lan966x_common_vtss-y := lan966x_main_vtss.o

# Enable Debug
ccflags-$(CONFIG_LAN966X_INTERNAL_DEBUG) += -DDEBUG

# Provide include files
ccflags-y += -I$(srctree)/drivers/net/ethernet/microchip/vcap
