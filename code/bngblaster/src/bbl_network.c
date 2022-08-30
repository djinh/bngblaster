/*
 * BNG Blaster (BBL) - Network Interface Functions
 *
 * Christian Giese, August 2022
 *
 * Copyright (C) 2020-2022, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bbl.h"
#include "bbl_session.h"
#include "bbl_stream.h"

void
bbl_network_interface_rate_job(timer_s *timer) {
    bbl_network_interface_s *interface = timer->data;
    bbl_compute_avg_rate(&interface->stats.rate_packets_tx, interface->stats.packets_tx);
    bbl_compute_avg_rate(&interface->stats.rate_packets_rx, interface->stats.packets_rx);
    bbl_compute_avg_rate(&interface->stats.rate_bytes_tx, interface->stats.bytes_tx);
    bbl_compute_avg_rate(&interface->stats.rate_bytes_rx, interface->stats.bytes_rx);
    bbl_compute_avg_rate(&interface->stats.rate_mc_tx, interface->stats.mc_tx);
    bbl_compute_avg_rate(&interface->stats.rate_li_rx, interface->stats.li_rx);
    bbl_compute_avg_rate(&interface->stats.rate_l2tp_data_rx, interface->stats.l2tp_data_rx);
    bbl_compute_avg_rate(&interface->stats.rate_l2tp_data_tx, interface->stats.l2tp_data_tx);
    if(g_ctx->stats.stream_traffic_flows) {
        bbl_compute_avg_rate(&interface->stats.rate_stream_tx, interface->stats.stream_tx);
        bbl_compute_avg_rate(&interface->stats.rate_stream_rx, interface->stats.stream_rx);
    }
    if(g_ctx->stats.session_traffic_flows) {
        bbl_compute_avg_rate(&interface->stats.rate_session_ipv4_tx, interface->stats.session_ipv4_tx);
        bbl_compute_avg_rate(&interface->stats.rate_session_ipv4_rx, interface->stats.session_ipv4_rx);
        bbl_compute_avg_rate(&interface->stats.rate_session_ipv6_tx, interface->stats.session_ipv6_tx);
        bbl_compute_avg_rate(&interface->stats.rate_session_ipv6_rx, interface->stats.session_ipv6_rx);
        bbl_compute_avg_rate(&interface->stats.rate_session_ipv6pd_tx, interface->stats.session_ipv6pd_tx);
        bbl_compute_avg_rate(&interface->stats.rate_session_ipv6pd_rx, interface->stats.session_ipv6pd_rx);
    }
}

static bool
bbl_network_interfaces_duplicate(bbl_interface_s *interface, uint16_t vlan) {
    bbl_network_interface_s *network_interface = interface->network;
    while(network_interface) {
        if(network_interface->vlan == vlan) {
            return true;
        }
        network_interface = network_interface->next;
    }
    return false;
}

/**
 * bbl_network_interfaces_add
 */
bool
bbl_network_interfaces_add()
{
    bbl_network_config_s *network_config = g_ctx->config.network_config;
    bbl_network_interface_s *network_interface;
    bbl_interface_s *interface;
    isis_instance_s *isis;
    bool isis_result;

    char ifname[SUB_STR_LEN];

    while(network_config) {
        /* Generate sub-interface name */
        if(network_config->vlan) {
            snprintf(ifname, sizeof(ifname), "%s:%u", 
                     network_config->interface, network_config->vlan);
        } else {
            snprintf(ifname, sizeof(ifname), "%s", 
                     network_config->interface);
        }
        
        interface = bbl_interface_get(network_config->interface);
        if (!interface) {
            LOG(ERROR, "Failed to add network interface %s (interface not found)\n", ifname);
            return false;
        }
        if(interface->access && network_config->vlan == 0) {
            LOG(ERROR, "Failed to add network interface %s (untagged not allowed on access interfaces)\n", ifname);
            return false;
        }
        if(bbl_network_interfaces_duplicate(interface, network_config->vlan)) {
            LOG(ERROR, "Failed to add network interface %s (duplicate)\n", ifname);
            return false;
        }
        network_interface = calloc(1, sizeof(bbl_network_interface_s));
        network_interface->next = interface->network;
        interface->network = network_interface;
        network_interface->interface = interface;
        network_config->network_interface = network_interface;
        network_interface->name = strdup(ifname);

        CIRCLEQ_INSERT_TAIL(&g_ctx->network_interface_qhead, network_interface, network_interface_qnode);

        /* Init TXQ */
        network_interface->txq = calloc(1, sizeof(bbl_txq_s));
        bbl_txq_init(network_interface->txq, BBL_TXQ_DEFAULT_SIZE);

        /* Init ethernet */
        network_interface->vlan = network_config->vlan;
        if(*(uint64_t*)network_config->mac & 0xffffffffffff00) {
            memcpy(network_interface->mac, network_config->mac, ETH_ADDR_LEN);
        } else {
            memcpy(network_interface->mac, interface->mac, ETH_ADDR_LEN);
        }        

        /* Copy gateway MAC from config (default 00:00:00:00:00:00) */
        memcpy(network_interface->gateway_mac, network_config->gateway_mac, ETH_ADDR_LEN);

        /* Init IPv4 */
        if(network_config->ip.address && network_config->gateway) {
            network_interface->ip.address = network_config->ip.address;
            network_interface->ip.len = network_config->ip.len;
            network_interface->gateway = network_config->gateway;
            /* Send initial ARP request */
            network_interface->send_requests |= BBL_IF_SEND_ARP_REQUEST;
        }

        /* Init link-local IPv6 address */
        network_interface->ip6_ll[0]  = 0xfe;
        network_interface->ip6_ll[1]  = 0x80;
        network_interface->ip6_ll[8]  = network_interface->mac[0];
        network_interface->ip6_ll[9]  = network_interface->mac[1];
        network_interface->ip6_ll[10] = network_interface->mac[2];
        network_interface->ip6_ll[11] = 0xff;
        network_interface->ip6_ll[12] = 0xfe;
        network_interface->ip6_ll[13] = network_interface->mac[3];
        network_interface->ip6_ll[14] = network_interface->mac[4];
        network_interface->ip6_ll[15] = network_interface->mac[5];

        /* Init IPv6 */
        if(ipv6_prefix_not_zero(&network_config->ip6) && 
           ipv6_addr_not_zero(&network_config->gateway6)) {
            /* Init global IPv6 address */
            memcpy(&network_interface->ip6, &network_config->ip6, sizeof(ipv6_prefix));
            memcpy(&network_interface->gateway6, &network_config->gateway6, sizeof(ipv6addr_t));
            memcpy(&network_interface->gateway6_solicited_node_multicast, &ipv6_solicited_node_multicast, sizeof(ipv6addr_t));
            memcpy(((uint8_t*)&network_interface->gateway6_solicited_node_multicast)+13,
                   ((uint8_t*)&network_interface->gateway6)+13, 3);

            /* Send initial ICMPv6 NS */
            network_interface->send_requests |= BBL_IF_SEND_ICMPV6_NS;
        }

        network_interface->gateway_resolve_wait = network_config->gateway_resolve_wait;

        /* Init TCP */
        if(!bbl_tcp_network_interface_init(network_interface, network_config)) {
            LOG(ERROR, "Failed to init TCP for network interface %s\n", ifname);
            return false;
        }

        /* Init routing protocols */ 
        if(network_config->isis_instance_id) {
            isis_result = false;
            isis = g_ctx->isis_instances;
            while (isis) {
                if(isis->config->id == network_config->isis_instance_id) {
                    isis_result = isis_adjacency_init(network_interface, network_config, isis);
                    if(!isis_result) {
                        LOG(ERROR, "Failed to enable IS-IS for network interface %s (adjacency init failed)\n", ifname);
                        return false;
                    }
                    break;
                }
                isis = isis->next;
            }
            if(!isis_result) {
                LOG(ERROR, "Failed to enable IS-IS for network interface %s (instance not found)\n", ifname);
                return false;
            }
        }

        /* TX list init */
        CIRCLEQ_INIT(&network_interface->l2tp_tx_qhead);

        /* Timer to compute periodic rates */
        timer_add_periodic(&g_ctx->timer_root, &network_interface->rate_job, "Rate Computation", 1, 0, network_interface,
                           &bbl_network_interface_rate_job);

        LOG(DEBUG, "Added network interface %s\n", ifname);
        network_config = network_config->next;
    }
    return true;
}

/**
 * bbl_network_interface_get
 *
 * @brief This function returns the network interface
 * with the given name and VLAN or the first network 
 * interface found if name is NULL.
 *
 * @param interface_name interface name
 * @return network interface
 */
bbl_network_interface_s*
bbl_network_interface_get(char *interface_name)
{
    struct bbl_interface_ *interface;
    bbl_network_interface_s *network_interface;

    CIRCLEQ_FOREACH(interface, &g_ctx->interface_qhead, interface_qnode) {
        network_interface = interface->network;
        while(network_interface) {
            if(!interface_name) {
                return network_interface;
            }
            if(strcmp(network_interface->name, interface_name) == 0) {
                return network_interface;
            }
            network_interface = network_interface->next;
        }
    }
    return NULL;
}

static void
bbl_network_update_eth(bbl_network_interface_s *interface,
                       bbl_ethernet_header_t *eth) {
    eth->dst = eth->src;
    eth->src = interface->mac;
    eth->vlan_outer = interface->vlan;
    eth->vlan_inner = 0;
    eth->vlan_three = 0;
    if(interface->tx_label.label) {
        eth->mpls = &interface->tx_label;
    } else {
        eth->mpls = NULL;
    }
}

static bbl_txq_result_t
bbl_network_arp_reply(bbl_network_interface_s *interface,
                      bbl_ethernet_header_t *eth,
                      bbl_arp_t *arp) {
    bbl_network_update_eth(interface, eth);
    arp->code = ARP_REPLY;
    arp->sender = interface->mac;
    arp->sender_ip = arp->target_ip;
    arp->target = interface->gateway_mac;
    arp->target_ip = interface->gateway;
    return bbl_txq_to_buffer(interface->txq, eth);
}

static bbl_txq_result_t
bbl_network_icmp_reply(bbl_network_interface_s *interface,
                       bbl_ethernet_header_t *eth,
                       bbl_ipv4_t *ipv4,
                       bbl_icmp_t *icmp) {
    uint32_t dst = ipv4->dst;
    bbl_network_update_eth(interface, eth);
    ipv4->dst = ipv4->src;
    ipv4->src = dst;
    ipv4->ttl = 64;
    icmp->type = ICMP_TYPE_ECHO_REPLY;
    return bbl_txq_to_buffer(interface->txq, eth);
}

static bbl_txq_result_t
bbl_network_icmpv6_na(bbl_network_interface_s *interface,
                      bbl_ethernet_header_t *eth,
                      bbl_ipv6_t *ipv6,
                      bbl_icmpv6_t *icmpv6) {
    bbl_network_update_eth(interface, eth);
    ipv6->dst = ipv6->src;
    ipv6->src = icmpv6->prefix.address;
    ipv6->ttl = 255;
    icmpv6->type = IPV6_ICMPV6_NEIGHBOR_ADVERTISEMENT;
    icmpv6->mac = interface->mac;
    icmpv6->flags = 0;
    icmpv6->data = NULL;
    icmpv6->data_len = 0;
    icmpv6->dns1 = NULL;
    icmpv6->dns2 = NULL;
    return bbl_txq_to_buffer(interface->txq, eth);
}

static bbl_txq_result_t
bbl_network_icmpv6_echo_reply(bbl_network_interface_s *interface,
                              bbl_ethernet_header_t *eth,
                              bbl_ipv6_t *ipv6,
                              bbl_icmpv6_t *icmpv6) {
    uint8_t *dst = ipv6->dst;
    bbl_network_update_eth(interface, eth);
    ipv6->dst = ipv6->src;
    ipv6->src = dst;
    ipv6->ttl = 255;
    icmpv6->type = IPV6_ICMPV6_ECHO_REPLY;
    return bbl_txq_to_buffer(interface->txq, eth);
}

static void
bbl_network_rx_arp(bbl_network_interface_s *interface, bbl_ethernet_header_t *eth) {
    bbl_secondary_ip_s *secondary_ip;

    bbl_arp_t *arp = (bbl_arp_t*)eth->next;
    if(arp->sender_ip == interface->gateway) {
        interface->arp_resolved = true;
        if(*(uint32_t*)interface->gateway_mac == 0) {
            memcpy(interface->gateway_mac, arp->sender, ETH_ADDR_LEN);
        }
        if(arp->code == ARP_REQUEST) {
            if(arp->target_ip == interface->ip.address) {
                bbl_network_arp_reply(interface, eth, arp);
            } else {
                secondary_ip = g_ctx->config.secondary_ip_addresses;
                while(secondary_ip) {
                    if(arp->target_ip == secondary_ip->ip) {
                        bbl_network_arp_reply(interface, eth, arp);
                        return;
                    }
                    secondary_ip = secondary_ip->next;
                }
            }
        }
    }
}

static void
bbl_network_rx_icmpv6(bbl_network_interface_s *interface, 
                      bbl_ethernet_header_t *eth) {
    bbl_ipv6_t *ipv6;
    bbl_icmpv6_t *icmpv6;
    bbl_secondary_ip6_s *secondary_ip6;

    ipv6 = (bbl_ipv6_t*)eth->next;
    icmpv6 = (bbl_icmpv6_t*)ipv6->next;

    if(memcmp(ipv6->src, interface->gateway6, IPV6_ADDR_LEN) == 0) {
        interface->icmpv6_nd_resolved = true;
        if(*(uint32_t*)interface->gateway_mac == 0) {
            memcpy(interface->gateway_mac, eth->src, ETH_ADDR_LEN);
        }
    }
    if(icmpv6->type == IPV6_ICMPV6_NEIGHBOR_SOLICITATION) {
        if(memcmp(icmpv6->prefix.address, interface->ip6.address, IPV6_ADDR_LEN) == 0) {
            bbl_network_icmpv6_na(interface, eth, ipv6, icmpv6);
        } else if(memcmp(icmpv6->prefix.address, interface->ip6_ll, IPV6_ADDR_LEN) == 0) {
            bbl_network_icmpv6_na(interface, eth, ipv6, icmpv6);
        } else {
            secondary_ip6 = g_ctx->config.secondary_ip6_addresses;
            while(secondary_ip6) {
                if(memcmp(icmpv6->prefix.address, secondary_ip6->ip, IPV6_ADDR_LEN) == 0) {
                    bbl_network_icmpv6_na(interface, eth, ipv6, icmpv6);
                    return;
                }
                secondary_ip6 = secondary_ip6->next;
            }
        }
    } else if(icmpv6->type == IPV6_ICMPV6_ECHO_REQUEST) {
        bbl_network_icmpv6_echo_reply(interface, eth, ipv6, icmpv6);
    }
}

static void
bbl_network_rx_icmp(bbl_network_interface_s *interface, 
                    bbl_ethernet_header_t *eth, bbl_ipv4_t *ipv4)
{
    bbl_icmp_t *icmp = (bbl_icmp_t*)ipv4->next;
    if(icmp->type == ICMP_TYPE_ECHO_REQUEST) {
        /* Send ICMP reply... */
        bbl_network_icmp_reply(interface, eth, ipv4, icmp);
    }
}

/**
 * bbl_network_rx_handler
 *
 * This function handles all packets received on network interfaces.
 *
 * @param interface pointer to network interface on which packet was received
 * @param eth pointer to ethernet header structure of received packet
 */
void
bbl_network_rx_handler(bbl_network_interface_s *interface, 
                       bbl_ethernet_header_t *eth)
{
    bbl_ipv4_t *ipv4 = NULL;
    bbl_ipv6_t *ipv6 = NULL;
    bbl_udp_t *udp = NULL;

    switch(eth->type) {
        case ETH_TYPE_ARP:
            bbl_network_rx_arp(interface, eth);
            return;
        case ETH_TYPE_IPV4:
            if(memcmp(interface->mac, eth->dst, ETH_ADDR_LEN) != 0) {
                /* Drop wrong MAC */
                return;
            }
            ipv4 = (bbl_ipv4_t*)eth->next;
            if(ipv4->protocol == PROTOCOL_IPV4_UDP) {
                udp = (bbl_udp_t*)ipv4->next;
                if(udp->protocol == UDP_PROTOCOL_QMX_LI) {
                    bbl_qmx_li_handler_rx(interface, eth, (bbl_qmx_li_t*)udp->next);
                    return;
                } else if(udp->protocol == UDP_PROTOCOL_L2TP) {
                    bbl_l2tp_handler_rx(interface, eth, (bbl_l2tp_t*)udp->next);
                    return;
                }
            } else if(ipv4->protocol == PROTOCOL_IPV4_ICMP) {
                interface->stats.icmp_rx++;
                bbl_network_rx_icmp(interface, eth, ipv4);
                return;
            } else if(ipv4->protocol == PROTOCOL_IPV4_TCP) {
                interface->stats.tcp_rx++;
                bbl_tcp_ipv4_rx(interface, eth, ipv4);
                return;
            }
            break;
        case ETH_TYPE_IPV6:
            ipv6 = (bbl_ipv6_t*)eth->next;
            if(ipv6->protocol == IPV6_NEXT_HEADER_ICMPV6) {
                bbl_network_rx_icmpv6(interface, eth);
                return;
            }
            break;
        case ISIS_PROTOCOL_IDENTIFIER:
            isis_handler_rx(interface, eth);
            return;
        default:
            break;
    }
    interface->interface->stats.unknown++;
}