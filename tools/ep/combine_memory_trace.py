#!/usr/bin/env python

"""Export EP Combine Memory profile logs as three Chrome Trace files."""

import argparse
import json
import math
import re
from pathlib import Path


PROFILE_PREFIX = "EP_COMBINE_MEMORY_PROFILE"
PROFILE_VERSION = 1
PROFILE_CYCLES_PER_US = 1000
MAX_PROFILE_CORES = 200
PROFILE_CORE_ROLES = ()
DEFAULT_OUTPUT_PREFIX = "ep_combine_memory"
TRACE_KERNEL_NAME = "ep_combine_memory"
TRACE_SCHEMA = "tilexr_ep_combine_memory_trace.v1"
TRACE_OUTPUT_PREFIX = "EP_COMBINE_MEMORY_TRACE"
TRACE_DESCRIPTION = __doc__
SAFE_OUTPUT_COMPONENT = re.compile(r"[A-Za-z0-9_.-]{1,128}")
TIME_POINT_NAMES = (
    "init_begin",
    "static_init_end",
    "reuse_wait_end",
    "send_count_split_end",
    "send_buffer_init_end",
    "assist_load_end",
    "send_issue_end",
    "dispatch_end",
    "receive_buffer_init_end",
    "receive_prepare_end",
    "first_token_ready",
    "all_tokens_ready",
    "local_copy_end",
    "final_barrier_end",
    "retire_begin",
    "retire_end",
    "final_end",
)
METRIC_NAMES = (
    "assist_load",
    "dispatch_input_load",
    "dispatch_pack",
    "dispatch_store_issue",
    "scale_load",
    "flag_prepare",
    "flag_load",
    "flag_compare_scan",
    "payload_load",
    "reduce_issue",
    "flag_clear_issue",
    "output_cast_store_issue",
)
DIAGNOSTIC_NAMES = (
    "send_rows",
    "receive_tokens",
    "flag_poll_attempts",
    "flag_poll_misses",
    "blocks_per_token",
    "dispatch_buffer_num",
    "receive_buffer_num",
)
PROVENANCE_INT_NAMES = (
    "world_size",
    "bs",
    "h",
    "k",
    "experts",
    "quant_mode",
    "comm_quant_mode",
    "active_mask_type",
)


def parse_fields(line):
    fields = {}
    for item in line.strip().split()[1:]:
        if "=" in item:
            key, value = item.split("=", 1)
            fields[key] = value
    return fields


def required_int(fields, name, path, line_number):
    if name not in fields:
        raise ValueError("{}:{} missing {}".format(path, line_number, name))
    try:
        return int(fields[name])
    except ValueError as error:
        raise ValueError(
            "{}:{} invalid {}={}".format(
                path, line_number, name, fields[name])) from error


def required_text(fields, name, path, line_number):
    value = fields.get(name, "")
    if not value:
        raise ValueError("{}:{} missing {}".format(path, line_number, name))
    return value


def safe_output_component(value):
    if SAFE_OUTPUT_COMPONENT.fullmatch(value) is None:
        raise argparse.ArgumentTypeError(
            "value must contain 1-128 letters, digits, dot, underscore, or hyphen")
    return value


def parse_profiles(paths):
    profiles = {}
    for path in paths:
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.startswith(PROFILE_PREFIX):
                    continue
                fields = parse_fields(line)
                version = required_int(fields, "profile_version", path, line_number)
                point_count = required_int(fields, "point_count", path, line_number)
                metric_count = required_int(fields, "metric_count", path, line_number)
                if version != PROFILE_VERSION or point_count != len(TIME_POINT_NAMES) or \
                        metric_count != len(METRIC_NAMES):
                    raise ValueError(
                        "{}:{} unsupported profile schema version={} points={} metrics={}".
                        format(path, line_number, version, point_count, metric_count))
                iteration = required_int(fields, "iteration", path, line_number)
                rank = required_int(fields, "rank", path, line_number)
                core = required_int(fields, "core", path, line_number)
                run_id = required_text(fields, "run_id", path, line_number)
                if re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", run_id) is None:
                    raise ValueError(
                        "{}:{} invalid run_id={}".format(path, line_number, run_id))
                record = {
                    "block_dim": required_int(fields, "block_dim", path, line_number),
                    "generation": required_int(fields, "generation", path, line_number),
                    "cycles_per_us": required_int(fields, "cycles_per_us", path, line_number),
                    "points": tuple(
                        required_int(fields, "t{}".format(index), path, line_number)
                        for index in range(point_count)),
                    "metrics": tuple(
                        required_int(fields, "m{}".format(index), path, line_number)
                        for index in range(metric_count)),
                    "diagnostics": {
                        name: required_int(fields, name, path, line_number)
                        for name in DIAGNOSTIC_NAMES
                    },
                    "provenance": {
                        name: required_int(fields, name, path, line_number)
                        for name in PROVENANCE_INT_NAMES
                    },
                }
                record["provenance"]["dtype"] = required_text(
                    fields, "dtype", path, line_number)
                if PROFILE_CORE_ROLES:
                    record["core_role"] = required_text(
                        fields, "core_role", path, line_number)
                    if record["core_role"] not in PROFILE_CORE_ROLES:
                        raise ValueError(
                            "{}:{} invalid core_role={}".format(
                                path, line_number, record["core_role"]))
                key = (run_id, iteration, rank, core)
                if key in profiles and profiles[key] != record:
                    raise ValueError(
                        "{}:{} conflicting profile record {}".
                        format(path, line_number, key))
                profiles[key] = record
    if not profiles:
        raise ValueError("no {} records found".format(PROFILE_PREFIX))
    return profiles


def select_run_id(profiles, requested):
    run_ids = sorted({key[0] for key in profiles})
    if requested is None:
        if len(run_ids) != 1:
            raise ValueError(
                "multiple run_id values found {}; pass --run-id".format(run_ids))
        return run_ids[0]
    if requested not in run_ids:
        raise ValueError("run_id {} is missing".format(requested))
    return requested


def select_iteration(profiles, run_id, requested):
    iterations = sorted({key[1] for key in profiles if key[0] == run_id})
    if requested is None:
        return iterations[-1]
    if requested not in iterations:
        raise ValueError("iteration {} is missing".format(requested))
    return requested


def validate_iteration(profiles, run_id, iteration, world_size, expected_provenance):
    rank_records = {}
    generations = set()
    available_ranks = {
        key[2] for key in profiles if key[0] == run_id and key[1] == iteration
    }
    expected_ranks = set(range(world_size))
    if available_ranks != expected_ranks:
        raise ValueError(
            "run_id {} iteration {} rank set mismatch missing={} extra={}".format(
                run_id, iteration, sorted(expected_ranks - available_ranks),
                sorted(available_ranks - expected_ranks)))
    for rank in range(world_size):
        records = {
            core: record
            for (record_run_id, record_iteration, record_rank, core), record in profiles.items()
            if record_run_id == run_id and record_iteration == iteration and record_rank == rank
        }
        if not records or 0 not in records:
            raise ValueError("iteration {} rank {} has no core-0 record".format(iteration, rank))
        block_dim = records[0]["block_dim"]
        if block_dim <= 0 or block_dim > MAX_PROFILE_CORES:
            raise ValueError(
                "iteration {} rank {} has invalid block_dim {}".
                format(iteration, rank, block_dim))
        if set(records) != set(range(block_dim)):
            missing = sorted(set(range(max(block_dim, 0))) - set(records))
            raise ValueError(
                "iteration {} rank {} has incomplete cores {}".
                format(iteration, rank, missing))
        generation = records[0]["generation"]
        cycles_per_us = records[0]["cycles_per_us"]
        if generation <= 0 or cycles_per_us != PROFILE_CYCLES_PER_US:
            raise ValueError("rank {} has invalid generation or cycle divisor".format(rank))
        for core, record in records.items():
            if record["block_dim"] != block_dim or record["generation"] != generation or \
                    record["cycles_per_us"] != cycles_per_us:
                raise ValueError("rank {} core {} has inconsistent metadata".format(rank, core))
            if record["provenance"] != expected_provenance:
                raise ValueError(
                    "rank {} core {} provenance mismatch expected={} actual={}".
                    format(rank, core, expected_provenance, record["provenance"]))
            points = record["points"]
            if any(point <= 0 for point in points) or any(
                    points[index] > points[index + 1]
                    for index in range(len(points) - 1)):
                raise ValueError("rank {} core {} has invalid timestamps".format(rank, core))
            if any(metric < 0 for metric in record["metrics"]):
                raise ValueError("rank {} core {} has invalid metrics".format(rank, core))
            if PROFILE_CORE_ROLES:
                alltoall_core_count = records[0]["diagnostics"]["alltoall_core_count"]
                if not 0 <= alltoall_core_count <= block_dim:
                    raise ValueError("rank {} has invalid alltoall core count".format(rank))
                expected_role = PROFILE_CORE_ROLES[0] if core < alltoall_core_count \
                    else PROFILE_CORE_ROLES[1]
                if record["diagnostics"]["alltoall_core_count"] != alltoall_core_count or \
                        record["core_role"] != expected_role:
                    raise ValueError(
                        "rank {} core {} role boundary mismatch".format(rank, core))
        generations.add(generation)
        rank_records[rank] = records
    if len(generations) != 1:
        raise ValueError("iteration {} has inconsistent generations {}".format(
            iteration, sorted(generations)))
    return rank_records, next(iter(generations))


def select_ranks(rank_records):
    elapsed = []
    for rank, records in rank_records.items():
        max_cycles = max(record["points"][-1] - record["points"][0]
                         for record in records.values())
        divisor = records[0]["cycles_per_us"]
        elapsed.append((max_cycles / divisor, rank))
    elapsed.sort(key=lambda item: (item[0], item[1]))
    p50_index = int(math.ceil(0.5 * len(elapsed))) - 1
    return (
        ("fastest", elapsed[0]),
        ("p50", elapsed[p50_index]),
        ("slowest", elapsed[-1]),
    ), elapsed


def metadata_event(name, pid, args, tid=0):
    return {"name": name, "ph": "M", "pid": pid, "tid": tid, "args": args}


def complete_event(name, category, pid, tid, start_us, end_us, args):
    return {
        "name": name,
        "cat": category,
        "ph": "X",
        "pid": pid,
        "tid": tid,
        "ts": round(start_us, 6),
        "dur": round(max(0.0, end_us - start_us), 6),
        "args": args,
    }


def build_trace(role, elapsed_us, rank, records, run_id, iteration, generation,
                rank_order, provenance):
    divisor = records[0]["cycles_per_us"]
    origin = min(record["points"][0] for record in records.values())
    events = [
        metadata_event("process_name", rank, {
            "name": "{} rank {} ({:.6f} us)".format(role, rank, elapsed_us),
        }),
        metadata_event("process_sort_index", rank, {"sort_index": 0}),
    ]
    for core in sorted(records):
        record = records[core]
        points = record["points"]
        tid = core
        events.append(metadata_event("thread_name", rank, {
            "name": "AIV core {}{}".format(
                core,
                " ({})".format(record["core_role"]) if PROFILE_CORE_ROLES else ""),
        }, tid=tid))
        events.append(metadata_event("thread_sort_index", rank, {
            "sort_index": core,
        }, tid=tid))
        for index in range(len(TIME_POINT_NAMES) - 1):
            events.append(complete_event(
                "{} -> {}".format(TIME_POINT_NAMES[index], TIME_POINT_NAMES[index + 1]),
                "phase", rank, tid,
                (points[index] - origin) / divisor,
                (points[index + 1] - origin) / divisor,
                {"start_point": "t{}".format(index), "end_point": "t{}".format(index + 1)}))
        metric_args = {
            "{}_us".format(name): round(record["metrics"][index] / divisor, 6)
            for index, name in enumerate(METRIC_NAMES)
        }
        events.append({
            "name": "Accumulated metrics",
            "cat": "metrics",
            "ph": "i",
            "s": "t",
            "pid": rank,
            "tid": tid,
            "ts": round((points[-1] - origin) / divisor, 6),
            "args": metric_args,
        })
    return {
        "traceEvents": events,
        "otherData": {
            "schema": TRACE_SCHEMA,
            "selection": role,
            "selection_elapsed_us": elapsed_us,
            "rank": rank,
            "run_id": run_id,
            "iteration": iteration,
            "generation": generation,
            "provenance": provenance,
            "selection_rule": (
                "rank duration=max active-core final_end-init_begin; fastest=min; "
                "p50=ceil(0.5*N)-1; slowest=max; ties by rank"),
            "clock_alignment": (
                "selected rank normalized to earliest AIV init_begin; "
                "absolute cycles are not compared across NPUs"),
            "rank_order": [
                {"position": index + 1, "rank": ordered_rank, "elapsed_us": ordered_us}
                for index, (ordered_us, ordered_rank) in enumerate(rank_order)
            ],
            "warning": "profiling perturbs latency and is for attribution only",
        },
    }


def main():
    parser = argparse.ArgumentParser(description=TRACE_DESCRIPTION)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--world-size", type=int, required=True)
    parser.add_argument("--run-id")
    parser.add_argument("--iteration", type=int)
    parser.add_argument("--bs", type=int, required=True)
    parser.add_argument("--hidden-size", type=int, required=True)
    parser.add_argument("--topk", type=int, required=True)
    parser.add_argument("--experts", type=int, required=True)
    parser.add_argument("--dtype", choices=("fp16", "bf16"), required=True)
    parser.add_argument("--quant-mode", type=int, required=True)
    parser.add_argument("--comm-quant-mode", type=int, required=True)
    parser.add_argument("--active-mask-type", type=int, required=True)
    parser.add_argument("--expert-token-nums-type", type=int, default=1)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--prefix", type=safe_output_component, default=DEFAULT_OUTPUT_PREFIX)
    args = parser.parse_args()
    if args.world_size <= 0 or args.bs <= 0 or args.hidden_size <= 0 or \
            args.topk <= 0 or args.experts <= 0:
        parser.error("world size, BS, hidden size, topK, and experts must be positive")
    if args.topk > args.experts:
        parser.error("--topk must not exceed --experts")

    profiles = parse_profiles(args.logs)
    run_id = select_run_id(profiles, args.run_id)
    iteration = select_iteration(profiles, run_id, args.iteration)
    provenance_values = {
        "world_size": args.world_size,
        "bs": args.bs,
        "h": args.hidden_size,
        "k": args.topk,
        "experts": args.experts,
        "quant_mode": args.quant_mode,
        "comm_quant_mode": args.comm_quant_mode,
        "active_mask_type": args.active_mask_type,
        "expert_token_nums_type": args.expert_token_nums_type,
        "dtype": args.dtype,
    }
    provenance = {
        name: provenance_values[name] for name in PROVENANCE_INT_NAMES
    }
    provenance["dtype"] = args.dtype
    rank_records, generation = validate_iteration(
        profiles, run_id, iteration, args.world_size, provenance)
    selected, rank_order = select_ranks(rank_records)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for role, (elapsed_us, rank) in selected:
        output = args.output_dir / (
            "{}_{}_iteration{}_{}_rank{}_trace.json".
            format(args.prefix, run_id, iteration, role, rank))
        trace = build_trace(
            role, elapsed_us, rank, rank_records[rank], run_id, iteration,
            generation, rank_order, provenance)
        with output.open("w", encoding="utf-8") as stream:
            json.dump(trace, stream, indent=2, sort_keys=True)
            stream.write("\n")
        print("{} run_id={} role={} rank={} elapsed_us={:.6f} output={}".
              format(TRACE_OUTPUT_PREFIX, run_id, role, rank, elapsed_us, output))


if __name__ == "__main__":
    main()
