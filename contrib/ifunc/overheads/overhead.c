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
#include <time.h>

#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Support.h>

#define BILLION 1E9
#define UCP_IFUNC_NAME_MAX 8
#define UCP_IFUNC_SYMBOL_MAX    (UCP_IFUNC_NAME_MAX + 16)


int iterations = 1000;
unsigned int seed = 58;
LLVMOrcLLJITRef orcjit;
LLVMOrcJITDylibRef jd;
LLVMBool ret;
LLVMErrorRef err;
LLVMOrcDefinitionGeneratorRef sym_gen;
LLVMOrcLLJITBuilderRef builder;

typedef void (*ifunc_main_f)(void *payload,
                             size_t payload_size,
                             void *target_args);

typedef size_t (*ifunc_payload_bound_f)(void *source_args,
                                        size_t source_args_size);

typedef int (*ifunc_payload_init_f)(void *source_args,
                                    size_t source_args_size,
                                    void *payload,
                                    size_t *payload_size);



typedef uint8_t(*target_entry_f)(uint8_t*, float);

typedef struct bar_target_args {
    target_entry_f target_entry;
    ifunc_main_f foo_main;
} bar_target_args_t;

typedef struct ifunc {

    char name[UCP_IFUNC_NAME_MAX];

    ifunc_main_f main;
    ifunc_payload_bound_f payload_bound;
    ifunc_payload_init_f payload_init;

    char *bc_raw;
    size_t bc_raw_size;

    char *deps;
    size_t deps_size;
} ifunc_t;

typedef struct ifunc    *ifunc_h;


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


// DO NOT MEASURE, ONE TIME COST
// This function shows everything needed to create an RDMA-ready LLVM bitcode buffer
static inline char *jit_load_bc(ifunc_h ih, const char *bc_name)
{
    LLVMBool err;
    LLVMMemoryBufferRef bc_buf;
    char *bc_buf_raw;
    char *out_buf = NULL;
    char *msg = NULL;

    err = LLVMCreateMemoryBufferWithContentsOfFile(bc_name, &bc_buf, &msg);
    if (err) {
        printf("Could not load bitcode %s\n", bc_name);
        ih->bc_raw_size = 0;
        return NULL;
    }

    bc_buf_raw = (char *)LLVMGetBufferStart(bc_buf);
    ih->bc_raw_size = LLVMGetBufferSize(bc_buf);

    out_buf = malloc(ih->bc_raw_size);
    memcpy(out_buf, bc_buf_raw, ih->bc_raw_size);

    LLVMDisposeMemoryBuffer(bc_buf);

    return out_buf;
}

static inline int
orcjit_allow_symbols(void* ctx, LLVMOrcSymbolStringPoolEntryRef sym)
{
    // Just allow everything
    (void)ctx;
    (void)sym;
    return 1;
}

static LLVMOrcObjectLayerRef
orcjit_create_object_layer(void* ctx, LLVMOrcExecutionSessionRef ES, const char *Triple)
{
    LLVMOrcObjectLayerRef OLL;
    (void)ctx;
    (void)Triple;
    OLL = LLVMOrcCreateRTDyldObjectLinkingLayerWithSectionMemoryManager(ES);
    LLVMOrcRTDyldObjectLinkingLayerRegisterJITEventListener(OLL, LLVMCreatePerfJITEventListener());
    LLVMOrcRTDyldObjectLinkingLayerRegisterJITEventListener(OLL, LLVMCreateGDBRegistrationListener());
    return OLL;
}


static inline LLVMOrcThreadSafeModuleRef jit_create_ts_mod(LLVMMemoryBufferRef bc)
{
    LLVMOrcThreadSafeContextRef ctx_ts;
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBool ret;
    char *msg = NULL;
    LLVMOrcThreadSafeModuleRef mod_ts;

    ctx_ts = LLVMOrcCreateNewThreadSafeContext();
    assert(ctx_ts != NULL);

    ctx = LLVMOrcThreadSafeContextGetContext(ctx_ts);
    assert(ctx != NULL);
    
    // Ownership of the LLVMMemoryBuffer is transferred to the LLVMModule
    ret = LLVMParseIRInContext(ctx, bc, &mod, &msg);
    assert(!ret);
    (void)ret;

    mod_ts = LLVMOrcCreateNewThreadSafeModule(mod, ctx_ts);
    assert(mod_ts != NULL);

    LLVMOrcDisposeThreadSafeContext(ctx_ts);

    return mod_ts;
}


void load_deps_list(ifunc_h ih, const char *filename)
{
    FILE *fp = fopen(filename, "r");
    char *dep_name = NULL;
    size_t dep_name_buf_size;
    ssize_t dep_name_size;

    ih->deps = NULL;
    ih->deps_size = 0;

    while ((dep_name_size = getline(&dep_name, &dep_name_buf_size, fp)) != -1) {
        /* Erase the '\n' at the end. */
        dep_name[dep_name_size - 1] = '\0';

        ih->deps = realloc(ih->deps, ih->deps_size + dep_name_size);
        memcpy(ih->deps + ih->deps_size, dep_name, dep_name_size);
        ih->deps_size += dep_name_size;

        free(dep_name);
        dep_name = NULL;
    }
    free(dep_name);

    fclose(fp);
}

static inline LLVMOrcJITTargetAddress
pull_symbol_bc(LLVMOrcLLJITRef jit, const char *fmt, const char *ifunc_name)
{
    char symbol_name[UCP_IFUNC_SYMBOL_MAX];
    
    LLVMOrcJITTargetAddress addr;
    LLVMErrorRef err;
    snprintf(symbol_name, UCP_IFUNC_SYMBOL_MAX, fmt, ifunc_name);
    err = LLVMOrcLLJITLookup(jit, &addr, symbol_name);
    
    assert(err == NULL);
    (void)err;
    
    return addr;
}

int handleError(LLVMErrorRef Err) {
  char *ErrMsg = LLVMGetErrorMessage(Err);
  fprintf(stderr, "LLVM Error: %s\n", ErrMsg);
  LLVMDisposeErrorMessage(ErrMsg);
  return 1;
}

static inline void register_ifunc_bc(ifunc_h ih){

    LLVMMemoryBufferRef bc;
    LLVMOrcThreadSafeModuleRef mod;
    LLVMBool ret;
    LLVMErrorRef err;
    LLVMOrcJITTargetAddress addr_main, addr_payload_bound, addr_payload_init;
    
    char *deps_iterator = ih->deps;;
    while ((deps_iterator != NULL) &&
          ((deps_iterator - ih->deps) < (ssize_t)ih->deps_size)) {
        ret = LLVMLoadLibraryPermanently(deps_iterator);
        
        if (ret) {
            // printf("Error loading %s\n", deps_iterator);
        }
        deps_iterator = strchr(deps_iterator, '\0') + 1;
    }

    bc = LLVMCreateMemoryBufferWithMemoryRangeCopy(ih->bc_raw, ih->bc_raw_size, ih->name);
    mod = jit_create_ts_mod(bc);
    err = LLVMOrcLLJITAddLLVMIRModule(orcjit, jd, mod);
    
    if (err) {
        handleError(err);
    }
    assert(err == NULL);
    (void)err;
    
    addr_main = pull_symbol_bc(orcjit, "%s_main", ih->name);
    
    ih->main = (ifunc_main_f)addr_main;
    assert(ih->main != NULL);

    addr_payload_bound = pull_symbol_bc(orcjit, "%s_payload_bound", ih->name);
    
    ih->payload_bound = (ifunc_payload_bound_f)addr_payload_bound;
    assert(ih->payload_bound != NULL);

    addr_payload_init = pull_symbol_bc(orcjit, "%s_payload_init", ih->name);
    
    ih->payload_init = (ifunc_payload_init_f)addr_payload_init;
    assert(ih->payload_init != NULL);    
}

int main()
{
    struct timespec t0, t1, td;
    double diff;
    size_t ret_val = 777;

    // Load bitcode before all the measurements. The bitcode arrives in memory
    // and gets copied
    // TODO: add archive

    ifunc_h foo_h;
    foo_h = calloc(1, sizeof(*foo_h));

    strncpy(foo_h->name, "foo", UCP_IFUNC_NAME_MAX - 1);
    foo_h->name[UCP_IFUNC_NAME_MAX - 1] = 0;
    foo_h->bc_raw = jit_load_bc(foo_h, "food.bc");
    load_deps_list(foo_h, "food.deps");

    ifunc_h bar_h;
    bar_h = calloc(1, sizeof(*bar_h));

    strncpy(bar_h->name, "bar", UCP_IFUNC_NAME_MAX - 1);
    bar_h->name[UCP_IFUNC_NAME_MAX - 1] = 0;
    bar_h->bc_raw = jit_load_bc(bar_h, "bark.bc");
    load_deps_list(bar_h, "bark.deps");

    ifunc_h b_h;
    b_h = calloc(1, sizeof(*b_h));

    strncpy(b_h->name, "b", UCP_IFUNC_NAME_MAX - 1);
    b_h->name[UCP_IFUNC_NAME_MAX - 1] = 0;
    b_h->bc_raw = jit_load_bc(b_h, "b.bc");
    load_deps_list(b_h, "b.deps");

    for (int j = 0; j < 2; j++) {
        printf("real work %d\n", j);

        clock_gettime(CLOCK_MONOTONIC_RAW , &t0);
        for (int i = 0; i < iterations; i ++) {
            // init LLVM
            LLVMInitializeCore(LLVMGetGlobalPassRegistry());
            ret = LLVMInitializeNativeTarget();
            assert(!ret);
            ret = LLVMInitializeNativeAsmPrinter();
            assert(!ret);
            (void)ret;  /* Supress -Werror when ucs_assert() is disabled. */

            builder = LLVMOrcCreateLLJITBuilder();
            LLVMOrcLLJITBuilderSetObjectLinkingLayerCreator(builder, &orcjit_create_object_layer, NULL);

            err = LLVMOrcCreateLLJIT(&orcjit, builder);
            assert(err == NULL);

            jd = LLVMOrcLLJITGetMainJITDylib(orcjit);
            assert(jd != NULL);

            /* Enable symbol resolution in the JIT. */
            err = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
                    &sym_gen,
                    LLVMOrcLLJITGetGlobalPrefix(orcjit),
                    orcjit_allow_symbols,
                    NULL);
            assert(err == NULL);
            (void)err;  /* Supress -Werror when ucs_assert() is disabled. */
            LLVMOrcJITDylibAddGenerator(jd, sym_gen);

            register_ifunc_bc(foo_h);
            register_ifunc_bc(bar_h);
            if (j == 1) {
                register_ifunc_bc(b_h);
            }

            if (0) {
                size_t return_tgt;

                foo_h->main((void*)0xdeadbeef, 43, &return_tgt);

                ret_val = return_tgt;

                double x = 42.0;
                void *pay = (void *)&x;
                bar_target_args_t args;
                args.foo_main = foo_h->main;
                args.target_entry = target_entry;

                bar_h->main(pay, 0, &args);
            }

            assert(NULL == LLVMOrcDisposeLLJIT(orcjit));
        }

        // Collect end-time
        clock_gettime(CLOCK_MONOTONIC_RAW , &t1);

        printf("ret = %lu\n", ret_val);

        // Calculate time
        td.tv_sec  = t1.tv_sec  - t0.tv_sec;
        td.tv_nsec = t1.tv_nsec - t0.tv_nsec;

        diff = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / BILLION;

        printf("Elapsed time:\n");
        printf("mode, iterations, t0.tv_sec, t0.tv_nsec, t1.tv_sec, t1.tv_nsec, td.tv_sec, td.tv_nsec\n");
        printf("%d,%d,%ld,%ld,%ld,%ld,%ld,%ld\n",j,iterations,t0.tv_sec, t0.tv_nsec, t1.tv_sec, t1.tv_nsec, td.tv_sec, td.tv_nsec);
        printf("%5.15f\n", diff);
    }

    LLVMShutdown();

    return 0;
}
