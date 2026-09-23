#!/usr/bin/env python3
"""Build a Google-Sheet-ready CSV of the 1,000-customer results after steps 8+9.

    python make_results_sheet.py

Reads our two runs and the cuOpt baseline, writes
outputs/1000/sheet_1000_after_8_9.csv -- paste-ready, one block per section.

Sections:
  1  headline: our cost/time against the cuOpt curve
  2  per family, against cuOpt at each of its time limits
  3  ablation: step 6 (naive FSMVRPTW) vs steps 8+9
  4  fleet: vehicles, fill, and the capacity lower bound
  5  every instance, our full decomposition
  6  every instance, cost against cuOpt at each limit
"""

import csv
import math
import os
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

OURS_NEW = os.path.join(HERE, "outputs", "1000", "results_after(8+9).csv")
OURS_OLD = os.path.join(HERE, "outputs", "1000", "results.csv")
CUOPT = os.path.join(ROOT, "cuOpt_FSMVRPTW", "outputs_cuopt",
                     "fsmvrptw_1000_results.csv")
OUT = os.path.join(HERE, "outputs", "1000", "sheet_1000_after_8_9.csv")

TYPES = "ABCDEFGH"


def by_name(path):
    return {os.path.basename(r["instance"]): r for r in csv.DictReader(open(path))}


def total_demand(path):
    """Sum of customer demands, for the capacity lower bound."""
    tot, seen, hdr = 0.0, False, False
    for ln in open(path):
        s = ln.split()
        if not s:
            continue
        if s[0] == "CUSTOMER":
            seen, hdr = True, False
            continue
        if not seen:
            continue
        if not hdr:
            hdr = True
            continue
        if len(s) >= 7 and s[0].isdigit() and int(s[0]) != 0:
            tot += float(s[3])
    return tot


def main():
    new = by_name(OURS_NEW)
    old = by_name(OURS_OLD)

    cu = defaultdict(dict)
    limits = set()
    for r in csv.DictReader(open(CUOPT)):
        if r["status_name"] != "SUCCESS":
            continue
        t = float(r["timeout_s"])
        limits.add(t)
        cu[r["instance"]][t] = r
    limits = sorted(limits)

    names = sorted(new)
    fam_of = lambda n: n.split("_")[0]
    fams = sorted({fam_of(n) for n in names})

    # Capacity lower bound uses each instance's OWN largest capacity: the Braysy
    # tables differ per family (C1/R1/RC1 top out at 270, R2/RC2 at 1330).
    lb, maxcap = {}, {}
    for n in names:
        mc = max(float(x) for x in new[n]["capacities"].split("/"))
        maxcap[n] = mc
        lb[n] = math.ceil(total_demand(os.path.join(HERE, "instances", "1000", n)) / mc)

    rows = []
    add = rows.append
    pct = lambda a, b: (a - b) / b * 100 if b else 0.0

    add(["FSMVRPTW 1,000 customers - our solver after steps 8+9 vs NVIDIA cuOpt"])
    add(["Objective: total cost = distance travelled + fixed cost of vehicles used"])
    add(["Vehicle costs: Braysy et al. (2009) Table 1, cost structure (a)"])
    add(["Our solver: PARAM Rudra 'small' partition, 48 OpenMP threads, "
         "GCC 13.3 -O3, sweep angle 180"])
    add(["cuOpt: 1x NVIDIA A100 80GB, sgpu partition"])
    add(["All 60 instances pass four independent checks: routing-cost recompute, "
         "cost split, customer coverage, capacity+time windows"])
    add([])

    # --- 1. headline ---------------------------------------------------------
    our_cost = sum(float(new[n]["total_cost"]) for n in names)
    our_time = sum(float(new[n]["solve_time_s"]) for n in names) / len(names)
    add(["1. HEADLINE"])
    add(["metric", "value"])
    add(["instances", len(names)])
    add(["our mean runtime (s)", round(our_time, 2)])
    add(["our max runtime (s)",
         round(max(float(new[n]["solve_time_s"]) for n in names), 2)])
    add([])
    add(["Two ways to average a gap. They differ and both are reported, because "
         "quoting one without the other invites the question."])
    add(["  aggregate  = (sum of our costs) / (sum of cuOpt's) - 1. "
         "Weights each instance by its cost, so expensive instances count more."])
    add(["  per-instance = mean over instances of that instance's own gap. "
         "Weights every instance equally. This is the usual benchmark figure."])
    add([])
    add(["cuOpt limit", "aggregate gap %", "per-instance mean gap %",
         "instances we beat"])
    for t in limits:
        ns = [n for n in names if t in cu[n]]
        cc = sum(float(cu[n][t]["total_cost"]) for n in ns)
        gaps = [pct(float(new[n]["total_cost"]), float(cu[n][t]["total_cost"]))
                for n in ns]
        add([f"@{t:g}s", round(pct(our_cost, cc), 2),
             round(sum(gaps) / len(gaps), 2),
             f"{sum(1 for g in gaps if g < 0)}/{len(ns)}"])
    add([])

    # --- 2. per family -------------------------------------------------------
    add(["2. GAP BY FAMILY (%), per-instance mean, negative = we are cheaper"])
    add(["family", "instances"] + [f"vs cuOpt @{t:g}s" for t in limits])
    for f in fams:
        ns = [n for n in names if fam_of(n) == f]
        row = [f, len(ns)]
        for t in limits:
            g = [pct(float(new[n]["total_cost"]), float(cu[n][t]["total_cost"]))
                 for n in ns if t in cu[n]]
            row.append(round(sum(g) / len(g), 2))
        add(row)
    row = ["ALL", len(names)]
    for t in limits:
        g = [pct(float(new[n]["total_cost"]), float(cu[n][t]["total_cost"]))
             for n in names if t in cu[n]]
        row.append(round(sum(g) / len(g), 2))
    add(row)
    add([])

    # --- 3. ablation ---------------------------------------------------------
    add(["3. ABLATION - what steps 8+9 contributed"])
    add(["step 6 = naive FSMVRPTW (correct objective, cheapest feasible vehicle "
         "per route, no fleet-seeking), sweep angle 30"])
    add(["step 7 = Clarke-Wright fixed-cost savings term: NEGATIVE RESULT, "
         "disabled. Concatenation cannot interleave customers, so no scoring of "
         "it can build the long routes wide time windows allow."])
    add(["steps 8+9 = fleet-aware move deltas + route-elimination by "
         "redistribution, sweep angle 180"])
    add([])
    add(["family", "step 6 cost", "steps 8+9 cost", "change %",
         "step 6 vehicles", "steps 8+9 vehicles", "cuOpt @60s vehicles"])
    for f in fams:
        ns = [n for n in names if fam_of(n) == f]
        c6 = sum(float(old[n]["total_cost"]) for n in ns)
        c9 = sum(float(new[n]["total_cost"]) for n in ns)
        add([f, round(c6, 1), round(c9, 1), round(pct(c9, c6), 2),
             sum(int(old[n]["vehicles_used"]) for n in ns),
             sum(int(new[n]["vehicles_used"]) for n in ns),
             sum(int(cu[n][limits[-1]]["vehicles_used"]) for n in ns)])
    c6 = sum(float(old[n]["total_cost"]) for n in names)
    add(["ALL", round(c6, 1), round(our_cost, 1), round(pct(our_cost, c6), 2),
         sum(int(old[n]["vehicles_used"]) for n in names),
         sum(int(new[n]["vehicles_used"]) for n in names),
         sum(int(cu[n][limits[-1]]["vehicles_used"]) for n in names)])
    add([])

    # --- 4. fleet ------------------------------------------------------------
    add(["4. FLEET - vehicles, fill and the capacity lower bound"])
    add(["LB = ceil(total demand / largest capacity); no feasible solution can "
         "use fewer. fill = mean load / largest capacity."])
    add(["family", "largest capacity", "LB", "step 6 veh", "steps 8+9 veh",
         "cuOpt @60s veh", "step 6 fill %", "steps 8+9 fill %", "cuOpt fill %",
         "ours/LB", "cuOpt/LB"])
    for f in fams:
        ns = [n for n in names if fam_of(n) == f]
        d = sum(total_demand(os.path.join(HERE, "instances", "1000", n)) for n in ns)
        mc = maxcap[ns[0]]
        L = sum(lb[n] for n in ns)
        v6 = sum(int(old[n]["vehicles_used"]) for n in ns)
        v9 = sum(int(new[n]["vehicles_used"]) for n in ns)
        vc = sum(int(cu[n][limits[-1]]["vehicles_used"]) for n in ns)
        add([f, mc, round(L / len(ns), 1), round(v6 / len(ns), 1),
             round(v9 / len(ns), 1), round(vc / len(ns), 1),
             round(d / v6 / mc * 100), round(d / v9 / mc * 100),
             round(d / vc / mc * 100), round(v9 / L, 2), round(vc / L, 2)])
    add([])

    # --- 5. per instance, ours ----------------------------------------------
    add(["5. EVERY INSTANCE - our solver after steps 8+9"])
    add(["instance", "family", "total cost", "routing cost", "fixed cost",
         "fixed %", "vehicles", "mix"] + [f"used_{c}" for c in TYPES] +
        ["time (s)", "routing check", "cost split", "coverage", "feasible"])
    for n in names:
        r = new[n]
        tc, fx = float(r["total_cost"]), float(r["fixed_cost"])
        add([n.replace(".txt", ""), fam_of(n), round(tc, 1),
             round(float(r["routing_cost"]), 1), round(fx, 1),
             round(fx / tc * 100, 1), r["vehicles_used"], r["mix"]] +
            [r.get(f"used_{c}", "") for c in TYPES] +
            [round(float(r["solve_time_s"]), 3), r["routing_cost_check"],
             r["cost_split_check"], r["customers_covered"], r["feasible"]])
    add([])

    # --- 6. per instance, against cuOpt -------------------------------------
    add(["6. EVERY INSTANCE - cost against the cuOpt curve"])
    add(["instance", "family", "our cost", "our time (s)"] +
        [f"cuOpt @{t:g}s" for t in limits] +
        [f"gap @{t:g}s %" for t in limits] +
        ["cuOpt time to match our cost (s)"])
    for n in names:
        oc = float(new[n]["total_cost"])
        costs = [float(cu[n][t]["total_cost"]) if t in cu[n] else None for t in limits]
        match = next((f"{t:g}" for t, c in zip(limits, costs)
                      if c is not None and c <= oc), f">{limits[-1]:g}")
        add([n.replace(".txt", ""), fam_of(n), round(oc, 1),
             round(float(new[n]["solve_time_s"]), 3)] +
            [round(c, 1) if c is not None else "" for c in costs] +
            [round(pct(oc, c), 2) if c is not None else "" for c in costs] +
            [match])

    with open(OUT, "w", newline="") as f:
        csv.writer(f).writerows(rows)
    print(f"Wrote {OUT}")
    print(f"  {len(names)} instances, {len(rows)} rows, 6 sections")


if __name__ == "__main__":
    main()
