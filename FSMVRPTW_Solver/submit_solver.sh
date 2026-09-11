#!/bin/bash
#SBATCH --job-name=fsm_solver
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=48
#SBATCH --partition=small
#SBATCH --time=04:00:00
#SBATCH --output=fsm_solver.%J.out
#SBATCH --error=fsm_solver.%J.err

# ---------------------------------------------------------------------------
# This is a CPU / OpenMP solver, so it belongs on the 'small' partition, NOT on
# sgpu. Per the PARAM Rudra login banner:
#
#   small*  1 - 48 cores    2-00:00:00   3 running / 7 queued per user
#   sgpu    1 core / 1 GPU  2-00:00:00   1 running / 2 queued per user
#
# small is the cluster default, allows up to 48 cores, and permits three
# concurrent jobs rather than one -- so solver runs need not queue behind the
# cuOpt jobs.
#
# Usage:  sbatch submit_solver.sh 1000
#         sbatch submit_solver.sh 10000
# ---------------------------------------------------------------------------

SET="${1:-1000}"

cd "$SLURM_SUBMIT_DIR"

module purge
source /home/apps/spack/share/spack/setup-env.sh

# OpenMP threads = the cores we asked SLURM for.
export OMP_NUM_THREADS="${SLURM_CPUS_PER_TASK:-48}"
echo "OMP_NUM_THREADS=$OMP_NUM_THREADS"
echo

# Build the parallel variant (adds -fopenmp -DUSE_PARALLEL).
make clean
make par

bash run_solver.sh "${SET}"
