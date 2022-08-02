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

#include "pointer_chase.h"

size_t stop_payload_bound(void *source_args, size_t source_args_size)
{
	(void)source_args;
	return source_args_size;
}


int stop_payload_init(void *source_args,
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


void stop_main(void *payload, size_t payload_size, void *target_args)
{
	(void)payload_size;
	(void)payload;

	pc_tgt_args_t *tgt_args = (pc_tgt_args_t *)target_args;

	tgt_args->run = false;
}
