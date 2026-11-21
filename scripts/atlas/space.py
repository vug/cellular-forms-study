"""The space the search moves in: a genome is the base values of the model, the optional food
sources and extensions that are switched on with their values, and the start shape.

The ranges are those of scripts/explore.py (BASE_SPACE, SOURCES); this module adds draws that
fill the space evenly, crossing two genomes, and a plain vector form for the direction-learning
search. Only the Python standard library is used.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import explore  # noqa: E402

BASE = explore.BASE_SPACE
SOURCES = explore.SOURCES
DEFAULT_INIT = {"shape": "icosphere:2", "food": "random"}
# Start shapes other than the icosphere (cf_run --new): the same mesh stretched, or with smooth
# noise on its radius.
OTHER_INITS = [{"shape": "ellipsoid:2:2:1:1", "food": "random"}, {"shape": "ellipsoid:2:1:1:0.5", "food": "random"}, {"shape": "noisy:2:0.3:2", "food": "random"},
               {"shape": "icosphere:3", "food": "random"}, {"shape": "icosphere:1", "food": "random"}, {"shape": "icosphere:2", "food": "zero"}]


def _from_unit(u, lo, hi, log):
    return math.exp(math.log(lo) + u * (math.log(hi) - math.log(lo))) if log else lo + u * (hi - lo)


def _to_unit(value, lo, hi, log):
    u = (math.log(value) - math.log(lo)) / (math.log(hi) - math.log(lo)) if log else (value - lo) / (hi - lo)
    return min(max(u, 0.0), 1.0)


def even_genomes(count, rng, source_probability=0.25):
    """`count` genomes that fill the base space evenly (a Latin hypercube: every range is cut
    into `count` slices and each slice is used once). Sources are switched on at random."""
    names = sorted(BASE)
    slices = {}
    for name in names:
        order = list(range(count))
        rng.shuffle(order)
        slices[name] = order
    genomes = []
    for i in range(count):
        genome = {"base": {name: _from_unit((slices[name][i] + rng.random()) / count, *BASE[name]) for name in names}, "sources": {}, "init": dict(DEFAULT_INIT)}
        for name, space in SOURCES.items():
            if rng.random() < source_probability:
                genome["sources"][name] = {k: explore.draw(rng, *v) for k, v in space.items()}
                if name == "light":
                    genome["sources"][name]["direction"] = explore.random_direction(rng)
        genomes.append(genome)
    return genomes


def default_genome():
    """The model's defaults (cf_run --list-params) in genome form."""
    return {"base": {"springFactor": 0.15, "planarFactor": 0.1, "bulgeFactor": 0.1, "repulsionStrength": 0.1, "radiusOfInfluence": 2.0, "foodMax": 0.002,
                     "daughterOffset": 0.1}, "sources": {}, "init": dict(DEFAULT_INIT)}


def to_params(genome):
    return explore.to_params({"base": dict(genome["base"]), "sources": genome["sources"]})


def mutate(rng, genome, strength=0.12):
    child = explore.mutate(rng, {"base": genome["base"], "sources": genome["sources"]}, strength)
    child["init"] = dict(genome.get("init") or DEFAULT_INIT)
    return child


def cross(rng, a, b):
    """A child of two genomes: every base value from one parent or the other, or a point between
    them; a source that only one parent has is inherited half of the time."""
    child = {"base": {}, "sources": {}, "init": dict((a if rng.random() < 0.5 else b).get("init") or DEFAULT_INIT)}
    for name, (lo, hi, log) in BASE.items():
        ua, ub = _to_unit(a["base"][name], lo, hi, log), _to_unit(b["base"][name], lo, hi, log)
        pick = rng.random()
        u = ua if pick < 0.35 else ub if pick < 0.7 else ua + rng.random() * (ub - ua)
        child["base"][name] = _from_unit(u, lo, hi, log)
    for name in SOURCES:
        have = [p["sources"][name] for p in (a, b) if name in p["sources"]]
        if len(have) == 2 or (len(have) == 1 and rng.random() < 0.5):
            child["sources"][name] = json.loads(json.dumps(have[int(rng.random() * len(have))]))
    return child


# ---- a genome as a point with coordinates in [0, 1]: the base values, then the values of the
# sources that are on (which sources are on is not part of the point: `like` gives that).

def coordinates(genome):
    point = [_to_unit(genome["base"][name], *BASE[name]) for name in sorted(BASE)]
    for name in sorted(genome["sources"]):
        for k in sorted(SOURCES[name]):
            point.append(_to_unit(genome["sources"][name][k], *SOURCES[name][k]))
    return point


def from_coordinates(point, like):
    """The genome at `point`, with the sources (and the light's direction, and the start) of `like`."""
    genome = {"base": {}, "sources": {}, "init": dict(like.get("init") or DEFAULT_INIT)}
    it = iter(point)
    for name in sorted(BASE):
        genome["base"][name] = _from_unit(min(max(next(it), 0.0), 1.0), *BASE[name])
    for name in sorted(like["sources"]):
        genome["sources"][name] = {}
        for k in sorted(SOURCES[name]):
            genome["sources"][name][k] = _from_unit(min(max(next(it), 0.0), 1.0), *SOURCES[name][k])
        if name == "light":
            genome["sources"][name]["direction"] = list(like["sources"][name]["direction"])
    return genome


def describe(genome):
    """A short line for lists: the sources that are on, and a start that is not the usual one."""
    parts = sorted(genome.get("sources") or {})
    init = genome.get("init") or DEFAULT_INIT
    if init != DEFAULT_INIT:
        parts.append(init["shape"] + ("" if init.get("food", "random") == "random" else ", no food at the start"))
    return ", ".join(parts) if parts else "base"
