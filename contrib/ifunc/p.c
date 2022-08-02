/**
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 */
/* For benchmarking ifunc-initiated ping-pong. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <ucp/api/ucp.h>


typedef struct pp_args {
    size_t counter;
    ucp_ep_h ep;
    ucp_ifunc_msg_t im;
    uint64_t heap_aslr;
    ucp_rkey_h rmt_rkey;
    ucs_status_t (*flush)(bool);
} pp_args_t;


size_t p_payload_bound(void *source_args, size_t source_args_size)
{
    (void)source_args;
    return source_args_size;
}


int p_payload_init(void *source_args,
                   size_t source_args_size,
                   void *payload,
                   size_t *payload_size)
{
    (void)source_args;
    (void)source_args_size;
    (void)payload;
    (void)payload_size;
    return 0;
}


void p_main(void *payload, size_t payload_size, void *target_args)
{
    (void)payload;
    (void)payload_size;

    pp_args_t *args = (pp_args_t*)target_args;

    ucp_ifunc_send_nbix(args->ep, args->im, args->heap_aslr, args->rmt_rkey);

    args->counter += 1;

    /*
     * Note that this function uses global variables on the calling process!
     *
     * It also calls ucp_worker_progress so this might not work when the main
     * function is executed by the UCX runtime.
     */
    args->flush(false);
}
