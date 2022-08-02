# This file is a part of UCX.jl. License is MIT: https://github.com/JuliaParallel/UCX.jl/blob/main/LICENSE.md
# Author: Valentin Churavy, 2022
module IFunc
    using UCX
    import UCX.API: ucs_status_t, ucp_ep_h, ucp_rkey_h

    import ..UCXAsap: ucp_ifunc_msg_t

    # Need to be relocatable
    @inline function ucp_ifunc_msg_get_payload_ptr(msg)
        r_payload = Ref{Ptr{Cvoid}}()
        # r_size = Ref{Csize_t}()

        GC.@preserve r_payload begin
            Base.llvmcall(("""
                declare signext i8 @ucp_ifunc_msg_get_payload_ptr({ i64, i64, i64 }*, i64, i64) #1

                define i8 @entry([ 3 x i64 ], i64, i64) #0 {
                    %msg = alloca { i64, i64, i64 }, align 16
                    %msg_ = bitcast { i64, i64, i64 }* %msg to [ 3 x i64 ]*
                    store [ 3 x i64 ] %0, [ 3 x i64 ]* %msg_, align 8

                    %s = call signext i8 @ucp_ifunc_msg_get_payload_ptr({ i64, i64, i64 }* nonnull align 16 %msg, i64 %1, i64 %2)
                    ret i8 %s
                }

                attributes #0 = { alwaysinline }
                attributes #1 = { "frame-pointer"="none" "no-trapping-math"="true"}""", "entry"),
                ucs_status_t, Tuple{ucp_ifunc_msg_t, Ptr{Ptr{Cvoid}}, Ptr{Csize_t}}, msg, Base.unsafe_convert(Ptr{Ptr{Cvoid}}, r_payload), Base.unsafe_convert(Ptr{Csize_t}, C_NULL))
        end
    #     ccall("extern ucp_ifunc_msg_get_payload_ptr", llvmcall, ucs_status_t, (Ptr{ucp_ifunc_msg_t}, Ptr{Ptr{Cvoid}}, Ptr{Csize_t}), Ref(msg), r_payload, r_size)
        return r_payload[], 0
    end

@static if Sys.ARCH === :x86_64
    @inline function ucp_ifunc_send_nbix(ep, msg, remote_addr, rkey)
        # ABI mismatch between ccall and llvmcall
        # ccall("extern ucp_ifunc_send_nbix", llvmcall, ucs_status_t, (ucp_ep_h, ucp_ifunc_msg_t, UInt64, ucp_rkey_h), ep, msg, remote_addr, rkey)
        Base.llvmcall(("""
            declare signext i8 @ucp_ifunc_send_nbix(i64, { i64, i64, i64 }* byval({ i64, i64, i64 }) align 8, i64, i64) #1

            define i8 @entry(i64, [ 3 x i64 ], i64, i64) #0 {
                %msg = alloca { i64, i64, i64 }, align 16
                %msg_ = bitcast { i64, i64, i64 }* %msg to [ 3 x i64 ]*
                store [ 3 x i64 ] %1, [ 3 x i64 ]* %msg_, align 8
                
                %s = call signext i8 @ucp_ifunc_send_nbix(i64 %0, { i64, i64, i64 }* nonnull byval({ i64, i64, i64 }) align 16 %msg, i64 %2, i64 %3)
                ret i8 %s
            }

            attributes #0 = { alwaysinline }
            attributes #1 = { "frame-pointer"="none" "no-trapping-math"="true"}""", "entry"),
            ucs_status_t, Tuple{ucp_ep_h, ucp_ifunc_msg_t, UInt64, ucp_rkey_h}, ep, msg, remote_addr, rkey)
    end
else
    @inline function ucp_ifunc_send_nbix(ep, msg, remote_addr, rkey)
        # ABI mismatch between ccall and llvmcall
        # ccall("extern ucp_ifunc_send_nbix", llvmcall, ucs_status_t, (ucp_ep_h, ucp_ifunc_msg_t, UInt64, ucp_rkey_h), ep, msg, remote_addr, rkey)
        Base.llvmcall(("""
            declare signext i8 @ucp_ifunc_send_nbix(i64, { i64, i64, i64 }*, i64, i64) #1

            define i8 @entry(i64, [ 3 x i64 ], i64, i64) #0 {
                %msg = alloca { i64, i64, i64 }, align 16
                %msg_ = bitcast { i64, i64, i64 }* %msg to [ 3 x i64 ]*
                store [ 3 x i64 ] %1, [ 3 x i64 ]* %msg_, align 8
                
                %s = call signext i8 @ucp_ifunc_send_nbix(i64 %0, { i64, i64, i64 }* nonnull %msg, i64 %2, i64 %3)
                ret i8 %s
            }

            attributes #0 = { alwaysinline }
            attributes #1 = { "frame-pointer"="none" "no-trapping-math"="true"}""", "entry"),
            ucs_status_t, Tuple{ucp_ep_h, ucp_ifunc_msg_t, UInt64, ucp_rkey_h}, ep, msg, remote_addr, rkey)
    end
end # @static

    function safe_print(val)
        ccall("extern jl_", llvmcall, Cvoid, (Any,), val)
    end

    module Compiler
        using GPUCompiler
        using LLVM
        include(joinpath(dirname(pathof(GPUCompiler)), "..", "test", "definitions", "native.jl"))

        function emit_fn((f, tt), name, ctx)
            job, _ = native_job(f, tt)
            ir, meta = GPUCompiler.codegen(:llvm, job; validate=false, ctx)
            LLVM.name!(meta.entry, name)

            if haskey(globals(ir), "llvm.compiler.used")
                unsafe_delete!(ir, globals(ir)["llvm.compiler.used"])
            end

            LLVM.linkage!(meta.entry, LLVM.API.LLVMExternalLinkage)

            return ir
        end

        function emit((bound, init, main), name, arch)
            JuliaContext() do ctx
                ir_bound = emit_fn(bound, string(name, "_payload_bound"), ctx)
                ir_init = emit_fn(init, string(name, "_payload_init"), ctx)
                ir_main = emit_fn(main, string(name, "_main"), ctx)

                LLVM.link!(ir_bound, ir_init)
                LLVM.link!(ir_bound, ir_main)

                ModulePassManager() do mpm
                    tail_call_elimination!(mpm)
                    global_dce!(mpm)
                    gvn!(mpm)
                    scalar_repl_aggregates_ssa!(mpm)
                    load_store_vectorizer!(mpm)
                    run!(mpm, ir_bound)
                end

                open("$name.$arch.bc", "w") do io
                    write(io, ir_bound)
                end
            end
        end

        function fake_crosscompile(file, arch, name)
            # run(`opt --mtriple $arch $file -o $name.$arch.bc`)
            Context() do ctx
                mod = parse(LLVM.Module, open(read, file); ctx)
                triple!(mod, arch)
                datalayout!(mod, "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128") # TODO only aarch64
                write("$name.$arch.bc", mod)
            end
        end
    end
end
