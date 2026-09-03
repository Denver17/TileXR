from __future__ import annotations

import argparse
import importlib
import importlib.metadata
import json
import math
import os
import platform
import subprocess
import time
from dataclasses import replace
from pathlib import Path
from typing import Callable, Mapping

from .config import (
    BenchmarkCase,
    apply_overrides,
    build_case_parser,
    load_cases,
    select_cases,
)
from .case_factory import make_correctness_case
from .contracts import BackendUnavailableError, MoonEPBackend, MoonEPDimensions
from .correctness import CorrectnessRunner
from .expert_forward import run_expert_forward
from .planner_reference import (
    DEFAULT_ROUTE_DISTRIBUTION,
    build_reference_plan,
    deterministic_all_topk,
    deterministic_rank_topk,
)
from .rendezvous import (
    completion_barrier_from_env,
    hold_for_managed_abort,
    signal_managed_abort,
)
from .report import write_json, write_jsonl
from .torch_npu_backend import TorchNpuMoonEPBackend


DEFAULT_CORRECTNESS_BACKEND = "tools.moonep.tilexr_backend:create_backend"
DEFAULT_BENCHMARK_WARMUP = 5
DEFAULT_BENCHMARK_ITERATIONS = 20


def apply_mode_defaults(case: BenchmarkCase, *, mode: str) -> BenchmarkCase:
    if mode != "benchmark":
        return case
    return replace(
        case,
        warmup=DEFAULT_BENCHMARK_WARMUP,
        iterations=DEFAULT_BENCHMARK_ITERATIONS,
    )


def _reference_process_group_backend(
    environment: Mapping[str, str], *, mode: str
) -> str:
    if mode == "correctness":
        return "gloo"
    if mode != "reference":
        raise ValueError(f"reference process group is invalid for mode {mode!r}")
    return "gloo" if environment.get("TILEXR_OVERSUBSCRIBED") == "1" else "hccl"


STAGE_ORDER = (
    "planning",
    "dispatch_forward",
    "prefetch_weight",
    "expert_forward",
    "combine_forward",
    "dispatch_backward",
    "expert_backward",
    "combine_backward",
    "reduce_grad",
)

_NATIVE_STAGE_KERNEL_VERSIONS = {
    "planning": "tilexr_ep_plan_kernel (PlannerV3)",
    "dispatch": "tilexr_moonep_dispatch_urma_kernel (DispatchV2)",
    "prefetch_weight": "tilexr_moonep_prefetch_weight_kernel (V1)",
    "combine": "tilexr_moonep_combine_v2_kernel (CombineV2)",
    "reduce_grad": "tilexr_moonep_reduce_grad_kernel (V2)",
}

FINAL_SHARED_STATUS_SUCCESS = 0
REDUCE_GRAD_STATUS_SUCCESS = 0


def stage_execution_metadata(
    capabilities: Mapping[str, object], *, torch_npu_version: str
) -> dict[str, dict[str, object]]:
    implementations = capabilities.get("implementations")
    if not isinstance(implementations, Mapping):
        raise ValueError("MoonEP capabilities do not contain stage implementations")
    result = {}
    for stage, kernel_version in _NATIVE_STAGE_KERNEL_VERSIONS.items():
        implementation = str(implementations.get(stage, "unavailable"))
        native = implementation == "native"
        result[stage] = {
            "native": native,
            "implementation": implementation,
            "kernel_version": (
                kernel_version if native else f"N/A ({implementation})"
            ),
        }
    result["expert"] = {
        "native": False,
        "implementation": "torch_npu",
        "kernel_version": f"torch_npu {torch_npu_version} (GMM+SwiGLU)",
    }
    return result


def _hold_unsafe_teardown(
    *,
    output_root: Path,
    rank_dir: Path,
    result: dict[str, object],
    rank: int,
    reason: str,
    world_size: int,
) -> None:
    try:
        try:
            write_json(rank_dir / "result.json", result)
        except Exception:
            pass
        try:
            signal_managed_abort(output_root, rank, reason)
        except Exception:
            pass
    finally:
        if world_size > 1:
            hold_for_managed_abort()
    raise RuntimeError(reason)


class DeviceEventTimer:
    def __init__(self, torch_module):
        self._torch = torch_module
        self._events: dict[str, tuple[object, object]] = {}
        self._e2e_start = self._event()
        self._e2e_end = self._event()

    def _event(self):
        return self._torch.npu.Event(enable_timing=True)

    def start(self) -> None:
        self._e2e_start.record()

    def call(self, name: str, callback: Callable[[], object]):
        start = self._event()
        end = self._event()
        start.record()
        value = callback()
        end.record()
        self._events[name] = (start, end)
        return value

    def finish(self) -> dict[str, float]:
        self._e2e_end.record()
        self._torch.npu.synchronize()
        timings = {
            name: float(start.elapsed_time(end)) * 1000.0
            for name, (start, end) in self._events.items()
        }
        timings["end_to_end"] = float(
            self._e2e_start.elapsed_time(self._e2e_end)
        ) * 1000.0
        return timings


def _timed_call(timer: DeviceEventTimer | None, name: str, callback):
    trace = os.environ.get("TILEXR_MOONEP_TRACE_STAGES", "0") == "1"
    rank = os.environ.get("RANK", "0")
    if trace:
        print(f"[TileXR MoonEP rank {rank}] {name}_begin", flush=True)
    value = callback() if timer is None else timer.call(name, callback)
    if trace:
        print(f"[TileXR MoonEP rank {rank}] {name}_end", flush=True)
    return value


def _trace_stage_sync(buffer, stage: str) -> None:
    if os.environ.get("TILEXR_MOONEP_TRACE_SYNC_EACH_STAGE", "0") != "1":
        return
    rank = os.environ.get("RANK", "0")
    print(f"[TileXR MoonEP rank {rank}] {stage}_sync_begin", flush=True)
    buffer.synchronize()
    print(f"[TileXR MoonEP rank {rank}] {stage}_sync_end", flush=True)


def _oversubscribed_planning_barrier(buffer, case_id: str, epoch: int) -> None:
    if os.environ.get("TILEXR_OVERSUBSCRIBED", "0") != "1":
        return
    rank = int(os.environ.get("RANK", "0"))
    world_size = int(os.environ.get("WORLD_SIZE", "1"))
    if world_size <= 1:
        return
    buffer.synchronize()
    decision = completion_barrier_from_env(
        rank,
        world_size,
        case_id=f"{case_id}.planning.{epoch}",
        quiesced=True,
        passed=True,
    )
    if not decision.release or decision.abort:
        raise RuntimeError("oversubscribed Planning launch rendezvous failed")


def _multi_node_stage_barrier(
    buffer, case_id: str, epoch: int, stage: str, *, quiesced: bool = False
) -> None:
    if int(os.environ.get("NODE_COUNT", "1")) <= 1:
        return
    rank = int(os.environ.get("RANK", "0"))
    world_size = int(os.environ.get("WORLD_SIZE", "1"))
    if world_size <= 1:
        return
    trace = os.environ.get("TILEXR_MOONEP_TRACE_STAGES", "0") == "1"
    if trace:
        print(
            f"[TileXR MoonEP rank {rank}] stage_barrier_{stage}_enter "
            f"quiesced={int(quiesced)}",
            flush=True,
        )
    if not quiesced:
        buffer.synchronize()
    if trace:
        print(
            f"[TileXR MoonEP rank {rank}] stage_barrier_{stage}_rendezvous_begin",
            flush=True,
        )
    decision = completion_barrier_from_env(
        rank,
        world_size,
        case_id=f"{case_id}.{stage}.{epoch}",
        quiesced=True,
        passed=True,
    )
    if trace:
        print(
            f"[TileXR MoonEP rank {rank}] stage_barrier_{stage}_rendezvous_end",
            flush=True,
        )
    if not decision.release or decision.abort:
        raise RuntimeError(f"multi-node {stage} rendezvous failed")


def execute_iteration(
    buffer,
    inputs: dict[str, object],
    timer: DeviceEventTimer | None = None,
    *,
    torch_module,
    torch_npu_module=None,
    stage_barrier: Callable[[str], None] | None = None,
):
    trace_checksums = os.environ.get("TILEXR_MOONEP_TRACE_CHECKSUMS", "0") == "1"
    rank = os.environ.get("RANK", "0")
    if trace_checksums:
        print(
            f"[TileXR MoonEP rank {rank}] stage_checksum "
            f"input_hidden={_checksum(inputs['hidden'])} "
            f"input_hidden_weighted={_weighted_checksum(torch_module, inputs['hidden'])}",
            flush=True,
        )
    if stage_barrier is not None:
        stage_barrier("planning")
    if timer is not None:
        timer.start()
    plan, cu_seqlens = _timed_call(
        timer,
        "planning",
        lambda: buffer.planning(
            inputs["topk_experts"],
            inputs["tokens_per_expert"],
        ),
    )
    hidden_nvsh, route_weights_nvs, _, _ = _timed_call(
        timer,
        "dispatch_forward",
        lambda: buffer.dispatch(
            inputs["hidden"],
            inputs["route_weights"],
            plan=plan,
        ),
    )
    if trace_checksums:
        print(
            f"[TileXR MoonEP rank {rank}] stage_checksum "
            f"dispatch_forward={_checksum(hidden_nvsh)} "
            f"dispatch_forward_weighted={_weighted_checksum(torch_module, hidden_nvsh)}",
            flush=True,
        )
    _timed_call(
        timer,
        "prefetch_weight",
        lambda: buffer.prefetch_weight(plan, inputs["projections"]),
    )
    _trace_stage_sync(buffer, "prefetch_weight")

    zero = torch_module.zeros_like(cu_seqlens[:1])
    counts = torch_module.cat((zero, cu_seqlens))
    counts = counts[1:] - counts[:-1]
    local_begin = buffer.context.planner_group_rank * buffer.context.experts_per_rank
    local_end = local_begin + buffer.context.experts_per_rank
    slot_begin = buffer.context.expert_count
    compact_counts = torch_module.cat(
        (counts[local_begin:local_end], counts[slot_begin:slot_begin +
            buffer.context.experts_per_rank])
    )
    compact_cu_seqlens = compact_counts.cumsum(dim=0, dtype=cu_seqlens.dtype)
    if trace_checksums:
        print(
            f"[TileXR MoonEP rank {rank}] stage_checksum "
            f"compact_cu_seqlens={compact_cu_seqlens.cpu().tolist()}",
            flush=True,
        )

    expert_output = _timed_call(
        timer,
        "expert_forward",
        lambda: run_expert_forward(
            torch_module,
            hidden_nvsh,
            compact_cu_seqlens,
            inputs["projections"],
            route_weights_nvs,
            torch_npu_module=torch_npu_module,
        ).hidden,
    )
    _trace_stage_sync(buffer, "expert_forward")
    if trace_checksums:
        print(
            f"[TileXR MoonEP rank {rank}] stage_checksum "
            f"projection_gate={_weighted_checksum(torch_module, inputs['projections'].gate)} "
            f"projection_up={_weighted_checksum(torch_module, inputs['projections'].up)} "
            f"projection_down={_weighted_checksum(torch_module, inputs['projections'].down)} "
            f"expert_forward={_checksum(expert_output)} "
            f"expert_forward_weighted={_weighted_checksum(torch_module, expert_output)}",
            flush=True,
        )
    if stage_barrier is not None:
        stage_barrier("expert_forward")
    forward_hidden, forward_weights, _ = _timed_call(
        timer,
        "combine_forward",
        lambda: buffer.combine(
            plan,
            expert_output,
            route_weights_nvs,
            phase_barrier=(
                None if stage_barrier is None else
                lambda phase: stage_barrier(f"combine_forward_{phase}")
            ),
        ),
    )
    if trace_checksums:
        print(
            f"[TileXR MoonEP rank {rank}] stage_checksum "
            f"combine_forward={_checksum(forward_hidden)}",
            flush=True,
        )
    dispatched_grad, _, _, _ = _timed_call(
        timer,
        "dispatch_backward",
        lambda: buffer.dispatch(inputs["grad_output"], plan=plan),
    )

    def expert_backward():
        scale = inputs["projections"].gate[-buffer.context.prefetch_slots :].mean()
        output = dispatched_grad * scale.to(dtype=dispatched_grad.dtype)
        if route_weights_nvs is not None:
            route_weights = route_weights_nvs.to(dtype=output.dtype)
            output = output * route_weights.reshape(buffer.context.nv_s, 1)
        return output

    grad_expert_hidden = _timed_call(timer, "expert_backward", expert_backward)
    backward_hidden, _, _ = _timed_call(
        timer,
        "combine_backward",
        lambda: buffer.combine(
            plan,
            grad_expert_hidden,
            phase_barrier=(
                None if stage_barrier is None else
                lambda phase: stage_barrier(f"combine_backward_{phase}")
            ),
        ),
    )
    _timed_call(
        timer,
        "reduce_grad",
        lambda: buffer.reduce_grad(
            plan,
            full_gate_grad=inputs["gradients"].gate,
            full_up_grad=inputs["gradients"].up,
            full_down_grad=inputs["gradients"].down,
            gate_reduce_buffer=inputs["gradients"].gate_reduce,
            up_reduce_buffer=inputs["gradients"].up_reduce,
            down_reduce_buffer=inputs["gradients"].down_reduce,
        ),
    )
    if stage_barrier is not None:
        stage_barrier("reduce_grad")
    timings = None if timer is None else timer.finish()
    buffer.synchronize()
    return plan, cu_seqlens, forward_hidden, forward_weights, backward_hidden, timings


def _tensor_row_bytes(tensor) -> int:
    shape = tuple(int(value) for value in tensor.shape)
    if not shape or shape[0] <= 0:
        raise ValueError("performance tensors must have a positive row dimension")
    total_bytes = int(tensor.numel()) * int(tensor.element_size())
    if total_bytes % shape[0] != 0:
        raise ValueError("performance tensor bytes are not divisible by rows")
    return total_bytes // shape[0]


def algorithm_bytes(plan, inputs: Mapping[str, object], context) -> dict[str, int]:
    remote_routes = 0
    for encoded in _tensor_values(plan.dst):
        raw = -int(encoded) - 1 if int(encoded) < 0 else int(encoded)
        destination = raw // int(context.nv_s)
        if destination < 0 or destination >= int(context.planner_group_size):
            raise RuntimeError(f"plan destination rank is out of range: {destination}")
        if destination != int(context.planner_group_rank):
            remote_routes += 1

    remote_stats = _tensor_values(plan.remote_stats)
    if len(remote_stats) != 2 or any(int(value) < 0 for value in remote_stats):
        raise RuntimeError(f"invalid plan remote_stats: {remote_stats}")
    hidden_row_bytes = _tensor_row_bytes(inputs["hidden"])
    route_weight_bytes = int(inputs["route_weights"].element_size())
    projection_row_bytes = sum(
        _tensor_row_bytes(getattr(inputs["projections"], name))
        for name in ("gate", "up", "down")
    )
    gradient_row_bytes = sum(
        _tensor_row_bytes(getattr(inputs["gradients"], name))
        for name in ("gate", "up", "down")
    )
    route_bytes = remote_routes * (2 * hidden_row_bytes + route_weight_bytes)
    return {
        "dispatch": route_bytes,
        "prefetch_weight": int(remote_stats[0]) * projection_row_bytes,
        "combine": route_bytes,
        "reduce_grad": int(remote_stats[1]) * gradient_row_bytes,
    }


def _torch_dtype(torch_module, name: str):
    return {
        "bfloat16": torch_module.bfloat16,
    }[name]


def make_inputs(torch_module, case, context):
    from tilexr_moonep import ProjectionBuffers

    device = f"npu:{context.device_index}"
    torch_module.manual_seed(case.seed + context.global_rank)
    if hasattr(torch_module.npu, "manual_seed_all"):
        torch_module.npu.manual_seed_all(case.seed + context.global_rank)
    dtype = _torch_dtype(torch_module, case.dtype)
    route_ids = torch_module.tensor(
        deterministic_rank_topk(
            context.planner_group_rank,
            context.planner_group_size,
            case.tokens_per_rank,
            case.topk,
            case.expert_count,
            case.route_distribution,
        ),
        dtype=torch_module.int32,
        device=device,
    ).reshape(case.tokens_per_rank, case.topk)
    tokens_per_expert = torch_module.bincount(
        route_ids.reshape(-1).to(dtype=torch_module.int64),
        minlength=case.expert_count,
    ).to(dtype=torch_module.int32)
    hidden = torch_module.randn(
        (case.tokens_per_rank, case.hidden_size), dtype=dtype, device=device
    )
    route_weights = torch_module.full(
        (case.tokens_per_rank, case.topk),
        1.0 / case.topk,
        dtype=torch_module.float32,
        device=device,
    )
    rows = case.expert_count + context.prefetch_slots
    intermediate_size = (
        int(case.intermediate_size)
        if case.intermediate_size is not None
        else int(case.hidden_size)
    )
    local_gate_up_shape = (
        context.experts_per_rank, case.hidden_size, intermediate_size
    )
    local_down_shape = (
        context.experts_per_rank, intermediate_size, case.hidden_size
    )
    gate_up_shape = (rows, case.hidden_size, intermediate_size)
    down_shape = (rows, intermediate_size, case.hidden_size)
    gate_up_reduce_shape = (
        context.planner_group_size,
        context.prefetch_slots,
        case.hidden_size,
        intermediate_size,
    )
    down_reduce_shape = (
        context.planner_group_size,
        context.prefetch_slots,
        intermediate_size,
        case.hidden_size,
    )
    projections = ProjectionBuffers.from_local_weights(
        context,
        torch_module.full(local_gate_up_shape, 0.5, dtype=dtype, device=device),
        torch_module.full(local_gate_up_shape, 0.25, dtype=dtype, device=device),
        torch_module.full(local_down_shape, 0.75, dtype=dtype, device=device),
        torch_module=torch_module,
    )
    gradients = ProjectionBuffers(
        gate=torch_module.ones(gate_up_shape, dtype=torch_module.float32, device=device),
        up=torch_module.ones(gate_up_shape, dtype=torch_module.float32, device=device),
        down=torch_module.ones(down_shape, dtype=torch_module.float32, device=device),
        gate_reduce=torch_module.zeros(
            gate_up_reduce_shape, dtype=torch_module.float32, device=device
        ),
        up_reduce=torch_module.zeros(
            gate_up_reduce_shape, dtype=torch_module.float32, device=device
        ),
        down_reduce=torch_module.zeros(
            down_reduce_shape, dtype=torch_module.float32, device=device
        ),
    )
    return {
        "topk_experts": route_ids,
        "tokens_per_expert": tokens_per_expert,
        "hidden": hidden,
        "route_weights": route_weights,
        "projections": projections,
        "grad_output": torch_module.ones_like(hidden),
        "gradients": gradients,
    }


def _checksum(tensor) -> float:
    value = float(tensor.float().sum().item())
    if not math.isfinite(value):
        raise RuntimeError("non-finite deterministic checksum")
    return value


def _weighted_checksum(torch_module, tensor) -> float:
    flat = tensor.float().reshape(-1)
    weights = torch_module.arange(
        1, int(flat.numel()) + 1,
        dtype=torch_module.float32,
        device=flat.device,
    )
    return _checksum(flat * weights)


def _flatten(values) -> list[int]:
    if isinstance(values, (list, tuple)):
        result = []
        for value in values:
            result.extend(_flatten(value))
        return result
    return [int(values)]


def _tensor_values(tensor) -> list[int]:
    return _flatten(tensor.cpu().tolist())


def _require_exact(name: str, actual: list[int], expected) -> None:
    expected_values = [int(value) for value in expected]
    if actual == expected_values:
        return
    mismatch = next(
        (
            index
            for index, (left, right) in enumerate(zip(actual, expected_values))
            if left != right
        ),
        min(len(actual), len(expected_values)),
    )
    actual_value = actual[mismatch] if mismatch < len(actual) else "<missing>"
    expected_value = (
        expected_values[mismatch] if mismatch < len(expected_values) else "<missing>"
    )
    raise RuntimeError(
        f"Planner {name} mismatch at flat index {mismatch}: "
        f"actual={actual_value}, expected={expected_value}"
    )


def validate_plan(
    plan, context, cu_seqlens=None, *, expected_status: int = 0,
    route_distribution: str = DEFAULT_ROUTE_DISTRIBUTION,
    all_topk=None,
) -> dict[str, object]:
    status = int(plan.status.item())
    if status != int(expected_status):
        raise RuntimeError(
            f"MoonEP device status is {status}, expected {int(expected_status)}"
        )
    if all_topk is None:
        all_topk = deterministic_all_topk(
            context.planner_group_size,
            context.tokens_per_rank,
            context.topk,
            context.expert_count,
            route_distribution,
        )
    reference = build_reference_plan(
        rank=context.planner_group_rank,
        rank_size=context.planner_group_size,
        tokens_per_rank=context.tokens_per_rank,
        topk=context.topk,
        expert_count=context.expert_count,
        prefetch_slots=context.prefetch_slots,
        token_padding=context.token_padding,
        all_topk=all_topk,
    )
    _require_exact("dst", _tensor_values(plan.dst), reference.dst)
    if cu_seqlens is None:
        cu_seqlens = getattr(plan, "cu_seqlens", None)
    if cu_seqlens is None:
        raise RuntimeError("Planner cu_seqlens output is required")
    _require_exact("cu_seqlens", _tensor_values(cu_seqlens), reference.cu_seqlens)
    _require_exact(
        "zero_fill_ranges",
        _tensor_values(plan.zero_fill_ranges),
        reference.zero_fill_ranges,
    )
    _require_exact(
        "experts_to_copy",
        _tensor_values(plan.experts_to_copy),
        reference.experts_to_copy,
    )
    _require_exact(
        "remote_stats", _tensor_values(plan.remote_stats), reference.remote_stats
    )
    return {
        "passed": True,
        "mode": "planner_cpu_oracle",
        "checks": {
            "status_expected": True,
            "dst_exact": True,
            "cu_seqlens_exact": True,
            "zero_fill_ranges_exact": True,
            "experts_to_copy_exact": True,
            "remote_stats_exact": True,
        },
    }


def validate_stub_flow(torch_module, inputs, forward, backward, context) -> dict[str, bool]:
    scale = inputs["projections"].gate[-context.experts_per_rank :].mean()
    local_weights = inputs["route_weights"].reshape(-1)[: context.tokens_per_rank]
    local_weights = local_weights.reshape(context.tokens_per_rank, 1)
    expected_backward = inputs["grad_output"] * scale.to(dtype=inputs["grad_output"].dtype)
    expected_backward = expected_backward * local_weights.to(dtype=expected_backward.dtype)
    checks = {
        "forward_hidden_finite": bool(torch_module.isfinite(forward.hidden).all().item()),
        "forward_weights_exact": bool(
            torch_module.equal(forward.route_weights, inputs["route_weights"])
        ),
        "backward_hidden_exact": bool(torch_module.equal(backward.hidden, expected_backward)),
        "gate_reduce_copy_exact": bool(
            torch_module.equal(inputs["gradients"].gate_reduce, inputs["gradients"].gate)
        ),
        "up_reduce_copy_exact": bool(
            torch_module.equal(inputs["gradients"].up_reduce, inputs["gradients"].up)
        ),
        "down_reduce_copy_exact": bool(
            torch_module.equal(inputs["gradients"].down_reduce, inputs["gradients"].down)
        ),
    }
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        raise RuntimeError(f"remaining stub flow mismatch: {', '.join(failed)}")
    return checks


def _version_or_unknown(distribution: str) -> str:
    try:
        return importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        return "unavailable"


def _git_sha(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def _git_dirty(root: Path) -> bool | str:
    try:
        status = subprocess.check_output(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=root,
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return bool(status.strip())
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def environment_metadata(torch_module, root: Path) -> dict[str, object]:
    try:
        soc = str(torch_module.npu.get_device_name())
    except Exception:
        soc = "unknown"
    return {
        "git_sha": _git_sha(root),
        "git_dirty": _git_dirty(root),
        "python": platform.python_version(),
        "torch": getattr(torch_module, "__version__", "unknown"),
        "torch_npu": _version_or_unknown("torch-npu"),
        "cann_version": os.environ.get(
            "ASCEND_TOOLKIT_VERSION", os.environ.get("CANN_VERSION", "unknown")
        ),
        "cann_home": os.environ.get("ASCEND_HOME_PATH", "unknown"),
        "driver_version": os.environ.get("TILEXR_DRIVER_VERSION", "unknown"),
        "soc": soc,
    }


def topology_metadata(context) -> dict[str, object]:
    physical = int(os.environ.get("TILEXR_PHYSICAL_DEVICE_COUNT", "1"))
    ranks_per_device = int(os.environ.get("TILEXR_RANKS_PER_DEVICE", "1"))
    default_block_dim = 32 if ranks_per_device == 2 else 64
    planner_block_dim = int(
        os.environ.get("TILEXR_MOONEP_PLANNER_BLOCK_DIM", str(default_block_dim))
    )
    if planner_block_dim < 1 or planner_block_dim > 64:
        raise ValueError(
            "effective TILEXR_MOONEP_PLANNER_BLOCK_DIM must satisfy "
            "1 <= blockDim <= 64"
        )
    dispatch_aiv_core_count = int(os.environ.get(
        "TILEXR_MOONEP_DISPATCH_AIV_CORE_COUNT", "64"))
    if dispatch_aiv_core_count < 1 or dispatch_aiv_core_count > 64:
        raise ValueError(
            "effective TILEXR_MOONEP_DISPATCH_AIV_CORE_COUNT must satisfy "
            "1 <= coreCount <= 64"
        )
    return {
        "global_rank": context.global_rank,
        "global_world_size": context.global_world_size,
        "node_rank": context.node_rank,
        "node_count": context.node_count,
        "visible_devices": os.environ.get("ASCEND_RT_VISIBLE_DEVICES", "unknown"),
        "local_rank": context.local_rank,
        "local_world_size": context.local_world_size,
        "planner_group_rank": context.planner_group_rank,
        "planner_group_size": context.planner_group_size,
        "lane_group_rank": context.lane_group_rank,
        "lane_group_size": context.lane_group_size,
        "physical_device_count": physical,
        "ranks_per_device": ranks_per_device,
        "oversubscribed": os.environ.get("TILEXR_OVERSUBSCRIBED", "0") == "1",
        "planner_block_dim": planner_block_dim,
        "planner_block_dim_source": os.environ.get(
            "TILEXR_MOONEP_PLANNER_BLOCK_DIM_SOURCE",
            "default_oversubscribed" if ranks_per_device == 2 else "default_native",
        ),
        "dispatch_aiv_core_count": dispatch_aiv_core_count,
        "dispatch_aiv_core_count_source": os.environ.get(
            "TILEXR_MOONEP_DISPATCH_AIV_CORE_COUNT_SOURCE", "default"
        ),
        "udma_qp_route_spec": os.environ.get(
            "TILEXR_UDMA_QP_ROUTE_SPEC", "unknown"
        ),
        "peer_memory_cross_node": context.node_count > 1,
        "cross_node_validated": os.environ.get(
            "TILEXR_MOONEP_CROSS_NODE_VALIDATED", "0"
        ) == "1",
    }


def run_case(torch_module, case, args, root: Path) -> None:
    from tilexr_moonep import TileXRMoonEPBuffer, TileXRMoonEPContext

    dtype = _torch_dtype(torch_module, case.dtype)
    output_root = Path(args.output_dir).resolve()
    global_rank = int(os.environ.get("RANK", "0"))
    rank_dir = (output_root / case.case_id / f"rank_{global_rank}").resolve()
    if output_root not in rank_dir.parents:
        raise ValueError(f"case artifact path escapes output root: {rank_dir}")
    environment = environment_metadata(torch_module, root)
    result = {
        "schema_version": 1,
        "status": "failed",
        "failure_reason": None,
        "mode": args.mode,
        "launch_id": os.environ.get("TILEXR_MOONEP_LAUNCH_ID", ""),
        "case": case.as_dict(),
        "rank": global_rank,
        "topology": None,
        "capabilities": None,
        "environment": environment,
        "benchmark_config": {"wait_iterations": int(args.wait_iterations)},
        "stage_execution": None,
        "validation": {"passed": False, "mode": "not_run"},
        "stage_order": list(STAGE_ORDER),
    }
    context = None
    buffer = None
    failure = None

    def record_failure(step: str, exc: Exception) -> None:
        nonlocal failure
        reason = f"{step} failed: {type(exc).__name__}: {exc}"
        if result["failure_reason"]:
            result["failure_reason"] += f"; {reason}"
        else:
            result["status"] = "failed"
            result["failure_reason"] = reason
        if failure is None:
            failure = (exc, exc.__traceback__)

    try:
        context = TileXRMoonEPContext.from_env(
            tokens_per_rank=case.tokens_per_rank,
            hidden_size=case.hidden_size,
            topk=case.topk,
            expert_count=case.expert_count,
            dtype=dtype,
            token_padding=case.token_padding,
            prefetch_slots=case.prefetch_slots,
            install_prefix=args.install_prefix,
            torch_module=torch_module,
        )
        buffer = TileXRMoonEPBuffer(
            context, wait_iterations=args.wait_iterations, torch_module=torch_module
        )
        capabilities = context.runtime.capabilities.as_dict()
        result["rank"] = context.global_rank
        result["capabilities"] = capabilities
        result["stage_execution"] = stage_execution_metadata(
            capabilities,
            torch_npu_version=str(environment["torch_npu"]),
        )
        result["topology"] = topology_metadata(context)
        if os.environ.get("TILEXR_MOONEP_TRACE_STAGES", "0") == "1":
            print(
                f"[TileXR MoonEP rank {context.global_rank}] make_inputs_begin",
                flush=True,
            )
        inputs = make_inputs(torch_module, case, context)
        if os.environ.get("TILEXR_MOONEP_TRACE_STAGES", "0") == "1":
            print(
                f"[TileXR MoonEP rank {context.global_rank}] make_inputs_end",
                flush=True,
            )
        buffer.register_projection_buffers(inputs["projections"])
        if os.environ.get("TILEXR_MOONEP_TRACE_STAGES", "0") == "1":
            print(
                f"[TileXR MoonEP rank {context.global_rank}] projections_registered",
                flush=True,
            )
        planning_epoch = 0

        def coordinated_iteration(timer=None):
            nonlocal planning_epoch
            epoch = planning_epoch
            _oversubscribed_planning_barrier(buffer, case.case_id, epoch)
            planning_epoch += 1
            return execute_iteration(
                buffer,
                inputs,
                timer,
                torch_module=torch_module,
                stage_barrier=lambda stage: _multi_node_stage_barrier(
                    buffer,
                    case.case_id,
                    epoch,
                    stage,
                    quiesced=(
                        stage.startswith("combine_") or
                        (
                            stage == "expert_forward" and
                            os.environ.get(
                                "TILEXR_MOONEP_TRACE_SYNC_EACH_STAGE", "0"
                            ) == "1"
                        )
                    ),
                ),
            )

        if int(inputs["tokens_per_expert"].sum().item()) != context.route_count:
            raise RuntimeError("tokens_per_expert does not sum to S*K")
        validation = {"passed": True, "mode": "disabled"}
        if case.correctness:
            first = coordinated_iteration()
            validation = validate_plan(
                first[0],
                context,
                first[1],
                expected_status=FINAL_SHARED_STATUS_SUCCESS,
                route_distribution=case.route_distribution,
            )
            implementations = capabilities["implementations"]
            stub_stages = ("dispatch", "prefetch_weight", "combine", "reduce_grad")
            if all(implementations[stage] == "stub" for stage in stub_stages):
                validation["stub_flow_checks"] = validate_stub_flow(
                    torch_module, inputs, first[2], first[4], context
                )
                validation["mode"] = "planner_cpu_oracle_and_stub_flow"
            elif capabilities.get("transport_correctness_valid", False):
                validation["mode"] = "planner_cpu_oracle_and_native_status"
            first_forward_checksum = _checksum(first[2])
            first_backward_checksum = _checksum(first[4])
            first_checksum = first_forward_checksum + first_backward_checksum
            second = coordinated_iteration()
            second_forward_checksum = _checksum(second[2])
            second_backward_checksum = _checksum(second[4])
            second_checksum = second_forward_checksum + second_backward_checksum
            if os.environ.get("TILEXR_MOONEP_TRACE_CHECKSUMS", "0") == "1":
                print(
                    f"[TileXR MoonEP rank {context.global_rank}] "
                    f"checksums first_forward={first_forward_checksum} "
                    f"first_backward={first_backward_checksum} "
                    f"second_forward={second_forward_checksum} "
                    f"second_backward={second_backward_checksum}",
                    flush=True,
                )
            if first_checksum != second_checksum:
                raise RuntimeError(
                    f"MoonEP flow is not deterministic: {first_checksum} != {second_checksum}"
                )
            validation["checksum"] = first_checksum
            validation["deterministic"] = True
        warmup_plan = None
        for _ in range(case.warmup):
            warmup_plan = coordinated_iteration()[0]
        if (
            warmup_plan is not None
            and int(warmup_plan.status.item()) != FINAL_SHARED_STATUS_SUCCESS
        ):
            raise RuntimeError(
                f"MoonEP device status is {int(warmup_plan.status.item())} during "
                f"warmup, expected {FINAL_SHARED_STATUS_SUCCESS}"
            )
        if (
            warmup_plan is not None
            and int(warmup_plan.reduce_grad_status.item()) != REDUCE_GRAD_STATUS_SUCCESS
        ):
            raise RuntimeError(
                "MoonEP ReduceGrad device status is "
                f"{int(warmup_plan.reduce_grad_status.item())} during warmup, "
                f"expected {REDUCE_GRAD_STATUS_SUCCESS}"
            )
        samples = []
        for iteration in range(case.iterations):
            timer = DeviceEventTimer(torch_module)
            plan, _, forward, _, backward, timings = coordinated_iteration(timer)
            if int(plan.status.item()) != FINAL_SHARED_STATUS_SUCCESS:
                raise RuntimeError(
                    f"MoonEP device status is {int(plan.status.item())}, expected "
                    f"{FINAL_SHARED_STATUS_SUCCESS}"
                )
            if int(plan.reduce_grad_status.item()) != REDUCE_GRAD_STATUS_SUCCESS:
                raise RuntimeError(
                    "MoonEP ReduceGrad device status is "
                    f"{int(plan.reduce_grad_status.item())}, expected "
                    f"{REDUCE_GRAD_STATUS_SUCCESS}"
                )
            samples.append(
                {
                    "iteration": iteration,
                    "timings_us": timings,
                    "algorithm_bytes": algorithm_bytes(plan, inputs, context),
                    "checksums": {
                        "forward": _checksum(forward),
                        "backward": _checksum(backward),
                    },
                }
            )
        write_jsonl(rank_dir / "samples.jsonl", samples)
        result["status"] = "passed"
        result["validation"] = validation
    except Exception as exc:
        result["failure_reason"] = f"{type(exc).__name__}: {exc}"
        failure = (exc, exc.__traceback__)
    finally:
        world_size = int(os.environ.get("WORLD_SIZE", "1"))
        quiesced = buffer is None
        if buffer is not None:
            try:
                buffer.quiesce()
                quiesced = True
                buffer.check_pending_status()
            except Exception as sync_exc:
                record_failure("synchronize", sync_exc)
        local_passed = failure is None and result["status"] == "passed"
        try:
            decision = completion_barrier_from_env(
                global_rank,
                world_size,
                case_id=case.case_id,
                quiesced=quiesced,
                passed=local_passed,
            )
        except Exception as barrier_exc:
            record_failure("completion rendezvous", barrier_exc)
            _hold_unsafe_teardown(
                output_root=output_root,
                rank_dir=rank_dir,
                result=result,
                rank=global_rank,
                reason=str(barrier_exc),
                world_size=world_size,
            )
        if not decision.release:
            unsafe = RuntimeError("one or more ranks could not prove local NPU quiescence")
            record_failure("completion rendezvous", unsafe)
            _hold_unsafe_teardown(
                output_root=output_root,
                rank_dir=rank_dir,
                result=result,
                rank=global_rank,
                reason=str(unsafe),
                world_size=world_size,
            )
        if decision.abort and local_passed:
            record_failure(
                "completion rendezvous",
                RuntimeError("another rank failed the benchmark case"),
            )
        owner = buffer if buffer is not None else context
        if owner is not None:
            try:
                owner.close()
            except Exception as close_exc:
                record_failure("close", close_exc)
        write_json(rank_dir / "result.json", result)
    if failure is not None:
        raise failure[0].with_traceback(failure[1])


def _correctness_dimensions(case, *, rank: int, world_size: int) -> MoonEPDimensions:
    return MoonEPDimensions(
        rank=rank,
        world_size=world_size,
        tokens_per_rank=int(case.tokens_per_rank),
        topk=int(case.topk),
        expert_count=int(case.expert_count),
        prefetch_slots=int(case.expert_count) // world_size,
        token_padding=int(case.token_padding),
        hidden_size=int(case.hidden_size),
        intermediate_size=(
            int(case.intermediate_size)
            if case.intermediate_size is not None
            else int(case.hidden_size)
        ),
    )


def load_candidate_backend(spec: str, *, torch_module, dimensions, case, args):
    module_name, separator, factory_name = spec.partition(":")
    if not separator or not module_name or not factory_name:
        raise ValueError("candidate backend must use MODULE:FACTORY syntax")
    module = importlib.import_module(module_name)
    factory = getattr(module, factory_name, None)
    if factory is None or not callable(factory):
        raise BackendUnavailableError(
            f"candidate backend factory {spec!r} is not callable"
        )
    backend = factory(
        torch_module=torch_module,
        dimensions=dimensions,
        case=case,
        args=args,
    )
    if not isinstance(backend, MoonEPBackend):
        raise BackendUnavailableError(
            f"candidate backend factory {spec!r} did not return MoonEPBackend"
        )
    return backend


def run_correctness_case(torch_module, case, args, root: Path) -> None:
    del root
    rank = int(os.environ.get("RANK", "0"))
    world_size = int(os.environ.get("WORLD_SIZE", "1"))
    dimensions = _correctness_dimensions(case, rank=rank, world_size=world_size)
    device = f"npu:{int(os.environ.get('LOCAL_RANK', str(rank)))}"
    rank_dir = Path(args.output_dir) / case.case_id / f"rank_{rank}"
    dump_enabled = bool(getattr(args, "dump_stage_tensors", False))
    preview_elements = int(getattr(args, "tensor_preview_elements", 8))
    tensor_dump_dir = rank_dir / "tensor_dumps" if dump_enabled else None
    result = {
        "schema_version": 1,
        "status": "failed",
        "failure_reason": None,
        "mode": args.mode,
        "launch_id": os.environ.get("TILEXR_MOONEP_LAUNCH_ID", ""),
        "case": case.as_dict(),
        "rank": rank,
        "performance_valid": False,
        "tensor_dump": {
            "enabled": dump_enabled,
            "preview_elements": preview_elements if dump_enabled else None,
            "directory": "tensor_dumps" if dump_enabled else None,
        },
        "validation": None,
    }
    reference = None
    candidate = None
    failure = None
    try:
        canonical = make_correctness_case(
            torch_module,
            dimensions,
            case_id=case.case_id,
            routing_pattern=case.routing_pattern,
            device=device,
        )
        reference = TorchNpuMoonEPBackend(torch_module, dimensions)
        if args.mode == "correctness":
            candidate = load_candidate_backend(
                args.candidate_backend or DEFAULT_CORRECTNESS_BACKEND,
                torch_module=torch_module,
                dimensions=dimensions,
                case=case,
                args=args,
            )
        runner = CorrectnessRunner(
            torch_module,
            reference,
            candidate,
            artifact_dir=rank_dir / "stages",
            tensor_dump_dir=tensor_dump_dir,
            preview_elements=preview_elements,
            preview_sink=print,
        )
        report = (
            runner.run_reference(canonical)
            if args.mode == "reference"
            else runner.run_differential(canonical)
        )
        result["status"] = "passed"
        result["validation"] = report.as_dict()
    except Exception as exc:
        result["failure_reason"] = f"{type(exc).__name__}: {exc}"
        failure = (exc, exc.__traceback__)
    finally:
        cleanup_errors = []
        for name, backend in (("candidate", candidate), ("reference", reference)):
            if backend is None:
                continue
            try:
                backend.close()
            except Exception as close_exc:
                cleanup_errors.append(
                    f"{name}: {type(close_exc).__name__}: {close_exc}"
                )
                if failure is None:
                    result["status"] = "failed"
                    result["failure_reason"] = (
                        f"{type(close_exc).__name__}: {close_exc}"
                    )
                    failure = (close_exc, close_exc.__traceback__)
        if cleanup_errors:
            result["cleanup_errors"] = cleanup_errors
        write_json(rank_dir / "result.json", result)
    if failure is not None:
        raise failure[0].with_traceback(failure[1])


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="TileXR MoonEP complete-flow benchmark")
    build_case_parser(parser)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--install-prefix", default=None)
    parser.add_argument("--wait-iterations", type=int, default=1_000_000)
    parser.add_argument(
        "--mode", choices=("benchmark", "reference", "correctness"), default="benchmark"
    )
    parser.add_argument(
        "--benchmark-kind", choices=("flow", "model_flow"), default="flow"
    )
    parser.add_argument("--candidate-backend", default=None, metavar="MODULE:FACTORY")
    parser.add_argument(
        "--dump-stage-tensors",
        action="store_true",
        help="save complete untimed correctness-stage inputs and outputs",
    )
    parser.add_argument("--tensor-preview-elements", type=int, default=8)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.wait_iterations <= 0 or args.tensor_preview_elements <= 0:
        raise ValueError("wait_iterations and tensor_preview_elements must be positive")
    if args.mode == "benchmark" and args.dump_stage_tensors:
        raise ValueError("--dump-stage-tensors is only valid in reference/correctness mode")
    if args.mode != "benchmark" and args.benchmark_kind != "flow":
        raise ValueError("--benchmark-kind model_flow requires --mode benchmark")
    if (
        int(os.environ.get("WORLD_SIZE", "1")) > 1
        and os.environ.get("TILEXR_MOONEP_MANAGED_LAUNCH") != "1"
    ):
        raise RuntimeError("multi-rank MoonEP workers require the managed launcher")
    import torch

    if args.mode != "benchmark":
        importlib.import_module("torch_npu")

    local_rank = int(os.environ.get("LOCAL_RANK", "0"))
    torch.npu.set_device(local_rank)
    initialized_group = False
    if args.mode != "benchmark" and int(os.environ.get("WORLD_SIZE", "1")) > 1:
        if not torch.distributed.is_initialized():
            torch.distributed.init_process_group(
                backend=_reference_process_group_backend(os.environ, mode=args.mode),
                rank=int(os.environ["RANK"]),
                world_size=int(os.environ["WORLD_SIZE"]),
            )
            initialized_group = True
    cases = select_cases(load_cases(args.cases), args.case_ids)
    cases = [apply_mode_defaults(case, mode=args.mode) for case in cases]
    cases = [apply_overrides(case, args) for case in cases]
    root = Path(__file__).resolve().parents[2]
    if int(os.environ.get("RANK", "0")) == 0:
        write_json(
            Path(args.output_dir) / "metadata.json",
            {
                "schema_version": 1,
                "cases": [case.as_dict() for case in cases],
                "environment": environment_metadata(torch, root),
            },
        )
    try:
        for case in cases:
            if args.mode == "benchmark":
                if args.benchmark_kind == "model_flow":
                    from .model_flow import run_model_case

                    run_model_case(torch, case, args, root)
                else:
                    run_case(torch, case, args, root)
            else:
                run_correctness_case(torch, case, args, root)
    finally:
        if initialized_group:
            torch.distributed.destroy_process_group()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
