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
    return Cint(0)
end

function main(payload::Ptr{Cvoid}, payload_size::Csize_t, target_args::Ptr{Cvoid})::Cvoid
    tgt_args = Base.unsafe_convert(Ptr{TgtArgs}, target_args)
    unsafe_setfield!(tgt_args, :run, false)
    return nothing
end

const fns = (
    (bound, Tuple{Ptr{Cvoid}, Csize_t}),
    (init, Tuple{Ptr{Cvoid}, Csize_t, Ptr{Cvoid}, Csize_t}),
    (main, Tuple{Ptr{Cvoid}, Csize_t, Ptr{Cvoid}})
)


IFunc.Compiler.emit(fns, "jstop", "$(Sys.ARCH)-unknown-linux-gnu")
