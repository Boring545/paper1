#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path


SIGNAL_COUNTS = [50, 80, 120, 150, 200, 250]
ECU_COUNTS = [5, 8]
SCHEMES = ("foundation", "baseline1")
THRESHOLDS_MS = [1, 2, 4, 8, 16]


def parse_section(path: Path, section_name: str) -> list[dict[str, str]]:
    current = None
    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    with path.open("r", encoding="utf-8", errors="ignore", newline="") as handle:
        for raw in handle:
            line = raw.rstrip("\r\n")
            if line.startswith("[") and line.endswith("]"):
                current = line[1:-1]
                header = None
                continue
            if current != section_name or not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if header is None:
                header = parts
            else:
                rows.append(dict(zip(header, parts)))
    return rows


def dataset_dimensions(config: str) -> tuple[int | None, int | None]:
    # E5S250_5ecu_250signals
    try:
        prefix = config.split("_", 1)[0]
        e_part, s_part = prefix[1:].split("S", 1)
        return int(e_part), int(s_part)
    except Exception:
        return None, None


def fmt_percent(value: float) -> str:
    return f"{value * 100:.2f}%"


def export_bandwidth_reduction(analysis_dir: Path, output_dir: Path) -> Path:
    rows = parse_section(analysis_dir / "compare_summary_tab.txt", "bandwidth_utilization")
    by_dataset: dict[str, dict[str, dict[str, str]]] = defaultdict(dict)
    for row in rows:
        if row.get("scheme") in SCHEMES:
            by_dataset[row["dataset"]][row["scheme"]] = row

    values: dict[tuple[int, int], list[tuple[float, float]]] = defaultdict(list)
    for schemes in by_dataset.values():
        if "foundation" not in schemes or "baseline1" not in schemes:
            continue
        foundation = schemes["foundation"]
        baseline1 = schemes["baseline1"]
        if foundation.get("schedulable") != "1" or baseline1.get("schedulable") != "1":
            continue
        ecu_count, signal_count = dataset_dimensions(foundation["config"])
        if ecu_count is None or signal_count is None:
            continue
        values[(ecu_count, signal_count)].append(
            (
                float(foundation["compare_bandwidth_utilization"]),
                float(baseline1["compare_bandwidth_utilization"]),
            )
        )

    output = output_dir / "bandwidth_reduction_signal_vs_frame_tab.txt"
    with output.open("w", encoding="utf-8", newline="") as handle:
        handle.write("表3-4 信号备份方法相对报文备份方法的平均带宽利用率降低比例\n\n")
        handle.write("ECU数量\t" + "\t".join(f"{count}信号" for count in SIGNAL_COUNTS) + "\n")
        for ecu_count in ECU_COUNTS:
            cells = [str(ecu_count)]
            for signal_count in SIGNAL_COUNTS:
                samples = values.get((ecu_count, signal_count), [])
                if not samples:
                    cells.append("")
                    continue
                avg_foundation = sum(item[0] for item in samples) / len(samples)
                avg_baseline1 = sum(item[1] for item in samples) / len(samples)
                reduction = (avg_baseline1 - avg_foundation) / avg_baseline1 if avg_baseline1 > 0 else 0.0
                cells.append(fmt_percent(reduction))
            handle.write("\t".join(cells) + "\n")
    return output


def read_cdf_points(analysis_dir: Path) -> list[dict[str, str]]:
    cdf_path = analysis_dir / "comparison_figures" / "signal_wcrt_cdf" / "signal_wcrt_cdf_points_tab.txt"
    rows: list[dict[str, str]] = []
    with cdf_path.open("r", encoding="utf-8", errors="ignore", newline="") as handle:
        header: list[str] | None = None
        for raw in handle:
            line = raw.rstrip("\r\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "config":
                header = parts
                continue
            if header is not None:
                rows.append(dict(zip(header, parts)))
    return rows


def cdf_at_threshold(points: list[tuple[float, float]], threshold_ms: float) -> float:
    result = 0.0
    for wcrt, cdf in points:
        if wcrt <= threshold_ms + 1e-12:
            result = cdf
        else:
            break
    return result


def export_cdf_threshold_diff(analysis_dir: Path, output_dir: Path) -> list[Path]:
    rows = read_cdf_points(analysis_dir)
    grouped: dict[tuple[int, int, str], list[tuple[float, float]]] = defaultdict(list)
    for row in rows:
        ecu_count, signal_count = dataset_dimensions(row["config"])
        if ecu_count is None or signal_count is None:
            continue
        grouped[(ecu_count, signal_count, row["scheme"])].append(
            (float(row["threshold_wcrt_ms"]), float(row["cdf"]))
        )
    for key in list(grouped.keys()):
        grouped[key].sort()

    outputs: list[Path] = []
    for ecu_count in ECU_COUNTS:
        output = output_dir / f"wcrt_cdf_threshold_diff_{ecu_count}ecu_tab.txt"
        with output.open("w", encoding="utf-8", newline="") as handle:
            handle.write(f"表3-{5 if ecu_count == 5 else 6} 典型 WCRT 阈值处累计比例差值({ecu_count} ECU)\n\n")
            handle.write("信号数量\t" + "\t".join(f"WCRT≤{threshold}ms" for threshold in THRESHOLDS_MS) + "\n")
            for signal_count in SIGNAL_COUNTS:
                cells = [str(signal_count)]
                foundation = grouped.get((ecu_count, signal_count, "foundation"), [])
                baseline1 = grouped.get((ecu_count, signal_count, "baseline1"), [])
                for threshold in THRESHOLDS_MS:
                    diff = cdf_at_threshold(foundation, threshold) - cdf_at_threshold(baseline1, threshold)
                    cells.append(fmt_percent(diff))
                handle.write("\t".join(cells) + "\n")
        outputs.append(output)
    return outputs


def main() -> None:
    parser = argparse.ArgumentParser(description="Export Word-ready paper summary tables.")
    parser.add_argument("analysis_dir", type=Path)
    args = parser.parse_args()

    analysis_dir = args.analysis_dir.resolve()
    output_dir = analysis_dir / "comparison_figures"
    output_dir.mkdir(parents=True, exist_ok=True)

    generated = [export_bandwidth_reduction(analysis_dir, output_dir)]
    generated.extend(export_cdf_threshold_diff(analysis_dir, output_dir))
    for path in generated:
        print(f"Generated: {path}")


if __name__ == "__main__":
    main()
