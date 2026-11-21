"""When to stop growing a form because nothing is expected of it.

A run is watched at every quarter decade of cells. At three of those stages (1,000, 3,162 and
10,000 cells) a rule looks at how the form has grown so far and gives the chance that it will
end with structure: its structure and relief now, how fast they rise with size, how folded and
how compressed the sheet is (a compressed sheet has folds to come), how fast it grows. The rule
is a logistic regression per stage, fitted on the runs of the database that were grown to the
end. A run is stopped when its chance is below a threshold that is set so that only a small,
stated share of the forms that do end with structure would have been stopped (their share over
all three stages is `miss`, 2 % by default).

How good the rule is, is measured on runs it was not fitted on (five folds): the share of the
dull forms it stops and at which stage, the share of the forms with structure it stops by
mistake, and the share of the simulation work (cells times steps) it saves.

`settled()` is a second, simpler test, for any form: the form has stopped changing in kind when
its scale-free numbers change by less than a tenth per half decade of cells.

Only the Python standard library is used.
"""
import math
import random

DECISION_STAGES = (12, 14, 16)  # 1,000, 3,162 and 10,000 cells
INPUTS = ["log structure", "log micro relief", "log meso relief", "log macro relief", "log area ratio", "rise of structure", "rise of area ratio",
          "buried share", "log reach", "log thickness", "link length", "spread of link length", "fold ratio", "log steps"]


def inputs(by_stage, stage):
    """What the rule sees of a run at a decision stage, or None when the run has no descriptors
    there or half a decade before."""
    now, before = by_stage.get(stage), by_stage.get(stage - 2)
    if now is None or before is None:
        return None
    octaves = math.log(max(now["cells"], 2) / max(before["cells"], 1))

    def lg(d, name, floor):
        return math.log(max(d.get(name, 0.0) or 0.0, floor))

    rise = lambda name, floor: (lg(now, name, floor) - lg(before, name, floor)) / octaves if octaves > 0 else 0.0  # noqa: E731
    return [lg(now, "structure", 1e-4), lg(now, "relief_micro", 1e-4), lg(now, "relief_meso", 1e-4), lg(now, "relief_macro", 1e-4), lg(now, "area_ratio", 0.5),
            rise("structure", 1e-4), rise("area_ratio", 0.5), now.get("buried_fraction", 0.0), lg(now, "reach", 0.5), lg(now, "thickness_p50", 0.01),
            now.get("link_mean", 1.0), now.get("link_std", 0.0), now.get("fold_ratio", 1.0), math.log(max(now.get("step", 1), 1))]


def _solve(a, b):
    """Solves a x = b for a small symmetric positive matrix (Gauss with pivoting)."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(m[r][col]))
        m[col], m[pivot] = m[pivot], m[col]
        if abs(m[col][col]) < 1e-12:
            continue
        for r in range(col + 1, n):
            f = m[r][col] / m[col][col]
            for c in range(col, n + 1):
                m[r][c] -= f * m[col][c]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        s = m[r][n] - sum(m[r][c] * x[c] for c in range(r + 1, n))
        x[r] = s / m[r][r] if abs(m[r][r]) > 1e-12 else 0.0
    return x


def fit_logistic(rows, labels, ridge=1.0, rounds=25):
    """Logistic regression on standardised inputs (Newton steps). Returns the model as a dict."""
    n, d = len(rows), len(rows[0])
    mean = [sum(r[k] for r in rows) / n for k in range(d)]
    std = [math.sqrt(sum((r[k] - mean[k]) ** 2 for r in rows) / n) or 1.0 for k in range(d)]
    x = [[1.0] + [(r[k] - mean[k]) / std[k] for k in range(d)] for r in rows]
    w = [0.0] * (d + 1)
    for _ in range(rounds):
        gradient = [0.0] * (d + 1)
        hessian = [[0.0] * (d + 1) for _ in range(d + 1)]
        for xi, yi in zip(x, labels):
            z = sum(a * b for a, b in zip(w, xi))
            p = 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))
            s = max(p * (1.0 - p), 1e-6)
            for i in range(d + 1):
                gradient[i] += (yi - p) * xi[i]
                row = hessian[i]
                for j in range(i, d + 1):
                    row[j] += s * xi[i] * xi[j]
        for i in range(d + 1):
            for j in range(i):
                hessian[i][j] = hessian[j][i]
            if i > 0:  # the constant term is not held back
                hessian[i][i] += ridge
                gradient[i] -= ridge * w[i]
        step = _solve(hessian, gradient)
        w = [a + b for a, b in zip(w, step)]
        if max(abs(v) for v in step) < 1e-6:
            break
    return {"mean": mean, "std": std, "weights": w}


def chance(model, row):
    z = model["weights"][0] + sum(w * (v - m) / s for w, v, m, s in zip(model["weights"][1:], row, model["mean"], model["std"]))
    return 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))


class StopRule:
    """Per decision stage: a model and the chance below which a run is stopped."""

    def __init__(self, models, thresholds, structure_threshold, miss):
        self.models = models                  # {stage: model}
        self.thresholds = thresholds          # {stage: chance}
        self.structure_threshold = structure_threshold
        self.miss = miss

    def decision_stages(self):
        return [s for s in DECISION_STAGES if s in self.models]

    def decide(self, stages, stage):
        """`stages`: the descriptors of the run so far. Returns {"stop": bool, "expected": chance}."""
        by_stage = {d["stage"]: d for d in stages}
        row = inputs(by_stage, stage)
        if row is None or stage not in self.models:
            return {"stop": False, "expected": None}
        p = chance(self.models[stage], row)
        return {"stop": p < self.thresholds[stage], "expected": p}

    def to_json(self):
        return {"models": {str(k): v for k, v in self.models.items()}, "thresholds": {str(k): v for k, v in self.thresholds.items()},
                "structure_threshold": self.structure_threshold, "miss": self.miss, "inputs": INPUTS}

    @staticmethod
    def from_json(j):
        if not j:
            return None
        return StopRule({int(k): v for k, v in j["models"].items()}, {int(k): v for k, v in j["thresholds"].items()}, j["structure_threshold"], j["miss"])


def split_threshold(values):
    """The value that best splits a list of numbers in two groups (Otsu: the split with the
    largest spread between the two means)."""
    v = sorted(values)
    n = len(v)
    total = sum(v)
    best, at, left = -1.0, v[n // 2], 0.0
    for i in range(1, n):
        left += v[i - 1]
        between = i * (n - i) * (left / i - (total - left) / (n - i)) ** 2
        if between > best and v[i] > v[i - 1]:
            best, at = between, 0.5 * (v[i] + v[i - 1])
    return at


def work(stages, until_stage=None):
    """Simulation work of a run in cell-steps, up to a stage (or all of it)."""
    total, last = 0.0, None
    for d in stages:
        if last is not None:
            total += (d["step"] - last["step"]) * 0.5 * (d["cells"] + last["cells"])
        last = d
        if until_stage is not None and d["stage"] >= until_stage:
            break
    return total


def _fit(examples, structure_threshold, miss):
    """examples: [(by_stage, final structure)]. Thresholds: per stage the chance below which a
    third of `miss` of the forms that end with structure fall."""
    models, thresholds = {}, {}
    for stage in DECISION_STAGES:
        rows, labels = [], []
        for by_stage, final in examples:
            row = inputs(by_stage, stage)
            if row is not None:
                rows.append(row), labels.append(1.0 if final >= structure_threshold else 0.0)
        if len(rows) < 50 or sum(labels) < 10 or sum(labels) > len(labels) - 10:
            continue
        model = fit_logistic(rows, labels)
        good = sorted(chance(model, r) for r, y in zip(rows, labels) if y > 0.5)
        thresholds[stage] = good[int(miss / len(DECISION_STAGES) * len(good))] * 0.999
        models[stage] = model
    return StopRule(models, thresholds, structure_threshold, miss)


def _replay(rule, by_stage):
    """At which stage the rule would have stopped this run (None: it lets it run)."""
    for stage in rule.decision_stages():
        if stage not in by_stage:
            return None  # the run ended by itself before this decision
        if rule.decide(list(by_stage.values()), stage)["stop"]:
            return stage
    return None


def fit_and_test(runs, structure_threshold=None, miss=0.02, folds=5, seed=1):
    """`runs`: [(run id, stages)] of runs that were grown without a stop rule. Returns
    (rule fitted on all of them, report dict with the results on held-out runs)."""
    examples = []
    for run, stages in runs:
        if len(stages) < 3:
            continue
        examples.append(({d["stage"]: d for d in stages}, stages[-1].get("structure", 0.0), stages, run))
    finals = [math.log(max(e[1], 1e-4)) for e in examples]
    if structure_threshold is None:
        structure_threshold = math.exp(split_threshold(finals))
    order = list(range(len(examples)))
    random.Random(seed).shuffle(order)
    stopped = {s: {"dull": 0, "structured": 0} for s in DECISION_STAGES}
    totals = {"dull": 0, "structured": 0}
    work_all = work_saved = 0.0
    mistakes = []
    for fold in range(folds):
        held = set(order[fold::folds])
        rule = _fit([(e[0], e[1]) for i, e in enumerate(examples) if i not in held], structure_threshold, miss)
        for i in held:
            by_stage, final, stages, run = examples[i]
            kind = "structured" if final >= structure_threshold else "dull"
            totals[kind] += 1
            w = work(stages)
            work_all += w
            at = _replay(rule, by_stage)
            if at is not None:
                stopped[at][kind] += 1
                work_saved += w - work(stages, at)
                if kind == "structured":
                    mistakes.append({"run": run, "stage": at, "final_structure": final})
    rule = _fit([(e[0], e[1]) for e in examples], structure_threshold, miss)
    report = {"runs": len(examples), "structure_threshold": structure_threshold, "miss_asked": miss, "dull": totals["dull"], "structured": totals["structured"],
              "stopped": {str(s): stopped[s] for s in DECISION_STAGES},
              "dull_stopped_share": sum(stopped[s]["dull"] for s in DECISION_STAGES) / max(totals["dull"], 1),
              "structured_stopped_share": sum(stopped[s]["structured"] for s in DECISION_STAGES) / max(totals["structured"], 1),
              "work_saved_share": work_saved / work_all if work_all > 0 else 0.0, "mistakes": sorted(mistakes, key=lambda m: -m["final_structure"])[:20],
              "weights": {str(s): dict(zip(["constant"] + INPUTS, [round(w, 3) for w in rule.models[s]["weights"]])) for s in rule.models}}
    return rule, report


SETTLED_BY = ("structure", "area_ratio", "relief_total", "buried_fraction", "thickness_p50")


def settled(by_stage, stage, limit=0.1):
    """True when the scale-free numbers of the form changed by less than `limit` (as a share,
    per half decade of cells) over the last half decade before `stage`."""
    now, before = by_stage.get(stage), by_stage.get(stage - 2)
    if now is None or before is None:
        return False
    for name in SETTLED_BY:
        a, b = now.get(name, 0.0) or 0.0, before.get(name, 0.0) or 0.0
        if name == "buried_fraction":
            if abs(a - b) > limit:
                return False
        elif abs(math.log(max(a, 1e-4) / max(b, 1e-4))) > limit:
            return False
    return True
