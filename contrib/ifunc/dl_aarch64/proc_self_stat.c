// Read /proc//proc/self/stat
// man proc > log
// Copyright (C) 2019-2022 Arm Ltd. All rights reserved.
#include <stdio.h>
#include <stdint.h>

int main()
{
    FILE* f = fopen("/proc/self/stat", "r");

    int ret;
    uint64_t startcode, endcode, startstack, kstkesp, kstkeip, start_data, end_data, start_brk;

    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*s");
    ret = fscanf(f, "%*c");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%lu", &startcode);
    ret = fscanf(f, "%lu", &endcode);
    ret = fscanf(f, "%lu", &startstack);
    ret = fscanf(f, "%lu", &kstkesp);
    ret = fscanf(f, "%lu", &kstkeip);
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*d");
    ret = fscanf(f, "%lu", &start_data);
    ret = fscanf(f, "%lu", &end_data);
    ret = fscanf(f, "%lu", &start_brk);
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*u");
    ret = fscanf(f, "%*d");

    (void)ret;
    fclose(f);

    printf("startcode = %lx\n", startcode);
    printf("endcode = %lx\n", endcode);
    printf("startstack = %lx\n", startstack);
    printf("kstkesp = %lx\n", kstkesp);
    printf("kstkeip = %lx\n", kstkeip);
    printf("start_data = %lx\n", start_data);
    printf("end_data = %lx\n", end_data);
    printf("start_brk = %lx\n", start_brk);

    return 0;
}
