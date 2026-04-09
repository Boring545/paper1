#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import (
    configure_matplotlib,
    dataset_sort_key,
    resolve_compare_dir,
    short_dataset_label,
)


ASIL_ORDER = ["A", "B", "C", "D"]
ASIL_COLORS = {
    "A": "#4f5d75",
    "B": "#6b7280",
    "C": "#7c8577",
    "D": "#7a5c45",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="绘制基础方法下最坏信号故障概率与阈值对比图。")
    parser.add_argument("input_path", type=Path, help="分析批次目录，或其中的 comparison_reports / compare 目录")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="输出目录，默认 <analysis_batch>/comparison_figures",
    )
    return parser.parse_args()


def parse_signal_detail(compare_file: Path) -> list[dict]:
    current_section = None
    header = None
    rows: list[dict] = []

    with compare_file.open("r", encoding="utf-8", newline="") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                current_section = line[1:-1]
                header = None
                continue
            if current_section != "signal_detail":
                continue
            if header is None:
                header = line.split("\t")
                continue
            row = next(csv.reader([line], delimiter="\t"))
            rows.append(dict(zip(header, row)))

    return rows


def collect_worst_points(compare_dir: Path) -> dict[str, dict[str, dict]]:
    result: dict[str, dict[str, dict]] = {}
    compare_files = sorted(compare_dir.glob("*.txt"), key=lambda path: dataset_sort_key(path.stem))

    for compare_file in compare_files:
        dataset_name = compare_file.stem
        signal_rows = parse_signal_detail(compare_file)
        foundation_rows = [row for row in signal_rows if row["scheme"] == "foundation"]
        result[dataset_name] = {}

        for asil in ASIL_ORDER:
            asil_rows = [row for row in foundation_rows if row["asil"] == asil]
            if not asil_rows:
                result[dataset_name][asil] = {"code": "", "p_fault": 0.0, "p_threshold": 0.0, "ratio": 0.0}
                continue

            best_row = None
            best_ratio = -1.0
            for row in asil_rows:
                p_fault = float(row["p_fault"])
                p_threshold = float(row["p_threshold"])
                ratio = p_fault / p_threshold if p_threshold > 0 else math.inf
                if ratio > best_ratio:
                    best_ratio = ratio
                    best_row = row

            assert best_row is not None
            result[dataset_name][asil] = {
                "code": best_row["code"],
                "p_fault": float(best_row["p_fault"]),
                "p_threshold": float(best_row["p_threshold"]),
                "ratio": best_ratio,
            }

    return result


def write_summary_table(output_dir: Path, dataset_order: list[str], points: dict[str, dict[str, dict]]) -> Path:
    output_path = output_dir / "foundation_fault_probability_vs_threshold_tab.txt"
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# foundation 方法下各数据集、各 ASIL 组的最坏信号故障概率与对应阈值\n")
        handle.write("# 最坏信号定义：在该数据集和 ASIL 组内，使 p_fault / p_threshold 最大的信号\n\n")
        handle.write("dataset\tasil\tcode\tmax_p_fault\tp_threshold\tfault_to_threshold_ratio\n")
        for dataset_name in dataset_order:
            for asil in ASIL_ORDER:
                item = points[dataset_name][asil]
                handle.write(
                    f"{short_dataset_label(dataset_name)}\t{asil}\t{item['code']}\t"
                    f"{item['p_fault']}\t{item['p_threshold']}\t{item['ratio']}\n"
                )
    return output_path


def plot_fault_vs_threshold(output_dir: Path, dataset_order: list[str], points: dict[str, dict[str, dict]]) -> Path:
    configure_matplotlib(font_size=10.5)
    x_labels = [short_dataset_label(name) for name in dataset_order]
    x_positions = list(range(len(dataset_order)))

    fig, axes = plt.subplots(2, 2, figsize=(13.2, 8.2), constrained_layout=True, squeeze=False)

    positive_values = []
    for dataset_name in dataset_order:
        for asil in ASIL_ORDER:
            item = points[dataset_name][asil]
            if item["p_fault"] > 0:
                positive_values.append(item["p_fault"])
            if item["p_threshold"] > 0:
                positive_values.append(item["p_threshold"])
    log_floor = min(positive_values) / 10.0 if positive_values else 1e-20

    for index, asil in enumerate(ASIL_ORDER):
        ax = axes[index // 2][index % 2]
        fault_values = []
        threshold_values = []
        for dataset_name in dataset_order:
            item = points[dataset_name][asil]
            fault_values.append(item["p_fault"] if item["p_fault"] > 0 else log_floor)
            threshold_values.append(item["p_threshold"] if item["p_threshold"] > 0 else log_floor)

        ax.bar(
            x_positions,
            fault_values,
            width=0.55,
            color=ASIL_COLORS[asil],
            edgecolor="black",
            linewidth=0.6,
            label="最坏信号故障概率" if index == 0 else None,
        )
        ax.plot(
            x_positions,
            threshold_values,
            color="black",
            linestyle="--",
            linewidth=1.6,
            label="对应阈值" if index == 0 else None,
        )

        ax.set_yscale("log")
        ax.set_ylim(bottom=log_floor)
        ax.set_xticks(x_positions)
        ax.set_xticklabels(x_labels)
        ax.set_xlabel("数据集")
        ax.set_ylabel("故障概率")
        ax.set_title(f"ASIL {asil}")

    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=1, loc="upper left", bbox_to_anchor=(1.01, 0.96))
    fig.suptitle("基础方法在不同数据集和ASIL等级下的最坏信号故障概率与阈值对比", fontsize=14)

    output_path = output_dir / "foundation_fault_probability_vs_threshold.png"
    fig.savefig(output_path, bbox_inches="tight")
    plt.close(fig)
    return output_path


def main() -> None:
    args = parse_args()
    compare_dir = resolve_compare_dir(args.input_path)
    output_dir = args.output_dir.resolve() if args.output_dir is not None else compare_dir.parent / "comparison_figures"
    output_dir.mkdir(parents=True, exist_ok=True)

    points = collect_worst_points(compare_dir)
    dataset_order = sorted(points.keys(), key=dataset_sort_key)

    generated = [
        write_summary_table(output_dir, dataset_order, points),
        plot_fault_vs_threshold(output_dir, dataset_order, points),
    ]

    print(f"Compare dir: {compare_dir}")
    print(f"Output dir: {output_dir}")
    for path in generated:
        print(f"Generated: {path}")


if __name__ == "__main__":
    main()
