#!/usr/bin/env python3

import argparse
import html
import json
import re
from collections import defaultdict
from pathlib import Path


TRACE_SCHEMA = "tilexr_perf_trace_report.v1"
RUN_SCHEMA = "tilexr_perf_trace_run.v1"
HOST_SCHEMA = "tilexr_collective_profile_host.v1"
RANK_LAUNCH_RE = re.compile(r"rank([0-9]+)[/\\]launch([0-9]+)[/\\]trace\.json$")
PERFETTO_LAUNCH_GAP_US = 50.0
PERFETTO_LAUNCH_WINDOW_TID = 1000000
TILEXR_TRACE_TRANSPORT_MEMORY = "MEMORY"
TILEXR_TRACE_TRANSPORT_UNKNOWN = "UNKNOWN"
TILEXR_TRACE_INFERRED_METRICS_SOURCE = "tilexr_profile_inferred"
TILEXR_COPY_STAGES = frozenset(("local_input_to_ipc", "peer_ipc_to_output"))
TILEXR_OP_TYPE_ALL_GATHER = 3
TILEXR_TRACE_UNKNOWN_RANK = -1


def parse_args():
    parser = argparse.ArgumentParser(description="Aggregate TileXR collective profile traces")
    parser.add_argument("profile_dir")
    parser.add_argument("--warmup-iters", type=int, default=0)
    parser.add_argument("--iters", type=int, default=0)
    parser.add_argument("--profile-sample-every", type=int, default=1)
    parser.add_argument("--emit-ai-prompt", action="store_true")
    return parser.parse_args()


def relpath(path, root):
    return path.relative_to(root).as_posix()


def parse_rank_launch(path, root):
    match = RANK_LAUNCH_RE.search(relpath(path, root))
    if match is None:
        return None
    return int(match.group(1)), int(match.group(2))


def load_host_infos(root, diagnostics):
    hosts = {}
    for path in sorted(root.glob("rank*/host_info.json")):
        source = relpath(path, root)
        try:
            info = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            diagnostics.append(f"invalid json in {source}: {exc}")
            continue
        if not isinstance(info, dict):
            diagnostics.append(f"invalid top-level host info type in {source}: expected object")
            continue
        if info.get("schema") != HOST_SCHEMA:
            diagnostics.append(f"invalid schema in {source}")
            continue
        rank = as_int(info.get("rank"), None)
        if rank is None:
            parsed = re.search(r"rank([0-9]+)[/\\]host_info\.json$", source)
            rank = int(parsed.group(1)) if parsed else None
        if rank is None:
            diagnostics.append(f"invalid rank in {source}")
            continue
        hosts[rank] = {
            "rank": rank,
            "host": str(info.get("host") or f"rank{rank}"),
            "ip": str(info.get("ip") or ""),
            "comm_mode": str(info.get("comm_mode") or ""),
            "clock_offset_ns": str(info.get("clock_offset_ns") or ""),
            "clock_reference": str(info.get("clock_reference") or ""),
            "epoch_ns": str(info.get("epoch_ns") or ""),
            "source": source,
        }
    return hosts


def load_traces(root):
    traces = []
    diagnostics = []

    for path in sorted(root.glob("rank*/launch*/trace.json")):
        parsed = parse_rank_launch(path, root)
        if parsed is None:
            diagnostics.append(f"ignored unrecognized trace path {relpath(path, root)}")
            continue

        rank, launch_id = parsed
        source = relpath(path, root)
        try:
            trace = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            diagnostics.append(f"invalid json in {source}: {exc}")
            continue

        if not isinstance(trace, dict):
            diagnostics.append(f"invalid top-level trace type in {source}: expected object")
            continue

        if trace.get("schema") != TRACE_SCHEMA:
            diagnostics.append(f"invalid schema in {source}")
            continue

        trace = sanitize_trace(trace, source, diagnostics)
        traces.append({"rank": rank, "launch_id": launch_id, "path": path, "trace": trace})

    return traces, diagnostics


def sanitize_trace(trace, source, diagnostics):
    trace = dict(trace)
    if trace.get("incomplete"):
        reason = str(trace.get("incomplete_reason") or "unknown")
        diagnostics.append(f"incomplete trace in {source}: {reason}")

    stats = trace.get("stats", [])
    if not isinstance(stats, list):
        diagnostics.append(f"invalid stats in {source}: expected list")
        trace["stats"] = []
        return trace

    valid_stats = []
    for index, stat in enumerate(stats):
        if not isinstance(stat, dict):
            diagnostics.append(f"invalid stat entry in {source} stats[{index}]: expected object")
            continue
        if as_int(stat.get("count")) <= 0:
            raw_cycles = as_int(stat.get("raw_cycles"))
            max_cycles = as_int(stat.get("max_cycles"))
            last_end = as_int(stat.get("last_end_cycle"))
            if raw_cycles != 0 or max_cycles != 0 or last_end != 0:
                diagnostics.append(f"ignored count=0 stat in {source} stats[{index}]")
            continue
        valid_stats.append(stat)
    trace["stats"] = valid_stats
    return trace


def group_key(entry):
    trace = entry["trace"]
    return (
        trace.get("op_type"),
        trace.get("op_name", "Unknown"),
        trace.get("rank_size"),
        trace.get("max_core_count"),
        trace.get("block_dim"),
        trace.get("message_bytes"),
        trace.get("stage_count"),
        trace.get("cycle_to_us_divisor"),
    )


def expected_group_launch_ids(observed_launch_ids, measured_iters, sample_every):
    if not observed_launch_ids or measured_iters <= 0 or sample_every <= 0:
        return observed_launch_ids

    first_launch = min(observed_launch_ids)
    launch_base = (first_launch // measured_iters) * measured_iters
    return [
        launch_id
        for launch_id in range(launch_base, launch_base + measured_iters)
        if launch_id % sample_every == 0
    ]


def as_int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def as_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def cycles_to_us(cycles, divisor):
    if divisor == 0:
        return 0.0
    return float(cycles) / float(divisor)


def bandwidth_gbps(size_bytes, duration_us):
    if size_bytes <= 0 or duration_us <= 0:
        return 0.0
    return round((float(size_bytes) / float(duration_us)) / 1000.0, 6)


def communication_stage_total_bytes(stage, group):
    message_bytes = as_int(group.get("message_bytes"))
    if stage not in TILEXR_COPY_STAGES or message_bytes <= 0:
        return 0
    if stage == "peer_ipc_to_output" and as_int(group.get("op_type")) == TILEXR_OP_TYPE_ALL_GATHER:
        rank_size = max(0, as_int(group.get("rank_size")))
        return message_bytes * max(0, rank_size - 1)
    return message_bytes


def communication_task_size_bytes(stage, group, active_stage_bars):
    stage_total_bytes = communication_stage_total_bytes(stage, group)
    if stage_total_bytes <= 0 or active_stage_bars <= 0:
        return 0
    return int(stage_total_bytes // active_stage_bars)


def communication_transport_type(stage):
    if stage in TILEXR_COPY_STAGES:
        return TILEXR_TRACE_TRANSPORT_MEMORY
    return TILEXR_TRACE_TRANSPORT_UNKNOWN


def communication_metadata_for_bar(bar, group, active_stage_bars):
    stage = bar["stage"]
    rank = bar["rank"]
    src_rank = rank
    dst_rank = TILEXR_TRACE_UNKNOWN_RANK
    if stage == "peer_ipc_to_output":
        src_rank = TILEXR_TRACE_UNKNOWN_RANK
        dst_rank = rank

    size_bytes = communication_task_size_bytes(stage, group, active_stage_bars)
    return {
        "src rank": src_rank,
        "dst rank": dst_rank,
        "transport type": communication_transport_type(stage),
        "size(Byte)": size_bytes,
        "bandwidth(GB/s)": bandwidth_gbps(size_bytes, bar["sum_us"]),
        "communication_metrics_source": TILEXR_TRACE_INFERRED_METRICS_SOURCE,
    }


def annotate_communication_metadata(group):
    active_stage_counts = defaultdict(int)
    for bar in group["bars"]:
        if bar["duration_us"] > 0:
            active_stage_counts[(bar["launch_id"], bar["rank"], bar["stage"])] += 1

    for bar in group["bars"]:
        key = (bar["launch_id"], bar["rank"], bar["stage"])
        bar.update(communication_metadata_for_bar(bar, group, active_stage_counts.get(key, 0)))


def rank_host_info(hosts, rank):
    return hosts.get(rank, {"rank": rank, "host": f"rank{rank}", "ip": "", "comm_mode": "", "source": ""})


def rank_label(hosts, rank):
    info = rank_host_info(hosts, rank)
    host = info.get("host") or f"rank{rank}"
    if host == f"rank{rank}":
        return f"rank{rank}"
    return f"rank{rank}@{host}"


def format_hosts(hosts, rank_ids):
    parts = []
    for rank in rank_ids:
        info = rank_host_info(hosts, rank)
        label = rank_label(hosts, rank)
        ip = info.get("ip") or "unknown-ip"
        parts.append(f"{label}({ip})")
    return ", ".join(parts) if parts else "unknown"


def normalized_bars(entries, root, hosts):
    bars = []
    entries_by_rank_launch = defaultdict(list)
    for entry in entries:
        entries_by_rank_launch[(entry["rank"], entry["launch_id"])].append(entry)

    for (rank, launch_id), rank_entries in sorted(entries_by_rank_launch.items()):
        all_stats = []
        for entry in rank_entries:
            all_stats.extend(entry["trace"].get("stats", []))

        nonzero_starts = [
            as_int(stat.get("first_start_cycle"))
            for stat in all_stats
            if as_int(stat.get("first_start_cycle")) > 0
        ]
        zero_cycle = min(nonzero_starts) if nonzero_starts else 0

        for entry in rank_entries:
            divisor = as_int(entry["trace"].get("cycle_to_us_divisor"))
            source = relpath(entry["path"], root)
            drilldown = relpath(entry["path"].with_name("report.html"), root)
            for stat in entry["trace"].get("stats", []):
                count = as_int(stat.get("count"))
                first = as_int(stat.get("first_start_cycle"))
                last = as_int(stat.get("last_end_cycle"))
                if count == 0 and first == 0 and last == 0:
                    continue

                start_cycles = max(0, first - zero_cycle)
                end_cycles = max(start_cycles, last - zero_cycle)
                duration_cycles = max(0, last - first)
                core = as_int(stat.get("core"))
                stat_rank = as_int(stat.get("rank"), rank)
                host_info = rank_host_info(hosts, stat_rank)
                raw_cycles = as_int(stat.get("raw_cycles"))

                bars.append({
                    "launch_id": launch_id,
                    "rank": stat_rank,
                    "core": core,
                    "stage": str(stat.get("stage", "unknown")),
                    "stage_id": as_int(stat.get("stage_id")),
                    "start_cycles": start_cycles,
                    "end_cycles": end_cycles,
                    "duration_cycles": duration_cycles,
                    "start_us": cycles_to_us(start_cycles, divisor),
                    "end_us": cycles_to_us(end_cycles, divisor),
                    "duration_us": cycles_to_us(duration_cycles, divisor),
                    "sum_us": as_float(stat.get("sum_us"), cycles_to_us(raw_cycles, divisor)),
                    "raw_cycles": raw_cycles,
                    "count": count,
                    "max_cycles": as_int(stat.get("max_cycles")),
                    "host": host_info["host"],
                    "host_ip": host_info["ip"],
                    "source": source,
                    "drilldown": drilldown,
                    "lane": f"{rank_label(hosts, stat_rank)}/core{core}",
                })

    return sorted(bars, key=lambda bar: (
        bar["launch_id"],
        bar["rank"],
        bar["core"],
        bar["start_cycles"],
        bar["stage_id"],
        bar["stage"],
    ))


def summarize_group(group):
    stage_totals = defaultdict(float)
    launch_kernel = defaultdict(float)
    rank_core_max = {"rank": 0, "core": 0, "stage": "", "duration_us": 0.0, "max_cycles": 0, "max_us": 0.0}
    rank_launch_kernel = defaultdict(dict)

    for bar in group["bars"]:
        stage_totals[bar["stage"]] += bar["sum_us"]
        if bar["stage"] == "kernel_total":
            launch_kernel[bar["launch_id"]] = max(launch_kernel[bar["launch_id"]], bar["duration_us"])
            rank_launch_kernel[bar["rank"]][bar["launch_id"]] = max(
                rank_launch_kernel[bar["rank"]].get(bar["launch_id"], 0.0),
                bar["duration_us"],
            )
        max_us = cycles_to_us(bar["max_cycles"], group["cycle_to_us_divisor"])
        if max_us > rank_core_max["max_us"]:
            rank_core_max = {
                "rank": bar["rank"],
                "core": bar["core"],
                "stage": bar["stage"],
                "duration_us": bar["duration_us"],
                "max_cycles": bar["max_cycles"],
                "max_us": max_us,
            }

    top_stage = max(stage_totals.items(), key=lambda item: item[1]) if stage_totals else ("none", 0.0)
    slowest_launch = max(launch_kernel.items(), key=lambda item: item[1]) if launch_kernel else (None, 0.0)
    rank_kernel = summarize_rank_kernel(rank_launch_kernel)
    slowest_rank = rank_kernel[0] if rank_kernel else {
        "rank": None,
        "launch_count": 0,
        "avg_kernel_us": 0.0,
        "max_kernel_us": 0.0,
        "slowest_launch_id": None,
    }

    return {
        "top_stage": {"stage": top_stage[0], "sum_us": top_stage[1]},
        "slowest_launch": {"launch_id": slowest_launch[0], "kernel_us": slowest_launch[1]},
        "rank_core_max": rank_core_max,
        "rank_kernel": rank_kernel,
        "slowest_rank": slowest_rank,
    }


def summarize_rank_kernel(rank_launch_kernel):
    summaries = []
    for rank, launch_values in sorted(rank_launch_kernel.items()):
        if not launch_values:
            continue
        values = list(launch_values.values())
        slowest_launch_id, max_kernel_us = max(
            launch_values.items(),
            key=lambda item: (item[1], -item[0]),
        )
        summaries.append({
            "rank": rank,
            "launch_count": len(values),
            "avg_kernel_us": sum(values) / len(values),
            "max_kernel_us": max_kernel_us,
            "slowest_launch_id": slowest_launch_id,
        })
    return sorted(
        summaries,
        key=lambda item: (-item["avg_kernel_us"], -item["max_kernel_us"], item["rank"]),
    )


def build_index(root, args):
    traces, diagnostics = load_traces(root)
    hosts = load_host_infos(root, diagnostics)
    grouped = defaultdict(list)
    for entry in traces:
        grouped[group_key(entry)].append(entry)

    add_incompatible_group_diagnostics(grouped, root, diagnostics)
    groups = []

    def sort_key(item):
        key = item[0]
        return (
            key[1] or "",
            as_int(key[2]),
            as_int(key[3]),
            as_int(key[4]),
            as_int(key[5]),
            as_int(key[6]),
            as_int(key[7]),
            as_int(key[0]),
        )

    for key, entries in sorted(grouped.items(), key=sort_key):
        op_type, op_name, rank_size, max_core_count, block_dim, message_bytes, stage_count, divisor = key
        rank_ids = sorted({entry["rank"] for entry in entries})
        launch_ids = sorted({entry["launch_id"] for entry in entries})
        effective_rank_size = as_int(rank_size, len(rank_ids))
        expected_ranks = list(range(effective_rank_size))
        expected_launches = expected_group_launch_ids(launch_ids, args.iters, args.profile_sample_every)

        for launch_id in expected_launches:
            for rank in expected_ranks:
                if not any(entry["rank"] == rank and entry["launch_id"] == launch_id for entry in entries):
                    diagnostics.append(f"missing trace for rank{rank} launch{launch_id}")

        group = {
            "op_type": op_type,
            "op_name": op_name,
            "rank_size": effective_rank_size,
            "max_core_count": as_int(max_core_count),
            "block_dim": as_int(block_dim),
            "message_bytes": as_int(message_bytes),
            "stage_count": as_int(stage_count),
            "cycle_to_us_divisor": as_int(divisor),
            "rank_ids": rank_ids,
            "launch_ids": launch_ids,
            "trace_statuses": trace_statuses(entries, root),
            "sources": [
                relpath(entry["path"], root)
                for entry in sorted(entries, key=lambda item: (item["launch_id"], item["rank"], relpath(item["path"], root)))
            ],
            "bars": normalized_bars(entries, root, hosts),
        }
        annotate_communication_metadata(group)
        group["summary"] = summarize_group(group)
        groups.append(group)

    return {
        "schema": RUN_SCHEMA,
        "profile_dir": str(root),
        "warmup_iters": args.warmup_iters,
        "measured_iters": args.iters,
        "profile_sample_every": args.profile_sample_every,
        "hosts": public_hosts(hosts),
        "groups": groups,
        "diagnostics": diagnostics,
    }


def add_incompatible_group_diagnostics(grouped, root, diagnostics):
    keys_by_launch = defaultdict(set)
    for key, entries in grouped.items():
        for entry in entries:
            keys_by_launch[entry["launch_id"]].add(key)

    for launch_id, keys in sorted(keys_by_launch.items()):
        if len(keys) <= 1:
            continue
        group_details = [
            describe_group(key, grouped[key], root)
            for key in sorted(keys, key=lambda item: (
                str(item[1]),
                as_int(item[2]),
                as_int(item[3]),
                as_int(item[4]),
                as_int(item[5]),
                as_int(item[6]),
                as_int(item[7]),
                as_int(item[0]),
            ))
        ]
        diagnostics.append(
            f"incompatible trace groups detected for launch{launch_id}: " + "; ".join(group_details)
        )


def describe_group(key, entries, root):
    op_type, op_name, rank_size, max_core_count, block_dim, message_bytes, stage_count, divisor = key
    sources = ", ".join(
        relpath(entry["path"], root)
        for entry in sorted(entries, key=lambda item: (item["launch_id"], item["rank"], relpath(item["path"], root)))
    )
    return (
        f"op_type={op_type} op_name={op_name} rank_size={rank_size} "
        f"max_core_count={max_core_count} block_dim={block_dim} "
        f"message_bytes={message_bytes} stage_count={stage_count} "
        f"cycle_to_us_divisor={divisor} sources=[{sources}]"
    )


def trace_statuses(entries, root):
    statuses = []
    for entry in sorted(entries, key=lambda item: (item["launch_id"], item["rank"], relpath(item["path"], root))):
        trace = entry["trace"]
        statuses.append({
            "rank": entry["rank"],
            "launch_id": entry["launch_id"],
            "source": relpath(entry["path"], root),
            "incomplete": bool(trace.get("incomplete")),
            "reason": str(trace.get("incomplete_reason") or ""),
            "synthetic": bool(trace.get("synthetic")),
        })
    return statuses


def format_launches(launch_ids):
    if not launch_ids:
        return "none"
    return ", ".join(f"launch{launch_id}" for launch_id in launch_ids)


def format_slowest_launch(slowest):
    if slowest["launch_id"] is None:
        return "unavailable"
    return f"launch{slowest['launch_id']} at {slowest['kernel_us']:.3f} us"


def public_hosts(hosts):
    return {rank: dict(info) for rank, info in sorted(hosts.items())}


def render_analysis(index):
    lines = [
        "# TileXR Collective Profile Run Analysis",
        "",
        f"- Warmup iterations: {index['warmup_iters']}",
        f"- Measured iterations: {index['measured_iters']}",
        f"- Profile sample every: {index['profile_sample_every']}",
        "- Note: cross-NPU raw cycle offsets are not assumed to be synchronized.",
        "",
    ]

    if index["diagnostics"]:
        lines.append("## Diagnostics")
        lines.extend(f"- {item}" for item in index["diagnostics"])
        lines.append("")

    if not index["groups"]:
        lines.append("No valid trace groups were found.")
        lines.append("")
        return "\n".join(lines)

    for group_index, group in enumerate(index["groups"]):
        summary = group["summary"]
        slowest = summary["slowest_launch"]
        rank_core = summary["rank_core_max"]
        stage_totals = defaultdict(float)
        for bar in group["bars"]:
            stage_totals[bar["stage"]] += bar["sum_us"]
        stage_summary = ", ".join(
            f"{stage}={total:.3f} us"
            for stage, total in sorted(stage_totals.items(), key=lambda item: (-item[1], item[0]))
        )
        lines.append(f"## Group {group_index}: {group['op_name']} message_bytes={group['message_bytes']}")
        lines.append(f"- Launches: {format_launches(group['launch_ids'])}")
        lines.append(f"- Hosts: {format_hosts(index.get('hosts', {}), group['rank_ids'])}")
        lines.append(f"- Slowest launch: {format_slowest_launch(slowest)}")
        lines.append(f"- Top stage: {summary['top_stage']['stage']} at {summary['top_stage']['sum_us']:.3f} us")
        lines.append(f"- Stage totals: {stage_summary}")
        lines.append(f"- Slowest rank: {format_slowest_rank(summary['slowest_rank'], index.get('hosts', {}))}")
        lines.append(f"- Rank kernel totals: {format_rank_kernel(summary['rank_kernel'], index.get('hosts', {}))}")
        lines.append(
            f"- Rank/core max: rank{rank_core['rank']} core{rank_core['core']} "
            f"{rank_core['stage']} max {rank_core['max_us']:.3f} us"
        )
        lines.append("")

    return "\n".join(lines)


def format_slowest_rank(slowest_rank, hosts=None):
    rank = slowest_rank.get("rank")
    if rank is None:
        return "unavailable"
    launch_id = slowest_rank.get("slowest_launch_id")
    launch_text = "unknown launch" if launch_id is None else f"launch{launch_id}"
    label = rank_label(hosts or {}, rank)
    return (
        f"{label} avg {slowest_rank.get('avg_kernel_us', 0.0):.3f} us "
        f"max {slowest_rank.get('max_kernel_us', 0.0):.3f} us at {launch_text}"
    )


def format_rank_kernel(rank_kernel, hosts=None):
    if not rank_kernel:
        return "unavailable"
    return "; ".join(
        f"{rank_label(hosts or {}, item['rank'])} avg={item['avg_kernel_us']:.3f} us max={item['max_kernel_us']:.3f} us"
        for item in rank_kernel
    )


def render_ai_prompt(index):
    lines = [
        "# TileXR collective profiling run",
        "",
        "Analyze this TileXR collective profiling run and suggest bottleneck investigation steps.",
        "",
        f"warmup iterations: {index['warmup_iters']}",
        f"measured iterations: {index['measured_iters']}",
        f"profile sample every: {index['profile_sample_every']}",
        "cross-NPU raw cycle offsets are not assumed to be synchronized",
        "",
    ]

    for group in index["groups"]:
        summary = group["summary"]
        slowest = summary["slowest_launch"]
        lines.append(f"- {group['op_name']} bytes={group['message_bytes']} launches={group['launch_ids']}")
        if slowest["launch_id"] is None:
            lines.append("  slowest_launch=unavailable")
        else:
            lines.append(f"  slowest_launch=launch{slowest['launch_id']} kernel_us={slowest['kernel_us']:.3f}")
        lines.append(f"  top_stage={summary['top_stage']['stage']} sum_us={summary['top_stage']['sum_us']:.3f}")
        lines.append(f"  hosts={format_hosts(index.get('hosts', {}), group['rank_ids'])}")
        lines.append(f"  slowest_rank={format_slowest_rank(summary['slowest_rank'], index.get('hosts', {}))}")
        lines.append(f"  rank_kernel_totals={format_rank_kernel(summary['rank_kernel'], index.get('hosts', {}))}")

    if index["diagnostics"]:
        lines.append("")
        lines.append("diagnostics:")
        lines.extend(f"- {item}" for item in index["diagnostics"])

    return "\n".join(lines) + "\n"


def render_perfetto_trace(index):
    events = []
    seen_ranks = set()
    seen_threads = set()
    seen_launch_threads = set()
    launch_offsets = compute_perfetto_launch_offsets(index)

    for group in index["groups"]:
        for rank in group["rank_ids"]:
            if rank not in seen_ranks:
                seen_ranks.add(rank)
                events.append({
                    "name": "process_name",
                    "ph": "M",
                    "pid": rank,
                    "args": {"name": rank_label(index.get("hosts", {}), rank)},
                })

        for launch_id in group["launch_ids"]:
            launch_rank_bars = defaultdict(list)
            for bar in group["bars"]:
                if bar["launch_id"] == launch_id:
                    launch_rank_bars[bar["rank"]].append(bar)
            for rank, rank_bars in sorted(launch_rank_bars.items()):
                if rank not in seen_launch_threads:
                    seen_launch_threads.add(rank)
                    events.append({
                        "name": "thread_name",
                        "ph": "M",
                        "pid": rank,
                        "tid": PERFETTO_LAUNCH_WINDOW_TID,
                        "args": {"name": f"{rank_label(index.get('hosts', {}), rank)}/launch_windows"},
                    })
                max_end_us = max((bar["end_us"] for bar in rank_bars), default=0.0)
                if max_end_us <= 0:
                    continue
                launch_offset_us = launch_offsets.get(launch_id, 0.0)
                events.append({
                    "name": f"launch{launch_id}/{rank_label(index.get('hosts', {}), rank)}/window",
                    "cat": "launch_window",
                    "ph": "X",
                    "pid": rank,
                    "tid": PERFETTO_LAUNCH_WINDOW_TID,
                    "ts": launch_offset_us,
                    "dur": max_end_us,
                    "args": {
                        "launch_id": launch_id,
                        "rank": rank,
                        "host": rank_host_info(index.get("hosts", {}), rank).get("host", ""),
                        "host_ip": rank_host_info(index.get("hosts", {}), rank).get("ip", ""),
                        "launch_offset_us": launch_offset_us,
                    },
                })

        for status in group.get("trace_statuses", []):
            if not status.get("incomplete"):
                continue
            rank = as_int(status.get("rank"))
            launch_id = as_int(status.get("launch_id"))
            if rank not in seen_launch_threads:
                seen_launch_threads.add(rank)
                events.append({
                    "name": "thread_name",
                    "ph": "M",
                    "pid": rank,
                    "tid": PERFETTO_LAUNCH_WINDOW_TID,
                    "args": {"name": f"{rank_label(index.get('hosts', {}), rank)}/launch_windows"},
                })
            rank_status_label = rank_label(index.get("hosts", {}), rank)
            events.append({
                "name": f"launch{launch_id}/{rank_status_label}/incomplete_trace",
                "cat": "trace_status",
                "ph": "X",
                "pid": rank,
                "tid": PERFETTO_LAUNCH_WINDOW_TID,
                "ts": launch_offsets.get(launch_id, 0.0),
                "dur": 1.0,
                "args": {
                    "launch_id": launch_id,
                    "rank": rank,
                    "rank_label": rank_status_label,
                    "reason": str(status.get("reason") or ""),
                    "source": status.get("source", ""),
                    "op_name": group["op_name"],
                    "message_bytes": group["message_bytes"],
                    "rank_size": group["rank_size"],
                    "block_dim": group.get("block_dim", 0),
                    "max_core_count": group.get("max_core_count", 0),
                },
            })

        for bar in group["bars"]:
            thread_key = (bar["rank"], bar["core"])
            if thread_key not in seen_threads:
                seen_threads.add(thread_key)
                events.append({
                    "name": "thread_name",
                    "ph": "M",
                    "pid": bar["rank"],
                    "tid": bar["core"],
                    "args": {"name": f"{rank_label(index.get('hosts', {}), bar['rank'])}/core{bar['core']}"},
                })

            if bar["duration_us"] <= 0:
                continue

            launch_offset_us = launch_offsets.get(bar["launch_id"], 0.0)
            bar_rank_label = rank_label(index.get("hosts", {}), bar["rank"])
            events.append({
                "name": f"launch{bar['launch_id']}/{bar_rank_label}/{bar['stage']}",
                "cat": group["op_name"],
                "ph": "X",
                "pid": bar["rank"],
                "tid": bar["core"],
                "ts": launch_offset_us + bar["start_us"],
                "dur": bar["duration_us"],
                "args": {
                    "launch_id": bar["launch_id"],
                    "launch_offset_us": launch_offset_us,
                    "normalized_ts": bar["start_us"],
                    "rank": bar["rank"],
                    "rank_label": bar_rank_label,
                    "host": bar.get("host", ""),
                    "host_ip": bar.get("host_ip", ""),
                    "core": bar["core"],
                    "stage": bar["stage"],
                    "stage_id": bar["stage_id"],
                    "sum_us": bar["sum_us"],
                    "raw_cycles": bar["raw_cycles"],
                    "max_cycles": bar["max_cycles"],
                    "count": bar["count"],
                    "source": bar["source"],
                    "op_name": group["op_name"],
                    "message_bytes": group["message_bytes"],
                    "rank_size": group["rank_size"],
                    "src rank": bar["src rank"],
                    "dst rank": bar["dst rank"],
                    "transport type": bar["transport type"],
                    "size(Byte)": bar["size(Byte)"],
                    "bandwidth(GB/s)": bar["bandwidth(GB/s)"],
                    "communication_metrics_source": bar["communication_metrics_source"],
                },
            })

    return {
        "displayTimeUnit": "us",
        "traceEvents": events,
    }


def compute_perfetto_launch_offsets(index):
    max_end_by_launch = defaultdict(float)
    for group in index["groups"]:
        for launch_id in group.get("launch_ids", []):
            parsed_launch_id = as_int(launch_id, None)
            if parsed_launch_id is not None:
                max_end_by_launch[parsed_launch_id] = max(max_end_by_launch[parsed_launch_id], 0.0)
        for status in group.get("trace_statuses", []):
            parsed_launch_id = as_int(status.get("launch_id"), None)
            if parsed_launch_id is not None:
                max_end_by_launch[parsed_launch_id] = max(max_end_by_launch[parsed_launch_id], 0.0)
        for bar in group["bars"]:
            max_end_by_launch[bar["launch_id"]] = max(max_end_by_launch[bar["launch_id"]], bar["end_us"])

    offsets = {}
    cursor = 0.0
    for launch_id in sorted(max_end_by_launch):
        offsets[launch_id] = cursor
        cursor += max_end_by_launch[launch_id] + PERFETTO_LAUNCH_GAP_US
    return offsets


def json_for_script(value):
    return (
        json.dumps(value, separators=(",", ":"), ensure_ascii=False)
        .replace("&", "\\u0026")
        .replace("<", "\\u003c")
        .replace(">", "\\u003e")
        .replace("\u2028", "\\u2028")
        .replace("\u2029", "\\u2029")
    )


def render_html(index):
    data = json_for_script(index)
    summary_items = []
    rank_summary_rows = []
    clock_rows = []
    trace_status_rows = []
    fallback_rows = []

    for rank, host_info in sorted(index.get("hosts", {}).items()):
        clock_rows.append(
            "<tr>"
            f"<td>rank{rank}</td>"
            f"<td>{html.escape(str(host_info.get('host', '')))}</td>"
            f"<td>{html.escape(str(host_info.get('ip', '')))}</td>"
            f"<td>{html.escape(str(host_info.get('clock_offset_ns', '')))}</td>"
            f"<td>{html.escape(str(host_info.get('clock_reference', '')))}</td>"
            f"<td>{html.escape(str(host_info.get('epoch_ns', '')))}</td>"
            "</tr>"
        )

    for group in index["groups"]:
        summary = group["summary"]
        slowest = summary["slowest_launch"]
        rank_core = summary["rank_core_max"]
        summary_items.append(
            "<li>"
            f"{html.escape(str(group['op_name']))} bytes={group['message_bytes']}: "
            f"Slowest launch {html.escape(format_slowest_launch(slowest))}; "
            f"Hosts {html.escape(format_hosts(index.get('hosts', {}), group['rank_ids']))}; "
            f"Slowest rank {html.escape(format_slowest_rank(summary['slowest_rank'], index.get('hosts', {})))}; "
            f"Top stage {html.escape(str(summary['top_stage']['stage']))} {summary['top_stage']['sum_us']:.3f} us; "
            f"Rank/core max rank{rank_core['rank']} core{rank_core['core']} "
            f"{html.escape(str(rank_core['stage']))} max {rank_core['max_us']:.3f} us"
            "</li>"
        )

        for rank_item in summary["rank_kernel"]:
            launch_id = rank_item["slowest_launch_id"]
            drilldown = find_rank_launch_drilldown(group["bars"], rank_item["rank"], launch_id)
            rank_summary_rows.append(
                "<tr>"
                f"<td>{html.escape(str(group['op_name']))}</td>"
                f"<td>{group['message_bytes']}</td>"
                f"<td>{html.escape(rank_label(index.get('hosts', {}), rank_item['rank']))}</td>"
                f"<td>{html.escape(rank_host_info(index.get('hosts', {}), rank_item['rank']).get('ip', ''))}</td>"
                f"<td>{rank_item['launch_count']}</td>"
                f"<td>{rank_item['avg_kernel_us']:.3f}</td>"
                f"<td>{rank_item['max_kernel_us']:.3f}</td>"
                f"<td>launch{launch_id}</td>"
                f"<td><a href=\"{html.escape(drilldown)}\">open launch report</a></td>"
                "</tr>"
            )

        for status in group.get("trace_statuses", []):
            if not status.get("incomplete") and not status.get("synthetic"):
                continue
            status_text = "incomplete" if status.get("incomplete") else "synthetic"
            trace_status_rows.append(
                "<tr>"
                f"<td>{html.escape(str(group['op_name']))}</td>"
                f"<td>{group['message_bytes']}</td>"
                f"<td>launch{status.get('launch_id')}</td>"
                f"<td>{html.escape(rank_label(index.get('hosts', {}), as_int(status.get('rank'))))}</td>"
                f"<td>{html.escape(status_text)}</td>"
                f"<td>{html.escape(str(status.get('reason') or ''))}</td>"
                f"<td>{html.escape(str(status.get('source') or ''))}</td>"
                "</tr>"
            )

        for bar in group["bars"]:
            fallback_rows.append(
                "<tr>"
                f"<td>launch{bar['launch_id']}</td>"
                f"<td>{html.escape(rank_label(index.get('hosts', {}), bar['rank']))}</td>"
                f"<td>{html.escape(str(bar.get('host_ip', '')))}</td>"
                f"<td>core{bar['core']}</td>"
                f"<td>{html.escape(str(bar['stage']))}</td>"
                f"<td>{bar['duration_us']:.3f}</td>"
                f"<td>{bar['sum_us']:.3f}</td>"
                f"<td><a href=\"{html.escape(bar['drilldown'])}\">open launch report</a></td>"
                "</tr>"
            )

    if not summary_items:
        summary_items.append("<li>No valid trace groups were found.</li>")

    diagnostics = "".join(f"<li>{html.escape(item)}</li>" for item in index["diagnostics"])
    summary_html = "".join(summary_items) + diagnostics
    clock_html = "".join(clock_rows) or "<tr><td colspan=\"6\">No host clock metadata available.</td></tr>"
    rank_summary_html = "".join(rank_summary_rows) or "<tr><td colspan=\"9\">No rank kernel totals available.</td></tr>"
    trace_status_html = "".join(trace_status_rows) or "<tr><td colspan=\"7\">No incomplete or synthetic traces.</td></tr>"
    fallback_html = "".join(fallback_rows) or "<tr><td colspan=\"8\">No trace bars available.</td></tr>"

    stage_filter_html = "\n".join(
        f"<label><input type=\"checkbox\" checked onchange=\"toggleStage('{name}', this.checked)\"> {name}</label>"
        for name in ("wait", "copy", "sync", "total")
    )

    return f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>TileXR Collective Profile Run Report</title>
<style>
body{{font-family:Arial,sans-serif;margin:24px;color:#172033;background:#f8fafc}}
button{{margin-right:8px;padding:6px 10px;border:1px solid #94a3b8;background:#fff;border-radius:4px;cursor:pointer}}
.panel{{background:#fff;border:1px solid #cbd5e1;border-radius:6px;padding:16px;margin:16px 0}}
.timeline-wrap{{overflow:auto;border:1px solid #cbd5e1;background:#fff;height:520px;position:relative}}
.timeline{{position:relative;height:100%;min-width:100%;transform-origin:0 0}}
.bar{{position:absolute;height:16px;border-radius:3px;color:#0f172a;font-size:11px;overflow:hidden;white-space:nowrap;border:1px solid rgba(15,23,42,.25);text-decoration:none}}
.stage-total{{background:#cbd5e1}} .stage-wait{{background:#fca5a5}} .stage-copy{{background:#93c5fd}} .stage-sync{{background:#a7f3d0}}
.launch-line{{position:absolute;top:0;bottom:0;border-left:1px dashed #94a3b8;color:#475569;font-size:12px;padding-left:4px;text-decoration:none}}
table{{border-collapse:collapse;background:#fff;width:100%}} td,th{{border:1px solid #cbd5e1;padding:6px 8px}} th{{background:#e2e8f0;text-align:left}}
</style>
</head>
<body>
<h1>TileXR Collective Profile Run Report</h1>
<section class="panel">
<h2>Bottleneck First</h2>
<p>Warmup iterations: {index['warmup_iters']}; measured iterations: {index['measured_iters']}; sample every: {index['profile_sample_every']}.</p>
<ul>{summary_html}</ul>
</section>
<section class="panel">
<h2>Clock Sync</h2>
<p>Offsets are sampled by the multi-host runner before launch. Positive offset means that rank host clock is ahead of the rank0 reference clock.</p>
<table><thead><tr><th>Rank</th><th>Host</th><th>Host IP</th><th>Offset ns vs rank0</th><th>Reference</th><th>Rank epoch ns</th></tr></thead><tbody>{clock_html}</tbody></table>
</section>
<section class="panel">
<h2>Rank-Level Summary</h2>
<p>Kernel totals are grouped by rank using the slowest core per rank/launch. This is the fastest view for spotting a slow rank before drilling into core-level stages.</p>
<table><thead><tr><th>Op</th><th>Bytes</th><th>Rank@Host</th><th>Host IP</th><th>Launches</th><th>Avg kernel us</th><th>Max kernel us</th><th>Slowest launch</th><th>Drilldown</th></tr></thead><tbody>{rank_summary_html}</tbody></table>
</section>
<section class="panel">
<h2>Trace Status</h2>
<p>Incomplete traces keep the same single-launch report schema but indicate that kernel execution failed before device stats could be copied back.</p>
<table><thead><tr><th>Op</th><th>Bytes</th><th>Launch</th><th>Rank@Host</th><th>Status</th><th>Reason</th><th>Source</th></tr></thead><tbody>{trace_status_html}</tbody></table>
</section>
<section class="panel">
<h2>Chronological Timeline</h2>
<p>Each rank/launch lane is normalized independently; cross-NPU raw cycle offsets are not assumed to be synchronized.</p>
<div>
<button onclick="zoomBy(1.25)">Zoom In</button>
<button onclick="zoomBy(0.8)">Zoom Out</button>
<button onclick="fitTimeline()">Fit</button>
<button onclick="toggleLaneMode()">Fold Cores</button>
<label>Launch <select id="launchFilter" onchange="renderTimeline()"><option value="all">all</option></select></label>
{stage_filter_html}
</div>
<div class="timeline-wrap" id="wrap"><div class="timeline" id="timeline"></div></div>
</section>
<section class="panel">
<h2>Fallback Table</h2>
<table><thead><tr><th>Launch</th><th>Rank@Host</th><th>Host IP</th><th>Core</th><th>Stage</th><th>Duration us</th><th>Sum us</th><th>Drilldown</th></tr></thead><tbody>{fallback_html}</tbody></table>
</section>
<script>
const traceIndex = {data};
let scale = 1;
let foldCores = false;
const stageVisibility = {{wait: true, copy: true, sync: true, total: true}};
function category(stage) {{
  if (stage.includes('wait') || stage.includes('poll')) return 'wait';
  if (stage.includes('copy') || stage.includes('ipc')) return 'copy';
  if (stage.includes('sync') || stage.includes('barrier')) return 'sync';
  return 'total';
}}
function selectedLaunchId() {{
  const filter = document.getElementById('launchFilter');
  return filter ? filter.value : 'all';
}}
function visibleLaunches() {{
  const selectedLaunch = selectedLaunchId();
  const launches = [];
  for (const group of traceIndex.groups) {{
    for (const launchId of group.launch_ids) {{
      if (selectedLaunch !== 'all' && selectedLaunch !== String(launchId)) continue;
      const launchBars = group.bars.filter(bar => bar.launch_id === launchId);
      launches.push({{group, launchId, launchBars}});
    }}
  }}
  return launches;
}}
function timelineWidthAt(nextScale) {{
  const launchGap = 80;
  let xBase = 0;
  for (const launch of visibleLaunches()) {{
    const maxEnd = Math.max(1, ...launch.launchBars.map(bar => bar.end_us));
    xBase += Math.max(220, maxEnd * nextScale * 4) + launchGap;
  }}
  return xBase;
}}
function launchDrilldown(launchBars) {{
  const preferred = launchBars.find(bar => bar.stage === 'kernel_total') || launchBars[0];
  return preferred ? preferred.drilldown : '#';
}}
function renderTimeline() {{
  const root = document.getElementById('timeline');
  const wrap = document.getElementById('wrap');
  root.innerHTML = '';
  refreshLaunchFilter();
  const laneHeight = 28;
  const launchGap = 80;
  let xBase = 0;
  const lanes = new Map();
  let laneCount = 0;
  for (const launch of visibleLaunches()) {{
    const launchId = launch.launchId;
    const launchBars = launch.launchBars;
    const maxEnd = Math.max(1, ...launchBars.map(bar => bar.end_us));
    const width = Math.max(220, maxEnd * scale * 4);
    const line = document.createElement('a');
    line.href = launchDrilldown(launchBars);
    line.className = 'launch-line';
    line.style.left = `${{xBase}}px`;
    line.textContent = `launch${{launchId}}`;
    root.appendChild(line);
    for (const bar of launchBars) {{
      const rankLabel = bar.host && bar.host !== `rank${{bar.rank}}` ? `rank${{bar.rank}}@${{bar.host}}` : `rank${{bar.rank}}`;
      const lane = foldCores ? rankLabel : `${{rankLabel}}/core${{bar.core}}`;
      if (!lanes.has(lane)) lanes.set(lane, laneCount++);
      const cat = category(bar.stage);
      const item = document.createElement('a');
      item.href = bar.drilldown;
      item.className = `bar stage-${{cat}}`;
      item.dataset.stageCategory = cat;
      item.style.display = stageVisibility[cat] ? '' : 'none';
      item.style.left = `${{xBase + bar.start_us * scale * 4}}px`;
      item.style.top = `${{24 + lanes.get(lane) * laneHeight}}px`;
      item.style.width = `${{Math.max(2, (bar.end_us - bar.start_us) * scale * 4)}}px`;
      item.title = `launch${{bar.launch_id}} ${{lane}} ${{bar.stage}} start=${{bar.start_us.toFixed(3)}}us end=${{bar.end_us.toFixed(3)}}us duration=${{bar.duration_us.toFixed(3)}}us sum=${{bar.sum_us.toFixed(3)}}us max_cycles=${{bar.max_cycles}} count=${{bar.count}} source=${{bar.source}}`;
      item.textContent = `${{lane}} ${{bar.stage}}`;
      root.appendChild(item);
    }}
    xBase += width + launchGap;
  }}
  root.style.width = `${{Math.max(wrap.clientWidth, xBase)}}px`;
  root.style.height = `${{Math.max(520, 60 + laneCount * laneHeight)}}px`;
}}
function zoomBy(factor) {{ scale = Math.max(0.1, Math.min(20, scale * factor)); renderTimeline(); }}
function fitTimeline() {{
  const wrap = document.getElementById('wrap');
  const target = Math.max(320, wrap.clientWidth - 16);
  let low = 0.1;
  let high = 20;
  if (timelineWidthAt(low) > target) {{
    scale = low;
  }} else {{
    for (let i = 0; i < 24; i++) {{
      const mid = (low + high) / 2;
      if (timelineWidthAt(mid) <= target) low = mid; else high = mid;
    }}
    scale = low;
  }}
  renderTimeline();
  wrap.scrollLeft = 0;
}}
function toggleLaneMode() {{ foldCores = !foldCores; renderTimeline(); }}
function refreshLaunchFilter() {{
  const select = document.getElementById('launchFilter');
  const current = select.value || 'all';
  const launches = new Set();
  for (const group of traceIndex.groups) for (const launchId of group.launch_ids) launches.add(String(launchId));
  select.innerHTML = '<option value="all">all</option>' + Array.from(launches).sort((a, b) => Number(a) - Number(b)).map(id => `<option value="${{id}}">launch${{id}}</option>`).join('');
  select.value = launches.has(current) ? current : 'all';
}}
function toggleStage(categoryName, visible) {{
  stageVisibility[categoryName] = visible;
  for (const item of document.querySelectorAll(`[data-stage-category="${{categoryName}}"]`)) {{
    item.style.display = visible ? '' : 'none';
  }}
}}
renderTimeline();
</script>
</body>
</html>
"""


def find_rank_launch_drilldown(bars, rank, launch_id):
    for bar in bars:
        if bar["rank"] == rank and bar["launch_id"] == launch_id and bar["stage"] == "kernel_total":
            return bar["drilldown"]
    for bar in bars:
        if bar["rank"] == rank and bar["launch_id"] == launch_id:
            return bar["drilldown"]
    return "#"


def write_outputs(root, index, emit_ai_prompt):
    (root / "trace_index.json").write_text(json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    perfetto_json = json.dumps(render_perfetto_trace(index), indent=2, sort_keys=True) + "\n"
    (root / "trace.json").write_text(perfetto_json, encoding="utf-8")
    (root / "perfetto_trace.json").write_text(perfetto_json, encoding="utf-8")
    (root / "analysis.md").write_text(render_analysis(index), encoding="utf-8")
    (root / "report.html").write_text(render_html(index), encoding="utf-8")

    prompt_path = root / "ai_prompt.md"
    if emit_ai_prompt:
        prompt_path.write_text(render_ai_prompt(index), encoding="utf-8")
    elif prompt_path.exists():
        prompt_path.unlink()


def main():
    args = parse_args()
    root = Path(args.profile_dir)
    root.mkdir(parents=True, exist_ok=True)
    index = build_index(root, args)
    write_outputs(root, index, args.emit_ai_prompt)
    print(f"wrote TileXR collective profile report to {root / 'report.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
