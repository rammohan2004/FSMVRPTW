#!/bin/bash
#SBATCH --job-name=cuopt_probe
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --partition=sgpu
#SBATCH --time=00:40:00
#SBATCH --output=cuopt_probe.%J.out
#SBATCH --error=cuopt_probe.%J.err

# ---------------------------------------------------------------------------
# One 10,000-customer instance, one time limit, three fleet sizes.
#
# The full 10k run returned status 4 (undocumented -- cuOpt's SolutionStatus is
# only 0..3) with solve_time_s = 664 against a 60 s limit. The time limit
# governs the search, so 11 minutes inside Solve() is time spent somewhere else,
# and the prime suspect is the fleet: 8 types x 10,000 = 80,000 vehicles, ten
# times what the 1,000-customer runs used.
#
# This sweeps the fleet down and prints the notes column, which now carries
# whatever cuOpt's own get_message / get_error_status / get_error_message say.
#
# Read the output for two things:
#   notes         - the real reason, instead of a bare status number
#   solve_time_s  - if it drops back near 60 s as the fleet shrinks, the fleet
#                   was the cause
# ---------------------------------------------------------------------------

cd "$SLURM_SUBMIT_DIR"

module purge
source /home/apps/spack/share/spack/setup-env.sh
spack load python@3.12.13
source ~/cuopt_env/bin/activate

nvidia-smi --query-gpu=name,memory.total --format=csv

mkdir -p probe_in outputs_cuopt
cp instances/10000/R1_10k_1_a.txt probe_in/
rm -f outputs_cuopt/probe.csv

# full fleet first (80,000) so the failure is reproduced, then smaller
for FLEET in 0 8000 4000; do
    if [ "$FLEET" = "0" ]; then
        echo "=== full fleet: 8 types x 10000 = 80000 vehicles ==="
        python3 -u solve_cuopt_fsmvrptw.py --testcase_dir probe_in \
            --output_csv outputs_cuopt/probe.csv --timeouts 60
    else
        echo "=== --max-fleet ${FLEET} ==="
        python3 -u solve_cuopt_fsmvrptw.py --testcase_dir probe_in \
            --output_csv outputs_cuopt/probe.csv --timeouts 60 \
            --max-fleet "${FLEET}"
    fi
    echo
done

echo "--- summary ---"
python3 - outputs_cuopt/probe.csv <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
print("%10s %10s %12s %10s %s" % ("fleet", "status", "solve_time_s",
                                  "vehicles", "notes"))
for r in rows:
    print("%10s %10s %12.1f %10s %s" % (
        r["n_vehicles_available"], r["status_name"],
        float(r["solve_time_s"] or 0), r["vehicles_used"] or "-",
        (r["notes"] or "")[:70]))
PY
