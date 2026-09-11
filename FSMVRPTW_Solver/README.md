# FSMVRPTW solver

Our solver for the Fleet Size and Mix VRP with Time Windows, built by extending Dinesh
Kumar's CVRPTW solver.

**Objective:** `total cost = total distance travelled + fixed cost of the vehicles used`.
Vehicle types differ in capacity and fixed cost, with unlimited supply of each.

## State: unported

The source here is a **byte-identical copy** of `Dinesh-Code-Base/` (verified with
`diff -r`). It currently solves homogeneous CVRPTW — one capacity, no fixed costs — and
will misread the FSMVRPTW instance files, because their VEHICLE section has eight rows
where the parser expects one.

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
reference/cvrptw_1000     the 60 ORIGINAL homogeneous instances    <- regression baseline
reference/*.txt           three small Solomon instances for quick tests
outputs/                  results land here
```

`reference/cvrptw_1000` matters: those are the same customers without the heterogeneous
fleet. After the port, running them should reproduce Dinesh's original numbers, which is how
we check the port did not break the CVRPTW behaviour underneath.

The 100,000-customer instances are deliberately **not** copied. The solver holds a full
triangular distance matrix — 40 GB at that size — so 100k needs a different distance
strategy first (section I of the plan). They live in `../cuOpt_FSMVRPTW/instances/100000`.

## Building and running

```bash
make            # sequential
make par        # OpenMP

bash run_solver.sh 1000                      # instances/1000  -> outputs/1000/
bash run_solver.sh 10000
bash run_solver.sh reference/cvrptw_1000     # regression against Dinesh's numbers
```

The binary takes `<instance> <angle_range>`; the angle controls the sweep clustering and
defaults to 30 in `run_solver.sh`.

On Paramshakti:

```bash
sbatch submit_solver.sh 1000
```

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

Once step H of the plan lands, this solver emits the same CSV columns as the cuOpt harness
(`total_cost, routing_cost, fixed_cost, vehicles_used, mix, used_A..used_H, solve_time_s`)
so a single sheet can hold both.
