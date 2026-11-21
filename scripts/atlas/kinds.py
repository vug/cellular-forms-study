"""Sorting the forms of the database into kinds, and laying them out on a map.

Each form is its features in population spreads from the median form (features.Scales.standard).
The map is the two directions in which the forms differ most (principal components). The kinds
are the groups of k-means, with the number of groups chosen by how well separated they come out
(silhouette), unless it is given. Every kind gets its most typical form, a handful of forms from
its middle and from its edge, and a working name made of the features that set it apart; Uğur
names them properly on the page.

Only the Python standard library is used.
"""
import math
import random

from .features import NAMES

# feature: (what a low value looks like, what a high value looks like)
WORDS = {
    "relief_s1": ("fine-smooth", "grainy"), "relief_s2": ("fine-smooth", "grainy"), "relief_s4": ("unfolded", "folded"), "relief_s8": ("unfolded", "folded"),
    "relief_s16": ("round body", "lumpy body"), "relief_rest": ("round body", "lumpy body"), "skew_s4": ("creased", "bumpy"), "skew_s8": ("creased", "bumpy"),
    "relief_patchiness": ("even texture", "patchy"), "growth_unevenness": ("", "one-sided growth"), "reach": ("compact", "reaching out"),
    "lobe_2": ("", "long or flat"), "lobe_3": ("", "lobed"), "lobe_4": ("", "lobed"), "thickness_p50": ("thin-walled", "bulky"), "thickness_p10": ("thin parts", ""),
    "buried_fraction": ("open", "tightly folded"), "area_ratio": ("ball", "much surface"), "elongation": ("", "elongated"), "flatness": ("", "flattened"),
    "curvature_std": ("smooth", "rough"), "concave_fraction": ("convex", "hollowed"),
}


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def principal_directions(points, count=2, rounds=200):
    """The `count` directions of largest spread of the points (power iteration on the covariance)."""
    n, d = len(points), len(points[0])
    mean = [sum(p[k] for p in points) / n for k in range(d)]
    cov = [[0.0] * d for _ in range(d)]
    for p in points:
        c = [p[k] - mean[k] for k in range(d)]
        for i in range(d):
            ci, row = c[i], cov[i]
            for j in range(i, d):
                row[j] += ci * c[j]
    for i in range(d):
        for j in range(i):
            cov[i][j] = cov[j][i]
    directions, shares = [], []
    total = sum(cov[i][i] for i in range(d)) or 1.0
    rng = random.Random(1)
    for _ in range(count):
        v = [rng.gauss(0, 1) for _ in range(d)]
        value = 0.0
        for _ in range(rounds):
            w = [_dot(row, v) for row in cov]
            for u in directions:  # stay at right angles to the directions already found
                f = _dot(w, u)
                w = [a - f * b for a, b in zip(w, u)]
            value = math.sqrt(_dot(w, w))
            if value < 1e-12:
                break
            v = [a / value for a in w]
        directions.append(v)
        shares.append(value / total)
    return mean, directions, shares


def _nearest(point, centres):
    best, at = float("inf"), 0
    for k, c in enumerate(centres):
        d = 0.0
        for x, y in zip(point, c):
            d += (x - y) * (x - y)
            if d >= best:
                break
        if d < best:
            best, at = d, k
    return at, best


def k_means(points, k, rng, rounds=40):
    centres = [list(rng.choice(points))]
    nearest = [float("inf")] * len(points)
    while len(centres) < k:  # k-means++: new centres far from the ones there are
        for i, p in enumerate(points):
            nearest[i] = min(nearest[i], sum((x - y) ** 2 for x, y in zip(p, centres[-1])))
        pick, acc = rng.random() * sum(nearest), 0.0
        for i, d in enumerate(nearest):
            acc += d
            if acc >= pick:
                centres.append(list(points[i]))
                break
        else:
            centres.append(list(rng.choice(points)))
    group = [0] * len(points)
    for _ in range(rounds):
        moved = False
        for i, p in enumerate(points):
            g, _d = _nearest(p, centres)
            if g != group[i]:
                group[i], moved = g, True
        sums = [[0.0] * len(points[0]) for _ in range(k)]
        counts = [0] * k
        for p, g in zip(points, group):
            counts[g] += 1
            s = sums[g]
            for j, x in enumerate(p):
                s[j] += x
        for g in range(k):
            if counts[g] > 0:
                centres[g] = [x / counts[g] for x in sums[g]]
        if not moved:
            break
    cost = sum(_nearest(p, centres)[1] for p in points)
    return group, centres, cost


def silhouette(points, group, k, rng, sample=500):
    """How well separated the groups are, -1 to 1 (on a sample of the points)."""
    index = list(range(len(points)))
    rng.shuffle(index)
    index = index[:sample]
    total = 0.0
    for i in index:
        sums, counts = [0.0] * k, [0] * k
        for j in index:
            if i == j:
                continue
            sums[group[j]] += math.sqrt(sum((x - y) ** 2 for x, y in zip(points[i], points[j])))
            counts[group[j]] += 1
        own = sums[group[i]] / counts[group[i]] if counts[group[i]] else 0.0
        other = min((sums[g] / counts[g] for g in range(k) if g != group[i] and counts[g]), default=0.0)
        total += (other - own) / max(own, other, 1e-12)
    return total / len(index)


def sort_into_kinds(ids, points, k=None, seed=1):
    """ids and standardised feature vectors of the forms. Returns (assignments {id: kind},
    map positions {id: (x, y)}, kinds [dict], report dict)."""
    rng = random.Random(seed)
    mean, directions, shares = principal_directions(points, 2)
    positions = {i: (_dot([a - m for a, m in zip(p, mean)], directions[0]), _dot([a - m for a, m in zip(p, mean)], directions[1])) for i, p in zip(ids, points)}
    tried = {}
    if k is None:
        best = None
        for candidate in range(4, 13):
            group, centres, cost = min((k_means(points, candidate, rng) for _ in range(3)), key=lambda r: r[2])
            score = silhouette(points, group, candidate, rng)
            tried[candidate] = round(score, 3)
            if best is None or score > best[0]:
                best = (score, candidate, group, centres)
        _score, k, group, centres = best
    else:
        group, centres, _cost = min((k_means(points, k, rng) for _ in range(5)), key=lambda r: r[2])
    # Kinds in the order of their size, largest first.
    sizes = sorted(range(k), key=lambda g: -sum(1 for x in group if x == g))
    rank = {g: n + 1 for n, g in enumerate(sizes)}
    kinds = []
    for g in sizes:
        members = [(sum((x - y) ** 2 for x, y in zip(points[i], centres[g])), ids[i]) for i in range(len(ids)) if group[i] == g]
        if not members:
            continue
        members.sort()
        profile = {name: round(centres[g][j], 2) for j, name in enumerate(NAMES)}
        words = []
        for name, z in sorted(profile.items(), key=lambda item: -abs(item[1])):
            word = WORDS.get(name, ("", ""))[1 if z > 0 else 0]
            if abs(z) >= 0.6 and word and word not in words:
                words.append(word)
            if len(words) == 3:
                break
        kinds.append({"id": rank[g], "name": ", ".join(words) if words else "middling", "count": len(members), "typical": members[0][1],
                      "middle": [m[1] for m in members[:12]], "edge": [m[1] for m in members[-6:]], "profile": profile, "centre": [round(x, 4) for x in centres[g]]})
    assignment = {ids[i]: rank[group[i]] for i in range(len(ids))}
    report = {"forms": len(ids), "kinds": len(kinds), "silhouette_by_count": tried, "map_shares": [round(s, 3) for s in shares],
              "map_mean": [round(x, 5) for x in mean], "map_axes": [[round(x, 5) for x in d] for d in directions],
              "map_directions": [{name: round(v, 2) for name, v in zip(NAMES, d) if abs(v) >= 0.25} for d in directions]}
    return assignment, positions, kinds, report


def place(point, kinds, report):
    """The kind whose centre is nearest to a form, and the form's position on the map, for
    forms that came after the sorting."""
    best = min(kinds, key=lambda k: sum((x - y) ** 2 for x, y in zip(point, k["centre"])))
    centred = [a - m for a, m in zip(point, report["map_mean"])]
    return best["id"], (_dot(centred, report["map_axes"][0]), _dot(centred, report["map_axes"][1]))

