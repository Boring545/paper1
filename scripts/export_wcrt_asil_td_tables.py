#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

from plot_utils import dataset_config_name, dataset_sort_key, resolve_compare_dir


TARGET_SCHEMES = ("foundation", "baseline1")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export WCRT tables grouped by ASIL, period(T), deadline(D) from signal_detail."
    )
    parser.add_argument("input_path", type=Path, help="Analysis directory or comparison_reports directory")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory, defaults to <analysis_batch>/comparison_figures/wcrt_asil_td_tables",
    )
    return parser.parse_args()


def parse_section_tsv(path: Path, section_name: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    current_section = None
    header = None
    with path.open("r", encoding="utf-8", newline="") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                current_section = line[1:-1]
                header = None
                continue
            if current_section != section_name:
                continue
            if header is None:
                header = line.split("\t")
                continue
            values = next(csv.reader([line], delimiter="\t"))
            if len(values) != len(header):
                raise ValueError(f"Malformed row in {path}: {line}")
            rows.append(dict(zip(header, values)))
    return rows


def load_schedulable_pairs(summary_path: Path) -> set[tuple[str, str]]:
    allowed: set[tuple[str, str]] = set()
    for row in parse_section_tsv(summary_path, "bandwidth_utilization"):
        scheme = row["scheme"]
        if scheme not in TARGET_SCHEMES:
            continue
        if row.get("schedulable") == "1":
            allowed.add((row["dataset"], scheme))
    return allowed


def load_grouped_rows(compare_dir: Path, allowed_pairs: set[tuple[str, str]]) -> dict[tuple[str, str], list[tuple[str, int, int, float]]]:
    grouped: dict[tuple[str, str], list[tuple[str, int, int, float]]] = defaultdict(list)
    compare_files = sorted(compare_dir.glob("*.txt"), key=lambda p: dataset_sort_key(p.stem))

    for compare_file in compare_files:
        dataset_tag = compare_file.stem
        config_tag = dataset_config_name(dataset_tag)
        signal_rows = parse_section_tsv(compare_file, "signal_detail")
        for row in signal_rows:
            scheme = row["scheme"]
            if scheme not in TARGET_SCHEMES:
                continue
            if (dataset_tag, scheme) not in allowed_pairs:
                continue
            asil = row["asil"]
            period_ms = int(row["period_ms"])
            deadline_ms = int(row["period_ms"]) if "deadline_ms" not in row else int(row["deadline_ms"])
            wcrt = float(row["threshold_wcrt_ms"])
            grouped[(config_tag, scheme)].append((asil, period_ms, deadline_ms, wcrt))

    return grouped


def export_tables(output_dir: Path, grouped_rows: dict[tuple[str, str], list[tuple[str, int, int, float]]]) -> list[Path]:
    generated: list[Path] = []

    for (config_tag, scheme), records in sorted(grouped_rows.items(), key=lambda kv: (dataset_sort_key(kv[0][0]), kv[0][1])):
        bucket: dict[tuple[str, int, int], list[float]] = defaultdict(list)
        for asil, period_ms, deadline_ms, wcrt in records:
            bucket[(asil, period_ms, deadline_ms)].append(wcrt)

        output_path = output_dir / f"{config_tag}_{scheme}_ASIL_T_D_WCRT_tab.txt"
        with output_path.open("w", encoding="utf-8", newline="") as handle:
            handle.write("# Grouped by ASIL, T(period_ms), D(deadline_ms); WCRT is average threshold_wcrt_ms.\n")
            handle.write("# Only schedulable dataset+scheme samples are included.\n\n")
            handle.write("ASIL\tT\tD\tWCRT\n")
            for (asil, t_val, d_val), wcrts in sorted(bucket.items(), key=lambda x: (x[0][0], x[0][1], x[0][2])):
                avg_wcrt = sum(wcrts) / len(wcrts)
                handle.write(f"{asil}\t{t_val}\t{d_val}\t{avg_wcrt}\n")

        generated.append(output_path)

    return generated


def main() -> None:
    args = parse_args()
    compare_dir = resolve_compare_dir(args.input_path)
    summary_path = compare_dir.parent / "compare_summary_tab.txt"
    if not summary_path.is_file():
        raise FileNotFoundError(f"Missing compare_summary_tab.txt: {summary_path}")

    output_dir = (
        args.output_dir.resolve()
        if args.output_dir is not None
        else compare_dir.parent / "comparison_figures" / "wcrt_asil_td_tables"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    allowed_pairs = load_schedulable_pairs(summary_path)
    grouped_rows = load_grouped_rows(compare_dir, allowed_pairs)
    generated = export_tables(output_dir, grouped_rows)

    print(f"Compare dir: {compare_dir}")
    print(f"Output dir: {output_dir}")
    print(f"Schedulable dataset+scheme pairs: {len(allowed_pairs)}")
    for path in generated:
        print(f"Generated: {path}")


if __name__ == "__main__":
    main()
