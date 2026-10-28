"""Benchmark TileRelay against an explicitly selected Host pipeline baseline.

Launch two ranks with torchrun. Build the matching external relay ELF first.
All correctness checks are enabled; timings exclude allocation and CPU checks.
"""

import argparse
import ctypes
from datetime import timedelta
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import sys
import time

import torch
import torch_npu
import torch.distributed as dist

from historical_pipeline import execute, gemm
from check_relay import MODES, check_launches

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src/tilelang/python"))
sys.path.insert(0, str(ROOT / "integrations/vllm_ascend"))
from tilexr_collectives.runtime import TileXRCollectivesRuntime
from tilexr_relay.contracts import Binding
from tilexr_relay.planning import RelayPlan
from tilexr_relay.runtime import RelayRuntime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=1024)
    parser.add_argument("--n", type=int, default=4096)
    parser.add_argument("--k", type=int, default=4096)
    parser.add_argument("--blocks", type=int, required=True)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--delay-rank-ms", type=float, default=0)
    parser.add_argument("--baseline-lib", type=Path, required=True,
                        help="standalone pipeline benchmark library or frozen historical baseline")
    parser.add_argument("--compiler-revision", required=True,
                        help="source revision attestation for the deployed compiler; actual library is hashed")
    parser.add_argument("--generic-manifest", type=Path, required=True, help="ABI-v1 AG-GEMM plan manifest")
    parser.add_argument("--generic-runtime", type=Path, required=True, help="current libtilexr-relay.so")
    parser.add_argument("--prepared-execution", action="store_true",
                        help="also compare native prepared execution using the same generic DSO")
    parser.add_argument("--interleave-seed", type=int,
                        help="deterministically shuffle mode order within each round")
    parser.add_argument("--host-build-flags", default="unrecorded",
                        help="build attestation for compared Host DSOs; record exact optimization flags")
    parser.add_argument("--reverse-mode-order", action="store_true", help="repeat order-sensitive comparisons with reversed mode order")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (args.rows <= 0 or args.rows % 128 or args.n <= 0 or args.n % 256 or
            args.k <= 0 or args.k % 64 or args.blocks <= 0 or args.iterations <= 0 or
            args.warmup < 0 or args.delay_rank_ms < 0):
        parser.error("invalid dimensions, workers or sample count")
    if os.environ.get("WORLD_SIZE") != "2" or os.environ.get("TASK_QUEUE_ENABLE") != "0":
        parser.error("requires two ranks and TASK_QUEUE_ENABLE=0")
    description = RelayPlan.from_json(args.generic_manifest.read_text())
    shape = {"rows": args.rows, "n": args.n, "k": args.k, "blocks": args.blocks}
    if (description.artifact.operator_id != "allgather_gemm" or
            description.artifact.measurement_mode != "baseline" or
            dict(description.artifact.specialization) != shape):
        parser.error("generic artifact operator/shape/measurement mismatch")
    rank = int(os.environ["RANK"])
    torch.npu.set_device(rank)
    torch.set_num_threads(4)
    dist.init_process_group("gloo", timeout=timedelta(seconds=180))
    args.output.mkdir(parents=True, exist_ok=True)
    torch.manual_seed(41)
    full_a = (torch.randn(2 * args.rows, args.k) * 0.1).half()
    weight_cpu = (torch.randn(args.k, args.n) * 0.1).half()
    source_cpu = full_a.chunk(2)[rank].contiguous()
    source, weight = source_cpu.npu(), weight_cpu.npu()
    gathered = torch.empty((2 * args.rows, args.k), dtype=torch.float16, device="npu")
    output = torch.empty((2 * args.rows, args.n), dtype=torch.float16, device="npu")
    full_kernel, tile_kernel = gemm(2 * args.rows, args.n, args.k), gemm(256, args.n, args.k)
    comm_stream, compute_stream = torch.npu.Stream(), torch.npu.Stream()
    runtime = TileXRCollectivesRuntime(rank, 2, os.environ["TILEXR_INSTALL_PREFIX"], initialize=False)
    baseline = ctypes.CDLL(str(args.baseline_lib.resolve()))
    baseline.TileXRAllGatherGemmPipeline.argtypes = [ctypes.c_void_p] * 6 + [ctypes.c_int64] * 4 + [
        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
    baseline.TileXRAllGatherGemmPipeline.restype = ctypes.c_int
    generic_plan = RelayRuntime(args.generic_runtime).create_plan(description)
    generic_workspace = torch.zeros(description.workspace_bytes, device="npu", dtype=torch.uint8)
    generic_bindings = [Binding(t.data_ptr(), t.numel() * t.element_size(), s.dtype_tag, s.layout_tag)
                        for t, s in zip((source, weight, gathered, output), description.binding_specs)]
    generic_prepared = generic_plan.prepare_bindings(generic_bindings,
        workspace=generic_workspace.data_ptr(), workspace_bytes=description.workspace_bytes)
    generic_execution = None
    modes = MODES
    if args.prepared_execution:
        modes += ("generic_execution",)
    if args.reverse_mode_order:
        modes = tuple(reversed(modes))
    samples = {mode: [] for mode in modes}
    rounds = range(args.warmup + args.iterations)
    batches = [(mode, rounds) for mode in modes]
    if args.interleave_seed is not None:
        rng = random.Random(args.interleave_seed)
        batches = []
        for iteration in rounds:
            order = list(modes)
            rng.shuffle(order)
            batches.extend((mode, (iteration,)) for mode in order)
    activities = [torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU]
    try:
        runtime.initialize()
        if args.prepared_execution:
            generic_execution = generic_plan.prepare_execution(runtime.comm_ptr, generic_prepared)
        with torch_npu.profiler.profile(activities=activities,
                schedule=torch_npu.profiler.schedule(wait=0, warmup=0,
                    active=len(modes) * (args.warmup + args.iterations), repeat=1),
                on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(args.output / f"rank{rank}")),
                record_shapes=True) as profiler:
            for mode, mode_iterations in batches:
                for iteration in mode_iterations:
                    expected_a = (full_a + iteration * 0.015625).half()
                    source.copy_(expected_a.chunk(2)[rank].contiguous().npu())
                    gathered.fill_(float("nan"))
                    output.fill_(float("nan"))
                    torch.npu.synchronize()
                    dist.barrier()
                    if rank == 1 and args.delay_rank_ms:
                        time.sleep(args.delay_rank_ms / 1000)
                    with torch.autograd.profiler.record_function(f"{mode}/round{iteration}"):
                        if mode in ("generic_relay", "generic_execution"):
                            begin = time.perf_counter_ns()
                            if mode == "generic_execution":
                                generic_execution.launch(compute_stream.npu_stream)
                            else:
                                generic_plan.launch_prepared(runtime.comm_ptr, generic_prepared, compute_stream.npu_stream)
                            total = (time.perf_counter_ns() - begin) / 1000
                            actual, actual_a = output.cpu(), gathered.cpu()
                        else:
                            shards = [source] if mode == "serial" else list(source.split(128))
                            actual, actual_a, timing = execute(mode, runtime, shards, weight,
                                full_kernel if mode == "serial" else tile_kernel,
                                comm_stream, compute_stream, baseline)
                            total = timing["total_us"]
                    torch.testing.assert_close(actual_a, expected_a, rtol=0, atol=0)
                    expected_output = (expected_a.float() @ weight_cpu.float()).half()
                    torch.testing.assert_close(actual, expected_output, rtol=0.01, atol=0.01)
                    if iteration >= args.warmup:
                        samples[mode].append(total)
                    print(json.dumps({"rank": rank, "mode": mode, "round": iteration,
                                      "correct": True, "total_us": total}), flush=True)
                    profiler.step()
        traces = list((args.output / f"rank{rank}").rglob("trace_view.json"))
        if len(traces) != 1:
            raise ValueError("expected one framework trace per rank")
        raw = json.loads(traces[0].read_text())
        events = raw.get("traceEvents", []) if isinstance(raw, dict) else raw
        counts = check_launches(events, args.rows, args.warmup, args.iterations, modes)
        import tilelang
        local = {"rank": rank, "correct": True, "profiler": True,
            "warmup": args.warmup, "iterations": args.iterations, "mode_order": modes,
            "interleave_seed": args.interleave_seed,
            "host_build_flags": args.host_build_flags,
            "timing_boundary": "synchronous Host launch to stream completion; allocation/reference excluded",
            "tilelang": tilelang.__version__, "compiler_revision": args.compiler_revision,
            "compiler_revision_source": "explicit launch attestation; actual compiler library hashed",
            "shape": {"rows": args.rows, "n": args.n, "k": args.k, "blocks": args.blocks},
            "remote_bytes": args.rows * args.k * 2,
            "kernel_launches": counts, "latency_us": {m: statistics.median(v) for m,v in samples.items()},
            "framework_auxiliary_kernels": sorted({e.get("name") for e in events if
                e.get("args", {}).get("Task Type", "").startswith("KERNEL_") and
                e.get("name", "").startswith("aclnn")}),
            "samples_us": samples, "stage_timing": "fused relay stages not independently observable",
            "artifacts": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for name,path in {"runner": Path(__file__), "baseline": Path(baseline._name),
                    "trace_checker": Path(__file__).with_name("check_relay.py"),
                    "relay_generator": Path(__file__).resolve().parents[2] / "src/tilelang/python/tilexr_relay/ops/allgather_gemm.py",
                    "peer_macros": Path(__file__).resolve().parents[2] / "src/tilelang/python/tilexr_relay/device/peer.py",
                    "canonical_abi": Path(__file__).resolve().parents[2] / "src/tilelang/python/tilexr_relay/device/abi.py",
                    "baseline_adapter": Path(__file__).with_name("historical_pipeline.py"),
                    "relay_host_source": ROOT / "src/relay/tilexr_relay.cpp",
                    "generic_runtime": args.generic_runtime.resolve(),
                    "generic_manifest": args.generic_manifest.resolve(),
                    "generic_binary": args.generic_runtime.resolve().parent / "tilexr/kernels" / description.artifact.file_name,
                    "gemm_full": Path(full_kernel.adapter.lib._name), "gemm_tile": Path(tile_kernel.adapter.lib._name),
                    "tilelang_compiler": Path(tilelang._LIB_PATH), "comm": Path(runtime._comm_lib._name),
                    "collectives": Path(runtime._collectives_lib._name),
                    "collectives_kernel": Path(runtime._collectives_lib._name).resolve().parent /
                        "tilexr/kernels/tilexr_collectives_op.o"}.items()}}
        (args.output / f"result-rank{rank}.json").write_text(json.dumps(local, indent=2))
        all_results = [None, None]
        dist.all_gather_object(all_results, local)
        if rank == 0:
            summary = {}
            for mode in modes:
                latencies = [r["latency_us"][mode] for r in all_results]
                bandwidth = [r["remote_bytes"] / r["latency_us"][mode] / 1000 for r in all_results]
                summary[mode] = {key: {"median": statistics.median(values), "min": min(values), "max": max(values)}
                    for key,values in {"latency_us": latencies, "end_to_end_effective_GB_s": bandwidth}.items()}
            (args.output / "summary.json").write_text(json.dumps({"ranks": all_results, "summary": summary}, indent=2))
        dist.barrier()
    finally:
        torch.npu.synchronize()
        if generic_execution:
            generic_execution.destroy()
        generic_plan.destroy()
        runtime.close()
        dist.destroy_process_group()


if __name__ == "__main__":
    main()
