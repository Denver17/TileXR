# Purpose: Render paired capture samples and provenance as Perfetto trace events.
# 用途与关系：将配对后的 capture 样本和来源信息渲染为 Perfetto 事件。
"""Perfetto/Chrome Trace Event rendering."""

import json
from typing import Any, Dict, Optional

from .io import json_chunks

from .analysis import AnalysisResult
from .model import ProfileRun
from .provenance import build_provenance, report_payload


def _trace_events(run, analysis):
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
        yield (
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
    next_sort_index: Dict[int, int] = {}

    def add_track(pid: int, name: str, tid: Optional[int] = None):
        if tid is None:
            tid = 0
            while tid in used_tids[pid]:
                tid += 1
            used_tids[pid].add(tid)
        sort_index = next_sort_index.get(pid, 0)
        next_sort_index[pid] = sort_index + 1
        yield from (
            [
                {
                    "name": "thread_name", "ph": "M", "pid": pid, "tid": tid,
                    "args": {"name": name},
                },
                {
                    "name": "thread_sort_index", "ph": "M", "pid": pid, "tid": tid,
                    "args": {"sort_index": sort_index},
                },
            ]
        )
        return tid

    for launch_id, lane in lane_groups:
        device = lane.device or "unknown"
        pid = pids[(launch_id, lane.rank, device)]
        track_prefix = f"host{lane.host if lane.host is not None else 'unknown'}/rank{lane.rank}/device{device}/core{lane.core}"
        yield from add_track(pid, f"{track_prefix}/stream{lane.stream_id}", lane.stream_id)

        grouped_tracks = sorted(
            {
                sample.attributes.get("track")
                for sample in lane.samples
                if isinstance(sample.attributes.get("track"), str)
            }
        )
        for track_name in grouped_tracks:
            grouped_key = (pid, lane.stream_id, track_name)
            grouped_tids[grouped_key] = yield from add_track(pid, f"{track_prefix}/{track_name}")

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
            instance_tids[instance_key] = yield from add_track(
                pid, f"{track_prefix}/{stage_name}[{instance_id}]"
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
            event = {
                "name": event_name,
                "cat": sample.category,
                "ph": "i" if sample.kind == "instant" else "X",
                "pid": pid,
                "tid": event_tid,
                "ts": sample.start_us,
                "args": args,
            }
            if sample.kind == "instant":
                event["s"] = "t"
            else:
                event["dur"] = sample.duration_us
            yield event

    # Diagnostics retain their original clock domains in report metadata.
    # Host and device timestamps must not be placed on an uncalibrated shared axis.


def _perfetto_envelope(run, analysis):
    return {
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
            "display_origin": run.display_origin,
            "cycles_per_us": run.cycles_per_us,
            "timestamp_source": run.timestamp_source,
            "marker_overhead_us": run.marker_overhead_us,
            "expected_stream_count": run.expected_stream_count,
            "present_stream_count": run.present_stream_count,
            "usable_stream_count": run.usable_stream_count,
            "missing_stream_count": run.missing_stream_count if run.manifest_metadata else None,
            "dropped_stream_count": run.dropped_stream_count,
            "overflowed_stream_count": run.overflowed_stream_count,
            "excluded_stream_count": run.excluded_stream_count,
            "dropped_record_count": run.dropped_record_count,
            "provenance": build_provenance(run),
            "report": report_payload(run, analysis),
        },
    }


def iter_perfetto(run, analysis):
    yield '{"traceEvents":['
    first = True
    for event in _trace_events(run, analysis):
        if not first:
            yield ","
        first = False
        # Bound encoding to one event. Repeated iterencode calls create recursive
        # closure cycles whose retention depends on the interpreter's GC timing.
        yield json.dumps(event, ensure_ascii=False, allow_nan=False, sort_keys=True)
    yield "]"
    for key, value in _perfetto_envelope(run, analysis).items():
        yield ","
        yield from json_chunks(key)
        yield ":"
        yield from json_chunks(value)
    yield "}\n"


def render_perfetto(run: ProfileRun, analysis: AnalysisResult) -> Dict[str, Any]:
    return dict(traceEvents=list(_trace_events(run, analysis)), **_perfetto_envelope(run, analysis))
