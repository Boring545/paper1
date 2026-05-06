#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import configure_matplotlib, convert_numeric_rows, parse_sectioned_tsv


@dataclass
class ExperimentPoint:
    selected_asild_count: int
    avg_on_demand_normal_bandwidth: float
    avg_on_demand_fault_bandwidth: float
    avg_always_on_bandwidth: float
    avg_on_demand_normal_e2e_ms: float
    avg_on_demand_fault_e2e_ms: float
    avg_always_on_e2e_ms: float
    avg_on_demand_route_max_wcrt_ms: float
    no_redundancy_bandwidth: float
    no_redundancy_e2e_ms: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "ED experiment: ECU=5, signals=200, 1 dataset; select 0/2/4/6/8/10 ASIL-D signals as ECU-redundant "
            "and plot average bandwidth/E2E delay."
        )
    )
    parser.add_argument(
        "--dataset-spec",
        default="E5S200",
        help="Dataset spec passed to paper1.exe (default: E5S200)",
    )
    parser.add_argument(
        "--max-batches",
        type=int,
        default=1,
        help="Number of datasets used per point (default: 1)",
    )
    parser.add_argument(
        "--selected-counts",
        default="0,2,4,6,8,10",
        help="Comma separated ASIL-D selected counts",
    )
    parser.add_argument(
        "--exe",
        type=Path,
        default=Path("build/paper1/Debug/paper1.exe"),
        help="Path to paper1 executable",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory, default: storage/analysis/<timestamp>_ed_asild",
    )
    parser.add_argument(
        "--plot-only",
        action="store_true",
        help="Only regenerate figures from an existing summary file; do not rerun experiments.",
    )
    parser.add_argument(
        "--summary-path",
        type=Path,
        default=None,
        help="Summary tab file path used by --plot-only. "
        "Default: <output-dir>/ed_asild_redundancy_summary_tab.txt",
    )
    return parser.parse_args()


def parse_selected_counts(value: str) -> list[int]:
    counts: list[int] = []
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        counts.append(int(token))
    if not counts:
        raise ValueError("selected counts must not be empty")
    return counts


def dataset_dir_for_spec(project_root: Path, dataset_spec: str) -> Path:
    match = re.fullmatch(r"E(\d+)S(\d+)", dataset_spec.strip())
    if match is None:
        raise ValueError(f"Unsupported dataset spec format: {dataset_spec}")
    ecu = int(match.group(1))
    signals = int(match.group(2))
    return project_root / "storage" / "datasets" / f"{dataset_spec}_{ecu}ecu_{signals}signals"


def read_dataset_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        return list(reader)


def write_dataset_rows(path: Path, rows: list[dict[str, str]]) -> None:
    if not rows:
        raise ValueError(f"Empty dataset rows for {path}")
    fieldnames = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def enforce_asild_selected_count(path: Path, selected_count: int) -> int:
    rows = read_dataset_rows(path)

    primary_rows: list[tuple[int, int]] = []
    for row_index, row in enumerate(rows):
        comm_id = int(row.get("comm_id", "0"))
        level = int(row["level"])
        if comm_id == 0 and level == 3:
            primary_rows.append((int(row["code"]), row_index))

    primary_rows.sort(key=lambda item: item[0])
    chosen_indices = {row_index for _, row_index in primary_rows[:selected_count]}

    for row_index, row in enumerate(rows):
        if int(row.get("comm_id", "0")) != 0:
            continue
        row["type"] = "1" if row_index in chosen_indices else "0"

    write_dataset_rows(path, rows)
    return len(chosen_indices)


def run_algorithm2_batch(project_root: Path, exe_path: Path, dataset_spec: str, max_batches: int) -> Path:
    cmd = [
        str(exe_path.resolve()),
        "--algorithm2",
        "--run-dataset-batches",
        "--dataset-specs",
        dataset_spec,
        "--max-batches-per-spec",
        str(max_batches),
        "--skip-figures",
    ]

    result = subprocess.run(
        cmd,
        cwd=project_root,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=True,
    )

    match = re.search(r"批量测试输出目录:\s*(.+)", result.stdout)
    if match is None:
        raise RuntimeError("Cannot find analysis output dir in algorithm2 stdout")
    return Path(match.group(1).strip())


def safe_average(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def parse_algorithm2_point(analysis_dir: Path) -> tuple[float, float, float, float, float, float, float]:
    summary_path = analysis_dir / "algorithm2_summary_tab.txt"
    sections = parse_sectioned_tsv(summary_path)

    scheme_rows = convert_numeric_rows(sections.get("scheme_summary", []), {"dataset", "config", "scheme"})
    cluster_rows = convert_numeric_rows(sections.get("cluster_wcrt_summary", []), {"dataset", "config", "scheme"})
    route_rows = convert_numeric_rows(sections.get("route_summary", []), {"dataset", "config", "scheme"})

    on_demand_normal: list[float] = []
    on_demand_fault: list[float] = []
    always_on_bandwidth: list[float] = []

    for row in scheme_rows:
        scheme = row["scheme"]
        if scheme == "on_demand_tmr":
            on_demand_normal.append(float(row["normal_bandwidth_utilization"]))
            on_demand_fault.append(float(row["fault_bandwidth_utilization"]))
        elif scheme == "always_on_tmr":
            always_on_bandwidth.append(float(row["normal_bandwidth_utilization"]))

    on_demand_normal_e2e_by_dataset: dict[str, float] = {}
    on_demand_fault_e2e_by_dataset: dict[str, float] = {}
    always_on_e2e_by_dataset: dict[str, float] = {}
    on_demand_route_wcrt_by_dataset: dict[str, float] = {}

    for row in cluster_rows:
        dataset = str(row["dataset"])
        scheme = str(row["scheme"])
        normal_e2e = float(row["end_to_end_normal_wcrt_ms"])
        fault_e2e = float(row["end_to_end_fault_wcrt_ms"])

        if scheme == "on_demand_tmr":
            on_demand_normal_e2e_by_dataset[dataset] = max(on_demand_normal_e2e_by_dataset.get(dataset, 0.0), normal_e2e)
            on_demand_fault_e2e_by_dataset[dataset] = max(on_demand_fault_e2e_by_dataset.get(dataset, 0.0), fault_e2e)
        elif scheme == "always_on_tmr":
            always_on_e2e_by_dataset[dataset] = max(always_on_e2e_by_dataset.get(dataset, 0.0), fault_e2e)

    for row in route_rows:
        dataset = str(row["dataset"])
        scheme = str(row["scheme"])
        if scheme != "on_demand_tmr":
            continue
        route_wcrt = float(row["max_wcrt_ms"])
        on_demand_route_wcrt_by_dataset[dataset] = max(on_demand_route_wcrt_by_dataset.get(dataset, 0.0), route_wcrt)

    return (
        safe_average(on_demand_normal),
        safe_average(on_demand_fault),
        safe_average(always_on_bandwidth),
        safe_average(list(on_demand_normal_e2e_by_dataset.values())),
        safe_average(list(on_demand_fault_e2e_by_dataset.values())),
        safe_average(list(always_on_e2e_by_dataset.values())),
        safe_average(list(on_demand_route_wcrt_by_dataset.values())),
    )


def write_summary(points: list[ExperimentPoint], output_dir: Path) -> Path:
    summary_path = output_dir / "ed_asild_redundancy_summary_tab.txt"
    with summary_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write(
            "selected_asild_count\tavg_on_demand_normal_bandwidth\tavg_on_demand_fault_bandwidth\t"
            "avg_always_on_bandwidth\tavg_on_demand_normal_e2e_ms\tavg_on_demand_fault_e2e_ms\tavg_always_on_e2e_ms\t"
            "avg_on_demand_route_max_wcrt_ms\tno_redundancy_bandwidth\tno_redundancy_e2e_ms\n"
        )
        for p in points:
            handle.write(
                f"{p.selected_asild_count}\t{p.avg_on_demand_normal_bandwidth}\t{p.avg_on_demand_fault_bandwidth}\t"
                f"{p.avg_always_on_bandwidth}\t{p.avg_on_demand_normal_e2e_ms}\t"
                f"{p.avg_on_demand_fault_e2e_ms}\t{p.avg_always_on_e2e_ms}\t"
                f"{p.avg_on_demand_route_max_wcrt_ms}\t{p.no_redundancy_bandwidth}\t{p.no_redundancy_e2e_ms}\n"
            )
    return summary_path


def read_summary_points(path: Path) -> tuple[list[ExperimentPoint], float | None, float | None]:
    points: list[ExperimentPoint] = []
    homogeneous_bandwidth: float | None = None
    homogeneous_e2e_ms: float | None = None

    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        for row in reader:
            selected_count = int(row["selected_asild_count"])
            point = ExperimentPoint(
                selected_asild_count=selected_count,
                avg_on_demand_normal_bandwidth=float(row["avg_on_demand_normal_bandwidth"]),
                avg_on_demand_fault_bandwidth=float(row["avg_on_demand_fault_bandwidth"]),
                avg_always_on_bandwidth=float(row["avg_always_on_bandwidth"]),
                avg_on_demand_normal_e2e_ms=float(row["avg_on_demand_normal_e2e_ms"]),
                avg_on_demand_fault_e2e_ms=float(row["avg_on_demand_fault_e2e_ms"]),
                avg_always_on_e2e_ms=float(row["avg_always_on_e2e_ms"]),
                avg_on_demand_route_max_wcrt_ms=float(row.get("avg_on_demand_route_max_wcrt_ms", "0") or "0"),
                no_redundancy_bandwidth=float(row.get("no_redundancy_bandwidth", "0") or "0"),
                no_redundancy_e2e_ms=float(row.get("no_redundancy_e2e_ms", "0") or "0"),
            )
            points.append(point)

    if points and points[0].no_redundancy_bandwidth > 0:
        homogeneous_bandwidth = points[0].no_redundancy_bandwidth
    if points and points[0].no_redundancy_e2e_ms > 0:
        homogeneous_e2e_ms = points[0].no_redundancy_e2e_ms

    for p in points:
        if p.selected_asild_count == 0:
            if homogeneous_bandwidth is None:
                homogeneous_bandwidth = p.avg_on_demand_normal_bandwidth
            if homogeneous_e2e_ms is None:
                if p.avg_on_demand_route_max_wcrt_ms > 0:
                    homogeneous_e2e_ms = p.avg_on_demand_route_max_wcrt_ms
                elif p.avg_on_demand_normal_e2e_ms > 0:
                    homogeneous_e2e_ms = p.avg_on_demand_normal_e2e_ms
            break

    return points, homogeneous_bandwidth, homogeneous_e2e_ms


def plot_bandwidth(points: list[ExperimentPoint], output_dir: Path, homogeneous_bandwidth: float | None) -> Path:
    configure_matplotlib(font_size=10.0)
    filtered = [p for p in points if p.selected_asild_count != 0]
    x = [p.selected_asild_count for p in filtered]
    y_on_demand_normal = [p.avg_on_demand_normal_bandwidth for p in filtered]
    y_on_demand_fault = [p.avg_on_demand_fault_bandwidth for p in filtered]
    y_always_on = [p.avg_always_on_bandwidth for p in filtered]

    fig, ax = plt.subplots(figsize=(7.0, 4.8), constrained_layout=True)
    ax.plot(x, y_on_demand_normal, marker="o", linewidth=2.0, label="按需三模冗余-正常态", color="#1b6ca8")
    ax.plot(x, y_on_demand_fault, marker="o", linewidth=2.0, label="按需三模冗余-最坏故障态", color="#d66a1f")
    ax.plot(x, y_always_on, marker="o", linewidth=2.0, label="三模冗余", color="#2a9d5b")
    if homogeneous_bandwidth is not None:
        ax.plot(
            x,
            [homogeneous_bandwidth] * len(x),
            linewidth=2.0,
            linestyle="--",
            color="#666666",
            label="无节点冗余",
        )
    ax.set_xlabel("节点冗余信号数量")
    ax.set_ylabel("平均带宽利用率")
    ax.set_xticks(x)
    ax.legend(frameon=False)

    output_path = output_dir / "ed_bandwidth.png"
    fig.savefig(output_path)
    plt.close(fig)
    return output_path


def plot_e2e(points: list[ExperimentPoint], output_dir: Path, homogeneous_e2e_ms: float | None) -> Path:
    configure_matplotlib(font_size=10.0)
    filtered = [p for p in points if p.selected_asild_count != 0]
    x = [p.selected_asild_count for p in filtered]
    y_on_demand_normal = [p.avg_on_demand_normal_e2e_ms for p in filtered]
    y_on_demand_fault = [p.avg_on_demand_fault_e2e_ms for p in filtered]
    y_always_on = [p.avg_always_on_e2e_ms for p in filtered]

    fig, ax = plt.subplots(figsize=(7.0, 4.8), constrained_layout=True)
    ax.plot(x, y_on_demand_normal, marker="o", linewidth=2.0, label="按需三模冗余-正常态", color="#1b6ca8")
    ax.plot(x, y_on_demand_fault, marker="o", linewidth=2.0, label="按需三模冗余-最坏故障态", color="#d66a1f")
    ax.plot(x, y_always_on, marker="o", linewidth=2.0, label="三模冗余", color="#2a9d5b")
    if homogeneous_e2e_ms is not None:
        ax.plot(
            x,
            [homogeneous_e2e_ms] * len(x),
            linewidth=2.0,
            linestyle="--",
            color="#666666",
            label="无节点冗余",
        )
    ax.set_xlabel("节点冗余信号数量")
    ax.set_ylabel("平均最坏响应时间")
    ax.set_xticks(x)
    ax.legend(frameon=False)

    output_path = output_dir / "ed_e2e_delay.png"
    fig.savefig(output_path)
    plt.close(fig)
    return output_path


def main() -> int:
    args = parse_args()
    selected_counts = parse_selected_counts(args.selected_counts)

    project_root = Path(__file__).resolve().parents[1]
    if args.plot_only:
        output_dir = (
            args.output_dir.resolve()
            if args.output_dir is not None
            else (args.summary_path.resolve().parent if args.summary_path is not None else None)
        )
        if output_dir is None:
            raise ValueError("--plot-only requires --output-dir or --summary-path")
        summary_path = (
            args.summary_path.resolve()
            if args.summary_path is not None
            else output_dir / "ed_asild_redundancy_summary_tab.txt"
        )
        if not summary_path.exists():
            raise FileNotFoundError(f"Summary file not found: {summary_path}")

        points, homogeneous_bandwidth, homogeneous_e2e_ms = read_summary_points(summary_path)
        if not points:
            raise RuntimeError(f"No points in summary file: {summary_path}")

        bandwidth_fig = plot_bandwidth(points, output_dir, homogeneous_bandwidth)
        e2e_fig = plot_e2e(points, output_dir, homogeneous_e2e_ms)
        print(f"Plot-only mode output dir: {output_dir}")
        print(f"Summary: {summary_path}")
        print(f"Bandwidth figure: {bandwidth_fig}")
        print(f"E2E figure: {e2e_fig}")
        return 0

    exe_path = (project_root / args.exe).resolve()
    if not exe_path.exists():
        raise FileNotFoundError(f"paper1 executable not found: {exe_path}")

    dataset_dir = dataset_dir_for_spec(project_root, args.dataset_spec)
    if not dataset_dir.is_dir():
        raise FileNotFoundError(f"Dataset directory not found: {dataset_dir}")

    dataset_files = sorted(dataset_dir.glob("msg_*_tab.txt"))[: args.max_batches]
    if len(dataset_files) < args.max_batches:
        raise RuntimeError(f"Expected at least {args.max_batches} dataset files in {dataset_dir}")

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = (
        args.output_dir.resolve()
        if args.output_dir is not None
        else project_root / "storage" / "analysis" / f"{timestamp}_ed_asild"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    backup_dir = output_dir / "dataset_backup"
    backup_dir.mkdir(parents=True, exist_ok=True)
    for file_path in dataset_files:
        shutil.copy2(file_path, backup_dir / file_path.name)

    points: list[ExperimentPoint] = []
    realized_count_log: list[str] = []
    homogeneous_bandwidth: float | None = None
    homogeneous_e2e_ms: float | None = None

    try:
        for selected_count in selected_counts:
            per_file_realized_counts: list[int] = []
            for file_path in dataset_files:
                realized = enforce_asild_selected_count(file_path, selected_count)
                per_file_realized_counts.append(realized)

            analysis_dir = run_algorithm2_batch(project_root, exe_path, args.dataset_spec, args.max_batches)
            (
                avg_on_demand_normal_bandwidth,
                avg_on_demand_fault_bandwidth,
                avg_always_on_bandwidth,
                avg_on_demand_normal_e2e_ms,
                avg_on_demand_fault_e2e_ms,
                avg_always_on_e2e_ms,
                avg_on_demand_route_max_wcrt_ms,
            ) = parse_algorithm2_point(analysis_dir)

            if selected_count == 0:
                homogeneous_bandwidth = avg_on_demand_normal_bandwidth
                homogeneous_e2e_ms = avg_on_demand_route_max_wcrt_ms

            points.append(
                ExperimentPoint(
                    selected_asild_count=selected_count,
                    avg_on_demand_normal_bandwidth=avg_on_demand_normal_bandwidth,
                    avg_on_demand_fault_bandwidth=avg_on_demand_fault_bandwidth,
                    avg_always_on_bandwidth=avg_always_on_bandwidth,
                    avg_on_demand_normal_e2e_ms=avg_on_demand_normal_e2e_ms,
                    avg_on_demand_fault_e2e_ms=avg_on_demand_fault_e2e_ms,
                    avg_always_on_e2e_ms=avg_always_on_e2e_ms,
                    avg_on_demand_route_max_wcrt_ms=avg_on_demand_route_max_wcrt_ms,
                    no_redundancy_bandwidth=homogeneous_bandwidth or 0.0,
                    no_redundancy_e2e_ms=homogeneous_e2e_ms or 0.0,
                )
            )

            realized_count_log.append(
                f"{selected_count}\t{min(per_file_realized_counts)}\t"
                f"{max(per_file_realized_counts)}\t{safe_average([float(v) for v in per_file_realized_counts])}"
            )
            print(f"[ED] selected={selected_count}, analysis={analysis_dir}")
    finally:
        for file_path in dataset_files:
            backup_path = backup_dir / file_path.name
            if backup_path.exists():
                shutil.copy2(backup_path, file_path)

    if homogeneous_bandwidth is None:
        homogeneous_bandwidth = 0.0
    if homogeneous_e2e_ms is None:
        homogeneous_e2e_ms = 0.0
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
            no_redundancy_bandwidth=homogeneous_bandwidth,
            no_redundancy_e2e_ms=homogeneous_e2e_ms,
        )
        for p in points
    ]

    summary_path = write_summary(points, output_dir)
    bandwidth_fig = plot_bandwidth(points, output_dir, homogeneous_bandwidth)
    e2e_fig = plot_e2e(points, output_dir, homogeneous_e2e_ms)

    realized_path = output_dir / "ed_asild_realized_count_tab.txt"
    with realized_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("target_selected_count\tmin_realized\tmax_realized\tavg_realized\n")
        for line in realized_count_log:
            handle.write(line + "\n")

    print(f"Output dir: {output_dir}")
    print(f"Summary: {summary_path}")
    print(f"Bandwidth figure: {bandwidth_fig}")
    print(f"E2E figure: {e2e_fig}")
    print(f"Realized count table: {realized_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
