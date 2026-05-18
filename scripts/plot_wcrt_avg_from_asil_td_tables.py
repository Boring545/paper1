#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import configure_matplotlib


SCHEME_ORDER = ["foundation", "baseline1"]
SCHEME_LABELS = {
    "foundation": "信号备份",
    "baseline1": "报文备份",
}
SCHEME_COLORS = {
    "foundation": "#1b6ca8",
    "baseline1": "#d66a1f",
}
SCHEME_MARKERS = {
    "foundation": "o",
    "baseline1": "s",
}

FILE_PATTERN = re.compile(
    r"^(E(?P<ecu>\d+)S(?P<signals>\d+)_\d+ecu_\d+signals)_(?P<scheme>foundation|baseline1)_ASIL_T_D_WCRT_tab\.txt$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot average WCRT vs signal count from ASIL-T-D grouped tables."
    )
    parser.add_argument(
        "analysis_dir",
        type=Path,
        help="Analysis directory, e.g. storage/analysis/202653_162843",
    )
    return parser.parse_args()


def read_wcrt_values(path: Path) -> list[float]:
    values: list[float] = []
    header: list[str] | None = None
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
                raise ValueError(f"Malformed row in {path}: {line}")
            mapped = dict(zip(header, row))
            values.append(float(mapped["WCRT"]))
    return values


def collect_averages(tables_dir: Path) -> dict[int, dict[int, dict[str, float]]]:
    grouped: dict[int, dict[int, dict[str, float]]] = defaultdict(lambda: defaultdict(dict))

    for file_path in sorted(tables_dir.glob("*_ASIL_T_D_WCRT_tab.txt")):
        match = FILE_PATTERN.match(file_path.name)
        if not match:
            continue
        ecu_count = int(match.group("ecu"))
        signal_count = int(match.group("signals"))
        scheme = match.group("scheme")
        values = read_wcrt_values(file_path)
        if not values:
            continue
        grouped[ecu_count][signal_count][scheme] = sum(values) / len(values)

    return grouped


def collect_averages_by_period(
    tables_dir: Path,
) -> tuple[dict[int, dict[int, dict[int, dict[str, float]]]], list[int]]:
    # ecu -> period -> signal_count -> scheme -> avg_wcrt
    grouped: dict[int, dict[int, dict[int, dict[str, float]]]] = defaultdict(
        lambda: defaultdict(lambda: defaultdict(dict))
    )
    period_values: set[int] = set()

    for file_path in sorted(tables_dir.glob("*_ASIL_T_D_WCRT_tab.txt")):
        match = FILE_PATTERN.match(file_path.name)
        if not match:
            continue
        ecu_count = int(match.group("ecu"))
        signal_count = int(match.group("signals"))
        scheme = match.group("scheme")

        header: list[str] | None = None
        period_buckets: dict[int, list[float]] = defaultdict(list)
        with file_path.open("r", encoding="utf-8", newline="") as handle:
            for raw_line in handle:
                line = raw_line.strip()
                if not line or line.startswith("#"):
                    continue
                if header is None:
                    header = line.split("\t")
                    continue
                row = next(csv.reader([line], delimiter="\t"))
                if len(row) != len(header):
                    raise ValueError(f"Malformed row in {file_path}: {line}")
                mapped = dict(zip(header, row))
                period = int(float(mapped["T"]))
                wcrt = float(mapped["WCRT"])
                period_buckets[period].append(wcrt)
                period_values.add(period)

        for period, values in period_buckets.items():
            if not values:
                continue
            grouped[ecu_count][period][signal_count][scheme] = sum(values) / len(values)

    return grouped, sorted(period_values)


def write_summary_table(grouped: dict[int, dict[int, dict[str, float]]], output_path: Path) -> None:
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# Average WCRT by config and scheme, derived from ASIL-T-D grouped tables.\n\n")
        handle.write("ecu_count\tsignal_count\tscheme\tavg_wcrt_ms\n")
        for ecu_count in sorted(grouped.keys()):
            for signal_count in sorted(grouped[ecu_count].keys()):
                for scheme in SCHEME_ORDER:
                    if scheme not in grouped[ecu_count][signal_count]:
                        continue
                    handle.write(
                        f"{ecu_count}\t{signal_count}\t{scheme}\t{grouped[ecu_count][signal_count][scheme]}\n"
                    )


def plot_for_ecu(grouped: dict[int, dict[int, dict[str, float]]], ecu_count: int, output_path: Path) -> None:
    signal_map = grouped.get(ecu_count, {})
    if not signal_map:
        return

    fig, ax = plt.subplots(1, 1, figsize=(6.2, 4.6))
    signal_counts = sorted(signal_map.keys())
    x_positions = list(range(len(signal_counts)))

    y_max = 0.0
    for scheme in SCHEME_ORDER:
        y_values = [signal_map[count].get(scheme, float("nan")) for count in signal_counts]
        finite_values = [v for v in y_values if v == v]
        if finite_values:
            y_max = max(y_max, max(finite_values))
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
    ax.set_xticklabels([str(count) for count in signal_counts])
    ax.set_xlabel("信号数量")
    ax.set_ylabel("平均 WCRT（ms）")
    ax.set_title(f"{ecu_count} 个 ECU")
    ax.set_ylim(0.0, y_max * 1.15 if y_max > 0 else 1.0)
    ax.legend(frameon=False, loc="upper left")

    fig.subplots_adjust(top=0.90, bottom=0.15, left=0.14, right=0.98)
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def plot_for_ecu_by_period(
    grouped_by_period: dict[int, dict[int, dict[int, dict[str, float]]]],
    ecu_count: int,
    periods: list[int],
    output_path: Path,
) -> None:
    period_map = grouped_by_period.get(ecu_count, {})
    if not period_map:
        return

    subplot_periods = [p for p in periods if p in period_map]
    if not subplot_periods:
        return

    rows, cols = 2, 4
    fig, axes = plt.subplots(rows, cols, figsize=(13.2, 6.8), sharey=False, squeeze=False)
    flat_axes = [ax for row in axes for ax in row]

    global_max = 0.0
    for p in subplot_periods:
        for signal_count in period_map[p]:
            for scheme in SCHEME_ORDER:
                val = period_map[p][signal_count].get(scheme)
                if val is not None and val > global_max:
                    global_max = val
    y_top = global_max * 1.15 if global_max > 0 else 1.0

    for idx, ax in enumerate(flat_axes):
        if idx >= len(subplot_periods):
            ax.axis("off")
            continue
        period = subplot_periods[idx]
        signal_map = period_map[period]

        signal_counts = sorted(signal_map.keys())
        x_positions = list(range(len(signal_counts)))

        for scheme in SCHEME_ORDER:
            y_values = [signal_map[count].get(scheme, float("nan")) for count in signal_counts]
            ax.plot(
                x_positions,
                y_values,
                marker=SCHEME_MARKERS[scheme],
                linewidth=1.2,
                markersize=4.5,
                color=SCHEME_COLORS[scheme],
                linestyle="-",
                label=SCHEME_LABELS[scheme],
            )

        ax.set_xticks(x_positions)
        ax.set_xticklabels([str(count) for count in signal_counts], fontsize=8)
        ax.set_ylim(0.0, y_top)
        ax.tick_params(axis="y", labelsize=8, labelleft=True)
        ax.legend(frameon=False, ncol=1, loc="upper left", fontsize=8, handlelength=2.0, borderaxespad=0.3)
        subfig_prefix = f"({chr(ord('a') + idx)})"
        ax.text(
            0.5,
            -0.28,
            f"{subfig_prefix} T={period}ms",
            transform=ax.transAxes,
            ha="center",
            va="top",
            fontsize=9,
        )

        if idx % cols == 0:
            ax.set_ylabel("平均WCRT(ms)", fontsize=9)
        if idx // cols == rows - 1:
            ax.set_xlabel("信号数量", fontsize=9)

    fig.text(0.5, 0.01, f"{ecu_count}个ECU", ha="center", va="center", fontsize=11)
    fig.subplots_adjust(top=0.92, bottom=0.12, left=0.06, right=0.99, wspace=0.18, hspace=0.48)
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)


def main() -> None:
    args = parse_args()
    analysis_dir = args.analysis_dir.resolve()
    tables_dir = analysis_dir / "comparison_figures" / "wcrt_asil_td_tables"
    if not tables_dir.is_dir():
        raise FileNotFoundError(f"Missing table directory: {tables_dir}")

    output_dir = analysis_dir / "comparison_figures" / "wcrt_avg_from_asil_td"
    output_dir.mkdir(parents=True, exist_ok=True)

    configure_matplotlib(font_size=10.0)
    grouped = collect_averages(tables_dir)
    grouped_by_period, periods = collect_averages_by_period(tables_dir)

    write_summary_table(grouped, output_dir / "wcrt_avg_by_config_tab.txt")
    plot_for_ecu(grouped, 5, output_dir / "wcrt_avg_5ecu.png")
    plot_for_ecu(grouped, 8, output_dir / "wcrt_avg_8ecu.png")
    plot_for_ecu_by_period(grouped_by_period, 5, periods, output_dir / "wcrt_avg_5ecu_by_period.png")
    plot_for_ecu_by_period(grouped_by_period, 8, periods, output_dir / "wcrt_avg_8ecu_by_period.png")

    print(f"Input tables: {tables_dir}")
    print(f"Output dir: {output_dir}")
    print(f"Generated: {output_dir / 'wcrt_avg_by_config_tab.txt'}")
    print(f"Generated: {output_dir / 'wcrt_avg_5ecu.png'}")
    print(f"Generated: {output_dir / 'wcrt_avg_8ecu.png'}")
    print(f"Generated: {output_dir / 'wcrt_avg_5ecu_by_period.png'}")
    print(f"Generated: {output_dir / 'wcrt_avg_8ecu_by_period.png'}")


if __name__ == "__main__":
    main()
