##
# Dynamic adaptive pointer chasing application
#
# Author: Valentin Churavy
# Massachussetts Institute of Technology, 2022
#
# Original implementation:
# Author: Luis E. P.
# Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
#
##
using UCX

include("UCXAsap.jl")
import .UCXAsap

include("IFunc.jl")
import .IFunc 

include("unsafe.jl")
include("common.jl")

function bound(source_args::Ptr{Cvoid}, source_args_size::Csize_t)::Csize_t
	return source_args_size
end

function init(source_args::Ptr{Cvoid}, source_args_size::Csize_t, payload::Ptr{Cvoid}, payload_size::Csize_t)::Cint
    src = Base.unsafe_convert(Ptr{SrcArgs}, source_args)
    dst = Base.unsafe_convert(Ptr{SrcArgs}, payload)

    unsafe_setfield!(dst, :depth, unsafe_getfield(src, :depth))
    unsafe_setfield!(dst, :addr, unsafe_getfield(src, :addr))
    unsafe_setfield!(dst, :dest, unsafe_getfield(src, :dest))

    return Cint(0)
end

function main(payload::Ptr{Cvoid}, payload_size::Csize_t, target_args::Ptr{Cvoid})::Cvoid
    src_args = Base.unsafe_convert(Ptr{SrcArgs}, payload)
    tgt_args = Base.unsafe_convert(Ptr{TgtArgs}, target_args)

    depth = unsafe_getfield(src_args, :depth)
    addr  = unsafe_getfield(src_args, :addr)
    dest  = unsafe_getfield(src_args, :dest)%Int64

    data = unsafe_getfield(tgt_args, :data)
    this_shard = unsafe_getfield(tgt_args, :shard_id)
    server_qty = unsafe_getfield(tgt_args, :machine_qty) - 1
    shard_size = unsafe_getfield(tgt_args, :shard_size)
    worker = unsafe_getfield(tgt_args, :wrkr)

    max_index = server_qty * shard_size

    # addr = mod(addr, max_index)
    # @assert addr in 0:(max_index-1)
    which_shard = unsafe_div(addr, shard_size)

    if which_shard == this_shard # data is local
		addr = unsafe_rem(addr, shard_size) # get address within a shard

        next = Base.unsafe_load(data, (addr + 1)%Int64) # Julia is 1-based
        # @static if VERBOSE
        #     @printf("Result is %lu\n", next)
        # end

        # Trim down next to fit
        # next = unsafe_rem(next, max_index)
        # @assert next in 0:(max_index-1)

        depth -= 1

        if depth == 0
            dest += 1 # `dest` is a offset, turn to 1-based
            # write to original
            heap_u64 = reinterpret(UInt64, unsafe_getfield(tgt_args, :heap))
            heap_aslr = UInt64(heap_u64 + Base.unsafe_load(unsafe_getfield(tgt_args, :aslr_diff), dest))

            result_msg = unsafe_getfield(tgt_args, :result_msg)
            result_payload, _ = IFunc.ucp_ifunc_msg_get_payload_ptr(result_msg)

            Base.unsafe_store!(Base.unsafe_convert(Ptr{UInt64}, result_payload), next)

            ep = Base.unsafe_load(unsafe_getfield(tgt_args, :ep), dest)
            rmt_rkey = Base.unsafe_load(unsafe_getfield(tgt_args, :rmt_rkey), dest)

            s = IFunc.ucp_ifunc_send_nbix(ep, result_msg, heap_aslr, rmt_rkey)
            
            if s != UCX.API.UCS_OK
                flush = unsafe_getfield(tgt_args, :flush)
                assume(flush != C_NULL)
                ccall(flush, UCX.API.ucs_status_t, (UCX.API.ucp_worker_h, UCX.API.ucp_ep_h), worker, ep)
            end
            return
        end

        # update values
        unsafe_setfield!(src_args, :depth, depth)
        unsafe_setfield!(src_args, :addr, next)

        which_shard = unsafe_div(next, shard_size)
        if which_shard == this_shard 
            # shortcircuit
            main(payload, payload_size, target_args)
            return
        end

    end

    # forward to other ep
    destination = (which_shard%Int64 + 1) + 1 # client is 1, first server is 2
    chaser_msg  = unsafe_getfield(tgt_args, :chaser_msg)
    tgt_payload, _ = IFunc.ucp_ifunc_msg_get_payload_ptr(chaser_msg)

    msg_args = Base.unsafe_convert(Ptr{SrcArgs}, tgt_payload)
    unsafe_setfield!(msg_args, :depth, depth)
    unsafe_setfield!(msg_args, :addr, addr)
    unsafe_setfield!(msg_args, :dest, dest % UInt64)

    # @static if VERBOSE
    #     @printf("Remote addr:  %lu\n", msg_args.addr)
	# 	@printf("Remote dest:  %lu\n", msg_args.dest)
	# 	@printf("Remote depth: %lu\n", msg_args.depth)
    # end

    heap_u64 = reinterpret(UInt64, unsafe_getfield(tgt_args, :heap))
    heap_aslr = UInt64(heap_u64 + Base.unsafe_load(unsafe_getfield(tgt_args, :aslr_diff), destination))

    ep = Base.unsafe_load(unsafe_getfield(tgt_args, :ep), destination)
    rmt_rkey = Base.unsafe_load(unsafe_getfield(tgt_args, :rmt_rkey), destination)

    s = IFunc.ucp_ifunc_send_nbix(ep, chaser_msg, heap_aslr, rmt_rkey)

    if s != UCX.API.UCS_OK
        flush = unsafe_getfield(tgt_args, :flush)
        assume(flush != C_NULL)
        ccall(flush, UCX.API.ucs_status_t, (UCX.API.ucp_worker_h, UCX.API.ucp_ep_h), worker, ep)
    end

    return nothing
end

const fns = (
    (bound, Tuple{Ptr{Cvoid}, Csize_t}),
    (init, Tuple{Ptr{Cvoid}, Csize_t, Ptr{Cvoid}, Csize_t}),
    (main, Tuple{Ptr{Cvoid}, Csize_t, Ptr{Cvoid}})
)

IFunc.Compiler.emit(fns, "jchaser", "$(Sys.ARCH)-unknown-linux-gnu")
