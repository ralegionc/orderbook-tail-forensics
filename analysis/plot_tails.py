"""Latency survival curves from replay_bench --dump output.

usage: python analysis/plot_tails.py results/  ->  results/tails.png
"""
import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

TAGS = ["new_level", "level_gone", "rehash", "multi_level", "miss", "overflow", "pool_grow"]
REC = np.dtype([("ticks", "<u4"), ("tags", "u1"), ("type", "u1")])


def load(path, per_ns):
    rec = np.fromfile(path, dtype=REC)
    return rec["ticks"] / per_ns, rec["tags"]


def survival(ns):
    x = np.sort(ns)
    y = 1.0 - np.arange(len(x)) / len(x)
    # thin the body, keep every point in the tail
    keep = np.unique(np.concatenate([np.geomspace(1, len(x), 4000).astype(int) - 1, np.arange(max(0, len(x) - 20000), len(x))]))
    return x[keep], y[keep]


def main(out):
    out = Path(out)
    meta = next(csv.DictReader(open(out / "meta.csv")))
    per_ns = float(meta["tsc_per_ns"])
    files = sorted(out.glob("*.lat"))
    fig, (a, b) = plt.subplots(1, 2, figsize=(13, 5))
    for f in files:
        ns, _ = load(f, per_ns)
        a.loglog(*survival(ns), label=f.stem, lw=1.4)
    a.set_xlabel("latency per event (ns)")
    a.set_ylabel("fraction of events slower")
    a.set_title("All variants, last warm pass")
    a.grid(True, which="both", alpha=0.3)
    a.legend()

    # Split one variant's curve by what the event did.
    pick = out / "map-std.lat"
    if pick.exists():
        ns, tags = load(pick, per_ns)
        for bit, name in enumerate(TAGS):
            m = (tags >> bit) & 1 == 1
            if m.sum() > 1000:
                b.loglog(*survival(ns[m]), label=f"{name} ({m.mean():.2%})", lw=1.2)
        b.loglog(*survival(ns[tags == 0]), label=f"no tag ({(tags == 0).mean():.2%})", color="k", lw=1.2)
        b.set_title("map-std, split by event tag")
        b.set_xlabel("latency per event (ns)")
        b.grid(True, which="both", alpha=0.3)
        b.legend()
    fig.tight_layout()
    fig.savefig(out / "tails.png", dpi=130)
    print("wrote", out / "tails.png")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "results")
