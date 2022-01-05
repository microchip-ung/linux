/* Copyright (c) 2015 Microsemi Corporation

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/


#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/u64_stats_sync.h>

#include "vtss_ifh.h"

struct ifmux_chip {
	u8 soc;
	u8 ifh_id;
	u8 ifh_len;
	u8 cpu_port;
	u16 ifh_offs_port_mask;
	u8 ifh_encap_len;
	const u8 *hdr_tmpl_vlan;
	const u8 *hdr_tmpl_port;
	size_t ifh_encap_vlan_len;
	size_t ifh_encap_port_len;
	void (*set_port_value)(struct ifmux_chip *cfg, u8 *hdr, u16 port);
	void (*set_vlan_value)(struct ifmux_chip *cfg, u8 *hdr, struct sk_buff *skb, u16 vlan);
	bool internal_cpu;
};

struct vtss_if_mux_pcpu_stats {
    u64                         rx_packets;
    u64                         rx_bytes;
    u64                         rx_multicast;
    u64                         rx_dropped;
    u64                         rx_errors;

    u64                         tx_packets;
    u64                         tx_bytes;
    u64                         tx_dropped;
    u64                         tx_errors;

    struct u64_stats_sync       syncp;
};

struct vtss_if_mux_dev_priv {
    bool                                    port_if;
    u16                                     port;
    u16                                     vlan_id;
    struct vtss_if_mux_pcpu_stats __percpu *vtss_if_mux_pcpu_stats;
    bool                                    fdb_dump_pending;
};

static inline struct vtss_if_mux_dev_priv *vtss_if_mux_dev_priv(
        const struct net_device *dev) {
    return netdev_priv(dev);
}

// Number of chip ports
#define VTSS_IF_MUX_PORT_CNT 66

extern bool vtss_if_mux_nl_notify_pending;
extern struct ifmux_chip *vtss_if_mux_chip;
extern struct net_device *vtss_if_mux_parent_dev;
extern struct net_device *vtss_if_mux_vlan_net_dev[VLAN_N_VID];
extern struct net_device *vtss_if_mux_port_net_dev[VTSS_IF_MUX_PORT_CNT];
extern int vtss_if_mux_vlan_up[VLAN_N_VID];

void vtss_if_mux_setup(struct net_device *netdev);

int vtss_if_mux_netlink_init(void);
void vtss_if_mux_netlink_uninit(void);
int vtss_if_mux_dev_init(void);
void vtss_if_mux_dev_uninit(void);

int vtss_if_mux_genetlink_init(void);
void vtss_if_mux_genetlink_uninit(void);

rx_handler_result_t vtss_if_mux_rx_handler(struct sk_buff **pskb);
struct net_device *vtss_if_mux_parent_dev_get(void);

void vtss_if_mux_rt_notify(struct net_device *dev);

int vtss_if_mux_filter_apply(struct sk_buff *skb, unsigned int vid, unsigned int
                             ether_type_offset);
int vtss_if_mux_filter_vlan_pop(u32 vid, u32 chip_port, u32 etype_outer, u32 etype_inner);

u16 vtss_if_mux_vsi2vid(u16 vsi);
