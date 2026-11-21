"""The numbers by which two forms are compared: a fixed list of features taken from the
descriptors of a form (cf_describe), each on a scale where equal steps mean about equally much.

Two scales belong to every feature. The spread between seeds is how much the feature differs
between two runs of the same parameters: a difference below about two of those is no difference.
The spread of the population is how much it differs between the forms in the database: it puts
the features on a common footing, so that none of them rules the distance by being precise.

    distance(a, b)^2 = sum over features of ((|a - b| - 2 seed spread)+ / population spread)^2

Only the Python standard library is used.
"""
import math
import statistics

# name, kind of scale, floor (log) or bound (clip)
FEATURES = [
    ("relief_s1", "log", 1e-4), ("relief_s2", "log", 1e-4), ("relief_s4", "log", 1e-4), ("relief_s8", "log", 1e-4), ("relief_s16", "log", 1e-4),
    ("relief_rest", "log", 1e-3),
    ("skew_s4", "clip", 4.0), ("skew_s8", "clip", 4.0),
    ("relief_patchiness", "clip", 3.0), ("growth_unevenness", "clip", 2.0),
    ("reach", "log", 0.5), ("lobe_2", "log", 3e-3), ("lobe_3", "log", 3e-3), ("lobe_4", "log", 3e-3),
    ("thickness_p50", "log", 0.01), ("thickness_p10", "log", 0.01), ("buried_fraction", "lin", 0.0),
    ("area_ratio", "log", 0.5), ("elongation", "log", 0.5), ("flatness", "log", 0.5),
    ("curvature_std", "lin", 0.0), ("concave_fraction", "lin", 0.0),
]
NAMES = [f[0] for f in FEATURES]

# What the page lists for every form, with a few words each (the features and more).
METRICS = [
    ("structure", "total relief times how evenly it is spread over the scales: one number for 'has structure'"),
    ("relief_total", "all relief together, over the form's radius"),
    ("relief_evenness", "1: every scale has its share of the relief; towards 0: one scale has it all"),
    ("relief_micro", "relief up to 2 links: grain"),
    ("relief_meso", "relief between grain and a quarter of the sheet's radius: folds, bumps"),
    ("relief_macro", "relief from a quarter of the sheet's radius, and what is left of the shape: lobes, the body"),
    ("relief_peak_sigma", "the smoothing length (links) of the band with the most relief"),
    ("relief_patchiness", "how unevenly the fine relief is spread over 32 regions (0: the same everywhere)"),
    ("growth_unevenness", "how unevenly the young cells are spread over the regions (0: even growth)"),
    ("reach", "how far the farthest cells are from the centre, over the radius of gyration (1: sphere)"),
    ("lobe_degree", "the strongest many-fold pattern of the body, 2 to 6 (0: none)"),
    ("lobe_2", "strength of a two-fold pattern: long or flat"), ("lobe_3", "three-fold"), ("lobe_4", "four-fold"),
    ("thickness_p50", "median thickness through the form, over the radius of gyration (2: sphere)"),
    ("thickness_p10", "thickness of the thinnest tenth"),
    ("buried_fraction", "share of the cells that face another part of the form nearby"),
    ("gap_median", "median distance to that other part, in links"),
    ("area_ratio", "area over that of a sphere with the same volume (1: sphere)"),
    ("sphericity", "1: sphere; towards 0: folded or thin"),
    ("elongation", "longest over middle axis"), ("flatness", "middle over shortest axis"),
    ("ellipsoid_fill", "volume over that of the ellipsoid with the same proportions"),
    ("curvature_mean", "mean curvature per link"), ("curvature_std", "its spread"), ("concave_fraction", "share of cells in a hollow"),
    ("box_dimension", "box-counting dimension of the cells (2: a smooth sheet)"),
    ("skew_s4", "which way the heights lean at 4 links (+ peaks, - pits)"), ("skew_s8", "the same at 8 links"),
    ("link_mean", "mean link length in rest lengths (health)"), ("link_std", "its spread (health)"),
    ("fold_ratio", "how much the sheet is compressed (health)"), ("close_pairs_per_cell", "self-contact (health)"),
    ("radius", "radius of gyration, in rest lengths"), ("sheet_radius", "radius of the sphere the cells would cover, in links"),
]


def healthy(d):
    """False for a form whose sheet is badly stretched, badly compressed or lies on itself:
    the mean link is outside 0.6 to 1.6 rest lengths, or more than one cell in twenty has
    another, unconnected cell within its own radius. 2.4 % of the survey. Such forms are kept
    and shown, but a search does not count them as finds and does not breed from them: left
    alone, a search for what is new drifts into crumpled sheets, which are unlike everything."""
    return 0.6 < d.get("link_mean", 1.0) < 1.6 and d.get("close_pairs_per_cell", 0.0) < 0.05


def raw(d, name):
    """The value of a feature or metric in a descriptor dict, or None when the form has none
    (a band the form is too small for)."""
    if name in d:
        return d[name]
    if name.startswith("relief_s") or name.startswith("skew_s"):
        sigma = float(name.split("_s")[1])
        values = d.get("relief" if name.startswith("relief") else "relief_skew", [])
        for s, v in zip(d.get("relief_sigma", []), values):
            if s == sigma:
                return v
        return None
    if name.startswith("lobe_") and name[5:].isdigit():
        lobes = d.get("lobes", [])
        k = int(name[5:]) - 1
        return lobes[k] if k < len(lobes) else None
    return None


def scaled(value, kind, bound):
    if value is None:
        return None
    if kind == "log":
        return math.log(max(value, bound))
    if kind == "clip":
        return max(-bound, min(bound, value))
    return value


def vector(d):
    """The features of a form on their scales (None where the form has none)."""
    return [scaled(raw(d, name), kind, bound) for name, kind, bound in FEATURES]


class Scales:
    """The two spreads of every feature, measured on the database (measure()) and kept in it."""

    def __init__(self, seed, population, centre):
        self.seed = seed              # spread between seeds, per feature
        self.population = population  # spread between forms, per feature
        self.centre = centre          # the median form, per feature (stands in for a missing value)

    @staticmethod
    def load(store):
        m = store.meta("feature_scales")
        if not m or m.get("names") != NAMES:
            return None
        return Scales(m["seed"], m["population"], m["centre"])

    def save(self, store):
        store.set_meta("feature_scales", {"names": NAMES, "seed": self.seed, "population": self.population, "centre": self.centre})

    def fill(self, v):
        return [self.centre[k] if x is None else x for k, x in enumerate(v)]

    def standard(self, v):
        """The features in population spreads from the median form (for maps and sorting into kinds)."""
        return [max(-4.0, min(4.0, (x - self.centre[k]) / self.population[k])) for k, x in enumerate(self.fill(v))]

    def distance(self, a, b):
        total = 0.0
        for k, (x, y) in enumerate(zip(self.fill(a), self.fill(b))):
            gap = abs(x - y) - 2.0 * self.seed[k]
            if gap > 0.0:
                total += (gap / self.population[k]) ** 2
        return math.sqrt(total)


def measure(finals, pairs):
    """`finals`: feature vectors of the forms of the database (one per parameter set);
    `pairs`: pairs of vectors of the same parameters with different seeds."""
    seed, population, centre = [], [], []
    for k in range(len(FEATURES)):
        values = sorted(v[k] for v in finals if v[k] is not None)
        if len(values) < 8:
            seed.append(0.0), population.append(1.0), centre.append(0.0)
            continue
        centre.append(statistics.median(values))
        spread = (values[int(0.9 * (len(values) - 1))] - values[int(0.1 * (len(values) - 1))]) / 2.563  # 10th to 90th percentile, as a standard deviation
        population.append(spread if spread > 1e-9 else 1.0)
        # The spread between seeds from the median difference of the pairs, not from the mean
        # square: a few parameter sets sit between two kinds of form, where one seed in ten
        # gives another form, and they would set the scale for all the rest.
        differences = sorted(abs(a[k] - b[k]) for a, b in pairs if a[k] is not None and b[k] is not None)
        seed.append(1.0483 * differences[len(differences) // 2] if differences else 0.0)
    return Scales(seed, population, centre)
