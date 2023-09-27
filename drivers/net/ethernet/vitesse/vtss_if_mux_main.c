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
 *  MicroSemi Switch Software.
 *
 */

#include <linux/skbuff.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/rtnetlink.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "vtss_if_mux.h"

/* Default parent interface name - DT: parent-interface = ".." */
static const char *parent_if_name = "vtss.ifh";

struct ifmux_chip *vtss_if_mux_chip;
struct net_device *vtss_if_mux_parent_dev;
struct net_device *vtss_if_mux_vlan_net_dev[VLAN_N_VID];
struct net_device *vtss_if_mux_port_net_dev[VTSS_IF_MUX_PORT_CNT];
int vtss_if_mux_vlan_up[VLAN_N_VID];

// Eth encap + IFH
#define IFH_ENCAP_LEN(x) (12+4+x)

// Ethernet encapslation
#define _encap(_x_)                                        \
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,                \
        0xfe, 0xff, 0xff, 0xff, 0xff, 0xff,                \
        0x88, 0x80, 0x00, _x_

// Vlan tag placeholder
#define _vlantag                                \
    0x00, 0x00, 0x00, 0x00

static const u8 hdr_tmpl_vlan_luton[IFH_ENCAP_LEN(IFH_LEN_LUTON)+4] = {
        _encap(IFH_ID_LUTON),
        0x00, 0x00, 0x28, 0x0f, 0x00, 0x40, 0x00, 0x01,
        _vlantag,
};

#define _ifh_tmpl_serval_family                                \
        0x00, 0x2e, 0xe5, 0x41, 0x16, 0x58, 0x02, 0x00, \
        0x00, 0x00, 0x28, 0x0f, 0x00, 0x40, 0x00, 0x01

static const u8 hdr_tmpl_vlan_serval1[IFH_ENCAP_LEN(IFH_LEN_SERVAL1)+4] = {
        _encap(IFH_ID_SERVAL1),
        _ifh_tmpl_serval_family,
        _vlantag,
};

static const u8 hdr_tmpl_vlan_ocelot[IFH_ENCAP_LEN(IFH_LEN_OCELOT)+4] = {
        _encap(IFH_ID_OCELOT),
        _ifh_tmpl_serval_family,
        _vlantag,
};

static const u8 hdr_tmpl_vlan_servalt[IFH_ENCAP_LEN(IFH_LEN_JAGUAR2)+4] = {
        _encap(IFH_ID_SERVALT),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01,
        0xa9, 0x00, 0x00, 0x00,
        _vlantag,
};

static const u8 hdr_tmpl_vlan_jaguar2[IFH_ENCAP_LEN(IFH_LEN_JAGUAR2)+4] = {
        _encap(IFH_ID_JAGUAR2),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01,
        0xa9, 0x00, 0x00, 0x00,
        _vlantag,
};

static const u8 hdr_tmpl_vlan_sparx5[IFH_ENCAP_LEN(IFH_LEN_SPARX5)+4] = {
        _encap(IFH_ID_SPARX5),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* v-rsv1 1 vq-ingr-drop-mode 1 */
        0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        /* f-update-fcs 1 f-src-port 65 m-pipeline-act 2 */
        0x00, 0x00, 0x00, 0x08, 0x00, 0x10, 0x48, 0x00,
        0x00, 0x00, 0x00, 0x00,
        _vlantag,
};

static const u8 hdr_tmpl_vlan_lan966x[IFH_ENCAP_LEN(IFH_LEN_LAN966X)+4] = {
        _encap(IFH_ID_LAN966X),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        _vlantag,
};

static const u8 hdr_tmpl_vlan_lan969x[IFH_ENCAP_LEN(IFH_LEN_LAN969X)+4] = {
        _encap(IFH_ID_LAN969X),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* v-rsv1 1 vq-ingr-drop-mode 1 */
        0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        /* f-update-fcs 1 f-src-port 30 m-pipeline-act 2 */
        0x00, 0x00, 0x00, 0x04, 0x7c, 0x07, 0x88, 0x00,
        0x00, 0x00, 0x00, 0x00,
        _vlantag,
};

static const u8 hdr_tmpl_port_luton[IFH_ENCAP_LEN(IFH_LEN_LUTON)] = {
        _encap(IFH_ID_LUTON),
        0x80, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, /* BYPASS=1, POP_CNT=3 */
};

#define _ifh_tmpl_port_serval_family                                        \
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* BYPASS=1 */        \
        0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00  /* POP_CNT=3 */

static const u8 hdr_tmpl_port_serval1[IFH_ENCAP_LEN(IFH_LEN_SERVAL1)] = {
        _encap(IFH_ID_SERVAL1),
        _ifh_tmpl_port_serval_family,
};

static const u8 hdr_tmpl_port_ocelot[IFH_ENCAP_LEN(IFH_LEN_OCELOT)] = {
        _encap(IFH_ID_OCELOT),
        _ifh_tmpl_port_serval_family,
};

#define _ifh_tmpl_port_jaguar2_family(b1, b2)           \
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
        /* VS.MUST_BE_1=1, VS.INGR_DROP_MODE=1 */        \
        0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, \
        /* UPDATE_FCS=1 */                                \
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,       \
        b1, b1,                                                \
        /* PL_ACT=1 (INJ), PL_PT=16 (ANA_DONE) */        \
        0xc0, 0x00, 0x00

static const u8 hdr_tmpl_port_servalt[IFH_ENCAP_LEN(IFH_LEN_JAGUAR2)] = {
        _encap(IFH_ID_SERVALT),
        // DST_MODE=3, SRC_PORT=11 (CPU), DO_NOT_REW=1
        _ifh_tmpl_port_jaguar2_family(0xc0, 0x5c),
};

static const u8 hdr_tmpl_port_jaguar2[IFH_ENCAP_LEN(IFH_LEN_JAGUAR2)] = {
        _encap(IFH_ID_JAGUAR2),
        // DST_MODE=3, SRC_PORT=53 (CPU), DO_NOT_REW=1
        _ifh_tmpl_port_jaguar2_family(0xc1, 0xac),
};

static const u8 hdr_tmpl_port_sparx5[IFH_ENCAP_LEN(IFH_LEN_SPARX5)] = {
        _encap(IFH_ID_SPARX5),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* v-rsv1 1 vq-ingr-drop-mode 1 */
        0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        /* f-update-fcs 1 f-src-port 65 f-do-not-rew m-pipeline-act 1 m-pipeline-pt 16 */
        0x00, 0x00, 0x00, 0x08, 0x00, 0x10, 0x66, 0x00,
        0x00, 0x00, 0x00, 0x00
};

static const u8 hdr_tmpl_port_lan966x[IFH_ENCAP_LEN(IFH_LEN_LAN966X)] = {
        _encap(IFH_ID_LAN966X),
        /* BYPASS = 1 */
        0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
};

static const u8 hdr_tmpl_port_lan969x[IFH_ENCAP_LEN(IFH_LEN_LAN969X)] = {
        _encap(IFH_ID_LAN969X),
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* v-rsv1 1 vq-ingr-drop-mode 1 */
        0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        /* f-update-fcs 1 f-src-port 30 f-do-not-rew m-pipeline-act 1 m-pipeline-pt 17 */
        0x00, 0x00, 0x00, 0x04, 0x00, 0x07, 0x86, 0x20,
        0x00, 0x00, 0x00, 0x00
};

static u32 lan966x_ifh_extract(const u8 *ifh, u32 pos, u32 len)
{
    u32 i, j, k, v = 0, val = 0;

    for (i = 0; i < len; i++) {
        j = (pos + i);
        k = (j % 8);
        if (i == 0 || k == 0) {
            // Read IFH byte
            v = ifh[28 - (j / 8) - 1];
        }
        if (v & (1 << k)) {
            val |= (1 << i);
        }
    }
    return val;
}

struct net_device *vtss_if_mux_parent_dev_get(void) {
    int err;

    if (vtss_if_mux_parent_dev)
        return vtss_if_mux_parent_dev;

    vtss_if_mux_parent_dev = dev_get_by_name(&init_net, parent_if_name);
    if (!vtss_if_mux_parent_dev) {
        return NULL;
    }

    err = netdev_rx_handler_register(vtss_if_mux_parent_dev,
                                     vtss_if_mux_rx_handler, 0);
    if (err) {
        pr_info("failed to register handler\n");
        goto exit_unreg;
    }

    return vtss_if_mux_parent_dev;

exit_unreg:
    netdev_rx_handler_unregister(vtss_if_mux_parent_dev);
    dev_put(vtss_if_mux_parent_dev);
    vtss_if_mux_parent_dev = 0;
    return NULL;
}

rx_handler_result_t vtss_if_mux_rx_handler(struct sk_buff **pskb)
{
    struct sk_buff *skb;
    struct sk_buff *skb_new;
    struct vtss_if_mux_pcpu_stats *stats;
    struct net_device *dev;
    unsigned int vid = 0;
    int ether_type_offset, pop;
    u16 *ether_type;
    int rx_ok = 1;
    u32 chip_port, min_size;
    u16 etype;

    if (!pskb || !(*pskb))
        return RX_HANDLER_PASS;

    skb = *pskb;

#if 0
    pr_info("RX %u bytes\n", skb->len);
    print_hex_dump(KERN_INFO, "RX: ", DUMP_PREFIX_OFFSET, 16, 1,
                   skb->data, skb->len, false);
#endif

    // Frame layout:
    if (vtss_if_mux_chip->internal_cpu)
            // 2 bytes IFH_ID, IFH_LEN bytes IFH, original Ethernet frame, FCS
            min_size = 2 + vtss_if_mux_chip->ifh_len + ETH_ZLEN + ETH_FCS_LEN;
    else
            // 2 bytes IFH_ID, IFH_LEN bytes IFH, original Ethernet frame w/o FCS
            min_size = 2 + vtss_if_mux_chip->ifh_len + ETH_ZLEN;

    if (skb->len < min_size) {
        // Where should this be counted??
        pr_err("Error: %s#%d: Short frame of %u bytes (minimum expected = %u bytes)\n", __FILE__, __LINE__, skb->len, min_size);
        print_hex_dump(KERN_ERR, "Rx ", DUMP_PREFIX_ADDRESS, 16, 1, skb->data, skb->len, true);
        return RX_HANDLER_PASS;
    }

    if (skb->protocol != htons(0x8880) || skb->data[0] != 0 || skb->data[1] != vtss_if_mux_chip->ifh_id) {
        // TODO, Where should this be counted??
        pr_info("Not for us: 0x%04x %02hhx %02hhx\n",
               skb->protocol, skb->data[0], skb->data[1]);
        return RX_HANDLER_PASS;
    }

    // IFH: Check SFLOW/ACL discard and parse the VID/port
    if (vtss_if_mux_chip->soc == SOC_LUTON) {
            if ((skb->data[5] & 0x1f) <= 27) {
                    /* SFLOW discard */
                    return RX_HANDLER_PASS;
            }
            vid = (((skb->data[8] << 8) | skb->data[9]) & 0xfff);
            chip_port = ((skb->data[2] << 8) | skb->data[3]);
            chip_port = ((chip_port >> 3) & 0x1f);
    } else if (vtss_if_mux_chip->soc == SOC_SERVAL1 ||
               vtss_if_mux_chip->soc == SOC_OCELOT) {
            if ((skb->data[13] & 0xf) <= 12) {
                    /* SFLOW discard */
                    return RX_HANDLER_PASS;
            }
            if (skb->data[13] & 0x20) {
                    /* ACL discard (ACL_ID, bit 0) */
                    return RX_HANDLER_PASS;
            }
            vid = (((skb->data[16] << 8) | skb->data[17]) & 0xfff);
            chip_port = ((skb->data[12] >> 3) & 0xf);
    } else if (vtss_if_mux_chip->soc == SOC_JAGUAR2 ||
               vtss_if_mux_chip->soc == SOC_SERVALT) {
#define IFH_OFF  2  // We have IFH ID first (2 bytes)
            // When using an external CPU along with an NPI port, it may happen that
            // frames we send *switched* from the CPU get back to the NPI port.
            // These frames must be discarded. They can be detected by the
            // IFH.VSTAX.SRC having the following properties (when originally
            // injected with IFH.PIPELINE_ACT = INJ_MASQ = 1):
            //    SRC_ADDR_MODE == 0
            //    SRC_PORT_TYPE == 1
            //    SRC_INTPN     == 15
            u16 vstax_src = ((skb->data[IFH_OFF + 20] & 0xf) << 8) | skb->data[IFH_OFF + 21];
            if (vstax_src == 0x80f) {
                    pr_info("VSTAX.SRC = 0x%x. Discarding\n", vstax_src);
                    // Returning RX_HANDLER_CONSUMED makes sure not even to forward this
                    // on the raw vtss.ifh socket (to the application).
                    return RX_HANDLER_CONSUMED;
            }

            if ((skb->data[IFH_OFF + 23] >> 5) & 1) {
                    /* SFLOW discard */
                    return RX_HANDLER_PASS;
            }
            if (skb->data[IFH_OFF + 9] & 0x10) {
                    /* ACL discard (CL_RSLT, bit 1) */
                    return RX_HANDLER_PASS;
            }
            if (skb->data[IFH_OFF + 6] & 0x20) {
                    /* GEN_IDX_MODE is VSI, translate to VID */
                    vid = vtss_if_mux_vsi2vid(((skb->data[IFH_OFF + 5] << 8) | skb->data[IFH_OFF + 6]) >> 6);
            }
            if (vid == 0) {
                    vid = (((skb->data[IFH_OFF + 18] << 8) | skb->data[IFH_OFF + 19]) & 0xfff);
            }
            chip_port = ((skb->data[IFH_OFF + 23] << 8) | skb->data[IFH_OFF + 24]);
            chip_port = ((chip_port >> 3) & 0x3f);

            // When using an external CPU along with an NPI port, it may happen that
            // frames we send *directed* from the CPU get back to the NPI port. These
            // frames must be discarded. They can be detected by the IFH.SRC_PORT being
            // that of the CPU.
            if (chip_port == vtss_if_mux_chip->cpu_port) {
                    pr_info("IFH.SRC_PORT == 0x%x. Discarding\n", chip_port);
                    // Returning RX_HANDLER_CONSUMED makes sure not even to forward this on
                    // the raw vtss.ifh socket (to the application).
                    return RX_HANDLER_CONSUMED;
            }
    } else if (vtss_if_mux_chip->soc == SOC_SPARX5) {
            pr_debug("Sparx5 IFH detected: Now check various IFH flags\n");
            /* TODO: check for drop conditions */
            chip_port = ((skb->data[IFH_OFF + 29] & 0x1f) << 2) | ((skb->data[IFH_OFF + 30] & 0xc0) >> 6);
            vid = (((skb->data[IFH_OFF + 23] << 7) | (skb->data[IFH_OFF + 24] >> 1)) & 0xfff);
            pr_debug("%s:%d %s: chip port: %d, vid: %d\n",
                    __FILE__, __LINE__, __func__,
                    chip_port, vid);
    } else if (vtss_if_mux_chip->soc == SOC_LAN966X) {
            chip_port = lan966x_ifh_extract(&skb->data[IFH_OFF], 141, 4);
            vid = lan966x_ifh_extract(&skb->data[IFH_OFF], 103, 17) & 0xfff;
    } else if (vtss_if_mux_chip->soc == SOC_LAN969X) {
            chip_port = (skb->data[IFH_OFF + 26] & 0x3f) >> 1;
            vid = (((u16)(skb->data[IFH_OFF + 23]) << 8 | skb->data[IFH_OFF + 24]) >> 1) & 0xfff;
    } else {
            if (printk_ratelimit())
                    printk("Invalid architecture type\n");
            return RX_HANDLER_CONSUMED;
    }

    if (vid < 1 || vid >= VLAN_N_VID) {
        return RX_HANDLER_PASS;
    }

    // Port device takes precedence
    dev = vtss_if_mux_port_net_dev[chip_port];

    if (!dev) {
        // Do nothing if we have no dependent device
        dev = vtss_if_mux_vlan_net_dev[vid];
        if (!dev) {
            //pr_info("Discard, no dependent device\n");
            return RX_HANDLER_PASS;
        }
    }

    // VLAN filtering and tag popping
    ether_type_offset = 2 + vtss_if_mux_chip->ifh_len + 12; // skip id, ifh and mac addresses
    ether_type = (u16 *)(skb->data + ether_type_offset);
    pop = vtss_if_mux_filter_vlan_pop(vid, chip_port, ntohs(ether_type[0]), ntohs(ether_type[2]));
    if (pop < 0) {
        //pr_err("Discard, chip_port: %u, vid: %u\n", chip_port, vid);
        return RX_HANDLER_PASS;
    }
    etype = ntohs(ether_type[pop/2]);
    if (etype != 0x0800 && etype != 0x0806 && etype != 0x86dd) {
        //pr_err("Discard, non-IP/ARP frame, port: %u, vid: %u\n", chip_port, vid);
        return RX_HANDLER_PASS;
    }
    //pr_err("Pop %u bytes, chip_port: %u, vid: %u\n", pop, chip_port, vid);

    // Apply IP filter
    if (vtss_if_mux_filter_apply(skb, vid, ether_type_offset + pop)) {
        //pr_info("Discard, IP filter");
        return RX_HANDLER_PASS;
    }

    // Check if SMAC matches our own address
    if (ether_addr_equal(skb->data + ether_type_offset - 6, dev->dev_addr)) {
        //pr_err("Discard own SMAC, chip_port: %u, vid: %u\n", chip_port, vid);
        return RX_HANDLER_PASS;
    }

    skb_new = skb_clone(skb, GFP_ATOMIC /* invoked from softirq context */);
    if (!skb_new) {
        rx_ok = 0;
        goto DO_CNT;
    }

    // Front: Discard the VTSS headers (IFH + 2-byte IFH_ID)
    skb_pull_inline(skb_new, vtss_if_mux_chip->ifh_len + 2);

    if (vtss_if_mux_chip->internal_cpu) {
            // Back: Discard the FCS, since - on Rx - the VC3FDMA driver includes
            // the FCS, because it on some platforms contains meta data (sFlow)
            skb_trim(skb_new, skb_new->len - ETH_FCS_LEN);
    }

#if 0
    pr_info("RX %u bytes on vlan %u\n", skb_new->len, vid);
    print_hex_dump(KERN_INFO, "RX: ", DUMP_PREFIX_OFFSET, 16, 1,
                   skb_new->data, skb_new->len, false);
#endif

    // Update the position of the MAC header according to the new "data" pointer
    skb_new->protocol = eth_type_trans(skb_new, dev);
    if (pop) {
        skb_pull_inline(skb_new, pop);
        skb_new->protocol = htons(etype);
    }

    // Lower-layer might have a different MAC-address, or the MAC-address of the
    // IFH might not match the encapsulated address. Either case we need to
    // reconsider the pkt_type based on the new DMAC.
    if (ether_addr_equal(eth_hdr(skb_new)->h_dest, dev->dev_addr))
        skb_new->pkt_type = PACKET_HOST;

    // pr_info("protocol: 0x%04hx pkt_type: %i\n",
    //        ntohs(skb_new->protocol), skb_new->pkt_type);
    netif_rx(skb_new);

DO_CNT:
    stats = this_cpu_ptr(vtss_if_mux_dev_priv(dev)->vtss_if_mux_pcpu_stats);
    u64_stats_update_begin(&stats->syncp);
    if (likely(rx_ok)) {
        stats->rx_packets++;
        stats->rx_bytes += skb_new->len;
        if (skb_new->pkt_type == PACKET_MULTICAST)
            stats->rx_multicast++;
    } else {
        stats->rx_errors++;
    }
    u64_stats_update_end(&stats->syncp);

    return RX_HANDLER_PASS;
}

static int dev_notification(struct notifier_block *unused, unsigned long event,
                            void *ptr) {
    struct net_device *dev = netdev_notifier_info_to_dev(ptr);

    pr_debug("%s:%d %s: device: %s, event: %lu\n", __FILE__, __LINE__, __func__,
        dev->name, event);
    if (dev != vtss_if_mux_parent_dev) {
        int vlan_id;

        if (sscanf(dev->name, "vtss.vlan.%d", &vlan_id) == 1) {
            if (vlan_id == 1) {
                switch (event) {
                case NETDEV_PRE_UP:
                    vtss_if_mux_vlan_up[1] = 1;
                    pr_debug("%s:%d %s: set vlan 1 up\n", __FILE__, __LINE__, __func__);
                    break;
                }
            }
        }
        return NOTIFY_DONE;
    }

    switch (event) {
        case NETDEV_UNREGISTER:
            if (vtss_if_mux_parent_dev) {
                pr_info("Releasing reference to parent device\n");
                netdev_rx_handler_unregister(vtss_if_mux_parent_dev);
                dev_put(vtss_if_mux_parent_dev);
                vtss_if_mux_parent_dev = 0;
            }
            break;

        default:;
    }

    return NOTIFY_DONE;
}

static struct notifier_block dev_notifier_block __read_mostly = {
    .notifier_call = dev_notification,
};

static int ifmux_probe(struct platform_device *pdev)
{
    int err = 0, i;
    vtss_if_mux_parent_dev = 0;

    pr_info("Loading module vtss-if-mux\n");

    for (i = 0; i < VTSS_IF_MUX_PORT_CNT; ++i)
        vtss_if_mux_port_net_dev[i] = 0;

    for (i = 0; i < VLAN_N_VID; ++i)
        vtss_if_mux_vlan_net_dev[i] = 0;

    err = register_netdevice_notifier(&dev_notifier_block);
    if (err < 0) {
        pr_info("Failed: register_netdevice_notifier\n");
        goto exit;
    }

    err = vtss_if_mux_netlink_init();
    if (err < 0) {
        pr_info("Failed: vtss_if_mux_netlink_init\n");
        goto exit_1;
    }

    err = vtss_if_mux_genetlink_init();
    if (err < 0) {
        pr_info("Failed: vtss_if_mux_genetlink_init\n");
        goto exit_2;
    }

    err = vtss_if_mux_dev_init();
    if (err < 0) {
        pr_info("Failed: vtss_if_mux_dev_init\n");
        goto exit_3;
    }

    /* Save chip properties (global) */
    vtss_if_mux_chip = (struct ifmux_chip*) device_get_match_data(&pdev->dev);
    vtss_if_mux_chip->internal_cpu = !device_property_read_bool(&pdev->dev, "external-cpu");
    (void) device_property_read_string(&pdev->dev, "parent-interface", &parent_if_name);

    pr_debug("vtss_if_mux_dev_init: use IFH id: 0x%x\n", vtss_if_mux_chip->ifh_id);
    return 0;

exit_3:
    vtss_if_mux_genetlink_uninit();

exit_2:
    vtss_if_mux_netlink_uninit();

exit_1:
    unregister_netdevice_notifier(&dev_notifier_block);

exit:
    return err;
}

static int ifmux_remove(struct platform_device *pdev)
{
    int i;
    struct net_device *dev;

    pr_info("Unloading module vtss-if-mux\n");
    unregister_netdevice_notifier(&dev_notifier_block);
    vtss_if_mux_netlink_uninit();
    vtss_if_mux_genetlink_uninit();
    vtss_if_mux_dev_uninit();

    for (i = 0; i < VTSS_IF_MUX_PORT_CNT; ++i) {
        dev = vtss_if_mux_port_net_dev[i];
        if (dev) {
            pr_info("unreg net device port=%d %p\n", i, dev);
            unregister_netdev(dev);
            free_netdev(dev);
            vtss_if_mux_port_net_dev[i] = 0;
        }
    }

    for (i = 0; i < VLAN_N_VID; ++i) {
        dev = vtss_if_mux_vlan_net_dev[i];
        if (dev) {
            pr_info("unreg net device vlan=%d %p\n", i, dev);
            unregister_netdev(dev);
            free_netdev(dev);
            vtss_if_mux_vlan_net_dev[i] = 0;
        }
    }

    if (vtss_if_mux_parent_dev) {
        pr_info("unreg\n");
        rtnl_lock();
        netdev_rx_handler_unregister(vtss_if_mux_parent_dev);
        rtnl_unlock();

        dev_put(vtss_if_mux_parent_dev);
        vtss_if_mux_parent_dev = 0;
    }
    return 0;
}

/****************************************************************************/
/****************************************************************************/

static void set_bitmapped_port(struct ifmux_chip *cfg, u8 *hdr, u16 port)
{
    unsigned int offset;

    // Write the template header
    memcpy(hdr, cfg->hdr_tmpl_port, cfg->ifh_encap_port_len);

    // Set the destination port bit
    offset = (cfg->ifh_offs_port_mask + port);
    hdr[cfg->ifh_encap_port_len - 1 - (offset / 8)] |= (1 << (offset % 8));
}

static void set_numbered_port(struct ifmux_chip *cfg, u8 *hdr, u16 port)
{
    unsigned int offset;

    // Write the template header
    memcpy(hdr, cfg->hdr_tmpl_port, cfg->ifh_encap_port_len);

    // The source port value is hardcoded in the template
    // Set the destination port value: the port mask offset is LSB
    offset = cfg->ifh_offs_port_mask / 8;
    hdr[offset] = hdr[offset] | ((port << 5) & 0xe0);
    hdr[offset-1] = hdr[offset-1] | ((port >>  3) & 0x1f);

}


static void set_vlan_id(struct ifmux_chip *cfg, u8 *hdr, struct sk_buff *skb,
    u16 vlan_id)
{
    int i;
    u8 *mac;

    // Write the template header
    memcpy(hdr, cfg->hdr_tmpl_vlan, cfg->ifh_encap_vlan_len);

#if 0
    pr_debug("Template: %lu bytes\n", cfg->ifh_encap_vlan_len);
    print_hex_dump(KERN_INFO, "template: ", DUMP_PREFIX_OFFSET, 16, 1,
                   vtss_if_mux_chip->hdr_tmpl_vlan, cfg->ifh_encap_vlan_len, false);

    pr_info("TX2 %u bytes on vlan: %u\n", skb->len, vlan_id);
    print_hex_dump(KERN_INFO, "TX2: ", DUMP_PREFIX_OFFSET, 16, 1,
                   skb->data, skb->len, false);
#endif
    hdr += cfg->ifh_encap_len;
    mac = hdr + VLAN_HLEN;
    // move the da/sa to make room for vlan tag (dummy tag is last in template)
    for (i = 0; i < 2*ETH_ALEN; ++i) {
        *hdr++ = *mac++;
    }
    // Write the vlan tag
    *hdr++ = 0x81;
    *hdr++ = 0x00;
    *hdr++ = (vlan_id >> 8) & 0x0f;
    *hdr++ = vlan_id & 0xff;

    // update the placement of the mac-header
    skb->mac_header = cfg->ifh_encap_vlan_len;
}


static void set_vlan_id_sparx5(struct ifmux_chip *cfg, u8 *hdr, struct sk_buff *skb,
    u16 vlan_id)
{
    int i;

    // Write the template header
    memcpy(hdr, cfg->hdr_tmpl_vlan, cfg->ifh_encap_vlan_len);

#if 0
    pr_debug("Template: %lu bytes\n", cfg->ifh_encap_vlan_len);
    print_hex_dump(KERN_INFO, "template: ", DUMP_PREFIX_OFFSET, 16, 1,
                   vtss_if_mux_chip->hdr_tmpl_vlan, cfg->ifh_encap_vlan_len, false);

    pr_info("TX2 %u bytes on vlan: %u\n", skb->len, vlan_id);
    print_hex_dump(KERN_INFO, "TX2: ", DUMP_PREFIX_OFFSET, 16, 1,
                   skb->data, skb->len, false);
#endif
    // move the da/sa to make room for vlan tag (dummy tag is last in template)
    for (i = 0; i < 12; ++i) {

        skb->data[i + cfg->ifh_encap_vlan_len - 4] = skb->data[i + cfg->ifh_encap_vlan_len];
    }

    // Write the vlan tag
    skb->data[cfg->ifh_encap_vlan_len + 12 - 4 + 0] = 0x81;
    skb->data[cfg->ifh_encap_vlan_len + 12 - 4 + 1] = 0x00;
    skb->data[cfg->ifh_encap_vlan_len + 12 - 4 + 2] = (vlan_id >> 8) & 0x0f;
    skb->data[cfg->ifh_encap_vlan_len + 12 - 4 + 3] = vlan_id & 0xff;

    // update the placement of the mac-header
    skb->mac_header = cfg->ifh_encap_vlan_len;
}

static const struct ifmux_chip luton_chip = {
        .soc                    = SOC_LUTON,
        .ifh_id             = IFH_ID_LUTON,
        .ifh_len            = IFH_LEN_LUTON,
        .ifh_encap_len      = IFH_LEN_LUTON + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_LUTON,
        .cpu_port            = 0,        /* Unused */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_luton,
        .hdr_tmpl_port            = hdr_tmpl_port_luton,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_luton),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_luton),
        .set_port_value     = set_bitmapped_port,
        .set_vlan_value     = set_vlan_id,
};

static const struct ifmux_chip serval1_chip = {
        .soc                    = SOC_SERVAL1,
        .ifh_id             = IFH_ID_SERVAL1,
        .ifh_len            = IFH_LEN_SERVAL1,
        .ifh_encap_len      = IFH_LEN_SERVAL1 + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_SERVAL1,
        .cpu_port            = 0,        /* Unused */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_serval1,
        .hdr_tmpl_port            = hdr_tmpl_port_serval1,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_serval1),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_serval1),
        .set_port_value     = set_bitmapped_port,
        .set_vlan_value     = set_vlan_id,
};

static const struct ifmux_chip ocelot_chip = {
        .soc                    = SOC_OCELOT,
        .ifh_id             = IFH_ID_OCELOT,
        .ifh_len            = IFH_LEN_OCELOT,
        .ifh_encap_len      = IFH_LEN_OCELOT + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_OCELOT,
        .cpu_port            = 0,        /* Unused */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_ocelot,
        .hdr_tmpl_port            = hdr_tmpl_port_ocelot,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_ocelot),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_ocelot),
        .set_port_value     = set_bitmapped_port,
        .set_vlan_value     = set_vlan_id,
};

static const struct ifmux_chip servalt_chip = {
        .soc                    = SOC_SERVALT,
        .ifh_id             = IFH_ID_SERVALT,
        .ifh_len            = IFH_LEN_JAGUAR2,
        .ifh_encap_len      = IFH_LEN_JAGUAR2 + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_JAGUAR2,
        .cpu_port            = 11, /* CPU port == 11 on ServalT */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_servalt,
        .hdr_tmpl_port            = hdr_tmpl_port_servalt,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_servalt),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_servalt),
        .set_port_value     = set_bitmapped_port,
        .set_vlan_value     = set_vlan_id,
};

static const struct ifmux_chip jaguar2_chip = {
        .soc                    = SOC_JAGUAR2,
        .ifh_id             = IFH_ID_JAGUAR2,
        .ifh_len            = IFH_LEN_JAGUAR2,
        .ifh_encap_len      = IFH_LEN_JAGUAR2 + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_JAGUAR2,
        .cpu_port            = 53, /* CPU port == 53 on JR2 */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_jaguar2,
        .hdr_tmpl_port            = hdr_tmpl_port_jaguar2,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_jaguar2),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_jaguar2),
        .set_port_value     = set_bitmapped_port,
        .set_vlan_value     = set_vlan_id,
};

static const struct ifmux_chip sparx5_chip = {
        .soc                    = SOC_SPARX5,
        .ifh_id             = IFH_ID_SPARX5,
        .ifh_len            = IFH_LEN_SPARX5,
        .ifh_encap_len      = IFH_LEN_SPARX5 + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_SPARX5,
        .cpu_port            = 65, /* CPU port 0 == chipport 65 on FA */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_sparx5,
        .hdr_tmpl_port            = hdr_tmpl_port_sparx5,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_sparx5),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_sparx5),
        .set_port_value     = set_numbered_port,
        .set_vlan_value     = set_vlan_id_sparx5,
};

static const struct ifmux_chip lan966x_chip = {
        .soc                    = SOC_LAN966X,
        .ifh_id             = IFH_ID_LAN966X,
        .ifh_len            = IFH_LEN_LAN966X,
        .ifh_encap_len      = IFH_LEN_LAN966X + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_LAN966X,
        .cpu_port            = 8, /* CPU port 0 == chip port 8 on lan966x */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_lan966x,
        .hdr_tmpl_port            = hdr_tmpl_port_lan966x,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_lan966x),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_lan966x),
        .set_port_value     = set_bitmapped_port,
        .set_vlan_value     = set_vlan_id,
};

static const struct ifmux_chip lan969x_chip = {
        .soc                    = SOC_LAN969X,
        .ifh_id             = IFH_ID_LAN969X,
        .ifh_len            = IFH_LEN_LAN969X,
        .ifh_encap_len      = IFH_LEN_LAN969X + 2*ETH_ALEN + VLAN_HLEN,
        .ifh_offs_port_mask = IFH_OFFS_PORT_MASK_LAN969X,
        .cpu_port            = 30, /* CPU port 0 == chip port 30 on lan969x */
        .hdr_tmpl_vlan            = hdr_tmpl_vlan_lan969x,
        .hdr_tmpl_port            = hdr_tmpl_port_lan969x,
        .ifh_encap_vlan_len = sizeof(hdr_tmpl_vlan_lan969x),
        .ifh_encap_port_len = sizeof(hdr_tmpl_port_lan969x),
        .set_port_value     = set_numbered_port,
        .set_vlan_value     = set_vlan_id_sparx5,
};

static const struct of_device_id mscc_ifmux_id_table[] = {
        {
                .compatible = "mscc,luton-ifmux",
                .data = &luton_chip,
        },
        {
                .compatible = "mscc,serval-ifmux",
                .data = &serval1_chip,
        },
        {
                .compatible = "mscc,ocelot-ifmux",
                .data = &ocelot_chip,
        },
        {
                .compatible = "mscc,servalt-ifmux",
                .data = &servalt_chip,
        },
        {
                .compatible = "mscc,jaguar2-ifmux",
                .data = &jaguar2_chip,
        },
        {
                .compatible = "microchip,sparx5-ifmux",
                .data = &sparx5_chip,
        },
        {
                .compatible = "microchip,lan966x-ifmux",
                .data = &lan966x_chip,
        },
        {
                .compatible = "microchip,lan969x-ifmux",
                .data = &lan969x_chip,
        },
        {}
};
MODULE_DEVICE_TABLE(of, mscc_ifmux_id_table);

static struct platform_driver mscc_ifmux_driver = {
        .remove      = ifmux_remove,
        .driver = {
                .name = "mscc_ifmux",
                .of_match_table = mscc_ifmux_id_table,
        },
};

module_platform_driver_probe(mscc_ifmux_driver, ifmux_probe);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Allan W. Nielsen <anielsen@vitesse.com>");
