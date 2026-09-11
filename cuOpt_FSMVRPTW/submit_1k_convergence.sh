#!/bin/bash
#SBATCH --job-name=cuopt_1kconv
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --partition=sgpu
#SBATCH --time=03:00:00
#SBATCH --output=cuopt_1kconv.%J.out
#SBATCH --error=cuopt_1kconv.%J.err

# ---------------------------------------------------------------------------
# Is the 1,000-customer baseline converged at 60 s, or does it understate cuOpt?
#
# The baseline was run at 60/10/5/2 s. Across its 60 instances the mean cost was
# still improving 5.1% from 10 s to 60 s, and vehicles used were still falling
# (65 -> 49), which is the fleet-mix consolidation cuOpt performs as it gets
# more time. cuOpt's own documented default limit is num_locations/5 = 200 s at
# this size, so 60 s may well be short.
#
# This runs six instances -- one from each family, so both tight-window
# (C1/R1/RC1) and wide-window (C2/R2/RC2) behaviour is covered -- at 60, 200 and
# 600 s.
#
# Read the summary for where the curve flattens:
#   60 -> 200 small (say under 2%)   the existing baseline stands as run
#   60 -> 200 large                  the baseline understates cuOpt and the
#                                    whole 60-instance run should be repeated
#                                    at the converged limit
#   200 -> 600 still large           1k needs even longer than cuOpt's default
#
# Fleet is left at the file default (8,000). The time x fleet grid showed fleet
# size does not affect quality once the limit is adequate, so there is nothing
# to vary here.
#
# Runtime: 6 instances x (60+200+600) s = ~1.5 h plus overhead, against 3 h.
# ---------------------------------------------------------------------------

cd "$SLURM_SUBMIT_DIR"

module purge
source /home/apps/spack/share/spack/setup-env.sh
spack load python@3.12.13
source ~/cuopt_env/bin/activate

nvidia-smi --query-gpu=name,memory.total --format=csv

OUT=outputs_cuopt/conv_1000.csv
rm -f "$OUT"
rm -rf conv_in
mkdir -p conv_in outputs_cuopt
for f in C1_10_1_a C2_10_1_a R1_10_1_a R2_10_1_a RC1_10_1_a RC2_10_1_a; do
    cp "instances/1000/${f}.txt" conv_in/
done

python3 -u solve_cuopt_fsmvrptw.py \
    --testcase_dir conv_in \
    --output_csv "$OUT" \
    --timeouts 600 200 60

echo
echo "=============================================================="
echo "SUMMARY - where does 1,000-customer cost stop improving?"
echo "=============================================================="
python3 - "$OUT" <<'PY'
import csv, sys
from collections import defaultdict
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r.get("total_cost")]
g = defaultdict(list)
for r in rows:
    g[float(r["timeout_s"])].append(r)
m = lambda rs, k: sum(float(x[k]) for x in rs) / len(rs)

print()
print("%9s %5s %12s %12s %12s %10s %13s"
      % ("limit(s)", "n", "total", "routing", "fixed", "vehicles", "vs previous"))
prev = None
for L in sorted(g):
    rs = g[L]
    t = m(rs, "total_cost")
    imp = "" if prev is None else "%+.1f%%" % (100 * (t - prev) / prev)
    print("%9.0f %5d %12.0f %12.0f %12.0f %10.1f %13s"
          % (L, len(rs), t, m(rs, "routing_cost"), m(rs, "fixed_cost"),
             m(rs, "vehicles_used"), imp))
    prev = t

print()
print("Per family, total cost at each limit")
fams = sorted({r["family"] for r in rows})
lims = sorted(g)
print("%6s" % "family", end="")
for L in lims:
    print("%12s" % ("%gs" % L), end="")
print("%12s" % "60->max")
for f in fams:
    print("%6s" % f, end="")
    vals = []
    for L in lims:
        rs = [r for r in g[L] if r["family"] == f]
        v = m(rs, "total_cost") if rs else None
        vals.append(v)
        print("%12.0f" % v if v else "%12s" % "-", end="")
    if vals[0] and vals[-1]:
        print("%11.1f%%" % (100 * (vals[-1] - vals[0]) / vals[0]))
    else:
        print()

print()
print("overshoot past the limit:", end=" ")
for L in sorted(g):
    print("%gs -> %.0fs" % (L, m(g[L], "solve_time_s") - L), end="   ")
print()
print("verification failures:",
      sum(1 for r in rows if r["routing_cost_check"] != "OK"
          or r["cost_split_check"] != "OK"))
PY
