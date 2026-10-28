"""Two-card registered-launch qualification for ABI-v1 Relay.

Run with torchrun under an outer timeout on explicitly free devices. Reference
GEMMs and CPU copies are outside the launch sequence. No per-generation Host
barrier masks peer flag reuse. All device allocations survive until completion.
"""

import argparse
from datetime import timedelta
import hashlib
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src/tilelang/python"))
sys.path.insert(0, str(ROOT / "integrations/vllm_ascend"))

import torch
import torch_npu
import torch.distributed as dist
from tilexr_collectives.runtime import TileXRCollectivesRuntime
from tilexr_relay.contracts import Binding
from tilexr_relay.planning import RelayPlan
from tilexr_relay.runtime import RelayRuntime
from tilexr_relay.adapters.torch import bind_tensors
from echo_plan import echo_plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", choices=("abi", "ag", "ar", "mixed"), required=True)
    parser.add_argument("--kernels", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=12)
    parser.add_argument("--execution", action="store_true",
                        help="use native prepared execution instead of prepared bindings")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("WORLD_SIZE") != "2" or os.environ.get("TASK_QUEUE_ENABLE") != "0":
        parser.error("requires two ranks and TASK_QUEUE_ENABLE=0")
    if not 1 <= args.rounds <= 64:
        parser.error("rounds must be 1..64")
    rank = int(os.environ["RANK"])
    torch.npu.set_device(rank)
    torch.set_num_threads(4)
    dist.init_process_group("gloo", timeout=timedelta(seconds=180))
    comm = TileXRCollectivesRuntime(rank, 2, os.environ["TILEXR_INSTALL_PREFIX"])
    runtime = RelayRuntime(args.runtime)
    streams = [torch.npu.Stream(), torch.npu.Stream()]
    args.output.mkdir(parents=True, exist_ok=True)
    native_plans, prepared = [], []
    torch.manual_seed(2026)
    if args.probe == "abi":
        metadata = json.loads((args.kernels / "abi_echo.json").read_text())
        descriptions = [echo_plan(metadata)]
    else:
        wanted = {"ag": {"allgather_gemm"}, "ar": {"gemm_allreduce"},
                  "mixed": {"allgather_gemm", "gemm_allreduce"}}[args.probe]
        descriptions = []
        for path in sorted(args.kernels.glob("tilexr_relay_*.json")):
            plan = RelayPlan.from_json(path.read_text())
            if plan.artifact.operator_id in wanted:
                descriptions.append(plan)
        if not descriptions or {p.artifact.operator_id for p in descriptions} != wanted:
            raise ValueError("missing required operator manifest")
    agreed = [p.identity for p in descriptions]
    identities = [None, None]
    dist.all_gather_object(identities, agreed)
    if identities[0] != identities[1]:
        raise ValueError("rank plan identity/order disagreement")
    ready = {}
    for plan in descriptions:
        native_plans.append(runtime.create_plan(plan))
        assert native_plans[-1].workspace_bytes == plan.workspace_bytes
        if plan.workspace_bytes:
            ready[plan.identity] = torch.zeros(plan.workspace_bytes, device="npu", dtype=torch.uint8)
    precision_generations = set()
    for generation in range(args.rounds):
        index = generation % len(descriptions)
        plan = descriptions[index]
        if args.probe == "abi":
            tensors = [torch.full((32 if slot == 0 else 4,),
                                  slot * 65537 + generation, dtype=torch.int64, device="npu")
                       for slot in range(16) if slot != 7]
            prepared.append((index, plan, tensors, None, None, None))
            continue
        shape = dict(plan.artifact.specialization)
        rows, n, k = (shape[name] for name in ("rows", "n", "k"))
        ar = plan.artifact.operator_id == "gemm_allreduce"
        full_a = (torch.randn((2, rows, k)) * 0.1 + generation * 0.015625).half()
        full_b = (torch.randn((2 if ar else 1, k, n)) * 0.1).half()
        if ar and not precision_generations:
            # FP32 partial0=1+2^-11, partial1=2^-12. Rounding partial0 to
            # FP16 first would give 1.0, whereas the correct final is 1+2^-10.
            full_a.zero_()
            full_b.zero_()
            full_a[0, 0, 0], full_a[0, 0, 1] = 1, 2 ** -11
            full_b[0, 0, 0], full_b[0, 1, 0] = 1, 1
            full_a[1, 0, 0], full_b[1, 0, 0] = 2 ** -12, 1
            precision_generations.add(generation)
        a = full_a[rank].contiguous().npu()
        b = full_b[rank if ar else 0].contiguous().npu()
        output = torch.full((rows if ar else 2 * rows, n), float("nan"), device="npu", dtype=torch.float16)
        if ar:
            tensors = [a, b, output]
        else:
            gathered = torch.full((2 * rows, k), float("nan"), device="npu", dtype=torch.float16)
            tensors = [a, b, gathered, output]
        prepared.append((index, plan, tensors, full_a, full_b, shape))
    calls, executions = [], []
    for index, plan, tensors, _, _, _ in prepared:
        bindings = ([Binding(t.data_ptr(), t.numel() * t.element_size(), spec.dtype_tag, spec.layout_tag)
                     for t, spec in zip(tensors, plan.binding_specs)]
                    if args.probe == "abi" else bind_tensors(plan, tensors))
        workspace = ready.get(plan.identity)
        calls.append(native_plans[index].prepare_bindings(bindings,
            workspace=workspace.data_ptr() if workspace is not None else 0,
            workspace_bytes=plan.workspace_bytes))
        if args.execution:
            executions.append(native_plans[index].prepare_execution(comm.comm_ptr, calls[-1]))
    if args.execution:
        calls.clear()  # Native preparation must not retain borrowed ctypes descriptors.
    torch.npu.synchronize()
    dist.barrier()
    runtime_path = args.runtime
    digest_before = hashlib.sha256(runtime_path.read_bytes()).hexdigest()
    samples = []
    try:
        with torch_npu.profiler.profile(
                activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
                schedule=torch_npu.profiler.schedule(wait=0, warmup=0, active=args.rounds, repeat=1),
                on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(args.output / f"rank{rank}"))) as profiler:
            for generation, (index, plan, tensors, _, _, shape) in enumerate(prepared):
                stream = streams[generation % len(streams)]
                if rank == generation % 2:
                    time.sleep(0.01)
                with torch.autograd.profiler.record_function(f"relay/{args.probe}/round{generation}"):
                    begin = time.perf_counter_ns()
                    if args.execution:
                        executions[generation].launch(stream.npu_stream)
                        executions[generation].launch(streams[(generation + 1) % len(streams)].npu_stream)
                    elif generation % 2:
                        native_plans[index].launch_prepared(comm.comm_ptr, calls[generation], stream.npu_stream)
                    else:
                        call = calls[generation]
                        native_plans[index].launch(comm.comm_ptr, call.bindings,
                            call.workspace, call.workspace_bytes, stream.npu_stream)
                    samples.append((time.perf_counter_ns() - begin) / 1000)
                profiler.step()
        epochs = []
        for generation, (_, plan, tensors, full_a, full_b, shape) in enumerate(prepared):
            if args.probe == "abi":
                actual = tensors[0].cpu().tolist()
                for slot in range(1, 16):
                    if slot != 7:
                        assert actual[slot] == slot * 65537 + generation, (slot, actual)
                assert actual[7] == -1 and actual[17:19] == [rank, 2], actual
                assert actual[16] > (epochs[-1] if epochs else 0), actual
                epochs.append(actual[16])
            elif plan.artifact.operator_id == "gemm_allreduce":
                expected = (full_a[0].float() @ full_b[0].float() + full_a[1].float() @ full_b[1].float()).half()
                tolerance = 0 if generation in precision_generations else 0.01
                torch.testing.assert_close(tensors[2].cpu(), expected, rtol=tolerance, atol=tolerance)
            else:
                gathered = full_a.reshape(2 * shape["rows"], shape["k"])
                torch.testing.assert_close(tensors[2].cpu(), gathered, rtol=0, atol=0)
                expected = (gathered.float() @ full_b[0].float()).half()
                torch.testing.assert_close(tensors[3].cpu(), expected, rtol=0.01, atol=0.01)
            print(json.dumps({"rank": rank, "generation": generation, "correct": True}), flush=True)
        traces = list((args.output / f"rank{rank}").rglob("trace_view.json"))
        if len(traces) != 1:
            raise ValueError("expected exactly one profiler trace")
        trace = json.loads(traces[0].read_text())
        events = trace.get("traceEvents", []) if isinstance(trace, dict) else trace
        launches = [e for e in events if e.get("args", {}).get("Task Type") in
                    ("KERNEL_MIX_AIC", "MIX_AIC") and e.get("ph") == "X"]
        expected_launches = args.rounds * (2 if args.execution else 1)
        if len(launches) != expected_launches:
            raise ValueError(f"expected {expected_launches} mixed main launches, got {len(launches)}")
        digest_after = hashlib.sha256(runtime_path.read_bytes()).hexdigest()
        assert digest_before == digest_after
        result = {"correct": True, "rank": rank, "probe": args.probe, "rounds": args.rounds,
                  "runtime": str(runtime_path), "runtime_sha256": digest_before, "plan_identities": agreed,
                  "per_generation_host_barrier": False, "framework_profiler": True,
                  "launch_count": len(launches), "epochs": epochs, "host_us": samples,
                  "prepared_bindings": True,
                  "prepared_execution": args.execution,
                  "launches_per_round": 2 if args.execution else 1,
                  "stream_count": len(streams),
                  "launch_api": "execution" if args.execution else "one_shot_and_bindings",
                  "fp32_partial_precision_generations": sorted(precision_generations)}
        (args.output / f"rank{rank}.json").write_text(json.dumps(result, indent=2))
        dist.barrier()
    finally:
        torch.npu.synchronize()
        for native in native_plans:
            native.destroy()
        # Exercise parent-before-child teardown with real native handles.
        for execution in executions:
            assert runtime._library.TileXRRelayExecutionLaunch(execution._handle, streams[0].npu_stream) == -1
            try:
                execution.launch(streams[0].npu_stream)
            except RuntimeError:
                pass
            else:
                raise AssertionError("execution accepted a destroyed parent")
            execution.destroy()
        comm.close()
        dist.destroy_process_group()


if __name__ == "__main__":
    main()
