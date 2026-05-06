#!/usr/bin/env python
from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import configure_matplotlib, dataset_config_name, dataset_dimensions, dataset_sort_key, resolve_compare_dir


SCHEME_ORDER = ["foundation", "baseline1"]
SCHEME_LABELS = {
    "foundation": "信号备份",
    "baseline1": "报文备份",
}
SCHEME_COLORS = {
    "foundation": "#1b6ca8",
    "baseline1": "#d66a1f",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot pooled signal deterministic-WCRT CDF figures by configuration.")
    parser.add_argument("input_path", type=Path, help="Analysis directory or comparison_reports directory")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory, defaults to <analysis_batch>/comparison_figures/signal_wcrt_cdf",
    )
    return parser.parse_args()


def parse_signal_detail(compare_file: Path) -> list[dict[str, str]]:
    current_section = None
    header = None
    rows: list[dict[str, str]] = []

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
            if len(row) != len(header):
                raise ValueError(f"Malformed signal_detail row in {compare_file.name}: {line}")
            rows.append(dict(zip(header, row)))

    return rows


def load_schedulable_dataset_schemes(summary_path: Path) -> set[tuple[str, str]]:
    allowed: set[tuple[str, str]] = set()
    current_section = None
    header = None

    with summary_path.open("r", encoding="utf-8", newline="") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                current_section = line[1:-1]
                header = None
                continue
            if current_section != "bandwidth_utilization":
                continue
            if header is None:
                header = line.split("\t")
                continue
            row = next(csv.reader([line], delimiter="\t"))
            if len(row) != len(header):
                raise ValueError(f"Malformed summary row in {summary_path.name}: {line}")
            mapped = dict(zip(header, row))
            if mapped.get("schedulable") == "1":
                allowed.add((mapped["dataset"], mapped["scheme"]))

    return allowed


def collect_pooled_samples(compare_dir: Path, allowed_dataset_schemes: set[tuple[str, str]]) -> dict[str, dict[str, list[float]]]:
    pooled: dict[str, dict[str, list[float]]] = {}
    compare_files = sorted(compare_dir.glob("*.txt"), key=lambda path: dataset_sort_key(path.stem))

    for compare_file in compare_files:
        config = dataset_config_name(compare_file.stem)
        pooled.setdefault(config, {scheme: [] for scheme in SCHEME_ORDER})

        for row in parse_signal_detail(compare_file):
            scheme = row["scheme"]
            if scheme not in SCHEME_ORDER:
                continue
            if (compare_file.stem, scheme) not in allowed_dataset_schemes:
                continue
            try:
                value = float(row["threshold_wcrt_ms"])
            except (KeyError, ValueError):
                continue
            pooled[config][scheme].append(value)

    return pooled


def ecdf_points(samples: list[float]) -> tuple[list[float], list[float]]:
    if not samples:
        return [], []
    values = sorted(samples)
    total = len(values)
    y_values = [(index + 1) / total for index in range(total)]
    return values, y_values


def write_summary_table(output_dir: Path, pooled: dict[str, dict[str, list[float]]]) -> Path:
    output_path = output_dir / "signal_wcrt_cdf_pooling_tab.txt"
    configs = sorted(pooled.keys(), key=dataset_sort_key)

    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# Signal deterministic-WCRT CDF pooling summary.\n")
        handle.write("# Pooling method: merge all signal threshold_wcrt_ms samples from all datasets under the same config.\n\n")
        handle.write("config\tscheme\tsample_count\tmin_wcrt_ms\tp50_wcrt_ms\tp95_wcrt_ms\tmax_wcrt_ms\n")

        for config in configs:
            for scheme in SCHEME_ORDER:
                samples = sorted(pooled[config].get(scheme, []))
                if not samples:
                    handle.write(f"{config}\t{scheme}\t0\t0\t0\t0\t0\n")
                    continue
                count = len(samples)
                p50 = samples[min(count - 1, int(0.50 * count))]
                p95 = samples[min(count - 1, int(0.95 * count))]
                handle.write(
                    f"{config}\t{scheme}\t{count}\t{samples[0]}\t{p50}\t{p95}\t{samples[-1]}\n"
                )

    return output_path


def write_samples_table(output_dir: Path, pooled: dict[str, dict[str, list[float]]]) -> Path:
    output_path = output_dir / "signal_wcrt_cdf_samples_tab.txt"
    configs = sorted(pooled.keys(), key=dataset_sort_key)

    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# Raw pooled deterministic-WCRT samples used for the CDF plots.\n\n")
        handle.write("config\tscheme\tsample_index\tthreshold_wcrt_ms\n")

        for config in configs:
            for scheme in SCHEME_ORDER:
                samples = sorted(pooled[config].get(scheme, []))
                for sample_index, value in enumerate(samples, start=1):
                    handle.write(f"{config}\t{scheme}\t{sample_index}\t{value}\n")

    return output_path


def write_ecdf_points_table(output_dir: Path, pooled: dict[str, dict[str, list[float]]]) -> Path:
    output_path = output_dir / "signal_wcrt_cdf_points_tab.txt"
    configs = sorted(pooled.keys(), key=dataset_sort_key)

    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# ECDF points used for the deterministic-WCRT CDF plots.\n\n")
        handle.write("config\tscheme\tpoint_index\tthreshold_wcrt_ms\tcdf\n")

        for config in configs:
            for scheme in SCHEME_ORDER:
                x_values, y_values = ecdf_points(pooled[config].get(scheme, []))
                for point_index, (x_value, y_value) in enumerate(zip(x_values, y_values), start=1):
                    handle.write(f"{config}\t{scheme}\t{point_index}\t{x_value}\t{y_value}\n")

    return output_path


def plot_ecu_group_cdf(
    ecu_count: int, config_samples: list[tuple[str, dict[str, list[float]]]], output_dir: Path
) -> Path | None:
    if not config_samples:
        return None

    fig, axes = plt.subplots(3, 2, figsize=(10.8, 10.8), sharey=False, squeeze=False)
    flat_axes = [ax for row in axes for ax in row]
    legend_handles = []
    legend_labels = []

    for axis_index, ax in enumerate(flat_axes):
        if axis_index >= len(config_samples):
            ax.axis("off")
            continue

        config, scheme_samples = config_samples[axis_index]
        max_x = 0.0
        active_schemes = [scheme for scheme in SCHEME_ORDER if scheme_samples.get(scheme)]
        for scheme in active_schemes:
            x_values, y_values = ecdf_points(scheme_samples[scheme])
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
        ax.set_xlabel("WCRT（ms）")
        ax.set_ylabel("累计比例")
        ax.set_xlim(left=0.0, right=max_x * 1.03 if max_x > 0 else 1.0)
        ax.set_ylim(0.0, 1.0)
        ax.tick_params(axis="y", labelleft=True)

    fig.legend(legend_handles, legend_labels, frameon=False, ncol=2, loc="lower center", bbox_to_anchor=(0.5, 0.03))
    fig.subplots_adjust(top=0.92, bottom=0.12, left=0.08, right=0.98, wspace=0.20, hspace=0.30)

    output_path = output_dir / f"signal_wcrt_cdf_{ecu_count}ecu.png"
    fig.savefig(output_path, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)
    return output_path


def main() -> None:
    args = parse_args()
    compare_dir = resolve_compare_dir(args.input_path)
    summary_path = compare_dir.parent / "compare_summary_tab.txt"
    output_dir = (
        args.output_dir.resolve()
        if args.output_dir is not None
        else compare_dir.parent / "comparison_figures" / "signal_wcrt_cdf"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    configure_matplotlib(font_size=10.0)
    allowed_dataset_schemes = load_schedulable_dataset_schemes(summary_path)
    pooled = collect_pooled_samples(compare_dir, allowed_dataset_schemes)
    configs = sorted(pooled.keys(), key=dataset_sort_key)

    generated: list[Path] = [
        write_summary_table(output_dir, pooled),
        write_samples_table(output_dir, pooled),
        write_ecdf_points_table(output_dir, pooled),
    ]

    grouped_by_ecu: dict[int, list[tuple[str, dict[str, list[float]]]]] = {}
    for config in configs:
        ecu_count, _ = dataset_dimensions(config)
        if ecu_count is None:
            continue
        grouped_by_ecu.setdefault(ecu_count, []).append((config, pooled[config]))

    for ecu_count in sorted(grouped_by_ecu.keys()):
        if ecu_count not in {5, 8}:
            continue
        grouped_by_ecu[ecu_count].sort(key=lambda item: dataset_sort_key(item[0]))
        figure_path = plot_ecu_group_cdf(ecu_count, grouped_by_ecu[ecu_count], output_dir)
        if figure_path is not None:
            generated.append(figure_path)

    print(f"Compare dir: {compare_dir}")
    print(f"Output dir: {output_dir}")
    for path in generated:
        print(f"Generated: {path}")


if __name__ == "__main__":
    main()
