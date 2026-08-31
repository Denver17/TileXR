"""Public report pipeline and command-line entry point."""

import argparse
import json
import math
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Mapping, Optional, Sequence, Tuple

from .analysis import analyze_run
from .html_report import render_analysis_markdown, render_report_html
from .io import atomic_write_json, atomic_write_text
from .mapping import MappingInput
from .model import ProfilingInputError
from .pairing import load_profile_run
from .perfetto import render_perfetto


_ARTIFACTS = ("perfetto_trace.json", "analysis.md", "report.html")


@dataclass(frozen=True)
class GeneratedReports:
    """Paths and completeness status for one published report set."""

    status: str
    output_dir: Path
    artifacts: Tuple[Path, ...]


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate TileXR reports from mapping.v1 and records.v1 inputs"
    )
    parser.add_argument(
        "--mapping",
        required=True,
        type=Path,
        help="human-authored tilexr_profiling_mapping.v1 JSON document",
    )
    parser.add_argument(
        "--records",
        action="append",
        required=True,
        type=Path,
        help="tilexr_profiling_records.v1 binary file; repeat as needed",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        type=Path,
        help="directory for perfetto_trace.json, analysis.md, and report.html",
    )
    parser.add_argument(
        "--attribute",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="host-side provenance to add to run metadata; repeat as needed",
    )
    return parser


def _parse_attributes(values: Sequence[str]) -> dict:
    attributes = {}
    for item in values:
        key, separator, raw_value = item.partition("=")
        if (
            not separator
            or not key
            or key.strip() != key
            or any(character.isspace() for character in key)
        ):
            raise ValueError(f"invalid profiling attribute, expected KEY=VALUE: {item!r}")
        if key in attributes:
            raise ValueError(f"duplicate profiling attribute: {key}")
        try:
            value = json.loads(raw_value)
        except json.JSONDecodeError:
            value = raw_value
        attributes[key] = value
    return attributes


def _validate_attribute_value(value: Any, where: str, depth: int = 0) -> Any:
    if depth > 16:
        raise ValueError(f"{where} exceeds maximum nesting depth")
    if value is None or isinstance(value, (str, bool, int)):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"{where} must contain only finite numbers")
        return value
    if isinstance(value, list):
        return [
            _validate_attribute_value(item, f"{where}[{index}]", depth + 1)
            for index, item in enumerate(value)
        ]
    if isinstance(value, Mapping):
        result = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise ValueError(f"{where} keys must be strings")
            result[key] = _validate_attribute_value(
                item, f"{where}.{key}", depth + 1
            )
        return result
    raise ValueError(f"{where} contains unsupported value type {type(value).__name__}")


def _validate_attributes(attributes: Mapping[str, Any]) -> dict:
    return _validate_attribute_value(attributes, "report attributes")


def _publish_new_directory(staging: Path, output_dir: Path) -> bool:
    try:
        os.replace(staging, output_dir)
        return True
    except FileExistsError:
        return False
    except OSError:
        if output_dir.exists():
            return False
        raise


def _write_artifacts(output_dir: Path, perfetto: object, markdown: str, report: str) -> None:
    output_dir = output_dir.resolve()
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    if output_dir.exists():
        if not output_dir.is_dir() or any(output_dir.iterdir()):
            raise ValueError(f"output directory is not empty: {output_dir}")
        output_dir.rmdir()
    staging = Path(
        tempfile.mkdtemp(prefix=f".{output_dir.name}.staging.", dir=str(output_dir.parent))
    )
    published = False
    try:
        atomic_write_json(staging / "perfetto_trace.json", perfetto)
        atomic_write_text(staging / "analysis.md", markdown)
        atomic_write_text(staging / "report.html", report)
        if not _publish_new_directory(staging, output_dir):
            raise ValueError(f"output directory already exists: {output_dir}")
        published = True
    finally:
        if not published and staging.exists():
            shutil.rmtree(staging)


def generate_reports(
    mapping: MappingInput,
    records: Sequence[Path],
    output_dir: Path,
    attributes: Optional[Mapping[str, Any]] = None,
) -> GeneratedReports:
    """Generate and atomically publish reports from a mapping and record files."""
    run = load_profile_run(mapping, records)
    if attributes:
        merged_attributes = dict(run.attributes)
        merged_attributes.update(_validate_attributes(attributes))
        run = replace(run, attributes=merged_attributes)

    analysis = analyze_run(run)
    perfetto = render_perfetto(run, analysis)
    markdown = render_analysis_markdown(run, analysis)
    report = render_report_html(run, analysis)
    destination = Path(output_dir).resolve()
    _write_artifacts(destination, perfetto, markdown, report)
    return GeneratedReports(
        status=run.status,
        output_dir=destination,
        artifacts=tuple(destination / name for name in _ARTIFACTS),
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = generate_reports(
            args.mapping,
            args.records,
            args.output_dir,
            _parse_attributes(args.attribute),
        )
    except (ProfilingInputError, OSError, UnicodeError, ValueError) as exc:
        print(f"profiling error: {exc}", file=sys.stderr)
        return 1

    print(f"wrote profiling report to {result.output_dir}")
    return 2 if result.status == "incomplete" else 0
