#!/usr/bin/env python3
"""读取单算子测量证据；不启动 profiler、设备进程或通信。"""

import math
import json
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ep"))
import msprof_kernel_stats as profiler


KERNEL_NAME = "tilexr_ep_dispatch_memory_server_dedup_kernel"
RESULT_SEPARATOR = "=" * 32


def render_case_description(report):
    """Describe the effective case from its result, rather than guessed CLI defaults."""
    config = report.get("chain_configuration")
    if not config:
        return "checkpoint=" + report.get("checkpoint", "unknown")
    return (
        f"Dispatch server-dedup（空 kernel） mode={config['run_mode']} "
        f"loops={config['loops']} warmup={config['warmup']}\n"
        f"BS={config['bs']} H={config['h']} TopK={config['topk']} "
        f"experts={config['moe_expert_num']} dtype={config['dtype']} "
        f"cards={report.get('rank_size', 'unknown')} quant_mode={config.get('quant_mode', 0)}"
    )


BREAKDOWN_STAGES = ("entry_checks", "record_prepare", "empty_total")
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


def _breakdown_values(record):
    evidence = record.get("breakdown")
    if not isinstance(evidence, dict):
        raise ValueError("缺少 breakdown 设备记录")
    _integer(evidence.get("generation"), "breakdown generation", 1)
    cores = _integer(evidence.get("block_dim"), "breakdown block_dim", 1)
    rows = evidence.get("records")
    if (not isinstance(rows, list) or len(rows) != cores
            or any(not isinstance(row, dict) or type(row.get("core")) is not int for row in rows)
            or {row["core"] for row in rows} != set(range(cores))):
        raise ValueError("breakdown core 记录缺失、重复或越界")
    samples = {name: [] for name in BREAKDOWN_STAGES}
    for row in rows:
        start, checked, end = [_integer(row.get(key), "breakdown " + key, 1)
                               for key in ("start", "checked", "end")]
        if not start <= checked <= end or start == end or end > (1 << 64) - 1:
            raise ValueError("breakdown 时间戳无效或顺序错误")
        for name, value in zip(BREAKDOWN_STAGES, (checked - start, end - checked, end - start)):
            samples[name].append(value)
    return {name: max(values) for name, values in samples.items()}


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
        _breakdown_values(record)
        evidence = record["breakdown"]
        if (evidence.get("run_mode") != mode or evidence["generation"] != warmup + loops
                or any(_integer(evidence.get(key), "breakdown " + key) != value
                       for key, value in (("loops", loops), ("warmup", warmup)))):
            raise ValueError("breakdown 模式、批次或 generation 与本次配置不一致")
        if chain_config["block_dim"] != "auto" and evidence["block_dim"] != int(chain_config["block_dim"]):
            raise ValueError("breakdown block_dim 与本次配置不一致")
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
    summary = {"run_mode": mode, "backend": "memory", "native_status": "empty",
               "kernel": KERNEL_NAME, "kernel_sha256": kernel_sha,
               "rank_size": rank_size,
               "configuration": {name: chain_config[name] for name in
                                 ("bs", "h", "topk", "moe_expert_num", "dtype", "block_dim", "loops", "warmup")},
               "host_barrier": "enabled", "submission": "continuous_batch_then_sync_and_host_barrier"}
    if mode == "functional":
        return summary
    summary.update(algorithm_communication_bytes=0, bandwidth="N/A")
    if mode == "performance":
        medians = [row["performance"]["median_us"] for row in ranks]
        summary["performance"] = {"unit": "us", "selection": "last_loops_after_warmup",
                                  "reduction": "statistics_of_rank_sample_medians",
                                  "rank_median_us": medians, **_statistics(medians, "_us")}
    else:
        values = [_breakdown_values(row) for row in ranks]
        breakdown = {"unit": "ticks", "selection": "final_measured_iteration",
                     "reduction": "core_max_per_stage_then_statistics_across_ranks",
                     "stages": {name: _statistics([row[name] for row in values]) for name in BREAKDOWN_STAGES},
                     "rank_stages": [{"rank": row["rank"], **value} for row, value in zip(ranks, values)],
                     "scope": "empty entry checks and record preparation; final record write is not timed"}
        breakdown["ticks_per_us"] = A5_SYSTEM_TICKS_PER_US
        breakdown["clock"] = dict(SYSTEM_CLOCK_SOURCE)
        breakdown["stages_us"] = {name: {key: value / A5_SYSTEM_TICKS_PER_US for key, value in stats.items()}
                                  for name, stats in breakdown["stages"].items()}
        summary["breakdown"] = breakdown
    return summary


def render_measurements(summary):
    """生成与结构化结果一致的简短中文报告；functional 无性能输出。"""
    if summary["run_mode"] == "functional":
        return ""
    lines = [f"单算子测量：mode={summary['run_mode']} backend=memory native=empty "
             f"ranks={summary['rank_size']} 算法通信=0B 带宽=N/A",
             f"kernel={summary['kernel']} sha256={summary['kernel_sha256']}"]

    def append(label, stats, unit, suffix=""):
        lines.append(f"{label}：median={stats['median' + suffix]:.3f} {unit} "
                     f"min={stats['minimum' + suffix]:.3f} {unit} "
                     f"max={stats['maximum' + suffix]:.3f} {unit}")

    if "performance" in summary:
        append("空 kernel（每 rank 测量轮次 median 的跨 rank 汇总）", summary["performance"], "us", "_us")
    if "breakdown" in summary:
        breakdown = summary["breakdown"]
        lines.append("GetSystemCycle：Ascend950PR/950DT，1 GHz，1000 ticks/us；原始 ticks 保留在 JSON。")
        for name in BREAKDOWN_STAGES:
            append("最后一轮各核最大值 " + name, breakdown["stages_us"][name], "us")
        lines.append("breakdown 仅为空入口检查和记录准备，未计量最终记录写出；各段最大值不保证可相加。")
    return "\n".join(lines)


def export_breakdown_traces(rank_records, chain_config, output_dir, run_id, kernel_sha):
    """Export every card and Memory-style fastest/p50/slowest Chrome traces."""
    summary = summarize_measurements(rank_records, chain_config, kernel_sha)
    if summary["run_mode"] != "breakdown":
        raise ValueError("trace export requires breakdown records")
    ranks = sorted(rank_records, key=lambda row: row["rank"])
    order = sorted((max(core["end"] - core["start"] for core in row["breakdown"]["records"])
                    / A5_SYSTEM_TICKS_PER_US, row["rank"]) for row in ranks)
    selected = dict(fastest=order[0][1], p50=order[(len(order) - 1) // 2][1], slowest=order[-1][1])
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    provenance = dict(run_id=run_id, kernel_sha256=kernel_sha, configuration=chain_config,
                      selection="final_measured_iteration", generation=chain_config["warmup"] + chain_config["loops"],
                      clock_alignment="each rank normalized to its earliest core start; not synchronized across NPUs",
                      selection_rule="max core end-start per rank; ascending duration, ties by rank; p50=ceil(N/2)-1",
                      clock=SYSTEM_CLOCK_SOURCE, native_status="empty",
                      scope="entry checks and record preparation only; final record write excluded")

    def write(name, events, **extra):
        path = output_dir / name
        path.write_text(json.dumps(dict(traceEvents=events, displayTimeUnit="ns",
                                        otherData=dict(provenance, **extra)), indent=2) + "\n", encoding="utf-8")
        return str(path)

    per_rank, rank_events, all_events = {}, {}, []
    for row in ranks:
        rank = row["rank"]
        cores = sorted(row["breakdown"]["records"], key=lambda core: core["core"])
        origin = min(core["start"] for core in cores)
        events = [dict(ph="M", name="process_name", pid=rank, tid=0,
                       args=dict(name=f"Dispatch rank {rank} host={row.get('host', 'unknown')} device={row.get('device', 'unknown')}"))]
        for core in cores:
            tid = core["core"]
            events.append(dict(ph="M", name="thread_name", pid=rank, tid=tid, args=dict(name=f"AIV core {tid}")))
            for name, begin, end in (("entry_checks", core["start"], core["checked"]),
                                     ("record_prepare", core["checked"], core["end"])):
                events.append(dict(ph="X", cat="kernel", name=name, pid=rank, tid=tid,
                                   ts=(begin - origin) / A5_SYSTEM_TICKS_PER_US,
                                   dur=(end - begin) / A5_SYSTEM_TICKS_PER_US,
                                   args=dict(start_ticks=begin, end_ticks=end, generation=row["breakdown"]["generation"])))
        rank_events[rank] = events
        all_events.extend(events)
        per_rank[str(rank)] = write(f"rank-{rank}.trace.json", events, rank=rank, origin_ticks=origin)
    selection_files = {role: write(f"{role}.trace.json", rank_events[rank], role=role, rank=rank)
                       for role, rank in selected.items()}
    index = dict(all_ranks=write("all-ranks.trace.json", all_events), ranks=per_rank,
                 selected_ranks=selected, selected=selection_files,
                 rank_order=[dict(rank=rank, elapsed_us=elapsed) for elapsed, rank in order])
    (output_dir / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    return index


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Export Chrome traces from an existing breakdown summary; no NPU launch.")
    parser.add_argument("summary", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    try:
        report = json.loads(args.summary.read_text(encoding="utf-8"))
        if report["status"] != "PASS" or report.get("errors"):
            raise ValueError("requires a successful breakdown summary")
        records = [rank for node in report["nodes"] for rank in node["reported_result"]["ranks"]]
        index = export_breakdown_traces(records, dict(report["chain_configuration"], rank_size=report["rank_size"]),
            args.output_dir or args.summary.parent / "traces", report["run_id"], report["measurements"]["kernel_sha256"])
        print("\n" + RESULT_SEPARATOR + "\nPASS\nTrace: " + index["all_ranks"] + "\n" + RESULT_SEPARATOR + "\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"ERROR trace export: {error}", file=sys.stderr)
        sys.exit(1)
