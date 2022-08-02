/*
 * Stops the ifunc polling loop. TODO: pure?
 * Author: Luis E. P.
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <ucp/api/ucp.h>

size_t result_payload_bound(void *source_args, size_t source_args_size)
{
	(void)source_args;
	return source_args_size;
}


int result_payload_init(void *source_args,
						size_t source_args_size,
						void *payload,
						size_t *payload_size)
{
    (void)source_args_size;
    (void)payload_size;

    uint64_t *result_src = (uint64_t *)source_args;
    uint64_t *result_pay = (uint64_t *)payload;

    *result_pay = *result_src;

    return 0;
}

// TODO: maybe make a more ellaborate struct with validity and stuff
void result_main(void *payload, size_t payload_size, void *target_args)
{
	(void)payload_size;

	uint64_t *result_pay = (uint64_t *)payload;
	uint64_t *result_tgt = (uint64_t *)target_args;

	*result_tgt = *result_pay;
}
