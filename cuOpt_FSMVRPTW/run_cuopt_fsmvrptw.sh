#!/bin/bash
# run_cuopt_fsmvrptw.sh
#
# Runs cuOpt on one instance-size directory and writes fixed cost, routing cost
# and the vehicle decomposition to outputs_cuopt/.
#
# Usage:
#   bash run_cuopt_fsmvrptw.sh 1000
#   bash run_cuopt_fsmvrptw.sh 10000
#   bash run_cuopt_fsmvrptw.sh 100000
#   bash run_cuopt_fsmvrptw.sh 1000 outputs_cuopt/custom.csv

set -euo pipefail

SIZE="${1:?usage: run_cuopt_fsmvrptw.sh <1000|10000|100000> [output.csv]}"
TESTCASE_DIR="instances/${SIZE}"
OUTPUT_CSV="${2:-outputs_cuopt/fsmvrptw_${SIZE}_results.csv}"

# Same ladder used for every run in this project, so result sheets stay
# directly comparable across sizes and against earlier runs.
TIMEOUTS=(60 10 5 2)

if [ ! -d "${TESTCASE_DIR}" ]; then
    echo "ERROR: ${TESTCASE_DIR} not found." >&2
    echo "Expected instances/1000, instances/10000 or instances/100000." >&2
    exit 1
fi

mkdir -p "$(dirname "${OUTPUT_CSV}")"

echo "Instances   : ${TESTCASE_DIR}  ($(ls "${TESTCASE_DIR}" | wc -l) files)"
echo "Timeouts (s): ${TIMEOUTS[*]}"
echo "Objective   : total distance + fixed cost of vehicles used"
echo "Output      : ${OUTPUT_CSV}"
echo

# -u: unbuffered, so the SLURM .out file shows progress instead of sitting
# empty behind Python's block buffering.
python3 -u solve_cuopt_fsmvrptw.py \
    --testcase_dir "${TESTCASE_DIR}" \
    --output_csv "${OUTPUT_CSV}" \
    --timeouts "${TIMEOUTS[@]}"

echo
echo "Done. Results in ${OUTPUT_CSV}"
