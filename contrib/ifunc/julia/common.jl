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
# Note need to match the defintions in `pointer_chase.h`
struct SrcArgs
    depth::UInt64
    addr::UInt64
    dest::UInt64
end

# Must stay in alignment with `pointer_chase.h`
# `pc_tgt_args`
struct TgtArgs
    shard_id::Int32
    data::Ptr{UInt64}
    ep::Ptr{UCX.API.ucp_ep_h}
    wrkr::UCX.API.ucp_worker_h
    chaser_msg::UCXAsap.ucp_ifunc_msg_t
    result_msg::UCXAsap.ucp_ifunc_msg_t
    rmt_rkey::Ptr{UCX.API.ucp_rkey_h}
    flush::Ptr{Cvoid}
    aslr_diff::Ptr{Cptrdiff_t}
    heap::Ptr{Cvoid}
    machine_qty::Cint
    shard_size::UInt64
    run::Bool
end

const AM_SIG_MAGIC = 0x55

const am_sig_t = UInt8
const am_func_id = UInt8

struct am_msg_t
	id::am_func_id
	src_args::SrcArgs
    sig::am_sig_t
end 

struct result_msg_t
	result::UInt64
	sig::am_sig_t
end

function flush_callback(request, status)
    return nothing
end
cflush(worker, endpoint) = cflush(worker.handle, endpoint.handle)
function cflush(worker::UCX.API.ucp_worker_h, endpoint::UCX.API.ucp_ep_h)
    # if flushing worker, please pass endpoints as NULL
    cb = @cfunction(flush_callback, Cvoid, (Ptr{Cvoid}, UCX.API.ucs_status_t))
    if endpoint != C_NULL
        request = UCX.API.ucp_ep_flush_nb(endpoint, 0, cb)
    else
        request = UCX.API.ucp_worker_flush_nb(worker, 0, cb)
    end

    if request == C_NULL
        return UCX.API.UCS_OK
    elseif UCX.UCS_PTR_IS_ERR(request)
        return UCX.UCS_PTR_STATUS(request);
    else
        status = UCX.API.UCS_INPROGRESS
        while status == UCX.API.UCS_INPROGRESS
            UCX.API.ucp_worker_progress(worker)
            status = UCX.API.ucp_request_check_status(request)
        end
        UCX.API.ucp_request_free(request)
        return status
    end
end
