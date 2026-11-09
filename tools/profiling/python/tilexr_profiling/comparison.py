# Purpose: Compare qualified machine reports against explicit regression thresholds.
# 用途与关系：按显式回归阈值比较满足可比条件的机器报表。
"""Conservative comparison of independently captured, versioned analyses."""

import json
import math
from dataclasses import asdict, dataclass
from pathlib import Path
from collections.abc import Mapping

from .mapping import _reject_duplicate_keys, _reject_nonfinite_constant
from .model import MAPPING_SCHEMA_V2, ProfilingInputError
from .serialization import ANALYSIS_SCHEMA, STATISTICS_VERSION

COMPARISON_SCHEMA = "tilexr_profiling_comparison.v1"
MAX_ANALYSIS_BYTES = 32 * 1024 * 1024
_STAGE_METRICS = frozenset({"total_us", "median_us", "minimum_us", "maximum_us",
                          "union_us", "exclusive_us", "critical_lane_us",
                          "critical_lane_union_us", "critical_lane_exclusive_us",
                          "bandwidth_gbps"})


@dataclass(frozen=True)
class Threshold:
    """Regression must exceed both configured absolute and relative tolerance.

    Absolute tolerance uses the metric unit; relative tolerance is a fraction.
    Lower latency and higher bandwidth are typical explicit directions.
    """

    metric: str = "latency.median_us"
    direction: str = "lower"
    absolute: float = 0.0
    relative: float = 0.05
    stage_id: int = None


def _bounded_document(value):
    if isinstance(value, Mapping):
        raw = dict(value)
    else:
        try:
            with Path(value).open("rb") as handle:
                encoded = handle.read(MAX_ANALYSIS_BYTES + 1)
            if len(encoded) > MAX_ANALYSIS_BYTES:
                raise ProfilingInputError("analysis exceeds comparison byte budget")
            raw = json.loads(encoded.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys,
                             parse_constant=_reject_nonfinite_constant)
        except (OSError, UnicodeError, ValueError, RecursionError) as exc:
            raise ProfilingInputError(f"cannot read comparison analysis: {exc}") from exc
    nodes = 0

    def visit(item, depth):
        nonlocal nodes
        nodes += 1
        if depth > 32 or nodes > 1000000:
            raise ProfilingInputError("analysis exceeds comparison nesting or item budget")
        if isinstance(item, dict):
            if any(not isinstance(key, (str, int)) or isinstance(key, bool) for key in item):
                raise ProfilingInputError("analysis object keys must be strings or stage IDs")
            for child in item.values():
                visit(child, depth + 1)
        elif isinstance(item, (list, tuple)):
            for child in item:
                visit(child, depth + 1)
        elif isinstance(item, float) and not math.isfinite(item):
            raise ProfilingInputError("analysis numbers must be finite")
        elif isinstance(item, int) and item.bit_length() > 256:
            raise ProfilingInputError("analysis integer exceeds supported range")
        elif item is not None and not isinstance(item, (str, bool, int, float)):
            raise ProfilingInputError("analysis must contain JSON values")
    visit(raw, 0)
    try:
        encoded = json.dumps(raw, allow_nan=False, ensure_ascii=True)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ProfilingInputError("analysis cannot be encoded as JSON") from exc
    if len(encoded.encode("utf-8")) > MAX_ANALYSIS_BYTES:
        raise ProfilingInputError("analysis exceeds comparison byte budget")
    if not isinstance(raw, dict) or raw.get("schema") != ANALYSIS_SCHEMA:
        raise ProfilingInputError("comparison requires " + ANALYSIS_SCHEMA)
    if raw.get("statistics_version") != STATISTICS_VERSION:
        raise ProfilingInputError("unsupported comparison statistics version")
    for key in ("analysis", "provenance", "health", "comparison_context", "units"):
        if not isinstance(raw.get(key), dict):
            raise ProfilingInputError(f"analysis.{key} must be an object")
    analysis = raw["analysis"]
    if not isinstance(analysis.get("latency"), dict) or not isinstance(analysis.get("stages"), dict):
        raise ProfilingInputError("analysis latency and stages must be objects")
    for key, stage in analysis["stages"].items():
        if (not str(key).isdigit() or not isinstance(stage, dict) or not isinstance(stage.get("states", {}), dict)
                or any(not isinstance(value, dict) for value in stage.get("states", {}).values())):
            raise ProfilingInputError("analysis stage entries must have numeric IDs and object values/states")
    if not isinstance(raw["provenance"].get("coverage", {}), dict):
        raise ProfilingInputError("analysis provenance coverage must be an object")
    return raw


def _missing_context(context):
    missing = []
    for name in ("operator", "soc", "cann_version", "driver_version", "shape", "execution", "sampling"):
        if context.get(name) is None or context.get(name) in ("", "unknown", "not provided"):
            missing.append(name)
    devices = context.get("devices")
    if not isinstance(devices, list) or not devices:
        missing.append("devices")
    else:
        for device in devices:
            if not isinstance(device, dict) or any(device.get(key) is None
                                                   for key in ("host", "device", "namespace", "rank")):
                missing.append("devices.identity")
                break
    clock = context.get("clock", {})
    if not isinstance(clock, dict) or any(clock.get(key) is None for key in ("source", "cycles_per_us", "basis")):
        missing.append("clock")
    execution = context.get("execution")
    if isinstance(execution, dict):
        if execution.get("kind") == "communication":
            if not execution.get("stages"):
                missing.append("execution.stages")
            for name in ("framework_profiler", "stage_barrier"):
                if name not in execution:
                    missing.append("execution." + name)
        elif execution.get("kind") != "compute":
            missing.append("execution.kind")
    return sorted(set(missing))


def _threshold(value):
    if isinstance(value, Mapping):
        try:
            value = Threshold(**value)
        except TypeError as exc:
            raise ProfilingInputError("invalid threshold fields") from exc
    if not isinstance(value, Threshold):
        raise ProfilingInputError("threshold must be Threshold or an object")
    if value.direction not in ("lower", "higher"):
        raise ProfilingInputError("threshold direction must be lower or higher")
    for number in (value.absolute, value.relative):
        if isinstance(number, bool) or not isinstance(number, (int, float)) or not math.isfinite(number) or number < 0:
            raise ProfilingInputError("threshold tolerances must be finite nonnegative numbers")
    if value.stage_id is None:
        if value.metric not in {"latency.minimum_us", "latency.median_us", "latency.maximum_us"}:
            raise ProfilingInputError("unsupported capture threshold metric")
    elif isinstance(value.stage_id, bool) or not isinstance(value.stage_id, int) or value.stage_id < 0 or value.metric not in _STAGE_METRICS:
        raise ProfilingInputError("stage threshold requires a nonnegative stage_id and supported metric")
    return value


def _metric(document, rule):
    analysis = document["analysis"]
    if rule.stage_id is None:
        return analysis.get("latency", {}).get(rule.metric.split(".")[1]), None
    stages = analysis.get("stages", {})
    stage = stages.get(str(rule.stage_id), stages.get(rule.stage_id, {}))
    state = stage.get("states", {}).get(rule.metric, {})
    return stage.get(rule.metric), state


def compare_reports(baseline, candidate, thresholds=None):
    """Return pass/regression/incomparable/missing_data, never rank incomplete data.

    Both inputs are analysis documents or paths to analysis.json. Unknown or
    mismatched measurement context blocks performance classification.
    """
    left, right = _bounded_document(baseline), _bounded_document(candidate)
    rules = tuple(_threshold(item) for item in ([Threshold()] if thresholds is None else thresholds))
    if not rules or len(rules) > 4096:
        raise ProfilingInputError("comparison needs between 1 and 4096 thresholds")
    result = {"schema": COMPARISON_SCHEMA, "status": "pass", "reasons": [], "checks": [],
              "baseline_capture_id": left["provenance"].get("capture_id"),
              "candidate_capture_id": right["provenance"].get("capture_id"),
              "threshold_rule": "degradation must exceed both absolute and relative tolerance"}
    missing = []
    for label, document in (("baseline", left), ("candidate", right)):
        health = document["health"]
        if (health.get("capture_status") != "complete" or health.get("health_status") != "complete"
                or document["analysis"].get("status") != "complete"
                or document["provenance"].get("status") != "complete"):
            missing.append(label + ": capture or health observations are incomplete/unverified")
        if not document["provenance"].get("coverage", {}).get("verified"):
            missing.append(label + ": expected participant coverage is unverified")
        missing.extend(label + ": missing context " + name
                       for name in _missing_context(document["comparison_context"]))
        if document["provenance"].get("mapping_schema") == MAPPING_SCHEMA_V2:
            contract = document["comparison_context"].get("pairing_contract")
            fingerprint = document["provenance"].get("pairing_sha256")
            if (not isinstance(fingerprint, str) or len(fingerprint) != 64 or
                    any(char not in "0123456789abcdef" for char in fingerprint) or
                    contract != {"schema": MAPPING_SCHEMA_V2, "sha256": fingerprint}):
                missing.append(label + ": missing or contradictory pairing contract")
    if missing:
        result.update(status="missing_data", reasons=missing)
        return result
    differences = [name for name in sorted(set(left["comparison_context"]) | set(right["comparison_context"]))
                   if left["comparison_context"].get(name) != right["comparison_context"].get(name)]
    if left["units"] != right["units"]:
        differences.append("units")
    def stage_contract(document):
        return sorted((int(key), value.get("name"), value.get("category"), value.get("parent_stage_id"))
                      for key, value in document["analysis"].get("stages", {}).items())
    if stage_contract(left) != stage_contract(right):
        differences.append("stage definitions")
    if differences:
        result.update(status="incomparable", reasons=["measurement context differs: " + name for name in differences])
        return result
    for rule in rules:
        old, old_state = _metric(left, rule)
        new, new_state = _metric(right, rule)
        check = {"threshold": asdict(rule), "baseline": old, "candidate": new}
        valid = all(isinstance(number, (int, float)) and not isinstance(number, bool)
                    and math.isfinite(number) and number >= 0 for number in (old, new))
        if not valid or (rule.stage_id is not None and
                         any(not state or state.get("state") != "known" for state in (old_state, new_state))):
            check.update(status="missing_data", reason="metric is unavailable or its coverage is partial")
        else:
            change = new - old
            degradation = change if rule.direction == "lower" else -change
            tolerance = max(rule.absolute, abs(old) * rule.relative)
            if not all(math.isfinite(number) for number in (change, degradation, tolerance)):
                raise ProfilingInputError("comparison arithmetic exceeds finite range")
            relative_change = change / old if old else None
            if relative_change is not None and not math.isfinite(relative_change):
                raise ProfilingInputError("comparison relative change exceeds finite range")
            check.update(status="regression" if degradation > tolerance else "pass",
                         change=change, relative_change=relative_change,
                         degradation=degradation, allowed_degradation=tolerance)
        result["checks"].append(check)
    statuses = {check["status"] for check in result["checks"]}
    result["status"] = "missing_data" if "missing_data" in statuses else "regression" if "regression" in statuses else "pass"
    return result
