# This file is a part of UCX.jl. License is MIT: https://github.com/JuliaParallel/UCX.jl/blob/main/LICENSE.md
# Author: Valentin Churavy, 2022

if haskey(ENV, "SLURM_NODELIST")
  nodelist = readlines(`scontrol show hostnames`)
else
  error("must run under slurm")
end


hostname = first(split(gethostname(), "."))
primary = hostname == first(nodelist)

@info "Starting benchmark" primary nodelist hostname

include("pointer_chase.jl")

if !primary
  @info "Starting server" hostname
  main()
else
  sleep(60)
  @info "Starting client" hostname
  popfirst!(nodelist)
  main(TEST_GETS, nodelist)
end
