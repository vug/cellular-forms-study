"""Plot a cf_run output directory: growth curve, step time and the final link-length histogram.

    python apps/cf_run/plot_run.py <run dir> <out.png> [title]

Reads log.csv and snapshots.jsonl. Needs matplotlib. Three small panels, one measure each.
"""
import csv
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SURFACE, INK, MUTED, GRID, SERIES = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df", "#2a78d6"


def style(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", fontsize=11, color=INK, pad=8)
    ax.set_xlabel(xlabel, fontsize=9, color=MUTED)
    ax.set_ylabel(ylabel, fontsize=9, color=MUTED)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=MUTED, labelsize=8, length=0)


def main():
    run, out = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(os.path.normpath(run))
    rows = [r for r in csv.DictReader(open(os.path.join(run, "log.csv")))]
    step = [int(r["step"]) for r in rows]
    cells = [int(r["cells"]) for r in rows]
    ms = [float(r["ms_per_step"]) for r in rows]
    snaps = [json.loads(line) for line in open(os.path.join(run, "snapshots.jsonl")) if line.strip()]
    last = max(snaps, key=lambda s: s["cells"])

    fig, axes = plt.subplots(1, 3, figsize=(13.5, 3.9), facecolor=SURFACE)
    fig.suptitle(title, x=0.012, ha="left", fontsize=12, color=INK)

    ax = axes[0]
    ax.plot(step, cells, color=SERIES, linewidth=2)
    ax.set_yscale("log")
    style(ax, "Cell count", "step", "cells (log scale)")
    ax.annotate(f"{cells[-1]:,} cells at step {step[-1]:,}", (step[-1], cells[-1]), textcoords="offset points", xytext=(-10, 4), ha="right", fontsize=9, color=INK)

    ax = axes[1]
    ax.plot(cells, ms, color=SERIES, linewidth=2)
    ax.set_xscale("log")
    ax.set_yscale("log")
    style(ax, "Step time", "cells (log scale)", "ms per step (log scale)")
    ax.annotate(f"{ms[-1]:.1f} ms at {cells[-1]:,} cells", (cells[-1], ms[-1]), textcoords="offset points", xytext=(-10, 4), ha="right", fontsize=9, color=INK)

    ax = axes[2]
    hist = last["link_hist"]
    width = 2.0 / len(hist)
    centres = [(k + 0.5) * width for k in range(len(hist))]
    total = float(sum(hist))
    ax.bar(centres, [100.0 * h / total for h in hist], width=width * 0.82, color=SERIES)
    ax.axvline(1.0, color=MUTED, linewidth=1, linestyle=(0, (3, 3)))
    ax.set_ylim(0, ax.get_ylim()[1] * 1.12)
    ax.text(0.97, ax.get_ylim()[1] * 0.94, "rest length", fontsize=8, color=MUTED, ha="right")
    style(ax, f"Link lengths at {last['cells']:,} cells", "link length / rest length", "% of links")
    ax.text(0.98, 0.80, f"mean {last['link_mean']:.3f}\nstd {last['link_std']:.3f}\nmax {last['link_max']:.2f}", transform=ax.transAxes, ha="right", va="top",
            fontsize=8.5, color=INK)

    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(out, dpi=130, facecolor=SURFACE)
    print("wrote", out)


if __name__ == "__main__":
    main()
