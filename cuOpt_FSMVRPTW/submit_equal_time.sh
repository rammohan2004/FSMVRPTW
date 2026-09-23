#!/bin/bash
#SBATCH --job-name=cuopt_eqtime
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --partition=sgpu
#SBATCH --time=24:00:00
#SBATCH --output=cuopt_eqtime.%J.out
#SBATCH --error=cuopt_eqtime.%J.err

# ---------------------------------------------------------------------------
# EQUAL-TIME cuOpt baseline: each instance gets cuOpt's time limit set to OUR
# OWN runtime on that same instance.
#
#   sbatch submit_equal_time.sh 1000      # equal time + a 2 s control
#   sbatch submit_equal_time.sh 10000     # equal time + 600 s and 2000 s
#
# WHY PER-INSTANCE. Comparing our result against a fixed ladder rung is only
# approximately equal time. At 1,000 customers our runtimes span 1.22-2.78 s and
# 58 of 60 instances finish in UNDER 2 s, so measuring against cuOpt's 2 s rung
# hands cuOpt more time than we used on almost every instance. Matching
# instance-by-instance removes that slack in both directions.
#
# THE LADDER ON TOP. At 10k, cuOpt's documented default is num_locations/5 =
# 2000 s, and it was still improving 8.5% from 600 to 2000 s, so those rungs
# answer "what if cuOpt gets its normal budget?" in the same table. At 1k the
# full 2/5/10/60 ladder is already measured, so only a 2 s control is re-run --
# it should reproduce the existing numbers, which checks run-to-run stability.
#
# FLEET CAP 8000 -- the same number the 1,000-customer baseline used, where it
# was the full declared fleet (1000 per type x 8 types) and never bound.
#
# At 10k the declared fleet is 10,000 per type = 80,000, and cuOpt dies on it
# with error_status=6 'Memory allocation failed', returning status 4 (outside
# its own documented enum). So SOME cap is unavoidable here; 8000 keeps the
# number consistent with 1k and is the conservative choice against us, since our
# own solver has no cap at all and simply uses what it needs (~304 vehicles).
#
# The 60 s fleet sweep made 8000 look worse than 4000 (576-586 vehicles vs
# 443-459), but every row there is at 60 s -- about 3% of cuOpt's documented
# default at this size -- and the fleet effect was separately measured to vanish
# with adequate time (22.7% gap at 60 s, 0.9% at 300 s, -1.8% at 2000 s). No cap
# in that sweep ever actually bound, so the differences are search behaviour,
# not the constraint.
#
# COST. 1k: ~88 s of solve time for all 60. 10k: ~8 h for all 10.
# ---------------------------------------------------------------------------

set -uo pipefail

SIZE="${1:?usage: sbatch submit_equal_time.sh <1000|10000>}"

case "${SIZE}" in
    1000)  LADDER=(2) ;;
    10000) LADDER=(600 2000) ;;
    *)     LADDER=() ;;
esac

MATCH="outputs_cuopt/our_times_${SIZE}.csv"
OUT="outputs_cuopt/equal_time_${SIZE}.csv"

cd "$SLURM_SUBMIT_DIR"

# No `module purge`: in the batch environment the module function points at a
# path that does not exist, returns 0 anyway, and leaves PATH without a usable
# toolchain. No sample script in the PARAM Rudra manual purges either.
export SPACK_ROOT=/home/apps/spack
. "$SPACK_ROOT/share/spack/setup-env.sh"
spack load python@3.12.13
source ~/cuopt_env/bin/activate

if [ ! -f "${MATCH}" ]; then
    echo "ERROR: ${MATCH} not found." >&2
    echo "It supplies the per-instance equal-time budgets. Create it with:" >&2
    echo "    cp ~/FSMVRPTW_Solver/outputs/${SIZE}/results.csv ${MATCH}" >&2
    exit 1
fi

nvidia-smi --query-gpu=name,memory.total --format=csv
echo
echo "instance set      : instances/${SIZE}"
echo "equal-time from   : ${MATCH}"
echo "extra ladder      : ${LADDER[*]:-none}"
echo "fleet cap         : 8000"
echo "output            : ${OUT}"
echo

python3 -u solve_cuopt_fsmvrptw.py \
    --testcase_dir "instances/${SIZE}" \
    --output_csv "${OUT}" \
    --match-times "${MATCH}" \
    --timeouts "${LADDER[@]}" \
    --max-fleet 8000
