# This file is a part of UCX.jl. License is MIT: https://github.com/JuliaParallel/UCX.jl/blob/main/LICENSE.md
# Author: Valentin Churavy, 2022

module UCXAsap
using UCX
import UCX: @check
import UCX.API: libucp, ucs_status_t, ucp_context_h, ucp_worker_h, ucp_ep_h, ucp_rkey_h

const ucp_ifunc_h = Ptr{Cvoid}

struct ucp_ifunc_msg_t
    ifunc_h::ucp_ifunc_h
    frame::Ptr{Cvoid}
    frame_size::Csize_t
end

struct ucp_ifunc_reg_param_t
    name::Ptr{Cchar}
    pure::Cint
    llvm_bc::Cint
end

struct ucp_llvm_bc_t
    triple::Ptr{Cchar}
    bc::Ptr{Cvoid}
    size::UInt32
end

function ucp_register_ifunc_im(ctx, param, bcs::Vector{ucp_llvm_bc_t}, deps::Vector{Ptr{UInt8}})
    r_ifunc = Ref{ucp_ifunc_h}()
    @check ccall((:ucp_register_ifunc_im, libucp), ucs_status_t,
        (ucp_context_h, ucp_ifunc_reg_param_t, Ptr{ucp_llvm_bc_t}, Cint, Ptr{Ptr{Cchar}}, Cint, Ptr{ucp_ifunc_h}),
        ctx, param, bcs, length(bcs), deps, length(deps), r_ifunc)
    return r_ifunc[]
end

function register_ifunc_bc(ctx, name, bcs::Vector{Pair{String, Vector{UInt8}}}, deps::Vector{String}=String[]; pure=false) 
    cname = Base.cconvert(Cstring, name)
    GC.@preserve deps cname bcs begin
        raw_bcs = map(bcs) do (triple, bc)
            ucp_llvm_bc_t(pointer(triple), pointer(bc), length(bc))
        end
        raw_deps = map(dep->pointer(dep), deps)
        param = UCXAsap.ucp_ifunc_reg_param_t(Base.unsafe_convert(Cstring, cname), pure, #=llvm_bc=#1)
        return ucp_register_ifunc_im(ctx, param, raw_bcs, raw_deps)
    end
end

function ucp_ifunc_msg_create(handle, source_args, source_args_size)
    r_msg = Ref{ucp_ifunc_msg_t}()
    @check ccall((:ucp_ifunc_msg_create, libucp), ucs_status_t, (ucp_ifunc_h, Ptr{Cvoid}, Csize_t, Ptr{ucp_ifunc_msg_t}), handle, source_args, source_args_size, r_msg)
    return r_msg[]
end

function ucp_poll_ifunc(context, buffer, buffer_size, target_args, worker)
    return ccall((:ucp_poll_ifunc, libucp), ucs_status_t, (ucp_context_h, Ptr{Cvoid}, Csize_t, Ptr{Cvoid}, ucp_worker_h), context, buffer, buffer_size, target_args, worker)
end

function ucp_ifunc_msg_free(msg)
    @check ccall((:ucp_ifunc_msg_free, libucp), ucs_status_t, (ucp_ifunc_msg_t,), msg)
    return nothing
end

function ucp_deregister_ifunc(ctx, handle)
   @check ccall((:ucp_deregister_ifunc, libucp), ucs_status_t, (ucp_context_h, ucp_ifunc_h), ctx, handle)
   return nothing
end

function ucp_ifunc_msg_get_payload_ptr(msg)
    r_payload = Ref{Ptr{Cvoid}}()
    r_size = Ref{Csize_t}()

    @check ccall((:ucp_ifunc_msg_get_payload_ptr, libucp), ucs_status_t, (Ptr{ucp_ifunc_msg_t}, Ptr{Ptr{Cvoid}}, Ptr{Csize_t}), Ref(msg), r_payload, r_size)
    return (r_payload[], r_size[])
end

function ucp_ifunc_send_nbix(ep, msg, remote_addr, rkey)
    ccall((:ucp_ifunc_send_nbix, libucp), ucs_status_t, (ucp_ep_h, ucp_ifunc_msg_t, UInt64, ucp_rkey_h), ep, msg, remote_addr, rkey)
end

end # UCXAsap
