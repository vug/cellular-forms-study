"""The gallery of the web app (web/app): a chosen set of forms of the database with what the
app needs to grow each again (start shape, initial food, seed, parameters), a few of its
measures, and its picture in a contact sheet.

    python scripts/atlas web-gallery [--count 120]        writes web/app/gallery
    python scripts/atlas web-starts                       writes web/app/starts

The choice: every form Uğur rated 4 or 5; then, from every kind, its typical form and forms
from its middle; then the forms with the most structure, reach, patchiness and uneven growth.
Only healthy forms that were grown to the standard size. Each chosen form is grown again for
its picture. The start shapes are written as state files without food (cf_run); the app gives
the cells their first food itself, from the seed.

Only the Python standard library is used.
"""
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import explore  # noqa: E402

from . import features, space
from .commands import grown
from .runner import LOOK, Job, Runner
from .store import Store

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHOWN = ("structure", "relief_micro", "relief_meso", "relief_macro", "reach", "area_ratio", "buried_fraction", "thickness_p50", "relief_patchiness", "growth_unevenness")
TILE = "320x240"
COLUMNS = 8


def choose(store, count):
    runs = [(r, s[-1]) for r, s in grown(store) if features.healthy(s[-1]) and r["genome"]]
    by_id = {r["id"]: (r, d) for r, d in runs}
    labels = store.labels()
    chosen = []

    def take(run):
        if run in by_id and run not in chosen and len(chosen) < count:
            chosen.append(run)

    for run, label in sorted(labels.items()):
        if (label["rating"] or 0) >= 4:
            take(run)
    kinds = store.meta("kinds", [])
    per_kind = max(2, (count * 2 // 3) // max(len(kinds), 1))
    for kind in kinds:
        for run in ([kind["typical"]] + kind["middle"])[:per_kind]:
            take(run)
    rest = max(count - len(chosen), 0)
    for name in ("structure", "reach", "relief_patchiness", "growth_unevenness"):
        best = sorted(runs, key=lambda rd: -(features.raw(rd[1], name) or 0.0))
        taken = 0
        for r, _d in best:
            if taken >= rest // 4:
                break
            if r["id"] not in chosen:
                take(r["id"])
                taken += 1
    return [by_id[run] for run in chosen]


def cmd_web_gallery(args):
    store = Store(args.root)
    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    picked = choose(store, args.count)
    runner = Runner(store, photos=False)
    states = []
    for r, d in picked:
        job = Job.__new__(Job)
        job.init, job.seed, job.params, job.target = r["init"], r["seed"], r["params"], r["target_cells"]
        runner.sequence += 1
        run_dir = os.path.join(runner.work, f"g{runner.sequence:06d}")
        os.makedirs(run_dir, exist_ok=True)
        try:
            runner._simulate(job, run_dir, r["target_cells"], False)
        except RuntimeError as e:
            print("atlas:", e)
            continue
        if os.path.exists(os.path.join(run_dir, "latest.cfs")):
            states.append((r, d, run_dir))
    sheets, items = [], []
    per_sheet = 6 * COLUMNS
    kinds = {k["id"]: k for k in store.meta("kinds", [])}
    names = store.meta("kind_names", {})
    labels = store.labels()
    for first in range(0, len(states), per_sheet):
        part = states[first:first + per_sheet]
        name = f"sheet_{len(sheets)}.jpg"
        cmd = [explore.tool("cf_view")]
        for _r, _d, run_dir in part:
            cmd += ["--snapshot", os.path.join(run_dir, "latest.cfs")]
        cmd += ["--frame", "all", "--sheet", "--sheet-columns", str(COLUMNS), "--size", TILE, "--screenshot", os.path.join(out, name)] + LOOK
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise SystemExit("atlas: cf_view failed: " + (proc.stderr or proc.stdout)[-300:])
        for k, (r, d, _run_dir) in enumerate(part):
            label = labels.get(r["id"], {})
            items.append({
                "id": r["id"], "kind": r["cluster"], "seed": r["seed"], "init": r["init"]["shape"], "food": r["init"].get("food", "random"),
                "sources": sorted((r["genome"].get("sources") or {}).keys()), "params": {key: float(f"{v:.9g}") for key, v in r["params"].items()},
                "target": r["target_cells"], "cells": r["cells"], "step": r["step"],
                "metrics": {m: float(f"{features.raw(d, m):.4g}") for m in SHOWN if features.raw(d, m) is not None},
                "tile": [len(sheets), k % COLUMNS, k // COLUMNS], "rating": label.get("rating"), "category": label.get("category"),
            })
        sheets.append(name)
    for _r, _d, run_dir in states:
        shutil.rmtree(run_dir, ignore_errors=True)
    data = {"tile": [320, 240], "columns": COLUMNS, "sheets": sheets,
            "kinds": [{"id": k, "name": names.get(str(k)) or kinds[k]["name"]} for k in sorted(kinds)],
            "metrics": {m: about for m, about in features.METRICS if m in SHOWN}, "items": items}
    with open(os.path.join(out, "gallery.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))
    size = sum(os.path.getsize(os.path.join(out, n)) for n in os.listdir(out))
    print(f"{out}: {len(items)} forms in {len(sheets)} contact sheets, {size / 1e6:.1f} MB")


def cmd_web_starts(args):
    """The start shapes of the app, as state files without food and with the default parameters."""
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    shapes = [space.DEFAULT_INIT["shape"]] + sorted({i["shape"] for i in space.OTHER_INITS}) + ["icosphere:0", "icosphere:4", "ellipsoid:2:3:1:1", "noisy:3:0.3:2"]
    listed = []
    work = os.path.join(out, "_work")
    for shape in dict.fromkeys(shapes):
        shutil.rmtree(work, ignore_errors=True)
        proc = subprocess.run([explore.tool("cf_run"), "--new", shape, "--seed", "1", "--init-food", "zero", "--steps", "0", "--out", work, "--snapshot-factor", "0",
                               "--checkpoint-factor", "0"], capture_output=True, text=True)
        if proc.returncode != 0 or not os.path.exists(os.path.join(work, "latest.cfs")):
            print("atlas: could not make", shape, (proc.stderr or proc.stdout)[-200:])
            continue
        name = shape.replace(":", "_").replace(".", "p") + ".cfs"
        shutil.copy(os.path.join(work, "latest.cfs"), os.path.join(out, name))
        cells = int(explore.parse_summary(proc.stdout).get("cells", 0))
        listed.append({"shape": shape, "file": name, "cells": cells})
    shutil.rmtree(work, ignore_errors=True)
    with open(os.path.join(out, "starts.json"), "w", encoding="utf-8") as f:
        json.dump(listed, f, indent=1)
    print(f"{out}: " + ", ".join(f"{s['shape']} ({s['cells']} cells)" for s in listed))


def register(sub):
    p = sub.add_parser("web-gallery", help="write the gallery of the web app: chosen forms with their settings and pictures")
    p.add_argument("--count", type=int, default=120)
    p.add_argument("--out", default=os.path.join(REPO, "web", "app", "gallery"))
    p.set_defaults(run=cmd_web_gallery)

    p = sub.add_parser("web-starts", help="write the start shapes of the web app as state files")
    p.add_argument("--out", default=os.path.join(REPO, "web", "app", "starts"))
    p.set_defaults(run=cmd_web_starts)
