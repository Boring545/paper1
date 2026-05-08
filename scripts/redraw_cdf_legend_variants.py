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


CDF_SCHEME_ORDER = ["foundation", "baseline1"]
SCHEME_COLORS = {"foundation": "#1b6ca8", "baseline1": "#d66a1f"}
SCHEME_LINESTYLES = {"foundation": "-", "baseline1": "--"}
SCHEME_LABELS = {"foundation": "信号备份", "baseline1": "报文备份"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("analysis_dir", type=Path)
    return parser.parse_args()


def read_tsv_rows(path: Path) -> list[dict[str, str]]:
    header = None
    rows: list[dict[str, str]] = []
    with path.open("r", encoding="utf-8", newline="") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if header is None:
                header = line.split("\t")
                continue
            row = next(csv.reader([line], delimiter="\t"))
            rows.append(dict(zip(header, row)))
    return rows


def draw_variant(
    grouped_by_ecu: dict[int, list[tuple[str, dict[str, tuple[list[float], list[float]]]]]],
    output_dir: Path,
    suffix: str,
    label_map: dict[str, str],
    legend_y: float,
) -> list[Path]:
    outs: list[Path] = []
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
                    label=label_map[scheme],
                )
            _, signal_count = dataset_dimensions(config)
            ax.set_xlabel("WCRT(ms)")
            ax.set_ylabel("累计比例(%)")
            ax.set_xlim(left=0.0, right=max_x * 1.03 if max_x > 0 else 1.0)
            ax.set_ylim(0.0, 1.0)
            ax.yaxis.set_major_formatter(PercentFormatter(xmax=1.0, decimals=0))
            ax.tick_params(axis="y", labelleft=True)
            ax.legend(
                frameon=False,
                ncol=1,
                loc="upper left",
                bbox_to_anchor=(0.02, legend_y),
                fontsize=8.5,
                handlelength=2.0,
                borderaxespad=0.2,
            )
            subfig_prefix = f"({chr(ord('a') + axis_index)})"
            subfig_desc = f"{signal_count}信号" if signal_count is not None else config
            ax.text(0.5, -0.28, f"{subfig_prefix} {subfig_desc}", transform=ax.transAxes, ha="center", va="top", fontsize=10)

        fig.subplots_adjust(top=0.92, bottom=0.08, left=0.08, right=0.98, wspace=0.20, hspace=0.42)
        out = output_dir / f"signal_wcrt_cdf_{ecu_count}ecu{suffix}.png"
        fig.savefig(out, bbox_inches="tight", pad_inches=0.04)
        plt.close(fig)
        outs.append(out)
    return outs


def main() -> None:
    args = parse_args()
    analysis_dir = args.analysis_dir.resolve()
    cdf_dir = analysis_dir / "comparison_figures" / "signal_wcrt_cdf"
    cdf_points_tab = cdf_dir / "signal_wcrt_cdf_points_tab.txt"

    rows = read_tsv_rows(cdf_points_tab)
    grouped: dict[str, dict[str, tuple[list[float], list[float]]]] = defaultdict(lambda: defaultdict(lambda: ([], [])))
    for row in rows:
        config = row["config"]
        scheme = row["scheme"]
        xs, ys = grouped[config][scheme]
        xs.append(float(row["threshold_wcrt_ms"]))
        ys.append(float(row["cdf"]))

    grouped_by_ecu: dict[int, list[tuple[str, dict[str, tuple[list[float], list[float]]]]]] = defaultdict(list)
    for config in sorted(grouped.keys(), key=dataset_sort_key):
        ecu_count, _ = dataset_dimensions(config)
        if ecu_count in {5, 8}:
            grouped_by_ecu[ecu_count].append((config, grouped[config]))

    configure_matplotlib(font_size=10.0)
    outs = []
    outs += draw_variant(grouped_by_ecu, cdf_dir, "_legend_up", SCHEME_LABELS, 1.10)
    outs += draw_variant(grouped_by_ecu, cdf_dir, "_legend_short", {"foundation": "信备", "baseline1": "报备"}, 0.98)
    for p in outs:
        print(f"Generated: {p}")


if __name__ == "__main__":
    main()
