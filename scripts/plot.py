#!/usr/bin/env python3
"""Plot results/<tag>_scaling.csv and results/<tag>_granularity.csv.

usage: scripts/plot.py <tag> [--pcores N]
Writes results/<tag>_speedup.png and results/<tag>_granularity.png
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
tag = sys.argv[1] if len(sys.argv) > 1 else "mac"
pcores = int(sys.argv[sys.argv.index("--pcores") + 1]) if "--pcores" in sys.argv else None

# Categorical slots 1-3 of the reference palette, fixed per implementation.
COLOR = {"ws": "#2a78d6", "cq": "#eb6834", "omp": "#1baf7a"}
LABEL = {"ws": "work stealing (ours)", "cq": "central queue (ours)", "omp": "OpenMP tasks (libgomp)"}
MARK = {"ws": "o", "cq": "s", "omp": "^"}
INK, MUTED, GRID = "#0b0b0b", "#52514e", "#e4e3df"

plt.rcParams.update({
    "font.size": 10, "axes.edgecolor": MUTED, "axes.labelcolor": INK, "xtick.color": MUTED,
    "ytick.color": MUTED, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8, "figure.facecolor": "white",
})


def load(name):
    with open(ROOT / "results" / name) as f:
        return list(csv.DictReader(f))


# ---------------------------------------------------------------- speedup
rows = load(f"{tag}_scaling.csv")
by = defaultdict(dict)  # (bench, impl) -> {threads: median}
for r in rows:
    by[(r["bench"], r["impl"])][int(r["threads"])] = float(r["median_s"])

benches = ["fib", "qsort", "nqueens", "uts"]
titles = {
    "fib": "fib(35), task per call",
    "qsort": "quicksort, 20M ints",
    "nqueens": "n-queens, n=14",
    "uts": "UTS binomial, 2.9M nodes",
}
fig, axes = plt.subplots(2, 2, figsize=(9, 7), sharex=True)
for ax, b in zip(axes.flat, benches):
    serial = by[(b, "serial")][1]
    # GCC turns serial fib into something ~30x faster than any task version, so
    # for fib we measure against our own 1-thread run instead.
    base = by[(b, "ws")][1] if b == "fib" else serial
    tmax = max(by[(b, "ws")])
    ax.plot([1, tmax], [1, tmax], color=MUTED, lw=1, ls=":", label="ideal")
    for impl in ["ws", "cq", "omp"]:
        d = by[(b, impl)]
        ts = sorted(d)
        ax.plot(ts, [base / d[t] for t in ts], color=COLOR[impl], lw=2, marker=MARK[impl], ms=6,
                label=LABEL[impl])
    if pcores:
        ax.axvline(pcores, color=GRID, lw=1.5, zorder=0)
        ax.text(pcores + 0.15, tmax + 0.3, "efficiency cores →", color=MUTED, fontsize=8, va="top")
    ax.set_title(titles[b], color=INK, fontsize=11, loc="left")
    ax.set_ylim(0, tmax + 0.5)
    ax.set_ylabel("speedup vs. 1-thread work stealing" if b == "fib" else "speedup vs. serial")
for ax in axes[1]:
    ax.set_xlabel("threads")
h, l = axes[0, 0].get_legend_handles_labels()
fig.legend(h, l, loc="upper center", ncol=4, frameon=False, bbox_to_anchor=(0.5, 1.0))
fig.tight_layout(rect=(0, 0, 1, 0.95))
fig.savefig(ROOT / "results" / f"{tag}_speedup.png", dpi=160)
print("wrote", f"results/{tag}_speedup.png")

# ---------------------------------------------------------------- granularity
rows = load(f"{tag}_granularity.csv")
g = defaultdict(dict)
nthreads = None
for r in rows:
    g[r["impl"]][int(r["cutoff"])] = float(r["median_s"])
    if r["impl"] != "serial":
        nthreads = r["threads"]
fig, ax = plt.subplots(figsize=(6.5, 4))
cs = sorted(g["serial"])
ax.plot(cs, [g["serial"][c] for c in cs], color=MUTED, lw=1.5, ls="--", label="serial")
for impl in ["ws", "cq", "omp"]:
    ax.plot(cs, [g[impl][c] for c in cs], color=COLOR[impl], lw=2, marker=MARK[impl], ms=6,
            label=f"{LABEL[impl]}, {nthreads} threads")
ax.set_yscale("log")
ax.set_xlabel("cutoff (calls with n ≤ cutoff run serially)")
ax.set_ylabel("time (s, log scale)")
ax.set_title("fib(35): runtime vs. task granularity", color=INK, fontsize=11, loc="left")
ax.legend(frameon=False, fontsize=9)
fig.tight_layout()
fig.savefig(ROOT / "results" / f"{tag}_granularity.png", dpi=160)
print("wrote", f"results/{tag}_granularity.png")
