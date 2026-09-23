#!/usr/bin/env python3
"""cuOpt results in the wide per-instance layout: one row per instance, one
column group per time limit.

    python make_wide_sheet.py 10000     -> outputs_cuopt/wide_10000.csv
    python make_wide_sheet.py 1000      -> outputs_cuopt/wide_1000.csv

Layout (matching the existing 1,000-customer sheet):

    Instance | Family | Types | <limit 1: Total, Routing, Fixed, Vehicles, Mix>
                              | <limit 2: ...> | ...

At 10,000 customers the first group is the EQUAL-TIME budget, which differs per
instance -- it is each instance's own budget, set to our solver's runtime on that
same instance -- so the actual seconds are carried in their own column rather
than being assumed constant across the group.
"""

import csv
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "outputs_cuopt")

# Which file holds each size, and which limits to show in which order.
# None means "this instance's own equal-time budget".
SOURCES = {
    "10000": [(os.path.join(OUT_DIR, "equal_time_10000.csv"),
               [None, 600.0, 2000.0])],
    "1000": [(os.path.join(OUT_DIR, "equal_time_1000.csv"), [None]),
             (os.path.join(OUT_DIR, "fsmvrptw_1000_results.csv"),
              [60.0, 10.0, 5.0, 2.0])],
}

FIELDS = ["total_cost", "routing_cost", "fixed_cost", "vehicles_used", "mix"]
LABELS = ["Total cost", "Routing cost", "Fixed cost", "Vehicles", "Fleet mix"]


def load(path):
    """instance -> {timeout: row}, SUCCESS only."""
    out = defaultdict(dict)
    if not os.path.exists(path):
        return out
    for r in csv.DictReader(open(path)):
        if r.get("status_name") and r["status_name"] != "SUCCESS":
            continue
        out[os.path.basename(r["instance"])][round(float(r["timeout_s"]), 1)] = r
    return out


def main():
    size = sys.argv[1] if len(sys.argv) > 1 else "10000"
    if size not in SOURCES:
        print("usage: make_wide_sheet.py <1000|10000>")
        return 2

    # Merge every source file for this size into one instance -> {timeout: row}.
    data = defaultdict(dict)
    groups = []           # (source_index, timeout or None)
    for si, (path, limits) in enumerate(SOURCES[size]):
        d = load(path)
        if not d:
            print(f"  (skipping missing {os.path.basename(path)})")
            continue
        for k, v in d.items():
            data[k].update(v)
        for t in limits:
            groups.append(t)

    if not data:
        print("No cuOpt results found for size", size)
        return 1

    # Our own runtimes supply the equal-time budget per instance.
    ours = {}
    ours_path = os.path.join(os.path.dirname(HERE), "FSMVRPTW_Solver",
                             "outputs", size,
                             "results_after(8+9).csv" if size == "1000"
                             else "results.csv")
    if os.path.exists(ours_path):
        for r in csv.DictReader(open(ours_path)):
            ours[os.path.basename(r["instance"])] = round(
                float(r["solve_time_s"]), 1)

    def label_for(t):
        if t is None:
            return "equal time (= our runtime, per instance)"
        return f"{t:g} s time limit"

    rows = []
    # --- two header rows, as in the existing sheet ---
    h1 = ["Instance", "Family", "Types", "Our time (s)"]
    h2 = ["Instance", "Family", "Types", "Our time (s)"]
    for t in groups:
        h1 += [label_for(t)] + [""] * (len(FIELDS) - 1)
        h2 += LABELS
    rows.append(h1)
    rows.append(h2)

    def sort_key(n):
        # C1_10_2_a before C1_10_10_a: sort on the numeric index, not the string.
        p = n.replace(".txt", "").split("_")
        num = next((int(x) for x in reversed(p) if x.isdigit()), 0)
        return (p[0], num)

    missing = 0
    for n in sorted(data, key=sort_key):
        stem = n.replace(".txt", "")
        fam = stem.split("_")[0]
        any_row = next(iter(data[n].values()))
        r = [stem, fam, any_row.get("n_types", ""), ours.get(n, "")]
        for t in groups:
            key = ours.get(n) if t is None else t
            row = data[n].get(key)
            if row:
                # 2 dp on the money columns; cuOpt emits full float precision,
                # which is unreadable in a sheet and implies accuracy it has not
                # got (its own run-to-run spread is ~1.5% on a single instance).
                for f in FIELDS:
                    v = row[f]
                    if f.endswith("_cost"):
                        try:
                            v = f"{float(v):.2f}"
                        except ValueError:
                            pass
                    r.append(v)
            else:
                r += [""] * len(FIELDS)
                missing += 1
        rows.append(r)

    out = os.path.join(OUT_DIR, f"wide_{size}.csv")
    with open(out, "w", newline="") as f:
        csv.writer(f).writerows(rows)

    print(f"Wrote {out}")
    print(f"  {len(rows)-2} instances x {len(groups)} time limits")
    print(f"  groups: {', '.join(label_for(t) for t in groups)}")
    if missing:
        print(f"  NOTE: {missing} instance/limit cells had no matching result")
    return 0


if __name__ == "__main__":
    sys.exit(main())
