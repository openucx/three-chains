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
    result_src = Base.unsafe_convert(Ptr{UInt64}, source_args)
    result_pay = Base.unsafe_convert(Ptr{UInt64}, payload)

    Base.unsafe_store!(result_pay, Base.unsafe_load(result_src))

    return Cint(0)
end

function main(payload::Ptr{Cvoid}, payload_size::Csize_t, target_args::Ptr{Cvoid})::Cvoid
    result_pay = Base.unsafe_convert(Ptr{UInt64}, payload)
    result_tgt = Base.unsafe_convert(Ptr{UInt64}, target_args)

    Base.unsafe_store!(result_tgt, Base.unsafe_load(result_pay))

    return nothing
end

const fns = (
    (bound, Tuple{Ptr{Cvoid}, Csize_t}),
    (init, Tuple{Ptr{Cvoid}, Csize_t, Ptr{Cvoid}, Csize_t}),
    (main, Tuple{Ptr{Cvoid}, Csize_t, Ptr{Cvoid}})
)

IFunc.Compiler.emit(fns, "jresult", "$(Sys.ARCH)-unknown-linux-gnu")
