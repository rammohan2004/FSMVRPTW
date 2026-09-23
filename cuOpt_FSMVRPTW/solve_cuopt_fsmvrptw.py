#!/usr/bin/env python3
"""
solve_cuopt_fsmvrptw.py

Runs NVIDIA cuOpt on FSMVRPTW instances (Fleet Size and Mix VRP with Time
Windows) produced by instance-generator/solomon_to_fsmvrptw.py, and reports the
fixed cost, the routing cost and the vehicle decomposition separately.

DIFFERENCE FROM solve_cuopt.py
------------------------------
The original script solves plain CVRPTW: one capacity for every vehicle, no
fixed costs, and cuOpt's default objective (COST accumulated from the cost
matrix = total distance). This script adds:

  * a parser for the extended VEHICLE section with multiple vehicle types
  * per-vehicle capacities        -> add_capacity_dimension
  * per-vehicle fixed costs       -> set_vehicle_fixed_costs
  * objective = COST + VEHICLE_FIXED_COST

    total cost = total distance travelled + sum of fixed costs of vehicles used

  * reporting of routing cost, fixed cost and per-type vehicle counts

The original script is left untouched, since it is still the baseline for plain
CVRPTW runs.

Note that cuOpt vehicle *types* are not declared. cuOpt uses vehicle types to
index cost and transit matrices (add_cost_matrix takes a vehicle_type argument,
and each declared type needs its own matrix), which is for fleets whose travel
cost or speed differs by type. Ours does not: every type pays the same cost per
unit distance and only capacity and fixed cost differ, and both of those are
per-vehicle arrays. --per-type-matrices declares types and registers a matrix
per type, for the later variant where running cost does differ by type.

INPUT FORMAT
------------
Extended Solomon format:

    c1_10_1_a

    VEHICLE
    TYPE  NUMBER  CAPACITY  FIXED_COST
       A     604        30          50
       B     363        50          80
       ...

    CUSTOMER
    CUST NO.  XCOORD.  YCOORD.  DEMAND  READY TIME  DUE DATE  SERVICE TIME
    ...

Plain Solomon files (single NUMBER/CAPACITY line, no fixed costs) are also
accepted and treated as a single vehicle type with zero fixed cost, so the same
script can produce a like-for-like CVRPTW baseline.

OUTPUT
------
One CSV row per (instance, time limit). Column groups, in order:

  identity      instance, family, variant, n_customers, n_types, capacities,
                fixed_costs, n_vehicles_available
  run config    timeout_s, objective, status, status_name
  results       total_cost, routing_cost, fixed_cost, vehicles_used, mix,
                used_A..used_H
  verification  routing_cost_recomputed, routing_cost_check, cost_split_check,
                fleet_cap_binding, solve_time_s, notes

The three numbers asked for are reported separately:

    routing_cost    the distance component
    fixed_cost      the vehicle fixed-cost component
    mix + used_X    the fleet composition, e.g. "A2B4C9D5" plus one column per
                    type. 0 means the type was offered and not chosen; blank
                    means the family has no such type.

Both cost components come from cuOpt's own per-objective breakdown
(get_objective_values), and two independent checks guard them:

    routing_cost_check  routing_cost vs the distance of the routes cuOpt
                        actually returned, recomputed here from coordinates
    cost_split_check    routing_cost + fixed_cost vs total_cost, i.e. whether
                        the split really decomposes what cuOpt minimised

Either reading MISMATCH means the row's numbers should not be quoted. Use
--save-routes to keep the routes themselves for later verification.

The original script is the plain-CVRPTW baseline; column names are kept the
same where the two scripts report the same quantity, so the sheets line up.

USAGE
-----
    python3 solve_cuopt_fsmvrptw.py \
        --testcase_dir instances/fsmvrptw_1000 \
        --output_csv outputs_cuopt/fsmvrptw_results.csv \
        --timeouts 60 10 5 2

Requires cuopt and cudf, and a visible CUDA GPU. See submit_cuopt_job.sh.
"""

import argparse
import csv
import glob
import math
import os
import re
import sys
import time

TYPE_LETTERS = "ABCDEFGHIJKLMNOP"

# Solver status codes, per cuOpt's routing.Assignment.get_status():
STATUS_NAMES = {0: "SUCCESS", 1: "FAIL", 2: "TIMEOUT", 3: "EMPTY"}


def _import_cuopt():
    try:
        import cudf
        from cuopt import routing
    except ImportError as exc:
        sys.stderr.write(
            "ERROR: could not import cudf / cuopt. Make sure you are running "
            "inside an environment with cuopt installed and a GPU available.\n"
            f"Original error: {exc}\n"
        )
        sys.exit(1)
    return cudf, routing


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

def parse_instance(filepath):
    """Parse an FSMVRPTW (or plain Solomon) instance.

    Returns a dict with coords/demand/ready/due/service plus vehicle_types,
    a list of (letter, count, capacity, fixed_cost).
    """
    with open(filepath, "r") as f:
        raw = [line.rstrip("\n").rstrip("\r") for line in f]
    lines = [ln.strip() for ln in raw if ln.strip() != ""]
    if not lines:
        raise ValueError("empty file")

    name = lines[0]
    upper = [ln.upper() for ln in lines]
    if "VEHICLE" not in upper:
        raise ValueError("no VEHICLE section")
    if "CUSTOMER" not in upper:
        raise ValueError("no CUSTOMER section")

    veh_idx = upper.index("VEHICLE")
    cust_idx = upper.index("CUSTOMER")
    header = upper[veh_idx + 1]

    vehicle_types = []
    if "FIXED_COST" in header or "TYPE" in header:
        # Extended multi-type section: rows run until the CUSTOMER header.
        for ln in lines[veh_idx + 2:cust_idx]:
            parts = ln.split()
            if len(parts) < 4:
                continue
            letter = parts[0]
            try:
                count = int(parts[1])
                capacity = int(float(parts[2]))
                fixed = float(parts[3])
            except ValueError:
                continue
            vehicle_types.append((letter, count, capacity, fixed))
        if not vehicle_types:
            raise ValueError("VEHICLE section declared types but none parsed")
    else:
        # Plain Solomon: one homogeneous type, no fixed cost.
        parts = lines[veh_idx + 2].split()
        vehicle_types.append(("A", int(parts[0]), int(parts[1]), 0.0))

    coords, demand, ready, due, service = [], [], [], [], []
    for ln in lines[cust_idx + 2:]:
        parts = ln.split()
        if len(parts) < 7:
            continue
        try:
            _cid = int(parts[0])
            x = float(parts[1])
            y = float(parts[2])
            dem = int(float(parts[3]))
            r = float(parts[4])
            d = float(parts[5])
            s = float(parts[6])
        except ValueError:
            continue
        coords.append((x, y))
        demand.append(dem)
        ready.append(r)
        due.append(d)
        service.append(s)

    if len(coords) < 2:
        raise ValueError("fewer than 2 locations parsed")

    return {
        "name": name,
        "coords": coords,
        "demand": demand,
        "ready": ready,
        "due": due,
        "service": service,
        "vehicle_types": vehicle_types,
    }


def expand_fleet(vehicle_types, max_fleet=None):
    """Flatten the type table into per-vehicle arrays.

    cuOpt indexes capacities and fixed costs per *vehicle*, not per type, so a
    type with count 604 is expanded into 604 identical entries. Returns
    (capacity, fixed_cost, type_id, type_letter) lists, all the same length.

    If max_fleet is given, every type's count is scaled down by the same factor
    (never below 1). Scaling proportionally rather than truncating the flat list
    matters: the list is built type by type, so a plain truncation would delete
    the largest vehicle types outright and quietly change the problem.
    """
    counts = [count for (_l, count, _q, _f) in vehicle_types]
    total = sum(counts)
    if max_fleet is not None and total > max_fleet:
        scale = float(max_fleet) / total
        counts = [max(1, int(math.floor(c * scale))) for c in counts]

    caps, costs, type_ids, letters = [], [], [], []
    for tid, (letter, _count, capacity, fixed) in enumerate(vehicle_types):
        for _ in range(counts[tid]):
            caps.append(capacity)
            costs.append(float(fixed))
            type_ids.append(tid)
            letters.append(letter)
    return caps, costs, type_ids, letters


def matrix_bytes(n_locations):
    """Bytes for ONE n x n float32 matrix. cuOpt needs two (cost + transit)."""
    return n_locations * n_locations * 4


def build_distance_matrix(coords, block=2048):
    """Euclidean distance matrix as a float32 numpy array.

    Built with numpy in row blocks rather than Python loops. At 10,000
    locations the old list-of-lists version needed ~3.2 GB of Python floats and
    10^8 interpreted iterations; this produces the same 400 MB float32 array in
    seconds. Blocking bounds the temporary to block x n x 2 floats rather than
    materialising an n x n x 2 difference array.
    """
    try:
        import numpy as np
    except ImportError:
        sys.exit("ERROR: numpy is required to build the distance matrix.")

    pts = np.asarray(coords, dtype=np.float32)
    n = pts.shape[0]
    mat = np.empty((n, n), dtype=np.float32)
    for i0 in range(0, n, block):
        i1 = min(i0 + block, n)
        diff = pts[i0:i1, None, :] - pts[None, :, :]
        np.sqrt((diff * diff).sum(axis=-1), out=mat[i0:i1])
    return mat


# --------------------------------------------------------------------------
# Solving
# --------------------------------------------------------------------------

def solve_instance(cudf, routing, inst, time_limit, args, dist_matrix=None):
    coords = inst["coords"]
    n_locations = len(coords)

    caps, fixed_costs, type_ids, letters = expand_fleet(
        inst["vehicle_types"], args.max_fleet)
    n_vehicles = len(caps)

    # The matrix depends only on the coordinates, so the caller builds it once
    # per instance and reuses it across time limits. Building it here instead
    # costs ~0.15 s per solve at 1000 locations, which is nothing next to the
    # solve, but scales with the square of the location count -- ~15 s each at
    # 10,000, which is not.
    if dist_matrix is None:
        dist_matrix = build_distance_matrix(coords)
    cost_matrix = cudf.DataFrame(dist_matrix, dtype="float32")

    data_model = routing.DataModel(n_locations, n_vehicles)

    # Routing cost is Euclidean distance, and every vehicle type travels at the
    # same cost per unit distance (Rupesh's decision; Liu & Shen likewise assume
    # uniform running costs and put the heterogeneity purely in the fixed cost).
    #
    # Because of that, set_vehicle_types is deliberately NOT called. cuOpt
    # indexes cost/transit matrices by vehicle type -- add_cost_matrix takes a
    # vehicle_type argument and each declared type needs its own matrix -- so
    # declaring 6 types while registering one matrix would be invalid. The fleet
    # heterogeneity we actually need (capacity and fixed cost) is expressed
    # per-vehicle instead, which needs no vehicle types at all. --per-type-
    # matrices registers a matrix per type for later work where running cost
    # does differ by type (the carbon-footprint variant).
    if args.per_type_matrices:
        n_types = len(inst["vehicle_types"])
        for tid in range(n_types):
            data_model.add_cost_matrix(cost_matrix.copy(deep=True),
                                       vehicle_type=tid)
            data_model.add_transit_time_matrix(cost_matrix.copy(deep=True),
                                               vehicle_type=tid)
        data_model.set_vehicle_types(cudf.Series(type_ids, dtype="uint8"))
    else:
        data_model.add_cost_matrix(cost_matrix)
        data_model.add_transit_time_matrix(cost_matrix.copy(deep=True))

    # Heterogeneous capacity: one entry per vehicle. cuOpt wants int32 here.
    data_model.add_capacity_dimension(
        "demand",
        cudf.Series(inst["demand"], dtype="int32"),
        cudf.Series(caps, dtype="int32"),
    )

    # Time windows.
    data_model.set_order_time_windows(
        cudf.Series(inst["ready"]), cudf.Series(inst["due"]))
    data_model.set_order_service_times(cudf.Series(inst["service"]))
    depot_ready, depot_due = inst["ready"][0], inst["due"][0]
    data_model.set_vehicle_time_windows(
        cudf.Series([depot_ready] * n_vehicles),
        cudf.Series([depot_due] * n_vehicles))

    # Fleet size and mix: per-vehicle fixed costs, plus an objective that
    # charges distance and fixed cost together, so cuOpt chooses the mix rather
    # than being handed one.
    #
    #     total cost = total distance + sum of fixed costs of vehicles used
    #
    # Without this the default objective is COST alone (pure distance), and the
    # fixed costs would be ignored no matter what is passed above.
    has_fixed = any(c > 0 for c in fixed_costs)
    fixed_in_objective = has_fixed and not args.distance_only
    if fixed_in_objective:
        data_model.set_vehicle_fixed_costs(
            cudf.Series(fixed_costs, dtype="float32"))
        data_model.set_objective_function(
            cudf.Series([routing.Objective.COST,
                         routing.Objective.VEHICLE_FIXED_COST]),
            cudf.Series([1.0, 1.0], dtype="float32"),
        )

    solver_settings = routing.SolverSettings()
    solver_settings.set_time_limit(float(time_limit))

    t0 = time.perf_counter()
    solution = routing.Solve(data_model, solver_settings)
    elapsed = time.perf_counter() - t0

    status = solution.get_status()
    result = {
        "status": status,
        "status_name": STATUS_NAMES.get(status, "UNKNOWN(%s)" % status),
        "solve_time_s": elapsed,
        "n_vehicles_available": n_vehicles,
        "objective": ("distance+fixed" if fixed_in_objective else "distance"),
    }

    if status not in (0, 2):  # neither SUCCESS nor TIMEOUT-with-solution
        # cuOpt's documented SolutionStatus only runs 0..3 (SUCCESS, FAIL,
        # TIMEOUT, EMPTY), so anything else is undocumented and the numeric code
        # alone says nothing. The Assignment carries its own diagnostics --
        # collect whatever it will give us instead of just recording a number.
        diag = []
        for name in ("get_message", "get_error_status", "get_error_message"):
            try:
                val = getattr(solution, name)()
            except Exception:
                continue
            if val not in (None, "", 0):
                diag.append("%s=%s" % (name[4:], val))
        try:
            bad = solution.get_infeasible_orders()
            n_bad = int(sum(1 for x in bad.to_arrow().to_pylist() if x)) \
                if hasattr(bad, "to_arrow") else len(bad)
            if n_bad:
                diag.append("infeasible_orders=%d" % n_bad)
        except Exception:
            pass
        if diag:
            result["notes"] = "; ".join(diag)
        return result

    try:
        result["total_cost"] = float(solution.get_total_objective())
    except Exception:
        result["total_cost"] = None
    try:
        result["vehicles_used"] = int(solution.get_vehicle_count())
    except Exception:
        result["vehicles_used"] = None

    # --- cost split from cuOpt's own per-objective breakdown ---------------
    routing_cost = fixed_cost = None
    try:
        obj_values = solution.get_objective_values()
        for key, value in obj_values.items():
            key_name = getattr(key, "name", str(key)).upper()
            if "FIXED" in key_name:
                fixed_cost = float(value)
            elif "COST" in key_name:
                routing_cost = float(value)
    except Exception as exc:
        result["notes"] = "get_objective_values failed: %s" % exc

    # --- vehicle decomposition, and an independent routing-cost check ------
    # Accumulated into locals and only committed once the whole walk succeeds.
    # A half-finished walk would otherwise leave a partial decomposition that
    # silently understates the mix, the fixed cost and the cap check, which is
    # worse than reporting nothing.
    per_type = {}
    recomputed = None
    routes = None
    try:
        route_df = solution.get_route().to_pandas()
        walk_per_type, walk_dist, walk_routes = {}, 0.0, {}
        for truck, grp in route_df.groupby("truck_id"):
            locs = [int(l) for l in grp["location"]]
            if not any(l != 0 for l in locs):
                continue  # vehicle never left the depot
            truck = int(truck)
            if truck >= len(type_ids):
                raise IndexError(
                    "truck_id %d outside fleet of %d" % (truck, len(type_ids)))
            letter = inst["vehicle_types"][type_ids[truck]][0]
            walk_per_type[letter] = walk_per_type.get(letter, 0) + 1
            walk_routes[truck] = (letter, locs)
            for a, b in zip(locs, locs[1:]):
                ax, ay = coords[a]
                bx, by = coords[b]
                walk_dist += math.hypot(ax - bx, ay - by)
        per_type, recomputed, routes = walk_per_type, walk_dist, walk_routes
    except Exception as exc:
        result["notes"] = ((result.get("notes", "") + "; ") if result.get("notes")
                           else "") + "get_route failed: %s" % exc

    # Where the split came from. "cuopt" means both components were reported
    # independently by the solver; "derived" means at least one was reconstructed
    # here. This distinction matters for the checks below: a derived split
    # cannot be validated by adding it back up.
    cost_source = "cuopt" if (routing_cost is not None
                              and fixed_cost is not None) else "derived"

    # If cuOpt did not hand back a usable breakdown, derive what we can.
    # The fixed cost of a solution is fully determined by which vehicles it
    # uses, so it can always be recovered from the decomposition.
    if fixed_cost is None and per_type:
        fixed_by_letter = {l: fc for (l, _c, _cap, fc) in inst["vehicle_types"]}
        fixed_cost = sum(cnt * fixed_by_letter[letter]
                         for letter, cnt in per_type.items())
    if routing_cost is None:
        if not fixed_in_objective:
            # Objective was distance alone, so the objective IS the routing cost
            # and the fixed cost above is reported for information only.
            routing_cost = result.get("total_cost")
        elif result.get("total_cost") is not None and fixed_cost is not None:
            routing_cost = result["total_cost"] - fixed_cost
    result["cost_source"] = cost_source

    result["routing_cost"] = routing_cost
    result["fixed_cost"] = fixed_cost
    result["routing_cost_recomputed"] = recomputed
    result["routes"] = routes

    # Check 1: does the reported routing cost match the distance of the routes
    # cuOpt actually returned? Catches a wrong objective or a misread breakdown.
    # A MISMATCH on every instance more likely means get_route() represents
    # routes differently than assumed (e.g. omitting the return to the depot)
    # than that every solve is wrong -- inspect one route before concluding.
    if routing_cost is not None and recomputed is not None:
        denom = max(abs(routing_cost), 1e-9)
        result["routing_cost_check"] = (
            "OK" if abs(routing_cost - recomputed) / denom < 0.01
            else "MISMATCH")
    else:
        result["routing_cost_check"] = ""

    # Check 2: do the two components add back up to the objective? Only
    # meaningful when cuOpt reported both independently. A derived split sets
    # routing = total - fixed, so it adds up by construction and checking it
    # would prove nothing -- it is reported as "derived", not "OK", so a
    # reconstructed split is never mistaken for a verified one.
    total = result.get("total_cost")
    if not fixed_in_objective:
        result["cost_split_check"] = ""
    elif cost_source != "cuopt":
        result["cost_split_check"] = "derived"
    elif total is not None and routing_cost is not None and fixed_cost is not None:
        denom = max(abs(total), 1e-9)
        result["cost_split_check"] = (
            "OK" if abs((routing_cost + fixed_cost) - total) / denom < 0.01
            else "MISMATCH")
    else:
        result["cost_split_check"] = ""

    result["per_type"] = per_type
    result["mix"] = "".join(
        "%s%d" % (letter, per_type[letter])
        for (letter, _cnt, _cap, _fc) in inst["vehicle_types"]
        if per_type.get(letter))

    # FSMVRPTW assumes unlimited vehicles per type, but the model is given a
    # finite count. If a solution uses every vehicle of some type, that count
    # was a binding constraint: the solver may have wanted more of that type and
    # could not have them, so the run is not really "unlimited supply" and the
    # result should not be quoted as an FSMVRPTW solution without a rerun.
    offered = {}
    for letter in letters:
        offered[letter] = offered.get(letter, 0) + 1
    binding = [letter for letter, used in sorted(per_type.items())
               if used >= offered.get(letter, 0)]
    result["fleet_cap_binding"] = ",".join(binding)
    return result


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def save_routes(out_dir, instance_name, timeout, routes):
    """Write one solution's routes, so a result can be re-verified later.

    Format: one route per line, "<type letter> <loc> <loc> ...", depot included
    as returned by cuOpt. Keeps the CSV to one row per run while still making
    the full solution recoverable.
    """
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(instance_name)[0]
    path = os.path.join(out_dir, "%s_t%g.routes" % (stem, timeout))
    with open(path, "w") as fh:
        for truck in sorted(routes):
            letter, locs = routes[truck]
            fh.write("%s %s\n" % (letter, " ".join(str(l) for l in locs)))


def split_name(instance_name):
    """Pull family (C1/R2/RC1...) and cost variant (a/b/c) out of a filename."""
    stem = os.path.splitext(instance_name)[0].upper()
    fam = ""
    for candidate in ("RC1", "RC2", "R1", "R2", "C1", "C2"):
        if stem.startswith(candidate):
            fam = candidate
            break
    m = re.search(r"_([ABC])$", stem)
    return fam, (m.group(1).lower() if m else "")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--testcase_dir", default="instances/fsmvrptw_1000",
                        help="directory of FSMVRPTW instances (default assumes "
                             "instances/ sits inside this cuOpt folder)")
    parser.add_argument("--output_csv",
                        default="outputs_cuopt/fsmvrptw_results.csv",
                        help="CSV to append results to")
    # nargs="*" so `--timeouts` with no values runs ONLY the per-instance
    # equal-time budgets from --match-times, with no ladder on top.
    parser.add_argument("--timeouts", type=float, nargs="*",
                        default=[60, 10, 5, 2],
                        help="time limits in seconds per instance; same ladder "
                             "as solve_cuopt.py so the two result sheets line "
                             "up at each time limit")
    parser.add_argument("--match-times", metavar="CSV", default=None,
                        help="CSV from our own solver (columns: instance, "
                             "solve_time_s). For each instance, cuOpt is ALSO "
                             "given that instance's own runtime as a time "
                             "limit, giving an exact equal-time comparison "
                             "rather than one read off a coarse ladder. "
                             "Instances are matched on basename.")
    parser.add_argument("--max-fleet", type=int, default=None,
                        help="cap the total vehicles handed to cuOpt; every "
                             "type is scaled down proportionally, never below 1")
    parser.add_argument("--max-types", type=int, default=8,
                        help="how many used_X columns to emit")
    parser.add_argument("--save-routes", metavar="DIR", default=None,
                        help="also write each solution's routes to DIR, one "
                             "file per (instance, time limit), so results can "
                             "be re-verified without rerunning the solve")
    parser.add_argument("--distance-only", action="store_true",
                        help="ignore fixed costs and minimise distance alone "
                             "(the plain-CVRPTW baseline objective)")
    parser.add_argument("--per-type-matrices", action="store_true",
                        help="declare vehicle types and register one cost / "
                             "transit matrix per type. Not needed while all "
                             "types share a cost per unit distance; it exists "
                             "for a future variable-running-cost variant, and "
                             "multiplies matrix memory by the type count")
    args = parser.parse_args()

    cudf, routing = _import_cuopt()

    os.makedirs(os.path.dirname(args.output_csv) or ".", exist_ok=True)
    write_header = not os.path.exists(args.output_csv)

    files = sorted(p for p in glob.glob(os.path.join(args.testcase_dir, "*"))
                   if os.path.isfile(p))
    if not files:
        sys.exit("No files found in %s" % args.testcase_dir)

    type_cols = ["used_%s" % TYPE_LETTERS[i] for i in range(args.max_types)]
    fieldnames = ([
        # identity / instance description
        "instance", "family", "variant", "n_customers", "n_types",
        "capacities", "fixed_costs", "n_vehicles_available",
        # run configuration
        "timeout_s", "objective", "status", "status_name",
        # the numbers being reported
        "total_cost", "routing_cost", "fixed_cost",
        "vehicles_used", "mix",
    ] + type_cols + [
        # verification
        "cost_source", "routing_cost_recomputed", "routing_cost_check",
        "cost_split_check", "fleet_cap_binding", "solve_time_s", "notes",
    ])

    with open(args.output_csv, "a", newline="") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames,
                                extrasaction="ignore")
        if write_header:
            writer.writeheader()

        # Per-instance equal-time budgets, if our own results were supplied.
        match_times = {}
        if args.match_times:
            with open(args.match_times, newline="") as mf:
                for r in csv.DictReader(mf):
                    key = os.path.basename(r.get("instance", "")).strip()
                    try:
                        match_times[key] = float(r["solve_time_s"])
                    except (KeyError, ValueError):
                        continue
            print("Loaded %d per-instance time budgets from %s\n"
                  % (len(match_times), args.match_times))

        for filepath in files:
            instance_name = os.path.basename(filepath)
            try:
                inst = parse_instance(filepath)
            except Exception as exc:
                print("[SKIP] %s: failed to parse (%s)" % (instance_name, exc))
                continue

            fam, variant = split_name(instance_name)
            n_customers = len(inst["coords"]) - 1
            total_fleet = sum(c for (_l, c, _cap, _fc)
                              in inst["vehicle_types"])
            print("=== %s  (%d customers, %d types, %d vehicles available) ==="
                  % (instance_name, n_customers,
                     len(inst["vehicle_types"]), total_fleet))

            # Built once and shared by every time limit for this instance.
            dist_matrix = build_distance_matrix(inst["coords"])

            # The instance's own equal-time budget first, then the ladder.
            # Rounded to 0.1 s so the column stays readable; deduped in case a
            # ladder rung already sits on that value.
            timeouts = list(args.timeouts)
            if instance_name in match_times:
                matched = round(match_times[instance_name], 1)
                if matched not in timeouts:
                    timeouts = [matched] + timeouts
                print("  equal-time budget for this instance: %g s" % matched)

            for timeout in timeouts:
                row = {
                    "instance": instance_name,
                    "family": fam,
                    "variant": variant,
                    "n_customers": n_customers,
                    "n_types": len(inst["vehicle_types"]),
                    "capacities": "/".join(
                        str(q) for (_l, _c, q, _f) in inst["vehicle_types"]),
                    "fixed_costs": "/".join(
                        "%g" % fc for (_l, _c, _q, fc) in inst["vehicle_types"]),
                    "timeout_s": timeout,
                }
                try:
                    res = solve_instance(cudf, routing, inst, timeout, args,
                                         dist_matrix=dist_matrix)
                    row.update({k: v for k, v in res.items()
                                if k not in ("per_type", "routes")})
                    # Explicit 0 for a type the instance offers but the solution
                    # did not use, so "offered but unused" is distinguishable
                    # from "this family has no such type" (left blank).
                    if res.get("status") in (0, 2):
                        for (letter, _c, _q, _f) in inst["vehicle_types"]:
                            row["used_%s" % letter] = 0
                    for letter, cnt in res.get("per_type", {}).items():
                        row["used_%s" % letter] = cnt
                    print("  timeout=%5s s  status=%-8s total=%s  "
                          "routing=%s  fixed=%s  vehicles=%s  mix=%s  [%s]"
                          % (timeout, res["status_name"],
                             _fmt(res.get("total_cost")),
                             _fmt(res.get("routing_cost")),
                             _fmt(res.get("fixed_cost")),
                             res.get("vehicles_used"),
                             res.get("mix", ""),
                             res.get("routing_cost_check", "")))
                    if res.get("fleet_cap_binding"):
                        print("    WARNING: type(s) %s fully used -- the fleet "
                              "count was binding, so this is not an unlimited-"
                              "supply solution. Rerun with a larger fleet."
                              % res["fleet_cap_binding"])
                    if res.get("cost_split_check") == "MISMATCH":
                        print("    WARNING: routing + fixed != total objective; "
                              "the cost split is not trustworthy for this row.")
                    if res.get("cost_source") == "derived":
                        print("    NOTE: cuOpt did not report a usable objective "
                              "breakdown; the split was reconstructed here. See "
                              "the notes column.")
                    if args.save_routes and res.get("routes"):
                        save_routes(args.save_routes, instance_name, timeout,
                                    res["routes"])
                except Exception as exc:
                    row.update({"status": "", "status_name": "ERROR",
                                "notes": str(exc)})
                    print("  timeout=%5s s  ERROR: %s" % (timeout, exc))

                writer.writerow(row)
                csvfile.flush()

    print("\nDone. Results written to %s" % args.output_csv)


def _fmt(v):
    return "%.2f" % v if isinstance(v, float) else str(v)


if __name__ == "__main__":
    main()
