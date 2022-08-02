/*
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>


float f_g                = 3.14;
const float f_gc         = 2.71;
static float f_gs        = 1.14;
static const float f_gsc = 1.73;

// Un-initialized empty array (.bss)
uint8_t array[8192];

typedef size_t (*foo_main_f)(size_t, void*);
typedef uint8_t (*target_entry_f)(uint8_t*, float);

typedef struct bar_target_args {
    target_entry_f target_entry;
    foo_main_f foo_main;
} bar_target_args_t;


uint8_t xor(uint8_t* data, size_t data_size)
{
    uint8_t result = 0;
    for (size_t i = 0; i < data_size; i++) {
        result ^= data[i];
    }
    return result;
}


void bar_main(double* f, void* target_args)
{
    static float f_fs = 6.02;
    static const float f_fsc = 1.66;

    printf("f_g = %f\n", f_g);
    printf("f_gc = %f\n", f_gc);
    printf("f_gs = %f\n", f_gs);
    printf("f_gsc = %f\n", f_gsc);
    printf("f_fs = %f\n", f_fs);
    printf("f_fsc = %f\n", f_fsc);

    bar_target_args_t* argsp = (bar_target_args_t*)target_args;

    *f += 1.0;
    const size_t ret = argsp->foo_main((int)(*f), f);
    printf("ret = %lu\n", ret);

    float len = sizeof(array);
    const uint8_t ret_t = argsp->target_entry(array, len);
    const uint8_t ret_l = xor(array, (size_t)len);

    assert(ret_t == ret_l);
}
