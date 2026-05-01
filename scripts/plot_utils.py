from __future__ import annotations

import csv
from pathlib import Path
from typing import Dict, Iterable, List

import matplotlib.pyplot as plt


def configure_matplotlib(font_size: float = 10.0) -> None:
    plt.rcParams.update(
        {
            "figure.dpi": 160,
            "savefig.dpi": 220,
            "font.size": font_size,
            "font.sans-serif": ["Microsoft YaHei", "SimHei", "Noto Sans CJK SC", "DejaVu Sans"],
            "axes.unicode_minus": False,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.grid": True,
            "grid.alpha": 0.25,
            "grid.linestyle": "--",
            "axes.axisbelow": True,
        }
    )


def parse_sectioned_tsv(path: Path) -> Dict[str, List[dict]]:
    sections: Dict[str, List[dict]] = {}
    current_section = None
    header = None

    with path.open("r", encoding="utf-8", newline="") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                current_section = line[1:-1]
                header = None
                sections[current_section] = []
                continue
            if current_section is None:
                continue
            if header is None:
                header = line.split("\t")
                continue

            row = next(csv.reader([line], delimiter="\t"))
            if len(row) != len(header):
                raise ValueError(f"Malformed row in [{current_section}]: {line}")
            sections[current_section].append(dict(zip(header, row)))

    return sections


def to_number(value: str):
    try:
        if value.isdigit():
            return int(value)
        return float(value)
    except ValueError:
        return value


def convert_numeric_rows(rows: Iterable[dict], skip_keys: set[str] | None = None) -> List[dict]:
    skip_keys = skip_keys or set()
    converted: List[dict] = []
    for row in rows:
        item = {}
        for key, value in row.items():
            item[key] = value if key in skip_keys else to_number(value)
        converted.append(item)
    return converted


def normalize_dataset_name(name: str) -> str:
    dataset = name
    if dataset.startswith("msg_"):
        dataset = dataset[4:]
    if dataset.endswith("_tab"):
        dataset = dataset[:-4]
    return dataset


def short_dataset_label(name: str) -> str:
    return normalize_dataset_name(name).split("_")[0]


def long_dataset_label(name: str) -> str:
    parts = normalize_dataset_name(name).split("_")
    if len(parts) >= 3:
        return f"{parts[0]} ({parts[1]}, {parts[2]})"
    return normalize_dataset_name(name)


def dataset_dimensions(name: str) -> tuple[int | None, int | None]:
    dataset = normalize_dataset_name(name)
    parts = dataset.split("_")

    ecu_count = None
    signal_count = None
    for part in parts:
        if part.endswith("ecu") and part[:-3].isdigit():
            ecu_count = int(part[:-3])
        elif part.endswith("signals") and part[:-7].isdigit():
            signal_count = int(part[:-7])

    prefix = parts[0] if parts else dataset
    if (ecu_count is None or signal_count is None) and prefix.startswith("E") and "S" in prefix:
        ecu_part, signal_part = prefix[1:].split("S", maxsplit=1)
        if ecu_part.isdigit() and signal_part.isdigit():
            ecu_count = int(ecu_part)
            signal_count = int(signal_part)

    return ecu_count, signal_count


def group_datasets_by_ecu(names: Iterable[str]) -> List[tuple[int, List[str]]]:
    grouped: Dict[int, List[str]] = {}
    for name in names:
        ecu_count, _ = dataset_dimensions(name)
        if ecu_count is None:
            continue
        grouped.setdefault(ecu_count, []).append(name)

    return [(ecu_count, sorted(items, key=dataset_sort_key)) for ecu_count, items in sorted(grouped.items())]


def dataset_sort_key(name: str) -> tuple:
    dataset = normalize_dataset_name(name)
    ecu_count, signal_count = dataset_dimensions(dataset)
    if ecu_count is not None and signal_count is not None:
        return (0, ecu_count, signal_count, dataset)

    prefix = dataset.split("_")[0]
    if prefix.startswith("M") and prefix[1:].isdigit():
        return (1, int(prefix[1:]), dataset)
    return (999, dataset)


def resolve_compare_dir(input_path: Path) -> Path:
    path = input_path.resolve()
    if path.is_dir() and path.name in {"comparison_reports", "compare"}:
        return path
    if (path / "comparison_reports").is_dir():
        return path / "comparison_reports"
    if (path / "compare").is_dir():
        return path / "compare"
    raise FileNotFoundError(f"无法定位 comparison report 目录: {path}")
