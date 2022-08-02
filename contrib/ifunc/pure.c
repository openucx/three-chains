/**
 * Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
 *
 * See file LICENSE for terms.
 */
#include <stddef.h>
#include <stdint.h>


// Target entry function.
typedef uint8_t (*target_entry_f)(uint8_t n, const float* f);


// To avoid GOT ref:
//  Global var cannot be used, static var works but non-const ones are empty?
//  Functions must be static.

static float f_gs = 3.141;
static const float f_gsc = 3.141;


static uint8_t xor(uint8_t* data, size_t data_size)
{
    uint8_t result = 0;
    for (size_t n = 0; n < 99; n++) {
        for (size_t i = 0; i < data_size; i++) {
            result ^= data[i];
        }
    }
    return result;
}


size_t pure_payload_bound(void *source_args, size_t source_args_size)
{
    (void)source_args;
    return source_args_size;
}


int pure_payload_init(void *source_args,
                      size_t source_args_size,
                      void *payload,
                      size_t *payload_size)
{
    (void)source_args_size;
    /* Don't use memcpy here. */
    for (size_t i = 0; i < *payload_size; i++) {
        ((char*)payload)[i] = ((char*)source_args)[i];
    }
    return 0;
}


void pure_main(void *payload, size_t payload_size, void *target_args)
{
    static float f_fs = 2.718;
    static const float f_fsc = 2.718;

    target_entry_f entry = (target_entry_f)target_args;
    const uint8_t ret = entry(xor(payload, payload_size), &f_gs);
    entry(ret, &f_fs);
    entry(ret, &f_gsc);
    entry(ret, &f_fsc);

    // Make sure the static variables can be modified.
    f_gs = -1.1;
    f_fs = -1.1;
    entry(ret, &f_gs);
    entry(ret, &f_fs);
}
