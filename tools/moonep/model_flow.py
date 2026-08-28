from __future__ import annotations

import array
import base64
import csv
import hashlib
import heapq
import json
import math
import os
import sys
import time
import zlib
from collections import Counter
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Mapping

from .report import write_json, write_jsonl


@dataclass(frozen=True)
class ModelFlowCaseSpec:
    world_size: int
    replay_filename: str


MODEL_FLOW_CASE_ID = "model-flow-8rank-4k-ep8-mindspeed"
MODEL_FLOW_CASE_SPECS = {
    MODEL_FLOW_CASE_ID: ModelFlowCaseSpec(
        world_size=8,
        replay_filename="mindspeed_4k_ep8_route_counts.json",
    ),
    "model-flow-16rank-4k-ep16-mindspeed": ModelFlowCaseSpec(
        world_size=16,
        replay_filename="mindspeed_4k_ep16_route_counts.json",
    ),
    "model-flow-8rank-8k-k16-ep8-mindspeed": ModelFlowCaseSpec(
        world_size=8,
        replay_filename="mindspeed_8k_k16_ep8_route_counts.json",
    ),
    "model-flow-16rank-8k-k16-ep16-mindspeed": ModelFlowCaseSpec(
        world_size=16,
        replay_filename="mindspeed_8k_k16_ep16_route_counts.json",
    ),
}
MODEL_FORWARD_CALLS = 10
MODEL_BACKWARD_CALLS = 5
MODEL_LOGICAL_STAGE_COUNTS = Counter(
    {
        "planning": MODEL_FORWARD_CALLS,
        "dispatch_forward": MODEL_FORWARD_CALLS,
        "prefetch_weight": MODEL_FORWARD_CALLS,
        "combine_forward": MODEL_FORWARD_CALLS,
        "dispatch_backward": MODEL_BACKWARD_CALLS,
        "combine_backward": MODEL_BACKWARD_CALLS,
        "reduce_grad": MODEL_BACKWARD_CALLS,
    }
)

_FORWARD_STAGES = (
    "planning",
    "dispatch_forward",
    "prefetch_weight",
    "combine_forward",
)
_BACKWARD_STAGES = (
    "dispatch_backward",
    "combine_backward",
    "reduce_grad",
)
_FORWARD_CALL_INDICES = (0, 1, 2, 3, 4, 5, 7, 9, 11, 13)
_BACKWARD_CALL_INDICES = (6, 8, 10, 12, 14)
_FORWARD_DISPATCH_PAIRS = (
    (0, 1),
    (2, 3),
    (4, 5),
    (6, 7),
    (8, 9),
    (10, 11),
    (13, 14),
    (16, 17),
    (19, 20),
    (22, 23),
)
_BACKWARD_DISPATCH_OLD = (12, 15, 18, 21, 24)
_FORWARD_COMBINE_V2 = (0, 1, 2, 3, 4, 5, 8, 11, 14, 17)
_BACKWARD_COMBINE_V2 = ((6, 7), (9, 10), (12, 13), (15, 16), (18, 19))

_KERNEL_NAMES = {
    "planning": ("tilexr_ep_plan_kernel", "tilexr_moonep_planner_kernel"),
    "dispatch": ("tilexr_moonep_dispatch_urma_kernel",),
    "prefetch_weight": ("tilexr_moonep_prefetch_weight_kernel",),
    "combine_v1": ("tilexr_moonep_combine_kernel",),
    "combine_v2": ("tilexr_moonep_combine_v2_kernel",),
    "reduce_grad": ("tilexr_moonep_reduce_grad_kernel",),
}

MODEL_ROUTE_REPLAY = (
    Path(__file__).resolve().parent / "cases" / "mindspeed_4k_ep8_route_counts.json"
)


def model_flow_case_spec(
    case_id: str, *, dynamic_world_size: int | None = None
) -> ModelFlowCaseSpec:
    try:
        return MODEL_FLOW_CASE_SPECS[case_id]
    except KeyError as exc:
        if case_id.startswith("model-replay-"):
            if dynamic_world_size is None or dynamic_world_size <= 0:
                raise ValueError(
                    "dynamic model replay requires a positive runtime world size"
                ) from exc
            return ModelFlowCaseSpec(
                world_size=dynamic_world_size,
                replay_filename="",
            )
        supported = ", ".join(sorted(MODEL_FLOW_CASE_SPECS))
        raise ValueError(
            f"unsupported model_flow case {case_id!r}; expected one of: {supported}"
        ) from exc


def model_route_replay_path(spec: ModelFlowCaseSpec) -> Path:
    override = os.environ.get("TILEXR_MOONEP_MODEL_ROUTE_REPLAY")
    if override:
        return Path(override).expanduser().resolve()
    if not spec.replay_filename:
        raise ValueError(
            "dynamic model replay requires TILEXR_MOONEP_MODEL_ROUTE_REPLAY"
        )
    return Path(__file__).resolve().parent / "cases" / spec.replay_filename


def model_operator_order() -> list[dict[str, object]]:
    result = []
    occurrences: Counter[str] = Counter()

    def append(phase: str, layer: int, stage: str) -> None:
        result.append(
            {
                "sequence": len(result),
                "phase": phase,
                "layer": layer,
                "stage": stage,
                "stage_occurrence": occurrences[stage],
            }
        )
        occurrences[stage] += 1

    for layer in range(MODEL_BACKWARD_CALLS):
        for stage in _FORWARD_STAGES:
            append("initial_forward", layer, stage)
    for layer in reversed(range(MODEL_BACKWARD_CALLS)):
        for stage in _FORWARD_STAGES:
            append("recompute_backward", layer, stage)
        for stage in _BACKWARD_STAGES:
            append("recompute_backward", layer, stage)
    if occurrences != MODEL_LOGICAL_STAGE_COUNTS:
        raise RuntimeError(f"invalid model operator order: {occurrences}")
    return result


def routes_from_expert_counts(
    torch_module,
    expert_counts: Iterable[int],
    *,
    tokens_per_rank: int,
    topk: int,
    device,
):
    counts = [int(value) for value in expert_counts]
    if tokens_per_rank <= 0 or topk <= 0 or len(counts) < topk:
        raise ValueError("route replay dimensions must be positive and E >= K")
    if any(value < 0 or value > tokens_per_rank for value in counts):
        raise ValueError("each replay expert count must be in [0, S]")
    if sum(counts) != tokens_per_rank * topk:
        raise ValueError("replay expert counts must sum to S*K")

    heap = [(-count, expert) for expert, count in enumerate(counts) if count]
    heapq.heapify(heap)
    routes = []
    for _ in range(tokens_per_rank):
        if len(heap) < topk:
            raise ValueError("replay histogram cannot provide unique TopK experts")
        selected = [heapq.heappop(heap) for _ in range(topk)]
        routes.extend(expert for _, expert in selected)
        for negative_count, expert in selected:
            if negative_count + 1 < 0:
                heapq.heappush(heap, (negative_count + 1, expert))
    if heap:
        raise RuntimeError("route replay left unassigned expert counts")
    return torch_module.tensor(
        routes, dtype=torch_module.int32, device=device
    ).reshape(tokens_per_rank, topk)


def load_route_replay(
    path: str | Path,
    *,
    rank: int,
    world_size: int,
    tokens_per_rank: int,
    topk: int,
    expert_count: int,
    include_topk: bool = False,
    include_plan: bool = False,
):
    source = Path(path)
    with source.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    expected_dimensions = {
        "world_size": world_size,
        "tokens_per_rank": tokens_per_rank,
        "topk": topk,
        "expert_count": expert_count,
        "forward_calls": MODEL_FORWARD_CALLS,
    }
    if payload.get("schema_version") != 1:
        raise ValueError("model route replay schema_version must be 1")
    if payload.get("dimensions") != expected_dimensions:
        raise ValueError("model route replay dimensions do not match Case 17")
    ranks = payload.get("ranks")
    if not isinstance(ranks, dict) or set(ranks) != {
        str(value) for value in range(world_size)
    }:
        raise ValueError("model route replay must contain every rank exactly once")
    records = ranks[str(rank)]
    if not isinstance(records, list) or len(records) != MODEL_FORWARD_CALLS:
        raise ValueError("model route replay must contain ten forward calls per rank")
    counts = []
    topk_routes = []
    captured_plans = []
    prefetch_slots = expert_count // world_size
    for call, record in enumerate(records):
        if not isinstance(record, dict) or int(record.get("call", -1)) != call:
            raise ValueError("model route replay call order is invalid")
        values = record.get("tokens_per_expert")
        if not isinstance(values, list) or len(values) != expert_count:
            raise ValueError("model route replay expert histogram is invalid")
        normalized = [int(value) for value in values]
        if sum(normalized) != tokens_per_rank * topk:
            raise ValueError("model route replay histogram does not sum to S*K")
        counts.append(normalized)
        if include_topk:
            if record.get("topk_encoding") != "int32-le-zlib-base64":
                raise ValueError("model route replay TopK encoding is invalid")
            try:
                raw = zlib.decompress(
                    base64.b64decode(record["topk_zlib_base64"], validate=True)
                )
            except (KeyError, ValueError, zlib.error) as error:
                raise ValueError("model route replay TopK payload is invalid") from error
            if (
                len(raw) != tokens_per_rank * topk * 4
                or hashlib.sha256(raw).hexdigest() != record.get("topk_sha256")
            ):
                raise ValueError("model route replay TopK checksum is invalid")
            values = array.array("i")
            values.frombytes(raw)
            if values.itemsize != 4:
                raise RuntimeError("Case 17 requires 32-bit array integers")
            if sys.byteorder != "little":
                values.byteswap()
            topk_routes.append(values)
        if include_plan:
            remote_stats = [int(value) for value in record.get("remote_stats", [])]
            experts_to_copy = [
                [int(value) for value in row]
                for row in record.get("experts_to_copy", [])
            ]
            if (
                len(remote_stats) != 2
                or any(value < 0 for value in remote_stats)
                or len(experts_to_copy) != world_size
                or any(len(row) != prefetch_slots for row in experts_to_copy)
                or any(
                    expert < -1 or expert >= expert_count
                    for row in experts_to_copy
                    for expert in row
                )
            ):
                raise ValueError("model route replay captured plan is invalid")
            captured_plans.append(
                {
                    "remote_stats": remote_stats,
                    "experts_to_copy": experts_to_copy,
                }
            )
    provenance = payload.get("provenance")
    if not isinstance(provenance, dict):
        raise ValueError("model route replay provenance is missing")
    if include_topk and include_plan:
        return counts, topk_routes, captured_plans, dict(provenance)
    if include_topk:
        return counts, topk_routes, dict(provenance)
    if include_plan:
        return counts, captured_plans, dict(provenance)
    return counts, dict(provenance)


def _model_projection_shapes(case, context) -> dict[str, tuple[int, ...]]:
    local = int(context.experts_per_rank)
    rows = int(case.expert_count) + int(context.prefetch_slots)
    rank_size = int(context.planner_group_size)
    hidden = int(case.hidden_size)
    intermediate = int(case.intermediate_size)
    return {
        "local_fc1": (local, hidden, 2 * intermediate),
        "local_up_dummy": (local, 32),
        "local_fc2": (local, intermediate, hidden),
        "full_fc1_weight": (rows, hidden, 2 * intermediate),
        "full_fc2_weight": (rows, intermediate, hidden),
        "full_fc1_grad": (rows, hidden, 2 * intermediate),
        "full_up_dummy_grad": (rows, 16),
        "full_fc2_grad": (rows, intermediate, hidden),
        "fc1_reduce": (rank_size, int(context.prefetch_slots), hidden, 2 * intermediate),
        "up_dummy_reduce": (rank_size, int(context.prefetch_slots), 16),
        "fc2_reduce": (rank_size, int(context.prefetch_slots), intermediate, hidden),
    }


def make_model_inputs(torch_module, case, context, *, replay_path=None):
    if replay_path is None:
        replay_path = model_route_replay_path(
            model_flow_case_spec(
                case.case_id,
                dynamic_world_size=int(context.planner_group_size),
            )
        )
    from tilexr_moonep import ProjectionBuffers

    device = f"npu:{context.device_index}"
    dtype = torch_module.bfloat16
    route_counts, replay_routes, captured_plans, route_provenance = load_route_replay(
        replay_path,
        rank=int(context.planner_group_rank),
        world_size=int(context.planner_group_size),
        tokens_per_rank=int(case.tokens_per_rank),
        topk=int(case.topk),
        expert_count=int(case.expert_count),
        include_topk=True,
        include_plan=True,
    )
    routes = [
        torch_module.tensor(values, dtype=torch_module.int32, device=device).reshape(
            int(case.tokens_per_rank), int(case.topk)
        )
        for values in replay_routes
    ]
    tokens_per_expert = [
        torch_module.tensor(counts, dtype=torch_module.int32, device=device)
        for counts in route_counts
    ]

    torch_module.manual_seed(int(case.seed) + int(context.global_rank))
    if hasattr(torch_module.npu, "manual_seed_all"):
        torch_module.npu.manual_seed_all(int(case.seed) + int(context.global_rank))
    hidden = torch_module.randn(
        (int(case.tokens_per_rank), int(case.hidden_size)),
        dtype=dtype,
        device=device,
    )
    route_weights = torch_module.full(
        (int(case.tokens_per_rank), int(case.topk)),
        1.0 / int(case.topk),
        dtype=torch_module.float32,
        device=device,
    )
    shapes = _model_projection_shapes(case, context)
    local_fc1 = torch_module.zeros(shapes["local_fc1"], dtype=dtype, device=device)
    local_up = torch_module.zeros(
        shapes["local_up_dummy"], dtype=dtype, device=device
    )
    local_fc2 = torch_module.zeros(shapes["local_fc2"], dtype=dtype, device=device)

    def arena(required_bytes, arena_dtype):
        backing = context.promote_projection_arena(
            torch_module, arena_dtype, required_bytes
        )
        if backing is None:
            raise RuntimeError(
                "Case 17 requires TILEXR_MOONEP_UDMA_ARENA_RESERVE_BYTES"
            )
        return backing

    projections = ProjectionBuffers.from_local_weights(
        context,
        local_fc1,
        local_up,
        local_fc2,
        registration_backing_factory=arena,
        torch_module=torch_module,
    )
    del local_fc1, local_up, local_fc2

    full_fc1_weight = torch_module.zeros(
        shapes["full_fc1_weight"], dtype=dtype, device=device
    )
    full_fc2_weight = torch_module.zeros(
        shapes["full_fc2_weight"], dtype=dtype, device=device
    )
    local_begin = int(context.planner_group_rank) * int(context.experts_per_rank)
    local_end = local_begin + int(context.experts_per_rank)
    full_fc1_weight[local_begin:local_end].copy_(
        projections.gate[: int(context.experts_per_rank)]
    )
    full_fc2_weight[local_begin:local_end].copy_(
        projections.down[: int(context.experts_per_rank)]
    )

    full_fc1 = torch_module.zeros(
        shapes["full_fc1_grad"], dtype=torch_module.float32, device=device
    )
    full_up = torch_module.zeros(
        shapes["full_up_dummy_grad"], dtype=torch_module.float32, device=device
    )
    full_fc2 = torch_module.zeros(
        shapes["full_fc2_grad"], dtype=torch_module.float32, device=device
    )
    fc1_reduce = torch_module.zeros(
        shapes["fc1_reduce"], dtype=torch_module.float32, device=device
    )
    up_reduce_elements = math.prod(shapes["up_dummy_reduce"])
    up_reduce_allocation = torch_module.zeros(
        (max(up_reduce_elements, 2 * 1024 * 1024 // 4),),
        dtype=torch_module.float32,
        device=device,
    )
    up_reduce = up_reduce_allocation.narrow(0, 0, up_reduce_elements).reshape(
        shapes["up_dummy_reduce"]
    )
    up_reduce._tilexr_registration_backing = up_reduce_allocation
    fc2_reduce = torch_module.zeros(
        shapes["fc2_reduce"], dtype=torch_module.float32, device=device
    )
    gradients = ProjectionBuffers(
        gate=full_fc1,
        up=full_up,
        down=full_fc2,
        gate_reduce=fc1_reduce,
        up_reduce=up_reduce,
        down_reduce=fc2_reduce,
    )
    return {
        "topk_experts": routes,
        "tokens_per_expert": tokens_per_expert,
        "hidden": hidden,
        "route_weights": route_weights,
        "grad_output": torch_module.ones_like(hidden),
        "projections": projections,
        "compute_weights": {
            "fc1": full_fc1_weight,
            "fc2": full_fc2_weight,
        },
        "gradients": gradients,
        "route_provenance": route_provenance,
        "captured_plans": captured_plans,
        "projection_shapes": shapes,
    }


def _row_bytes(tensor) -> int:
    return int(tensor.numel()) * int(tensor.element_size()) // int(tensor.shape[0])


def _operator_bytes(stage: str, plan, inputs, context) -> int | None:
    route_count = int(context.route_count)
    hidden_bytes = route_count * _row_bytes(inputs["hidden"])
    route_weight_bytes = route_count * int(inputs["route_weights"].element_size())
    if stage == "planning":
        return None
    if stage in ("dispatch_forward", "combine_backward"):
        return hidden_bytes + route_weight_bytes
    if stage in ("dispatch_backward", "combine_forward"):
        return hidden_bytes
    remote_stats = [int(value) for value in plan.remote_stats.cpu().tolist()]
    if len(remote_stats) != 2 or any(value < 0 for value in remote_stats):
        raise RuntimeError(f"invalid model plan remote_stats: {remote_stats}")
    if stage == "prefetch_weight":
        row_bytes = sum(
            _row_bytes(getattr(inputs["projections"], name))
            for name in ("gate", "up", "down")
        )
        return remote_stats[0] * row_bytes
    if stage == "reduce_grad":
        row_bytes = sum(
            _row_bytes(getattr(inputs["gradients"], name))
            for name in ("gate", "up", "down")
        )
        return remote_stats[1] * row_bytes
    raise ValueError(f"unknown model stage {stage}")


def run_packed_expert_compute(
    torch_module,
    plan,
    hidden_nvsh,
    cu_seqlens,
    projections,
    route_weights_nvs,
    *,
    context,
    compute_weights,
    torch_npu_module,
):
    local = int(context.experts_per_rank)
    slot_begin = int(context.expert_count)
    experts = plan.experts_to_copy[int(context.planner_group_rank)]
    for slot in range(int(context.prefetch_slots)):
        if int(experts[slot].item()) < 0:
            continue
        compute_weights["fc1"][slot_begin + slot].copy_(
            projections.gate[local + slot]
        )
        compute_weights["fc2"][slot_begin + slot].copy_(
            projections.down[local + slot]
        )

    group_list = cu_seqlens.to(dtype=torch_module.int64).contiguous()
    valid_rows = int(group_list[-1].item())
    output = torch_module.zeros_like(hidden_nvsh)
    if valid_rows == 0:
        return output

    grouped_args = {
        "split_item": 3,
        "group_list_type": 0,
        "group_type": 0,
        "group_list": group_list,
    }
    fc1 = torch_npu_module.npu_grouped_matmul(
        x=[hidden_nvsh[:valid_rows]],
        weight=[compute_weights["fc1"]],
        **grouped_args,
    )
    if not isinstance(fc1, (tuple, list)) or len(fc1) != 1:
        raise RuntimeError("Case 17 FC1 grouped matmul must return one tensor")
    activated = torch_npu_module.npu_swiglu(fc1[0], dim=-1)
    if route_weights_nvs is not None:
        activated = (activated * route_weights_nvs[:valid_rows].unsqueeze(-1)).to(
            activated.dtype
        )
    fc2 = torch_npu_module.npu_grouped_matmul(
        x=[activated],
        weight=[compute_weights["fc2"]],
        **grouped_args,
    )
    if not isinstance(fc2, (tuple, list)) or len(fc2) != 1:
        raise RuntimeError("Case 17 FC2 grouped matmul must return one tensor")
    expert_output = fc2[0]
    output[:valid_rows].copy_(expert_output)
    return output


def execute_model_iteration(
    buffer, inputs, *, torch_module, expert_compute=None,
    stage_barrier: Callable[[str], None] | None = None,
):
    calls = []
    forward_plans = []

    def record(phase: str, layer: int, stage: str, plan) -> None:
        calls.append({"phase": phase, "layer": layer, "stage": stage, "plan": plan})

    def forward(call: int, phase: str, layer: int):
        if stage_barrier is not None:
            stage_barrier(f"{phase}_{layer}_dispatch_forward")
        hidden, route_weights, cu_seqlens, plan = buffer.dispatch(
            inputs["hidden"],
            inputs["route_weights"],
            inputs["topk_experts"][call],
            inputs["tokens_per_expert"][call],
        )
        record(phase, layer, "planning", plan)
        record(phase, layer, "dispatch_forward", plan)
        forward_plans.append(plan)
        buffer.prefetch_weight(plan, inputs["projections"])
        record(phase, layer, "prefetch_weight", plan)
        expert_output = hidden
        if expert_compute is not None:
            expert_output = expert_compute(
                plan, hidden, cu_seqlens, inputs["projections"], route_weights
            )
        if stage_barrier is not None:
            stage_barrier(f"{phase}_{layer}_combine_forward")
        combined, _, _ = buffer.combine(plan, expert_output)
        record(phase, layer, "combine_forward", plan)
        return plan, hidden, route_weights, cu_seqlens, combined

    final = None
    for layer in range(MODEL_BACKWARD_CALLS):
        _, _, _, _, final = forward(layer, "initial_forward", layer)
    for backward_index, layer in enumerate(reversed(range(MODEL_BACKWARD_CALLS))):
        call = MODEL_BACKWARD_CALLS + backward_index
        plan, _, dispatched_route_weights, cu_seqlens, final = forward(
            call, "recompute_backward", layer
        )
        if stage_barrier is not None:
            stage_barrier(f"recompute_backward_{layer}_dispatch_backward")
        dispatched_grad, _, _, returned_plan = buffer.dispatch(
            inputs["grad_output"], plan=plan
        )
        if returned_plan is not plan:
            raise RuntimeError("model backward Dispatch returned a different plan")
        record("recompute_backward", layer, "dispatch_backward", plan)
        backward_input = dispatched_grad
        if expert_compute is not None:
            backward_input = expert_compute(
                plan,
                dispatched_grad,
                cu_seqlens,
                inputs["projections"],
                dispatched_route_weights,
            )
        if stage_barrier is not None:
            stage_barrier(f"recompute_backward_{layer}_combine_backward")
        final, _, _ = buffer.combine(
            plan, backward_input, dispatched_route_weights
        )
        record("recompute_backward", layer, "combine_backward", plan)
        gradients = inputs["gradients"]
        if stage_barrier is not None:
            stage_barrier(f"recompute_backward_{layer}_reduce_grad")
        buffer.reduce_grad(
            plan,
            full_gate_grad=gradients.gate,
            full_up_grad=gradients.up,
            full_down_grad=gradients.down,
            gate_reduce_buffer=gradients.gate_reduce,
            up_reduce_buffer=gradients.up_reduce,
            down_reduce_buffer=gradients.down_reduce,
        )
        record("recompute_backward", layer, "reduce_grad", plan)
        # MindSpeed's compatibility wrapper completes each ReduceGrad before the
        # next layer because the runtime owns a single shared ReduceGrad arena.
        buffer.synchronize()

    expected = model_operator_order()
    actual = [(item["phase"], item["layer"], item["stage"]) for item in calls]
    expected_keys = [(item["phase"], item["layer"], item["stage"]) for item in expected]
    if actual != expected_keys:
        raise RuntimeError("model flow operator order diverged from the profiler contract")
    buffer.synchronize()
    captured_plans = inputs.get("captured_plans")
    if captured_plans is not None:
        if len(captured_plans) != len(forward_plans):
            raise RuntimeError("model captured plan count does not match forward calls")
        for call, (plan, expected_plan) in enumerate(
            zip(forward_plans, captured_plans)
        ):
            remote_stats = [int(value) for value in plan.remote_stats.cpu().tolist()]
            experts_to_copy = [
                [int(value) for value in row]
                for row in plan.experts_to_copy.cpu().tolist()
            ]
            if (
                remote_stats != expected_plan["remote_stats"]
                or experts_to_copy != expected_plan["experts_to_copy"]
            ):
                raise RuntimeError(f"model plan replay mismatch at forward call {call}")
    operator_bytes = [
        _operator_bytes(str(item["stage"]), call["plan"], inputs, buffer.context)
        for item, call in zip(expected, calls)
    ]
    checksum = float(final.float().sum().item())
    if not math.isfinite(checksum):
        raise RuntimeError("model flow produced a non-finite checksum")
    return {"operator_bytes": operator_bytes, "checksum": checksum}


@contextmanager
def capture_npu_profile(torch_npu_module, output_dir: str | Path):
    profiler = torch_npu_module.profiler
    handler = profiler.tensorboard_trace_handler(
        str(Path(output_dir)), analyse_flag=True
    )
    with profiler.profile(
        activities=[profiler.ProfilerActivity.NPU],
        on_trace_ready=handler,
        record_shapes=False,
        profile_memory=False,
        with_stack=False,
        with_modules=False,
    ) as session:
        yield session


def _find_kernel_csv(profile_root: Path, *, timeout_seconds: float = 120.0) -> Path:
    deadline = time.monotonic() + timeout_seconds
    while True:
        paths = list(profile_root.rglob("kernel_details.csv"))
        if len(paths) == 1:
            return paths[0]
        if len(paths) > 1:
            raise RuntimeError(f"Case 17 profiler produced multiple Kernel CSV files: {paths}")
        if time.monotonic() >= deadline:
            raise TimeoutError("Case 17 profiler did not produce kernel_details.csv")
        time.sleep(0.1)


def write_rank_model_profile(
    rank_dir: Path,
    *,
    iterations: int,
    iteration_samples: list[dict[str, object]],
) -> dict[str, object]:
    kernel_csv = _find_kernel_csv(rank_dir / "profiler")
    with kernel_csv.open(newline="", encoding="utf-8-sig") as handle:
        operators = parse_tilexr_kernel_rows(csv.DictReader(handle), iterations=iterations)
    if len(iteration_samples) != iterations:
        raise ValueError("model sample count does not match profiler iterations")
    for sequence, operator in enumerate(operators):
        byte_values = [
            sample["operator_bytes"][sequence] for sample in iteration_samples
        ]
        operator["algorithm_bytes"] = byte_values
    profile = {
        "schema_version": 1,
        "timing_source": "torch_npu_profiler_kernel_details",
        "kernel_csv": str(kernel_csv.relative_to(rank_dir)),
        "iterations": iterations,
        "operators": operators,
    }
    write_json(rank_dir / "model_profile.json", profile)
    return profile


def run_model_case(torch_module, case, args, root: Path) -> None:
    from tilexr_moonep import TileXRMoonEPBuffer, TileXRMoonEPContext

    from .benchmark import (
        _hold_unsafe_teardown,
        _multi_node_stage_barrier,
        environment_metadata,
        stage_execution_metadata,
        topology_metadata,
    )
    from .rendezvous import completion_barrier_from_env

    global_rank = int(os.environ.get("RANK", "0"))
    world_size = int(os.environ.get("WORLD_SIZE", "1"))
    spec = model_flow_case_spec(case.case_id, dynamic_world_size=world_size)
    if world_size != spec.world_size:
        raise ValueError(
            f"{case.case_id} requires exactly {spec.world_size} ranks"
        )
    output_root = Path(args.output_dir).resolve()
    rank_dir = (output_root / case.case_id / f"rank_{global_rank}").resolve()
    if output_root not in rank_dir.parents:
        raise ValueError(f"case artifact path escapes output root: {rank_dir}")
    rank_dir.mkdir(parents=True, exist_ok=True)
    environment = environment_metadata(torch_module, root)
    result = {
        "schema_version": 1,
        "benchmark_kind": "model_flow",
        "status": "failed",
        "failure_reason": None,
        "mode": args.mode,
        "launch_id": os.environ.get("TILEXR_MOONEP_LAUNCH_ID", ""),
        "case": case.as_dict(),
        "rank": global_rank,
        "topology": None,
        "capabilities": None,
        "environment": environment,
        "benchmark_config": {
            "wait_iterations": int(args.wait_iterations),
            "model_compute": "packed_fc1_gmm_swiglu_fc2_gmm",
        },
        "stage_execution": None,
        "model_operator_order": model_operator_order(),
        "profile_timing_source": "torch_npu_profiler_kernel_details",
        "validation": {"passed": False, "mode": "not_run"},
    }
    context = None
    buffer = None
    failure = None

    def record_failure(step: str, exc: Exception) -> None:
        nonlocal failure
        reason = f"{step} failed: {type(exc).__name__}: {exc}"
        result["failure_reason"] = (
            f"{result['failure_reason']}; {reason}"
            if result["failure_reason"]
            else reason
        )
        result["status"] = "failed"
        if failure is None:
            failure = (exc, exc.__traceback__)

    try:
        context = TileXRMoonEPContext.from_env(
            tokens_per_rank=case.tokens_per_rank,
            hidden_size=case.hidden_size,
            topk=case.topk,
            expert_count=case.expert_count,
            dtype=torch_module.bfloat16,
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
            combine_version=int(getattr(context.runtime, "combine_version", 2)),
        )
        result["topology"] = topology_metadata(context)
        inputs = make_model_inputs(torch_module, case, context)
        result["route_provenance"] = inputs["route_provenance"]
        result["projection_shapes"] = {
            name: list(shape) for name, shape in inputs["projection_shapes"].items()
        }
        buffer.register_projection_buffers(inputs["projections"])
        torch_npu_module = __import__("torch_npu")

        def expert_compute(plan, hidden, cu_seqlens, projections, route_weights):
            return run_packed_expert_compute(
                torch_module,
                plan,
                hidden,
                cu_seqlens,
                projections,
                route_weights,
                context=context,
                compute_weights=inputs["compute_weights"],
                torch_npu_module=torch_npu_module,
            )

        stage_barrier_epoch = 0

        def coordinated_model_iteration():
            nonlocal stage_barrier_epoch
            epoch = stage_barrier_epoch
            stage_barrier_epoch += 1
            return execute_model_iteration(
                buffer,
                inputs,
                torch_module=torch_module,
                expert_compute=expert_compute,
                stage_barrier=lambda stage: _multi_node_stage_barrier(
                    buffer, case.case_id, epoch, stage
                ),
            )

        warmup_checksums = []
        for _ in range(int(case.warmup)):
            warmup_checksums.append(coordinated_model_iteration()["checksum"])

        samples = []
        if int(case.iterations) > 0:
            with capture_npu_profile(torch_npu_module, rank_dir / "profiler") as profiler:
                for iteration in range(int(case.iterations)):
                    sample = coordinated_model_iteration()
                    samples.append({"iteration": iteration, **sample})
                    step = getattr(profiler, "step", None)
                    if callable(step):
                        step()
            write_rank_model_profile(
                rank_dir,
                iterations=int(case.iterations),
                iteration_samples=samples,
            )
        write_jsonl(rank_dir / "samples.jsonl", samples)
        checksums = warmup_checksums + [float(item["checksum"]) for item in samples]
        if checksums and any(value != checksums[0] for value in checksums[1:]):
            raise RuntimeError(f"{case.case_id} model flow is not deterministic")
        result["status"] = "passed"
        result["validation"] = {
            "passed": True,
            "mode": "model_order_status_and_checksum",
            "operator_count": len(model_operator_order()),
            "checksum": checksums[0] if checksums else None,
        }
    except Exception as exc:
        result["failure_reason"] = f"{type(exc).__name__}: {exc}"
        failure = (exc, exc.__traceback__)
    finally:
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


def _duration_values(rows: Iterable[Mapping[str, object]], names: tuple[str, ...]):
    return [
        float(row["Duration(us)"])
        for row in rows
        if str(row.get("Name", "")) in names
    ]


def _chunks(values: list[float], size: int, iterations: int, stage: str):
    if len(values) != size * iterations:
        raise ValueError(
            f"unexpected {stage} Kernel count {len(values)}; "
            f"expected {size * iterations} for {iterations} iterations"
        )
    return [values[index * size : (index + 1) * size] for index in range(iterations)]


def _select(values: list[float], indices: Iterable[int]) -> list[float]:
    return [values[index] for index in indices]


def _pair_sums(values: list[float], pairs: Iterable[tuple[int, int]]) -> list[float]:
    return [values[left] + values[right] for left, right in pairs]


def parse_tilexr_kernel_rows(
    rows: Iterable[Mapping[str, object]], *, iterations: int
) -> list[dict[str, object]]:
    if iterations <= 0:
        raise ValueError("model profiler iterations must be positive")
    rows = list(rows)
    planning = _duration_values(rows, _KERNEL_NAMES["planning"])
    dispatch = _duration_values(rows, _KERNEL_NAMES["dispatch"])
    prefetch = _duration_values(rows, _KERNEL_NAMES["prefetch_weight"])
    combine_v1 = _duration_values(rows, _KERNEL_NAMES["combine_v1"])
    combine_v2 = _duration_values(rows, _KERNEL_NAMES["combine_v2"])
    reduce_grad = _duration_values(rows, _KERNEL_NAMES["reduce_grad"])

    planning_chunks = _chunks(planning, 10, iterations, "Planning")
    prefetch_chunks = _chunks(prefetch, 10, iterations, "PrefetchWeight")
    reduce_chunks = _chunks(reduce_grad, 5, iterations, "ReduceGrad")

    if len(dispatch) == 15 * iterations:
        dispatch_chunks = _chunks(dispatch, 15, iterations, "Dispatch")
        dispatch_forward = [
            _select(values, _FORWARD_CALL_INDICES) for values in dispatch_chunks
        ]
        dispatch_backward = [
            _select(values, _BACKWARD_CALL_INDICES) for values in dispatch_chunks
        ]
        dispatch_forward_launches = 1
    elif len(dispatch) == 25 * iterations:
        dispatch_chunks = _chunks(dispatch, 25, iterations, "Dispatch")
        dispatch_forward = [
            _pair_sums(values, _FORWARD_DISPATCH_PAIRS) for values in dispatch_chunks
        ]
        dispatch_backward = [
            _select(values, _BACKWARD_DISPATCH_OLD) for values in dispatch_chunks
        ]
        dispatch_forward_launches = 2
    else:
        raise ValueError(f"unexpected TileXR Dispatch Kernel count {len(dispatch)}")

    if combine_v2 and not combine_v1:
        if len(combine_v2) == 15 * iterations:
            combine_chunks = _chunks(combine_v2, 15, iterations, "CombineV2")
            combine_forward = [
                _select(values, _FORWARD_CALL_INDICES) for values in combine_chunks
            ]
            combine_backward = [
                _select(values, _BACKWARD_CALL_INDICES) for values in combine_chunks
            ]
            combine_backward_launches = 1
        elif len(combine_v2) == 20 * iterations:
            combine_chunks = _chunks(combine_v2, 20, iterations, "CombineV2")
            combine_forward = [
                _select(values, _FORWARD_COMBINE_V2) for values in combine_chunks
            ]
            combine_backward = [
                _pair_sums(values, _BACKWARD_COMBINE_V2) for values in combine_chunks
            ]
            combine_backward_launches = 2
        else:
            raise ValueError(f"unexpected CombineV2 Kernel count {len(combine_v2)}")
        combine_kernel = _KERNEL_NAMES["combine_v2"][0]
        combine_forward_launches = 1
    elif combine_v1 and not combine_v2:
        combine_chunks = _chunks(combine_v1, 15, iterations, "CombineV1")
        combine_forward = [
            _select(values, _FORWARD_CALL_INDICES) for values in combine_chunks
        ]
        combine_backward = [
            _select(values, _BACKWARD_CALL_INDICES) for values in combine_chunks
        ]
        combine_kernel = _KERNEL_NAMES["combine_v1"][0]
        combine_forward_launches = 1
        combine_backward_launches = 1
    else:
        raise ValueError(
            "expected exactly one TileXR Combine kernel version, got "
            f"v1={len(combine_v1)} v2={len(combine_v2)}"
        )

    per_iteration = []
    for iteration in range(iterations):
        per_iteration.append(
            {
                "planning": planning_chunks[iteration],
                "dispatch_forward": dispatch_forward[iteration],
                "prefetch_weight": prefetch_chunks[iteration],
                "combine_forward": combine_forward[iteration],
                "dispatch_backward": dispatch_backward[iteration],
                "combine_backward": combine_backward[iteration],
                "reduce_grad": reduce_chunks[iteration],
            }
        )

    kernel_metadata = {
        "planning": (_KERNEL_NAMES["planning"][0], 1),
        "dispatch_forward": (_KERNEL_NAMES["dispatch"][0], dispatch_forward_launches),
        "prefetch_weight": (_KERNEL_NAMES["prefetch_weight"][0], 1),
        "combine_forward": (combine_kernel, combine_forward_launches),
        "dispatch_backward": (_KERNEL_NAMES["dispatch"][0], 1),
        "combine_backward": (combine_kernel, combine_backward_launches),
        "reduce_grad": (_KERNEL_NAMES["reduce_grad"][0], 1),
    }
    result = []
    for item in model_operator_order():
        stage = str(item["stage"])
        occurrence = int(item["stage_occurrence"])
        kernel_name, launches = kernel_metadata[stage]
        result.append(
            {
                **item,
                "kernel_name": kernel_name,
                "kernel_launches_per_call": launches,
                "values_us": [
                    values[stage][occurrence] for values in per_iteration
                ],
            }
        )
    return result
