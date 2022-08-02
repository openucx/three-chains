# Launching

## Development

Start the server with:

```
julia --project -L pointer_chase.jl -e 'main()'
```

```
julia --project -L pointer_chase.jl -e 'main(TEST_GETS, ["127.0.0.1"])'
```

## Heterogenous on SLURM

Setup a Julia installation per architecture:

E.g.

```
$HOME/x86_64/julia-1.8.0-beta3
$HOME/aarch64/julia-1.8.0-beta3
```

Using the binaries downloaded from the Julia website.


First let's create a wrapper script that deals with the multiarchitecture `launch.sh`:

```sh
ARCH=$(uname -m)
export JULIA_LOAD_PATH=":${HOME}/${ARCH}/ucx"
export PATH="${HOME}/${ARCH}/julia-1.8.0-beta3/bin:${PATH}"
exec $*
```

### Setting up UCX_jll

in `contrib/julia/build.sh` we have a script 
that will download the correct LLVM version for the version of Julia you are using. It will automatically
bifurcate the prefix into multiple architectures
and create the correct files in `x86_64/ucx/` redirecting Julia to load the newly built libraries.

```
rm -rf aarch64/{bin, include, lib, share, ucx}
srun -p thor -N 1 -C aarch64 launch.sh ucx-asap/contrib/ifunc/julia/build.sh $HOME

rm -rf x86_64/{bin, include, lib, share, ucx}
srun -p thor -N 1 -C x86_64 launch.sh ucx-asap/contrib/ifunc/julia/build.sh $HOME
```

### Instantiating the Project
We need to instantiate the Project on both architectures. 

```sh
srun -p thor -N 1 -C aarch64 launch.sh julia -e "import Pkg; Pkg.instantiate(); Pkg.precompile()"
srun -p thor -N 1 -C x86_64 launch.sh julia -e "import Pkg; Pkg.instantiate(); Pkg.precompile()"
```

### SBatch script

```sh
#!/usr/bin/env bash

#SBATCH --ntasks-per-node=1
#SBATCH --partition=thor
#SBATCH --time=2:00:00
#SBATCH --job-name=julia-ucx
#SBATCH --exclusive
# #SBATCH --contiguous
#SBATCH --output=slurm_%j_out.log
#SBATCH --error=slurm_%j_err.log


cat > launch.sh << EoF_s
#! /bin/sh
ARCH=\$(uname -m)
export JULIA_LOAD_PATH=":${HOME}/\${ARCH}/ucx"
export PATH="${HOME}/\${ARCH}/julia-1.8.0-beta3/bin:\${PATH}"
exec \$*
EoF_s
chmod +x launch.sh

SRC_DIR=${HOME}/ucx-asap/contrib/ifunc/julia
export JULIA_PROJECT=${SRC_DIR}

srun --label -K -N $SLURM_NNODES ./launch.sh julia ${SRC_DIR}/launch.jl
```

### Running it

```
sbatch -N 2 --constraint="aarch64*1" sbatch.sh
```
