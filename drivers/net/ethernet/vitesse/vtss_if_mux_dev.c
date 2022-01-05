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

#include <linux/skbuff.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/rtnetlink.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>
#include <linux/etherdevice.h>
#include <net/switchdev.h>
#include <linux/proc_fs.h>

#include "vtss_if_mux.h"

static struct proc_dir_entry *proc_dump_ifh = 0;

static int debug_dump_ifh_(struct seq_file *s, void *v)
{
    unsigned int i;

    seq_printf(s, "IFH (port): ");

    for (i = 0; i < vtss_if_mux_chip->ifh_encap_port_len; i++) {
        seq_printf(s, "%02x", vtss_if_mux_chip->hdr_tmpl_port[i]);
    }

    seq_printf(s, "\nIFH (vlan): ");

    // Exclude VLAN tag placeholder
    for (i = 0; i < vtss_if_mux_chip->ifh_encap_vlan_len - 4; i++) {
        seq_printf(s, "%02x", vtss_if_mux_chip->hdr_tmpl_vlan[i]);
    }

    seq_printf(s, "\nPort-Mask-Offset: %u\n", vtss_if_mux_chip->ifh_offs_port_mask);

    return 0;
}

static int debug_dump_ifh(struct inode *inode, struct file *f)
{
        return single_open(f, debug_dump_ifh_, NULL);
}

static const struct proc_ops dump_ifh_fops = {
    .proc_open    = debug_dump_ifh,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int internal_dev_open(struct net_device *netdev) {
    struct vtss_if_mux_dev_priv *priv = vtss_if_mux_dev_priv(netdev);

    if (priv->port_if) {
        //pr_info("internal_dev_open, dev: %p, port: %u\n", netdev, priv->port);
    } else {
        //pr_info("internal_dev_open, dev: %p, vlan_id: %u\n", netdev, priv->vlan_id);
        if (vtss_if_mux_vlan_up[priv->vlan_id]) {
            netif_carrier_on(netdev);
        } else {
            netif_carrier_off(netdev);
        }
    }
    return 0;
}

static int internal_dev_xmit(struct sk_buff *skb, struct net_device *netdev) {
    int tx_ok = 1;
    unsigned char *hdr;
    unsigned int ret, len;
    struct vtss_if_mux_pcpu_stats *stats;
    struct vtss_if_mux_dev_priv *priv = vtss_if_mux_dev_priv(netdev);
    int ifh_encap_len = (priv->port_if ?
                         vtss_if_mux_chip->ifh_encap_port_len :
                         vtss_if_mux_chip->ifh_encap_vlan_len);

    if (!vtss_if_mux_parent_dev) {
        tx_ok = 0;
        //pr_info("No parent device\n");
        goto DO_CNT;
    }

    if (skb_headroom(skb) < ifh_encap_len) {
        tx_ok = 0;
        pr_info("Not enough room for VTSS-header: %u\n",
               skb_headroom(skb));
        goto DO_CNT;
    }

    if (!vtss_if_mux_chip->internal_cpu) {
            // Make sure the original frame is at least 60 bytes long (w/o FCS), because
            // the NIC driver cannot see if we are transmitting an undersize frame,
            // once we have added the NPI encapsulation header, so it's not able to pad
            // it itself. When the frame arrives on the switch's NPI-port, the switch
            // strips the NPI header and IFH, and - bam - possible continues working
            // with an undersized frame.
            if (skb->len < ETH_ZLEN) {
                    u32 missing = ETH_ZLEN - skb->len;
                    // Add IFH ethernet encapsulation header
                    memset(skb_put(skb, missing), 0, missing);
            }
    }

#if 0
    pr_info("TX1 %u bytes on %s %u\n", skb->len, priv->port_if ? "port" : "vlan", priv->port_if ? priv->port : priv->vlan_id);
    print_hex_dump(KERN_INFO, "TX1: ", DUMP_PREFIX_OFFSET, 16, 1,
                   skb->data, skb->len, false);
#endif

    // Make room for the VTSS-IFH
    hdr = skb_push(skb, ifh_encap_len);
    if (priv->port_if) {
        vtss_if_mux_chip->set_port_value(vtss_if_mux_chip, hdr, priv->port);
    } else {
        vtss_if_mux_chip->set_vlan_value(vtss_if_mux_chip, hdr, skb, priv->vlan_id);
    }

    len = skb->len;
    skb->dev = vtss_if_mux_parent_dev;

#if 0
    pr_info("TX3 %u bytes on %s %u\n", len, priv->port_if ? "port" : "vlan", priv->port_if ? priv->port : priv->vlan_id);
    print_hex_dump(KERN_INFO, "TX3: ", DUMP_PREFIX_OFFSET, 16, 1,
                   skb->data, skb->len, false);
#endif

    ret = dev_queue_xmit(skb);


DO_CNT:
    stats = this_cpu_ptr(priv->vtss_if_mux_pcpu_stats);
    u64_stats_update_begin(&stats->syncp);
    if (likely(tx_ok)) {
        if (likely(ret == NET_XMIT_SUCCESS || ret == NET_XMIT_CN)) {
            stats->tx_packets++;
            stats->tx_bytes += len;
        } else {
            stats->tx_dropped++;
        }
    } else {
        stats->tx_errors++;
    }
    u64_stats_update_end(&stats->syncp);

    return 0;
}

static void internal_dev_get_stats(struct net_device *netdev, struct rtnl_link_stats64 *stats)
{
    if (vtss_if_mux_dev_priv(netdev)->vtss_if_mux_pcpu_stats) {
        int i;
        struct vtss_if_mux_pcpu_stats *p;

        for_each_possible_cpu(i) {
            u64 rx_packets, rx_bytes, rx_multicast, rx_dropped, rx_errors;
            u64 tx_packets, tx_bytes,               tx_dropped, tx_errors;

            unsigned int start;
            p = per_cpu_ptr(
                    vtss_if_mux_dev_priv(netdev)->vtss_if_mux_pcpu_stats, i);
            do {
                start = u64_stats_fetch_begin(&p->syncp);
                rx_packets   = p->rx_packets;
                rx_bytes     = p->rx_bytes;
                rx_multicast = p->rx_multicast;
                rx_dropped   = p->rx_dropped;
                rx_errors    = p->rx_errors;
                tx_packets   = p->tx_packets;
                tx_bytes     = p->tx_bytes;
                tx_dropped   = p->tx_dropped;
                tx_errors    = p->tx_errors;
            } while (u64_stats_fetch_retry(&p->syncp, start));

            stats->rx_packets    += rx_packets;
            stats->rx_bytes      += rx_bytes;
            stats->multicast     += rx_multicast;
            stats->rx_dropped    += rx_dropped;
            stats->rx_errors     += rx_errors;
            stats->tx_packets    += tx_packets;
            stats->tx_bytes      += tx_bytes;
            stats->tx_dropped    += tx_dropped;
            stats->tx_errors     += tx_errors;
        }
    }
}

static int internal_dev_change_mtu(struct net_device *netdev, int new_mtu)
{
    pr_info("internal_dev_change_mtu\n");
    if (new_mtu < 68)
        return -EINVAL;

    netdev->mtu = new_mtu;
    return 0;
}


/*
 * I/F mux network devices have devices nesting below it, and are a special
 * "super class" of normal network devices; split their locks off into a
 * separate class since they always nest.
 */
static struct lock_class_key internal_netdev_xmit_lock_key;
static struct lock_class_key internal_netdev_addr_lock_key;

static void internal_dev_set_lockdep_one(struct net_device *dev,
                                         struct netdev_queue *txq,
                                         void *unused)
{
   lockdep_set_class(&txq->_xmit_lock, &internal_netdev_xmit_lock_key);
}

static void internal_dev_set_lockdep_class(struct net_device *dev)
{
    lockdep_set_class(&dev->addr_list_lock, &internal_netdev_addr_lock_key);
    netdev_for_each_tx_queue(dev, internal_dev_set_lockdep_one, NULL);
}

static int internal_dev_init(struct net_device *dev) {
    int i;
    struct vtss_if_mux_dev_priv *priv = vtss_if_mux_dev_priv(dev);

    netif_carrier_off(dev);

    priv->fdb_dump_pending = false;

    priv->vtss_if_mux_pcpu_stats =
            alloc_percpu(struct vtss_if_mux_pcpu_stats);

    if (!priv->vtss_if_mux_pcpu_stats)
        return -ENOMEM;

    internal_dev_set_lockdep_class(dev);

    for_each_possible_cpu(i) {
        struct vtss_if_mux_pcpu_stats *s;
        s = per_cpu_ptr(priv->vtss_if_mux_pcpu_stats, i);
        u64_stats_init(&s->syncp);
    }

    return 0;
}

static void internal_dev_uninit(struct net_device *dev)
{
}

static void rt_notify_task(struct work_struct *work) {
    int i;
    struct net_device *dev = 0;

    rtnl_lock();

    for (i = 0; i < VTSS_IF_MUX_PORT_CNT; ++i) {
        dev = vtss_if_mux_vlan_net_dev[i];
        if (dev) {
            vtss_if_mux_rt_notify(dev);
        }
    }

    for (i = 0; i < VLAN_N_VID; ++i) {
        dev = vtss_if_mux_vlan_net_dev[i];
        if (dev) {
            vtss_if_mux_rt_notify(dev);
        }
    }

    rtnl_unlock();
}

static DECLARE_WORK(rt_notify_work, rt_notify_task);

int vtss_if_mux_dev_init(void) {
    proc_dump_ifh = proc_create("vtss_if_mux_ifh", S_IRUGO, NULL, &dump_ifh_fops);

    return 0;
}

void vtss_if_mux_dev_uninit(void)
{
    cancel_work_sync(&rt_notify_work);

    if (proc_dump_ifh)
        proc_remove(proc_dump_ifh);
}

static void internal_dev_set_rx_mode(struct net_device *dev) {
    struct vtss_if_mux_dev_priv *priv = vtss_if_mux_dev_priv(dev);

    if (!priv->fdb_dump_pending) {
        priv->fdb_dump_pending = true;
        schedule_work(&rt_notify_work);
    }
}

static void internal_dev_destructor(struct net_device *dev) {
    struct vtss_if_mux_dev_priv *priv = vtss_if_mux_dev_priv(dev);

    free_percpu(priv->vtss_if_mux_pcpu_stats);
    priv->vtss_if_mux_pcpu_stats = NULL;
    free_netdev(dev);
}

static int internal_dev_fdb_dump(struct sk_buff *skb,
                                 struct netlink_callback *cb,
                                 struct net_device *dev,
                                 struct net_device *filter_dev,
                                 int *idx) {
    struct vtss_if_mux_dev_priv *priv = vtss_if_mux_dev_priv(dev);

    // Re-arm the notification mechanism
    priv->fdb_dump_pending = false;
    return ndo_dflt_fdb_dump(skb, cb, dev, filter_dev, idx);
}

static int internal_dev_set_mac_address(struct net_device *dev, void *p) {
    struct sockaddr *addr = p;

    if (!is_valid_ether_addr(addr->sa_data))
        return -EADDRNOTAVAIL;

    eth_hw_addr_set(dev, addr->sa_data);
    return 0;
}

static int internal_dev_change_carrier(struct net_device *dev, bool new_carrier) {
    if (new_carrier) {
        netif_carrier_on(dev);
    } else {
        netif_carrier_off(dev);
    }
    return 0;
}

static const struct net_device_ops internal_dev_netdev_ops = {
    .ndo_init                  = internal_dev_init,
    .ndo_uninit                = internal_dev_uninit,
    .ndo_open                  = internal_dev_open,
    //.ndo_stop: Not used
    .ndo_start_xmit            = internal_dev_xmit,
    .ndo_change_mtu            = internal_dev_change_mtu,
    .ndo_get_stats64           = internal_dev_get_stats,
    .ndo_set_rx_mode           = internal_dev_set_rx_mode,
    .ndo_set_mac_address       = internal_dev_set_mac_address,
    .ndo_fdb_dump              = internal_dev_fdb_dump,
    .ndo_change_carrier        = internal_dev_change_carrier,
};

void vtss_if_mux_setup(struct net_device *netdev) {
    struct net_device *parent_dev = vtss_if_mux_parent_dev_get();
    unsigned short    parent_headroom;
    unsigned short    parent_tailroom;
    int ifh_encap_len;

    ether_setup(netdev);
    netdev->netdev_ops = &internal_dev_netdev_ops;

    if (parent_dev) {
        parent_headroom = parent_dev->needed_headroom;
        parent_tailroom = parent_dev->needed_tailroom;
    } else {
        parent_headroom = 0;
        parent_tailroom = 0;
    }
    ifh_encap_len = max(vtss_if_mux_chip->ifh_encap_port_len,
                        vtss_if_mux_chip->ifh_encap_vlan_len);

    // We add injection header plus 4 byte VLAN tag
    netdev->needed_headroom = ifh_encap_len + parent_headroom;
    netdev->needed_tailroom = parent_tailroom;

    netdev->priv_destructor = internal_dev_destructor;
    netdev->priv_flags |= IFF_UNICAST_FLT;
    netdev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
    netdev->priv_flags &= ~(IFF_XMIT_DST_RELEASE | IFF_TX_SKB_SHARING);
    netdev->tx_queue_len = 0;
}
