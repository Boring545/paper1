#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import configure_matplotlib, dataset_dimensions, dataset_sort_key


BANDWIDTH_SCHEME_ORDER = ["foundation", "baseline1", "baseline2"]
CDF_SCHEME_ORDER = ["foundation", "baseline1"]
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="从导出的 txt 数据重画带宽图和 WCRT CDF 图。")
    parser.add_argument("analysis_dir", type=Path, help="分析目录，例如 storage/analysis/202651_134233")
    return parser.parse_args()


def read_tsv_rows(path: Path) -> list[dict[str, str]]:
    header = None
    rows: list[dict[str, str]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if header is None:
                header = line.split("\t")
                continue
            row = next(csv.reader([line], delimiter="\t"))
            if len(row) != len(header):
                raise ValueError(f"Malformed row in {path.name}: {line}")
            rows.append(dict(zip(header, row)))
    return rows


def redraw_bandwidth(bandwidth_tab: Path, output_path: Path) -> None:
    rows = read_tsv_rows(bandwidth_tab)
    grouped: dict[int, dict[int, dict[str, float]]] = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        ecu_count = int(row["ecu_count"])
        signal_count = int(row["signal_count"])
        grouped[ecu_count][signal_count][row["scheme"]] = float(row["avg_bandwidth_utilization"])

    filtered_keys = [ecu_count for ecu_count in sorted(grouped.keys()) if ecu_count in {5, 8}]
    fig, axes = plt.subplots(1, len(filtered_keys), figsize=(5.2 * len(filtered_keys), 4.5), sharey=False, squeeze=False)
    flat_axes = list(axes[0])

    global_max = 0.0
    for signal_map in grouped.values():
        for scheme_map in signal_map.values():
            global_max = max(global_max, max(scheme_map.values(), default=0.0))
    y_top = global_max * 1.15 if global_max > 0 else 1.0

    for axis_index, ecu_count in enumerate(filtered_keys):
        ax = flat_axes[axis_index]
        signal_map = grouped[ecu_count]
        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme in BANDWIDTH_SCHEME_ORDER:
            y_values = [signal_map[count].get(scheme, 0.0) for count in signal_counts]
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
        ax.tick_params(axis="y", labelleft=True)

    handles, labels = flat_axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncol=3, loc="lower center", bbox_to_anchor=(0.5, 0.03))
    fig.subplots_adjust(top=0.84, bottom=0.20, left=0.08, right=0.99, wspace=0.12)
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def redraw_cdf(cdf_points_tab: Path, output_dir: Path) -> list[Path]:
    rows = read_tsv_rows(cdf_points_tab)
    grouped: dict[str, dict[str, tuple[list[float], list[float]]]] = defaultdict(
        lambda: defaultdict(lambda: ([], []))
    )

    for row in rows:
        config = row["config"]
        scheme = row["scheme"]
        x_values, y_values = grouped[config][scheme]
        x_values.append(float(row["threshold_wcrt_ms"]))
        y_values.append(float(row["cdf"]))

    grouped_by_ecu: dict[int, list[tuple[str, dict[str, tuple[list[float], list[float]]]]]] = defaultdict(list)
    for config in sorted(grouped.keys(), key=dataset_sort_key):
        ecu_count, _ = dataset_dimensions(config)
        if ecu_count is None or ecu_count not in {5, 8}:
            continue
        grouped_by_ecu[ecu_count].append((config, grouped[config]))

    generated: list[Path] = []
    for ecu_count in sorted(grouped_by_ecu.keys()):
        fig, axes = plt.subplots(3, 2, figsize=(10.8, 10.8), sharey=False, squeeze=False)
        flat_axes = [ax for row in axes for ax in row]
        legend_handles = []
        legend_labels = []

        for axis_index, ax in enumerate(flat_axes):
            if axis_index >= len(grouped_by_ecu[ecu_count]):
                ax.axis("off")
                continue

            config, config_group = grouped_by_ecu[ecu_count][axis_index]
            max_x = 0.0
            for scheme in CDF_SCHEME_ORDER:
                x_values, y_values = config_group.get(scheme, ([], []))
                if not x_values:
                    continue
                max_x = max(max_x, x_values[-1])
                line = ax.step(
                    x_values,
                    y_values,
                    where="post",
                    linewidth=2.0,
                    color=SCHEME_COLORS[scheme],
                    label=SCHEME_LABELS[scheme] if axis_index == 0 else None,
                )
                if axis_index == 0:
                    legend_handles.append(line[0])
                    legend_labels.append(SCHEME_LABELS[scheme])

            _, signal_count = dataset_dimensions(config)
            ax.set_title(f"{signal_count} 个信号" if signal_count is not None else config, fontsize=11)
            ax.set_xlabel("确定性 WCRT（ms）")
            ax.set_ylabel("累计比例")
            ax.set_xlim(left=0.0, right=max_x * 1.03 if max_x > 0 else 1.0)
            ax.set_ylim(0.0, 1.0)
            ax.tick_params(axis="y", labelleft=True)

        fig.legend(legend_handles, legend_labels, frameon=False, ncol=2, loc="lower center", bbox_to_anchor=(0.5, 0.03))
        fig.subplots_adjust(top=0.92, bottom=0.12, left=0.08, right=0.98, wspace=0.20, hspace=0.30)

        output_path = output_dir / f"signal_wcrt_cdf_{ecu_count}ecu.png"
        fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
        plt.close(fig)
        generated.append(output_path)

    return generated


def main() -> None:
    args = parse_args()
    analysis_dir = args.analysis_dir.resolve()
    comparison_dir = analysis_dir / "comparison_figures"
    cdf_dir = comparison_dir / "signal_wcrt_cdf"
    bandwidth_tab = comparison_dir / "bandwidth_utilization_plot_tab.txt"
    cdf_points_tab = cdf_dir / "signal_wcrt_cdf_points_tab.txt"

    if not bandwidth_tab.is_file():
        raise FileNotFoundError(f"Missing bandwidth plot data: {bandwidth_tab}")
    if not cdf_points_tab.is_file():
        raise FileNotFoundError(f"Missing CDF point data: {cdf_points_tab}")

    configure_matplotlib(font_size=10.0)
    redraw_bandwidth(bandwidth_tab, comparison_dir / "bandwidth_utilization_line.png")
    generated = redraw_cdf(cdf_points_tab, cdf_dir)

    print(f"Analysis dir: {analysis_dir}")
    print(f"Redrawn: {comparison_dir / 'bandwidth_utilization_line.png'}")
    for path in generated:
        print(f"Redrawn: {path}")


if __name__ == "__main__":
    main()
