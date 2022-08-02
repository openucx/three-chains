# This file is a part of UCX.jl. License is MIT: https://github.com/JuliaParallel/UCX.jl/blob/main/LICENSE.md
# Author: Valentin Churavy, 2022

using LLVM
using LLVM.Interop

Base.@pure function find_field(::Type{T}, fieldname) where T
    findfirst(f->f === fieldname, fieldnames(T))
end

@inline unsafe_setfield!(base_ptr, fieldname, val) = __unsafe_setfield!(base_ptr, Val(fieldname), val)

@generated function __unsafe_setfield!(base_ptr::Ptr{T}, ::Val{fieldname}, val) where {T, fieldname}
    field = find_field(T, fieldname)
    @assert field !== nothing
    fieldT = fieldtype(T, field)

    Context() do ctx
        eltyp = convert(LLVMType, T; ctx)
        T_int = LLVM.IntType(sizeof(Int)*8; ctx)
        T_ptr = LLVM.PointerType(eltyp)
        T_field = convert(LLVMType, fieldT; ctx)
        llvmf, _ = create_function(T_field, [T_int, T_field])

        Builder(ctx) do builder
            entry = BasicBlock(llvmf, "entry"; ctx)
            position!(builder, entry)

            ptr = inttoptr!(builder, parameters(llvmf)[1], T_ptr)
            ptr = struct_gep!(builder, ptr, ConstantInt(field - 1; ctx))

            st = store!(builder, parameters(llvmf)[2], ptr)
            alignment!(st, Base.datatype_alignment(fieldT))

            ret!(builder, parameters(llvmf)[2])
        end
        return call_function(llvmf, fieldT, Tuple{Ptr{T}, fieldT}, :(base_ptr), :(convert($fieldT, val)))
    end
end

# val = ptr->field
@inline unsafe_getfield(base_ptr, fieldname) = __unsafe_getfield(base_ptr, Val(fieldname))
@generated function __unsafe_getfield(base_ptr::Ptr{T}, ::Val{fieldname}) where {T, fieldname}
    field = find_field(T, fieldname)
    @assert field !== nothing
    fieldT = fieldtype(T, field)

    Context() do ctx
        eltyp = convert(LLVMType, T; ctx)
        T_int = LLVM.IntType(sizeof(Int)*8; ctx)
        T_ptr = LLVM.PointerType(eltyp)
        T_field = convert(LLVMType, fieldT; ctx)
        llvmf, _ = create_function(T_field, [T_int])

        Builder(ctx) do builder
            entry = BasicBlock(llvmf, "entry"; ctx)
            position!(builder, entry)

            ptr = inttoptr!(builder, parameters(llvmf)[1], T_ptr)
            ptr = struct_gep!(builder, ptr, ConstantInt(field - 1; ctx))

            val = load!(builder, ptr)
            alignment!(val, Base.datatype_alignment(fieldT))

            ret!(builder, val)
        end
        return call_function(llvmf, fieldT, Tuple{Ptr{T}}, :(base_ptr))
    end
end
field_ptr(base_ptr::Ptr{T}, fieldname) where {T} = __field_ptr(base_ptr, Val(fieldname))
@generated function __field_ptr(base_ptr::Ptr{T}, ::Val{fieldname}) where {T, fieldname}
    field = find_field(T, fieldname)
    @assert field !== nothing
    fieldT = fieldtype(T, field)

    Context() do ctx
        eltyp = convert(LLVMType, T; ctx)
        T_int = LLVM.IntType(sizeof(Int)*8; ctx)
        T_ptr = LLVM.PointerType(eltyp)
        T_field = convert(LLVMType, fieldT; ctx)
        llvmf, _ = create_function(T_int, [T_int])

        Builder(ctx) do builder
            entry = BasicBlock(llvmf, "entry"; ctx)
            position!(builder, entry)

            ptr = inttoptr!(builder, parameters(llvmf)[1], T_ptr)
            ptr = struct_gep!(builder, ptr, ConstantInt(field - 1; ctx))

            ret!(builder, ptrtoint!(builder, ptr, T_int))
        end
        return call_function(llvmf, Ptr{fieldT}, Tuple{Ptr{T}}, :(base_ptr))
    end
end

unsafe_rem(a::T,b::T) where T<:Unsigned = Core.Intrinsics.urem_int(a, b)
unsafe_rem(a::T,b::T) where T<:Signed = Core.Intrinsics.srem_int(a, b)
unsafe_div(a::T,b::T) where T<:Unsigned = Core.Intrinsics.udiv_int(a, b)
unsafe_div(a::T,b::T) where T<:Signed = Core.Intrinsics.sdiv_int(a, b)

@inline trap() = ccall("llvm.trap", llvmcall, Cvoid, ())

@inline assume(cond::Bool) = Base.llvmcall(("""
        declare void @llvm.assume(i1)

        define void @entry(i8) #0 {
            %cond = icmp eq i8 %0, 1
            call void @llvm.assume(i1 %cond)
            ret void
        }

        attributes #0 = { alwaysinline }""", "entry"),
    Nothing, Tuple{Bool}, cond)

# do-block syntax
@inline function assume(f::F) where {F}
    assume(f())
    return
end
@inline function assume(f::F, val) where {F}
    assume(f(val))
    return val
end

@inline function memory_cpu_load_fence()
    Base.llvmcall(("""
        define void @entry() #0 {
            fence acquire
            ret void
        }

        attributes #0 = { alwaysinline }""", "entry"), Cvoid, Tuple{})
end
