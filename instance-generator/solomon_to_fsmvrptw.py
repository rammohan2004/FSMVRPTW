#!/usr/bin/env python3
"""
solomon_to_fsmvrptw.py

Converts homogeneous-fleet Solomon / Homberger-Gehring CVRPTW instances into
heterogeneous-fleet FSMVRPTW instances, following the design of

    F-H Liu and S-Y Shen (1999), "The fleet size and mix vehicle routing problem
    with time windows", J. Opl Res. Soc. 50(7), 721-732.

WHAT IS ADDED
-------------
The customer data (coordinates, demands, time windows, service times) is copied
through unchanged. Only the VEHICLE section is replaced: instead of a single
(NUMBER, CAPACITY) pair, the output declares several vehicle TYPES, each with its
own capacity and fixed cost.

CAPACITY DESIGN
---------------
  * largest type capacity  = the original instance capacity Q
        (preserves the routing character of the base instance; this is what Liu
         & Shen did for R1, RC1, R2, C2 and RC2)
  * smallest type capacity = max customer demand, rounded up to --cap-round
        (a design choice, not a feasibility requirement -- it means every type
         can serve every customer. Override with --min-capacity to widen the
         low end; Liu & Shen's own R1 table starts below some demands.)
  * intermediate types are spaced geometrically between the two, matching the
    ~1.5-1.7x successive ratios Liu & Shen used where the range allowed.

FIXED COST DESIGN
-----------------
Liu & Shen's tables cannot be copied directly: they have 3-6 vehicle types
depending on the family (R1:5, C1:3, RC1:4, R2:4, C2:4, RC2:6), and we need the
same number of types for every instance. So instead of copying the numbers we
extract what is INVARIANT across all six of their families and apply that.

The invariant is in cost per unit capacity:

    family   smallest veh   largest veh   ratio
    R1           1.67           2.50       1.50
    C1           3.00           4.50       1.50
    RC1          1.50           2.25       1.50
    R2           1.50           2.50       1.67
    C2           2.50           3.86       1.54
    RC2          1.50           2.50       1.67

In every family the largest vehicle costs about 1.5-1.67x as much PER UNIT
CAPACITY as the smallest. Large vehicles are deliberately penalised, which is
what keeps the fleet-mix decision non-trivial: a big vehicle means fewer routes
and less distance, but a disproportionately higher fixed cost.

The absolute level is family-dependent -- C families sit roughly twice as high as
R/RC, consistent with clustered instances having shorter routes and therefore
less routing cost to balance against.

We preserve BOTH endpoints per family and interpolate geometrically across our
six capacities:

    cost(c) = c * ratio_lo * (c / c_min) ** beta
    beta    = ln(ratio_hi / ratio_lo) / ln(c_max / c_min)

Validation: RC2 is the one family where Liu & Shen also used 6 types, so it is a
direct comparison.

    theirs   1.50  1.75  1.83  2.00  2.20  2.50
    ours     1.50  1.66  1.85  2.04  2.26  2.50

Anchoring on endpoints rather than on a fitted exponent matters. Their exponents
were fitted over each family's own capacity range, some very narrow (C2 spans
only 400-700). Extrapolating C2's exponent of 1.775 across our wider 50-700 range
drives the smallest vehicle's cost/capacity to 0.50 against their actual 2.50.

Alternative shapes are available for sensitivity analysis, but note that only the
superlinear shape keeps the problem non-degenerate:

  superlinear : cost/capacity rises -> large vehicles pay a premium that offsets
                                       their routing advantage. Genuine trade-off.
  linear      : cost/capacity flat  -> serving demand D costs about k*D for ANY
                                       well-packed mix, so fixed cost cannot
                                       discriminate between mixes. Routing then
                                       strictly favours the largest vehicle and
                                       the fleet-mix decision DEGENERATES.
  step        : cost/capacity falls -> large vehicles are cheaper per unit AND
                                       better for routing. Degenerates harder.
  random      : monotone but arbitrary; may degenerate, needs --seed to be
                                       reproducible, and gives each instance
                                       different costs so results are not
                                       comparable across instances.

COST VARIANTS
-------------
Each instance is emitted once per cost variant. Following Liu & Shen, the
variants share one shape and differ only in MAGNITUDE:

        a = full scale,   b = a / 5,   c = a / 10

This varies the fixed-cost-to-routing-cost ratio, which is the axis that
demonstrably changes the optimum. From Liu & Shen's own published solutions for
R101:

        variant a -> mix A1 B10 C12,  23 vehicles
        variant b -> mix A3 B4  C11 D3,  21 vehicles
        variant c -> mix A2 B4  C9  D5,  20 vehicles

Different mixes, different vehicle counts, different types used. Variant (a)
makes vehicles expensive relative to travel and favours few large ones; variant
(c) makes them cheap and favours many small ones.

OUTPUT FORMAT
-------------
Solomon format with an extended VEHICLE section:

    <name>

    VEHICLE
    TYPE  NUMBER  CAPACITY  FIXED_COST
      A      359        50          82
      ...

    CUSTOMER
    CUST NO.  XCOORD.  YCOORD.  DEMAND  READY TIME  DUE DATE  SERVICE TIME
    ...

NUMBER stands in for "unlimited". FSMVRPTW assumes an unlimited supply of every
type, but a file must write down some number, and solvers that require an
explicit fleet (cuOpt does) read it. It is therefore set to a bound no solution
can reach: the number of customers n, since every vehicle used serves at least
one customer. Capped by --max-vehicles-per-type, which trades exactness for
solver speed -- if a type is used up to its cap in a solution, the cap was
binding and the result is constrained rather than unlimited.

USAGE
-----
    # one instance, all three cost variants
    python3 solomon_to_fsmvrptw.py --input C1_10_1.txt --output-dir out/

    # a whole directory
    python3 solomon_to_fsmvrptw.py --input testcases/ --output-dir out/

    # inspect the vehicle table without writing files
    python3 solomon_to_fsmvrptw.py --input C1_10_1.txt --dry-run
"""

import argparse
import math
import os
import random
import sys

TYPE_LETTERS = "ABCDEFGHIJKLMNOP"

# Liu & Shen's cost variants: same shape, three magnitudes.
DEFAULT_VARIANTS = {"a": 1.0, "b": 0.2, "c": 0.1}

# Per-family calibration extracted from Liu & Shen's Table 7.
#
# For each family they published a capacity/cost table. Two numbers summarise its
# cost structure:
#
#   ratio_at_q = cost/capacity of the LARGEST vehicle
#   alpha      = 1 + ln(ratio_largest / ratio_smallest) / ln(cap_largest / cap_smallest)
#
# alpha > 1 everywhere means cost per unit capacity RISES with vehicle size in all
# six families -- large vehicles are deliberately penalised, which is what keeps the
# fleet-mix decision non-trivial.
#
# Note they calibrated each family separately: C families carry roughly double the
# cost-per-capacity of R/RC families, consistent with clustered instances having
# shorter routes and therefore lower routing cost to balance against. No single
# (alpha, ratio) reproduces all six tables -- errors range 5%-44% -- so we keep the
# per-family values rather than inventing a global formula.
#
#            cost/cap at    cost/cap at    (largest / smallest)
#            SMALLEST veh   LARGEST veh
LIU_SHEN_FAMILIES = {
    "R1":  (1.67, 2.50),   # 1.50
    "C1":  (3.00, 4.50),   # 1.50
    "RC1": (1.50, 2.25),   # 1.50
    "R2":  (1.50, 2.50),   # 1.67
    "C2":  (2.50, 3.86),   # 1.54
    "RC2": (1.50, 2.50),   # 1.67
}

# Used when the family cannot be identified.
FALLBACK_RATIOS = (1.50, 2.50)

# Liu & Shen's Table 7 verbatim: (capacity, variant-a fixed cost) per family.
# Used by --liu-shen-exact, which skips all capacity/cost design and copies these
# straight through. Variants b and c are these costs / 5 and / 10.
#
# The capacities transfer directly to the 1000-customer Homberger-Gehring
# instances because H-G kept Solomon's vehicle capacities when scaling up:
# R1 200, RC1 200, R2 1000, C2 700, RC2 1000 all match. C1 is the one exception
# -- its largest type is 300 against a base capacity of 200, so that family ends
# up with a largest vehicle bigger than the original homogeneous one.
LIU_SHEN_TABLES = {
    "R1":  [(30, 50), (50, 80), (80, 140), (120, 250), (200, 500)],
    "C1":  [(100, 300), (200, 800), (300, 1350)],
    "RC1": [(40, 60), (80, 150), (150, 300), (200, 450)],
    "R2":  [(300, 450), (400, 700), (600, 1200), (1000, 2500)],
    "C2":  [(400, 1000), (500, 1400), (600, 2000), (700, 2700)],
    "RC2": [(100, 150), (200, 350), (300, 550), (400, 800),
            (500, 1100), (1000, 2500)],
}

# Braysy, Porkka, Dullaert, Repoussis & Tarantilis (2009), "A well-scalable
# metaheuristic for the fleet size and mix vehicle routing problem with time
# windows", Expert Systems with Applications 36, 8460-8475, Table 1.
# Used by --braysy-2009. Same format: (capacity, cost-structure-A fixed cost).
#
# This is the table designed for the large Gehring-Homberger instances, which is
# what we solve, so it is the closer match to our setting than Liu & Shen's
# 100-customer design. Three things differ from Liu & Shen:
#
#   1. Eight vehicle types in every family, not 3-6.
#   2. VB (the base instance's own capacity) is the SIXTH type, so two types are
#      LARGER than VB. Liu & Shen never went above VB, and this paper argues
#      that made their instances "somewhat easier to solve (optimizing the
#      capacity utilization of larger vehicles is often harder)".
#   3. Cost per unit capacity FALLS with size (economies of scale), where Liu &
#      Shen's rises. The authors surveyed real Finnish truck types and applied
#      the linear trend from the three largest to all eight.
#
# Verified against the paper: in all six families the sixth entry's cost equals
# Liu & Shen's cost for that same capacity, which is how the paper anchors it
# ("The cost of the vehicle with a carrying capacity VB is the same as for the
# corresponding Liu and Shen (1999) 100-customer problem set").
#
# NOTE: the paper reports results for cost structures A and C only -- it omitted
# B "to limit the computational tests". So published reference numbers exist for
# variants a and c, but not b.
BRAYSY_2009_TABLES = {
    "R1":  [(40, 140), (70, 230), (100, 310), (140, 405),
            (170, 460), (200, 500), (240, 550), (270, 565)],
    "C1":  [(40, 200), (70, 335), (100, 460), (140, 615),
            (170, 715), (200, 800), (240, 910), (270, 975)],
    "RC1": [(40, 125), (70, 205), (100, 275), (140, 355),
            (170, 420), (200, 450), (240, 495), (270, 500)],
    "R2":  [(170, 590), (340, 1115), (500, 1550), (670, 1945),
            (840, 2270), (1000, 2500), (1170, 2690), (1330, 2795)],
    "C2":  [(120, 575), (240, 1100), (350, 1540), (470, 1975),
            (580, 2320), (700, 2700), (820, 2955), (930, 3160)],
    "RC2": [(170, 590), (340, 1115), (500, 1550), (670, 1945),
            (840, 2270), (1000, 2500), (1170, 2690), (1330, 2795)],
}

# The base instance capacity (VB) each Braysy table is anchored to. Used only to
# warn when the table is applied to instances it was not designed for -- the
# capacities are fixed multiples of VB, so they are wrong if VB differs.
BRAYSY_2009_VB = {"R1": 200, "C1": 200, "RC1": 200,
                  "R2": 1000, "C2": 700, "RC2": 1000}


def detect_family(name):
    """Identify the Solomon family (R1, C2, RC1, ...) from an instance name.

    Handles both 100-customer names (c101, rc205) and Homberger-Gehring names
    (c1_10_1, rc2_10_5). Returns None if it cannot be determined.
    """
    stem = os.path.basename(name).split(".")[0].upper()
    for fam in ("RC1", "RC2", "R1", "R2", "C1", "C2"):
        if stem.startswith(fam):
            return fam
    return None


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

class Instance:
    """A parsed Solomon-format CVRPTW instance."""

    def __init__(self, name, n_vehicles, capacity, customers):
        self.name = name
        self.n_vehicles = n_vehicles
        self.capacity = capacity
        # customers[0] is the depot
        self.customers = customers

    @property
    def n_customers(self):
        return len(self.customers) - 1

    @property
    def demands(self):
        return [c[3] for c in self.customers[1:]]

    @property
    def max_demand(self):
        return max(self.demands)

    @property
    def total_demand(self):
        return sum(self.demands)


def parse_solomon(path):
    """Parse a Solomon / Homberger-Gehring CVRPTW instance file.

    Returns an Instance. Raises ValueError if the file does not look like one.
    """
    with open(path, "r") as fh:
        raw = [line.rstrip("\n").rstrip("\r") for line in fh]

    lines = [ln.strip() for ln in raw if ln.strip() != ""]
    if not lines:
        raise ValueError("file is empty")

    name = lines[0]

    upper = [ln.upper() for ln in lines]
    if "VEHICLE" not in upper:
        raise ValueError("no VEHICLE section")
    if "CUSTOMER" not in upper:
        raise ValueError("no CUSTOMER section")

    veh_idx = upper.index("VEHICLE")
    # line after VEHICLE is the "NUMBER CAPACITY" header, then the values
    veh_values = lines[veh_idx + 2].split()
    n_vehicles = int(veh_values[0])
    capacity = int(veh_values[1])

    cust_idx = upper.index("CUSTOMER")
    # line after CUSTOMER is the column header; data starts after that
    customers = []
    for ln in lines[cust_idx + 2:]:
        parts = ln.split()
        if len(parts) < 7:
            continue
        try:
            cid = int(parts[0])
            x = float(parts[1])
            y = float(parts[2])
            dem = int(float(parts[3]))
            ready = float(parts[4])
            due = float(parts[5])
            serv = float(parts[6])
        except ValueError:
            continue  # stray text / footer
        customers.append((cid, x, y, dem, ready, due, serv))

    if len(customers) < 2:
        raise ValueError("fewer than 2 locations parsed")

    return Instance(name, n_vehicles, capacity, customers)


# --------------------------------------------------------------------------
# Vehicle type design
# --------------------------------------------------------------------------

def design_capacities(inst, n_types, cap_round, min_capacity=None):
    """Geometrically spaced capacities, from a floor up to the original Q.

    By default the floor is the largest single customer demand, rounded up.
    That is NOT a feasibility requirement -- FSMVRPTW has an unlimited supply of
    every type, so a large customer is simply served by a larger vehicle (Liu &
    Shen's own R1 table starts at capacity 30, below some customer demands).
    It is a design choice meaning "every type can serve every customer".

    Pass min_capacity to override it and widen the low end of the range.
    """
    q_max = inst.capacity
    if min_capacity is not None:
        floor = int(min_capacity)
    else:
        floor = int(math.ceil(inst.max_demand / cap_round) * cap_round)

    if floor >= q_max:
        raise ValueError(
            "cannot build %d types: max demand %d rounds to %d which is not "
            "below the instance capacity %d"
            % (n_types, inst.max_demand, floor, q_max)
        )

    if n_types == 1:
        return [q_max]

    ratio = (q_max / floor) ** (1.0 / (n_types - 1))
    caps = []
    for i in range(n_types):
        raw = floor * (ratio ** i)
        caps.append(int(round(raw / cap_round) * cap_round))

    caps[0] = floor
    caps[-1] = q_max

    # Rounding can collide; nudge upward to keep capacities strictly increasing.
    for i in range(1, len(caps)):
        if caps[i] <= caps[i - 1]:
            caps[i] = caps[i - 1] + cap_round
    if caps[-1] != q_max:
        caps[-1] = q_max
        if caps[-1] <= caps[-2]:
            raise ValueError(
                "capacity range %d..%d is too narrow for %d types at "
                "--cap-round %d" % (floor, q_max, n_types, cap_round)
            )
    return caps


def design_costs(caps, shape, ratio_lo, ratio_hi, rng):
    """Fixed cost per vehicle type, at full (variant 'a') scale.

    The superlinear shape is anchored on Liu & Shen's cost-per-capacity ENDPOINTS
    for the family rather than on a fitted exponent. Their exponents were fitted
    over each family's own capacity range (as narrow as 400-700 for C2), and
    extrapolating one over a wider range produces nonsense -- C2's alpha of 1.775
    applied over 50-700 drives the smallest vehicle's cost/capacity down to 0.50
    against their actual 2.50.

    Anchoring on endpoints instead reproduces their real invariant: in all six
    families the largest vehicle costs about 1.5-1.67x as much PER UNIT CAPACITY
    as the smallest. The absolute level is family-dependent (1.5 for R/RC,
    2.5-3.0 for C, reflecting clustered instances' shorter routes).

        cost(c) = c * ratio_lo * (c / c_min) ** beta
        beta    = ln(ratio_hi / ratio_lo) / ln(c_max / c_min)
    """
    q_max = caps[-1]
    cost_at_q = ratio_hi * q_max

    if shape == "superlinear":
        c_min = caps[0]
        if len(caps) == 1 or c_min == q_max:
            return [ratio_hi * c for c in caps]
        beta = math.log(ratio_hi / ratio_lo) / math.log(q_max / c_min)
        return [c * ratio_lo * (c / c_min) ** beta for c in caps]

    if shape == "linear":
        # Cost strictly proportional to capacity; cost/capacity is constant.
        return [cost_at_q * (c / q_max) for c in caps]

    if shape == "step":
        # Equal increments per type index, independent of the capacity gaps.
        n = len(caps)
        first = cost_at_q / n
        step = (cost_at_q - first) / (n - 1) if n > 1 else 0.0
        return [first + i * step for i in range(n)]

    if shape == "random":
        # Uniform draws, sorted so cost still increases with capacity.
        draws = sorted(rng.uniform(0.15, 1.0) for _ in caps)
        return [cost_at_q * d for d in draws]

    raise ValueError("unknown cost shape: %s" % shape)


def fleet_counts(inst, caps, cap_limit):
    """Vehicle count per type: enough that the count never binds.

    FSMVRPTW assumes an unlimited supply of every type, but a file has to write
    down some number, and solvers that need an explicit fleet (cuOpt does) read
    it. That number must be a bound no optimal solution can reach, otherwise the
    "unlimited" assumption is silently replaced by a fleet-size constraint.

    The bound used is simply the number of customers, n. Every vehicle that is
    used serves at least one customer and there are n customers, so no solution
    can use more than n vehicles of any one type. This holds under *any*
    constraint set, time windows included.

    An earlier version used ceil(total_demand / capacity), which is wrong here.
    That is a capacity argument: it assumes each vehicle is filled to capacity,
    so demand runs out after that many vehicles. Time windows break the
    assumption -- a vehicle whose customers' windows conflict must return to the
    depot part-loaded, so more vehicles of that type are needed to move the same
    demand than the capacity bound predicts. On R1 it allowed only 604 of the
    capacity-30 type when up to 1000 could legitimately be used, which caps the
    fleet mix rather than letting the solver choose it.
    """
    return [min(inst.n_customers, cap_limit) for _c in caps]


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def write_instance(path, inst, variant, caps, costs, counts):
    with open(path, "w") as fh:
        fh.write("%s_%s\n\n" % (inst.name, variant))

        fh.write("VEHICLE\n")
        fh.write("TYPE  NUMBER  CAPACITY  FIXED_COST\n")
        for i, (cap, cost, cnt) in enumerate(zip(caps, costs, counts)):
            fh.write("%4s  %6d  %8d  %10d\n"
                     % (TYPE_LETTERS[i], cnt, cap, int(round(cost))))
        fh.write("\n")

        fh.write("CUSTOMER\n")
        fh.write("CUST NO.  XCOORD.    YCOORD.    DEMAND   READY TIME  "
                 "DUE DATE   SERVICE TIME\n\n")
        for (cid, x, y, dem, ready, due, serv) in inst.customers:
            fh.write("%5d  %10g %10g %10d %10g %10g %10g\n"
                     % (cid, x, y, dem, ready, due, serv))


def describe(inst, caps, costs, counts, variants, fam, ratio_lo, ratio_hi):
    """Human-readable summary of the generated vehicle table."""
    out = []
    out.append("%s  (%d customers, original capacity %d, max demand %d, "
               "total demand %d)"
               % (inst.name, inst.n_customers, inst.capacity,
                  inst.max_demand, inst.total_demand))
    out.append("  family %s -> cost/capacity %.2f (smallest) to %.2f (largest), "
               "spread %.2fx  [Liu & Shen]"
               % (fam or "unknown (fallback)", ratio_lo, ratio_hi,
                  ratio_hi / ratio_lo))
    header = "  type  capacity  count" + "".join(
        "%12s" % ("cost(%s)" % v) for v in variants)
    out.append(header + "     cost/cap(a)")
    for i, cap in enumerate(caps):
        row = "  %4s  %8d  %5d" % (TYPE_LETTERS[i], cap, counts[i])
        for v in variants:
            row += "%12d" % int(round(costs[i] * variants[v]))
        row += "%15.2f" % (costs[i] / cap)
        out.append(row)
    return "\n".join(out)


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def process(path, args, rng):
    inst = parse_solomon(path)

    # Per-family calibration from Liu & Shen, unless overridden on the CLI.
    fam = detect_family(inst.name) or detect_family(path)

    # --liu-shen-exact and --braysy-2009 both copy a published vehicle table
    # straight through, skipping all capacity/cost design.
    table_src = None
    if args.liu_shen_exact:
        table_src = ("--liu-shen-exact", LIU_SHEN_TABLES,
                     "Liu & Shen (1999) Table 7 verbatim")
    elif args.braysy_2009:
        table_src = ("--braysy-2009", BRAYSY_2009_TABLES,
                     "Braysy et al. (2009) Table 1 verbatim")

    if table_src:
        flag, tables, label = table_src
        if fam not in tables:
            raise ValueError(
                "%s needs a recognisable family (R1/C1/RC1/R2/C2/RC2); "
                "could not identify one from %r" % (flag, inst.name))

        # The table is applied verbatim whatever the instance size. This follows
        # the paper: "The same 8 vehicle types and costs are used for every
        # problem size. The vehicle capacities and costs differ only between the
        # six problem sets." Only the vehicle COUNT per type varies with the
        # customer count (see fleet_counts).
        #
        # Braysy could do that because Gehring-Homberger keeps one capacity per
        # family across all sizes, so VB is constant. Randomly generated
        # instances compute their own capacity, so note when it differs -- the
        # table still applies, but the balance between the capacity limit and
        # the time-window limit on route length will not match the G-H sets.
        if args.braysy_2009:
            expected = BRAYSY_2009_VB.get(fam)
            if expected is not None and inst.capacity != expected:
                print("  note: instance capacity %d differs from the G-H value "
                      "%d this table was built for; table applied unchanged, "
                      "per the paper" % (inst.capacity, expected))

        table = tables[fam]
        caps = [c for c, _ in table]
        costs = [float(k) for _, k in table]
        counts = fleet_counts(inst, caps, args.max_vehicles_per_type)
        variants = {k: DEFAULT_VARIANTS[k] for k in args.variants}
        print("%s  (%d customers, base capacity %d, max demand %d, "
              "total demand %d)"
              % (inst.name, inst.n_customers, inst.capacity,
                 inst.max_demand, inst.total_demand))
        print("  family %s -> %s, %d vehicle types"
              % (fam, label, len(caps)))
        hdr = "  type  capacity  count" + "".join(
            "%12s" % ("cost(%s)" % v) for v in variants)
        print(hdr + "     cost/cap(a)")
        for i, cap in enumerate(caps):
            row = "  %4s  %8d  %5d" % (TYPE_LETTERS[i], cap, counts[i])
            for v in variants:
                row += "%12d" % int(round(costs[i] * variants[v]))
            row += "%15.2f" % (costs[i] / cap)
            print(row)
        print()
        if not args.dry_run:
            base = os.path.splitext(os.path.basename(path))[0]
            for vname, scale in variants.items():
                write_instance(
                    os.path.join(args.output_dir, "%s_%s.txt" % (base, vname)),
                    inst, vname, caps, [c * scale for c in costs], counts)
        return

    fam_lo, fam_hi = LIU_SHEN_FAMILIES.get(fam, FALLBACK_RATIOS)
    ratio_lo = args.ratio_smallest if args.ratio_smallest is not None else fam_lo
    ratio_hi = args.ratio_largest if args.ratio_largest is not None else fam_hi

    caps = design_capacities(inst, args.num_types, args.cap_round,
                             args.min_capacity)
    costs = design_costs(caps, args.cost_shape, ratio_lo, ratio_hi, rng)
    counts = fleet_counts(inst, caps, args.max_vehicles_per_type)

    variants = {k: DEFAULT_VARIANTS[k] for k in args.variants}
    print(describe(inst, caps, costs, counts, variants,
                   fam, ratio_lo, ratio_hi))
    print()

    if args.dry_run:
        return

    base = os.path.splitext(os.path.basename(path))[0]
    for vname, scale in variants.items():
        scaled = [c * scale for c in costs]
        out_path = os.path.join(args.output_dir, "%s_%s.txt" % (base, vname))
        write_instance(out_path, inst, vname, caps, scaled, counts)


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--input", required=True,
                   help="Solomon instance file, or a directory of them")
    p.add_argument("--output-dir", default="fsmvrptw_instances",
                   help="where to write generated instances")
    p.add_argument("--num-types", type=int, default=6,
                   help="number of vehicle types (default 6)")
    p.add_argument("--cost-shape", default="superlinear",
                   choices=["superlinear", "linear", "step", "random"],
                   help="fixed-cost shape; superlinear matches Liu & Shen")
    p.add_argument("--liu-shen-exact", action="store_true",
                   help="copy Liu & Shen's Table 7 capacities and costs "
                        "verbatim for the family; ignores --num-types and all "
                        "cost-shape options. Gives 3-6 types depending on "
                        "family rather than a uniform count.")
    p.add_argument("--braysy-2009", action="store_true",
                   help="copy Braysy et al. (2009) Table 1 capacities and costs "
                        "verbatim for the family; ignores --num-types and all "
                        "cost-shape options. 8 types per family, designed for "
                        "the large Gehring-Homberger instances, with two types "
                        "LARGER than the base capacity and economies of scale "
                        "in cost per unit capacity. Published reference results "
                        "exist for variants a and c only.")
    p.add_argument("--ratio-smallest", type=float, default=None,
                   help="fixed cost per unit capacity for the SMALLEST vehicle; "
                        "default is Liu & Shen's value for this family")
    p.add_argument("--ratio-largest", type=float, default=None,
                   help="fixed cost per unit capacity for the LARGEST vehicle; "
                        "default is Liu & Shen's value for this family "
                        "(2.25-2.50 for R/RC, 3.86-4.50 for C)")
    p.add_argument("--min-capacity", type=int, default=None,
                   help="smallest vehicle capacity; default is the largest "
                        "customer demand rounded up (see design_capacities)")
    p.add_argument("--cap-round", type=int, default=10,
                   help="round capacities to this multiple (default 10)")
    p.add_argument("--max-vehicles-per-type", type=int, default=10000,
                   help="cap on the over-provisioned count per type")
    p.add_argument("--variants", default="a,b,c",
                   help="comma-separated cost variants to emit (a, b, c)")
    p.add_argument("--seed", type=int, default=12345,
                   help="RNG seed, used only by --cost-shape random")
    p.add_argument("--dry-run", action="store_true",
                   help="print the vehicle tables without writing files")
    args = p.parse_args()

    if args.liu_shen_exact and args.braysy_2009:
        p.error("--liu-shen-exact and --braysy-2009 are two different published "
                "vehicle tables; pick one")

    args.variants = [v.strip() for v in args.variants.split(",") if v.strip()]
    for v in args.variants:
        if v not in DEFAULT_VARIANTS:
            p.error("unknown variant %r (expected a, b or c)" % v)

    if not args.dry_run:
        os.makedirs(args.output_dir, exist_ok=True)

    rng = random.Random(args.seed)

    if os.path.isdir(args.input):
        files = sorted(os.path.join(args.input, f)
                       for f in os.listdir(args.input)
                       if os.path.isfile(os.path.join(args.input, f)))
    else:
        files = [args.input]

    if not files:
        sys.exit("no input files found in %s" % args.input)

    ok = failed = 0
    for path in files:
        try:
            process(path, args, rng)
            ok += 1
        except Exception as exc:
            print("[SKIP] %s: %s" % (os.path.basename(path), exc),
                  file=sys.stderr)
            failed += 1

    print("processed %d instance(s), %d skipped" % (ok, failed))
    if not args.dry_run:
        print("output written to %s/" % args.output_dir)


if __name__ == "__main__":
    main()
