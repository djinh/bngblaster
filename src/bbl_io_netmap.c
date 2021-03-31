/*
 * BNG Blaster (BBL) - Netmap
 *
 * Christian Giese, October 2021
 *
 * Copyright (C) 2020-2021, RtBrick, Inc.
 */

#ifdef BNGBLASTER_NETMAP

#include "bbl.h"
#include "bbl_io_netmap.h"
#include "bbl_pcap.h"
#include "bbl_rx.h"
#include "bbl_tx.h"

void
bbl_io_netmap_rx_job (timer_s *timer)
{
    bbl_interface_s *interface;
    bbl_ctx_s *ctx;
    bbl_io_netmap_ctx *io_ctx;

	struct netmap_ring *ring;
	unsigned int i;

    uint8_t *eth_start;
    uint eth_len;

    bbl_ethernet_header_t *eth;
    protocol_error_t decode_result;

    interface = timer->data;
    if (!interface) {
        return;
    }
    ctx = interface->ctx;
    io_ctx = interface->io_ctx;

    /* Get RX timestamp */
    clock_gettime(CLOCK_REALTIME, &interface->rx_timestamp);

    ring = NETMAP_RXRING(io_ctx->port->nifp, 0);
    while (!nm_ring_empty(ring)) {

        i = ring->cur;
        eth_start = (uint8_t*)NETMAP_BUF(ring, ring->slot[i].buf_idx);
        eth_len = ring->slot[i].len;
        interface->stats.packets_rx++;
        interface->stats.bytes_rx += eth_len;

        /*
	     * Dump the packet into pcap file.
	     */
        if (ctx->pcap.write_buf) {
	        pcapng_push_packet_header(ctx, &interface->rx_timestamp, eth_start, eth_len,
				                      interface->pcap_index, PCAPNG_EPB_FLAGS_INBOUND);
        }

        decode_result = decode_ethernet(eth_start, eth_len, interface->ctx->sp_rx, SCRATCHPAD_LEN, &eth);
        if(decode_result == PROTOCOL_SUCCESS) {
            /* Copy RX timestamp */
            eth->timestamp.tv_sec = ring->ts.tv_sec;
            eth->timestamp.tv_nsec = ring->ts.tv_usec * 1000;
            if(interface->access) {
                bbl_rx_handler_access(eth, interface);
            } else {
                bbl_rx_handler_network(eth, interface);
            }
        } else if (decode_result == UNKNOWN_PROTOCOL) {
            interface->stats.packets_rx_drop_unknown++;
        } else {
            interface->stats.packets_rx_drop_decode_error++;
        }

        ring->head = ring->cur = nm_ring_next(ring, i);
    }
    pcapng_fflush(ctx);
    ioctl(io_ctx->port->fd, NIOCRXSYNC, NULL);
}

void
bbl_io_netmap_tx_job (timer_s *timer)
{
    bbl_interface_s *interface;
    bbl_ctx_s *ctx;
    bbl_io_netmap_ctx *io_ctx;
    bool send = false;

    struct netmap_ring *ring;
	unsigned int i;

    uint8_t *buf;
    uint16_t len;

    protocol_error_t tx_result = IGNORED;

    interface = timer->data;
    if (!interface) {
        return;
    }
    ctx = interface->ctx;
    io_ctx = interface->io_ctx;

    /* Get TX timestamp */
    clock_gettime(CLOCK_REALTIME, &interface->tx_timestamp);

    ring = NETMAP_TXRING(io_ctx->port->nifp, 0);
    while(tx_result != EMPTY) {
        /* Check if this slot available for writing. */
        if (nm_ring_empty(ring)) {
            interface->stats.no_tx_buffer++;
            break;
        } 
        i = ring->cur;
        buf = (uint8_t*)NETMAP_BUF(ring, ring->slot[i].buf_idx);

        tx_result = bbl_tx(ctx, interface, buf, &len);
        if (tx_result == PROTOCOL_SUCCESS) {
            send = true;
            interface->stats.packets_tx++;
            interface->stats.bytes_tx += len;
            ring->slot[i].len = len;
            ring->head = ring->cur = nm_ring_next(ring, i);
            /* Dump the packet into pcap file. */
            if (ctx->pcap.write_buf) {
                pcapng_push_packet_header(ctx, &interface->tx_timestamp,
                                          buf, len, interface->pcap_index, 
                                          PCAPNG_EPB_FLAGS_OUTBOUND);
            }
        }
    }
    if(send) {
        pcapng_fflush(ctx);
        ioctl(io_ctx->port->fd, NIOCTXSYNC, NULL);
    }
}

/** 
 * bbl_io_netmap_send 
 * 
 * @param interface interface.
 * @param packet packet to be send
 * @param packet_len packet length
 */
bool
bbl_io_netmap_send (bbl_interface_s *interface, uint8_t *packet, uint16_t packet_len) {
    bbl_ctx_s *ctx;
    bbl_io_netmap_ctx *io_ctx;

    struct netmap_ring *ring;
	unsigned int i;

    uint8_t *buf;

    ctx = interface->ctx;
    io_ctx = interface->io_ctx;

    ring = NETMAP_TXRING(io_ctx->port->nifp, 0);
    if (nm_ring_empty(ring)) {
        interface->stats.no_tx_buffer++;
        return false;
    }

    i = ring->cur;
    buf = (uint8_t*)NETMAP_BUF(ring, ring->slot[i].buf_idx);
    memcpy(buf, packet, packet_len);
    ring->slot[i].len = packet_len;
    ring->head = ring->cur = nm_ring_next(ring, i);

    interface->stats.packets_tx++;
    interface->stats.bytes_tx += packet_len;
    /* Dump the packet into pcap file. */
    if (ctx->pcap.write_buf) {
        pcapng_push_packet_header(ctx, &interface->tx_timestamp,
                                  packet, packet_len, interface->pcap_index, 
                                  PCAPNG_EPB_FLAGS_OUTBOUND);
        pcapng_fflush(ctx);
    }
    return true;   
}

/** 
 * bbl_io_netmap_add_interface 
 * 
 * @param ctx global context
 * @param interface interface.
 * @param slots ring buffer size (currently not used)
 */
bool
bbl_io_netmap_add_interface(bbl_ctx_s *ctx, bbl_interface_s *interface, int slots) {
    bbl_io_netmap_ctx *io_ctx;
    char timer_name[128];
    char netmap_port[128];

    UNUSED(slots);

    snprintf(netmap_port, sizeof(netmap_port), "netmap:%s", interface->name);

    io_ctx = calloc(1, sizeof(bbl_io_netmap_ctx));
    interface->io_mode = IO_MODE_NETMAP;
    interface->io_ctx = io_ctx;

    /*
     * Open netmap port.
     */
    io_ctx->port = nm_open(netmap_port, NULL, NETMAP_NO_TX_POLL, NULL);
    if (io_ctx->port == NULL) {
		if (!errno) {
            LOG(ERROR, "Failed to nm_open(%s): not a netmap port\n", netmap_port);
		} else {
			LOG(ERROR, "Failed to nm_open(%s): %s\n", netmap_port, strerror(errno));
		}
        return false;
	}

    /*
     * Add an periodic timer for polling I/O.
     */
    snprintf(timer_name, sizeof(timer_name), "%s TX", interface->name);
    timer_add_periodic(&ctx->timer_root, &interface->tx_job, timer_name, 0, ctx->config.tx_interval, interface, bbl_io_netmap_tx_job);
    snprintf(timer_name, sizeof(timer_name), "%s RX", interface->name);
    timer_add_periodic(&ctx->timer_root, &interface->rx_job, timer_name, 0, ctx->config.rx_interval, interface, bbl_io_netmap_rx_job);

    return true;
}

#endif