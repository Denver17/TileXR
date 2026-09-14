#!/usr/bin/env python3
"""读取单算子测量证据；不启动 profiler、设备进程或通信。"""

import math
import json
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ep"))
import msprof_kernel_stats as profiler


# EP contributes only event mapping; parsing, pairing and traces use the shared package.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from profiling import RECORDS_SCHEMA, generate_reports, generate_reports_for_run
from profiling.analysis import analyze_run
from profiling.pairing import load_profile_run
from uuid import UUID

KERNEL_NAME = "tilexr_ep_dispatch_memory_server_dedup_kernel"
RESULT_SEPARATOR = "=" * 32


def render_case_description(report):
    """Describe the effective case from its result, rather than guessed CLI defaults."""
    if report.get("checkpoint") == "ep-sdedup-p1-memory-boundary":
        return (f"MemoryDomain mode=functional cards={report.get('rank_size', 'unknown')} capacity/rank=4 GiB\n"
                "Local boundary DataCopy offset=4294967264 bytes=32 pattern=0xFF")
    config = report.get("chain_configuration")
    if not config:
        return "checkpoint=" + report.get("checkpoint", "unknown")
    return (
        f"Dispatch server-dedup scope={config.get('kernel_scope', 'unknown')} mode={config['run_mode']} "
        f"loops={config['loops']} warmup={config['warmup']}\n"
        f"BS={config['bs']} H={config['h']} TopK={config['topk']} "
        f"experts={config['moe_expert_num']} dtype={config['dtype']} "
        f"cards={report.get('rank_size', 'unknown')} quant_mode={config.get('quant_mode', 0)} fp8_format={config.get('fp8_format', 'N/A')} "
        f"routes={config.get('route_mode', 'unknown')} seed={config.get('seed', 'unknown')}"
    )


PROFILING_RECORDS_PER_LAUNCH = 65536
PROFILING_MAX_RECORDS_PER_CORE = 0x03fffff8


def capture_capacity(loops, warmup):
    _integer(loops, "loops", 1)
    _integer(warmup, "warmup")
    launches = loops + warmup
    if launches > PROFILING_MAX_RECORDS_PER_CORE // PROFILING_RECORDS_PER_LAUNCH:
        raise ValueError("profiling capture capacity exceeds standard records-per-core limit")
    return launches * PROFILING_RECORDS_PER_LAUNCH


def profiling_mapping(mapping_path=None):
    if mapping_path is not None:
        return Path(mapping_path)
    module = Path(__file__).resolve()
    candidates = (module.parents[2] / "src/ep/profiling/server_dedup.mapping.json",
                  module.parents[2] / "profiling/server_dedup.mapping.json")
    for path in candidates:
        if path.is_file():
            return path
    raise ValueError("missing installed Server Dedup profiling mapping")


def _selection(config):
    return dict(skip_launches=config["warmup"], launch_count=config["loops"],
                expected_launch_count=config["warmup"] + config["loops"])


def breakdown_record(path, config, rank, capture_id, output_dir, mapping_path=None):
    """Validate producer identity using the standard reader and retain incomplete reports."""
    mapping = profiling_mapping(mapping_path)
    selection = _selection(config)
    run = load_profile_run(mapping, [Path(path)], **selection)
    if run.capture_id != str(UUID(capture_id)):
        raise ValueError("breakdown capture UUID does not match this run")
    if any(lane.rank != rank for launch in run.launches for lane in launch.lanes):
        raise ValueError("breakdown stream rank does not match its producer")
    expected_cores = set(range(64))
    if any({lane.core for lane in launch.lanes} != expected_cores for launch in run.launches):
        raise ValueError("breakdown requires all 64 core streams per launch")
    generated = generate_reports_for_run(run, Path(output_dir))
    # Common analysis owns timing semantics; EP only reduces each rank's result.
    analysis = analyze_run(run)
    interval_names = {sample.name for launch in run.launches for lane in launch.lanes
                      for sample in lane.samples if sample.kind == "interval"}
    durations = [max((sample.duration_us for lane in launch.lanes for sample in lane.samples
                      if sample.critical), default=0.0) for launch in run.launches]
    return dict(schema=RECORDS_SCHEMA, capture_id=run.capture_id, records_path=str(Path(path).resolve()),
                run_mode="breakdown", loops=config["loops"], warmup=config["warmup"],
                launches=config["loops"] + config["warmup"], measured_launches=len(run.launches),
                status=generated.status, sample_us=durations, dropped_records=run.dropped_record_count,
                diagnostics=[dict(code=item.code, message=item.message) for item in run.diagnostics],
                stages_us={item.name: dict(sample_count=item.sample_count, total=item.total_us,
                                          median=item.median_us, minimum=item.minimum_us, maximum=item.maximum_us)
                           for item in analysis.stages.values() if item.name in interval_names},
                artifacts={item.name: str(item) for item in generated.artifacts})


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


def performance_record(output_dir, loops, warmup):
    """独立 rank 的本次 profile 必须恰好包含 warmup+loops 次目标 kernel。"""
    _integer(loops, "loops", 1)
    _integer(warmup, "warmup")
    inputs = profiler.discover_profile_inputs(Path(output_dir))
    if len(inputs) != 1:
        raise ValueError(f"每 rank 需要唯一 profile 输入，发现 {len(inputs)} 个")
    samples, source = profiler.series_from_input(
        inputs[0], KERNEL_NAME, loops, strict_samples=True)
    if source["matched_samples"] != warmup + loops:
        raise ValueError(f"目标 kernel 样本数应为 {warmup + loops}，实际 {source['matched_samples']}")
    return {"kernel": KERNEL_NAME, "samples": loops, "sample_us": samples,
            "matched_samples": source.pop("matched_samples"), "source": source,
            **_statistics(samples, "_us")}


def validate_rank_measurements(record, chain_config):
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
        if evidence.get("status") != "complete" or evidence.get("measured_launches") != loops:
            raise ValueError("breakdown capture is incomplete; communication correctness is independent")
        samples = evidence.get("sample_us")
        if not isinstance(samples, list) or len(samples) != loops:
            raise ValueError("breakdown measured launch count mismatch")
        for sample in samples:
            _number(sample, "kernel duration", positive=True)
        UUID(evidence["capture_id"])
        if not isinstance(evidence.get("records_path"), str) or not evidence["records_path"]:
            raise ValueError("breakdown records path is missing")
        if not isinstance(evidence.get("stages_us"), dict):
            raise ValueError("breakdown generic stage analysis is missing")
        return
    evidence = record.get("performance")
    if not isinstance(evidence, dict) or evidence.get("kernel") != KERNEL_NAME:
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
            or any(not isinstance(name, str) or not profiler.kernel_name_matches(name, KERNEL_NAME)
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


def summarize_measurements(rank_records, chain_config, kernel_sha):
    """设备性能先取每 rank 测量轮次 median，再汇总跨 rank median/min/max。"""
    rank_size = _integer(chain_config["rank_size"], "rank_size", 1)
    if (not isinstance(rank_records, list) or len(rank_records) != rank_size
            or any(not isinstance(row, dict) or type(row.get("rank")) is not int for row in rank_records)
            or {row["rank"] for row in rank_records} != set(range(rank_size))):
        raise ValueError("测量 rank 缺失、重复或越界")
    ranks = sorted(rank_records, key=lambda row: row["rank"])
    for record in ranks:
        validate_rank_measurements(record, chain_config)
    mode = chain_config["run_mode"]
    source_send = chain_config.get("kernel_scope") == "source_send"
    dispatch = chain_config.get("kernel_scope") == "dispatch"
    kinds = ("first_hop", "direct", "relay") if dispatch else ("first_hop", "direct")
    if source_send or dispatch:
        for record in ranks:
            (validate_dispatch_traffic if dispatch else validate_source_send_traffic)(record, chain_config)
        for kind in kinds:
            if sum(row[kind + "_packets"] for row in ranks) != sum(row[kind + "_received"] for row in ranks):
                raise ValueError(kind + " 全 Rank 发送/接收包数不一致")
    count_scope = chain_config.get("kernel_scope") == "cnt_cumsum"
    summary = {"run_mode": mode, "backend": "memory", "native_status": "dispatch" if dispatch else "source_send" if source_send else "cnt_cumsum" if count_scope else "empty",
               "kernel": KERNEL_NAME, "kernel_sha256": kernel_sha,
               "rank_size": rank_size,
               "configuration": {name: chain_config[name] for name in
                                 ("bs", "h", "topk", "moe_expert_num", "dtype", "block_dim", "loops", "warmup")},
               "host_barrier": "enabled", "submission": "continuous_batch_then_sync_and_host_barrier"}
    if mode == "functional":
        return summary
    summary.update(algorithm_communication_bytes=0, bandwidth="N/A")
    if count_scope or source_send or dispatch:
        # One 32B record per remote expert per source rank; excludes self-target
        # counts, local prefix traffic and polling reads.
        remote_bytes = (chain_config["moe_expert_num"] - chain_config["moe_expert_num"] // rank_size) * 32
        summary.update(algorithm_communication_bytes=remote_bytes * rank_size,
                       communication_bytes_per_rank=remote_bytes,
                       communication_scope="remote cnt records; excludes local traffic and polling")
    if source_send or dispatch:
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
        medians = [row["performance"]["median_us"] for row in ranks]
        if count_scope:
            summary.update(bandwidth="effective remote-count GB/s",
                           bandwidth_gbps=_statistics([remote_bytes / (us * 1000) for us in medians]),
                           route_mode=chain_config["route_mode"], seed=chain_config["seed"])
        if source_send or dispatch:
            summary.update(bandwidth=("effective Dispatch with relay" if dispatch else "effective source-send") +
                               " (including local copies) plus remote-count GB/s over whole kernel",
                           bandwidth_gbps=_statistics([(sum(row[kind + "_bytes"] for kind in kinds) + remote_bytes) / (us * 1000)
                                                      for row, us in zip(ranks, medians)]))
        summary["performance"] = {"unit": "us", "selection": "last_loops_after_warmup",
                                  "reduction": "statistics_of_rank_sample_medians",
                                  "rank_median_us": medians, **_statistics(medians, "_us")}
    else:
        if len({row["breakdown"]["capture_id"] for row in ranks}) != 1:
            raise ValueError("rank captures do not share one UUID")
        medians = [statistics.median(row["breakdown"]["sample_us"]) for row in ranks]
        stage_names = sorted(set().union(*(row["breakdown"]["stages_us"] for row in ranks)))
        summary["breakdown"] = dict(schema=RECORDS_SCHEMA, selection="measured_launches_after_warmup",
            reduction="per-rank sample medians, then statistics across ranks",
            ticks_per_us=A5_SYSTEM_TICKS_PER_US, clock=dict(SYSTEM_CLOCK_SOURCE),
            kernel_us=_statistics(medians), rank_median_us=medians,
            stages_us={name: _statistics([row["breakdown"]["stages_us"][name]["median"]
                                         for row in ranks if name in row["breakdown"]["stages_us"]])
                       for name in stage_names},
            timing="Scalar arrival; existing completion fences remain; issue is not DMA completion; atomic is nested in metadata")
    return summary


def render_measurements(summary):
    """生成与结构化结果一致的简短中文报告；functional 无性能输出。"""
    if summary["run_mode"] == "functional":
        return ""
    lines = [f"单算子测量：mode={summary['run_mode']} backend=memory native={summary['native_status']} "
             f"ranks={summary['rank_size']} 算法通信={summary['algorithm_communication_bytes']}B 带宽={summary['bandwidth']}",
             f"kernel={summary['kernel']} sha256={summary['kernel_sha256']}"]

    def append(label, stats, unit, suffix=""):
        lines.append(f"{label}：median={stats['median' + suffix]:.3f} {unit} "
                     f"min={stats['minimum' + suffix]:.3f} {unit} "
                     f"max={stats['maximum' + suffix]:.3f} {unit}")

    if "performance" in summary:
        append("kernel（每 rank 测量轮次 median 的跨 rank 汇总）", summary["performance"], "us", "_us")
    if "bandwidth_gbps" in summary:
        append("有效通信带宽（字节范围见上文）", summary["bandwidth_gbps"], "GB/s")
    if "breakdown" in summary:
        breakdown = summary["breakdown"]
        append("kernel（测量轮次各核最大值的每 rank median）", breakdown["kernel_us"], "us")
        for name, stats in breakdown["stages_us"].items():
            append("标准打点区间 " + name, stats, "us")
        lines.append("Scalar 时间戳直接写 GM；issue 表示下发，atomic 包含于 metadata；performance 使用关闭打点的产物。")
    return "\n".join(lines)


def export_breakdown_traces(rank_records, chain_config, output_dir, run_id, kernel_sha, mapping_path=None):
    """Delegate all serialization to the standard report framework."""
    if chain_config["run_mode"] != "breakdown":
        raise ValueError("trace export requires breakdown records")
    ranks = sorted(rank_records, key=lambda row: row["rank"])
    if [row["rank"] for row in ranks] != list(range(chain_config["rank_size"])):
        raise ValueError("trace export requires every rank")
    mapping = profiling_mapping(mapping_path)
    selection = _selection(chain_config)
    paths = [Path(row["breakdown"]["records_path"]) for row in ranks]
    attributes = dict(run_id=run_id, kernel_sha256=kernel_sha, configuration=chain_config,
                      timing="Scalar arrival; issue timestamps are not DMA completion", backend="memory")
    output_dir = Path(output_dir).resolve()
    combined = generate_reports(mapping, paths, output_dir / "all-ranks", attributes=attributes, **selection)
    per_rank = {}
    for row, path in zip(ranks, paths):
        reports = generate_reports(mapping, [path], output_dir / ("rank-" + str(row["rank"])),
                                   attributes=attributes, **selection)
        per_rank[str(row["rank"])] = str(reports.output_dir / "perfetto_trace.json")
    order = sorted((statistics.median(row["breakdown"]["sample_us"]), row["rank"]) for row in ranks)
    selected = dict(fastest=order[0][1], p50=order[(len(order) - 1) // 2][1], slowest=order[-1][1])
    result = dict(status=combined.status, all_ranks=str(combined.output_dir / "perfetto_trace.json"),
                  report=str(combined.output_dir / "report.html"), ranks=per_rank, selected_ranks=selected,
                  selected={role: per_rank[str(rank)] for role, rank in selected.items()},
                  records=[str(path) for path in paths], mapping=str(mapping))
    (output_dir / "index.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Report retained standard profiling records; no NPU launch.")
    parser.add_argument("summary", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--mapping", type=Path)
    args = parser.parse_args()
    try:
        report = json.loads(args.summary.read_text(encoding="utf-8"))
        records = [rank for node in report["nodes"] for rank in node["reported_result"]["ranks"]]
        index = export_breakdown_traces(records, dict(report["chain_configuration"], rank_size=report["rank_size"]),
            args.output_dir or args.summary.parent / "traces", report["run_id"], report["measurements"]["kernel_sha256"], args.mapping)
        print("Trace: " + index["all_ranks"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"ERROR trace export: {error}", file=sys.stderr)
        sys.exit(1)
