#!/bin/bash
#SBATCH --job-name=cuopt_timegrid
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --partition=sgpu
#SBATCH --time=08:00:00
#SBATCH --output=cuopt_timegrid.%J.out
#SBATCH --error=cuopt_timegrid.%J.err

# ---------------------------------------------------------------------------
# Does the fleet size actually matter, or were we comparing two runs that had
# barely started?
#
# At 10,000 customers a 60 s limit gave a 20.9% cost gap between fleets of 8000
# and 4000 (single probe). But cuOpt's own documented default limit is
# num_locations/5 -- about 2000 s at this size -- so 60 s is roughly 3% of what
# it expects. Both runs may simply have been undercooked and landed in
# different places for that reason, not because of the fleet.
#
# A time sweep at one fleet size would only show convergence; a fleet sweep at
# one time limit only shows the gap at that limit. Separating the two effects
# needs the grid: BOTH fleet sizes at EVERY time limit.
#
#   fleet 8000 x limits 60, 300, 600, 2000
#   fleet 4000 x limits 60, 300, 600, 2000
#
# Three instances, so no single stochastic run drives the conclusion. The fleet
# sweep already covers the 60 s point across five instances, so this job only
# has to answer whether the gap SHRINKS as the limit grows -- and a 21% gap
# collapsing to ~1% does not need many samples to see.
#
# Read the summary for three things:
#
#   1. Does the 8000-vs-4000 gap shrink with the limit? If it collapses by
#      2000 s, fleet size was never the issue -- undercooked runs were, and the
#      baseline just needs a longer limit.
#   2. Where does cost flatten? That is convergence at this size, and it gives
#      an honest converged cuOpt baseline to compare our solver against.
#   3. Is the overshoot a FIXED chunk? 104 s over a 60 s limit at fleet 8000.
#      If roughly the same 104 s appears over a 2000 s limit, it is one unit of
#      work, consistent with cuOpt checking the limit only periodically (which
#      its LP/MILP documentation states explicitly).
#
# Runtime: 3 instances x 2 fleets x (60+300+600+2000) s plus measured overshoot
# = about 5.5 h against the 8 h allocation.
# ---------------------------------------------------------------------------

cd "$SLURM_SUBMIT_DIR"

module purge
source /home/apps/spack/share/spack/setup-env.sh
spack load python@3.12.13
source ~/cuopt_env/bin/activate

nvidia-smi --query-gpu=name,memory.total --format=csv

OUT=outputs_cuopt/time_fleet_grid.csv
rm -f "$OUT"
rm -rf grid_in
mkdir -p grid_in outputs_cuopt
for i in 1 2 3; do
    cp "instances/10000/R1_10k_${i}_a.txt" grid_in/
done

for FLEET in 8000 4000; do
    for LIMIT in 60 300 600 2000; do
        echo "=== fleet ${FLEET}, time limit ${LIMIT} s ==="
        python3 -u solve_cuopt_fsmvrptw.py \
            --testcase_dir grid_in \
            --output_csv "$OUT" \
            --timeouts "${LIMIT}" \
            --max-fleet "${FLEET}"
        echo
    done
done

echo "=============================================================="
echo "SUMMARY"
echo "=============================================================="
python3 - "$OUT" <<'PY'
import csv, sys
from collections import defaultdict
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r.get("total_cost")]
if not rows:
    sys.exit("no solved rows")
g = defaultdict(list)
for r in rows:
    g[(int(r["n_vehicles_available"]), float(r["timeout_s"]))].append(r)

fleets = sorted({k[0] for k in g}, reverse=True)
limits = sorted({k[1] for k in g})
big, small = fleets[0], fleets[-1]


def mean(rs, f):
    return sum(f(x) for x in rs) / len(rs)


print()
print("1. Mean total cost, and the gap between fleets")
print("%10s %8s %14s %14s %9s" % ("limit(s)", "n", "fleet %d" % big,
                                  "fleet %d" % small, "gap %"))
for L in limits:
    a, b = g.get((big, L), []), g.get((small, L), [])
    if not a or not b:
        continue
    ma, mb = mean(a, lambda r: float(r["total_cost"])), \
             mean(b, lambda r: float(r["total_cost"]))
    print("%10.0f %8d %14.0f %14.0f %8.1f%%"
          % (L, len(a), ma, mb, 100.0 * (ma - mb) / ma))

print()
print("2. Convergence: cost change from one limit to the next")
for fl in fleets:
    print("   fleet %5d:" % fl, end=" ")
    prev = None
    for L in limits:
        rs = g.get((fl, L), [])
        if not rs:
            continue
        m = mean(rs, lambda r: float(r["total_cost"]))
        if prev:
            print("%.0f->%.0f %+.1f%%" % (prev[0], L,
                  100.0 * (m - prev[1]) / prev[1]), end="   ")
        prev = (L, m)
    print()

print()
print("3. Overshoot past the limit -- fixed chunk, or proportional?")
print("%10s %14s %12s %14s %12s" % ("limit(s)", "%d time" % big, "over",
                                    "%d time" % small, "over"))
for L in limits:
    a, b = g.get((big, L), []), g.get((small, L), [])
    if not a or not b:
        continue
    ta, tb = mean(a, lambda r: float(r["solve_time_s"])), \
             mean(b, lambda r: float(r["solve_time_s"]))
    print("%10.0f %14.1f %12.1f %14.1f %12.1f" % (L, ta, ta - L, tb, tb - L))

bad = [r for r in rows if r["fleet_cap_binding"]]
print()
print("rows where a vehicle type ran out: %d  (not FSMVRPTW; exclude them)" % len(bad))
print("verification failures: %d"
      % sum(1 for r in rows if r["routing_cost_check"] != "OK"
            or r["cost_split_check"] != "OK"))
PY
