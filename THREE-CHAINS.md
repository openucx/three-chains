# Basic Three-Chains bringup instructions

- Set the following environment variables:
```
export UCX_PATH=${HOME}/opt
export LLVM_PATH=${HOME}/llvm
export LD_LIBRARY_PATH=${LLVM_PATH}/lib:${UCX_PATH}/lib
export PATH=${PATH}:${HOME}/julia-1.8.0-beta3/bin
export CPATH=${UCX_PATH}/include:${LLVM_PATH}/include
export C_INCLUDE_PATH=${UCX_PATH}/include:${LLVM_PATH}/include
export CPLUS_INCLUDE_PATH=${UCX_PATH}/include:${LLVM_PATH}/include
export INCLUDE=${UCX_PATH}/include:${LLVM_PATH}/include
export INCLUDEPATH=${UCX_PATH}/include:${LLVM_PATH}/include
export INCLUDE_PATH=${UCX_PATH}/include:${LLVM_PATH}/include
```

- Clone this repo
- Download and install LLVM:
```
$ contri/ifunc/orcjit/build_llvm.sh
```
- Download and extract Julia on your home (this is the aarch64 version):
```
$ wget https://julialang-s3.julialang.org/bin/linux/aarch64/1.8/julia-1.8.0-beta3-linux-aarch64.tar.gz
$ tar xfvz julia-1.8.0-beta3-linux-aarch64.tar.gz 
```
- Compile UCX + 3C
```
$ ./autogen.sh
$ ./contrib/configure-release --prefix=${HOME}/opt --with-llvm=${HOME}/llvm
$ make -j$nproc
$ make install -j$nproc
```

# Running examples

Examples are located in `contrib/ifunc`

Make sure to set `export UCX_IFUNC_LIB_DIR=${PWD}` from the directory where the example was compiled.

