#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import configure_matplotlib, parse_sectioned_tsv


LABEL_NO_NODE = "\u65e0\u8282\u70b9\u5197\u4f59"
LABEL_ON_DEMAND_NORMAL = "\u6309\u9700\u4e09\u6a21\u5197\u4f59-\u6b63\u5e38\u6001"
LABEL_ALWAYS_ON = "\u4e09\u6a21\u5197\u4f59"
LABEL_ON_DEMAND_FAULT = "\u6309\u9700\u4e09\u6a21\u5197\u4f59-\u6700\u574f\u6545\u969c\u6001"
LABEL_X = "\u8282\u70b9\u5197\u4f59\u4fe1\u53f7\u6570\u91cf"
LABEL_Y = "\u5e26\u5bbd\u5229\u7528\u7387"
LABEL_UNSCHED_ON_DEMAND = "\u6309\u9700\u4e0d\u53ef\u8c03\u5ea6"
LABEL_UNSCHED_ALWAYS_ON = "\u4e09\u6a21\u4e0d\u53ef\u8c03\u5ea6"


def parse_count_dir(value: str) -> tuple[int, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("selected dir must use COUNT=PATH, for example 2=storage/analysis/xxx")
    count_text, path_text = value.split("=", maxsplit=1)
    try:
        count = int(count_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid selected count: {count_text}") from exc
    return count, Path(path_text)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot per-dataset ED bandwidth curves from existing algorithm2 summary files."
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="ED experiment output directory, for example storage/analysis/20260517_193901_ed_asild",
    )
    parser.add_argument(
        "--baseline-dir",
        type=Path,
        required=True,
        help="Algorithm2 baseline analysis directory containing [foundation_summary].",
    )
    parser.add_argument(
        "--selected-dir",
        action="append",
        type=parse_count_dir,
        required=True,
        help="Selected-count analysis directory in COUNT=PATH form. Repeat for 2/4/6/8/10.",
    )
    parser.add_argument(
        "--mark-unschedulable",
        action="store_true",
        help="Mark unschedulable points with a red hollow circle. Disabled by default for clean figures.",
    )
    parser.add_argument(
        "--no-grid",
        action="store_true",
        help="Only write per-dataset figures, not the combined grid figure.",
    )
    return parser.parse_args()


def load_foundation(baseline_dir: Path) -> dict[str, dict[str, float | int]]:
    sections = parse_sectioned_tsv(baseline_dir / "algorithm2_summary_tab.txt")
    result: dict[str, dict[str, float | int]] = {}
    for row in sections.get("foundation_summary", []):
        result[row["dataset"]] = {
            "bandwidth": float(row["bandwidth_utilization"]),
            "schedulable": int(row["schedulable"]),
            "copies": int(row["total_added_signal_copies"]),
        }
    return result


def load_scheme_records(selected_dirs: dict[int, Path]) -> dict[str, dict[int, dict[str, float | int]]]:
    records: dict[str, dict[int, dict[str, float | int]]] = {}
    for selected, directory in selected_dirs.items():
        sections = parse_sectioned_tsv(directory / "algorithm2_summary_tab.txt")
        for row in sections.get("scheme_summary", []):
            dataset = row["dataset"]
            scheme = row["scheme"]
            point = records.setdefault(dataset, {}).setdefault(selected, {})
            if scheme == "on_demand_tmr":
                point["on_demand_normal"] = float(row["normal_bandwidth_utilization"])
                point["on_demand_fault"] = float(row["fault_bandwidth_utilization"])
                point["on_demand_sched"] = int(row["schedulable"])
                point["on_demand_copies"] = int(row["total_added_signal_copies"])
            elif scheme == "always_on_tmr":
                point["always_on"] = float(row["normal_bandwidth_utilization"])
                point["always_on_sched"] = int(row["schedulable"])
                point["always_on_copies"] = int(row["total_added_signal_copies"])
    return records


def write_points_table(
    path: Path,
    records: dict[str, dict[int, dict[str, float | int]]],
    foundation: dict[str, dict[str, float | int]],
    counts: list[int],
) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(
            [
                "dataset",
                "selected_count",
                "no_node_bandwidth",
                "on_demand_normal_bandwidth",
                "on_demand_fault_bandwidth",
                "always_on_bandwidth",
                "on_demand_schedulable",
                "always_on_schedulable",
                "on_demand_copies",
                "always_on_copies",
            ]
        )
        for dataset in sorted(records):
            for selected in counts:
                point = records[dataset].get(selected, {})
                writer.writerow(
                    [
                        dataset,
                        selected,
                        foundation.get(dataset, {}).get("bandwidth", ""),
                        point.get("on_demand_normal", ""),
                        point.get("on_demand_fault", ""),
                        point.get("always_on", ""),
                        point.get("on_demand_sched", ""),
                        point.get("always_on_sched", ""),
                        point.get("on_demand_copies", ""),
                        point.get("always_on_copies", ""),
                    ]
                )


def values_for_dataset(
    dataset: str,
    records: dict[str, dict[int, dict[str, float | int]]],
    foundation: dict[str, dict[str, float | int]],
    counts: list[int],
) -> tuple[list[float], list[float], list[float], list[float]]:
    no_node = float(foundation.get(dataset, {}).get("bandwidth", 0.0))
    y_no_node = [no_node for _ in counts]
    y_on_demand_normal = [float(records[dataset].get(c, {}).get("on_demand_normal", math.nan)) for c in counts]
    y_on_demand_fault = [float(records[dataset].get(c, {}).get("on_demand_fault", math.nan)) for c in counts]
    y_always_on = [float(records[dataset].get(c, {}).get("always_on", math.nan)) for c in counts]
    return y_no_node, y_on_demand_normal, y_on_demand_fault, y_always_on


def add_curves(ax, counts: list[int], y_no, y_on_normal, y_on_fault, y_always) -> None:
    ax.plot(
        counts,
        y_no,
        marker="D",
        markersize=5.0,
        markerfacecolor="white",
        markeredgecolor="black",
        linewidth=1.1,
        linestyle=":",
        color="black",
        label=LABEL_NO_NODE,
    )
    ax.plot(
        counts,
        y_on_normal,
        marker="s",
        markersize=5.5,
        markerfacecolor="white",
        markeredgecolor="black",
        linewidth=1.1,
        linestyle="-",
        color="black",
        label=LABEL_ON_DEMAND_NORMAL,
    )
    ax.plot(
        counts,
        y_always,
        marker="x",
        markersize=6.0,
        markeredgewidth=1.0,
        linewidth=1.1,
        linestyle="-.",
        color="black",
        label=LABEL_ALWAYS_ON,
    )
    ax.plot(
        counts,
        y_on_fault,
        marker="^",
        markersize=6.0,
        markerfacecolor="white",
        markeredgecolor="black",
        linewidth=1.1,
        linestyle="--",
        color="black",
        label=LABEL_ON_DEMAND_FAULT,
    )


def mark_unschedulable_points(ax, dataset: str, records: dict[str, dict[int, dict[str, float | int]]], counts: list[int]) -> None:
    for count in counts:
        point = records[dataset].get(count, {})
        if point.get("on_demand_sched") == 0:
            ax.scatter(
                [count],
                [float(point["on_demand_normal"])],
                marker="o",
                s=90,
                facecolors="none",
                edgecolors="red",
                linewidths=1.5,
                zorder=5,
            )
            ax.scatter(
                [count],
                [float(point["on_demand_fault"])],
                marker="o",
                s=90,
                facecolors="none",
                edgecolors="red",
                linewidths=1.5,
                zorder=5,
            )
        if point.get("always_on_sched") == 0:
            ax.scatter(
                [count],
                [float(point["always_on"])],
                marker="o",
                s=90,
                facecolors="none",
                edgecolors="red",
                linewidths=1.5,
                zorder=5,
            )


def unschedulable_note(dataset: str, records: dict[str, dict[int, dict[str, float | int]]], counts: list[int]) -> str:
    on_demand = [str(c) for c in counts if records[dataset].get(c, {}).get("on_demand_sched") == 0]
    always_on = [str(c) for c in counts if records[dataset].get(c, {}).get("always_on_sched") == 0]
    lines = []
    if on_demand:
        lines.append(f"{LABEL_UNSCHED_ON_DEMAND}: {','.join(on_demand)}")
    if always_on:
        lines.append(f"{LABEL_UNSCHED_ALWAYS_ON}: {','.join(always_on)}")
    return "\n".join(lines)


def plot_one_dataset(
    out_dir: Path,
    dataset: str,
    records: dict[str, dict[int, dict[str, float | int]]],
    foundation: dict[str, dict[str, float | int]],
    counts: list[int],
    mark_unschedulable: bool,
) -> Path:
    y_no, y_on_normal, y_on_fault, y_always = values_for_dataset(dataset, records, foundation, counts)
    fig, ax = plt.subplots(figsize=(7.2, 4.8), constrained_layout=True)
    add_curves(ax, counts, y_no, y_on_normal, y_on_fault, y_always)
    if mark_unschedulable:
        mark_unschedulable_points(ax, dataset, records, counts)
        note = unschedulable_note(dataset, records, counts)
        if note:
            ax.text(
                0.02,
                0.98,
                note,
                transform=ax.transAxes,
                va="top",
                ha="left",
                fontsize=8.0,
                color="red",
                bbox={"facecolor": "white", "edgecolor": "red", "alpha": 0.85},
            )
    ax.set_title(dataset)
    ax.set_xlabel(LABEL_X)
    ax.set_ylabel(LABEL_Y)
    ax.set_xticks(counts)
    ax.legend(frameon=True, fancybox=False, edgecolor="black", facecolor="white", framealpha=1.0, fontsize=8.5)
    output_path = out_dir / f"{dataset}_bandwidth.png"
    fig.savefig(output_path, dpi=300)
    plt.close(fig)
    return output_path


def plot_grid(
    out_dir: Path,
    records: dict[str, dict[int, dict[str, float | int]]],
    foundation: dict[str, dict[str, float | int]],
    counts: list[int],
    mark_unschedulable: bool,
) -> Path:
    datasets = sorted(records)
    cols = 2
    rows = math.ceil(len(datasets) / cols)
    fig, axes = plt.subplots(rows, cols, figsize=(12, max(5, rows * 3.4)), constrained_layout=True)
    flat_axes = axes.ravel() if hasattr(axes, "ravel") else [axes]
    for ax, dataset in zip(flat_axes, datasets):
        y_no, y_on_normal, y_on_fault, y_always = values_for_dataset(dataset, records, foundation, counts)
        add_curves(ax, counts, y_no, y_on_normal, y_on_fault, y_always)
        if mark_unschedulable:
            mark_unschedulable_points(ax, dataset, records, counts)
        ax.set_title(dataset, fontsize=9.0)
        ax.set_xticks(counts)
    for ax in flat_axes[len(datasets) :]:
        ax.axis("off")
    handles, labels = flat_axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=4, frameon=True)
    output_path = out_dir / "all_datasets_bandwidth_grid.png"
    fig.savefig(output_path, dpi=300)
    plt.close(fig)
    return output_path


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    out_dir = output_dir / "per_dataset_bandwidth"
    out_dir.mkdir(parents=True, exist_ok=True)

    selected_dirs = {count: path.resolve() for count, path in args.selected_dir}
    counts = sorted(selected_dirs)

    foundation = load_foundation(args.baseline_dir.resolve())
    records = load_scheme_records(selected_dirs)
    if not records:
        raise RuntimeError("No scheme_summary rows found in selected directories.")

    points_path = out_dir / "per_dataset_bandwidth_points_tab.txt"
    write_points_table(points_path, records, foundation, counts)

    configure_matplotlib(font_size=10.0)
    for dataset in sorted(records):
        plot_one_dataset(out_dir, dataset, records, foundation, counts, args.mark_unschedulable)
    if not args.no_grid:
        plot_grid(out_dir, records, foundation, counts, args.mark_unschedulable)

    print(f"Output directory: {out_dir}")
    print(f"Point table: {points_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
