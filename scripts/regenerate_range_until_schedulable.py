#!/usr/bin/env python3
"""Regenerate a range of dataset batches in parallel until each is schedulable."""

from __future__ import annotations

import argparse
import concurrent.futures
import subprocess
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def run_one(root: Path, spec: str, index: int, max_attempts: int, timeout_sec: int) -> tuple[int, bool, str]:
    cmd = [
        sys.executable,
        str(root / "scripts" / "regenerate_until_schedulable.py"),
        "--dataset-spec",
        spec,
        "--batch-index",
        str(index),
        "--max-attempts",
        str(max_attempts),
        "--timeout-sec",
        str(timeout_sec),
    ]
    proc = subprocess.run(
        cmd,
        cwd=str(root),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = proc.stdout.decode("utf-8", errors="replace")
    return index, proc.returncode == 0, output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-spec", required=True)
    parser.add_argument("--start-index", type=int, required=True)
    parser.add_argument("--end-index", type=int, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--max-attempts", type=int, default=20)
    parser.add_argument("--timeout-sec", type=int, default=900)
    args = parser.parse_args()

    if args.start_index <= 0 or args.end_index < args.start_index:
        raise ValueError("Invalid index range")
    if args.jobs <= 0:
        raise ValueError("--jobs must be positive")

    root = repo_root()
    indices = list(range(args.start_index, args.end_index + 1))
    failed: list[int] = []

    print(
        f"[range] spec={args.dataset_spec}, indices={args.start_index:03d}..{args.end_index:03d}, "
        f"jobs={args.jobs}, max_attempts={args.max_attempts}"
    )

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_map = {
            executor.submit(run_one, root, args.dataset_spec, index, args.max_attempts, args.timeout_sec): index
            for index in indices
        }
        for future in concurrent.futures.as_completed(future_map):
            index, ok, output = future.result()
            print(f"\n===== batch {index:03d} {'SUCCESS' if ok else 'FAILED'} =====")
            print(output, end="" if output.endswith("\n") else "\n")
            if not ok:
                failed.append(index)

    if failed:
        print("[range] failed indices: " + ",".join(f"{index:03d}" for index in sorted(failed)))
        return 1

    print(f"[range] all {len(indices)} batches schedulable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
