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

#include <net/ipv6.h>
#include <net/genetlink.h>
#include <uapi/linux/ipv6.h>
#include <linux/inetdevice.h>
#include "vtss_if_mux.h"
#include <linux/proc_fs.h>

enum vtss_if_mux_action {
	VTSS_IF_MUX_ACTION_DROP,
	VTSS_IF_MUX_ACTION_CHECK_WHITE,
	VTSS_IF_MUX_ACTION_ACCEPT,
};

enum vtss_if_mux_list {
	VTSS_IF_MUX_LIST_WHITE,
	VTSS_IF_MUX_LIST_BLACK,
};

enum vtss_if_mux_attr {
	VTSS_IF_MUX_ATTR_NONE,
	VTSS_IF_MUX_ATTR_ID,
	VTSS_IF_MUX_ATTR_OWNER,
	VTSS_IF_MUX_ATTR_LIST,
	VTSS_IF_MUX_ATTR_ACTION,
	VTSS_IF_MUX_ATTR_RULE,
	VTSS_IF_MUX_ATTR_ELEMENT,
	VTSS_IF_MUX_ATTR_ELEMENT_TYPE,
	VTSS_IF_MUX_ATTR_ELEMENT_PORT_MASK,
	VTSS_IF_MUX_ATTR_ELEMENT_ADDR,
	VTSS_IF_MUX_ATTR_ELEMENT_INT,
	VTSS_IF_MUX_ATTR_ELEMENT_PREFIX,
	VTSS_IF_MUX_ATTR_PORT_CONF,
	VTSS_IF_MUX_ATTR_PORT_CONF_ENTRY,
	VTSS_IF_MUX_ATTR_PORT_CONF_CHIP_PORT,
	VTSS_IF_MUX_ATTR_PORT_CONF_ETYPE,
	VTSS_IF_MUX_ATTR_PORT_CONF_ETYPE_CUSTOM,
	VTSS_IF_MUX_ATTR_PORT_CONF_VLAN_MASK,
	VTSS_IF_MUX_ATTR_PORT_CONF_RX_FILTER,
	VTSS_IF_MUX_ATTR_PORT_CONF_RX_FORWARD,
	VTSS_IF_MUX_ATTR_PORT_CONF_TX_FORWARD,
        VTSS_IF_MUX_ATTR_VLAN_VSI_MAP,

	// Add new entries here, and remember to update user-space applications
	VTSS_IF_MUX_ATTR_END,
};
#define VTSS_IF_MUX_ATTR_MAX (VTSS_IF_MUX_ATTR_END - 1)

enum vtss_if_mux_genl {
	VTSS_IF_MUX_GENL_NOOP,
	VTSS_IF_MUX_GENL_RULE_CREATE,
	VTSS_IF_MUX_GENL_RULE_DELETE,
	VTSS_IF_MUX_GENL_RULE_MODIFY,
	VTSS_IF_MUX_GENL_RULE_GET,
	VTSS_IF_MUX_GENL_PORT_CONF_SET,
	VTSS_IF_MUX_GENL_VLAN_VSI_MAP_SET,

	// Add new entries here, and remember to update user-space applications
};

enum vtss_if_mux_filter_type {
	VTSS_IF_MUX_FILTER_TYPE_none = 0,
	VTSS_IF_MUX_FILTER_TYPE_port_mask = 1,
	VTSS_IF_MUX_FILTER_TYPE_mac_src = 2,
	VTSS_IF_MUX_FILTER_TYPE_mac_dst = 3,
	VTSS_IF_MUX_FILTER_TYPE_mac_src_or_dst = 4,
	VTSS_IF_MUX_FILTER_TYPE_vlan = 5,
	VTSS_IF_MUX_FILTER_TYPE_ether_type = 6,
	VTSS_IF_MUX_FILTER_TYPE_ipv4_src = 7,
	VTSS_IF_MUX_FILTER_TYPE_ipv4_dst = 8,
	VTSS_IF_MUX_FILTER_TYPE_ipv4_src_or_dst = 9,
	VTSS_IF_MUX_FILTER_TYPE_ipv6_src = 10,
	VTSS_IF_MUX_FILTER_TYPE_ipv6_dst = 11,
	VTSS_IF_MUX_FILTER_TYPE_ipv6_src_or_dst = 12,
	VTSS_IF_MUX_FILTER_TYPE_arp_operation = 22,
	VTSS_IF_MUX_FILTER_TYPE_arp_hw_sender = 23,
	VTSS_IF_MUX_FILTER_TYPE_arp_hw_target = 24,
	VTSS_IF_MUX_FILTER_TYPE_arp_proto_sender = 25,
	VTSS_IF_MUX_FILTER_TYPE_arp_proto_target = 26,
	VTSS_IF_MUX_FILTER_TYPE_acl_id = 27,
	VTSS_IF_MUX_FILTER_TYPE_arp_gratuitous = 28,
};

/* Fwd */
static struct genl_family vtss_if_mux_genl_family;

#define VLAN_MASK_LEN 512
#define PORT_MASK_BITS 128
#define PORT_MASK_LEN (PORT_MASK_BITS/8)

static struct nla_policy genel_policy[VTSS_IF_MUX_ATTR_END] = {
		[VTSS_IF_MUX_ATTR_NONE] = {.type = NLA_UNSPEC},
		[VTSS_IF_MUX_ATTR_ID] = {.type = NLA_U32},
		[VTSS_IF_MUX_ATTR_OWNER] = {.type = NLA_U64},
		[VTSS_IF_MUX_ATTR_LIST] = {.type = NLA_U32},
		[VTSS_IF_MUX_ATTR_ACTION] = {.type = NLA_U32},
		[VTSS_IF_MUX_ATTR_RULE] = {.type = NLA_NESTED},
		[VTSS_IF_MUX_ATTR_ELEMENT] = {.type = NLA_NESTED},
		[VTSS_IF_MUX_ATTR_ELEMENT_TYPE] = {.type = NLA_U32},
		[VTSS_IF_MUX_ATTR_ELEMENT_PORT_MASK] = {.type = NLA_BINARY,
						   .len = PORT_MASK_LEN},
		[VTSS_IF_MUX_ATTR_ELEMENT_ADDR] = {.type = NLA_BINARY,
						   .len = MAX_ADDR_LEN},
		[VTSS_IF_MUX_ATTR_ELEMENT_INT] = {.type = NLA_U32},
		[VTSS_IF_MUX_ATTR_ELEMENT_PREFIX] = {.type = NLA_U32},
                [VTSS_IF_MUX_ATTR_PORT_CONF] = {.type = NLA_NESTED},
                [VTSS_IF_MUX_ATTR_PORT_CONF_ENTRY] = {.type = NLA_NESTED},
                [VTSS_IF_MUX_ATTR_PORT_CONF_CHIP_PORT] = {.type = NLA_U32},
                [VTSS_IF_MUX_ATTR_PORT_CONF_ETYPE] = {.type = NLA_U32},
                [VTSS_IF_MUX_ATTR_PORT_CONF_ETYPE_CUSTOM] = {.type = NLA_U32},
                [VTSS_IF_MUX_ATTR_PORT_CONF_VLAN_MASK] =  {.type = NLA_BINARY,
                                                           .len = VLAN_MASK_LEN},
                [VTSS_IF_MUX_ATTR_PORT_CONF_RX_FILTER] = {.type = NLA_U32},
                [VTSS_IF_MUX_ATTR_PORT_CONF_RX_FORWARD] = {.type = NLA_U32},
                [VTSS_IF_MUX_ATTR_PORT_CONF_TX_FORWARD] = {.type = NLA_U32},
};

struct vtss_if_mux_filter_element {
	// Type of element
	int type;

	// We need to hold the prefix and the ip addresses.
	int prefix;

	union {
		// Various numbers
		u32 i;

		// Used as port mask - must be converted to physical ports in
		// user-space
		unsigned long mask_value[BITS_TO_LONGS(PORT_MASK_BITS)];
		u8 mask[PORT_MASK_LEN];

		// We need 16 bytes to hold a IPv6 address
		char address[16];
	} data;
};

struct vtss_if_mux_filter_rule {
	struct list_head list;
	struct rcu_head rcu;

	u32 id;
	u32 bitmaks_idx;

	enum vtss_if_mux_action action;

	u64 owner;

	int cnt;
	struct vtss_if_mux_filter_element elements[0];
};

struct frame_data {
	unsigned int vid;
	struct sk_buff *skb;

	int fallback;

	unsigned int ether_type_offset;

	u64 whitelist_mask;
};

struct owner_bit_mask {
	struct list_head list;
	u32 bit_idx;
	u64 owner;
	u32 ref_cnt;
};

struct port_conf {
    struct rcu_head rcu;
    u32 etype;
    u32 etype_custom;
    u32 rx_forward;
    u32 tx_forward;
    u32 rx_filter;
    u8  vlan_filter[VLAN_MASK_LEN];
};

#define PORT_CNT 67
static struct port_conf *if_mux_port_conf[PORT_CNT];

static DEFINE_MUTEX(vtss_if_mux_genl_sem);
static struct list_head VTSS_IF_MUX_FILTER_WHITE_LIST;
static struct list_head VTSS_IF_MUX_FILTER_BLACK_LIST;
static struct proc_dir_entry *proc_dump = 0;
static struct proc_dir_entry *proc_dump_port_conf = 0;
static u64 OWNER_BIT_MASK_POOL = 0;
static struct list_head OWNER_BIT_MASK_ASSOCIATION;
static u16 vsi2vid[VLAN_N_VID];

#define VTSS_HDR (vtss_if_mux_chip->ifh_len + 2)
#define ETHERTYPE_LENGTH 2

static inline int vtss_port_check(struct frame_data *d, unsigned long *mask)
{
	u64 p = 0;
	if (vtss_if_mux_chip->soc == SOC_LUTON) {
		p = d->skb->data[3];
		p = (p >> 3);
		p &= 0x1f;
	} else if (vtss_if_mux_chip->soc == SOC_SERVAL1 ||
		   vtss_if_mux_chip->soc == SOC_OCELOT) {
		p = d->skb->data[12];
		p = (p >> 3);
		p &= 0xf;

	} else if (vtss_if_mux_chip->soc == SOC_JAGUAR2 ||
		   vtss_if_mux_chip->soc == SOC_SERVALT) {
		p = d->skb->data[25] & 1;
		p <<= 5;
		p |= (d->skb->data[26] >> 3) & 0x1f;
		//printk(KERN_ERR "CHIP-PORT: %llu - delete line when tested!", p);
	} else if (vtss_if_mux_chip->soc == SOC_SPARX5) {
		p = ((d->skb->data[31] << 2) |
		     (d->skb->data[32] >> 6)) & 0x7f;
		// pr_info("port: %llu: enabled: %d ", p, test_bit(p, mask));
	} else if (vtss_if_mux_chip->soc == SOC_LAN966X) {
		p = (d->skb->data[12] >> 5) & 0x7;
	} else {
		if (printk_ratelimit())
			printk("Invalid architecture type\n");
		return 0;
	}
	return test_bit(p, mask);
}

static inline int vtss_mac_src_check(struct frame_data *d, char *mac)
{
	return ether_addr_equal(mac, d->skb->data + VTSS_HDR + 6);
}

static inline int vtss_mac_dst_check(struct frame_data *d, char *mac)
{
	return ether_addr_equal(mac, d->skb->data + VTSS_HDR);
}

static inline int vtss_vlan_check(struct frame_data *d, u32 vid)
{
	return d->vid == vid;
}

static inline int vtss_ether_check(struct frame_data *d, u32 ether_type)
{
	__be16 *et = (u16 *)(d->skb->data + d->ether_type_offset);
	return ntohs(*et) == ether_type;
}

static inline struct iphdr *vtss_ipv4_hdr(struct frame_data *d)
{
	__be16 *et = (u16 *)(d->skb->data + d->ether_type_offset);
	if (*et != htons(ETH_P_IP))
		return NULL;

	if (d->skb->len <
	    (d->ether_type_offset + ETHERTYPE_LENGTH + sizeof(struct iphdr)))
		return NULL;

	return (struct iphdr *)(d->skb->data + d->ether_type_offset +
				ETHERTYPE_LENGTH);
}

static inline int vtss_ipv4_src_check(struct frame_data *d, char *addr, int p)
{
	struct iphdr *h;
	__be32 mask, match;

	h = vtss_ipv4_hdr(d);
	if (!h)
		return 0;

	match = *((__be32 *)addr);
	mask = inet_make_mask(p);

	return (h->saddr & mask) == (match & mask);
}

static inline int vtss_ipv4_dst_check(struct frame_data *d, char *addr, int p)
{
	struct iphdr *h;
	__be32 mask, match;

	h = vtss_ipv4_hdr(d);
	if (!h)
		return 0;

	match = *((__be32 *)addr);
	mask = inet_make_mask(p);

	return (h->daddr & mask) == (match & mask);
}

static inline struct ipv6hdr *vtss_ipv6_hdr(struct frame_data *d)
{
	__be16 *et = (__be16 *)(d->skb->data + d->ether_type_offset);
	if (*et != htons(ETH_P_IPV6))
		return NULL;

	if (d->skb->len <
	    (d->ether_type_offset + ETHERTYPE_LENGTH + sizeof(struct ipv6hdr)))
		return NULL;

	return (struct ipv6hdr *)(d->skb->data + d->ether_type_offset +
				  ETHERTYPE_LENGTH);
}

static inline int vtss_ipv6_src_check(struct frame_data *d, char *addr, int p)
{
	struct ipv6hdr *h;

	h = vtss_ipv6_hdr(d);
	if (!h)
		return 0;

	return ipv6_prefix_equal((struct in6_addr *)addr, &h->saddr, p);
}

static inline int vtss_ipv6_dst_check(struct frame_data *d, char *addr, int p)
{
	struct ipv6hdr *h;

	h = vtss_ipv6_hdr(d);
	if (!h)
		return 0;

	return ipv6_prefix_equal((struct in6_addr *)addr, &h->daddr, p);
}

static inline u8 *vtss_arp_hdr(struct frame_data *d)
{
	u16 *et = (u16 *)(d->skb->data + d->ether_type_offset);
	if (*et != htons(ETH_P_ARP))
		return NULL;

	if (d->skb->len < (d->ether_type_offset + ETHERTYPE_LENGTH + 28))
		return NULL;

	return (u8 *)d->skb->data + d->ether_type_offset + ETHERTYPE_LENGTH;
}

static inline int vtss_arp_operation_check(struct frame_data *d, int opr)
{
	__be16 *o;
	u8 *hdr = vtss_arp_hdr(d);

	if (!hdr)
		return 0;

	o = (__be16 *)(hdr + 6);

	return ntohs(*o) == opr;
}

static inline int vtss_arp_hw_sender_check(struct frame_data *d, char *addr)
{
	char *mac;
	char *hdr = (char *)vtss_arp_hdr(d);

	if (!hdr)
		return 0;

	mac = hdr + 8;

	return ether_addr_equal(addr, mac);
}

static inline int vtss_arp_hw_target_check(struct frame_data *d, char *addr)
{
	char *mac;
	char *hdr = (char *)vtss_arp_hdr(d);

	if (!hdr)
		return 0;

	mac = hdr + 18;

	return ether_addr_equal(addr, mac);
}

static inline int vtss_arp_proto_sender_check(struct frame_data *d, char *addr,
					      int p)
{
	__be32 *a, *b, mask;
	char *hdr = (char *)vtss_arp_hdr(d);

	if (!hdr)
		return 0;

	a = (__be32 *)addr;
	b = (__be32 *)(hdr + 14);
	mask = inet_make_mask(p);

	return (*a & mask) == (*b & mask);
}

static inline int vtss_arp_proto_target_check(struct frame_data *d, char *addr,
					      int p)
{
	__be32 *a, *b, mask;
	char *hdr = (char *)vtss_arp_hdr(d);

	if (!hdr)
		return 0;

	a = (__be32 *)addr;
	b = (__be32 *)(hdr + 24);
	mask = inet_make_mask(p);

	return (*a & mask) == (*b & mask);
}

static inline int vtss_arp_proto_gratuitous_check(struct frame_data *d)
{
	__be32 *a, *b;
	char *hdr = (char *)vtss_arp_hdr(d);

	if (!hdr)
		return 0;

	a = (__be32 *)(hdr + 14); // proto_sender
	b = (__be32 *)(hdr + 24); // proto_target

	return *a == *b;
}

static inline int vtss_acl_id_check(struct frame_data *d, char *mask, int offset)
{
	if (vtss_if_mux_chip->soc == SOC_LUTON) {
		u32 hit = 0, id = 0;
		hit = d->skb->data[6];
		hit = (hit >> 7);
		hit &= 0x01;
		id = ((u32)d->skb->data[4] << 8) | ((u32)d->skb->data[5]);
		id = (id >> 5);
		id &= 0xff;
		//printk(KERN_ERR "ACL-ID: %d %d\n", hit, id);

		if (!hit)
			return 0;

		if (id < offset * 128 || id >= (offset + 1) * 128)
			return 0;

		id -= offset * 128;
		return mask[id / 8] & (1 << (id % 8));
	}
	printk(KERN_ERR "PLATFORM-NOT-SUPPORTED!\n");
	return 0;
}

static inline int element_match(struct vtss_if_mux_filter_element *e,
				struct frame_data *d)
{
	switch (e->type) {
	case VTSS_IF_MUX_FILTER_TYPE_port_mask:
		return vtss_port_check(d, e->data.mask_value);

	case VTSS_IF_MUX_FILTER_TYPE_mac_src:
		return vtss_mac_src_check(d, e->data.address);

	case VTSS_IF_MUX_FILTER_TYPE_mac_dst:
		return vtss_mac_dst_check(d, e->data.address);

	case VTSS_IF_MUX_FILTER_TYPE_mac_src_or_dst:
		return vtss_mac_src_check(d, e->data.address) ||
		       vtss_mac_dst_check(d, e->data.address);

	case VTSS_IF_MUX_FILTER_TYPE_vlan:
		return vtss_vlan_check(d, e->data.i);

	case VTSS_IF_MUX_FILTER_TYPE_ether_type:
		return vtss_ether_check(d, e->data.i);

	case VTSS_IF_MUX_FILTER_TYPE_ipv4_src:
		return vtss_ipv4_src_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_ipv4_dst:
		return vtss_ipv4_dst_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_ipv4_src_or_dst:
		return vtss_ipv4_src_check(d, e->data.address, e->prefix) ||
		       vtss_ipv4_dst_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_ipv6_src:
		return vtss_ipv6_src_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_ipv6_dst:
		return vtss_ipv6_dst_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_ipv6_src_or_dst:
		return vtss_ipv6_src_check(d, e->data.address, e->prefix) ||
		       vtss_ipv6_dst_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_arp_operation:
		return vtss_arp_operation_check(d, e->data.i);

	case VTSS_IF_MUX_FILTER_TYPE_arp_hw_sender:
		return vtss_arp_hw_sender_check(d, e->data.address);

	case VTSS_IF_MUX_FILTER_TYPE_arp_hw_target:
		return vtss_arp_hw_target_check(d, e->data.address);

	case VTSS_IF_MUX_FILTER_TYPE_arp_proto_sender:
		return vtss_arp_proto_sender_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_arp_proto_target:
		return vtss_arp_proto_target_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_arp_gratuitous:
		return vtss_arp_proto_gratuitous_check(d);

	case VTSS_IF_MUX_FILTER_TYPE_acl_id:
		return vtss_acl_id_check(d, e->data.address, e->prefix);

	case VTSS_IF_MUX_FILTER_TYPE_none:
	default:
		return d->fallback;
	}

	return d->fallback;
}

static inline int rule_match(struct vtss_if_mux_filter_rule *r,
			     struct frame_data *d)
{
	int i;

	for (i = 0; i < r->cnt; ++i) {
		if (!element_match(&r->elements[i], d)) {
			// printk(KERN_ERR "Element %d did not match\n", i);
			return 0;
		}
	}

	// printk(KERN_ERR "All elements match\n");
	return 1;
}

static inline enum vtss_if_mux_action apply_black_list(struct frame_data *d)
{
	struct vtss_if_mux_filter_rule *r;
	enum vtss_if_mux_action a = VTSS_IF_MUX_ACTION_ACCEPT;
	d->fallback = 1;
	d->whitelist_mask = 0;

	// printk(KERN_ERR "Black list\n");
	rcu_read_lock();
	list_for_each_entry_rcu (r, &VTSS_IF_MUX_FILTER_BLACK_LIST, list) {
		if (rule_match(r, d)) {
			a = r->action;
			if (a == VTSS_IF_MUX_ACTION_DROP) {
				// printk(KERN_ERR "Black list -> drop\n");
				break;

			} else if (a == VTSS_IF_MUX_ACTION_CHECK_WHITE) {
				u64 m = 1;
				m <<= r->bitmaks_idx;
				d->whitelist_mask |= m;
				// printk(KERN_ERR "Black list -> white 0x%08llx, %u\n", d->whitelist_mask, r->bitmaks_idx);

			} else {
				BUG();
			}
		}
	}
	rcu_read_unlock();

	// printk(KERN_ERR "Black list -> %d\n", a);
	return a;
}

static inline enum vtss_if_mux_action apply_white_list(struct frame_data *d)
{
	struct vtss_if_mux_filter_rule *r;
	d->fallback = 0;

	// printk(KERN_ERR "White list - 0x%08llx\n", d->whitelist_mask);
	rcu_read_lock();
	list_for_each_entry_rcu (r, &VTSS_IF_MUX_FILTER_WHITE_LIST, list) {
		u64 m = 1;
		m <<= r->bitmaks_idx;
		// printk(KERN_ERR "CHECK 0x%08llx 0x%08llx\n", m, d->whitelist_mask);

		if ((d->whitelist_mask & m) == 0llu)
			continue;

		if (rule_match(r, d)) {
			d->whitelist_mask &= ~m;
			// printk(KERN_ERR "MATCH 0x%08llx 0x%08llx\n", m, d->whitelist_mask);
		}

		if (!d->whitelist_mask)
			break;
	}
	rcu_read_unlock();

	if (d->whitelist_mask) {
		// printk(KERN_ERR "White list drop 0x%08llx\n",
		// d->whitelist_mask);
		return VTSS_IF_MUX_ACTION_DROP;
	} else {
		// printk(KERN_ERR "White accept\n");
		return VTSS_IF_MUX_ACTION_ACCEPT;
	}

	// Unreachable
	return VTSS_IF_MUX_ACTION_DROP;
}

int vtss_if_mux_filter_apply(struct sk_buff *skb, unsigned int vid,
			     unsigned int ether_type_offset)
{
	int res = 1;
	enum vtss_if_mux_action black_action;
	struct frame_data d = {};
	d.vid = vid;
	d.skb = skb;
	d.ether_type_offset = ether_type_offset;

	// printk(KERN_ERR "%d\n", __LINE__);
	// print_hex_dump(KERN_ERR, "RX: ", DUMP_PREFIX_OFFSET, 16, 1,
	//                skb->data, skb->len, false);
	black_action = apply_black_list(&d);
	switch (black_action) {
	case VTSS_IF_MUX_ACTION_DROP:
		res = 1;
		break;

	case VTSS_IF_MUX_ACTION_CHECK_WHITE:
		res = 1;
		break;

	case VTSS_IF_MUX_ACTION_ACCEPT:
		res = 0;
		break;

	default:
		res = 1;
		printk(KERN_ERR "ERROR!! %d\n", __LINE__);
		break;
	}

	if (black_action == VTSS_IF_MUX_ACTION_CHECK_WHITE) {
		black_action = apply_white_list(&d);
		switch (black_action) {
		case VTSS_IF_MUX_ACTION_DROP:
			res = 1;
			break;

		case VTSS_IF_MUX_ACTION_ACCEPT:
			res = 0;
			break;

		default:
			res = 1;
			printk(KERN_ERR "ERROR!! %d\n", __LINE__);
			break;
		}
	}

	return res;
}

static int filter_size(int element_cnt)
{
	int alloc_size = sizeof(struct vtss_if_mux_filter_rule);
	alloc_size += element_cnt * sizeof(struct vtss_if_mux_filter_element);
	return alloc_size;
}

static int get_free_id(void)
{
	static u32 last_id = 0;
	struct vtss_if_mux_filter_rule *r;

	mutex_lock(&vtss_if_mux_genl_sem);
	rcu_read_lock();
AGAIN:
	last_id++;

	// Handle wrap around
	if (last_id <= 0) {
		last_id = 0;
		goto AGAIN;
	}

	list_for_each_entry_rcu (r, &VTSS_IF_MUX_FILTER_BLACK_LIST, list) {
		if (r->id == last_id)
			goto AGAIN;
	}

	list_for_each_entry_rcu (r, &VTSS_IF_MUX_FILTER_WHITE_LIST, list) {
		if (r->id == last_id)
			goto AGAIN;
	}

	rcu_read_unlock();
	mutex_unlock(&vtss_if_mux_genl_sem);
	return last_id;
};

static int vtss_if_mux_genl_cmd_noop(struct sk_buff *skb, struct genl_info *info)
{
	return 0;
}

static int bitmask_idx_alloc(struct vtss_if_mux_filter_rule *r)
{
	// TODO, assume locked
	u32 i;
	u64 mask;
	int bit_idx = -1;
	struct owner_bit_mask *e;

	// Check the cache
	list_for_each_entry (e, &OWNER_BIT_MASK_ASSOCIATION, list) {
		if (e->owner == r->owner) {
			e->ref_cnt++;
			r->bitmaks_idx = e->bit_idx;
			return 0;
		}
	}

	// Find an non-used bit
	mask = 1;
	for (i = 0; i < sizeof(OWNER_BIT_MASK_POOL) * 8; ++i) {
		if ((OWNER_BIT_MASK_POOL & mask) == 0llu) {
			bit_idx = i;
			break;
		}

		mask <<= 1;
	}

	if (bit_idx < 0) {
		printk(KERN_ERR "Too many owners\n");
		return -1;  // TODO, find better error value
	}

	e = kmalloc(sizeof(struct owner_bit_mask), GFP_KERNEL | __GFP_ZERO);
	if (!e)
		return -ENOMEM;

	r->bitmaks_idx = bit_idx;
	e->bit_idx = bit_idx;
	e->owner = r->owner;
	e->ref_cnt = 1;
	OWNER_BIT_MASK_POOL &= mask;
	list_add(&e->list, &OWNER_BIT_MASK_ASSOCIATION);

	return 0;
}

static void bitmask_idx_free(struct vtss_if_mux_filter_rule *r)
{
	// TODO, assume locked
	u64 mask;
	int bit_idx = -1;
	struct owner_bit_mask *e;

	// Check the cache
	list_for_each_entry (e, &OWNER_BIT_MASK_ASSOCIATION, list) {
		if (e->owner == r->owner) {
			if (e->ref_cnt > 1) {
				e->ref_cnt -= 1;
				return;
			} else {
				bit_idx = e->bit_idx;
				break;
			}
		}
	}

	BUG_ON(bit_idx == -1);

	mask = 1;
	mask <<= bit_idx;
	OWNER_BIT_MASK_POOL &= ~mask;

	list_del(&e->list);
	kfree(e);
}

static void rule_free(struct rcu_head *head)
{
	struct vtss_if_mux_filter_rule *r =
		container_of(head, struct vtss_if_mux_filter_rule, rcu);

	bitmask_idx_free(r);
	kfree(r);
}

static int parse_elements(struct vtss_if_mux_filter_element *e,
			  struct nlattr *rule)
{
	int err;
	struct nlattr *element_attr[VTSS_IF_MUX_ATTR_END];

	err = nla_parse_nested(element_attr, VTSS_IF_MUX_ATTR_MAX, rule,
			       genel_policy, NULL);
	if (err < 0) {
		printk(KERN_ERR "Failed to parse rule-elements\n");
		return -EINVAL;
	}

	if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_TYPE]) {
		printk(KERN_ERR "No type!\n");
		return -EINVAL;
	}

	e->type = nla_get_u32(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_TYPE]);

	// The element data is required, but depends on the type
	switch (e->type) {
	case VTSS_IF_MUX_FILTER_TYPE_arp_gratuitous:
		// No data needed.
		break;

	case VTSS_IF_MUX_FILTER_TYPE_port_mask:
		if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_PORT_MASK]) {
			printk(KERN_ERR "No port mask!\n");
			return -EINVAL;
		}
		memcpy(e->data.mask,
		       nla_data(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_PORT_MASK]),
		       PORT_MASK_LEN);
		pr_debug("Port Mask: %*pbl\n", PORT_MASK_BITS, e->data.mask_value);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_mac_src:
	case VTSS_IF_MUX_FILTER_TYPE_mac_dst:
	case VTSS_IF_MUX_FILTER_TYPE_mac_src_or_dst:
	case VTSS_IF_MUX_FILTER_TYPE_arp_hw_sender:
	case VTSS_IF_MUX_FILTER_TYPE_arp_hw_target:
		if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]) {
			printk(KERN_ERR "No address!\n");
			return -EINVAL;
		}

		if (nla_len(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]) != 6) {
			printk(KERN_ERR "Unexpected length: %d!\n",
			       nla_len(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]));
			return -EINVAL;
		}

		memcpy(e->data.address,
		       nla_data(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]), 6);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_vlan:
	case VTSS_IF_MUX_FILTER_TYPE_ether_type:
	case VTSS_IF_MUX_FILTER_TYPE_arp_operation:
		if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_INT]) {
			printk(KERN_ERR "No int!\n");
			return -EINVAL;
		}
		e->data.i =
			nla_get_u32(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_INT]);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_ipv4_src:
	case VTSS_IF_MUX_FILTER_TYPE_ipv4_dst:
	case VTSS_IF_MUX_FILTER_TYPE_ipv4_src_or_dst:
	case VTSS_IF_MUX_FILTER_TYPE_arp_proto_sender:
	case VTSS_IF_MUX_FILTER_TYPE_arp_proto_target:
		if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]) {
			printk(KERN_ERR "No address!\n");
			return -EINVAL;
		}

		if (nla_len(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]) != 4) {
			printk(KERN_ERR "Unexpected length: %d!\n",
			       nla_len(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]));
			return -EINVAL;
		}

		memcpy(e->data.address,
		       nla_data(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]), 4);

		if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_PREFIX]) {
			printk(KERN_ERR "No prefix!\n");
			return -EINVAL;
		}

		e->prefix = nla_get_u32(
			element_attr[VTSS_IF_MUX_ATTR_ELEMENT_PREFIX]);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_acl_id:
		if (vtss_if_mux_chip->soc != SOC_LUTON) {
			printk(KERN_ERR "Platform not supported\n");
			return -EINVAL;
		}
		fallthrough;
	case VTSS_IF_MUX_FILTER_TYPE_ipv6_src: // fallthrough
	case VTSS_IF_MUX_FILTER_TYPE_ipv6_dst: // fallthrough
	case VTSS_IF_MUX_FILTER_TYPE_ipv6_src_or_dst:
		if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]) {
			printk(KERN_ERR "No address!\n");
			return -EINVAL;
		}

		if (nla_len(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]) != 16) {
			printk(KERN_ERR "Unexpected length: %d!\n",
			       nla_len(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]));
			return -EINVAL;
		}

		memcpy(e->data.address,
		       nla_data(element_attr[VTSS_IF_MUX_ATTR_ELEMENT_ADDR]), 16);

		if (!element_attr[VTSS_IF_MUX_ATTR_ELEMENT_PREFIX]) {
			printk(KERN_ERR "No prefix!\n");
			return -EINVAL;
		}

		e->prefix = nla_get_u32(
			element_attr[VTSS_IF_MUX_ATTR_ELEMENT_PREFIX]);
		break;

	default:
		printk(KERN_ERR "Unsupported type!\n");
		return -EINVAL;
	}

	return 0;
}

static struct vtss_if_mux_filter_rule *parse_rule(struct sk_buff *skb,
						  struct genl_info *info,
						  int id, int *err)
{
	struct vtss_if_mux_filter_rule *r = NULL;
	struct nlattr *rule;
	int element_cnt = 0;
	int rem, i;

	// Not sure if there is a better way to get the number of elements
	if (info->attrs[VTSS_IF_MUX_ATTR_RULE]) {
		nla_for_each_nested (rule, info->attrs[VTSS_IF_MUX_ATTR_RULE],
				     rem) {
			element_cnt += 1;
		}
	}

	r = kmalloc(filter_size(element_cnt), GFP_KERNEL | __GFP_ZERO);
	if (!r) {
		*err = -ENOMEM;
		return NULL;
	}

	r->id = id;

	if (info->attrs[VTSS_IF_MUX_ATTR_OWNER]) {
		r->owner = nla_get_u64(info->attrs[VTSS_IF_MUX_ATTR_OWNER]);
	} else {
		r->owner = 0llu;
	}

	// Validate and copy the desired action.
	if (info->attrs[VTSS_IF_MUX_ATTR_ACTION]) {
		switch (nla_get_u32(info->attrs[VTSS_IF_MUX_ATTR_ACTION])) {
		case VTSS_IF_MUX_ACTION_DROP:
			r->action = VTSS_IF_MUX_ACTION_DROP;
			break;

		case VTSS_IF_MUX_ACTION_CHECK_WHITE:
			r->action = VTSS_IF_MUX_ACTION_CHECK_WHITE;
			break;

		default:
			*err = -EINVAL;
			goto ERR;
		}
	} else {
		r->action = VTSS_IF_MUX_ACTION_DROP;
	}

	// Copy all the elements from the netlink message into the allocated
	// rule
	i = 0;
	nla_for_each_nested (rule, info->attrs[VTSS_IF_MUX_ATTR_RULE], rem) {
		BUG_ON(i >= element_cnt);

		if (parse_elements(&(r->elements[i]), rule) < 0) {
			*err = -EINVAL;
			goto ERR;
		}

		// Rule has been added, increment the count.
		r->cnt++;
		i++;
	}

	return r;

ERR:
	kfree(r);
	return NULL;
}

static int vtss_if_mux_genl_cmd_rule_create(struct sk_buff *skb,
					    struct genl_info *info)
{
	int id;
	void *hdr;
	int err = -1;
	struct vtss_if_mux_filter_rule *r = NULL;
	struct sk_buff *msg;

	if (!info->attrs[VTSS_IF_MUX_ATTR_LIST])
		return -EINVAL;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &vtss_if_mux_genl_family, 0,
			  VTSS_IF_MUX_GENL_RULE_CREATE);
	if (!hdr) {
		err = -EMSGSIZE;
		goto ERROR_MEM_MSG;
	}

	id = get_free_id();
	if (nla_put_u32(msg, VTSS_IF_MUX_ATTR_ID, id)) {
		err = -EMSGSIZE;
		goto ERROR_GENLMSG;
	}

	r = parse_rule(skb, info, id, &err);
	if (!r)
		goto ERROR_GENLMSG;

	err = bitmask_idx_alloc(r);
	if (err < 0)
		goto ERROR_MEM_RULE;

	// Install the rule into the configured list
	switch (nla_get_u32(info->attrs[VTSS_IF_MUX_ATTR_LIST])) {
	case VTSS_IF_MUX_LIST_WHITE:
		mutex_lock(&vtss_if_mux_genl_sem);
		list_add_rcu(&r->list, &VTSS_IF_MUX_FILTER_WHITE_LIST);
		mutex_unlock(&vtss_if_mux_genl_sem);
		break;

	case VTSS_IF_MUX_LIST_BLACK:
		mutex_lock(&vtss_if_mux_genl_sem);
		list_add_rcu(&r->list, &VTSS_IF_MUX_FILTER_BLACK_LIST);
		mutex_unlock(&vtss_if_mux_genl_sem);
		break;

	default:
		err = -EINVAL;
		goto ERROR_BITMASK;
	}

	genlmsg_end(msg, hdr);
	return genlmsg_unicast(genl_info_net(info), msg, info->snd_portid);

ERROR_BITMASK:
	bitmask_idx_free(r);

ERROR_MEM_RULE:
	kfree(r);

ERROR_GENLMSG:
	genlmsg_cancel(skb, hdr);

ERROR_MEM_MSG:
	nlmsg_free(msg);

	return err;
}

static struct vtss_if_mux_filter_rule *find_rule_by_id(int id,
                                                       enum vtss_if_mux_list *list)
{
	struct vtss_if_mux_filter_rule *res = NULL, *r;

	rcu_read_lock();
	list_for_each_entry (r, &VTSS_IF_MUX_FILTER_BLACK_LIST, list) {
		if (r->id == id) {
			BUG_ON(res);
			res = r;
			if (list)
				*list = VTSS_IF_MUX_LIST_BLACK;
		}
	}

	list_for_each_entry (r, &VTSS_IF_MUX_FILTER_WHITE_LIST, list) {
		if (r->id == id) {
			BUG_ON(res);
			res = r;
			if (list)
				*list = VTSS_IF_MUX_LIST_WHITE;
		}
	}
	rcu_read_unlock();

	return res;
}

static int genl_rule_del_by_id(u32 id)
{
	struct vtss_if_mux_filter_rule *r;

	r = find_rule_by_id(id, NULL);

	if (!r)
		return -ENOENT;

	mutex_lock(&vtss_if_mux_genl_sem);
	list_del_rcu(&r->list);
	call_rcu(&r->rcu, rule_free);
	mutex_unlock(&vtss_if_mux_genl_sem);

	return 0;
}

// TODO, should be owner,pid
static int genl_rule_del_by_owner(u64 owner)
{
	int cnt = 0;
	struct vtss_if_mux_filter_rule *r;

	mutex_lock(&vtss_if_mux_genl_sem);
	list_for_each_entry (r, &VTSS_IF_MUX_FILTER_BLACK_LIST, list) {
		if (r->owner == owner) {
			cnt++;
			list_del_rcu(&r->list);
			call_rcu(&r->rcu, rule_free);
		}
	}

	list_for_each_entry (r, &VTSS_IF_MUX_FILTER_WHITE_LIST, list) {
		if (r->owner == owner) {
			cnt++;
			list_del_rcu(&r->list);
			call_rcu(&r->rcu, rule_free);
		}
	}
	mutex_unlock(&vtss_if_mux_genl_sem);

	return cnt;
}

static int genl_rule_del_all(void)
{
	int cnt = 0;
	struct vtss_if_mux_filter_rule *r;

	mutex_lock(&vtss_if_mux_genl_sem);
	list_for_each_entry (r, &VTSS_IF_MUX_FILTER_BLACK_LIST, list) {
		cnt++;
		list_del_rcu(&r->list);
		call_rcu(&r->rcu, rule_free);
	}

	list_for_each_entry (r, &VTSS_IF_MUX_FILTER_WHITE_LIST, list) {
		cnt++;
		list_del_rcu(&r->list);
		call_rcu(&r->rcu, rule_free);
	}
	mutex_unlock(&vtss_if_mux_genl_sem);

	return cnt;
}

static int genl_cmd_rule_delete(struct sk_buff *skb, struct genl_info *info)
{
	if (info->attrs[VTSS_IF_MUX_ATTR_ID] &&
	    info->attrs[VTSS_IF_MUX_ATTR_OWNER])
		return -EINVAL;

	if (info->attrs[VTSS_IF_MUX_ATTR_ID]) {
		return genl_rule_del_by_id(
			nla_get_u32(info->attrs[VTSS_IF_MUX_ATTR_ID]));

	} else if (info->attrs[VTSS_IF_MUX_ATTR_OWNER]) {
		genl_rule_del_by_owner(
			nla_get_u32(info->attrs[VTSS_IF_MUX_ATTR_OWNER]));
		return 0;

	} else {
		genl_rule_del_all();
		return 0;
	}

	return -EINVAL;
}

static int genl_cmd_rule_modify(struct sk_buff *skb, struct genl_info *info)
{
	int id;
	int err = -1;
	enum vtss_if_mux_list list_old;
	struct vtss_if_mux_filter_rule *r_new = NULL, *r_old = NULL;

	if (!info->attrs[VTSS_IF_MUX_ATTR_ID])
		return -EINVAL;

	id = nla_get_u32(info->attrs[VTSS_IF_MUX_ATTR_ID]);

	r_new = parse_rule(skb, info, id, &err);
	if (!r_new)
		goto ERROR;

	r_old = find_rule_by_id(id, &list_old);
	if (!r_old) {
		err = -ENOENT;
		goto ERROR;
	}

	if (!info->attrs[VTSS_IF_MUX_ATTR_OWNER])
		r_new->owner = r_old->owner;

	if (!info->attrs[VTSS_IF_MUX_ATTR_ACTION])
		r_new->action = r_old->action;

	err = bitmask_idx_alloc(r_new);
	if (err < 0)
		goto ERROR;

	// If a list is provided, then ensure that it is the same list as the
	// rule was found in. It is not allowed to move a rule from one list to
	// another by doing an update.
	if (info->attrs[VTSS_IF_MUX_ATTR_LIST]) {
		switch (nla_get_u32(info->attrs[VTSS_IF_MUX_ATTR_LIST])) {
		case VTSS_IF_MUX_LIST_WHITE:
			if (list_old != VTSS_IF_MUX_LIST_WHITE) {
				err = -EINVAL;
				goto ERROR_BITMASK;
			}
			break;

		case VTSS_IF_MUX_LIST_BLACK:
			if (list_old != VTSS_IF_MUX_LIST_BLACK) {
				err = -EINVAL;
				goto ERROR_BITMASK;
			}
			break;

		default:
			err = -EINVAL;
			goto ERROR_BITMASK;
		}
	}

	mutex_lock(&vtss_if_mux_genl_sem);
	list_replace_rcu(&r_old->list, &r_new->list);
	call_rcu(&r_old->rcu, rule_free);
	mutex_unlock(&vtss_if_mux_genl_sem);

	return 0;

ERROR_BITMASK:
	bitmask_idx_free(r_new);

ERROR:
	kfree(r_new);
	return err;
}

static int genl_cmd_rule_get(struct sk_buff *skb, struct genl_info *info)
{
	printk(KERN_ERR "Not implemented yet: genl_cmd_rule_get\n");
	return -ENOSYS;
}

int vtss_if_mux_filter_vlan_pop(u32 vid, u32 chip_port, u32 etype_outer, u32 etype_inner)
{
    int              pop = 0; // No tag popping by default
    struct port_conf *p;

    if (chip_port < PORT_CNT && vid < 4096) {
        rcu_read_lock();
        p = rcu_dereference(if_mux_port_conf[chip_port]);
        if (p != NULL) {
            if (!p->rx_forward || (p->rx_filter && (p->vlan_filter[vid/8] & (1 << (vid % 8))))) {
                // Ingress filtering
                pop = -1;
            } else if (etype_outer == p->etype) {
                if (etype_inner == 0x8100 || etype_inner == 0x88a8 || etype_inner == p->etype_custom) {
                    // Pop outer and inner tag
                    pop = 8;
                } else {
                    // Pop outer tag
                    pop = 4;
                }
            }
        }
        rcu_read_unlock();
    }
    return pop;
}

static int genl_cmd_port_conf_set(struct sk_buff *skb, struct genl_info *info)
{
    int i, rem, err;
    u32 chip_port;
    struct nlattr *conf, *port_attr, *etype_attr, *etype_custom_attr, *vlan_attr;
    struct nlattr *rx_filter_attr, *rx_forward_attr, *tx_forward_attr, *element_attr[VTSS_IF_MUX_ATTR_END];
    struct port_conf *p, *p_old;
    u8 vlan_filter[VLAN_MASK_LEN];

    if (!info->attrs[VTSS_IF_MUX_ATTR_PORT_CONF]) {
        printk(KERN_ERR "No port_conf attr\n");
        return -EINVAL;
    }

    nla_for_each_nested (conf, info->attrs[VTSS_IF_MUX_ATTR_PORT_CONF], rem) {

	    err = nla_parse_nested(element_attr, VTSS_IF_MUX_ATTR_MAX, conf, genel_policy, NULL);
        if (err < 0) {
            printk(KERN_ERR "Failed to parse port_conf elements\n");
            return -EINVAL;
        }

        // Chip port number
        port_attr = element_attr[VTSS_IF_MUX_ATTR_PORT_CONF_CHIP_PORT];
        if (!port_attr) {
            printk(KERN_ERR "No chip_port!\n");
            return -EINVAL;
        }
        chip_port = nla_get_u32(port_attr);
        if (chip_port >= PORT_CNT) {
            printk(KERN_ERR "illegal chip_port: %u!\n", chip_port);
            return -EINVAL;
        }

        // Ethernet type
        etype_attr = element_attr[VTSS_IF_MUX_ATTR_PORT_CONF_ETYPE];
        if (!etype_attr) {
            printk(KERN_ERR "No etype!\n");
            return -EINVAL;
        }

        // Ethernet type custom
        etype_custom_attr = element_attr[VTSS_IF_MUX_ATTR_PORT_CONF_ETYPE_CUSTOM];
        if (!etype_custom_attr) {
            printk(KERN_ERR "No etype_custom!\n");
            return -EINVAL;
        }

        // Rx filter
        rx_filter_attr = element_attr[VTSS_IF_MUX_ATTR_PORT_CONF_RX_FILTER];
        if (!rx_filter_attr) {
            printk(KERN_ERR "No rx_filter!\n");
            return -EINVAL;
        }

        // Rx forward
        rx_forward_attr = element_attr[VTSS_IF_MUX_ATTR_PORT_CONF_RX_FORWARD];
        if (!rx_forward_attr) {
            printk(KERN_ERR "No rx_forward!\n");
            return -EINVAL;
        }

        // Tx forward
        tx_forward_attr = element_attr[VTSS_IF_MUX_ATTR_PORT_CONF_TX_FORWARD];
        if (!tx_forward_attr) {
            printk(KERN_ERR "No tx_forward!\n");
            return -EINVAL;
        }

        // VLAN mask
        vlan_attr = element_attr[VTSS_IF_MUX_ATTR_PORT_CONF_VLAN_MASK];
        if (!vlan_attr) {
            printk(KERN_ERR "No vlan_mask!\n");
            return -EINVAL;
        }
        if (nla_len(vlan_attr) != VLAN_MASK_LEN) {
            printk(KERN_ERR "Unexpected length: %d!\n", nla_len(vlan_attr));
            return -EINVAL;
        }

        // Update port configuration
        p = kmalloc(sizeof(*p), GFP_KERNEL);
        if (p == NULL) {
            return -ENOMEM;
        }
        p->etype = nla_get_u32(etype_attr);
        p->etype_custom = nla_get_u32(etype_custom_attr);
        p->rx_filter = nla_get_u32(rx_filter_attr);
        p->rx_forward = nla_get_u32(rx_forward_attr);
        p->tx_forward = nla_get_u32(tx_forward_attr);
        memcpy(p->vlan_filter, nla_data(vlan_attr), VLAN_MASK_LEN);
        p_old = if_mux_port_conf[chip_port];
        mutex_lock(&vtss_if_mux_genl_sem);
        rcu_assign_pointer(if_mux_port_conf[chip_port], p);
        mutex_unlock(&vtss_if_mux_genl_sem);
        if (p_old != NULL) {
            kfree_rcu(p_old, rcu);
        }
    }

    // Update VLAN operational state
    memset(vlan_filter, 0xff, sizeof(vlan_filter));
    for (chip_port = 0; chip_port < PORT_CNT; chip_port++) {
        rcu_read_lock();
        p = rcu_dereference(if_mux_port_conf[chip_port]);
        if (p != NULL && p->tx_forward) {
            // Port is Tx forwarding, apply VLAN filter
            for (i = 0; i < VLAN_MASK_LEN; i++) {
                vlan_filter[i] &= p->vlan_filter[i];
            }
        }
        rcu_read_unlock();
    }
    for (i = 1; i < VLAN_N_VID; i++) {
        struct net_device *dev;
        int up = (vlan_filter[i/8] & (1 << (i % 8)) ? 0 : 1);

        if (vtss_if_mux_vlan_up[i] != up) {
            printk(KERN_INFO "VLAN %u %s!\n", i, up ? "UP" : "DOWN");
            vtss_if_mux_vlan_up[i] = up;
            dev = vtss_if_mux_vlan_net_dev[i];
            if (dev) {
                if (up) {
                    netif_carrier_on(dev);
                } else {
                    netif_carrier_off(dev);
                }
            }
        }
    }
    return 0;
}

u16 vtss_if_mux_vsi2vid(u16 vsi)
{
    return (vsi < VLAN_N_VID ? vsi2vid[vsi] : 0);
}

typedef struct {
    u16 vsi[VLAN_N_VID]; /**< Virtual Switching Instance number */
} vlan_vsi_map_t;

static int genl_cmd_vlan_vsi_map_set(struct sk_buff *skb, struct genl_info *info)
{
    int            vsi, vid;
    struct nlattr  *attr = info->attrs[VTSS_IF_MUX_ATTR_VLAN_VSI_MAP];
    vlan_vsi_map_t *map;

    if (!attr) {
        printk(KERN_ERR "no VLAN/NSI map\n");
        return -EINVAL;
    }
    if (nla_len(attr) != sizeof(*map)) {
        printk(KERN_ERR "unexpected length: %u\n", nla_len(attr));
        return -EINVAL;
    }
    //printk(KERN_ERR "VLAN/VSI map set\n");
    map = nla_data(attr);
    for (vsi = 1; vsi < VLAN_N_VID; vsi++) {
        vsi2vid[vsi] = 0;
    }
    for (vid = 1; vid < VLAN_N_VID; vid++) {
        vsi = map->vsi[vid];
        if (vsi) {
            vsi2vid[vsi] = vid;
            //printk(KERN_ERR "vid %u, vsi %u\n", vid, vsi);
        }
    }
    return 0;
}

static int genl_cmd_rule_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	printk(KERN_ERR "Not implemented yet: genl_cmd_rule_dump\n");
	return -ENOSYS;
}

static void debug_dump_element(struct seq_file *s, struct vtss_if_mux_filter_element *e)
{
#define CASE(X)                                \
	case VTSS_IF_MUX_FILTER_TYPE_##X:      \
		seq_printf(s, "    " #X ": "); \
		break

	switch (e->type) {
		CASE(port_mask);
		CASE(mac_src);
		CASE(mac_dst);
		CASE(mac_src_or_dst);
		CASE(vlan);
		CASE(ether_type);
		CASE(ipv4_src);
		CASE(ipv4_dst);
		CASE(ipv4_src_or_dst);
		CASE(ipv6_src);
		CASE(ipv6_dst);
		CASE(ipv6_src_or_dst);
		CASE(arp_operation);
		CASE(arp_hw_sender);
		CASE(arp_hw_target);
		CASE(arp_proto_sender);
		CASE(arp_proto_target);
		CASE(acl_id);
		CASE(arp_gratuitous);
	default:
		seq_printf(s, "    UNKNOWN (%d) !!!\n", (int)e->type);
		return;
	}
#undef CASE

	switch (e->type) {
	case VTSS_IF_MUX_FILTER_TYPE_port_mask:
		seq_printf(s, "%*pbl", PORT_MASK_BITS, e->data.mask_value);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_mac_src:
	case VTSS_IF_MUX_FILTER_TYPE_mac_dst:
	case VTSS_IF_MUX_FILTER_TYPE_mac_src_or_dst:
	case VTSS_IF_MUX_FILTER_TYPE_arp_hw_sender:
	case VTSS_IF_MUX_FILTER_TYPE_arp_hw_target:
		seq_printf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
			   (unsigned char)e->data.address[0],
			   (unsigned char)e->data.address[1],
			   (unsigned char)e->data.address[2],
			   (unsigned char)e->data.address[3],
			   (unsigned char)e->data.address[4],
			   (unsigned char)e->data.address[5]);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_vlan:
		seq_printf(s, "%u", e->data.i);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_ether_type:
	case VTSS_IF_MUX_FILTER_TYPE_arp_operation:
		seq_printf(s, "0x%04x", e->data.i);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_ipv4_src:
	case VTSS_IF_MUX_FILTER_TYPE_ipv4_dst:
	case VTSS_IF_MUX_FILTER_TYPE_ipv4_src_or_dst:
	case VTSS_IF_MUX_FILTER_TYPE_arp_proto_sender:
	case VTSS_IF_MUX_FILTER_TYPE_arp_proto_target:
		seq_printf(s, "%hhu.%hhu.%hhu.%hhu/%d",
			   (unsigned char)e->data.address[0],
			   (unsigned char)e->data.address[1],
			   (unsigned char)e->data.address[2],
			   (unsigned char)e->data.address[3],
			   e->prefix);
		break;

	case VTSS_IF_MUX_FILTER_TYPE_acl_id:
		seq_printf(s,
			   "Offset: %d %02hhx%02hhx%02hhx%02hhx "
			   "%02hhx%02hhx%02hhx%02hhx "
			   "%02hhx%02hhx%02hhx%02hhx "
			   "%02hhx%02hhx%02hhx%02hhx",
			   e->prefix,
			   (unsigned char) e->data.address[15],
			   (unsigned char) e->data.address[14],
			   (unsigned char) e->data.address[13],
			   (unsigned char) e->data.address[12],
			   (unsigned char) e->data.address[11],
			   (unsigned char) e->data.address[10],
			   (unsigned char) e->data.address[9],
			   (unsigned char) e->data.address[8],
			   (unsigned char) e->data.address[7],
			   (unsigned char) e->data.address[6],
			   (unsigned char) e->data.address[5],
			   (unsigned char) e->data.address[4],
			   (unsigned char) e->data.address[3],
			   (unsigned char) e->data.address[2],
			   (unsigned char) e->data.address[1],
			   (unsigned char) e->data.address[0]);
                break;

	case VTSS_IF_MUX_FILTER_TYPE_ipv6_src:
	case VTSS_IF_MUX_FILTER_TYPE_ipv6_dst:
	case VTSS_IF_MUX_FILTER_TYPE_ipv6_src_or_dst:
		seq_printf(s,
			   "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:"
			   "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx/%d",
			   (unsigned char) e->data.address[0],
			   (unsigned char) e->data.address[1],
			   (unsigned char) e->data.address[2],
			   (unsigned char) e->data.address[3],
			   (unsigned char) e->data.address[4],
			   (unsigned char) e->data.address[5],
			   (unsigned char) e->data.address[6],
			   (unsigned char) e->data.address[7],
			   (unsigned char) e->data.address[8],
			   (unsigned char) e->data.address[9],
			   (unsigned char) e->data.address[10],
			   (unsigned char) e->data.address[11],
			   (unsigned char) e->data.address[12],
			   (unsigned char) e->data.address[13],
			   (unsigned char) e->data.address[14],
			   (unsigned char) e->data.address[15],
			   e->prefix);
		break;
	}

	seq_printf(s, "\n");
}

static void debug_dump_rule(struct seq_file *s,
			    struct vtss_if_mux_filter_rule *r, bool black)
{
	int i;
	seq_printf(s, "  ID: %d, Owner: 0x%08llx White-list: %u", r->id,
		   r->owner, r->bitmaks_idx);
	if (black) {
		switch (r->action) {
		case VTSS_IF_MUX_ACTION_DROP:
			seq_printf(s, ", Action: DROP");
			break;
		case VTSS_IF_MUX_ACTION_CHECK_WHITE:
			seq_printf(s, ", Action: CHECK-WHITE-LIST");
			break;
		default:
			seq_printf(s, ", Action: UNKNOWN");
		}
	} else {
	}
	seq_printf(s, ", #Elements: %d\n", r->cnt);

	for (i = 0; i < r->cnt; ++i) {
		debug_dump_element(s, &r->elements[i]);
	}
}

static int debug_dump_(struct seq_file *s, void *v)
{
	struct vtss_if_mux_filter_rule *r;

	seq_printf(s, "BLACK_LIST:\n");
	rcu_read_lock();
	list_for_each_entry_rcu (r, &VTSS_IF_MUX_FILTER_BLACK_LIST, list) {
		debug_dump_rule(s, r, true);
	}

	seq_printf(s, "WHITE_LIST:\n");
	list_for_each_entry_rcu (r, &VTSS_IF_MUX_FILTER_WHITE_LIST, list) {
		debug_dump_rule(s, r, false);
	}
	rcu_read_unlock();
	seq_printf(s, "END\n");

	return 0;
}

static int debug_dump(struct inode *inode, struct file *f)
{
	return single_open(f, debug_dump_, NULL);
}

static int debug_dump_port_conf_(struct seq_file *s, void *v)
{
    u32              chip_port, i, n;
    u16              vid, vsi;
    struct port_conf *p;

    for (chip_port = 0; chip_port < PORT_CNT; chip_port++) {
        rcu_read_lock();
        p = rcu_dereference(if_mux_port_conf[chip_port]);
        if (p != NULL) {
            seq_printf(s, "\nCHIP_PORT   : %u\n", chip_port);
            seq_printf(s, "ETYPE       : 0x%04x\n", p->etype);
            seq_printf(s, "ETYPE_CUSTOM: 0x%04x\n", p->etype_custom);
            seq_printf(s, "RX_FILTER   : %u\n", p->rx_filter);
            seq_printf(s, "RX_FORWARD  : %u\n", p->rx_forward);
            seq_printf(s, "TX_FORWARD  : %u\n", p->tx_forward);
            seq_printf(s, "VLAN_MASK   :\n");
            for (i = 0; i < VLAN_MASK_LEN; i++) {
                n = (i & 15);
                if (n == 0) {
                    seq_printf(s, "%-4u-%-4u: ", i*8, i*8 + 127);
                }
                seq_printf(s, "%02x%s", p->vlan_filter[i], n == 15 ? "\n" : "-");
            }
        }
        rcu_read_unlock();
    }
    seq_printf(s, "\n");
    for (vsi = 1; vsi < VLAN_N_VID; vsi++) {
        vid = vsi2vid[vsi];
        if (vid) {
            seq_printf(s, "VSI: %u, VID: %u\n", vsi, vid);
        }
    }
    return 0;
}

static int debug_dump_port_conf(struct inode *inode, struct file *f)
{
	return single_open(f, debug_dump_port_conf_, NULL);
}

static struct genl_ops vtss_if_mux_genl_ops[] = {
	{
	 .cmd = VTSS_IF_MUX_GENL_NOOP,
	 .doit = vtss_if_mux_genl_cmd_noop,
	 // No access control
	},
	{
	 .cmd = VTSS_IF_MUX_GENL_RULE_CREATE,
	 .doit = vtss_if_mux_genl_cmd_rule_create,
	 .flags = GENL_ADMIN_PERM,
	},
	{
	 .cmd = VTSS_IF_MUX_GENL_RULE_DELETE,
	 .doit = genl_cmd_rule_delete,
	 .flags = GENL_ADMIN_PERM,
	},
	{
	 .cmd = VTSS_IF_MUX_GENL_RULE_MODIFY,
	 .doit = genl_cmd_rule_modify,
	 .flags = GENL_ADMIN_PERM,
	},
	{
	 .cmd = VTSS_IF_MUX_GENL_RULE_GET,
	 .doit = genl_cmd_rule_get,
	 .dumpit = genl_cmd_rule_dump,
	 .flags = GENL_ADMIN_PERM,
	},
	{
	 .cmd = VTSS_IF_MUX_GENL_PORT_CONF_SET,
	 .doit = genl_cmd_port_conf_set,
	 .flags = GENL_ADMIN_PERM,
	},
	{
	 .cmd = VTSS_IF_MUX_GENL_VLAN_VSI_MAP_SET,
	 .doit = genl_cmd_vlan_vsi_map_set,
	 .flags = GENL_ADMIN_PERM,
	},
};

static struct genl_family vtss_if_mux_genl_family = {
	.hdrsize = 0,
	.name = "vtss_if_mux",
	.version = 1,
	.maxattr = VTSS_IF_MUX_ATTR_MAX,
	.policy  = genel_policy,
	.ops     = vtss_if_mux_genl_ops,
	.n_ops   = ARRAY_SIZE(vtss_if_mux_genl_ops),
};

static const struct proc_ops dump_fops = {
	.proc_open = debug_dump,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops dump_port_conf = {
	.proc_open = debug_dump_port_conf,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int vtss_if_mux_genetlink_init(void)
{
	int err;

	INIT_LIST_HEAD_RCU(&VTSS_IF_MUX_FILTER_WHITE_LIST);
	INIT_LIST_HEAD_RCU(&VTSS_IF_MUX_FILTER_BLACK_LIST);
	INIT_LIST_HEAD(&OWNER_BIT_MASK_ASSOCIATION);

	proc_dump = proc_create("vtss_if_mux_filter", S_IRUGO, NULL, &dump_fops);
	proc_dump_port_conf = proc_create("vtss_if_mux_port_conf", S_IRUGO, NULL, &dump_port_conf);

	err = genl_register_family(&vtss_if_mux_genl_family);
	if (err == -1) {
		printk(KERN_ERR "genl_register_family failed\n");
	}

	return err;
}

void vtss_if_mux_genetlink_uninit(void)
{
        int i;
	if (proc_dump)
		proc_remove(proc_dump);
        if (proc_dump_port_conf)
            proc_remove(proc_dump_port_conf);

	genl_unregister_family(&vtss_if_mux_genl_family);

        for (i = 0; i < PORT_CNT; i++) {
            struct port_conf *p = if_mux_port_conf[i];
            mutex_lock(&vtss_if_mux_genl_sem);
            rcu_assign_pointer(if_mux_port_conf[i], NULL);
            mutex_unlock(&vtss_if_mux_genl_sem);
            if (p != NULL) {
                kfree_rcu(p, rcu);
            }
        }
}
