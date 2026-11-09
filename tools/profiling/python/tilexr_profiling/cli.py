# Purpose: Publish offline reports and expose command-line analysis entry points.
# 用途与关系：发布离线报表，并提供命令行分析入口。
"""Public report pipeline and command-line entry point."""

import argparse
import json
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Mapping, Optional, Sequence, Tuple

from .analysis import analyze_run
from .html_report import iter_analysis_markdown, iter_report_html
from .io import atomic_write_json, atomic_write_chunks, json_chunks
from .mapping import MappingInput
from .model import ProfilingInputError
from .pairing import load_profile_run
from .perfetto import iter_perfetto
from .provenance import validate_attributes


ARTIFACT_NAMES = {"perfetto": "perfetto_trace.json", "markdown": "analysis.md",
                  "html": "report.html", "json": "analysis.json", "csv": "analysis.csv"}
MAX_OUTPUT_BYTES = 256 * 1024 * 1024


@dataclass(frozen=True)
class GeneratedReports:
    """Paths and completeness status for one published report set."""

    status: str
    output_dir: Path
    artifacts: Tuple[Path, ...]
    health_status: str = "unverified"
    metadata_readiness: str = "not_provided"
    coverage: dict = field(default_factory=dict)

    @property
    def capture_status(self):
        return self.status


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate TileXR reports from mapping.v1 and records.v1 inputs"
    )
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument(
        "--mapping",
        type=Path,
        help="human-authored tilexr_profiling_mapping.v1 JSON document",
    )
    inputs.add_argument("--capture-set", type=Path, help="versioned multi-measurement capture set")
    parser.add_argument("--artifacts", nargs="+", choices=tuple(ARTIFACT_NAMES),
                        help="explicit output subset; default: perfetto markdown html")
    parser.add_argument("--baseline", type=Path, help="compare against analysis.json")
    parser.add_argument("--threshold-relative", type=float, default=0.05)
    parser.add_argument("--threshold-absolute-us", type=float, default=0.0)
    parser.add_argument("--max-input-bytes", type=int)
    parser.add_argument("--max-records", type=int)
    parser.add_argument("--max-memory-bytes", type=int)
    parser.add_argument(
        "--records",
        action="append",
        default=[],
        type=Path,
        help="tilexr_profiling_records.v1 binary file; repeat as needed",
    )
    parser.add_argument("--manifest", type=Path,
                        help="tilexr_profiling_manifest.v1 identity, coverage, metrics and health sidecar")
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


def _publish_artifacts(output_dir, writers, *, max_output_bytes=MAX_OUTPUT_BYTES,
                       publication_manifest=False):
    """Publish exactly the selected set; failed writers never expose a partial set."""
    output_dir = Path(output_dir)
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
        total = 0
        for name, writer in writers.items():
            if Path(name).name != name:
                raise ProfilingInputError("artifact name must be a filename")
            writer(staging / name)
            total += (staging / name).stat().st_size
            if total > max_output_bytes:
                raise ProfilingInputError("profiling output byte budget exceeded")
        if publication_manifest:
            atomic_write_json(staging / "artifacts.json", {
                "schema": "tilexr_profiling_artifacts.v1", "artifacts": list(writers)})
            total += (staging / "artifacts.json").stat().st_size
            if total > max_output_bytes:
                raise ProfilingInputError("profiling output byte budget exceeded")
        if not _publish_new_directory(staging, output_dir):
            raise ValueError(f"output directory already exists: {output_dir}")
        published = True
    finally:
        if not published and staging.exists():
            shutil.rmtree(staging)


def _artifact_writers(run, analysis, artifacts=None):
    selected = ("perfetto", "markdown", "html") if artifacts is None else tuple(artifacts)
    if not selected or len(set(selected)) != len(selected) or any(key not in ARTIFACT_NAMES for key in selected):
        raise ProfilingInputError("artifacts must be a nonempty unique selection")

    def machine_document():
        from .serialization import build_analysis_document
        return build_analysis_document(run, analysis)

    def csv_chunks():
        from .serialization import iter_analysis_csv
        return iter_analysis_csv(run, analysis)

    renderers = {
        "perfetto": lambda: iter_perfetto(run, analysis),
        "json": lambda: json_chunks(machine_document()),
        "markdown": lambda: iter_analysis_markdown(run, analysis),
        "html": lambda: iter_report_html(run, analysis, trace_available="perfetto" in selected),
        "csv": csv_chunks,
    }
    def writer(factory):
        return lambda path: atomic_write_chunks(path, factory(), max_bytes=MAX_OUTPUT_BYTES)
    return {ARTIFACT_NAMES[key]: writer(renderers[key]) for key in selected}


def generate_reports(
    mapping: MappingInput,
    records: Sequence[Path],
    output_dir: Path,
    attributes: Optional[Mapping[str, Any]] = None,
    *, manifest=None, artifacts=None, limits=None,
) -> GeneratedReports:
    """Generate and atomically publish reports from a mapping and record files."""
    run = _load_run(mapping, records, manifest=manifest, limits=limits, attributes=attributes)

    analysis = analyze_run(run)
    destination = Path(output_dir).resolve()
    writers = _artifact_writers(run, analysis, artifacts)
    _publish_artifacts(destination, writers, publication_manifest=artifacts is not None)
    return GeneratedReports(
        status=run.status,
        output_dir=destination,
        artifacts=tuple(destination / name for name in writers) +
                  ((destination / "artifacts.json",) if artifacts is not None else ()),
        health_status=run.health_status,
        metadata_readiness=run.metadata_readiness,
        coverage=run.coverage,
    )


def _load_run(mapping, records, *, manifest=None, limits=None, attributes=None):
    run = load_profile_run(mapping, records, manifest=manifest, limits=limits)
    if attributes is not None:
        merged = dict(run.attributes)
        merged.update(validate_attributes(attributes))
        run = replace(run, attributes=merged)
    return run


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        from .budgets import InputLimits
        requested_limits = {"max_bytes": args.max_input_bytes, "max_records": args.max_records,
                            "max_memory_bytes": args.max_memory_bytes}
        limits = InputLimits(**{key: value for key, value in requested_limits.items() if value is not None})
        if args.capture_set:
            if args.records or args.manifest or args.attribute or args.baseline:
                raise ProfilingInputError("capture set owns member records, manifests and attributes")
            from .capture_set import generate_capture_set_reports
            result = generate_capture_set_reports(args.capture_set, args.output_dir,
                                                   artifacts=args.artifacts, limits=limits)
            print(f"wrote profiling capture set to {result.output_dir}")
            return 2 if result.status == "incomplete" else 0
        if args.baseline:
            from .comparison import Threshold, compare_reports
            from .serialization import build_analysis_document
            run = _load_run(args.mapping, args.records, manifest=args.manifest, limits=limits,
                            attributes=_parse_attributes(args.attribute))
            analysis = analyze_run(run)
            comparison = compare_reports(args.baseline, build_analysis_document(run, analysis),
                [Threshold(absolute=args.threshold_absolute_us, relative=args.threshold_relative)])
            writers = _artifact_writers(run, analysis, args.artifacts)
            writers["comparison.json"] = lambda path: atomic_write_json(path, comparison)
            _publish_artifacts(args.output_dir, writers, publication_manifest=True)
            print(json.dumps(comparison, ensure_ascii=False, allow_nan=False))
            return {"pass": 0, "regression": 3, "incomparable": 4, "missing_data": 2}[comparison["status"]]
        result = generate_reports(
            args.mapping,
            args.records,
            args.output_dir,
            _parse_attributes(args.attribute),
            manifest=args.manifest,
            artifacts=args.artifacts, limits=limits,
        )
    except (ProfilingInputError, OSError, UnicodeError, ValueError) as exc:
        print(f"profiling error: {exc}", file=sys.stderr)
        return 1

    print(f"wrote profiling report to {result.output_dir}")
    return 2 if result.status == "incomplete" or result.health_status == "incomplete" else 0
