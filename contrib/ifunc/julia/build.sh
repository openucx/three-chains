#!/bin/bash
#
# Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
#
SRC_DIR=$(realpath $(dirname ${BASH_SOURCE})/../../..)

ARCH=$(uname -m) # use arch instead?
PREFIX=$(realpath ${1}/${ARCH})
LLVM_VER=$(julia -e 'print(Base.libllvm_version)')

echo "Building ucx-asap for ${ARCH} and LLVM ${LLVM_VER}"
echo "PREFIX: ${PREFIX}"
echo "SRC_DIR: ${SRC_DIR}"
julia --version

julia --project=llvm_${ARCH} -e "import Pkg; Pkg.add(name=\"LLVM_full_jll\", version=v\"${LLVM_VER}\")" 
LLVM_DIR=$(julia --project=llvm_${ARCH} -e 'using LLVM_full_jll; print(LLVM_full_jll.artifact_dir)')

echo "Determined LLVM_DIR=${LLVM_DIR}"

pushd ${SRC_DIR}
./configure \
  --prefix=${PREFIX} \
  --with-llvm=${LLVM_DIR} \
  --disable-static \
  --enable-shared \
  --enable-cma \
  --enable-mt \
  --enable-frame-pointer \
  --without-bfd \
  --without-rocm \
  --without-cuda \
  --enable-logging
     

export LD_LIBRARY_PATH=${LLVM_DIR}/lib

make clean -j # so that we can build multiple architectures
make -j
make install

popd

mkdir -p ${PREFIX}/ucx

echo """
export JULIA_LOAD_PATH=":${PREFIX}/ucx"
export LLVM_DIR=${LLVM_DIR}
export LD_LIBRARY_PATH=${LLVM_DIR}/lib:${PREFIX}/lib:\${LD_LIBRARY_PATH}
export UCX_PATH=${PREFIX}
export LLVM_PATH=${LLVM_DIR}
""" > ${PREFIX}/ucx/env.sh

echo """
[extras]
UCX_jll = \"16e4e860-d6b8-5056-a518-93e88b6392ae\"
rdma_core_jll = \"69dc3629-5c98-505f-8bcd-225213cebe70\"
""" > ${PREFIX}/ucx/Project.toml

echo """
[UCX_jll]
libucp_path = \"${PREFIX}/lib/libucp.so\"
libucs_path = \"${PREFIX}/lib/libucs.so\"
""" > ${PREFIX}/ucx/LocalPreferences.toml

if [[ -f "/lib64/libibverbs.so.1" ]]; then
  echo """
[rdma_core_jll]
libibverbs_path = \"/lib64/libibverbs.so.1\"
lbrdmacm_path = \"/lib64/librdmacm.so.1\"
""" >> ${PREFIX}/ucx/LocalPreferences.toml
fi
