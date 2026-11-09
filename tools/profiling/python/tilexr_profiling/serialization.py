# Purpose: Serialize the shared analysis to lossless JSON and spreadsheet-safe CSV.
# 用途与关系：将共享分析序列化为无损 JSON 与避免公式注入的 CSV。
"""Versioned machine views of the same analysis used by human reports."""

import csv
import io
import json
from dataclasses import fields

from .analysis import StageSummary
from .io import json_chunks
from .provenance import report_payload
from .model import MAPPING_SCHEMA_V2

ANALYSIS_SCHEMA = "tilexr_profiling_analysis.v1"
STATISTICS_VERSION = "1"


def _comparison_context(run):
    manifest = run.manifest_metadata
    environment = manifest.get("environment", {})
    execution = manifest.get("execution", {})
    attributes = dict(manifest.get("attributes", {}), **run.attributes)
    lanes = [lane for launch in run.launches for lane in launch.lanes
             if lane.attributes.get("sampled", True)]
    sampling = {"scope": run.completeness_scope,
                "streams": sorted(lane.stream_id for lane in lanes)}
    policy = attributes.get("sampling", {}).get("policy") if isinstance(attributes.get("sampling"), dict) else None
    if policy is not None or "sampling_policy" in attributes:
        sampling["policy"] = policy if policy is not None else attributes["sampling_policy"]
    context = {
        "operator": run.operator,
        "soc": environment.get("soc", attributes.get("soc")),
        "cann_version": environment.get("cann_version", attributes.get("cann_version")),
        "driver_version": environment.get("driver_version", attributes.get("driver_version")),
        "devices": [{"rank": lane.rank, "host": lane.host, "device": lane.device,
                     "namespace": lane.attributes.get("device_namespace")}
                    for lane in sorted(lanes, key=lambda lane: lane.stream_id)],
        "clock": {"source": run.timestamp_source, "cycles_per_us": run.cycles_per_us,
                  "basis": environment.get("clock_basis", attributes.get("clock_basis"))},
        "shape": attributes.get("shape", environment.get("shape")),
        "execution": execution or None,
        "sampling": sampling,
        "byte_semantics": sorted({metric.byte_semantics for lane in lanes
                                  for metric in lane.stage_metrics if metric.bytes is not None}),
        "stage_workload": [{"stream_id": lane.stream_id, "stage_id": metric.stage_id,
                            "bytes": metric.bytes, "byte_semantics": metric.byte_semantics,
                            "scope": metric.scope, "applicability": metric.attributes.get("status", "known")}
                           for lane in sorted(lanes, key=lambda lane: lane.stream_id)
                           for metric in sorted(lane.stage_metrics, key=lambda metric: metric.stage_id)],
        "statistics_version": STATISTICS_VERSION,
    }
    # Historical v1 comparisons retain their existing context contract.
    if run.mapping_schema == MAPPING_SCHEMA_V2:
        context["pairing_contract"] = {"schema": run.mapping_schema, "sha256": run.pairing_sha256}
    return context


def build_analysis_document(run, analysis):
    """Return finite, lossless JSON data; unavailable numbers remain null."""
    document = {
        "schema": ANALYSIS_SCHEMA,
        "statistics_version": STATISTICS_VERSION,
        "units": {"time": "us", "bytes": "byte", "bandwidth": "GB/s (10^9 byte/s)",
                  "share": "fraction", "timestamps": "raw device cycles"},
        "comparison_context": _comparison_context(run),
        **report_payload(run, analysis),
    }
    # Validate public callers' manually constructed models as well as parser input.
    for _ in json_chunks(document):
        pass
    return document


def render_analysis_json(run, analysis):
    return json.dumps(build_analysis_document(run, analysis), ensure_ascii=True,
                      allow_nan=False, sort_keys=True, indent=2) + "\n"


def _csv_value(value):
    if value is None:
        return ""
    if isinstance(value, (dict, list, tuple)):
        return json.dumps(value, ensure_ascii=True, allow_nan=False, sort_keys=True)
    # CSV may be opened in a spreadsheet. Caller text must not become a formula.
    if isinstance(value, str) and value.lstrip().startswith(("=", "+", "-", "@")):
        return "'" + value
    return value


def iter_analysis_csv(run, analysis):
    """Stage view with original values, states/reasons and coverage provenance."""
    document = build_analysis_document(run, analysis)
    stage_fields = [field.name for field in fields(StageSummary)]
    metric_fields = [name for name in stage_fields
                     if name.endswith("_us") or name in {
                         "sample_count", "instance_count", "bytes", "byte_semantics", "bandwidth_gbps", "share_of_critical",
                         "union_share_of_critical", "exclusive_share_of_critical"}]
    state_fields = [name + suffix for name in metric_fields for suffix in ("_state", "_reason")]
    stream = io.StringIO(newline="")
    writer = csv.writer(stream, lineterminator="\n")
    common_fields = ["record_type", "schema", "statistics_version", "capture_id", "capture_status",
                     "health_status", "coverage", "units", "provenance"]
    def row(values):
        stream.seek(0)
        stream.truncate()
        writer.writerow(values)
        return stream.getvalue()

    yield row([*common_fields, *stage_fields, *state_fields])
    common_values = [ANALYSIS_SCHEMA, STATISTICS_VERSION, run.capture_id, run.status,
                     run.health_status, run.coverage]
    metadata = ["metadata", *common_values, document["units"], document["provenance"]]
    yield row([_csv_value(value) for value in metadata] + [""] * (len(stage_fields) + len(state_fields)))
    prefix = ["stage", *common_values, "", ""]
    for stage in analysis.stages.values():
        values = [getattr(stage, name) for name in stage_fields]
        states = [stage.states.get(name, {}).get(key, "")
                  for name in metric_fields for key in ("state", "reason")]
        yield row([_csv_value(value) for value in (*prefix, *values, *states)])


def render_analysis_csv(run, analysis):
    return "".join(iter_analysis_csv(run, analysis))
