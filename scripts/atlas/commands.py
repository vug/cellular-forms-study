"""The later commands of the atlas: features, early stop, kinds (search, export and serve are
registered from their own modules)."""
import json
import math
import os

from . import features, kinds, stop
from .store import Store


STANDARD_TARGET = 31623  # the size at which forms are compared (stage 18)


def grown(store, origins=None, target=STANDARD_TARGET):
    """Runs that were grown to their target without a stop rule: [(run dict, stages)]. Only
    runs with the standard target, unless another (or None for any) is given: the features
    of a form move with its size, so forms of different sizes are not compared."""
    stages = store.all_stages()
    out = []
    for r in store.runs("WHERE status = 'ok' AND stopped_at IS NULL ORDER BY id"):
        if origins and r["origin"] not in origins:
            continue
        if target is not None and r["target_cells"] != target:
            continue
        s = stages.get(r["id"], [])
        if s and s[-1]["cells"] >= 0.9 * r["target_cells"]:
            out.append((r, s))
    return out


def cmd_features(args):
    """Measures the two spreads of every feature on the database: between seeds (from the pairs
    of runs that differ only in their seed) and between forms."""
    store = Store(args.root)
    runs = grown(store)
    by_parameters = {}
    for r, s in runs:
        by_parameters.setdefault(json.dumps([r["init"], r["params"]], sort_keys=True), []).append(features.vector(s[-1]))
    finals = [group[0] for group in by_parameters.values()]
    pairs = [(group[i], group[j]) for group in by_parameters.values() for i in range(len(group)) for j in range(i + 1, len(group))]
    scales = features.measure(finals, pairs)
    scales.save(store)
    print(f"{len(finals)} parameter sets, {len(pairs)} pairs that differ only in their seed")
    print(f"  {'feature':20s} {'median':>9s} {'between forms':>14s} {'between seeds':>14s} {'ratio':>7s}")
    for k, (name, kind, _bound) in enumerate(features.FEATURES):
        ratio = scales.population[k] / scales.seed[k] if scales.seed[k] > 0 else float("inf")
        print(f"  {name:20s} {scales.centre[k]:9.3f} {scales.population[k]:14.3f} {scales.seed[k]:14.4f} {ratio:7.1f}   ({kind} scale)")
    # How far apart two seeds of the same parameters are, against two different forms.
    same = sorted(scales.distance(a, b) for a, b in pairs)
    import random
    rng = random.Random(1)
    other = sorted(scales.distance(rng.choice(finals), rng.choice(finals)) for _ in range(2000))
    if same:
        q = lambda v, p: v[int(p * (len(v) - 1))]  # noqa: E731
        print(f"distance between two seeds of one parameter set: median {q(same, 0.5):.2f}, 90 % below {q(same, 0.9):.2f}")
        print(f"distance between two forms of the database:      median {q(other, 0.5):.2f}, 10 % below {q(other, 0.1):.2f}")
        store.set_meta("distance_scale", {"same_median": q(same, 0.5), "same_p90": q(same, 0.9), "other_median": q(other, 0.5), "other_p10": q(other, 0.1)})


def cmd_stop_fit(args):
    store = Store(args.root)
    runs = [(r["id"], s) for r, s in grown(store, {"survey"} if args.survey_only else None)]
    rule, report = stop.fit_and_test(runs, args.structure, args.miss)
    store.set_meta("stop_rule", rule.to_json())
    store.set_meta("stop_report", report)
    print(f"{report['runs']} runs grown to the end; a form counts as having structure from structure = {report['structure_threshold']:.4f}:"
          f" {report['structured']} have it, {report['dull']} are dull")
    print(f"On runs the rule was not fitted on (five folds), allowing {100 * report['miss_asked']:.1f} % of the forms with structure to be stopped:")
    for s in stop.DECISION_STAGES:
        c = report["stopped"][str(s)]
        print(f"  at {10 ** (s / 4):6.0f} cells: stops {c['dull']:5d} dull forms ({100 * c['dull'] / max(report['dull'], 1):5.1f} %) and {c['structured']:3d} with structure"
              f" ({100 * c['structured'] / max(report['structured'], 1):4.1f} %)")
    print(f"  in all: {100 * report['dull_stopped_share']:.1f} % of the dull forms stopped, {100 * report['structured_stopped_share']:.1f} % of the forms with structure"
          f" stopped by mistake, {100 * report['work_saved_share']:.1f} % of the simulation work saved")
    for s, weights in report["weights"].items():
        strongest = sorted(((abs(w), n, w) for n, w in weights.items() if n != "constant"), reverse=True)[:5]
        print(f"  what counts at stage {s}: " + ", ".join(f"{n} {w:+.2f}" for _a, n, w in strongest))
    # The second test: forms that have stopped changing in kind.
    settled_at, drift = {s: 0 for s in stop.DECISION_STAGES}, []
    for _id, s in runs:
        by_stage = {d["stage"]: d for d in s}
        for stage in stop.DECISION_STAGES:
            if stop.settled(by_stage, stage):
                settled_at[stage] += 1
                if stage == 16:
                    drift.append(abs(math.log(max(s[-1]["structure"], 1e-4) / max(by_stage[16]["structure"], 1e-4))))
    print("Forms whose scale-free numbers changed by less than a tenth over the last half decade: " +
          ", ".join(f"{settled_at[s]} at {10 ** (s / 4):.0f} cells" for s in stop.DECISION_STAGES) +
          (f"; their structure at the end differs from that at 10,000 cells by a factor {math.exp(sorted(drift)[len(drift) // 2]):.2f} in the median" if drift else ""))
    store.set_meta("settled_report", {"settled": {str(k): v for k, v in settled_at.items()}, "runs": len(runs),
                                      "median_factor_after_10000": math.exp(sorted(drift)[len(drift) // 2]) if drift else None})


def cmd_kinds(args):
    store = Store(args.root)
    scales = features.Scales.load(store)
    if scales is None:
        raise SystemExit("atlas: run 'features' first")
    runs = grown(store)
    ids = [r["id"] for r, _s in runs]
    points = [scales.standard(features.vector(s[-1])) for _r, s in runs]
    if args.assign:
        # Forms that came after the sorting go to the kind whose centre is nearest; the kinds
        # themselves, their names and the map stay as they are.
        found, report = store.meta("kinds"), store.meta("kinds_report")
        if not found or "map_axes" not in (report or {}):
            raise SystemExit("atlas: sort the forms first (kinds without --assign)")
        placed = 0
        for (r, _s), point in zip(runs, points):
            if r["cluster"] is None:
                kind, (x, y) = kinds.place(point, found, report)
                store.update_run(r["id"], cluster=kind, map_x=x, map_y=y)
                placed += 1
        store.commit()
        counts = {}
        for row in store.db.execute("SELECT cluster, COUNT(*) AS n FROM runs WHERE cluster IS NOT NULL GROUP BY cluster"):
            counts[row["cluster"]] = row["n"]
        for kind in found:
            kind["count"] = counts.get(kind["id"], 0)
        store.set_meta("kinds", found)
        print(f"{placed} forms placed in the {len(found)} kinds there are")
        return
    assignment, positions, found, report = kinds.sort_into_kinds(ids, points, args.count)
    store.db.execute("UPDATE runs SET cluster = NULL, map_x = NULL, map_y = NULL")
    for run in ids:
        store.update_run(run, cluster=assignment[run], map_x=positions[run][0], map_y=positions[run][1])
    store.commit()
    store.set_meta("kinds", found)
    store.set_meta("kinds_report", report)
    print(f"{report['forms']} forms in {report['kinds']} kinds" + (f" (how well separated, by number of kinds: {report['silhouette_by_count']})" if report["silhouette_by_count"] else ""))
    print(f"the map shows {100 * sum(report['map_shares']):.0f} % of the spread; across: {report['map_directions'][0]}; up: {report['map_directions'][1]}")
    for kind in found:
        print(f"  kind {kind['id']:2d}: {kind['count']:5d} forms, typical run {kind['typical']:5d}: {kind['name']}")


def long_rows(store, target):
    """Every form that was grown on to `target` (origin "long"), with the form it came from:
    [{run, long_run, structure_small, structure_large or None, status, cells}]."""
    stages = store.all_stages()
    by_id = {r["id"]: r for r in store.runs("")}
    rows = []
    for big in by_id.values():
        if big["origin"] != "long" or big["target_cells"] != target or big["parent"] not in by_id:
            continue
        small = stages.get(big["parent"], [])
        s_big = stages.get(big["id"], [])
        reached = big["status"] == "ok" and s_big and s_big[-1]["cells"] >= 0.9 * target
        rows.append({"run": big["parent"], "long_run": big["id"], "structure_small": small[-1]["structure"] if small else None,
                     "structure_large": s_big[-1]["structure"] if reached else None, "status": big["status"], "cells": big["cells"]})
    return rows


def cmd_long_check(args):
    """Do dull forms stay dull when they are grown ten times larger? Grows forms of the survey on
    to --target: either --count forms across the whole range of structure (two thirds of them
    below the threshold), or --dull forms drawn at random from the dull ones. Then reports on
    every form that was ever grown on."""
    import random
    from .runner import Job, Runner
    store = Store(args.root)
    rule = store.meta("stop_rule")
    if not rule:
        raise SystemExit("atlas: run stop-fit first")
    threshold = rule["structure_threshold"]
    runs = sorted(((s[-1]["structure"], r) for r, s in grown(store, {"survey"}) if r["seed"] == 1), key=lambda x: x[0])
    dull = [x for x in runs if x[0] < threshold]
    structured = [x for x in runs if x[0] >= threshold]

    def spread(items, count):
        return [items[int(k * (len(items) - 1) / max(count - 1, 1))] for k in range(min(count, len(items)))]

    if args.dull:
        picked = random.Random(args.seed).sample(dull, min(args.dull, len(dull)))
    else:
        picked = spread(dull, args.count * 2 // 3) + spread(structured, args.count - args.count * 2 // 3)
    jobs = [Job(r["genome"], seed=r["seed"], origin="long", parent=r["id"], target=args.target) for _structure, r in picked]
    Runner(store, max_steps=250000, max_seconds=900.0).run_many(jobs)
    rows = long_rows(store, args.target)
    done = [x for x in rows if x["structure_large"] is not None]
    was_dull = [x for x in done if x["structure_small"] < threshold]
    bloomed = [x for x in was_dull if x["structure_large"] >= threshold]
    was_structured = [x for x in done if x["structure_small"] >= threshold]
    faded = [x for x in was_structured if x["structure_large"] < threshold]
    factors = sorted(x["structure_large"] / max(x["structure_small"], 1e-4) for x in done)
    report = {"target": args.target, "threshold": threshold, "picked": len(rows), "reached": len(done), "dull": len(was_dull), "bloomed": len(bloomed),
              "structured": len(was_structured), "faded": len(faded), "median_factor": factors[len(factors) // 2] if factors else None, "rows": rows}
    store.set_meta("long_check", report)
    print(f"{len(rows)} forms have been grown on to {args.target} cells, {len(done)} reached it")
    print(f"  dull at the small size (structure below {threshold:.4f}): {len(was_dull)}; of those, with structure at the large size: {len(bloomed)}")
    print(f"  with structure at the small size: {len(was_structured)}; of those, dull at the large size: {len(faded)}")
    if factors:
        print(f"  structure at the large size over structure at the small size: median {factors[len(factors) // 2]:.2f}, from {factors[0]:.2f} to {factors[-1]:.2f}")


def later_examples(store):
    """The dull forms that were grown ten times larger, as examples for the model of which dull
    forms fold later: (inputs at the standard size, folded later or not, run id)."""
    report = store.meta("long_check")
    if not report:
        return [], None
    threshold = report["threshold"]
    stages = store.all_stages()
    out = []
    for x in long_rows(store, report["target"]):
        if x["structure_large"] is None or x["structure_small"] is None or x["structure_small"] >= threshold:
            continue
        row = stop.inputs({d["stage"]: d for d in stages.get(x["run"], [])}, 18)
        if row is not None:
            out.append((row, 1.0 if x["structure_large"] >= threshold else 0.0, x["run"]))
    return out, report


def cmd_later_fit(args):
    """Which dull forms are worth growing on? Fits, on the dull forms that were grown ten times
    larger (long-check), the chance that a form that is dull at the standard size has structure
    at the large one, from what it looks like at the standard size; tests it on held-out forms."""
    import random
    store = Store(args.root)
    examples, report = later_examples(store)
    if len(examples) < 40:
        raise SystemExit(f"atlas: only {len(examples)} dull forms were grown on (long-check); too few to fit")
    rows = [e[0] for e in examples]
    labels = [e[1] for e in examples]
    order = list(range(len(rows)))
    random.Random(1).shuffle(order)
    folds = 5
    right = missed = false_alarm = 0
    for fold in range(folds):
        held = set(order[fold::folds])
        model = stop.fit_logistic([rows[i] for i in order if i not in held], [labels[i] for i in order if i not in held], ridge=2.0)
        for i in held:
            says = stop.chance(model, rows[i]) >= 0.5
            right += says == (labels[i] > 0.5)
            missed += (not says) and labels[i] > 0.5
            false_alarm += says and labels[i] < 0.5
    model = stop.fit_logistic(rows, labels, ridge=2.0)
    bloomers = int(sum(labels))
    weights = dict(zip(["constant"] + stop.INPUTS, [round(w, 3) for w in model["weights"]]))
    result = {"model": model, "inputs": stop.INPUTS, "forms": len(rows), "bloomers": bloomers, "right": right, "missed": missed, "false_alarm": false_alarm,
              "target": report["target"], "threshold": report["threshold"], "weights": weights}
    store.set_meta("later_rule", result)
    print(f"{len(rows)} forms that are dull at the standard size were grown on to {report['target']} cells; {bloomers} of them have structure there")
    print(f"  on held-out forms the model is right for {right} of {len(rows)}: it misses {missed} of the {bloomers} that fold later and expects folds of {false_alarm} that stay dull")
    strongest = sorted(((abs(w), n, w) for n, w in weights.items() if n != "constant"), reverse=True)[:5]
    print("  what counts: " + ", ".join(f"{n} {w:+.2f}" for _a, n, w in strongest))


def cmd_inits(args):
    """Does the start shape matter? Grows parameter sets of the survey again from each of the
    other start shapes and measures how far the form ends up from the one grown from the usual
    icosphere, against how far two seeds end up from each other."""
    import random
    from . import space
    from .runner import Job, Runner
    store = Store(args.root)
    scales = features.Scales.load(store)
    if scales is None:
        raise SystemExit("atlas: run 'features' first")
    rng = random.Random(args.seed)
    usual = [(r, s) for r, s in grown(store, {"survey"}) if r["init"] == space.DEFAULT_INIT and r["seed"] == 1]
    picked = rng.sample(usual, min(args.sets, len(usual)))
    jobs, meaning = [], []
    for r, _s in picked:
        for init in space.OTHER_INITS:
            genome = dict(r["genome"], init=dict(init))
            jobs.append(Job(genome, seed=1, origin="inits", parent=r["id"], target=r["target_cells"]))
            meaning.append((r["id"], init))
        jobs.append(Job(r["genome"], seed=2, origin="inits", parent=r["id"], target=r["target_cells"]))  # the yardstick: another seed
        meaning.append((r["id"], "seed"))
    ids = Runner(store).run_many(jobs)
    final = {r["id"]: features.vector(s[-1]) for r, s in picked}
    by_init = {}
    for (parent, init), run in zip(meaning, ids):
        stages = store.stages(run) if run is not None else []
        r = store.runs("WHERE id = ?", (run,))[0] if run is not None else None
        if not r or r["status"] != "ok" or not stages:
            by_init.setdefault("did not finish", []).append(None)
            continue
        name = "another seed" if init == "seed" else init["shape"] + ("" if init.get("food", "random") == "random" else ", no food at the start")
        by_init.setdefault(name, []).append(scales.distance(final[parent], features.vector(stages[-1])))
    print(f"{len(picked)} parameter sets; distance of the form from the one grown from icosphere:2 with seed 1 (median, 90 % below)")
    report = {}
    for name, d in sorted(by_init.items(), key=lambda x: x[0] != "another seed"):
        d = sorted(x for x in d if x is not None)
        if d:
            report[name] = {"runs": len(d), "median": d[len(d) // 2], "p90": d[int(0.9 * (len(d) - 1))]}
            print(f"  {name:36s} {len(d):3d} runs  median {d[len(d) // 2]:5.2f}   90 % below {d[int(0.9 * (len(d) - 1))]:5.2f}")
    store.set_meta("inits_report", report)


def cmd_import_labels(args):
    """Takes ratings, categories, notes and kind names that were entered on the published page
    into the database. --dir is a folder with labels/<run id>.json and kinds/<kind id>.json, as
    a read of the page's store saves them."""
    store = Store(args.root)

    def fields(path):
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        for key in ("data", "fields", "value"):  # the document itself, or wrapped
            if isinstance(doc.get(key), dict) and not any(k in doc for k in ("rating", "category", "note", "name")):
                doc = doc[key]
        return doc

    labels = names = 0
    folder = os.path.join(args.dir, "labels")
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if name.endswith(".json") and name[:-5].isdigit():
            doc = fields(os.path.join(folder, name))
            rating = doc.get("rating")
            store.set_label(int(name[:-5]), rating=None if rating is None else int(rating), category=doc.get("category"), note=doc.get("note"))
            labels += 1
    folder = os.path.join(args.dir, "kinds")
    given = store.meta("kind_names", {})
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if name.endswith(".json") and name[:-5].isdigit():
            doc = fields(os.path.join(folder, name))
            if doc.get("name") is not None:
                given[name[:-5]] = str(doc["name"])[:80]
                names += 1
    store.set_meta("kind_names", given)
    print(f"{labels} ratings and {names} kind names taken into {store.root}")


def register(sub):
    p = sub.add_parser("import-labels", help="take ratings and kind names entered on the published page into the database")
    p.add_argument("--dir", required=True)
    p.set_defaults(run=cmd_import_labels)

    p = sub.add_parser("inits", help="grow parameter sets of the survey from other start shapes: does the start matter?")
    p.add_argument("--sets", type=int, default=24)
    p.add_argument("--seed", type=int, default=1)
    p.set_defaults(run=cmd_inits)

    p = sub.add_parser("long-check", help="grow forms of the survey ten times larger: do dull forms stay dull?")
    p.add_argument("--count", type=int, default=60, help="forms across the whole range of structure")
    p.add_argument("--dull", type=int, default=0, help="instead: this many of the dull forms, drawn at random")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--target", type=int, default=316228)
    p.set_defaults(run=cmd_long_check)

    p = sub.add_parser("later-fit", help="fit and test which dull forms will have structure when grown ten times larger")
    p.set_defaults(run=cmd_later_fit)

    p = sub.add_parser("features", help="measure the spread between seeds and between forms of every feature")
    p.set_defaults(run=cmd_features)

    p = sub.add_parser("stop-fit", help="fit the early-stop rule on the database and test it on held-out runs")
    p.add_argument("--structure", type=float, default=None, help="structure from which a form counts as having structure (default: the best split of the database)")
    p.add_argument("--miss", type=float, default=0.02, help="share of the forms with structure that may be stopped by mistake")
    p.add_argument("--survey-only", action="store_true", help="fit on the survey alone (not on runs a search made)")
    p.set_defaults(run=cmd_stop_fit)

    p = sub.add_parser("kinds", help="sort the forms into kinds and lay out the map")
    p.add_argument("--count", type=int, default=None, help="number of kinds (default: the best separated between 4 and 12)")
    p.add_argument("--assign", action="store_true", help="keep the kinds there are and place the forms that came since in the nearest one")
    p.set_defaults(run=cmd_kinds)

    for name in ("search", "export", "serve", "webgallery", "nativegallery"):
        try:
            module = __import__("atlas." + name, fromlist=["register"])
        except ImportError:
            continue
        module.register(sub)
