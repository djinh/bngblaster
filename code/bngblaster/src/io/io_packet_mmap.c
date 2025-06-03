/*
 * BNG Blaster (BBL) - IO PACKET_MMAP
 *
 * Christian Giese, July 2022
 *
 * PACKET_MMAP provides a size configurable circular buffer mapped in user space
 * that can be used to either send or receive packets. This way reading packets
 * just needs to wait for them, most of the time there is no need to issue a single
 * system call. Concerning transmission, multiple packets can be sent through one
 * system call to get the highest bandwidth. By using a shared buffer between the
 * kernel and the user also has the benefit of minimizing packet copies.
 *
 * https://www.kernel.org/doc/Documentation/networking/packet_mmap.txt
 *
 * Copyright (C) 2020-2025, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "io.h"

extern bool g_init_phase;
extern bool g_traffic;

static void
poll_kernel(io_handle_s *io, short events)
{
    struct pollfd pollset;
    pollset.fd = io->fd;
    pollset.events = events;
    pollset.revents = 0;
    io->stats.polled++;
    if(poll(&pollset, 1, 0) == -1) {
        LOG(IO, "Failed to poll interface %s", 
            io->interface->name);
    }
}

/**
 * This job is for PACKET_MMAP RX in main thread!
 */
void
io_packet_mmap_rx_job(timer_s *timer)
{
    io_handle_s *io = timer->data;
    bbl_interface_s *interface = io->interface;

    uint8_t *frame_ptr;
    struct tpacket2_hdr *tphdr;

    bbl_ethernet_header_s *eth;
    uint16_t vlan;

    protocol_error_t decode_result;
    bool pcap = false;

    assert(io->mode == IO_MODE_PACKET_MMAP);
    assert(io->direction == IO_INGRESS);
    assert(io->thread == NULL);

    frame_ptr = io->ring + (io->cursor * io->req.tp_frame_size);
    tphdr = (struct tpacket2_hdr*)frame_ptr;
    if(!(tphdr->tp_status & TP_STATUS_USER)) {
        /* If no buffer is available poll kernel */
        poll_kernel(io, POLLIN);
        return;
    }

    /* Get RX timestamp */
    //clock_gettime(CLOCK_MONOTONIC, &io->timestamp);
    io->timestamp.tv_sec = timer->timestamp->tv_sec;
    io->timestamp.tv_nsec = timer->timestamp->tv_nsec;
    while(tphdr->tp_status & TP_STATUS_USER) {
        io->buf = (uint8_t*)tphdr + tphdr->tp_mac;
        io->buf_len = tphdr->tp_len;
        io->stats.packets++;
        io->stats.bytes += io->buf_len;
        decode_result = decode_ethernet(io->buf, io->buf_len, g_ctx->sp, SCRATCHPAD_LEN, &eth);
        if(decode_result == PROTOCOL_SUCCESS) {
            vlan = tphdr->tp_vlan_tci & BBL_ETH_VLAN_ID_MAX;
            if(vlan && eth->vlan_outer != vlan) {
                /* The outer VLAN is stripped from header */
                eth->vlan_inner = eth->vlan_outer;
                eth->vlan_inner_priority = eth->vlan_outer_priority;
                eth->vlan_outer = vlan;
                eth->vlan_outer_priority = tphdr->tp_vlan_tci >> 13;
                if(tphdr->tp_vlan_tpid == ETH_TYPE_QINQ) {
                    eth->qinq = true;
                }
            }
            /* Copy RX timestamp */
            //eth->timestamp.tv_sec = tphdr->tp_sec; /* ktime/hw timestamp */
            //eth->timestamp.tv_nsec = tphdr->tp_nsec; /* ktime/hw timestamp */
            eth->timestamp.tv_sec = io->timestamp.tv_sec;
            eth->timestamp.tv_nsec = io->timestamp.tv_nsec;
            /* Dump the packet into pcap file */
            if(g_ctx->pcap.write_buf && (!eth->bbl || g_ctx->pcap.include_streams)) {
                pcap = true;
                pcapng_push_packet_header(&io->timestamp, io->buf, io->buf_len,
                                          interface->ifindex, PCAPNG_EPB_FLAGS_INBOUND);
            }
            bbl_rx_handler(interface, eth);
        } else {
            /* Dump the packet into pcap file */
            if(g_ctx->pcap.write_buf) {
                pcap = true;
                pcapng_push_packet_header(&io->timestamp, io->buf, io->buf_len,
                                          interface->ifindex, PCAPNG_EPB_FLAGS_INBOUND);
            }
            if(decode_result == UNKNOWN_PROTOCOL) {
                io->stats.unknown++;
            } else {
                io->stats.protocol_errors++;
            }
        }
        /* Return ownership back to kernel */
        tphdr->tp_status = TP_STATUS_KERNEL; 
        /* Get next packet */
        io->cursor = (io->cursor + 1) % io->req.tp_frame_nr;
        frame_ptr = io->ring + (io->cursor * io->req.tp_frame_size);
        tphdr = (struct tpacket2_hdr*)frame_ptr;
    }
    if(pcap) {
        pcapng_fflush();
    }
}

/**
 * This job is for PACKET_MMAP TX in main thread!
 */
void
io_packet_mmap_tx_job(timer_s *timer)
{
    io_handle_s *io = timer->data;
    bbl_interface_s *interface = io->interface;

    struct tpacket2_hdr* tphdr;
    uint8_t *frame_ptr;

    bbl_stream_s *stream = NULL;
    uint16_t burst = interface->config->io_burst;
    uint64_t now;

    bool ctrl = true;
    bool pcap = false;

    assert(io->mode == IO_MODE_PACKET_MMAP);
    assert(io->direction == IO_EGRESS);
    assert(io->thread == NULL);

    if(io->update_streams) {
        io_stream_update_pps(io);
    }

    frame_ptr = io->ring + (io->cursor * io->req.tp_frame_size);
    tphdr = (struct tpacket2_hdr*)frame_ptr;
    if(tphdr->tp_status != TP_STATUS_AVAILABLE) {
        /* If no buffer is available poll kernel */
        poll_kernel(io, POLLOUT);
        io->stats.no_buffer++;
    } else {
        /* Get TX timestamp */
        //clock_gettime(CLOCK_MONOTONIC, &io->timestamp);
        io->timestamp.tv_sec = timer->timestamp->tv_sec;
        io->timestamp.tv_nsec = timer->timestamp->tv_nsec;
        now = timespec_to_nsec(timer->timestamp);
        while(burst) {
            /* Check if this slot available for writing. */
            if(tphdr->tp_status != TP_STATUS_AVAILABLE) {
                io->stats.no_buffer++;
                break;
            }
            io->buf = frame_ptr + TPACKET2_HDRLEN - sizeof(struct sockaddr_ll);

            if(unlikely(ctrl)) {
                /* First send all control traffic which has higher priority. */
                if(bbl_tx(interface, io->buf, &io->buf_len) != PROTOCOL_SUCCESS) {
                    ctrl = false;
                    continue;
                }
            } else {
                if(!(g_traffic && g_init_phase == false && interface->state == INTERFACE_UP)) {
                    bbl_stream_io_stop(io);
                    break;
                }
                stream = bbl_stream_io_send_iter(io, now);
                if(unlikely(stream == NULL)) {
                    break;
                }
                memcpy(io->buf, stream->tx_buf, stream->tx_len);
                io->buf_len = stream->tx_len;
                stream->tx_packets++;
                stream->flow_seq++;
            } 
            tphdr->tp_len = io->buf_len;
            tphdr->tp_status = TP_STATUS_SEND_REQUEST;

            io->queued++;
            io->stats.packets++;
            io->stats.bytes += io->buf_len;
            burst--;

            /* Dump the packet into pcap file. */
            if(g_ctx->pcap.write_buf && (ctrl || g_ctx->pcap.include_streams)) {
                pcap = true;
                pcapng_push_packet_header(&io->timestamp, io->buf, io->buf_len,
                                          interface->ifindex, PCAPNG_EPB_FLAGS_OUTBOUND);
            }

            /* Get next slot. */
            io->cursor = (io->cursor + 1) % io->req.tp_frame_nr;
            frame_ptr = io->ring + (io->cursor * io->req.tp_frame_size);
            tphdr = (struct tpacket2_hdr *)frame_ptr;
        }
        if(pcap) {
            pcapng_fflush();
        }
    }

    if(io->queued) {
        /* Notify kernel. */
        if(sendto(io->fd, NULL, 0, 0, NULL, 0) < 0) {
            LOG(IO, "PACKET_MMAP sendto on interface %s failed with error %s (%d)\n", 
                interface->name, strerror(errno), errno);
            io->stats.io_errors++;
        } else {
            io->queued = 0;
        }
    }
}

void
io_packet_mmap_thread_rx_run_fn(io_thread_s *thread)
{
    io_handle_s *io = thread->io;

    uint16_t cursor = io->cursor;
    uint16_t frame_size = io->req.tp_frame_size;
    uint16_t frame_nr = io->req.tp_frame_nr;
    uint8_t *frame_ptr;
    uint8_t *ring = io->ring;

    struct tpacket2_hdr *tphdr;

    assert(io->mode == IO_MODE_PACKET_MMAP);
    assert(io->direction == IO_INGRESS);
    assert(io->thread);

    struct timespec sleep, rem;

    sleep.tv_sec = 0;
    sleep.tv_nsec = 10000; /* 0.01ms */

    while(thread->active) {
        frame_ptr = ring + (cursor * frame_size);
        tphdr = (struct tpacket2_hdr*)frame_ptr;
        if(!(tphdr->tp_status & TP_STATUS_USER)) {
            /* If no buffer is available poll kernel */
            //poll_kernel(io, POLLIN);
            nanosleep(&sleep, &rem);
            continue;
        }

        /* Get RX timestamp */
        clock_gettime(CLOCK_MONOTONIC, &io->timestamp);
        while(tphdr->tp_status & TP_STATUS_USER) {
            io->buf = (uint8_t*)tphdr + tphdr->tp_mac;
            io->buf_len = tphdr->tp_len;
            io->vlan_tci = tphdr->tp_vlan_tci;
            io->vlan_tpid = tphdr->tp_vlan_tpid;
            /* Process packet */
            if(io_thread_rx_handler(thread, io) == IO_FULL) {
                break;
            }
            /* Return ownership back to kernel */
            tphdr->tp_status = TP_STATUS_KERNEL; 
            /* Get next packet */
            cursor = (cursor + 1) % frame_nr;
            frame_ptr = ring + (cursor * frame_size);
            tphdr = (struct tpacket2_hdr*)frame_ptr;
        }
        nanosleep(&sleep, &rem);
    }
}

void
io_packet_mmap_thread_tx_run_fn(io_thread_s *thread)
{
    io_handle_s *io = thread->io;
    bbl_interface_s *interface = io->interface;

    bbl_txq_s *txq = thread->txq;
    bbl_txq_slot_t *slot;

    struct tpacket2_hdr* tphdr;
    uint8_t *frame_ptr;

    bbl_stream_s *stream = NULL;
    uint16_t io_burst = interface->config->io_burst;
    uint16_t burst = 0;
    uint64_t now;

    bool ctrl = true;

    struct timespec sleep, rem;
    sleep.tv_sec = 0;
    sleep.tv_nsec = 10;

    assert(io->mode == IO_MODE_PACKET_MMAP);
    assert(io->direction == IO_EGRESS);
    assert(io->thread);

    while(thread->active) {
        nanosleep(&sleep, &rem);
        if(io->update_streams) {
            io_stream_update_pps(io);
        }

        frame_ptr = io->ring + (io->cursor * io->req.tp_frame_size);
        tphdr = (struct tpacket2_hdr *)frame_ptr;
        if(tphdr->tp_status != TP_STATUS_AVAILABLE) {
            /* If no buffer is available poll kernel. */
            io->stats.no_buffer++;
            poll_kernel(io, POLLOUT);
            continue;
        }

        /* Get TX timestamp */
        clock_gettime(CLOCK_MONOTONIC, &io->timestamp);
        
        burst = io_burst;
        ctrl = true;
        now = timespec_to_nsec(&io->timestamp);
        while(burst) {
            if(tphdr->tp_status != TP_STATUS_AVAILABLE) {
                io->stats.no_buffer++;
                poll_kernel(io, POLLOUT);
                break;
            }
            io->buf = frame_ptr + TPACKET2_HDRLEN - sizeof(struct sockaddr_ll);

            if(unlikely(ctrl)) {
                /* First send all control traffic which has higher priority. */
                slot = bbl_txq_read_slot(txq);
                if(slot) {
                    io->buf_len = slot->packet_len;
                    memcpy(io->buf, slot->packet, slot->packet_len);
                    bbl_txq_read_next(txq);
                } else {
                    ctrl = false;
                    continue;
                }
            } else {
                if(!(g_traffic && g_init_phase == false && interface->state == INTERFACE_UP)) {
                    bbl_stream_io_stop(io);
                    break;
                }
                /* Send traffic streams up to allowed burst. */
                stream = bbl_stream_io_send_iter(io, now);
                if(unlikely(stream == NULL)) {
                    break;
                }
                memcpy(io->buf, stream->tx_buf, stream->tx_len);
                io->buf_len = stream->tx_len;
                stream->tx_packets++;
                stream->flow_seq++;
            }

            tphdr->tp_len = io->buf_len;
            tphdr->tp_status = TP_STATUS_SEND_REQUEST;

            io->queued++;
            io->stats.packets++;
            io->stats.bytes += io->buf_len;
            burst--;

            /* Get next slot. */
            io->cursor = (io->cursor + 1) % io->req.tp_frame_nr;
            frame_ptr = io->ring + (io->cursor * io->req.tp_frame_size);
            tphdr = (struct tpacket2_hdr *)frame_ptr;
        }

        if(io->queued) {
            /* Notify kernel. */
            if(sendto(io->fd, NULL, 0, 0, NULL, 0) < 0) {
                LOG(IO, "PACKET_MMAP sendto on interface %s failed with error %s (%d)\n", 
                    interface->name, strerror(errno), errno);
                io->stats.io_errors++;
            } else {
                io->queued = 0;
            }
        }
    }
}

bool
io_packet_mmap_init(io_handle_s *io)
{
    bbl_interface_s *interface = io->interface;
    bbl_link_config_s *config = interface->config;
    
    io_thread_s *thread = io->thread;
    
    if(!io_socket_open(io)) {
        return false;
    }

    if(thread) {
        if(io->direction == IO_INGRESS) {
            thread->run_fn = io_packet_mmap_thread_rx_run_fn;
        } else {
            thread->run_fn = io_packet_mmap_thread_tx_run_fn;
        }
    } else {
        if(io->direction == IO_INGRESS) {
            timer_add_periodic(&g_ctx->timer_root, &interface->io.rx_job, "RX", 0, 
                config->rx_interval, io, &io_packet_mmap_rx_job);
        } else {
            timer_add_periodic(&g_ctx->timer_root, &interface->io.tx_job, "TX", 0, 
                config->tx_interval, io, &io_packet_mmap_tx_job);
        }
    }
    return true;
}

void
io_packet_mmap_set_max_stream_len()
{
    uint16_t len = getpagesize() - BBL_MAX_STREAM_OVERHEAD - (TPACKET2_HDRLEN - sizeof(struct sockaddr_ll));

    if(len < g_ctx->config.io_max_stream_len) {
        LOG(DEBUG, "Set max allowed stream length to %u because of packet_mmap limitations\n", len);
        g_ctx->config.io_max_stream_len = len;
    }
}