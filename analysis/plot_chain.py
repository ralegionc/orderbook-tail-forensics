"""Hash-index chain length vs latency (both OSes) and vs hardware cost per event.

usage: python analysis/plot_chain.py  ->  results/chain.png
"""
import csv
from pathlib import Path

import matplotlib.pyplot as plt

R = Path("results")
RUNS = {"Windows (native)": R / "lf-cpu6", "Linux (WSL2)": R / "linux-lf-cpu6"}


def rows(path):
    return {r["variant"]: r for r in csv.DictReader(open(path))}


fig, (a, b) = plt.subplots(1, 2, figsize=(12, 4.6))
for (label, d), marker in zip(RUNS.items(), "os"):
    ch = rows(d / "chains.csv")
    pts = sorted((float(r["mean_chain"]), float(r["p50_ns"]), float(r["p99_ns"]), v) for v, r in ch.items())
    x = [p[0] for p in pts]
    a.plot(x, [p[1] for p in pts], marker=marker, label=f"p50, {label}")
    a.plot(x, [p[2] for p in pts], marker=marker, ls="--", label=f"p99, {label}")
    for p in pts:
        if p[3] in ("map-pool-peak", "map-pool-reserve") and marker == "o":
            a.annotate(p[3].replace("map-pool-", ""), (p[0], p[2]), textcoords="offset points", xytext=(6, 4), fontsize=8)
a.set_xlabel("mean entries per lookup bucket (chain length)")
a.set_ylabel("latency per event (ns)")
a.set_yscale("log")
a.set_title("Latency follows chain length; rehashes do not matter")
a.grid(True, which="both", alpha=0.3)
a.legend(fontsize=8)

perf = rows(R / "perf" / "per_event.csv")
chain = rows(R / "linux-lf-cpu6" / "chains.csv")
pts = sorted((float(chain[v]["mean_chain"]), float(perf[v]["cache-misses"]), float(perf[v]["instructions"]), v)
             for v in chain if v in perf)
x = [p[0] for p in pts]
b.plot(x, [p[1] for p in pts], marker="o", label="cache misses / event")
b2 = b.twinx()
b2.plot(x, [p[2] for p in pts], marker="s", color="C3", label="instructions / event")
b2.set_ylim(0, 600)
b.set_ylim(0, 16)
b.set_xlabel("mean entries per lookup bucket (chain length)")
b.set_ylabel("cache misses per event")
b2.set_ylabel("instructions per event")
b.set_title("Same instructions, more misses (perf, WSL2)")
b.grid(True, alpha=0.3)
h1, l1 = b.get_legend_handles_labels()
h2, l2 = b2.get_legend_handles_labels()
b.legend(h1 + h2, l1 + l2, fontsize=8, loc="upper left")
fig.tight_layout()
fig.savefig(R / "chain.png", dpi=130)
print("wrote", R / "chain.png")
