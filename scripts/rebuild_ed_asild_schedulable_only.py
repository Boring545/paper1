#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from plot_utils import convert_numeric_rows, parse_sectioned_tsv
from run_ed_asild_redundancy_experiment import ExperimentPoint, plot_bandwidth, plot_e2e, write_summary


@dataclass
class ParsedSummary:
    scheme_rows: list[dict]
    foundation_rows: list[dict]
    cluster_rows: list[dict]
    route_rows: list[dict]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Rebuild ED summary/figures after dropping any base dataset that is unschedulable "
            "in one or more node-redundancy configurations."
        )
    )
    parser.add_argument(
        "--analysis-map",
        nargs="+",
        required=True,
        metavar="N=DIR",
        help="Mapping from selected node-redundant signal count to C++ analysis dir, e.g. 2=storage/analysis/xxx",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory. Default: storage/analysis/<timestamp>_ed_asild_schedulable_only",
    )
    parser.add_argument(
        "--required-schemes",
        default="on_demand_tmr",
        help="Comma-separated schemes used for the all-config schedulability filter. Default: on_demand_tmr",
    )
    return parser.parse_args()


def safe_average(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def parse_analysis_map(items: list[str]) -> dict[int, Path]:
    result: dict[int, Path] = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"Invalid --analysis-map item: {item}")
        count_text, path_text = item.split("=", maxsplit=1)
        count = int(count_text)
        path = Path(path_text)
        if not (path / "algorithm2_summary_tab.txt").is_file():
            raise FileNotFoundError(f"Missing algorithm2_summary_tab.txt under {path}")
        result[count] = path
    if not result:
        raise ValueError("--analysis-map must not be empty")
    return dict(sorted(result.items()))


def parse_required_schemes(value: str) -> set[str]:
    schemes = {item.strip() for item in value.split(",") if item.strip()}
    if not schemes:
        raise ValueError("--required-schemes must not be empty")
    return schemes


def read_parsed_summary(analysis_dir: Path) -> ParsedSummary:
    sections = parse_sectioned_tsv(analysis_dir / "algorithm2_summary_tab.txt")
    return ParsedSummary(
        scheme_rows=convert_numeric_rows(sections.get("scheme_summary", []), {"dataset", "config", "scheme"}),
        foundation_rows=convert_numeric_rows(sections.get("foundation_summary", []), {"dataset", "config", "scheme"}),
        cluster_rows=convert_numeric_rows(sections.get("cluster_wcrt_summary", []), {"dataset", "config", "scheme"}),
        route_rows=convert_numeric_rows(sections.get("route_summary", []), {"dataset", "config", "scheme"}),
    )


def datasets_for_required_schemes(summary: ParsedSummary, required_schemes: set[str]) -> set[str]:
    by_dataset: dict[str, set[str]] = {}
    for row in summary.scheme_rows:
        scheme = str(row["scheme"])
        if scheme not in required_schemes:
            continue
        by_dataset.setdefault(str(row["dataset"]), set()).add(scheme)
    return {dataset for dataset, schemes in by_dataset.items() if required_schemes.issubset(schemes)}


def unschedulable_datasets(summary: ParsedSummary, required_schemes: set[str]) -> set[str]:
    bad: set[str] = set()
    for row in summary.scheme_rows:
        scheme = str(row["scheme"])
        if scheme not in required_schemes:
            continue
        if int(row.get("schedulable", 1)) != 1:
            bad.add(str(row["dataset"]))
    return bad


def build_kept_datasets(summaries: dict[int, ParsedSummary], required_schemes: set[str]) -> tuple[set[str], set[str]]:
    common: set[str] | None = None
    dropped: set[str] = set()

    for summary in summaries.values():
        datasets = datasets_for_required_schemes(summary, required_schemes)
        common = datasets if common is None else common & datasets
        dropped |= unschedulable_datasets(summary, required_schemes)

    kept = (common or set()) - dropped
    return kept, dropped


def max_cluster_e2e_by_dataset(rows: list[dict], scheme_name: str, field_name: str, kept_datasets: set[str]) -> dict[str, float]:
    result: dict[str, float] = {}
    for row in rows:
        dataset = str(row["dataset"])
        if dataset not in kept_datasets or str(row["scheme"]) != scheme_name:
            continue
        result[dataset] = max(result.get(dataset, 0.0), float(row[field_name]))
    return result


def max_route_wcrt_by_dataset(rows: list[dict], scheme_name: str, kept_datasets: set[str]) -> dict[str, float]:
    result: dict[str, float] = {}
    for row in rows:
        dataset = str(row["dataset"])
        if dataset not in kept_datasets or str(row["scheme"]) != scheme_name:
            continue
        result[dataset] = max(result.get(dataset, 0.0), float(row["max_wcrt_ms"]))
    return result


def build_point(selected_count: int, summary: ParsedSummary, kept_datasets: set[str]) -> ExperimentPoint:
    on_demand_normal: list[float] = []
    on_demand_fault: list[float] = []
    always_on_bandwidth: list[float] = []
    on_demand_failed_count = 0
    always_on_failed_count = 0

    for row in summary.scheme_rows:
        dataset = str(row["dataset"])
        if dataset not in kept_datasets:
            continue
        scheme = str(row["scheme"])
        schedulable = int(row.get("schedulable", 1)) == 1
        if scheme == "on_demand_tmr":
            if not schedulable:
                on_demand_failed_count += 1
            on_demand_normal.append(float(row["normal_bandwidth_utilization"]))
            on_demand_fault.append(float(row["fault_bandwidth_utilization"]))
        elif scheme == "always_on_tmr":
            if not schedulable:
                always_on_failed_count += 1
            always_on_bandwidth.append(float(row["normal_bandwidth_utilization"]))

    no_redundancy_bandwidth = [
        float(row["bandwidth_utilization"])
        for row in summary.foundation_rows
        if str(row["dataset"]) in kept_datasets
    ]
    no_redundancy_e2e = [
        float(row["max_wcrt_ms"])
        for row in summary.foundation_rows
        if str(row["dataset"]) in kept_datasets
    ]

    on_demand_normal_e2e = max_cluster_e2e_by_dataset(
        summary.cluster_rows, "on_demand_tmr", "end_to_end_normal_wcrt_ms", kept_datasets
    )
    on_demand_fault_e2e = max_cluster_e2e_by_dataset(
        summary.cluster_rows, "on_demand_tmr", "end_to_end_fault_wcrt_ms", kept_datasets
    )
    always_on_e2e = max_cluster_e2e_by_dataset(
        summary.cluster_rows, "always_on_tmr", "end_to_end_fault_wcrt_ms", kept_datasets
    )
    on_demand_route_wcrt = max_route_wcrt_by_dataset(summary.route_rows, "on_demand_tmr", kept_datasets)

    on_demand_total = len(on_demand_normal)
    always_on_total = len(always_on_bandwidth)
    return ExperimentPoint(
        selected_asild_count=selected_count,
        avg_on_demand_normal_bandwidth=safe_average(on_demand_normal),
        avg_on_demand_fault_bandwidth=safe_average(on_demand_fault),
        avg_always_on_bandwidth=safe_average(always_on_bandwidth),
        avg_on_demand_normal_e2e_ms=safe_average(list(on_demand_normal_e2e.values())),
        avg_on_demand_fault_e2e_ms=safe_average(list(on_demand_fault_e2e.values())),
        avg_always_on_e2e_ms=safe_average(list(always_on_e2e.values())),
        avg_on_demand_route_max_wcrt_ms=safe_average(list(on_demand_route_wcrt.values())),
        no_redundancy_bandwidth=safe_average(no_redundancy_bandwidth),
        no_redundancy_e2e_ms=safe_average(no_redundancy_e2e),
        on_demand_valid_count=max(0, on_demand_total - on_demand_failed_count),
        on_demand_failed_count=on_demand_failed_count,
        always_on_valid_count=max(0, always_on_total - always_on_failed_count),
        always_on_failed_count=always_on_failed_count,
    )


def write_dataset_filter_report(output_dir: Path, kept: set[str], dropped: set[str]) -> Path:
    path = output_dir / "ed_asild_schedulable_dataset_filter_tab.txt"
    all_datasets = sorted(kept | dropped)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(["dataset", "kept"])
        for dataset in all_datasets:
            writer.writerow([dataset, 1 if dataset in kept else 0])
    return path


def main() -> int:
    args = parse_args()
    project_root = Path(__file__).resolve().parents[1]
    output_dir = (
        args.output_dir.resolve()
        if args.output_dir is not None
        else project_root / "storage" / "analysis" / f"{datetime.now().strftime('%Y%m%d_%H%M%S')}_ed_asild_schedulable_only"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    analysis_map = parse_analysis_map(args.analysis_map)
    required_schemes = parse_required_schemes(args.required_schemes)
    summaries = {count: read_parsed_summary(path) for count, path in analysis_map.items()}
    kept_datasets, dropped_datasets = build_kept_datasets(summaries, required_schemes)
    if not kept_datasets:
        raise RuntimeError("No dataset remains after the all-config schedulability filter")

    points = [build_point(count, summary, kept_datasets) for count, summary in summaries.items()]
    baseline_bw_values = [p.no_redundancy_bandwidth for p in points if p.no_redundancy_bandwidth > 0]
    baseline_e2e_values = [p.no_redundancy_e2e_ms for p in points if p.no_redundancy_e2e_ms > 0]
    baseline_bw = min(baseline_bw_values) if baseline_bw_values else 0.0
    baseline_e2e = min(baseline_e2e_values) if baseline_e2e_values else 0.0
    points = [
        ExperimentPoint(
            selected_asild_count=p.selected_asild_count,
            avg_on_demand_normal_bandwidth=p.avg_on_demand_normal_bandwidth,
            avg_on_demand_fault_bandwidth=p.avg_on_demand_fault_bandwidth,
            avg_always_on_bandwidth=p.avg_always_on_bandwidth,
            avg_on_demand_normal_e2e_ms=p.avg_on_demand_normal_e2e_ms,
            avg_on_demand_fault_e2e_ms=p.avg_on_demand_fault_e2e_ms,
            avg_always_on_e2e_ms=p.avg_always_on_e2e_ms,
            avg_on_demand_route_max_wcrt_ms=p.avg_on_demand_route_max_wcrt_ms,
            no_redundancy_bandwidth=baseline_bw,
            no_redundancy_e2e_ms=baseline_e2e,
            on_demand_valid_count=p.on_demand_valid_count,
            on_demand_failed_count=p.on_demand_failed_count,
            always_on_valid_count=p.always_on_valid_count,
            always_on_failed_count=p.always_on_failed_count,
        )
        for p in points
    ]

    summary_path = write_summary(points, output_dir)
    bandwidth_fig = plot_bandwidth(points, output_dir, baseline_bw)
    e2e_fig = plot_e2e(points, output_dir, baseline_e2e)
    filter_report = write_dataset_filter_report(output_dir, kept_datasets, dropped_datasets)

    print(f"Output dir: {output_dir}")
    print(f"Kept datasets: {len(kept_datasets)}")
    print(f"Dropped datasets: {len(dropped_datasets)}")
    print(f"Summary: {summary_path}")
    print(f"Dataset filter report: {filter_report}")
    print(f"Bandwidth figure: {bandwidth_fig}")
    print(f"E2E figure: {e2e_fig}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
