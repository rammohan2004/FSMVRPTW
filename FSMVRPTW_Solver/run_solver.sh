#!/bin/bash
# run_solver.sh
#
# Runs the solver over every instance in one directory.
#
#   bash run_solver.sh 1000              # -> outputs/1000/
#   bash run_solver.sh 10000
#   bash run_solver.sh reference/cvrptw_1000 outputs/ref 30
#
# Per-instance stdout (routes, per-phase costs) goes to outputs/<set>/<name>.out
# The solver's summary line goes to stderr, collected in outputs/<set>/results.txt
#
# NOTE: until step H of Solver-Port-Plan.md is done, the solver prints Dinesh's
# original CVRPTW summary, not the FSMVRPTW decomposition. Once that lands this
# script should emit the same CSV columns the cuOpt harness uses, so one sheet
# can hold both solvers.

set -euo pipefail

SET="${1:?usage: run_solver.sh <1000|10000|path> [outdir] [angle]}"
if [ -d "instances/${SET}" ]; then
    IN="instances/${SET}"
    TAG="${SET}"
else
    IN="${SET}"
    TAG="$(basename "${SET}")"
fi
OUTDIR="${2:-outputs/${TAG}}"
ANGLE="${3:-30}"

if [ ! -d "${IN}" ]; then
    echo "ERROR: ${IN} not found" >&2
    exit 1
fi

make

mkdir -p "${OUTDIR}"
RESULTS="${OUTDIR}/results.txt"
: > "${RESULTS}"

echo "Instances : ${IN}  ($(ls "${IN}" | wc -l) files)"
echo "Angle     : ${ANGLE}"
echo "Output    : ${OUTDIR}"
echo

for infile in "${IN}"/*.txt; do
    name="$(basename "${infile}" .txt)"
    echo "  ${name}"
    ./solve_cvrptw "${infile}" "${ANGLE}" \
        > "${OUTDIR}/${name}.out" 2>> "${RESULTS}"
done

echo
echo "Done. Summary lines in ${RESULTS}"
