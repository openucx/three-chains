#!/usr/bin/env bash
#
# Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
#

# Set LLVM_ENABLE_ASSERTIONS=ON for development
# Requires libffi

MAKE_JOBS=$(nproc)
LINK_JOBS=$(nproc)

LLVM_VER=13.0.1
LLVM_PROJECTS="clang;openmp"

export TMPDIR=/dev/shm
export LDFLAGS="-Wl,-O1"

wget https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz
tar xf llvm-project-$LLVM_VER.src.tar.xz
rm llvm-project-$LLVM_VER.src.tar.xz
mv llvm-project-$LLVM_VER.src llvm_source

cmake -S llvm_source/llvm                       \
      -B llvm_objdir                            \
      -DCMAKE_BUILD_TYPE=Release                \
      -DCMAKE_INSTALL_PREFIX="$HOME/llvm"       \
      -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS"       \
      -DLLVM_TARGETS_TO_BUILD="AArch64;X86"     \
      -DLLVM_ENABLE_FFI=ON                      \
      -DLLVM_ENABLE_LTO=OFF                     \
      -DLLVM_ENABLE_ASSERTIONS=OFF              \
      -DLLVM_ENABLE_PROJECTS="$LLVM_PROJECTS"   \
      -DLLVM_BUILD_TOOLS=ON                     \
      -DLLVM_BUILD_UTILS=ON                     \
      -DLLVM_BUILD_LLVM_DYLIB=ON                \
      -DLLVM_PARALLEL_LINK_JOBS="$LINK_JOBS"    \
      -DLLVM_PARALLEL_COMPILE_JOBS="$MAKE_JOBS" \
      -DLLVM_APPEND_VC_REV=OFF                  \
      -DLLVM_USE_PERF=ON                        \
      -DCLANG_ENABLE_ARCMT=OFF                  \
      -DCLANG_ENABLE_STATIC_ANALYZER=OFF        \
      -DLIBOMP_OMPT_SUPPORT=OFF                 \
      -DLIBOMP_OMPD_SUPPORT=OFF                 \
      -DLIBOMP_INSTALL_ALIASES=OFF              \
      -DOPENMP_ENABLE_LIBOMPTARGET=OFF

make -C llvm_objdir -j $MAKE_JOBS install

rm -rf llvm_source llvm_objdir
