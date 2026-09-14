"""Pair numeric event records and build the normalized profiling model."""

import math
from bisect import bisect_right
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

    def abandon_interval(key: Tuple[int, Optional[int]], begin: RawRecord) -> None:
        instance_label = "" if key[1] is None else f" instance {key[1]}"
        diagnostics.append(
            _diagnostic(
                "unmatched_begin",
                f"stage {key[0]}{instance_label} began without a matching end event",
                stream.source,
                stream_id=stream.id,
                stage_id=key[0],
                instance_id=key[1],
                event_id=begin.event_id,
                record_index=begin.source_index,
                raw_timestamp=begin.timestamp,
            )
        )

    def abandon_children(stage_id: int) -> None:
        for key in list(open_intervals):
            if key[0] in descendants[stage_id]:
                abandon_interval(key, open_intervals.pop(key))
        invalid_intervals.difference_update(
            key for key in tuple(invalid_intervals) if key[0] in descendants[stage_id]
        )

    descendants = {stage.id: set() for stage in mapping.stages}
    for stage in mapping.stages:
        parent = stage.parent_stage_id
        while parent is not None:
            descendants[parent].add(stage.id)
            parent = stages[parent].parent_stage_id

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
            # A new parent invocation cannot complete a child from an earlier
            # invocation, even when both use the same numeric event IDs.
            if not any(key[0] == stage.id and key != interval_key for key in open_intervals):
                abandon_children(stage.id)
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
            invalid_intervals.remove(interval_key)
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

        if not any(key[0] == stage.id for key in open_intervals):
            abandon_children(stage.id)

    for key, begin in sorted(
        open_intervals.items(),
        key=lambda item: (
            item[0][0],
            item[0][1] if item[0][1] is not None else -1,
        ),
    ):
        abandon_interval(key, begin)

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
    # Prefix maxima allow overlapping named instances while avoiding a scan of
    # every parent for every repeated child (quadratic for long captures).
    parent_bounds = {}
    for stage_id, stage_samples in samples_by_stage.items():
        starts = []
        ends = []
        for sample in sorted(stage_samples, key=lambda item: item.source_index):
            starts.append(sample.source_index)
            end = sample.end_source_index if sample.end_source_index is not None else sample.source_index
            ends.append(max(ends[-1] if ends else -1, end))
        parent_bounds[stage_id] = (starts, ends)
    for sample in samples:
        if sample.parent_stage_id is None:
            continue
        starts, ends = parent_bounds.get(sample.parent_stage_id, ([], []))
        index = bisect_right(starts, sample.source_index) - 1
        sample_end = sample.end_source_index if sample.end_source_index is not None else sample.source_index
        if index < 0 or ends[index] < sample_end:
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
    diagnostics = [
        replace(item, details={**item.details, "launch_id": definition.launch_id})
        for item in diagnostics
    ]
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


def _split_stream(stream: RecordStream, mapping: ProfilingMapping) -> Dict[int, RecordStream]:
    """Split only on declared critical begins; raw timestamps are not launch IDs."""
    critical_ids = {stage.id for stage in mapping.stages if stage.critical}
    starts = {
        event.id for event in mapping.events
        if event.stage_id in critical_ids and event.edge == "begin"
    }
    chunks = {}
    begin_indices = [
        index for index, record in enumerate(stream.records) if record.event_id in starts
    ]
    if not begin_indices:
        # Keep a stream with no begin available for diagnosis, including empty
        # streams. It cannot supply a usable launch interval.
        return {-1: stream}
    if begin_indices[0]:
        chunks[-1] = replace(stream, records=stream.records[:begin_indices[0]])
    ends = begin_indices[1:] + [len(stream.records)]
    for launch_id, (begin, end) in enumerate(zip(begin_indices, ends)):
        chunks[launch_id] = replace(stream, records=stream.records[begin:end])
    return chunks


def _validate_launch_count(value: Optional[int], name: str, allow_zero: bool) -> None:
    if value is not None and (
        isinstance(value, bool) or not isinstance(value, int) or value < (0 if allow_zero else 1)
    ):
        qualifier = "non-negative" if allow_zero else "positive"
        raise ProfilingInputError(f"{name} must be a {qualifier} integer")


def load_profile_run(
    mapping_input: MappingInput,
    record_paths: Sequence[Path],
    *,
    skip_launches: int = 0,
    launch_count: Optional[int] = None,
    expected_launch_count: Optional[int] = None,
) -> ProfileRun:
    """Normalize launches in capture order, optionally selecting measured launches.

    Launch IDs are the ordinal of critical begins in each stream, not inferred
    from unsynchronized timestamps. Completeness diagnostics cover the entire
    capture even when warmup or later launches are excluded from timing reports.
    """
    if skip_launches is None:
        raise ProfilingInputError("skip_launches must be a non-negative integer")
    _validate_launch_count(skip_launches, "skip_launches", True)
    _validate_launch_count(launch_count, "launch_count", False)
    _validate_launch_count(expected_launch_count, "expected_launch_count", False)
    if expected_launch_count is not None and (
        skip_launches >= expected_launch_count or
        (launch_count is not None and skip_launches + launch_count > expected_launch_count)
    ):
        raise ProfilingInputError("launch selection exceeds expected_launch_count")
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
    lanes_by_launch: Dict[int, List[Lane]] = {}
    usable_streams = set()
    launch_counts = {}
    usable_lane_count = 0
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
        chunks = _split_stream(stream, mapping)
        launch_counts[stream_id] = sum(launch_id >= 0 for launch_id in chunks)
        for launch_id, chunk in chunks.items():
            definition = StreamDefinition(
                id=stream.id,
                launch_id=launch_id,
                rank=stream.id >> 16,
                host=None,
                device=None,
                core=stream.id & 0xFFFF,
                attributes={"launch_identity": "critical_begin_ordinal"},
                stage_metrics=(),
            )
            lane, pairing_diagnostics, usable = _pair_stream(chunk, definition, mapping)
            diagnostics.extend(pairing_diagnostics)
            lanes_by_launch.setdefault(launch_id, []).append(lane)
            selected = launch_id >= skip_launches and (
                launch_count is None or launch_id < skip_launches + launch_count
            )
            if usable and selected:
                usable_streams.add(stream_id)
                usable_lane_count += 1

    observed_launch_count = max(launch_counts.values(), default=0)
    target_launch_count = expected_launch_count or observed_launch_count
    missing_lane_count = 0
    for stream_id, count in launch_counts.items():
        if count != target_launch_count:
            missing_lane_count += max(0, target_launch_count - count)
            diagnostics.append(
                _diagnostic(
                    "launch_count_mismatch",
                    f"stream {stream_id} has {count} launch begin(s); expected {target_launch_count}; "
                    "a wholly missing launch cannot be located from timestamps",
                    streams_by_id[stream_id].source,
                    stream_id=stream_id,
                    observed_launch_count=count,
                    expected_launch_count=target_launch_count,
                )
            )
    stop = skip_launches + launch_count if launch_count is not None else observed_launch_count
    if skip_launches >= observed_launch_count or (
        launch_count is not None and stop > observed_launch_count
    ):
        diagnostics.append(
            _diagnostic(
                "launch_selection_incomplete",
                "the requested launch selection is not fully present in the capture",
                None,
                skip_launches=skip_launches,
                launch_count=launch_count,
                observed_launch_count=observed_launch_count,
            )
        )
    launches = tuple(
        Launch(
            id=launch_id,
            lanes=tuple(sorted(lanes, key=lambda lane: (lane.rank, lane.core, lane.stream_id))),
            attributes={"identity": "critical_begin_ordinal"} if launch_id >= 0 else {"fragment": True},
        )
        for launch_id, lanes in sorted(lanes_by_launch.items())
        if skip_launches <= launch_id < stop or (launch_id == -1 and skip_launches == 0)
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
        attributes={
            "observed_launch_count": observed_launch_count,
            "expected_launch_count": expected_launch_count,
            "skip_launches": skip_launches,
            "launch_count": launch_count,
            "selected_launch_ids": [launch.id for launch in launches if launch.id >= 0],
            "launch_identity": "critical_begin_ordinal",
            "missing_launch_lane_count": missing_lane_count,
            "usable_launch_lane_count": usable_lane_count,
        },
        sources=(mapping.source,) + tuple(record_file.source for record_file in record_files),
        expected_stream_count=len(streams_by_id),
        present_stream_count=len(streams_by_id),
        usable_stream_count=len(usable_streams),
        missing_stream_count=0,
        dropped_stream_count=dropped_stream_count,
        overflowed_stream_count=overflowed_stream_count,
        excluded_stream_count=len(streams_by_id) - len(usable_streams),
        dropped_record_count=dropped_record_count,
    )
