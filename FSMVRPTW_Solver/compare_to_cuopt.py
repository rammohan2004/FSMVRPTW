#!/usr/bin/env python3
"""Compare our solver's results against the cuOpt curve.

    python compare_to_cuopt.py outputs/1000/results.csv \
        ../cuOpt_FSMVRPTW/outputs_cuopt/fsmvrptw_1000_results.csv

Our solver is a terminating pipeline: it runs to a local optimum and stops, so
it yields ONE (time, cost) point per instance. cuOpt yields a curve, measured at
several time limits. The comparison is therefore time-to-target, read two ways:

  equal time  -- at our runtime T, what cost had cuOpt reached?
  equal cost  -- to reach our cost C, how long did cuOpt need?

Writes a sheet-ready CSV next to the input and prints a summary.
"""

import csv
import os
import sys
from collections import defaultdict


def load_ours(path):
    """Our rows, keyed by instance basename. Refuses rows that failed a check."""
    rows, rejected = {}, []
    for r in csv.DictReader(open(path)):
        name = os.path.basename(r["instance"])
        checks = ("routing_cost_check", "cost_split_check",
                  "customers_covered", "feasible")
        bad = [c for c in checks if r.get(c) != "OK"]
        if bad:
            # A failed solution must never reach a comparison sheet. Reporting a
            # cost we cannot vouch for is worse than reporting nothing.
            rejected.append((name, ",".join(bad)))
            continue
        rows[name] = r
    return rows, rejected


def load_cuopt(path):
    """cuOpt cost per (instance, timeout), plus the sorted list of timeouts."""
    curve = defaultdict(dict)
    timeouts = set()
    for r in csv.DictReader(open(path)):
        if r["status_name"] != "SUCCESS":
            continue
        t = float(r["timeout_s"])
        timeouts.add(t)
        curve[r["instance"]][t] = r
    return curve, sorted(timeouts)


def family_of(name):
    return name.split("_")[0]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ours_path, cuopt_path = sys.argv[1], sys.argv[2]

    ours, rejected = load_ours(ours_path)
    curve, timeouts = load_cuopt(cuopt_path)

    if rejected:
        print(f"REFUSED {len(rejected)} row(s) that failed verification:")
        for name, why in rejected:
            print(f"  {name}: {why}")
        print()

    shared = sorted(set(ours) & set(curve))
    only_ours = sorted(set(ours) - set(curve))
    if only_ours:
        print(f"NOTE: {len(only_ours)} instance(s) not in the cuOpt file, skipped:"
              f" {', '.join(only_ours[:5])}"
              f"{' ...' if len(only_ours) > 5 else ''}\n")
    if not shared:
        print("No instances in common -- nothing to compare.")
        return 1

    out_path = os.path.join(os.path.dirname(ours_path) or ".",
                            "comparison_vs_cuopt.csv")
    header = (["instance", "family", "our_total_cost", "our_routing_cost",
               "our_fixed_cost", "our_vehicles", "our_time_s"]
              + [f"cuopt_cost_{t:g}s" for t in timeouts]
              + [f"gap_vs_{t:g}s_%" for t in timeouts]
              + ["cuopt_time_to_match_s", "cuopt_vehicles_60s"])

    rows_out = []
    # Accumulators for the summary.
    gap_sum = defaultdict(float)
    gap_n = defaultdict(int)
    by_family = defaultdict(lambda: defaultdict(list))
    matched_at = defaultdict(int)
    our_time_total = 0.0

    for name in shared:
        o = ours[name]
        our_cost = float(o["total_cost"])
        our_time = float(o["solve_time_s"])
        our_time_total += our_time
        fam = family_of(name)

        row = [name, fam, f"{our_cost:.2f}", f"{float(o['routing_cost']):.2f}",
               f"{float(o['fixed_cost']):.2f}", o["vehicles_used"],
               f"{our_time:.3f}"]

        costs = {}
        for t in timeouts:
            r = curve[name].get(t)
            costs[t] = float(r["total_cost"]) if r else None
            row.append(f"{costs[t]:.2f}" if costs[t] is not None else "")

        for t in timeouts:
            if costs[t] is None:
                row.append("")
                continue
            # Positive gap = we are more expensive than cuOpt at that limit.
            gap = (our_cost - costs[t]) / costs[t] * 100.0
            row.append(f"{gap:.2f}")
            gap_sum[t] += gap
            gap_n[t] += 1
            by_family[fam][t].append(gap)

        # Equal cost: the shortest cuOpt limit that already reaches our cost.
        match = next((t for t in timeouts
                      if costs[t] is not None and costs[t] <= our_cost), None)
        if match is None:
            row.append(f">{timeouts[-1]:g}")
            matched_at["none"] += 1
        else:
            row.append(f"{match:g}")
            matched_at[match] += 1

        last = curve[name].get(timeouts[-1])
        row.append(last["vehicles_used"] if last else "")
        rows_out.append(row)

    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows_out)

    # ---- summary ----------------------------------------------------------
    n = len(shared)
    print(f"Instances compared : {n}")
    print(f"Our mean runtime   : {our_time_total / n:.2f} s\n")

    print("Mean gap vs cuOpt (positive = we cost more):")
    for t in timeouts:
        if gap_n[t]:
            print(f"  cuOpt @ {t:>5g} s : {gap_sum[t] / gap_n[t]:+7.2f} %")

    print("\nBy family:")
    fams = sorted(by_family)
    print("  family  " + "".join(f"{t:>10g}s" for t in timeouts))
    for fam in fams:
        cells = ""
        for t in timeouts:
            g = by_family[fam][t]
            cells += f"{sum(g) / len(g):+10.2f}" if g else f"{'':>10}"
        print(f"  {fam:<7} " + cells)

    print("\nTime cuOpt needed to match our cost:")
    for t in timeouts:
        if matched_at[t]:
            print(f"  {t:>5g} s or less : {matched_at[t]:3d} instance(s)")
    if matched_at["none"]:
        print(f"  never (>{timeouts[-1]:g} s) : {matched_at['none']:3d} instance(s)"
              "   <- we win outright on these")

    print(f"\nWrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
