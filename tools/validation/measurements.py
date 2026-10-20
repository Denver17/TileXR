#!/usr/bin/env python3
"""读取单算子测量证据；不启动 profiler、设备进程或通信。"""

import math
import json
import hashlib
import os
import shutil
import tempfile
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ep"))
import msprof_kernel_stats as profiler


# EP supplies stage boundaries; record parsing, analysis and rendering use the shared package.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from profiling import RECORDS_SCHEMA, generate_reports_for_run
from profiling.analysis import analyze_run
from profiling.pairing import load_profile_run
from profiling.io import atomic_write_json
from dataclasses import replace
from uuid import NAMESPACE_URL, UUID, uuid5

KERNEL_NAME = "tilexr_ep_dispatch_memory_server_dedup_kernel"
KERNEL_NAMES = {"dispatch": KERNEL_NAME, "combine": "tilexr_ep_combine_memory_server_dedup_kernel"}
RESULT_SEPARATOR = "=" * 32
PERFORMANCE_SAMPLE_LIMIT = 50
RANKS_PER_SERVER = 8


def render_case_description(report):
    """Describe the effective case from its result, rather than guessed CLI defaults."""
    if report.get("checkpoint") == "ep-sdedup-p1-memory-boundary":
        return (f"MemoryDomain mode=functional cards={report.get('rank_size', 'unknown')} capacity/rank=4 GiB\n"
                "Local boundary DataCopy offset=4294967264 bytes=32 pattern=0xFF")
    config = report.get("chain_configuration")
    if not config:
        return "checkpoint=" + report.get("checkpoint", "unknown")
    operator = config.get("operator_mode", "dispatch")
    title = "Dispatch" if operator == "dispatch" else "Combine" if operator == "combine" else "Dispatch+Combine"
    return (
        f"{title} server-dedup scope={config.get('kernel_scope', 'unknown')} mode={config['run_mode']} "
        f"loops={config['loops']} warmup={config['warmup']}\n"
        f"BS={config['bs']} H={config['h']} TopK={config['topk']} "
        f"experts={config['moe_expert_num']} dtype={config['dtype']} "
        f"cards={report.get('rank_size', 'unknown')} quant_mode={config.get('quant_mode', 0)} fp8_format={config.get('fp8_format', 'N/A')} "
        f"routes={config.get('route_mode', 'unknown')} seed={config.get('seed', 'unknown')}"
    )


PROFILING_RECORDS_PER_LAUNCH = 65536
PROFILE_ARTIFACTS = ("perfetto_trace.json", "analysis.md", "report.html")
PROFILE_SUMMARY_SCHEMA = "tilexr_profiling_rank_summary.v1"


def capture_capacity(loops, warmup):
    _integer(loops, "loops", 1)
    _integer(warmup, "warmup")
    return PROFILING_RECORDS_PER_LAUNCH


def profiling_mapping(mapping_path=None, *, operator="dispatch"):
    kernel_name(operator)
    if mapping_path is not None:
        return Path(mapping_path)
    module = Path(__file__).resolve()
    filename = "server_dedup.mapping.json" if operator == "dispatch" else "server_dedup_combine.mapping.json"
    candidates = (module.parents[2] / "src/ep/profiling" / filename,
                  module.parents[2] / "profiling" / filename)
    for path in candidates:
        if path.is_file():
            return path
    raise ValueError("missing installed Server Dedup profiling mapping")


def kernel_name(operator):
    if operator not in KERNEL_NAMES:
        raise ValueError("operator must be dispatch/combine")
    return KERNEL_NAMES[operator]


def _selection(config):
    capture_capacity(config["loops"], config["warmup"])
    return dict(launch_count=1, expected_launch_count=1)


def _file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def breakdown_timeline(run):
    """Turn EP boundary observations into the shared framework's interval samples."""
    # Keep stage IDs stable across send/receive lanes, including optional branches.
    spans = (
        ("init", "start", "init_end", "setup", "kernel_total"),
        ("send_cnt", "init_end", "send_cnt_end", "issue", "kernel_total"),
        ("send_data", "send_cnt_end", "send_data_end", "issue", "kernel_total"),
        ("recv_cnt", "send_cnt_end", "recv_cnt_end", "wait", "kernel_total"),
        ("cumsum", "recv_cnt_end", "cumsum_end", "compute", "kernel_total"),
        ("prefix_consume", "cumsum_end", "prefix_end", "copy", "kernel_total"),
        ("recv_data", "prefix_end", "recv_data_end", "copy", "kernel_total"),
        ("finish", "work_end", "end", "finalize", "kernel_total"),
        ("recv_enter", "prefix_end", "recv_begin", "sync", "recv_data"),
        ("recv_prefix_load", "recv_begin", "recv_prefix_end", "copy", "recv_data"),
        ("recv_tasks", "recv_prefix_end", "recv_tasks_end", "compute", "recv_data"),
        ("recv_prepare", "recv_tasks_end", "recv_prepare_end", "setup", "recv_data"),
        ("recv_loop", "recv_prepare_end", "recv_loop_end", "copy", "recv_data"),
        ("recv_drain", "recv_loop_end", "recv_drain_end", "wait", "recv_data"),
        ("recv_clear_issue", "recv_drain_end", "recv_clear_issue_end", "issue", "recv_data"),
        ("recv_clear_wait", "recv_clear_issue_end", "recv_clear_end", "sync", "recv_data"),
        ("recv_return", "recv_return_begin", "recv_data_end", "finalize", "recv_data"),
        ("finish_enter", "work_end", "finish_begin", "finalize", "finish"),
        ("finish_local", "finish_begin", "finish_local_end", "finalize", "finish"),
        ("finish_sync", "finish_local_end", "finish_sync_end", "sync", "finish"),
        ("finish_counter", "finish_sync_end", "finish_counter_end", "finalize", "finish"),
        ("finish_publish", "finish_counter_end", "finish_publish_end", "issue", "finish"),
        ("finish_wait", "finish_publish_end", "finish_wait_end", "wait", "finish"),
        ("finish_return", "finish_return_begin", "end", "finalize", "finish"),
        ("send_prepare", "send_cnt_end", "first_issue", "setup", "send_data"),
        ("packet_issue_interval", None, None, "issue", "send_data"),
        ("send_drain_and_outputs", "last_issue", "send_data_end", "finalize", "send_data"),
    )
    stage_ids = {span[0]: 0x53010000 + index for index, span in enumerate(spans)}
    launches = []
    for launch in run.launches:
        lanes = []
        for lane in launch.lanes:
            samples = [sample for sample in lane.samples
                       if sample.kind == "interval" and sample.raw_end > sample.raw_start]
            total = next((sample for sample in samples if sample.critical), None)
            if total is None:
                lanes.append(replace(lane, samples=tuple(samples)))
                continue
            markers = {sample.name: sample for sample in lane.samples if sample.kind == "instant"}
            markers["start"] = total
            markers["end"] = replace(total, raw_start=total.raw_end, start_us=total.end_us,
                                     begin_event_id=total.end_event_id, source_index=total.end_source_index)
            # Role belongs to the capture; the producer's send/recv split can change.
            markers["work_end"] = markers.get("send_data_end") or markers.get("recv_data_end")
            # No-task receivers return after PrepareEnd; old captures also have counter reset.
            markers["recv_return_begin"] = markers.get("recv_clear_end")
            prepared, received = markers.get("recv_prepare_end"), markers.get("recv_data_end")
            if (markers["recv_return_begin"] is None and prepared is not None and received is not None
                    and received.source_index == prepared.source_index + 1):
                markers["recv_return_begin"] = prepared
            if lane.core == 63:
                # Old captures include the removed cross-rank completion handshake.
                legacy_finish = "finish_publish_end" in markers or "finish_wait_end" in markers
                markers["finish_return_begin"] = markers.get(
                    "finish_wait_end" if legacy_finish else "finish_counter_end") or markers.get("finish_sync_end")
            else:
                markers["finish_return_begin"] = markers.get("finish_sync_end")
            # Current kernels return after local completion, without a final SyncAll.
            if markers["finish_return_begin"] is None:
                markers["finish_return_begin"] = markers.get("finish_local_end")
            packets = [sample for sample in lane.samples if sample.name == "packet_issue"]
            markers["first_issue"] = packets[0] if packets else markers.get("send_data_end")
            markers["last_issue"] = packets[-1] if packets else None
            first_hop = next((sample for sample in samples if sample.name == "send_first_hop"), None)
            direct = next((sample for sample in samples if sample.name == "send_direct"), None)
            if first_hop is not None:
                markers["first_issue"] = first_hop
            if direct is not None:
                markers["last_issue"] = replace(direct, raw_start=direct.raw_end, start_us=direct.end_us,
                    begin_event_id=direct.end_event_id, source_index=direct.end_source_index)

            def append_span(name, begin, end, category, parent, **attributes):
                if begin is None or end is None:
                    return
                if (begin.raw_start >= end.raw_start or begin.source_index >= end.source_index
                        or begin.source_index < total.source_index
                        or end.source_index > total.end_source_index):
                    return
                samples.append(replace(begin, name=name, stage_id=stage_ids[name], kind="interval",
                    parent_stage_id=total.stage_id if parent == "kernel_total" else stage_ids[parent],
                    instance_id=None, category=category, critical=False, raw_end=end.raw_start,
                    end_us=end.start_us, duration_us=(end.raw_start - begin.raw_start) / run.cycles_per_us,
                    end_event_id=end.begin_event_id, end_source_index=end.source_index,
                    attributes={"timing": "Scalar observation interval; not DMA completion", **attributes}))

            for name, begin, end, category, parent in spans:
                append_span(name, markers.get(begin), markers.get(end), category, parent)
            for index, (begin, end) in enumerate(zip(packets, packets[1:])):
                append_span("packet_issue_interval", begin, end, "issue", "send_data", packet_index=index)
            samples.sort(key=lambda sample: (sample.raw_start, -sample.raw_end, sample.stage_id))
            lanes.append(replace(lane, samples=tuple(samples)))
        launches.append(replace(launch, lanes=tuple(lanes)))
    return replace(run, launches=tuple(launches), attributes={**run.attributes,
        "timeline": "per_core_stage_intervals.v1",
        "packet_timing": "send loop boundaries, or legacy packet issues; final boundary to send_data_end is cleanup",
        "omitted_markers": "instant and zero-duration samples; raw records and diagnostics retained"})


def breakdown_record(path, config, rank, capture_id, output_dir, mapping_path=None, *,
                     operator="dispatch", run_id=None, kernel_sha=None):
    """Validate producer identity using the standard reader and retain incomplete reports."""
    mapping = profiling_mapping(mapping_path, operator=operator)
    selection = _selection(config)
    run = load_profile_run(mapping, [Path(path)], **selection)
    if run.capture_id != str(UUID(capture_id)):
        raise ValueError("breakdown capture UUID does not match this run")
    if any(lane.rank != rank for launch in run.launches for lane in launch.lanes):
        raise ValueError("breakdown stream rank does not match its producer")
    expected_cores = set(range(64))
    if any({lane.core for lane in launch.lanes} != expected_cores for launch in run.launches):
        raise ValueError("breakdown requires all 64 core streams per launch")
    # This reader is also imported by the Ubuntu 20.04 / Python 3.8 Host CI.
    # Both registered kernel names end in the fixed suffix below.
    if run.operator != kernel_name(operator)[:-len("_kernel")]:
        raise ValueError("breakdown mapping operator does not match its producer")
    identity = dict(operator=operator, kernel=kernel_name(operator))
    if run_id is not None:
        identity["run_id"] = run_id
    if kernel_sha is not None:
        identity["kernel_sha256"] = kernel_sha
    run = replace(run, attributes={**run.attributes, "selection": "last_launch",
                  "capture_mode": "latest_launch", "captured_launch": config["warmup"] + config["loops"] - 1,
                  "submitted_launches": config["warmup"] + config["loops"], **identity})
    run = breakdown_timeline(run) if operator == "dispatch" else combine_breakdown_timeline(run, config)
    generated = generate_reports_for_run(run, Path(output_dir))
    # Common analysis owns timing semantics; EP only reduces each rank's result.
    analysis = analyze_run(run)
    interval_names = {sample.name for launch in run.launches for lane in launch.lanes
                      for sample in lane.samples if sample.kind == "interval"}
    durations = [max((sample.duration_us for lane in launch.lanes for sample in lane.samples
                      if sample.critical), default=0.0) for launch in run.launches if launch.id >= 0]
    evidence = dict(schema=RECORDS_SCHEMA, summary_schema=PROFILE_SUMMARY_SCHEMA, **identity,
                rank=rank, capture_id=run.capture_id, records_path=str(Path(path).resolve()),
                run_mode="breakdown", loops=config["loops"], warmup=config["warmup"],
                launches=config["loops"] + config["warmup"], measured_launches=len(durations),
                selection="last_launch", capture_mode="latest_launch", captured_launches=1,
                captured_launch=config["loops"] + config["warmup"] - 1,
                status=generated.status, sample_us=durations, dropped_records=run.dropped_record_count,
                diagnostics=[dict(code=item.code, message=item.message) for item in run.diagnostics[:64]],
                diagnostic_count=len(run.diagnostics),
                stages_us={item.name: dict(sample_count=item.sample_count, total=item.total_us,
                                          median=item.median_us, minimum=item.minimum_us, maximum=item.maximum_us)
                           for item in analysis.stages.values() if item.name in interval_names},
                artifacts={item.name: str(item) for item in generated.artifacts},
                artifact_manifest={item.name: dict(size_bytes=item.stat().st_size, sha256=_file_digest(item))
                                   for item in generated.artifacts},
                summary_path=str(generated.output_dir / "summary.json"))
    if operator == "combine":
        totals = combine_loop_core_totals(run)
        if totals:
            evidence["core_totals_us"] = totals
    atomic_write_json(Path(evidence["summary_path"]), evidence)
    evidence["summary_sha256"] = _file_digest(evidence["summary_path"])
    return evidence


# Preparation and loop intervals exist only in new non-dedup captures.
COMBINE_LOOP_MODULES = {
    "m3_prepare": "m3_direct", "m3_server_select": "m3_direct",
    "m3_loop_read": "m3_direct", "m3_loop_pack_send": "m3_direct", "m3_retire": "m3_direct",
    "m6_prepare": "m6_final", "m6_ready_wait": "m6_final",
    "m6_token_prepare": "m6_final", "m6_loop_input_wait": "m6_final",
    "m6_loop_compute_prefetch": "m6_final", "m6_output": "m6_final", "m6_retire": "m6_final",
}


def combine_loop_core_totals(run):
    """Sum repeated intervals per core; never sum different cores into latency."""
    totals = {}
    for launch in run.launches:
        for lane in launch.lanes:
            for sample in lane.samples:
                if sample.name in COMBINE_LOOP_MODULES:
                    cores = totals.setdefault(sample.name, {})
                    key = str(lane.core)
                    cores[key] = cores.get(key, 0.0) + sample.duration_us
    return {name: dict(cores=cores, **_statistics(list(cores.values()))) for name, cores in totals.items()}


def combine_breakdown_timeline(run, config=None):
    """Keep real module intervals; retained C6 captures contain only the total."""
    if config and config.get("implemented_through") in ("M3", "M4", "M5"):
        if config["implemented_through"] != "M5" and (config["loops"] != 1 or config["warmup"] != 0):
            raise ValueError("M3/M4 captures exactly one launch without warmup")
        # Retained captures may predate this split. If either child is present,
        # require the full split on every participating core, not just totals.
        m2_split = any(sample.name in ("m2_prepare_notify", "m2_server_wait")
                       for launch in run.launches for lane in launch.lanes for sample in lane.samples)
        loop_detail = any(sample.name in COMBINE_LOOP_MODULES
                          for launch in run.launches for lane in launch.lanes for sample in lane.samples)
        if loop_detail and config.get("server_dedup_enabled") is not False:
            raise ValueError("Combine loop detail requires dedup=0")
        launches = []
        for launch in run.launches:
            lanes = []
            for lane in launch.lanes:
                expected = {"kernel_total", "m1_init", "m1_stage", "m6_drain"}
                expected.add("m2_control" if lane.core >= config.get("combine_send_core_num", 32) else "m3_direct")
                if m2_split:
                    if lane.core >= 56:
                        expected.add("m2_prepare_notify")
                    if lane.core >= config.get("combine_send_core_num", 32):
                        expected.add("m2_server_wait")
                if config["implemented_through"] in ("M4", "M5") and lane.core >= config.get("combine_send_core_num", 32):
                    expected.add("m4_partial")
                if config["implemented_through"] == "M5" and lane.core >= config.get("combine_send_core_num", 32):
                    expected.add("m5_final")
                if config.get("server_dedup_enabled") is False:
                    expected = {"kernel_total", "m1_init", "m6_drain"}
                    expected.add("m6_final" if lane.core >= config.get("combine_send_core_num", 32) else "m3_direct")
                token_stages = {"m4_token_issue": "m4_partial", "m5_token_issue": "m5_final", "m6_token_issue": "m6_final"}
                actual = [sample.name for sample in lane.samples
                          if sample.kind == "interval" and sample.name not in token_stages and sample.name not in COMBINE_LOOP_MODULES]
                if set(actual) != expected or len(actual) != len(expected):
                    raise ValueError("Combine module/core role coverage mismatch: " + str(lane.core))
                samples = []
                if loop_detail:
                    counts = {name: sum(s.name == name for s in lane.samples) for name in COMBINE_LOOP_MODULES}
                    recv = 64 - config.get("combine_send_core_num", 32)
                    local = lane.core - config.get("combine_send_core_num", 32)
                    required = {}
                    if local >= 0:
                        tokens = config["bs"] // recv + (local < config["bs"] % recv)
                        required = dict(m6_prepare=1, m6_retire=1, m6_ready_wait=tokens,
                            m6_token_prepare=tokens, m6_output=tokens,
                            m6_loop_input_wait=tokens * config["topk"],
                            m6_loop_compute_prefetch=tokens * config["topk"])
                    else:
                        selected = counts["m3_server_select"] != 0
                        required = dict(m3_prepare=1, m3_server_select=config["rank_size"] // 8 if selected else 0,
                            m3_retire=int(selected), m3_loop_read=counts["m3_loop_read"],
                            m3_loop_pack_send=counts["m3_loop_read"])
                    if any(counts[name] != required.get(name, 0) for name in counts):
                        raise ValueError("Combine loop/core role coverage mismatch: " + str(lane.core))
                modules = {**token_stages, **COMBINE_LOOP_MODULES}
                sequences = dict.fromkeys(modules, 0)
                for sample in lane.samples:
                    if sample.name in sequences:
                        if modules[sample.name] not in expected:
                            raise ValueError("Combine token/core role coverage mismatch: " + str(lane.core))
                        # records.v1 contains timestamps and event IDs, not TokenId.
                        # M4 and M5 share a core, but have independent task sets.
                        # Identify each task by its module-local execution order.
                        sequence = sequences[sample.name]
                        sample = replace(sample, instance_id=sequence,
                            attributes={**sample.attributes, "task_sequence": sequence,
                                        "task_sequence_scope": "stage/core" if sample.name in COMBINE_LOOP_MODULES else "module/core",
                                        "timing": "Scalar elapsed including existing waits; no added completion fence",
                                        **({"token_sequence": sequence // config["topk"],
                                            "topk_iteration": sequence % config["topk"]}
                                           if sample.name in ("m6_loop_input_wait", "m6_loop_compute_prefetch") else {})})
                        sequences[sample.name] += 1
                    if sample.name == "m4_partial" and config["implemented_through"] == "M5":
                        sample = replace(sample, attributes={**sample.attributes,
                            "timing": "instruction issue and read/vector handoff; MTE3 may remain in flight",
                            "mte3_completion_required": False})
                    if sample.name in ("m2_prepare_notify", "m2_server_wait"):
                        sample = replace(sample, attributes={**sample.attributes,
                            "timing": ("local 64-core prepare wait, clear and ready notification issue"
                                       if sample.name == "m2_prepare_notify" else
                                       "local server 8-card ready wait and clear issue"),
                            "mte3_completion_required": False})
                    if sample.name == "m3_mte3_complete" and "m3_direct" not in expected:
                        raise ValueError("Combine M3 completion marker on non-Send core")
                    samples.append(sample)
                lanes.append(replace(lane, samples=tuple(samples)))
            launches.append(replace(launch, lanes=tuple(lanes)))
        return replace(run, launches=tuple(launches), attributes={**run.attributes,
            "scope": "full" if config["implemented_through"] == "M5" else "partial", "implemented_through": config["implemented_through"],
            **({"m4_m5_handoff": "read/vector retired; output slots inherited without MTE3 drain"}
               if config["implemented_through"] == "M5" and config.get("server_dedup_enabled") is not False else {}),
            "timing": "latest launch; module intervals and optional per-token instruction issue, no added fences"})
    if config and config.get("implemented_through") == "M2":
        common = {"kernel_total", "m1_init", "m1_select", "m1_stage_copy", "m1_prepare_publish", "m6_drain", "p21_delay_50us"}
        for launch in run.launches:
            for lane in launch.lanes:
                expected = set(common)
                if lane.core >= config.get("combine_send_core_num", 32):
                    expected.update(("m2_ready_wait", "m2_ready_clear"))
                if lane.core >= 56:
                    expected.update(("m2_prepare_wait", "m2_notify"))
                actual = [sample.name for sample in lane.samples if sample.kind == "interval"]
                if set(actual) != expected or len(actual) != len(expected):
                    raise ValueError("M1/M2 breakdown core role or interval coverage mismatch: " + str(lane.core))
        return replace(run, attributes={**run.attributes, "scope": "partial", "implemented_through": "M2",
            "timing": "M1/M2 explicit intervals; 50 us wait separately measured, never subtracted from total"})
    launches = []
    for launch in run.launches:
        lanes = []
        for lane in launch.lanes:
            samples = list(lane.samples)
            samples.extend(replace(sample, name="empty_wait", stage_id=0x3003,
                parent_stage_id=sample.stage_id, category="wait", critical=False,
                attributes={**sample.attributes, "scope": "empty/framework"})
                for sample in lane.samples if sample.critical and sample.raw_end > sample.raw_start)
            lanes.append(replace(lane, samples=tuple(samples)))
        launches.append(replace(launch, lanes=tuple(lanes)))
    return replace(run, launches=tuple(launches), attributes={**run.attributes,
        "scope": "empty/framework", "algorithm_bytes": 0, "algorithm_bandwidth": "N/A",
        "timing": "Scalar boundaries around the empty kernel's 50 us wait; no Combine algorithm"})


# CANN 9.1 的 GetSystemCycle 文档明确规定 950PR/950DT 为 1 GHz；A2/A3 的 50 MHz 不适用。
A5_SYSTEM_TICKS_PER_US = 1000
SYSTEM_CLOCK_SOURCE = {
    "api": "AscendC::GetSystemCycle", "soc": "Ascend950PR/Ascend950DT",
    "frequency_hz": 1000000000, "version": "9.1.0",
    "commit": "91c03327210113585f52739a6c85c35ece492970",
    "url": "https://gitcode.com/cann/asc-devkit/blob/91c03327210113585f52739a6c85c35ece492970/"
           "docs/api/SIMD-API/基础API/工具接口/系统资源与变量/GetSystemCycle_ISASI.md",
}


def _integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f"{name} 必须为不小于 {minimum} 的整数")
    return value


def _number(value, name, positive=False):
    if (type(value) not in (int, float) or not math.isfinite(value)
            or (value <= 0 if positive else value < 0)):
        raise ValueError(f"{name} 必须为有限{'正' if positive else '非负'}数")
    return value


def _statistics(samples, suffix=""):
    return {"median" + suffix: statistics.median(samples),
            "minimum" + suffix: min(samples), "maximum" + suffix: max(samples),
            "mean" + suffix: math.fsum(samples) / len(samples)}


def performance_record(output_dir, loops, warmup, *, operator="dispatch"):
    """独立 rank 的本次 profile 必须恰好包含 warmup+loops 次目标 kernel。"""
    _integer(loops, "loops", 1)
    _integer(warmup, "warmup")
    inputs = profiler.discover_profile_inputs(Path(output_dir))
    if len(inputs) != 1:
        raise ValueError(f"每 rank 需要唯一 profile 输入，发现 {len(inputs)} 个")
    target = kernel_name(operator)
    samples, source = profiler.series_from_input(
        inputs[0], target, loops, strict_samples=True)
    if source["matched_samples"] != warmup + loops:
        raise ValueError(f"目标 kernel 样本数应为 {warmup + loops}，实际 {source['matched_samples']}")
    return {"kernel": target, "samples": loops, "sample_us": samples,
            "matched_samples": source.pop("matched_samples"), "source": source,
            **_statistics(samples, "_us")}


def validate_rank_measurements(record, chain_config, *, operator="dispatch"):
    """校验单 rank 的测量内容；run/build/rank 身份由调用方校验。"""
    mode = chain_config["run_mode"]
    if mode == "functional":
        return
    if mode not in ("performance", "breakdown"):
        raise ValueError("未知测量模式")
    loops = _integer(chain_config["loops"], "loops", 1)
    warmup = _integer(chain_config["warmup"], "warmup")
    if mode == "breakdown":
        evidence = record.get("breakdown")
        if not isinstance(evidence, dict) or evidence.get("schema") != RECORDS_SCHEMA:
            raise ValueError("breakdown requires standard records.v1 capture")
        if (evidence.get("run_mode") != mode or evidence.get("launches") != warmup + loops
                or evidence.get("loops") != loops or evidence.get("warmup") != warmup):
            raise ValueError("breakdown capture launch configuration mismatch")
        if (evidence.get("selection") != "last_launch" or evidence.get("capture_mode") != "latest_launch"
                or evidence.get("captured_launch") != warmup + loops - 1 or evidence.get("captured_launches") != 1):
            raise ValueError("breakdown must describe the last launch only")
        if evidence.get("status") != "complete" or evidence.get("measured_launches") != 1:
            raise ValueError("breakdown capture is incomplete; communication correctness is independent")
        samples = evidence.get("sample_us")
        if not isinstance(samples, list) or len(samples) != 1:
            raise ValueError("breakdown measured launch count mismatch")
        for sample in samples:
            _number(sample, "kernel duration", positive=True)
        UUID(evidence["capture_id"])
        if not isinstance(evidence.get("records_path"), str) or not evidence["records_path"]:
            raise ValueError("breakdown records path is missing")
        if not isinstance(evidence.get("stages_us"), dict):
            raise ValueError("breakdown generic stage analysis is missing")
        for name, stage in evidence["stages_us"].items():
            if not isinstance(name, str) or not isinstance(stage, dict):
                raise ValueError("breakdown stage summary is malformed")
            _integer(stage.get("sample_count"), "stage sample count", 1)
            for key in ("total", "median", "minimum", "maximum"):
                _number(stage.get(key), "stage " + key)
            if not stage["minimum"] <= stage["median"] <= stage["maximum"] <= stage["total"]:
                raise ValueError("breakdown stage statistics are inconsistent")
        return
    evidence = record.get("performance")
    target = kernel_name(operator)
    if not isinstance(evidence, dict) or evidence.get("kernel") != target:
        raise ValueError("缺少目标 kernel 的 performance 记录")
    samples = evidence.get("sample_us")
    if (not isinstance(samples, list) or len(samples) != loops
            or type(evidence.get("samples")) is not int or evidence["samples"] != loops
            or type(evidence.get("matched_samples")) is not int
            or evidence["matched_samples"] != warmup + loops):
        raise ValueError("performance 样本数与本次 warmup/loops 不一致")
    for sample in samples:
        _number(sample, "kernel duration", positive=True)
    for name, value in _statistics(samples, "_us").items():
        if _number(evidence.get(name), name, positive=True) != value:
            raise ValueError("performance 统计值与原始样本不一致：" + name)
    source = evidence.get("source")
    if (not isinstance(source, dict) or not isinstance(source.get("path"), str) or not source["path"]
            or source.get("source_type") not in ("op_summary_csv", "ai_core_op_summary_db")
            or not isinstance(source.get("matched_kernel_names"), list) or not source["matched_kernel_names"]
            or any(not isinstance(name, str) or not profiler.kernel_name_matches(name, target)
                   for name in source["matched_kernel_names"])):
        raise ValueError("performance 缺少有效的 profile 来源")


def _validate_data_traffic(record, config, kinds):
    stride = _integer(record.get("token_stride"), "token_stride", 1)
    for kind in kinds:
        packets = _integer(record.get(kind + "_packets"), kind + "_packets")
        _integer(record.get(kind + "_received"), kind + "_received")
        if _integer(record.get(kind + "_bytes"), kind + "_bytes") != packets * stride:
            raise ValueError(kind + " bytes 与包数/stride 不一致")
    if not 0 < record["first_hop_packets"] + record["direct_packets"] <= config["bs"] * config["topk"]:
        raise ValueError("源端物理包数超出原始 route 范围")
    local, same, cross = [_integer(record.get(key + "_data_bytes"), key + "_data_bytes")
                          for key in ("local", "same_server", "cross_server")]
    if (any(value % stride for value in (local, same, cross)) or
            local + same + cross != sum(record[kind + "_bytes"] for kind in kinds) or
            cross < record["first_hop_bytes"]):
        raise ValueError("本卡/跨卡/跨 Server 流量与物理包数不一致")


def validate_source_send_traffic(record, config):
    """Read retained source-only v3/v4 results with their original byte scope."""
    _validate_data_traffic(record, config, ("first_hop", "direct"))


def validate_dispatch_traffic(record, config):
    """Full Dispatch counts every physical send, including relay fan-out."""
    kinds = ("first_hop", "direct", "relay")
    _validate_data_traffic(record, config, kinds)
    if _integer(record.get("data_bytes"), "data_bytes") != sum(record[kind + "_bytes"] for kind in kinds):
        raise ValueError("Dispatch data_bytes 未包含完整 first-hop/DIRECT/relay 流量")
    if config.get("server_dedup_enabled") is False:
        if (any(record[kind + suffix] != 0 for kind in ("first_hop", "relay")
                for suffix in ("_packets", "_received", "_bytes")) or
                record["direct_packets"] != config["bs"] * config["topk"]):
            raise ValueError("关闭 server 去重时必须全部 DIRECT，且无 first-hop/relay")


def summarize_measurements(rank_records, chain_config, kernel_sha, *, output_valid=True, operator="dispatch"):
    """performance 汇总各 server 最后最多 50 轮均值；breakdown 取最后一轮。"""
    rank_size = _integer(chain_config["rank_size"], "rank_size", 1)
    if (not isinstance(rank_records, list) or len(rank_records) != rank_size
            or any(not isinstance(row, dict) or type(row.get("rank")) is not int for row in rank_records)
            or {row["rank"] for row in rank_records} != set(range(rank_size))):
        raise ValueError("测量 rank 缺失、重复或越界")
    ranks = sorted(rank_records, key=lambda row: row["rank"])
    # The consumer reports the compiled mode; no runtime setting selects it.
    # Retained reports without mode provenance remain readable.
    if "server_dedup_enabled" not in chain_config and any("server_dedup_enabled" in row for row in ranks):
        chain_config = dict(chain_config, server_dedup_enabled=ranks[0].get("server_dedup_enabled"))
    for record in ranks:
        if "server_dedup_enabled" in chain_config and (
                type(chain_config["server_dedup_enabled"]) is not bool or
                record.get("server_dedup_enabled") is not chain_config["server_dedup_enabled"]):
            raise ValueError("Dispatch server_dedup_enabled 与本次配置不一致")
        validate_rank_measurements(record, chain_config, operator=operator)
    output_valid = output_valid and all(row.get("validation_status") != "FAIL" for row in ranks)
    mode = chain_config["run_mode"]
    source_send = chain_config.get("kernel_scope") == "source_send"
    dispatch = chain_config.get("kernel_scope") == "dispatch"
    kinds = ("first_hop", "direct", "relay") if dispatch else ("first_hop", "direct")
    if output_valid and (source_send or dispatch):
        for record in ranks:
            (validate_dispatch_traffic if dispatch else validate_source_send_traffic)(record, chain_config)
        for kind in kinds:
            if sum(row[kind + "_packets"] for row in ranks) != sum(row[kind + "_received"] for row in ranks):
                raise ValueError(kind + " 全 Rank 发送/接收包数不一致")
    count_scope = chain_config.get("kernel_scope") == "cnt_cumsum"
    summary = {"run_mode": mode, "backend": "memory", "native_status": "dispatch" if dispatch else "source_send" if source_send else "cnt_cumsum" if count_scope else "empty",
               "kernel": kernel_name(operator), "kernel_sha256": kernel_sha,
               "rank_size": rank_size, "validation_status": "PASS" if output_valid else "FAIL",
               "configuration": {name: chain_config[name] for name in
                                 ("bs", "h", "topk", "moe_expert_num", "dtype", "block_dim", "loops", "warmup")},
               "host_barrier": "enabled", "submission": "continuous_batch_then_sync_and_host_barrier"}
    for name in ("server_dedup_enabled",):
        if name in chain_config:
            summary["configuration"][name] = chain_config[name]
    if mode == "functional":
        return summary
    summary.update(algorithm_communication_bytes=0 if output_valid else None, bandwidth="N/A")
    if output_valid and (count_scope or source_send or dispatch):
        # One 32B record per remote expert per source rank; excludes self-target
        # counts, local prefix traffic and polling reads.
        remote_bytes = (chain_config["moe_expert_num"] - chain_config["moe_expert_num"] // rank_size) * 32
        summary.update(algorithm_communication_bytes=remote_bytes * rank_size,
                       communication_bytes_per_rank=remote_bytes,
                       communication_scope="remote cnt records; excludes local traffic and polling")
    if output_valid and (source_send or dispatch):
        summary.pop("communication_bytes_per_rank", None)
        traffic = [{"rank": row["rank"], "original_routes": chain_config["bs"] * chain_config["topk"],
                    **{kind + suffix: row[kind + suffix] for kind in kinds for suffix in ("_packets", "_received", "_bytes")},
                    "data_bytes": sum(row[kind + "_bytes"] for kind in kinds),
                    "token_stride": row["token_stride"], "cross_server_data_bytes": row["cross_server_data_bytes"],
                    "same_server_data_bytes": row["same_server_data_bytes"], "local_data_bytes": row["local_data_bytes"],
                    "remote_count_bytes": remote_bytes,
                    "cross_server_count_bytes": (chain_config["moe_expert_num"] - chain_config["moe_expert_num"] // rank_size * 8) * 32,
                    "same_server_count_bytes": chain_config["moe_expert_num"] // rank_size * 7 * 32,
                    "local_count_bytes": chain_config["moe_expert_num"] // rank_size * 32}
                   for row in ranks]
        summary.update(measurement="single_cold_submission" if chain_config["loops"] == 1 and chain_config["warmup"] == 0
                       else "continuous_batch",
                       route_mode=chain_config["route_mode"], seed=chain_config["seed"],
                       quant_mode=chain_config["quant_mode"], fp8_format=chain_config["fp8_format"],
                       rank_traffic=traffic,
                       algorithm_communication_bytes=sum(sum(row[kind + "_bytes"] for kind in kinds) + remote_bytes for row in ranks),
                       communication_scope=("first-hop, DIRECT and relay" if dispatch else "first-hop and DIRECT") +
                           " full T including local copies, plus remote cnt records; excludes polling/local prefix/completion epoch traffic")
    if mode == "performance":
        if rank_size % RANKS_PER_SERVER:
            raise ValueError("performance 每个 server 必须包含完整的 8 个 rank")
        selected = min(chain_config["loops"], PERFORMANCE_SAMPLE_LIMIT)
        # The workflow validates host_index == rank // 8 before aggregation.
        servers = [ranks[start:start + RANKS_PER_SERVER]
                   for start in range(0, rank_size, RANKS_PER_SERVER)]
        server_means = [math.fsum(sample for row in server
                                 for sample in row["performance"]["sample_us"][-selected:]) /
                        (RANKS_PER_SERVER * selected) for server in servers]
        summary["performance"] = {
            "unit": "us", "selection": "last_up_to_50_measured_launches",
            "reduction": "statistics_of_server_sample_means",
            "samples_per_rank": selected, "samples_per_server": RANKS_PER_SERVER * selected,
            "ranks_per_server": RANKS_PER_SERVER, "server_count": len(servers),
            "server_mean_us": server_means, **_statistics(server_means, "_us")}
        if output_valid and (count_scope or source_send or dispatch):
            server_bytes = [sum(remote_bytes + (sum(row[kind + "_bytes"] for kind in kinds)
                                                if source_send or dispatch else 0)
                                for row in server) for server in servers]
            bandwidth = [byte_count / (us * 1000) for byte_count, us in zip(server_bytes, server_means)]
            summary.update(server_bandwidth_gbps=bandwidth, bandwidth_gbps=_statistics(bandwidth),
                           bandwidth="effective per-server GB/s; sum of 8 rank bytes / server mean kernel latency")
            if count_scope:
                summary.update(route_mode=chain_config["route_mode"], seed=chain_config["seed"])
    else:
        if len({row["breakdown"]["capture_id"] for row in ranks}) != 1:
            raise ValueError("rank captures do not share one UUID")
        durations = [row["breakdown"]["sample_us"][0] for row in ranks]
        stage_names = sorted(set().union(*(row["breakdown"]["stages_us"] for row in ranks)))
        summary["measurement"] = "last_launch"
        summary["breakdown"] = dict(schema=RECORDS_SCHEMA, selection="last_launch",
            reduction="statistics across ranks of each rank's last-launch slowest core",
            ticks_per_us=A5_SYSTEM_TICKS_PER_US, clock=dict(SYSTEM_CLOCK_SOURCE),
            kernel_us=_statistics(durations), rank_us=durations,
            stages_us={name: _statistics([row["breakdown"]["stages_us"][name]["median"]
                                         for row in ranks if name in row["breakdown"]["stages_us"]])
                       for name in stage_names},
            timing="Scalar arrival; existing completion fences remain; issue is not DMA completion; atomic is nested in metadata")
        total_names = sorted(set().union(*(row["breakdown"].get("core_totals_us", {}) for row in ranks)))
        if total_names:
            summary["breakdown"]["core_totals_us"] = {
                name: _statistics([row["breakdown"]["core_totals_us"][name]["maximum"]
                                   for row in ranks if name in row["breakdown"].get("core_totals_us", {})])
                for name in total_names}
    return summary


def render_measurements(summary):
    """生成与结构化结果一致的简短中文报告；functional 无性能输出。"""
    if summary["run_mode"] == "functional":
        return ""
    traffic = summary["algorithm_communication_bytes"]
    traffic_text = f"{traffic}B" if traffic is not None else "N/A（流量未通过完整校验）"
    lines = [f"单算子测量：mode={summary['run_mode']} backend=memory native={summary['native_status']} "
             f"ranks={summary['rank_size']} 算法通信={traffic_text} 带宽={summary['bandwidth']}",
             f"kernel={summary['kernel']} sha256={summary['kernel_sha256']}"]

    def append(label, stats, unit, suffix="", server=False):
        fields = (("min", "minimum"), ("mean", "mean"), ("max", "maximum")) if server else (
            ("min", "minimum"), ("median", "median"), ("max", "maximum"))
        lines.append(label + "：" + " ".join(
            f"{name}={stats[key + suffix]:.3f} {unit}" for name, key in fields))

    if "performance" in summary:
        performance = summary["performance"]
        append(f"kernel（每 server 8 卡 × 最后 {performance['samples_per_rank']} 轮均值的跨 server 汇总）",
               performance, "us", "_us", server=True)
    if "bandwidth_gbps" in summary:
        append("有效通信带宽（每 server；字节范围见上文）", summary["bandwidth_gbps"], "GB/s", server=True)
    if "breakdown" in summary:
        breakdown = summary["breakdown"]
        append("kernel（最后一轮每 rank 最慢核的跨 rank 汇总）", breakdown["kernel_us"], "us")
        for name, stats in breakdown["stages_us"].items():
            append("标准打点区间 " + name, stats, "us")
        for name, stats in breakdown.get("core_totals_us", {}).items():
            append("每核累计最大值（跨 rank） " + name, stats, "us")
        lines.append("Scalar 时间戳直接写 GM；issue 表示下发，atomic 包含于 metadata；performance 使用关闭打点的产物。")
    return "\n".join(lines)


def relocate_breakdown_report(evidence, node_log, rank, host, *, local_records=False):
    """Rebase completed reports while retaining the raw capture's source location."""
    result = dict(evidence)
    directory = Path(node_log) / f"profile-rank-{rank}"
    result.update(source_records_path=evidence.get("source_records_path", evidence.get("records_path")),
                  source_host=host, records_collected=local_records,
                  local_records_path=str(Path(node_log) / f"rank-{rank}.records.v1") if local_records else None,
                  summary_path=str(directory / "summary.json"),
                  artifacts={name: str(directory / name) for name in PROFILE_ARTIFACTS})
    return result


def validate_report_artifacts(evidence, rank, chain_config):
    """Read only the small producer summary; verify large artifacts by streaming hash."""
    summary_path = Path(evidence["summary_path"])
    if not summary_path.is_file() or summary_path.is_symlink() or summary_path.parent.is_symlink():
        raise ValueError("profiling report summary must be a regular local file")
    if not 0 < summary_path.stat().st_size <= 1024 * 1024:
        raise ValueError("profiling report summary is empty or exceeds 1 MiB")
    if _file_digest(summary_path) != evidence.get("summary_sha256"):
        raise ValueError("profiling report summary checksum mismatch")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if not isinstance(summary, dict) or summary.get("summary_schema") != PROFILE_SUMMARY_SCHEMA:
        raise ValueError("profiling report summary schema is invalid")
    if summary.get("rank") != rank or evidence.get("rank") != rank:
        raise ValueError("profiling report summary rank mismatch")
    identity = ("schema", "capture_id", "run_mode", "loops", "warmup", "launches", "measured_launches",
                "selection", "capture_mode", "captured_launch", "captured_launches", "status", "sample_us",
                "stages_us", "core_totals_us", "dropped_records", "artifact_manifest", "operator", "kernel", "run_id", "kernel_sha256",
                "last_launch", "scope", "native_status", "algorithm_bytes", "algorithm_bandwidth_gbps", "algorithm_bandwidth",
                "implemented_through", "stage_copy_bytes", "control_publish_bytes", "wait_us",
                "stage_payload_bytes", "direct_payload_bytes", "direct_wire_bytes", "partial_payload_bytes", "partial_wire_bytes", "partial_self_read_bytes", "partial_stage_read_bytes", "final_read_bytes", "y_out_bytes", "flag_clear_bytes", "traffic_scope", "performance_scope")
    if any(summary.get(key) != evidence.get(key) for key in identity):
        raise ValueError("profiling report summary disagrees with node scalar result")
    if (summary.get("loops") != chain_config["loops"] or summary.get("warmup") != chain_config["warmup"]
            or summary.get("selection") != "last_launch"):
        raise ValueError("profiling report summary configuration mismatch")
    manifest = summary.get("artifact_manifest")
    if not isinstance(manifest, dict) or set(manifest) != set(PROFILE_ARTIFACTS):
        raise ValueError("profiling report artifact manifest is incomplete")
    if set(evidence.get("artifacts", {})) != set(PROFILE_ARTIFACTS):
        raise ValueError("profiling report artifact paths are incomplete")
    for name in PROFILE_ARTIFACTS:
        artifact = Path(evidence["artifacts"][name])
        if (not artifact.is_file() or artifact.parent.resolve() != summary_path.parent.resolve()
                or artifact.name != name or artifact.is_symlink()):
            raise ValueError("profiling report artifact escaped its rank directory")
        metadata = manifest[name]
        if (not isinstance(metadata, dict) or type(metadata.get("size_bytes")) is not int
                or metadata["size_bytes"] <= 0 or not isinstance(metadata.get("sha256"), str)):
            raise ValueError("profiling report artifact metadata is malformed")
        if artifact.stat().st_size != metadata["size_bytes"] or _file_digest(artifact) != metadata["sha256"]:
            raise ValueError("profiling report artifact is truncated or corrupt: " + name)
    return summary


def copy_rank_trace(source, directory, rank):
    """Copy finished Chrome JSON without parsing it or buffering it in memory."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / f"rank-{rank}.json"
    with tempfile.NamedTemporaryFile(dir=directory, prefix=f".rank-{rank}.", suffix=".tmp", delete=False) as handle:
        temporary = Path(handle.name)
    try:
        # Older atomic_write_json output starts with this sorted, optional
        # header. Chrome rejects "us" although event ts/dur must remain in us.
        # Strip only that legacy header; preserve events and producer artifacts.
        legacy_header = b'{\n  "displayTimeUnit": "us",\n'
        with Path(source).open("rb") as reader, temporary.open("wb") as writer:
            prefix = reader.read(len(legacy_header))
            writer.write(b'{\n' if prefix == legacy_header else prefix)
            shutil.copyfileobj(reader, writer, length=1024 * 1024)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return str(destination)


def _write_trace_index(output_dir, result):
    # Re-exporting a partial run must not leave an old rank looking available.
    current_files = {Path(path).name for path in result["ranks"].values()}
    for previous in Path(result["trace_dir"]).glob("rank-*.json"):
        if previous.stem[5:].isdigit() and previous.name not in current_files:
            previous.unlink()
    atomic_write_json(Path(output_dir) / "index.json", result)


def collect_breakdown_reports(rank_records, chain_config, output_dir, run_id, kernel_sha, *, operator="dispatch"):
    """Collect one Chrome JSON per rank and a compact index; never reparse captures."""
    if chain_config["run_mode"] != "breakdown":
        raise ValueError("report collection requires breakdown mode")
    rank_size = _integer(chain_config["rank_size"], "rank_size", 1)
    output_dir = Path(output_dir).resolve()
    trace_dir = output_dir / "chrome"
    trace_dir.mkdir(parents=True, exist_ok=True)
    records = {}
    for row in rank_records:
        rank = _integer(row.get("rank"), "rank")
        if rank >= rank_size or rank in records:
            raise ValueError("profiling report ranks are duplicated or outside the requested run")
        records[rank] = row
    reports, errors, values, traces = {}, [], {}, {}
    expected_capture_id = str(uuid5(NAMESPACE_URL, "tilexr-validation:" + run_id))
    for rank in range(rank_size):
        evidence = records.get(rank, {}).get("breakdown")
        entry = dict(rank=rank, status="unavailable", artifacts_verified=False)
        row = records.get(rank, {})
        entry["validation_status"] = row.get("validation_status", row.get("status", "UNKNOWN"))
        if entry["validation_status"] == "FAIL":
            entry["validation_error"] = row.get("validation_error")
        if isinstance(evidence, dict):
            entry.update(artifacts=evidence.get("artifacts", {}), summary=evidence.get("summary_path"),
                         raw_records=dict(host=evidence.get("source_host"),
                             path=evidence.get("source_records_path", evidence.get("records_path")),
                             local_path=evidence.get("local_records_path")))
        try:
            if not isinstance(evidence, dict):
                raise ValueError("rank has no completed profiling report")
            validate_report_artifacts(evidence, rank, chain_config)
            if evidence.get("capture_id") != expected_capture_id:
                raise ValueError("profiling report capture UUID does not belong to this run")
            traces[str(rank)] = copy_rank_trace(evidence["artifacts"]["perfetto_trace.json"], trace_dir, rank)
            entry.update(status="incomplete", artifacts_verified=True)
            validate_rank_measurements(dict(rank=rank, breakdown=evidence), chain_config, operator=operator)
            entry["status"] = "complete"
            entry["kernel_us"] = evidence["sample_us"][0]
            values[rank] = entry["kernel_us"]
        except (OSError, ValueError, TypeError, KeyError) as error:
            entry["error"] = str(error)
            errors.append(dict(rank=rank, message=str(error)))
        reports[str(rank)] = entry
    order = sorted((value, rank) for rank, value in values.items())
    selected = (dict(fastest=order[0][1], p50=order[(len(order) - 1) // 2][1], slowest=order[-1][1])
                if order else {})
    result = dict(schema="tilexr_profiling_report_index.v1", run_id=run_id, kernel_sha256=kernel_sha,
                  status="incomplete" if errors else "complete", selection="last_launch",
                  rank_size=rank_size, available_ranks=len(values), reports=reports, errors=errors,
                  kernel_us=_statistics(list(values.values())) if values else None,
                  rank_us={str(rank): value for rank, value in sorted(values.items())},
                  reduction="statistics across available ranks of each last-launch slowest core",
                  p50_representative="lower middle rank after sorting by (kernel_us, rank); numeric median averages the two middle values",
                  selected_ranks=selected,
                  selected={role: traces[str(rank)] for role, rank in selected.items()},
                  ranks=traces, trace_dir=str(trace_dir),
                  report=str(trace_dir), index=str(output_dir / "index.json"),
                  raw_records_collected=False)

    _write_trace_index(output_dir, result)
    return result


def export_breakdown_traces(rank_records, chain_config, output_dir, run_id, kernel_sha, mapping_path=None, records_dir=None):
    """Explicit offline reprocessing of locally retained/copied raw captures."""
    if chain_config["run_mode"] != "breakdown":
        raise ValueError("trace export requires breakdown records")
    ranks = sorted(rank_records, key=lambda row: row["rank"])
    if [row["rank"] for row in ranks] != list(range(chain_config["rank_size"])):
        raise ValueError("trace export requires every rank")
    mapping = profiling_mapping(mapping_path)
    selection = _selection(chain_config)
    paths = []
    for row in ranks:
        evidence = row.get("breakdown", {})
        if records_dir is not None:
            path = Path(records_dir) / f"rank-{row['rank']}.records.v1"
        elif evidence.get("records_collected") is False:
            raise ValueError("raw capture remains on its producer; copy it locally and pass --records-dir")
        else:
            path = Path(evidence.get("local_records_path") or evidence["records_path"])
        if not path.is_file():
            raise ValueError("missing offline raw capture: " + str(path))
        paths.append(path)
    attributes = dict(run_id=run_id, kernel_sha256=kernel_sha, configuration=chain_config,
                      timing="Scalar arrival; issue timestamps are not DMA completion", backend="memory",
                      selection="last_launch", capture_mode="latest_launch",
                      captured_launch=chain_config["warmup"] + chain_config["loops"] - 1)
    output_dir = Path(output_dir).resolve()
    trace_dir = output_dir / "chrome"
    status = "complete"
    per_rank, order = {}, []
    for row, path in zip(ranks, paths):
        run = load_profile_run(mapping, [path], **selection)
        run = replace(run, attributes={**run.attributes, **attributes})
        if (run.capture_id != str(uuid5(NAMESPACE_URL, "tilexr-validation:" + run_id))
                or any(lane.rank != row["rank"] for launch in run.launches for lane in launch.lanes)):
            raise ValueError("offline raw capture does not match this run/rank")
        run = breakdown_timeline(run)
        reports = generate_reports_for_run(run, output_dir / ("rank-" + str(row["rank"])))
        per_rank[str(row["rank"])] = copy_rank_trace(reports.output_dir / "perfetto_trace.json", trace_dir, row["rank"])
        if reports.status != "complete":
            status = "incomplete"
        if reports.status == "complete":
            duration = max((sample.duration_us for launch in run.launches for lane in launch.lanes
                            for sample in lane.samples if sample.critical), default=0)
            if duration > 0:
                order.append((duration, row["rank"]))
    order.sort()
    selected = (dict(fastest=order[0][1], p50=order[(len(order) - 1) // 2][1], slowest=order[-1][1])
                if order else {})
    result = dict(status=status, trace_dir=str(trace_dir),
                  report=str(trace_dir), ranks=per_rank, selected_ranks=selected,
                  selected={role: per_rank[str(rank)] for role, rank in selected.items()},
                  records=[str(path) for path in paths], mapping=str(mapping))
    _write_trace_index(output_dir, result)
    return result


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Collect per-rank Chrome trace JSON from finished reports; no NPU launch.")
    parser.add_argument("summary", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--mapping", type=Path)
    parser.add_argument("--records-dir", type=Path,
                        help="directory containing explicitly copied rank-N.records.v1 captures")
    args = parser.parse_args()
    try:
        report = json.loads(args.summary.read_text(encoding="utf-8"))
        records = []
        for node in report["nodes"]:
            result = node.get("reported_result") or {}
            records.extend(result.get("ranks", []))
            records.extend(item["communication"] for item in result.get("profiling_failures", [])
                           if isinstance(item.get("communication"), dict))
        config = dict(report["chain_configuration"], rank_size=report["rank_size"])
        destination = args.output_dir or args.summary.parent / "offline-traces"
        kernel_sha = report.get("measurements", report.get("traces", {}))["kernel_sha256"]
        if args.records_dir is not None or args.mapping is not None:
            index = export_breakdown_traces(records, config, destination, report["run_id"],
                                             kernel_sha, args.mapping, args.records_dir)
        else:
            index = collect_breakdown_reports(records, config, destination, report["run_id"], kernel_sha)
        print("Chrome trace JSON directory: " + index["trace_dir"])
        if index["status"] != "complete":
            sys.exit(1)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"ERROR trace export: {error}", file=sys.stderr)
        sys.exit(1)
