"""Hardware counters per book event, from `perf stat -x,` runs of replay_bench.

Each variant ran the same replay (3 passes) as the do-nothing `null` variant, so
(variant - null) / (passes * events) is the cost of the book work alone, with file
loading, histogramming and timer reads cancelled out.

usage: python analysis/perf_table.py results/perf results/linux-cpu6 [results/linux-lf-cpu6]
"""
import csv
import sys
from pathlib import Path

PASSES = 3
COLS = ["cycles", "instructions", "cache-references", "cache-misses", "L1-dcache-load-misses",
        "dTLB-load-misses", "branch-misses"]


def read_perf(path):
    out = {}
    for line in open(path):
        if not line.strip() or line.startswith("#"):
            continue
        f = line.strip().split(",")
        try:
            out[f[2]] = float(f[0])
        except (ValueError, IndexError):
            pass  # <not supported> / <not counted>
    return out


def read_latency(dirs):
    lat = {}
    for d in dirs:
        p = Path(d) / "summary.csv"
        if p.exists():
            for r in csv.DictReader(open(p)):
                if r["phase"] == "warm":
                    lat[r["variant"]] = r
    return lat


def main(perf_dir, *lat_dirs):
    perf_dir = Path(perf_dir)
    meta = next(csv.DictReader(open(Path(lat_dirs[0]) / "meta.csv")))
    n = PASSES * int(meta["events"])
    runs = {p.stem: read_perf(p) for p in sorted(perf_dir.glob("*.csv")) if p.stem != "per_event"}
    null = runs.pop("null")
    lat = read_latency(lat_dirs)
    rows = []
    for v, c in runs.items():
        row = {"variant": v}
        for k in COLS:
            if k in c and k in null:
                row[k] = (c[k] - null[k]) / n
        if "cycles" in row and "instructions" in row:
            row["ipc"] = row["instructions"] / row["cycles"]
        for q in ("p50_ns", "p99_ns", "p999_ns"):
            if v in lat:
                row[q] = float(lat[v][q])
        rows.append(row)
    rows.sort(key=lambda r: r.get("cycles", 0))
    keys = ["variant", "cycles", "instructions", "ipc", "L1-dcache-load-misses", "cache-misses",
            "dTLB-load-misses", "branch-misses", "p50_ns", "p99_ns", "p999_ns"]
    with open(perf_dir / "per_event.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    print("| " + " | ".join(keys) + " |")
    print("|" + "---|" * len(keys))
    for r in rows:
        cells = [r["variant"]] + [
            ("%.2f" % r[k] if k in ("ipc",) or (k in r and r[k] < 10) else "%.0f" % r[k]) if k in r else ""
            for k in keys[1:]
        ]
        print("| " + " | ".join(cells) + " |")
    print("\nwrote", perf_dir / "per_event.csv")


if __name__ == "__main__":
    main(*sys.argv[1:])
