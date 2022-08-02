/*
 * Header file for pointer chasing app
 * Author: Luis E. P.
 *
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 *
 */

 // pointer-chasing specific defines

#include <ucp/api/ucp.h>

#define VERBOSE 0
#define CLIENT 0
#define SERVER 1 // index of first server

#define MODE_IFUNC_SO 0
#define MODE_IFUNC_BC 1
#define MODE_GETS	  2
#define MODE_AM       3

typedef struct pc_src_args {
	uint64_t depth;
	uint64_t addr;
	uint64_t dest;
} pc_src_args_t;

typedef struct pc_tgt_args {
	int             shard_id;
	uint64_t	   *data;
	ucp_ep_h 	   *ep;
	ucp_worker_h   wrkr;
	ucp_ifunc_msg_t	chaser_msg;
	ucp_ifunc_msg_t	result_msg;
	ucp_rkey_h 	   *rmt_rkey;
	ucs_status_t  (*flush)(ucp_worker_h,ucp_ep_h);
	ptrdiff_t      *aslr_diff;
	void           *heap;
	int             machine_qty;
	uint64_t        shard_size;
	bool            run;      // this variable can be used to break out of while
} pc_tgt_args_t;


// AM stuff
ucs_status_t am_chase(pc_src_args_t *src_args, pc_tgt_args_t *tgt_args);

#define AM_SIG_MAGIC 0x55

typedef uint8_t     am_sig_t;
typedef uint8_t		am_func_id;

typedef struct am_msg {
	am_func_id 	  id;
	pc_src_args_t src_args;
	size_t        frame_size;
    am_sig_t      sig;
} am_msg_t;

typedef struct result_msg {
	uint64_t result;
	am_sig_t sig;
} result_msg_t;
