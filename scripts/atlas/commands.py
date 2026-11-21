"""The later commands of the atlas: features, early stop, kinds (search, export and serve are
registered from their own modules)."""
import json
import math
import os

from . import features, kinds, stop
from .store import Store


def grown(store, origins=None):
    """Runs that were grown to their target without a stop rule: [(run dict, stages)]."""
    stages = store.all_stages()
    out = []
    for r in store.runs("WHERE status = 'ok' AND stopped_at IS NULL ORDER BY id"):
        if origins and r["origin"] not in origins:
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


def register(sub):
    p = sub.add_parser("features", help="measure the spread between seeds and between forms of every feature")
    p.set_defaults(run=cmd_features)

    p = sub.add_parser("stop-fit", help="fit the early-stop rule on the database and test it on held-out runs")
    p.add_argument("--structure", type=float, default=None, help="structure from which a form counts as having structure (default: the best split of the database)")
    p.add_argument("--miss", type=float, default=0.02, help="share of the forms with structure that may be stopped by mistake")
    p.add_argument("--survey-only", action="store_true", help="fit on the survey alone (not on runs a search made)")
    p.set_defaults(run=cmd_stop_fit)

    p = sub.add_parser("kinds", help="sort the forms into kinds and lay out the map")
    p.add_argument("--count", type=int, default=None, help="number of kinds (default: the best separated between 4 and 12)")
    p.set_defaults(run=cmd_kinds)

    for name in ("search", "export", "serve"):
        try:
            module = __import__("atlas." + name, fromlist=["register"])
        except ImportError:
            continue
        module.register(sub)
