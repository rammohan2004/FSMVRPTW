#!/usr/bin/env python3
"""
make_sheet.py

Turns cuOpt result CSVs into a single CSV laid out for reading, with one block
per instance size. Import the output straight into Google Sheets:
File > Import > Upload, and choose "Replace spreadsheet".

    python3 make_sheet.py                              # all of outputs_cuopt/
    python3 make_sheet.py outputs_cuopt sheet.csv      # a directory
    python3 make_sheet.py a.csv b.csv c.csv sheet.csv  # explicit files

Layout:
    1. run summary (what was solved, and the verification checks)
    2. summary table: one row per size x family
    3. one detail block per size: one row per instance

Time limits run across the columns. Each block has a two-row header: the first
names the time-limit group, the second names the five measures repeated under
each (Total, Routing, Fixed, Vehicles, Fleet mix). Freeze the top two rows in
Sheets to keep them visible while scrolling. No colours or formulas -- plain
values, so it survives any importer.
"""

import csv
import glob
import os
import sys
from collections import defaultdict, OrderedDict

FAMILIES = ["C1", "C2", "R1", "R2", "RC1", "RC2"]
MEASURES = ["Total cost", "Routing cost", "Fixed cost", "Vehicles", "Fleet mix"]
IDENT = ["Instance", "Family", "Types"]


def num(x, nd=2):
    """Plain rounded number, or blank. No thousands separators -- let the
    spreadsheet do its own formatting."""
    if x is None or x == "":
        return ""
    return round(float(x), nd)


def size_label(n):
    n = int(n)
    return "%s customers" % format(n, ",")


def sort_key(instance):
    """C1_10_1 before C1_10_2 before C1_10_10 (numeric, not string)."""
    stem = instance.replace(".txt", "")
    parts = stem.split("_")
    fam = parts[0]
    try:
        idx = int(parts[2])
    except (IndexError, ValueError):
        idx = 0
    return (FAMILIES.index(fam) if fam in FAMILIES else 99, idx, stem)


def stats(group):
    """Averages over a group of result rows. Skips rows with no solution."""
    g = [r for r in group if r.get("total_cost")]
    if not g:
        return None
    n = len(g)
    return (sum(float(x["total_cost"]) for x in g) / n,
            sum(float(x["routing_cost"]) for x in g) / n,
            sum(float(x["fixed_cost"]) for x in g) / n,
            sum(int(x["vehicles_used"]) for x in g) / n,
            sum(float(x["solve_time_s"]) for x in g) / n)


def load_rows(args):
    """Accept a directory, or explicit CSV paths."""
    paths = []
    for a in args:
        if os.path.isdir(a):
            paths.extend(sorted(glob.glob(os.path.join(a, "*results*.csv"))))
        else:
            paths.append(a)
    rows = []
    for p in paths:
        if not os.path.exists(p):
            sys.stderr.write("skipping missing %s\n" % p)
            continue
        with open(p) as fh:
            found = list(csv.DictReader(fh))
        rows.extend(found)
        print("read %-52s %4d rows" % (p, len(found)))
    return rows


def main():
    argv = sys.argv[1:]
    if not argv:
        argv = ["outputs_cuopt", "outputs_cuopt/fsmvrptw_sheet.csv"]
    if len(argv) >= 2 and argv[-1].lower().endswith(".csv") \
            and not os.path.exists(argv[-1]):
        dst = argv[-1]
        srcs = argv[:-1]
    elif len(argv) >= 2 and not os.path.isdir(argv[-1]) \
            and argv[-1].lower().endswith(".csv") and len(argv) > 1:
        dst = argv[-1]
        srcs = argv[:-1]
    else:
        srcs = argv
        dst = "outputs_cuopt/fsmvrptw_sheet.csv"

    rows = load_rows(srcs)
    if not rows:
        sys.exit("no result rows found in %s" % srcs)
    print()

    # time limits, longest first: the longest is the headline, the rest show
    # how far from converged the shorter limits are
    timeouts = sorted({r["timeout_s"] for r in rows},
                      key=lambda t: -float(t))
    # sizes, smallest first
    sizes = sorted({int(r["n_customers"]) for r in rows})

    out = []
    w = out.append

    # ---- 1. run summary -------------------------------------------------
    solved = [r for r in rows if r.get("status_name") == "SUCCESS"]
    checks_ok = all(
        r["routing_cost_check"] == "OK" and r["cost_split_check"] == "OK"
        and r["cost_source"] == "cuopt" and not r["fleet_cap_binding"]
        and not r["notes"] for r in solved) and len(solved) == len(rows)

    w(["NVIDIA cuOpt on FSMVRPTW"])
    w([])
    w(["Objective", "total distance travelled + fixed cost of vehicles used"])
    w(["Vehicle types", "Braysy et al. (2009) Table 1, cost structure a"])
    w(["Sizes", ", ".join(format(s, ",") for s in sizes)])
    w(["Time limits (s)", ", ".join("%g" % float(t) for t in timeouts)])
    w(["Runs", len(rows)])
    w(["Solved", "%d / %d" % (len(solved), len(rows))])
    w(["Verification",
       "all checks passed" if checks_ok else "SEE the check columns in the raw CSV"])
    if checks_ok:
        w(["", "cost split reported by cuOpt, not derived"])
        w(["", "routing cost matches distance of the returned routes"])
        w(["", "routing + fixed reconciles to the objective"])
        w(["", "no vehicle type was exhausted (fleet bound never binding)"])
    w([])
    w(["Total cost = Routing cost + Fixed cost, at each time limit."])
    w([])
    w([])

    agg = defaultdict(list)
    for r in rows:
        agg[(int(r["n_customers"]), r["family"], r["timeout_s"])].append(r)

    # ---- 2. summary: size x family, time limits across -------------------
    w(["SUMMARY - averages over the instances of each family"])
    w([])
    top = ["Customers", "Family", "Instances", "Types"]
    bot = ["Customers", "Family", "Instances", "Types"]
    for t in timeouts:
        top += ["%g s time limit" % float(t)] + [""] * 3
        bot += ["Total cost", "Routing cost", "Fixed cost", "Vehicles"]
    w(top)
    w(bot)

    for size in sizes:
        fams = [f for f in FAMILIES if (size, f, timeouts[0]) in agg]
        for fam in fams:
            first = agg[(size, fam, timeouts[0])]
            row = [size, fam, len(first), first[0]["n_types"]]
            for t in timeouts:
                s = stats(agg.get((size, fam, t), []))
                row += ["", "", "", ""] if s is None else \
                       [num(s[0]), num(s[1]), num(s[2]), num(s[3], 1)]
            w(row)
        # all families at this size
        row = [size, "ALL", "", ""]
        for t in timeouts:
            s = stats([r for r in rows
                       if int(r["n_customers"]) == size and r["timeout_s"] == t])
            row += ["", "", "", ""] if s is None else \
                   [num(s[0]), num(s[1]), num(s[2]), num(s[3], 1)]
        w(row)
        w([])
    w([])

    # ---- 3. one detail block per size ------------------------------------
    by_inst = defaultdict(dict)
    for r in rows:
        by_inst[(int(r["n_customers"]), r["instance"])][r["timeout_s"]] = r

    top = list(IDENT)
    bot = list(IDENT)
    for t in timeouts:
        top += ["%g s time limit" % float(t)] + [""] * (len(MEASURES) - 1)
        bot += MEASURES

    for size in sizes:
        w(["%s" % size_label(size).upper()])
        w([])
        w(top)
        w(bot)

        insts = sorted({r["instance"] for r in rows
                        if int(r["n_customers"]) == size}, key=sort_key)
        for inst in insts:
            first = by_inst[(size, inst)][timeouts[0]]
            row = [inst.replace(".txt", ""), first["family"], first["n_types"]]
            for t in timeouts:
                r = by_inst[(size, inst)].get(t)
                if not r or not r.get("total_cost"):
                    row += [r["status_name"] if r else ""] + [""] * 4
                    continue
                row += [num(r["total_cost"]), num(r["routing_cost"]),
                        num(r["fixed_cost"]), int(r["vehicles_used"]), r["mix"]]
            w(row)

        w([])
        w(["Averages by family"])
        for fam in [f for f in FAMILIES if (size, f, timeouts[0]) in agg]:
            first = agg[(size, fam, timeouts[0])]
            row = ["%s average" % fam, fam, first[0]["n_types"]]
            for t in timeouts:
                s = stats(agg.get((size, fam, t), []))
                row += [""] * 5 if s is None else \
                       [num(s[0]), num(s[1]), num(s[2]), num(s[3], 1), ""]
            w(row)

        row = ["%s AVERAGE" % size_label(size).upper(), "", ""]
        for t in timeouts:
            s = stats([r for r in rows
                       if int(r["n_customers"]) == size and r["timeout_s"] == t])
            row += [""] * 5 if s is None else \
                   [num(s[0]), num(s[1]), num(s[2]), num(s[3], 1), ""]
        w(row)
        w([])
        w([])

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "w", newline="") as fh:
        csv.writer(fh).writerows(out)

    print("wrote %s  (%d lines, %d sizes)" % (dst, len(out), len(sizes)))


if __name__ == "__main__":
    main()
