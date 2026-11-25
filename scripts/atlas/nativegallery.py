"""The gallery of the desktop and VR viewers: every form of the database that has a picture and
is not broken, with what is needed to grow it again (start shape, initial food, seed,
parameters), where it came from, a few of its measures and its tiles in the contact sheets the
atlas already has.

    python scripts/atlas native-gallery            writes runs/atlas/gallery/gallery.json

The file has the schema of the web app's gallery (webgallery.py: tile, columns, sheets, kinds,
metrics, items), plus `thumbs` (the second, smaller set of sheets) and per form `origin`,
`status`, `stopped_at` and `thumb`. The sheets are referenced by paths relative to the file;
nothing is grown again and no picture is made, so it takes seconds.

Only the Python standard library is used.
"""
import json
import os
import time

from . import features
from .store import Store
from .webgallery import SHOWN

TILE = [240, 180]   # the runner's tile sheets (runner.TILE)
THUMB = [120, 90]   # and its smaller ones
COLUMNS = 8


def build(store, out_dir):
    """The gallery document. Sheet paths are made relative to `out_dir`."""
    images = store.images()
    labels = store.labels()
    kinds = store.meta("kinds", [])
    names = store.meta("kind_names", {})
    # Only the last stage of every run (the form as pictured), in one query.
    final = {}
    for row in store.db.execute("SELECT s.run, s.descriptors FROM stages s JOIN (SELECT run, MAX(stage) AS top FROM stages GROUP BY run) m "
                                "ON s.run = m.run AND s.stage = m.top"):
        final[row["run"]] = json.loads(row["descriptors"])
    sheets = {"tile": [], "thumb": []}
    index = {"tile": {}, "thumb": {}}

    def place(view, entry):
        path, where = entry.split("#")
        if path not in index[view]:
            index[view][path] = len(sheets[view])
            sheets[view].append(os.path.relpath(os.path.join(store.root, path), out_dir).replace(os.sep, "/"))
        col, row = (int(v) for v in where.split(","))
        return [index[view][path], col, row]

    items = []
    for r in store.runs("WHERE status != 'broken' ORDER BY id"):
        img = images.get(r["id"], {})
        if "tile" not in img:
            continue
        d = final.get(r["id"], {})
        label = labels.get(r["id"], {})
        genome = r["genome"] or {}
        item = {
            "id": r["id"], "origin": r["origin"], "status": r["status"], "kind": r["cluster"], "seed": r["seed"],
            "init": r["init"]["shape"], "food": r["init"].get("food", "random"), "sources": sorted((genome.get("sources") or {}).keys()),
            "params": {key: float(f"{v:.9g}") for key, v in r["params"].items()},  # nine digits, as they are passed to the tools
            "target": r["target_cells"], "cells": r["cells"], "step": r["step"], "stopped_at": r["stopped_at"],
            "metrics": {m: float(f"{features.raw(d, m):.4g}") for m in SHOWN if features.raw(d, m) is not None},
            "tile": place("tile", img["tile"]), "rating": label.get("rating"), "category": label.get("category"),
        }
        if "thumb" in img:
            item["thumb"] = place("thumb", img["thumb"])
        items.append(item)
    return {
        "generated": time.strftime("%Y-%m-%d %H:%M"), "source": "atlas",
        "tile": TILE, "columns": COLUMNS, "sheets": sheets["tile"],
        "thumbs": {"tile": THUMB, "columns": COLUMNS, "sheets": sheets["thumb"]},
        "kinds": [{"id": k["id"], "name": names.get(str(k["id"])) or k["name"]} for k in sorted(kinds, key=lambda k: k["id"])],
        "metrics": {m: about for m, about in features.METRICS if m in SHOWN}, "items": items,
    }


def cmd_native_gallery(args):
    t0 = time.time()
    store = Store(args.root)
    out = os.path.abspath(args.out) if args.out else os.path.join(store.root, "gallery")
    os.makedirs(out, exist_ok=True)
    data = build(store, out)
    path = os.path.join(out, "gallery.json")
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))
    os.replace(path + ".tmp", path)
    statuses = {}
    for item in data["items"]:
        statuses[item["status"]] = statuses.get(item["status"], 0) + 1
    print(f"{path}: {len(data['items'])} forms (" + ", ".join(f"{k} {v}" for k, v in sorted(statuses.items())) + f"), {len(data['sheets'])} tile sheets,"
          f" {len(data['thumbs']['sheets'])} thumbnail sheets, {os.path.getsize(path) / 1e6:.1f} MB, {time.time() - t0:.1f} s")


def register(sub):
    p = sub.add_parser("native-gallery", help="write the gallery of the desktop and VR viewers: every form with a picture, from the existing sheets")
    p.add_argument("--out", default=None, help="folder of gallery.json (default <root>/gallery)")
    p.set_defaults(run=cmd_native_gallery)
