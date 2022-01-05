/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2019 Microchip Technology Inc. */

#ifndef _LAN966X_VCAP_H_
#define _LAN966X_VCAP_H_

#include <linux/mutex.h>
#include "lan966x_board.h"
#include "lan966x_vcap_types.h"
#include "lan966x_qos.h"

#if defined(ASIC)
#include "lan966x_vcap_ag_api.h"
#else
#include "lan966x_vcap_ag_api_fpga.h"
#endif

/* Forward declarations to avoid including lan966x_main.h */
struct lan966x;
struct lan966x_port;

#define LAN966X_VCAP_LOOKUP_MASK 0x3 /* 2 bit field */
#define LAN966X_VCAP_NUM_LOOKUPS_ES0 1
#define LAN966X_VCAP_NUM_LOOKUPS_IS1 3
#define LAN966X_VCAP_NUM_LOOKUPS_IS2 2
#define LAN966X_VCAP_NUM_LOOKUPS_MAX 3

/**
 * enum lan966x_vcap_user - Enumerates the users of the VCAP library
 *
 * Each user has a priority that is determined by the enum value.
 * The first enum has the highest priority.
 * The last enum has the lowest priority.
 */
enum lan966x_vcap_user {
	LAN966X_VCAP_USER_PTP,
	LAN966X_VCAP_USER_MRP,
	LAN966X_VCAP_USER_CFM,
	LAN966X_VCAP_USER_VLAN,
	LAN966X_VCAP_USER_QOS,
	LAN966X_VCAP_USER_VCAP_UTIL,

	/* add new users above here */
	LAN966X_VCAP_USER_TC,
	LAN966X_VCAP_USER_TC_IS2_X4_ALL,

	/* used to define LAN966X_VCAP_USER_MAX below */
	__LAN966X_VCAP_USER_AFTER_LAST,
	LAN966X_VCAP_USER_MAX = __LAN966X_VCAP_USER_AFTER_LAST - 1,
};

/**
 * struct lan966x_vcap_is1_rule - Combines key and action for IS1 rules
 */
struct lan966x_vcap_is1_rule {
	struct lan966x_vcap_is1_key_fields key;
	struct lan966x_vcap_is1_action_fields action;
};

/**
 * struct lan966x_vcap_is2_rule - Combines key and action for IS2 rules
 */
struct lan966x_vcap_is2_rule {
	struct lan966x_vcap_is2_key_fields key;
	struct lan966x_vcap_is2_action_fields action;
};

/**
 * struct lan966x_vcap_es0_rule - Combines key and action for ES0 rules
 */
struct lan966x_vcap_es0_rule {
	struct lan966x_vcap_es0_key_fields key;
	struct lan966x_vcap_es0_action_fields action;
};

/**
 * struct lan966x_vcap_rule - Combines all rules in a union and adds rule
 * related objects.
 * @is1: IS1 rule.
 * @is2: IS2 rule.
 * @es0: ES0 rule.
 * @sfi: Rule is associated with a stream filter instance.
 * @sfi_ix: Rule is associated with this stream filter instance index.
 * @sgi_user: Rule is associated with a stream gate instance for this user.
 * @sgi_id: Rule is associated with this stream gate instance id.
 * @pol_user: Rule is associated with a policer for this user.
 * @pol_id: Rule is associated with this policer id.
 * @mirroring: Rule is associated with mirroring.
 * @is2_x4_all: IS2 rule is associated with an extra X4 match all rule.
 */
struct lan966x_vcap_rule {
	union {
		struct lan966x_vcap_is1_rule is1;
		struct lan966x_vcap_is2_rule is2;
		struct lan966x_vcap_es0_rule es0;
	};
	bool sfi;
	u32 sfi_ix;
	enum lan966x_res_pool_user sgi_user;
	u32 sgi_id;
	enum lan966x_res_pool_user pol_user;
	u32 pol_id;
	bool mirroring;
	bool is2_x4_all;
};

/**
 * struct lan966x_vcap_rule_entry - List entry for each rule
 * @list: List for linking entries.
 * @size: Number of subwords occupied in VCAP. 1, 2 or 4. 4 = highest priority.
 * @user: The user of this entry. 0 = highest priority.
 * @prio: The priority of this entry. 0 = highest priority.
 * @cookie: User supplied value for identifying this entry.
 * @sort_key: Sort key generated from size, user and prio. Optimizes insertion.
 * @rule: The rule for this entry that is packed and transferred to hw.
 *
 * These entries are sorted in the same way as they are written in the hw.
 */
struct lan966x_vcap_rule_entry {
	struct list_head list;
	u8 size;
	enum lan966x_vcap_user user;
	u16 prio;
	unsigned long cookie; /* must match coockie size in tc */
	u32 sort_key;
	struct lan966x_vcap_rule rule;
};

/**
 * enum lan966x_vcap_admin - Administration struct for each VCAP
 * @list: List for linking entries.
 * @lock: Protects the list during traversal, inserts and removals.
 * @last_valid_addr: Last valid address in VCAP. Initialized to the number of
 *		     addresses in the TCAM - 1.
 * @last_used_addr: Last used address in VCAP. Initialized to the number of
 *		    addresses in the TCAM and counts down when antries are added.
 * @num_rules: Number of rules in list for each lookup.
 */
struct lan966x_vcap_admin {
	struct list_head list;
	struct mutex lock;
	u32 last_valid_addr;
	u32 last_used_addr;
	u32 num_rules[LAN966X_VCAP_NUM_LOOKUPS_MAX];
};

/**
 * struct lan966x_vcap_is1_port_admin - Administration struct per port for each
 * VCAP IS1 lookup.
 * @smac: If true then match on SMAC instead of DMAC in key S1_DMAC_VID.
 * @dmac_dip: If true then match on DMAC/DIP instead of SMAC/SIP in key
 *            S1_NORMAL and S1_NORMAL_IP6.
 * @key_ip6: Key to generate in IS1 for IPv6 frames.
 * @key_ip4: Key to generate in IS1 for IPv4 frames.
 * @key_other: Key to generate in IS1 for all other frames than above.
 */
struct lan966x_vcap_is1_port_admin {
	bool smac;
	bool dmac_dip;
	enum lan966x_vcap_is1_key key_ip6;
	enum lan966x_vcap_is1_key key_ip4;
	enum lan966x_vcap_is1_key key_other;
};

/**
 * struct lan966x_vcap_is2_port_admin - Administration struct per port for each
 * VCAP IS2 lookup.
 * @key_ip6: Key to generate in IS2 for IPv6 frames.
 */
struct lan966x_vcap_is2_port_admin {
	enum lan966x_vcap_is2_key key_ip6;
};

/*******************************************************************************
 * VCAP control
 ******************************************************************************/
/**
 * lan966x_vcap_add - Add a new VCAP entry
 * @lan966x: switch device.
 * @vcap: VCAP to use.
 * @user: which user this entry belongs to. Low value = high priority.
 * @prio: priority within each user. Low value = high priority.
 * @cookie: cookie. Must be unique within each priority.
 * @rule: rule to add.
 *
 * It is allowed to add more than one entry with the same priority within each
 * user as long as the cookie is different.
 * The latest entry added has the highest priority.
 *
 * An error is returned if an entry exists with same user, prio and cookie.
 */
int lan966x_vcap_add(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     const struct lan966x_vcap_rule *rule);

/**
 * lan966x_vcap_mod - Modify an existing VCAP entry
 * @lan966x: switch device.
 * @vcap: VCAP to use.
 * @user: entry user.
 * @prio: entry priority.
 * @cookie: entry cookie.
 * @rule: new rule that replace the old rule.
 *
 * An entry must exists with same user, prio and cookie.
 *
 * Modifying key or action in the rule is only allowed if the key and action
 * size remains the same.
 *
 * The rule to modify is normally fetched with lan966x_vcap_get.
 */
int lan966x_vcap_mod(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     const struct lan966x_vcap_rule *rule);

/**
 * lan966x_vcap_get - Get an existing VCAP entry and corresponding hit counter
 * @lan966x: switch device.
 * @vcap: VCAP to use.
 * @user: entry user.
 * @prio: entry priority.
 * @cookie: entry cookie.
 * @rule: returned rule. Set to NULL if no interest in rule.
 * @hits: returned hit counter. Set to NULL if no interest in hit counter.
 *        The returned value is relative to the last time lan966x_vcap_get()
 *        was called with hits != NULL.
 */
int lan966x_vcap_get(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     struct lan966x_vcap_rule *rule,
		     u32 *hits);

/**
 * lan966x_vcap_del - Delete an existing VCAP rule
 * @lan966x: switch device.
 * @vcap: VCAP to use.
 * @user: entry user.
 * @prio: entry priority.
 * @cookie: entry cookie.
 * @rule: deleted rule. Set to NULL if no interest in rule.
 */
int lan966x_vcap_del(struct lan966x *lan966x,
		     enum lan966x_vcap vcap,
		     enum lan966x_vcap_user user,
		     u16 prio, unsigned long cookie,
		     struct lan966x_vcap_rule *rule);


/*******************************************************************************
 * VCAP configuration
 ******************************************************************************/
/**
 * lan966x_vcap_igr_port_mask_set - Set ingress port mask in VCAP rule
 * @lan966x: switch device.
 * @vcap: VCAP to use.
 * @rule: VCAP rule.
 * @mask: Port mask.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if key is invalid or has no port mask
 */
int lan966x_vcap_igr_port_mask_set(struct lan966x *lan966x,
				   enum lan966x_vcap vcap,
				   struct lan966x_vcap_rule *rule,
				   const struct lan966x_vcap_u16 *mask);

/**
 * lan966x_vcap_igr_port_mask_get - Get ingress port mask from VCAP rule
 * @lan966x: switch device.
 * @vcap: VCAP to use.
 * @rule: VCAP rule.
 * @mask: Port mask.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if key is invalid or has no port mask
 */
int lan966x_vcap_igr_port_mask_get(struct lan966x *lan966x,
				   enum lan966x_vcap vcap,
				   const struct lan966x_vcap_rule *rule,
				   struct lan966x_vcap_u16 *mask);

/**
 * lan966x_vcap_num_rules_get - Get number of rules for a specific lookup
 * @lan966x: switch device.
 * @vcap: VCAP to use.
 * @lookup: VCAP lookup.
 *
 * Returns:
 * number of rules for lookup
 * -EINVAL if vcap or lookup is invalid
 */
int lan966x_vcap_num_rules_get(const struct lan966x *lan966x,
			       enum lan966x_vcap vcap,
			       u8 lookup);

/**
 * lan966x_vcap_is1_port_smac_set - Set smac for a specific port/lookup.
 * Only allowed if VCAP IS1 is empty.
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @smac: The smac value.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup is invalid
 * -EBUSY if IS1 is not empty
 */
int lan966x_vcap_is1_port_smac_set(struct lan966x_port *port,
				   u8 lookup,
				   bool smac);

/**
 * lan966x_vcap_is1_port_smac_get - Get smac for a specific port/lookup
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @smac: The smac value.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup is invalid
 */
int lan966x_vcap_is1_port_smac_get(const struct lan966x_port *port,
				   u8 lookup,
				   bool *smac);

/**
 * lan966x_vcap_is1_port_dmac_dip_set - Set dmac_dip for a specific port/lookup.
 * Only allowed if VCAP IS1 is empty.
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @dmac_dip: The dmac_dip value.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup is invalid
 * -EBUSY if IS1 is not empty
 */
int lan966x_vcap_is1_port_dmac_dip_set(struct lan966x_port *port,
				       u8 lookup,
				       bool dmac_dip);

/**
 * lan966x_vcap_is1_port_dmac_dip_get - Get dmac_dip for a specific port/lookup.
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @dmac_dip: The dmac_dip value.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup is invalid
 */
int lan966x_vcap_is1_port_dmac_dip_get(const struct lan966x_port *port,
				       u8 lookup,
				       bool *dmac_dip);

/**
 * enum lan966x_vcap_is1_frame_type - Enumerates the frame types used in IS1
 */
enum lan966x_vcap_is1_frame_type {
	LAN966X_VCAP_IS1_FRAME_TYPE_IPV4,
	LAN966X_VCAP_IS1_FRAME_TYPE_IPV6,
	LAN966X_VCAP_IS1_FRAME_TYPE_OTHER,
	LAN966X_VCAP_IS1_FRAME_TYPE_ALL,
};

/**
 * lan966x_vcap_is1_port_key_set - Set key value for a specific port/lookup/frame_type.
 * Only allowed if VCAP IS1 is empty.
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @frame_type: frame type.
 * @key: The key to generate. Valid values depends on the frame type:
 *
 *   IPV4:
 *     S1_NORMAL,
 *     S1_7TUPLE,
 *     S1_5TUPLE_IP4,
 *     S1_DBL_VID,
 *     S1_DMAC_VID.
 *     Defaults to S1_7TUPLE.
 *
 *   IPV6:
 *     S1_NORMAL,
 *     S1_7TUPLE,
 *     S1_5TUPLE_IP4,
 *     S1_NORMAL_IP6,
 *     S1_5TUPLE_IP6,
 *     S1_DBL_VID,
 *     S1_DMAC_VID.
 *     Defaults to S1_7TUPLE.
 *
 *   OTHER and ALL:
 *     S1_NORMAL,
 *     S1_7TUPLE,
 *     S1_DBL_VID,
 *     S1_DMAC_VID.
 *     Defaults to S1_7TUPLE.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup, frame_type or key is invalid
 * -EBUSY if IS1 is not empty
 */
int lan966x_vcap_is1_port_key_set(struct lan966x_port *port,
				  u8 lookup,
				  enum lan966x_vcap_is1_frame_type frame_type,
				  enum lan966x_vcap_is1_key key);

/**
 * lan966x_vcap_is1_port_key_get - Get key value for a specific port/lookup/frame_type.
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @frame_type: frame type.
 * @key: The key to generate for frame_type.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup is invalid
 */
int lan966x_vcap_is1_port_key_get(const struct lan966x_port *port,
				  u8 lookup,
				  enum lan966x_vcap_is1_frame_type frame_type,
				  enum lan966x_vcap_is1_key *key);

/**
 * lan966x_vcap_is2_port_key_ipv6_set - Set key_ipv6 value for a specific port/lookup.
 * Only allowed if VCAP IS2 is empty.
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @key: The key to generate for IPv6 frames. Must be one of:
 *       MAC_ETYPE,
 *       IP4_TCP_UDP, (Non TCP_UDP IPv6 generates IP4_OTHER)
 *       IP6_STD,
 *       IP6_TCP_UDP. (Non TCP_UDP IPv6 generates IP6_OTHER)
 *       Defaults to IP4_TCP_UDP.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup or key is invalid
 * -EBUSY if IS2 is not empty
 */
int lan966x_vcap_is2_port_key_ipv6_set(struct lan966x_port *port,
				       u8 lookup,
				       enum lan966x_vcap_is2_key key);

/**
 * lan966x_vcap_is2_port_key_ipv6_get - Get key_ipv6 value for a specific port/lookup.
 * @port: ingress port.
 * @lookup: VCAP lookup.
 * @key: The key to generate for real-time frames.
 *
 * Returns:
 * 0 if ok
 * -EINVAL if lookup is invalid
 */
int lan966x_vcap_is2_port_key_ipv6_get(const struct lan966x_port *port,
				       u8 lookup,
				       enum lan966x_vcap_is2_key *key);

/*******************************************************************************
 * Utilities used by pack functions
 ******************************************************************************/
/**
 * lan966x_vcap_key_set - Set up to 32 bita in a key field
 * @data: where to write the bits.
 * @offset: where to start writing the bits.
 * @width: number of bits to write.
 * @value: value to write.
 * @mask: mask to write.
 */
void lan966x_vcap_key_set(struct lan966x_vcap_data *data, u32 offset,
			  u32 width, u32 value, u32 mask);

/**
 * lan966x_vcap_key_bit_set - Set a single bit in a key field
 * @data: where to write the bits.
 * @offset: where to start writing the bits.
 * @val: value to write.
 */
void lan966x_vcap_key_bit_set(struct lan966x_vcap_data *data, u32 offset,
			      enum lan966x_vcap_bit val);

/**
 * lan966x_vcap_key_bytes_set - Set more than 32 bits in a key field
 * @data: where to write the bits.
 * @offset: where to start writing the bits.
 * @val: array of values to write.
 * @msk: array of masks to write.
 * @count: number of bytes to write.
 *
 * This function is normally used for writing MAC or IPv6 addresses.
 */
void lan966x_vcap_key_bytes_set(struct lan966x_vcap_data *data,
				u32 offset, const u8 *val, const u8 *msk,
				u32 count);

/**
 * lan966x_vcap_action_set - Set up to 32 bits in an action field
 * @data: where to write the bits.
 * @offset: where to start writing the bits.
 * @width: number of bits to write.
 * @value: value to write.
 */
void lan966x_vcap_action_set(struct lan966x_vcap_data *data, u32 offset,
			     u32 width, u32 value);

/**
 * lan966x_vcap_action_bit_set - Set a single bit in an action field
 * @data: where to write the bits.
 * @offset: where to start writing the bits.
 * @val: value to write.
 */
void lan966x_vcap_action_bit_set(struct lan966x_vcap_data *data, u32 offset,
				 u32 value);

/*******************************************************************************
 * Initialization
 ******************************************************************************/
void lan966x_vcap_init(struct lan966x *lan966x);
void lan966x_vcap_uninit(struct lan966x *lan966x);

void lan966x_vcap_port_enable(struct lan966x *lan966x,
			      struct lan966x_port *port);

#endif
