# Purpose: Validate capture provenance and attach declared coverage to binary records.
# 用途与关系：校验 capture 来源信息，将声明的覆盖范围与二进制记录关联。
"""Bounded, explicit metadata and coverage contracts for one capture attempt."""

import copy
import hashlib
import json
import math
from pathlib import Path
from typing import Mapping
from uuid import UUID

from .mapping import (_known_keys, _required_keys, _object, _array, _string,
                      _load_json, _reject_duplicate_keys, _reject_nonfinite_constant,
                      load_mapping)
from .model import ProfilingInputError

MANIFEST_SCHEMA = "tilexr_profiling_manifest.v1"
HOST_EVENTS_SCHEMA = "tilexr_profiling_host_events.v1"
SHARD_SCHEMA = "tilexr_profiling_manifest_shard.v1"
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_ITEMS = 65536
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1


def _bounded(value):
    nodes = 0

    def visit(item, depth):
        nonlocal nodes
        nodes += 1
        if depth > 16 or nodes > 262144:
            raise ProfilingInputError("manifest exceeds nesting or item budget")
        if item is None or isinstance(item, (str, bool)):
            if isinstance(item, str) and len(item) > MAX_MANIFEST_BYTES:
                raise ProfilingInputError("manifest string exceeds byte budget")
            return
        if isinstance(item, int):
            if item.bit_length() > 256:
                raise ProfilingInputError("manifest integer exceeds supported range")
            return
        if isinstance(item, float):
            if not math.isfinite(item):
                raise ProfilingInputError("manifest numbers must be finite")
            return
        if isinstance(item, dict):
            for key, child in item.items():
                _string(key, "manifest key")
                visit(child, depth + 1)
            return
        if isinstance(item, list):
            for child in item:
                visit(child, depth + 1)
            return
        raise ProfilingInputError("manifest must contain JSON values")

    visit(value, 0)
    try:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=False, allow_nan=False).encode("utf-8")
    except (ValueError, UnicodeError) as exc:
        raise ProfilingInputError("manifest cannot be encoded as JSON") from exc
    if len(encoded) > MAX_MANIFEST_BYTES:
        raise ProfilingInputError("manifest exceeds byte budget")
    return copy.deepcopy(value)


def _document(value):
    if isinstance(value, Mapping):
        return _object(_bounded(dict(value)), "manifest")
    path = Path(value)
    try:
        with path.open("rb") as handle:
            data = handle.read(MAX_MANIFEST_BYTES + 1)
        if len(data) > MAX_MANIFEST_BYTES:
            raise ProfilingInputError("manifest exceeds byte budget")
        raw = json.loads(data.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys,
                         parse_constant=_reject_nonfinite_constant)
    except (OSError, UnicodeError, ValueError, RecursionError) as exc:
        raise ProfilingInputError(f"cannot read manifest {path}: {exc}") from exc
    return _object(_bounded(raw), "manifest")


def mapping_fingerprint(mapping):
    """SHA256 of validated input JSON, UTF-8, sorted keys and compact separators."""
    raw = dict(mapping) if isinstance(mapping, Mapping) else _load_json(Path(mapping))
    load_mapping(raw)
    return hashlib.sha256(json.dumps(raw, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest()


def _uint(value, where, maximum=UINT32_MAX):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ProfilingInputError(f"{where} must be an unsigned integer <= {maximum}")
    return value


def _bool(value, where):
    if not isinstance(value, bool):
        raise ProfilingInputError(f"{where} must be a boolean")
    return value


def _identity(obj):
    _string(obj.get("attempt_id"), "attempt_id")
    if "capture_id" not in obj:
        raise ProfilingInputError("missing capture_id")
    if obj["capture_id"] is not None:
        try:
            if not isinstance(obj["capture_id"], str):
                raise ValueError("not a UUID string")
            obj["capture_id"] = str(UUID(obj["capture_id"]))
        except (ValueError, AttributeError) as exc:
            raise ProfilingInputError("capture_id must be a UUID or null") from exc


def _events(values):
    events = _array(values, "events")
    if len(events) > MAX_ITEMS:
        raise ProfilingInputError("too many capture events")
    by_id = {}
    required = {"event_id", "code", "severity", "phase", "scope", "count", "source", "resolved"}
    allowed = required | {"rank", "stream_id", "message", "status_code", "acl_error",
                          "operation_id", "retry", "timestamp", "clock_domain"}
    for event in events:
        _object(event, "event")
        _known_keys(event, allowed, "event")
        _required_keys(event, required, "event")
        for key in ("event_id", "code", "phase", "source"):
            _string(event[key], key)
        if _string(event["severity"], "event.severity") not in {"info", "warning", "error"}:
            raise ProfilingInputError("invalid event severity")
        if _string(event["scope"], "event.scope") not in {"attempt", "rank", "lane"}:
            raise ProfilingInputError("invalid event scope")
        _uint(event["count"], "event.count")
        _bool(event["resolved"], "event.resolved")
        if event["scope"] in {"rank", "lane"}:
            _uint(event.get("rank"), "event.rank", 65535)
        if event["scope"] == "lane":
            _uint(event.get("stream_id"), "event.stream_id")
        if "rank" in event:
            _uint(event["rank"], "event.rank", 65535)
        if "stream_id" in event:
            _uint(event["stream_id"], "event.stream_id")
            if "rank" in event and event["rank"] != event["stream_id"] >> 16:
                raise ProfilingInputError("event rank disagrees with stream ID")
        if "retry" in event:
            _uint(event["retry"], "event.retry")
        if "message" in event and not isinstance(event["message"], str):
            raise ProfilingInputError("event.message must be a string")
        for key in ("operation_id", "clock_domain"):
            if key in event:
                _string(event[key], key)
        for key in ("status_code", "acl_error"):
            if key in event:
                value = event[key]
                if isinstance(value, bool) or not isinstance(value, (str, int)):
                    raise ProfilingInputError(f"event.{key} must be a string or integer")
        if ("timestamp" in event) != ("clock_domain" in event):
            raise ProfilingInputError("event time requires timestamp and clock_domain")
        if "timestamp" in event:
            time = event["timestamp"]
            if isinstance(time, bool) or not isinstance(time, (int, float)) or time < 0:
                raise ProfilingInputError("event timestamp must be nonnegative and finite")
        old = by_id.get(event["event_id"])
        if old is not None and old != event:
            # A later resolved snapshot may acknowledge the same historical event.
            before, after = dict(old), dict(event)
            before.pop("resolved"); after.pop("resolved")
            if before != after:
                raise ProfilingInputError("conflicting duplicate event ID")
            event = dict(event, resolved=old["resolved"] or event["resolved"])
        by_id[event["event_id"]] = event
    return list(by_id.values())


def load_manifest(value, mapping_input):
    """Validate a manifest and its exact mapping binding; return an owned JSON copy."""
    obj = _document(value)
    required = {"schema", "attempt_id", "capture_id", "mapping_sha256", "participants"}
    allowed = required | {"stage_metrics", "events", "environment", "versions", "execution",
                          "dropped_events", "attributes", "missing_shard_ranks", "unobserved_event_ranks",
                          "completion_snapshots"}
    _known_keys(obj, allowed, "manifest")
    _required_keys(obj, required, "manifest")
    if obj["schema"] != MANIFEST_SCHEMA:
        raise ProfilingInputError("unsupported manifest schema")
    _identity(obj)
    if obj["mapping_sha256"] != mapping_fingerprint(mapping_input):
        raise ProfilingInputError("manifest mapping fingerprint mismatch")
    mapping = load_mapping(mapping_input)
    participants = _array(obj["participants"], "participants")
    if not participants or len(participants) > MAX_ITEMS:
        raise ProfilingInputError("participants must contain 1..65536 entries")
    by_id = {}
    for participant in participants:
        _object(participant, "participant")
        _known_keys(participant, {"stream_id", "rank", "logical_lane", "sampled", "host", "device",
                                  "acl_stream_label", "physical_core"}, "participant")
        _required_keys(participant, {"stream_id", "rank", "logical_lane", "sampled"}, "participant")
        sid = _uint(participant["stream_id"], "stream_id")
        rank = _uint(participant["rank"], "rank", 65535)
        lane = _uint(participant["logical_lane"], "logical_lane", 65535)
        if sid != (rank << 16) | lane:
            raise ProfilingInputError("participant rank/lane disagrees with stream ID")
        if sid in by_id:
            raise ProfilingInputError("duplicate participant stream ID")
        _bool(participant["sampled"], "sampled")
        for key in ("host", "acl_stream_label", "physical_core"):
            if key in participant:
                _string(participant[key], key)
        if "device" in participant:
            device = _object(participant["device"], "device")
            _known_keys(device, {"id", "namespace"}, "device")
            _required_keys(device, {"id", "namespace"}, "device")
            _string(device["namespace"], "device.namespace")
            if isinstance(device["id"], str):
                _string(device["id"], "device.id")
            else:
                _uint(device["id"], "device.id")
        by_id[sid] = participant
    metrics = _array(obj.get("stage_metrics", []), "stage_metrics")
    if len(metrics) > MAX_ITEMS:
        raise ProfilingInputError("too many stage metrics")
    metric_keys = set()
    for metric in metrics:
        _object(metric, "stage metric")
        _known_keys(metric, {"stream_id", "stage_id", "stage", "bytes", "byte_semantics", "scope",
                             "source", "status", "reason"}, "stage metric")
        _required_keys(metric, {"stream_id", "stage_id", "stage", "source"}, "stage metric")
        sid = _uint(metric["stream_id"], "metric.stream_id")
        stage_id = _uint(metric["stage_id"], "metric.stage_id")
        if sid not in by_id or stage_id >= len(mapping.stages) or mapping.stages[stage_id].name != metric["stage"]:
            raise ProfilingInputError("metric refers to unknown participant or mismatched stage ID/name")
        key = (sid, stage_id)
        if key in metric_keys:
            raise ProfilingInputError("duplicate stage metric")
        metric_keys.add(key)
        _string(metric["source"], "metric.source")
        status = metric.get("status", "known")
        if status == "not_applicable":
            _string(metric.get("reason"), "metric.reason")
            if any(key in metric for key in ("bytes", "byte_semantics", "scope")):
                raise ProfilingInputError("not_applicable metric must not supply byte values")
        elif status == "known":
            _uint(metric.get("bytes"), "metric.bytes", UINT64_MAX)
            _string(metric.get("byte_semantics"), "metric.byte_semantics")
            if _string(metric.get("scope"), "metric.scope") not in {"per_interval", "per_lane_total"}:
                raise ProfilingInputError("unsupported metric scope")
        else:
            raise ProfilingInputError("unsupported metric status")
    obj["stage_metrics"] = metrics
    if "events" in obj:
        obj["events"] = _events(obj["events"])
    ranks = {p["rank"] for p in participants}
    for event in obj.get("events", []):
        if "rank" in event and event["rank"] not in ranks:
            raise ProfilingInputError("event refers to unknown rank")
        if "stream_id" in event and event["stream_id"] not in by_id:
            raise ProfilingInputError("event refers to unknown participant")
    obj["dropped_events"] = _uint(obj.get("dropped_events", 0), "dropped_events")
    for key in ("environment", "versions", "attributes"):
        _object(obj.get(key, {}), key)
    for section, names in (("environment", ("soc", "cann_version", "driver_version")),
                           ("versions", ("sdk_version", "kernel_version"))):
        for name in names:
            if name in obj.get(section, {}):
                _string(obj[section][name], section + "." + name)
    execution = _object(obj.get("execution", {}), "execution")
    _known_keys(execution, {"kind", "stages", "framework_profiler", "stage_barrier"}, "execution")
    if "kind" in execution and _string(execution["kind"], "execution.kind") not in {"compute", "communication"}:
        raise ProfilingInputError("unsupported execution kind")
    for key in ("framework_profiler", "stage_barrier"):
        if key in execution:
            _bool(execution[key], key)
    execution_keys = set()
    for row in _array(execution.get("stages", []), "execution.stages"):
        _object(row, "execution stage")
        required_execution = {"rank", "stage_id", "stage", "backend", "native_status", "kernel_version"}
        _known_keys(row, required_execution, "execution stage")
        _required_keys(row, required_execution, "execution stage")
        rank = _uint(row["rank"], "execution.rank", 65535)
        stage_id = _uint(row["stage_id"], "execution.stage_id")
        if rank not in {p["rank"] for p in participants} or stage_id >= len(mapping.stages) or row["stage"] != mapping.stages[stage_id].name:
            raise ProfilingInputError("unknown execution rank or stage")
        if (rank, stage_id) in execution_keys:
            raise ProfilingInputError("duplicate execution stage")
        execution_keys.add((rank, stage_id))
        for key in ("backend", "native_status", "kernel_version"):
            _string(row[key], "execution." + key)
    for name in ("missing_shard_ranks", "unobserved_event_ranks"):
        if name not in obj:
            continue
        values = _array(obj[name], name)
        for rank in values:
            _uint(rank, "missing_shard_rank", 65535)
            if rank not in {p["rank"] for p in participants}:
                raise ProfilingInputError("unknown missing shard rank")
        if len(values) != len(set(values)):
            raise ProfilingInputError("duplicate missing shard rank")
    if "completion_snapshots" in obj:
        seen_completion = set()
        for item in _array(obj["completion_snapshots"], "completion snapshots"):
            snapshot = _completion(item, obj["capture_id"])
            for row in snapshot["streams"]:
                if row["stream_id"] not in by_id or row["stream_id"] in seen_completion:
                    raise ProfilingInputError("unknown or duplicate completion participant")
                seen_completion.add(row["stream_id"])
    return obj


def _completion(value, capture_id):
    snapshot = _document(value)
    required = {"schema", "capture_id", "generation", "streams"}
    _known_keys(snapshot, required, "completion snapshot")
    _required_keys(snapshot, required, "completion snapshot")
    if snapshot["schema"] != "tilexr_profiling_completion.v1":
        raise ProfilingInputError("unsupported completion snapshot schema")
    if snapshot["capture_id"] != capture_id or capture_id is None:
        raise ProfilingInputError("completion snapshot capture binding mismatch")
    _uint(snapshot["generation"], "generation", UINT64_MAX)
    streams = _array(snapshot["streams"], "completion streams")
    if len(streams) > MAX_ITEMS:
        raise ProfilingInputError("too many completion streams")
    seen = set()
    for row in streams:
        _object(row, "completion stream")
        _known_keys(row, {"stream_id", "completion"}, "completion stream")
        _required_keys(row, {"stream_id", "completion"}, "completion stream")
        sid = _uint(row["stream_id"], "completion stream ID")
        _uint(row["completion"], "completion", 2)
        if sid in seen:
            raise ProfilingInputError("duplicate completion stream")
        seen.add(sid)
    return snapshot


def attach_completion(manifest, snapshot):
    """Attach raw EMPTY/WRITING/COMMITTED facts without inferring execution state."""
    result = _document(manifest)
    completion = _completion(snapshot, result.get("capture_id"))
    previous = result.setdefault("completion_snapshots", [])
    if completion not in previous:
        old_ids = {row["stream_id"] for item in previous for row in item["streams"]}
        if any(row["stream_id"] in old_ids for row in completion["streams"]):
            raise ProfilingInputError("conflicting completion snapshot")
        previous.append(completion)
    return _bounded(result)


def attach_host_events(manifest, snapshot):
    """Attach a bounded SDK event snapshot without changing capture identity."""
    result, events = _document(manifest), _document(snapshot)
    _known_keys(events, {"schema", "attempt_id", "capture_id", "events", "dropped_events"}, "host events")
    _required_keys(events, {"schema", "attempt_id", "capture_id", "events", "dropped_events"}, "host events")
    if events["schema"] != HOST_EVENTS_SCHEMA:
        raise ProfilingInputError("unsupported host events schema")
    _identity(result); _identity(events)
    if any(result[key] != events[key] for key in ("attempt_id", "capture_id")):
        raise ProfilingInputError("host events attempt/capture binding mismatch")
    result["events"] = _events(result.get("events", []) + events["events"])
    # Snapshots are cumulative for one attempt, so repeated imports use max.
    result["dropped_events"] = max(_uint(result.get("dropped_events", 0), "dropped_events"),
                                   _uint(events["dropped_events"], "dropped_events"))
    return _bounded(result)


def merge_manifest_shards(expected, shards, mapping):
    """Merge explicit rank-owned shards while preserving the independent expected set."""
    result = load_manifest(expected, mapping)
    if not isinstance(shards, (list, tuple)) or len(shards) > MAX_ITEMS:
        raise ProfilingInputError("shards must be an explicit bounded list")
    participants = {p["stream_id"]: p for p in result["participants"]}
    seen_ranks = set()
    observed_event_ranks = set()
    for value in shards:
        shard = _document(value)
        if shard.get("schema") != SHARD_SCHEMA:
            raise ProfilingInputError("unsupported manifest shard schema")
        rank = _uint(shard.pop("rank", None), "shard.rank", 65535)
        if rank in seen_ranks or rank not in {p["rank"] for p in participants.values()}:
            raise ProfilingInputError("duplicate or unexpected shard rank")
        seen_ranks.add(rank)
        shard["schema"] = MANIFEST_SCHEMA
        shard = load_manifest(shard, mapping)
        if any(shard[key] != result[key] for key in ("attempt_id", "capture_id", "mapping_sha256")):
            raise ProfilingInputError("shard attempt/capture/mapping binding mismatch")
        for participant in shard["participants"]:
            sid = participant["stream_id"]
            if participant["rank"] != rank or sid not in participants:
                raise ProfilingInputError("shard participant violates rank ownership")
            target = participants[sid]
            if any(key in target and target[key] != value for key, value in participant.items()):
                raise ProfilingInputError("shard participant conflicts with expected identity")
            target.update(participant)
        for event in shard.get("events", []):
            if event.get("rank") != rank:
                raise ProfilingInputError("shard event must identify its owner rank")
        for row in shard.get("execution", {}).get("stages", []):
            if row["rank"] != rank:
                raise ProfilingInputError("shard execution violates rank ownership")
        for snapshot in shard.get("completion_snapshots", []):
            if any(row["stream_id"] >> 16 != rank for row in snapshot["streams"]):
                raise ProfilingInputError("completion snapshot violates rank ownership")
            result = attach_completion(result, snapshot)
            participants = {p["stream_id"]: p for p in result["participants"]}
        result["stage_metrics"].extend(shard["stage_metrics"])
        if "events" in shard:
            result.setdefault("events", []).extend(shard["events"])
            observed_event_ranks.add(rank)
        if len(result["stage_metrics"]) > MAX_ITEMS or len(result.get("events", [])) > MAX_ITEMS:
            raise ProfilingInputError("merged metrics/events exceed item budget")
        result["dropped_events"] = min(UINT32_MAX, result["dropped_events"] + shard["dropped_events"])
        for key in ("environment", "versions", "attributes"):
            target = result.setdefault(key, {})
            for name, value in shard.get(key, {}).items():
                if name in target and target[name] != value:
                    raise ProfilingInputError(f"conflicting shard {key}.{name}")
                target[name] = value
        target = result.setdefault("execution", {})
        for name, value in shard.get("execution", {}).items():
            if name == "stages":
                target.setdefault(name, []).extend(value)
            elif name in target and target[name] != value:
                raise ProfilingInputError("conflicting shard execution metadata")
            else:
                target[name] = value
    sampled_ranks = {p["rank"] for p in participants.values() if p["sampled"]}
    result["missing_shard_ranks"] = sorted(sampled_ranks - seen_ranks)
    result["unobserved_event_ranks"] = sorted(sampled_ranks - observed_event_ranks)
    return load_manifest(result, mapping)
