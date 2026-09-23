# FSMVRPTW solver

Our solver for the Fleet Size and Mix VRP with Time Windows, built by extending Dinesh
Kumar's CVRPTW solver.

**Objective:** `total cost = total distance travelled + fixed cost of the vehicles used`.
Vehicle types differ in capacity and fixed cost, with unlimited supply of each.

## State: unported

The source here is a **byte-identical copy** of `Dinesh-Code-Base/` (verified with
`diff -r`). It currently solves homogeneous CVRPTW — one capacity, no fixed costs — and
reads only our FSMVRPTW format: a plain Solomon file is rejected with a clear error, since
the solver has no use for homogeneous instances.

That is deliberate. Keeping the starting point identical means `diff -r ../Dinesh-Code-Base`
shows exactly what we changed, which is what the thesis needs.

**The plan of changes is `../Solver-Port-Plan.md`.** Work through it in order; step 1 is the
parser and the vehicle-type table.

## Layout

```
solve_cvrptw.cpp          main pipeline: read -> cluster -> Clarke-Wright -> improve -> post
Makefile                  make (sequential) | make par (OpenMP)
lib/vrp.{h,cpp}           data model: Point[], flat triangular dist[], ONE scalar capacity
lib/route_utils.{h,cpp}   distance, total cost, feasibility checks  <- the objective lives here
lib/cluster/              angle-sweep clustering, sized by capacity
lib/clark/                Clarke-Wright with a waiting-time penalty
lib/optim/                inter-route (relocate, swap, 2-opt*) and intra-route (TSP) moves

instances/1000            60 FSMVRPTW instances, 1,000 customers   <- our target
instances/10000           10 FSMVRPTW instances, 10,000 customers
outputs/                  results land here
```

The 100,000-customer instances are deliberately **not** copied. The solver holds a full
triangular distance matrix — 40 GB at that size — so 100k needs a different distance
strategy first (section I of the plan). They live in `../cuOpt_FSMVRPTW/instances/100000`.

## Building and running

```bash
make            # sequential
make par        # OpenMP

bash run_solver.sh 1000     # instances/1000  -> outputs/1000/
                            # (needs solve_cvrptw already built)
bash run_solver.sh 10000
```

The binary takes `<instance> <angle_range>`; the angle controls the sweep clustering and
defaults to 30 in `run_solver.sh`.

On Paramshakti it is **two steps, and they run on different machines**:

```bash
bash build_solver.sh          # ON THE LOGIN NODE -- compiles
sbatch submit_solver.sh 1000  # compute node -- runs only
```

The PARAM Rudra manual (p.39) states: *"Compilations are performed on the login node. Only
the execution is scheduled via SLURM on the compute nodes."* The cluster enforces this by
not installing a compiler on the compute nodes at all — `/usr/bin/g++` exists on the login
node and does not exist on `rscn*`. A job script that tries to build its own binary fails
with `g++: command not found`. Compiling on the login node is expected; only *jobs* are
forbidden there.

`build_solver.sh` uses the manual's own spack incantation, hash included, because a bare
version spec is ambiguous across the cluster's several GCC installs:

```bash
. /home/apps/spack/share/spack/setup-env.sh
spack load gcc/wnu2dj5        # gcc@13.3.0
```

`submit_solver.sh` loads the same GCC even though it compiles nothing: the binary links
against that GCC's `libstdc++` and `libgomp`, and the manual requires matching libraries
between compilation and execution.

Two traps worth knowing. **Never `module purge` in a batch script** — the `module` function
there points at a path that does not exist, prints an error, *returns 0 anyway*, and leaves
`PATH` with no compiler; because it reports success, `|| fallback` never fires. And
`-march=native` is unsafe when compiling and running on different machines, so the Makefile
targets an ISA level (`-march=haswell`, i.e. AVX2/FMA/BMI2) instead. Override with
`make ARCH=-march=cascadelake par` if a measurement justifies it.

This is a **CPU / OpenMP** job, so it uses the `small` partition — not `sgpu`, which is for
the GPU cuOpt runs. `small` allows up to 48 cores and three concurrent jobs, so solver runs
do not queue behind cuOpt.

## How results get compared

cuOpt's numbers are in `../cuOpt_FSMVRPTW/outputs_cuopt/`, measured across several time
limits. This solver is a **terminating pipeline** — it runs to a local optimum and stops —
so it produces a single `(time, cost)` point rather than a curve.

The comparison is therefore **time-to-target**, read off the cuOpt curve two ways:

- at our runtime `T`, what cost did cuOpt reach? (equal time, compare cost)
- to reach our cost `C`, how long did cuOpt need? (equal cost, compare time)

See `../cuOpt-Baseline-Findings.md` for the measured cuOpt curve and the reasoning.

This solver emits the same CSV columns as the cuOpt harness (`total_cost, routing_cost,
fixed_cost, vehicles_used, mix, used_A..used_H, solve_time_s`) so a single sheet can hold
both, written to `outputs/<set>/results.csv`.

Four extra columns — `routing_cost_check`, `cost_split_check`, `customers_covered`,
`feasible` — carry the verification result with every row. All four must read `OK`; the
solver also exits non-zero and `run_solver.sh` reports the count, so a failure cannot pass
unnoticed into a sheet.
