/*
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 */
#include <stdio.h>
#include <stdlib.h>

#include <omp.h>
#include <dlfcn.h>
#include <openssl/sha.h>


size_t fib(size_t n)
{
    if (n < 2) {
        return n;
    } else {
        return fib(n - 1) + fib(n - 2);
    }
}


size_t foo_main(size_t n, void* ptr)
{
    void* self_ptr = dlopen("", RTLD_NOW);

    printf("n = %lu, ptr = %p, self = %p\n", n, ptr, self_ptr);

    // This could also work if run.cpp is compiled with -fopenmp
    // But dlopen libomp.so inside this file won't work
    printf("omp_get_num_threads() -> %d\n", omp_get_num_threads());
    printf("omp_get_max_threads() -> %d\n", omp_get_max_threads());

    const size_t N = 1UL << 25;
    uint8_t* buf = malloc(N);

    // Let's try OpenMP!
    #pragma omp parallel for schedule(static) num_threads(4) default(none) shared(N, buf)
    for (size_t i = 0; i < N; i++) {
        buf[i] = rand() % 253;
    }

    SHA256_CTX sha256;
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, buf, N);
    SHA256_Final(hash, &sha256);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        printf("%02X", hash[i]);
    }
    printf("\n");

    free(buf);

    return fib(n);
}
