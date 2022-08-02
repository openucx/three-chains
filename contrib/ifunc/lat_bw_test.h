/*
 * Header file for latency-bw test
 * Author: Luis E. P.
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
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


typedef struct pc_tgt_args {
	size_t counter;
} pc_tgt_args_t;

typedef struct pc_src_args {
	size_t payload_size;
} pc_src_args_t;

ucs_status_t am_b(pc_src_args_t *src_args, pc_tgt_args_t *tgt_args);

#define AM_SIG_MAGIC 0x55

typedef uint8_t     am_sig_t;
typedef uint8_t		am_func_id;

typedef struct am_msg {
	am_func_id 	  id;
	pc_src_args_t src_args;
    size_t        frame_size;
    am_sig_t      sig;
} am_msg_t;