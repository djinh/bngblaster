/*
 * BNG Blaster (BBL) - OSPF Definitions
 *
 * Christian Giese, May 2023
 *
 * Copyright (C) 2020-2023, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __BBL_OSPF_DEF_H__
#define __BBL_OSPF_DEF_H__

/* DEFINITIONS ... */

#define OSPF_DEFAULT_HELLO_INTERVAL     10
#define OSPF_DEFAULT_DEAD_INTERVAL      40
#define OSPF_DEFAULT_ROUTER_PRIORITY    64
#define OSPF_DEFAULT_METRIC             10

#define OSPF_LSA_TYPES                  12

#define OSPF_VERSION_2                  2
#define OSPF_VERSION_3                  3

#define OSPF_PDU_LEN_MIN                16
#define OSPF_PDU_LEN_MAX                UINT16_MAX

#define OSPF_DEFAULT_TEARDOWN_TIME      5

#define OSPF_LSA_GC_INTERVAL            30

#define OSPF_OFFSET_VERSION             0
#define OSPF_OFFSET_TYPE                1
#define OSPF_OFFSET_PACKET_LEN          2
#define OSPF_OFFSET_ROUTER_ID           4
#define OSPF_OFFSET_AREA_ID             8
#define OSPF_OFFSET_CHECKSUM            12

#define OSPFV2_OFFSET_AUTH_TYPE         14
#define OSPFV2_OFFSET_AUTH_DATA         16
#define OSPFV2_OFFSET_PACKET            24

#define OSPFV2_AUTH_DATA_LEN            8

#define OSPFV3_OFFSET_INSTANCE_ID       14
#define OSPFV3_OFFSET_PACKET            16

typedef struct ospf_config_ ospf_config_s;
typedef struct ospf_instance_ ospf_instance_s;
typedef struct ospf_interface_ ospf_interface_s;
typedef struct ospf_neighbor_ ospf_neighbor_s;

/* ENUMS ... */

typedef enum ospf_interface_type_ {
    OSPF_INTERFACE_P2P          = 0,
    OSPF_INTERFACE_BROADCAST    = 1,
    OSPF_INTERFACE_VIRTUAL      = 2,
    OSPF_INTERFACE_NBMA         = 3,
    OSPF_INTERFACE_P2M          = 4
} ospf_interface_type;    

typedef enum ospf_interface_state_ {
    OSPF_IFSTATE_DOWN       = 0,
    OSPF_IFSTATE_LOOPBACK   = 1,
    OSPF_IFSTATE_WAITING    = 2,
    OSPF_IFSTATE_P2P        = 3,
    OSPF_IFSTATE_DR_OTHER   = 4,
    OSPF_IFSTATE_BACKUP     = 5,
    OSPF_IFSTATE_DR         = 6
} ospf_interface_state;

typedef enum ospf_neighbor_state_ {
    OSPF_NBSTATE_DOWN       = 0,
    OSPF_NBSTATE_ATTEMPT    = 1,
    OSPF_NBSTATE_INIT       = 2,
    OSPF_NBSTATE_2WAY       = 3,
    OSPF_NBSTATE_EXSTART    = 4,
    OSPF_NBSTATE_EXCHANGE   = 5,
    OSPF_NBSTATE_LOADING    = 6,
    OSPF_NBSTATE_FULL       = 7
} ospf_neighbor_state;

typedef enum ospf_adjacency_state_ {
    OSPF_ADJACENCY_STATE_DOWN   = 0,
    OSPF_ADJACENCY_STATE_UP     = 1
} ospf_adjacency_state;    

typedef enum ospf_p2p_adjacency_state_ {
    OSPF_P2P_ADJACENCY_STATE_UP     = 0,
    OSPF_P2P_ADJACENCY_STATE_INIT   = 1,
    OSPF_P2P_ADJACENCY_STATE_DOWN   = 2
} ospf_p2p_adjacency_state;

typedef enum ospf_auth_type_ {
    OSPF_AUTH_NONE              = 0,
    OSPF_AUTH_CLEARTEXT         = 1,
    OSPF_AUTH_MD5               = 2
} __attribute__ ((__packed__)) ospf_auth_type;

typedef enum ospf_lsp_source_ {
    OSPF_SOURCE_SELF,       /* Self originated LSA */
    OSPF_SOURCE_ADJACENCY,  /* LSA learned from neighbors */
    OSPF_SOURCE_EXTERNAL    /* LSA injected externally (e.g. MRT file, ...) */
} ospf_lsp_source;

typedef enum ospf_pdu_type_ {
    OSPF_PDU_HELLO      = 1,
    OSPF_PDU_DB_DESC    = 2,
    OSPF_PDU_LS_REQUEST = 3,
    OSPF_PDU_LS_UPDATE  = 4,
    OSPF_PDU_LS_ACK     = 5,
} ospf_pdu_type;

typedef enum ospf_lsa_type_ {
    OSPF_LSA_TYPE_1     = 1,
    OSPF_LSA_TYPE_2     = 2,
    OSPF_LSA_TYPE_3     = 3,
    OSPF_LSA_TYPE_4     = 4,
    OSPF_LSA_TYPE_5     = 5,
    OSPF_LSA_TYPE_6     = 6,
    OSPF_LSA_TYPE_7     = 7,
    OSPF_LSA_TYPE_8     = 8,
    OSPF_LSA_TYPE_9     = 9,
    OSPF_LSA_TYPE_10    = 10,
    OSPF_LSA_TYPE_11    = 11,
    OSPF_LSA_TYPE_MAX,
} ospf_lsa_type;

typedef enum ospf_lsa_scope_ {
    OSPF_LSA_SCOPE_LINK_LOCAL   = 0x0,
    OSPF_LSA_SCOPE_AREA         = 0x2,
    OSPF_LSA_SCOPE_AS           = 0x4
} ospf_lsa_scope;

/* STRUCTURES ... */

/*
 * OSPF PDU context
 */
typedef struct ospf_pdu_ {
    uint8_t  pdu_type;
    uint8_t  pdu_version;

    uint32_t router_id;
    uint32_t area_id;
    uint16_t checksum;

    uint8_t  auth_type;
    uint8_t  auth_data_len;
    uint16_t auth_data_offset;
    uint16_t packet_offset;

    uint16_t cur; /* current position */

    uint8_t *pdu;
    uint16_t pdu_len;
    uint16_t pdu_buf_len;
} ospf_pdu_s;

typedef struct ospf_lsa_entry_ {
    uint16_t  lifetime;
    uint64_t  lsp_id;
    uint32_t  seq;
    uint16_t  checksum;
} __attribute__ ((__packed__)) isis_lsa_entry_s;

typedef struct ospf_external_connection_ {
    const char         *router_id_str;
    ipv4addr_t          router_id;
    uint32_t            metric;
    struct ospf_external_connection_ *next;
} ospf_external_connection_s;

/*
 * OSPF Instance Configuration
 */
typedef struct ospf_config_ {

    uint16_t id; /* OSPF instance identifier */
    uint8_t  version; /* OSPF version (default 2) */

    const char         *area_str;
    ipv4addr_t          area;

    const char         *router_id_str;
    ipv4addr_t          router_id;
    uint8_t             router_priority;

    bool                overload;

    ospf_auth_type      auth_type;
    char               *auth_key;

    uint16_t            hello_interval;
    uint16_t            dead_interval;

    uint16_t            teardown_time;

    const char         *hostname;

    char *external_mrt_file;
    struct ospf_external_connection_ *external_connection;

    /* Pointer to next instance */
    struct ospf_config_ *next; 
} ospf_config_s;

typedef struct ospf_neighbor_ {
    ospf_interface_s *interface;
    ospf_neighbor_s *next;

    uint32_t id;
    uint8_t state;

} ospf_neighbor_s;

typedef struct ospf_interface_ {
    bbl_network_interface_s *interface;
    ospf_instance_s *instance;
    ospf_neighbor_s *neighbors;
    ospf_interface_s *next;
    
    uint8_t version;    /* OSPF version */
    uint8_t type;       /* OSPF inteface type (P2P, broadcast, ...) */

    struct {
        uint32_t hello_rx;
        uint32_t hello_tx;
        uint32_t db_des_rx;
        uint32_t db_des_tx;
        uint32_t ls_req_rx;
        uint32_t ls_req_tx;
        uint32_t ls_upd_rx;
        uint32_t ls_upd_tx;
        uint32_t ls_ack_rx;
        uint32_t ls_ack_tx;
    } stats;

    struct timer_  *timer_hello;

} ospf_interface_s;

typedef struct ospf_instance_ {
    ospf_config_s  *config;
    bool            overload;

    bool            teardown;
    struct timer_  *timer_teardown;
    struct timer_  *timer_lsa_gc;

    struct {
        hb_tree *db;
    } lsdb[OSPF_LSA_TYPES];

    ospf_interface_s *interfaces;

    struct ospf_instance_ *next; /* pointer to next instance */
} ospf_instance_s;

#endif