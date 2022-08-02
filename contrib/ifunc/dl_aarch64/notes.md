# ELF and dynamic linking

ELF files contain code and data, but they also contain tables of other types of information. Type definitions for the entries of the tables can be found in glibc's `/usr/include/elf.h`, kernel's `include/uapi/linux/elf.h` and LLVM's `llvm/include/llvm/BinaryFormat/ELF.h`.

During linking, we need to know which symbols need external linkage or global/static attributes. Relocation entries are used to resolve the symbols before the program can run, and they are resolved by the linker statically or dynamically.

Relocatable object files (`*.o`) are also ELF files. If examined with `readelf -SW`, we may see `.rela.text` and/or `.rela.data`. Are they the relocation info sections for the `.text` and `.data` sections that act like lists of TODOs?

During linking, symbol resolution happens before symbol relocation, b/c we want to know which symbol to use (which definition of this function/variable should the linker pick?). If resolution becomes ambiguous, the linker picks strong symbols (defined functions & initialized variables) over weak symbols (uninitialized variables). Multiple strong symbols results in failure; multiple weak symbols and no strong symbol results in the linker pick a random one.

This explains why sometimes we have to put library flags (e.g. `-lm`) at the end of GCC invocations. The linker is dumb and forget about symbols that are seemingly unused at the moment (seeing the lib before seeing an object file that uses functions from that lib), so there's an ordering issue for including the `.o`, `.a` and `.so` files.

Disassemble an object file with `objdump -dx`, references to unresolved symbols are likely to be all empty(zeros) and the relocation types will be displayed as well.

After we have found a single definition of each symbol to use in resolution, now we do relocation: update each reference to a symbol with the run-time address of its definition. All sections of the same type are merged to form a binary. In PC-relative addressing, the PC always points to the next instruction, so the calculated offset needs to be fixed with an addend to account for this difference.

What about dynamic linking, where symbol resolution and relocation must be done partly at run-time?

When linking an executable that will use dynamic libraries, we need the relocation and symbol table from the dynamic libraries (lib names, symbol names, sizes, type signatures)! The static linker creates a `.interp` section, which will be used by the loader/dynamic linker to do run-time linking.

Dealing with variables in link-time is easier, the `.got` section (usually mapped just before the `.data` section in case of overflows) contains indirections to all global variables. The linker had generated a relocation entry for each variable in the GOT and is using PC-relative access in the `.text` section for GOT entries.

Linking view: sections. Execution view: sections grouped into segments, some segments are loadable. In the output of `readelf -lW`, the two LOADs are code & data segments.

Program (segment) headers are used to instruct the loader about which parts of the binary file gets loaded into what memory addresses, with what permissions. Expansion of the `.bss` section is also controlled by this.

VMA: virtual memory areas, each is a contiguous chunk of memory pages(`/proc/$pid/maps` or use `pmap`). VMAs do not overlap, accessing an VMA with invalid permission or an non-existent VMA results in segfaults. VMAs must be homogeneous in terms of permission & type(`MAP_SHARED/MAP_PRIVATE`) & backing file (if any), so we change the permission in the middle of a VMA, it will get split into three VMAs(suppose there are three pages in the original VMA).

Relevant sections: `.plt` is read-only and contains code, `.got` may or may not be read-only (at execution-time) and contains binary data (pointers `Elf64_Addr`).

How does `.plt` and `.got.plt` work together? Generally speaking, the latter is populated by the former dynamically during program execution.

On x64, calls to `foo` code jumps to `foo`'s unique slot in the PLT, then jumps to `foo`'s corresponding slot in the GOT. However, initially the GOT entry points right back to the next instruction of the jump to the GOT! So effectively we continue to execute the code contained in the PLT slot, which pushes `foo`'s GOT slot ID to the stack and calls the code stub in the first PLT slot, which calls the dynamic linker to do symbol relocation and replace `foo`'s GOT slot with the its correct address. Finally, the dynamic linker calls `foo` on behalf of the user. When `foo` is called for the 2nd time, its GOT slot already contains the virtual address of `foo`'s code and the execution continues as normal. This is called "lazy-binding".

Note that the exact way of bouncing between the PLT and the GOT differs for different ISAs. On AArch64, the PLT loads `IP0/x16` with the GOT slot address, loads `IP1/x17` with the content of the GOT slot, and branches to `x17`.

Some implications: PLT contains code that will be called directly, so it must have a fixed relative offset from `.text`; GOT needs to be writeable(at least for a while) and is allocated at a known static virtual address.

Difference between `.got` and `.got.plt`? Not sure. It looks like that `.got` entries are never resolved lazily, but `.got.plt` entries can be resolved lazily (trade security for better start-up performance). And `.got` could contain addresses of global variables, while `.got.plt` only contains addresses of procedures that will be filled by the `.plt` section and the dynamic loader.

`_GLOBAL_OFFSET_TABLE_[0]` holds the address of the `.dynamic` section.

Check the `elf_machine_runtime_setup` function in `glibc/sysdeps/aarch64/dl-machine.h` for what's in `.got.plt[1]` and `.got.plt[2]`.

RELRO: RELocation Read Only.

What are the `.rela.dyn` and `.rela.plt` sections? They are relocation records. Check the type column and find the relocation type in _ELF for the Arm 64-bit Architecture(AArch64)_, `GLOB_DAT` -> GOT data symbol, `JUMP_SLOT` -> GOT code targets.

What about `.symtab` and `.dynsym`? The first contain references for all symbols used during static link editing, and the latter contain only those symbols needed for dynamic linking. So the relocations in the `.rela*` sections should all be in the `.dynsym` section.

# ELF related tools

## ld

`--verbose`: display the linker script.

`-z relro`: enables read-only relocations (the non-PLT part of `.got`, `.dynamic` and possibly other sections). It is only partial-RELRO without `-z now`.

`-z now`: enable non-lazy run-time binding (`.got.plt` are merged to `.got` and relocations are resolved at load-time and then remapped as read-only for run-time). Combined with `-z relro` we have full-RELRO.

## ld.so

`LD_BIND_NOW=""`: non-empty -> resolve symbols at load-time, otherwise use lazy binding. Run-time equivalent of `ld -z now` (not exactly the same).

`LD_BIND_NOT=""`: non-empty -> do not update GOT after resolving a function symbol.

`LD_DEBUG=all`: print what did `ld.so` do.

## objdump

`-d`: disassemble all executable sections.

`-D`: disassemble all sections even if it doesn't make sense. Not sure what's the point.

`-s`: display all sections in HEX and ASCII view, used to inspect `.got`, `.rodata`, string tables, etc.

`-j`: display only the contents of requested section (use with `-s`).

`-t`: display the content of `.symtab`(`Elf64_Sym`).

`-T`: display the content of `.dynsym`(`Elf64_Sym`).

`-r`: display the relocations entries.

`-R`: display the dynamic relocations entries. Just use `readelf -rW` instead.

`-x`:

## readelf

`-W`: use wide display format.

`-h`: display ELF header info(`Elf64_Ehdr`).

Entry point address(`_start`).

Number and sizes of section headers.

`-S`: display section header table(`Elf64_Shdr`).

Address -> address of section after loaded to memory.

Off -> section offset in the ELF file.

`-l`: display program header table(`Elf64_Phdr`).

Loadable segments have `Elf64_Phdr.p_type = PT_LOAD`.

The 1st LOAD -> executable and/or relocation-related read-only sections(code, rodata, etc.).

VirtAddr & PhysAddr shown here are different for PIE/non-PIE executables, which is controlled by `ld`, can be seen with `ld --verbose | grep SEGMENT_START`.

Also, due to alignment requirements, the VirtAddr may not be respected when the segments are actually loaded (example???).

According to `Elf64_Phdr` struct's section in ELF manpage, `.p_offset % pagesize == .p_vaddr % pagesize` if `p_align` is neither 0 nor 1.

The 2nd LOAD -> run-time modifiable sections(all kinds of data). The difference between `FileSiz` and `MemSiz` is because `.bss` occupies no space in the binary and is mapped anonymously later. Segments have alignment requirements so the 2nd LOAD is usually located "far behind" the 1st LOAD.

For the segment marked with `PT_GNU_RELRO`, the dynamic linker changes it to `PROT_READ` with `mprotect` after finishing the relocations. Is this why there are three VMAs (RX-R-RW) for the executable in `/proc/self/maps`? Check the output of `strace -e trace=memory` to confirm.

Interestingly, `.dynstr` seems to be the only string table that gets loaded into memory.

`-s`: display entries of the symbol table `.symtab` & `.dynsym`(`Elf64_Sym`). Local binding means static? What about WEAK? HIDDEN visibility?

`--dyn-syms`: display entries of `.dynsym` only. For libraries these are the dynamic symbols that can be found by `dlopen` & `dlsym`?

`-r`: display contents of the relocation sections `.rela.dyn` & `.rela.plt`(`Elf64_Rel` & `Elf64_Rela`).

`-d`: display contents of the dynamic section `.dynamic`(`Elf64_Dyn`).

## nm

This tool requires debug info?

`-D`: display the content of `.dynsym` instead of the default `.symtab`(`Elf64_Sym`).

## strace

`-e mmap,munmap`: trace `mmap/munmap` calls.

`-e trace=memory`: trace memory operations line `brk`, `mmap/munmap`, `mprotect`, etc.

## pmap

`-x`: show details of a process's virtual memory mappings.

## Misc

[HEX <-> AArch64 (little endian)](https://armconverter.com/)

# Tutorial

## Setup on Ubuntu

To see the source of `ld.so`, install `libc6-dbg`, and download glibc's source code of the right version. Then in `.gdbinit`, put:

    set debug-file-directory /usr/lib/debug
    directory /path/to/glibc-2.31/elf

Launch GDB with `gdb -q -tui ./main_nopie.x`, use `layout split` to see both the source code and the disassembly at the same time (use ctrl-x to switch focus).

If GDB still cannot display the source of `ld.so`, check the output of `info sharedlibrary` to see if the debug symbols are loaded.

The file `/lib/ld-linux-aarch64.so.1` is actually a symlink to `/lib/aarch64-linux-gnu/ld-2.31.so`, with the corresponding debug symbols at `/usr/lib/debug/lib/aarch64-linux-gnu/ld-2.31.so`.

But sometimes GDB is too dumb to do a `readlink` to find the true path and load the debug symbols. This can be fixed by three different ways:

- Compile the program with `-Wl,--dynamic-linker=/lib/aarch64-linux-gnu/ld-2.31.so`.

- Create a symlink at `/usr/lib/debug/lib/ld-2.31.so` that points to `/usr/lib/debug/lib/aarch64-linux-gnu/ld-2.31.so`.

- Run `add-symbol-file /usr/lib/debug/lib/aarch64-linux-gnu/ld-2.31.so $ADDR` in GDB, where `$ADDR` is the memory address of the dynamic linker shown in `info sharedlibrary`.

GDB disables ASLR by default to make analysis much easier, enable with `set disable-randomization off` if needed.

Watch memory location with GDB watchpoints: `watch variable_foo`, `watch *(type*)0x12345678`. For write `watch`, for read `rwatch`, for both `awatch`. Use `(gdb) show can-use-hw-watchpoints` to check if HW supports this.

Use `disas 'foo@plt'` to check the PLT entry of a function, use `x/4i 0x12345678` to force disassemble 4 instructions at an address.

Use `x/4gx 0x12345678` to check four 64-bit value at an address.

Use `info proc mappings` and `info proc stat` to check memory mappings and the base address of `.text`.

Ubuntu defaults to use `-pie -z relro -z now` in GCC for security reasons, this can be confirmed in the output of `gcc --verbose main.c`. So by default lazy binding is disabled and the entire GOT is read-only for execution-time (full RELRO).

If `-fno-pie` and `-no-pie` are added, GCC removes `-pie` and `-z now`, but still keeps `-z relro`.

Check the executable with `size --format=sysv`, absolute address means PIE, relative address means non-PIE. The `file` command can also show the difference.

For PIE, the `ELF_ET_DYN_BASE` macro in `linux/arch/arm64/include/asm/elf.h` determines the base address.

## Calling printf

Let's step through the execution of `main.c`.

If the PIE version is used, the first time we call `printf@plt` we will see (`x/gx $x16`) that the GOT slot already contains the virtual address of `printf` (`p/x $x17` & `x/2i $x17`). Restart GDB and watch that GOT slot with `b _start` and `watch *(void**)0x12345678`, we can see that the slot is modified by `ld.so`, after `_start` is called but before the control is transferred to the binary. This is the effect of non-lazy binding `-z now`.

This can also be confirmed by comparing the output of the PIE and the non-PIE version while setting `LD_DEBUG=bindings,reloc`. For the PIE version all symbols are resolved before the program is initialized.

So we use the non-PIE version to see the PLT in action.

Run the program, we can see that the virtual address base is always `0x400000`, so the locations of all the 'internal' symbols are fixed, while the address of the DL trampoline (`_dl_runtime_resolve`) and the `link_map` are still affected by ASLR.

Program layout for reference:

```
0x4008f0: PLT[0]
...
0x400a10: PLT[m] = printf@plt
...
0x411fd8: .got[0] = _DYNAMIC
0x411fe0: .got[1] = 0x0
0x411fe8: .got.plt[0] = 0x0
0x411ff0: .got.plt[1] = link_map
0x411ff8: .got.plt[2] = trampoline
0x412000: .got.plt[3] = &PLT[0] (initially)
...
0x412080: .got.plt[n] = printf@got.plt
```

Now let's start the program in GDB and `si` to the first invocation of `printf@plt`. This time `x/4i $x17` shows the content of PLT[0].

PLT[0]'s actions: subtract 16 from `SP` (stack grows downwards) and store `x16` and `x30`(link register, with the address of the instruction after the call to `printf`) onto the stack; load the address of `.got.plt[2]` into `x16`; load the content of `.got.plt[2]` into `x17`; branch to the trampoline. The requirements for calling the trampoline can be found near the top of `dl-trampoline.S`, these need to be satisfied if we want to 'hijack' the trampoline.

One interesting question is: who filled `.got.plt[1]` and `.got.plt[2]`?

The trampoline first stores register `x0~x9, q0~q7` to the stack (`10 * 8 + 8 * 16 = 208`), then loads the link map stored at `[x16 - 8] = .got.plt[1]` into `x0`, and loads the address of `printf@got.plt` into `x1`.

Then the offset of `printf` in the relocation table must be calculated before we call `_dl_fixup`, here's how:

```
#define RELA_SIZE (PTR_SIZE * 3) // RELA relocations are 3 pointers
x1 = x1 - ip0 = x1 - x16 = printf@got.plt - trampoline@got.plt = &.got.plt[n] - &.got.plt[2] = (n - 2) * 8
x1 = x1 + (x1 << 1) = x1 * 3 = (n - 2) * 8 * 3
x1 = x1 << 3 = (n - 2) * 8 * 3 * 8
x1 = x1 - (RELA_SIZE << 3) = (n - 2) * 8 * 3 * 8 - ((8 * 3) * 8)
x1 = x1 >> 3 = (n - 2) * 8 * 3 - (8 * 3) = (n - 3) * 8 * 3
```

Effectively, it calculates `n`, subtracts 3 because there are three reserved slots in `.got.plt`, and multiplies that with `RELA_SIZE`. Then the trampoline calls `_dl_fixup`.

Taking a closer look at `_dl_fixup` in `glibc-2.31/elf/dl-runtime.c`, a lot of info is being fetched from the linker struct, while the offset `reloc_arg == reloc_offset` is used only once. Watching the memory location shows that the linker struct is populated between the `_start` of `ld.so` and the `_start` of the main program, by the `_dl_new_object` function. The `link_map` struct defined in `glibc-2.31/include/link.h` looks super complicated, but it seems that we have to be able to add a new node to this linked list for our ifunc library.

Now what? Maybe study `dlopen` is faster? Essentially we're doing the same thing.

# References

`man elf`

`man ld.so`

[LSB, ELF, SysV ABI and other specs](https://refspecs.linuxfoundation.org/)

[LSB-Core-ELF-Dynamic Liniking](https://refspecs.linuxbase.org/lsb.shtml)

[LSB - what are the ELF sections?](https://refspecs.linuxbase.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/specialsections.html)

[Systems programming lectures](https://www.youtube.com/c/ChrisKanich/videos)

[LWN - How programs get run: ELF binaries](https://lwn.net/Articles/631631/)

[Basic ELF info 1](https://wiki.osdev.org/ELF)

[Basic ELF info 2](https://web.archive.org/web/20121126063759/http://www.acsu.buffalo.edu/~charngda/elf.html)

[Basic ELF info 3](https://greek0.net/elf.html)

[PIC in shared libraries](https://eli.thegreenplace.net/2011/11/03/position-independent-code-pic-in-shared-libraries)

[Gentoo Wiki PIC internals](https://wiki.gentoo.org/wiki/Hardened/Position_Independent_Code_internals)

[PIE](https://eklitzke.org/position-independent-executables)

[Understanding ELF using readelf and objdump](https://web.archive.org/web/20180320120939/http://www.linuxforums.org/articles/understanding-elf-using-readelf-and-objdump_125.html)

[GOT diagram](http://bottomupcs.sourceforge.net/csbu/x3824.htm)

[Simple GOT & PLT example](https://systemoverlord.com/2017/03/19/got-and-plt-for-pwning.html)

[Understanding Symbols Relocation](https://stac47.github.io/c/relocation/elf/tutorial/2018/03/01/understanding-relocation-elf.html)

[Relocations](http://www.mindfruit.co.uk/2012/06/relocations-relocations.html)

[How To Write Shared Libraries - Ulrich Drepper](https://www.akkadia.org/drepper/dsohowto.pdf)

[Static & dynamic linking summary](https://gist.github.com/reveng007/b9ef8c7c7ed7a46b10a325f4dee42ac4)

[PLT GOT & interposition](https://www.macieira.org/blog/2012/01/sorry-state-of-dynamic-libraries-on-linux/)

[PLT entries & relocation table entries](https://refspecs.linuxfoundation.org/ELF/zSeries/lzsabi0_zSeries/x2251.html)

[GOT hijacking](http://www.infosecwriters.com/text_resources/pdf/GOT_Hijack.pdf)

[ELF VMA memory layout](https://gist.github.com/CMCDragonkai/10ab53654b2aa6ce55c11cfc5b2432a4)

[Linux program startup(all the hidden crt functions)](http://dbp-consulting.com/tutorials/debugging/linuxProgramStartup.html)

[ELF and ptrace](https://linuxgazette.net/issue85/sandeep.html)

[Resolving ELF relocations](http://em386.blogspot.com/2006/10/resolving-elf-relocation-name-symbols.html)

[Examine dynamic relocations on x64](https://www.cs.dartmouth.edu/~sergey/cs108/dyn-linking-with-gdb.txt)

[Liniking the hard way](https://gist.github.com/jsimmons/cb8cdc58f8c82976ef379c126bf09efc)

[Minimal ld.so implementation](https://github.com/jserv/min-dl)

[dlsym() for remote processes](https://gist.github.com/resilar/24bb92087aaec5649c9a2afc0b4350c8)

Book: _Learning Linux Binary Analysis_
