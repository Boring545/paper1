#!/usr/bin/env python
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_utils import configure_matplotlib, convert_numeric_rows, parse_sectioned_tsv


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="根据重传概率报告生成聚合图。")
    parser.add_argument("report", type=Path, help="retry report 文件路径")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="输出目录，默认 <report_parent>/retry_distribution_figures",
    )
    return parser.parse_args()


def choose_epsilon(positive_values: list[float]) -> float:
    if not positive_values:
        return 1e-20
    return min(positive_values) / 10.0


def group_timeout_probability_by_period(frame_rows: list[dict]) -> list[dict]:
    grouped: dict[int, list[float]] = defaultdict(list)
    for row in frame_rows:
        grouped[int(row["period_ms"])].append(float(row["p_timeout"]))

    positive_values = [value for values in grouped.values() for value in values if value > 0]
    epsilon = choose_epsilon(positive_values)

    result = []
    for period in sorted(grouped):
        values = grouped[period]
        avg_probability = sum(values) / len(values)
        max_probability = max(values)
        result.append(
            {
                "period_ms": period,
                "frame_count": len(values),
                "avg_p_timeout_raw": avg_probability,
                "max_p_timeout_raw": max_probability,
                "avg_p_timeout_plot": avg_probability if avg_probability > 0 else epsilon,
                "max_p_timeout_plot": max_probability if max_probability > 0 else epsilon,
            }
        )
    return result


def group_retry_distribution_by_period(frame_rows: list[dict], retry_rows: list[dict]) -> list[dict]:
    frame_period = {int(row["frame_id"]): int(row["period_ms"]) for row in frame_rows}
    grouped: dict[int, dict[int, list[float]]] = defaultdict(lambda: defaultdict(list))

    for row in retry_rows:
        frame_id = int(row["frame_id"])
        period = frame_period.get(frame_id)
        if period is None:
            continue
        grouped[period][int(row["retry_count"])].append(float(row["probability"]))

    result = []
    for period in sorted(grouped):
        retry_map = grouped[period]
        sorted_retry_counts = sorted(retry_map)
        tail_retry_count = sorted_retry_counts[-1]
        for retry_count in sorted_retry_counts:
            values = retry_map[retry_count]
            label = f">={retry_count}次" if retry_count == tail_retry_count else f"{retry_count}次"
            result.append(
                {
                    "period_ms": period,
                    "retry_count": retry_count,
                    "retry_bucket_label": label,
                    "frame_count": len(values),
                    "avg_probability": sum(values) / len(values),
                    "max_probability": max(values),
                }
            )
    return result


def write_timeout_summary(output_dir: Path, rows: list[dict]) -> Path:
    output_path = output_dir / "period_group_timeout_probability_tab.txt"
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# 按周期分组聚合的帧超时概率统计\n")
        handle.write("# plot 列会把 0 替换成自适应 epsilon，便于对数坐标绘图。\n\n")
        handle.write(
            "period_ms\tframe_count\tavg_p_timeout_raw\tmax_p_timeout_raw\tavg_p_timeout_plot\tmax_p_timeout_plot\n"
        )
        for row in rows:
            handle.write(
                f"{row['period_ms']}\t{row['frame_count']}\t{row['avg_p_timeout_raw']}\t"
                f"{row['max_p_timeout_raw']}\t{row['avg_p_timeout_plot']}\t{row['max_p_timeout_plot']}\n"
            )
    return output_path


def write_retry_distribution_summary(output_dir: Path, rows: list[dict]) -> Path:
    output_path = output_dir / "period_group_retry_distribution_tab.txt"
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# 按周期分组聚合的重传次数分布统计\n")
        handle.write("# 每个周期组内，统计各重传次数桶的平均概率和最大概率。\n")
        handle.write("# 最后一个桶统一记为 >=k次，表示保守尾部桶。\n\n")
        handle.write("period_ms\tretry_count\tretry_bucket_label\tframe_count\tavg_probability\tmax_probability\n")
        for row in rows:
            handle.write(
                f"{row['period_ms']}\t{row['retry_count']}\t{row['retry_bucket_label']}\t{row['frame_count']}\t"
                f"{row['avg_probability']}\t{row['max_probability']}\n"
            )
    return output_path


def plot_retry_distribution(rows: list[dict], output_dir: Path) -> Path:
    periods = sorted({int(row["period_ms"]) for row in rows})
    fig, axes = plt.subplots(4, 2, figsize=(12.0, 10.4), constrained_layout=True, squeeze=False)

    for index, period in enumerate(periods):
        ax = axes[index // 2][index % 2]
        period_rows = sorted(
            [row for row in rows if int(row["period_ms"]) == period],
            key=lambda row: int(row["retry_count"]),
        )
        x_positions = list(range(len(period_rows)))
        labels = [row["retry_bucket_label"] for row in period_rows]
        avg_values = [row["avg_probability"] for row in period_rows]
        max_values = [row["max_probability"] for row in period_rows]

        bar_width = 0.36
        ax.bar(
            [x - bar_width / 2 for x in x_positions],
            avg_values,
            width=bar_width,
            color="#1b6ca8",
            edgecolor="black",
            linewidth=0.5,
            label="平均概率" if index == 0 else None,
        )
        ax.bar(
            [x + bar_width / 2 for x in x_positions],
            max_values,
            width=bar_width,
            color="#d66a1f",
            edgecolor="black",
            linewidth=0.5,
            label="最大概率" if index == 0 else None,
        )
        ax.set_yscale("log")
        ax.set_xticks(x_positions)
        ax.set_xticklabels(labels)
        ax.set_xlabel("重传次数")
        ax.set_ylabel("概率")
        ax.set_title(f"周期组 {period} ms")

    for index in range(len(periods), 8):
        axes[index // 2][index % 2].axis("off")

    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False, bbox_to_anchor=(0.5, 1.02))
    fig.suptitle("按周期分组的重传次数分布", fontsize=14)

    output_path = output_dir / "period_group_retry_distribution.png"
    fig.savefig(output_path, bbox_inches="tight")
    plt.close(fig)
    return output_path


def main() -> None:
    args = parse_args()
    report_path = args.report.resolve()
    output_dir = args.output_dir.resolve() if args.output_dir is not None else report_path.parent / "retry_distribution_figures"
    output_dir.mkdir(parents=True, exist_ok=True)

    configure_matplotlib()
    sections = {
        name: convert_numeric_rows(rows)
        for name, rows in parse_sectioned_tsv(report_path).items()
    }
    frame_rows = sections["frame_summary"]
    timeout_rows = group_timeout_probability_by_period(frame_rows)
    retry_rows = group_retry_distribution_by_period(frame_rows, sections["frame_retry_distribution"])

    generated = [
        write_timeout_summary(output_dir, timeout_rows),
        write_retry_distribution_summary(output_dir, retry_rows),
        plot_retry_distribution(retry_rows, output_dir),
    ]

    print(f"Retry report: {report_path}")
    print(f"Output dir: {output_dir}")
    for path in generated:
        print(f"Generated: {path}")


if __name__ == "__main__":
    main()
