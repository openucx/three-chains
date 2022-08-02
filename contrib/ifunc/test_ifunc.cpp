/**
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 *
 */
// Minimal UCP application to test ifunc
// Single pair of workers & endpoints, single 'symmetric heap' & rmt_rkey
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


#define PORT 13337
#define HEAP_SIZE_LOG 31
#define HEAP_SIZE (1UL << HEAP_SIZE_LOG)
#define USE_LLVM 0

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
size_t prkey_sz, prkey_rmt_sz;
ucp_rkey_h rmt_rkey;

ucp_address_t* wrkr_addr;
ucp_address_t* wrkr_rmt_addr;
size_t wrkr_addr_sz, wrkr_rmt_addr_sz;

static char in_b[] = "b";
static char in_p[] = "p";
static char in_pure[] = "pure";
static char in_hello[] = "hello";
static char in_multi[] = "multi";

struct ucx_request {
    uint64_t completed;
};


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


uint8_t pure_ifunc_entry_f(uint8_t n, const float* f)
{
    std::cout << "Received n = " << (int)n << ", f = " << *f << '\n';
    return n + 1;
}


// For ifunc-initiated ping-pong
struct pp_args_t {
    size_t counter;
    ucp_ep_h ep;
    ucp_ifunc_msg_t im;
    uint64_t heap_aslr;
    ucp_rkey_h rmt_rkey;
    ucs_status_t (*flush)(bool);
};


void run_server()
{
    ucs_status_t s;

    // Server polls for pure ifunc
    do {
        s = ucp_poll_ifunc(ucp_ctx, heap, HEAP_SIZE, (void*)pure_ifunc_entry_f, wrkr);
    } while (s != UCS_OK);

    // Todo: extra worker progressing because the client wasn't flushing
    ucp_worker_progress(wrkr);
    socket_p2p_sync();

    // Server polls for hello ifunc
    do {
        s = ucp_poll_ifunc(ucp_ctx, heap, HEAP_SIZE, (void*)0xdeadbeef, wrkr);
    } while (s != UCS_OK);

    ucp_worker_progress(wrkr);
    socket_p2p_sync();

    // Server polls for multi ifunc
    // Building AArch64 bitcode fails on x64 if it uses OpenSSL, message will be discarded
    do {
        s = ucp_poll_ifunc(ucp_ctx, heap, HEAP_SIZE, nullptr, wrkr);
    } while ((s != UCS_OK) && (s != UCS_ERR_CANCELED));

    ucp_worker_progress(wrkr);
    socket_p2p_sync();

    std::cout << "\nBegin throughput benchmark\n";

    timespec t0, t1;

    volatile uint8_t* signal = (uint8_t*)heap + HEAP_SIZE - 1;

    for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
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
            if (i == n_warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            auto ptr = (uint8_t*)heap + ((i % n_slots) * slot_sz);
            do {
                s = ucp_poll_ifunc(ucp_ctx, ptr, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);

            if ((i + 1) % n_slots == 0) {
                ucp_put_nbi(ep,
                            const_cast<uint8_t*>(signal),
                            1,
                            ptr_to_u64_aslr(signal),
                            rmt_rkey);
                flush(false);
            }
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        assert(counter == n_iters + n_warmup);

        const double T = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;

        std::cout << std::setw(7) << (1UL << pl_sz_log) << ": "
                  << std::scientific << std::setprecision(3)
                  << (n_iters / T) << " messages/s\n";
    }

    socket_p2p_sync();

    std::cout << "\nBegin ping-pong benchmark\n";

    // IFUNCTODO: ifunc "b" is not registered as pure on the client to test
    // double registration detection on the server. Not useful once we get rid
    // of auto-registration.
    ucp_ifunc_h ih_b;
    ucp_ifunc_reg_param_t irp_b;
    irp_b.name = in_b;
    irp_b.pure = 0;
    irp_b.llvm_bc = USE_LLVM;
    ucp_register_ifunc(ucp_ctx, irp_b, &ih_b);

    ucp_ifunc_msg_t im_b;

    for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        size_t counter = 0;

        ucp_ifunc_msg_create(ih_b, NULL, 1UL << pl_sz_log, &im_b);

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            // Server receive first, then send.
            do {
                s = ucp_poll_ifunc(ucp_ctx, heap, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);

            ucp_ifunc_send_nbix(ep, im_b, ptr_to_u64_aslr(heap), rmt_rkey);

            // Flush is needed for inter-node!
            flush(false);
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        assert(counter == n_iters + n_warmup);

        const double T = (t1.tv_sec - t0.tv_sec) * 1e6 +
                         (t1.tv_nsec - t0.tv_nsec) / 1e3;

        std::cout << std::setw(7) << (1UL << pl_sz_log) << ": "
                  << std::scientific << std::setprecision(3)
                  << (T / n_iters) << " us\n";

        ucp_ifunc_msg_free(im_b);
    }

    // Intentionally forget to de-register ih_b to test auto-cleanup.

    socket_p2p_sync();

    std::cout << "\nBegin ping-pong(ifunc-initiated) benchmark\n";

    ucp_ifunc_h ih_p;
    ucp_ifunc_reg_param_t irp_p;
    irp_p.name = in_p;
    irp_p.pure = 0;
    irp_p.llvm_bc = USE_LLVM;
    ucp_register_ifunc(ucp_ctx, irp_p, &ih_p);

    pp_args_t pp_args;
    pp_args.ep = ep;
    pp_args.heap_aslr = ptr_to_u64_aslr(heap);
    pp_args.rmt_rkey = rmt_rkey;
    pp_args.flush = flush;

    for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        pp_args.counter = 0;

        ucp_ifunc_msg_create(ih_p, NULL, 1UL << pl_sz_log, &pp_args.im);

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            do {
                s = ucp_poll_ifunc(ucp_ctx, heap, slot_sz, &pp_args, wrkr);
            } while (s != UCS_OK);
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        assert(pp_args.counter == n_iters + n_warmup);

        const double T = (t1.tv_sec - t0.tv_sec) * 1e6 +
                         (t1.tv_nsec - t0.tv_nsec) / 1e3;

        std::cout << std::setw(7) << (1UL << pl_sz_log) << ": "
                  << std::scientific << std::setprecision(3)
                  << (T / n_iters) << " us\n";

        ucp_ifunc_msg_free(pp_args.im);
    }

    ucp_deregister_ifunc(ucp_ctx, ih_p);
}


void run_client()
{
    ucs_status_t s;

    // Client sends a pure ifunc
    ucp_ifunc_h ih_pure;
    ucp_ifunc_reg_param_t irp_pure;
    irp_pure.name = in_pure;
    irp_pure.pure = 1;
    irp_pure.llvm_bc = USE_LLVM;
    ucp_register_ifunc(ucp_ctx, irp_pure, &ih_pure);
    ucp_ifunc_msg_t im_pure;
    ucp_ifunc_msg_create(ih_pure, heap, 65536, &im_pure);

    std::cout << "\nSending the pure ifunc message, size = "
              << im_pure.frame_size << '\n';

    ucp_ifunc_send_nbix(ep, im_pure, ptr_to_u64_aslr(heap), rmt_rkey);
    flush(false);
    ucp_ifunc_msg_free(im_pure);

    ucp_deregister_ifunc(ucp_ctx, ih_pure);

    socket_p2p_sync();

    // Client sends a hello ifunc
    ucp_ifunc_h ih_hello;
    ucp_ifunc_reg_param_t irp_hello;
    irp_hello.name = in_hello;
    irp_hello.pure = 0;
    irp_hello.llvm_bc = USE_LLVM;
    ucp_register_ifunc(ucp_ctx, irp_hello, &ih_hello);
    ucp_ifunc_msg_t im_hello;
    ucp_ifunc_msg_create(ih_hello, heap, HEAP_SIZE, &im_hello);

    std::cout << "\nSending the hello ifunc message, size = "
              << im_hello.frame_size << '\n';

    ucp_ifunc_send_nbix(ep, im_hello, ptr_to_u64_aslr(heap), rmt_rkey);
    flush(false);
    ucp_ifunc_msg_free(im_hello);

    ucp_deregister_ifunc(ucp_ctx, ih_hello);

    socket_p2p_sync();

    // Client sends a multi library ifunc
    ucp_ifunc_h ih_multi;
    ucp_ifunc_reg_param_t irp_multi;
    irp_multi.name = in_multi;
    irp_multi.pure = 0;
    irp_multi.llvm_bc = USE_LLVM;
    ucp_register_ifunc(ucp_ctx, irp_multi, &ih_multi);

    std::cout << "\nPreparing the multi ifunc message\n";

    auto uncompressed = new uint8_t[1UL << 26]();
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<> dist(0, 15);
    for (size_t i = 0; i < (1UL << 26); i++) {
        uncompressed[i] = dist(gen);
    }

    ucp_ifunc_msg_t im_multi;
    ucp_ifunc_msg_create(ih_multi, uncompressed, 1UL << 26, &im_multi);

    delete [] uncompressed;

    std::cout << "\nSending the multi ifunc message, size = "
              << im_multi.frame_size << '\n';

    ucp_ifunc_send_nbix(ep, im_multi, ptr_to_u64_aslr(heap), rmt_rkey);
    flush(false);

    ucp_ifunc_msg_free(im_multi);

    ucp_deregister_ifunc(ucp_ctx, ih_multi);

    socket_p2p_sync();

    std::cout << "\nBegin throughput benchmark\n";

    timespec t0, t1;

    ucp_ifunc_h ih_b;
    ucp_ifunc_reg_param_t irp_b;
    irp_b.name = in_b;
    irp_b.pure = 0;
    irp_b.llvm_bc = USE_LLVM;
    ucp_register_ifunc(ucp_ctx, irp_b, &ih_b);

    ucp_ifunc_msg_t im_b;

    // Use end of the heap as signal, volatile prevents aggressive optimization
    volatile uint8_t* signal = (uint8_t*)heap + HEAP_SIZE - 1;

    // Payload size: 1B to 1MB
    for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_slots = (1UL << HEAP_SIZE_LOG) / slot_sz - 1;
        const size_t n_iters = std::max((size_t)75000, n_slots * 4);
        const size_t n_warmup = n_iters / 10;

        ucp_ifunc_msg_create(ih_b, NULL, 1UL << pl_sz_log, &im_b);

        *signal = 0;

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                flush(false);
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            auto ptr = (uint8_t*)heap + ((i % n_slots) * slot_sz);
            ucp_ifunc_send_nbix(ep,
                                im_b,
                                ptr_to_u64_aslr(ptr),
                                rmt_rkey);

            // Filled the entire ring buffer, flush and wait on the signal
            if ((i + 1) % n_slots == 0) {
                flush(false);
                while (*signal != 0x3C) {
                    ucp_worker_progress(wrkr);
                };
                *signal = 0;
            }
        }

        flush(false);
        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        const double T = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;

        std::cout << std::setw(7) << (1UL << pl_sz_log)
                  << "(" << std::setw(7) << im_b.frame_size << "): "
                  << std::scientific << std::setprecision(3)
                  << (n_iters / T) << " messages/s\n";

        ucp_ifunc_msg_free(im_b);
    }

    socket_p2p_sync();

    std::cout << "\nBegin ping-pong benchmark\n";

    for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        size_t counter = 0;

        ucp_ifunc_msg_create(ih_b, NULL, 1UL << pl_sz_log, &im_b);

        socket_p2p_sync();

        for (size_t i = 0; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            // Client send first, then recv.
            ucp_ifunc_send_nbix(ep, im_b, ptr_to_u64_aslr(heap), rmt_rkey);
            flush(false);

            do {
                s = ucp_poll_ifunc(ucp_ctx, heap, slot_sz, &counter, wrkr);
            } while (s != UCS_OK);
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        assert(counter == n_iters + n_warmup);

        const double T = (t1.tv_sec - t0.tv_sec) * 1e6 +
                         (t1.tv_nsec - t0.tv_nsec) / 1e3;

        std::cout << std::setw(7) << (1UL << pl_sz_log)
                  << "(" << std::setw(7) << im_b.frame_size << "): "
                  << std::scientific << std::setprecision(3)
                  << (T / n_iters) << " us\n";

        ucp_ifunc_msg_free(im_b);
    }

    ucp_deregister_ifunc(ucp_ctx, ih_b);

    socket_p2p_sync();

    std::cout << "\nBegin ping-pong(ifunc-initiated) benchmark\n";

    ucp_ifunc_h ih_p;
    ucp_ifunc_reg_param_t irp_p;
    irp_p.name = in_p;
    irp_p.pure = 0;
    irp_p.llvm_bc = USE_LLVM;
    ucp_register_ifunc(ucp_ctx, irp_p, &ih_p);

    pp_args_t pp_args;
    pp_args.ep = ep;
    pp_args.heap_aslr = ptr_to_u64_aslr(heap);
    pp_args.rmt_rkey = rmt_rkey;
    pp_args.flush = flush;

    for (int pl_sz_log = 0; pl_sz_log <= 20; pl_sz_log++) {
        // Assume sizeof(hdr + code + sig) <= 8 KiB
        const size_t slot_sz = std::max(1UL << 14, (1UL << pl_sz_log) + (1UL << 13));
        const size_t n_iters = pl_sz_log > 14 ? 30000 : 1000000;
        const size_t n_warmup = n_iters / 10;

        memset(heap, 42, slot_sz);

        // Pretend that we've already received a ping-pong ifunc (kick start).
        pp_args.counter = 1;

        ucp_ifunc_msg_create(ih_p, NULL, 1UL << pl_sz_log, &pp_args.im);

        socket_p2p_sync();

        // Client kicks start the ping-pong iterations.
        ucp_ifunc_send_nbix(ep, pp_args.im, pp_args.heap_aslr, pp_args.rmt_rkey);
        flush(false);

        for (size_t i = 1; i < n_iters + n_warmup; i++) {
            if (i == n_warmup) {
                clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
            }

            do {
                s = ucp_poll_ifunc(ucp_ctx, heap, slot_sz, &pp_args, wrkr);
            } while (s != UCS_OK);
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

        assert(pp_args.counter == n_iters + n_warmup);

        const double T = (t1.tv_sec - t0.tv_sec) * 1e6 +
                         (t1.tv_nsec - t0.tv_nsec) / 1e3;

        std::cout << std::setw(7) << (1UL << pl_sz_log)
                  << "(" << std::setw(7) << pp_args.im.frame_size << "): "
                  << std::scientific << std::setprecision(3)
                  << (T / n_iters) << " us\n";

        ucp_ifunc_msg_free(pp_args.im);
    }

    ucp_deregister_ifunc(ucp_ctx, ih_p);
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

    std::cout << "\nServer waiting for connection...\n\n";

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
    ucp_config_t* env_cfg;
    ucp_config_read(NULL, NULL, &env_cfg);
    // ucp_config_print(env_cfg, stdout, "UCX environment variables", UCS_CONFIG_PRINT_HEADER);
    // ucp_config_print(env_cfg, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);

    ucp_params_t params;

    params.field_mask = UCP_PARAM_FIELD_FEATURES     |
                        UCP_PARAM_FIELD_REQUEST_INIT |
                        UCP_PARAM_FIELD_REQUEST_SIZE |
                        UCP_PARAM_FIELD_REQUEST_CLEANUP;

    params.features = UCP_FEATURE_RMA;

    params.request_size = sizeof(ucx_request);
    params.request_init = request_init;
    params.request_cleanup = request_cleanup;

    ucp_init(&params, env_cfg, &ucp_ctx);

    ucp_config_release(env_cfg);

    // ucp_context_print_info(ucp_ctx, stdout);

    // Map memory
    ucp_mem_map_params_t mp;

    mp.field_mask = UCP_MEM_MAP_PARAM_FIELD_FLAGS |
                    UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    mp.length = HEAP_SIZE;
    mp.flags = UCP_MEM_MAP_ALLOCATE;

    ucp_mem_map(ucp_ctx, &mp, &mh);

    ucp_mem_attr_t ma;
    ma.field_mask = UCP_MEM_ATTR_FIELD_LENGTH |
                    UCP_MEM_ATTR_FIELD_ADDRESS;

    ucp_mem_query(mh, &ma);

    assert(ma.length >= HEAP_SIZE);

    std::cout << "Mapped 0x" << std::hex << ma.length << std::dec
              << " bytes of memory @ " << ma.address << "\n";
    heap = ma.address;

    // Setup a worker
    ucp_worker_params_t wrkr_params;

    wrkr_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    wrkr_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    ucp_worker_create(ucp_ctx, &wrkr_params, &wrkr);

    // ucp_worker_print_info(wrkr, stdout);

    ucp_worker_get_address(wrkr, &wrkr_addr, &wrkr_addr_sz);

    if (argc == 1) {    // Server
        memset(heap, 0, HEAP_SIZE);
        conn_skt = server_connect();
    } else {            // Client
        memset(heap, 17, HEAP_SIZE);
        conn_skt = client_connect(argv[1]);
    }

    find_aslr_diff();
    exchange_rkey();
    exchange_worker_addr();
    prepare_ep_rkey();
    flush(true);    // Required for wire up

    socket_p2p_sync();

    if (argc == 1) {    // Server
        run_server();
    } else {            // Client
        run_client();
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
