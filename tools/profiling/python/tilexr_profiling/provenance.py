"""Bounded caller metadata and the common provenance for every report."""

import json
import math
from collections.abc import Mapping
from typing import Any, Dict

from .model import ProfileRun, ProfilingInputError
from .version import __version__

MAX_ATTRIBUTE_BYTES = 65536
MAX_ATTRIBUTE_DEPTH = 8
MAX_ATTRIBUTE_NODES = 4096
_FACT_KEYS = frozenset({
    "capture_id", "run_id", "producer", "operator", "timing_unit", "cycles_per_us",
    "clock_domain", "timestamp_source", "display_origin", "completeness_scope",
    "sources", "event_ids", "status", "statistics", "protocol", "provenance",
    "expected_stream_count", "present_stream_count", "usable_stream_count",
    "missing_stream_count", "dropped_stream_count", "overflowed_stream_count",
    "excluded_stream_count", "dropped_record_count", "analysis_version",
})

STATISTICS = {
    "inclusive": "Sum of untrimmed paired interval durations; overlaps count repeatedly.",
    "union": "Per lane: interval union clipped to its unique critical interval; lane values sum, never merge across lanes.",
    "exclusive": "Stage union minus intersection with the union of its explicitly declared direct child stages; overlapping parent instances share stage coverage.",
    "critical_shares": "Critical lane coverage divided by its unique critical duration; unknown for missing, multiple or zero-duration critical intervals.",
    "wait_sync_pressure": "Union of all wait/sync intervals on the critical lane, clipped to critical; overlaps count once.",
    "instant": "Instant samples contribute zero interval length.",
    "sibling_overlap": "Sibling stages may overlap; exclusive shares need not sum to 100%.",
}


def validate_attributes(attributes: Mapping) -> Dict[str, Any]:
    """Copy finite JSON attributes; reject overriding protocol facts and excess size."""
    if not isinstance(attributes, Mapping):
        raise ProfilingInputError("report attributes must be an object")
    conflicts = _FACT_KEYS.intersection(attributes)
    if conflicts:
        raise ProfilingInputError("report attributes cannot override facts: " + ", ".join(sorted(conflicts)))
    nodes = 0

    def copy_value(value, depth):
        nonlocal nodes
        nodes += 1
        if nodes > MAX_ATTRIBUTE_NODES or depth > MAX_ATTRIBUTE_DEPTH:
            raise ProfilingInputError("report attributes exceed nesting or item limit")
        if value is None or isinstance(value, (bool, int, str)):
            if isinstance(value, str) and len(value) > MAX_ATTRIBUTE_BYTES:
                raise ProfilingInputError("report attribute string is too large")
            if isinstance(value, int) and value.bit_length() > 256:
                raise ProfilingInputError("report attribute integer is too large")
            return value
        if isinstance(value, float):
            if not math.isfinite(value):
                raise ProfilingInputError("report attributes must contain finite numbers")
            return value
        if isinstance(value, Mapping):
            result = {}
            for key, item in value.items():
                if not isinstance(key, str) or not key or len(key) > 256:
                    raise ProfilingInputError("report attribute keys must be nonempty strings of at most 256 characters")
                result[key] = copy_value(item, depth + 1)
            return result
        if isinstance(value, (list, tuple)):
            return [copy_value(item, depth + 1) for item in value]
        raise ProfilingInputError("unsupported report attribute type: " + type(value).__name__)

    result = copy_value(attributes, 0)
    try:
        encoded = json.dumps(result, allow_nan=False, ensure_ascii=True).encode("utf-8")
    except (ValueError, UnicodeError) as error:
        raise ProfilingInputError("report attributes cannot be encoded as JSON") from error
    if len(encoded) > MAX_ATTRIBUTE_BYTES:
        raise ProfilingInputError("report attributes exceed 65536 encoded bytes")
    return result


def build_provenance(run: ProfileRun) -> Dict[str, Any]:
    """Build the same authoritative, JSON-compatible metadata for all outputs."""
    attributes = validate_attributes(run.attributes)
    supplied = {key: attributes.get(key, "unknown") for key in (
        "sdk_version", "kernel_version", "soc", "cann_version", "driver_version",
        "backend", "native_status",
    )}
    return {
        "capture_id": run.capture_id,
        "run_id": run.run_id,
        "producer": run.producer,
        "operator": run.operator,
        "status": run.status,
        "analysis_version": __version__,
        "protocol": "tilexr_profiling_records.v1",
        "sources": list(run.sources),
        "event_ids": sorted({record.event_id for launch in run.launches for lane in launch.lanes for record in lane.records}),
        "identity_source": "records capture UUID and stream IDs; operator from mapping",
        "timing": {
            "unit": run.timing_unit,
            "cycles_per_us": run.cycles_per_us,
            "conversion_source": "mapping.cycles_per_us",
            "timestamp_source": run.timestamp_source,
            "clock_domain": run.clock_domain,
            "display_origin": run.display_origin,
            "calibration": "unknown; display alignment does not establish hardware clock synchronization",
            "device_identity": "unknown unless supplied by the caller; rank is not a hardware identifier",
            "marker_overhead_us": run.marker_overhead_us if run.marker_overhead_us is not None else "unknown",
        },
        "completeness": {
            "scope": run.completeness_scope,
            "boundary": "Internal completeness of provided inputs only; unprovided rank files and participants cannot be detected.",
            "expected_stream_count": run.expected_stream_count,
            "present_stream_count": run.present_stream_count,
            "usable_stream_count": run.usable_stream_count,
            "missing_stream_count": run.missing_stream_count,
            "dropped_stream_count": run.dropped_stream_count,
            "overflowed_stream_count": run.overflowed_stream_count,
            "excluded_stream_count": run.excluded_stream_count,
            "dropped_record_count": run.dropped_record_count,
        },
        "reported_environment": supplied,
        "environment_source": "caller attributes; not automatically identified by records.v1",
        "attributes": attributes,
        "statistics": dict(STATISTICS),
    }


def provenance_items(provenance: Dict[str, Any]):
    """Yield leaf values with stable dotted labels for text and HTML tables."""
    def walk(value, prefix):
        # Preserve arbitrary caller key structure (including dots and empty objects).
        if isinstance(value, dict) and prefix != "attributes" and not prefix.startswith("reported_environment."):
            for key, item in value.items():
                yield from walk(item, prefix + "." + key if prefix else key)
        else:
            yield prefix, value
    return walk(provenance, "")
