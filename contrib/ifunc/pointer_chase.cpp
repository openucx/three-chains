/*
 * Dynamic adaptive pointer chasing application
 * 
 * Author: Luis E. P.
 *
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 */
 

/* How to run
Servers: ./pointer_chase.x
Client: ./pointer_chase <SERVER0> <SERVER1> <TEST_MODE> (see header file for TEST MODES)
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
#include <array>

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <ucp/api/ucp.h>

#include "pointer_chase.h"

#define PORT 13337
#define HEAP_SIZE_LOG 30
#define HOSTNAME_SIZE 16
#define HEAP_SIZE (1UL << HEAP_SIZE_LOG)
#define HALF_HEAP (1UL << (HEAP_SIZE_LOG-1))

#define ITERATIONS 8388608 //(2**23)
#define MAX_DEPTH_LOG 12
#define DEPTH 1000
#define RETRIES 10

int test_mode;
int use_llvm;
int g_argc;
char **g_argv;

// helps self-id
char hostname[HOSTNAME_SIZE];
int whoami;
int machine_qty; // servers + client
uint64_t datapoints; // max number of datapoints
uint64_t shard_size; // number of items per shard

in_addr_t *server_addrs;

int client_socket;
int* conn_skt;

void* heap;
void** heap_rmt;

ptrdiff_t *aslr_diff;

ucp_context_h ucp_ctx;
ucp_mem_h mh;
ucp_worker_h wrkr;
ucp_ep_h *ep;

void* prkey;
void** prkey_rmt;
size_t prkey_sz;
size_t *prkey_rmt_sz;
ucp_rkey_h *rmt_rkey;

ucp_address_t* wrkr_addr;
ucp_address_t** wrkr_rmt_addr;
size_t wrkr_addr_sz;
size_t *wrkr_rmt_addr_sz;


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
    pc_tgt_args_t *tgt_args = (pc_tgt_args_t *)target_args;

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
        status = UCS_ERR_CANCELED;
        *hdr_sig = 0;
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
            tgt_args->run = false;
            status = UCS_OK;
            break;
        case 1:
            status = am_chase(&(hdr->src_args), (pc_tgt_args_t *)target_args);
            break;
        default:
            printf("AM %u is not valid\n", hdr->id);
            status = UCS_ERR_CANCELED;
            break;
    }
err:
    return status;
}


// TODO: active message function. TOo lazy to play with Makefile to move to
// another file
ucs_status_t am_chase(pc_src_args_t *src_args, pc_tgt_args_t *tgt_args)
{
    ucs_status_t s;
    uint64_t heap_aslr;

    uint64_t depth = src_args->depth;
    uint64_t addr  = src_args->addr;
    uint64_t dest  = src_args->dest;

    uint64_t *data = tgt_args->data;
    int this_shard = tgt_args->shard_id;
    int server_qty = tgt_args->machine_qty - 1;
    int shard_size = tgt_args->shard_size;

    int max_index  = server_qty * shard_size;

    uint64_t next;

    if (VERBOSE) {
        printf("\nI'm going to chase these pointers!\n");
        printf("Depth: %lu\n", depth);
        printf("Addr:  %lu\n", addr);
        printf("Dest:  %lu\n", src_args->dest);
        printf("My shard ID is %d\n", this_shard);
    }

    addr = addr % max_index;    // truncate large numbers
    int which_shard = addr / shard_size;

    if (which_shard == this_shard) {    // data is local
        addr = addr % shard_size;       // get address within a shard

        next = data[addr];
        if (VERBOSE) {
            printf("Result is %lu\n", next);
        }

        // Trim down next to fit
        next = next % max_index;

        depth--;

        if (depth == 0) {
            if (VERBOSE) {
                printf("Reached max depth\n");
            }
            // write to original
            heap_aslr = (uintptr_t)tgt_args->heap + tgt_args->aslr_diff[dest];
            result_msg_t result_msg;
            result_msg.sig = AM_SIG_MAGIC;
            result_msg.result = next;

            s = ucp_put_nbi(tgt_args->ep[dest], &result_msg, sizeof(result_msg),
                            heap_aslr, tgt_args->rmt_rkey[dest]);
            if (s != UCS_OK) { // check if the operation completed immediately
                s = tgt_args->flush(tgt_args->wrkr, ep[dest]);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }
            
            return s;
        }

        // Update values
        src_args->depth = depth;
        src_args->addr  = next;

        which_shard = next / shard_size;

        if (VERBOSE) {
            printf("Next shard: %d\n", which_shard);
        }

        if (which_shard == this_shard) {
            s = am_chase(src_args, tgt_args);
            return s;
        }

    }

    // forward to other ep

    int destination = which_shard + 1; // because 0 is the client

    size_t am_frame_size;
    volatile am_sig_t *trailer_sig;

    am_frame_size = sizeof(am_msg_t) + sizeof(am_sig_t);

    am_msg_t am;
    am.id = 1;
    am.sig = AM_SIG_MAGIC;
    am.frame_size = am_frame_size;

    pc_src_args_t *msg_args = &(am.src_args); // arguments of the message to be sent
    
    trailer_sig = (volatile am_sig_t *)((char *)&am + am_frame_size - sizeof(am_sig_t));

    *trailer_sig = AM_SIG_MAGIC;

    msg_args->depth = src_args->depth;
    msg_args->addr  = src_args->addr;
    msg_args->dest  = src_args->dest;

    if (VERBOSE) {
        printf("Remote addr:  %lu\n", msg_args->addr);
        printf("Remote dest:  %lu\n", msg_args->dest);
        printf("Remote depth: %lu\n", msg_args->depth);
    }

    heap_aslr = (uintptr_t)tgt_args->heap + tgt_args->aslr_diff[destination];

    s = ucp_put_nbi(tgt_args->ep[destination], &am, am.frame_size,
                    heap_aslr, tgt_args->rmt_rkey[destination]);
    if (s != UCS_OK) { // check if the operation completed immediately
        s = tgt_args->flush(tgt_args->wrkr, ep[destination]);
        if (s != UCS_OK) { // check if there was an error flushing
            printf("Error flushing!\n");
        }
    }

    return s;
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


/* if flushing worker, please pass endpoint as NULL */
ucs_status_t flush(ucp_worker_h worker, ucp_ep_h endpoint)
{
    void* request;
    if (endpoint) {
        request = ucp_ep_flush_nb(endpoint, 0, flush_callback);
    } else {
        request = ucp_worker_flush_nb(worker, 0, flush_callback);
    }
    if (request == NULL) {
        return UCS_OK;
    } else if (UCS_PTR_IS_ERR(request)) {
        return UCS_PTR_STATUS(request);
    } else {
        ucs_status_t status;
        do {
            ucp_worker_progress(worker);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);
        ucp_request_free(request);
        return status;
    }
}


void socket_p2p_sync(int socket_id)
{
    ssize_t status;
    const int8_t send_var = 42;
    int8_t recv_var       = 0;

    status = send(conn_skt[socket_id], static_cast<const void*>(&send_var), sizeof(send_var), 0);
    assert(status == sizeof(send_var));

    status = recv(conn_skt[socket_id], static_cast<void*>(&recv_var), sizeof(recv_var), 0);
    assert(status == sizeof(send_var));

    assert(recv_var == 42);
}


template <typename T>
uint64_t ptr_to_u64_aslr(const T* ptr, int socket_id)
{
    static_assert(sizeof(ptr) == sizeof(uint64_t));
    auto ptr_u64 = reinterpret_cast<const uint64_t>(ptr);
    return static_cast<uint64_t>(ptr_u64 + aslr_diff[socket_id]);
}


void run_server_ifunc()
{   
    ucs_status_t s;
    uint64_t  dummy_number = 787;
    uint64_t *data = (uint64_t *)((uint8_t *)heap + HALF_HEAP);

    uint64_t *data_tmp = (uint64_t *)((uint8_t *)heap + HALF_HEAP)
                       + ((whoami-1) * shard_size);

    // copy data into same pointer
    for (uint64_t i = 0; i < shard_size; i++) {
        data[i] = data_tmp[i];
    }

    // Register chaser function
    ucp_ifunc_reg_param_t irp_chaser;
    ucp_ifunc_h ih_chaser;
    char in_chaser[] = "chaser";
    irp_chaser.name = in_chaser;
    irp_chaser.pure = 0;
    irp_chaser.llvm_bc = use_llvm;
    s = ucp_register_ifunc(ucp_ctx, irp_chaser, &ih_chaser);

    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error registering %s ifunc\n", in_chaser);
        return;
    }

    // result registration
    ucp_ifunc_h ih_result;
    ucp_ifunc_reg_param_t irp_result;
    char in_result[] = "result";
    irp_result.name = in_result;
    irp_result.pure = 0;
    irp_result.llvm_bc = use_llvm;
    s = ucp_register_ifunc(ucp_ctx, irp_result, &ih_result);

    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error registering %s ifunc\n", in_result);
        return;
    }

    // Create a chaser ifunc that can be used to be sent by main or ifuncs
    // chaser ifunc message source arguments
    pc_src_args_t pc_args;
    pc_args.depth = dummy_number;
    pc_args.addr  = dummy_number;
    pc_args.dest  = dummy_number;

    // main target args including ifunc to be sent
    pc_tgt_args_t tgt_args;
    tgt_args.shard_id = whoami-1; //client is whoami = 0
    tgt_args.machine_qty = machine_qty;
    tgt_args.shard_size  = shard_size;
    tgt_args.data     = data;
    tgt_args.ep       = ep;
    tgt_args.wrkr     = wrkr;
    tgt_args.rmt_rkey = rmt_rkey;
    tgt_args.flush    = flush;
    tgt_args.aslr_diff= aslr_diff;
    tgt_args.heap     = heap;

    // create chaser ifunc message
    s = ucp_ifunc_msg_create(ih_chaser, &pc_args, sizeof(pc_args), &tgt_args.chaser_msg);
    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error creating %s ifunc message\n", in_chaser);
    }

    // create result ifunc message
    s = ucp_ifunc_msg_create(ih_result, &dummy_number, sizeof(uint64_t), &tgt_args.result_msg);
    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error creating %s ifunc message\n", in_result);
    }

    // Sync
    socket_p2p_sync(CLIENT);

    // Start receiving ifuncs
    tgt_args.run = true;
    while(tgt_args.run) {
        do {
            s = ucp_poll_ifunc(ucp_ctx, heap, HALF_HEAP, &tgt_args, wrkr);
        } while (s != UCS_OK);
        // TODO: extra worker progressing because the client wasn't flushing. Maybe ok now?
        ucp_worker_progress(wrkr);
    }

    // Sync
    socket_p2p_sync(CLIENT);

    // cleanup
    ucp_ifunc_msg_free(tgt_args.chaser_msg);
    ucp_ifunc_msg_free(tgt_args.result_msg);
    ucp_deregister_ifunc(ucp_ctx, ih_chaser);
    ucp_deregister_ifunc(ucp_ctx, ih_result);
}


void run_client_ifunc()
{
    ucs_status_t s;
    uint64_t result = 747;
    uint64_t dummy_number = 737;

    int warmup, iterations;

    // chaser registration
    ucp_ifunc_h ih_chaser;
    ucp_ifunc_reg_param_t irp_chaser;
    char in_chaser[] = "chaser";
    irp_chaser.name = in_chaser;
    irp_chaser.pure = 0;
    irp_chaser.llvm_bc = use_llvm;
    s = ucp_register_ifunc(ucp_ctx, irp_chaser, &ih_chaser);

    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error registering %s ifunc\n", in_chaser);
    }

    // Register stop function
    ucp_ifunc_reg_param_t irp_stop;
    ucp_ifunc_h ih_stop;
    char in_stop[] ="stop";
    irp_stop.name = in_stop;
    irp_stop.pure = 0;
    irp_stop.llvm_bc = use_llvm;
    s = ucp_register_ifunc(ucp_ctx, irp_stop, &ih_stop);
    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error registering %s ifunc\n", in_stop);
    }
    
    pc_src_args_t pc_args;
    pc_args.addr  = dummy_number;
    pc_args.depth = dummy_number;
    pc_args.dest  = whoami;

    // create chaser ifunc
    ucp_ifunc_msg_t im_chaser;
    s = ucp_ifunc_msg_create(ih_chaser, &pc_args, sizeof(pc_args), &im_chaser);
    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error creating %s ifunc message\n", in_chaser);
    }

    // get pointer to actual payload of message
    void *payload;
    size_t payload_size;
    s = ucp_ifunc_msg_get_payload_ptr(&im_chaser, &payload, &payload_size);
    if (s != UCS_OK) {
        printf("Error getting payload pointer for %s\n", in_chaser);
    }

    pc_src_args_t *msg_args = (pc_src_args_t *)payload;

    // create stop ifunc
    ucp_ifunc_msg_t im_stop;
    s = ucp_ifunc_msg_create(ih_stop, NULL, sizeof(void *), &im_stop);
    if (s != UCS_OK) {
        // TODO: better error handling
        printf("Error creating %s ifunc message\n", in_stop);
    }

    // Sync
    for (int i = 1; i < machine_qty; i++) {
        socket_p2p_sync(i);
    }

    msg_args->depth = DEPTH;

    // Start pointer chase
    printf("\n=== testing ===\n");
    printf("Depth: %lu\n", msg_args->depth);
    printf("Message size: %lu\n", im_chaser.frame_size);
    printf("Payload size: %lu\n", payload_size);
    for (int j = 0; j < 20; j++) {
        msg_args->addr  = j;        // TODO: how to generate number?
        
        printf("[%2lu]: ", msg_args->addr);

        // Send to first server
        s = ucp_ifunc_send_nbix(ep[SERVER], im_chaser, ptr_to_u64_aslr(heap, SERVER),
                                rmt_rkey[SERVER]);

        if (s != UCS_OK) { // check if the operation completed immediately
            s = flush(wrkr, ep[SERVER]);
            if (s != UCS_OK) { // check if there was an error flushing
                printf("Error flushing!\n");
            }
        }

        // await for result
        do {
                s = ucp_poll_ifunc(ucp_ctx, heap, HALF_HEAP, &result, wrkr);
        } while (s != UCS_OK);
        ucp_worker_progress(wrkr); // TODO: necessary?

        printf("%2lu\n", result);
    }


    printf("\n=== benchmarking ===\n");
    timespec t0, t1;

    printf("Doing depth sweep powers of 2 from 1 to %ld\n", (1UL << MAX_DEPTH_LOG));

    msg_args->depth = 1;
    for (int p = 0; p <= MAX_DEPTH_LOG; p++) {

        iterations = ITERATIONS/msg_args->depth;
        warmup = iterations/16;
        for (int j = 0; j < warmup + iterations; j++) {
            msg_args->addr  = j;        // TODO: how to generate number?
            if (j == warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            // Send to first server
            s = ucp_ifunc_send_nbix(ep[SERVER], im_chaser, ptr_to_u64_aslr(heap, SERVER),
                                    rmt_rkey[SERVER]);

            if (s != UCS_OK) { // check if the operation completed immediately
                s = flush(wrkr, ep[SERVER]);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }

            // await for result
            do {
                s = ucp_poll_ifunc(ucp_ctx, heap, HALF_HEAP, &result, wrkr);
            } while (s != UCS_OK);
            ucp_worker_progress(wrkr); // TODO: necessary?
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        const double T = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;
        
        std::cout << hostname << ",";
        std::cout << g_argv[1];

        for (int m = 2; m < g_argc; m++) {
            std::cout << "-" << g_argv[m];
        }

        std::cout << ",ifunc_" << use_llvm << ","
                  << im_chaser.frame_size  << ","
                  << payload_size          << ","
                  << iterations            << ","
                  << msg_args->depth       << ","
                  << datapoints            << ","
                  << machine_qty-1         << ","
                  << std::scientific       << std::setprecision(3)
                  << (iterations / T)      << "\n"; // messages/s\n";

        // increment depth
        msg_args->depth = msg_args->depth * 2;        
    }

    // stop servers
    for (int i = 1; i < machine_qty; i++) {
        s = ucp_ifunc_send_nbix(ep[i], im_stop, ptr_to_u64_aslr(heap, i), rmt_rkey[i]);

        if (s != UCS_OK) { // check if the operation completed immediately
            s = flush(wrkr, ep[i]);
            if (s != UCS_OK) { // check if there was an error flushing
                printf("Error flushing!\n");
            }
        }
    }

    // Sync
    for (int i = 1; i < machine_qty; i++) {
        socket_p2p_sync(i);
    }

    // cleanup
    ucp_ifunc_msg_free(im_chaser);
    ucp_ifunc_msg_free(im_stop);
    ucp_deregister_ifunc(ucp_ctx, ih_chaser);
    ucp_deregister_ifunc(ucp_ctx, ih_stop);
}


void run_server_am()
{
    ucs_status_t s;
    uint64_t *data = (uint64_t *)((uint8_t *)heap + HALF_HEAP);

    uint64_t *data_tmp = (uint64_t *)((uint8_t *)heap + HALF_HEAP)
                       + ((whoami-1) * shard_size);

    // copy data into same pointer
    for (uint64_t i = 0; i < shard_size; i++) {
        data[i] = data_tmp[i];
    }

    // main target args including ifunc to be sent
    pc_tgt_args_t tgt_args;
    tgt_args.shard_id    = whoami-1; //client is whoami = 0
    tgt_args.machine_qty = machine_qty;
    tgt_args.shard_size  = shard_size;
    tgt_args.data        = data;
    tgt_args.ep          = ep;
    tgt_args.wrkr        = wrkr;
    tgt_args.rmt_rkey    = rmt_rkey;
    tgt_args.flush       = flush;
    tgt_args.aslr_diff   = aslr_diff;
    tgt_args.heap        = heap;

    // Sync
    socket_p2p_sync(CLIENT);

    // Start receiving am
    tgt_args.run = true;
    while(tgt_args.run) {
        do {
            s = poll_am(heap, HALF_HEAP, &tgt_args, wrkr);
        } while (s != UCS_OK);
        // Todo: extra worker progressing because the client wasn't flushing. Maybe ok now?
        ucp_worker_progress(wrkr);
    }

    // Sync
    socket_p2p_sync(CLIENT);
}


void run_client_am()
{
    ucs_status_t s;
    uint64_t result = 747;
    uint64_t dummy_number = 737;

    int warmup, iterations;

    // Define AM

    am_msg_t *am;
    size_t am_frame_size;
    volatile am_sig_t *trailer_sig;

    // The payload is included in the am_msg_t structure
    am_frame_size = sizeof(am_msg_t) + sizeof(am_sig_t);
    am = (am_msg_t *)malloc(am_frame_size);

    am->id = 1;
    am->sig = AM_SIG_MAGIC;
    am->frame_size = am_frame_size;

    pc_src_args_t *msg_args = &(am->src_args);
    trailer_sig = (volatile am_sig_t *)((char *)am + am_frame_size - sizeof(am_sig_t));

    *trailer_sig = AM_SIG_MAGIC;

    msg_args->addr  = dummy_number;
    msg_args->depth = dummy_number;
    msg_args->dest  = whoami;

    result_msg_t *result_msg = (result_msg_t *)heap;
    result_msg->result = result;
    volatile uint8_t *result_sig = (uint8_t *)&(result_msg->sig);

    *result_sig = 0;

    // Sync
    for (int i = 1; i < machine_qty; i++) {
        socket_p2p_sync(i);
    }

    msg_args->depth = DEPTH;

    // Start pointer chase
    printf("\n=== testing ===\n");
    printf("Depth: %lu\n", msg_args->depth);
    for (int j = 0; j < 20; j++) {
        msg_args->addr = j;        // TODO: how to generate number?
        
        printf("[%2lu]: ", msg_args->addr);

        // Send to first server
        s = ucp_put_nbi(ep[SERVER], am, am->frame_size, ptr_to_u64_aslr(heap, SERVER),
                        rmt_rkey[SERVER]);

        if (s != UCS_OK) { // check if the operation completed immediately
            s = flush(wrkr, ep[SERVER]);
            if (s != UCS_OK) { // check if there was an error flushing
                printf("Error flushing!\n");
            }
        }

        // await for result
        while(*result_sig != AM_SIG_MAGIC) {
            ucp_worker_progress(wrkr);
        }

        *result_sig = 0;

        printf("%2lu\n", result_msg->result);
    }

    printf("\n=== benchmarking ===\n");
    timespec t0, t1;

    printf("Doing depth sweep powers of 2 from 1 to %ld\n", (1UL << MAX_DEPTH_LOG));

    msg_args->depth = 1;
    for (int p = 0; p <= MAX_DEPTH_LOG; p++) {

        iterations = ITERATIONS/msg_args->depth;
        warmup = iterations/16;
        for (int j = 0; j < warmup + iterations; j++) {
            msg_args->addr  = j;        // TODO: how to generate number?
            if (j == warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            // Send to first server
            s = ucp_put_nbi(ep[SERVER], am, am->frame_size, ptr_to_u64_aslr(heap, SERVER),
                            rmt_rkey[SERVER]);
            
            if (s != UCS_OK) { // check if the operation completed immediately
                s = flush(wrkr, ep[SERVER]);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }

            // await for result
            while(*result_sig != AM_SIG_MAGIC) {
                ucp_worker_progress(wrkr);
            }

            *result_sig = 0;
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        const double T = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;

        std::cout << hostname << ",";
        std::cout << g_argv[1];

        for (int m = 2; m < g_argc; m++) {
            std::cout << "-" << g_argv[m];
        }

        std::cout << ",am"                 << ","
                  << am->frame_size        << ","
                  << sizeof(pc_src_args_t) << ","
                  << iterations            << ","
                  << msg_args->depth       << ","
                  << datapoints            << ","
                  << machine_qty-1         << ","
                  << std::scientific       << std::setprecision(3)
                  << (iterations / T)      << "\n"; // messages/s\n";

        // increment depth
        msg_args->depth = msg_args->depth * 2;        
    }

    // stop servers
    for (int i = 1; i < machine_qty; i++) {
        // ID 0 shuts down server
        am->id = 0;
        s = ucp_put_nbi(ep[i], am, am->frame_size, ptr_to_u64_aslr(heap, i),
                        rmt_rkey[i]);
        if (s != UCS_OK) { // check if the operation completed immediately
            s = flush(wrkr, ep[SERVER]);
            if (s != UCS_OK) { // check if there was an error flushing
                printf("Error flushing!\n");
            }
        }
    }

    free(am);

    // Sync
    for (int i = 1; i < machine_qty; i++) {
        socket_p2p_sync(i);
    }
}


void run_server_gets()
{   
    volatile uint8_t *run = (uint8_t *)heap + sizeof(uint64_t);
    uint64_t *data = (uint64_t *)((uint8_t *)heap + HALF_HEAP);

    uint64_t *data_tmp = (uint64_t *)((uint8_t *)heap + HALF_HEAP)
                       + ((whoami-1) * shard_size);

    // run as long as 123
    *run = 123;

    // copy data into same pointer
    for (uint64_t i = 0; i < shard_size; i++) {
        data[i] = data_tmp[i];
    }

    // ready to accept

    socket_p2p_sync(CLIENT);

    // start receiving gets
    while (*run == 123) {
        ucp_worker_progress(wrkr);
    }
    ucp_worker_progress(wrkr); // TODO: needed?
    socket_p2p_sync(CLIENT);
    // 

}


void run_client_gets()
{
    ucs_status_t s; //TODO should be checking codes
    uint64_t addr;
    uint64_t depth;
    uint64_t next;

    int which_shard;
    int destination;
    int warmup, iterations;

    volatile uint64_t *result = (uint64_t *)heap;
    volatile uint8_t *run = (uint8_t *)heap + sizeof(uint64_t);
    uint64_t *data = (uint64_t *)((uint8_t *)heap + HALF_HEAP);

    *run = 7; // stop signal
    *result = 777;

    depth = DEPTH;

    // Sync
    for (int i = 1; i < machine_qty; i++) {
        socket_p2p_sync(i);
    }

    printf("\n=== testing ===\n");
    printf("Depth: %lu\n", depth);
    // Start pointer chase
    for (int j = 0; j < 20; j++) {
        addr  = j;        // TODO: how to generate number?

        printf("[%2lu]: ", addr);

        for (uint64_t k = 0; k < depth; k++) {
            addr = addr % datapoints;
            which_shard = addr / shard_size;
            destination = which_shard + 1; // client is 0
            addr = addr % shard_size;

            uint64_t *data_ptr = data + addr;

            s = ucp_get_nbi(ep[destination], const_cast<uint64_t*>(result),
                            sizeof(uint64_t), ptr_to_u64_aslr(data_ptr, destination),
                            rmt_rkey[destination]);
            // TODO: Warning, it is not clear if UCS_OK means the get is completed
            if (s != UCS_OK) { // check if the operation completed immediately
                s = flush(wrkr, ep[destination]);
                if (s != UCS_OK) { // check if there was an error flushing
                    printf("Error flushing!\n");
                }
            }

            next = *result;
            addr = next;
        }

        printf("%2lu\n", next);
    }

    printf("\n=== benchmarking ===\n");
    timespec t0, t1;

    printf("Doing depth sweep powers of 2 from 1 to %ld\n", (1UL << MAX_DEPTH_LOG));

    depth = 1;
    for (int p = 0; p <= MAX_DEPTH_LOG; p++) {
        iterations = ITERATIONS/depth; // 10,000,000
        warmup = iterations/16;

        for (int j = 0; j < warmup + iterations; j++) {
            addr  = j;        // TODO: how to generate number?
            if (j == warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            for (uint64_t k = 0; k < depth; k++) {
                addr = addr % datapoints;
                which_shard = addr / shard_size;
                destination = which_shard + 1; // client is 0
                addr = addr % shard_size;

                uint64_t *data_ptr = data + addr;

                s = ucp_get_nbi(ep[destination], const_cast<uint64_t*>(result),
                                sizeof(uint64_t), ptr_to_u64_aslr(data_ptr, destination),
                                rmt_rkey[destination]);
                // TODO: check for status of get UCS_OK, ERR if not flush
                // TODO: Warning, it is not clear if UCS_OK means the get is completed
                if (s != UCS_OK) { // check if the operation completed immediately
                    s = flush(wrkr, ep[destination]);
                    if (s != UCS_OK) { // check if there was an error flushing
                        printf("Error flushing!\n");
                    }
                }

                next = *result;
                addr = next;
            }
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        const double T = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;

        std::cout << hostname << ",";
        std::cout << g_argv[1];

        for (int m = 2; m < g_argc; m++) {
            std::cout << "-" << g_argv[m];
        }

        std::cout << ",gets"           << ","
                  << sizeof(uint64_t)  << ","
                  << sizeof(uint64_t)  << ","
                  << iterations        << ","
                  << depth             << ","
                  << datapoints        << ","
                  << machine_qty-1     << ","
                  << std::scientific   << std::setprecision(3)
                  << (iterations / T)  << "\n"; //messages/s\n";

        // increment depth
        depth = depth * 2;
    }
    // stop servers
    for (int i = 1; i < machine_qty; i++) {
        s = ucp_put_nbi(ep[i], const_cast<uint8_t*>(run), 1,
                        ptr_to_u64_aslr(run, i), rmt_rkey[i]);
        if (s != UCS_OK) { // check if the operation completed immediately
            s = flush(wrkr, ep[i]);
            if (s != UCS_OK) { // check if there was an error flushing
                printf("Error flushing!\n");
            }
        }
    }

    // Sync
    for (int i = 1; i < machine_qty; i++) {
        socket_p2p_sync(i);
    }

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


int client_connect(const char* server_name, int node_id)
{
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    hostent* he = gethostbyname(server_name);

    sockaddr_in addr;

    addr.sin_port = htons(PORT);
    addr.sin_family = he->h_addrtype;

    memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

    memcpy(&(addr.sin_addr), he->h_addr_list[0], he->h_length);

    // save the list of addresses to send over later
    server_addrs[node_id] = addr.sin_addr.s_addr;

    int ret = connect(client_fd, (sockaddr*)&addr, sizeof(addr));
    assert(ret == 0);

    return client_fd;
}


int send_server_info(int node_id)
{
    ssize_t nbytes, server_buff_size;

    // send server ID
    nbytes = send(conn_skt[node_id], &node_id, sizeof(whoami), 0);
    assert(nbytes == sizeof(whoami));

    // send test_mode
    nbytes = send(conn_skt[node_id], &test_mode, sizeof(test_mode), 0);
    assert(nbytes == sizeof(test_mode));

    // send number of machines
    nbytes = send(conn_skt[node_id], &machine_qty, sizeof(machine_qty), 0);
    assert(nbytes == sizeof(machine_qty));

    server_buff_size = machine_qty * sizeof(in_addr_t);
    nbytes = send(conn_skt[node_id], server_addrs, server_buff_size, 0);
    assert(nbytes == server_buff_size);

    return 0;
}


int recv_server_info()
{
    ssize_t nbytes, server_buff_size;

    // receive my server ID
    nbytes = recv(client_socket, &whoami, sizeof(whoami), 0);
    assert(nbytes == sizeof(whoami));

    // receive test mode
    nbytes = recv(client_socket, &test_mode, sizeof(test_mode), 0);
    assert(nbytes == sizeof(test_mode));

    // recevie number of servers
    nbytes = recv(client_socket, &machine_qty, sizeof(machine_qty), 0);
    assert(nbytes == sizeof(machine_qty));

    // allocate space for server address list
    server_addrs = (in_addr_t *)malloc(machine_qty * sizeof(in_addr_t));

    server_buff_size = machine_qty * sizeof(in_addr_t);
    nbytes = recv(client_socket, server_addrs, server_buff_size, 0);
    assert(nbytes == server_buff_size);

    return 0;
}


int listen_server(int conn_qty)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    int optval = 1;

    if (conn_qty == 0) {
        return 0;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

    int ret = bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    assert(ret == 0);

    listen(server_fd, 0);

    // listen for the number of connections requested
    for (int i = 0; i < conn_qty; i++) {
        int connected_fd = accept(server_fd, NULL, NULL);

        // get who just connected
        int other_side;
        ssize_t nbytes;

        nbytes = recv(connected_fd, &other_side, sizeof(whoami), 0);
        assert(nbytes == sizeof(whoami));

        conn_skt[other_side] = connected_fd;
        printf("%d connected to me\n", other_side);
    }
    close(server_fd);

    return 0;
}


int connect_server(int node_id)
{
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr;

    addr.sin_port = htons(PORT);
    addr.sin_family = AF_INET;

    memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

    memcpy(&(addr.sin_addr), &server_addrs[node_id], sizeof(in_addr_t));

    int ret = connect(client_fd, (sockaddr*)&addr, sizeof(addr));
    // try to connect, the server might not be listening yet
    for (int i = 0; i < RETRIES && (ret != 0); i++) {
        printf("Couldn't connect, retrying %d more times...\n", (RETRIES-i));
        sleep(1);
        ret = connect(client_fd, (sockaddr*)&addr, sizeof(addr));
    }
    assert(ret == 0);

    // send who I am
    ssize_t nbytes;
    nbytes = send(client_fd, &whoami, sizeof(whoami), 0);
    assert(nbytes == sizeof(whoami));

    conn_skt[node_id] = client_fd;
    printf("Connected to %d\n", node_id);

    return 0;
}


void find_aslr_diff(int socket_id)
{
    ssize_t nbytes;

    nbytes = send(conn_skt[socket_id], &heap, sizeof(heap), 0);
    assert(nbytes == sizeof(heap));

    nbytes = recv(conn_skt[socket_id], &heap_rmt[socket_id], sizeof(heap_rmt[socket_id]), 0);
    assert(nbytes == sizeof(heap_rmt[socket_id]));

    // TODO: why do we do uint8_t here? bit width of ptrdiff_t "is not less than 17"
    aslr_diff[socket_id] = static_cast<uint8_t*>(heap_rmt[socket_id]) - static_cast<uint8_t*>(heap);

    std::cout << "ASLR difference: " << std::hex;
    if (aslr_diff[socket_id] >= 0) {
        std::cout << "0x" << aslr_diff[socket_id];
    } else {
        std::cout << "-0x" << -aslr_diff[socket_id];
    }
    std::cout << std::dec << '\n';
}


void exchange_rkey(int socket_id)
{
    const ucs_status_t s = ucp_rkey_pack(ucp_ctx, mh, &prkey, &prkey_sz);
    assert(s == UCS_OK);

    ssize_t nbytes;

    nbytes = send(conn_skt[socket_id], &prkey_sz, sizeof(prkey_sz), 0);
    assert(nbytes == sizeof(prkey_sz));

    nbytes = recv(conn_skt[socket_id], &prkey_rmt_sz[socket_id], sizeof(prkey_rmt_sz[socket_id]), 0);
    assert(nbytes == sizeof(prkey_rmt_sz[socket_id]));

    prkey_rmt[socket_id] = malloc(prkey_rmt_sz[socket_id]);
    assert(prkey_rmt[socket_id] != nullptr);

    socket_p2p_sync(socket_id);

    nbytes = send(conn_skt[socket_id], prkey, prkey_sz, 0);
    assert(nbytes == static_cast<ssize_t>(prkey_sz));

    nbytes = recv(conn_skt[socket_id], prkey_rmt[socket_id], prkey_rmt_sz[socket_id], 0);
    assert(nbytes == static_cast<ssize_t>(prkey_rmt_sz[socket_id]));

    std::cout << "Local prkey " << prkey_sz << " bytes, remote prkey "
              << prkey_rmt_sz[socket_id] << " bytes\n";
}


void exchange_worker_addr(int socket_id)
{
    ssize_t nbytes;

    nbytes = send(conn_skt[socket_id], &wrkr_addr_sz, sizeof(wrkr_addr_sz), 0);
    assert(nbytes == sizeof(wrkr_addr_sz));

    nbytes = send(conn_skt[socket_id], wrkr_addr, wrkr_addr_sz, 0);
    assert(nbytes == static_cast<ssize_t>(wrkr_addr_sz));

    nbytes = recv(conn_skt[socket_id], &wrkr_rmt_addr_sz[socket_id],
                  sizeof(wrkr_rmt_addr_sz[socket_id]), 0);
    assert(nbytes == sizeof(wrkr_rmt_addr_sz[socket_id]));

    wrkr_rmt_addr[socket_id] = (ucp_address_t*)malloc(wrkr_rmt_addr_sz[socket_id]);

    nbytes = recv(conn_skt[socket_id], wrkr_rmt_addr[socket_id],
                  wrkr_rmt_addr_sz[socket_id], 0);
    assert(nbytes == static_cast<ssize_t>(wrkr_rmt_addr_sz[socket_id]));

    std::cout << "Local worker address " << wrkr_addr_sz
              << " bytes, remote worker address " << wrkr_rmt_addr_sz[socket_id]
              << " bytes\n";
}


void prepare_ep_rkey(int socket_id)
{
    ucs_status_t s;
    ucp_ep_params_t ep_params;

    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address = wrkr_rmt_addr[socket_id];

    s = ucp_ep_create(wrkr, &ep_params, &ep[socket_id]);
    assert(s == UCS_OK);
    s = ucp_ep_rkey_unpack(ep[socket_id], prkey_rmt[socket_id], &rmt_rkey[socket_id]);
    assert(s == UCS_OK);
}


int main(int argc, char** argv)
{
    // Need to be used elsewhere
    g_argc = argc-1; // remove test-mode argument
    g_argv = argv;

    ucs_status_t s;

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

    // TODO: need to pass a power of 2 server qty (server needs 0)
    if ((argc-1) != 0 &&    // for servers
        (argc-2) != 1 &&
        (argc-2) != 2 &&
        (argc-2) != 4 &&
        (argc-2) != 8 &&
        (argc-2) != 16 &&
        (argc-2) != 32) {
        printf("NEEDS TO BE POWER OF TWO!\n");
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
    if (argc == 1) {    // Server
        gethostname(hostname, HOSTNAME_SIZE);
        hostname[HOSTNAME_SIZE-1] = '\0';
        memset(heap, 0, HEAP_SIZE);
        // first connection is to client
        client_socket = server_connect();
        printf("Client connected\n");
        recv_server_info();
        // Allocate space for all sockets
        conn_skt = (int *)malloc(machine_qty * sizeof(int));
        conn_skt[CLIENT] = client_socket;
        socket_p2p_sync(CLIENT);
    } else {            // Client
        gethostname(hostname, HOSTNAME_SIZE);
        hostname[HOSTNAME_SIZE-1] = '\0';
        whoami = 0;
        test_mode = atoi(argv[argc-1]);
        machine_qty = argc-1;
        memset(heap, 17, HEAP_SIZE);

        // allocate space for the server addresses // TODO: add error checking
        server_addrs = (in_addr_t *)malloc(machine_qty * sizeof(in_addr_t));
        // Allocate space for all sockets
        conn_skt = (int *)malloc(machine_qty * sizeof(int));

        // connect to all servers
        for (int i = 1; i < machine_qty; i++) {
            conn_skt[i] = client_connect(argv[i], i);
            printf("Connected to %d\n", i);
        }

        for (int i = 1; i < machine_qty; i++) {
            send_server_info(i);
        }

        // sync all servers with client
        for (int i = 1; i < machine_qty; i++) {
            socket_p2p_sync(i);
        }
    }

    printf("I am %d\n", whoami);

    // do connections between severs
    if (whoami > 0) {
        // listen for smaller than whoami
        printf("Listening %d times\n", (whoami - 1));
        listen_server(whoami - 1);
        // connect to larger than whoami
        for (int i = (whoami + 1); i < machine_qty; i++){
            printf("Conecting...\n");
            connect_server(i);
        }
    }

    // sync
    if (whoami == 0) {
        for (int i = 1; i < machine_qty; i++) {
            socket_p2p_sync(i);
        }
    } else {
        socket_p2p_sync(CLIENT);
    }

    // allocate structures dependent on number of connections
    heap_rmt = (void **)malloc(machine_qty * sizeof(void *));
    aslr_diff = (ptrdiff_t *)malloc(machine_qty * sizeof(ptrdiff_t));
    ep = (ucp_ep_h *)malloc(machine_qty * sizeof(ucp_ep_h));
    prkey_rmt = (void **)malloc(machine_qty * sizeof(void *));
    prkey_rmt_sz = (size_t *)malloc(machine_qty * sizeof(size_t));
    rmt_rkey = (ucp_rkey_h *)malloc(machine_qty * sizeof(ucp_rkey_h));
    wrkr_rmt_addr = (ucp_address_t **)malloc(machine_qty * sizeof(ucp_address_t *));
    wrkr_rmt_addr_sz = (size_t *)malloc(machine_qty * sizeof(size_t));

    // Setup remote stuff
    for (int i = 0; i < machine_qty; i++) {
        if (whoami != i) {
            find_aslr_diff(i);
            exchange_rkey(i);
            exchange_worker_addr(i);
            prepare_ep_rkey(i);
        }
    }

    // sync
    if (whoami == 0) {
        for (int i = 1; i < machine_qty; i++) {
            socket_p2p_sync(i);
        }
    } else {
        socket_p2p_sync(CLIENT);
    }

    flush(wrkr, NULL);    // Required for wire up

    // Data lives on the second half of the heap
    datapoints = HALF_HEAP/sizeof(uint64_t);

    int server_qty = machine_qty - 1;
    shard_size = datapoints / server_qty;

    uint64_t *deita = (uint64_t *)((uint8_t *)heap + HALF_HEAP);

    printf("Generating %lu datapoints...\n", datapoints);
    for (uint64_t i = 0; i < datapoints; i++) {
        deita[i] = i;
    }

    unsigned seed = 777;
    uint64_t tmp;
    uint64_t idx;

    printf("Randomizing datapoints using seed %u...\n", seed);
    std::default_random_engine g(seed);
    // inspired from https://www.cplusplus.com/reference/algorithm/shuffle/
    for (uint64_t i = 0; i < datapoints; i++) {
        std::uniform_int_distribution<uint64_t> d(0,i);
        idx = d(g); 
        tmp = deita[i];
        deita[i] = deita[idx];
        deita[idx] = tmp;
    }

    printf("Randomization completed with first datapoint: %lu\n", *deita);

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
            case MODE_GETS:
                run_client_gets();
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
            case MODE_GETS:
                run_server_gets();
                break;
            case MODE_AM:
                run_server_am();
                break;
            default:
                printf("Please select a valid TEST_MODE\n");
                break;
        }
    }

    // destroy remote stuff
    for (int i = 0; i < machine_qty; i++) {
        if (whoami != i) {
            ucp_rkey_destroy(rmt_rkey[i]);
            free(wrkr_rmt_addr[i]);
            free(prkey_rmt[i]);
        }
    }

    ucp_worker_release_address(wrkr, wrkr_addr);
    ucp_worker_destroy(wrkr);
    ucp_rkey_buffer_release(prkey);
    ucp_mem_unmap(ucp_ctx, mh);
    ucp_cleanup(ucp_ctx);

    // free up structures
    free(heap_rmt);
    free(aslr_diff);
    free(ep);
    free(prkey_rmt);
    free(prkey_rmt_sz);
    free(rmt_rkey);
    free(wrkr_rmt_addr);
    free(wrkr_rmt_addr_sz);

    // close socket connections
    for (int i = 0; i < machine_qty; i++) {
        if (whoami != i) {
            close(conn_skt[i]);
        }
    }

    free(server_addrs);
    free(conn_skt);
}
