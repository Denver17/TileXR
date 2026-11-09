# Purpose: Build capture models by pairing binary records with declared stage boundaries.
# 用途与关系：按声明的阶段边界配对二进制记录，生成 capture 模型。
"""Pair numeric event records and build the normalized profiling model."""

import math
from dataclasses import replace
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .mapping import MappingInput, load_mapping, pairing_fingerprint
from .model import (
    Diagnostic,
    Lane,
    Launch,
    ProfileRun,
    ProfilingMapping,
    ProfilingInputError,
    RawRecord,
    RecordStream,
    MAPPING_SCHEMA_V2,
    StageMetric,
    StageSample,
    StreamDefinition,
)
from .records import visit_record_files


def _to_us(value: float, cycles_per_us: float) -> float:
    result = value / cycles_per_us
    if not math.isfinite(result):
        raise ProfilingInputError("timestamp conversion must produce finite microseconds")
    return result


def _diagnostic(
    code: str,
    message: str,
    source: Optional[str],
    **details: Any,
) -> Diagnostic:
    return Diagnostic("incomplete", code, message, source, details)


def _compile_boundaries(mapping, budget):
    """Index subscriptions once per mapping, shared by all logical streams."""
    budget.charge(memory_bytes=len(mapping.events) * 512)
    events, selectors, precise = {}, {}, []
    for event in mapping.events:
        events.setdefault(event.id, {}).setdefault((event.occurrence, event.edge), []).append(event)
        selectors.setdefault((event.stage_id, event.instance_id), {})[event.edge] = event.selector()
        if event.occurrence is not None:
            precise.append(event)
    return events, selectors, precise


def _pair_stream(
    stream: RecordStream,
    definition: StreamDefinition,
    mapping: ProfilingMapping,
    budget,
    boundaries,
) -> Tuple[Lane, Tuple[Diagnostic, ...], bool]:
    events, selectors, precise = boundaries
    open_intervals: Dict[Tuple[int, Optional[int]], RawRecord] = {}
    invalid_intervals = set()
    samples = []
    diagnostics: List[Diagnostic] = []
    counts = {}
    trusted_occurrences = stream.complete and not stream.dropped_count and not stream.overflow

    def report(code, message, source=None, **details):
        budget.charge(memory_bytes=1024)
        diagnostics.append(_diagnostic(code, message, source or stream.source,
                                       stream_id=stream.id, **details))

    def selector_diagnostic(code, event, message):
        report(code, message, stage_id=event.stage_id, instance_id=event.instance_id,
               event_id=event.id, occurrence=event.occurrence,
               observed_occurrences=counts.get(event.id, 0))

    if not trusted_occurrences:
        for event in precise:
            invalid_intervals.add((event.stage_id, event.instance_id))
            selector_diagnostic("selector_unverifiable", event,
                                 "event occurrence cannot be verified in an incomplete or lossy stream")

    def emit_sample(event, begin, end=None):
        stage = mapping.stages[event.stage_id]
        key = (stage.id, event.instance_id)
        finish = begin if end is None else end
        duration = _to_us(finish.timestamp - begin.timestamp, mapping.cycles_per_us)
        budget.charge(samples=1, memory_bytes=1024)
        attributes = (dict(stage.attributes, pairing=selectors[key])
                      if mapping.schema == MAPPING_SCHEMA_V2 else stage.attributes)
        samples.append(StageSample(
            name=stage.name, stage_id=stage.id, parent_stage_id=stage.parent_stage_id,
            instance_id=event.instance_id, category=stage.category, kind=stage.kind,
            raw_start=begin.timestamp, raw_end=finish.timestamp, start_us=0.0, end_us=duration,
            duration_us=duration, critical=stage.critical if end is not None else False,
            begin_event_id=begin.event_id, end_event_id=end.event_id if end is not None else None,
            attributes=attributes, source=begin.source, source_index=begin.source_index,
            end_source_index=end.source_index if end is not None else None))

    def consume(event, record):
        key = (event.stage_id, event.instance_id)
        if key in invalid_intervals:
            return
        if event.edge == "instant":
            emit_sample(event, record)
            return
        details = dict(stage_id=event.stage_id, instance_id=event.instance_id,
                       event_id=record.event_id, record_index=record.source_index,
                       raw_timestamp=record.timestamp)
        if event.edge == "begin":
            if key in open_intervals:
                report("overlapping_begin", f"stage {event.stage_id} began again before its previous interval ended",
                       **details)
                open_intervals.pop(key)
                invalid_intervals.add(key)
            else:
                open_intervals[key] = record
            return
        begin = open_intervals.pop(key, None)
        if begin is None:
            report("unmatched_end", f"stage {event.stage_id} ended without a matching begin event", **details)
        else:
            emit_sample(event, begin, record)

    for record in stream.records:
        subscriptions = events.get(record.event_id)
        if subscriptions is None:
            raise ProfilingInputError(
                f"stream {stream.id} contains undeclared event ID {record.event_id} in {stream.source}")
        counts[record.event_id] = counts.get(record.event_id, 0) + 1
        occurrence = counts[record.event_id]
        # End subscriptions run before begin subscriptions, independent of stage order.
        groups = [subscriptions.get((selector, edge), ())
                  for edge in ("end", "begin", "instant") for selector in (None, occurrence)]
        budget.charge(pairing_actions=sum(len(group) for group in groups))
        for group in groups:
            for event in group:
                consume(event, record)

    if trusted_occurrences:
        for event in precise:
            if counts.get(event.id, 0) < event.occurrence:
                selector_diagnostic("selector_not_observed", event,
                                     "the explicitly selected event occurrence was not observed")

    for (stage_id, instance_id), begin in sorted(
        open_intervals.items(), key=lambda item: (item[0][0], item[0][1] if item[0][1] is not None else -1)
    ):
        report("unmatched_begin", f"stage {stage_id} began without a matching end event",
               stage_id=stage_id, instance_id=instance_id, event_id=begin.event_id,
               record_index=begin.source_index, raw_timestamp=begin.timestamp)

    samples.sort(
        key=lambda sample: (
            sample.raw_start,
            sample.stage_id,
            sample.instance_id if sample.instance_id is not None else -1,
            sample.source_index or 0,
        )
    )
    samples_by_stage: Dict[int, List[StageSample]] = {}
    for sample in samples:
        samples_by_stage.setdefault(sample.stage_id, []).append(sample)
    for sample in samples:
        if sample.parent_stage_id is None:
            continue
        if not any(
            parent.raw_start <= sample.raw_start and parent.raw_end >= sample.raw_end
            for parent in samples_by_stage.get(sample.parent_stage_id, [])
        ):
            report(
                "child_outside_parent",
                f"stage {sample.stage_id} sample is not contained by parent stage "
                f"{sample.parent_stage_id}",
                sample.source,
                stage_id=sample.stage_id,
                parent_stage_id=sample.parent_stage_id,
                instance_id=sample.instance_id,
                record_index=sample.source_index,
                raw_timestamp=sample.raw_start,
            )
    critical_count = sum(1 for sample in samples if sample.critical)
    usable = critical_count == 1
    if not usable:
        report(
            "invalid_critical_interval_count",
            f"stream {stream.id} has {critical_count} complete critical intervals; expected exactly one",
            stream.source,
            critical_interval_count=critical_count,
        )

    lane = Lane(
        stream_id=stream.id,
        rank=definition.rank,
        host=definition.host,
        device=definition.device,
        core=definition.core,
        samples=tuple(samples),
        records=stream.records,
        stage_metrics=definition.stage_metrics,
        attributes=definition.attributes,
    )
    return lane, tuple(diagnostics), usable


def _normalize_timeline(
    launches: Tuple[Launch, ...], mapping: ProfilingMapping
) -> Tuple[Launch, ...]:
    origins: Dict[Tuple[int, int, str], int] = {}
    for launch in launches:
        for lane in launch.lanes:
            if not lane.records:
                continue
            clock_key = (launch.id, lane.rank, lane.device or "unknown")
            minimum = min(record.timestamp for record in lane.records)
            origins[clock_key] = min(origins.get(clock_key, minimum), minimum)

    normalized_launches = []
    for launch in launches:
        normalized_lanes = []
        for lane in launch.lanes:
            clock_key = (launch.id, lane.rank, lane.device or "unknown")
            origin = origins.get(clock_key, 0)
            normalized_samples = tuple(
                replace(
                    sample,
                    start_us=_to_us(sample.raw_start - origin, mapping.cycles_per_us),
                    end_us=_to_us(sample.raw_end - origin, mapping.cycles_per_us),
                )
                for sample in lane.samples
            )
            normalized_lanes.append(replace(lane, samples=normalized_samples))
        normalized_launches.append(replace(launch, lanes=tuple(normalized_lanes)))
    return tuple(normalized_launches)


def load_profile_run(
    mapping_input: MappingInput,
    record_paths: Sequence[Path],
    *, manifest=None, limits=None, _budget=None,
) -> ProfileRun:
    """Validate a marker mapping and record files, then normalize one run."""
    from .budgets import InputBudget
    budget = _budget or InputBudget(limits)
    budget.document(mapping_input)
    mapping = load_mapping(mapping_input)
    boundaries = _compile_boundaries(mapping, budget)
    from .manifest import load_manifest
    if manifest is not None:
        budget.document(manifest)
    manifest_data = load_manifest(manifest, mapping_input) if manifest is not None else None
    if manifest_data is not None:
        budget.manifest(manifest_data)
    streams_by_id: Dict[int, RecordStream] = {}
    paired = {}
    observed_capture = []

    def consume_stream(capture, stream):
        if observed_capture and observed_capture[0] != capture:
            raise ProfilingInputError("capture ID mismatch across record files")
        if not observed_capture:
            observed_capture.append(capture)
        if manifest_data is not None and manifest_data["capture_id"] != str(capture):
            raise ProfilingInputError("manifest capture ID mismatch")
        if stream.id in streams_by_id:
            raise ProfilingInputError(f"duplicate stream ID {stream.id} across record files")
        definition = StreamDefinition(stream.id, 0, stream.id >> 16, None, None, stream.id & 65535,
            {"complete": stream.complete, "dropped_count": stream.dropped_count,
             "overflow": stream.overflow, "present": True, "sampled": True}, ())
        paired[stream.id] = _pair_stream(stream, definition, mapping, budget, boundaries)
        streams_by_id[stream.id] = stream

    record_files = visit_record_files(record_paths, consume_stream, budget=budget) if record_paths or manifest_data is None else ()
    capture_id = record_files[0].capture_id if record_files else manifest_data["capture_id"]
    if any(record_file.capture_id != capture_id for record_file in record_files):
        raise ProfilingInputError("capture ID mismatch across record files")
    if manifest_data is not None and record_files and manifest_data["capture_id"] != str(capture_id):
        raise ProfilingInputError("manifest capture ID mismatch")

    if manifest_data is not None:
        missing_definitions = {p["stream_id"] for p in manifest_data["participants"]} - streams_by_id.keys()
        budget.charge(streams=len(missing_definitions), memory_bytes=len(missing_definitions) * 2048)

    diagnostics: List[Diagnostic] = []
    if not streams_by_id:
        diagnostics.append(
            _diagnostic(
                "no_record_streams",
                "record files contain no profiling streams",
                record_files[0].source if record_files else "manifest",
            )
        )
    lanes: List[Lane] = []
    usable_stream_count = 0
    dropped_record_count = 0
    dropped_stream_count = 0
    overflowed_stream_count = 0

    for stream_id in sorted(streams_by_id):
        stream = streams_by_id[stream_id]
        if not stream.complete:
            diagnostics.append(
                _diagnostic(
                    "stream_not_complete",
                    f"stream {stream.id} was not marked complete",
                    stream.source,
                    stream_id=stream.id,
                )
            )
        if stream.dropped_count:
            dropped_stream_count += 1
            diagnostics.append(
                _diagnostic(
                    "records_dropped",
                    f"stream {stream.id} dropped {stream.dropped_count} record(s)",
                    stream.source,
                    stream_id=stream.id,
                    dropped_count=stream.dropped_count,
                )
            )
        if stream.overflow:
            overflowed_stream_count += 1
            diagnostics.append(
                _diagnostic(
                    "stream_overflow",
                    f"stream {stream.id} overflowed its fixed capture capacity",
                    stream.source,
                    stream_id=stream.id,
                )
            )
        dropped_record_count += stream.dropped_count
        lane, pairing_diagnostics, usable = paired.pop(stream_id)
        lane = replace(lane, attributes=dict(lane.attributes,
            capture_status="incomplete" if pairing_diagnostics or not stream.complete or stream.dropped_count or stream.overflow else "complete",
            timing_usable=usable, dropped_count_is_lower_bound=stream.dropped_count == 0xFFFFFFFF))
        diagnostics.extend(pairing_diagnostics)
        lanes.append(lane)
        if usable:
            usable_stream_count += 1

    launches = (
        Launch(
            id=0,
            lanes=tuple(
                sorted(
                    lanes,
                    key=lambda lane: (
                        lane.rank,
                        lane.host or "",
                        lane.device or "",
                        lane.core,
                        lane.stream_id,
                    ),
                )
            ),
            attributes={},
        ),
    )
    launches = _normalize_timeline(launches, mapping)

    status = (
        "incomplete"
        if any(diagnostic.severity == "incomplete" for diagnostic in diagnostics)
        else "complete"
    )
    run = ProfileRun(
        capture_id=str(capture_id) if capture_id is not None else "",
        run_id=str(capture_id) if capture_id is not None else manifest_data["attempt_id"],
        producer="tilexr",
        operator=mapping.operator,
        mapping_schema=mapping.schema,
        pairing_sha256=pairing_fingerprint(mapping) if mapping.schema == MAPPING_SCHEMA_V2 else None,
        timing_unit="cycles",
        cycles_per_us=mapping.cycles_per_us,
        clock_domain="uncalibrated_device_system_timer",
        timestamp_source="AscendC::GetSystemCycle",
        marker_overhead_us=None,
        status=status,
        launches=launches,
        diagnostics=tuple(diagnostics),
        attributes={},
        sources=(mapping.source,) + tuple(record_file.source for record_file in record_files),
        expected_stream_count=len(streams_by_id),
        present_stream_count=len(streams_by_id),
        usable_stream_count=usable_stream_count,
        missing_stream_count=0,
        dropped_stream_count=dropped_stream_count,
        overflowed_stream_count=overflowed_stream_count,
        excluded_stream_count=len(streams_by_id) - usable_stream_count,
        dropped_record_count=dropped_record_count,
        stages=tuple(replace(stage, attributes=dict(stage.attributes,
            configured_instance_count=len({event.instance_id for event in mapping.events
                                           if event.stage_id == stage.id and event.instance_id is not None})))
                     for stage in mapping.stages),
        coverage={"verified": False, "missing_lanes": None, "missing_ranks": None,
                  "metadata_reasons": ["No manifest was supplied"]},
    )
    if manifest_data is not None:
        source = "<manifest>" if isinstance(manifest, dict) else str(manifest)
        run = replace(run, sources=run.sources + (source,))
        return _apply_manifest(run, manifest_data, mapping)
    return run


def _apply_manifest(run, manifest, mapping):
    """Join caller declarations with retained records facts, never override them."""
    participants = {p["stream_id"]: p for p in manifest["participants"]}
    expected = {sid for sid, p in participants.items() if p["sampled"]}
    observed = {lane.stream_id: lane for launch in run.launches for lane in launch.lanes}
    present = set(observed)
    missing = expected - present
    unexpected = present - expected
    diagnostics = list(run.diagnostics)
    events = manifest.get("events", [])
    skipped = set()
    for sid in missing:
        rank = sid >> 16
        if any(event["code"] == "capture_skipped" and not event["resolved"] and
               (event["scope"] == "attempt" or
                event["scope"] == "rank" and event.get("rank") == rank or
                event["scope"] == "lane" and event.get("stream_id") == sid) for event in events):
            skipped.add(sid)
        diagnostics.append(_diagnostic("capture_skipped" if sid in skipped else "missing_participant",
            f"expected profiling stream {sid} has no records", "manifest", stream_id=sid,
            rank=rank, phase="coverage", scope="lane"))
    for sid in unexpected:
        diagnostics.append(_diagnostic("unexpected_participant", f"profiling stream {sid} was not scheduled for sampling",
                                       "manifest", stream_id=sid, rank=sid >> 16, scope="lane", phase="coverage"))
    metrics = {}
    for item in manifest["stage_metrics"]:
        attributes = {"status": item.get("status", "known"), "source": item["source"]}
        if "reason" in item:
            attributes["reason"] = item["reason"]
        metrics.setdefault(item["stream_id"], []).append(StageMetric(item["stage_id"], item.get("bytes"),
            item.get("byte_semantics", "not_applicable"), item.get("scope", "not_applicable"), attributes))
    raw_completion = {row["stream_id"]: (row["completion"], snapshot["generation"])
                      for snapshot in manifest.get("completion_snapshots", []) for row in snapshot["streams"]}
    lanes = []
    metadata_reasons = []
    for sid in sorted(present | set(participants)):
        participant = participants.get(sid, {})
        lane = observed.get(sid)
        sampled = participant.get("sampled", True)
        if lane is None:
            state = "not_sampled" if not sampled else "capture_skipped" if sid in skipped else "missing"
            lane = Lane(sid, sid >> 16, None, None, sid & 65535, (), (), (),
                        {"capture_status": state, "present": False, "complete": False,
                         "dropped_count": 0, "overflow": False, "timing_usable": False})
        attributes = dict(lane.attributes, sampled=sampled)
        if sid in raw_completion:
            completion, generation = raw_completion[sid]
            if lane.attributes.get("present") and (completion == 2) != lane.attributes.get("complete"):
                raise ProfilingInputError("completion snapshot conflicts with records commit flag")
            attributes["raw_completion"] = ("EMPTY", "WRITING", "COMMITTED")[completion]
            attributes["generation"] = generation
        device = participant.get("device")
        attributes["device_namespace"] = device["namespace"] if device else None
        for key in ("acl_stream_label", "physical_core"):
            if key in participant:
                attributes[key] = participant[key]
        lane = replace(lane, host=participant.get("host"), device=str(device["id"]) if device else None,
                       attributes=attributes, stage_metrics=tuple(metrics.get(sid, ())))
        lanes.append(lane)
        if sampled:
            if not lane.host or device is None:
                metadata_reasons.append(f"stream {sid}: host/device identity not provided")
            declared_stages = {metric.stage_id for metric in lane.stage_metrics}
            for stage in mapping.stages:
                if stage.id not in declared_stages:
                    metadata_reasons.append(f"stream {sid}, stage {stage.id}: byte metric not provided")
    for section, keys in (("environment", ("soc", "cann_version", "driver_version")),
                          ("versions", ("sdk_version", "kernel_version"))):
        for key in keys:
            if manifest.get(section, {}).get(key) in (None, "", "unknown"):
                metadata_reasons.append(f"{section}.{key} not provided")
    execution = manifest.get("execution", {})
    if "kind" not in execution:
        metadata_reasons.append("execution.kind not provided")
    elif execution["kind"] == "communication":
        rows = {(row["rank"], row["stage_id"]) for row in execution.get("stages", [])
                if all(row[name] != "unknown" for name in ("backend", "native_status", "kernel_version"))}
        for rank in sorted({sid >> 16 for sid in expected}):
            for stage in mapping.stages:
                if (rank, stage.id) not in rows:
                    metadata_reasons.append(f"rank {rank}, stage {stage.id}: execution metadata not provided")
    for event in events:
        diagnostics.append(Diagnostic(event["severity"], event["code"], event.get("message", event["code"]),
                                      event["source"], dict(event)))
    if manifest["dropped_events"]:
        diagnostics.append(Diagnostic("error", "host_events_dropped", "Host health observation lost events",
                                      "manifest", {"count": manifest["dropped_events"],
                                       "count_is_lower_bound": manifest["dropped_events"] == 0xFFFFFFFF,
                                       "scope": "attempt", "phase": "health"}))
    for rank in manifest.get("missing_shard_ranks", []):
        diagnostics.append(Diagnostic("error", "missing_manifest_shard", "Expected rank did not provide its metadata/event shard",
                                      "manifest", {"rank": rank, "scope": "rank", "phase": "health"}))
    # Final records decide capture completeness; historical/resolved host failures do not.
    capture_incomplete = bool(missing or unexpected or run.status == "incomplete")
    if not expected and not present:
        status = "not_sampled"
        diagnostics = [d for d in diagnostics if d.code != "no_record_streams"]
    else:
        status = "incomplete" if capture_incomplete else "complete"
    unhealthy = bool(status == "incomplete" or manifest["dropped_events"] or manifest.get("missing_shard_ranks") or
                     any(event["severity"] == "error" and not event["resolved"] for event in events))
    expected_ranks = {sid >> 16 for sid in expected}
    present_ranks = {sid >> 16 for sid in present}
    usable_ranks = {lane.rank for lane in lanes if lane.attributes.get("timing_usable") and lane.stream_id in expected}
    coverage = {
        "verified": True, "expected_ranks": sorted(expected_ranks), "present_ranks": sorted(present_ranks),
        "usable_ranks": sorted(usable_ranks), "missing_ranks": sorted(expected_ranks - present_ranks),
        "expected_lanes": sorted(expected), "missing_lanes": sorted(missing - skipped),
        "unexpected_lanes": sorted(unexpected), "skipped_lanes": sorted(skipped),
        "not_sampled_lanes": sorted(set(participants) - expected),
        "unsubmitted_lanes": sorted(lane.stream_id for lane in lanes if lane.attributes.get("present") and not lane.attributes.get("complete")),
        "affected_lanes": sorted({lane.stream_id for lane in lanes if lane.attributes.get("capture_status") not in {"complete", "not_sampled"}} | unexpected),
        "dropped_records_is_lower_bound": any(lane.attributes.get("dropped_count_is_lower_bound") for lane in lanes),
        "missing_shard_ranks": manifest.get("missing_shard_ranks", []),
        "unobserved_event_ranks": manifest.get("unobserved_event_ranks", []),
        "metadata_reasons": metadata_reasons,
        "publication_status": _lifecycle_status(events, {"export"}, "export_succeeded"),
        "cleanup_status": _lifecycle_status(events, {"close", "cleanup"}, "close_succeeded"),
    }
    observed_health = "events" in manifest and not manifest.get("unobserved_event_ranks")
    return replace(run, status=status, health_status="incomplete" if unhealthy else "complete" if observed_health else "unverified",
        metadata_readiness="incomplete" if metadata_reasons else "ready", coverage=coverage,
        launches=_normalize_timeline((Launch(0, tuple(lanes)),), mapping), diagnostics=tuple(diagnostics),
        expected_stream_count=len(expected), missing_stream_count=len(missing),
        excluded_stream_count=len(expected) - sum(1 for lane in lanes if lane.stream_id in expected and lane.attributes.get("timing_usable")),
        completeness_scope="manifest_verified", capture_events=tuple(events), manifest_metadata=manifest)


def _lifecycle_status(events, phases, success_code):
    if any(event["phase"] in phases and event["severity"] == "error" and not event["resolved"] for event in events):
        return "incomplete"
    if any(event["phase"] in phases and event["code"] == success_code and event["severity"] == "info" and event["resolved"] for event in events):
        return "complete"
    return "unverified"
