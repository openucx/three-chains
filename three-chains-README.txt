sudo apt install build-essential (cmake, make, g++, gcc)
git clone https://github.com/openucx/three-chains.git

# TO install LLVM:
sudo apt install libffi-dev autoconf libtool
cd three-chains/contrib/ifunc/orcjit
# Assuming to install at $HOME/llvm
./build_llvm.sh

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

cd ../../../three-chains
# installing in opt in home dir
./autogen.sh 
./contrib/configure-release --prefix=${HOME}/opt --with-llvm=${HOME}/llvm
# make sure at the end  LLVM support:   enabled

#Build UCX
make -j$nproc
#install UCX
make install

# To build ifuncs
cd contrib/ifunc
make -i (to ignore arm64) 

export UCX_IFUNC_LIB_DIR=${PWD} 

Open two terminals:
In one:
./lat_bw_test.x 
Another:
./lat_bw_test.x localhost 1


# To install Julia
 wget https://julialang-s3.julialang.org/bin/linux/aarch64/1.8/julia-1.8.0-beta3-linux-aarch64.tar.gz   (OR CORRECT ARCHITECTURE)
# untar into known directory and make sure to set PATH to the bin (see exports.sh)

