#!/usr/bin/env python
"""Aggregate per-server and multi-server msprof kernel durations."""

from __future__ import annotations

import argparse
import csv
import json
import math
import sqlite3
import sys
from pathlib import Path


DEFAULT_KERNEL_NAME = "tilexr_ep_dispatch_memory_kernel"
LAST_SAMPLE_COUNT = 50
SCHEMA = "tilexr_msprof_op_summary_stats_v2"
STATIC_HASH_LENGTH = 16
SERVER_SAMPLES = "server-samples"
SERVER_AVERAGES = "server-averages"


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Select the last 50 kernel executions per card, average all card "
            "samples per server, then summarize the server averages."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help=(
            "op_summary CSV, ai_core_op_summary DB, prior summary JSON, "
            "or msprof output directory"
        ),
    )
    parser.add_argument(
        "--kernel-name",
        default=DEFAULT_KERNEL_NAME,
        help=(
            "canonical Op Name to match; the corresponding "
            "_static_<16-hex> names are also accepted "
            f"(default: {DEFAULT_KERNEL_NAME})"
        ),
    )
    parser.add_argument(
        "--aggregation",
        choices=(SERVER_SAMPLES, SERVER_AVERAGES),
        default=SERVER_SAMPLES,
        help=(
            "server-samples averages the last 50 samples from every CSV; "
            "server-averages reports min/average/max across prior server summaries"
        ),
    )
    parser.add_argument(
        "--expected-inputs",
        type=positive_int,
        help="fail unless input discovery produces exactly this many inputs",
    )
    parser.add_argument("--label", default="", help="label stored in the result")
    parser.add_argument("--output", type=Path, help="write the full result JSON here")
    return parser.parse_args()


def finite_number(value: object, context: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{context} is not finite")
    return number


def kernel_name_matches(actual_name: str, kernel_name: str) -> bool:
    if actual_name == kernel_name:
        return True
    kernel_suffix = "_kernel"
    if not kernel_name.endswith(kernel_suffix):
        return False
    static_prefix = kernel_name[: -len(kernel_suffix)] + "_static_"
    if not actual_name.startswith(static_prefix):
        return False
    static_hash = actual_name[len(static_prefix) :]
    return len(static_hash) == STATIC_HASH_LENGTH and all(
        character in "0123456789abcdefABCDEF" for character in static_hash
    )


def extract_csv_series(
    path: Path, kernel_name: str
) -> tuple[list[float], int, list[str]]:
    matches: list[tuple[float, float]] = []
    matched_kernel_names: set[str] = set()
    with path.open("r", encoding="utf-8-sig", newline="") as csv_file:
        reader = csv.DictReader(csv_file)
        required = {"Op Name", "Task Start Time(us)", "Task Duration(us)"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"{path}: missing required op_summary columns")
        for row_number, row in enumerate(reader, start=2):
            actual_name = row.get("Op Name", "")
            if not kernel_name_matches(actual_name, kernel_name):
                continue
            matched_kernel_names.add(actual_name)
            start = finite_number(
                row["Task Start Time(us)"], f"{path}:{row_number}: Task Start Time(us)"
            )
            duration = finite_number(
                row["Task Duration(us)"], f"{path}:{row_number}: Task Duration(us)"
            )
            matches.append((start, duration))

    matches.sort(key=lambda item: item[0])
    if len(matches) < LAST_SAMPLE_COUNT:
        raise ValueError(
            f"{path}: expected at least {LAST_SAMPLE_COUNT} kernel samples, "
            f"matched={len(matches)}"
        )
    return (
        [duration for _, duration in matches[-LAST_SAMPLE_COUNT:]],
        len(matches),
        sorted(matched_kernel_names),
    )


def extract_sqlite_series(
    path: Path, kernel_name: str
) -> tuple[list[float], int, list[str]]:
    matches: list[tuple[float, float]] = []
    matched_kernel_names: set[str] = set()
    connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        tables = {
            row[0]
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table'"
            )
        }
        required_tables = {"ge_summary", "task_time"}
        if not required_tables.issubset(tables):
            raise ValueError(f"{path}: missing required profiler tables")

        rows = connection.execute(
            """
            SELECT ge.op_name, task.start_time, task.duration_time
            FROM ge_summary AS ge
            JOIN task_time AS task
              ON ge.task_id = task.task_id
             AND ge.stream_id = task.stream_id
             AND ge.index_id = task.index_id
             AND ge.model_id = task.model_id
             AND ge.batch_id = task.batch_id
            """
        )
        for row_number, (actual_name, start_value, duration_value) in enumerate(
            rows, start=1
        ):
            if not kernel_name_matches(actual_name or "", kernel_name):
                continue
            start = finite_number(
                start_value, f"{path}:row {row_number}: start_time"
            )
            duration_ns = finite_number(
                duration_value, f"{path}:row {row_number}: duration_time"
            )
            if start < 0 or duration_ns < 0:
                continue
            matched_kernel_names.add(actual_name)
            matches.append((start, duration_ns / 1000.0))
    finally:
        connection.close()

    matches.sort(key=lambda item: item[0])
    if len(matches) < LAST_SAMPLE_COUNT:
        raise ValueError(
            f"{path}: expected at least {LAST_SAMPLE_COUNT} valid kernel samples, "
            f"matched={len(matches)}"
        )
    return (
        [duration for _, duration in matches[-LAST_SAMPLE_COUNT:]],
        len(matches),
        sorted(matched_kernel_names),
    )


def extract_server_average(path: Path, kernel_name: str) -> tuple[float, dict]:
    with path.open("r", encoding="utf-8") as json_file:
        payload = json.load(json_file)
    if (
        not isinstance(payload, dict)
        or payload.get("schema") != SCHEMA
        or payload.get("aggregation") != SERVER_SAMPLES
    ):
        raise ValueError(f"{path}: unsupported summary JSON")
    if payload.get("kernel") != kernel_name:
        raise ValueError(
            f"{path}: kernel mismatch: expected {kernel_name}, got {payload.get('kernel')}"
        )
    input_count = payload.get("input_count")
    total_sample_count = payload.get("total_sample_count")
    if (
        not isinstance(input_count, int)
        or input_count <= 0
        or payload.get("samples_per_input") != LAST_SAMPLE_COUNT
        or total_sample_count != input_count * LAST_SAMPLE_COUNT
    ):
        raise ValueError(f"{path}: invalid per-server sample counts")
    average = finite_number(payload.get("average_us"), f"{path}: average_us")
    return average, {
        "path": str(path),
        "label": payload.get("label", ""),
        "average_us": average,
        "card_count": input_count,
        "sample_count": total_sample_count,
    }


def latest_file(paths: list[Path]) -> Path | None:
    if not paths:
        return None
    return max(paths, key=lambda path: (path.stat().st_mtime_ns, path.name))


def discover_profile_inputs(root: Path) -> list[Path]:
    sessions = [root] if root.name.startswith("PROF_") else sorted(root.glob("PROF_*"))
    profile_inputs = []
    for session in sessions:
        csv_path = latest_file(
            list(session.glob("mindstudio_profiler_output/op_summary*.csv"))
        )
        if csv_path is not None:
            profile_inputs.append(csv_path)
            continue
        database_path = latest_file(
            list(session.glob("device_*/sqlite/ai_core_op_summary.db"))
        )
        if database_path is not None:
            profile_inputs.append(database_path)

    if not profile_inputs:
        raise ValueError(
            f"{root}: no op_summary CSV or ai_core_op_summary DB files found"
        )
    return profile_inputs


def expand_inputs(inputs: list[Path]) -> list[Path]:
    expanded: list[Path] = []
    for input_path in inputs:
        if input_path.is_dir():
            expanded.extend(discover_profile_inputs(input_path))
        elif input_path.is_file():
            expanded.append(input_path)
        else:
            raise ValueError(f"input does not exist: {input_path}")
    return expanded


def series_from_input(path: Path, kernel_name: str) -> tuple[list[float], dict]:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        series, matched, matched_kernel_names = extract_csv_series(path, kernel_name)
        source_type = "op_summary_csv"
    elif suffix == ".db":
        series, matched, matched_kernel_names = extract_sqlite_series(
            path, kernel_name
        )
        source_type = "ai_core_op_summary_db"
    else:
        raise ValueError(
            f"{path}: server-samples expects op_summary CSV or "
            "ai_core_op_summary DB input"
        )
    return series, {
        "path": str(path),
        "source_type": source_type,
        "matched_samples": matched,
        "matched_kernel_names": matched_kernel_names,
    }


def build_server_samples_result(
    input_paths: list[Path], kernel_name: str, label: str
) -> dict:
    series = []
    source_details = []
    for input_path in input_paths:
        input_series, details = series_from_input(input_path, kernel_name)
        series.append(input_series)
        source_details.append(details)

    samples = [duration for input_series in series for duration in input_series]
    return {
        "schema": SCHEMA,
        "aggregation": SERVER_SAMPLES,
        "label": label,
        "kernel": kernel_name,
        "unit": "us",
        "selection": f"last_{LAST_SAMPLE_COUNT}_per_input",
        "reduction": "mean_of_all_selected_samples",
        "input_count": len(input_paths),
        "samples_per_input": LAST_SAMPLE_COUNT,
        "total_sample_count": len(samples),
        "matched_kernel_names": sorted(
            {
                name
                for details in source_details
                for name in details["matched_kernel_names"]
            }
        ),
        "average_us": sum(samples) / len(samples),
        "minimum_us": min(samples),
        "maximum_us": max(samples),
        "sources": source_details,
    }


def build_server_averages_result(
    input_paths: list[Path], kernel_name: str, label: str
) -> dict:
    server_averages = []
    servers = []
    for input_path in input_paths:
        average, details = extract_server_average(input_path, kernel_name)
        server_averages.append(average)
        servers.append(details)

    return {
        "schema": SCHEMA,
        "aggregation": SERVER_AVERAGES,
        "label": label,
        "kernel": kernel_name,
        "unit": "us",
        "selection": f"last_{LAST_SAMPLE_COUNT}_per_card",
        "reduction": "min_max_mean_of_server_averages",
        "server_count": len(server_averages),
        "minimum_us": round(min(server_averages), 3),
        "maximum_us": round(max(server_averages), 3),
        "average_us": round(sum(server_averages) / len(server_averages), 3),
        "server_average_us": [round(value, 6) for value in server_averages],
        "servers": servers,
    }


def main() -> int:
    args = parse_args()
    try:
        input_paths = expand_inputs(args.inputs)
        if args.expected_inputs is not None and len(input_paths) != args.expected_inputs:
            raise ValueError(
                f"expected {args.expected_inputs} inputs, discovered {len(input_paths)}"
            )

        if args.aggregation == SERVER_SAMPLES:
            result = build_server_samples_result(
                input_paths, args.kernel_name, args.label
            )
        else:
            result = build_server_averages_result(
                input_paths, args.kernel_name, args.label
            )

        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(result, ensure_ascii=True, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )

        print("MSPROF_PERF_RESULT")
        print(f"minimum_us={result['minimum_us']:.3f}")
        print(f"maximum_us={result['maximum_us']:.3f}")
        print(f"average_us={result['average_us']:.3f}")
        return 0
    except (
        OSError,
        ValueError,
        TypeError,
        KeyError,
        csv.Error,
        sqlite3.Error,
        json.JSONDecodeError,
    ) as error:
        print(f"msprof kernel statistics failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
