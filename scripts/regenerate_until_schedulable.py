#!/usr/bin/env python3
"""Regenerate selected dataset batches until all schemes are schedulable."""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from pathlib import Path


SCHEMES = ("foundation", "baseline1", "baseline2")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def paper_exe(root: Path) -> Path:
    exe = root / "build" / "paper1" / "Debug" / "paper1.exe"
    if not exe.is_file():
        raise FileNotFoundError(f"Missing executable: {exe}")
    return exe


def run_command(cmd: list[str], cwd: Path, timeout_sec: int) -> str:
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout_sec,
    )
    output = proc.stdout.decode("utf-8", errors="replace")
    print(output, end="")
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd, output=output)
    return output


def extract_analysis_dir(output: str) -> Path | None:
    marker = "批量测试输出目录:"
    for line in output.splitlines():
        if marker in line:
            return Path(line.split(marker, 1)[1].strip())
    return None


def parse_section(path: Path, section_name: str) -> list[dict[str, str]]:
    current = None
    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    with path.open("r", encoding="utf-8", errors="ignore", newline="") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\r\n")
            if line.startswith("[") and line.endswith("]"):
                current = line[1:-1]
                header = None
                continue
            if current != section_name or not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if header is None:
                header = parts
                continue
            rows.append(dict(zip(header, parts)))
    return rows


def read_schedulability(analysis_dir: Path, dataset_tag: str) -> tuple[bool, list[dict[str, str]]]:
    summary = analysis_dir / "compare_summary_tab.txt"
    if not summary.is_file():
        raise FileNotFoundError(f"Missing summary: {summary}")
    rows = [
        row
        for row in parse_section(summary, "bandwidth_utilization")
        if row.get("dataset") == dataset_tag and row.get("scheme") in SCHEMES
    ]
    if len(rows) != len(SCHEMES):
        raise RuntimeError(f"Expected {len(SCHEMES)} scheme rows for {dataset_tag}, got {len(rows)}")
    ok = all(row.get("schedulable") == "1" for row in rows)
    return ok, rows


def dataset_tag(spec: str, index: int) -> str:
    # E8S250 -> E8S250_8ecu_250signals_002
    if not spec.startswith("E") or "S" not in spec:
        raise ValueError(f"Unsupported dataset spec format: {spec}")
    ecu_text, signal_text = spec[1:].split("S", 1)
    return f"{spec}_{int(ecu_text)}ecu_{int(signal_text)}signals_{index:03d}"


def run_once(root: Path, exe: Path, spec: str, index: int, timeout_sec: int) -> tuple[bool, Path, list[dict[str, str]]]:
    idx = str(index)
    run_command(
        [str(exe), "--generate-dataset-batches", "--dataset-specs", spec, "--batch-indices", idx],
        root,
        timeout_sec,
    )
    output = run_command(
        [str(exe), "--run-dataset-batches", "--dataset-specs", spec, "--batch-indices", idx, "--skip-figures"],
        root,
        timeout_sec,
    )
    analysis_dir = extract_analysis_dir(output)
    if analysis_dir is None:
        raise RuntimeError("Could not locate analysis output directory from program output")
    ok, rows = read_schedulability(analysis_dir, dataset_tag(spec, index))
    return ok, analysis_dir, rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-spec", required=True, help="Dataset spec, e.g. E8S250")
    parser.add_argument("--batch-index", type=int, required=True, help="1-based batch index")
    parser.add_argument("--max-attempts", type=int, default=10)
    parser.add_argument("--timeout-sec", type=int, default=900)
    args = parser.parse_args()

    root = repo_root()
    exe = paper_exe(root)
    for attempt in range(1, args.max_attempts + 1):
        print(f"[attempt {attempt}/{args.max_attempts}] spec={args.dataset_spec}, index={args.batch_index:03d}")
        ok, analysis_dir, rows = run_once(root, exe, args.dataset_spec, args.batch_index, args.timeout_sec)
        print(f"[attempt {attempt}] analysis={analysis_dir}")
        for row in rows:
            print(
                "[attempt {attempt}] {scheme}: sched={sched}, bandwidth={bw}, copies={copies}".format(
                    attempt=attempt,
                    scheme=row["scheme"],
                    sched=row["schedulable"],
                    bw=row["compare_bandwidth_utilization"],
                    copies=row["total_added_signal_copies"],
                )
            )
        if ok:
            print(f"[success] schedulable dataset found at attempt {attempt}")
            return 0
    print("[failed] no fully schedulable dataset found")
    return 1


if __name__ == "__main__":
    sys.exit(main())
