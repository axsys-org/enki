#!/usr/bin/env python3
"""Compare one wisp binary with PL_JIT=0/1 (or --toggle PL_BATCH).

Copy the source tree into a fresh output directory, install P6 once per
process, and measure all six existing workloads repeatedly. No user source
or snapshot is modified. Each process gets a new snapshot and no persistent
compiler cache. The first sample of each workload is retained as warmup but
excluded from the reported medians. Counters are disabled for timings.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import time

WORKLOADS = {
    "nfib": 20,
    "listwork": 3,
    "rowwork": 3,
    "sortwork": 3,
    "bstwork": 3,
    "compwork": 300,
}


def prepare(source, output, samples):
    src = output / "src"
    shutil.copytree(source, src)
    bench = src / "reaver/plangrm-bench.rvr"
    original = bench.read_text()
    marker = "  (Seq warm\n    (begin\n      (define whit"
    start = original.index(marker)
    # Extract the existing measurement body unchanged, except for its wrapper.
    measure = original[start:].replace(
        "  (Seq warm\n    (begin\n",
        "(define (measure which iters installed)\n  (begin\n",
        1,
    ).rstrip()[:-1] + "\n"
    prefix = original[:start]
    at = prefix.index("(define (bench-main args)")
    calls = [
        f'      (measure "{name}" {iters} installed)'
        for _ in range(samples + 1)
        for name, iters in WORKLOADS.items()
    ]
    bench.write_text(
        prefix[:at] + measure + "\n" + prefix[at:]
        + "  (Seq warm\n    (begin\n" + "\n".join(calls) + ")))\n"
    )
    return src, hashlib.sha256(original.encode()).hexdigest()


def summary(records):
    groups = collections.defaultdict(list)
    for record in records:
        seen = set()
        for name, ns in record["samples"]:
            if name in seen:
                groups[record.get("mode", record["jit"]), name].append(ns / 1e6)
            seen.add(name)
    rows = []
    for name in WORKLOADS:
        off, on = groups["0", name], groups["1", name]
        if off and on:
            a, b = statistics.median(off), statistics.median(on)
            rows.append(dict(workload=name, iterations=WORKLOADS[name],
                             off_ms=a, on_ms=b, speedup=a / b,
                             interpreter_samples=len(off), jit_samples=len(on)))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--source", type=Path, default=Path("reaver/src"))
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for isolated sources, snapshots, logs and JSON")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--modes", default="0,1,1,0")
    parser.add_argument("--toggle", choices=("PL_JIT", "PL_BATCH"), default="PL_JIT")
    args = parser.parse_args()
    modes = args.modes.split(",")
    if args.samples < 1 or any(mode not in ("0", "1") for mode in modes):
        parser.error("samples must be positive; modes must contain only 0 and 1")
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    src, digest = prepare(args.source.resolve(), output, args.samples)
    metadata = dict(platform=platform.platform(), binary=str(binary),
                    binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    original_benchmark_sha256=digest, workloads=WORKLOADS,
                    warmups=1, samples=args.samples, modes=modes, toggle=args.toggle)
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    records = []
    for run, mode in enumerate(modes):
        wd = output / f"run-{run}-{mode}"
        wd.mkdir()
        env = os.environ.copy()
        env.update(PL_CODECACHE="0", PL_CODECACHE_DIR=str(output / "cache"))
        env[args.toggle] = mode
        for key in ("PL_JIT_STATS", "PL_JIT_THRESHOLD", "PL_NO_BYTECODE"):
            env.pop(key, None)
        log = output / f"run-{run}-{mode}.log"
        started = time.monotonic()
        with log.open("w") as stream:
            result = subprocess.run(
                [str(binary), "--file-root", str(src), str(src / "plan"),
                 "plangrm-bench", "main", "p6", "all", "1"],
                cwd=wd, env=env, stdout=stream, stderr=subprocess.STDOUT,
                timeout=1800,
            )
        elapsed = time.monotonic() - started
        text = log.read_text()
        samples = [(name, int(ns)) for name, ns in re.findall(
            r'"BENCH-NS" "p6" "(\w+)" "(\d+)"', text)]
        records.append(dict(run=run, mode=mode, jit=env.get("PL_JIT", "1"), wall_s=elapsed,
                            returncode=result.returncode, samples=samples))
        (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        expected = (args.samples + 1) * len(WORKLOADS)
        if result.returncode or len(samples) != expected or text.count('"BENCH-OK"') != expected:
            raise SystemExit(f"Benchmark failed; see {log}")
        print(f"{args.toggle}={mode}: {elapsed:.2f}s total, {len(samples)} checked samples", flush=True)
        rows = summary(records)
        (output / "summary.json").write_text(json.dumps(rows, indent=2) + "\n")
    for row in summary(records):
        print(f"{row['workload']:12} {row['off_ms']:9.3f} -> "
              f"{row['on_ms']:9.3f} ms  {row['speedup']:.3f}x")


if __name__ == "__main__":
    main()
