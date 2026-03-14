#!/usr/bin/env python3
"""
plot_latency.py — Parse bench_matching output and render latency histograms.

Usage:
    ./build/bench_matching | python3 scripts/plot_latency.py
    # or from a saved file:
    python3 scripts/plot_latency.py bench_output.txt

Requires: Python >= 3.10, matplotlib, numpy
    pip install -r requirements.txt
"""

import sys
import re
from typing import Dict, Tuple, List
import numpy as np
import matplotlib.pyplot as plt


def parse_benchmark_output(text: str) -> Dict[str, dict]:
    """
    Extract per-benchmark statistics and histogram buckets from the
    bench_matching stdout format.
    """
    benchmarks = {}
    current = None

    for line in text.splitlines():
        line = line.strip()

        # Detect new benchmark section.
        m = re.match(r"^=== (.+) ===$", line)
        if m:
            current = m.group(1)
            benchmarks[current] = {"stats": {}, "buckets": []}
            continue

        if current is None:
            continue

        # Parse summary statistics (e.g. "  p99     : 123.4 ns").
        m = re.match(r"(\w[\w.]+)\s*:\s*([\d.]+)\s*ns", line)
        if m:
            benchmarks[current]["stats"][m.group(1)] = float(m.group(2))
            continue

        # Parse histogram rows (e.g. "  [    50,    100) : 42").
        m = re.match(r"\[\s*([\d.]+),\s*([\d.]+)\)\s*:\s*(\d+)", line)
        if m:
            lo, hi, count = float(m.group(1)), float(m.group(2)), int(m.group(3))
            benchmarks[current]["buckets"].append((lo, hi, count))

    return benchmarks


def plot_benchmarks(benchmarks: Dict[str, dict]) -> None:
    latency_benchmarks = {k: v for k, v in benchmarks.items()
                          if v["buckets"]}

    if not latency_benchmarks:
        print("No latency histogram data found in input.")
        return

    n = len(latency_benchmarks)
    fig, axes = plt.subplots(1, n, figsize=(6 * n, 5))
    if n == 1:
        axes = [axes]

    for ax, (name, data) in zip(axes, latency_benchmarks.items()):
        buckets = data["buckets"]
        stats   = data["stats"]

        labels = [f"[{lo:.0f},{hi:.0f})" for lo, hi, _ in buckets]
        counts = [c for _, _, c in buckets]

        bars = ax.bar(range(len(counts)), counts, tick_label=labels, color="#4C72B0",
                      edgecolor="white", linewidth=0.5)
        ax.set_title(name, fontsize=11, fontweight="bold")
        ax.set_xlabel("Latency (ns)")
        ax.set_ylabel("Count")
        ax.tick_params(axis="x", rotation=45)

        # Annotate with p50/p99.
        annotation = "\n".join(
            f"{k} = {v:.1f} ns" for k, v in stats.items()
            if k in ("p50", "p99", "p99.9", "mean")
        )
        ax.text(0.97, 0.95, annotation, transform=ax.transAxes,
                fontsize=9, va="top", ha="right",
                bbox=dict(boxstyle="round,pad=0.3", fc="lightyellow", ec="gray"))

    fig.suptitle("LOB Simulator — Latency Distribution", fontsize=13, fontweight="bold")
    plt.tight_layout()

    out = "latency_histogram.png"
    plt.savefig(out, dpi=150)
    print(f"Saved: {out}")
    plt.show()


def main() -> None:
    if len(sys.argv) > 1:
        with open(sys.argv[1]) as f:
            text = f.read()
    else:
        text = sys.stdin.read()

    benchmarks = parse_benchmark_output(text)
    if not benchmarks:
        print("No benchmark sections found.  Run ./build/bench_matching first.")
        sys.exit(1)

    plot_benchmarks(benchmarks)


if __name__ == "__main__":
    main()
