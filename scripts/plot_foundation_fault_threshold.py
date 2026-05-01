#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import ticker as mticker

from plot_utils import configure_matplotlib, dataset_dimensions, dataset_sort_key, group_datasets_by_ecu, resolve_compare_dir


ASIL_ORDER = ["B", "C", "D"]
ASIL_COLORS = {
    "B": "#6b7280",
    "C": "#7c8577",
    "D": "#7a5c45",
}
ASIL_THRESHOLDS = {
    "B": 1e-7,
    "C": 1e-7,
    "D": 1e-8,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot foundation fault probability against thresholds.")
    parser.add_argument("input_path", type=Path, help="Analysis directory or comparison_reports directory")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory, defaults to <analysis_batch>/comparison_figures",
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

            best_row = max(asil_rows, key=lambda row: float(row["p_fault"]))
            result[dataset_name][asil] = {
                "code": best_row["code"],
                "p_fault": float(best_row["p_fault"]),
                "p_threshold": ASIL_THRESHOLDS[asil],
                "ratio": float(best_row["p_fault"]) / ASIL_THRESHOLDS[asil],
            }

    return result


def write_summary_table(output_dir: Path, dataset_order: list[str], points: dict[str, dict[str, dict]]) -> Path:
    output_path = output_dir / "foundation_fault_probability_vs_threshold_tab.txt"
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# Worst foundation-scheme fault probability by dataset and ASIL group.\n")
        handle.write("# Only ASIL B/C/D are shown because ASIL A does not carry a useful threshold here.\n\n")
        handle.write("dataset\til\tcode\tmax_p_fault\tp_threshold\tfault_to_threshold_ratio\n".replace("\til\t", "\tasil\t"))
        for dataset_name in dataset_order:
            for asil in ASIL_ORDER:
                item = points[dataset_name][asil]
                handle.write(
                    f"{dataset_name}\t{asil}\t{item['code']}\t{item['p_fault']}\t"
                    f"{item['p_threshold']}\t{item['ratio']}\n"
                )
    return output_path


def plot_fault_vs_threshold(output_dir: Path, dataset_order: list[str], points: dict[str, dict[str, dict]]) -> Path:
    configure_matplotlib(font_size=10.0)
    dataset_groups = group_datasets_by_ecu(dataset_order)

    fig, axes = plt.subplots(
        1,
        len(dataset_groups),
        figsize=(5.6 * len(dataset_groups), 4.8),
        sharey=False,
        squeeze=False,
    )
    flat_axes = list(axes[0])
    legend_handles = []
    legend_labels = []

    bar_width = 0.18
    offsets = [-0.22, 0.0, 0.22]

    for axis_index, (ecu_count, group_datasets) in enumerate(dataset_groups):
        ax = flat_axes[axis_index]
        threshold_ax = ax.twinx()
        x_positions = list(range(len(group_datasets)))
        x_labels = [str(dataset_dimensions(dataset)[1]) for dataset in group_datasets]
        axis_fault_values = []
        axis_threshold_values = []

        for asil_index, asil in enumerate(ASIL_ORDER):
            fault_values = []
            for dataset_name in group_datasets:
                p_fault = points[dataset_name][asil]["p_fault"]
                if p_fault > 0:
                    fault_values.append(p_fault)
                    axis_fault_values.append(p_fault)
                else:
                    fault_values.append(1e-20)
            axis_threshold_values.append(ASIL_THRESHOLDS[asil])

            label = f"ASIL {asil} fault" if axis_index == 0 else None
            bar = ax.bar(
                [x + offsets[asil_index] for x in x_positions],
                fault_values,
                width=bar_width,
                color=ASIL_COLORS[asil],
                edgecolor="black",
                linewidth=0.6,
                label=label,
            )
            line = threshold_ax.hlines(
                ASIL_THRESHOLDS[asil],
                x_positions[0] - 0.5,
                x_positions[-1] + 0.5,
                color=ASIL_COLORS[asil],
                linestyle="--",
                linewidth=1.2,
                label=f"ASIL {asil} threshold" if axis_index == 0 else None,
            )
            if axis_index == 0:
                legend_handles.extend([line, bar[0]])
                legend_labels.extend([f"ASIL {asil} threshold", f"ASIL {asil} fault"])

        ax.set_yscale("log")
        fault_min = min(axis_fault_values) / 3.0 if axis_fault_values else 1e-20
        fault_max = max(axis_fault_values) * 3.0 if axis_fault_values else 1e-16
        ax.set_ylim(bottom=fault_min, top=fault_max)
        ax.yaxis.set_major_locator(mticker.LogLocator(base=10.0, numticks=12))
        ax.yaxis.set_minor_locator(mticker.LogLocator(base=10.0, subs=tuple(range(2, 10)), numticks=100))
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())
        ax.grid(True, axis="y", which="major", linestyle="--", linewidth=0.6, alpha=0.4)
        ax.grid(True, axis="y", which="minor", linestyle=":", linewidth=0.4, alpha=0.25)

        threshold_ax.set_yscale("log")
        threshold_min = min(axis_threshold_values) / 3.0 if axis_threshold_values else 1e-9
        threshold_max = max(axis_threshold_values) * 3.0 if axis_threshold_values else 1e-6
        threshold_ax.set_ylim(bottom=threshold_min, top=threshold_max)
        threshold_ax.grid(False)
        threshold_ax.spines["top"].set_visible(False)
        threshold_ax.set_ylabel("ASIL Threshold")

        ax.set_xticks(x_positions)
        ax.set_xticklabels(x_labels)
        ax.set_title(f"{ecu_count} ECU")
        ax.tick_params(axis="x", labelsize=9)
        ax.set_xlabel("Signal Count")
        ax.set_ylabel("Fault Probability")

    fig.legend(legend_handles, legend_labels, frameon=False, ncol=2, loc="lower center", bbox_to_anchor=(0.5, 0.03))
    fig.suptitle("Foundation Worst-Signal Fault Probability by ECU Group", fontsize=14, y=0.96)
    fig.subplots_adjust(top=0.82, bottom=0.24, wspace=0.08)

    output_path = output_dir / "foundation_fault_probability_vs_threshold.png"
    fig.savefig(output_path)
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
