/*
 * Copyright (C) 2019-2022 Arm Ltd. All rights reserved.
 *
 * Notes:
 *  If we need to locate .plt, we probably have to read the section header table
 *  from the ELF file (SHT won't be loaded to memory) and check each entries'
 *  Elf64_Shdr.sh_name. Or get the .dynamic entry that has tag D_RELSZ, and
 *  subtract it from the entry point (reliable?).
 *
 * TODO:
 *  -lm and some math funciton.
 *  dlopen()
 */
#define _GNU_SOURCE // readlink

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h> // readlink
#include <stdbool.h>

#include <elf.h>        // Elf64_xxx
#include <link.h>       // link_map type definition
#include <sys/auxv.h>   // getauxval

#include <dlfcn.h>      // dlopen


extern Elf64_Addr _GLOBAL_OFFSET_TABLE_[];
extern Elf64_Dyn _DYNAMIC[];

const uint8_t var_rodata[8] = {42};
uint8_t var_data[8] = {42};
uint8_t var_bss[8];


void print_u64(uint64_t* v, char* fmt, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf(fmt, i, v[i]);
    }
}


int main()
{
    printf("var_rodata  @ %p\n", (void*)var_rodata);
    printf("var_data    @ %p\n", (void*)var_data);
    printf("var_bss     @ %p\n", (void*)var_bss);
    printf("printf()    @ 0x%lx\n", (uint64_t)printf);
    printf("print_u64() @ 0x%lx\n", (uint64_t)print_u64);

    printf("_DYNAMIC[]  = %p\n", (void*)_DYNAMIC);

    printf("_GLOBAL_OFFSET_TABLE_[]  = %p\n", (void*)_GLOBAL_OFFSET_TABLE_);

    // The content of .got.plt will change after the libc functions are called
    // for non-PIE version of this program
    // TODO: better way to get size of GOT?
    print_u64(_GLOBAL_OFFSET_TABLE_, "_GLOBAL_OFFSET_TABLE_[%d] = 0x%lx\n", 10);

    // Get executable name
    char* elf_file_name = malloc(512);
    ssize_t elf_file_name_len = readlink("/proc/self/exe", elf_file_name, 511);
    elf_file_name[elf_file_name_len] = '\0';
    printf("ELF file: %s\n", elf_file_name);

    FILE* elf_file = fopen(elf_file_name, "rb");
    free(elf_file_name);

    Elf64_Ehdr ehdr;
    fread(&ehdr, sizeof(Elf64_Ehdr), 1, elf_file);

    // Elf64_Shdr shdr_rela_dyn, shdr_rela_plt;
    // Elf64_Shdr shdr_dynstr, shdr_strtab, shdr_shstrtab;

    // fseek(elf_file, ehdr.e_shoff, SEEK_SET);
    // for (size_t i = 0; i < ehdr.e_shnum; i++) {
    //     Elf64_Shdr shdr;
    //     fread(&shdr, sizeof(Elf64_Shdr), 1, elf_file);
    //     if (shdr.sh_type == SHT_RELA) {
    //         printf("Found RELA section!\n");
    //     } else if (shdr.sh_type == SHT_STRTAB) {
    //         printf("Found string table section!\n");
    //     }
    // }


    fclose(elf_file);

    const size_t offset_entry = ehdr.e_entry;
    const size_t offset_phdr  = ehdr.e_phoff;
    printf("Entry point:          0x%lx\n", offset_entry);
    printf("Program header table: 0x%lx\n", offset_phdr);

    const uint64_t vaddr_entry = getauxval(AT_ENTRY);
    const uint64_t vaddr_phdr  = getauxval(AT_PHDR);
    printf("AT_ENTRY = 0x%lx\nAT_PHDR  = 0x%lx\n", vaddr_entry, vaddr_phdr);

    bool is_pie;
    if ((vaddr_entry - offset_entry) == (vaddr_phdr - offset_phdr)) {
        printf("Probably an PIE executable\n");
        is_pie = true;
    } else {
        printf("Probably a non-PIE executable\n");
        assert(vaddr_entry == offset_entry);
        is_pie = false;
    }

    const size_t vaddr_base = vaddr_phdr - offset_phdr;
    printf("Virtual address base = 0x%lx\n", vaddr_base);
    printf("vaddr_base[0~3] = 0x%x '%c' '%c' '%c'\n",
           ((char*)vaddr_base)[0], ((char*)vaddr_base)[1],
           ((char*)vaddr_base)[2], ((char*)vaddr_base)[3]);

    // Get program segment header table
    assert(sizeof(Elf64_Phdr) == getauxval(AT_PHENT));
    Elf64_Phdr* phdr = (Elf64_Phdr*)vaddr_phdr;
    for (size_t i = 0; i < getauxval(AT_PHNUM); i++) {
        if (phdr->p_type != PT_DYNAMIC) {
            phdr++;
        }
    }
    printf("Offset of .dynamic   = 0x%lx\n", phdr->p_offset);
    printf("VirtAddr of .dynamic = 0x%lx\n", phdr->p_vaddr);

    // Get the DT_PLTGOT entry of .dynamic
    Elf64_Dyn* dyn;
    if (is_pie) {
        dyn = (Elf64_Dyn*)(vaddr_base + phdr->p_vaddr);
    } else {
        dyn = (Elf64_Dyn*)(phdr->p_vaddr);
    }

    while (dyn->d_tag != DT_PLTGOT) {
        dyn++;
        if (dyn->d_tag == DT_NULL) {
            assert(0);
        }
    }

    // Address of ld.so (text or data???)
    printf("AT_BASE = 0x%lx\n", getauxval(AT_BASE));

    if (is_pie) {
        // TODO: PIE executables do not even have .got.plt, so wtf is this?
        printf("dyn->d_un.d_ptr = %p\n", (void*)dyn->d_un.d_ptr);
        print_u64((uint64_t*)dyn->d_un.d_ptr,
                  "dyn->d_un.d_ptr[%lu] = 0x%lx\n", 10);
    } else {
        // This is .got.plt!
        void** gotplt = (void**)dyn->d_un.d_ptr;
        printf(".got.plt @ %p\n", (void*)gotplt);

        // Print the link_map linked list, it's the 2nd entry of .got.plt
        struct link_map* l = (struct link_map*)gotplt[1];
        printf(".got.plt[1] = link_map = %p\n", (void*)l);
        int link_map_i = 0;
        while (l->l_next != NULL) {
            // l->l_ld points to .dynamic of the object
            printf("link_map[%d].l_addr = 0x%lx\n", link_map_i, l->l_addr);
            printf("link_map[%d].l_name = %s\n", link_map_i, l->l_name);
            l = l->l_next;
        }

        // The entry to ld.so's trampoline is the 3rd entry of .got.plt
        printf(".got.plt[2] = trampoline @ %p\n", gotplt[2]);
    }

    // Check ld.so and other mappings
    FILE* proc_maps = fopen("/proc/self/maps", "r");
    int c;
    while ((c = getc(proc_maps)) != EOF) {
        putchar(c);
    }
    fclose(proc_maps);

    // Experiment with dlopen()
    void* foo_handle = dlopen("./libfoo.so", RTLD_NOW);
    void(*foo_entry)() = (void(*)())dlsym(foo_handle, "entry");

    foo_entry();

    dlclose(foo_handle);

    return 0;
}
