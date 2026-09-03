"""Pair numeric event records and build the normalized profiling model."""

import math
from dataclasses import replace
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .mapping import MappingInput, load_mapping
from .model import (
    Diagnostic,
    Lane,
    Launch,
    ProfileRun,
    ProfilingMapping,
    ProfilingInputError,
    RawRecord,
    RecordStream,
    StageDefinition,
    StageSample,
    StreamDefinition,
)
from .records import load_record_files


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


def _pair_stream(
    stream: RecordStream,
    definition: StreamDefinition,
    mapping: ProfilingMapping,
) -> Tuple[Lane, Tuple[Diagnostic, ...], bool]:
    stages: Dict[int, StageDefinition] = {stage.id: stage for stage in mapping.stages}
    events = {event.id: event for event in mapping.events}
    open_intervals: Dict[Tuple[int, Optional[int]], RawRecord] = {}
    invalid_intervals = set()
    samples = []
    diagnostics: List[Diagnostic] = []

    for record in stream.records:
        event = events.get(record.event_id)
        if event is None:
            raise ProfilingInputError(
                f"stream {stream.id} contains undeclared event ID {record.event_id} in {stream.source}"
            )
        stage = stages[event.stage_id]
        interval_key = (stage.id, event.instance_id)
        if event.edge == "instant":
            samples.append(
                StageSample(
                    name=stage.name,
                    stage_id=stage.id,
                    parent_stage_id=stage.parent_stage_id,
                    instance_id=event.instance_id,
                    category=stage.category,
                    kind=stage.kind,
                    raw_start=record.timestamp,
                    raw_end=record.timestamp,
                    start_us=0.0,
                    end_us=0.0,
                    duration_us=0.0,
                    critical=False,
                    begin_event_id=record.event_id,
                    end_event_id=None,
                    attributes=stage.attributes,
                    source=record.source,
                    source_index=record.source_index,
                )
            )
            continue

        if event.edge == "begin":
            if interval_key in invalid_intervals:
                continue
            if interval_key in open_intervals:
                instance_label = (
                    "" if event.instance_id is None else f" instance {event.instance_id}"
                )
                diagnostics.append(
                    _diagnostic(
                        "overlapping_begin",
                        f"stage {stage.id}{instance_label} began again before its previous interval ended",
                        stream.source,
                        stream_id=stream.id,
                        stage_id=stage.id,
                        instance_id=event.instance_id,
                        event_id=record.event_id,
                        record_index=record.source_index,
                        raw_timestamp=record.timestamp,
                    )
                )
                open_intervals.pop(interval_key)
                invalid_intervals.add(interval_key)
            else:
                open_intervals[interval_key] = record
            continue

        if interval_key in invalid_intervals:
            continue
        begin = open_intervals.pop(interval_key, None)
        if begin is None:
            instance_label = (
                "" if event.instance_id is None else f" instance {event.instance_id}"
            )
            diagnostics.append(
                _diagnostic(
                    "unmatched_end",
                    f"stage {stage.id}{instance_label} ended without a matching begin event",
                    stream.source,
                    stream_id=stream.id,
                    stage_id=stage.id,
                    instance_id=event.instance_id,
                    event_id=record.event_id,
                    record_index=record.source_index,
                    raw_timestamp=record.timestamp,
                )
            )
            continue
        duration_us = _to_us(record.timestamp - begin.timestamp, mapping.cycles_per_us)
        samples.append(
            StageSample(
                name=stage.name,
                stage_id=stage.id,
                parent_stage_id=stage.parent_stage_id,
                instance_id=event.instance_id,
                category=stage.category,
                kind=stage.kind,
                raw_start=begin.timestamp,
                raw_end=record.timestamp,
                start_us=0.0,
                end_us=duration_us,
                duration_us=duration_us,
                critical=stage.critical,
                begin_event_id=begin.event_id,
                end_event_id=record.event_id,
                attributes=stage.attributes,
                source=begin.source,
                source_index=begin.source_index,
                end_source_index=record.source_index,
            )
        )

    for (stage_id, instance_id), begin in sorted(
        open_intervals.items(),
        key=lambda item: (
            item[0][0],
            item[0][1] if item[0][1] is not None else -1,
        ),
    ):
        instance_label = "" if instance_id is None else f" instance {instance_id}"
        diagnostics.append(
            _diagnostic(
                "unmatched_begin",
                f"stage {stage_id}{instance_label} began without a matching end event",
                stream.source,
                stream_id=stream.id,
                stage_id=stage_id,
                instance_id=instance_id,
                event_id=begin.event_id,
                record_index=begin.source_index,
                raw_timestamp=begin.timestamp,
            )
        )

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
            diagnostics.append(
                _diagnostic(
                    "child_outside_parent",
                    f"stage {sample.stage_id} sample is not contained by parent stage "
                    f"{sample.parent_stage_id}",
                    sample.source,
                    stream_id=stream.id,
                    stage_id=sample.stage_id,
                    parent_stage_id=sample.parent_stage_id,
                    instance_id=sample.instance_id,
                    record_index=sample.source_index,
                    raw_timestamp=sample.raw_start,
                )
            )
    critical_count = sum(1 for sample in samples if sample.critical)
    usable = critical_count == 1
    if not usable:
        diagnostics.append(
            _diagnostic(
                "invalid_critical_interval_count",
                f"stream {stream.id} has {critical_count} complete critical intervals; expected exactly one",
                stream.source,
                stream_id=stream.id,
                critical_interval_count=critical_count,
            )
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
) -> ProfileRun:
    """Validate a marker mapping and record files, then normalize one run."""
    mapping = load_mapping(mapping_input)
    record_files = load_record_files(record_paths)
    capture_id = record_files[0].capture_id

    streams_by_id: Dict[int, RecordStream] = {}
    for record_file in record_files:
        if record_file.capture_id != capture_id:
            raise ProfilingInputError(
                "capture ID mismatch across record files: "
                f"{record_files[0].source} and {record_file.source}"
            )
        for stream in record_file.streams:
            if stream.id in streams_by_id:
                raise ProfilingInputError(f"duplicate stream ID {stream.id} across record files")
            streams_by_id[stream.id] = stream

    diagnostics: List[Diagnostic] = []
    if not streams_by_id:
        diagnostics.append(
            _diagnostic(
                "no_record_streams",
                "record files contain no profiling streams",
                record_files[0].source,
            )
        )
    lanes: List[Lane] = []
    usable_stream_count = 0
    dropped_record_count = 0
    dropped_stream_count = 0
    overflowed_stream_count = 0

    for stream_id in sorted(streams_by_id):
        stream = streams_by_id[stream_id]
        definition = StreamDefinition(
            id=stream.id,
            launch_id=0,
            rank=stream.id >> 16,
            host=None,
            device=None,
            core=stream.id & 0xFFFF,
            attributes={},
            stage_metrics=(),
        )
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
        lane, pairing_diagnostics, usable = _pair_stream(stream, definition, mapping)
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
    return ProfileRun(
        capture_id=str(capture_id),
        run_id=str(capture_id),
        producer="tilexr",
        operator=mapping.operator,
        timing_unit="cycles",
        cycles_per_us=mapping.cycles_per_us,
        clock_domain="per_core",
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
    )
