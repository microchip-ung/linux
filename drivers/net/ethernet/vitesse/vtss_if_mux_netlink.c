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

/*
 *  Vitesse Switch Software.
 *
 */

#include <net/rtnetlink.h>
#include <linux/if_vlan.h>
#include <linux/netdevice.h>

#include "vtss_if_mux.h"

static const struct nla_policy vtss_if_mux_policy[IFLA_VLAN_MAX + 1] = {
    [IFLA_VLAN_ID] = { .type = NLA_U16 },
};

static int vlan_id_from_ifname(struct nlattr *tb[]) {
    int vlan_id = -1;

    if (!tb[IFLA_IFNAME])
        return -1;

    if (nla_len(tb[IFLA_IFNAME]) > IFNAMSIZ) {
        pr_info("err 1\n");
        return -1;
    }

    if (sscanf(nla_data(tb[IFLA_IFNAME]), "vtss.vlan.%d", &vlan_id) != 1) {
        pr_info("vlan_id_from_ifname: failed when looking in %s\n", (char *)nla_data(tb[IFLA_IFNAME]));
        return -1;
    }

    if (vlan_id <= 0 || vlan_id >= VLAN_N_VID)
        return -1;

    return vlan_id;
}

static int port_from_ifname(struct nlattr *tb[]) {
    int port;

    if (!tb[IFLA_IFNAME])
        return -1;

    if (nla_len(tb[IFLA_IFNAME]) > IFNAMSIZ) {
        pr_info("err 1\n");
        return -1;
    }

    if (sscanf(nla_data(tb[IFLA_IFNAME]), "vtss.port.%d", &port) != 1) {
        pr_info("port_from_ifname: failed when looking in %s\n", (char *)nla_data(tb[IFLA_IFNAME]));
        return -1;
    }

    if (port >= VTSS_IF_MUX_PORT_CNT)
        return -1;

    return port;
}

static int vtss_if_mux_validate(struct nlattr *tb[], struct nlattr *data[],
                                struct netlink_ext_ack *extack) {
    if (vlan_id_from_ifname(tb) == -1 &&
        port_from_ifname(tb) == -1) {
        pr_info("vtss_if_mux_validate: vlan_id_from_ifname and port_from_ifname failed\n");
        return -EINVAL;
    }

    return 0;
}

static int vtss_if_mux_changelink(struct net_device *dev,
                                  struct nlattr *tb[], struct nlattr *data[],
                                  struct netlink_ext_ack *extack) {
    pr_info("vtss_if_mux_changelink\n");

    return 0;
}

static int vtss_if_mux_newlink(struct net *src_net, struct net_device *dev,
                               struct nlattr *tb[], struct nlattr *data[],
                               struct netlink_ext_ack *extack) {
    int err;
    int vlan_id;
    int port;
    struct net_device *parent_dev;
    struct vtss_if_mux_dev_priv *priv = vtss_if_mux_dev_priv(dev);

    pr_info("vtss_if_mux_newlink %p\n", dev);
    port = port_from_ifname(tb);
    vlan_id = vlan_id_from_ifname(tb);

    if (port >= 0) {
        //pr_info("port = %d\n", port);
        if (vtss_if_mux_port_net_dev[port]) {
            pr_info("port exists already: %d\n", port);
            return -EEXIST;
        }
        priv->port_if = 1;

    } else if (vlan_id >= 0) {
        if (vtss_if_mux_vlan_net_dev[vlan_id]) {
            pr_info("vlan exists already: %d\n", vlan_id);
            return -EEXIST;
        }
        priv->port_if = 0;

    } else {
        pr_info("No ID found\n");
        return -ENODEV;
    }


    parent_dev = vtss_if_mux_parent_dev_get();
    if (!parent_dev) {
        pr_info("No parent device\n");
        return -ENODEV;
    }

    //pr_info("reg new device\n");
    err = register_netdevice(dev);
    if (err != 0) {
        pr_info("Failed to register device\n");
        goto exit_rx_unreg;
    }

    if (priv->port_if) {
        priv->port = port;
        vtss_if_mux_port_net_dev[port] = dev;
        netif_carrier_on(dev);
        pr_info("vtss_if_mux_newlink port=%u, addr=%p\n", port, dev);
    } else {
        priv->vlan_id = vlan_id;
        vtss_if_mux_vlan_net_dev[vlan_id] = dev;

        //netif_stacked_transfer_operstate(parent_dev, dev);
        if (vtss_if_mux_vlan_up[vlan_id]) {
            netif_carrier_on(dev);
        }
        pr_info("vtss_if_mux_newlink vlan=%u, addr=%p\n", vlan_id, dev);
    }

    return 0;

exit_rx_unreg:
    rtnl_lock();
    netdev_rx_handler_unregister(parent_dev);
    rtnl_unlock();

    return err;
}

static size_t vtss_if_mux_get_size(const struct net_device *dev) {
    return nla_total_size(2) +    /* IFLA_VLAN_PROTOCOL */
           nla_total_size(2);     /* IFLA_VLAN_ID */
}

/*
static int vtss_if_mux_fill_info(struct sk_buff *skb,
                                 const struct net_device *dev) {
    //pr_info("vtss_if_mux_fill_info\n");
    return 0;
}
*/

static int vtss_if_mux_rt_notify_fill(struct sk_buff *skb,
                                      struct net_device *dev) {
    struct nlmsghdr *nlh;
    struct ndmsg *ndm;

    nlh = nlmsg_put(skb, 0, 0, RTM_GETNEIGH, sizeof(*ndm), 0);
    if (nlh == NULL)
        return -EMSGSIZE;

    ndm = nlmsg_data(nlh);
    ndm->ndm_family  = AF_UNSPEC;
    ndm->ndm_pad1    = 0;
    ndm->ndm_pad2    = 0;
    ndm->ndm_flags   = NTF_SELF;
    ndm->ndm_type    = RTM_GETNEIGH;
    ndm->ndm_ifindex = dev->ifindex;
    ndm->ndm_state   = 0;

    nlmsg_end(skb, nlh);

    return skb->len;
}

void vtss_if_mux_rt_notify(struct net_device *dev) {
    int err;
    struct net *net = dev_net(dev);
    struct sk_buff *skb;

    skb = nlmsg_new(1024, GFP_KERNEL); // TODO
    if (skb == NULL) {
        pr_info("ALLOC ERROR\n");
        return;
    }

    err = vtss_if_mux_rt_notify_fill(skb, dev);

    if (err < 0) {
        pr_info("vtss_if_mux_rt_notify_fill ERROR\n");
        kfree_skb(skb);
        goto errout;
    }

    rtnl_notify(skb, net, 0, RTNLGRP_NEIGH, NULL, GFP_KERNEL);
    return;

errout:
    rtnl_set_sk_err(net, RTNLGRP_NOTIFY, err);
}

static void vtss_if_mux_dellink(struct net_device *dev, struct list_head *head) {
    int i;

    for (i = 0; i < VTSS_IF_MUX_PORT_CNT; ++i) {
        if (vtss_if_mux_port_net_dev[i] == dev) {
            vtss_if_mux_port_net_dev[i] = 0;
        }
    }

    for (i = 0; i < VLAN_N_VID; ++i) {
        if (vtss_if_mux_vlan_net_dev[i] == dev) {
            vtss_if_mux_vlan_net_dev[i] = 0;
        }
    }

    pr_info("unregister_vlan_dev\n");
    unregister_netdevice_queue(dev, head);
}

struct rtnl_link_ops vtss_if_mux_link_ops __read_mostly = {
    .kind         = "vtss_if_mux",
    .maxtype      = IFLA_VLAN_MAX,
    .policy       = vtss_if_mux_policy,
    .priv_size    = sizeof(struct vtss_if_mux_dev_priv),
    .setup        = vtss_if_mux_setup,
    .validate     = vtss_if_mux_validate,
    .newlink      = vtss_if_mux_newlink,
    .changelink   = vtss_if_mux_changelink,
    .dellink      = vtss_if_mux_dellink,
    .get_size     = vtss_if_mux_get_size,
    //.fill_info    = vtss_if_mux_fill_info,
};

int vtss_if_mux_netlink_init(void) {
    return rtnl_link_register(&vtss_if_mux_link_ops);
}

void vtss_if_mux_netlink_uninit(void) {
    rtnl_link_unregister(&vtss_if_mux_link_ops);
}

//MODULE_ALIAS_RTNL_LINK("vtss_if_mux");
