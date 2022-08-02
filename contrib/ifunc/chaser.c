/*
 * Recursive pointer-chasing ifunc 
 * Author: Luis E. P.
 *
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include <ucp/api/ucp.h>

#include "pointer_chase.h"

size_t chaser_payload_bound(void *source_args, size_t source_args_size)
{
	(void)source_args;
	return source_args_size;
}


int chaser_payload_init(void *source_args,
						size_t source_args_size,
						void *payload,
						size_t *payload_size)
{
	(void)source_args_size;
	(void)payload_size;

	pc_src_args_t *src = (pc_src_args_t *)source_args;
	pc_src_args_t *dst = (pc_src_args_t *)payload;

	dst->depth = src->depth;
	dst->addr  = src->addr;
	dst->dest  = src->dest;

	return 0;
}


void chaser_main(void *payload, size_t payload_size, void *target_args)
{
	(void)payload_size;

	pc_src_args_t *src_args = (pc_src_args_t *)payload;
	pc_tgt_args_t *tgt_args = (pc_tgt_args_t *)target_args;
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

	addr = addr % max_index;	// truncate large numbers
	int which_shard = addr / shard_size;

	if (which_shard == this_shard) {	// data is local
		addr = addr % shard_size;		// get address within a shard

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
			uint64_t *result_ptr;
			ucp_ifunc_msg_t result_msg = tgt_args->result_msg;
			ucp_ifunc_msg_get_payload_ptr(&result_msg, (void **)&result_ptr, NULL);

			*result_ptr = next;

			ucp_ifunc_send_nbix(tgt_args->ep[dest], tgt_args->result_msg,
				                heap_aslr, tgt_args->rmt_rkey[dest]);

			tgt_args->flush(tgt_args->wrkr, tgt_args->ep[dest]);

			return;
		}

		// Update values
		src_args->depth = depth;
		src_args->addr  = next;

		which_shard = next / shard_size;

		if (which_shard == this_shard) {
			chaser_main(payload, payload_size, target_args);
			return;
		}

	}

	// forward to other ep

	int destination = which_shard + 1; // because 0 is the client

	pc_src_args_t *msg_args;	// arguments of the message to be sent
	ucp_ifunc_msg_t i_msg = tgt_args->chaser_msg;

	void *tgt_payload;

    ucp_ifunc_msg_get_payload_ptr(&i_msg, &tgt_payload, NULL);
    
    msg_args = (pc_src_args_t *)tgt_payload;
    msg_args->depth = src_args->depth;
    msg_args->addr  = src_args->addr;
    msg_args->dest  = src_args->dest;

    if (VERBOSE) {
		printf("Remote addr:  %lu\n", msg_args->addr);
		printf("Remote dest:  %lu\n", msg_args->dest);
		printf("Remote depth: %lu\n", msg_args->depth);
	}

	heap_aslr = (uintptr_t)tgt_args->heap + tgt_args->aslr_diff[destination];

	ucp_ifunc_send_nbix(tgt_args->ep[destination], tgt_args->chaser_msg,
		                heap_aslr, tgt_args->rmt_rkey[destination]);
	
	tgt_args->flush(tgt_args->wrkr, tgt_args->ep[destination]);
}
