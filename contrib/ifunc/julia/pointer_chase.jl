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
using Sockets
using Random
using UnsafeAtomics
using CEnum

import TypedTables: Table
import CSV
import Dates

include("UCXAsap.jl")
import .UCXAsap

include("common.jl")
include("unsafe.jl")

ptroffset(ptr::Ptr{T}, offset) where T = ptr+offset*sizeof(T)

const VERBOSE = false

@info "Using" libucp=UCX.API.libucp libucs=UCX.API.libucs

@cenum TEST_MODE::Int begin
    TEST_GETS  = 1
    TEST_AM    = 2
    TEST_IFUNC = 3
    TEST_IFUNC_JL = 4
end

const PORT = Int32(13337)
const HEAP_SIZE_LOG = 30
const HEAP_SIZE = UInt64(1) << HEAP_SIZE_LOG
const HALF_HEAP = UInt64(1) << (HEAP_SIZE_LOG-1)

const ITERATIONS = 2097152 # (2^21)
const MAX_DEPTH_LOG = 12
const DEPTH = 1000
const RETRIES = 10

const CLIENT = 1
const SERVER = 2

struct State
    whoami::Int
    machine_qty::Int
    shard_size::Int
    datapoints::Int
    worker::UCX.UCXWorker
    mh::UCX.Memory
    heap::Ptr{UInt8}
    heap_rmt::Vector{Ptr{Cvoid}}
    aslr_diff::Vector{Cptrdiff_t}
    ep::Vector{UCX.UCXEndpoint}
    rmt_rkey::Vector{UCX.RemoteKey}
    connections::Vector{TCPSocket}
end

function ptr_to_u64_aslr(state, ptr::Ptr{T}, id) where T
    ptr_u64 = reinterpret(UInt64, ptr)
    UInt64(ptr_u64 + state.aslr_diff[id])
end

function flush(state, flush_worker, id)
    # Base.flush on a UCX object is a non-blocking flush, returning a UCXRequest
    req = if flush_worker
        Base.flush(state.worker)
    else
        Base.flush(state.ep[id])
    end
    wait(req)
end

@noinline function poll_am(buffer, buffer_size, tgt_args, worker)
    hdr = Base.unsafe_convert(Ptr{am_msg_t}, buffer)
    hdr_sig = field_ptr(hdr, :sig) # volatile

    if UnsafeAtomics.load(hdr_sig) != AM_SIG_MAGIC
        status = UCX.API.UCS_ERR_NO_MESSAGE
        UCX.API.ucp_worker_progress(worker)
        return status
    end

    # TODO: should the SIGNAL be sent after the actual payload? Two puts?

    # Claim this ifunc message to be handeled by us, no matter the result.
    UnsafeAtomics.store!(hdr_sig, 0x00)

    # TODO: add unlikely
    if buffer_size < sizeof(am_msg_t)
        @error "AM rejected, message too long" hdr_id sizeof(am_msg_t) buffer_size
        status = UCX.API.UCS_ERR_CANCELED

        UnsafeAtomics.store!(hdr_sig, 0x00)
        return status
    end 

    memory_cpu_load_fence() # This is iffy we need to protect the data loads below
    hdr_id = unsafe_getfield(hdr, :id)
    if VERBOSE
        @info "AM executing" id=hdr_id
    end

    if hdr_id == 0
        unsafe_setfield!(tgt_args, :run, false)
        status = UCX.API.UCS_OK
        return status
    elseif hdr_id == 1
        status = am_chase(field_ptr(hdr, :src_args), tgt_args)
        return status
    else
        @error "AM is not valid" hdr_id
        status = UCX.API.UCS_ERR_CANCELED
        return status
    end
end

function am_chase(src_args, tgt_args)
    depth = unsafe_getfield(src_args, :depth)
    addr = unsafe_getfield(src_args, :addr)
    dest = unsafe_getfield(src_args, :dest)

    data = unsafe_getfield(tgt_args, :data)
    this_shard = unsafe_getfield(tgt_args, :shard_id)
    server_qty = unsafe_getfield(tgt_args, :machine_qty) - 1
    shard_size = unsafe_getfield(tgt_args, :shard_size)
    worker = unsafe_getfield(tgt_args, :wrkr)

    max_index = server_qty * shard_size

    next = UInt64(0)

    @static if VERBOSE
        @info "Chasing these pointers" depth addr dest this_shard
    end

    # addr = mod(addr, max_index)
    # @assert addr in 0:(max_index-1)
    which_shard = div(addr, shard_size)

    if which_shard == this_shard # data is local
        addr = mod(addr, shard_size) # get address within a shard

        next = Base.unsafe_load(data, addr%Int64 + 1) # addr is offset
        @static if VERBOSE
            @info "result is" next
        end

        # tirm down next to fit
        # next = mod(next, max_index)
        # @assert next in 0:(max_index-1)

        depth -= 1

        if depth == 0
            @static if VERBOSE
                @info "Reached max depth"
            end

            # write to original
            result_msg = result_msg_t(next, AM_SIG_MAGIC)
            heap_u64 = reinterpret(UInt64, unsafe_getfield(tgt_args, :heap))
            heap_aslr = UInt64(heap_u64 + Base.unsafe_load(unsafe_getfield(tgt_args, :aslr_diff), dest%Int64))

            ep = Base.unsafe_load(unsafe_getfield(tgt_args, :ep), dest%Int64)
            rmt_rkey = Base.unsafe_load(unsafe_getfield(tgt_args, :rmt_rkey), dest%Int64)

            s = UCX.API.ucp_put_nbi(ep,
                            Ref(result_msg), sizeof(result_msg_t),
                            heap_aslr, rmt_rkey);
            
            if s != UCX.API.UCS_OK
                cflush(worker, ep)
            end
            return UCX.API.UCS_OK
        end

        unsafe_setfield!(src_args, :depth, depth)
        unsafe_setfield!(src_args, :addr, next)

        which_shard = div(next, shard_size)
        @static if VERBOSE
            @info "Next shard" which_shard
        end

        if which_shard == this_shard
            s = am_chase(src_args, tgt_args)
            return s
        end
    end

    # forward to other ep

    destination = (which_shard%Int64 + 1) + 1 # client is 1, first server is 2
    am = am_msg_t(1, unsafe_load(src_args), AM_SIG_MAGIC)

    @static if VERBOSE
        @info "Remote" addr=am.src_args.addr dest=am.src_args.dest depth=am.src_args.depth
    end

    heap_u64 = reinterpret(UInt64, unsafe_getfield(tgt_args, :heap))
    heap_aslr = UInt64(heap_u64 + Base.unsafe_load(unsafe_getfield(tgt_args, :aslr_diff), destination))

    ep = Base.unsafe_load(unsafe_getfield(tgt_args, :ep), destination)
    rmt_rkey = Base.unsafe_load(unsafe_getfield(tgt_args, :rmt_rkey), destination)

    s = UCX.API.ucp_put_nbi(ep,
                    Ref(am), sizeof(am_msg_t),
                    heap_aslr, rmt_rkey);
            
    if s != UCX.API.UCS_OK
        cflush(worker,ep)
    end
    return UCX.API.UCS_OK
end

const triples = [
    "x86_64-linux-gnu" => "x86_64-unknown-linux-gnu",
    "aarch64-linux-gnu" => "aarch64-unknown-linux-gnu"
]

load_bcs(name, triples) = map(triples) do (triple, full_triple)
    triple => open(read, joinpath(@__DIR__, "..", "$name.$full_triple.bc"))
end 
function register_ifunc(ctx, name, triples)
    bcs = load_bcs(name, triples)
    deps = ["libucp.so"]
    return UCXAsap.register_ifunc_bc(ctx, name, bcs, deps)
end

function run_server_ifunc(state, jl=false)
    heap = state.heap
    shard_size = state.shard_size
    whoami = state.whoami
    ctx = state.worker.context
    datapoints = state.datapoints

    dummy_number = UInt64(787)
    data = Base.unsafe_convert(Ptr{UInt64}, heap + HALF_HEAP)
    data_tmp = ptroffset(data, ((whoami-2) * shard_size))

    # copy data into same pointer
    # data is in offset format
    for i in 1:shard_size
        val = unsafe_load(data_tmp, i)
        @assert val in 0:(datapoints-1)
        unsafe_store!(data, val, i)
    end

    # Register chaser function
    ih_chaser = register_ifunc(ctx, jl ? :jchaser : :chaser, triples)

    # result registration
    ih_result = register_ifunc(ctx, jl ? :jresult : :result, triples)

    # Create a chaser ifunc that can be used to be sent by main or ifuncs
    # chaser ifunc message source arguments
    pc_args = SrcArgs(dummy_number, dummy_number, dummy_number)

    chaser_msg = UCXAsap.ucp_ifunc_msg_create(ih_chaser, Ref(pc_args), sizeof(SrcArgs))
    result_msg = UCXAsap.ucp_ifunc_msg_create(ih_result, Ref(dummy_number), sizeof(UInt64))

    # main target args including ifunc to be sent
    # Necessary since we need the raw handles
    ep = state.ep
    raw_ep = map_undefined(ep->ep.handle, ep)
    rmt_rkey = state.rmt_rkey
    raw_rmt_rkey = map_undefined(rmt_rkey->rmt_rkey.handle, rmt_rkey)
    aslr_diff = state.aslr_diff
    GC.@preserve ep raw_ep raw_rmt_rkey aslr_diff begin
        # main target args including ifunc to be sent
        r_tgt_args = Ref(TgtArgs(
            whoami - 2, # client us whoami = 1
            data,
            pointer(raw_ep),
            state.worker.handle,
            chaser_msg,
            result_msg,
            pointer(raw_rmt_rkey),
            @cfunction(cflush, UCX.API.ucs_status_t, (UCX.API.ucp_worker_h, UCX.API.ucp_ep_h)),
            pointer(aslr_diff),
            heap,
            state.machine_qty,
            shard_size,
            true
        ))

        socket_p2p_sync(state.connections, CLIENT)

        # Start receiving ifuncs
        GC.@preserve r_tgt_args begin
            tgt_args = Base.unsafe_convert(Ptr{TgtArgs}, r_tgt_args)
            while unsafe_getfield(tgt_args, :run)
                while true
                    s = UCXAsap.ucp_poll_ifunc(ctx, heap, HALF_HEAP, tgt_args, state.worker)
                    if s == UCX.API.UCS_OK
                        break
                    end
                    yield()
                end
            end
        end
    end
    socket_p2p_sync(state.connections, CLIENT)

    # cleanup
    UCXAsap.ucp_ifunc_msg_free(chaser_msg)
    UCXAsap.ucp_ifunc_msg_free(result_msg)
    UCXAsap.ucp_deregister_ifunc(ctx, ih_chaser)
    UCXAsap.ucp_deregister_ifunc(ctx, ih_result)
end

function run_client_ifunc(state, servers, jl = false)
    heap = state.heap
    machine_qty = state.machine_qty
    shard_size = state.shard_size
    datapoints = state.datapoints
    shard_size = state.shard_size
    whoami = state.whoami
    ctx = state.worker.context

    dummy_number = UInt64(787)
    # Register chaser function
    ih_chaser = register_ifunc(ctx, jl ? :jchaser : :chaser, triples)

    # stop registration
    ih_stop = register_ifunc(ctx, jl ? :jstop : :stop, triples)

    # Convert whoami to offset 
    pc_args = SrcArgs(dummy_number, dummy_number, whoami-1)

    im_chaser = UCXAsap.ucp_ifunc_msg_create(ih_chaser, Ref(pc_args), sizeof(SrcArgs))

    payload, payload_size = UCXAsap.ucp_ifunc_msg_get_payload_ptr(im_chaser)
    @assert payload_size == sizeof(SrcArgs)
    msg_args = Base.unsafe_convert(Ptr{SrcArgs}, payload)

    im_stop = UCXAsap.ucp_ifunc_msg_create(ih_stop, Ref(dummy_number), sizeof(UInt64))

    # sync
    for i in 2:state.machine_qty
        socket_p2p_sync(state.connections, i)
    end

    unsafe_setfield!(msg_args, :depth, DEPTH)

    @info "Testing" depth=unsafe_getfield(msg_args, :depth)

    # Start pointer chase
    for j in 0:(20-1)
        unsafe_setfield!(msg_args, :addr, UInt64(j)) # TODO: how to generate number?

        @info "Addr" addr=unsafe_getfield(msg_args, :addr)
        # Send to first server
        s = UCXAsap.ucp_ifunc_send_nbix(state.ep[SERVER], im_chaser, 
            ptr_to_u64_aslr(state, heap, SERVER), state.rmt_rkey[SERVER])

        if s != UCX.API.UCS_OK
            flush(state, false, SERVER)
        end

        result = Ref{UInt64}(787)
        s = UCX.API.UCS_INPROGRESS
        while s != UCX.API.UCS_OK
            s = UCXAsap.ucp_poll_ifunc(ctx, heap, HALF_HEAP, result, state.worker)
            yield()
        end
        UCX.API.ucp_worker_progress(state.worker) # TODO: necessary?

        @info "Result" result = result[]
    end

    @info """
    Benchmarking

    Doing a depth sweeps of powers of 2
    """ max_depth = 1 << MAX_DEPTH_LOG

    results = Table(
            client = String[],
            servers = String[],
            test_type = String[],
            frame = Int[],
            payload = Int[],
            iterations = Int[],
            depth = Int[],
            datapoints = Int[],
            server_qty = Int[],
            message_rate = Float64[],
            )
    client = first(split(Base.gethostname(), '.'))
    test_type = jl ? "ifunc_jl" : "ifunc_c"
    frame = -1
    payload = -1

    t0 = UInt64(0)

    depth = 1
    for _ in 0:MAX_DEPTH_LOG
        iterations = div(ITERATIONS, depth)
        warmup = div(iterations, 16)
        unsafe_setfield!(msg_args, :depth, depth)

        for j in 0:(warmup + iterations - 1)
            if j == warmup
                t0 = time_ns()
            end

            unsafe_setfield!(msg_args, :addr, UInt64(j)) # TODO: how to generate number?

            # Send to first server
            s = UCXAsap.ucp_ifunc_send_nbix(state.ep[SERVER], im_chaser, 
                ptr_to_u64_aslr(state, heap, SERVER), state.rmt_rkey[SERVER])

            if s != UCX.API.UCS_OK
                flush(state, false, SERVER)
            end

            result = Ref{UInt64}(787)
            s = UCX.API.UCS_INPROGRESS
            while s != UCX.API.UCS_OK
                s = UCXAsap.ucp_poll_ifunc(ctx, heap, HALF_HEAP, result, state.worker)
                yield()
            end
            UCX.API.ucp_worker_progress(state.worker) # TODO: necessary?
        end

        t1 = time_ns()
        T = (t1 - t0) / 1e9

        @info "Benchmark result for IFunc" jl iterations depth datapoints machine_qty-1 iterations/T hostname=Base.gethostname()
        push!(results, (;client, servers, test_type, frame, payload, iterations, depth, datapoints, server_qty=machine_qty-1, message_rate=iterations/T))

        depth *= 2
    end

    # Stop servers
    for i in 2:state.machine_qty
        s = UCXAsap.ucp_ifunc_send_nbix(state.ep[i], im_stop, 
            ptr_to_u64_aslr(state, heap, i), state.rmt_rkey[i])

        if s != UCX.API.UCS_OK
            flush(state, false, i)
        end
    end

    # sync
    for i in 2:state.machine_qty
        socket_p2p_sync(state.connections, i)
    end

    CSV.write(jl ? "$(Dates.now())_$(machine_qty-1)s_ifunc_jl.csv" : "$(Dates.now())_$(machine_qty-1)s_ifunc_c.csv", results)

    # cleanup
    UCXAsap.ucp_ifunc_msg_free(im_chaser)
    UCXAsap.ucp_ifunc_msg_free(im_stop)
    UCXAsap.ucp_deregister_ifunc(ctx, ih_chaser)
    UCXAsap.ucp_deregister_ifunc(ctx, ih_stop)
end

function run_client_am(state, servers)
    heap = state.heap
    whoami = state.whoami
    shard_size = state.shard_size
    machine_qty = state.machine_qty
    datapoints = state.datapoints

    result = UInt64(747)
    dummy_number = UInt64(737)

    r_am = Ref(am_msg_t(1, SrcArgs(dummy_number, dummy_number, whoami), AM_SIG_MAGIC))
    GC.@preserve r_am begin
        am = Base.unsafe_convert(Ptr{am_msg_t}, r_am)

        msg_args = field_ptr(am, :src_args)

        result_msg = Base.unsafe_convert(Ptr{result_msg_t}, heap)
        unsafe_setfield!(result_msg, :result, result)
        result_sig = field_ptr(result_msg, :sig)

        UnsafeAtomics.store!(result_sig, UInt8(0))

        # sync
        for i in 2:state.machine_qty
            socket_p2p_sync(state.connections, i)
        end

        unsafe_setfield!(msg_args, :depth, DEPTH)

        @info "Testing" depth=unsafe_getfield(msg_args, :depth)

        # Start pointer chase
        for j in 0:(20-1)
            unsafe_setfield!(msg_args, :addr, UInt64(j)) # TODO: how to generate number?

            @assert unsafe_getfield(am, :id) === 0x01
            @assert unsafe_getfield(am, :sig) === 0x55
            @info "Addr" addr=unsafe_getfield(msg_args, :addr) ep=state.ep[SERVER] am sizeof(am_msg_t) rkey=state.rmt_rkey[SERVER] target=ptr_to_u64_aslr(state, heap, SERVER)
            # Send to first server
            s = UCX.API.ucp_put_nbi(
                state.ep[SERVER], am, sizeof(am_msg_t),
                ptr_to_u64_aslr(state, heap, SERVER),
                state.rmt_rkey[SERVER]
            )
            # @info "Flushing" s
            if s != UCX.API.UCS_OK
                cflush(state.worker, state.ep[SERVER])
            end

            @info "Awaiting"
            # await for result
            while UnsafeAtomics.load(result_sig) != AM_SIG_MAGIC
                UCX.API.ucp_worker_progress(state.worker)
                yield()
            end

            UnsafeAtomics.store!(result_sig, UInt8(0))

            @info "Result" result = unsafe_getfield(result_msg, :result)
        end

        @info """
        Benchmarking

        Doing a depth sweeps of powers of 2
        """ max_depth = 1 << MAX_DEPTH_LOG

        results = Table(
            client = String[],
            servers = String[],
            test_type = String[],
            frame = Int[],
            payload = Int[],
            iterations = Int[],
            depth = Int[],
            datapoints = Int[],
            server_qty = Int[],
            message_rate = Float64[],
            )

        client = first(split(Base.gethostname(), '.'))
        test_type = "am"
        frame = -1
        payload = -1

        t0 = UInt64(0)

        depth = 1
        for _ in 0:MAX_DEPTH_LOG
            iterations = div(ITERATIONS, depth)
            warmup = div(iterations, 16)
            unsafe_setfield!(msg_args, :depth, depth)

            for j in 0:(warmup + iterations - 1)
                if j == warmup
                    t0 = time_ns()
                end

                unsafe_setfield!(msg_args, :addr, UInt64(j)) # TODO: how to generate number?

                # Send to first server
                s = UCX.API.ucp_put_nbi(
                    state.ep[SERVER], am, sizeof(am_msg_t),
                    ptr_to_u64_aslr(state, heap, SERVER),
                    state.rmt_rkey[SERVER]
                )
                if s != UCX.API.UCS_OK
                    flush(state, false, SERVER)
                end

                # await for result
                while UnsafeAtomics.load(result_sig) != AM_SIG_MAGIC
                    UCX.API.ucp_worker_progress(state.worker)
                    yield()
                end

                UnsafeAtomics.store!(result_sig, UInt8(0))
            end

            t1 = time_ns()
            T = (t1 - t0) / 1e9

            @info "Benchmark result for AM" iterations depth datapoints machine_qty-1 iterations/T hostname=Base.gethostname()
            push!(results, (;client, servers, test_type, frame, payload, iterations, depth, datapoints, server_qty=machine_qty-1, message_rate=iterations/T))


            depth *= 2
        end

        # stop servers
        for i in 2:state.machine_qty
            # ID 0 shuts down the server
            unsafe_setfield!(am, :id, UInt8(0))

            s = UCX.API.ucp_put_nbi(
                state.ep[i], am, sizeof(am_msg_t),
                ptr_to_u64_aslr(state, heap, i),
                state.rmt_rkey[i]
            )
            if s != UCX.API.UCS_OK
                flush(state, false, i)
            end
        end

        # sync
        for i in 2:state.machine_qty
            socket_p2p_sync(state.connections, i)
        end

        CSV.write("$(Dates.now())_$(machine_qty-1)s_am.csv", results)
    end
end

function map_undefined(f, arr)
    T = Core.Compiler.return_type(f, Tuple{eltype(arr)})
    out = similar(arr, T)
    for i in eachindex(arr)
        if isassigned(arr, i)
            out[i] = f(arr[i])
        else
            out[i] = C_NULL
        end
    end
    return out
end

function run_server_am(state)
    heap = state.heap
    whoami = state.whoami
    shard_size = state.shard_size
    datapoints = state.datapoints

    data = Base.unsafe_convert(Ptr{UInt64}, heap + HALF_HEAP)
    data_tmp = ptroffset(data, ((whoami-2) * shard_size))

    # copy data into same pointer
    for i in 1:shard_size
        val = unsafe_load(data_tmp, i)
        @assert val in 0:(datapoints-1)
        unsafe_store!(data, val, i)
    end

    # Necessary since we need the raw handles
    ep = state.ep
    raw_ep = map_undefined(ep->ep.handle, ep)
    rmt_rkey = state.rmt_rkey
    raw_rmt_rkey = map_undefined(rmt_rkey->rmt_rkey.handle, rmt_rkey)
    aslr_diff = state.aslr_diff
    GC.@preserve ep raw_ep raw_rmt_rkey aslr_diff begin
        # main target args including ifunc to be sent
        r_tgt_args = Ref(TgtArgs(
            whoami - 2, # client uses whoami = 1
            data,
            pointer(raw_ep),
            state.worker.handle,
            Ref{UCXAsap.ucp_ifunc_msg_t}()[],
            Ref{UCXAsap.ucp_ifunc_msg_t}()[],
            pointer(raw_rmt_rkey),
            @cfunction(cflush, UCX.API.ucs_status_t, (UCX.API.ucp_worker_h, UCX.API.ucp_ep_h)),
            pointer(aslr_diff),
            heap,
            state.machine_qty,
            shard_size,
            true
        ))

        socket_p2p_sync(state.connections, CLIENT)

        GC.@preserve r_tgt_args begin
            tgt_args = Base.unsafe_convert(Ptr{TgtArgs}, r_tgt_args)
            @info "Polling on" heap
            while unsafe_getfield(tgt_args, :run)
                while true
                    s = poll_am(heap, HALF_HEAP, tgt_args, state.worker)
                    yield()
                    if s == UCX.API.UCS_OK
                        break
                    end
                end
            end
        end
    end

    socket_p2p_sync(state.connections, CLIENT)
end

function run_server_gets(state)
    heap = state.heap
    whoami = state.whoami
    shard_size = state.shard_size
    datapoints = state.datapoints

    # Julia doesn't have volatile
    # Use UnsafeAtomics.load and UnsafeAtomics.store! to access run
    run::Ptr{UInt8} = heap + sizeof(UInt64)

    data = Base.unsafe_convert(Ptr{UInt64}, heap + HALF_HEAP)
    data_tmp = ptroffset(data, ((whoami-2) * shard_size))

    # run as long as 123
    UnsafeAtomics.store!(run, UInt8(123))

    # copy data into same pointer
    for i in 1:shard_size
        val = unsafe_load(data_tmp, i)
        @assert val in 0:(datapoints-1)
        unsafe_store!(data, val, i)
    end

    # ready to accept

    socket_p2p_sync(state.connections, CLIENT)

    while UnsafeAtomics.load(run) == 123
        UCX.API.ucp_worker_progress(state.worker)
        yield()
    end
    UCX.API.ucp_worker_progress(state.worker)

    socket_p2p_sync(state.connections, CLIENT)
end

function run_client_gets(state, servers)
    heap = state.heap
    machine_qty = state.machine_qty
    shard_size = state.shard_size
    datapoints = state.datapoints
    ep = state.ep

    # Julia doesn't have volatile
    # Use UnsafeAtomics.load and UnsafeAtomics.store! to access `run` and `result`
    result = Base.unsafe_convert(Ptr{UInt64}, heap)
    run::Ptr{UInt8} = heap + sizeof(UInt64)
    data = Base.unsafe_convert(Ptr{UInt64}, heap + HALF_HEAP)

    UnsafeAtomics.store!(run, UInt8(7))
    UnsafeAtomics.store!(result, UInt64(777))

    depth = DEPTH

    # sync
    for i in 2:state.machine_qty
        socket_p2p_sync(state.connections, i)
    end
    @info "Testing" depth

    next = UInt64(0)
    # Start pointer chase
    for j in 0:(20-1)
        addr = UInt64(j) # TODO: how to generate number?

        for _ in 1:depth
            # addr = mod(addr, datapoints)
            if !(addr in 0:(datapoints-1))
                @error "Address out of bounds" addr max_=UInt64(datapoints)
                @assert addr in 0:(datapoints-1)
            end
            #  div(Int, Int) -> Int
            which_shard = div(addr, shard_size)
            destination = (which_shard + 1) + 1
            addr = UInt64(mod(addr, shard_size))

            data_ptr = ptroffset(data, addr)

            s = UCX.API.ucp_get_nbi(
                ep[destination], result, sizeof(UInt64),
                ptr_to_u64_aslr(state, data_ptr, destination),
                state.rmt_rkey[destination]
            )
            if s != UCX.API.UCS_OK
                cflush(state.worker.handle, ep[destination].handle)
            end

            next = UnsafeAtomics.load(result)
            addr = next
        end

        @show next
    end

    @info """
    Benchmarking

    Doing a depth sweeps of powers of 2
    """ max_depth = 1 << MAX_DEPTH_LOG

    results = Table(
            client = String[],
            servers = String[],
            test_type = String[],
            frame = Int[],
            payload = Int[],
            iterations = Int[],
            depth = Int[],
            datapoints = Int[],
            server_qty = Int[],
            message_rate = Float64[],
            )

    client = first(split(Base.gethostname(), '.'))
    test_type = "gets"
    frame = -1
    payload = -1

    t0 = UInt64(0)

    depth = 1
    for _ in 0:MAX_DEPTH_LOG
        iterations = div(ITERATIONS, depth)
        warmup = div(iterations, 16)

        for j in 0:(warmup + iterations - 1)
            addr = UInt64(j) # TODO: how to generate number
            if j == warmup
                t0 = time_ns()
            end

            for _ in 1:depth
                # addr = mod(addr, datapoints)
                if !(addr in 0:(datapoints-1))
                    @error "Address out of bounds" addr max_=UInt64(datapoints)
                    @assert addr in 0:(datapoints-1)
                end
                which_shard = div(addr, shard_size)
                destination = (which_shard + 1) + 1 # CLIENT is 1
                addr = UInt64(mod(addr, shard_size))

                data_ptr = ptroffset(data, addr)

                s = UCX.API.ucp_get_nbi(
                    ep[destination], result, sizeof(UInt64),
                    ptr_to_u64_aslr(state, data_ptr, destination),
                    state.rmt_rkey[destination]
                )
                if s != UCX.API.UCS_OK
                    flush(state, false, destination)
                end

                next = UnsafeAtomics.load(result)
                addr = next
            end
        end

        t1 = time_ns()
        T = (t1 - t0) / 1e9

        @info "Benchmark result for gets" iterations depth datapoints machine_qty-1 iterations/T hostname=Base.gethostname()
        push!(results, (;client, servers, test_type, frame, payload, iterations, depth, datapoints, server_qty=machine_qty-1, message_rate=iterations/T))


        depth = depth * 2
    end

    # stop servers
    for i in 2:state.machine_qty
        s = UCX.API.ucp_put_nbi(
                ep[i], run, 1,
                ptr_to_u64_aslr(state, run, i),
                state.rmt_rkey[i]
            )
        if s != UCX.API.UCS_OK
            flush(state, false, i)
        end
    end

    @info "Waiting for servers to stop"
    # sync
    for i in 2:state.machine_qty
        socket_p2p_sync(state.connections, i)
    end

    CSV.write("$(Dates.now())_$(machine_qty-1)s_gets.csv", results)
end

function run_gets(state::State, servers)
    if state.whoami == 1
        run_client_gets(state, servers)
    else
        run_server_gets(state)
    end
end

function run_am(state::State, servers)
    if state.whoami == 1
        run_client_am(state, servers)
    else
        run_server_am(state)
    end
end

function run_ifunc(state::State, servers)
    if state.whoami == 1
        run_client_ifunc(state, servers)
    else
        run_server_ifunc(state)
    end
end

function run_ifunc_jl(state::State, servers)
    if state.whoami == 1
        run_client_ifunc(state, servers, true)
    else
        run_server_ifunc(state, true)
    end
end

###
# Setup
###

function query(mem::UCX.Memory)
    field_mask = UCX.API.UCP_MEM_ATTR_FIELD_ADDRESS |
                 UCX.API.UCP_MEM_ATTR_FIELD_LENGTH

    attr = Ref{UCX.API.ucp_mem_attr_t}()
    UCX.memzero!(attr)
    UCX.set!(attr, :field_mask,   field_mask)

    UCX.API.ucp_mem_query(mem, attr)
    return attr[]
end

function server_connect(port=PORT)
    server = Sockets.listen(IPv4(0), port)
    @info "Server waiting for connection" port
    client = accept(server)
    close(server)
    return client
end

function client_connect(_addr)
    addr_info = split(_addr, ':')
    addr = first(addr_info)
    if length(addr_info) == 2
        port = parse(Int32, addr_info[2])
    else
        port = PORT
    end
    client = nothing
    for tries in 1:RETRIES
        try
            client = connect(addr, port)
            break
        catch
            @info "Retrying connection" tries
            sleep(3)
        end
    end
    @assert client !== nothing
    peer, _ = getpeername(client)
    return client, (peer, port)
end

function listen_server(connections, conn_qty, port=PORT)
    if conn_qty == 0
        return
    end
    @info "Listening for connections" conn_qty
    server = Sockets.listen(IPv4(0), port)
    for _ in 1:conn_qty
        client = accept(server)
        other_side = read(client, Int)
        connections[other_side] = client
        @info "Connected to client" other_side
    end
    close(server)
    return nothing
end

function connect_server((addr, port), whoami)
    client = nothing
    for tries in 1:RETRIES
        try
            client = connect(addr, port)
            break
        catch exc
            @info "Retrying connection" tries exc
        end
    end
    @assert client !== nothing
    write(client, whoami)
    @info "Connected to server" whoami addr
    return client
end

function recv_server_info(socket)
    # receive my server ID
    whoami = read(socket, Int)

    # receive test mode
    test_mode = read(socket, TEST_MODE)

    # receive number of servers & server addresses
    machine_qty = read(socket, Int)
    peers = Vector{Tuple{IPv4, Int32}}(undef, machine_qty)
    read!(socket, peers)
    return whoami, test_mode, machine_qty, peers
end

function send_server_info(socket, id, test_mode, machine_qty, peers)
    write(socket, Int(id))
    write(socket, test_mode::TEST_MODE)
    write(socket, Int(machine_qty))
    @assert length(peers) == machine_qty
    write(socket, peers)
    return nothing
end

function find_aslr_diff(socket, heap_ptr, )
    write(socket, heap_ptr)
    heap_ptr_rmt = read(socket, Ptr{Cvoid})

    # Convert UInt64 without overflow check into Int
    return (heap_ptr_rmt - heap_ptr) % Cptrdiff_t, heap_ptr_rmt
end

# LEP: I have a lot of questions about this function
function exchange_rkey(socket, mh)
    rkey = UCX.rkey_pack(mh)
    write(socket, length(rkey))
    rmt_len = read(socket, Int)

    buffer = Vector{UInt8}(undef, rmt_len)
    write(socket, rkey)
    read!(socket, buffer)
    return buffer
end

function exchange_worker_addr(socket, addr)
    write(socket, addr.len)
    addr_len = read(socket, Csize_t)
    unsafe_write(socket, Base.unsafe_convert(Ptr{UInt8}, addr.handle), addr.len)
    other_addr = read(socket, addr_len)
    return other_addr
end


function socket_p2p_sync(connections, id)
    conn = connections[id]
    write(conn, 42)
    recv = read(conn, Int)

    @assert recv == 42
    return nothing
end

# primitive type Page 4096 end

import Profile
import PProf

function main(test_mode::Union{Nothing, TEST_MODE}=nothing, nodelist=nothing; profile=false, port=PORT)
    # Note default context has RMA
    ctx = UCX.UCXContext()

    # Next map memory
    # life time of allocation is tied to mh
    mh = UCX.Memory(ctx, Vector{UInt8}(undef, HEAP_SIZE))
    ma = query(mh)

    @info "Mapped" ma.length ma.address
    heap = ma.address
    # Obtain allocated Julia object through `obj` handle
    data = mh.obj::Vector{UInt8}

    @assert heap == pointer(mh.obj)

    # Note: Default thread mode is multi
    # LEP: We have only tested in UCS_THREAD_MODE_SINGLE. Maybe we need to set
    #      up as such?
    worker = UCX.UCXWorker(ctx)
    addr = UCX.UCXAddress(worker)

    server = test_mode === nothing
    if server
        # Zero out data
        data .= 0

        client = server_connect(port)
        @info "Client connected"
        whoami, test_mode, machine_qty, peers = recv_server_info(client)
        connections = Vector{TCPSocket}(undef, machine_qty)
        connections[CLIENT] = client
        socket_p2p_sync(connections, CLIENT)
        servers = nothing
    else
        whoami = CLIENT # Client ID is 1 to make indexing easier in Julia
        machine_qty = length(nodelist) + 1
        data .= 17

        connections = Vector{TCPSocket}(undef, machine_qty)
        peers = Vector{Tuple{IPv4, Int32}}(undef, machine_qty)
        for (i, arg) in enumerate(nodelist)
            server, peer = client_connect(arg)
            connections[i+1] = server
            peers[i+1] = peer
        end
        # Obtain our own address and send it to all servers
        let
            peer, _ = getsockname(connections[2])
            peers[1] = (peer, PORT)
        end

        for id in 2:machine_qty
            server = connections[id]
            send_server_info(server, id, test_mode, machine_qty, peers)
        end

        # sync all servers with clients
        for id in 2:machine_qty
            socket_p2p_sync(connections, id)
        end
        servers = join(nodelist, "-")
    end
    test_mode::TEST_MODE

    @info "I am" whoami machine_qty test_mode

    if test_mode == TEST_GETS
        func = run_gets
    elseif test_mode == TEST_AM
        func = run_am
    elseif test_mode == TEST_IFUNC
        func = run_ifunc
    elseif test_mode == TEST_IFUNC_JL
        func = run_ifunc_jl
    else
        error("Not implemented")
    end

    # Wireup between servers
    if whoami > CLIENT
        # listen for smaller than whoami
        listen_server(connections, length(2:(whoami - 1)), port)

        for i in (whoami+1):machine_qty
            @info "Connecting to" peers[i] whoami
            connections[i] = connect_server(peers[i], whoami)
        end
    end

    # sync
    if whoami == CLIENT
        for id in 2:machine_qty
            socket_p2p_sync(connections, id)
        end
    else
        socket_p2p_sync(connections, CLIENT)
    end

    # allocate structures dependent on number of connections
    heap_rmt = Vector{Ptr{Cvoid}}(undef, machine_qty)
    aslr_diff = Vector{Cptrdiff_t}(undef, machine_qty)
    ep = Vector{UCX.UCXEndpoint}(undef, machine_qty)
    rmt_rkey = Vector{UCX.RemoteKey}(undef, machine_qty)

    @info "Exchanging UCX info"
    for i in 1:machine_qty
        if whoami == i
            continue
        end
        conn = connections[i]
        aslr, heap_ptr_rmt = find_aslr_diff(conn, heap)
        heap_rmt[i] = heap_ptr_rmt
        aslr_diff[i] = aslr
        rkey = exchange_rkey(conn, mh)
        other_addr = exchange_worker_addr(conn, addr)
        # LEP: are these two equivalent to the one in the C++ file?
        ep[i] = UCX.UCXEndpoint(worker, other_addr)
        rmt_rkey[i] = UCX.RemoteKey(ep[i], rkey)
    end

    # sync
    if whoami == CLIENT
        for id in 2:machine_qty
            socket_p2p_sync(connections, id)
        end
    else
        socket_p2p_sync(connections, CLIENT)
    end

    # LEP: need to discuss flush
    # flush(state, true, -1)
    wait(Base.flush(worker)) # Required for wire up

    datapoints = div(HALF_HEAP, sizeof(UInt64))
    server_qty = machine_qty - 1
    shard_size = div(datapoints, server_qty)

    deita = Base.unsafe_convert(Ptr{UInt64}, heap + HALF_HEAP)

    @info "Generating" datapoints
    for i in 1:datapoints
        # Fill with 0-based offsets so that we can
        # use this data from C
        Base.unsafe_store!(deita, i-1, i)
    end

    Random.seed!(777)
    for i in 1:datapoints
        # LEP: is this pseudo-random? We want pseudo random for verification
        #      purposes
        idx = rand(1:i)
        tmp = Base.unsafe_load(deita, i)
        Base.unsafe_store!(deita, Base.unsafe_load(deita, idx), i)
        Base.unsafe_store!(deita, tmp, idx)
    end

    state = State(
        whoami,
        machine_qty,
        shard_size,
        datapoints,
        worker,
        mh,
        heap,
        heap_rmt,
        aslr_diff,
        ep,
        rmt_rkey,
        connections
    )

    @info "Initialization done"
    profile && Profile.start_timer()
    GC.@preserve mh state begin
        func(state, servers)
        if profile
            Profile.stop_timer()
            PProf.pprof(out="proc-$whoami.pb.gz", web=false)
        end
    end
end

#if !isinteractive()
#     if !(length(ARGS) == 0 || length(ARGS) >= 1)
#         error("Must provide test_mode: ifunc, gets, am")
#     end
#     if length(ARGS) == 0
#         test_mode = nothing
#     else
#         mode = Symbol(pop!(ARGS))
#         if mode === :ifunc
#             test_mode = TEST_IFUNC
#         elseif mode === :gets
#             test_mode = TEST_GETS
#         elseif mode === :am
#             test_mode = TEST_AM
#         else
#             error("Unkown test mode $mode")
#         end
#     end
#     main(test_mode, ARGS)
# end
