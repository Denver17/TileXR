#!/usr/bin/env python3
"""Measure compact Dispatch -> ordinary Torch mul with full or metadata waits.

Run one process per rank, after the external occupancy/mapping gate passes. Use
fresh output directories and the same options for both --mode values. Set
TILEXR_COMM_ID=HOST:PORT for TileXR and a distinct --rendezvous tcp://HOST:PORT
for the CPU Gloo stage barrier. This script does not qualify HCCL or choose free
devices; it uses the explicitly supplied device. Example rank arguments: --rank 0 --device 0 --comm-domain 240 --mode metadata
--rendezvous tcp://127.0.0.1:29500 --output /tmp/pipeline --bs 16 --h 128.

Framework profiling is always enabled. Device durations/gaps come only from
unambiguous hardware tasks in its trace; missing native-runtime events produce
an unavailable result, never a Host-time substitute. The caller retains the
build/loaded-binary manifest alongside these traces. Host wall intervals include
Python/runtime work and are auxiliary. No overlap speedup is assumed.
"""
import argparse
from datetime import timedelta
from decimal import Decimal
import gc
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import time

import torch
import torch.distributed as dist
import torch_npu

from tilexr_elastic import DeepEPBuffer, ElasticMemory, _C


KERNEL = "tilexr_ep_dispatch_compact_kernel"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def distribution(values):
    ordered = sorted(values)
    return dict(median=statistics.median(ordered), minimum=ordered[0],
                maximum=ordered[-1], p95=ordered[math.ceil(len(ordered) * .95) - 1])


def artifact(path):
    path = Path(path).resolve()
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def inputs_and_expected(rank, bs, width):
    # 数值在 BF16 可精确表示范围内；oracle 独立遍历 expert/source/token/topk。
    sources = [(torch.arange(bs, dtype=torch.float32).remainder(31) + source * 32 + 1)
               [:, None] + torch.arange(width, dtype=torch.float32).remainder(4)[None, :] / 4
               for source in range(2)]
    routes = (torch.arange(bs, dtype=torch.int32)[:, None] * 2
              + torch.arange(2, dtype=torch.int32)[None, :] + rank).remainder(4)
    rows, assist, counts, expert_counts = [], [], [], []
    for expert in range(rank * 2, rank * 2 + 2):
        before = len(rows)
        for source in range(2):
            for token in range(bs):
                for k in range(2):
                    if (token * 2 + k + source) % 4 == expert:
                        rows.append(sources[source][token])
                        assist.append([source, token, k])
            counts.append(len(rows))
        expert_counts.append(len(rows) - before)
    expected = (torch.stack(rows).to(torch.bfloat16),
                torch.tensor(assist, dtype=torch.int32),
                torch.tensor(counts, dtype=torch.int32),
                torch.tensor(expert_counts, dtype=torch.int64))
    return sources[rank].to(device="npu", dtype=torch.bfloat16), routes.to("npu"), expected


def retire(pool):
    # 退休等待在样本之外；不 trim，后续样本继续复用已增长的 segment。
    gc.collect()
    deadline = time.monotonic() + 10
    while True:
        stats = pool.collect()
        if stats["live_bytes"] == stats["pending_retire_bytes"] == 0:
            return
        require(time.monotonic() < deadline, "output leases did not retire")
        time.sleep(.002)


def device_timing(directory, rounds, warmup):
    paths = list(directory.rglob("trace_view.json"))
    unavailable = dict(status="unavailable", traces=[str(p) for p in paths])
    if len(paths) != 1:
        return dict(unavailable, reason="expected exactly one framework trace")
    raw = json.loads(paths[0].read_text())
    events = raw.get("traceEvents", []) if isinstance(raw, dict) else raw
    # Task Type 是 CANN 设备任务字段；排除 CPU 的 aten::mul/aclnnMul 调用区间。
    hardware = [e for e in events if e.get("ph") == "X" and
                str(e.get("args", {}).get("Task Type", "")).startswith("KERNEL_")]
    dispatch = sorted([e for e in hardware if KERNEL in e.get("name", "")],
                      key=lambda e: Decimal(str(e["ts"])))
    mul = sorted([e for e in hardware if "mul" in e.get("name", "").lower()],
                 key=lambda e: Decimal(str(e["ts"])))
    if len(dispatch) != rounds or len(mul) != rounds:
        return dict(unavailable, reason="native Dispatch/mul task counts are missing or ambiguous",
                    dispatch_tasks=len(dispatch), mul_tasks=len(mul),
                    hardware_names=sorted({e.get("name", "") for e in hardware}))
    rows = []
    for ordinal, (producer, consumer) in enumerate(zip(dispatch, mul)):
        pstart, pdur = Decimal(str(producer["ts"])), Decimal(str(producer["dur"]))
        cstart, cdur = Decimal(str(consumer["ts"])), Decimal(str(consumer["dur"]))
        if (producer.get("pid") is None or producer.get("pid") != consumer.get("pid")
                or pdur <= 0 or cdur <= 0
                or cstart < pstart + pdur or (ordinal + 1 < rounds and
                cstart + cdur > Decimal(str(dispatch[ordinal + 1]["ts"])))):
            return dict(unavailable, reason="hardware task pairing/time domain is inconsistent",
                        ordinal=ordinal, producer=producer, consumer=consumer)
        rows.append(dict(ordinal=ordinal, warmup=ordinal < warmup,
                         dispatch_us=float(pdur), mul_us=float(cdur),
                         dispatch_to_mul_gap_us=float(cstart - pstart - pdur),
                         device_pipeline_us=float(cstart + cdur - pstart),
                         producer=producer, consumer=consumer))
    return dict(status="available", trace=artifact(paths[0]), samples=rows,
                units="microseconds (Chrome trace ts/dur; no Host clock subtraction)",
                latency_us={key: distribution([row[key] for row in rows[warmup:]])
                            for key in ("dispatch_us", "mul_us", "dispatch_to_mul_gap_us",
                                        "device_pipeline_us")})


def environment():
    keys = ("ASCEND_HOME_PATH", "ASCEND_TOOLKIT_HOME", "ASCEND_RT_VISIBLE_DEVICES",
            "TILEXR_COMM_ID", "TILEXR_INSTALL_PREFIX", "TILEXR_ENABLE_UDMA",
            "TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM", "TASK_QUEUE_ENABLE")
    env = {key: os.environ.get(key) for key in keys}
    versions = {}
    root = os.environ.get("ASCEND_HOME_PATH") or os.environ.get("ASCEND_TOOLKIT_HOME")
    if root:
        for relative in ("version.cfg", "version.info", "ascend_toolkit_install.info",
                         "aarch64-linux/ascend_toolkit_install.info"):
            path = Path(root) / relative
            if path.is_file():
                versions[str(path)] = path.read_text(errors="replace")[:16384]
    # /proc/self/maps 记录实际加载的 Host library，不能用安装目录猜测已加载版本。
    loaded = {}
    for line in Path("/proc/self/maps").read_text().splitlines():
        parts = line.split(maxsplit=5)
        if len(parts) == 6 and parts[5].startswith("/") and "tile" in parts[5].lower():
            path = Path(parts[5])
            if path.is_file() and str(path) not in loaded:
                loaded[str(path)] = artifact(path)
    kernels = [Path(path).resolve().parent / "tilexr/kernels" / (KERNEL + ".o")
               for path in loaded if Path(path).name.startswith("libtilexr-ep.so")]
    require(len(kernels) == 1 and kernels[0].is_file(), "cannot resolve owning EP kernel artifact")
    return dict(torch=torch.__version__, torch_npu=torch_npu.__version__, environment=env,
                cann_version_files=versions, cann_version_status="recorded" if versions else "unavailable",
                runner=artifact(__file__), extension=artifact(_C.__file__), loaded_host_libraries=loaded,
                kernel_artifact=artifact(kernels[0]),
                kernel_provenance="resolved relative to loaded EP library, matching native loader; "
                                  "retain source/build manifest separately")


def run(args):
    require(os.environ.get("TILEXR_COMM_ID"), "set explicit TILEXR_COMM_ID=HOST:PORT")
    require(args.warmup >= 1 and args.samples >= 1, "warmup and samples must be positive")
    directory = args.output.resolve() / args.mode / f"rank{args.rank}"
    directory.mkdir(parents=True, exist_ok=False)
    torch.npu.set_device(args.device)
    dist.init_process_group("gloo", init_method=args.rendezvous, rank=args.rank, world_size=2,
                            timeout=timedelta(seconds=300))
    pool = ElasticMemory(device=f"npu:{args.device}", segment_bytes=16 << 20,
                         max_cached_free_bytes=64 << 20, max_committed_bytes=64 << 20)
    require(pool.snapshot()["committed_bytes"] == 0, "initial data HBM is not zero")
    buffer = DeepEPBuffer(output_pool=pool, rank=args.rank, world_size=2,
                          comm_domain=args.comm_domain, staging_mib=2048)
    hidden, routes, expected = inputs_and_expected(args.rank, args.bs, args.h)
    expected_mul = expected[0] * 2
    provenance = environment()
    provenance.update(rank=args.rank, device=args.device, mode=args.mode,
        shape=dict(bs=args.bs, hidden=args.h, topk=2, experts=4, ranks=2, dtype="BF16"),
        route="(token * 2 + topk_index + source_rank) % 4", backend="memory", native=True,
        kernel=KERNEL, warmup=args.warmup, samples=args.samples, framework_profiler=True,
        stage_barrier="torch.npu.synchronize then CPU Gloo barrier before every round",
        staging_bytes=buffer.staging_bytes, peer_window_bytes=buffer.peer_window_bytes,
        memory_domain_guard_bytes=1 << 20,
        output_tensor_bytes=sum(t.numel() * t.element_size() for t in expected),
        hidden_output_bytes=expected[0].numel() * expected[0].element_size(),
        bandwidth="not reported: this pipeline is latency-oriented; output bytes are not transport bytes",
        pool_config=dict(segment_bytes=16 << 20, max_cached_free_bytes=64 << 20,
                         max_committed_bytes=64 << 20))
    (directory / "environment.json").write_text(json.dumps(provenance, indent=2) + "\n")
    rows = []
    total = args.warmup + args.samples
    with (directory / "rounds.jsonl").open("w") as log:
        with torch_npu.profiler.profile(
                activities=[torch_npu.profiler.ProfilerActivity.CPU,
                            torch_npu.profiler.ProfilerActivity.NPU],
                schedule=torch_npu.profiler.schedule(wait=0, warmup=0, active=total, repeat=1),
                on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(
                    str(directory / "profiler"), analyse_flag=True),
                record_shapes=False, profile_memory=False, with_stack=False) as profiler:
            for ordinal in range(total):
                torch.npu.synchronize()
                dist.barrier()
                before = pool.snapshot()
                with torch.autograd.profiler.record_function(f"elastic_pipeline/{args.mode}/round{ordinal}"):
                    start = time.perf_counter_ns()
                    pending = buffer.dispatch_compact_async(hidden, topk_experts=routes, num_experts=4)
                    result = pending.synchronize() if args.mode == "full_wait" else pending.materialize()
                    expert = result.hidden * 2
                    enqueued = time.perf_counter_ns()
                    result.synchronize()
                    torch.npu.synchronize()
                    complete = time.perf_counter_ns()
                require(type(result.hidden) is torch.Tensor, "expected ordinary Torch Tensor")
                require(result.hidden.data_ptr() == result.allocation_address, "output unexpectedly copied")
                for actual, wanted in zip((result.hidden, result.assist, result.send_counts,
                                           result.expert_counts), expected):
                    torch.testing.assert_close(actual.cpu(), wanted, rtol=0, atol=0)
                torch.testing.assert_close(expert.cpu(), expected_mul, rtol=0, atol=0)
                del actual  # zip 的最后一个 Tensor 引用也必须在退休前释放。
                after = pool.snapshot()
                require(after["live_bytes"] + after["pending_retire_bytes"] + after["free_bytes"]
                        == after["committed_bytes"], "pool accounting mismatch")
                if ordinal >= args.warmup:
                    require(before["growth_count"] == after["growth_count"], "measured sample grew pool")
                row = dict(ordinal=ordinal, warmup=ordinal < args.warmup, correct=True,
                           host_enqueue_us=(enqueued - start) / 1000,
                           host_complete_us=(complete - start) / 1000,
                           pool_before=before, pool_after=after)
                rows.append(row)
                log.write(json.dumps(row) + "\n")
                log.flush()
                result.discard()
                del pending, result, expert
                retire(pool)
                profiler.step()
    measured = rows[args.warmup:]
    require(measured[0]["pool_before"]["growth_count"] == measured[-1]["pool_after"]["growth_count"],
            "growth changed during measured region")
    timing = device_timing(directory / "profiler", total, args.warmup)
    require(artifact(provenance["kernel_artifact"]["path"]) == provenance["kernel_artifact"],
            "kernel file changed during run")
    before_trim = pool.snapshot()
    pool.trim(wait=True)
    after_trim = pool.snapshot()
    require(after_trim["committed_bytes"] == 0, "final trim retained data HBM")
    pool.close()
    summary = dict(provenance, correctness="PASS", measured_growth_unchanged=True,
                   before_trim=before_trim, after_trim=after_trim, device_timing=timing,
                   host_auxiliary_us={key: distribution([r[key] for r in measured])
                                      for key in ("host_enqueue_us", "host_complete_us")})
    (directory / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    gathered = [None, None]
    dist.all_gather_object(gathered, summary)
    if args.rank == 0:
        aggregate = dict(mode=args.mode, ranks=gathered, device_latency_us={})
        if all(rank["device_timing"]["status"] == "available" for rank in gathered):
            for key in ("dispatch_us", "mul_us", "dispatch_to_mul_gap_us", "device_pipeline_us"):
                aggregate["device_latency_us"][key] = dict(
                    across_rank_medians=distribution([r["device_timing"]["latency_us"][key]["median"]
                                                     for r in gathered]),
                    pooled_samples=distribution([s[key] for r in gathered
                        for s in r["device_timing"]["samples"] if not s["warmup"]]))
        (directory.parent / "summary.json").write_text(json.dumps(aggregate, indent=2) + "\n")
    print(json.dumps(dict(rank=args.rank, mode=args.mode, correctness="PASS",
                          device_timing=timing["status"], result=str(directory / "result.json"))), flush=True)
    dist.destroy_process_group()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rank", type=int, choices=(0, 1), required=True)
    parser.add_argument("--device", type=int, required=True)
    parser.add_argument("--comm-domain", type=int, required=True)
    parser.add_argument("--rendezvous", required=True, help="Gloo init_method, separate from TILEXR_COMM_ID")
    parser.add_argument("--mode", choices=("full_wait", "metadata"), required=True)
    parser.add_argument("--bs", type=int, choices=(16, 256), default=16)
    parser.add_argument("--h", type=int, choices=(128, 4096), default=128)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require((args.bs, args.h) in ((16, 128), (256, 4096)), "use bs16/h128 or bs256/h4096")
    run(args)


if __name__ == "__main__":
    main()
