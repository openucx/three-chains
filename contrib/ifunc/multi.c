/*
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#include <zlib.h>           // -lz
#include <pthread.h>        // -pthread
#include <openssl/sha.h>    // -lcrypto (libssl-dev)

typedef struct thread_args {
    uint8_t* data;
    size_t size;
} thread_args_t;

const size_t max_args = 1UL << 27;


void* print_sha256(void* ptr)
{
    thread_args_t* args = (thread_args_t*)ptr;

    SHA256_CTX sha256;
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, args->data, args->size);
    SHA256_Final(hash, &sha256);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        printf("%02X", hash[i]);
    }
    printf("\n");
    sleep(4);

    return NULL;
}


size_t multi_payload_bound(void *source_args, size_t source_args_size)
{
    (void)source_args;

    printf("** Source args size        %lu\n", source_args_size);

    const size_t c_size = compressBound(source_args_size);

    printf("** Estimated payload size  %lu\n", c_size);

    return c_size;
}


int multi_payload_init(void *source_args,
                       size_t source_args_size,
                       void *payload,
                       size_t *payload_size)
{
    size_t c_size = *payload_size;
    int ret = compress2((Bytef*)payload, &c_size, (Bytef*)source_args, source_args_size,
                        Z_BEST_COMPRESSION);
    assert(ret == Z_OK);
    assert(c_size <= *payload_size);

    *payload_size = c_size;
    printf("** Compressed payload size %lu (%f%%)\n", c_size, (c_size * 100.0) / source_args_size);

    thread_args_t args;
    args.data = source_args;
    args.size = source_args_size;
    print_sha256(&args);

    return 0;
}


void multi_main(void *payload, size_t payload_size, void *target_args)
{
    (void)target_args;

    printf("** Compressed data size %lu\n", payload_size);

    size_t u_size = max_args;
    uint8_t* u_data = (uint8_t*)malloc(u_size);
    int ret = uncompress((Bytef*)u_data, &u_size, (Bytef*)payload, payload_size);
    assert(ret == Z_OK);
    printf("** Decompressed size    %lu\n", u_size);

    thread_args_t args;
    args.data = u_data;
    args.size = u_size;

    pthread_t pid;
    pthread_create(&pid, NULL, print_sha256, &args);
    pthread_setname_np(pid, "Hash thread");
    pthread_join(pid, NULL);

    free(u_data);
}
