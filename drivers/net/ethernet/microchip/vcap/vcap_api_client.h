/*
 * Copyright (C) 2022 Microchip Technology Inc. and its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Microchip VCAP API
 */

#ifndef __VCAP_API_CLIENT__
#define __VCAP_API_CLIENT__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>

#include "vcap_api.h"

/* Client supplied VCAP rule key control part */
struct vcap_client_keyfield_ctrl {
	struct list_head list;  /* For insertion into a rule */
	enum vcap_key_field key;
	enum vcap_field_type type;
};

struct vcap_u1_key {
	u8 value;
	u8 mask;
};

struct vcap_u32_key {
	u32 value;
	u32 mask;
};

struct vcap_u48_key {
	u8 value[6];
	u8 mask[6];
};

struct vcap_u56_key {
	u8 value[7];
	u8 mask[7];
};

struct vcap_u64_key {
	u8 value[8];
	u8 mask[8];
};

struct vcap_u72_key {
	u8 value[9];
	u8 mask[9];
};

struct vcap_u112_key {
	u8 value[14];
	u8 mask[14];
};

struct vcap_u128_key {
	u8 value[16];
	u8 mask[16];
};

/* Client supplied VCAP rule field data */
struct vcap_client_keyfield_data {
	union {
		struct vcap_u1_key u1;
		struct vcap_u32_key u32;
		struct vcap_u48_key u48;
		struct vcap_u56_key u56;
		struct vcap_u64_key u64;
		struct vcap_u72_key u72;
		struct vcap_u112_key u112;
		struct vcap_u128_key u128;
	};
};

/* Client supplied VCAP rule key (value, mask) */
struct vcap_client_keyfield {
	struct vcap_client_keyfield_ctrl ctrl;
	struct vcap_client_keyfield_data data;
};

/* Client supplied VCAP rule action control part */
struct vcap_client_actionfield_ctrl {
	struct list_head list;  /* For insertion into a rule */
	enum vcap_action_field action;
	enum vcap_field_type type;
};

struct vcap_u1_action {
	u8 value;
};

struct vcap_u32_action {
	u32 value;
};

struct vcap_u48_action {
	u8 value[6];
};

struct vcap_u56_action {
	u8 value[7];
};

struct vcap_u64_action {
	u8 value[8];
};

struct vcap_u72_action {
	u8 value[9];
};

struct vcap_u112_action {
	u8 value[14];
};

struct vcap_u128_action {
	u8 value[16];
};

struct vcap_client_actionfield_data {
	union {
		struct vcap_u1_action u1;
		struct vcap_u32_action u32;
		struct vcap_u48_action u48;
		struct vcap_u56_action u56;
		struct vcap_u64_action u64;
		struct vcap_u72_action u72;
		struct vcap_u112_action u112;
		struct vcap_u128_action u128;
	};
};

struct vcap_client_actionfield {
	struct vcap_client_actionfield_ctrl ctrl;
	struct vcap_client_actionfield_data data;
};

enum vcap_bit {
	VCAP_BIT_ANY,
	VCAP_BIT_0,
	VCAP_BIT_1
};

struct vcap_counter {
	u32 value;
	bool sticky;
};

struct vcap_address {
	u32 start;
	u8 size;
};

struct vcap_key_list {
	int max; /* size of the key list */
	int cnt; /* count of keys actually in the list */
	enum vcap_key_field *keys; /* the list of keys */
};

struct vcap_keyset_match {
	struct vcap_keyset_list matches; /* fully matching the rule */
	enum vcap_keyfield_set best_match; /* matched keyset (not all keys) */
	struct vcap_key_list unmatched_keys; /* keys not found in partial match */
};

/* VCAP rule operations */
struct vcap_rule *vcap_alloc_rule(struct net_device *ndev,
				  int vcap_chain_id,
				  enum vcap_user user,
				  u16 priority,
				  u32 id);
/* Free mem of a rule owned by client */
void vcap_free_rule(struct vcap_rule *rule);
/* Validate a rule before adding it to the VCAP */
int vcap_val_rule(struct vcap_rule *rule, u16 l3_proto);
/* Transfer ownership of rule to VCAP library */
int vcap_add_rule(struct vcap_rule *rule);
/* Delete rule in the VCAP Library */
int vcap_del_rule(struct net_device *ndev, u32 id);
/* Give client a copy of the rule with ownership */
struct vcap_rule *vcap_get_rule(struct net_device *ndev, u32 id);
/* Update existing rule and transfer ownership of rule to VCAP library */
int vcap_mod_rule(struct vcap_rule *rule);
/* Set a rule counter id (for certain VCAPs only) */
void vcap_rule_set_counter_id(struct vcap_rule *rule, u32 counter_id);
/* Make a full copy of an existing rule with a new rule id */
struct vcap_rule *vcap_copy_rule(struct vcap_rule *erule);
/* Drop keys in a keylist and any keys that are not supported by the keyset */
int vcap_filter_rule_keys(struct vcap_rule *rule,
			  enum vcap_key_field keylist[], int length,
			  bool drop_unsupported);

/* Update the keyset for the rule */
int vcap_set_rule_set_keyset(struct vcap_rule *rule,
			     enum vcap_keyfield_set keyset);
/* Update the actionset for the rule */
int vcap_set_rule_set_actionset(struct vcap_rule *rule,
				enum vcap_actionfield_set actionset);

/* VCAP rule field operations */
int vcap_rule_add_key(struct vcap_rule *rule,
		      enum vcap_key_field key_id,
		      enum vcap_field_type ftype,
		      struct vcap_client_keyfield_data *data);
int vcap_rule_add_key_bit(struct vcap_rule *rule, enum vcap_key_field key,
			  enum vcap_bit val);
int vcap_rule_add_key_u32(struct vcap_rule *rule, enum vcap_key_field key,
			  u32 value, u32 mask);
int vcap_rule_add_key_u48(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u48_key *fieldval);
int vcap_rule_add_key_u56(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u56_key *fieldval);
int vcap_rule_add_key_u64(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u64_key *fieldval);
int vcap_rule_add_key_u72(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u72_key *fieldval);
int vcap_rule_add_key_u112(struct vcap_rule *rule, enum vcap_key_field key,
			   struct vcap_u112_key *fieldval);
int vcap_rule_add_key_u128(struct vcap_rule *rule, enum vcap_key_field key,
			   struct vcap_u128_key *fieldval);
int vcap_rule_add_action(struct vcap_rule *rule,
			 enum vcap_action_field action,
			 enum vcap_field_type ftype,
			 struct vcap_client_actionfield_data *data);
int vcap_rule_add_action_bit(struct vcap_rule *rule,
			     enum vcap_action_field action, enum vcap_bit val);
int vcap_rule_add_action_u32(struct vcap_rule *rule,
			     enum vcap_action_field action, u32 value);
int vcap_rule_add_action_u48(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u48_action *fieldval);
int vcap_rule_add_action_u56(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u56_action *fieldval);
int vcap_rule_add_action_u64(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u64_action *fieldval);
int vcap_rule_add_action_u72(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u72_action *fieldval);
int vcap_rule_add_action_u112(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u112_action *fieldval);
int vcap_rule_add_action_u128(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u128_action *fieldval);
int vcap_rule_mod_key(struct vcap_rule *rule,
		      enum vcap_key_field key,
		      enum vcap_field_type ftype,
		      struct vcap_client_keyfield_data *data);
int vcap_rule_mod_key_bit(struct vcap_rule *rule, enum vcap_key_field key,
			  enum vcap_bit val);
int vcap_rule_mod_key_u32(struct vcap_rule *rule, enum vcap_key_field key,
			  u32 value, u32 mask);
int vcap_rule_mod_key_u48(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u48_key *fieldval);
int vcap_rule_mod_key_u56(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u56_key *fieldval);
int vcap_rule_mod_key_u64(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u64_key *fieldval);
int vcap_rule_mod_key_u72(struct vcap_rule *rule, enum vcap_key_field key,
			  struct vcap_u72_key *fieldval);
int vcap_rule_mod_key_u112(struct vcap_rule *rule, enum vcap_key_field key,
			   struct vcap_u112_key *fieldval);
int vcap_rule_mod_key_u128(struct vcap_rule *rule, enum vcap_key_field key,
			   struct vcap_u128_key *fieldval);
int vcap_rule_mod_action(struct vcap_rule *rule,
			 enum vcap_action_field action,
			 enum vcap_field_type ftype,
			 struct vcap_client_actionfield_data *data);
int vcap_rule_mod_action_bit(struct vcap_rule *rule,
			     enum vcap_action_field action, enum vcap_bit val);
int vcap_rule_mod_action_u32(struct vcap_rule *rule,
			     enum vcap_action_field action, u32 value);
int vcap_rule_mod_action_u48(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u48_action *fieldval);
int vcap_rule_mod_action_u56(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u56_action *fieldval);
int vcap_rule_mod_action_u64(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u64_action *fieldval);
int vcap_rule_mod_action_u72(struct vcap_rule *rule, enum vcap_action_field action,
			     struct vcap_u72_action *fieldval);
int vcap_rule_mod_action_u112(struct vcap_rule *rule, enum vcap_action_field action,
			      struct vcap_u112_action *fieldval);
int vcap_rule_mod_action_u128(struct vcap_rule *rule, enum vcap_action_field action,
			      struct vcap_u128_action *fieldval);

int vcap_rule_rem_key(struct vcap_rule *rule, enum vcap_key_field key);
int vcap_rule_rem_action(struct vcap_rule *rule, enum vcap_action_field action);

/* VCAP rule counter operations */
int vcap_rule_set_counter(u32 id, struct vcap_counter *ctr);
int vcap_rule_get_counter(u32 id, struct vcap_counter *ctr);

/* Find vcap type instance count */
int vcap_admin_type_count(enum vcap_type vt);
/* Lookup a vcap instance using chain id */
struct vcap_admin *vcap_find_admin(int cid);
/* Get a vcap instance from a rule */
struct vcap_admin *vcap_rule_get_admin(struct vcap_rule *rule);
/* Convert a chain id to a VCAP lookup index */
int vcap_chain_id_to_lookup(struct vcap_admin *admin, int cid);
/* Get number of rules in a vcap instance lookup chain id range */
int vcap_admin_rule_count(struct vcap_admin *admin, int cid);

/* Find a vcap instance and chain id using vcap type and lookup index */
struct vcap_admin *vcap_find_admin_with_lookup(enum vcap_type vt,
					       int lookup,
					       int *cid);

/* Find information on a key field in a rule */
const struct vcap_field *vcap_lookup_keyfield(struct vcap_rule *rule,
					      enum vcap_key_field key);
/* Find information on a action field in a rule */
const struct vcap_field *vcap_lookup_actionfield(struct vcap_rule *rule,
					      enum vcap_action_field action);
/* Return the keyset information for the keyset */
const struct vcap_set *vcap_keyfieldset(enum vcap_type vt,
					enum vcap_keyfield_set keyset);
/* Return the actionset information for the actionset */
const struct vcap_set *vcap_actionfieldset(enum vcap_type vt,
					   enum vcap_actionfield_set actionset);

/* Return the number of keyfields in the keyset */
int vcap_keyfield_count(enum vcap_type vt, enum vcap_keyfield_set keyset);
/* Return the list of keyfields for the keyset */
const struct vcap_field *vcap_keyfields(enum vcap_type vt,
					enum vcap_keyfield_set keyset);
/* Return the number of actionfields in the actionset */
int vcap_actionfield_count(enum vcap_type vt,
			   enum vcap_actionfield_set actionset);
/* Return the list of actionfields for the actionset */
const struct vcap_field *vcap_actionfields(enum vcap_type vt,
					   enum vcap_actionfield_set actionset);
/* Find a client key field in a rule */
struct vcap_client_keyfield *vcap_find_keyfield(struct vcap_rule *rule,
						enum vcap_key_field key);
/* Find a client action field in a rule */
struct vcap_client_actionfield *vcap_find_actionfield(struct vcap_rule *rule,
						      enum vcap_action_field act);

/* Find a rule id with a provided cookie */
int vcap_lookup_rule_by_cookie(u64 cookie);

/* Provide all rules via a callback interface */
int vcap_rule_iter(int (*callback)(void *, struct vcap_rule *), void *arg);

/* Add a keyset to a keyset list */
bool vcap_keyset_list_add(struct vcap_keyset_list *keysetlist,
			  enum vcap_keyfield_set keyset);
/* Add a key to a key list */
bool vcap_key_list_add(struct vcap_key_list *keylist, enum vcap_key_field key);
/* Match a list of keys against the keysets available in a vcap type */
bool vcap_rule_match_keysets(enum vcap_type vtype,
			     struct vcap_key_list *keylist,
			     struct vcap_keyset_match *match);
/* Return keyset information that matches the keys in the rule */
bool vcap_rule_find_keysets(struct vcap_rule *rule,
			   struct vcap_keyset_match *match);

/* Cleanup a VCAP instance */
int vcap_del_rules(struct vcap_admin *admin);

/* VCAP rule address operations for netlink */
int vcap_rule_get_address(struct net_device *ndev, u32 id,
			  struct vcap_address *addr);

/* Debug interface */
void vcap_show_rule(int (*pf)(void *ct, const char *f, ...), void *ct,
		    struct vcap_admin *admin,
		    struct vcap_rule *rule);
int vcap_show_admin_info(int (*pf)(void *ct, const char *fmt, ...), void *ct,
			 struct vcap_admin *admin);
int vcap_show_admin(int (*pf)(void *ct, const char *fmt, ...), void *ct,
		    struct vcap_admin *admin);
int vcap_show_admin_raw(int (*pf)(void *ct, const char *fmt, ...), void *ct,
			struct vcap_admin *admin);

#endif /* __VCAP_API_CLIENT__ */
