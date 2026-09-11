# cuOpt on FSMVRPTW — self-contained run folder

Everything needed to benchmark NVIDIA cuOpt on the Fleet Size and Mix VRP with Time
Windows. Copy this whole folder to Paramshakti and run.

```bash
scp -r cuOpt_FSMVRPTW cs25m017@paramshakti.iitm.ac.in:~/
cd ~/cuOpt_FSMVRPTW
sbatch submit_1000.sh
```

## Objective

    total cost = total distance travelled + fixed cost of the vehicles used

Cost per unit distance is the same for every vehicle type; the types differ only in
capacity and fixed cost, and each is available in unlimited supply.

## Instances

All use **cost structure a only** (full fixed costs — no b or c).

| directory | files | customers | source |
|---|---|---|---|
| `instances/1000` | 60 | 1,000 | Gehring & Homberger (1999), the standard benchmark |
| `instances/10000` | 10 | 10,000 | randomly generated |
| `instances/100000` | 10 | 100,000 | randomly generated |

**Vehicle types** come from Bräysy, Porkka, Dullaert, Repoussis & Tarantilis (2009),
Table 1 — 8 types per family, applied unchanged at every size. This follows the paper:
*"The same 8 vehicle types and costs are used for every problem size. The vehicle
capacities and costs differ only between the six problem sets."* Only the vehicle
**count** per type changes with the customer count, set to the number of customers so
the count can never bind.

The 1,000-customer set spans all six families (C1, C2, R1, R2, RC1, RC2). The generated
sets use randomly positioned customers, so they carry the R1 vehicle table.

## Running

```bash
bash run_cuopt_fsmvrptw.sh 1000        # -> outputs_cuopt/fsmvrptw_1000_results.csv
bash run_cuopt_fsmvrptw.sh 10000
bash run_cuopt_fsmvrptw.sh 100000
```

or via SLURM: `sbatch submit_1000.sh`, `submit_10000.sh`, `submit_100000.sh`.

Time limits are 60, 10, 5 and 2 seconds per instance, the same ladder used for every run
in this project so results stay comparable.

Jobs use the **`sgpu`** partition. The `gpu` partition has a minimum of 2 GPUs, so a
single-GPU job belongs on `sgpu`.

## Results

One CSV row per (instance, time limit). The three quantities of interest are separate
columns — `routing_cost`, `fixed_cost`, and the fleet composition as `mix` (e.g.
`A126B30C1`) plus one `used_X` column per type.

Four verification columns guard the numbers:

| column | meaning |
|---|---|
| `cost_source` | `cuopt` if the solver reported the split itself, `derived` if reconstructed here |
| `routing_cost_check` | routing cost vs the distance of the returned routes, recomputed independently |
| `cost_split_check` | whether routing + fixed reconciles to the objective |
| `fleet_cap_binding` | non-empty means a vehicle type ran out, so the run was not unlimited-supply |

`make_sheet.py` turns a results CSV into a readable sheet for import into Google Sheets:

```bash
python3 make_sheet.py outputs_cuopt/fsmvrptw_1000_results.csv outputs_cuopt/sheet.csv
```

## Known limit at 100,000 customers

cuOpt takes a **dense** cost matrix and a **dense** transit-time matrix, each
(customers+1)² float32, and both must be in GPU memory alongside the solver's working set:

| customers | one matrix | both | fits an 80 GB A100? |
|---|---|---|---|
| 1,000 | 0.004 GB | 0.008 GB | yes |
| 10,000 | 0.40 GB | 0.80 GB | yes |
| 100,000 | 40 GB | **80 GB** | **no** |

The 100,000-customer runs are expected to fail on memory. They are included anyway
because the instances are needed for our own solver, and because the actual failure mode
is worth seeing rather than predicting. Run `submit_100000.sh` and read the `.err` file.

The distance matrix is built with numpy in row blocks. A pure-Python build would need
~3.2 GB and 10⁸ interpreted iterations at 10,000 customers, and is not viable at all
above that.

## Files

```
solve_cuopt_fsmvrptw.py    the solver harness (parses instances, drives cuOpt, verifies)
run_cuopt_fsmvrptw.sh      runs one size
submit_1000.sh             SLURM wrappers, one per size
submit_10000.sh
submit_100000.sh
make_sheet.py              results CSV -> readable sheet
instances/                 1000, 10000, 100000
outputs_cuopt/             results land here
```

## Environment on Paramshakti

One-time setup on the **login** node:

```bash
python3 -m venv ~/cuopt_env
source ~/cuopt_env/bin/activate
pip install --extra-index-url=https://pypi.nvidia.com 'cuopt-cu12==26.2.*'
```

Use `cuopt-cu13` if the cluster's CUDA runtime is 13.x — check with `nvidia-smi` on a GPU
node. numpy comes in as a cuOpt dependency.
