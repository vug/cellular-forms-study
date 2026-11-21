"""What the page shows: every run of the database as one JSON document (`build`), and a folder
with that document, the page and the contact sheets, ready to be published (`export`).

Only the Python standard library is used.
"""
import json
import os
import shutil
import time

from . import features, stop
from .store import Store

PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "page", "index.html")
TRAJECTORY = ("structure", "relief_micro", "relief_meso", "relief_macro", "area_ratio", "buried_fraction")


def _round(x, digits=4):
    if x is None or isinstance(x, (str, bool)):
        return x
    if isinstance(x, int):
        return x
    if x != x or x in (float("inf"), float("-inf")):
        return None
    return float(f"{x:.{digits}g}")


def build(store):
    scales = features.Scales.load(store)
    rule = stop.StopRule.from_json(store.meta("stop_rule"))
    later = store.meta("later_rule")  # which dull forms have structure when grown ten times larger
    long_check = store.meta("long_check")
    stages = store.all_stages()
    images = store.images()
    labels = store.labels()
    kinds = store.meta("kinds", [])
    kind_names = store.meta("kind_names", {})
    sheets = {"tile": [], "thumb": []}
    sheet_index = {"tile": {}, "thumb": {}}

    def place(kind, entry):
        path, where = entry.split("#")
        if path not in sheet_index[kind]:
            sheet_index[kind][path] = len(sheets[kind])
            sheets[kind].append(path)
        col, row = (int(v) for v in where.split(","))
        return [sheet_index[kind][path], col, row]

    runs, parameter_names = [], []
    for r in store.runs("ORDER BY id"):
        s = stages.get(r["id"], [])
        final = s[-1] if s else {}
        by_stage = {d["stage"]: d for d in s}
        for name in r["params"]:
            if name not in parameter_names:
                parameter_names.append(name)
        img = images.get(r["id"], {})
        tile = place("tile", img["tile"]) if "tile" in img else None
        thumb = place("thumb", img["thumb"]) if "thumb" in img else None
        chance = {}
        if rule is not None:
            for stage in rule.decision_stages():
                verdict = rule.decide(s, stage) if stage in by_stage else None
                if verdict and verdict["expected"] is not None:
                    chance[str(stage)] = _round(verdict["expected"], 3)
        label = labels.get(r["id"], {})
        metrics = {name: _round(features.raw(final, name)) for name, _about in features.METRICS}
        metrics["folds_later"] = None
        if later and r["status"] == "ok" and r["target_cells"] == 31623 and final.get("structure", 1.0) < later["threshold"]:
            row = stop.inputs(by_stage, 18)
            if row is not None:
                metrics["folds_later"] = _round(stop.chance(later["model"], row), 3)
        genome = r["genome"] or {}
        runs.append({
            "id": r["id"], "status": r["status"], "origin": r["origin"], "parent": r["parent"], "parent2": r["parent2"], "seed": r["seed"],
            "init": r["init"]["shape"], "food": r["init"].get("food", "random"), "sources": sorted((genome.get("sources") or {}).keys()),
            "params": {k: _round(v, 9) for k, v in r["params"].items()},  # nine digits, as they are passed to the tools
            "cells": r["cells"], "step": r["step"], "target": r["target_cells"], "wall": _round(r["wall_s"], 3), "stopped_at": r["stopped_at"],
            "predicted": _round(r["predicted"], 3), "kind": r["cluster"], "x": _round(r["map_x"], 4), "y": _round(r["map_y"], 4),
            "m": metrics,
            "z": [_round(v, 3) for v in scales.standard(features.vector(final))] if scales is not None and final else None,
            "relief": {"sigma": final.get("relief_sigma", []), "value": [_round(v) for v in final.get("relief", [])],
                       "skew": [_round(v, 3) for v in final.get("relief_skew", [])], "rest": _round(final.get("relief_rest"))},
            "lobes": [_round(v, 3) for v in final.get("lobes", [])],
            "traj": dict({"cells": [d["cells"] for d in s], "step": [d["step"] for d in s], "stage": [d["stage"] for d in s], "chance": chance},
                         **{name: [_round(d.get(name)) for d in s] for name in TRAJECTORY}),
            "img": {v: img[v] for v in ("a", "b") if v in img}, "tile": tile, "thumb": thumb,
            "rating": label.get("rating"), "category": label.get("category"), "note": label.get("note"),
            "healthy": features.healthy(final) if final else None,
        })
    for kind in kinds:
        kind["given"] = kind_names.get(str(kind["id"]))
    return {
        "generated": time.strftime("%Y-%m-%d %H:%M"),
        "metrics": [{"key": k, "about": about} for k, about in features.METRICS] +
                   [{"key": "folds_later", "about": "for a form that is dull at this size: the chance that it has structure when grown ten times larger"}],
        "features": features.NAMES,
        "params": parameter_names, "trajectory": list(TRAJECTORY), "runs": runs, "kinds": kinds,
        "sheets": {"files": sheets["tile"], "tile": [240, 180], "columns": 8},
        "thumbs": {"files": sheets["thumb"], "tile": [120, 90], "columns": 8},
        "stop": {"report": store.meta("stop_report"), "settled": store.meta("settled_report"), "decision_stages": list(stop.DECISION_STAGES),
                 "long": {k: v for k, v in long_check.items() if k != "rows"} if long_check else None,
                 "later": {k: v for k, v in later.items() if k != "model"} if later else None},
        "scales": store.meta("feature_scales"), "distance": store.meta("distance_scale"), "kinds_report": store.meta("kinds_report"),
    }


def cmd_export(args):
    """Writes <root>/publish: index.html, data.json and the contact sheets."""
    store = Store(args.root)
    out = os.path.join(store.root, "publish")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "img"))
    data = build(store)
    for r in data["runs"]:
        r["img"] = {}  # the published page shows the tiles only
    with open(os.path.join(out, "data.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))
    total = os.path.getsize(os.path.join(out, "data.json"))
    for path in data["sheets"]["files"] + data["thumbs"]["files"]:
        shutil.copy(os.path.join(store.root, path), os.path.join(out, path))
        total += os.path.getsize(os.path.join(out, path))
    if os.path.exists(PAGE):
        shutil.copy(PAGE, os.path.join(out, "index.html"))
    print(f"{out}: {len(data['runs'])} runs, {len(data['sheets']['files'])} + {len(data['thumbs']['files'])} contact sheets, {total / 1e6:.1f} MB")


def register(sub):
    p = sub.add_parser("export", help="write the folder that is published: the page, its data and the contact sheets")
    p.set_defaults(run=cmd_export)
