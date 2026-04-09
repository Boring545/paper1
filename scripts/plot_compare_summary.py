#!/usr/bin/env python
from __future__ import annotations

import argparse
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import (
    configure_matplotlib,
    convert_numeric_rows,
    dataset_sort_key,
    long_dataset_label,
    parse_sectioned_tsv,
)


SCHEME_ORDER = ["foundation", "baseline1", "baseline2"]
SCHEME_LABELS = {
    "foundation": "基础方法",
    "baseline1": "基线方法1",
    "baseline2": "基线方法2",
}
SCHEME_COLORS = {
    "foundation": "#1b6ca8",
    "baseline1": "#d66a1f",
    "baseline2": "#2a9d5b",
}
ASIL_ORDER = ["A", "B", "C", "D"]
STAT_ORDER = [("avg", "平均值"), ("max", "最大值")]
STAT_HATCH = {"avg": "", "max": "//"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="根据 compare_summary_tab.txt 生成对比图。")
    parser.add_argument("summary", type=Path, help="compare_summary_tab.txt 路径")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="输出目录，默认使用 <summary_parent>/comparison_figures",
    )
    return parser.parse_args()


def load_sections(summary_path: Path) -> dict[str, list[dict]]:
    sections = parse_sectioned_tsv(summary_path)
    return {
        name: convert_numeric_rows(rows, {"dataset", "scheme", "asil"})
        for name, rows in sections.items()
    }


def build_bandwidth_figure(rows: list[dict], output_dir: Path) -> Path:
    datasets = sorted({row["dataset"] for row in rows}, key=dataset_sort_key)
    x_positions = list(range(len(datasets)))
    labels = [long_dataset_label(name) for name in datasets]

    fig, ax = plt.subplots(figsize=(12.4, 4.8), constrained_layout=True)
    for scheme in SCHEME_ORDER:
        y_values = [
            next(
                row["compare_bandwidth_utilization"]
                for row in rows
                if row["dataset"] == dataset and row["scheme"] == scheme
            )
            for dataset in datasets
        ]
        ax.plot(
            x_positions,
            y_values,
            marker="o",
            linewidth=2.0,
            markersize=6,
            color=SCHEME_COLORS[scheme],
            label=SCHEME_LABELS[scheme],
        )

    ax.set_xticks(x_positions)
    ax.set_xticklabels(labels, rotation=15)
    ax.set_xlabel("数据集")
    ax.set_ylabel("带宽利用率")
    ax.set_title("不同数据集下的带宽利用率对比")
    ax.set_ylim(bottom=0.0)
    ax.legend(frameon=False, ncol=1, loc="upper left", bbox_to_anchor=(1.01, 0.98), borderaxespad=0.0)

    output_path = output_dir / "bandwidth_utilization.png"
    fig.savefig(output_path, bbox_inches="tight")
    plt.close(fig)
    return output_path


def build_grouped_bar_figure(
    rows: list[dict],
    output_dir: Path,
    title: str,
    y_label: str,
    avg_key: str,
    max_key: str,
    filename: str,
    yscale: str = "linear",
) -> Path:
    datasets = sorted({row["dataset"] for row in rows}, key=dataset_sort_key)
    cols = 3
    rows_count = math.ceil(len(datasets) / cols)
    fig, axes = plt.subplots(
        rows_count,
        cols,
        figsize=(17.6, 4.8 * rows_count),
        constrained_layout=True,
        squeeze=False,
    )

    positive_values = [
        value
        for row in rows
        for value in (row[avg_key], row[max_key])
        if value > 0
    ]
    log_floor = min(positive_values) / 10.0 if positive_values else 1e-20
    global_max = max((max(row[avg_key], row[max_key]) for row in rows), default=1.0)
    linear_top = global_max * 1.12 if global_max > 0 else 1.0

    centers = list(range(len(ASIL_ORDER)))
    bar_width = 0.11
    offsets = [-0.275, -0.165, -0.055, 0.055, 0.165, 0.275]

    for index, dataset in enumerate(datasets):
        ax = axes[index // cols][index % cols]
        dataset_rows = [row for row in rows if row["dataset"] == dataset]

        for scheme_index, scheme in enumerate(SCHEME_ORDER):
            scheme_rows = {row["asil"]: row for row in dataset_rows if row["scheme"] == scheme}
            for stat_index, (stat_name, stat_label) in enumerate(STAT_ORDER):
                offset = offsets[scheme_index * len(STAT_ORDER) + stat_index]
                values = []
                for asil in ASIL_ORDER:
                    source = scheme_rows[asil]
                    value = source[avg_key] if stat_name == "avg" else source[max_key]
                    if yscale == "log" and value <= 0:
                        value = log_floor
                    values.append(value)

                label = f"{SCHEME_LABELS[scheme]} {stat_label}" if index == 0 else None
                ax.bar(
                    [center + offset for center in centers],
                    values,
                    width=bar_width,
                    color=SCHEME_COLORS[scheme],
                    hatch=STAT_HATCH[stat_name],
                    edgecolor="black",
                    linewidth=0.5,
                    label=label,
                )

        ax.set_xticks(centers)
        ax.set_xticklabels(ASIL_ORDER)
        ax.set_title(long_dataset_label(dataset))
        ax.set_xlabel("ASIL等级")
        ax.set_ylabel(y_label)
        if yscale == "log":
            ax.set_yscale("log")
            ax.set_ylim(bottom=log_floor)
        else:
            ax.set_ylim(0.0, linear_top)

    for index in range(len(datasets), rows_count * cols):
        axes[index // cols][index % cols].axis("off")

    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper left", ncol=1, frameon=False, bbox_to_anchor=(1.01, 0.98))
    fig.suptitle(title, fontsize=14)

    output_path = output_dir / filename
    fig.savefig(output_path, bbox_inches="tight")
    plt.close(fig)
    return output_path


def main() -> None:
    args = parse_args()
    summary_path = args.summary.resolve()
    output_dir = args.output_dir.resolve() if args.output_dir is not None else summary_path.parent / "comparison_figures"
    output_dir.mkdir(parents=True, exist_ok=True)

    configure_matplotlib()
    sections = load_sections(summary_path)

    generated = [
        build_bandwidth_figure(sections["bandwidth_utilization"], output_dir),
        build_grouped_bar_figure(
            sections["wcrt_ratio_p95"],
            output_dir,
            title="不同数据集和ASIL等级下的WCRT/周期（P95，95%分位响应时间）对比",
            y_label="WCRT/周期比（P95）",
            avg_key="avg_wcrt_ratio_p95",
            max_key="max_wcrt_ratio_p95",
            filename="wcrt_ratio_p95.png",
            yscale="linear",
        ),
        build_grouped_bar_figure(
            sections["fault_probability"],
            output_dir,
            title="不同数据集和ASIL等级下的故障概率对比",
            y_label="故障概率",
            avg_key="avg_fault_probability",
            max_key="max_fault_probability",
            filename="fault_probability.png",
            yscale="log",
        ),
    ]

    print(f"Summary file: {summary_path}")
    print(f"Output dir: {output_dir}")
    for path in generated:
        print(f"Generated: {path}")


if __name__ == "__main__":
    main()
