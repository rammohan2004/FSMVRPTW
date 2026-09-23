#!/bin/bash
# build_solver.sh -- run this ON THE LOGIN NODE, not through sbatch.
#
#   bash build_solver.sh          # parallel (OpenMP) build, the default
#   bash build_solver.sh seq      # sequential build
#
# PARAM Rudra User Manual, p.39:
#   "Compilations are performed on the login node. Only the execution is
#    scheduled via SLURM on the compute nodes."
#
# That is why the compute nodes have no g++ -- by design, not by fault. Building
# inside the job was the wrong shape for this cluster and simply cannot work.
#
# The spack incantation is the manual's own (p.49), hash included: plain
# `spack load gcc@13.3.0` is ambiguous because several installs share that
# version across cascadelake / skylake_avx512 / x86_64 targets.
#
# Compiling is light work and is what login nodes are for; the manual's
# prohibition is on running JOBS there, not on running the compiler.

set -euo pipefail

MODE="${1:-par}"

export SPACK_ROOT=/home/apps/spack
. "$SPACK_ROOT/share/spack/setup-env.sh"
spack load gcc/wnu2dj5

echo "=== build environment ==="
echo "host : $(hostname)"
echo "g++  : $(command -v g++ || echo 'NOT FOUND')"
g++ --version | head -1
echo

if ! command -v g++ >/dev/null; then
    echo "No g++ after 'spack load gcc/wnu2dj5'." >&2
    echo "Check the hash is still current:  spack find -l gcc" >&2
    exit 1
fi

if [ "${MODE}" = "seq" ]; then
    make
else
    make par
fi

echo
echo "Built: $(ls -la solve_cvrptw | awk '{print $NF, $5" bytes"}')"
echo
echo "Now submit the run:   sbatch submit_solver.sh 1000"
