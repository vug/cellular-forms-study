"""How much of a form's description comes from its parameters, and how much from the seed?

    python scripts/seed_spread.py --out runs/study/seed_spread --sets 48 --seeds 8 --cells 30000
    python scripts/seed_spread.py --out runs/study/seed_spread --report

The first study for the parameter search (docs/design.md "Finding interesting parameters").
It draws parameter sets that fill the base parameter space evenly (a Latin hypercube; set 0 is
the defaults), grows each with several seeds, and describes every form at about 1,000, 3,200,
10,000 cells and at the end (cf_describe on the checkpoints). The report then says, for every
descriptor:

  - the spread between seeds of one parameter set against the spread between parameter sets:
    a descriptor is only worth searching over if parameters move it more than seeds do, and
    the spread between seeds is the unit in which two forms count as different;
  - how well a descriptor at a small size predicts the same descriptor at the end, which is
    how small a screening run may be;
  - which parameter sets have an unusually large spread between seeds (a sign of a set that
    sits between two kinds of form), and for which sets the seed decides whether the run
    finishes at all.

Every run is one line of <out>/log.jsonl; a run already in the log is not repeated, so a study
can be continued or enlarged. The state files are deleted once described (--keep keeps them).
Only the Python standard library is used. Same arguments and code, same GPU: same study.
"""
import argparse
import glob
import json
import math
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import explore  # noqa: E402  (the tools, the parameter ranges and the damping rule of the search)

# The descriptors that are single numbers (cf_describe prints a few lists as well).
SCALARS = ["area_ratio", "sphericity", "elongation", "flatness", "ellipsoid_fill", "curvature_mean", "curvature_std", "concave_fraction",
           "box_dimension", "link_mean", "link_std", "fold_ratio", "close_pairs_per_cell", "relief_micro", "relief_meso", "relief_macro", "relief_rest",
           "relief_s1", "relief_s2", "relief_s4", "relief_s8", "relief_s16", "skew_s4", "skew_s8", "skew_s16"]
CHECKPOINT_FACTOR = math.sqrt(10.0)  # checkpoints when the cell count passes 316, 1000, 3162, 10000, ...


def parameter_sets(count, seed):
    """Set 0: the defaults. The others: a Latin hypercube over the base ranges of explore.py
    (every range cut into `count` slices, each slice used once per parameter)."""
    rng = random.Random(seed)
    sets = [{}]
    names = sorted(explore.BASE_SPACE)
    slices = {}
    for name in names:
        order = list(range(count))
        rng.shuffle(order)
        slices[name] = order
    for i in range(count):
        base = {}
        for name in names:
            lo, hi, log = explore.BASE_SPACE[name]
            u = (slices[name][i] + rng.random()) / count
            base[name] = math.exp(math.log(lo) + u * (math.log(hi) - math.log(lo))) if log else lo + u * (hi - lo)
        sets.append(explore.to_params({"base": base, "sources": {}}))
    return sets


def run_one(out_dir, set_id, params, seed, cells, max_steps, max_seconds, keep):
    run_dir = os.path.join(out_dir, "runs", f"set{set_id:03d}_seed{seed:02d}")
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(run_dir, exist_ok=True)
    cmd = [explore.tool("cf_gpu"), "--new", "icosphere:2", "--seed", str(seed), "--capacity", str(int(cells * 1.5) + 4096), "--until-cells", str(cells),
           "--until-step", str(max_steps), "--max-seconds", str(max_seconds), "--out", run_dir, "--snapshot-factor", f"{CHECKPOINT_FACTOR:.9g}",
           "--checkpoint-factor", f"{CHECKPOINT_FACTOR:.9g}"]  # (cf_gpu writes a checkpoint only where it takes a snapshot)
    for k, v in params.items():
        cmd += ["--param", f"{k}={v:.9g}"]
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    summary = explore.parse_summary(proc.stdout)
    record = {"set": set_id, "seed": seed, "params": params, "command": " ".join(cmd), "wall_s": round(time.time() - t0, 2), "exit_code": proc.returncode,
              "summary": {k: summary.get(k) for k in ("stop_reason", "step", "cells", "clamped", "invariants", "wall_seconds")}, "sizes": []}
    if proc.returncode != 0 or summary.get("invariants") != "ok":
        record["status"] = "broken"
        record["detail"] = (proc.stderr or proc.stdout)[-400:]
    elif summary.get("stop_reason") != "until-cells":
        record["status"] = "stalled" if summary.get("stop_reason") in ("time-budget", "until-step", "steps") else str(summary.get("stop_reason"))
    else:
        record["status"] = "ok"
    # Describe what there is, also of a run that did not finish: how far it got is information.
    files = sorted(glob.glob(os.path.join(run_dir, "ckpt_c*.cfs")))
    if record["status"] == "ok":
        files.append(os.path.join(run_dir, "latest.cfs"))
    for path in files:
        described = subprocess.run([explore.tool("cf_describe"), path], capture_output=True, text=True)
        if described.returncode != 0:
            record["status"] = "broken"
            record["detail"] = described.stderr[-400:]
            break
        d = json.loads(described.stdout)
        # The relief of each band by its smoothing length (relief_s4: the band at 4 links), and
        # which way its heights lean. A form too small for a band has no entry for it.
        for sigma, relief, skew in zip(d.get("relief_sigma", []), d.get("relief", []), d.get("relief_skew", [])):
            d[f"relief_s{sigma:g}"] = relief
            d[f"skew_s{sigma:g}"] = skew
        m = re.search(r"ckpt_c0*(\d+)_", os.path.basename(path))
        # A checkpoint is named by the half decade it was taken at: 6 = about 1,000 cells,
        # 7 = about 3,200, 8 = about 10,000.
        d["stage"] = "end" if m is None else round(2.0 * math.log10(max(d["cells"], 1)))
        record["sizes"].append(d)
    if not keep:
        shutil.rmtree(run_dir, ignore_errors=True)
    return record


def load(out_dir):
    path = os.path.join(out_dir, "log.jsonl")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def ranks(values):
    order = sorted(range(len(values)), key=lambda i: values[i])
    out = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            out[order[k]] = 0.5 * (i + j)
        i = j + 1
    return out


def rank_correlation(a, b):
    if len(a) < 3:
        return float("nan")
    ra, rb = ranks(a), ranks(b)
    ma, mb = statistics.fmean(ra), statistics.fmean(rb)
    num = sum((x - ma) * (y - mb) for x, y in zip(ra, rb))
    den = math.sqrt(sum((x - ma) ** 2 for x in ra) * sum((y - mb) ** 2 for y in rb))
    return num / den if den > 0 else float("nan")


def report(out_dir, min_seeds=4):
    records = load(out_dir)
    if not records:
        sys.exit("seed_spread.py: nothing in the log yet")
    by_set = {}
    for r in records:
        by_set.setdefault(r["set"], []).append(r)

    def values(set_id, stage, name):
        out = []
        for r in by_set[set_id]:
            for d in r["sizes"]:
                if d["stage"] == stage and name in d:
                    out.append(d[name])
        return out

    lines = []
    say = lines.append
    sets = sorted(by_set)
    outcome = {"every seed finished": 0, "no seed finished": 0, "the seed decides": 0}
    mixed = []
    for s in sets:
        ok = sum(1 for r in by_set[s] if r["status"] == "ok")
        key = "every seed finished" if ok == len(by_set[s]) else "no seed finished" if ok == 0 else "the seed decides"
        outcome[key] += 1
        if key == "the seed decides":
            mixed.append((s, ok, len(by_set[s]), sorted({r["status"] for r in by_set[s]})))
    statuses = {}
    for r in records:
        statuses[r["status"]] = statuses.get(r["status"], 0) + 1
    say(f"{len(records)} runs of {len(sets)} parameter sets; runs by outcome: " + ", ".join(f"{k} {v}" for k, v in sorted(statuses.items())))
    say("parameter sets: " + ", ".join(f"{k}: {v}" for k, v in outcome.items()))
    for s, ok, n, kinds in mixed:
        say(f"  set {s}: {ok} of {n} seeds finished ({', '.join(kinds)})")

    usable = [s for s in sets if len(values(s, "end", "area_ratio")) >= min_seeds]
    if len(usable) < 3:
        say(f"Fewer than three sets have {min_seeds} finished seeds: too little for the spreads.")
        print("\n".join(lines))
        return
    say("")
    say(f"Spread at the end ({len(usable)} sets with at least {min_seeds} finished seeds).")
    say("  within = spread between seeds of one set (pooled); between = spread between sets (seed noise taken off);")
    say("  share = part of the total variance that the parameters explain; cv = spread between seeds over the mean (median over sets)")
    say(f"  {'descriptor':22s} {'min':>9s} {'median':>9s} {'max':>9s} {'within':>9s} {'between':>9s} {'ratio':>7s} {'share':>6s} {'cv':>7s}")
    table = {}
    for name in SCALARS:
        per_set = {s: values(s, "end", name) for s in usable}
        per_set = {s: v for s, v in per_set.items() if len(v) >= 2}
        if len(per_set) < 3:
            continue
        means = [statistics.fmean(v) for v in per_set.values()]
        within_var = statistics.fmean(statistics.variance(v) for v in per_set.values())
        mean_n = statistics.fmean(len(v) for v in per_set.values())
        between_var = max(statistics.variance(means) - within_var / mean_n, 0.0) if len(means) > 1 else 0.0
        cvs = [statistics.stdev(v) / abs(statistics.fmean(v)) for v in per_set.values() if abs(statistics.fmean(v)) > 1e-12]
        within, between = math.sqrt(within_var), math.sqrt(between_var)
        share = between_var / (between_var + within_var) if between_var + within_var > 0 else float("nan")
        table[name] = {"min": min(means), "median": statistics.median(means), "max": max(means), "within": within, "between": between,
                       "ratio": between / within if within > 0 else float("inf"), "share": share, "cv": statistics.median(cvs) if cvs else float("nan")}
        t = table[name]
        say(f"  {name:22s} {t['min']:9.4g} {t['median']:9.4g} {t['max']:9.4g} {t['within']:9.3g} {t['between']:9.3g} {t['ratio']:7.2f} {t['share']:6.2f} {t['cv']:7.3f}")

    say("")
    say("Does a small form predict the form at the end? Rank correlation of the set means, and how far the mean moves from that")
    say("stage to the end in spreads between seeds (median over sets).")
    say(f"  {'descriptor':22s} " + " ".join(f"{label:>22s}" for label in ("~1,000 cells", "~3,200 cells", "~10,000 cells")))
    prediction = {}
    for name in SCALARS:
        cells = []
        prediction[name] = {}
        for stage in (6, 7, 8):
            pairs = [(statistics.fmean(values(s, stage, name)), statistics.fmean(values(s, "end", name)), statistics.stdev(values(s, "end", name)))
                     for s in usable if len(values(s, stage, name)) >= min_seeds]
            if len(pairs) < 3:
                cells.append(f"{'':>22s}")
                continue
            rho = rank_correlation([p[0] for p in pairs], [p[1] for p in pairs])
            moved = [abs(p[1] - p[0]) / p[2] for p in pairs if p[2] > 0]
            drift = statistics.median(moved) if moved else float("nan")
            prediction[name][stage] = {"rank_correlation": rho, "drift_in_spreads": drift}
            cells.append(f"{'rho %.2f, moves %5.1f' % (rho, drift):>22s}")
        say(f"  {name:22s} " + " ".join(cells))

    # Sets whose seeds disagree most: the largest spread of a set over the typical spread, over
    # the descriptors that the parameters explain well.
    say("")
    good = [n for n in SCALARS if n in table and table[n]["share"] >= 0.8 and all(len(values(s, "end", n)) >= 2 for s in usable)]
    say("Sets whose seeds disagree most (spread of the set over the median spread of all sets; descriptors with share >= 0.8: " + ", ".join(good) + ")")
    typical = {n: statistics.median(statistics.stdev(values(s, "end", n)) for s in usable) for n in good}
    worst = []
    for s in usable:
        ratios = {n: statistics.stdev(values(s, "end", n)) / typical[n] for n in good if typical[n] > 0}
        if ratios:
            top = max(ratios, key=ratios.get)
            worst.append((ratios[top], s, top))
    worst.sort(reverse=True)
    for ratio, s, name in worst[:6]:
        v = values(s, "end", name)
        say(f"  set {s:3d}: {name} spread {ratio:4.1f} times the typical one; values {', '.join('%.3g' % x for x in sorted(v))}")
    say("")
    say("Area ratio of every set (mean over seeds, 1 = a sphere): " + ", ".join(f"{s}:{statistics.fmean(values(s, 'end', 'area_ratio')):.2f}" for s in usable))

    text = "\n".join(lines)
    print(text)
    with open(os.path.join(out_dir, "report.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    with open(os.path.join(out_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump({"runs": len(records), "sets": len(sets), "outcome": outcome, "spread": table, "prediction": prediction,
                   "disagree": [{"set": s, "descriptor": n, "times_typical": r} for r, s, n in worst[:12]]}, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output directory (created; a study can be continued in it)")
    ap.add_argument("--sets", type=int, default=48, help="parameter sets drawn besides the defaults")
    ap.add_argument("--seeds", type=int, default=8, help="seeds per parameter set (1, 2, ...)")
    ap.add_argument("--cells", type=int, default=30000, help="cell count every run grows to")
    ap.add_argument("--max-steps", type=int, default=60000, help="step budget per run; a run that needs more counts as stalled")
    ap.add_argument("--max-seconds", type=float, default=120.0, help="safety net per run, in seconds")
    ap.add_argument("--draw-seed", type=int, default=1, help="seed of the draw of the parameter sets")
    ap.add_argument("--keep", action="store_true", help="keep the state files of every run")
    ap.add_argument("--report", action="store_true", help="only print the report of what is in the log")
    args = ap.parse_args()
    if args.report:
        report(args.out)
        return
    os.makedirs(args.out, exist_ok=True)
    done = {(r["set"], r["seed"]) for r in load(args.out)}
    sets = parameter_sets(args.sets, args.draw_seed)
    with open(os.path.join(args.out, "study.json"), "w", encoding="utf-8") as f:
        json.dump({"arguments": vars(args), "code": explore.git_version(), "sets": sets}, f, indent=1)
    t0 = time.time()
    count = 0
    with open(os.path.join(args.out, "log.jsonl"), "a", encoding="utf-8") as log:
        # Seed by seed, so a study that is cut short has every set with the same seeds.
        for seed in range(1, args.seeds + 1):
            for set_id, params in enumerate(sets):
                if (set_id, seed) in done:
                    continue
                record = run_one(args.out, set_id, params, seed, args.cells, args.max_steps, args.max_seconds, args.keep)
                log.write(json.dumps(record) + "\n")
                log.flush()
                count += 1
            print(f"seed {seed}: {count} runs so far, {time.time() - t0:.0f} s", flush=True)
    print(f"{count} runs in {time.time() - t0:.0f} s")
    report(args.out)


if __name__ == "__main__":
    main()
