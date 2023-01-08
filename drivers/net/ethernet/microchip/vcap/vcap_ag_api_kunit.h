/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2022 Microchip Technology Inc. and its subsidiaries.
 * Microchip VCAP API interface for kunit testing
 * This is a different interface, to be able to include different VCAPs
 */

/* Use same include guard as the official API to be able to override it */
#ifndef __VCAP_AG_API__
#define __VCAP_AG_API__

enum vcap_type {
	VCAP_TYPE_ES0,
	VCAP_TYPE_ES2,
	VCAP_TYPE_IS0,
	VCAP_TYPE_IS1,
	VCAP_TYPE_IS2,
	VCAP_TYPE_MAX
};

/* Keyfieldset names with origin information */
enum vcap_keyfield_set {
	VCAP_KFS_NO_VALUE,                        /* initial value */
	VCAP_KFS_ARP,                             /* (sparx5 s2 X6), (sparx5 es2 X6), (lan966x s2 X2) */
	VCAP_KFS_CUSTOM,                          /*** This should be filtered out? (lan966x s2 X4) */
	VCAP_KFS_ETAG,                            /* (sparx5 s0 X2) */
	VCAP_KFS_IP4_OTHER,                       /* (sparx5 s2 X6), (sparx5 es2 X6), (lan966x s2 X2) */
	VCAP_KFS_IP4_TCP_UDP,                     /* (sparx5 s2 X6), (sparx5 es2 X6), (lan966x s2 X2) */
	VCAP_KFS_IP4_VID,                         /* (sparx5 es2 X3) */
	VCAP_KFS_IP6_OTHER,                       /* (lan966x s2 X4) */
	VCAP_KFS_IP6_STD,                         /* (lan966x s2 X2) */
	VCAP_KFS_IP6_TCP_UDP,                     /* (lan966x s2 X4) */
	VCAP_KFS_IP6_VID,                         /* (sparx5 s2 X6), (sparx5 es2 X6) */
	VCAP_KFS_IP_7TUPLE,                       /* (sparx5 s2 X12), (sparx5 es2 X12) */
	VCAP_KFS_LL_FULL,                         /* (sparx5 s0 X6) */
	VCAP_KFS_MAC_ETYPE,                       /* (sparx5 s2 X6), (sparx5 es2 X6), (lan966x s2 X2) */
	VCAP_KFS_MAC_LLC,                         /* (lan966x s2 X2) */
	VCAP_KFS_MAC_SNAP,                        /* (lan966x s2 X2) */
	VCAP_KFS_MLL,                             /* (sparx5 s0 X3) */
	VCAP_KFS_NORMAL,                          /* (sparx5 s0 X6) */
	VCAP_KFS_NORMAL_5TUPLE_IP4,               /* (sparx5 s0 X6) */
	VCAP_KFS_NORMAL_7TUPLE,                   /* (sparx5 s0 X12) */
	VCAP_KFS_OAM,                             /* (lan966x s2 X2) */
	VCAP_KFS_PURE_5TUPLE_IP4,                 /* (sparx5 s0 X3) */
	VCAP_KFS_S1_5TUPLE_IP4,                   /*** Maybe reuse VCAP_KFS_NORMAL_5TUPLE_IP4 (lan966x s1 X2) */
	VCAP_KFS_S1_5TUPLE_IP6,                   /* (lan966x s1 X4) */
	VCAP_KFS_S1_7TUPLE,                       /*** Maybe Reuse VCAP_KFS_IP_7TUPLE? (lan966x s1 X4) */
	VCAP_KFS_S1_DBL_VID,                      /*** Could be filtered out? (lan966x s1 X1) */
	VCAP_KFS_S1_DMAC_VID,                     /* (lan966x s1 X1) */
	VCAP_KFS_S1_NORMAL,                       /*** Maybe reuse VCAP_KFS_NORMAL? (lan966x s1 X2) */
	VCAP_KFS_S1_NORMAL_IP6,                   /* (lan966x s1 X4) */
	VCAP_KFS_S1_RT,                           /* (lan966x s1 X1) */
	VCAP_KFS_SMAC_SIP4,                       /* (lan966x s2 X1) */
	VCAP_KFS_SMAC_SIP6,                       /* (lan966x s2 X2) */
	VCAP_KFS_VID,                             /* (lan966x es0 X1) */
};

/* Keyfield names with origin information */
enum vcap_key_field {
	VCAP_KF_ACL_GRP_ID,                      /* Used in interface map table (sparx5 es2 W8) */
	VCAP_KF_ARP_ADDR_SPACE_OK,               /* Set if hardware address is Ethernet (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_ARP_LEN_OK,                      /* Set if hardware address length = 6 (Ethernet) and IP address length = 4 (IP). (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_ARP_OPCODE,                      /* ARP opcode (sparx5 s2 W2), (sparx5 es2 W2), (lan966x s2 W2) */
	VCAP_KF_ARP_OPCODE_UNKNOWN,              /* Set if not one of the codes defined in VCAP_KF_ARP_OPCODE (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_ARP_PROTO_SPACE_OK,              /* Set if protocol address space is 0x0800 (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_ARP_SENDER_MATCH,                /* Sender Hardware Address = SMAC (ARP) (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_ARP_TGT_MATCH,                   /* Target Hardware Address = SMAC (RARP) (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_COLOR,                           /* The frames drop precedence level mapped through DP_MAP configuration (sparx5 es2 W1) */
	VCAP_KF_COSID,                           /* Class of service (sparx5 es2 W3) */
	VCAP_KF_CUSTOM,                          /* Custom extracted data (only in the CUSTOM keyset) (lan966x s2 W320) */
	VCAP_KF_CUSTOM_TYPE,                     /* Special extract from payload (only in the CUSTOM keyset) (lan966x s2 W1) */
	VCAP_KF_DEI,                             /* LAN966x: Outer DEI (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1), (lan966x es0 W1) */
	VCAP_KF_DEI0,                            /* First DEI in multiple tags in CLM in the "TUPLE" keysets (sparx5 s0 W1) */
	VCAP_KF_DEI1,                            /* Second -"- (sparx5 s0 W1) */
	VCAP_KF_DEI2,                            /* Third -"- (sparx5 s0 W1) */
	VCAP_KF_DIP_EQ_SIP,                      /* Match IP boolean (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_DP,                              /* Frame’s drop precedence (DP) level after policing (similar to VCAP_KF_COLOR?) (lan966x es0 W1) */
	VCAP_KF_DST_ENTRY,                       /* Set if destination entry: probably the DMAC has matched an entry in the MAC Table (sparx5 s0 W1) */
	VCAP_KF_ECID_BASE,                       /* Used by 802.1BR Bridge Port Extention in an E-Tag (sparx5 s0 W12) */
	VCAP_KF_ECID_EXT,                        /* Used by 802.1BR Bridge Port Extention in an E-Tag (sparx5 s0 W8) */
	VCAP_KF_EGR_PORT,                        /* LAN966x has a simple egress port mask (lan966x es0 W4) */
	VCAP_KF_EGR_PORT_MASK,                   /* Sparx5 has a range setting for the egress ports, so only 32 physical ports in a range can be selected (sparx5 es2 W32) */
	VCAP_KF_EGR_PORT_MASK_RNG,               /* For Sparx5 this select which 32 port group is selected (or virtual ports or CPU queue) (sparx5 es2 W3) */
	VCAP_KF_ES0_ISDX_KEY_ENA,                /* The value taken from the IFH .FWD.ES0_ISDX_KEY_ENA (sparx5 es2 W1) */
	VCAP_KF_ETAGGED,                         /* Set for frames containing an E-TAG (802.1BR Ethertype 893f) (sparx5 s0 W1) */
	VCAP_KF_ETYPE,                           /* (sparx5 s0 W16), (sparx5 s2 W16), (sparx5 es2 W16), (lan966x s1 W16), (lan966x s2 W16) */
	VCAP_KF_ETYPE_LEN,                       /* Set if frame has EtherType >= 0x600 (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1) */
	VCAP_KF_ETYPE_MPLS,                      /* Type of MPLS Ethertype (or not) (sparx5 s0 W2) */
	VCAP_KF_FIRST,                           /* First or Second Lookup selection (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_KF_GRP,                             /* E-Tag group bits in 802.1BR Bridge Port Extension (sparx5 s0 W2) */
	VCAP_KF_G_IDX,                           /* Generic index - for chaining CLM instances (sparx5 s0 W12) */
	VCAP_KF_G_IDX_SEL,                       /* Select the mode of the Generic Index (sparx5 s0 W2) */
	VCAP_KF_HOST_MATCH,                      /* The action from the SMAC_SIP4 or SMAC_SIP6 lookups. Used for IP source guarding. (lan966x s2 W1) */
	VCAP_KF_IGR_ECID_BASE,                   /* Used by 802.1BR Bridge Port Extention in an E-Tag (sparx5 s0 W12) (sparx5 s0 W12) */
	VCAP_KF_IGR_ECID_EXT,                    /* Used by 802.1BR Bridge Port Extention in an E-Tag (sparx5 s0 W12) (sparx5 s0 W8) */
	VCAP_KF_IGR_PORT,                        /* Sparx5: Logical ingress port number retrieved from ANA_CL::PORT_ID_CFG.LPORT_NUM or ERLEG, LAN966x: ingress port nunmber (sparx5 s0 W7), (sparx5 es2 W9), (lan966x s1 W3), (lan966x s2 W4), (lan966x es0 W4) */
	VCAP_KF_IGR_PORT_MASK,                   /* Full or short (Sparx5 S2) ingress port mask (sparx5 s0 W65), (sparx5 s2 W32), (sparx5 s2 W65), (lan966x s1 W9), (lan966x s2 W9) */
	VCAP_KF_IGR_PORT_MASK_L3,                /* Selector for the short ingress port mask (sparx5 s2 W1) */
	VCAP_KF_IGR_PORT_MASK_RNG,               /* Selector for the short ingress port mask (sparx5 s2 W4) */
	VCAP_KF_IGR_PORT_MASK_SEL,               /* Selector for the short ingress port mask (sparx5 s0 W2), (sparx5 s2 W2) */
	VCAP_KF_IGR_PORT_SEL,                    /* Selector for IGR_PORT: physical port number or ERLEG (sparx5 es2 W1) */
	VCAP_KF_INNER_DEI,                       /* Inner Tag information (lan966x s1 W1) */
	VCAP_KF_INNER_PCP,                       /* Inner Tag information (lan966x s1 W3) */
	VCAP_KF_INNER_TPID,                      /* Inner Tag information (lan966x s1 W1) */
	VCAP_KF_INNER_VID,                       /* Inner Tag information (lan966x s1 W12) */
	VCAP_KF_IP4,                             /* Set if frame has EtherType = 0x800 and IP version = 4 (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_KF_IP_MC,                           /* Set if frame is IPv4 frame and frame’s destination MAC address is an IPv4 multicast address (0x01005E0 /25). Set if frame is IPv6 frame and frame’s destination MAC address is an IPv6 multicast address (0x3333/16). (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_KF_IP_PAYLOAD_5TUPLE,               /* Payload bytes after IP header (sparx5 s0 W32), (lan966x s1 W32) */
	VCAP_KF_IP_PAYLOAD_S1_IP6,               /* Payload after IPv6 header (lan966x s1 W112) */
	VCAP_KF_IP_SNAP,                         /* Set if frame is IPv4, IPv6, or SNAP frame (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_KF_ISDX,                            /* Classified ISDX (sparx5 s2 W12), (sparx5 es2 W12), (lan966x es0 W8) */
	VCAP_KF_ISDX_GT0,                        /* Set if classified ISDX > 0 (lan966x s2 W1), (lan966x es0 W1) */
	VCAP_KF_L2_BC,                           /* Set if frame’s destination MAC address is the broadcast address (FF-FF-FF-FF-FF-FF). (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1), (lan966x es0 W1) */
	VCAP_KF_L2_DMAC,                         /* (sparx5 s0 W48), (sparx5 s2 W48), (sparx5 es2 W48), (lan966x s1 W48), (lan966x s2 W48) */
	VCAP_KF_L2_FRM_TYPE,                     /* Frame subtype for specific EtherTypes (MRP, DLR) (lan966x s2 W4) */
	VCAP_KF_L2_FWD,                          /* Set if the frame is allowed to be forwarded to front ports (sparx5 s2 W1) */
	VCAP_KF_L2_LLC,                          /* LLC header and data after up to two VLAN tags and the type/length field (lan966x s2 W40) */
	VCAP_KF_L2_MAC,                          /* MAC address (FIRST=1: SMAC, FIRST=0: DMAC) (lan966x s1 W48) */
	VCAP_KF_L2_MC,                           /* Set if frame’s destination MAC address is a multicast address (bit 40 = 1). (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1), (lan966x es0 W1) */
	VCAP_KF_L2_PAYLOAD0,                     /* Payload bytes 0-1 after the frame’s EtherType (lan966x s2 W16) */
	VCAP_KF_L2_PAYLOAD1,                     /* Payload byte 4 after the frame’s EtherType. This is specifically for PTP frames. (lan966x s2 W8) */
	VCAP_KF_L2_PAYLOAD2,                     /* Bits 7, 2, and 1 from payload byte 6 after the frame’s EtherType. This is specifically for PTP frames. (lan966x s2 W3) */
	VCAP_KF_L2_PAYLOAD_ETYPE,                /* Byte 0-7 of L2 payload after Type/Len field and overloading for OAM (sparx5 s2 W64), (sparx5 es2 W64) */
	VCAP_KF_L2_SMAC,                         /* (sparx5 s0 W48), (sparx5 s2 W48), (sparx5 es2 W48), (lan966x s1 W48), (lan966x s2 W48) */
	VCAP_KF_L2_SNAP,                         /* SNAP header after LLC header (AA-AA-03) (lan966x s2 W40) */
	VCAP_KF_L3_DMAC_DIP_MATCH,               /* Match found in DIP security lookup in ANA_L3 (sparx5 s2 W1) */
	VCAP_KF_L3_DSCP,                         /* Frame’s DSCP value (sparx5 s0 W6), (lan966x s1 W6) */
	VCAP_KF_L3_DST,                          /* Set if lookup is done for egress router leg (sparx5 s2 W1) */
	VCAP_KF_L3_FRAGMENT,                     /* Set if IPv4 frame is fragmented (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_KF_L3_FRAGMENT_TYPE,                /* L3 Fragmentation type (none, initial, suspicious, valid follow up) (sparx5 s0 W2), (sparx5 s2 W2), (sparx5 es2 W2) */
	VCAP_KF_L3_FRAG_INVLD_L4_LEN,            /* Set if frame's L4 length is less than ANA_CL:COMMON:CLM_FRAGMENT_CFG.L4_MIN_L EN (sparx5 s0 W1), (sparx5 s2 W1) */
	VCAP_KF_L3_FRAG_OFS_GT0,                 /* Set if IPv4 frame is fragmented and it is not the first fragment (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_KF_L3_IP4_DIP,                      /* (sparx5 s0 W32), (sparx5 s2 W32), (sparx5 es2 W32), (lan966x s1 W32), (lan966x s2 W32) */
	VCAP_KF_L3_IP4_SIP,                      /* (sparx5 s0 W32), (sparx5 s2 W32), (sparx5 es2 W32), (lan966x s1 W32), (lan966x s2 W32) */
	VCAP_KF_L3_IP6_DIP,                      /* Sparx5: Full IPv6 DIP, LAN966x: Either Full IPv6 DIP or a subset depending on frame type (sparx5 s0 W128), (sparx5 s2 W128), (sparx5 es2 W128), (lan966x s1 W64), (lan966x s1 W128), (lan966x s2 W128) */
	VCAP_KF_L3_IP6_DIP_MSB,                  /* MS 16bits of IPv6 DIP (lan966x s1 W16) */
	VCAP_KF_L3_IP6_SIP,                      /* Sparx5: Full IPv6 SIP, LAN966x: Either Full IPv6 SIP or a subset depending on frame type (sparx5 s0 W128), (sparx5 s2 W128), (sparx5 es2 W128), (lan966x s1 W128), (lan966x s1 W64), (lan966x s2 W128) */
	VCAP_KF_L3_IP6_SIP_MSB,                  /* MS 16bits of IPv6 DIP (lan966x s1 W16) */
	VCAP_KF_L3_IP_PROTO,                     /* IP protocol / next header (sparx5 s0 W8), (sparx5 es2 W8), (lan966x s1 W8) */
	VCAP_KF_L3_OPTIONS,                      /* Set if IPv4 frame contains options (IP len > 5) (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_KF_L3_PAYLOAD,                      /* Sparx5: Payload bytes after IP header. IPv4: IPv4 options are not parsed so payload is always taken 20 bytes after the start of the IPv4 header, LAN966x: Bytes 0-6 after IP header (sparx5 s2 W96), (sparx5 es2 W96), (lan966x s2 W56) */
	VCAP_KF_L3_PROTO,                        /* IPv4 frames: IP protocol. IPv6 frames: Next header, same as VCAP_KF_L3_IP_PROTO (sparx5 s2 W8), (lan966x s2 W8) */
	VCAP_KF_L3_RT,                           /* Set if frame has hit a router leg (sparx5 s2 W1), (sparx5 es2 W1) */
	VCAP_KF_L3_SMAC_SIP_MATCH,               /* Match found in SIP security lookup in ANA_L3 (sparx5 s2 W1) */
	VCAP_KF_L3_TOS,                          /* Sparx5: Frame's IPv4/IPv6 DSCP and ECN fields, LAN966x: IP TOS field (sparx5 s2 W8), (sparx5 es2 W8), (lan966x s2 W8) */
	VCAP_KF_L3_TTL_GT0,                      /* Set if IPv4 TTL / IPv6 hop limit is greater than 0 (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_L4_1588_DOM,                     /* PTP over UDP: domainNumber (lan966x s2 W8) */
	VCAP_KF_L4_1588_VER,                     /* PTP over UDP: version (lan966x s2 W4) */
	VCAP_KF_L4_ACK,                          /* Sparx5 and LAN966x: TCP flag ACK, LAN966x only: PTP over UDP: flagField bit 2 (unicastFlag) (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_L4_DPORT,                        /* Sparx5: TCP/UDP destination port. Overloading for IP_7TUPLE: Non-TCP/UDP IP frames: L4_DPORT = L3_IP_PROTO, LAN966x: TCP/UDP destination port (sparx5 s2 W16), (sparx5 es2 W16), (lan966x s2 W16) */
	VCAP_KF_L4_FIN,                          /* TCP flag FIN, LAN966x: TCP flag FIN, and for PTP over UDP: messageType bit 1 (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_L4_PAYLOAD,                      /* Payload bytes after TCP/UDP header Overloading for IP_7TUPLE: Non TCP/UDP frames: Payload bytes 0–7 after IP header. IPv4 options are not parsed so payload is always taken 20 bytes after the start of the IPv4 header for non TCP/UDP IPv4 frames (sparx5 s2 W64), (sparx5 es2 W64) */
	VCAP_KF_L4_PSH,                          /* Sparx5: TCP flag PSH, LAN966x: TCP: TCP flag PSH. PTP over UDP: flagField bit 1 (twoStepFlag) (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_L4_RNG,                          /* Range checker bitmask (one for each range checker). Input into range checkers is taken from classified results (VID, DSCP) and frame (SPORT, DPORT, ETYPE, outer VID, inner VID) (sparx5 s0 W8), (sparx5 s2 W16), (sparx5 es2 W16), (lan966x s1 W8), (lan966x s2 W8) */
	VCAP_KF_L4_RST,                          /* Sparx5: TCP flag RST , LAN966x: TCP: TCP flag RST. PTP over UDP: messageType bit 3 (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_L4_SPORT,                        /* TCP/UDP source port (sparx5 s0 W16), (sparx5 s2 W16), (sparx5 es2 W16), (lan966x s1 W16), (lan966x s2 W16) */
	VCAP_KF_L4_SYN,                          /* Sparx5: TCP flag SYN, LAN966x: TCP: TCP flag SYN. PTP over UDP: messageType bit 2 (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_L4_URG,                          /* Sparx5: TCP flag URG, LAN966x: TCP: TCP flag URG. PTP over UDP: flagField bit 7 (reserved) (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_LOOKUP,                          /* 0: First lookup, 1: Second lookup, 2: Third lookup, Similar to VCAP_KF_FIRST but with extra info (lan966x s1 W2) */
	VCAP_KF_MIRROR_PROBE,                    /* Identifies frame copies generated as a result of mirroring (sparx5 es2 W2) */
	VCAP_KF_OAM_CCM_CNTS_EQ0,                /* Dual-ended loss measurement counters in CCM frames are all zero (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_OAM_DETECTED,                    /* This is missing in the datasheet, but present in the OAM keyset in XML (lan966x s2 W1) */
	VCAP_KF_OAM_FLAGS,                       /* Frame’s OAM flags (lan966x s2 W8) */
	VCAP_KF_OAM_IS_Y1731,                    /* Set if frame’s EtherType = 0x8902(lan966x s2 W1) */
	VCAP_KF_OAM_MEL_FLAGS,                   /* Encoding of MD level/MEG level (MEL) (lan966x s2 W7) */
	VCAP_KF_OAM_MEPID,                       /* CCM frame’s OAM MEP ID (lan966x s2 W16) */
	VCAP_KF_OAM_OPCODE,                      /* Frame’s OAM opcode (lan966x s2 W8) */
	VCAP_KF_OAM_VER,                         /* Frame’s OAM version (lan966x s2 W5) */
	VCAP_KF_OAM_Y1731,                       /* Set if frame’s EtherType = 0x8902, so similar to VCAP_KF_OAM_IS_Y1731 (sparx5 s2 W1), (sparx5 es2 W1) */
	VCAP_KF_PAG,                             /* Classified Policy Association Group: chains rules from IS1/CLM to IS2 (sparx5 s2 W8), (lan966x s2 W8) */
	VCAP_KF_PCP,                             /* Sparx5: Classified PCP, LAN966x: Frame’s outer PCP if frame is tagged, otherwise port default (sparx5 s2 W3), (sparx5 es2 W3), (lan966x s1 W3), (lan966x s2 W3), (lan966x es0 W3) */
	VCAP_KF_PCP0,                            /* First PCP in multiple tags in CLM in the "TUPLE" keysets (sparx5 s0 W3) */
	VCAP_KF_PCP1,                            /* Second -"- (sparx5 s0 W3) */
	VCAP_KF_PCP2,                            /* Third -"- (sparx5 s0 W3) */
	VCAP_KF_PDU_TYPE,                        /* PDU type value (none, OAM CCM, MRP, DLR, RTE, IPv4, IPv6, OAM non-CCM) (lan966x es0 W4) */
	VCAP_KF_PROT_ACTIVE,                     /* Protection is active: this appears to be a feature used in ES0 only (sparx5 es2 W1) */
	VCAP_KF_RTP_ID,                          /* Classified RTP_ID (lan966x es0 W10) */
	VCAP_KF_RT_FRMID,                        /* Profinet or OPC-UA FrameId (lan966x s1 W32) */
	VCAP_KF_RT_TYPE,                         /* Encoding of frame's EtherType: 0: Other, 1: Profinet, 2: OPC-UA, 3: Custom (ANA::RT_CUSTOM) (lan966x s1 W2) */
	VCAP_KF_RT_VLAN_IDX,                     /* Real-time VLAN index from ANA::RT_VLAN_PCP (lan966x s1 W3) */
	VCAP_KF_R_TAGGED,                        /* Set if frame contains an RTAG: IEEE 802.1CB (FRER Redundancy tag, Ethertype 0xf1c1) (lan966x s1 W1) */
	VCAP_KF_SEQUENCE_EQ0,                    /* Set if TCP sequence number is 0, LAN966x: Overlayed with PTP over UDP: messageType bit 0 (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_SERVICE_FRM,                     /* Set if classified ISDX > 0, similar to VCAP_KF_ISDX_GT0 (Sparx5)  (sparx5 s2 W1), (sparx5 es2 W1) */
	VCAP_KF_SPORT_EQ_DPORT,                  /* Set if UDP or TCP source port equals UDP or TCP destination port (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_KF_TCP,                             /* Set if frame is IPv4 TCP frame (IP protocol = 6) or IPv6 TCP frames (Next header = 6) (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_KF_TCP_UDP,                         /* Set if frame is IPv4/IPv6 TCP or UDP frame (IP protocol/next header equals 6 or 17) (sparx5 s0 W1), (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1) */
	VCAP_KF_TPID,                            /* TPID for outer tag: 0: Customer TPID 1: Service TPID (88A8 or programmable) (lan966x s1 W1) */
	VCAP_KF_TPID0,                           /* Tag protocol identifier of the frame’s first tag (outer tag): 0: Untagged, 1: 0x8100, 4: 0x88A8, 5: Custom value 1, 6: Custom value 2, 7: Custom value 3 (sparx5 s0 W3) */
	VCAP_KF_TPID1,                           /* Second tag info (as above) (sparx5 s0 W3) */
	VCAP_KF_TPID2,                           /* Third tag info (as above) (sparx5 s0 W3) */
	VCAP_KF_TYPE,                            /* Keyset type id - not exposed to clients (sparx5 s0 W2), (sparx5 s0 W1), (sparx5 s2 W4), (sparx5 s2 W2), (sparx5 es2 W3), (lan966x s1 W1), (lan966x s1 W2), (lan966x s2 W4), (lan966x s2 W2) */
	VCAP_KF_VID,                             /* IS1: Frame’s outer VID if frame is tagged, otherwise port default, IS2: Classified VID which is the result of the VLAN classification in basic classification and IS1, ES0: Classified VID (lan966x s1 W12), (lan966x s2 W12), (lan966x es0 W12) */
	VCAP_KF_VID0,                            /* VID from first tag (sparx5 s0 W12) */
	VCAP_KF_VID1,                            /* VID from second tag (sparx5 s0 W12) */
	VCAP_KF_VID2,                            /* VID from third tag (sparx5 s0 W12) */
	VCAP_KF_VLAN_DBL_TAGGED,                 /* Set if frame has two or more Q-tags. Independent of port VLAN awareness (lan966x s1 W1) */
	VCAP_KF_VLAN_TAGGED,                     /* Sparx5: Set if frame was received with a VLAN tag, LAN966x: Set if frame has one or more Q-tags. Independent of port VLAN awareness (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_KF_VLAN_TAGS,                       /* Number of VLAN tags in frame: 0: Untagged, 1: Single tagged, 3: Double tagged, 7: Triple tagged (sparx5 s0 W3) */
	VCAP_KF_XVID,                            /* Classified VID. Can also be Egress VID (various settings), Extention bit in IFH (but to what use?) (sparx5 s2 W13), (sparx5 es2 W13) */
};

/* Actionset names with origin information */
enum vcap_actionfield_set {
	VCAP_AFS_NO_VALUE,                        /* initial value */
	VCAP_AFS_BASE_TYPE,                       /* (sparx5 s2 X3), (sparx5 es2 X3), (lan966x s2 X2) */
	VCAP_AFS_CLASSIFICATION,                  /* (sparx5 s0 X2) */
	VCAP_AFS_CLASS_REDUCED,                   /* (sparx5 s0 X1) */
	VCAP_AFS_FULL,                            /* (sparx5 s0 X3) */
	VCAP_AFS_MLBS,                            /* (sparx5 s0 X2) */
	VCAP_AFS_MLBS_REDUCED,                    /* (sparx5 s0 X1) */
	VCAP_AFS_S1,                              /* (lan966x s1 X1) */
	VCAP_AFS_S1_RT,                           /* (lan966x s1 X1) */
	VCAP_AFS_SMAC_SIP,                        /* (lan966x s2 X1) */
	VCAP_AFS_VID,                             /* (lan966x es0 X1) */
};

/* Actionfield names with origin information */
enum vcap_action_field {
	VCAP_AF_ACL_ID,                          /* (lan966x s2 W6) */
	VCAP_AF_ACL_MAC,                         /* (sparx5 s2 W48) */
	VCAP_AF_ACL_RT_MODE,                     /* (sparx5 s2 W4) */
	VCAP_AF_ANA2_TSN_DIS,                    /* (lan966x s1 W1) */
	VCAP_AF_CNT_ID,                          /* (sparx5 s2 W12), (sparx5 es2 W11) */
	VCAP_AF_COPY_PORT_NUM,                   /* (sparx5 es2 W7) */
	VCAP_AF_COPY_QUEUE_NUM,                  /* (sparx5 es2 W16) */
	VCAP_AF_COSID_ENA,                       /* (sparx5 s0 W1) */
	VCAP_AF_COSID_VAL,                       /* (sparx5 s0 W3) */
	VCAP_AF_CPU_COPY,                        /* (sparx5 es2 W1) */
	VCAP_AF_CPU_COPY_ENA,                    /* (sparx5 s2 W1), (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_AF_CPU_DIS,                         /* (sparx5 s2 W1) */
	VCAP_AF_CPU_ENA,                         /* (sparx5 s0 W1) */
	VCAP_AF_CPU_Q,                           /* (sparx5 s0 W3) */
	VCAP_AF_CPU_QUEUE_NUM,                   /* (sparx5 es2 W3) */
	VCAP_AF_CPU_QU_NUM,                      /* (sparx5 s2 W3), (lan966x s1 W3), (lan966x s2 W3) */
	VCAP_AF_CT_SEL,                          /* (lan966x s1 W2) */
	VCAP_AF_CUSTOM_ACE_ENA,                  /* (sparx5 s0 W5) */
	VCAP_AF_CUSTOM_ACE_OFFSET,               /* (sparx5 s0 W2) */
	VCAP_AF_CUSTOM_ACE_TYPE_ENA,             /* (lan966x s1 W4) */
	VCAP_AF_DEI_A_VAL,                       /* (lan966x es0 W1) */
	VCAP_AF_DEI_B_VAL,                       /* (lan966x es0 W1) */
	VCAP_AF_DEI_ENA,                         /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_DEI_VAL,                         /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_DLB_OFFSET,                      /* (sparx5 s2 W3) */
	VCAP_AF_DLR_SEL,                         /* (lan966x s1 W2) */
	VCAP_AF_DMAC_OFFSET_ENA,                 /* (sparx5 s2 W1) */
	VCAP_AF_DP_ENA,                          /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_DP_VAL,                          /* (sparx5 s0 W2), (lan966x s1 W1) */
	VCAP_AF_DSCP_ENA,                        /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_DSCP_VAL,                        /* (sparx5 s0 W6), (lan966x s1 W6) */
	VCAP_AF_EGR_ACL_ENA,                     /* (sparx5 s2 W1) */
	VCAP_AF_ES2_REW_CMD,                     /* (sparx5 es2 W3) */
	VCAP_AF_ESDX,                            /* (lan966x es0 W8) */
	VCAP_AF_FID_SEL,                         /* (lan966x s1 W2) */
	VCAP_AF_FID_VAL,                         /* (lan966x s1 W13) */
	VCAP_AF_FWD_DIS,                         /* (sparx5 s0 W1) */
	VCAP_AF_FWD_ENA,                         /* (lan966x s1 W1) */
	VCAP_AF_FWD_KILL_ENA,                    /* (lan966x s2 W1) */
	VCAP_AF_FWD_MASK,                        /* (lan966x s1 W8) */
	VCAP_AF_FWD_MODE,                        /* (sparx5 es2 W2) */
	VCAP_AF_FWD_SEL,                         /* (lan966x s1 W2) */
	VCAP_AF_FWD_TYPE,                        /* (sparx5 s0 W3) */
	VCAP_AF_GVID_ADD_REPLACE_SEL,            /* (sparx5 s0 W3) */
	VCAP_AF_HIT_ME_ONCE,                     /* (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s2 W1) */
	VCAP_AF_HOST_MATCH,                      /* (lan966x s2 W1) */
	VCAP_AF_IGNORE_PIPELINE_CTRL,            /* (sparx5 s2 W1), (sparx5 es2 W1) */
	VCAP_AF_IGR_ACL_ENA,                     /* (sparx5 s2 W1) */
	VCAP_AF_INJ_MASQ_ENA,                    /* (sparx5 s0 W1) */
	VCAP_AF_INJ_MASQ_LPORT,                  /* (sparx5 s0 W7) */
	VCAP_AF_INJ_MASQ_PORT,                   /* (sparx5 s0 W7) */
	VCAP_AF_INTR_ENA,                        /* (sparx5 s2 W1), (sparx5 es2 W1) */
	VCAP_AF_ISDX_ADD_REPLACE_SEL,            /* (sparx5 s0 W1) */
	VCAP_AF_ISDX_ADD_VAL,                    /* (lan966x s1 W8) */
	VCAP_AF_ISDX_ENA,                        /* (lan966x s2 W1) */
	VCAP_AF_ISDX_REPLACE_ENA,                /* (lan966x s1 W1) */
	VCAP_AF_ISDX_VAL,                        /* (sparx5 s0 W12), (lan966x s1 W8) */
	VCAP_AF_IS_INNER_ACL,                    /* (sparx5 s2 W1) */
	VCAP_AF_L3_MAC_UPDATE_DIS,               /* (sparx5 s0 W1) */
	VCAP_AF_LLCT_ENA,                        /* (lan966x s1 W1) */
	VCAP_AF_LLCT_PORT,                       /* (lan966x s1 W3) */
	VCAP_AF_LOG_MSG_INTERVAL,                /* (sparx5 s2 W4) */
	VCAP_AF_LPM_AFFIX_ENA,                   /* (sparx5 s0 W1) */
	VCAP_AF_LPM_AFFIX_VAL,                   /* (sparx5 s0 W10) */
	VCAP_AF_LPORT_ENA,                       /* (sparx5 s0 W1) */
	VCAP_AF_LRN_DIS,                         /* (sparx5 s2 W1), (lan966x s2 W1) */
	VCAP_AF_MAP_IDX,                         /* (sparx5 s0 W9) */
	VCAP_AF_MAP_KEY,                         /* (sparx5 s0 W3) */
	VCAP_AF_MAP_LOOKUP_SEL,                  /* (sparx5 s0 W2) */
	VCAP_AF_MASK_MODE,                       /* (sparx5 s0 W3), (sparx5 s2 W3), (lan966x s2 W2) */
	VCAP_AF_MATCH_ID,                        /* (sparx5 s0 W16), (sparx5 s2 W16) */
	VCAP_AF_MATCH_ID_MASK,                   /* (sparx5 s0 W16), (sparx5 s2 W16) */
	VCAP_AF_MIP_SEL,                         /* (sparx5 s0 W2) */
	VCAP_AF_MIRROR_ENA,                      /* (lan966x s2 W1) */
	VCAP_AF_MIRROR_PROBE,                    /* (sparx5 s2 W2) */
	VCAP_AF_MIRROR_PROBE_ID,                 /* (sparx5 es2 W2) */
	VCAP_AF_MPLS_IP_CTRL_ENA,                /* (sparx5 s0 W1) */
	VCAP_AF_MPLS_MEP_ENA,                    /* (sparx5 s0 W1) */
	VCAP_AF_MPLS_MIP_ENA,                    /* (sparx5 s0 W1) */
	VCAP_AF_MPLS_OAM_FLAVOR,                 /* (sparx5 s0 W1) */
	VCAP_AF_MPLS_OAM_TYPE,                   /* (sparx5 s0 W3) */
	VCAP_AF_MRP_SEL,                         /* (lan966x s1 W2) */
	VCAP_AF_NUM_VLD_LABELS,                  /* (sparx5 s0 W2) */
	VCAP_AF_NXT_IDX,                         /* (sparx5 s0 W12) */
	VCAP_AF_NXT_IDX_CTRL,                    /* (sparx5 s0 W3) */
	VCAP_AF_NXT_KEY_TYPE,                    /* (sparx5 s0 W5) */
	VCAP_AF_NXT_NORMALIZE,                   /* (sparx5 s0 W1) */
	VCAP_AF_NXT_NORM_W16_OFFSET,             /* (sparx5 s0 W5) */
	VCAP_AF_NXT_NORM_W32_OFFSET,             /* (sparx5 s0 W2) */
	VCAP_AF_NXT_OFFSET_FROM_TYPE,            /* (sparx5 s0 W2) */
	VCAP_AF_NXT_TYPE_AFTER_OFFSET,           /* (sparx5 s0 W2) */
	VCAP_AF_OAM_IP_BFD_ENA,                  /* (sparx5 s0 W1) */
	VCAP_AF_OAM_SEL,                         /* (lan966x s1 W3) */
	VCAP_AF_OAM_TWAMP_ENA,                   /* (sparx5 s0 W1) */
	VCAP_AF_OAM_Y1731_SEL,                   /* (sparx5 s0 W3) */
	VCAP_AF_OWN_MAC,                         /* (lan966x s1 W1) */
	VCAP_AF_PAG_OVERRIDE_MASK,               /* (sparx5 s0 W8), (lan966x s1 W8) */
	VCAP_AF_PAG_VAL,                         /* (sparx5 s0 W8), (lan966x s1 W8) */
	VCAP_AF_PCP_A_VAL,                       /* (lan966x es0 W3) */
	VCAP_AF_PCP_B_VAL,                       /* (lan966x es0 W3) */
	VCAP_AF_PCP_ENA,                         /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_PCP_VAL,                         /* (sparx5 s0 W3), (lan966x s1 W3) */
	VCAP_AF_PIPELINE_ACT_SEL,                /* (sparx5 s0 W1) */
	VCAP_AF_PIPELINE_FORCE_ENA,              /* (sparx5 s0 W2), (sparx5 s2 W1) */
	VCAP_AF_PIPELINE_PT,                     /* (sparx5 s0 W5), (sparx5 s2 W5) */
	VCAP_AF_PIPELINE_PT_REDUCED,             /* (sparx5 s0 W3) */
	VCAP_AF_PN_STAT_OFS,                     /* (lan966x s1 W6) */
	VCAP_AF_POLICE_ENA,                      /* (sparx5 s2 W1), (sparx5 es2 W1), (lan966x s1 W1), (lan966x s2 W1) */
	VCAP_AF_POLICE_IDX,                      /* (sparx5 s2 W6), (sparx5 es2 W6), (lan966x s1 W9), (lan966x s2 W9) */
	VCAP_AF_POLICE_REMARK,                   /* (sparx5 es2 W1) */
	VCAP_AF_POLICE_VCAP_ONLY,                /* (lan966x s2 W1) */
	VCAP_AF_PORT_MASK,                       /* (sparx5 s0 W65), (sparx5 s2 W68), (lan966x s2 W8) */
	VCAP_AF_PTP_MASTER_SEL,                  /* (sparx5 s2 W2) */
	VCAP_AF_PUSH_INNER_TAG,                  /* (lan966x es0 W1) */
	VCAP_AF_PUSH_OUTER_TAG,                  /* (lan966x es0 W2) */
	VCAP_AF_QOS_ENA,                         /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_QOS_VAL,                         /* (sparx5 s0 W3), (lan966x s1 W3) */
	VCAP_AF_REW_CMD,                         /* (sparx5 s2 W11) */
	VCAP_AF_REW_OP,                          /* (lan966x s2 W16) */
	VCAP_AF_RLEG_DMAC_CHK_DIS,               /* (sparx5 s0 W1) */
	VCAP_AF_RLEG_STAT_IDX,                   /* (sparx5 s2 W3) */
	VCAP_AF_RSDX_ENA,                        /* (sparx5 s2 W1) */
	VCAP_AF_RSDX_VAL,                        /* (sparx5 s2 W12) */
	VCAP_AF_RSVD_LBL_VAL,                    /* (sparx5 s0 W4) */
	VCAP_AF_RTE_INB_UPD,                     /* (lan966x s1 W1) */
	VCAP_AF_RTP_ENA,                         /* (lan966x s1 W1) */
	VCAP_AF_RTP_ID,                          /* (lan966x s1 W10) */
	VCAP_AF_RTP_SUBID,                       /* (lan966x s1 W1) */
	VCAP_AF_RT_DIS,                          /* (sparx5 s2 W1) */
	VCAP_AF_RT_SEL,                          /* (sparx5 s0 W2) */
	VCAP_AF_S2_KEY_SEL_ENA,                  /* (sparx5 s0 W1) */
	VCAP_AF_S2_KEY_SEL_IDX,                  /* (sparx5 s0 W6) */
	VCAP_AF_SAM_SEQ_ENA,                     /* (sparx5 s2 W1) */
	VCAP_AF_SFID_ENA,                        /* (lan966x s1 W1) */
	VCAP_AF_SFID_VAL,                        /* (lan966x s1 W8) */
	VCAP_AF_SGID_ENA,                        /* (lan966x s1 W1) */
	VCAP_AF_SGID_VAL,                        /* (lan966x s1 W8) */
	VCAP_AF_SIP_IDX,                         /* (sparx5 s2 W5) */
	VCAP_AF_SRC_FILTER_ENA,                  /* (lan966x s1 W1) */
	VCAP_AF_SWAP_MAC_ENA,                    /* (sparx5 s2 W1) */
	VCAP_AF_TAG_A_DEI_SEL,                   /* (lan966x es0 W2) */
	VCAP_AF_TAG_A_PCP_SEL,                   /* (lan966x es0 W2) */
	VCAP_AF_TAG_A_TPID_SEL,                  /* (lan966x es0 W2) */
	VCAP_AF_TAG_A_VID_SEL,                   /* (lan966x es0 W1) */
	VCAP_AF_TAG_B_DEI_SEL,                   /* (lan966x es0 W2) */
	VCAP_AF_TAG_B_PCP_SEL,                   /* (lan966x es0 W2) */
	VCAP_AF_TAG_B_TPID_SEL,                  /* (lan966x es0 W2) */
	VCAP_AF_TAG_B_VID_SEL,                   /* (lan966x es0 W1) */
	VCAP_AF_TCP_UDP_DPORT,                   /* (sparx5 s2 W16) */
	VCAP_AF_TCP_UDP_ENA,                     /* (sparx5 s2 W1) */
	VCAP_AF_TCP_UDP_SPORT,                   /* (sparx5 s2 W16) */
	VCAP_AF_TC_ENA,                          /* (sparx5 s0 W1) */
	VCAP_AF_TC_LABEL,                        /* (sparx5 s0 W3) */
	VCAP_AF_TPID_SEL,                        /* (sparx5 s0 W2) */
	VCAP_AF_TTL_DECR_DIS,                    /* (sparx5 s0 W1) */
	VCAP_AF_TTL_ENA,                         /* (sparx5 s0 W1) */
	VCAP_AF_TTL_LABEL,                       /* (sparx5 s0 W3) */
	VCAP_AF_TTL_UPDATE_ENA,                  /* (sparx5 s2 W1) */
	VCAP_AF_TYPE,                            /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_VID_ADD_VAL,                     /* (lan966x s1 W12) */
	VCAP_AF_VID_A_VAL,                       /* (lan966x es0 W12) */
	VCAP_AF_VID_B_VAL,                       /* (lan966x es0 W12) */
	VCAP_AF_VID_REPLACE_ENA,                 /* (lan966x s1 W1) */
	VCAP_AF_VID_VAL,                         /* (sparx5 s0 W13) */
	VCAP_AF_VLAN_POP_CNT,                    /* (sparx5 s0 W2), (lan966x s1 W2) */
	VCAP_AF_VLAN_POP_CNT_ENA,                /* (sparx5 s0 W1), (lan966x s1 W1) */
	VCAP_AF_VLAN_PUSH_CNT,                   /* (sparx5 s0 W2) */
	VCAP_AF_VLAN_PUSH_CNT_ENA,               /* (sparx5 s0 W1) */
	VCAP_AF_VLAN_WAS_TAGGED,                 /* (sparx5 s0 W2) */
	VCAP_AF_XVID_ADD_REPLACE_SEL,            /* (sparx5 s0 W3) */
};

#endif /* __VCAP_AG_API__ */
