#!/usr/bin/env python
from __future__ import annotations

import argparse
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.axes import Axes

from plot_utils import configure_matplotlib, convert_numeric_rows, dataset_dimensions, group_datasets_by_ecu, parse_sectioned_tsv


SCHEME_ORDER = ["foundation", "baseline1", "baseline2"]
SCHEME_LABELS = {
    "foundation": "信号备份",
    "baseline1": "报文备份",
    "baseline2": "重传",
}
SCHEME_COLORS = {
    "foundation": "#1b6ca8",
    "baseline1": "#d66a1f",
    "baseline2": "#2a9d5b",
}

WCRT_SCHEME_ORDER = ["foundation_no_offset", "baseline1_no_offset", "foundation", "baseline1"]
WCRT_SCHEME_LABELS = {
    "foundation_no_offset": "Signal Backup",
    "baseline1_no_offset": "Frame Backup",
    "foundation": "Signal Backup + Offset",
    "baseline1": "Frame Backup + Offset",
}
WCRT_SCHEME_COLORS = {
    "foundation_no_offset": "#1b6ca8",
    "baseline1_no_offset": "#d66a1f",
    "foundation": "#0f4c81",
    "baseline1": "#a34712",
}
WCRT_SCHEME_STYLES = {
    "foundation_no_offset": "--",
    "baseline1_no_offset": "--",
    "foundation": "-",
    "baseline1": "-",
}

ASIL_ORDER = ["A", "B", "C", "D"]
ASIL_THRESHOLDS = {
    "A": None,
    "B": 1e-7,
    "C": 1e-7,
    "D": 1e-8,
}
PERIOD_ORDER = [1, 2, 5, 10, 20, 50, 100, 1000]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate configuration-level comparison figures from compare_summary_tab.txt.")
    parser.add_argument("summary", type=Path, help="Path to compare_summary_tab.txt")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory, defaults to <summary_parent>/comparison_figures",
    )
    parser.add_argument(
        "--only",
        choices=["all", "bandwidth", "bandwidth-line", "bandwidth-bar"],
        default="all",
        help="Restrict generated outputs to a subset.",
    )
    return parser.parse_args()


def load_sections(summary_path: Path) -> dict[str, list[dict]]:
    sections = parse_sectioned_tsv(summary_path)
    return {
        name: convert_numeric_rows(rows, {"dataset", "config", "scheme", "asil"})
        for name, rows in sections.items()
    }


def to_axes_grid(axes) -> list[Axes]:
    if isinstance(axes, Axes):
        return [axes]
    flat_axes: list[Axes] = []
    for row in axes:
        if isinstance(row, Axes):
            flat_axes.append(row)
        else:
            flat_axes.extend(list(row))
    return flat_axes


def safe_average(values: list[float]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


def aggregate_bandwidth(rows: list[dict]) -> dict[int, dict[int, dict[str, float]]]:
    grouped: dict[int, dict[int, dict[str, list[float]]]] = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for row in rows:
        if int(row.get("schedulable", 1)) != 1:
            continue
        ecu_count, signal_count = dataset_dimensions(row["config"])
        if ecu_count is None or signal_count is None:
            continue
        grouped[ecu_count][signal_count][row["scheme"]].append(float(row["compare_bandwidth_utilization"]))

    aggregated: dict[int, dict[int, dict[str, float]]] = defaultdict(dict)
    for ecu_count, signal_map in grouped.items():
        for signal_count, scheme_map in signal_map.items():
            aggregated[ecu_count][signal_count] = {
                scheme: safe_average(values) for scheme, values in scheme_map.items()
            }
    return aggregated


def write_bandwidth_table(rows: list[dict], output_dir: Path) -> Path:
    aggregated = aggregate_bandwidth(rows)
    output_path = output_dir / "bandwidth_utilization_plot_tab.txt"
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# Average bandwidth-utilization data used for plotting.\n\n")
        handle.write("config\tecu_count\tsignal_count\tscheme\tavg_bandwidth_utilization\n")
        for ecu_count in sorted(aggregated.keys()):
            for signal_count in sorted(aggregated[ecu_count].keys()):
                config = f"E{ecu_count}S{signal_count}_{ecu_count}ecu_{signal_count}signals"
                for scheme in SCHEME_ORDER:
                    if scheme not in aggregated[ecu_count][signal_count]:
                        continue
                    handle.write(
                        f"{config}\t{ecu_count}\t{signal_count}\t{scheme}\t"
                        f"{aggregated[ecu_count][signal_count][scheme]}\n"
                    )
    return output_path


def aggregate_fault_probability(rows: list[dict]) -> dict[str, dict[str, dict[str, float]]]:
    grouped: dict[str, dict[str, dict[str, list[float]]]] = defaultdict(
        lambda: defaultdict(lambda: defaultdict(list))
    )
    for row in rows:
        grouped[row["config"]][row["scheme"]][row["asil"]].append(float(row["avg_fault_probability"]))

    aggregated: dict[str, dict[str, dict[str, float]]] = defaultdict(lambda: defaultdict(dict))
    for config, scheme_map in grouped.items():
        for scheme, asil_map in scheme_map.items():
            for asil, values in asil_map.items():
                aggregated[config][scheme][asil] = safe_average(values)
    return aggregated


def collect_wcrt_ratio(rows: list[dict]) -> dict[str, dict[str, dict[int, float]]]:
    grouped: dict[str, dict[str, dict[int, float]]] = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        grouped[row["config"]][row["scheme"]][int(row["period_ms"])] = float(row["avg_wcrt_ratio_to_baseline2"])
    return grouped


def collect_schedulability(rows: list[dict]) -> dict[int, dict[int, dict[str, float]]]:
    grouped: dict[int, dict[int, dict[str, float]]] = defaultdict(dict)
    for row in rows:
        ecu_count, signal_count = dataset_dimensions(row["config"])
        if ecu_count is None or signal_count is None:
            continue
        grouped[ecu_count].setdefault(signal_count, {})
        grouped[ecu_count][signal_count][row["scheme"]] = float(row["unschedulable_ratio"])
    return grouped


def build_bandwidth_line_figure(rows: list[dict], output_dir: Path) -> Path:
    aggregated = aggregate_bandwidth(rows)
    ecu_groups = [(ecu_count, signal_map) for ecu_count, signal_map in sorted(aggregated.items()) if ecu_count in {5, 8}]

    fig, axes = plt.subplots(
        1,
        len(ecu_groups),
        figsize=(5.6 * len(ecu_groups), 4.8),
        sharey=True,
        squeeze=False,
    )
    flat_axes = to_axes_grid(axes[0])

    global_max = 0.0
    for signal_map in aggregated.values():
        for scheme_map in signal_map.values():
            global_max = max(global_max, max(scheme_map.values(), default=0.0))
    y_top = global_max * 1.15 if global_max > 0 else 1.0

    for axis_index, (ecu_count, signal_map) in enumerate(ecu_groups):
        ax = flat_axes[axis_index]
        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme in SCHEME_ORDER:
            y_values = [signal_map[count].get(scheme, math.nan) for count in signal_counts]
            ax.plot(
                x_positions,
                y_values,
                marker="o",
                linewidth=2.0,
                markersize=6,
                color=SCHEME_COLORS[scheme],
                label=SCHEME_LABELS[scheme] if axis_index == 0 else None,
            )

        ax.set_xticks(x_positions)
        ax.set_xticklabels([str(count) for count in signal_counts])
        ax.set_xlabel("信号数量")
        ax.set_ylabel("平均带宽利用率")
        ax.set_title(f"{ecu_count} 个 ECU")
        ax.set_ylim(0.0, y_top)

    handles, labels = flat_axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=3, loc="lower center", bbox_to_anchor=(0.5, 0.03))
    fig.subplots_adjust(top=0.84, bottom=0.22, wspace=0.10)

    output_path = output_dir / "bandwidth_utilization_line.png"
    fig.savefig(output_path)
    plt.close(fig)
    return output_path


def build_bandwidth_bar_figure(rows: list[dict], output_dir: Path) -> Path:
    aggregated = aggregate_bandwidth(rows)
    ecu_groups = sorted(aggregated.items())

    fig, axes = plt.subplots(
        1,
        len(ecu_groups),
        figsize=(5.8 * len(ecu_groups), 4.8),
        sharey=True,
        squeeze=False,
    )
    flat_axes = to_axes_grid(axes[0])

    global_max = 0.0
    for signal_map in aggregated.values():
        for scheme_map in signal_map.values():
            global_max = max(global_max, max(scheme_map.values(), default=0.0))
    y_top = global_max * 1.18 if global_max > 0 else 1.0
    bar_width = 0.22
    offsets = [-bar_width, 0.0, bar_width]

    for axis_index, (ecu_count, signal_map) in enumerate(ecu_groups):
        ax = flat_axes[axis_index]
        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme_index, scheme in enumerate(SCHEME_ORDER):
            y_values = [signal_map[count].get(scheme, 0.0) for count in signal_counts]
            ax.bar(
                [x + offsets[scheme_index] for x in x_positions],
                y_values,
                width=bar_width,
                color=SCHEME_COLORS[scheme],
                edgecolor="black",
                linewidth=0.5,
                label=SCHEME_LABELS[scheme] if axis_index == 0 else None,
            )

        ax.set_xticks(x_positions)
        ax.set_xticklabels([str(count) for count in signal_counts])
        ax.set_xlabel("Signal Count")
        ax.set_ylabel("Average Bandwidth Utilization")
        ax.set_title(f"{ecu_count} ECU")
        ax.set_ylim(0.0, y_top)

    handles, labels = flat_axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=3, loc="lower center", bbox_to_anchor=(0.5, 0.03))
    fig.suptitle("Average Bandwidth Utilization", fontsize=14, y=0.96)
    fig.subplots_adjust(top=0.84, bottom=0.22, wspace=0.10)

    output_path = output_dir / "bandwidth_utilization_bar.png"
    fig.savefig(output_path)
    plt.close(fig)
    return output_path


def build_schedulability_figure(rows: list[dict], output_dir: Path) -> Path | None:
    if not rows:
        return None

    aggregated = collect_schedulability(rows)
    ecu_groups = sorted(aggregated.items())
    if not ecu_groups:
        return None

    fig, axes = plt.subplots(
        1,
        len(ecu_groups),
        figsize=(5.8 * len(ecu_groups), 4.8),
        sharey=True,
        squeeze=False,
    )
    flat_axes = to_axes_grid(axes[0])
    bar_width = 0.22
    offsets = [-bar_width, 0.0, bar_width]

    for axis_index, (ecu_count, signal_map) in enumerate(ecu_groups):
        ax = flat_axes[axis_index]
        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme_index, scheme in enumerate(SCHEME_ORDER):
            y_values = [signal_map[count].get(scheme, 0.0) for count in signal_counts]
            ax.bar(
                [x + offsets[scheme_index] for x in x_positions],
                y_values,
                width=bar_width,
                color=SCHEME_COLORS[scheme],
                edgecolor="black",
                linewidth=0.5,
                label=SCHEME_LABELS[scheme] if axis_index == 0 else None,
            )

        ax.set_xticks(x_positions)
        ax.set_xticklabels([str(count) for count in signal_counts])
        ax.set_xlabel("Signal Count")
        ax.set_ylabel("Unschedulable Ratio")
        ax.set_title(f"{ecu_count} ECU")
        ax.set_ylim(0.0, 1.0)

    handles, labels = flat_axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=3, loc="lower center", bbox_to_anchor=(0.5, 0.03))
    fig.suptitle("Unschedulable Dataset Ratio", fontsize=14, y=0.96)
    fig.subplots_adjust(top=0.84, bottom=0.22, wspace=0.10)

    output_path = output_dir / "unschedulable_ratio_bar.png"
    fig.savefig(output_path)
    plt.close(fig)
    return output_path


def build_fault_probability_grid(rows: list[dict], output_dir: Path) -> Path:
    aggregated = aggregate_fault_probability(rows)
    configs = sorted(aggregated.keys(), key=lambda name: (dataset_dimensions(name)[0] or 0, dataset_dimensions(name)[1] or 0, name))

    fig, axes = plt.subplots(
        3,
        6,
        figsize=(20.0, 10.5),
        sharey=True,
        squeeze=False,
    )
    flat_axes = to_axes_grid(axes)
    positive_values = [
        value
        for config_map in aggregated.values()
        for scheme_map in config_map.values()
        for value in scheme_map.values()
        if value > 0
    ]
    y_bottom = min(positive_values) / 5.0 if positive_values else 1e-20
    y_top = max(positive_values) * 5.0 if positive_values else 1.0
    bar_width = 0.22
    offsets = [-bar_width, 0.0, bar_width]

    for axis_index, ax in enumerate(flat_axes):
        if axis_index >= len(configs):
            ax.axis("off")
            continue

        config = configs[axis_index]
        scheme_map = aggregated[config]
        x_positions = list(range(len(ASIL_ORDER)))

        for scheme_index, scheme in enumerate(SCHEME_ORDER):
            y_values = []
            for asil in ASIL_ORDER:
                value = scheme_map.get(scheme, {}).get(asil, 0.0)
                y_values.append(value if value > 0 else y_bottom)
            ax.bar(
                [x + offsets[scheme_index] for x in x_positions],
                y_values,
                width=bar_width,
                color=SCHEME_COLORS[scheme],
                edgecolor="black",
                linewidth=0.5,
                label=SCHEME_LABELS[scheme] if axis_index == 0 else None,
            )

        for x_position, asil in enumerate(ASIL_ORDER):
            threshold = ASIL_THRESHOLDS[asil]
            if threshold is None:
                continue
            ax.hlines(
                threshold,
                x_position - 0.45,
                x_position + 0.45,
                color="#444444",
                linestyle="--",
                linewidth=1.0,
                label="ASIL Threshold" if axis_index == 0 and asil == "B" else None,
            )

        ecu_count, signal_count = dataset_dimensions(config)
        ax.set_title(f"{ecu_count} ECU / {signal_count} Signals", fontsize=9)
        ax.set_xticks(x_positions)
        ax.set_xticklabels(ASIL_ORDER)
        ax.set_yscale("log")
        ax.set_ylim(y_bottom, y_top)
        ax.set_xlabel("ASIL")
        ax.set_ylabel("Average Fault Probability")

    handles, labels = flat_axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=4, loc="lower center", bbox_to_anchor=(0.5, 0.02))
    fig.suptitle("Average Fault Probability by Configuration", fontsize=14, y=0.98)
    fig.subplots_adjust(top=0.90, bottom=0.14, wspace=0.18, hspace=0.35)

    output_path = output_dir / "fault_probability_average_grid.png"
    fig.savefig(output_path)
    plt.close(fig)
    return output_path


def build_wcrt_ratio_grid(rows: list[dict], output_dir: Path) -> Path:
    grouped = collect_wcrt_ratio(rows)
    configs = sorted(grouped.keys(), key=lambda name: (dataset_dimensions(name)[0] or 0, dataset_dimensions(name)[1] or 0, name))

    fig, axes = plt.subplots(
        3,
        6,
        figsize=(20.0, 10.5),
        sharey=True,
        squeeze=False,
    )
    flat_axes = to_axes_grid(axes)

    global_max = 0.0
    for config_map in grouped.values():
        for period_map in config_map.values():
            global_max = max(global_max, max(period_map.values(), default=0.0))
    y_top = global_max * 1.12 if global_max > 0 else 1.0
    x_positions = list(range(len(PERIOD_ORDER)))

    for axis_index, ax in enumerate(flat_axes):
        if axis_index >= len(configs):
            ax.axis("off")
            continue

        config = configs[axis_index]
        scheme_map = grouped[config]

        for scheme in WCRT_SCHEME_ORDER:
            y_values = [scheme_map.get(scheme, {}).get(period_ms, math.nan) for period_ms in PERIOD_ORDER]
            ax.plot(
                x_positions,
                y_values,
                marker="o",
                linewidth=1.8,
                markersize=4.5,
                linestyle=WCRT_SCHEME_STYLES[scheme],
                color=WCRT_SCHEME_COLORS[scheme],
                label=WCRT_SCHEME_LABELS[scheme] if axis_index == 0 else None,
            )

        ecu_count, signal_count = dataset_dimensions(config)
        ax.axhline(1.0, color="#888888", linewidth=0.9, linestyle=":")
        ax.set_title(f"{ecu_count} ECU / {signal_count} Signals", fontsize=9)
        ax.set_xticks(x_positions)
        ax.set_xticklabels([str(period_ms) for period_ms in PERIOD_ORDER], rotation=35)
        ax.set_xlabel("Period (ms)")
        ax.set_ylabel("Average WCRT Ratio to Retry")
        ax.set_ylim(0.0, y_top)

    handles, labels = flat_axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=4, loc="lower center", bbox_to_anchor=(0.5, 0.02))
    fig.suptitle("Average WCRT Ratio by Configuration", fontsize=14, y=0.98)
    fig.subplots_adjust(top=0.90, bottom=0.14, wspace=0.18, hspace=0.40)

    output_path = output_dir / "wcrt_ratio_grid.png"
    fig.savefig(output_path)
    plt.close(fig)
    return output_path


def main() -> None:
    args = parse_args()
    summary_path = args.summary.resolve()
    output_dir = args.output_dir.resolve() if args.output_dir is not None else summary_path.parent / "comparison_figures"
    output_dir.mkdir(parents=True, exist_ok=True)

    configure_matplotlib()
    sections = load_sections(summary_path)

    generated = [write_bandwidth_table(sections["bandwidth_utilization"], output_dir)]

    if args.only in {"all", "bandwidth", "bandwidth-line"}:
        generated.append(build_bandwidth_line_figure(sections["bandwidth_utilization"], output_dir))
    if args.only in {"all", "bandwidth", "bandwidth-bar"}:
        generated.append(build_bandwidth_bar_figure(sections["bandwidth_utilization"], output_dir))

    if args.only == "all":
        generated.append(build_fault_probability_grid(sections["fault_probability"], output_dir))
        generated.append(build_wcrt_ratio_grid(sections["config_period_wcrt_ratio"], output_dir))

        sched_path = build_schedulability_figure(sections.get("config_schedulability", []), output_dir)
        if sched_path is not None:
            generated.append(sched_path)

    print(f"Summary file: {summary_path}")
    print(f"Output dir: {output_dir}")
    for path in generated:
        print(f"Generated: {path}")


if __name__ == "__main__":
    main()
