/*
 * BNG Blaster (BBL) - Streams
 *
 * Christian Giese, March 2021
 *
 * Copyright (C) 2020-2022, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bbl.h"
#include "bbl_session.h"
#include "bbl_stream.h"
#include "bbl_stats.h"

extern volatile bool g_teardown;
extern bool g_init_phase;
extern bool g_traffic;

const char g_multicast_traffic[] = "multicast-traffic";
const char g_session_traffic_ipv4_up[] = "session-traffic-ipv4-up";
const char g_session_traffic_ipv4_down[] = "session-traffic-ipv4-down";
const char g_session_traffic_ipv6_up[] = "session-traffic-ipv6-up";
const char g_session_traffic_ipv6_down[] = "session-traffic-ipv6-down";
const char g_session_traffic_ipv6pd_up[] = "session-traffic-ipv6pd-up";
const char g_session_traffic_ipv6pd_down[] = "session-traffic-ipv6pd-down";

static void
bbl_stream_delay(bbl_stream_s *stream, struct timespec *rx_timestamp, struct timespec *bbl_timestamp)
{
    struct timespec delay;
    uint64_t delay_nsec;
    timespec_sub(&delay, rx_timestamp, bbl_timestamp);
    delay_nsec = delay.tv_sec * 1000000000 + delay.tv_nsec;
    if(delay_nsec > stream->max_delay_ns) {
        stream->max_delay_ns = delay_nsec;
    }
    if(stream->min_delay_ns) {
        if(delay_nsec < stream->min_delay_ns) {
            stream->min_delay_ns = delay_nsec;
        }
    } else {
        stream->min_delay_ns = delay_nsec;
    }
}

static bool
bbl_stream_can_send(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;

    if(g_init_phase) {
        return false;
    }

    if(stream->reset) {
        stream->reset = false;
        stream->flow_seq = 1;
        goto FREE;
    }

    if(stream->config->stream_group_id == 0) {
        /* RAW stream */
        return true;
    }
    if(session && session->session_state == BBL_ESTABLISHED) {
        if(session->access_type == ACCESS_TYPE_PPPOE) {
            if(session->l2tp && session->l2tp_session == NULL) {
                goto FREE;
            }
            switch(stream->config->type) {
                case BBL_SUB_TYPE_IPV4:
                    if(session->ipcp_state == BBL_PPP_OPENED) {
                        return true;
                    }
                    break;
                case BBL_SUB_TYPE_IPV6:
                    if(session->ip6cp_state == BBL_PPP_OPENED &&
                       session->icmpv6_ra_received &&
                       *(uint64_t*)session->ipv6_address) {
                        return true;
                    }
                    break;
                case BBL_SUB_TYPE_IPV6PD:
                    if(session->ip6cp_state == BBL_PPP_OPENED &&
                       session->icmpv6_ra_received &&
                       *(uint64_t*)session->delegated_ipv6_address &&
                       session->dhcpv6_state >= BBL_DHCP_BOUND) {
                        return true;
                    }
                    break;
                default:
                    break;
            }
        } else if (session->access_type == ACCESS_TYPE_IPOE) {
            switch(stream->config->type) {
                case BBL_SUB_TYPE_IPV4:
                    if(session->ip_address) {
                        return true;
                    }
                    break;
                case BBL_SUB_TYPE_IPV6:
                    if(*(uint64_t*)session->ipv6_address &&
                       session->icmpv6_ra_received) {
                        return true;
                    }
                    break;
                case BBL_SUB_TYPE_IPV6PD:
                    if(*(uint64_t*)session->delegated_ipv6_address &&
                       session->icmpv6_ra_received &&
                       session->dhcpv6_state >= BBL_DHCP_BOUND) {
                        return true;
                    }
                    break;
                default:
                    break;
            }
        }
    }
FREE:
    /* Free of packet if not ready to send */
    if(stream->buf) {
        free(stream->buf);
        stream->buf = NULL;
        stream->tx_len = 0;
    }
    stream->send_window_packets = 0;
    return false;
}

static bool
bbl_stream_build_access_pppoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len;

    bbl_ethernet_header_t eth = {0};
    bbl_pppoe_session_t pppoe = {0};
    bbl_ipv4_t ipv4 = {0};
    bbl_ipv6_t ipv6 = {0};
    bbl_udp_t udp = {0};
    bbl_bbl_t bbl = {0};

    /* *
     * The corresponding network interfaces will be selected
     * in the following order:
     * - "network-interface" from stream section
     * - "network-interface" from access interface section
     * - first network interface from network section (default)
     */
    bbl_network_interface_s *network_interface;
    if(config->network_interface) {
        network_interface = bbl_network_interface_get(config->network_interface);
    } else {
        network_interface = session->network_interface;
    }
    if(!network_interface) {
        return false;
    }

    eth.dst = session->server_mac;
    eth.src = session->client_mac;
    eth.qinq = session->access_config->qinq;
    eth.vlan_outer = session->vlan_key.outer_vlan_id;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_inner_priority = config->vlan_priority;
    eth.vlan_three = session->access_third_vlan;
    eth.type = ETH_TYPE_PPPOE_SESSION;
    eth.next = &pppoe;
    pppoe.session_id = session->pppoe_session_id;
    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = BBL_TYPE_UNICAST_SESSION;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_UP;

    switch(stream->config->type) {
        case BBL_SUB_TYPE_IPV4:
            pppoe.protocol = PROTOCOL_IPV4;
            pppoe.next = &ipv4;
            /* Source address */
            if(stream->config->ipv4_access_src_address) {
                ipv4.src = stream->config->ipv4_access_src_address;
            } else {
                ipv4.src = session->ip_address;
            }
            /* Destination address */
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            } else {
                if(stream->config->ipv4_network_address) {
                    ipv4.dst = stream->config->ipv4_network_address;
                } else {
                    ipv4.dst = network_interface->ip.address;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = 64;
            ipv4.tos = config->priority;
            ipv4.protocol = PROTOCOL_IPV4_UDP;
            ipv4.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV4;
            if (config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            pppoe.protocol = PROTOCOL_IPV6;
            pppoe.next = &ipv6;
            /* Source address */
            if(*(uint64_t*)stream->config->ipv6_access_src_address) {
                ipv6.src = stream->config->ipv6_access_src_address;
            } else {
                if(stream->config->type == BBL_SUB_TYPE_IPV6) {
                    ipv6.src = session->ipv6_address;
                } else {
                    ipv6.src = session->delegated_ipv6_address;
                }
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(*(uint64_t*)stream->config->ipv6_network_address) {
                    ipv6.dst = stream->config->ipv6_network_address;
                } else {
                    ipv6.dst = network_interface->ip6.address;
                }
            }
            ipv6.ttl = 64;
            ipv6.tos = config->priority;
            ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            ipv6.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV6;
            if (config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + 64;
    if(buf_len < 256) buf_len = 256;
    stream->buf = malloc(buf_len);
    if(encode_ethernet(stream->buf, &stream->tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->buf);
        stream->buf = NULL;
        stream->tx_len = 0;
        return false;
    }
    return true;
}

static bool
bbl_stream_build_a10nsp_pppoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_a10nsp_session_s *a10nsp_session = session->a10nsp_session;
    bbl_a10nsp_interface_s *a10nsp_interface;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len;

    bbl_ethernet_header_t eth = {0};
    bbl_pppoe_session_t pppoe = {0};
    bbl_ipv4_t ipv4 = {0};
    bbl_ipv6_t ipv6 = {0};
    bbl_udp_t udp = {0};
    bbl_bbl_t bbl = {0};

    a10nsp_interface = bbl_a10nsp_interface_get(config->a10nsp_interface);
    if(!(a10nsp_interface && a10nsp_session)) {
        return false;
    }

    if(stream->direction == BBL_DIRECTION_UP) {
        bbl.direction = BBL_DIRECTION_UP;
        eth.dst = session->server_mac;
        eth.src = session->client_mac;
        eth.qinq = session->access_config->qinq;
        eth.vlan_outer = session->vlan_key.outer_vlan_id;
    } else {
        bbl.direction = BBL_DIRECTION_DOWN;
        eth.dst = session->client_mac;
        eth.src = session->server_mac;
        eth.qinq = a10nsp_interface->qinq;
        eth.vlan_outer = a10nsp_session->s_vlan;
    }
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_three = session->access_third_vlan;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner_priority = config->vlan_priority;
    eth.type = ETH_TYPE_PPPOE_SESSION;
    eth.next = &pppoe;
    pppoe.session_id = session->pppoe_session_id;
    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = BBL_TYPE_UNICAST_SESSION;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    switch(stream->config->type) {
        case BBL_SUB_TYPE_IPV4:
            pppoe.protocol = PROTOCOL_IPV4;
            pppoe.next = &ipv4;
            /* Source address */
            ipv4.src = session->ip_address;
            /* Destination address */
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            } else {
                if(stream->config->ipv4_network_address) {
                    ipv4.dst = stream->config->ipv4_network_address;
                } else {
                    ipv4.dst = A10NSP_IP_LOCAL;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = 64;
            ipv4.tos = config->priority;
            ipv4.protocol = PROTOCOL_IPV4_UDP;
            ipv4.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV4;
            if (config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            pppoe.protocol = PROTOCOL_IPV6;
            pppoe.next = &ipv6;
            /* Source address */
            if(stream->config->type == BBL_SUB_TYPE_IPV6) {
                ipv6.src = session->ipv6_address;
            } else {
                ipv6.src = session->delegated_ipv6_address;
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(*(uint64_t*)stream->config->ipv6_network_address) {
                    ipv6.dst = stream->config->ipv6_network_address;
                } else {
                    ipv6.dst = session->link_local_ipv6_address;
                }
            }
            ipv6.src = session->link_local_ipv6_address;
            ipv6.ttl = 64;
            ipv6.tos = config->priority;
            ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            ipv6.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV6;
            if (config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + 64;
    if(buf_len < 256) buf_len = 256;
    stream->buf = malloc(buf_len);
    if(encode_ethernet(stream->buf, &stream->tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->buf);
        stream->buf = NULL;
        stream->tx_len = 0;
        return false;
    }
    return true;
}

static bool
bbl_stream_build_a10nsp_ipoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_a10nsp_session_s *a10nsp_session = session->a10nsp_session;
    bbl_a10nsp_interface_s *a10nsp_interface;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len;

    bbl_ethernet_header_t eth = {0};
    bbl_ipv4_t ipv4 = {0};
    bbl_ipv6_t ipv6 = {0};
    bbl_udp_t udp = {0};
    bbl_bbl_t bbl = {0};

    a10nsp_interface = bbl_a10nsp_interface_get(config->a10nsp_interface);
    if(!(a10nsp_interface && a10nsp_session)) {
        return false;
    }

    if(stream->direction == BBL_DIRECTION_UP) {
        bbl.direction = BBL_DIRECTION_UP;
        eth.dst = session->server_mac;
        eth.src = session->client_mac;
        eth.qinq = session->access_config->qinq;
        eth.vlan_outer = session->vlan_key.outer_vlan_id;
    } else {
        bbl.direction = BBL_DIRECTION_DOWN;
        eth.dst = session->client_mac;
        eth.src = session->server_mac;
        eth.qinq = a10nsp_interface->qinq;
        eth.vlan_outer = a10nsp_session->s_vlan;
    }
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_three = session->access_third_vlan;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner_priority = config->vlan_priority;

    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = BBL_TYPE_UNICAST_SESSION;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    switch(stream->config->type) {
        case BBL_SUB_TYPE_IPV4:
            eth.type = ETH_TYPE_IPV4;
            eth.next = &ipv4;
            /* Source address */
            ipv4.src = session->ip_address;
            /* Destination address */
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            } else {
                if(stream->config->ipv4_network_address) {
                    ipv4.dst = stream->config->ipv4_network_address;
                } else {
                    ipv4.dst = A10NSP_IP_LOCAL;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = 64;
            ipv4.tos = config->priority;
            ipv4.protocol = PROTOCOL_IPV4_UDP;
            ipv4.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV4;
            if (config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            eth.type = ETH_TYPE_IPV6;
            eth.next = &ipv6;
            /* Source address */
            if(stream->config->type == BBL_SUB_TYPE_IPV6) {
                ipv6.src = session->ipv6_address;
            } else {
                ipv6.src = session->delegated_ipv6_address;
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(*(uint64_t*)stream->config->ipv6_network_address) {
                    ipv6.dst = stream->config->ipv6_network_address;
                } else {
                    ipv6.dst = session->link_local_ipv6_address;
                }
            }
            ipv6.src = session->link_local_ipv6_address;
            ipv6.ttl = 64;
            ipv6.tos = config->priority;
            ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            ipv6.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV6;
            if (config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + 64;
    if(buf_len < 256) buf_len = 256;
    stream->buf = malloc(buf_len);
    if(encode_ethernet(stream->buf, &stream->tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->buf);
        stream->buf = NULL;
        stream->tx_len = 0;
        return false;
    }
    return true;
}

static bool
bbl_stream_build_access_ipoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len;

    bbl_ethernet_header_t eth = {0};
    bbl_ipv4_t ipv4 = {0};
    bbl_ipv6_t ipv6 = {0};
    bbl_udp_t udp = {0};
    bbl_bbl_t bbl = {0};

    /* *
     * The corresponding network interfaces will be selected
     * in the following order:
     * - "network-interface" from stream section
     * - "network-interface" from access interface section
     * - first network interface from network section (default)
     */
    bbl_network_interface_s *network_interface;
    if(config->network_interface) {
        network_interface = bbl_network_interface_get(config->network_interface);
    } else {
        network_interface = session->network_interface;
    }
    if(!network_interface) {
        return false;
    }

    eth.dst = session->server_mac;
    eth.src = session->client_mac;
    eth.qinq = session->access_config->qinq;
    eth.vlan_outer = session->vlan_key.outer_vlan_id;
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_three = session->access_third_vlan;
    eth.vlan_inner_priority = config->vlan_priority;
    eth.vlan_outer_priority = config->vlan_priority;

    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = BBL_TYPE_UNICAST_SESSION;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_UP;

    switch(stream->config->type) {
        case BBL_SUB_TYPE_IPV4:
            eth.type = ETH_TYPE_IPV4;
            eth.next = &ipv4;
            /* Source address */
            if(stream->config->ipv4_access_src_address) {
                ipv4.src = stream->config->ipv4_access_src_address;
            } else {
                ipv4.src = session->ip_address;
            }
            /* Destination address */
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            } else {
                if(stream->config->ipv4_network_address) {
                    ipv4.dst = stream->config->ipv4_network_address;
                } else {
                    ipv4.dst = network_interface->ip.address;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = 64;
            ipv4.tos = config->priority;
            ipv4.protocol = PROTOCOL_IPV4_UDP;
            ipv4.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV4;
            if (config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            eth.type = ETH_TYPE_IPV6;
            eth.next = &ipv6;
            /* Source address */
            if(*(uint64_t*)stream->config->ipv6_access_src_address) {
                ipv6.src = stream->config->ipv6_access_src_address;
            } else {
                if(stream->config->type == BBL_SUB_TYPE_IPV6) {
                    ipv6.src = session->ipv6_address;
                } else {
                    ipv6.src = session->delegated_ipv6_address;
                }
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(*(uint64_t*)stream->config->ipv6_network_address) {
                    ipv6.dst = stream->config->ipv6_network_address;
                } else {
                    ipv6.dst = network_interface->ip6.address;
                }
            }
            ipv6.ttl = 64;
            ipv6.tos = config->priority;
            ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            ipv6.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV6;
            if (config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + 64;
    if(buf_len < 256) buf_len = 256;
    stream->buf = malloc(buf_len);
    if(encode_ethernet(stream->buf, &stream->tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->buf);
        stream->buf = NULL;
        stream->tx_len = 0;
        return false;
    }
    return true;
}

static bool
bbl_stream_build_network_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len;

    bbl_ethernet_header_t eth = {0};
    bbl_mpls_t mpls1 = {0};
    bbl_mpls_t mpls2 = {0};
    bbl_ipv4_t ipv4 = {0};
    bbl_ipv6_t ipv6 = {0};
    bbl_udp_t udp = {0};
    bbl_bbl_t bbl = {0};

    uint8_t mac[ETH_ADDR_LEN] = {0};

    bbl_network_interface_s *network_interface = stream->network_interface;

    eth.dst = network_interface->gateway_mac;
    eth.src = network_interface->mac;
    eth.vlan_outer = network_interface->vlan;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner = 0;

    /* Add MPLS labels */
    if(config->tx_mpls1) {
        eth.mpls = &mpls1;
        mpls1.label = config->tx_mpls1_label;
        mpls1.exp = config->tx_mpls1_exp;
        mpls1.ttl = config->tx_mpls1_ttl;
        if(config->tx_mpls2) {
            mpls1.next = &mpls2;
            mpls2.label = config->tx_mpls2_label;
            mpls2.exp = config->tx_mpls2_exp;
            mpls2.ttl = config->tx_mpls2_ttl;
        }
    }

    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = BBL_TYPE_UNICAST_SESSION;
    if(session) {
        bbl.session_id = session->session_id;
        bbl.ifindex = session->vlan_key.ifindex;
        bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
        bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    }
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_DOWN;
    switch(stream->config->type) {
        case BBL_SUB_TYPE_IPV4:
            eth.type = ETH_TYPE_IPV4;
            eth.next = &ipv4;
            /* Source address */
            if(stream->config->ipv4_network_address) {
                ipv4.src = stream->config->ipv4_network_address;
            } else {
                ipv4.src = network_interface->ip.address;
            }
            /* Destination address */
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
                /* All IPv4 multicast addresses start with 1110 */
                if((ipv4.dst & htobe32(0xf0000000)) == htobe32(0xe0000000)) {
                    /* Generate multicast destination MAC */
                    ipv4_multicast_mac(ipv4.dst, mac);
                    eth.dst = mac;
                    bbl.type = BBL_TYPE_MULTICAST;
                    bbl.mc_source = ipv4.src;
                    bbl.mc_group = ipv4.dst;
                }
            } else {
                if(session) {
                    ipv4.dst = session->ip_address;
                } else {
                    return false;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = 64;
            ipv4.tos = config->priority;
            ipv4.protocol = PROTOCOL_IPV4_UDP;
            ipv4.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV4;
            if (config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            eth.type = ETH_TYPE_IPV6;
            eth.next = &ipv6;
            /* Source address */
            if(*(uint64_t*)stream->config->ipv6_network_address) {
                ipv6.src = stream->config->ipv6_network_address;
            } else {
                ipv6.src = network_interface->ip6.address;
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(session) {
                    if(stream->config->type == BBL_SUB_TYPE_IPV6) {
                        ipv6.dst = session->ipv6_address;
                    } else {
                        ipv6.dst = session->delegated_ipv6_address;
                    }
                } else {
                    return false;
                }
            }
            ipv6.ttl = 64;
            ipv6.tos = config->priority;
            ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            ipv6.next = &udp;
            bbl.sub_type = BBL_SUB_TYPE_IPV6;
            if (config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + 64;
    if(buf_len < 256) buf_len = 256;
    stream->buf = malloc(buf_len);
    if(encode_ethernet(stream->buf, &stream->tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->buf);
        stream->buf = NULL;
        stream->tx_len = 0;
        return false;
    }
    return true;
}

static bool
bbl_stream_build_l2tp_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    bbl_l2tp_session_s *l2tp_session = stream->session->l2tp_session;
    bbl_l2tp_tunnel_s *l2tp_tunnel = l2tp_session->tunnel;

    bbl_network_interface_s *network_interface = l2tp_tunnel->interface;

    uint16_t buf_len;

    bbl_ethernet_header_t eth = {0};
    bbl_ipv4_t l2tp_ipv4 = {0};
    bbl_udp_t l2tp_udp = {0};
    bbl_l2tp_t l2tp = {0};
    bbl_ipv4_t ipv4 = {0};
    bbl_udp_t udp = {0};
    bbl_bbl_t bbl = {0};

    eth.dst = network_interface->gateway_mac;
    eth.src = network_interface->mac;
    eth.vlan_outer = network_interface->vlan;
    eth.vlan_inner = 0;
    eth.type = ETH_TYPE_IPV4;
    eth.next = &l2tp_ipv4;
    l2tp_ipv4.dst = l2tp_tunnel->peer_ip;
    l2tp_ipv4.src = l2tp_tunnel->server->ip;
    l2tp_ipv4.ttl = 64;
    l2tp_ipv4.tos = config->priority;
    l2tp_ipv4.protocol = PROTOCOL_IPV4_UDP;
    l2tp_ipv4.next = &l2tp_udp;
    l2tp_udp.src = L2TP_UDP_PORT;
    l2tp_udp.dst = L2TP_UDP_PORT;
    l2tp_udp.protocol = UDP_PROTOCOL_L2TP;
    l2tp_udp.next = &l2tp;
    l2tp.type = L2TP_MESSAGE_DATA;
    l2tp.tunnel_id = l2tp_tunnel->peer_tunnel_id;
    l2tp.session_id = l2tp_session->peer_session_id;
    l2tp.protocol = PROTOCOL_IPV4;
    l2tp.with_length = l2tp_tunnel->server->data_length;
    l2tp.with_offset = l2tp_tunnel->server->data_offset;
    l2tp.next = &ipv4;
    ipv4.dst = session->ip_address;
    ipv4.src = l2tp_tunnel->server->ip;
    if(config->ipv4_df) {
        ipv4.offset = IPV4_DF;
    }
    ipv4.ttl = 64;
    ipv4.tos = config->priority;
    ipv4.protocol = PROTOCOL_IPV4_UDP;
    ipv4.next = &udp;
    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = BBL_TYPE_UNICAST_SESSION;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_DOWN;
    bbl.sub_type = BBL_SUB_TYPE_IPV4;
    if (config->length > 76) {
        bbl.padding = config->length - 76;
    }
    buf_len = config->length + 128;
    if(buf_len < 256) buf_len = 256;
    stream->buf = malloc(buf_len);
    if(encode_ethernet(stream->buf, &stream->tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->buf);
        stream->buf = NULL;
        stream->tx_len = 0;
        return false;
    }
    return true;
}

static bool
bbl_stream_build_packet(bbl_stream_s *stream)
{
    if(stream->config->stream_group_id == 0) {
        /* RAW stream */
        return bbl_stream_build_network_packet(stream);
    }
    if(stream->session) {
        if(stream->session->access_type == ACCESS_TYPE_PPPOE) {
            if(stream->session->l2tp_session) {
                if(stream->direction == BBL_DIRECTION_UP) {
                    return bbl_stream_build_access_pppoe_packet(stream);
                } else {
                    return bbl_stream_build_l2tp_packet(stream);
                }
            } else if(stream->session->a10nsp_session) {
                return bbl_stream_build_a10nsp_pppoe_packet(stream);
            } else {
                switch(stream->config->type) {
                    case BBL_SUB_TYPE_IPV4:
                    case BBL_SUB_TYPE_IPV6:
                    case BBL_SUB_TYPE_IPV6PD:
                        if(stream->direction == BBL_DIRECTION_UP) {
                            return bbl_stream_build_access_pppoe_packet(stream);
                        } else {
                            return bbl_stream_build_network_packet(stream);
                        }
                    default:
                        break;
                }
            }
        } else if (stream->session->access_type == ACCESS_TYPE_IPOE) {
            if(stream->session->a10nsp_session) {
                return bbl_stream_build_a10nsp_ipoe_packet(stream);
            } else {
                if(stream->direction == BBL_DIRECTION_UP) {
                    return bbl_stream_build_access_ipoe_packet(stream);
                } else {
                    return bbl_stream_build_network_packet(stream);
                }
            }
        }
    }
    return false;
}

static uint64_t
bbl_stream_send_window(bbl_stream_s *stream, struct timespec *now) {

    uint64_t packets = 1;
    uint64_t packets_expected;

    struct timespec time_elapsed = {0};

    /** Enforce optional stream traffic start delay ... */
    if(stream->config->start_delay && stream->packets_tx == 0) {
        if(stream->wait) {
            timespec_sub(&time_elapsed, now, &stream->wait_start);
            if(time_elapsed.tv_sec < stream->config->start_delay) {
                /** Still waiting ... */
                return 0;
            }
            /** Stop wait window ... */
        } else {
            /** Start wait window ... */
            stream->wait = true;
            stream->wait_start.tv_sec = now->tv_sec;
            stream->wait_start.tv_nsec = now->tv_nsec;
            return 0;
        }   
    }

    if(stream->send_window_packets == 0) {
        /* Open new send window */
        stream->send_window_start.tv_sec = now->tv_sec;
        stream->send_window_start.tv_nsec = now->tv_nsec;
    } else {
        timespec_sub(&time_elapsed, now, &stream->send_window_start);
        packets_expected = time_elapsed.tv_sec * stream->config->pps;
        packets_expected += stream->config->pps * ((double)time_elapsed.tv_nsec / 1000000000.0);

        if(packets_expected > stream->send_window_packets) {
            packets = packets_expected - stream->send_window_packets;
        }
        if(packets > g_ctx->config.io_stream_max_ppi) {
            packets = g_ctx->config.io_stream_max_ppi;
        }
    }

    /** Enforce optional stream packet limit ... */
    if(stream->config->max_packets &&
       stream->packets_tx + packets > stream->config->max_packets) {
       if(stream->packets_tx < stream->config->max_packets) {
           packets = stream->config->max_packets - stream->packets_tx;
       } else {
           packets = 0;
       }
    }

    return packets;
}

static void
bbl_stream_tx_stats(bbl_stream_s *stream, uint64_t packets, uint64_t bytes)
{
    bbl_session_s *session = stream->session;
    bbl_access_interface_s *access_interface;
    bbl_network_interface_s *network_interface;
    bbl_a10nsp_interface_s *a10nsp_interface;

    if(stream->direction == BBL_DIRECTION_UP) {
        access_interface = stream->access_interface;
        session = stream->session;
        if(access_interface) {
            access_interface->stats.packets_tx += packets;
            access_interface->stats.bytes_tx += bytes;
            access_interface->stats.stream_tx += packets;
            if(session) {
                session->stats.packets_tx += packets;
                session->stats.bytes_tx += bytes;
                session->stats.accounting_packets_tx += packets;
                session->stats.accounting_bytes_tx += bytes;
                if(stream->session_traffic) {
                    switch(stream->type) {
                        case BBL_SUB_TYPE_IPV4:
                            access_interface->stats.session_ipv4_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            access_interface->stats.session_ipv6_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            access_interface->stats.session_ipv6pd_tx += packets;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    } else {
        if(stream->network_interface) {
            network_interface = stream->network_interface;
            network_interface->stats.packets_tx += packets;
            network_interface->stats.bytes_tx += bytes;
            network_interface->stats.stream_tx += packets;
            if(session) {
                if(session->l2tp_session) {
                    network_interface->stats.l2tp_data_tx += packets;
                    session->l2tp_session->tunnel->stats.data_tx += packets;
                    session->l2tp_session->stats.data_tx += packets;
                    if(stream->type == BBL_SUB_TYPE_IPV4) {
                        session->l2tp_session->stats.data_ipv4_tx += packets;
                    }
                }
                if(stream->session_traffic) {
                    switch(stream->type) {
                        case BBL_SUB_TYPE_IPV4:
                            network_interface->stats.session_ipv4_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            network_interface->stats.session_ipv6_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            network_interface->stats.session_ipv6pd_tx += packets;
                            break;
                        default:
                            break;
                    }
                }
            }
        } else if(stream->a10nsp_interface) {
            a10nsp_interface = stream->a10nsp_interface;
            a10nsp_interface->stats.packets_tx += packets;
            a10nsp_interface->stats.bytes_tx += bytes;
            a10nsp_interface->stats.stream_tx += packets;
            if(session) {
                if(session->a10nsp_session) {
                    session->a10nsp_session->stats.packets_tx += packets;
                }
                if(stream->session_traffic) {
                    switch(stream->type) {
                        case BBL_SUB_TYPE_IPV4:
                            a10nsp_interface->stats.session_ipv4_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            a10nsp_interface->stats.session_ipv6_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            a10nsp_interface->stats.session_ipv6pd_tx += packets;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
}

static void
bbl_stream_rx_stats(bbl_stream_s *stream, uint64_t packets, uint64_t bytes, uint64_t loss)
{
    bbl_session_s *session = stream->session;
    bbl_access_interface_s *access_interface;
    bbl_network_interface_s *network_interface;
    bbl_a10nsp_interface_s *a10nsp_interface;

    if(stream->direction == BBL_DIRECTION_DOWN) {
        access_interface = stream->access_interface;
        session = stream->session;
        if(access_interface) {
            access_interface->stats.packets_rx += packets;
            access_interface->stats.bytes_rx += bytes;
            access_interface->stats.stream_rx += packets;
            access_interface->stats.stream_loss += loss;
            if(session) {
                session->stats.packets_rx += packets;
                session->stats.bytes_rx += bytes;
                session->stats.accounting_packets_rx += packets;
                session->stats.accounting_bytes_rx += bytes;
                if(stream->session_traffic) {
                    switch(stream->type) {
                        case BBL_SUB_TYPE_IPV4:
                            access_interface->stats.session_ipv4_rx += packets;
                            access_interface->stats.session_ipv4_loss += loss;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            access_interface->stats.session_ipv6_rx += packets;
                            access_interface->stats.session_ipv6_loss += loss;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            access_interface->stats.session_ipv6pd_rx += packets;
                            access_interface->stats.session_ipv6pd_loss += loss;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    } else {
        if(stream->network_interface) {
            network_interface = stream->network_interface;
            network_interface->stats.packets_rx += packets;
            network_interface->stats.bytes_rx += bytes;
            network_interface->stats.stream_rx += packets;
            network_interface->stats.stream_loss += loss;
            if(session) {
                if(session->l2tp_session) {
                    network_interface->stats.l2tp_data_rx += packets;
                    session->l2tp_session->tunnel->stats.data_rx += packets;
                    session->l2tp_session->stats.data_rx += packets;
                    if(stream->type == BBL_SUB_TYPE_IPV4) {
                        session->l2tp_session->stats.data_ipv4_rx += packets;
                    }
                }
                if(stream->session_traffic) {
                    switch(stream->type) {
                        case BBL_SUB_TYPE_IPV4:
                            network_interface->stats.session_ipv4_rx += packets;
                            network_interface->stats.session_ipv4_loss += loss;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            network_interface->stats.session_ipv6_rx += packets;
                            network_interface->stats.session_ipv6_loss += loss;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            network_interface->stats.session_ipv6pd_rx += packets;
                            network_interface->stats.session_ipv6pd_loss += loss;
                            break;
                        default:
                            break;
                    }
                }
            }
        } else if(stream->a10nsp_interface) {
            a10nsp_interface = stream->a10nsp_interface;
            a10nsp_interface->stats.packets_rx += packets;
            a10nsp_interface->stats.bytes_rx += bytes;
            a10nsp_interface->stats.stream_rx += packets;
            a10nsp_interface->stats.stream_loss += loss;
            if(session) {
                if(session->a10nsp_session) {
                    session->a10nsp_session->stats.packets_rx += packets;
                }
                if(stream->session_traffic) {
                    switch(stream->type) {
                        case BBL_SUB_TYPE_IPV4:
                            a10nsp_interface->stats.session_ipv4_rx += packets;
                            a10nsp_interface->stats.session_ipv4_loss += loss;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            a10nsp_interface->stats.session_ipv6_rx += packets;
                            a10nsp_interface->stats.session_ipv6_loss += loss;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            a10nsp_interface->stats.session_ipv6pd_rx += packets;
                            a10nsp_interface->stats.session_ipv6pd_loss += loss;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
}

static void
bbl_stream_rx_wrong_session(bbl_stream_s *stream) 
{
    uint64_t packets;
    uint64_t packets_delta;

    packets = stream->wrong_session;
    packets_delta = packets - stream->last_sync_wrong_session;
    stream->last_sync_wrong_session = packets;

    if(stream->access_interface) {
        switch(stream->type) {
            case BBL_SUB_TYPE_IPV4:
                stream->access_interface->stats.session_ipv4_wrong_session += packets_delta;
                break;
            case BBL_SUB_TYPE_IPV6:
                stream->access_interface->stats.session_ipv6_wrong_session += packets_delta;
                break;
            case BBL_SUB_TYPE_IPV6PD:
                stream->access_interface->stats.session_ipv6pd_wrong_session += packets_delta;
                break;
            default:
                break;
        }
    }
}

void
bbl_stream_ctrl(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;

    uint64_t packets;
    uint64_t packets_delta;
    uint64_t bytes_delta;
    uint64_t loss_delta;

    if(unlikely(stream->wrong_session)) {
        bbl_stream_rx_wrong_session(stream);
    }

    if(!stream->verified) {
        if(stream->rx_first_seq) {
            if(stream->session_traffic) {
                if(session) {
                    stream->verified = true;
                    session->session_traffic.flows_verified++;
                    g_ctx->stats.session_traffic_flows_verified++;
                    if(g_ctx->stats.session_traffic_flows_verified == g_ctx->stats.session_traffic_flows) {
                        LOG_NOARG(INFO, "ALL SESSION TRAFFIC FLOWS VERIFIED\n");
                    }
                }
            } else {
                stream->verified = true;
                g_ctx->stats.stream_traffic_flows_verified++;
                if(g_ctx->stats.stream_traffic_flows_verified == g_ctx->stats.stream_traffic_flows) {
                    LOG_NOARG(INFO, "ALL STREAM TRAFFIC FLOWS VERIFIED\n");
                }
            }
        }
        if(stream->verified) {
            if(g_ctx->config.traffic_stop_verified) {
                stream->stop = true;
            }
        } else {
            return;
        }
    }

    /* Update rates. */
    bbl_compute_avg_rate(&stream->rate_packets_tx, stream->packets_tx);
    bbl_compute_avg_rate(&stream->rate_packets_rx, stream->packets_rx);

    /* Calculate TX packets/bytes since last sync. */
    packets = stream->packets_tx;
    packets_delta = packets - stream->last_sync_packets_tx;
    bytes_delta = packets_delta * stream->tx_len;
    stream->last_sync_packets_tx = packets;
    bbl_stream_tx_stats(stream, packets_delta, bytes_delta);
    /* Calculate RX packets/bytes since last sync. */
    packets = stream->packets_rx;
    packets_delta = packets - stream->last_sync_packets_rx;
    bytes_delta = packets_delta * stream->rx_len;
    stream->last_sync_packets_rx = packets;
    /* Calculate RX loss since last sync. */
    packets = stream->loss;
    loss_delta = packets - stream->last_sync_loss;
    stream->last_sync_loss = packets;
    bbl_stream_rx_stats(stream, packets_delta, bytes_delta, loss_delta);
}

void
bbl_stream_ctrl_job(timer_s *timer) {
    bbl_stream_s *stream = timer->data;
    bbl_stream_ctrl(stream);
}

void
bbl_stream_tx_job(timer_s *timer)
{
    bbl_stream_s *stream = timer->data;
    bbl_session_s *session = stream->session;

    io_handle_s *io;
    io_thread_s *thread = stream->thread;
    if(thread) {
        io = thread->io;
    } else {
        io = stream->interface->io.tx;
    }

    struct timespec now;

    uint64_t packets = 1;
    uint64_t send = 0;

    if(!bbl_stream_can_send(stream)) {
        return;
    }
    if(!stream->buf) {
        if(!bbl_stream_build_packet(stream)) {
            LOG(ERROR, "Failed to build packet for stream %s\n", stream->config->name);
            return;
        }
    }

    /* Close send window */
    if(!g_traffic || stream->stop) {
        stream->send_window_packets = 0;
        return;
    }
    if(session) {
        if(stream->session_traffic) {
            if(!session->session_traffic.active) {
                stream->send_window_packets = 0;
                return;
            }
        } else if(!session->streams.active) {
            stream->send_window_packets = 0;
            return;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    packets = bbl_stream_send_window(stream, &now);
    /* Update BBL header fields */
    *(uint32_t*)(stream->buf + (stream->tx_len - 8)) = now.tv_sec;
    *(uint32_t*)(stream->buf + (stream->tx_len - 4)) = now.tv_nsec;
    while(packets) {
        *(uint64_t*)(stream->buf + (stream->tx_len - 16)) = stream->flow_seq;
        /* Send packet */
        if(!io_send(io, stream->buf, stream->tx_len)) {
            return;
        }
        stream->send_window_packets++;
        stream->packets_tx++;
        stream->flow_seq++;
        packets--;
        send++;
    }
}

static bool
bbl_stream_add_jobs(bbl_stream_s *stream, time_t timer_sec, long timer_nsec)
{    
    bbl_interface_s *interface = stream->interface;
    
    io_handle_s *io = interface->io.tx;
    io_thread_s *thread = io->thread;

    if(thread) {
        while(io && io->thread) {
            if(io->thread->pps_reserved < thread->pps_reserved) {
                thread = io->thread;
            }
            io = io->next;
        }
        thread->pps_reserved += stream->config->pps;

        stream->thread = thread;
        timer_add_periodic(&thread->timer.root, &stream->timer_tx, stream->config->name, 
                           timer_sec, timer_nsec, stream, &bbl_stream_tx_job);
    } else {
        timer_add_periodic(&g_ctx->timer_root, &stream->timer_tx, stream->config->name, 
                           timer_sec, timer_nsec, stream, &bbl_stream_tx_job);
    }
    timer_add_periodic(&g_ctx->timer_root, &stream->timer_ctrl, "STREAM-CTRL", 
                       1, 0, stream, &bbl_stream_ctrl_job);
    return true;
}

static bool 
bbl_stream_add(bbl_stream_config_s *config, bbl_session_s *session)
{
    bbl_interface_s *interface = NULL;
    bbl_network_interface_s *network_interface = NULL;
    bbl_a10nsp_interface_s *a10nsp_interface = NULL;
    bbl_stream_s *stream = NULL;

    dict_insert_result result;

    time_t timer_sec = 0;
    long timer_nsec  = 0;

    /* *
    * The corresponding network interfaces will be selected
    * in the following order:
    * - "network-interface" from stream section
    * - "network-interface" from access interface section
    * - first network interface from network section (default)
    */
    if(config->network_interface) {
        network_interface = bbl_network_interface_get(config->network_interface);
    } else if(config->a10nsp_interface) {
        a10nsp_interface = bbl_a10nsp_interface_get(config->a10nsp_interface);
    } else if(session->network_interface) {
        network_interface = session->network_interface;
    } else if(session->a10nsp_interface) {
        a10nsp_interface = session->a10nsp_interface;
    }

    if(a10nsp_interface) {
        interface = a10nsp_interface->interface;
    } else if (network_interface) {
        interface = network_interface->interface;
    } else {
        LOG_NOARG(ERROR, "Failed to add stream because of missing network/a01nsp interface\n");
        return false;
    }

    timer_nsec = SEC / config->pps;
    timer_sec = timer_nsec / 1000000000;
    timer_nsec = timer_nsec % 1000000000;

    if(config->direction & BBL_DIRECTION_UP) {
        stream = calloc(1, sizeof(bbl_stream_s));
        stream->flow_id = g_ctx->flow_id++;
        stream->flow_seq = 1;
        stream->config = config;
        stream->type = config->type;
        stream->direction = BBL_DIRECTION_UP;
        stream->interface = session->access_interface->interface;
        stream->access_interface = session->access_interface;
        stream->session = session;
        stream->tx_interval = timer_sec * 1e9 + timer_nsec;
        result = dict_insert(g_ctx->stream_flow_dict, &stream->flow_id);
        if (!result.inserted) {
            LOG(ERROR, "Failed to insert stream %s\n", config->name);
            free(stream);
            return false;
        }
        *result.datum_ptr = stream;
        stream->session_next = session->streams.head;
        session->streams.head = stream;
        bbl_stream_add_jobs(stream, timer_sec, timer_nsec);
        g_ctx->stats.stream_traffic_flows++;
        LOG(DEBUG, "Traffic stream %s added to %s (upstream) with %lf PPS (timer: %lu sec %lu nsec)\n", 
            interface->name, config->name, config->pps, timer_sec, timer_nsec);
    }
    if(config->direction & BBL_DIRECTION_DOWN) {
        stream = calloc(1, sizeof(bbl_stream_s));
        stream->flow_id = g_ctx->flow_id++;
        stream->flow_seq = 1;
        stream->config = config;
        stream->type = config->type;
        stream->direction = BBL_DIRECTION_DOWN;
        stream->interface = interface;
        stream->network_interface = network_interface;
        stream->a10nsp_interface = a10nsp_interface;
        stream->session = session;
        stream->tx_interval = timer_sec * 1e9 + timer_nsec;
        result = dict_insert(g_ctx->stream_flow_dict, &stream->flow_id);
        if (!result.inserted) {
            LOG(ERROR, "Failed to insert stream %s\n", config->name);
            free(stream);
            return false;
        }
        *result.datum_ptr = stream;
        stream->session_next = session->streams.head;
        session->streams.head = stream;
        bbl_stream_add_jobs(stream, timer_sec, timer_nsec);
        g_ctx->stats.stream_traffic_flows++;
        LOG(DEBUG, "Traffic stream %s added to %s (downstream) with %lf PPS (timer %lu sec %lu nsec)\n", 
            interface->name, config->name, config->pps, timer_sec, timer_nsec);
    }
    return true;
}

bool
bbl_stream_session_init(bbl_session_s *session)
{
    bbl_stream_config_s *config;

    /** Add session traffic ... */
    if(g_ctx->config.stream_config_session_ipv4_up && session->endpoint.ipv4) {
        if(!bbl_stream_add(g_ctx->config.stream_config_session_ipv4_up, session)) {
            return false;
        }
        session->session_traffic.ipv4_up = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv4_down && session->endpoint.ipv4) {
        if(!bbl_stream_add(g_ctx->config.stream_config_session_ipv4_down, session)) {
            return false;
        }
        session->session_traffic.ipv4_down = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6_up && session->endpoint.ipv6) {
        if(!bbl_stream_add(g_ctx->config.stream_config_session_ipv6_up, session)) {
            return false;
        }
        session->session_traffic.ipv6_up = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6_down && session->endpoint.ipv6) {
        if(!bbl_stream_add(g_ctx->config.stream_config_session_ipv6_down, session)) {
            return false;
        }
        session->session_traffic.ipv6_down = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6pd_up && session->endpoint.ipv6pd) {
        if(!bbl_stream_add(g_ctx->config.stream_config_session_ipv6pd_up, session)) {
            return false;
        }
        session->session_traffic.ipv6pd_up = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6pd_down && session->endpoint.ipv6pd) {
        if(!bbl_stream_add(g_ctx->config.stream_config_session_ipv6pd_down, session)) {
            return false;
        }
        session->session_traffic.ipv6pd_down = session->streams.head;
    }

    /** Add streams of corresponding stream-group-id */
    if(session->streams.group_id) {
        config = g_ctx->config.stream_config;
        while(config) {
            if(config->stream_group_id == session->streams.group_id) {
                if(!bbl_stream_add(config, session)) {
                    return false;
                }
            }
            config = config->next;
        }
    }

    return true;
}

bool
bbl_stream_init() {

    bbl_stream_config_s *config;
    bbl_stream_s *stream;

    bbl_network_interface_s *network_interface;

    dict_insert_result result;

    time_t timer_sec = 0;
    long timer_nsec  = 0;
    int i;

    uint32_t group;
    uint32_t source;

    /* Add RAW streams */
    config = g_ctx->config.stream_config;
    while(config) {
        if(config->stream_group_id == 0) {
            network_interface = bbl_network_interface_get(config->network_interface);
            if(!network_interface) {
                LOG_NOARG(ERROR, "Failed to add RAW stream because of missing network interface\n");
                return false;
            }

            timer_nsec = SEC / config->pps;
            timer_sec = timer_nsec / 1000000000;
            timer_nsec = timer_nsec % 1000000000;

            if(config->direction & BBL_DIRECTION_DOWN) {
                stream = calloc(1, sizeof(bbl_stream_s));
                stream->flow_id = g_ctx->flow_id++;
                stream->flow_seq = 1;
                stream->config = config;
                stream->type = config->type;
                stream->direction = BBL_DIRECTION_DOWN;
                stream->interface = network_interface->interface;
                stream->network_interface = network_interface;
                stream->tx_interval = timer_sec * 1e9 + timer_nsec;
                result = dict_insert(g_ctx->stream_flow_dict, &stream->flow_id);
                if (!result.inserted) {
                    LOG(ERROR, "Failed to insert stream %s\n", config->name);
                    free(stream);
                    return false;
                }
                *result.datum_ptr = stream;
                bbl_stream_add_jobs(stream, timer_sec, timer_nsec);
                g_ctx->stats.stream_traffic_flows++;
                LOG(DEBUG, "RAW traffic stream %s added to %s (downstream) with %lf PPS (timer %lu sec %lu nsec)\n", 
                    network_interface->name, config->name, config->pps, timer_sec, timer_nsec);
            }
        }
        config = config->next;
    }

    /* Add autogenerated multicast streams */
    if(g_ctx->config.send_multicast_traffic && g_ctx->config.igmp_group_count) {
        network_interface = bbl_network_interface_get(g_ctx->config.multicast_traffic_network_interface);
        if(!network_interface) {
            LOG_NOARG(ERROR, "Failed to add multicast streams because of missing network interface\n");
            return false;
        }

        timer_nsec = SEC / g_ctx->config.multicast_traffic_pps;
        timer_sec = timer_nsec / 1000000000;
        timer_nsec = timer_nsec % 1000000000;

        for(i = 0; i < g_ctx->config.igmp_group_count; i++) {

            group = be32toh(g_ctx->config.igmp_group) + i * be32toh(g_ctx->config.igmp_group_iter);
            if(g_ctx->config.igmp_source) {
                source = g_ctx->config.igmp_source;
            } else {
                source = network_interface->ip.address;
            }
            group = htobe32(group);

            config = calloc(1, sizeof(bbl_stream_config_s));
            config->name = (char*)g_multicast_traffic;
            config->type = BBL_SUB_TYPE_IPV4;
            config->direction = BBL_DIRECTION_DOWN;
            config->pps = g_ctx->config.multicast_traffic_pps;
            config->length = g_ctx->config.multicast_traffic_len;
            config->priority = g_ctx->config.multicast_traffic_tos;
            config->ipv4_destination_address = group;
            config->ipv4_network_address = source;
        }
    }

    /* Add session traffic stream configurations */
    if(g_ctx->config.session_traffic_ipv4_pps) {
        /* Upstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv4_up;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV4;
        config->direction = BBL_DIRECTION_UP;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv4_pps;
        config->ipv4_network_address = g_ctx->config.session_traffic_ipv4_address;
        g_ctx->config.stream_config_session_ipv4_up = config;
        /* Downstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv4_down;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV4;
        config->direction = BBL_DIRECTION_DOWN;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv4_pps;
        config->ipv4_network_address = g_ctx->config.session_traffic_ipv4_address;
        if(g_ctx->config.session_traffic_ipv4_label) {
            config->tx_mpls1 = true;
            config->tx_mpls1_label = g_ctx->config.session_traffic_ipv4_label;
        }
        g_ctx->config.stream_config_session_ipv4_down = config;
    }
    if(g_ctx->config.session_traffic_ipv6_pps) {
        /* Upstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6_up;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6;
        config->direction = BBL_DIRECTION_UP;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6_pps;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        g_ctx->config.stream_config_session_ipv6_up = config;
        /* Downstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6_down;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6;
        config->direction = BBL_DIRECTION_DOWN;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6_pps;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        if(g_ctx->config.session_traffic_ipv6_label) {
            config->tx_mpls1 = true;
            config->tx_mpls1_label = g_ctx->config.session_traffic_ipv6_label;
        }
        g_ctx->config.stream_config_session_ipv6_down = config;
    }
    if(g_ctx->config.session_traffic_ipv6pd_pps) {
        /* Upstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6pd_up;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6PD;
        config->direction = BBL_DIRECTION_UP;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6_pps;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        g_ctx->config.stream_config_session_ipv6pd_up = config;
        /* Downstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6pd_down;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6PD;
        config->direction = BBL_DIRECTION_DOWN;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6_pps;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        if(g_ctx->config.session_traffic_ipv6_label) {
            config->tx_mpls1 = true;
            config->tx_mpls1_label = g_ctx->config.session_traffic_ipv6_label;
        }
        g_ctx->config.stream_config_session_ipv6pd_down = config;
    }
    return true;
}

void
bbl_stream_reset(bbl_stream_s *stream)
{
    if(!stream) return;

    stream->reset = true;

    stream->reset_packets_tx = stream->packets_tx;
    stream->reset_packets_rx = stream->packets_rx;
    stream->reset_loss = stream->loss;
    stream->reset_wrong_session = stream->wrong_session;

    stream->min_delay_ns = 0;
    stream->max_delay_ns = 0;
    stream->rx_len = 0;
    stream->rx_first_seq = 0;
    stream->rx_last_seq = 0;
    stream->rx_priority = 0;
    stream->rx_outer_vlan_pbit = 0;
    stream->rx_inner_vlan_pbit = 0;
    stream->rx_mpls1 = false;
    stream->rx_mpls1_exp = 0;
    stream->rx_mpls1_ttl = 0;
    stream->rx_mpls1_label = 0;
    stream->rx_mpls2 = false;
    stream->rx_mpls2_exp = 0;
    stream->rx_mpls2_ttl = 0;
    stream->rx_mpls2_label = 0;
    stream->verified = false;
    stream->stop = false;
}

json_t *
bbl_stream_json(bbl_stream_s *stream)
{
    json_t *root = NULL;

    if(!stream) {
        return NULL;
    }

    root = json_pack("{ss* ss si si si si si si si si si si si si si si si si si si sf sf sf}",
        "name", stream->config->name,
        "direction", stream->direction == BBL_DIRECTION_UP ? "upstream" : "downstream",
        "flow-id", stream->flow_id,
        "rx-first-seq", stream->rx_first_seq,
        "rx-last-seq", stream->rx_last_seq,
        "rx-tos-tc", stream->rx_priority,
        "rx-outer-vlan-pbit", stream->rx_outer_vlan_pbit,
        "rx-inner-vlan-pbit", stream->rx_inner_vlan_pbit,
        "rx-len", stream->rx_len,
        "tx-len", stream->tx_len,
        "rx-packets", stream->packets_rx,
        "tx-packets", stream->packets_tx,
        "rx-loss", stream->loss,
        "rx-delay-nsec-min", stream->min_delay_ns,
        "rx-delay-nsec-max", stream->max_delay_ns,
        "rx-pps", stream->rate_packets_rx.avg,
        "tx-pps", stream->rate_packets_tx.avg,
        "tx-bps-l2", stream->rate_packets_tx.avg * stream->tx_len * 8,
        "rx-bps-l2", stream->rate_packets_rx.avg * stream->rx_len * 8,
        "rx-bps-l3", stream->rate_packets_rx.avg * stream->config->length * 8,
        "tx-mbps-l2", (double)(stream->rate_packets_tx.avg * stream->tx_len * 8) / 1000000.0,
        "rx-mbps-l2", (double)(stream->rate_packets_rx.avg * stream->rx_len * 8) / 1000000.0,
        "rx-mbps-l3", (double)(stream->rate_packets_rx.avg * stream->config->length * 8) / 1000000.0);

    if(stream->config->rx_mpls1) { 
        json_object_set(root, "rx-mpls1-expected", json_integer(stream->config->rx_mpls1_label));
    }
    if(stream->rx_mpls1) {
        json_object_set(root, "rx-mpls1", json_integer(stream->rx_mpls1_label));
        json_object_set(root, "rx-mpls1-exp", json_integer(stream->rx_mpls1_exp));
        json_object_set(root, "rx-mpls1-ttl", json_integer(stream->rx_mpls1_ttl));
    }
    if(stream->config->rx_mpls2) { 
        json_object_set(root, "rx-mpls2-expected", json_integer(stream->config->rx_mpls2_label));
    }
    if(stream->rx_mpls2) {
        json_object_set(root, "rx-mpls2", json_integer(stream->rx_mpls2_label));
        json_object_set(root, "rx-mpls2-exp", json_integer(stream->rx_mpls2_exp));
        json_object_set(root, "rx-mpls2-ttl", json_integer(stream->rx_mpls2_ttl));
    }
    return root;
}

bbl_stream_s *
bbl_stream_rx(bbl_ethernet_header_t *eth, bbl_session_s *session)
{
    bbl_bbl_t *bbl = eth->bbl;
    bbl_stream_s *stream;
    bbl_mpls_t *mpls;
    void **search = NULL;

    uint64_t loss = 0;

    if(!(bbl && bbl->type == BBL_TYPE_UNICAST_SESSION)) {
        return NULL;
    }

    search = dict_search(g_ctx->stream_flow_dict, &bbl->flow_id);
    if(search) {
        stream = *search;
        if(stream->rx_first_seq) {
            /* Stream already verified */
            if((stream->rx_last_seq +1) < bbl->flow_seq) {
                loss = bbl->flow_seq - (stream->rx_last_seq +1);
                stream->loss += loss;
                LOG(LOSS, "LOSS flow: %lu seq: %lu last: %lu\n",
                    bbl->flow_id, bbl->flow_seq, stream->rx_last_seq);
            }
        } else {
            /* Verify stream ... */
            stream->rx_len = eth->length;
            stream->rx_priority = eth->tos;
            stream->rx_outer_vlan_pbit = eth->vlan_outer_priority;
            stream->rx_inner_vlan_pbit = eth->vlan_inner_priority;
            mpls = eth->mpls;
            if(mpls) {
                stream->rx_mpls1 = true;
                stream->rx_mpls1_label = mpls->label;
                stream->rx_mpls1_exp = mpls->exp;
                stream->rx_mpls1_ttl = mpls->ttl;
                mpls = mpls->next;
                if(mpls) {
                    stream->rx_mpls2 = true;
                    stream->rx_mpls2_label = mpls->label;
                    stream->rx_mpls2_exp = mpls->exp;
                    stream->rx_mpls2_ttl = mpls->ttl;
                }
            }
            if(stream->config->rx_mpls1_label) {
                /* Check if expected outer label is received ... */
                if(stream->rx_mpls1_label != stream->config->rx_mpls1_label) {
                    /* Wrong outer label received! */
                    return NULL;
                }
                if(stream->config->rx_mpls2_label) {
                    /* Check if expected inner label is received ... */
                    if(stream->rx_mpls2_label != stream->config->rx_mpls2_label) {
                        /* Wrong inner label received! */
                        return NULL;
                    }
                }
            }
            if(bbl->sub_type != stream->type || 
                bbl->direction != stream->direction) {
                return NULL;
            }
            if(session && stream->session_traffic) {
                if(bbl->outer_vlan_id != session->vlan_key.outer_vlan_id ||
                   bbl->inner_vlan_id != session->vlan_key.inner_vlan_id ||
                   bbl->session_id != session->session_id) {
                    stream->wrong_session++;
                    return NULL;
                }
            }
            stream->rx_first_seq = bbl->flow_seq;
        }
        stream->packets_rx++;
        stream->rx_last_seq = bbl->flow_seq;
        bbl_stream_delay(stream, &eth->timestamp, &bbl->timestamp);
        return stream;
    } else {
        return NULL;
    }
}