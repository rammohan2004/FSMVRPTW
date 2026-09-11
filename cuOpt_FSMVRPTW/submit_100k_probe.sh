#!/bin/bash
#SBATCH --job-name=cuopt_100kprobe
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --partition=sgpu
#SBATCH --time=01:30:00
#SBATCH --output=cuopt_100kprobe.%J.out
#SBATCH --error=cuopt_100kprobe.%J.err

# ---------------------------------------------------------------------------
# Can cuOpt solve a 100,000-customer instance at all, and if not, where exactly
# does it break?
#
# Two independent walls are expected:
#
#   1. Memory. cuOpt takes a DENSE cost matrix and a DENSE transit-time matrix,
#      each (customers+1)^2 float32. At 100,001 locations that is 40 GB each,
#      80 GB for the pair, against an 80 GB A100 -- before the solver's own
#      working set. Our numpy build of one matrix also needs 40 GB of host RAM.
#
#   2. Fleet. The unlimited-supply bound is n per type = 800,000 vehicles.
#      cuOpt already failed at 80,000 with 'Memory allocation failed' and an
#      undocumented status 4, so this alone would sink it.
#
# The run goes in stages so we learn WHICH wall is hit, rather than just seeing
# a job die. Each stage prints before it starts, so the last line printed tells
# us how far it got even if the process is killed outright.
#
# This matters beyond curiosity: if cuOpt genuinely cannot solve 100k, then a
# solver that can is a contribution in its own right, not just a faster one.
# That would shape how the whole project is pitched, so it is worth knowing now.
#
# Runtime: hard to predict. If it dies on allocation it will be quick. The 90 min
# allocation is generous; cancel with scancel if it sits doing nothing.
# ---------------------------------------------------------------------------

cd "$SLURM_SUBMIT_DIR"

module purge
source /home/apps/spack/share/spack/setup-env.sh
spack load python@3.12.13
source ~/cuopt_env/bin/activate

echo "=== GPU and host memory available ==="
nvidia-smi --query-gpu=name,memory.total,memory.free --format=csv
free -g | head -2
echo

echo "=== predicted requirement ==="
python3 - <<'PY'
for n in (1000, 10000, 100000):
    one = (n + 1) ** 2 * 4
    print("  %7d customers: one matrix %7.2f GB, cost+transit %7.2f GB"
          % (n, one / 1e9, 2 * one / 1e9))
PY
echo

mkdir -p probe100k outputs_cuopt
cp instances/100000/R1_100k_1_a.txt probe100k/

echo "=== STAGE 1: parse the instance and build the distance matrix only ==="
echo "    (no cuOpt involved -- tests whether we can even form the matrix)"
python3 -u - <<'PY'
import sys, time, traceback
sys.path.insert(0, ".")
from solve_cuopt_fsmvrptw import parse_instance, build_distance_matrix
try:
    t = time.perf_counter()
    inst = parse_instance("probe100k/R1_100k_1_a.txt")
    print("    parsed %d locations in %.1f s"
          % (len(inst["coords"]), time.perf_counter() - t))
    t = time.perf_counter()
    m = build_distance_matrix(inst["coords"])
    print("    built matrix %s %s, %.2f GB, in %.1f s"
          % (m.shape, m.dtype, m.nbytes / 1e9, time.perf_counter() - t))
    print("    STAGE 1 OK")
except Exception:
    print("    STAGE 1 FAILED:")
    traceback.print_exc()
PY
echo

echo "=== STAGE 2: full run, fleet capped to 8000 ==="
echo "    (8000 is the largest fleet cuOpt handled at 10k)"
python3 -u solve_cuopt_fsmvrptw.py \
    --testcase_dir probe100k \
    --output_csv outputs_cuopt/probe_100k.csv \
    --timeouts 60 \
    --max-fleet 8000
echo

echo "=== RESULT ==="
if [ -f outputs_cuopt/probe_100k.csv ]; then
    python3 - outputs_cuopt/probe_100k.csv <<'PY'
import csv, sys
for r in csv.DictReader(open(sys.argv[1])):
    print("  status      :", r["status_name"])
    print("  solve_time_s:", r["solve_time_s"])
    print("  total_cost  :", r["total_cost"] or "-")
    print("  vehicles    :", r["vehicles_used"] or "-")
    print("  notes       :", r["notes"] or "-")
PY
else
    echo "  no CSV written -- the process died before the first row."
    echo "  Read cuopt_100kprobe.*.err for the reason."
fi
