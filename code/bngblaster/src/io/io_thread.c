/*
 * BNG Blaster (BBL) - IO RX Threads
 *
 * Christian Giese, July 2022
 *
 * Copyright (C) 2020-2025, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "io.h"

/** 
 * This function redirects the packet in the
 * IO buffer to the main thread via the TXQ 
 * ring buffer. 
 * 
 * @param thread thread handle
 * @param io IO handle
 * @return IO_REDIRECT if successfully redirected
 */
static io_result_t
redirect(io_thread_s *thread, io_handle_s *io)
{
    bbl_txq_slot_t *slot;

    assert(io->direction == IO_INGRESS);
    assert(io->thread != NULL);
    if(io->buf_len > BBL_TXQ_BUFFER_LEN) {
        return IO_ERROR;
    }

    if((slot = bbl_txq_write_slot(thread->txq))) {
        slot->timestamp.tv_sec = io->timestamp.tv_sec;
        slot->timestamp.tv_nsec = io->timestamp.tv_nsec;
        slot->vlan_tci = io->vlan_tci;
        slot->vlan_tpid = io->vlan_tpid;
        slot->packet_len = io->buf_len;
        memcpy(slot->packet, io->buf, io->buf_len);
        bbl_txq_write_next(thread->txq);
        return IO_REDIRECT;
    }
    return IO_FULL;
}

/** 
 * This function processes all received packets
 * from RX threads. 
 * 
 * @param thread thread handle
 * @param io IO handle
 * @return IO result
 */
io_result_t
io_thread_rx_handler(io_thread_s *thread, io_handle_s *io)
{
    assert(io->direction == IO_INGRESS);
    assert(io->thread != NULL);

    bbl_ethernet_header_s *eth;
    uint16_t vlan;

    protocol_error_t decode_result;

    io->stats.packets++;
    io->stats.bytes += io->buf_len;
    if(packet_is_bbl(io->buf, io->buf_len)) {
        /** Process */
        decode_result = decode_ethernet(io->buf, io->buf_len, thread->sp, SCRATCHPAD_LEN, &eth);
        if(decode_result == PROTOCOL_SUCCESS) {
            if(io->vlan_tci) {
                vlan = io->vlan_tci & BBL_ETH_VLAN_ID_MAX;
                /* Restore outer VLAN */
                eth->vlan_inner = eth->vlan_outer;
                eth->vlan_inner_priority = eth->vlan_outer_priority;
                eth->vlan_outer = vlan;
                eth->vlan_outer_priority = io->vlan_tci >> 13;
                if(io->vlan_tpid == ETH_TYPE_QINQ) {
                    eth->qinq = true;
                }
            }
            /* Copy RX timestamp */
            eth->timestamp.tv_sec = io->timestamp.tv_sec;
            eth->timestamp.tv_nsec = io->timestamp.tv_nsec;
            if(bbl_rx_thread(io->interface, eth)) {
                return IO_SUCCESS;
            }
        } else if(decode_result == UNKNOWN_PROTOCOL) {
            io->stats.unknown++;
        } else {
            io->stats.protocol_errors++;
        }
    }
    /** Redirect to main thread. */
    return redirect(thread, io);
}

/** 
 * This job is scheduled in the main loop receiving 
 * packets from a RX thread via TXQ ring buffer. 
 */
void
io_thread_main_rx_job(timer_s *timer)
{
    bbl_interface_s *interface = timer->data;

    io_handle_s *io = interface->io.rx;
    io_thread_s *thread;

    bbl_txq_slot_t *slot;
    bbl_ethernet_header_s *eth;
    uint16_t vlan;

    protocol_error_t decode_result;
    bool pcap = false;
    while(io) {
        thread = io->thread;
        if(thread) {
            while((slot = bbl_txq_read_slot(thread->txq))) {
                decode_result = decode_ethernet(slot->packet, slot->packet_len, g_ctx->sp, SCRATCHPAD_LEN, &eth);
                if(decode_result == PROTOCOL_SUCCESS) {
                    if(slot->vlan_tci) {
                        vlan = slot->vlan_tci & BBL_ETH_VLAN_ID_MAX;
                        /* Restore outer VLAN */
                        eth->vlan_inner = eth->vlan_outer;
                        eth->vlan_inner_priority = eth->vlan_outer_priority;
                        eth->vlan_outer = vlan;
                        eth->vlan_outer_priority = slot->vlan_tci >> 13;
                        if(slot->vlan_tpid == ETH_TYPE_QINQ) {
                            eth->qinq = true;
                        }
                    }
                    /* Copy RX timestamp */
                    eth->timestamp.tv_sec = slot->timestamp.tv_sec;
                    eth->timestamp.tv_nsec = slot->timestamp.tv_nsec;
                    /* Dump the packet into pcap file. */
                    if(g_ctx->pcap.write_buf && (!eth->bbl || g_ctx->pcap.include_streams)) {
                        pcap = true;
                        pcapng_push_packet_header(&slot->timestamp, slot->packet, slot->packet_len,
                                                  interface->ifindex, PCAPNG_EPB_FLAGS_INBOUND);
                    }
                    bbl_rx_handler(interface, eth);
                } else {
                    /* Dump the packet into pcap file. */
                    if(g_ctx->pcap.write_buf) {
                        pcap = true;
                        pcapng_push_packet_header(&slot->timestamp, slot->packet, slot->packet_len,
                                                  interface->ifindex, PCAPNG_EPB_FLAGS_INBOUND);
                    }
                    if(decode_result == UNKNOWN_PROTOCOL) {
                        io->stats.unknown++;
                    } else {
                        io->stats.protocol_errors++;
                    }
                }
                bbl_txq_read_next(thread->txq);
            }
        }
        io = io->next;
    }
    if(pcap) {
        pcapng_fflush();
    }
}

/** 
 * This job is scheduled in the main loop sending 
 * packets to the TX thread via TXQ ring buffer. 
 */
void
io_thread_main_tx_job(timer_s *timer)
{
    bbl_interface_s *interface = timer->data;
    io_handle_s *io = interface->io.tx;
    io_thread_s *thread = io->thread;
    bbl_txq_s *txq = thread->txq;
    bbl_txq_slot_t *slot;

    protocol_error_t tx_result = IGNORED;

    bool pcap = false;

    /* Get TX timestamp */
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    while((slot = bbl_txq_write_slot(txq))) {
        tx_result = bbl_tx(interface, slot->packet, &slot->packet_len);
        if(tx_result == PROTOCOL_SUCCESS) {
            /* Dump the packet into pcap file. */
            if(g_ctx->pcap.write_buf) {
                pcap = true;
                pcapng_push_packet_header(&timestamp, slot->packet, slot->packet_len,
                                          interface->ifindex, PCAPNG_EPB_FLAGS_OUTBOUND);
            }
            bbl_txq_write_next(txq);
        } else if(tx_result == EMPTY) {
            break;
        }
    }
    if(pcap) {
        pcapng_fflush();
    }
}

void *
io_thread_main(void *thread_data)
{
    io_thread_s *thread = thread_data;
    if(thread->setup_fn) {
        (*thread->setup_fn)(thread);
    }
    if(thread->run_fn) {
        (*thread->run_fn)(thread);
    }
    if(thread->teardown_fn) {
        (*thread->teardown_fn)(thread);
    }
    thread->active = false;
    thread->stopped = true;
    return NULL;
}

bool
io_thread_init(io_handle_s *io)
{
    bbl_interface_s *interface = io->interface;
    bbl_link_config_s *config = interface->config;
    io_thread_s *thread;

    uint16_t slots = config->io_slots_tx;
    if(io->direction == IO_INGRESS) {
        LOG(DEBUG, "Init RX thread for interface %s\n", interface->name);
        slots = config->io_slots_rx;
    } else {
        LOG(DEBUG, "Init TX thread for interface %s\n", interface->name);
    }

    /* Add thread */
    thread = calloc(1, sizeof(io_thread_s));
    thread->next = g_ctx->io_threads;
    g_ctx->io_threads = thread;

    io->thread = thread;
    thread->io = io;
    io->fanout_id = interface->kernel_index;
    io->fanout_type = PACKET_FANOUT_HASH;

    /* Allocate thread scratchpad memory */
    thread->sp = malloc(SCRATCHPAD_LEN);

    /* Init thread TXQ */
    thread->txq = calloc(1, sizeof(bbl_txq_s));
    if(!(thread->txq && bbl_txq_init(thread->txq, slots))) {
        return false;
    }

    /* Init thread mutex */
    if(pthread_mutex_init(&thread->mutex, NULL) != 0) {
        LOG_NOARG(ERROR, "Failed to init mutex\n");
        return false;
    }

    /* Default run function which might be overwritten */
    thread->run_fn = NULL;

    /* Add thread main loop timers/jobs */
    if(io->direction == IO_INGRESS && !interface->io.rx_job) {
        /** Start job reading from RX thread TXQ */
        timer_add_periodic(&g_ctx->timer_root, &interface->io.rx_job, "RX", 
                           0, config->rx_interval, 
                           interface, &io_thread_main_rx_job);
    }

    if(io->direction == IO_EGRESS && !interface->io.tx_job) {
        /** Start job writing to first TX thread TXQ */
        timer_add_periodic(&g_ctx->timer_root, &interface->io.tx_job, "TX", 
                           0, config->tx_interval, 
                           interface, &io_thread_main_tx_job);
    }

    /* Set CPU affinity */
    if(io->direction == IO_INGRESS && config->rx_cpuset_count) {
        thread->set_cpu_affinity = true;
        CPU_ZERO(&thread->cpuset);
        CPU_SET(config->rx_cpuset[config->rx_cpuset_cur++], &thread->cpuset);
        if(config->rx_cpuset_cur >= config->rx_cpuset_count) {
            config->rx_cpuset_cur = 0;
        }
    }
    if(io->direction == IO_EGRESS && config->tx_cpuset_count) {
        thread->set_cpu_affinity = true;
        CPU_ZERO(&thread->cpuset);
        CPU_SET(config->tx_cpuset[config->tx_cpuset_cur++], &thread->cpuset);
        if(config->tx_cpuset_cur >= config->tx_cpuset_count) {
            config->tx_cpuset_cur = 0;
        }
    }
    return true;
}

void
io_thread_start_all()
{
    io_thread_s *thread = g_ctx->io_threads;
    struct timespec sleep = {0};
    struct timespec rem = {0};

    sleep.tv_nsec = 7800179; /* prime number between 7 and 8ms */
    while(thread) {
        thread->active = true;
        pthread_create(&thread->thread, NULL, io_thread_main, (void *)thread);
        if(thread->set_cpu_affinity) {
            if(pthread_setaffinity_np(thread->thread, sizeof(cpu_set_t), &thread->cpuset)) {
                LOG_NOARG(ERROR, "Failed to set thread CPU affinity\n");
                return;
            }
        }

        nanosleep(&sleep, &rem);
        thread = thread->next;
    }
}

void
io_thread_stop_all()
{
    io_thread_s *thread = g_ctx->io_threads;
    while(thread) {
        thread->active = false;
        thread = thread->next;
    }
    /* Wait for threads to be stopped */
    thread = g_ctx->io_threads;
    while(thread) {
        pthread_join(thread->thread, NULL);
        thread = thread->next;
    }
}
