#!/usr/bin/env python
"""Aggregate the last 50 kernel durations from msprof op_summary CSV files."""

import argparse
import csv
import json
import math
import sys
from pathlib import Path


DEFAULT_KERNEL_NAME = "tilexr_ep_dispatch_memory_kernel"
LAST_SAMPLE_COUNT = 50
SCHEMA = "tilexr_msprof_op_summary_stats_v1"


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "For every input, select the last 50 kernel executions, take the "
            "slowest input in each round, and report min/average/max."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help="op_summary CSV, prior summary JSON, or msprof output directory",
    )
    parser.add_argument(
        "--kernel-name",
        default=DEFAULT_KERNEL_NAME,
        help=f"exact Op Name to match (default: {DEFAULT_KERNEL_NAME})",
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


def extract_csv_series(path: Path, kernel_name: str) -> tuple[list[float], int]:
    matches: list[tuple[float, float]] = []
    with path.open("r", encoding="utf-8-sig", newline="") as csv_file:
        reader = csv.DictReader(csv_file)
        required = {"Op Name", "Task Start Time(us)", "Task Duration(us)"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"{path}: missing required op_summary columns")
        for row_number, row in enumerate(reader, start=2):
            if row.get("Op Name") != kernel_name:
                continue
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
    return [duration for _, duration in matches[-LAST_SAMPLE_COUNT:]], len(matches)


def extract_summary_series(path: Path, kernel_name: str) -> tuple[list[float], int]:
    with path.open("r", encoding="utf-8") as json_file:
        payload = json.load(json_file)
    if not isinstance(payload, dict) or payload.get("schema") != SCHEMA:
        raise ValueError(f"{path}: unsupported summary JSON")
    if payload.get("kernel") != kernel_name:
        raise ValueError(
            f"{path}: kernel mismatch: expected {kernel_name}, got {payload.get('kernel')}"
        )
    raw_durations = payload.get("round_durations_us")
    if not isinstance(raw_durations, list):
        raise ValueError(f"{path}: round_durations_us is missing")
    durations = [
        finite_number(value, f"{path}: round_durations_us") for value in raw_durations
    ]
    if len(durations) < LAST_SAMPLE_COUNT:
        raise ValueError(
            f"{path}: expected at least {LAST_SAMPLE_COUNT} summary rounds, "
            f"matched={len(durations)}"
        )
    return durations[-LAST_SAMPLE_COUNT:], len(durations)


def discover_csv_files(root: Path) -> list[Path]:
    latest_by_session: dict[Path, Path] = {}
    for candidate in root.glob("PROF_*/mindstudio_profiler_output/op_summary_*.csv"):
        session = candidate.parent.parent
        previous = latest_by_session.get(session)
        if previous is None or (candidate.stat().st_mtime_ns, candidate.name) > (
            previous.stat().st_mtime_ns,
            previous.name,
        ):
            latest_by_session[session] = candidate
    csv_files = sorted(latest_by_session.values(), key=lambda path: str(path))
    if not csv_files:
        raise ValueError(f"{root}: no op_summary CSV files found")
    return csv_files


def expand_inputs(inputs: list[Path]) -> list[Path]:
    expanded: list[Path] = []
    for input_path in inputs:
        if input_path.is_dir():
            expanded.extend(discover_csv_files(input_path))
        elif input_path.is_file():
            expanded.append(input_path)
        else:
            raise ValueError(f"input does not exist: {input_path}")
    return expanded


def series_from_input(path: Path, kernel_name: str) -> tuple[list[float], dict]:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        series, matched = extract_csv_series(path, kernel_name)
        source_type = "op_summary_csv"
    elif suffix == ".json":
        series, matched = extract_summary_series(path, kernel_name)
        source_type = "summary"
    else:
        raise ValueError(f"{path}: expected .csv or .json input")
    return series, {
        "path": str(path),
        "source_type": source_type,
        "matched_samples": matched,
    }


def main() -> int:
    args = parse_args()
    try:
        input_paths = expand_inputs(args.inputs)
        if args.expected_inputs is not None and len(input_paths) != args.expected_inputs:
            raise ValueError(
                f"expected {args.expected_inputs} inputs, discovered {len(input_paths)}"
            )

        series = []
        source_details = []
        for input_path in input_paths:
            input_series, details = series_from_input(input_path, args.kernel_name)
            series.append(input_series)
            source_details.append(details)

        round_durations = [
            max(input_series[round_index] for input_series in series)
            for round_index in range(LAST_SAMPLE_COUNT)
        ]
        result = {
            "schema": SCHEMA,
            "label": args.label,
            "kernel": args.kernel_name,
            "unit": "us",
            "selection": f"last_{LAST_SAMPLE_COUNT}_per_input",
            "reduction": "maximum_per_round",
            "input_count": len(input_paths),
            "sample_count": LAST_SAMPLE_COUNT,
            "average_us": round(sum(round_durations) / LAST_SAMPLE_COUNT, 3),
            "minimum_us": round(min(round_durations), 3),
            "maximum_us": round(max(round_durations), 3),
            "round_durations_us": [round(value, 6) for value in round_durations],
            "sources": source_details,
        }

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
    except (OSError, ValueError, TypeError, KeyError, csv.Error, json.JSONDecodeError) as error:
        print(f"msprof kernel statistics failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
