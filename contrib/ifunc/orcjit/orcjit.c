// Ref:
//  https://llvm.org/docs/ORCv2.html
//  llvm/examples/OrcV2Examples/OrcV2CBindingsBasicUsage
//  llvm/examples/OrcV2Examples/OrcV2CBindingsReflectProcessSymbols
//
// Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
//
// TODO:
//  Thread safety?
//  Codegen optimizations?
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Support.h>

typedef size_t(*foo_main_f)(size_t, void*);
typedef void(*bar_main_f)(double*, void*);

typedef uint8_t(*target_entry_f)(uint8_t*, float);

typedef struct bar_target_args {
    target_entry_f target_entry;
    foo_main_f foo_main;
} bar_target_args_t;


unsigned int seed = 58;


uint8_t target_entry(uint8_t* v, float l)
{
    // Access a global variable on the target
    srand(seed);

    uint8_t result = 0;
    for (int i = 0; i < (int)l; i++) {
        v[i] = rand() % 251;
        result ^= v[i];
    }
    return result;
}


// This function shows everything needed to create an RDMA-ready LLVM bitcode buffer
char* load_bc(size_t* out_buf_size, const char* bc_name)
{
    LLVMMemoryBufferRef bc_buf;
    char* msg = NULL;
    assert(!LLVMCreateMemoryBufferWithContentsOfFile(bc_name, &bc_buf, &msg));

    const char* bc_buf_raw = LLVMGetBufferStart(bc_buf);
    const size_t bc_buf_raw_size = LLVMGetBufferSize(bc_buf);
    printf("LLVM bitcode buffer @ %p, size = %lu\n", bc_buf_raw, bc_buf_raw_size);

    *out_buf_size = bc_buf_raw_size;
    char* out_buf = malloc(bc_buf_raw_size);
    // Let's pretend this is an RDMA write
    memcpy(out_buf, bc_buf_raw, bc_buf_raw_size);

    LLVMDisposeMemoryBuffer(bc_buf);

    return out_buf;
}


int allow_symbols(void* ctx, LLVMOrcSymbolStringPoolEntryRef sym)
{
    // Just allow everything
    (void)ctx;
    (void)sym;
    return 1;
}


LLVMOrcThreadSafeModuleRef create_ts_mod(LLVMMemoryBufferRef bc)
{
    LLVMOrcThreadSafeContextRef ctx_ts = LLVMOrcCreateNewThreadSafeContext();
    assert(ctx_ts != NULL);

    LLVMContextRef ctx = LLVMOrcThreadSafeContextGetContext(ctx_ts);
    assert(ctx != NULL);

    LLVMModuleRef mod;
    char* msg = NULL;
    // Ownership of the LLVMMemoryBuffer is transferred to the LLVMModule
    assert(!LLVMParseIRInContext(ctx, bc, &mod, &msg));

    LLVMOrcThreadSafeModuleRef mod_ts = LLVMOrcCreateNewThreadSafeModule(mod, ctx_ts);
    assert(mod_ts != NULL);

    LLVMOrcDisposeThreadSafeContext(ctx_ts);

    return mod_ts;
}


void load_deps(const char* filename)
{
    FILE* fp = fopen(filename, "r");
    char* lib_name = NULL;
    size_t lib_name_buf_size;
    ssize_t lib_name_size;
    while ((lib_name_size = getline(&lib_name, &lib_name_buf_size, fp)) != -1) {
        // Erase the '\n'
        lib_name[lib_name_size - 1] = '\0';
        LLVMBool ret = LLVMLoadLibraryPermanently(lib_name);
        if (ret) {
            printf("** Could not load %s\n", lib_name);
        }
        free(lib_name);
        lib_name = NULL;
    }
}


int main()
{
    // Load bitcode, then compile it into a module, all before initializing LLVM
    size_t foo_bc_raw_size;
    char* foo_bc_raw = load_bc(&foo_bc_raw_size, "foo.bc");
    LLVMMemoryBufferRef foo_bc = LLVMCreateMemoryBufferWithMemoryRangeCopy(foo_bc_raw, foo_bc_raw_size, "foo");
    free(foo_bc_raw);
    LLVMOrcThreadSafeModuleRef foo_mod = create_ts_mod(foo_bc);

    size_t bar_bc_raw_size;
    char* bar_bc_raw = load_bc(&bar_bc_raw_size, "bar.bc");
    LLVMMemoryBufferRef bar_bc = LLVMCreateMemoryBufferWithMemoryRangeCopy(bar_bc_raw, bar_bc_raw_size, "bar");
    free(bar_bc_raw);
    LLVMOrcThreadSafeModuleRef bar_mod = create_ts_mod(bar_bc);

    LLVMInitializeCore(LLVMGetGlobalPassRegistry());

    // Required for the JIT
    assert(!LLVMInitializeNativeTarget());
    assert(!LLVMInitializeNativeAsmPrinter());

    LLVMOrcLLJITRef orcjit;
    assert(NULL == LLVMOrcCreateLLJIT(&orcjit, NULL));

    LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(orcjit);
    assert(jd != NULL);

    // This enables symbol resolution in the JIT
    LLVMOrcDefinitionGeneratorRef sym_gen;
    assert(NULL == LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(&sym_gen, LLVMOrcLLJITGetGlobalPrefix(orcjit), allow_symbols, NULL));
    LLVMOrcJITDylibAddGenerator(jd, sym_gen);

    // Ownership of the ThreadSafeModule is transferred to the JIT
    assert(NULL == LLVMOrcLLJITAddLLVMIRModule(orcjit, jd, foo_mod));

    // Library dependencies must be loaded before the lookup
    load_deps("foo.deps");

    LLVMOrcJITTargetAddress foo_main_addr;
    assert(NULL == LLVMOrcLLJITLookup(orcjit, &foo_main_addr, "foo_main"));

    foo_main_f foo_main = (foo_main_f)foo_main_addr;

    printf("\nfoo_main function at %p\n\n", foo_main);

    const size_t ret = foo_main(43, (void*)0xdeadbeef);

    printf("ret = %lu\n", ret);

    // Now add bar.bc to the same JIT instance
    assert(NULL == LLVMOrcLLJITAddLLVMIRModule(orcjit, jd, bar_mod));
    LLVMOrcJITTargetAddress bar_main_addr;
    assert(NULL == LLVMOrcLLJITLookup(orcjit, &bar_main_addr, "bar_main"));
    bar_main_f bar_main = (bar_main_f)bar_main_addr;
    printf("\nbar_main function at %p\n\n", bar_main);

    double x = 42.0;
    bar_target_args_t args;
    args.foo_main = foo_main;
    args.target_entry = target_entry;

    bar_main(&x, &args);

    assert(NULL == LLVMOrcDisposeLLJIT(orcjit));
    LLVMShutdown();

    return 0;
}
