#!/bin/bash
#SBATCH --job-name=cuopt_fleetsweep
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --partition=sgpu
#SBATCH --time=02:00:00
#SBATCH --output=cuopt_fleetsweep.%J.out
#SBATCH --error=cuopt_fleetsweep.%J.err

# ---------------------------------------------------------------------------
# How much does the fleet size we hand cuOpt change its answer?
#
# FSMVRPTW assumes unlimited vehicles per type. The honest bound is n per type
# (every vehicle used serves at least one customer), but cuOpt fails on that at
# 10,000 customers -- error_status=6, 'Memory allocation failed', with 80,000
# vehicles. So the fleet must be capped, and the cap is a parameter that is not
# part of the problem definition. This measures how much it matters, so the
# baseline can be set deliberately rather than by accident.
#
# A single probe on one 10k instance showed the 4,000-vehicle answer was 20.9%
# CHEAPER than the 8,000-vehicle one, even though the smaller fleet's solution
# was feasible in the larger problem. That was n=1 and cuOpt is stochastic, so
# it needs confirming across instances.
#
# 1,000 customers is the CONTROL. There, overhead past the time limit was only
# 1-3 s (against 104 s at 10k), so if the overhead explanation is right, 1k
# should be flat. If 1k also swings ~20%, the explanation is wrong and we need
# to look elsewhere before reporting any of this.
#
# 60 s time limit only -- the shorter limits are meaningless at 10k, where fixed
# overhead alone exceeds them.
#
# Watch fleet_cap_binding: at the smallest fleets a vehicle type may run out,
# which makes that row a limited-fleet problem, not FSMVRPTW. Such rows are not
# comparable and a lower cost there means nothing.
#
# Runtime: 5 instances x 4 fleets x 2 sizes ~ 1 hour.
# ---------------------------------------------------------------------------

cd "$SLURM_SUBMIT_DIR"

module purge
source /home/apps/spack/share/spack/setup-env.sh
spack load python@3.12.13
source ~/cuopt_env/bin/activate

nvidia-smi --query-gpu=name,memory.total --format=csv

OUT=outputs_cuopt/fleet_sweep.csv
rm -f "$OUT"
rm -rf sweep_1000 sweep_10000
mkdir -p sweep_1000 sweep_10000 outputs_cuopt

# 1,000: one instance from each of five families, so the control spans both
# tight-window (C1/R1/RC1) and wide-window (C2/R2) behaviour.
for f in C1_10_1_a R1_10_1_a RC1_10_1_a C2_10_1_a R2_10_1_a; do
    cp "instances/1000/${f}.txt" sweep_1000/
done

# 10,000: the generated set is all R1, so take the first five.
for i in 1 2 3 4 5; do
    cp "instances/10000/R1_10k_${i}_a.txt" sweep_10000/
done

for SIZE in 1000 10000; do
    for FLEET in 8000 4000 2000 1000; do
        echo "=== ${SIZE} customers, --max-fleet ${FLEET} ==="
        python3 -u solve_cuopt_fsmvrptw.py \
            --testcase_dir "sweep_${SIZE}" \
            --output_csv "$OUT" \
            --timeouts 60 \
            --max-fleet "${FLEET}"
        echo
    done
done

echo "=============================================================="
echo "SUMMARY - mean total cost by size and fleet"
echo "=============================================================="
python3 - "$OUT" <<'PY'
import csv, sys
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
g = defaultdict(list)
for r in rows:
    g[(int(r["n_customers"]), int(r["n_vehicles_available"]))].append(r)

for size in sorted({k[0] for k in g}):
    print()
    print("%d customers" % size)
    print("  %8s %8s %14s %10s %12s %s"
          % ("fleet", "per type", "mean total", "mean veh", "mean solve_s", "cap binding"))
    base = None
    for fleet in sorted({k[1] for k in g if k[0] == size}, reverse=True):
        rs = [r for r in g[(size, fleet)] if r.get("total_cost")]
        if not rs:
            print("  %8d %8d %14s" % (fleet, fleet // 8, "ALL FAILED"))
            continue
        m = sum(float(r["total_cost"]) for r in rs) / len(rs)
        v = sum(int(r["vehicles_used"]) for r in rs) / len(rs)
        t = sum(float(r["solve_time_s"]) for r in rs) / len(rs)
        bind = sum(1 for r in rs if r["fleet_cap_binding"])
        if base is None:
            base = m
        print("  %8d %8d %14.0f %10.1f %12.1f %s"
              % (fleet, fleet // 8, m, v, t,
                 "%d/%d rows" % (bind, len(rs)) if bind else "-"))
    if base:
        best = min(sum(float(r["total_cost"]) for r in g[(size, f)]
                       if r.get("total_cost"))
                   / max(1, len([r for r in g[(size, f)] if r.get("total_cost")]))
                   for f in {k[1] for k in g if k[0] == size}
                   if any(r.get("total_cost") for r in g[(size, f)]))
        print("  spread between best and largest-fleet: %.1f%%"
              % (100.0 * (base - best) / base))
PY
