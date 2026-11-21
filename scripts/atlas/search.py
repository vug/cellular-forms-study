"""Looking for forms: four ways to choose what to grow next, all working on the same database.

    novelty   vary the forms that have the fewest forms like them; with --guided, many
              variations are drawn and only those are grown that a model of the database
              expects to be unlike what is there
    emitter   several local searches, each learning the direction in parameter space in which
              new kinds of form keep turning up, and starting elsewhere when that direction
              gives nothing new any more
    ascent    climb one metric: the slope is estimated from pairs of runs on opposite sides
              of the current point, each averaged over seeds
    breed     children (variations and crosses) of the forms Uğur rated highly, or of given runs

What "like" means is features.Scales.distance: differences below two spreads between seeds do
not count. A form is "new" when it is further from every form of the database than nine in ten
pairs of seeds of one parameter set are from each other (and at least 1).

Only the Python standard library is used.
"""
import math
import random
import time

from . import features, space, stop
from .commands import grown
from .runner import Job, Runner
from .store import Store


class Archive:
    """The forms of the database that were grown to the end, as points to measure against."""

    def __init__(self, store):
        self.store = store
        self.scales = features.Scales.load(store)
        if self.scales is None:
            raise SystemExit("atlas: run 'features' first")
        s = self.scales
        self.unit = [1.0 / p for p in s.population]
        self.dead = [2.0 * a / p for a, p in zip(s.seed, s.population)]
        scale = store.meta("distance_scale") or {}
        self.new_from = max(1.0, scale.get("same_p90", 1.0))
        self.ids, self.points, self.genomes, self.structure, self.healthy = [], [], [], [], []
        for r, stages in grown(store):
            self.add(r["id"], r["genome"], stages[-1])

    def point(self, descriptors):
        # (No feature counts for more than four spreads from the median form: one runaway
        # number, the curvature of a crumpled sheet for one, would otherwise be all the distance.)
        s = self.scales
        return [(c + max(-4.0 * p, min(4.0 * p, x - c))) * u for x, c, p, u in zip(s.fill(features.vector(descriptors)), s.centre, s.population, self.unit)]

    def add(self, run, genome, descriptors):
        self.ids.append(run)
        self.points.append(self.point(descriptors))
        self.genomes.append(genome)
        self.structure.append(descriptors.get("structure", 0.0))
        self.healthy.append(features.healthy(descriptors))

    def distance(self, a, b):
        total = 0.0
        for x, y, dead in zip(a, b, self.dead):
            gap = abs(x - y) - dead
            if gap > 0.0:
                total += gap * gap
        return math.sqrt(total)

    def nearest(self, point, among=None, count=1):
        """Distances to the `count` nearest forms (of `among`: indices, default all), ascending."""
        index = range(len(self.points)) if among is None else among
        best = sorted(self.distance(point, self.points[i]) for i in index)
        return best[:count]

    def sparseness(self, point, among, count=8):
        """Mean distance to the nearest few forms: large where the database is thin."""
        near = self.nearest(point, among, count + 1)
        near = near[1:] if near and near[0] == 0.0 else near[:count]  # (the point itself, when it is in the database)
        return sum(near) / max(len(near), 1)


def _runner(store, args, early_stop=True):
    rule = stop.StopRule.from_json(store.meta("stop_rule")) if early_stop and not args.no_early_stop else None
    return Runner(store, stop_rule=rule), rule


def _enter(archive, store, ids):
    """Adds the finished runs among `ids` to the archive. Returns [(run id, distance to the
    nearest form that was there before, or None when the run was not grown to the end)]."""
    out = []
    before = len(archive.points)
    for run in ids:
        if run is None:
            continue
        r = store.runs("WHERE id = ?", (run,))[0]
        stages = store.stages(run)
        if r["status"] != "ok" or not stages or stages[-1]["cells"] < 0.9 * r["target_cells"]:
            out.append((run, None))
            continue
        if run in archive.ids:
            out.append((run, 0.0))
            continue
        point = archive.point(stages[-1])
        # (An unhealthy form joins the archive, so that the search does not come back to it,
        # but counts as nothing new.)
        out.append((run, archive.nearest(point, range(before))[0] if features.healthy(stages[-1]) else 0.0))
        archive.add(run, r["genome"], stages[-1])
    return out


def _summary(label, entered, archive, t0):
    grown_ = [d for _run, d in entered if d is not None]
    new = [d for d in grown_ if d >= archive.new_from]
    print(f"{label}: {len(entered)} runs, {len(entered) - len(grown_)} not grown to the end, {len(new)} new forms"
          f" (furthest from the database: {max(grown_, default=0.0):.2f}; new from {archive.new_from:.2f}); {time.time() - t0:.0f} s", flush=True)
    return len(new)


# ---------------------------------------------------------------- novelty

def _coordinates_with_sources(genome):
    return space.coordinates({"base": genome["base"], "sources": {}}), frozenset(genome["sources"])


def _expected_point(archive, known, genome, count=5):
    """What a model of the database expects of a genome: the mean of the forms whose parameters
    are nearest (a different set of sources counts as half the width of the space per source)."""
    base, sources = _coordinates_with_sources(genome)
    near = []
    for i, (other_base, other_sources) in enumerate(known):
        d = sum((a - b) ** 2 for a, b in zip(base, other_base)) + 0.25 * len(sources ^ other_sources)
        near.append((d, i))
    near.sort()
    near = near[:count]
    weights = [1.0 / (d + 1e-3) for d, _i in near]
    total = sum(weights)
    width = len(archive.points[0])
    return [sum(w * archive.points[i][k] for w, (_d, i) in zip(weights, near)) / total for k in range(width)]


def cmd_novelty(store, args):
    archive = Archive(store)
    runner, rule = _runner(store, args)
    rng = random.Random(args.seed)
    quiet = 0
    for generation in range(args.generations):
        t0 = time.time()
        sample = rng.sample(range(len(archive.points)), min(600, len(archive.points)))
        fit = [i for i in range(len(archive.points)) if archive.healthy[i]]
        candidates = rng.sample(fit, min(160, len(fit)))
        ranked = sorted(candidates, key=lambda i: -archive.sparseness(archive.points[i], sample))
        parents = ranked[:max(args.batch // 2, 4)]
        children = []
        draws = args.batch * (8 if args.guided else 1)
        for _ in range(draws):
            a = rng.choice(parents)
            if rng.random() < 0.3:
                b = rng.choice(parents)
                genome, second = space.mutate(rng, space.cross(rng, archive.genomes[a], archive.genomes[b]), 0.06), archive.ids[b]
            else:
                genome, second = space.mutate(rng, archive.genomes[a], args.strength), None
            children.append((genome, archive.ids[a], second))
        if args.guided:
            known = [_coordinates_with_sources(g) for g in archive.genomes]
            scored = [(archive.sparseness(_expected_point(archive, known, c[0]), sample), k) for k, c in enumerate(children)]
            children = [children[k] for _s, k in sorted(scored, reverse=True)[:args.batch]]
        jobs = [Job(g, seed=1, origin="novelty-guided" if args.guided else "novelty", parent=p, parent2=p2, target=args.target) for g, p, p2 in children]
        entered = _enter(archive, store, runner.run_many(jobs))
        new = _summary(f"novelty, round {generation + 1}", entered, archive, t0)
        quiet = quiet + 1 if new <= 0.05 * len(entered) else 0
        if quiet >= 3:
            print("novelty: three rounds in a row with next to nothing new; stopping")
            break


# ---------------------------------------------------------------- emitters

class Emitter:
    """A local search that learns a direction. It keeps a point in parameter space, a step it
    carries over from round to round (the direction that worked), and how far it tries each
    parameter. Children that are new forms pull the point towards them."""

    def __init__(self, number, archive, start, rng):
        self.number = number
        self.like = archive.genomes[start]
        self.parent = archive.ids[start]
        self.mean = space.coordinates(self.like)
        self.carry = [0.0] * len(self.mean)
        self.width = [1.0] * len(self.mean)
        self.sigma = 0.08
        self.stale = 0
        self.found = 0
        self.rng = rng

    def children(self, count):
        out = []
        for _ in range(count):
            point = [min(max(m + c + self.sigma * w * self.rng.gauss(0, 1), 0.0), 1.0) for m, c, w in zip(self.mean, self.carry, self.width)]
            out.append(point)
        return out

    def learn(self, points, gains, new_from):
        good = sorted(((g, p) for g, p in zip(gains, points) if g is not None and g >= new_from), key=lambda x: -x[0])[:3]
        if not good:
            self.stale += 1
            self.sigma *= 0.7
            self.carry = [0.5 * c for c in self.carry]
            return
        self.found += len(good)
        weights = [g for g, _p in good]
        total = sum(weights)
        target = [sum(w * p[k] for w, (_g, p) in zip(weights, good)) / total for k in range(len(self.mean))]
        step = [t - m for t, m in zip(target, self.mean)]
        size = sum(abs(s) for s in step) / len(step) or 1e-9
        self.carry = [0.5 * c + 0.5 * s for c, s in zip(self.carry, step)]
        self.width = [min(max(0.8 * w + 0.2 * abs(s) / size, 0.3), 3.0) for w, s in zip(self.width, step)]
        self.mean = target
        self.sigma = min(self.sigma * 1.15, 0.25)
        self.stale = 0

    def direction(self):
        """The parameters the carried step moves most, as text."""
        names = sorted(space.BASE) + [f"{s}.{k}" for s in sorted(self.like["sources"]) for k in sorted(space.SOURCES[s])]
        strongest = sorted(zip(self.carry, names), key=lambda x: -abs(x[0]))[:3]
        return ", ".join(f"{n} {'+' if c > 0 else '-'}{abs(c):.2f}" for c, n in strongest if abs(c) > 0.005) or "none yet"


def cmd_emitter(store, args):
    archive = Archive(store)
    runner, rule = _runner(store, args)
    rng = random.Random(args.seed)

    def start():
        sample = rng.sample(range(len(archive.points)), min(600, len(archive.points)))
        fit = [i for i in range(len(archive.points)) if archive.healthy[i]]
        candidates = rng.sample(fit, min(120, len(fit)))
        return max(candidates, key=lambda i: archive.sparseness(archive.points[i], sample))

    emitters = [Emitter(k + 1, archive, start(), rng) for k in range(args.emitters)]
    restarts = 0
    for generation in range(args.generations):
        t0 = time.time()
        jobs, owner, points = [], [], []
        for e in emitters:
            for point in e.children(args.batch // len(emitters)):
                jobs.append(Job(space.from_coordinates(point, e.like), seed=1, origin="emitter", parent=e.parent, target=args.target))
                owner.append(e), points.append(point)
        ids = runner.run_many(jobs)
        entered = _enter(archive, store, ids)
        gains = dict(entered)
        for e in emitters:
            mine = [k for k, o in enumerate(owner) if o is e]
            e.learn([points[k] for k in mine], [gains.get(ids[k]) for k in mine], archive.new_from)
        _summary(f"emitters, round {generation + 1}", entered, archive, t0)
        for k, e in enumerate(emitters):
            print(f"    emitter {e.number} (from run {e.parent}): {e.found} new forms so far, reach {e.sigma:.3f}, direction: {e.direction()}", flush=True)
            if e.stale >= 2:  # nothing new twice in a row: this direction is used up
                restarts += 1
                emitters[k] = Emitter(args.emitters + restarts, archive, start(), rng)
                print(f"    emitter {e.number} found nothing new twice in a row and stops; emitter {emitters[k].number} starts from run {emitters[k].parent}", flush=True)


# ---------------------------------------------------------------- ascent

def _objective(descriptors, metric):
    """The logarithm of the metric; an unhealthy form (features.healthy) counts as a seventh of
    its value, so that a climb does not end in a crumpled sheet, which scores high on relief."""
    value = features.raw(descriptors, metric)
    return math.log(max(value if value is not None else 0.0, 1e-4)) - (0.0 if features.healthy(descriptors) else 2.0)


def cmd_ascent(store, args):
    runner, _rule = _runner(store, args, early_stop=False)
    rng = random.Random(args.seed)
    if args.start is None:
        best = max((rs for rs in grown(store) if rs[0]["origin"] != "ascent"), key=lambda rs: _objective(rs[1][-1], args.metric))
        start_run = best[0]
    else:
        start_run = store.runs("WHERE id = ?", (args.start,))[0]
    like = start_run["genome"]
    mean = space.coordinates(like)
    seeds = list(range(1, args.seeds + 1))
    print(f"ascent on {args.metric}, from run {start_run['id']}")

    def evaluate(points, parent):
        jobs = [Job(space.from_coordinates(p, like), seed=s, origin="ascent", parent=parent, target=args.target) for p in points for s in seeds]
        ids = runner.run_many(jobs)
        values = []
        for k in range(len(points)):
            got = []
            for run in ids[k * len(seeds):(k + 1) * len(seeds)]:
                stages = store.stages(run) if run is not None else []
                if stages:
                    got.append(_objective(stages[-1], args.metric))
            values.append(sum(got) / len(got) if got else None)
        return values, ids

    parent = start_run["id"]
    for iteration in range(args.iterations):
        t0 = time.time()
        directions = [[rng.gauss(0, 1) for _ in mean] for _ in range(args.pairs)]
        points = [mean]
        for d in directions:
            points.append([min(max(m + args.sigma * x, 0.0), 1.0) for m, x in zip(mean, d)])
            points.append([min(max(m - args.sigma * x, 0.0), 1.0) for m, x in zip(mean, d)])
        values, ids = evaluate(points, parent)
        slope = [0.0] * len(mean)
        used = 0
        for k, d in enumerate(directions):
            up, down = values[1 + 2 * k], values[2 + 2 * k]
            if up is None or down is None:
                continue
            used += 1
            for j, x in enumerate(d):
                slope[j] += (up - down) / (2.0 * args.sigma) * x
        length = math.sqrt(sum(s * s for s in slope))
        here = values[0]
        print(f"  step {iteration + 1}: {args.metric} = {math.exp(here) if here is not None else float('nan'):.4f} at run {ids[0]}"
              f" (slope from {used} pairs, {time.time() - t0:.0f} s)", flush=True)
        if length == 0.0:
            break
        mean = [min(max(m + args.rate * s / length, 0.0), 1.0) for m, s in zip(mean, slope)]
        parent = ids[0]
    values, ids = evaluate([mean], parent)
    print(f"  end: {args.metric} = {math.exp(values[0]) if values[0] is not None else float('nan'):.4f} at run {ids[0]}")


# ---------------------------------------------------------------- breed

def cmd_breed(store, args):
    runner, _rule = _runner(store, args)
    rng = random.Random(args.seed)
    if args.parents:
        ids = [int(x) for x in args.parents.split(",")]
    else:
        ids = [run for run, label in store.labels().items() if (label["rating"] or 0) >= args.min_rating]
    parents = [r for r in store.runs("ORDER BY id") if r["id"] in set(ids) and r["genome"]]
    if not parents:
        raise SystemExit(f"atlas: no parents (no run rated {args.min_rating} or more; give --parents)")
    jobs = []
    for _ in range(args.batch):
        a = rng.choice(parents)
        if len(parents) > 1 and rng.random() < 0.4:
            b = rng.choice([p for p in parents if p is not a])
            jobs.append(Job(space.mutate(rng, space.cross(rng, a["genome"], b["genome"]), 0.05), seed=1, origin="bred", parent=a["id"], parent2=b["id"], target=args.target))
        else:
            jobs.append(Job(space.mutate(rng, a["genome"], args.strength), seed=1, origin="bred", parent=a["id"], target=args.target))
    t0 = time.time()
    ids = runner.run_many(jobs)
    print(f"breed: {sum(1 for i in ids if i is not None)} children of {len(parents)} parents in {time.time() - t0:.0f} s: runs {min(i for i in ids if i)} to {max(i for i in ids if i)}")


def cmd_yield(args):
    """How many of the runs of each origin were new forms when they were made: further from
    every earlier form of the database than the 'new' distance. For the survey, a sample."""
    store = Store(args.root)
    archive = Archive(store)
    rng = random.Random(1)
    origin = {r["id"]: r["origin"] for r in store.runs("")}
    made = {}
    for r in store.runs(""):
        made.setdefault(r["origin"], []).append(r["id"])
    place = {run: k for k, run in enumerate(archive.ids)}
    report = {}
    print(f"a form is new when it is healthy and at a distance of {archive.new_from:.2f} or more from every earlier form")
    print(f"  {'origin':16s} {'runs':>6s} {'grown to the end':>17s} {'new forms':>10s} {'new per 100 runs':>17s}")
    for name, runs in sorted(made.items()):
        grown_ = [run for run in runs if run in place]
        checked = grown_ if name != "survey" else rng.sample(grown_, min(300, len(grown_)))
        new = 0
        for run in checked:
            k = place[run]
            if archive.healthy[k] and (k == 0 or archive.nearest(archive.points[k], range(k))[0] >= archive.new_from):
                new += 1
        share = new / len(checked) if checked else 0.0
        per_run = share * len(grown_) / len(runs) if runs else 0.0
        report[name] = {"runs": len(runs), "grown": len(grown_), "new_share_of_grown": share, "new_per_run": per_run}
        print(f"  {name:16s} {len(runs):6d} {len(grown_):17d} {round(share * len(grown_)):10d} {100 * per_run:17.1f}" + ("   (from a sample of 300)" if name == "survey" else ""))
    store.set_meta("yield_report", report)


def cmd_search(args):
    store = Store(args.root)
    {"novelty": cmd_novelty, "emitter": cmd_emitter, "ascent": cmd_ascent, "breed": cmd_breed}[args.driver](store, args)


def register(sub):
    p = sub.add_parser("search", help="grow new forms chosen by one of four drivers", description=__doc__, formatter_class=__import__("argparse").RawDescriptionHelpFormatter)
    p.add_argument("--driver", choices=["novelty", "emitter", "ascent", "breed"], required=True)
    p.add_argument("--generations", type=int, default=5, help="rounds (novelty, emitter)")
    p.add_argument("--batch", type=int, default=48, help="runs per round (novelty, emitter), or children (breed)")
    p.add_argument("--target", type=int, default=31623, help="cells every form grows to")
    p.add_argument("--seed", type=int, default=1, help="seed of the search's own choices")
    p.add_argument("--strength", type=float, default=0.12, help="size of a variation, as a share of each parameter's range")
    p.add_argument("--guided", action="store_true", help="novelty: draw eight times as many variations and grow those a model expects to be new")
    p.add_argument("--emitters", type=int, default=4, help="emitter: how many local searches run side by side")
    p.add_argument("--metric", default="structure", help="ascent: the metric to climb (any name cf_describe prints, relief_s8, lobe_3, ...)")
    p.add_argument("--start", type=int, default=None, help="ascent: the run to start from (default: the best in the database)")
    p.add_argument("--iterations", type=int, default=6, help="ascent: steps")
    p.add_argument("--pairs", type=int, default=6, help="ascent: pairs of runs per step")
    p.add_argument("--seeds", type=int, default=2, help="ascent: seeds every point is averaged over")
    p.add_argument("--sigma", type=float, default=0.06, help="ascent: how far the pairs lie from the point, as a share of each parameter's range")
    p.add_argument("--rate", type=float, default=0.08, help="ascent: length of a step, in the same unit")
    p.add_argument("--parents", default=None, help="breed: run ids, comma separated (default: the runs rated --min-rating or more)")
    p.add_argument("--min-rating", type=int, default=4)
    p.add_argument("--no-early-stop", action="store_true", help="grow every form to the end (default: the early-stop rule of the database is used when there is one)")
    p.set_defaults(run=cmd_search)

    p = sub.add_parser("yield", help="how many new forms each origin (survey, novelty, emitter, ...) gave per run")
    p.set_defaults(run=cmd_yield)
