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
DISPATCH_POINTS = ("start", "init_end", "send_cnt_end", "send_data_end", "recv_cnt_end", "cumsum_end", "prefix_end", "recv_data_end", "end")
DISPATCH_STAGES = ("init", "send_cnt", "send_data", "recv_cnt", "cumsum", "prefix_consume", "recv_data", "finish", "dispatch_total")
SEND_DETAIL_STAGES = ("send_prepare", "send_first_hop", "send_direct")
DETAIL_POINTS = ("recv_begin", "prefix_end", "tasks_end", "prepare_end", "loop_end", "drain_end",
                 "clear_issue_end", "clear_end", "finish_begin", "local_end", "sync_end", "counter_end",
                 "publish_end", "wait_end")
RECV_DETAIL_STAGES = ("recv_prefix", "recv_tasks", "recv_prepare", "recv_loop", "recv_drain_issue",
                      "recv_clear_issue", "recv_write_completion")
FINISH_DETAIL_STAGES = ("finish_local", "finish_sync", "finish_counter", "finish_publish", "finish_peer_wait")
DETAIL_METRICS = ("probe", "acquire_load", "metadata", "atomic", "output_issue", "relay_issue", "round_peer_wait")
DETAIL_COUNTERS = ("rows", "probe_batches", "probe_slots", "empty_probe_batches", "first_hop_rows",
                   "relay_packets", "lanes", "blocks", "round_polls", "round_slowest_peer")
DETAIL_TIMING = (
    "passive Scalar timestamps; no additional synchronization; acquire_load includes lane MTE3 dependency and "
    "packet MTE2 completion; output_issue/relay_issue measure submission, not DMA completion; atomic is nested "
    "in metadata; recv_drain_issue submits MTE2 waits, recv_write_completion covers remaining data and Flag writes "
    "at the existing PIPE_ALL; operation totals are sums, not sequential timeline durations; max spans are real intervals")
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


def _breakdown_spans(row, count_scope, source_send=False, dispatch=False):
    points = DISPATCH_POINTS if dispatch else SOURCE_SEND_POINTS if source_send else COUNT_POINTS if count_scope else ("start", "checked", "end")
    values = [_integer(row.get(key), "breakdown " + key, 1) for key in points]
    if values != sorted(values) or values[0] == values[-1] or values[-1] > (1 << 64) - 1:
        raise ValueError("breakdown 时间戳无效或顺序错误")
    names = DISPATCH_STAGES if dispatch else SOURCE_SEND_STAGES if source_send else COUNT_STAGES if count_scope else BREAKDOWN_STAGES
    return list(zip(names[:-1], values[:-1], values[1:])) + [(names[-1], values[0], values[-1])]


def _breakdown_values(record, chain_config=None):
    evidence = record.get("breakdown")
    if not isinstance(evidence, dict):
        raise ValueError("缺少 breakdown 设备记录")
    _integer(evidence.get("generation"), "breakdown generation", 1)
    cores = _integer(evidence.get("block_dim"), "breakdown block_dim", 1)
    dispatch = evidence.get("schema_version") in (5, 6)
    if dispatch and (cores != 64 or evidence.get("kernel_scope") != "dispatch"):
        raise ValueError("Dispatch breakdown 需要 64 核及正确 scope")
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
    samples = {name: [] for name in (DISPATCH_STAGES if dispatch else SOURCE_SEND_STAGES if source_send else COUNT_STAGES if count_scope else BREAKDOWN_STAGES)}
    for row in rows:
        for name, start, end in _breakdown_spans(row, count_scope, source_send, dispatch):
            samples[name].append(end - start)
    if evidence.get("schema_version") in (4, 5, 6):
        if chain_config is not None:
            _validate_send_details(record, chain_config)
        for name in SEND_DETAIL_STAGES:
            samples[name] = [end - begin for row in rows if row["core"] < 48
                             for stage, begin, end in _send_detail_spans(row) if stage == name]
    if evidence.get("schema_version") == 6:
        if chain_config is not None:
            _validate_receive_details(record, chain_config)
        for name in RECV_DETAIL_STAGES + FINISH_DETAIL_STAGES:
            samples[name] = [end - begin for row in rows
                             for stage, begin, end in _receive_detail_spans(row) if stage == name]
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


def _receive_detail_spans(core):
    points = core["detail"]["points"]
    spans = []
    if core["core"] >= 48:
        values = [points[name] for name in DETAIL_POINTS[:8]]
        spans.extend(zip(RECV_DETAIL_STAGES, values[:-1], values[1:]))
    values = [points[name] for name in DETAIL_POINTS[8:]]
    spans.extend(zip(FINISH_DETAIL_STAGES, values[:-1], values[1:]))
    return spans


def _validate_receive_details(record, config):
    """Validate v6 evidence without inferring missing points or asynchronous DMA durations."""
    received = sum(_integer(record.get(kind + "_received"), kind + "_received")
                   for kind in ("first_hop", "direct", "relay"))
    # Node validation receives a shape-only config; its record rank_size has
    # already been checked against the launch mapping by validate_rank_records.
    rank_size = _integer(config.get("rank_size", record.get("rank_size")), "rank_size", 1)
    stride = _integer(record.get("token_stride"), "token_stride", 512)
    if stride % 512:
        raise ValueError("detail token_stride 必须以 512B 对齐")
    first_hop_rows = relay_packets = 0
    for core in record["breakdown"]["records"]:
        detail = core.get("detail")
        if not isinstance(detail, dict) or set(detail) != {"points", "metrics", "counters"}:
            raise ValueError("缺少完整的 recv/finish detail")
        expected = (("points", DETAIL_POINTS), ("metrics", DETAIL_METRICS), ("counters", DETAIL_COUNTERS))
        for key, names in expected:
            if not isinstance(detail.get(key), dict) or set(detail[key]) != set(names):
                raise ValueError("detail 字段缺失或未知：" + key)
        points, metrics, counters = (detail[name] for name in ("points", "metrics", "counters"))
        for name, value in list(points.items()) + list(counters.items()):
            if _integer(value, "detail " + name) >= 1 << 64:
                raise ValueError("detail uint64 越界：" + name)
        cid = core["core"]
        finish = [points[name] for name in DETAIL_POINTS[8:]]
        if finish != sorted(finish) or not core["recv_data_end"] <= finish[0] <= finish[-1] <= core["end"]:
            raise ValueError("finish detail 时间戳顺序或父区间无效")
        if cid != 63 and any(points[name] != points["sync_end"] for name in ("counter_end", "publish_end", "wait_end")):
            raise ValueError("非 63 核不能包含 counter/round completion 操作")
        rows = received * (cid - 47) // 16 - received * (cid - 48) // 16 if cid >= 48 else 0
        if counters["rows"] != rows:
            raise ValueError("detail rows 与接收任务分区不一致")
        if cid >= 48:
            recv = [points[name] for name in DETAIL_POINTS[:8]]
            if recv != sorted(recv) or not core["prefix_end"] <= recv[0] <= recv[-1] <= core["recv_data_end"]:
                raise ValueError("recv detail 时间戳顺序或父区间无效")
            if not rows and any(points[name] != points["prepare_end"] for name in DETAIL_POINTS[4:8]):
                raise ValueError("空接收核不能包含 loop/drain/clear 耗时")
        elif any(points[name] for name in DETAIL_POINTS[:8]):
            raise ValueError("发送核不能包含 recv detail")
        recv_counters = DETAIL_COUNTERS[:8]
        if not rows:
            if any(counters[name] for name in recv_counters):
                raise ValueError("无接收任务的核不能包含 recv counter")
        elif (not 1 <= counters["lanes"] <= 8 or counters["blocks"] != stride // 512
              or counters["probe_batches"] < 1
              or not rows <= counters["probe_slots"] <= 16 * counters["probe_batches"]
              or counters["empty_probe_batches"] >= counters["probe_batches"]
              or counters["first_hop_rows"] > rows
              or counters["relay_packets"] < counters["first_hop_rows"]
              or counters["relay_packets"] > counters["first_hop_rows"] * (config["topk"] - 1)):
            raise ValueError("recv detail counter 范围或关联无效")
        if cid == 63:
            if counters["round_polls"] < rank_size or counters["round_slowest_peer"] >= rank_size:
                raise ValueError("round completion counter 无效")
        elif counters["round_polls"] or counters["round_slowest_peer"]:
            raise ValueError("非 63 核不能包含 round completion counter")
        counts = dict(probe=counters["probe_batches"], acquire_load=rows, metadata=rows, output_issue=rows,
                      atomic=counters["first_hop_rows"], relay_issue=counters["first_hop_rows"],
                      round_peer_wait=rank_size if cid == 63 else 0)
        for name, metric in metrics.items():
            if not isinstance(metric, dict) or set(metric) != {"total_ticks", "max_begin", "max_end", "count"}:
                raise ValueError("detail metric 字段无效：" + name)
            for key, value in metric.items():
                if _integer(value, name + " " + key) >= 1 << 64:
                    raise ValueError("detail metric uint64 越界")
            if metric["count"] != counts[name]:
                raise ValueError("detail metric count 与 counter 不一致：" + name)
            if not metric["count"]:
                if any(metric.values()):
                    raise ValueError("未使用的 detail metric 必须全零：" + name)
                continue
            begin, end = ((points["publish_end"], points["wait_end"]) if name == "round_peer_wait"
                          else (points["prepare_end"], points["loop_end"]))
            duration = metric["max_end"] - metric["max_begin"]
            if (not begin <= metric["max_begin"] <= metric["max_end"] <= end
                    or not duration <= metric["total_ticks"] <= min(end - begin, duration * metric["count"])):
                raise ValueError("detail metric 最大区间或累计耗时无效：" + name)
        if (metrics["atomic"]["total_ticks"] > metrics["metadata"]["total_ticks"]
                or metrics["atomic"]["max_end"] - metrics["atomic"]["max_begin"] >
                metrics["metadata"]["max_end"] - metrics["metadata"]["max_begin"]):
            raise ValueError("atomic 耗时必须属于 metadata 耗时")
        if sum(metrics[name]["total_ticks"] for name in DETAIL_METRICS[:-1] if name != "atomic") > (
                points["loop_end"] - points["prepare_end"]):
            raise ValueError("接收操作累计耗时超过 recv_loop")
        first_hop_rows += counters["first_hop_rows"]
        relay_packets += counters["relay_packets"]
    if first_hop_rows != record["first_hop_received"] or relay_packets != record["relay_packets"]:
        raise ValueError("detail first-hop/relay 总数与流量不一致")


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
        if chain_config.get("kernel_scope") == "dispatch" and evidence.get("schema_version") not in (5, 6):
            raise ValueError("完整 Dispatch 必须使用 breakdown v5/v6")
        if evidence.get("schema_version") in (5, 6) and chain_config.get("kernel_scope") != "dispatch":
            raise ValueError("breakdown scope 与本次配置不一致")
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


def _summarize_receive_details(ranks):
    operations, rank_operations = {}, []
    for row in ranks:
        cores = row["breakdown"]["records"]
        metrics = {}
        for name in DETAIL_METRICS:
            values = [core["detail"]["metrics"][name] for core in cores]
            metrics[name] = dict(core_total_ticks=max(value["total_ticks"] for value in values),
                                 all_core_total_ticks=sum(value["total_ticks"] for value in values),
                                 max_interval_ticks=max(value["max_end"] - value["max_begin"] for value in values),
                                 count=sum(value["count"] for value in values))
        rank_operations.append(dict(rank=row["rank"], metrics=metrics,
                                    counters={name: sum(core["detail"]["counters"][name] for core in cores)
                                              for name in DETAIL_COUNTERS if name not in ("lanes", "blocks", "round_slowest_peer")},
                                    round_slowest_peer=next(core for core in cores if core["core"] == 63)
                                        ["detail"]["counters"]["round_slowest_peer"]))
    for name in DETAIL_METRICS:
        operation = {key: _statistics([row["metrics"][name][key] for row in rank_operations])
                     for key in ("core_total_ticks", "all_core_total_ticks", "max_interval_ticks", "count")}
        for key in ("core_total", "all_core_total", "max_interval"):
            operation[key + "_us"] = {stat: value / A5_SYSTEM_TICKS_PER_US for stat, value in operation[key + "_ticks"].items()}
        operations[name] = operation
    return dict(operation_reduction="per-rank core maximum/sum and maximum real interval, then statistics across ranks; counts are rank sums",
                operations=operations, rank_operations=rank_operations)


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
        if source_send or dispatch:
            version = ranks[0]["breakdown"]["schema_version"]
            breakdown.update(schema_version=version,
                             scope="recv_data includes receive/relay and core-end Flag cleanup; finish includes final synchronization, counter reset and round completion; final trace writes excluded"
                                   if dispatch else "cnt/cumsum and source send; finish includes receiver work and final synchronization; final trace writes excluded"
                                   if version == 4 else "cnt/cumsum plus first-hop and DIRECT send; final trace write excluded",
                             phase_barriers="existing coarse boundaries use PIPE_ALL; packet issue samples add no barriers or events"
                                            if version in (4, 5, 6) else "PIPE_ALL at timestamp boundaries; diagnostic build only")
            if version in (4, 5, 6):
                breakdown["send_packet_timing"] = "sample immediately after MTE3 issue; interval to next packet issue, not DMA completion; final tail is cleanup"
            if version == 6:
                breakdown.update(detail_timing=DETAIL_TIMING, **_summarize_receive_details(ranks))
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
        if breakdown.get("schema_version") in (2, 3, 4, 5, 6):
            lines.append("各阶段含诊断同步；未计量最终记录写出，各段最大值不保证可相加；性能基线使用 performance 产物。")
            if breakdown.get("schema_version") in (4, 5, 6):
                lines.append("Token 段表示相邻 MTE3 下发点间隔，不等于搬运完成耗时；逐包打点未新增同步，最后一包之后单列收尾。")
            if breakdown.get("schema_version") == 6:
                for name, operation in breakdown["operations"].items():
                    append("最后一轮各核累计耗时最大值 " + name, operation["core_total_us"], "us")
                lines.append("recv/finish 细分未新增同步；acquire_load 含 lane 依赖和 MTE2 完成，issue 仅计下发，atomic 包含于 metadata；累计耗时不能拼成时间轴。")
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
    dispatch = summary["native_status"] == "dispatch"
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
    if count_scope or source_send or dispatch:
        provenance.update(schema_version=summary["breakdown"]["schema_version"], phase_barriers=summary["breakdown"]["phase_barriers"])
        if summary["breakdown"].get("schema_version") in (4, 5, 6):
            provenance["send_packet_timing"] = summary["breakdown"]["send_packet_timing"]
        if summary["breakdown"].get("schema_version") == 6:
            provenance["detail_timing"] = DETAIL_TIMING

    def write(name, events, **extra):
        path = output_dir / name
        trace = sort_trace_tracks(dict(traceEvents=events, displayTimeUnit="ns", otherData=dict(provenance, **extra)))
        path.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
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
            events.append(dict(ph="M", name="thread_name", pid=rank, tid=tid, args=dict(name=f"AIV core {tid:02d}")))
            for name, begin, end in _breakdown_spans(core, count_scope, source_send, dispatch)[:-1]:
                events.append(dict(ph="X", cat="kernel", name=name, pid=rank, tid=tid,
                                   ts=(begin - origin) / A5_SYSTEM_TICKS_PER_US,
                                   dur=(end - begin) / A5_SYSTEM_TICKS_PER_US,
                                   args=dict(start_ticks=begin, end_ticks=end, generation=row["breakdown"]["generation"])))
            if row["breakdown"].get("schema_version") in (4, 5, 6) and tid < 48:
                events.extend(_send_detail_events(core, rank, origin, row["breakdown"]["generation"]))
            if row["breakdown"].get("schema_version") == 6:
                events.extend(_receive_detail_events(core, rank, origin, row["breakdown"]["generation"]))
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


def _receive_detail_events(core, rank, origin, generation):
    """Render actual subspans and each operation's slowest real interval, never accumulated totals as slices."""
    detail = core["detail"]
    events = []

    def span(name, begin, end, category, **args):
        events.append(dict(ph="X", cat=category, name=name, pid=rank, tid=core["core"],
                           ts=(begin - origin) / A5_SYSTEM_TICKS_PER_US, dur=(end - begin) / A5_SYSTEM_TICKS_PER_US,
                           args=dict(start_ticks=begin, end_ticks=end, generation=generation, **args)))

    for name, begin, end in _receive_detail_spans(core):
        args = dict(timing=DETAIL_TIMING)
        if name in ("recv_loop", "finish_peer_wait"):
            names = ("round_peer_wait",) if name == "finish_peer_wait" else DETAIL_METRICS[:-1]
            args.update(metrics={metric: detail["metrics"][metric] for metric in names}, counters=detail["counters"])
        span(name, begin, end, "recv_phase" if name in RECV_DETAIL_STAGES else "finish_phase", **args)
    for name, metric in detail["metrics"].items():
        if metric["count"]:
            span(name + " max interval", metric["max_begin"], metric["max_end"], "operation_max_interval",
                 metric=name, total_ticks=metric["total_ticks"], total_us=metric["total_ticks"] / A5_SYSTEM_TICKS_PER_US,
                 count=metric["count"], timing=DETAIL_TIMING,
                 **({"peer": detail["counters"]["round_slowest_peer"]} if name == "round_peer_wait" else {}))
    return events


def sort_trace_tracks(trace):
    """Return a TileXR Chrome trace with explicit numeric rank/core ordering; also repairs retained traces."""
    if not isinstance(trace, dict) or not isinstance(trace.get("traceEvents"), list):
        raise ValueError("traceEvents 必须为列表")
    processes, threads, events = set(), set(), []
    for original in trace["traceEvents"]:
        if not isinstance(original, dict):
            raise ValueError("trace event 必须为对象")
        event = dict(original)
        pid, tid = event.get("pid"), event.get("tid")
        numeric = type(pid) is int and pid >= 0 and type(tid) is int and tid >= 0
        if numeric:
            processes.add(pid)
            threads.add((pid, tid))
            if event.get("ph") == "M":
                if event.get("name") in ("process_sort_index", "thread_sort_index"):
                    continue
                if event.get("name") == "thread_name":
                    event["args"] = dict(event.get("args", {}), name=f"AIV core {tid:02d}")
        events.append(event)
    metadata = [dict(ph="M", name="process_sort_index", pid=pid, tid=0, args=dict(sort_index=pid))
                for pid in sorted(processes)]
    metadata.extend(dict(ph="M", name="thread_sort_index", pid=pid, tid=tid, args=dict(sort_index=tid))
                    for pid, tid in sorted(threads))
    named = {(event.get("pid"), event.get("tid")) for event in events
             if event.get("ph") == "M" and event.get("name") == "thread_name"}
    metadata.extend(dict(ph="M", name="thread_name", pid=pid, tid=tid, args=dict(name=f"AIV core {tid:02d}"))
                    for pid, tid in sorted(threads - named))
    # Sorting JSON alone does not control Chrome's row ordering: the metadata above does.
    events = metadata + events
    events.sort(key=lambda event: (0 if event.get("ph") == "M" else 1,
                                   event.get("pid") if type(event.get("pid")) is int else -1,
                                   event.get("tid") if type(event.get("tid")) is int else -1))
    return dict(trace, traceEvents=events)


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
