/*
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 */
#include <stdio.h>
#include <stdlib.h>

#include <dlfcn.h>

size_t foo_payload_bound(void *source_args, size_t source_args_size)
{
    (void)source_args;
    return source_args_size;
}


int foo_payload_init(void *source_args,
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

size_t fib(size_t n)
{
    if (n < 2) {
        return n;
    } else {
        return fib(n - 1) + fib(n - 2);
    }
}


void foo_main(void *payload, size_t payload_size, void *target_args)
{
    void* self_ptr = dlopen("", RTLD_NOW);

    size_t n = payload_size;
    //printf("n = %lu, ptr = %p, self = %p\n", n, payload, self_ptr);

    size_t ret = fib(n);

    *((size_t *)target_args) = ret;
}
