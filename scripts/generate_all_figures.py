#!/usr/bin/env python
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="统一生成某个分析批次下的全部图表。")
    parser.add_argument(
        "analysis_dir",
        type=Path,
        help="分析批次目录，例如 storage/analysis/202649_10623",
    )
    parser.add_argument(
        "--skip-retry",
        action="store_true",
        help="跳过 retry 概率图生成",
    )
    return parser.parse_args()


def run_python_script(script_path: Path, *script_args: str) -> None:
    command = [sys.executable, str(script_path), *script_args]
    subprocess.run(command, check=True)


def resolve_retry_dir(analysis_dir: Path) -> Path | None:
    candidate_names = ["retry_probability_reports", "retry"]
    for name in candidate_names:
        candidate = analysis_dir / name
        if candidate.is_dir():
            return candidate
    return None


def main() -> None:
    args = parse_args()
    analysis_dir = args.analysis_dir.resolve()
    scripts_dir = Path(__file__).resolve().parent

    if not analysis_dir.is_dir():
        raise FileNotFoundError(f"分析目录不存在: {analysis_dir}")

    compare_summary = analysis_dir / "compare_summary_tab.txt"
    if not compare_summary.is_file():
        raise FileNotFoundError(f"缺少 compare_summary_tab.txt: {compare_summary}")

    print(f"Analysis dir: {analysis_dir}")

    run_python_script(scripts_dir / "plot_compare_summary.py", str(compare_summary))
    run_python_script(scripts_dir / "plot_foundation_fault_threshold.py", str(analysis_dir))

    if args.skip_retry:
        print("Skip retry figures.")
        return

    retry_dir = resolve_retry_dir(analysis_dir)
    if retry_dir is None:
        print("No retry directory found, skip retry figures.")
        return

    retry_reports = sorted(retry_dir.glob("*.txt"))
    if not retry_reports:
        print("No retry report files found, skip retry figures.")
        return

    for retry_report in retry_reports:
        print(f"Generate retry figures: {retry_report.name}")
        run_python_script(scripts_dir / "plot_retry_report.py", str(retry_report))


if __name__ == "__main__":
    main()
