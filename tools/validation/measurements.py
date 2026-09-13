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


BREAKDOWN_STAGES = ("entry_checks", "record_prepare", "empty_total")
COUNT_POINTS = ("start", "init_end", "send_cnt_end", "recv_cnt_end", "cumsum_end", "prefix_end", "end")
COUNT_STAGES = ("init", "send_cnt", "recv_cnt", "cumsum", "prefix_consume", "finish", "count_total")
SOURCE_SEND_POINTS = ("start", "init_end", "send_cnt_end", "send_data_end", "recv_cnt_end", "cumsum_end", "prefix_end", "end")
SOURCE_SEND_STAGES = ("init", "send_cnt", "send_data", "recv_cnt", "cumsum", "prefix_consume", "finish", "source_send_total")
SEND_DETAIL_STAGES = ("send_prepare", "send_first_hop", "send_direct")
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


def _breakdown_spans(row, count_scope, source_send=False):
    points = SOURCE_SEND_POINTS if source_send else COUNT_POINTS if count_scope else ("start", "checked", "end")
    values = [_integer(row.get(key), "breakdown " + key, 1) for key in points]
    if values != sorted(values) or values[0] == values[-1] or values[-1] > (1 << 64) - 1:
        raise ValueError("breakdown 时间戳无效或顺序错误")
    names = SOURCE_SEND_STAGES if source_send else COUNT_STAGES if count_scope else BREAKDOWN_STAGES
    return list(zip(names[:-1], values[:-1], values[1:])) + [(names[-1], values[0], values[-1])]


def _breakdown_values(record, chain_config=None):
    evidence = record.get("breakdown")
    if not isinstance(evidence, dict):
        raise ValueError("缺少 breakdown 设备记录")
    _integer(evidence.get("generation"), "breakdown generation", 1)
    cores = _integer(evidence.get("block_dim"), "breakdown block_dim", 1)
    source_send = evidence.get("schema_version") in (3, 4)
    if source_send and (cores != 64 or evidence.get("kernel_scope") != "source_send"):
        raise ValueError("source-send breakdown 需要 64 核及正确 scope")
    count_scope = evidence.get("schema_version") == 2
    if count_scope and (cores != 64 or evidence.get("kernel_scope") != "cnt_cumsum"):
        raise ValueError("cnt/cumsum breakdown 需要 64 核及正确 scope")
    rows = evidence.get("records")
    if (not isinstance(rows, list) or len(rows) != cores
            or any(not isinstance(row, dict) or type(row.get("core")) is not int for row in rows)
            or {row["core"] for row in rows} != set(range(cores))):
        raise ValueError("breakdown core 记录缺失、重复或越界")
    samples = {name: [] for name in (SOURCE_SEND_STAGES if source_send else COUNT_STAGES if count_scope else BREAKDOWN_STAGES)}
    for row in rows:
        for name, start, end in _breakdown_spans(row, count_scope, source_send):
            samples[name].append(end - start)
    if evidence.get("schema_version") == 4:
        if chain_config is not None:
            _validate_send_details(record, chain_config)
        for name in SEND_DETAIL_STAGES:
            samples[name] = [end - begin for row in rows if row["core"] < 48
                             for stage, begin, end in _send_detail_spans(row) if stage == name]
    return {name: max(values) for name, values in samples.items()}


def _send_detail_spans(core):
    packets = core["send_packets"]
    first_hop = core["first_hop_count"]
    begin, end = core["send_cnt_end"], core["send_data_end"]
    first = packets[0]["issue"] if packets else end
    direct = packets[first_hop]["issue"] if first_hop < len(packets) else end
    return list(zip(SEND_DETAIL_STAGES, (begin, first, direct), (first, direct, end)))


def _validate_send_details(record, config):
    """Reject incomplete detail before rendering; never invent missing issue times."""
    bs = _integer(config.get("bs"), "bs", 1)
    k = _integer(config.get("topk"), "topk", 1)
    totals = {"first_hop": 0, "direct": 0}
    for core in record["breakdown"]["records"]:
        cid = core["core"]
        if cid >= 48:
            if core.get("send_packets") or core.get("first_hop_count", 0):
                raise ValueError("接收核不能包含源端发送打点")
            continue
        packets = core.get("send_packets")
        first_hop = _integer(core.get("first_hop_count"), "first_hop_count")
        count = bs // 48 + (cid < bs % 48)
        start = cid * (bs // 48) + min(cid, bs % 48)
        if (not isinstance(packets, list) or not count <= len(packets) <= count * k or first_hop > len(packets)):
            raise ValueError("send_packets 缺失或包数超出本核 Token 范围")
        previous = core["send_cnt_end"]
        routes, tokens = set(), set()
        for packet in packets:
            if not isinstance(packet, dict):
                raise ValueError("无效 send_packets 记录")
            issue = _integer(packet.get("issue"), "MTE3 issue", 1)
            token = _integer(packet.get("token"), "send token")
            route = _integer(packet.get("k"), "send k")
            if (not previous <= issue <= core["send_data_end"] or not start <= token < start + count
                    or route >= k or (token, route) in routes):
                raise ValueError("MTE3 下发时间点或 Token/k 无效、越界、重复")
            previous = issue
            routes.add((token, route))
            tokens.add(token)
        if len(tokens) != count:
            raise ValueError("send_packets 遗漏本核 Token")
        totals["first_hop"] += first_hop
        totals["direct"] += len(packets) - first_hop
    if any(record.get(kind + "_packets") != total for kind, total in totals.items()):
        raise ValueError("send_packets 与实际 first-hop/DIRECT 包数不一致")


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
        evidence = record.get("breakdown", {})
        if not isinstance(evidence, dict):
            raise ValueError("缺少 breakdown 设备记录")
        if chain_config.get("kernel_scope") == "source_send" and evidence.get("schema_version") not in (3, 4):
            raise ValueError("source-send 必须使用 breakdown v3/v4")
        if evidence.get("schema_version") in (3, 4) and chain_config.get("kernel_scope") != "source_send":
            raise ValueError("breakdown scope 与本次配置不一致")
        if chain_config.get("kernel_scope") == "cnt_cumsum" and evidence.get("schema_version") != 2:
            raise ValueError("cnt/cumsum 不能使用旧空 kernel breakdown")
        if evidence.get("schema_version") == 2 and chain_config.get("kernel_scope") != "cnt_cumsum":
            raise ValueError("breakdown scope 与本次配置不一致")
        _breakdown_values(record, chain_config)
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


def validate_source_send_traffic(record, config):
    stride = _integer(record.get("token_stride"), "token_stride", 1)
    for kind in ("first_hop", "direct"):
        packets = _integer(record.get(kind + "_packets"), kind + "_packets")
        _integer(record.get(kind + "_received"), kind + "_received")
        if _integer(record.get(kind + "_bytes"), kind + "_bytes") != packets * stride:
            raise ValueError(kind + " bytes 与包数/stride 不一致")
    if not 0 < record["first_hop_packets"] + record["direct_packets"] <= config["bs"] * config["topk"]:
        raise ValueError("源端物理包数超出原始 route 范围")
    local, same, cross = [_integer(record.get(key + "_data_bytes"), key + "_data_bytes")
                          for key in ("local", "same_server", "cross_server")]
    if (any(value % stride for value in (local, same, cross)) or
            local + same + cross != record["first_hop_bytes"] + record["direct_bytes"] or
            cross < record["first_hop_bytes"]):
        raise ValueError("本卡/跨卡/跨 Server 流量与物理包数不一致")


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
    if source_send:
        for record in ranks:
            validate_source_send_traffic(record, chain_config)
        for kind in ("first_hop", "direct"):
            if sum(row[kind + "_packets"] for row in ranks) != sum(row[kind + "_received"] for row in ranks):
                raise ValueError(kind + " 全 Rank 发送/接收包数不一致")
    count_scope = chain_config.get("kernel_scope") == "cnt_cumsum"
    summary = {"run_mode": mode, "backend": "memory", "native_status": "source_send" if source_send else "cnt_cumsum" if count_scope else "empty",
               "kernel": KERNEL_NAME, "kernel_sha256": kernel_sha,
               "rank_size": rank_size,
               "configuration": {name: chain_config[name] for name in
                                 ("bs", "h", "topk", "moe_expert_num", "dtype", "block_dim", "loops", "warmup")},
               "host_barrier": "enabled", "submission": "continuous_batch_then_sync_and_host_barrier"}
    if mode == "functional":
        return summary
    summary.update(algorithm_communication_bytes=0, bandwidth="N/A")
    if count_scope or source_send:
        # One 32B record per remote expert per source rank; excludes self-target
        # counts, local prefix traffic and polling reads.
        remote_bytes = (chain_config["moe_expert_num"] - chain_config["moe_expert_num"] // rank_size) * 32
        summary.update(algorithm_communication_bytes=remote_bytes * rank_size,
                       communication_bytes_per_rank=remote_bytes,
                       communication_scope="remote cnt records; excludes local traffic and polling")
    if source_send:
        summary.pop("communication_bytes_per_rank", None)
        traffic = [{"rank": row["rank"], "original_routes": chain_config["bs"] * chain_config["topk"],
                    "first_hop_packets": row["first_hop_packets"], "direct_packets": row["direct_packets"],
                    "first_hop_bytes": row["first_hop_bytes"], "direct_bytes": row["direct_bytes"],
                    "token_stride": row["token_stride"], "cross_server_data_bytes": row["cross_server_data_bytes"],
                    "same_server_data_bytes": row["same_server_data_bytes"], "local_data_bytes": row["local_data_bytes"],
                    "remote_count_bytes": remote_bytes,
                    "cross_server_count_bytes": (chain_config["moe_expert_num"] - chain_config["moe_expert_num"] // rank_size * 8) * 32,
                    "same_server_count_bytes": chain_config["moe_expert_num"] // rank_size * 7 * 32,
                    "local_count_bytes": chain_config["moe_expert_num"] // rank_size * 32}
                   for row in ranks]
        summary.update(submission="single_launch_then_sync_and_host_barrier", measurement="single_cold_submission",
                       route_mode=chain_config["route_mode"], seed=chain_config["seed"],
                       quant_mode=chain_config["quant_mode"], fp8_format=chain_config["fp8_format"],
                       rank_traffic=traffic,
                       algorithm_communication_bytes=sum(row["first_hop_bytes"] + row["direct_bytes"] + remote_bytes for row in ranks),
                       communication_scope="first-hop and DIRECT full T including local copies, plus remote cnt records; excludes polling/local prefix traffic")
    if mode == "performance":
        medians = [row["performance"]["median_us"] for row in ranks]
        if count_scope:
            summary.update(bandwidth="effective remote-count GB/s",
                           bandwidth_gbps=_statistics([remote_bytes / (us * 1000) for us in medians]),
                           route_mode=chain_config["route_mode"], seed=chain_config["seed"])
        if source_send:
            summary.update(bandwidth="effective source-send (including local copies) plus remote-count GB/s over whole kernel",
                           bandwidth_gbps=_statistics([(row["first_hop_bytes"] + row["direct_bytes"] + remote_bytes) / (us * 1000)
                                                      for row, us in zip(ranks, medians)]))
        summary["performance"] = {"unit": "us", "selection": "last_loops_after_warmup",
                                  "reduction": "statistics_of_rank_sample_medians",
                                  "rank_median_us": medians, **_statistics(medians, "_us")}
    else:
        if len({row["breakdown"].get("schema_version") for row in ranks}) != 1:
            raise ValueError("各 rank breakdown 版本不一致")
        values = [_breakdown_values(row) for row in ranks]
        stage_names = values[0].keys()
        breakdown = {"unit": "ticks", "selection": "final_measured_iteration",
                     "reduction": "core_max_per_stage_then_statistics_across_ranks",
                     "stages": {name: _statistics([row[name] for row in values]) for name in stage_names},
                     "rank_stages": [{"rank": row["rank"], **value} for row, value in zip(ranks, values)],
                     "scope": "empty entry checks and record preparation; final record write is not timed"}
        if count_scope:
            breakdown.update(schema_version=2, scope="cnt/cumsum stages; final trace write excluded",
                             phase_barriers="PIPE_ALL at timestamp boundaries; diagnostic build only")
        if source_send:
            version = ranks[0]["breakdown"]["schema_version"]
            breakdown.update(schema_version=version,
                             scope="cnt/cumsum and source send; finish includes receiver work and final synchronization; final trace writes excluded"
                                   if version == 4 else "cnt/cumsum plus first-hop and DIRECT send; final trace write excluded",
                             phase_barriers="existing coarse boundaries use PIPE_ALL; packet issue samples add no barriers or events"
                                            if version == 4 else "PIPE_ALL at timestamp boundaries; diagnostic build only")
            if version == 4:
                breakdown["send_packet_timing"] = "sample immediately after MTE3 issue; interval to next packet issue, not DMA completion; final tail is cleanup"
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
        lines.append("GetSystemCycle：Ascend950PR/950DT，1 GHz，1000 ticks/us；原始 ticks 保留在 JSON。")
        for name in breakdown["stages_us"]:
            append("最后一轮各核最大值 " + name, breakdown["stages_us"][name], "us")
        if breakdown.get("schema_version") in (2, 3, 4):
            lines.append("各阶段含诊断同步；未计量最终记录写出，各段最大值不保证可相加；性能基线使用 performance 产物。")
            if breakdown.get("schema_version") == 4:
                lines.append("Token 段表示相邻 MTE3 下发点间隔，不等于搬运完成耗时；逐包打点未新增同步，最后一包之后单列收尾。")
        else:
            lines.append("breakdown 仅为空入口检查和记录准备，未计量最终记录写出；各段最大值不保证可相加。")
    return "\n".join(lines)


def export_breakdown_traces(rank_records, chain_config, output_dir, run_id, kernel_sha):
    """Export every card and Memory-style fastest/p50/slowest Chrome traces."""
    summary = summarize_measurements(rank_records, chain_config, kernel_sha)
    if summary["run_mode"] != "breakdown":
        raise ValueError("trace export requires breakdown records")
    ranks = sorted(rank_records, key=lambda row: row["rank"])
    source_send = summary["native_status"] == "source_send"
    count_scope = summary["native_status"] == "cnt_cumsum"
    order = sorted((max(core["end"] - core["start"] for core in row["breakdown"]["records"])
                    / A5_SYSTEM_TICKS_PER_US, row["rank"]) for row in ranks)
    selected = dict(fastest=order[0][1], p50=order[(len(order) - 1) // 2][1], slowest=order[-1][1])
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    provenance = dict(run_id=run_id, kernel_sha256=kernel_sha, configuration=chain_config,
                      selection="final_measured_iteration", generation=chain_config["warmup"] + chain_config["loops"],
                      clock_alignment="each rank normalized to its earliest core start; not synchronized across NPUs",
                      selection_rule="max core end-start per rank; ascending duration, ties by rank; p50=ceil(N/2)-1",
                      clock=SYSTEM_CLOCK_SOURCE, native_status=summary["native_status"],
                      scope=summary["breakdown"]["scope"])
    if count_scope or source_send:
        provenance.update(schema_version=summary["breakdown"]["schema_version"], phase_barriers=summary["breakdown"]["phase_barriers"])
        if summary["breakdown"].get("schema_version") == 4:
            provenance["send_packet_timing"] = summary["breakdown"]["send_packet_timing"]

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
            for name, begin, end in _breakdown_spans(core, count_scope, source_send)[:-1]:
                events.append(dict(ph="X", cat="kernel", name=name, pid=rank, tid=tid,
                                   ts=(begin - origin) / A5_SYSTEM_TICKS_PER_US,
                                   dur=(end - begin) / A5_SYSTEM_TICKS_PER_US,
                                   args=dict(start_ticks=begin, end_ticks=end, generation=row["breakdown"]["generation"])))
            if row["breakdown"].get("schema_version") == 4 and tid < 48:
                events.extend(_send_detail_events(core, rank, origin, row["breakdown"]["generation"]))
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


def _send_detail_events(core, rank, origin, generation):
    """Nest the three phases and issue intervals under the existing send_data span."""
    events = []

    def span(name, begin, end, category, **args):
        events.append(dict(ph="X", cat=category, name=name, pid=rank, tid=core["core"],
                           ts=(begin - origin) / A5_SYSTEM_TICKS_PER_US, dur=(end - begin) / A5_SYSTEM_TICKS_PER_US,
                           args=dict(start_ticks=begin, end_ticks=end, generation=generation, **args)))

    for name, begin, end in _send_detail_spans(core):
        span(name, begin, end, "send_phase")
    packets = core["send_packets"]
    for index, packet in enumerate(packets):
        kind = "first_hop" if index < core["first_hop_count"] else "direct"
        args = dict(packet_index=index, kind=kind, token=packet["token"], k=packet["k"], issue_ticks=packet["issue"])
        # Every physical packet retains a point, including the final packet.
        events.append(dict(ph="i", s="t", cat="mte3_issue", name=f"{kind} MTE3 issue", pid=rank, tid=core["core"],
                           ts=(packet["issue"] - origin) / A5_SYSTEM_TICKS_PER_US, args=dict(generation=generation, **args)))
        if index + 1 < len(packets):
            nxt = packets[index + 1]
            span(f"{kind} token={packet['token']} k={packet['k']} → next issue", packet["issue"], nxt["issue"],
                 "send_issue_interval", next_token=nxt["token"], next_k=nxt["k"], **args)
        else:
            span("send_drain_and_outputs", packet["issue"], core["send_data_end"], "send_cleanup",
                 timing="last issue to send_data_end; not an issue-to-issue interval", **args)
    return events


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
