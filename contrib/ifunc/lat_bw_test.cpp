/*
 * Latency and bandwidth test
 *
 * Author: Luis E. P.
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 * Based on Wenbin Lu's work
 *
 */

/* How to run
Server: ./lat_bw_test.x
Client: ./lat_bw_test.x <SERVER> <TEST_MODE>
*/

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <ctime>
#include <random>
#include <unistd.h>

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <ucp/api/ucp.h>
#include "lat_bw_test.h"


#define PORT 13337
#define HOSTNAME_SIZE 16
#define HEAP_SIZE_LOG 31
#define HEAP_SIZE (1UL << HEAP_SIZE_LOG)

int test_mode;
int use_llvm;
char **g_argv;

char hostname[HOSTNAME_SIZE];
int whoami;

int conn_skt;

void* heap;
void* heap_rmt;
ptrdiff_t aslr_diff;

ucp_context_h ucp_ctx;
ucp_mem_h mh;
ucp_worker_h wrkr;
ucp_ep_h ep;

void* prkey;
void* prkey_rmt;
size_t prkey_sz;
size_t prkey_rmt_sz;
ucp_rkey_h rmt_rkey;

ucp_address_t* wrkr_addr;
ucp_address_t* wrkr_rmt_addr;
size_t wrkr_addr_sz;
size_t wrkr_rmt_addr_sz;

struct ucx_request {
    uint64_t completed;
};


// Arch-specific barriers
static inline void barrier() {
#if defined(__x86_64__)
    asm volatile(""::: "memory");
#elif defined(__aarch64__)
    asm volatile ("dmb ishld" ::: "memory");
#else
#  error "Unsupported architecture"
#endif
}


ucs_status_t poll_am(void *buffer,
                     size_t buffer_size,
                     void *target_args,
                     ucp_worker_h worker)
{
    am_msg_t *hdr;
    volatile am_sig_t *hdr_sig;
    volatile am_sig_t *trailer_sig;
    ucs_status_t status;

    hdr = (am_msg_t *)buffer;
    hdr_sig = &(hdr->sig);

    if (*hdr_sig != AM_SIG_MAGIC) {
        status = UCS_ERR_NO_MESSAGE;
        ucp_worker_progress(worker);
        goto err;
    }

    /* Claim this active message to be handled by us, no matter the result. */
    *hdr_sig = 0;

    /* Required to read hdr correctly, volatile isn't enough */
    barrier();

    /* IFUNCTODO: buffer_size is used here only */
    if (ucs_unlikely(buffer_size < sizeof(am_msg_t))) {
        printf("AM %u rejected, message too long: %ld > %lu\n",
                 hdr->id, sizeof(am_msg_t), buffer_size);
        status = UCS_ERR_CANCELED;;
        goto err;
    }

    trailer_sig = (volatile am_sig_t *)((char *)buffer + hdr->frame_size - sizeof(am_sig_t));

    // wiat for rest of message to arrive
    if (*trailer_sig != AM_SIG_MAGIC) {
        ucp_worker_progress(worker);
    }

    *trailer_sig = 0;

    /* Required to read hdr correctly, volatile isn't enough */
    barrier();

    if (VERBOSE) {
        printf("AM executing %u\n", hdr->id);
    }

    // Determine which AM to invoke (TODO: can be imrpvoed)
    switch(hdr->id)
    {
        // shutdown server
        case 0:
            status = UCS_OK;
            break;
        case 1:
            status = am_b(&(hdr->src_args), (pc_tgt_args_t *)target_args);
            break;
        default:
            printf("AM %u is not valid\n", hdr->id);
            status = UCS_ERR_CANCELED;
            break;
    }
err:
    return status;
}

ucs_status_t am_b(pc_src_args_t *src_args, pc_tgt_args_t *tgt_args)
{
    (void)src_args;
    *((size_t *)tgt_args) += 1;
    return UCS_OK;
}


void request_init(void* request)
{
    auto r = (ucx_request*) request;
    r->completed = 0;
}


void request_cleanup(void* request)
{
    auto r       = static_cast<ucx_request*>(request);
    r->completed = 1;
}


void flush_callback(void *request, ucs_status_t status)
{
    (void)request;
    (void)status;
}


ucs_status_t flush(bool flush_worker)
{
    void* request;
    if (flush_worker) {
        request = ucp_worker_flush_nb(wrkr, 0, flush_callback);
    } else {
        request = ucp_ep_flush_nb(ep, 0, flush_callback);
    }
    if (request == NULL) {
        return UCS_OK;
    } else if (UCS_PTR_IS_ERR(request)) {
        return UCS_PTR_STATUS(request);
    } else {
        ucs_status_t status;
        do {
            ucp_worker_progress(wrkr);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);
        ucp_request_free(request);
        return status;
    }
}


void socket_p2p_sync()
{
    ssize_t status;
    const int8_t send_var = 42;
    int8_t recv_var       = 0;

    status = send(conn_skt, static_cast<const void*>(&send_var), sizeof(send_var), 0);
    assert(status == sizeof(send_var));

    status = recv(conn_skt, static_cast<void*>(&recv_var), sizeof(recv_var), 0);
    assert(status == sizeof(send_var));

    assert(recv_var == 42);
}


template <typename T>
uint64_t ptr_to_u64_aslr(const T* ptr)
{
    static_assert(sizeof(ptr) == sizeof(uint64_t));
    auto ptr_u64 = reinterpret_cast<const uint64_t>(ptr);
    return static_cast<uint64_t>(ptr_u64 + aslr_diff);
}


void run_server_ifunc()
{
    ucs_status_t s;

    printf("\n=== benchmarking throughput ===\n");

    volatile uint8_t *signal = (uint8_t *)heap + HEAP_SIZE - 1;

    //for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        //  payload <= 8 KiB -> slot size = 16 KiB
        //  payload > 8 KiB  -> slot size = payload + 8 KiB
        //  This is the same for all 3 tests, for both the server and the client
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        // Leave the last slot for signal
        const size_t n_slots = (1UL << HEAP_SIZE_LOG) / slot_sz - 1;
        const size_t n_iters = std::max((size_t)75000, n_slots * 4);
        const size_t n_warmup = n_iters / 10;

        size_t counter = 0;

        // Erase the buffer
        memset(heap, 42, HEAP_SIZE);
        *signal = 0x3C;

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {

            auto ptr = (uint8_t*)heap + ((i % n_slots) * slot_sz);
            do {
                s = ucp_poll_ifunc(ucp_ctx, ptr, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);

            if ((i + 1) % n_slots == 0) {
                s = ucp_put_nbi(ep, const_cast<uint8_t*>(signal), 1,
                                ptr_to_u64_aslr(signal), rmt_rkey);
                if (s != UCS_OK) { // check if the put completed immediately
                    s = flush(false);
                    if (s != UCS_OK) {
                        printf("Error flushing!\n");
                    }
                }
            }
        }

        assert(counter == n_iters + n_warmup);
    }

    socket_p2p_sync();

    printf("\n=== benchmarking latency ===\n");

    // IFUNCTODO: this function might have been registered in previous step
    ucp_ifunc_reg_param_t irp_b;
    ucp_ifunc_h ih_b;
    char in_b[] = "b";
    irp_b.name = in_b;
    irp_b.pure = 0;
    irp_b.llvm_bc = use_llvm;
    s = ucp_register_ifunc(ucp_ctx, irp_b, &ih_b);

    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error registering %s ifunc\n", in_b);
        return;
    }

    ucp_ifunc_msg_t im_b;

    //for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        size_t counter = 0;

        s = ucp_ifunc_msg_create(ih_b, NULL, 1UL << pl_sz_log, &im_b);
        if (s != UCS_OK) {
            // TODO: better error handling
            printf("Error creating %s ifunc message\n", in_b);
        }

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {

            // Server receives first, then sends
            do {
                s = ucp_poll_ifunc(ucp_ctx, heap, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);

            s = ucp_ifunc_send_nbix(ep, im_b, ptr_to_u64_aslr(heap), rmt_rkey);
            if (s != UCS_OK) { // check if the operation completed immediately
                s = flush(false);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }
        }

        assert(counter == n_iters + n_warmup);

        ucp_ifunc_msg_free(im_b);
    }

    ucp_deregister_ifunc(ucp_ctx, ih_b);

    socket_p2p_sync();
}


void run_client_ifunc()
{
    ucs_status_t s;

    // Register "b" ifunc
    ucp_ifunc_reg_param_t irp_b;
    ucp_ifunc_h ih_b;
    char in_b[] = "b";
    irp_b.name = in_b;
    irp_b.pure = 0;
    irp_b.llvm_bc = use_llvm;
    s = ucp_register_ifunc(ucp_ctx, irp_b, &ih_b);

    printf("\n=== benchmarking throughput ===\n");

    timespec t0, t1;

    ucp_ifunc_msg_t im_b;

    // Use end of the heap as signal, volatile prevents aggressive optimization
    volatile uint8_t *signal = (uint8_t *)heap + HEAP_SIZE - 1;

    // Payload size: 1B to 1MB
    // for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_slots = (1UL << HEAP_SIZE_LOG) / slot_sz - 1;
        const size_t n_iters = std::max((size_t)75000, n_slots * 4);
        const size_t n_warmup = n_iters / 10;

        s = ucp_ifunc_msg_create(ih_b, NULL, 1UL << pl_sz_log, &im_b);
        if (s != UCS_OK) {
            // TODO: better error handling
            printf("Error creating %s ifunc message\n", in_b);
        }

        *signal = 0;

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                s = flush(false);
                if (s != UCS_OK) {
                    printf("Error flushing!\n");
                }
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            auto ptr = (uint8_t*)heap + ((i % n_slots) * slot_sz);
            s = ucp_ifunc_send_nbix(ep, im_b, ptr_to_u64_aslr(ptr), rmt_rkey);

            // Filled the entire ring buffer, flush and wait on the signal
            if ((i + 1) % n_slots == 0) {
                s = flush(false);
                if (s != UCS_OK) {
                    printf("Error flushing!\n");
                }

                while (*signal != 0x3C) {
                    ucp_worker_progress(wrkr);
                };

                *signal = 0;
            }
        }

        s = flush(false);
        if (s != UCS_OK) {
            printf("Error flushing!\n");
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        const double T = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;

        std::cout << hostname << ",";
        std::cout << g_argv[1];

        std::cout << ",ifunc_" << use_llvm << ","
                  << im_b.frame_size       << ","
                  << (1UL << pl_sz_log)    << ","
                  << std::scientific       << std::setprecision(3)
                  << (n_iters / T)         << "\n"; // messages/s\n";

        ucp_ifunc_msg_free(im_b);
    }

    socket_p2p_sync();

    printf("\n=== benchmarking latency ===\n");

    //for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        size_t counter = 0;

        s = ucp_ifunc_msg_create(ih_b, NULL, 1UL << pl_sz_log, &im_b);
        if (s != UCS_OK) {
            // TODO: better error handling
            printf("Error creating %s ifunc message\n", in_b);
        }

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            // Client send first, then recv.
            s = ucp_ifunc_send_nbix(ep, im_b, ptr_to_u64_aslr(heap), rmt_rkey);
            if (s != UCS_OK) { // check if the operation completed immediately
                s = flush(false);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }

            do {
                s = ucp_poll_ifunc(ucp_ctx, heap, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        assert(counter == n_iters + n_warmup);

        const double T = (t1.tv_sec - t0.tv_sec) * 1e6 +
                         (t1.tv_nsec - t0.tv_nsec) / 1e3;

        std::cout << hostname << ",";
        std::cout << g_argv[1];

        std::cout << ",ifunc_" << use_llvm << ","
                  << im_b.frame_size       << ","
                  << (1UL << pl_sz_log)    << ","
                  << std::scientific       << std::setprecision(3)
                  << (T / (n_iters * 2))   << "\n"; // us\n";
                    // Reporting half-roundtrip latency

        ucp_ifunc_msg_free(im_b);
    }

    ucp_deregister_ifunc(ucp_ctx, ih_b);

    socket_p2p_sync();
}


void run_server_am()
{
    ucs_status_t s;

    printf("\n=== benchmarking throughput ===\n");

    volatile uint8_t *signal = (uint8_t *)heap + HEAP_SIZE - 1;

    //for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {        
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        //  payload <= 8 KiB -> slot size = 16 KiB
        //  payload > 8 KiB  -> slot size = payload + 8 KiB
        //  This is the same for all 3 tests, for both the server and the client
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        // Leave the last slot for signal
        const size_t n_slots = (1UL << HEAP_SIZE_LOG) / slot_sz - 1;
        const size_t n_iters = std::max((size_t)75000, n_slots * 4);
        const size_t n_warmup = n_iters / 10;

        size_t counter = 0;

        // Erase the buffer
        memset(heap, 42, HEAP_SIZE);
        *signal = 0x3C;

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {

            auto ptr = (uint8_t*)heap + ((i % n_slots) * slot_sz);
            do {
                s = poll_am(ptr, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);

            if ((i + 1) % n_slots == 0) {
                s = ucp_put_nbi(ep, const_cast<uint8_t*>(signal), 1,
                                ptr_to_u64_aslr(signal), rmt_rkey);
                if (s != UCS_OK) { // check if the put completed immediately
                    s = flush(false);
                    if (s != UCS_OK) {
                        printf("Error flushing!\n");
                    }
                }
            }
        }

        assert(counter == n_iters + n_warmup);
    }

    socket_p2p_sync();

    am_msg_t *am;
    size_t am_frame_size;
    volatile am_sig_t *trailer_sig;

    printf("\n=== benchmarking latency ===\n");

    //for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        size_t counter = 0;

        // The payload is not included in the am_msg_t structure
        am_frame_size = sizeof(am_msg_t) + (1UL << pl_sz_log) + sizeof(am_sig_t);
        am = (am_msg_t *)malloc(am_frame_size);

        am->id = 1;
        am->sig = AM_SIG_MAGIC;
        am->frame_size = am_frame_size;

        trailer_sig = (volatile am_sig_t *)((char *)am + am_frame_size - sizeof(am_sig_t));

        *trailer_sig = AM_SIG_MAGIC;

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {

            // Server receives first, then sends
            do {
                s = poll_am(heap, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);

            s = ucp_put_nbi(ep, am, am_frame_size, ptr_to_u64_aslr(heap), rmt_rkey);
            if (s != UCS_OK) { // check if the operation completed immediately
                s = flush(false);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }
        }

        assert(counter == n_iters + n_warmup);

        free(am);
    }

    socket_p2p_sync();
}


void run_client_am()
{
    ucs_status_t s;

    am_msg_t *am;
    size_t am_frame_size;
    volatile am_sig_t* trailer_sig;

    printf("\n=== benchmarking throughput ===\n");

    timespec t0, t1;


    // Use end of the heap as signal, volatile prevents aggressive optimization
    volatile uint8_t *signal = (uint8_t *)heap + HEAP_SIZE - 1;

    // Payload size: 1B to 1MB
    //for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_slots = (1UL << HEAP_SIZE_LOG) / slot_sz - 1;
        const size_t n_iters = std::max((size_t)75000, n_slots * 4);
        const size_t n_warmup = n_iters / 10;

        *signal = 0;

        am_frame_size = sizeof(am_msg_t) + (1UL << pl_sz_log) + sizeof(am_sig_t);
        am = (am_msg_t *)malloc(am_frame_size);

        am->id = 1;
        am->sig = AM_SIG_MAGIC;
        am->frame_size = am_frame_size;

        trailer_sig = (volatile am_sig_t *)((char *)am + am_frame_size - sizeof(am_sig_t));

        *trailer_sig = AM_SIG_MAGIC;

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                s = flush(false);
                if (s != UCS_OK) {
                    printf("Error flushing!\n");
                }
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            auto ptr = (uint8_t*)heap + ((i % n_slots) * slot_sz);

            // Put size equivalent to ifunc payload
            s = ucp_put_nbi(ep, am, am->frame_size, ptr_to_u64_aslr(ptr), rmt_rkey);

            // Filled the entire ring buffer, flush and wait on the signal
            if ((i + 1) % n_slots == 0) {
                s = flush(false);
                if (s != UCS_OK) {
                    printf("Error flushing!\n");
                }

                while (*signal != 0x3C) {
                    ucp_worker_progress(wrkr);
                };

                *signal = 0;
            }
        }

        s = flush(false);
        if (s != UCS_OK) {
            printf("Error flushing!\n");
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        const double T = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;

        std::cout << hostname << ",";
        std::cout << g_argv[1];

        std::cout << ",am"                                   << ","
                  << (sizeof(am_msg_t) + (1UL << pl_sz_log)) << ","
                  << (1UL << pl_sz_log)                      << ","
                  << std::scientific                         << std::setprecision(3)
                  << (n_iters / T)                           << "\n"; // messages/s\n";

        free(am);
    }

    socket_p2p_sync();

    printf("\n=== benchmarking latency ===\n");

    //for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
    for (int pl_sz_log = 0; pl_sz_log <= 0; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        size_t counter = 0;

        am_frame_size = sizeof(am_msg_t) + (1UL << pl_sz_log) + sizeof(am_sig_t);
        am = (am_msg_t *)malloc(am_frame_size);

        am->id = 1;
        am->sig = AM_SIG_MAGIC;
        am->frame_size = am_frame_size;

        trailer_sig = (volatile am_sig_t *)((char *)am + am_frame_size - sizeof(am_sig_t));

        *trailer_sig = AM_SIG_MAGIC;

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            // Client send first, then recv.
            s = ucp_put_nbi(ep, am, am->frame_size, ptr_to_u64_aslr(heap), rmt_rkey);
            if (s != UCS_OK) { // check if the operation completed immediately
                s = flush(false);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }

            do {
                s = poll_am(heap, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        assert(counter == n_iters + n_warmup);

        const double T = (t1.tv_sec - t0.tv_sec) * 1e6 +
                         (t1.tv_nsec - t0.tv_nsec) / 1e3;

        std::cout << hostname << ",";
        std::cout << g_argv[1];

        std::cout << ",am"                                   << ","
                  << (sizeof(am_msg_t) + (1UL << pl_sz_log)) << ","
                  << (1UL << pl_sz_log)                      << ","
                  << std::scientific                         << std::setprecision(3)
                  << (T / (n_iters * 2))                     << "\n"; // us\n";
                    // Reporting half-roundtrip latency

        free(am);
    }

    socket_p2p_sync();
}


int server_connect()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    int optval = 1;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

    int ret = bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    assert(ret == 0);

    listen(server_fd, 0);

    printf("\nServer waiting for connection...\n\n");

    int connected_fd = accept(server_fd, NULL, NULL);

    close(server_fd);

    return connected_fd;
}


int client_connect(const char* server_name)
{
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    hostent* he = gethostbyname(server_name);

    sockaddr_in addr;

    addr.sin_port = htons(PORT);
    addr.sin_family = he->h_addrtype;

    memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

    memcpy(&(addr.sin_addr), he->h_addr_list[0], he->h_length);

    int ret = connect(client_fd, (sockaddr*)&addr, sizeof(addr));
    assert(ret == 0);

    return client_fd;
}

int send_server_info()
{
    ssize_t nbytes;

    // send test_mode
    nbytes = send(conn_skt, &test_mode, sizeof(test_mode), 0);
    assert(nbytes == sizeof(test_mode));

    return 0;
}

int recv_server_info()
{
    ssize_t nbytes;

    // receive test mode
    nbytes = recv(conn_skt, &test_mode, sizeof(test_mode), 0);
    assert(nbytes == sizeof(test_mode));

    return 0;
}


void find_aslr_diff()
{
    ssize_t nbytes;

    nbytes = send(conn_skt, &heap, sizeof(heap), 0);
    assert(nbytes == sizeof(heap));

    nbytes = recv(conn_skt, &heap_rmt, sizeof(heap_rmt), 0);
    assert(nbytes == sizeof(heap_rmt));

    // TODO: why do we do uint8_t here? bit width of ptrdiff_t "is not less than 17"
    aslr_diff = static_cast<uint8_t*>(heap_rmt) - static_cast<uint8_t*>(heap);

    std::cout << "ASLR difference: " << std::hex;
    if (aslr_diff >= 0) {
        std::cout << "0x" << aslr_diff;
    } else {
        std::cout << "-0x" << -aslr_diff;
    }
    std::cout << std::dec << '\n';
}


void exchange_rkey()
{
    const ucs_status_t s = ucp_rkey_pack(ucp_ctx, mh, &prkey, &prkey_sz);
    assert(s == UCS_OK);

    ssize_t nbytes;

    nbytes = send(conn_skt, &prkey_sz, sizeof(prkey_sz), 0);
    assert(nbytes == sizeof(prkey_sz));

    nbytes = recv(conn_skt, &prkey_rmt_sz, sizeof(prkey_rmt_sz), 0);
    assert(nbytes == sizeof(prkey_rmt_sz));

    prkey_rmt = malloc(prkey_rmt_sz);
    assert(prkey_rmt != nullptr);

    socket_p2p_sync();

    nbytes = send(conn_skt, prkey, prkey_sz, 0);
    assert(nbytes == static_cast<ssize_t>(prkey_sz));

    nbytes = recv(conn_skt, prkey_rmt, prkey_rmt_sz, 0);
    assert(nbytes == static_cast<ssize_t>(prkey_rmt_sz));

    std::cout << "Local prkey " << prkey_sz << " bytes, remote prkey "
              << prkey_rmt_sz << " bytes\n";
}


void exchange_worker_addr()
{
    ssize_t nbytes;

    nbytes = send(conn_skt, &wrkr_addr_sz, sizeof(wrkr_addr_sz), 0);
    assert(nbytes == sizeof(wrkr_addr_sz));

    nbytes = send(conn_skt, wrkr_addr, wrkr_addr_sz, 0);
    assert(nbytes == static_cast<ssize_t>(wrkr_addr_sz));

    nbytes = recv(conn_skt, &wrkr_rmt_addr_sz, sizeof(wrkr_rmt_addr_sz), 0);
    assert(nbytes == sizeof(wrkr_rmt_addr_sz));

    wrkr_rmt_addr = (ucp_address_t*)malloc(wrkr_rmt_addr_sz);

    nbytes = recv(conn_skt, wrkr_rmt_addr, wrkr_rmt_addr_sz, 0);
    assert(nbytes == static_cast<ssize_t>(wrkr_rmt_addr_sz));

    std::cout << "Local worker address " << wrkr_addr_sz
              << " bytes, remote worker address " << wrkr_rmt_addr_sz
              << " bytes\n";
}


void prepare_ep_rkey()
{
    ucs_status_t s;
    ucp_ep_params_t ep_params;

    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address = wrkr_rmt_addr;

    s = ucp_ep_create(wrkr, &ep_params, &ep);
    assert(s == UCS_OK);
    s = ucp_ep_rkey_unpack(ep, prkey_rmt, &rmt_rkey);
    assert(s == UCS_OK);
}


int main(int argc, char** argv)
{
    ucs_status_t s;

    g_argv = argv;

    ucp_config_t* env_cfg;
    s = ucp_config_read(NULL, NULL, &env_cfg);
    if (s != UCS_OK) {
        printf("Error reading UCP configuration\n");
        return -1;
    }

    ucp_params_t params;

    params.field_mask = UCP_PARAM_FIELD_FEATURES     |
                        UCP_PARAM_FIELD_REQUEST_INIT |
                        UCP_PARAM_FIELD_REQUEST_SIZE |
                        UCP_PARAM_FIELD_REQUEST_CLEANUP;

    params.features = UCP_FEATURE_RMA;

    params.request_size = sizeof(ucx_request);
    params.request_init = request_init;
    params.request_cleanup = request_cleanup;

    if ((argc-1) != 0 && (argc-1) != 2) {
        printf("Need to pass either 0 or 2 args!\n");
        return -1;
    }

    s = ucp_init(&params, env_cfg, &ucp_ctx);
    if (s != UCS_OK) {
        printf("Error initalizing UCP\n");
        return -1;
    }

    ucp_config_release(env_cfg);

    // ucp_context_print_info(ucp_ctx, stdout);

    // Map memory
    ucp_mem_map_params_t mp;

    mp.field_mask = UCP_MEM_MAP_PARAM_FIELD_FLAGS |
                    UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    mp.length = HEAP_SIZE;
    mp.flags = UCP_MEM_MAP_ALLOCATE;

    s = ucp_mem_map(ucp_ctx, &mp, &mh);
    if (s != UCS_OK) {
        printf("Error UCP mem-mapping\n");
        return -1;
    }

    ucp_mem_attr_t ma;
    ma.field_mask = UCP_MEM_ATTR_FIELD_LENGTH |
                    UCP_MEM_ATTR_FIELD_ADDRESS;

    s = ucp_mem_query(mh, &ma);
    if (s != UCS_OK) {
        printf("Error querying UCP memory\n");
        return -1;
    }

    assert(ma.length >= HEAP_SIZE);

    std::cout << "Mapped 0x" << std::hex << ma.length << std::dec
              << " bytes of memory @ " << ma.address << "\n";
    heap = ma.address;

    // Setup a worker
    ucp_worker_params_t wrkr_params;

    wrkr_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    wrkr_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    s = ucp_worker_create(ucp_ctx, &wrkr_params, &wrkr);
    if (s != UCS_OK) {
        printf("Error creating worker\n");
    }

    // ucp_worker_print_info(wrkr, stdout);

    s = ucp_worker_get_address(wrkr, &wrkr_addr, &wrkr_addr_sz);
    if (s != UCS_OK) {
        printf("Error getting worker address\n");
    }

    // Determine who the client is
    if (argc == 1) { // server
        gethostname(hostname, HOSTNAME_SIZE);
        hostname[HOSTNAME_SIZE-1] = '\0';
        whoami = 1;
        memset(heap, 0, HEAP_SIZE);
        conn_skt = server_connect();
        printf("Client connected\n");
        recv_server_info();
    } else {
        gethostname(hostname, HOSTNAME_SIZE);
        hostname[HOSTNAME_SIZE-1] = '\0';
        whoami = 0;
        test_mode = atoi(argv[argc-1]);
        memset(heap, 17, HEAP_SIZE);
        conn_skt = client_connect(argv[1]);
        printf("Connected to server\n");
        send_server_info();
    }

    socket_p2p_sync();

    printf("I am %d\n", whoami);

    find_aslr_diff();
    exchange_rkey();
    exchange_worker_addr();
    prepare_ep_rkey();
    flush(true);    // Required for wire up

    socket_p2p_sync();

    printf("=== Initialization done ===\n\n");

    if (test_mode == MODE_IFUNC_BC) {
        use_llvm = 1;
    } else {
        use_llvm = 0;
    }

    if(whoami == 0) { // client
        switch(test_mode)
        {
            case MODE_IFUNC_SO:
            case MODE_IFUNC_BC:
                run_client_ifunc();
                break;
            case MODE_AM:
                run_client_am();
                break;
            default:
                printf("Please select a valid TEST_MODE\n");
                break;
        }
    } else { // server
        switch(test_mode)
        {
            case MODE_IFUNC_SO:
            case MODE_IFUNC_BC:
                run_server_ifunc();
                break;
            case MODE_AM:
                run_server_am();
                break;
            default:
                printf("Please select a valid TEST_MODE\n");
                break;
        }
    }    

    ucp_rkey_destroy(rmt_rkey);
    free(wrkr_rmt_addr);
    free(prkey_rmt);
    ucp_worker_release_address(wrkr, wrkr_addr);
    ucp_worker_destroy(wrkr);
    ucp_rkey_buffer_release(prkey);
    ucp_mem_unmap(ucp_ctx, mh);
    ucp_cleanup(ucp_ctx);

    close(conn_skt);
}
