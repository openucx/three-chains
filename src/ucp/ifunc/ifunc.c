/**
 * Copyright (C) 2019-2022 Arm Ltd. All rights reserved.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ifunc.h"

#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include <ucs/sys/sys.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <ucs/debug/assert.h>
#include <ucs/config/parser.h>

#ifdef HAVE_LLVM
#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Support.h>
#include <dirent.h>         /* To list all *.bc in a directory */
#endif  /* HAVE_LLVM */

#define CACHE_ENABLED
// define if you want all the poll operations to be done, but avoid jitting and executing
//#define DO_NOT_JIT_EXEC

#include <ucp/api/ucp.h>
#include <ucp/core/ucp_context.h>

#ifdef HAVE_LLVM
static inline char*
jit_load_bc(size_t *out_buf_size, const char *bc_name)
{
    LLVMBool err;
    LLVMMemoryBufferRef bc_buf;
    char* bc_buf_raw;
    char* out_buf = NULL;
    char* msg = NULL;

    err = LLVMCreateMemoryBufferWithContentsOfFile(bc_name, &bc_buf, &msg);
    if (err) {
        ucs_warn("IFUNC could not load bitcode %s\n", bc_name);
        *out_buf_size = 0;
        return NULL;
    }

    bc_buf_raw = (char*)LLVMGetBufferStart(bc_buf);
    *out_buf_size = LLVMGetBufferSize(bc_buf);

    /* Use our own buffer as the raw bitcode could also come from a message */
    out_buf = malloc(*out_buf_size);
    memcpy(out_buf, bc_buf_raw, *out_buf_size);

    LLVMDisposeMemoryBuffer(bc_buf);

    return out_buf;
}

static inline int
filter_ext_bc(const struct dirent *dir)
{
    char *ext = NULL;

    if (dir == NULL) {
        return 0;
    }

    if (dir->d_type == DT_REG) {
        ext = strrchr(dir->d_name, '.');
        if ((ext == NULL) || (ext == dir->d_name)) {
            return 0;
        } else if (strcmp(ext, ".bc") == 0) {
            return 1;
        }
    }

    return 0;
}

static inline bc_archive_t*
jit_create_bc_archive(ucp_context_h context,
                      size_t *bcarc_size,
                      ucp_ifunc_reg_param_t param)
{
    // context->ifunc_lib_dir, param.name
    struct dirent **bcs;
    int n_bc, prefix, triple_len, n_triples;
    char *dot0, *dot1;
    char* triples[UCP_IFUNC_BC_TARGETS_MAX];
    char bc_filename[UCP_IFUNC_FILE_NAME_MAX];
    char* bc_bufs[UCP_IFUNC_BC_TARGETS_MAX];
    size_t bc_sizes[UCP_IFUNC_BC_TARGETS_MAX];
    char* bcarc;
    size_t offset;

    /* List all *.bc files */
    n_bc = scandir(context->ifunc_lib_dir, &bcs, filter_ext_bc, alphasort);

    if (n_bc <= 0) {
        ucs_warn("IFUNC no bitcode file under %s\n", context->ifunc_lib_dir);
        return NULL;
    }

    n_triples = 0;

    /* Get all ifunc_name.{triple}.bc files */
    while (n_bc > 0) {
        /* Make sure the prefix matches, and is followed by a '.' */
        prefix = strncmp(param.name, bcs[n_bc - 1]->d_name, strlen(param.name));
        if ((prefix == 0) &&
            (bcs[n_bc - 1]->d_name[strlen(param.name)] == '.')) {
            dot0 = strchr(bcs[n_bc - 1]->d_name, '.');
            dot1 = strrchr(bcs[n_bc - 1]->d_name, '.');
            triple_len = dot1 - dot0 - 1;

            if (triple_len > 0) {
                triples[n_triples] = malloc(triple_len + 1);
                memcpy(triples[n_triples], dot0 + 1, triple_len);
                triples[n_triples][triple_len] = '\0';
                ucs_info("IFUNC found triple %s for ifunc %s\n",
                        triples[n_triples], param.name);
                n_triples++;
            }
        }
        free(bcs[n_bc - 1]);
        n_bc--;
    }
    free(bcs);

    if (n_triples == 0) {
        ucs_warn("IFUNC cannot find %s.{triple}.bc\n", param.name);
        return NULL;
    }

    *bcarc_size = sizeof(bc_archive_t);

    for (int i = 0; i < n_triples; i++) {
        snprintf(bc_filename, UCP_IFUNC_FILE_NAME_MAX, "%s/%s.%s.bc",
                 context->ifunc_lib_dir, param.name, triples[i]);
        bc_bufs[i] = jit_load_bc(&bc_sizes[i], bc_filename);

        /* BC size in the header, triple, and BC itself */
        *bcarc_size += sizeof(ifunc_sz_t) + UCP_IFUNC_TRIPLE_MAX + bc_sizes[i];
    }

    bcarc = (char*)malloc(*bcarc_size);
    ((bc_archive_t*)bcarc)->n_triples = n_triples;

    offset = sizeof(bc_archive_t) + n_triples * sizeof(ifunc_sz_t);

    for (int i = 0; i < n_triples; i++) {
        ((bc_archive_t*)bcarc)->bc_sizes[i] = bc_sizes[i];
        /* Pack the triple */
        memcpy(bcarc + offset, triples[i], strlen(triples[i]));
        free(triples[i]);
        offset += UCP_IFUNC_TRIPLE_MAX;
        /* Pack the bitcode */
        memcpy(bcarc + offset, bc_bufs[i], bc_sizes[i]);
        free(bc_bufs[i]);
        offset += bc_sizes[i];
    }

    ucs_assert(offset == *bcarc_size);

    return (bc_archive_t*)bcarc;
}

static inline char*
jit_get_bc_in_archive(bc_archive_t *bcarc, size_t *bc_size, const char *triple)
{
    const int n_triples = bcarc->n_triples;
    size_t offset = sizeof(bc_archive_t) + n_triples * sizeof(ifunc_sz_t);

    for (int i = 0; i < n_triples; i++) {
        if (strncmp((char*)bcarc + offset, triple, strlen(triple)) == 0) {
            *bc_size = bcarc->bc_sizes[i];
            return (char*)bcarc + offset + UCP_IFUNC_TRIPLE_MAX;
        }
        offset += UCP_IFUNC_TRIPLE_MAX + bcarc->bc_sizes[i];
    }

    return NULL;
}

static inline LLVMOrcThreadSafeModuleRef
jit_create_ts_mod(LLVMMemoryBufferRef bc)
{
    LLVMOrcThreadSafeContextRef ctx_ts;
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBool ret;
    char* msg = NULL;
    LLVMOrcThreadSafeModuleRef mod_ts;

    ctx_ts = LLVMOrcCreateNewThreadSafeContext();
    ucs_assert(ctx_ts != NULL);

    ctx = LLVMOrcThreadSafeContextGetContext(ctx_ts);
    ucs_assert(ctx != NULL);

    /* Ownership of the LLVMMemoryBuffer is transferred to the LLVMModule. */
    ret = LLVMParseIRInContext(ctx, bc, &mod, &msg);
    ucs_assert(!ret);
    (void)ret;  /* Supress -Werror when ucs_assert() is disabled. */

    mod_ts = LLVMOrcCreateNewThreadSafeModule(mod, ctx_ts);
    ucs_assert(mod_ts != NULL);

    LLVMOrcDisposeThreadSafeContext(ctx_ts);

    return mod_ts;
}

static inline LLVMOrcJITTargetAddress
pull_symbol_bc(LLVMOrcLLJITRef jit, const char *fmt, const char *ifunc_name)
{
    char symbol_name[UCP_IFUNC_SYMBOL_MAX];
    LLVMOrcJITTargetAddress addr;
    LLVMErrorRef err;

    snprintf(symbol_name, UCP_IFUNC_SYMBOL_MAX, fmt, ifunc_name);

    err = LLVMOrcLLJITLookup(jit, &addr, symbol_name);
    ucs_assert(err == NULL);
    (void)err;

    ucs_info("IFUNC pulled symbol %s @ 0x%lx\n", symbol_name, addr);
    return addr;
}

int handleError(LLVMErrorRef Err) {
  char *ErrMsg = LLVMGetErrorMessage(Err);
  fprintf(stderr, "LLVM Error: %s\n", ErrMsg);
  LLVMDisposeErrorMessage(ErrMsg);
  return 1;
}


/**
 * For bitcode-based ifunc, we need to support both on-disk and in-memory BCs.
 */
static inline ucs_status_t
register_ifunc_bc(ucp_context_h context,
                  ucp_ifunc_reg_param_t param,
                  ucp_ifunc_h ih,
                  char *msg_frame)
{
    ucs_status_t status;
    char ifunc_bc_deps_file[UCP_IFUNC_FILE_NAME_MAX];
    char* target_triple;
    size_t bcarc_size;
    bc_archive_t* bcarc;
    size_t bc_raw_size;
    char *bc_raw;
    FILE *fp;
    char *dep_name = NULL;
    char *deps_iterator;
    size_t dep_name_buf_size;
    ssize_t dep_name_size;
    ifunc_hdr_bc_t *hdr;
    LLVMMemoryBufferRef bc;
    LLVMOrcThreadSafeModuleRef mod;
    LLVMBool ret;
    LLVMErrorRef err;
    LLVMOrcJITTargetAddress addr_main, addr_payload_bound, addr_payload_init;

    status = UCS_OK;

    strncpy(ih->name, param.name, UCP_IFUNC_NAME_MAX - 1);
    ih->name[UCP_IFUNC_NAME_MAX - 1] = 0;

    snprintf(ifunc_bc_deps_file, UCP_IFUNC_FILE_NAME_MAX, "%s/%s.deps",
             context->ifunc_lib_dir, param.name);

    /**
     * The source process should also load the dependencies, in case
     * payload_bound() & paylod_init() also depend on external libraries.
     */
    if (msg_frame == NULL) {
        /* For on-disk bitcode & deps */
        bcarc = jit_create_bc_archive(context, &bcarc_size, param);

        if (bcarc == NULL) {
            ucs_warn("IFUNC could not create bitcode archive for %s\n",
                     param.name);
            status = UCS_ERR_IO_ERROR;
            goto err;
        }

        fp = fopen(ifunc_bc_deps_file, "r");
        if (fp == NULL) {
            ucs_warn("IFUNC failed to load %s\n", ifunc_bc_deps_file);
            status = UCS_ERR_IO_ERROR;
            goto err;
        }

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

        ih->pure = (param.pure != 0);
    } else {
        /* For in-memory bitcode & deps */
        hdr = (ifunc_hdr_bc_t*)msg_frame;

        /**
         * We save the bitcode archive and the dependencies, so that the target
         * process can register this ifunc using its name while not having the
         * bitcode files on-disk. The registration function thinks this is
         * double-registration.
         */
        bcarc_size = hdr->offset_deps - hdr->offset_bcarc;
        bcarc = malloc(bcarc_size);
        memcpy(bcarc, msg_frame + hdr->offset_bcarc, bcarc_size);

        ih->deps_size = hdr->frame_size - hdr->offset_deps - sizeof(ifunc_sig_t);
        ih->deps = malloc(ih->deps_size);
        memcpy(ih->deps, msg_frame + hdr->offset_deps, ih->deps_size);

        ih->pure = hdr->pure;
    }

    /* Extract the bitcode with the matching triple from the archive. */
    /* IFUNCTODO: what about aarch64-{redhat,pc,unknown}-linux-{gnu,gnueabi}? */
    target_triple = LLVMGetDefaultTargetTriple();
    bc_raw = jit_get_bc_in_archive(bcarc, &bc_raw_size, target_triple);

    if (bc_raw == NULL) {
        ucs_warn("IFUNC could not find bitcode for %s\n", target_triple);
        free(ih->deps);
        free(bcarc);
        status = UCS_ERR_NO_ELEM;
        goto err_free_triple;
    }

    deps_iterator = ih->deps;
    while ((ih->pure == 0) &&
           (deps_iterator != NULL) &&
           ((deps_iterator - ih->deps) < (ssize_t)ih->deps_size)) {
        ret = LLVMLoadLibraryPermanently(deps_iterator);
        if (ret) {
            ucs_warn("IFUNC could not load %s for ifunc %s\n", deps_iterator,
                     ih->name);
        }
        deps_iterator = strchr(deps_iterator, '\0') + 1;
    }

    bc = LLVMCreateMemoryBufferWithMemoryRangeCopy(bc_raw, bc_raw_size, param.name);
    mod = jit_create_ts_mod(bc);

    err = LLVMOrcLLJITAddLLVMIRModule(context->jit, context->jit_dlr, mod);
    if (err) {
        handleError(err);
    }
    ucs_assert(err == NULL);
    (void)err;

    addr_main = pull_symbol_bc(context->jit, "%s_main", param.name);
    ih->main = (ifunc_main_f)addr_main;
    ucs_assert(ih->main != NULL);

    addr_payload_bound = pull_symbol_bc(context->jit, "%s_payload_bound", param.name);
    ih->payload_bound = (ifunc_payload_bound_f)addr_payload_bound;
    ucs_assert(ih->payload_bound != NULL);

    addr_payload_init = pull_symbol_bc(context->jit, "%s_payload_init", param.name);
    ih->payload_init = (ifunc_payload_init_f)addr_payload_init;
    ucs_assert(ih->payload_init != NULL);

    ih->type = IFUNC_TYPE_BC;

    ih->bcarc = bcarc;
    ih->code_size = bcarc_size;

    ih->bc_sent = kh_init(ifunc_ep_set_t);

    /* Unused fields */
    ih->dlh = NULL;
    ih->code = NULL;
    ih->code_got_loc = NULL;
    ih->got_page = NULL;
    ih->code_end = NULL;
    ih->so_sent = NULL;

err_free_triple:
    LLVMDisposeMessage(target_triple);
err:
    return status;
}
#endif  /* HAVE_LLVM */

static inline void*
pull_symbol_so(void *dlh, const char *fmt, const char *ifunc_name)
{
    void *rsym;
    char symbol_name[UCP_IFUNC_SYMBOL_MAX];
    snprintf(symbol_name, UCP_IFUNC_SYMBOL_MAX, fmt, ifunc_name);
    rsym = dlsym(dlh, symbol_name);
    ucs_info("IFUNC pulled symbol %s @ %p, err = %s\n", symbol_name, rsym,
             dlerror());
    return rsym;
}

static inline ucs_status_t
register_ifunc_so(ucp_context_h context,
                  ucp_ifunc_reg_param_t param,
                  ucp_ifunc_h ih)
{
    ucs_status_t status;
    char ifunc_so_file[UCP_IFUNC_FILE_NAME_MAX];
    ifunc_patch_got_f patch_got;
    uint64_t pg_sz, code_sz, code_pg_sz;
    void *main_pg;
    void *old_got_page;
    int ret;

    status = UCS_OK;

    snprintf(ifunc_so_file, UCP_IFUNC_FILE_NAME_MAX, "%s/%s.so",
             context->ifunc_lib_dir, param.name);

    if (dlopen(ifunc_so_file, RTLD_NOW | RTLD_NOLOAD) == NULL) {
        ih->dlh = dlopen(ifunc_so_file, RTLD_NOW | RTLD_GLOBAL);
        if (ih->dlh == NULL) {
            ucs_warn("IFUNC dlopen of [%s] failed with error %s\n",
                     ifunc_so_file, dlerror());
            status = UCS_ERR_IO_ERROR;
            goto err;
        }
        ucs_info("IFUNC lib [%s] loaded successfully\n", ifunc_so_file);
    } else {
        /* IFUNCTODO: Do we allow double-registration? */
        ucs_info("IFUNC lib [%s] already loaded\n", ifunc_so_file);
    }

    strncpy(ih->name, param.name, UCP_IFUNC_NAME_MAX - 1);
    ih->name[UCP_IFUNC_NAME_MAX - 1] = 0;

    ih->main = pull_symbol_so(ih->dlh, "%s_preamble", param.name);
    ucs_assert(ih->main != NULL);

    ih->payload_bound = pull_symbol_so(ih->dlh, "%s_payload_bound", param.name);
    ucs_assert(ih->payload_bound != NULL);

    ih->payload_init = pull_symbol_so(ih->dlh, "%s_payload_init", param.name);
    ucs_assert(ih->payload_init != NULL);

    ih->code_got_loc = pull_symbol_so(ih->dlh, "%s_got", param.name);
    ucs_assert(ih->code_got_loc != NULL);
    ucs_assert((char*)ih->code_got_loc > (char*)ih->main);

    ih->code_end = pull_symbol_so(ih->dlh, "%s_code_end", param.name);
    ucs_assert(ih->code_end != NULL);
    ucs_assert((char*)ih->code_end > (char*)ih->code_got_loc);
    ih->code_size = (char*)ih->code_end - (char*)ih->main;

    ih->pure = (param.pure != 0);
    ih->type = IFUNC_TYPE_SO;

    ih->so_sent = kh_init(ifunc_ep_set_t);

    /* Unused fields */
    ih->bcarc = NULL;
    ih->deps = NULL;
    ih->deps_size = 0;
    ih->bc_sent = NULL;

    patch_got = pull_symbol_so(ih->dlh, "%s_patch_got", param.name);
    ucs_assert(patch_got != NULL);

    ucs_info("IFUNC %s, code size %d\n", param.name, ih->code_size);

    /* Make lib writeable to set GOT patch */
    pg_sz = sysconf(_SC_PAGESIZE);
    main_pg = (void*)(((uint64_t)ih->main / pg_sz) * pg_sz);
    code_sz = ih->code_size;
    code_pg_sz = (code_sz % pg_sz == 0) ? code_sz
                                        : ((code_sz / pg_sz) + 1) * pg_sz;
    ret = mprotect(main_pg, code_pg_sz, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (ret != 0) {
        ucs_warn("IFUNC failed to set rwx privilege on GOT ptr memory: %s\n",
                  strerror(errno));
        status = UCS_ERR_IO_ERROR;
        goto err;
    }

    /* IFUNCTODO: In the full implementation we don't need got_page. */
    old_got_page = *ih->code_got_loc;
    ih->got_page = patch_got();
    ucs_assert(ih->got_page != NULL);
    ucs_assert(ih->got_page == *(ih->code_got_loc));
    ucs_info("IFUNC patched ifunc %s GOT page from %p to %p\n", ih->name,
             old_got_page, ih->got_page);

    ret = mprotect(main_pg, code_pg_sz, PROT_READ | PROT_EXEC);
    if (ret != 0) {
        ucs_warn("IFUNC failed to set rx privilege on GOT ptr memory: %s\n",
                  strerror(errno));
    }

    /*
     * Now copy the GOT page-patched binary code over to ifunc_h's cache
     * N.B. we still need the OS-constructed GOT, so don't dlclose!
     */
    ret = posix_memalign(&(ih->code), pg_sz, ih->code_size);
    ucs_assert(ret == 0);
    memcpy(ih->code, ih->main, ih->code_size);
    ret = mprotect(ih->code, code_pg_sz, PROT_READ | PROT_WRITE | PROT_EXEC);
    ucs_assert(ret == 0);
    /* Make sure we are reading up-to-date instructions */
    ucs_clear_cache(ih->code, (void*)((char*)ih->code + ih->code_size));

    ih->payload_bound = ih->code + ((char*)ih->payload_bound - (char*)ih->main);
    ih->payload_init = ih->code + ((char*)ih->payload_init - (char*)ih->main);
    ih->code_got_loc = ih->code + ((char*)ih->code_got_loc - (char*)ih->main);
    ih->code_end = ih->code + ((char*)ih->code_end - (char*)ih->main);
    ih->main = ih->code;

err:
    return status;
}

/* Register an ifunc, on-disk or in-memory, SO or BC. */
static inline ucs_status_t
register_ifunc_aux(ucp_context_h context,
                   ucp_ifunc_reg_param_t param,
                   ucp_ifunc_h *ifunc_p,
                   char *msg_frame)
{
    ucs_status_t status;
    ucp_ifunc_h ih;
    khint_t map_iter;
    int ret, idx;

    /* Check if this ifunc is in registered state */
    map_iter = kh_get(ifunc_map_t, context->ifuncs_map, param.name);
    if (map_iter != kh_end(context->ifuncs_map)) {
        ucs_warn("IFUNC double registration of ifunc %s\n", param.name);
        idx = kh_val(context->ifuncs_map, map_iter);
        *ifunc_p = context->ifuncs[idx];
        return UCS_OK;
    }

    *ifunc_p = NULL;

    ih = calloc(1, sizeof(*ih));

    if (param.llvm_bc != 0) {
#ifdef HAVE_LLVM
        status = register_ifunc_bc(context, param, ih, msg_frame);
#else
        ucs_warn("IFUNC LLVM disabled, unable to load %s.bc\n", param.name);
        status = UCS_ERR_UNSUPPORTED;
#endif
    } else {
        status = register_ifunc_so(context, param, ih);
    }

    if (status != UCS_OK) {
        goto err;
    }

    /**
     * Handler creation done, update the registry and hash table.
     *
     * Warning: khash stores the address of the string key only, but it also
     *          uses the hash of the string to do internal calculations. Using
     *          the same address to refer to a different string key leads to
     *          strange behaviors, don't use ifunc_name here! Use a copy.
     */
    map_iter = kh_put(ifunc_map_t, context->ifuncs_map, ih->name, &ret);
    assert(ret != -1);
    for (idx = 0; idx < UCP_REG_IFUNC_MAX; idx++) {
        if (context->ifuncs[idx] == NULL) {
            context->ifuncs[idx] = ih;
            *ifunc_p = ih;
            kh_val(context->ifuncs_map, map_iter) = idx;
            return UCS_OK;
        }
    }

    status = UCS_ERR_EXCEEDS_LIMIT;
    ucs_warn("IFUNC ifunc registry is full, unable to register ifunc %s\n",
             param.name);

err:
    ucs_free(ih);
    return status;
}

/* Register an ifunc identified by its name, user-facing. */
ucs_status_t ucp_register_ifunc(ucp_context_h context,
                                ucp_ifunc_reg_param_t param,
                                ucp_ifunc_h *ifunc_p)
{
    return register_ifunc_aux(context, param, ifunc_p, NULL);
}

void ucp_deregister_ifunc(ucp_context_h context, ucp_ifunc_h ifunc_h)
{
    khint_t map_iter;

    if (ifunc_h == NULL) {
        ucs_warn("IFUNC deregistering NULL ifunc handler\n");
        return;
    }

    /**
     * IFUNCTODO: double deregistration could cause use-after-free when
     * accessing ifunc_h->name, how to plug this hole?
     */
    map_iter = kh_get(ifunc_map_t, context->ifuncs_map, ifunc_h->name);
    if (map_iter == kh_end(context->ifuncs_map)) {
        ucs_warn("IFUNC double ifunc deregistration\n");
        return;
    }

    ucs_info("IFUNC deregistering ifunc %s\n", ifunc_h->name);
    context->ifuncs[kh_val(context->ifuncs_map, map_iter)] = NULL;
    kh_del(ifunc_map_t, context->ifuncs_map, map_iter);

    if (ifunc_h->dlh != NULL) {
        dlclose(ifunc_h->dlh);
    }
#ifdef HAVE_LLVM
    free(ifunc_h->bcarc);
    free(ifunc_h->deps);
    kh_destroy(ifunc_ep_set_t, ifunc_h->bc_sent);
#endif
    free(ifunc_h->code);
    kh_destroy(ifunc_ep_set_t, ifunc_h->so_sent);
    free(ifunc_h);
}

ucs_status_t ucp_ifunc_msg_create(ucp_ifunc_h ifunc_h,
                                  void *source_args,
                                  size_t source_args_size,
                                  ucp_ifunc_msg_t *msg_p)
{
    size_t payload_size, frame_size;
    ifunc_hdr_bc_t *hdr_bc;
    ifunc_hdr_so_t *hdr_so;
    char *frame, *payload;
    int ret;

    ucs_assert(ifunc_h != NULL);
    ucs_assert(msg_p != NULL);

    payload_size = ifunc_h->payload_bound(source_args, source_args_size);

    if (ifunc_h->type == IFUNC_TYPE_BC) {
#ifdef HAVE_LLVM
        /* [header|payload|signal|bitcode|deps|signal] */
        frame_size = sizeof(ifunc_hdr_bc_t) + payload_size + sizeof(ifunc_sig_t)
                   + ifunc_h->code_size + ifunc_h->deps_size
                   + sizeof(ifunc_sig_t);
#else
        ucs_warn("IFUNC LLVM disabled, unable to create message for ifunc %s\n",
                 ifunc_h->name);
        return UCS_ERR_UNSUPPORTED;
#endif
    } else {
        /* [header|payload|signal|code|signal] */
        frame_size = sizeof(ifunc_hdr_so_t) + payload_size + sizeof(ifunc_sig_t)
                   + ifunc_h->code_size + sizeof(ifunc_sig_t);
    }

    /* Touches and clears the message frame memory. */
    frame = calloc(frame_size, 1);

    if (ifunc_h->type == IFUNC_TYPE_BC) {
        payload = frame + sizeof(ifunc_hdr_bc_t);
    } else {
        payload = frame + sizeof(ifunc_hdr_so_t);
    }

    ret = ifunc_h->payload_init(source_args, source_args_size, payload,
                                &payload_size);

    if (ret != 0) {
        ucs_warn("IFUNC payload_init failed with %d\n", ret);
        free(frame);
        return UCS_ERR_CANCELED;
    }

    /* Update the size of the message frame with the true payload size. */
    if (ifunc_h->type == IFUNC_TYPE_BC) {
        frame_size = sizeof(ifunc_hdr_bc_t) + payload_size + sizeof(ifunc_sig_t)
                   + ifunc_h->code_size + ifunc_h->deps_size
                   + sizeof(ifunc_sig_t);
    } else {
        frame_size = sizeof(ifunc_hdr_so_t) + payload_size + sizeof(ifunc_sig_t)
                   + ifunc_h->code_size + sizeof(ifunc_sig_t);
    }

    ucs_info("IFUNC payload size %ld, frame size %ld\n",
             payload_size, frame_size);


    if (ifunc_h->type == IFUNC_TYPE_BC) {
        hdr_bc               = (ifunc_hdr_bc_t*)frame;
        hdr_bc->frame_size   = frame_size;
        hdr_bc->offset_bcarc = sizeof(ifunc_hdr_bc_t) + payload_size
                             + sizeof(ifunc_sig_t);
        hdr_bc->offset_deps  = hdr_bc->offset_bcarc + ifunc_h->code_size;
        hdr_bc->type         = ifunc_h->type;
        hdr_bc->pure         = ifunc_h->pure;
        strncpy(hdr_bc->name, ifunc_h->name, UCP_IFUNC_NAME_MAX);
        hdr_bc->sig          = IFUNC_SIG_MAGIC;
        memcpy(frame + hdr_bc->offset_bcarc, ifunc_h->bcarc, ifunc_h->code_size);
        memcpy(frame + hdr_bc->offset_deps, ifunc_h->deps, ifunc_h->deps_size);

        *((ifunc_sig_t*)(frame + hdr_bc->offset_bcarc - sizeof(ifunc_sig_t))) = IFUNC_SIG_MAGIC;
    } else {
        hdr_so              = (ifunc_hdr_so_t*)frame;
        hdr_so->frame_size  = frame_size;
        hdr_so->offset_code = sizeof(ifunc_hdr_so_t) + payload_size + sizeof(ifunc_sig_t);
        hdr_so->offset_got  = (char*)ifunc_h->code_got_loc
                            - (char*)ifunc_h->main + hdr_so->offset_code;
        hdr_so->type        = ifunc_h->type;
        hdr_so->pure        = ifunc_h->pure;
        strncpy(hdr_so->name, ifunc_h->name, UCP_IFUNC_NAME_MAX);
        hdr_so->sig         = IFUNC_SIG_MAGIC;
        memcpy(frame + hdr_so->offset_code, ifunc_h->main, ifunc_h->code_size);

        *((ifunc_sig_t*)(frame + hdr_so->offset_code - sizeof(ifunc_sig_t))) = IFUNC_SIG_MAGIC;
    }

    msg_p->ifunc_h = ifunc_h;
    msg_p->frame = frame;
    msg_p->frame_size = frame_size;

    *((ifunc_sig_t*)(frame + frame_size - sizeof(ifunc_sig_t))) = IFUNC_SIG_MAGIC;

    return UCS_OK;
}

ucs_status_t ucp_ifunc_msg_get_payload_ptr(ucp_ifunc_msg_t *msg_p,
                                           void **payload,
                                           size_t *payload_size)
{
    void *frame;
    ucp_ifunc_h ifunc_h;

    ucs_assert(msg_p != NULL);

    ifunc_h = msg_p->ifunc_h;
    frame = msg_p->frame;

    if (ifunc_h->type == IFUNC_TYPE_BC) {
#ifdef HAVE_LLVM
        ifunc_hdr_bc_t *hdr_bc;
        hdr_bc = (ifunc_hdr_bc_t *)frame;

        /* [header|payload|signal|bitcode|deps|signal] */
        *payload = frame + sizeof(ifunc_hdr_bc_t);

        if (payload_size != NULL) {
            *payload_size = hdr_bc->offset_bcarc - sizeof(ifunc_hdr_bc_t)
                          - sizeof(ifunc_sig_t);
        }
#else
        ucs_warn("IFUNC LLVM disabled, unable to get pointer for ifunc %s\n",
                 ifunc_h->name);
        return UCS_ERR_UNSUPPORTED;
#endif
    } else {
        ifunc_hdr_so_t *hdr_so;
        hdr_so = (ifunc_hdr_so_t *)frame;

        /* [header|payload|signal|code|signal] */
        *payload = frame + sizeof(ifunc_hdr_so_t);
        if (payload_size != NULL) {
            *payload_size = hdr_so->offset_code - sizeof(ifunc_hdr_so_t)
                          - sizeof(ifunc_sig_t);
        }
    }

    return UCS_OK;
}

void ucp_ifunc_msg_free(ucp_ifunc_msg_t msg)
{
    /* IFUNCTODO: This doesn't actually modify msg! */
    msg.ifunc_h = NULL;
    msg.frame_size = 0;
    free(msg.frame);
}

ucs_status_t ucp_ifunc_send_nbix(ucp_ep_h ep,
                                 ucp_ifunc_msg_t msg,
                                 uint64_t remote_addr,
                                 ucp_rkey_h rkey)
{
    ucs_status_t status;
#ifdef CACHE_ENABLED
    int kh_ret;
    if (msg.ifunc_h->type == IFUNC_TYPE_BC) {
#ifdef HAVE_LLVM
        (void)kh_put(ifunc_ep_set_t, msg.ifunc_h->bc_sent, (intptr_t)ep, &kh_ret);
        if (ucs_likely(kh_ret == 0)) {
            /* The original msg is untouched */
            msg.frame_size = ((ifunc_hdr_bc_t*)msg.frame)->offset_bcarc;
        }
#else
        ucs_warn("IFUNC LLVM disabled, unable to send BC ifunc %s\n", msg.ifunc_h->name);
        return UCS_ERR_UNSUPPORTED;
#endif  /* HAVE_LLVM */
    } else {
        (void)kh_put(ifunc_ep_set_t, msg.ifunc_h->so_sent, (intptr_t)ep, &kh_ret);
        if (ucs_likely(kh_ret == 0)) {
            /* The original msg is untouched */
            msg.frame_size = ((ifunc_hdr_so_t*)msg.frame)->offset_code;
        }
    }
#endif

    status = ucp_put_nbi(ep, msg.frame, msg.frame_size, remote_addr, rkey);

    return status;
}

static inline int
arch_safe_cmp_wait_u8(volatile uint8_t *ptr, uint8_t val)
{
#ifdef __ARM_ARCH
    /* IFUNCTODO: Better than the WFE impl in ucs? */
    uint8_t tmp = 0;
    asm volatile("ldaxrb %w0, [%1]\n"
                 "cmp %w2, %w0, uxtb\n"
                 "b.eq 1f\n"
                 "wfe\n"
                 "1:\n"
                 : "=&r"(tmp)
                 : "r"(ptr),
                   "r"(val)
                 : "memory");
    return (tmp != val);
#else
    ucs_arch_wait_mem((uint8_t*)ptr);
    return (*ptr != val);
#endif /* __ARM_ARCH */
}

void flush_cb(void *request, ucs_status_t status)
{
    (void)request;
    (void)status;
}

ucs_status_t ucp_poll_ifunc(ucp_context_h context,
                            void *buffer,
                            size_t buffer_size,
                            void *target_args,
                            ucp_worker_h worker)
{
    ifunc_hdr_so_t *hdr_so;
    ifunc_hdr_bc_t *hdr_bc;
    ifunc_sig_t *hdr_sig;
    ifunc_sig_t *trailer_sig;
    khint_t map_iter;
    ucp_ifunc_h ifunc_h;
    void* payload;
    size_t payload_size;
    ucs_status_t status;
    ucp_ifunc_reg_param_t p_autoreg;


    /* In case someone changed the headers w/o making modifications here */
    ucs_assert(sizeof(ifunc_hdr_so_t) == sizeof(ifunc_hdr_bc_t));

    hdr_so = buffer;
    hdr_bc = buffer;
    hdr_sig = &(hdr_so->sig);
    ifunc_h = NULL;

    // IFUNCTODO: hdr_sig should be volatile
    if (*hdr_sig != IFUNC_SIG_MAGIC) {
        status = UCS_ERR_NO_MESSAGE;
        ucp_worker_progress(worker);
        goto err;
    }

    /* This ifunc message will be handeled by us, regardless of the result. */
    *hdr_sig = 0;

    /* Required to read hdr correctly, volatile isn't enough */
    ucs_memory_cpu_load_fence();

    if (hdr_so->type == IFUNC_TYPE_BC) {
#ifndef HAVE_LLVM
        ucs_warn("IFUNC LLVM disabled, discarding ifunc %s\n", hdr_so->name);
        status = UCS_ERR_UNSUPPORTED;
        goto err;
#endif
    }

    /* IFUNCTODO: buffer_size is used here only */
    if (ucs_unlikely(buffer_size < hdr_so->frame_size)) {
        ucs_warn("IFUNC ifunc %s rejected, message too long: %u > %lu\n",
                 hdr_so->name, hdr_so->frame_size, buffer_size);
        status = UCS_ERR_CANCELED;
        goto err;
    }

    /* Find the trailer signal location and wait for the entire message */
    trailer_sig = buffer + hdr_so->frame_size - sizeof(ifunc_sig_t);

    map_iter = kh_get(ifunc_map_t, context->ifuncs_map, hdr_so->name);
#ifdef CACHE_ENABLED
    if (ucs_unlikely(map_iter == kh_end(context->ifuncs_map))) {
        ucs_info("IFUNC received unknown ifunc %s\n", hdr_so->name);
    } else {
        /* For registered ifuncs, use shortened message length */
        if (hdr_so->type == IFUNC_TYPE_BC) {
            trailer_sig = buffer + hdr_bc->offset_bcarc - sizeof(ifunc_sig_t);
        } else {
            trailer_sig = buffer + hdr_so->offset_code - sizeof(ifunc_sig_t);
        }
    }
#endif

    if (*trailer_sig != IFUNC_SIG_MAGIC) {
        /* IFUNCTODO: this happens frequent enough, wait or goto err? */
        ucs_info("IFUNC waiting on trailer signal of ifunc %s\n", hdr_so->name);
        while (arch_safe_cmp_wait_u8(trailer_sig, IFUNC_SIG_MAGIC)) {
            ucp_worker_progress(worker);
        };
    }

    *trailer_sig = 0;

    /* Required to read hdr correctly, volatile isn't enough */
    ucs_memory_cpu_load_fence();

#ifdef DO_NOT_JIT_EXEC
    *((size_t*)target_args) += 1;
    return UCS_OK;
#endif

    /* Now that we have the entire message, we can do registration/execution */
    if (ucs_unlikely(map_iter == kh_end(context->ifuncs_map))) {
        p_autoreg.name = hdr_so->name;
        p_autoreg.pure = hdr_so->pure;
        p_autoreg.llvm_bc = (hdr_so->type == IFUNC_TYPE_BC);
        if (register_ifunc_aux(context, p_autoreg, &ifunc_h, buffer) != UCS_OK) {
            ucs_warn("IFUNC failed to auto-register unknown ifunc %s\n",
                      hdr_so->name);
            status = UCS_ERR_CANCELED;
            goto err;
        }
    } else {
        ifunc_h = context->ifuncs[kh_val(context->ifuncs_map, map_iter)];
    }

    if (hdr_so->type == IFUNC_TYPE_SO) {
        payload = (char*)buffer + sizeof(ifunc_hdr_so_t);
        payload_size = hdr_so->offset_code - sizeof(ifunc_hdr_so_t)
                     - sizeof(ifunc_sig_t);
    } else {
        payload = (char*)buffer + sizeof(ifunc_hdr_bc_t);
        payload_size = hdr_bc->offset_bcarc - sizeof(ifunc_hdr_bc_t)
                     - sizeof(ifunc_sig_t);
    }
    ifunc_h->main(payload, payload_size, target_args);

    status = UCS_OK;

err:
    return status;
}


ucs_status_t ucp_register_ifunc_im(ucp_context_h context,
                                   ucp_ifunc_reg_param_t param,
                                   ucp_llvm_bc_t *bcs,
                                   int n_bcs,
                                   char **deps,
                                   int n_deps,
                                   ucp_ifunc_h *ifunc_p)
{
    ifunc_sz_t frame_size, bcarc_size, offset;
    char* frame;
    ifunc_hdr_bc_t* hdr;
    bc_archive_t* bcarc;
    ucs_status_t s;

    /* Calculate the size of the pseudo message frame */

    /* Header, payload(0), first signal */
    frame_size = sizeof(ifunc_hdr_bc_t) + 0 + sizeof(ifunc_sig_t);

    /* Bitcode archive */
    bcarc_size = sizeof(bc_archive_t) + n_bcs * sizeof(ifunc_sz_t);
    for (int i = 0; i < n_bcs; i++) {
        bcarc_size += UCP_IFUNC_TRIPLE_MAX + bcs[i].size;
    }
    frame_size += bcarc_size;

    /* Deps */
    for (int i = 0; i < n_deps; i++) {
        frame_size += strlen(deps[i]) + 1;  /* With '\0' */
    }

    /* Second signal */
    frame_size += sizeof(ifunc_sig_t);

    frame = calloc(frame_size, 1);

    /* Construct the pseudo message frame */
    offset = 0;

    /* Initialize the header */
    hdr = (ifunc_hdr_bc_t*)frame;
    hdr->frame_size = frame_size;
    hdr->offset_bcarc = sizeof(ifunc_hdr_bc_t) + sizeof(ifunc_sig_t);
    hdr->offset_deps = hdr->offset_bcarc + bcarc_size;
    hdr->type = IFUNC_TYPE_BC;
    hdr->pure = (n_deps == 0);
    strncpy(hdr->name, param.name, UCP_IFUNC_NAME_MAX - 1);
    hdr->sig = IFUNC_SIG_MAGIC;

    offset += sizeof(ifunc_hdr_bc_t);

    *((ifunc_sig_t*)(frame + offset)) = IFUNC_SIG_MAGIC;

    offset += sizeof(ifunc_sig_t);

    /* Fill the bitcode archive */
    bcarc = (bc_archive_t*)(frame + offset);

    bcarc->n_triples = n_bcs;
    offset += sizeof(ifunc_sz_t);

    for (int i = 0; i < n_bcs; i++) {
        bcarc->bc_sizes[i] = bcs[i].size;
        offset += sizeof(ifunc_sz_t);
    }

    for (int i = 0; i < n_bcs; i++) {
        strncpy(((char*)frame + offset), bcs[i].triple, UCP_IFUNC_TRIPLE_MAX - 1);
        offset += UCP_IFUNC_TRIPLE_MAX;
        memcpy(((char*)frame + offset), bcs[i].bc, bcs[i].size);
        offset += bcs[i].size;
    }

    ucs_assert(offset == hdr->offset_deps);

    for (int i = 0; i < n_deps; i++) {
        memcpy(((char*)frame + offset), deps[i], strlen(deps[i]));
        offset += strlen(deps[i]) + 1;
    }

    *((ifunc_sig_t*)(frame + offset)) = IFUNC_SIG_MAGIC;

    ucs_assert(frame_size == offset + 1);

    s = register_ifunc_aux(context, param, ifunc_p, frame);

    free(frame);

    return s;
}
