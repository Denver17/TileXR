from __future__ import annotations

import argparse
import json
import struct
from collections import defaultdict
from pathlib import Path


TRACE_MARKER = 0x54584454
TRACE_VERSION = 1
TRACE_HEADER_BYTES = 4096
TRACE_CORE_COUNT = 64
TRACE_MAX_ITERATIONS = 100
TRACE_MAX_EVENTS_PER_CORE = 4096
TRACE_CYCLES_PER_US = 1000
TRACE_HEADER_FORMAT = "<IHH8I11Q"
TRACE_CORE_FORMAT = "<QQQ8IQ"
TRACE_EVENT_FORMAT = "<QQQQIi6I"
TRACE_CORE_RECORD_BYTES = struct.calcsize(TRACE_CORE_FORMAT)
TRACE_EVENT_BYTES = struct.calcsize(TRACE_EVENT_FORMAT)

PHASE_NAMES = (
    "route-load",
    "route-select",
    "peer-init",
    "wqe-build",
    "sq-publish",
    "doorbell",
    "cq-wait",
    "udma-execute",
    "credit-wait-mte2",
    "credit-publish-mte3",
    "completion-flag-wait",
    "local-copy",
    "sync-all",
    "output-copy",
    "final-quiet",
)


def trace_layout(iteration_count: int, event_capacity: int) -> dict[str, int]:
    if not 0 < iteration_count <= TRACE_MAX_ITERATIONS:
        raise ValueError(f"iteration_count must be in [1, {TRACE_MAX_ITERATIONS}]")
    if not 0 < event_capacity <= TRACE_MAX_EVENTS_PER_CORE:
        raise ValueError(
            f"event_capacity must be in [1, {TRACE_MAX_EVENTS_PER_CORE}]"
        )
    core_record_offset = TRACE_HEADER_BYTES
    core_count = iteration_count * TRACE_CORE_COUNT
    event_offset = core_record_offset + core_count * TRACE_CORE_RECORD_BYTES
    trace_bytes = event_offset + core_count * event_capacity * TRACE_EVENT_BYTES
    return {
        "core_record_offset": core_record_offset,
        "event_offset": event_offset,
        "trace_bytes": trace_bytes,
    }


def build_trace_header(
    *, rank: int, payload_mode: int, iteration_count: int,
    active_core_count: int, event_capacity: int,
) -> bytes:
    layout = trace_layout(iteration_count, event_capacity)
    if not 0 < active_core_count <= TRACE_CORE_COUNT:
        raise ValueError(f"active_core_count must be in [1, {TRACE_CORE_COUNT}]")
    return struct.pack(
        TRACE_HEADER_FORMAT,
        TRACE_MARKER,
        TRACE_VERSION,
        TRACE_HEADER_BYTES,
        rank,
        payload_mode,
        iteration_count,
        TRACE_CORE_COUNT,
        active_core_count,
        event_capacity,
        len(PHASE_NAMES),
        TRACE_EVENT_BYTES,
        TRACE_CYCLES_PER_US,
        layout["trace_bytes"],
        layout["core_record_offset"],
        layout["event_offset"],
        *([0] * 7),
    )


def _percentile(values: list[int | float], percentile: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * percentile
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def read_trace(path: str | Path) -> dict[str, object]:
    path = Path(path)
    data = path.read_bytes()
    if len(data) < TRACE_HEADER_BYTES:
        raise ValueError(f"short Dispatch trace file: {path}")
    fields = struct.unpack_from(TRACE_HEADER_FORMAT, data, 0)
    header = {
        "marker": fields[0],
        "version": fields[1],
        "header_bytes": fields[2],
        "rank": fields[3],
        "payload_mode": fields[4],
        "iteration_count": fields[5],
        "core_count": fields[6],
        "active_core_count": fields[7],
        "event_capacity": fields[8],
        "phase_count": fields[9],
        "event_bytes": fields[10],
        "cycles_per_us": fields[11],
        "trace_bytes": fields[12],
        "core_record_offset": fields[13],
        "event_offset": fields[14],
    }
    if header["marker"] != TRACE_MARKER or header["version"] != TRACE_VERSION:
        raise ValueError(f"invalid Dispatch trace marker/version in {path}")
    if (
        header["header_bytes"] != TRACE_HEADER_BYTES
        or header["core_count"] != TRACE_CORE_COUNT
        or header["phase_count"] != len(PHASE_NAMES)
        or header["event_bytes"] != TRACE_EVENT_BYTES
        or header["cycles_per_us"] <= 0
    ):
        raise ValueError(f"Dispatch trace ABI mismatch in {path}")
    layout = trace_layout(header["iteration_count"], header["event_capacity"])
    if (
        header["core_record_offset"] != layout["core_record_offset"]
        or header["event_offset"] != layout["event_offset"]
        or header["trace_bytes"] != layout["trace_bytes"]
        or len(data) != layout["trace_bytes"]
        or not 0 < header["active_core_count"] <= TRACE_CORE_COUNT
    ):
        raise ValueError(f"Dispatch trace layout mismatch in {path}")

    cores = []
    events = []
    dropped_total = 0
    for iteration in range(header["iteration_count"]):
        for core in range(header["active_core_count"]):
            core_index = iteration * TRACE_CORE_COUNT + core
            core_offset = header["core_record_offset"] + (
                core_index * TRACE_CORE_RECORD_BYTES
            )
            values = struct.unpack_from(TRACE_CORE_FORMAT, data, core_offset)
            record = {
                "begin_cycle": values[0],
                "end_cycle": values[1],
                "magic": values[2],
                "iteration": values[3],
                "core": values[4],
                "rank": values[5],
                "payload_mode": values[6],
                "event_count": values[7],
                "dropped_count": values[8],
                "status": values[9],
            }
            if record["begin_cycle"] == 0 or record["end_cycle"] < record["begin_cycle"]:
                raise ValueError(
                    f"missing/invalid core span rank={header['rank']} "
                    f"iteration={iteration} core={core}"
                )
            if (
                record["iteration"] != iteration
                or record["core"] != core
                or record["rank"] != header["rank"]
                or record["payload_mode"] != header["payload_mode"]
                or record["event_count"] > header["event_capacity"]
            ):
                raise ValueError(f"Dispatch trace core metadata mismatch in {path}")
            dropped_total += record["dropped_count"]
            cores.append(record)
            event_base = header["event_offset"] + (
                core_index * header["event_capacity"] * TRACE_EVENT_BYTES
            )
            for event_index in range(record["event_count"]):
                event_values = struct.unpack_from(
                    TRACE_EVENT_FORMAT,
                    data,
                    event_base + event_index * TRACE_EVENT_BYTES,
                )
                event = {
                    "begin_cycle": event_values[0],
                    "end_cycle": event_values[1],
                    "bytes": event_values[2],
                    "sequence": event_values[3],
                    "phase": event_values[4],
                    "peer": event_values[5],
                    "qp": event_values[6],
                    "group": event_values[7],
                    "chunk": event_values[8],
                    "wqe_count": event_values[9],
                    "status": event_values[10],
                    "iteration": iteration,
                    "core": core,
                    "rank": header["rank"],
                }
                if (
                    event["begin_cycle"] == 0
                    or event["end_cycle"] < event["begin_cycle"]
                    or event["phase"] >= len(PHASE_NAMES)
                    or event["sequence"] != event_index
                ):
                    raise ValueError(f"invalid Dispatch trace event in {path}")
                event["phase_name"] = PHASE_NAMES[event["phase"]]
                event["duration_cycles"] = event["end_cycle"] - event["begin_cycle"]
                events.append(event)
    if dropped_total:
        raise ValueError(f"Dispatch trace dropped {dropped_total} events in {path}")
    return {"path": str(path), "header": header, "cores": cores, "events": events}


def build_phase_summary(traces: list[dict[str, object]]) -> dict[str, object]:
    durations: dict[str, list[int]] = defaultdict(list)
    byte_counts: dict[str, list[int]] = defaultdict(list)
    for trace in traces:
        for event in trace["events"]:
            durations[event["phase_name"]].append(event["duration_cycles"])
            byte_counts[event["phase_name"]].append(event["bytes"])
    cycles_per_us = traces[0]["header"]["cycles_per_us"] if traces else 1
    phases = {}
    for phase, values in sorted(durations.items()):
        phases[phase] = {
            "count": len(values),
            "mean_us": sum(values) / len(values) / cycles_per_us,
            "p50_us": _percentile(values, 0.50) / cycles_per_us,
            "p95_us": _percentile(values, 0.95) / cycles_per_us,
            "max_us": max(values) / cycles_per_us,
        }
        transferred = byte_counts[phase]
        if any(transferred):
            bandwidths = [
                byte_count * cycles_per_us / duration / 1000.0
                for byte_count, duration in zip(transferred, values)
                if byte_count and duration
            ]
            phases[phase].update({
                "bytes_total": sum(transferred),
                "p50_effective_gb_s": _percentile(bandwidths, 0.50),
                "p95_effective_gb_s": _percentile(bandwidths, 0.95),
                "max_effective_gb_s": max(bandwidths),
            })
    return {"trace_count": len(traces), "phases": phases}


def build_chrome_trace(traces: list[dict[str, object]]) -> dict[str, object]:
    output = []
    for trace in traces:
        header = trace["header"]
        base = min(record["begin_cycle"] for record in trace["cores"])
        for event in trace["events"]:
            output.append({
                "name": event["phase_name"],
                "cat": "moonep-dispatch",
                "ph": "X",
                "pid": event["rank"],
                "tid": event["core"],
                "ts": (event["begin_cycle"] - base) / header["cycles_per_us"],
                "dur": event["duration_cycles"] / header["cycles_per_us"],
                "args": {
                    "iteration": event["iteration"],
                    "peer": event["peer"],
                    "qp": event["qp"],
                    "group": event["group"],
                    "chunk": event["chunk"],
                    "wqeCount": event["wqe_count"],
                    "bytes": event["bytes"],
                    "status": event["status"],
                },
            })
    return {"traceEvents": output}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Convert MoonEP Dispatch GM traces")
    parser.add_argument("inputs", nargs="+")
    parser.add_argument("--chrome-output", required=True)
    parser.add_argument("--summary-output", required=True)
    args = parser.parse_args(argv)
    traces = [read_trace(path) for path in args.inputs]
    Path(args.chrome_output).write_text(
        json.dumps(build_chrome_trace(traces), separators=(",", ":")),
        encoding="utf-8",
    )
    Path(args.summary_output).write_text(
        json.dumps(build_phase_summary(traces), indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
