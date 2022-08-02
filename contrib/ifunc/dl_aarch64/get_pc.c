/* 
 * Copyright (C) 2019-2022 Arm Ltd. All rights reserved.
 */
// The results aren't stable when O2 is used, will ISB help?
#include <stdio.h>
#include <stdint.h>


// If not inlined, this functions returns its own return address, so it's the next PC.
__attribute__ ((noinline))
void get_pc(void** pc)
{
    *pc = __builtin_return_address(0);
}

int main()
{
    // Three different ways to get the PC
    void *pc0, *pc1, *pc2;

    get_pc(&pc0);

label:
    __asm__ volatile("adr %0, ." : "=r"(pc1) : : "memory");

    // https://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html
    pc2 = &&label;

    printf("%p\n%p\n%p\n", pc0, pc1, pc2);

    // Get the offset of an instruction
    uint64_t offset;
    __asm__ volatile(".my_label:\n"
                     "mov %0, 0\n"
                     "add %0, %0, :lo12:.my_label" : "=r"(offset) : : "memory");
    printf("0x%lx\n", offset);
}
