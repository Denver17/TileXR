"""Perfetto/Chrome Trace Event rendering."""

from typing import Any, Dict, List

from .analysis import AnalysisResult
from .model import ProfileRun


def render_perfetto(run: ProfileRun, analysis: AnalysisResult) -> Dict[str, Any]:
    events: List[Dict[str, Any]] = []
    lane_groups = [
        (launch.id, lane)
        for launch in run.launches
        for lane in launch.lanes
    ]
    lane_groups.sort(
        key=lambda item: (
            item[0],
            item[1].rank,
            item[1].host or "",
            item[1].device or "",
            item[1].core,
            item[1].stream_id,
        )
    )

    process_keys = sorted(
        {(launch_id, lane.rank, lane.device or "unknown") for launch_id, lane in lane_groups}
    )
    pids = {key: index for index, key in enumerate(process_keys)}
    for launch_id, rank, device in process_keys:
        pid = pids[(launch_id, rank, device)]
        events.append(
            {
                "name": "process_name",
                "ph": "M",
                "pid": pid,
                "args": {
                    "name": f"{run.producer}/{run.operator}/launch{launch_id}/rank{rank}/device{device}"
                },
            }
        )

    used_tids: Dict[int, set] = {}
    for launch_id, lane in lane_groups:
        pid = pids[(launch_id, lane.rank, lane.device or "unknown")]
        used_tids.setdefault(pid, set()).add(lane.stream_id)

    instance_tids: Dict[tuple, int] = {}
    grouped_tids: Dict[tuple, int] = {}
    track_sort_indices: Dict[tuple, int] = {}
    next_sort_index: Dict[int, int] = {}
    for launch_id, lane in lane_groups:
        device = lane.device or "unknown"
        pid = pids[(launch_id, lane.rank, device)]
        main_track_key = ("stream", pid, lane.stream_id)
        track_sort_indices[main_track_key] = next_sort_index.get(pid, 0)
        next_sort_index[pid] = track_sort_indices[main_track_key] + 1
        events.extend(
            [
                {
                    "name": "thread_name",
                    "ph": "M",
                    "pid": pid,
                    "tid": lane.stream_id,
                    "args": {
                        "name": f"rank{lane.rank}/device{device}/core{lane.core}/stream{lane.stream_id}"
                    },
                },
                {
                    "name": "thread_sort_index",
                    "ph": "M",
                    "pid": pid,
                    "tid": lane.stream_id,
                    "args": {"sort_index": track_sort_indices[main_track_key]},
                },
            ]
        )

        grouped_tracks = sorted(
            {
                sample.attributes.get("track")
                for sample in lane.samples
                if isinstance(sample.attributes.get("track"), str)
            }
        )
        for track_name in grouped_tracks:
            grouped_key = (pid, lane.stream_id, track_name)
            candidate_tid = 0
            while candidate_tid in used_tids[pid]:
                candidate_tid += 1
            used_tids[pid].add(candidate_tid)
            grouped_tids[grouped_key] = candidate_tid
            track_key = ("grouped",) + grouped_key
            track_sort_indices[track_key] = next_sort_index[pid]
            next_sort_index[pid] += 1
            events.extend(
                [
                    {
                        "name": "thread_name",
                        "ph": "M",
                        "pid": pid,
                        "tid": candidate_tid,
                        "args": {
                            "name": (
                                f"rank{lane.rank}/device{device}/core{lane.core}/"
                                f"{track_name}"
                            )
                        },
                    },
                    {
                        "name": "thread_sort_index",
                        "ph": "M",
                        "pid": pid,
                        "tid": candidate_tid,
                        "args": {"sort_index": track_sort_indices[track_key]},
                    },
                ]
            )

        instance_tracks = sorted(
            {
                (sample.stage_id, sample.instance_id, sample.name)
                for sample in lane.samples
                if sample.instance_id is not None
                and not isinstance(sample.attributes.get("track"), str)
            }
        )
        for stage_id, instance_id, stage_name in instance_tracks:
            instance_key = (pid, lane.stream_id, stage_id, instance_id)
            candidate_tid = 0
            while candidate_tid in used_tids[pid]:
                candidate_tid += 1
            used_tids[pid].add(candidate_tid)
            instance_tids[instance_key] = candidate_tid
            track_key = ("instance",) + instance_key
            track_sort_indices[track_key] = next_sort_index[pid]
            next_sort_index[pid] += 1
            events.extend(
                [
                    {
                        "name": "thread_name",
                        "ph": "M",
                        "pid": pid,
                        "tid": candidate_tid,
                        "args": {
                            "name": (
                                f"rank{lane.rank}/device{device}/core{lane.core}/"
                                f"{stage_name}[{instance_id}]"
                            )
                        },
                    },
                    {
                        "name": "thread_sort_index",
                        "ph": "M",
                        "pid": pid,
                        "tid": candidate_tid,
                        "args": {"sort_index": track_sort_indices[track_key]},
                    },
                ]
            )

    for launch_id, lane in lane_groups:
        device = lane.device or "unknown"
        pid = pids[(launch_id, lane.rank, device)]
        metrics = {metric.stage_id: metric for metric in lane.stage_metrics}
        for sample in lane.samples:
            metric = metrics.get(sample.stage_id)
            event_name = (
                sample.name
                if sample.instance_id is None
                else f"{sample.name}[{sample.instance_id}]"
            )
            event_tid = lane.stream_id
            track_name = sample.attributes.get("track")
            if isinstance(track_name, str):
                event_tid = grouped_tids[(pid, lane.stream_id, track_name)]
            elif sample.instance_id is not None:
                event_tid = instance_tids[
                    (pid, lane.stream_id, sample.stage_id, sample.instance_id)
                ]
            args = {
                "capture_id": run.capture_id,
                "run_id": run.run_id,
                "producer": run.producer,
                "operator": run.operator,
                "launch_id": launch_id,
                "stream_id": lane.stream_id,
                "rank": lane.rank,
                "host": lane.host,
                "device": lane.device,
                "core": lane.core,
                "stage_id": sample.stage_id,
                "parent_stage_id": sample.parent_stage_id,
                "instance_id": sample.instance_id,
                "category": sample.category,
                "kind": sample.kind,
                "critical": sample.critical,
                "begin_event_id": sample.begin_event_id,
                "end_event_id": sample.end_event_id,
                "raw_start": sample.raw_start,
                "raw_end": sample.raw_end,
                "timing_unit": run.timing_unit,
                "source": sample.source,
                "source_index": sample.source_index,
                "end_source_index": sample.end_source_index,
                "attributes": sample.attributes,
                "bytes": metric.bytes if metric is not None else None,
                "byte_semantics": metric.byte_semantics if metric is not None else None,
                "byte_scope": metric.scope if metric is not None else None,
                "metric_attributes": metric.attributes if metric is not None else {},
            }
            if sample.kind == "instant":
                events.append(
                    {
                        "name": event_name,
                        "cat": sample.category,
                        "ph": "i",
                        "s": "t",
                        "pid": pid,
                        "tid": event_tid,
                        "ts": sample.start_us,
                        "args": args,
                    }
                )
            else:
                events.append(
                    {
                        "name": event_name,
                        "cat": sample.category,
                        "ph": "X",
                        "pid": pid,
                        "tid": event_tid,
                        "ts": sample.start_us,
                        "dur": sample.duration_us,
                        "args": args,
                    }
                )

    diagnostic_pid = 0
    diagnostic_tid = 0
    if analysis.critical_lane is not None:
        diagnostic_pid = pids.get(
            (
                analysis.critical_lane.launch_id,
                analysis.critical_lane.rank,
                analysis.critical_lane.device or "unknown",
            ),
            0,
        )
        diagnostic_tid = analysis.critical_lane.stream_id
    for index, diagnostic in enumerate(run.diagnostics):
        events.append(
            {
                "name": diagnostic.message,
                "cat": "diagnostic",
                "ph": "i",
                "s": "g",
                "pid": diagnostic_pid,
                "tid": diagnostic_tid,
                "ts": float(index),
                "args": {
                    "severity": diagnostic.severity,
                    "code": diagnostic.code,
                    "source": diagnostic.source,
                    "details": diagnostic.details,
                },
            }
        )

    return {
        "traceEvents": events,
        "displayTimeUnit": "us",
        "metadata": {
            "schema": "tilexr_perfetto_trace.v1",
            "capture_id": run.capture_id,
            "run_id": run.run_id,
            "producer": run.producer,
            "operator": run.operator,
            "status": analysis.status,
            "timing_unit": run.timing_unit,
            "clock_domain": run.clock_domain,
            "timestamp_source": run.timestamp_source,
            "marker_overhead_us": run.marker_overhead_us,
            "expected_stream_count": run.expected_stream_count,
            "present_stream_count": run.present_stream_count,
            "usable_stream_count": run.usable_stream_count,
            "missing_stream_count": run.missing_stream_count,
            "dropped_stream_count": run.dropped_stream_count,
            "overflowed_stream_count": run.overflowed_stream_count,
            "excluded_stream_count": run.excluded_stream_count,
            "dropped_record_count": run.dropped_record_count,
        },
    }
