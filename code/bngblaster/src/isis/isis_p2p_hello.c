/*
 * BNG Blaster (BBL) - IS-IS P2P Hello
 *
 * Christian Giese, February 2022
 *
 * Copyright (C) 2020-2022, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "isis.h"

void
isis_hello_timeout (timer_s *timer) {
    bbl_interface_s *interface = timer->data;
    interface->send_requests |= BBL_IF_SEND_ISIS_P2P_HELLO;
}

/**
 * isis_p2p_hello_encode
 *
 * @param interface send interface
 * @param buf send buffer
 * @param len send buffer length
 * @param eth send ethernet parent structure
 * @return PROTOCOL_SUCCESS on success
 */
protocol_error_t
isis_p2p_hello_encode(bbl_interface_s *interface, 
                      uint8_t *buf, uint16_t *len, 
                      bbl_ethernet_header_t *eth) {

    protocol_error_t result;
    isis_pdu_t pdu = {0};
    bbl_isis_t isis = {0};

    isis_adjacency_p2p_t *adjacency = interface->isis_adjacency_p2p;
    isis_instance_t      *instance  = adjacency->instance;
    isis_config_t        *config    = instance->config;

    isis_auth_type auth = ISIS_AUTH_NONE;
    char *key = NULL;

    if(adjacency->timeout > 3 && adjacency->state != ISIS_P2P_ADJACENCY_STATE_DOWN) {
        LOG(ISIS, "ISIS P2P-Hello timeout on interface %s\n",
            interface->name);

        adjacency->state = ISIS_P2P_ADJACENCY_STATE_DOWN;

        for(int i=0; i<ISIS_LEVELS; i++) {
            if(interface->isis_adjacency[i]) {
                isis_adjacency_down(interface->isis_adjacency[i]);
                isis_lsp_self_update(adjacency->instance, i+1);
            }
        }
    }

    /* Start next timer ... */
    timer_add(&interface->ctx->timer_root, &interface->timer_isis_hello, 
              "ISIS hello", config->hello_interval, 0, interface, 
              &isis_hello_timeout);

    if(config->level1_auth && config->level1_key) {
        auth = config->level1_auth;
        key = config->level1_key;
    } else if(config->level2_auth && config->level2_key) {
        auth = config->level2_auth;
        key = config->level2_key;
    }

    /* Build PDU */
    isis_pdu_init(&pdu, ISIS_PDU_P2P_HELLO);
    /* PDU header */
    isis_pdu_add_u8(&pdu, adjacency->level);
    isis_pdu_add_bytes(&pdu, config->system_id, ISIS_SYSTEM_ID_LEN);
    isis_pdu_add_u16(&pdu, config->holding_time);
    isis_pdu_add_u16(&pdu, 0);
    isis_pdu_add_u8(&pdu, 0x1);
    /* TLV section */
    isis_pdu_add_tlv_auth(&pdu, auth, key);
    isis_pdu_add_tlv_area(&pdu, config->area, config->area_count);
    isis_pdu_add_tlv_protocols(&pdu, config->protocol_ipv4, config->protocol_ipv6);
    isis_pdu_add_tlv_ipv4_int_address(&pdu, interface->ip.address);
    isis_pdu_add_tlv_ipv6_int_address(&pdu, &interface->ip6_ll);
    isis_pdu_add_tlv_p2p_adjacency_state(&pdu, adjacency->state);
    if(config->hello_padding) {
        isis_pdu_padding(&pdu);
    }
    isis_pdu_update_len(&pdu);
    isis_pdu_update_auth(&pdu, key);
    /* Build packet ... */
    eth->type = ISIS_PROTOCOL_IDENTIFIER;
    eth->next = &isis;
    eth->dst = g_isis_mac_p2p_hello;
    isis.type = pdu.pdu_type;
    isis.pdu = pdu.pdu;
    isis.pdu_len = pdu.pdu_len;
    result = encode_ethernet(buf, len, eth);
    if(result == PROTOCOL_SUCCESS) {
        LOG(DEBUG, "ISIS TX %s on interface %s\n",
            isis_pdu_type_string(isis.type), interface->name);
        adjacency->stats.hello_tx++;
        adjacency->timeout++;
    }
    return result;
}

/**
 * isis_p2p_hello_handler_rx
 *
 * @param interface receive interface
 * @param pdu received ISIS PDU
 */
void
isis_p2p_hello_handler_rx(bbl_interface_s *interface, isis_pdu_t *pdu) {

    isis_adjacency_p2p_t *adjacency = interface->isis_adjacency_p2p;
    isis_instance_t      *instance  = NULL;
    isis_config_t        *config    = NULL;

    isis_peer_t *peer;
    isis_tlv_t *tlv;

    uint8_t *state = NULL;
    uint8_t new_state = ISIS_P2P_ADJACENCY_STATE_UP;

    isis_auth_type auth = ISIS_AUTH_NONE;
    char *key = NULL;

    if(!adjacency) {
        return;
    }
    instance = adjacency->instance;
    config = instance->config;

    adjacency->stats.hello_rx++;

    if(config->level1_auth && config->level1_key) {
        auth = config->level1_auth;
        key = config->level1_key;
    } else if(config->level2_auth && config->level2_key) {
        auth = config->level2_auth;
        key = config->level2_key;
    }

    if(!isis_pdu_validate_auth(pdu, auth, key)) {
        LOG(ISIS, "ISIS RX P2P-Hello authentication failed on interface %s\n",
            adjacency->interface->name);
    }

    adjacency->timeout = 0;
    
    peer = adjacency->peer;
    peer->level = *PDU_OFFSET(pdu, ISIS_OFFSET_P2P_HELLO_LEVEL) & 0x03;
    memcpy(peer->system_id, PDU_OFFSET(pdu, ISIS_OFFSET_P2P_HELLO_SYSTEM_ID), ISIS_SYSTEM_ID_LEN);
    
    tlv = isis_pdu_first_tlv(pdu);
    while(tlv) {
        switch (tlv->type) {
            case ISIS_TLV_P2P_ADJACENCY_STATE:
                state = tlv->value;
                break;
            default:
                break;
        }
        tlv = isis_pdu_next_tlv(pdu);
    }

    if(state) {
        switch (*state) {
            case ISIS_P2P_ADJACENCY_STATE_UP:
                new_state = ISIS_P2P_ADJACENCY_STATE_UP;
                break;
            case ISIS_P2P_ADJACENCY_STATE_INIT:
                new_state = ISIS_P2P_ADJACENCY_STATE_UP;
                break;
            case ISIS_P2P_ADJACENCY_STATE_DOWN:
                new_state = ISIS_P2P_ADJACENCY_STATE_INIT;
                break;
            default:
                break;
        }
    }

    if(adjacency->state != new_state && new_state == ISIS_P2P_ADJACENCY_STATE_UP) {
        for(int i=0; i<ISIS_LEVELS; i++) {
            if(interface->isis_adjacency[i]) {
                isis_adjacency_up(interface->isis_adjacency[i]);
                isis_lsp_self_update(adjacency->instance, i+1);
            }
        }
    }
    adjacency->state = new_state;
}