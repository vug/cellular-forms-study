"""Explore the parameter space of the Cellular Forms model (M6; docs/design.md "Finding
interesting parameters").

    python scripts/explore.py --out runs/explore/night1 --runs 150 --cells 30000

What it does, in the order of the design document:
  1. Screen: many short GPU runs (cf_gpu) to a small cell count. A run that fails a guard or
     the invariant checker, or does not reach the cell count in its time budget, is logged
     and dropped.
  2. Describe: cf_describe gives every surviving form its shape descriptors.
  3. Keep diversity: a MAP-Elites archive over two descriptors (how folded the form is, and
     how elongated). Each archive cell keeps the healthiest form that landed in it. After a
     random start, new runs are mutations of forms already in the archive.
  4. Choose by eye: a thumbnail per kept form, one atlas image laid out like the archive, and
     atlas.html with every form's parameters and the command to grow it larger.
  5. Promote:  python scripts/explore.py --out <same dir> --promote <id> --cells 2000000

Every run is one line in <out>/log.jsonl: parameters, seed, code version, outcome, descriptors.
Only the Python standard library is used. Same --seed, same code, same GPU: same search.
"""
import argparse
import html
import json
import math
import os
import random
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name: (low, high, logarithmic)
BASE_SPACE = {
    "springFactor": (0.05, 0.40, False),
    "planarFactor": (0.00, 0.40, False),
    "bulgeFactor": (0.00, 0.40, False),
    "repulsionStrength": (0.02, 0.40, True),
    "radiusOfInfluence": (1.0, 3.0, False),
    "foodMax": (0.001, 0.02, True),
    "daughterOffset": (0.05, 0.30, False),
}
# Optional food sources (M5): each is off, or on with parameters drawn from these ranges.
SOURCES = {
    "light": {"lightAmount": (0.3, 1.0, False), "lightAmbient": (0.0, 0.3, False)},
    "field": {"fieldAmount": (0.3, 1.0, False), "fieldScale": (5.0, 60.0, True), "fieldContrast": (1.0, 4.0, False), "fieldOctaves": (1.0, 3.0, False)},
    "crowd": {"crowdLow": (0.70, 0.95, False), "crowdSpan": (0.05, 0.30, False)},
    "pulse": {"pulsePeriod": (50.0, 1000.0, True), "pulseDuty": (0.2, 0.8, False), "pulseLow": (0.0, 0.5, False)},
    "diffusion": {"foodDiffusion": (0.05, 0.5, False)},
}
SOURCE_PROBABILITY = 0.25

# The archive: area ratio (1 = sphere; folded forms reach 10+) by elongation (1 = round).
BINS_AREA = [1.0, 1.5, 2.2, 3.2, 4.5, 6.0, 8.0, 11.0, 1e9]
BINS_ELONGATION = [1.0, 1.08, 1.2, 1.4, 1.8, 2.6, 1e9]


def draw(rng, lo, hi, log):
    if log:
        return math.exp(rng.uniform(math.log(lo), math.log(hi)))
    return rng.uniform(lo, hi)


def random_genome(rng):
    genome = {"base": {k: draw(rng, *v) for k, v in BASE_SPACE.items()}, "sources": {}}
    for name, space in SOURCES.items():
        if rng.random() < SOURCE_PROBABILITY:
            genome["sources"][name] = {k: draw(rng, *v) for k, v in space.items()}
    if "light" in genome["sources"]:
        genome["sources"]["light"]["direction"] = random_direction(rng)
    return genome


def random_direction(rng):
    while True:
        v = [rng.uniform(-1, 1) for _ in range(3)]
        n = math.sqrt(sum(x * x for x in v))
        if 0.2 < n <= 1.0:
            return [x / n for x in v]


def mutate_value(rng, value, lo, hi, log, strength):
    if log:
        x = math.log(value) + rng.gauss(0, strength) * (math.log(hi) - math.log(lo))
        return math.exp(min(max(x, math.log(lo)), math.log(hi)))
    return min(max(value + rng.gauss(0, strength) * (hi - lo), lo), hi)


def mutate(rng, parent, strength=0.12):
    child = json.loads(json.dumps(parent))
    for k, space in BASE_SPACE.items():
        if rng.random() < 0.5:
            child["base"][k] = mutate_value(rng, child["base"][k], *space, strength)
    for name, space in SOURCES.items():
        if name in child["sources"]:
            if rng.random() < 0.08:
                del child["sources"][name]  # switch a source off
                continue
            for k, s in space.items():
                if rng.random() < 0.5:
                    child["sources"][name][k] = mutate_value(rng, child["sources"][name][k], *s, strength)
        elif rng.random() < 0.08:
            child["sources"][name] = {k: draw(rng, *v) for k, v in space.items()}  # switch one on
            if name == "light":
                child["sources"][name]["direction"] = random_direction(rng)
    return child


def to_params(genome):
    """Genome -> the simulation's key=value parameters."""
    p = dict(genome["base"])
    # Keep the three blend factors a damped blend (docs/model.md section 2).
    total = p["springFactor"] + p["planarFactor"] + p["bulgeFactor"]
    if total > 0.9:
        for k in ("springFactor", "planarFactor", "bulgeFactor"):
            p[k] *= 0.9 / total
    for name, values in genome["sources"].items():
        if name == "light":
            p["lightAmount"] = values["lightAmount"]
            p["lightAmbient"] = values["lightAmbient"]
            p["lightDirX"], p["lightDirY"], p["lightDirZ"] = values["direction"]
        elif name == "crowd":
            p["crowdLow"] = values["crowdLow"]
            p["crowdHigh"] = values["crowdLow"] + values["crowdSpan"]
        else:
            p.update(values)
    return p


def bin_of(value, edges):
    for i in range(len(edges) - 1):
        if value < edges[i + 1]:
            return i
    return len(edges) - 2


def quality(desc, summary):
    """Higher is better: relaxed links, no self-contact, no cells hitting the displacement cap."""
    clamped = summary.get("clamped", 0) / max(summary.get("cells", 1) * max(summary.get("step", 1), 1), 1)
    return -abs(desc["link_mean"] - 1.0) - 0.5 * desc["link_std"] - 5.0 * desc["close_pairs_per_cell"] - 20.0 * clamped


def tool(name):
    path = os.path.join(REPO, "build", "release", "bin", name + ".exe")
    if not os.path.exists(path):
        path = os.path.join(REPO, "build", "release", "bin", name)
    if not os.path.exists(path):
        sys.exit(f"explore.py: {path} not found; build the release preset first")
    return path


def parse_summary(text):
    out = {}
    for line in text.splitlines():
        if "=" in line and not line.startswith("milestone"):
            k, v = line.split("=", 1)
            try:
                out[k.strip()] = float(v) if "." in v else int(v)
            except ValueError:
                out[k.strip()] = v.strip()
    return out


def run_one(out_dir, run_id, params, seed, cells, max_seconds, level=2):
    run_dir = os.path.join(out_dir, "runs", f"{run_id:05d}")
    os.makedirs(run_dir, exist_ok=True)
    cmd = [tool("cf_gpu"), "--new", f"icosphere:{level}", "--seed", str(seed), "--capacity", str(int(cells * 1.5) + 4096),
           "--until-cells", str(cells), "--max-seconds", str(max_seconds), "--out", run_dir, "--snapshot-factor", "0", "--checkpoint-factor", "0"]
    for k, v in params.items():
        cmd += ["--param", f"{k}={v:.9g}"]
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    summary = parse_summary(proc.stdout)
    record = {"id": run_id, "seed": seed, "params": params, "command": " ".join(cmd), "wall_s": round(time.time() - t0, 2),
              "exit_code": proc.returncode, "summary": {k: summary.get(k) for k in ("stop_reason", "step", "cells", "clamped", "invariants", "wall_seconds")}}
    if proc.returncode != 0 or summary.get("invariants") != "ok":
        record["status"] = "broken"
        record["detail"] = (proc.stderr or proc.stdout)[-400:]
    elif summary.get("stop_reason") != "until-cells":
        record["status"] = "stalled" if summary.get("stop_reason") == "time-budget" else str(summary.get("stop_reason"))
    else:
        described = subprocess.run([tool("cf_describe"), os.path.join(run_dir, "latest.cfs")], capture_output=True, text=True)
        if described.returncode != 0:
            record["status"] = "broken"
            record["detail"] = described.stderr[-400:]
        else:
            record["status"] = "ok"
            record["descriptors"] = json.loads(described.stdout)
    return record, run_dir


def git_version():
    try:
        return subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        return "unknown"


def write_atlas(out_dir, archive, cells):
    """Thumbnails, the atlas image and atlas.html for the forms in the archive."""
    thumbs = os.path.join(out_dir, "thumbs")
    os.makedirs(thumbs, exist_ok=True)
    view = tool("cf_view")
    rows, cols = len(BINS_AREA) - 1, len(BINS_ELONGATION) - 1
    tiles = []
    for a in range(rows):
        for e in range(cols):
            entry = archive.get((a, e))
            tiles += ["--snapshot", os.path.join(entry["run_dir"], "latest.cfs") if entry else "-"]
            if entry:
                subprocess.run([view, "--snapshot", os.path.join(entry["run_dir"], "latest.cfs"), "--screenshot",
                                os.path.join(thumbs, f"{entry['id']:05d}.png"), "--size", "420x420", "--color", "uniform"], capture_output=True)
    frames = ",".join(str(i) for i in range(rows * cols))
    subprocess.run([view] + tiles + ["--frame", frames, "--sheet", "--sheet-columns", str(cols), "--screenshot", os.path.join(out_dir, "atlas.png"),
                    "--size", "300x300", "--color", "uniform"], capture_output=True)

    def cell_html(entry):
        if not entry:
            return "<td class='empty'></td>"
        d = entry["descriptors"]
        params = " ".join(f"{k}={v:.4g}" for k, v in entry["params"].items())
        promote = f"python scripts/explore.py --out {os.path.relpath(out_dir, REPO)} --promote {entry['id']} --cells 2000000"
        return ("<td><img src='thumbs/%05d.png' loading='lazy'><div class='id'>#%d</div>"
                "<div class='d'>area ratio %.2f, elongation %.2f, box dim %.2f, links %.2f</div>"
                "<details><summary>parameters</summary><code>%s</code><p>seed %d</p><p>grow it larger:</p><code>%s</code></details></td>") % (
            entry["id"], entry["id"], d["area_ratio"], d["elongation"], d["box_dimension"], d["link_mean"], html.escape(params), entry["seed"], html.escape(promote))

    body = []
    for a in range(rows):
        label = f"area ratio {BINS_AREA[a]:g}+" if a == rows - 1 else f"area ratio {BINS_AREA[a]:g} to {BINS_AREA[a + 1]:g}"
        body.append("<tr><th>%s</th>%s</tr>" % (label, "".join(cell_html(archive.get((a, e))) for e in range(cols))))
    head = "".join("<th>elongation %s</th>" % (f"{BINS_ELONGATION[e]:g}+" if e == cols - 1 else f"{BINS_ELONGATION[e]:g} to {BINS_ELONGATION[e + 1]:g}") for e in range(cols))
    page = f"""<!doctype html><html lang="en"><head><meta charset="utf-8"><title>Cellular Forms atlas</title>
<style>
body {{ font: 13px/1.4 system-ui, sans-serif; background: #17191e; color: #e6e3dc; margin: 16px; }}
table {{ border-collapse: collapse; }} th {{ font-weight: 600; color: #9aa0a8; padding: 6px; text-align: left; }}
td {{ border: 1px solid #2c313a; padding: 6px; vertical-align: top; width: 220px; }} td.empty {{ background: #14161a; }}
img {{ width: 208px; height: 208px; display: block; border-radius: 4px; }} .id {{ color: #e0b060; }} .d {{ color: #b9bec6; }}
code {{ display: block; white-space: pre-wrap; overflow-wrap: anywhere; font: 11px ui-monospace, Consolas, monospace; color: #cfd3da; }}
</style></head><body>
<h1>Cellular Forms atlas</h1>
<p>{len(archive)} forms kept, {cells} cells each. Rows: how folded (surface area relative to a sphere of the same volume).
Columns: how elongated. Each cell of the table keeps the healthiest form found there (links nearest to rest length, no self-contact).</p>
<table><tr><th></th>{head}</tr>{''.join(body)}</table></body></html>"""
    with open(os.path.join(out_dir, "atlas.html"), "w", encoding="utf-8") as f:
        f.write(page)


def load_log(out_dir):
    path = os.path.join(out_dir, "log.jsonl")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def promote(out_dir, run_id, cells, max_seconds):
    records = [r for r in load_log(out_dir) if r["id"] == run_id]
    if not records:
        sys.exit(f"explore.py: no run {run_id} in {out_dir}/log.jsonl")
    r = records[0]
    target = os.path.join(out_dir, "promoted", f"{run_id:05d}_{cells}")
    cmd = [tool("cf_gpu"), "--new", "icosphere:2", "--seed", str(r["seed"]), "--capacity", str(int(cells * 1.1) + 4096), "--until-cells", str(cells),
           "--max-seconds", str(max_seconds), "--out", target]
    for k, v in r["params"].items():
        cmd += ["--param", f"{k}={v:.9g}"]
    print(" ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    print(proc.stdout[-900:])
    subprocess.run([tool("cf_view"), "--run", target, "--frame", "-1", "--screenshot", os.path.join(target, "final.png"), "--size", "1400x1100", "--color", "uniform"])
    return proc.returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output directory (created; a search can be continued in it)")
    ap.add_argument("--runs", type=int, default=100, help="number of runs to add")
    ap.add_argument("--cells", type=int, default=30000, help="cell count every run grows to")
    ap.add_argument("--max-seconds", type=float, default=12.0, help="time budget per run")
    ap.add_argument("--seed", type=int, default=1, help="seed of the search itself")
    ap.add_argument("--random-start", type=int, default=40, help="random runs before mutation starts")
    ap.add_argument("--promote", type=int, help="grow run <id> to --cells instead of searching")
    args = ap.parse_args()
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    if args.promote is not None:
        sys.exit(promote(out_dir, args.promote, args.cells, max(args.max_seconds, 3600.0)))

    # Rebuild the archive from an existing log, so a search can be continued.
    log = load_log(out_dir)
    archive = {}
    for r in log:
        if r.get("status") == "ok" and r.get("kept"):
            d = r["descriptors"]
            key = (bin_of(d["area_ratio"], BINS_AREA), bin_of(d["elongation"], BINS_ELONGATION))
            if key not in archive or r["quality"] > archive[key]["quality"]:
                archive[key] = {**r, "run_dir": os.path.join(out_dir, "runs", f"{r['id']:05d}")}
    next_id = max((r["id"] for r in log), default=-1) + 1
    rng = random.Random(args.seed * 1000003 + next_id)
    version = git_version()
    counts = {}
    t_start = time.time()
    with open(os.path.join(out_dir, "log.jsonl"), "a", encoding="utf-8") as logfile:
        for k in range(args.runs):
            run_id = next_id + k
            if run_id < args.random_start or not archive:
                genome, parent = random_genome(rng), None
            else:
                parent_entry = rng.choice(sorted(archive.values(), key=lambda e: e["id"]))
                genome, parent = mutate(rng, parent_entry["genome"]), parent_entry["id"]
            record, run_dir = run_one(out_dir, run_id, to_params(genome), seed=1 + run_id, cells=args.cells, max_seconds=args.max_seconds)
            record.update({"genome": genome, "parent": parent, "code": version, "kept": False})
            counts[record["status"]] = counts.get(record["status"], 0) + 1
            keep = False
            if record["status"] == "ok":
                d = record["descriptors"]
                record["quality"] = quality(d, record["summary"])
                key = (bin_of(d["area_ratio"], BINS_AREA), bin_of(d["elongation"], BINS_ELONGATION))
                record["bin"] = list(key)
                old = archive.get(key)
                if old is None or record["quality"] > old["quality"]:
                    keep = True
                    if old is not None:
                        shutil.rmtree(old["run_dir"], ignore_errors=True)  # replaced: its files are no longer needed
                    archive[key] = {**record, "run_dir": run_dir}
            record["kept"] = keep
            if not keep:
                shutil.rmtree(run_dir, ignore_errors=True)
            logfile.write(json.dumps(record) + "\n")
            logfile.flush()
            if (k + 1) % 10 == 0 or k + 1 == args.runs:
                print(f"{k + 1}/{args.runs} runs, {len(archive)} archive cells filled, outcomes {counts}, {time.time() - t_start:.0f} s", flush=True)

    write_atlas(out_dir, archive, args.cells)
    summary = {"runs_total": next_id + args.runs, "archive_cells_filled": len(archive), "archive_cells": (len(BINS_AREA) - 1) * (len(BINS_ELONGATION) - 1),
               "outcomes_this_session": counts, "code": version, "cells": args.cells}
    with open(os.path.join(out_dir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps(summary))
    print("atlas:", os.path.join(out_dir, "atlas.png"), "and", os.path.join(out_dir, "atlas.html"))


if __name__ == "__main__":
    main()
