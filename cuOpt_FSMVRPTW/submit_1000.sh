#!/bin/bash
#SBATCH --job-name=cuopt_fsm_1000
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --partition=sgpu
#SBATCH --time=06:00:00
#SBATCH --output=cuopt_fsm_1000.%J.out
#SBATCH --error=cuopt_fsm_1000.%J.err

# sgpu is the single-GPU partition. The 'gpu' partition has a minimum of
# 2 GPUs, so a --gres=gpu:1 job belongs here.
#
# If an account is required, uncomment and fill in:
#   sacctmgr show assoc user=$USER
##SBATCH -A <account-name>

cd "$SLURM_SUBMIT_DIR"

module purge
source /home/apps/spack/share/spack/setup-env.sh
spack load python@3.12.13
source ~/cuopt_env/bin/activate

nvidia-smi --query-gpu=name,memory.total --format=csv

bash run_cuopt_fsmvrptw.sh 1000
