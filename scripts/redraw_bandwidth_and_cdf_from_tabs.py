#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import PercentFormatter

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

SCHEME_LINESTYLES = {
    "foundation": "-",
    "baseline1": "--",
    "baseline2": "-.",
}

SCHEME_MARKERS = {
    "foundation": "o",
    "baseline1": "s",
    "baseline2": "^",
}

ECU_COLORS = {
    5: "#1f77b4",
    8: "#ff7f0e",
}

COMBINED_SERIES_COLORS = {
    (5, "foundation"): "#1f77b4",
    (5, "baseline1"): "#2ca02c",
    (5, "baseline2"): "#d62728",
    (8, "foundation"): "#9467bd",
    (8, "baseline1"): "#8c564b",
    (8, "baseline2"): "#e377c2",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="从导出的 txt 重画带宽图和 WCRT CDF 图")
    parser.add_argument("analysis_dir", type=Path, help="分析目录，例如 storage/analysis/202653_162843")
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


def load_bandwidth_grouped(bandwidth_tab: Path) -> dict[int, dict[int, dict[str, float]]]:
    rows = read_tsv_rows(bandwidth_tab)
    grouped: dict[int, dict[int, dict[str, float]]] = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        ecu_count = int(row["ecu_count"])
        signal_count = int(row["signal_count"])
        grouped[ecu_count][signal_count][row["scheme"]] = float(row["avg_bandwidth_utilization"])
    return grouped


def _global_y_top(grouped: dict[int, dict[int, dict[str, float]]]) -> float:
    global_max = 0.0
    for signal_map in grouped.values():
        for scheme_map in signal_map.values():
            global_max = max(global_max, max(scheme_map.values(), default=0.0))
    return global_max * 1.15 if global_max > 0 else 1.0


def draw_single_ecu_bandwidth(
    grouped: dict[int, dict[int, dict[str, float]]],
    ecu_count: int,
    output_path: Path,
    color_mode: str,
) -> None:
    signal_map = grouped.get(ecu_count)
    if not signal_map:
        return

    fig, ax = plt.subplots(1, 1, figsize=(6.0, 4.6))
    signal_counts = sorted(signal_map.keys())
    x_positions = list(range(len(signal_counts)))
    y_top = _global_y_top(grouped)

    for scheme in BANDWIDTH_SCHEME_ORDER:
        y_values = [signal_map[count].get(scheme, 0.0) for count in signal_counts]

        if color_mode == "default":
            color = SCHEME_COLORS[scheme]
            linestyle = "-"
        elif color_mode == "same_color_diff_style":
            color = "#2f2f2f"
            linestyle = SCHEME_LINESTYLES[scheme]
        elif color_mode == "color_and_style":
            color = SCHEME_COLORS[scheme]
            linestyle = SCHEME_LINESTYLES[scheme]
        else:
            raise ValueError(f"Unknown color_mode: {color_mode}")

        ax.plot(
            x_positions,
            y_values,
            marker="o",
            linewidth=2.0,
            markersize=6,
            color=color,
            linestyle=linestyle,
            label=SCHEME_LABELS[scheme],
        )

    ax.set_xticks(x_positions)
    ax.set_xticklabels([str(count) for count in signal_counts])
    ax.set_xlabel("信号数量")
    ax.set_ylabel("平均带宽利用率(%)")
    ax.set_title(f"{ecu_count} 个 ECU")
    ax.set_ylim(0.0, y_top)
    ax.yaxis.set_major_formatter(PercentFormatter(xmax=1.0, decimals=0))
    ax.legend(frameon=False, loc="upper left")

    fig.subplots_adjust(top=0.90, bottom=0.15, left=0.14, right=0.98)
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def draw_bandwidth_two_panel(
    grouped: dict[int, dict[int, dict[str, float]]],
    output_path: Path,
    style_mode: str,
) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(10.8, 4.6), sharey=True, squeeze=False)
    flat_axes = list(axes[0])
    y_top = _global_y_top(grouped)

    for axis_index, ecu_count in enumerate((5, 8)):
        ax = flat_axes[axis_index]
        signal_map = grouped.get(ecu_count, {})
        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme in BANDWIDTH_SCHEME_ORDER:
            y_values = [signal_map[count].get(scheme, 0.0) for count in signal_counts]
            if style_mode == "color":
                color = SCHEME_COLORS[scheme]
                marker = "o"
            elif style_mode == "marker":
                color = "#2f2f2f"
                marker = SCHEME_MARKERS[scheme]
            elif style_mode == "color_marker":
                color = SCHEME_COLORS[scheme]
                marker = SCHEME_MARKERS[scheme]
            else:
                raise ValueError(f"Unknown style_mode: {style_mode}")

            ax.plot(
                x_positions,
                y_values,
                marker=marker,
                linewidth=1.2,
                markersize=4.5,
                color=color,
                linestyle="-",
                label=SCHEME_LABELS[scheme],
            )

        ax.set_xticks(x_positions)
        ax.set_xticklabels([str(count) for count in signal_counts])
        ax.set_xlabel("信号数量")
        ax.set_title(f"{ecu_count} 个 ECU")
        ax.set_title("")
        ax.set_ylim(0.0, y_top)
        ax.yaxis.set_major_formatter(PercentFormatter(xmax=1.0, decimals=0))
        ax.tick_params(axis="y", labelleft=True)

    flat_axes[0].set_ylabel("平均带宽利用率(%)")
    for ax in flat_axes:
        ax.legend(
            frameon=False,
            ncol=1,
            loc="upper left",
            fontsize=9,
            handlelength=2.0,
            borderaxespad=0.4,
        )
    fig.text(0.28, 0.01, "(a) 5个ECU", ha="center", va="center", fontsize=11)
    fig.text(0.74, 0.01, "(b) 8个ECU", ha="center", va="center", fontsize=11)
    fig.subplots_adjust(top=0.88, bottom=0.16, left=0.08, right=0.99, wspace=0.10)
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def draw_combined_bandwidth(grouped: dict[int, dict[int, dict[str, float]]], output_path: Path) -> None:
    fig, ax = plt.subplots(1, 1, figsize=(7.2, 4.8))
    y_top = _global_y_top(grouped)

    for ecu_count in (5, 8):
        signal_map = grouped.get(ecu_count, {})
        if not signal_map:
            continue
        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme in BANDWIDTH_SCHEME_ORDER:
            y_values = [signal_map[count].get(scheme, 0.0) for count in signal_counts]
            ax.plot(
                x_positions,
                y_values,
                marker="o",
                linewidth=2.0,
                markersize=5,
                color=ECU_COLORS[ecu_count],
                linestyle=SCHEME_LINESTYLES[scheme],
                label=f"{ecu_count} ECU - {SCHEME_LABELS[scheme]}",
            )

    ax.set_xticks(list(range(6)))
    ax.set_xticklabels(["50", "80", "120", "150", "200", "250"])
    ax.set_xlabel("信号数量")
    ax.set_ylabel("平均带宽利用率(%)")
    ax.set_ylim(0.0, y_top)
    ax.yaxis.set_major_formatter(PercentFormatter(xmax=1.0, decimals=0))
    ax.legend(frameon=False, ncol=2, fontsize=9, loc="upper left")

    fig.subplots_adjust(top=0.96, bottom=0.14, left=0.12, right=0.98)
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def draw_combined_bandwidth_all_diff_colors(grouped: dict[int, dict[int, dict[str, float]]], output_path: Path) -> None:
    fig, ax = plt.subplots(1, 1, figsize=(7.2, 4.8))
    y_top = _global_y_top(grouped)

    for ecu_count in (5, 8):
        signal_map = grouped.get(ecu_count, {})
        if not signal_map:
            continue
        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme in BANDWIDTH_SCHEME_ORDER:
            y_values = [signal_map[count].get(scheme, 0.0) for count in signal_counts]
            ax.plot(
                x_positions,
                y_values,
                marker="o",
                linewidth=1.5,
                markersize=5,
                color=COMBINED_SERIES_COLORS[(ecu_count, scheme)],
                linestyle="-",
                label=f"{ecu_count} ECU - {SCHEME_LABELS[scheme]}",
            )

    ax.set_xticks(list(range(6)))
    ax.set_xticklabels(["50", "80", "120", "150", "200", "250"])
    ax.set_xlabel("信号数量")
    ax.set_ylabel("平均带宽利用率(%)")
    ax.set_ylim(0.0, y_top)
    ax.yaxis.set_major_formatter(PercentFormatter(xmax=1.0, decimals=0))
    ax.legend(frameon=False, ncol=2, fontsize=9, loc="upper left")

    fig.subplots_adjust(top=0.96, bottom=0.14, left=0.12, right=0.98)
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def redraw_bandwidth_variants(bandwidth_tab: Path, output_dir: Path) -> list[Path]:
    grouped = load_bandwidth_grouped(bandwidth_tab)
    generated: list[Path] = []

    output = output_dir / "bandwidth_two_panel_color.png"
    draw_bandwidth_two_panel(grouped, output, "color")
    generated.append(output)

    output = output_dir / "bandwidth_two_panel_marker.png"
    draw_bandwidth_two_panel(grouped, output, "marker")
    generated.append(output)

    output = output_dir / "bandwidth_two_panel_color_marker.png"
    draw_bandwidth_two_panel(grouped, output, "color_marker")
    generated.append(output)

    for ecu_count in (5, 8):
        output = output_dir / f"bandwidth_{ecu_count}ecu_default.png"
        draw_single_ecu_bandwidth(grouped, ecu_count, output, "default")
        generated.append(output)

        output = output_dir / f"bandwidth_{ecu_count}ecu_same_color_diff_style.png"
        draw_single_ecu_bandwidth(grouped, ecu_count, output, "same_color_diff_style")
        generated.append(output)

        output = output_dir / f"bandwidth_{ecu_count}ecu_color_and_style.png"
        draw_single_ecu_bandwidth(grouped, ecu_count, output, "color_and_style")
        generated.append(output)

    combined_output = output_dir / "bandwidth_combined_ecu_color_method_style.png"
    draw_combined_bandwidth(grouped, combined_output)
    generated.append(combined_output)

    combined_all_color_output = output_dir / "bandwidth_combined_all_diff_colors_same_style.png"
    draw_combined_bandwidth_all_diff_colors(grouped, combined_all_color_output)
    generated.append(combined_all_color_output)

    return generated


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
                ax.step(
                    x_values,
                    y_values,
                    where="post",
                    linewidth=1.2,
                    color=SCHEME_COLORS[scheme],
                    linestyle=SCHEME_LINESTYLES[scheme],
                    label=SCHEME_LABELS[scheme],
                )

            _, signal_count = dataset_dimensions(config)
            ax.set_xlabel("确定性 WCRT（ms）")
            ax.set_ylabel("累计比例(%)")
            ax.set_xlim(left=0.0, right=max_x * 1.03 if max_x > 0 else 1.0)
            ax.set_ylim(0.0, 1.0)
            ax.yaxis.set_major_formatter(PercentFormatter(xmax=1.0, decimals=0))
            ax.tick_params(axis="y", labelleft=True)
            ax.legend(
                frameon=False,
                ncol=1,
                loc="upper left",
                fontsize=8.5,
                handlelength=2.0,
                borderaxespad=0.3,
            )
            subfig_prefix = f"({chr(ord('a') + axis_index)})"
            subfig_desc = f"{signal_count}信号" if signal_count is not None else config
            ax.text(
                0.5,
                -0.28,
                f"{subfig_prefix} {subfig_desc}",
                transform=ax.transAxes,
                ha="center",
                va="top",
                fontsize=10,
            )

        fig.subplots_adjust(top=0.92, bottom=0.08, left=0.08, right=0.98, wspace=0.20, hspace=0.42)

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

    bandwidth_generated = redraw_bandwidth_variants(bandwidth_tab, comparison_dir)
    cdf_generated = redraw_cdf(cdf_points_tab, cdf_dir)

    print(f"Analysis dir: {analysis_dir}")
    for path in bandwidth_generated:
        print(f"Redrawn: {path}")
    for path in cdf_generated:
        print(f"Redrawn: {path}")


if __name__ == "__main__":
    main()
