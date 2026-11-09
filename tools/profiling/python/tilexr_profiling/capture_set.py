# Purpose: Aggregate repeated captures while retaining sampling and coverage provenance.
# 用途与关系：聚合重复 capture，并保留采样策略及覆盖范围信息。
"""Versioned multi-measurement orchestration over the single-capture engine."""

import html
import json
import shutil
import statistics
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Optional
from uuid import UUID

from .analysis import analyze_run
from .io import atomic_write_json, atomic_write_text
from .manifest import _document, _uint, mapping_fingerprint
from .mapping import _array, _known_keys, _required_keys, _string
from .model import ProfilingInputError
from .pairing import load_profile_run

CAPTURE_SET_SCHEMA = "tilexr_profiling_capture_set.v1"
CAPTURE_SET_ANALYSIS_SCHEMA = "tilexr_profiling_capture_set_analysis.v1"
MAX_MEASUREMENTS = 1024
MAX_MEMBER_FILES = 4096
MAX_SET_BYTES = 512 * 1024 * 1024


@dataclass(frozen=True)
class Measurement:
    measurement_id: str
    iteration: int
    attempt_id: str
    capture_id: Optional[str]
    warmup: bool
    mapping: object
    records: tuple
    manifest: object = None


@dataclass(frozen=True)
class CaptureSet:
    experiment_id: str
    measurements: tuple
    source: str


@dataclass(frozen=True)
class MeasurementAnalysis:
    measurement: Measurement
    run: object
    analysis: object
    included: bool


@dataclass(frozen=True)
class CaptureSetAnalysis:
    capture_set: CaptureSet
    members: tuple
    summary: dict


@dataclass(frozen=True)
class GeneratedCaptureSet:
    status: str
    output_dir: Path
    artifacts: tuple
    summary: dict


def _member_path(value, root):
    name = _string(value, "capture-set member path")
    path = PurePosixPath(name)
    if path.is_absolute() or "\\" in name or ":" in name or any(part in {"..", "."} for part in name.split("/")):
        raise ProfilingInputError("capture-set members require relative POSIX paths without traversal")
    resolved = (root / Path(*path.parts)).resolve()
    if not resolved.is_relative_to(root):
        raise ProfilingInputError("capture-set member escapes input directory")
    if not resolved.is_file():
        raise ProfilingInputError(f"capture-set member is not a regular file: {name}")
    return resolved


def load_capture_set(value, *, base_dir=None):
    """Load explicit measurement membership, never group files by stream ID.

    A document's paths are relative to its directory. For a JSON object callers
    must supply base_dir. Absolute paths, traversal and escaping symlinks fail.
    """
    if isinstance(value, CaptureSet):
        return value
    raw = _document(value)
    if base_dir is None and isinstance(value, dict):
        raise ProfilingInputError("capture-set objects require base_dir")
    root = Path(base_dir if base_dir is not None else Path(value).resolve().parent).resolve()
    _known_keys(raw, {"schema", "experiment_id", "measurements"}, "capture set")
    _required_keys(raw, {"schema", "experiment_id", "measurements"}, "capture set")
    if raw["schema"] != CAPTURE_SET_SCHEMA:
        raise ProfilingInputError("unsupported capture-set schema")
    experiment_id = _string(raw["experiment_id"], "experiment_id")
    values = _array(raw["measurements"], "measurements")
    if not 1 <= len(values) <= MAX_MEASUREMENTS:
        raise ProfilingInputError("capture set requires 1..1024 measurements")
    members, ids, attempts, captures = [], set(), set(), set()
    file_paths = set()
    for item in values:
        if not isinstance(item, dict):
            raise ProfilingInputError("capture-set measurement must be an object")
        required = {"measurement_id", "iteration", "attempt_id", "capture_id", "warmup", "mapping", "records"}
        _required_keys(item, required, "measurement")
        _known_keys(item, required | {"manifest"}, "measurement")
        measurement_id = _string(item["measurement_id"], "measurement_id")
        attempt = _string(item["attempt_id"], "attempt_id")
        iteration = _uint(item["iteration"], "iteration")
        if measurement_id in ids or attempt in attempts:
            raise ProfilingInputError("duplicate measurement ID or attempt ID")
        ids.add(measurement_id)
        attempts.add(attempt)
        if not isinstance(item["warmup"], bool):
            raise ProfilingInputError("measurement warmup must be a boolean")
        capture = item["capture_id"]
        if capture is not None:
            try:
                capture = str(UUID(capture))
            except (ValueError, AttributeError, TypeError) as exc:
                raise ProfilingInputError("measurement capture_id must be UUID or null") from exc
            if capture in captures:
                raise ProfilingInputError("capture belongs to more than one measurement")
            captures.add(capture)
        mapping = item["mapping"] if isinstance(item["mapping"], dict) else _member_path(item["mapping"], root)
        paths = tuple(_member_path(path, root) for path in _array(item["records"], "measurement records"))
        if len(paths) != len(set(paths)):
            raise ProfilingInputError("duplicate measurement records path")
        manifest = item.get("manifest")
        if manifest is not None and not isinstance(manifest, dict):
            manifest = _member_path(manifest, root)
        if not paths and manifest is None:
            raise ProfilingInputError("diagnostic-only measurement requires manifest")
        for path in (mapping, manifest, *paths):
            if isinstance(path, Path):
                file_paths.add(path)
        if len(file_paths) > MAX_MEMBER_FILES:
            raise ProfilingInputError("capture set exceeds cumulative file budget")
        if sum(path.stat().st_size for path in file_paths) > MAX_SET_BYTES:
            raise ProfilingInputError("capture set exceeds cumulative byte budget")
        members.append(Measurement(measurement_id, iteration, attempt, capture,
                                   item["warmup"], mapping, paths, manifest))
    return CaptureSet(experiment_id, tuple(members), str(Path(value).resolve()) if not isinstance(value, dict) else "<object>")


def _distribution(values):
    numbers = [value for _, value in values]
    return {"count": len(numbers), "measurement_ids": [key for key, _ in values],
            "samples": numbers, "minimum": min(numbers) if numbers else None,
            "median": float(statistics.median(numbers)) if numbers else None,
            "maximum": max(numbers) if numbers else None,
            "mean": float(statistics.mean(numbers)) if numbers else None,
            "population_stdev": statistics.pstdev(numbers) if numbers else None}


def _signature(member):
    run = member.run
    metadata = run.manifest_metadata
    attributes = dict(metadata.get("attributes", {}), **run.attributes)
    if isinstance(attributes.get("sampling"), dict):
        attributes["sampling"] = {key: value for key, value in attributes["sampling"].items() if key != "iteration"}
    # Different shape/execution/device/timing/byte contracts never share repeats.
    return json.dumps({"mapping": mapping_fingerprint(member.measurement.mapping),
                       "environment": metadata.get("environment"),
                       "execution": metadata.get("execution"), "versions": metadata.get("versions"),
                       "attributes": attributes,
                       "participants": metadata.get("participants") if metadata else
                       [(lane.rank, lane.host, lane.device, lane.core, lane.attributes.get("sampled", True))
                        for launch in run.launches for lane in launch.lanes],
                       "metrics": [(lane.stream_id, [(metric.stage_id, metric.bytes,
                                                        metric.byte_semantics, metric.scope)
                                                       for metric in lane.stage_metrics])
                                   for launch in run.launches for lane in launch.lanes]},
                      sort_keys=True, separators=(",", ":"), allow_nan=False)


def analyze_capture_set(value, *, base_dir=None, limits=None):
    """Analyze each member with the existing engine, then summarize explicit repeats."""
    capture_set = load_capture_set(value, base_dir=base_dir)
    from .budgets import InputBudget
    budget = InputBudget(limits)
    if not isinstance(value, CaptureSet):
        size = (len(json.dumps(value, ensure_ascii=False).encode("utf-8")) if isinstance(value, dict)
                else Path(value).stat().st_size)
        budget.charge(files=0 if isinstance(value, dict) else 1, bytes=size,
                      memory_bytes=size * 4)
    members = []
    for measurement in capture_set.measurements:
        run = load_profile_run(measurement.mapping, measurement.records, manifest=measurement.manifest,
                               _budget=budget)
        if (run.capture_id or None) != measurement.capture_id:
            raise ProfilingInputError("capture-set member capture binding mismatch")
        if run.manifest_metadata and run.manifest_metadata.get("attempt_id") != measurement.attempt_id:
            raise ProfilingInputError("capture-set member attempt binding mismatch")
        members.append(MeasurementAnalysis(measurement, run, analyze_run(run), not measurement.warmup))
    included = [member for member in members if member.included]
    status = ("not_sampled" if not included else
              "incomplete" if any(m.run.status != "complete" or m.run.health_status == "incomplete" for m in included)
              else "complete")
    groups = {}
    for member in included:
        groups.setdefault(_signature(member), []).append(member)
    repeats = []
    for group in groups.values():
        valid = [member for member in group if member.run.status == "complete" and
                 member.run.health_status != "incomplete"]
        stages = {}
        for stage_id, stage in group[0].analysis.stages.items():
            stages[str(stage_id)] = {
                "name": stage.name,
                "critical_lane_union_us": _distribution([
                    (member.measurement.measurement_id, member.analysis.stages[stage_id].critical_lane_union_us)
                    for member in valid if member.analysis.stages[stage_id].critical_lane_union_us is not None]),
                "bandwidth_gbps": _distribution([
                    (member.measurement.measurement_id, member.analysis.stages[stage_id].bandwidth_gbps)
                    for member in valid if member.analysis.stages[stage_id].bandwidth_gbps is not None]),
            }
        repeats.append({"operator": group[0].run.operator,
                        "measurement_ids": [m.measurement.measurement_id for m in group],
                        "excluded_incomplete_ids": [m.measurement.measurement_id for m in group if m not in valid],
                        "scope": "manifest_verified" if all(m.run.manifest_metadata for m in group) else "provided_inputs_only",
                        "latency_us": _distribution([(m.measurement.measurement_id, m.analysis.critical_lane.duration_us)
                                                      for m in valid if m.analysis.critical_lane is not None]),
                        "stages": stages})
    summary = {"schema": CAPTURE_SET_ANALYSIS_SCHEMA, "experiment_id": capture_set.experiment_id,
               "status": status, "measurement_count": len(members),
               "warmup_excluded_ids": [m.measurement.measurement_id for m in members if not m.included],
               "measurements": [{"measurement_id": m.measurement.measurement_id,
                                  "iteration": m.measurement.iteration, "attempt_id": m.measurement.attempt_id,
                                  "capture_id": m.run.capture_id or None, "warmup": m.measurement.warmup,
                                  "capture_status": m.run.status, "health_status": m.run.health_status,
                                  "coverage": m.run.coverage, "rank_latency": m.analysis.rank_coverage}
                                 for m in members],
               "repeat_groups": repeats,
               "statistics": "Each sample is one capture's slowest usable lane. Warmup and incomplete measurements never enter complete repeat distributions. No timestamps are merged across captures or devices."}
    return CaptureSetAnalysis(capture_set, tuple(members), summary)


def generate_capture_set_reports(value, output_dir, *, base_dir=None, artifacts=None, limits=None):
    """Atomically publish the whole set; a corrupt member publishes nothing."""
    from .cli import _artifact_writers, _publish_new_directory, MAX_OUTPUT_BYTES
    result = analyze_capture_set(value, base_dir=base_dir, limits=limits)
    destination = Path(output_dir).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise ProfilingInputError("capture-set output directory already exists")
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}.staging.", dir=str(destination.parent)))
    published = False
    try:
        rows = []
        published_paths = []
        output_bytes = 0
        for index, member in enumerate(result.members):
            # Numeric output paths never interpolate untrusted measurement IDs.
            directory = staging / "measurements" / str(index)
            directory.mkdir(parents=True)
            writers = _artifact_writers(member.run, member.analysis, artifacts)
            for name, writer in writers.items():
                writer(directory / name)
                output_bytes += (directory / name).stat().st_size
                if output_bytes > MAX_OUTPUT_BYTES:
                    raise ProfilingInputError("capture-set output byte budget exceeded")
                published_paths.append(f"measurements/{index}/{name}")
            first = "report.html" if "report.html" in writers else next(iter(writers))
            rows.append((member.measurement.measurement_id, f"measurements/{index}/{first}", member.run.status,
                         "warmup excluded" if not member.included else "included"))
        atomic_write_json(staging / "capture_set.json", result.summary)
        text = json.dumps(result.summary, indent=2, sort_keys=True, ensure_ascii=False, allow_nan=False)
        atomic_write_text(staging / "analysis.md", "# Profiling capture set\n\n```json\n" + text.replace("`", "\\u0060") + "\n```\n")
        links = "".join(f'<li><a href="{path}">{html.escape(name)}</a>: {state}; {inclusion}</li>'
                        for name, path, state, inclusion in rows)
        atomic_write_text(staging / "report.html", '<!doctype html><html lang="en"><meta charset="utf-8">'
                          '<meta name="viewport" content="width=device-width"><title>Profiling capture set</title>'
                          '<h1>Profiling capture set</h1><p>Status: ' + result.summary["status"] + '</p><ul>' + links +
                          '</ul><details><summary>Repeat distributions and provenance</summary><pre>' +
                          html.escape(text) + '</pre></details></html>')
        published_paths += ["capture_set.json", "analysis.md", "report.html"]
        if artifacts is not None:
            atomic_write_json(staging / "artifacts.json", {"schema": "tilexr_profiling_artifacts.v1",
                                                          "artifacts": published_paths})
            published_paths.append("artifacts.json")
        # Include summary overhead as well as per-member artifacts in the limit.
        if sum(path.stat().st_size for path in staging.rglob("*") if path.is_file()) > MAX_OUTPUT_BYTES:
            raise ProfilingInputError("capture-set output byte budget exceeded")
        if not _publish_new_directory(staging, destination):
            raise ProfilingInputError("capture-set output directory already exists")
        published = True
    finally:
        if not published:
            shutil.rmtree(staging)
    return GeneratedCaptureSet(result.summary["status"], destination,
                               tuple(destination / name for name in published_paths),
                               result.summary)
