#!/usr/bin/env python
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate only bandwidth and signal-WCRT-CDF figures for one analysis batch.")
    parser.add_argument("analysis_dir", type=Path, help="Analysis directory, e.g. storage/analysis/202651_111242")
    return parser.parse_args()


def run_python_script(script_path: Path, *script_args: str) -> None:
    command = [sys.executable, str(script_path), *script_args]
    subprocess.run(command, check=True)


def main() -> None:
    args = parse_args()
    analysis_dir = args.analysis_dir.resolve()
    scripts_dir = Path(__file__).resolve().parent

    if not analysis_dir.is_dir():
        raise FileNotFoundError(f"Analysis directory not found: {analysis_dir}")

    compare_summary = analysis_dir / "compare_summary_tab.txt"
    if not compare_summary.is_file():
        raise FileNotFoundError(f"Missing compare_summary_tab.txt: {compare_summary}")

    print(f"Analysis dir: {analysis_dir}")
    run_python_script(scripts_dir / "plot_compare_summary.py", str(compare_summary), "--only", "bandwidth-table")
    run_python_script(scripts_dir / "plot_signal_wcrt_cdf.py", str(analysis_dir))
    run_python_script(scripts_dir / "redraw_bandwidth_and_cdf_from_tabs.py", str(analysis_dir))


if __name__ == "__main__":
    main()
