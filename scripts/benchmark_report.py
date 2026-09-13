#!/usr/bin/env python3
"""Summarize captured SplatKit benchmarks; never runs an app or certifies phone performance."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys

NUMBER = r"([0-9]+(?:\.[0-9]+)?)"
START = re.compile(r"benchmark started: (\d+) splats, one turn over " + NUMBER + r" s")
FRAME = re.compile(r"benchmark: (\d+) frames, " + NUMBER + r" fps mean, frame ms mean " +
                   NUMBER + r" p50 " + NUMBER + r" p95 " + NUMBER + r" max " + NUMBER)
GPU = re.compile(r"benchmark gpu ms: mean " + NUMBER + r" p50 " + NUMBER + r" p95 " + NUMBER + r" max " + NUMBER)
TAILS = re.compile(r"benchmark tails: frame p99 " + NUMBER + r", gpu p99 " + NUMBER + r", gpu samples (\d+) of (\d+)")
SAMPLE = re.compile(NUMBER + r" fps, gpu " + NUMBER + r" ms, sort " + NUMBER + r" ms, cull " +
                    NUMBER + r" ms, select " + NUMBER + r" ms, (\d+) drawn of (\d+) selected of (\d+)")
ERROR = re.compile(r"Fatal signal|FATAL EXCEPTION|VUID-|Validation Error|VK_ERROR_|GPU frame rejected|World failed:")


def summarize(contents, environment, pid=None, minimum_seconds=0):
    if not math.isfinite(minimum_seconds) or minimum_seconds < 0:
        raise ValueError("minimum seconds must be finite and nonnegative")
    runs, active, errors = [], None, []
    for line in contents.splitlines():
        if pid is not None:
            fields = line.split(maxsplit=6)
            if len(fields) < 7 or fields[2] != str(pid):
                continue
            line = fields[6]
        if ERROR.search(line):
            errors.append(line)
        start = START.search(line)
        if start:
            if active is not None:
                runs.append(active)
            active = {"status": "incomplete", "resident_records": int(start[1]),
                      "requested_seconds": float(start[2]), "windows": [], "samples": [],
                      "frame_p99_ms": None, "gpu_p99_ms": None, "gpu_sample_count": None}
        if active is None:
            continue
        if (match := SAMPLE.search(line)) and "gpu_mean_ms" not in active:
            active["samples"].append(list(map(float, match.groups())))
        if "benchmark window:" in line:
            active["windows"].append(line[line.index("benchmark window:"):])
        if match := FRAME.search(line):
            active.update(zip(("frames", "fps_mean", "frame_mean_ms", "frame_p50_ms", "frame_p95_ms", "frame_max_ms"), map(float, match.groups())))
            active["frames"] = int(active["frames"])
        if match := GPU.search(line):
            active.update(zip(("gpu_mean_ms", "gpu_p50_ms", "gpu_p95_ms", "gpu_max_ms"), map(float, match.groups())))
        if match := TAILS.search(line):
            active["frame_p99_ms"], active["gpu_p99_ms"] = float(match[1]), float(match[2])
            active["gpu_sample_count"] = int(match[3])
            active["tails_frame_count"] = int(match[4])
    if active is not None:
        runs.append(active)
    for run in runs:
        samples = run.pop("samples")
        run["stage_samples"] = len(samples)
        run["stage_sample_mean_ms"] = {
            name: statistics.mean(row[index] for row in samples) if samples else None
            for name, index in (("sort", 2), ("cull", 3), ("select", 4))}
        run["drawn_sample_range"] = [int(min(s[5] for s in samples)), int(max(s[5] for s in samples))] if samples else None
        if "frames" not in run or "gpu_mean_ms" not in run:
            continue
        run["observed_seconds_approx"] = run["frames"] * run["frame_mean_ms"] / 1000
        run["status"] = "complete"
        if (run["frames"] <= 0 or run["frame_mean_ms"] <= 0 or
                run.get("tails_frame_count", run["frames"]) != run["frames"] or
                (run["gpu_sample_count"] is not None and run["gpu_sample_count"] > run["frames"])):
            run["status"] = "invalid"
        # Printed means have 0.1 ms precision. Duration checks allow that rounding,
        # never pretend a short capture is a sustained run.
        upper_seconds = run["observed_seconds_approx"] + run["frames"] * 0.00005
        if upper_seconds < max(minimum_seconds, run["requested_seconds"]):
            run["status"] = "too_short"
        if run["gpu_sample_count"] == 0 or run["gpu_mean_ms"] == 0:
            for key in ("gpu_mean_ms", "gpu_p50_ms", "gpu_p95_ms", "gpu_p99_ms", "gpu_max_ms"):
                run[key] = None
    return {"schema_version": 1, "status": "passed" if runs and not errors and
            all(r["status"] == "complete" for r in runs) else "failed",
            "environment": environment, "pid": pid, "minimum_seconds_per_run": minimum_seconds,
            "runs": runs, "errors": errors, "phone_performance_validated": False,
            "scope": "log completeness and summaries only; frame intervals are host-loop intervals; GPU values may lag or repeat",
            "stage_scope": "periodic samples, not all-frame averages; legacy zero stage times may mean unavailable",
            "sustained_acceptance": "requires physical provenance, matched camera path, memory/thermal traces and visual review"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--environment", required=True, choices=("physical", "emulator", "host"))
    parser.add_argument("--pid", type=int, help="required for Android threadtime captures; isolates one process")
    parser.add_argument("--minimum-seconds", type=float, default=0)
    args = parser.parse_args()
    try:
        if args.pid is not None and args.pid <= 0:
            raise ValueError("PID must be positive")
        raw = args.log.read_bytes()
        if args.pid is None and re.search(r"^\d\d-\d\d\s+[\d:.]+\s+\d+\s+\d+\s+[A-Z]\s", raw.decode(), re.M):
            raise ValueError("Android threadtime logs require --pid; do not mix processes")
        result = summarize(raw.decode(), args.environment, args.pid, args.minimum_seconds)
        result.update(log=str(args.log.resolve()), log_sha256=hashlib.sha256(raw).hexdigest())
    except (OSError, ValueError) as error:
        result = {"schema_version": 1, "status": "blocked", "error": str(error)}
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
