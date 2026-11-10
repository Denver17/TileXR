# 驱动 MoonEP Dispatch 热循环，使用路由 oracle 检查输出并读取 DFX/完成状态；ACL event 提供批次时间。
from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from pathlib import Path

from .benchmark import environment_metadata, topology_metadata, validate_plan
from .config import apply_overrides, build_case_parser, load_cases, select_cases
from .planner_reference import (
    build_reference_plan,
    deterministic_rank_topk,
)
from .rendezvous import completion_barrier_from_env
from .report import write_json, write_jsonl

ROOT = Path(__file__).resolve().parents[2]
INTEGRATION = ROOT / "integrations" / "moonep_torch"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

DFX_MARKER = 0x54584444
KERNEL_STATUS_MARKER = 0x54584453
DIAGNOSTIC_VERSION = 3
KERNEL_STATUS_FEATURE_DFX_ENABLED = 1 << 0
KERNEL_STATUS_FEATURE_FUSED_EPOCH = 1 << 2
DISPATCH_CORE_CAPACITY = 64
DFX_COUNT = 64
COMPLETION_BYTES = 512 * 2 * 8
SIGNAL_BYTES = 64 * 64
DISPATCH_MODES = ("hidden", "weight", "pair")

def _active_dispatch_aiv_count() -> int:
    count = int(os.environ.get("TILEXR_MOONEP_DISPATCH_AIV_CORE_COUNT", "64"))
    if count < 1 or count > DISPATCH_CORE_CAPACITY:
        raise ValueError(
            "TILEXR_MOONEP_DISPATCH_AIV_CORE_COUNT must satisfy "
            f"1 <= coreCount <= {DISPATCH_CORE_CAPACITY}"
        )
    return count

ACL_EVENT_TIME_LINE = 0x00000008

class _AclEventApi:
    def __init__(self):
        self.library = ctypes.CDLL("libascendcl.so")
        self.library.aclrtCreateEventWithFlag.argtypes = [
            ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint32
        ]
        self.library.aclrtCreateEventWithFlag.restype = ctypes.c_int
        self.library.aclrtRecordEvent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.library.aclrtRecordEvent.restype = ctypes.c_int
        self.library.aclrtEventElapsedTime.argtypes = [
            ctypes.POINTER(ctypes.c_float), ctypes.c_void_p, ctypes.c_void_p
        ]
        self.library.aclrtEventElapsedTime.restype = ctypes.c_int
        self.library.aclrtDestroyEvent.argtypes = [ctypes.c_void_p]
        self.library.aclrtDestroyEvent.restype = ctypes.c_int

    @staticmethod
    def _check(operation: str, status: int) -> None:
        if status != 0:
            raise RuntimeError(f"{operation} failed with aclError={status}")

    def create(self) -> ctypes.c_void_p:
        event = ctypes.c_void_p()
        self._check("aclrtCreateEventWithFlag", self.library.aclrtCreateEventWithFlag(
            ctypes.byref(event), ACL_EVENT_TIME_LINE
        ))
        return event

    def record(self, event: ctypes.c_void_p, stream_ptr: int) -> None:
        self._check("aclrtRecordEvent", self.library.aclrtRecordEvent(
            event, ctypes.c_void_p(stream_ptr)
        ))

    def elapsed_us(self, start: ctypes.c_void_p, end: ctypes.c_void_p) -> float:
        milliseconds = ctypes.c_float()
        self._check("aclrtEventElapsedTime", self.library.aclrtEventElapsedTime(
            ctypes.byref(milliseconds), start, end
        ))
        return float(milliseconds.value) * 1000.0

    def destroy(self, event: ctypes.c_void_p) -> None:
        self._check("aclrtDestroyEvent", self.library.aclrtDestroyEvent(event))

def _create_acl_event_pairs(iterations: int):
    api = _AclEventApi()
    pairs = []
    try:
        for _ in range(iterations):
            start = api.create()
            try:
                end = api.create()
            except Exception:
                api.destroy(start)
                raise
            pairs.append((start, end))
    except Exception:
        for start, end in pairs:
            api.destroy(start)
            api.destroy(end)
        raise
    return api, pairs

def _destroy_acl_event_pairs(api: _AclEventApi, pairs) -> None:
    failure = None
    for start, end in pairs:
        for event in (start, end):
            try:
                api.destroy(event)
            except Exception as exc:
                if failure is None:
                    failure = exc
    if failure is not None:
        raise failure

class DispatchDfxRecord(ctypes.Structure):
    _fields_ = [
        ("marker", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("record_bytes", ctypes.c_uint16),
        ("payload_mode", ctypes.c_uint32),
        ("rank", ctypes.c_uint32),
        ("core", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("first_invalid_route", ctypes.c_uint32),
        ("first_invalid_dst", ctypes.c_int32),
        ("first_quiet_status", ctypes.c_uint32),
        ("first_quiet_phase", ctypes.c_uint32),
        ("timeout_peer", ctypes.c_uint32),
        ("timeout_phase", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
        ("expected_routes", ctypes.c_uint64),
        ("processed_routes", ctypes.c_uint64),
        ("magic", ctypes.c_uint64),
        ("timeout_expected_magic", ctypes.c_uint64),
        ("timeout_observed_flag", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 4),
    ]

class DispatchKernelStatus(ctypes.Structure):
    _fields_ = [
        ("marker", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("record_bytes", ctypes.c_uint16),
        ("status", ctypes.c_int32),
        ("payload_mode", ctypes.c_uint32),
        ("magic", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 5),
    ]

if (ctypes.sizeof(DispatchDfxRecord) != 128
        or ctypes.sizeof(DispatchKernelStatus) != 64):
    raise RuntimeError("Python Dispatch diagnostic ABI does not match the C++ records")

def _dtype(torch_module, name: str):
    return {"bfloat16": torch_module.bfloat16, "float16": torch_module.float16}[name]

def _case_rank_topk(case, rank: int, rank_size: int) -> tuple[int, ...]:
    route_count = case.tokens_per_rank * case.topk
    if case.routing_pattern == "skewed":
        return tuple(route % case.topk for route in range(route_count))
    return deterministic_rank_topk(
        rank,
        rank_size,
        case.tokens_per_rank,
        case.topk,
        case.expert_count,
        case.route_distribution,
    )

def _all_case_topk(case, rank_size: int) -> tuple[int, ...]:
    return tuple(
        expert
        for rank in range(rank_size)
        for expert in _case_rank_topk(case, rank, rank_size)
    )

def _inputs(torch_module, case, context):
    device = f"npu:{context.device_index}"
    route_count = case.tokens_per_rank * case.topk
    route_ids = torch_module.arange(
        route_count, dtype=torch_module.int32, device=device
    )
    topk = torch_module.tensor(
        _case_rank_topk(
            case, context.planner_group_rank, context.planner_group_size
        ),
        dtype=torch_module.int32,
        device=device,
    ).reshape(case.tokens_per_rank, case.topk)
    tpe = torch_module.bincount(
        topk.reshape(-1).to(dtype=torch_module.int64), minlength=case.expert_count
    ).to(dtype=torch_module.int32)
    rows = torch_module.arange(case.tokens_per_rank, dtype=torch_module.int32,
        device=device).reshape(-1, 1)
    cols = torch_module.arange(case.hidden_size, dtype=torch_module.int32,
        device=device).reshape(1, -1)
    hidden = ((rows * 13 + cols + context.planner_group_rank * 7) % 64).to(
        dtype=_dtype(torch_module, case.dtype)
    )
    weights = ((route_ids * 5 + context.planner_group_rank * 11) % 97).to(
        dtype=torch_module.float32
    ).reshape(case.tokens_per_rank, case.topk) / 128.0
    return topk, tpe, hidden.contiguous(), weights.contiguous()

def _reference_slot_assignments(case, context, destination_roll: int = 0):
    all_topk = _all_case_topk(case, context.planner_group_size)
    route_count = case.tokens_per_rank * case.topk
    destination_capacity = int(context.nv_s)
    source_by_slot = [-1] * destination_capacity
    token_by_slot = [-1] * destination_capacity
    route_by_slot = [-1] * destination_capacity
    for source in range(context.planner_group_size):
        plan = build_reference_plan(rank=source,
            rank_size=context.planner_group_size,
            tokens_per_rank=case.tokens_per_rank, topk=case.topk,
            expert_count=case.expert_count, all_topk=all_topk,
            prefetch_slots=context.prefetch_slots,
            token_padding=context.token_padding)
        destinations = plan.dst
        if destination_roll:
            shift = destination_roll % route_count
            destinations = destinations[-shift:] + destinations[:-shift]
        for route, encoded in enumerate(destinations):
            if encoded < 0:
                raise RuntimeError("Dispatch reference requires non-negative dst")
            raw = encoded
            target, slot = divmod(raw, destination_capacity)
            if target == context.planner_group_rank:
                if source_by_slot[slot] >= 0:
                    raise RuntimeError(f"reference destination collision at slot {slot}")
                source_by_slot[slot] = source
                token_by_slot[slot] = route // case.topk
                route_by_slot[slot] = route
    return source_by_slot, token_by_slot, route_by_slot

def _expected(torch_module, case, context, destination_roll: int = 0):
    source_by_slot, token_by_slot, route_by_slot = _reference_slot_assignments(
        case, context, destination_roll
    )
    device = f"npu:{context.device_index}"
    valid = torch_module.tensor(
        [source >= 0 for source in source_by_slot],
        dtype=torch_module.bool,
        device=device,
    )
    source = torch_module.tensor([max(0, value) for value in source_by_slot],
        dtype=torch_module.int32,
        device=device).reshape(-1, 1)
    token = torch_module.tensor([max(0, value) for value in token_by_slot],
        dtype=torch_module.int32,
        device=device).reshape(-1, 1)
    route = torch_module.tensor([max(0, value) for value in route_by_slot],
        dtype=torch_module.int32,
        device=device)
    cols = torch_module.arange(case.hidden_size, dtype=torch_module.int32,
        device=device).reshape(1, -1)
    hidden_values = ((token * 13 + cols + source * 7) % 64).to(
        dtype=_dtype(torch_module, case.dtype)
    )
    weight_values = ((route * 5 + source.reshape(-1) * 11) % 97).to(
        dtype=torch_module.float32
    ) / 128.0
    hidden = torch_module.where(
        valid.reshape(-1, 1), hidden_values, torch_module.zeros_like(hidden_values)
    )
    weights = torch_module.where(
        valid, weight_values, torch_module.zeros_like(weight_values)
    )
    return hidden, weights, source_by_slot

def _launch_dispatch(runtime, context, plan, hidden, hidden_out, stream,
    weights, weights_out, mode: str, workspace, workspace_bytes) -> None:
    if mode == "hidden":
        runtime.dispatch(context, plan, hidden, hidden_out, stream,
            registered_workspace=workspace,
            registered_workspace_bytes=workspace_bytes)
    elif mode == "weight":
        runtime.dispatch(context, plan, weights, weights_out, stream,
            registered_workspace=workspace,
            registered_workspace_bytes=workspace_bytes)
    elif mode == "pair":
        runtime.dispatch(context, plan, hidden, hidden_out, stream, weights,
            weights_out, registered_workspace=workspace,
            registered_workspace_bytes=workspace_bytes)
    else:
        raise ValueError(f"unsupported Dispatch mode: {mode}")

def _alternating_plan_check(torch_module, case, buffer, plan, hidden,
    hidden_out, weights, weights_out, mode: str) -> dict[str, object]:
    runtime = buffer.runtime
    context = buffer.context
    workspace, workspace_bytes = context.dispatch_workspace
    stream = buffer._stream_ptr()
    original_dst = plan.dst.clone()
    destination_roll = case.topk

    def launch() -> None:
        _launch_dispatch(runtime, context, plan, hidden, hidden_out, stream,
            weights, weights_out, mode, workspace, workspace_bytes)

    try:
        launch()
        buffer.quiesce()
        plan.dst.copy_(torch_module.roll(original_dst, shifts=destination_roll))
        torch_module.npu.synchronize()
        launch()
        buffer.quiesce()
        expected_hidden, expected_weights, _ = _expected(
            torch_module, case, context, destination_roll=destination_roll)
        hidden_ok = (
            bool(torch_module.equal(hidden_out, expected_hidden))
            if mode in ("hidden", "pair") else None
        )
        weight_ok = (
            bool(torch_module.equal(weights_out, expected_weights))
            if mode in ("weight", "pair") else None
        )
        if hidden_ok is False or weight_ok is False:
            raise RuntimeError(
                "alternating-plan Dispatch mismatch "
                f"hidden={hidden_ok} weight={weight_ok} roll={destination_roll}"
            )
        return {
            "enabled": True,
            "dispatch_mode": mode,
            "destination_roll": destination_roll,
            "hidden_exact": hidden_ok,
            "weight_exact": weight_ok,
        }
    finally:
        plan.dst.copy_(original_dst)
        torch_module.npu.synchronize()

def _repeated_exact_check(torch_module, case, buffer, plan, hidden,
    hidden_out, weights, weights_out, mode: str, rounds: int) -> dict[str, object]:
    if rounds <= 0:
        return {"enabled": False, "rounds": 0}
    runtime = buffer.runtime
    context = buffer.context
    workspace, workspace_bytes = context.dispatch_workspace
    stream = buffer._stream_ptr()
    original_dst = plan.dst.clone()
    destination_roll = case.topk
    try:
        for round_index in range(rounds):
            roll = destination_roll if round_index % 2 else 0
            if roll:
                plan.dst.copy_(torch_module.roll(original_dst, shifts=roll))
            else:
                plan.dst.copy_(original_dst)
            torch_module.npu.synchronize()
            _launch_dispatch(runtime, context, plan, hidden, hidden_out, stream,
                weights, weights_out, mode, workspace, workspace_bytes)
            buffer.quiesce()
            expected_hidden, expected_weights, _ = _expected(
                torch_module, case, context, destination_roll=roll)
            hidden_ok = (bool(torch_module.equal(hidden_out, expected_hidden))
                if mode in ("hidden", "pair") else None)
            weight_ok = (bool(torch_module.equal(weights_out, expected_weights))
                if mode in ("weight", "pair") else None)
            if hidden_ok is False or weight_ok is False:
                raise RuntimeError(
                    "repeated Dispatch mismatch "
                    f"round={round_index} hidden={hidden_ok} weight={weight_ok} "
                    f"roll={roll}"
                )
        return {
            "enabled": True,
            "rounds": rounds,
            "dispatch_mode": mode,
            "alternating_plan": True,
            "hidden_exact": mode in ("hidden", "pair"),
            "weight_exact": mode in ("weight", "pair"),
        }
    finally:
        plan.dst.copy_(original_dst)
        torch_module.npu.synchronize()

def _workspace_blob(context, byte_offset: int, byte_count: int) -> bytes:
    raw = context._dispatch_workspace_owner
    aligned_offset = context._dispatch_workspace_ptr - int(raw.data_ptr())
    view = raw.narrow(0, aligned_offset + byte_offset, byte_count)
    return view.cpu().numpy().tobytes()

def _record_dict(record) -> dict[str, object]:
    values = {}
    for name, _ in record._fields_:
        value = getattr(record, name)
        if isinstance(value, ctypes.Array):
            values[name] = [int(item) for item in value]
        else:
            values[name] = int(value)
    return values

def _record_matches(record, *, marker: int, record_bytes: int,
    payload_mode: int, rank: int, core: int) -> bool:
    return (
        int(record.marker) == marker
        and int(record.version) == DIAGNOSTIC_VERSION
        and int(record.record_bytes) == record_bytes
        and int(record.payload_mode) == payload_mode
        and int(record.rank) == rank
        and int(record.core) == core
        and int(record.magic) > 0
    )

def _diagnostic_layout(context) -> dict[str, int]:
    dfx_bytes = DFX_COUNT * ctypes.sizeof(DispatchDfxRecord)
    tail_bytes = COMPLETION_BYTES + SIGNAL_BYTES + 2 * dfx_bytes + 64
    common = context._dispatch_workspace_bytes - tail_bytes
    return {
        "common": common,
        "hidden_dfx": common + COMPLETION_BYTES + SIGNAL_BYTES,
        "weight_dfx": common + COMPLETION_BYTES + SIGNAL_BYTES + dfx_bytes,
        "kernel_status": common + COMPLETION_BYTES + SIGNAL_BYTES + 2 * dfx_bytes,
    }

def _kernel_status(context) -> dict[str, object] | None:
    offsets = _diagnostic_layout(context)
    blob = _workspace_blob(
        context, offsets["kernel_status"], ctypes.sizeof(DispatchKernelStatus))
    status = DispatchKernelStatus.from_buffer_copy(blob)
    if (int(status.marker) != KERNEL_STATUS_MARKER
            or int(status.version) != DIAGNOSTIC_VERSION
            or int(status.record_bytes) != ctypes.sizeof(DispatchKernelStatus)
            or int(status.magic) <= 0):
        return None
    return _record_dict(status)

def _diagnostics(context, *, require_complete: bool = True,
    required_modes: tuple[str, ...] = ("hidden", "weight")) -> dict[str, object]:
    active_count = _active_dispatch_aiv_count()
    dfx_bytes = DFX_COUNT * ctypes.sizeof(DispatchDfxRecord)
    offsets = _diagnostic_layout(context)
    kernel_status = _kernel_status(context)
    if require_complete and kernel_status is None:
        raise RuntimeError("missing or stale Dispatch kernel status")
    features = (int(kernel_status["reserved"][0])
        if kernel_status is not None else KERNEL_STATUS_FEATURE_DFX_ENABLED)
    dfx_enabled = bool(features & KERNEL_STATUS_FEATURE_DFX_ENABLED)
    fused_epoch = bool(features & KERNEL_STATUS_FEATURE_FUSED_EPOCH)
    shared_owner_mode = "weight" if fused_epoch else None
    result = {
        "kernel_status": kernel_status,
        "fused_epoch": fused_epoch,
        "shared_owner_mode": shared_owner_mode,
    }
    rank = int(context.planner_group_rank)
    for payload_mode, mode in enumerate(("hidden", "weight")):
        dfx_blob = _workspace_blob(context, offsets[f"{mode}_dfx"], dfx_bytes)
        dfx = []
        for core in range(active_count):
            record = DispatchDfxRecord.from_buffer_copy(
                dfx_blob, core * ctypes.sizeof(DispatchDfxRecord))
            if dfx_enabled and _record_matches(record, marker=DFX_MARKER,
                    record_bytes=ctypes.sizeof(DispatchDfxRecord),
                    payload_mode=payload_mode, rank=rank, core=core):
                dfx.append(_record_dict(record))
        if (require_complete and mode in required_modes and dfx_enabled
                and len(dfx) != active_count):
            raise RuntimeError(
                f"missing or stale Dispatch DFX records mode={mode}: "
                f"{len(dfx)}/{active_count}"
            )
        result[mode] = {
            "dfx_available": dfx_enabled,
            "shared_epoch_owner": fused_epoch and mode == shared_owner_mode,
            "dfx": dfx,
        }
    return result

def _tensor_mismatch_debug(actual, expected) -> dict[str, object]:
    mismatch = actual != expected
    coordinates = mismatch.nonzero()[:8].cpu().tolist()
    first = []
    for coordinate in coordinates:
        index = tuple(int(value) for value in coordinate)
        first.append({
            "index": list(index),
            "actual": float(actual[index].item()),
            "expected": float(expected[index].item()),
        })
    return {"count": int(mismatch.sum().item()), "first": first}

def _source_attribution_debug(torch_module, actual_hidden, expected_hidden,
    actual_weight, expected_weight, source_by_slot: list[int],
    rank_size: int) -> list[dict[str, object]]:
    hidden_exact = torch_module.all(actual_hidden == expected_hidden, dim=1).cpu().tolist()
    hidden_zero = torch_module.all(actual_hidden == 0, dim=1).cpu().tolist()
    weight_exact = (actual_weight == expected_weight).cpu().tolist()
    weight_zero = (actual_weight == 0).cpu().tolist()
    result = []
    for source in range(rank_size):
        slots = [slot for slot, owner in enumerate(source_by_slot) if owner == source]
        hidden_correct = [slot for slot in slots if hidden_exact[slot]]
        weight_correct = [slot for slot in slots if weight_exact[slot]]
        both_correct = [slot for slot in slots
            if hidden_exact[slot] and weight_exact[slot]]
        result.append({
            "source_rank": source,
            "slots": len(slots),
            "hidden_exact_slots": len(hidden_correct),
            "hidden_zero_slots": sum(1 for slot in slots if hidden_zero[slot]),
            "weight_exact_slots": len(weight_correct),
            "weight_zero_slots": sum(1 for slot in slots if weight_zero[slot]),
            "both_exact_slots": len(both_correct),
            "first_hidden_exact_slots": hidden_correct[:8],
            "first_hidden_mismatch_slots": [
                slot for slot in slots if not hidden_exact[slot]
            ][:8],
            "first_weight_exact_slots": weight_correct[:8],
            "first_weight_mismatch_slots": [
                slot for slot in slots if not weight_exact[slot]
            ][:8],
        })
    return result

def _measure(torch_module, case, buffer, plan, hidden, hidden_out, weights,
    weights_out, mode: str) -> list[dict[str, float]]:
    runtime = buffer.runtime
    context = buffer.context
    workspace, workspace_bytes = context.dispatch_workspace
    stream = buffer._stream_ptr()

    def launch():
        _launch_dispatch(runtime, context, plan, hidden, hidden_out, stream,
            weights, weights_out, mode, workspace, workspace_bytes)

    for _ in range(case.warmup):
        launch()
    buffer.synchronize()
    context.bind_stream(buffer, stream)
    event_api, event_pairs = _create_acl_event_pairs(case.iterations)
    host_us = []
    try:
        for iteration in range(case.iterations):
            start, end = event_pairs[iteration]
            event_api.record(start, stream)
            before = time.perf_counter_ns()
            launch()
            host_us.append((time.perf_counter_ns() - before) / 1000.0)
            event_api.record(end, stream)
        buffer.quiesce()
        status = int(plan.status.item())
        if status != 0:
            if status == 2005:
                context.mark_poisoned()
            raise RuntimeError(f"Dispatch device status is {status}")
        samples = [{"host_us": host_us[index],
            "kernel_us": event_api.elapsed_us(*event_pairs[index])}
            for index in range(case.iterations)]
    finally:
        _destroy_acl_event_pairs(event_api, event_pairs)
    context.release_stream(buffer)
    buffer._bound_stream_ptr = None
    return samples

def run_case(torch_module, case, args) -> None:
    from tilexr_moonep import TileXRMoonEPBuffer, TileXRMoonEPContext

    rank = int(os.environ.get("RANK", "0"))
    output_root = Path(args.output_dir).resolve()
    rank_dir = output_root / case.case_id / f"rank_{rank}"
    dispatch_modes = tuple(args.dispatch_modes)
    if not dispatch_modes or len(set(dispatch_modes)) != len(dispatch_modes):
        raise ValueError("dispatch_modes must be non-empty and contain no duplicates")
    diagnostic_modes = tuple(mode for mode in ("hidden", "weight") if
        mode in dispatch_modes or "pair" in dispatch_modes)
    context = TileXRMoonEPContext.from_env(tokens_per_rank=case.tokens_per_rank,
        hidden_size=case.hidden_size, topk=case.topk,
        expert_count=case.expert_count, dtype=_dtype(torch_module, case.dtype),
        install_prefix=args.install_prefix, torch_module=torch_module)
    buffer = TileXRMoonEPBuffer(context, wait_iterations=args.wait_iterations,
        torch_module=torch_module)
    result = {"schema_version": 1, "benchmark_kind": "dispatch_hot_loop",
        "dispatch_modes": list(dispatch_modes),
        "status": "failed", "failure_reason": None, "mode": "benchmark",
        "launch_id": os.environ.get("TILEXR_MOONEP_LAUNCH_ID", ""), "rank": rank,
        "case": case.as_dict(), "capabilities": context.runtime.capabilities.as_dict(),
        "topology": topology_metadata(context),
        "environment": environment_metadata(torch_module, ROOT),
        "validation": {"passed": False, "mode": "dispatch_bit_exact"}}
    failure = None
    quiesced = False
    try:
        topk, tpe, hidden, weights = _inputs(torch_module, case, context)
        plan, cu_seqlens = buffer.planning(topk, tpe)
        buffer.synchronize()
        validation = validate_plan(
            plan, context, cu_seqlens,
            route_distribution=case.route_distribution,
            all_topk=_all_case_topk(case, context.planner_group_size))
        route_count = context.dispatched_capacity
        hidden_out = torch_module.empty((route_count, case.hidden_size),
            dtype=hidden.dtype, device=hidden.device)
        weights_out = torch_module.empty((route_count,), dtype=torch_module.float32,
            device=hidden.device)
        alternating_plan = {"enabled": False}
        if os.environ.get("TILEXR_MOONEP_ALTERNATING_PLAN_CHECK") == "1":
            alternating_mode = "pair" if "pair" in dispatch_modes else dispatch_modes[0]
            alternating_plan = _alternating_plan_check(torch_module, case, buffer,
                plan, hidden, hidden_out, weights, weights_out, alternating_mode)
        exact_mode = "pair" if "pair" in dispatch_modes else dispatch_modes[0]
        repeated_exact = _repeated_exact_check(torch_module, case, buffer, plan,
            hidden, hidden_out, weights, weights_out, exact_mode,
            args.exact_rounds)
        modes = {}
        for mode in dispatch_modes:
            modes[mode] = _measure(torch_module, case, buffer, plan, hidden,
                hidden_out, weights, weights_out, mode)
        expected_hidden, expected_weights, source_by_slot = _expected(
            torch_module, case, context)
        validate_hidden = "hidden" in dispatch_modes or "pair" in dispatch_modes
        validate_weight = "weight" in dispatch_modes or "pair" in dispatch_modes
        hidden_ok = (bool(torch_module.equal(hidden_out, expected_hidden))
            if validate_hidden else None)
        weight_ok = (bool(torch_module.equal(weights_out, expected_weights))
            if validate_weight else None)
        if hidden_ok is False or weight_ok is False:
            result["diagnostics"] = _diagnostics(context,
                required_modes=diagnostic_modes)
            failure_debug = {
                "workspace_ptr": int(context._dispatch_workspace_ptr),
            }
            if validate_hidden:
                failure_debug["hidden"] = _tensor_mismatch_debug(
                    hidden_out, expected_hidden)
            if validate_weight:
                failure_debug["weight"] = _tensor_mismatch_debug(
                    weights_out, expected_weights)
            if validate_hidden and validate_weight:
                failure_debug["source_attribution"] = _source_attribution_debug(
                    torch_module, hidden_out, expected_hidden, weights_out,
                    expected_weights, source_by_slot,
                    context.planner_group_size)
            result["failure_debug"] = failure_debug
            raise RuntimeError(f"Dispatch bit-exact mismatch hidden={hidden_ok} weight={weight_ok}")
        samples = []
        for iteration in range(case.iterations):
            timings = {}
            for mode in dispatch_modes:
                timings[f"{mode}_host"] = modes[mode][iteration]["host_us"]
                timings[f"{mode}_kernel"] = modes[mode][iteration]["kernel_us"]
            samples.append({"iteration": iteration, "timings_us": timings})
        write_jsonl(rank_dir / "samples.jsonl", samples)
        result["diagnostics"] = _diagnostics(context,
            required_modes=diagnostic_modes)
        if validate_hidden:
            validation["dispatch_hidden_exact"] = hidden_ok
        if validate_weight:
            validation["dispatch_weight_exact"] = weight_ok
        validation["alternating_plan"] = alternating_plan
        validation["repeated_exact"] = repeated_exact
        validation["passed"] = True
        validation["mode"] = "planner_and_dispatch_bit_exact"
        result["validation"] = validation
        result["status"] = "passed"
    except Exception as exc:
        result["failure_reason"] = f"{type(exc).__name__}: {exc}"
        failure = exc
        try:
            result["diagnostics"] = _diagnostics(context, require_complete=False,
                required_modes=diagnostic_modes)
        except Exception as diagnostic_exc:
            result["diagnostic_failure"] = (
                f"{type(diagnostic_exc).__name__}: {diagnostic_exc}"
            )
    finally:
        try:
            buffer.quiesce()
            quiesced = True
            buffer.check_pending_status()
        except Exception as exc:
            if failure is None:
                failure = exc
                result["failure_reason"] = f"{type(exc).__name__}: {exc}"
        if failure is not None:
            print(f"rank {rank} pre-teardown failure: {result['failure_reason']}",
                file=sys.stderr, flush=True)
        write_json(rank_dir / "result.pre_teardown.json", result)
        decision = completion_barrier_from_env(rank,
            int(os.environ.get("WORLD_SIZE", "1")), case_id=case.case_id,
            quiesced=quiesced, passed=failure is None)
        if not decision.release:
            raise RuntimeError("distributed teardown was not released; "
                f"local failure: {result['failure_reason']}")
        if decision.abort and failure is None:
            failure = RuntimeError("another rank failed Dispatch hot-loop validation")
            result["status"] = "failed"
            result["failure_reason"] = str(failure)
        try:
            buffer.close()
        except Exception as exc:
            if failure is None:
                failure = exc
                result["failure_reason"] = f"{type(exc).__name__}: {exc}"
        write_json(rank_dir / "result.json", result)
    if failure is not None:
        raise failure

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="TileXR native Dispatch hot-loop worker")
    build_case_parser(parser)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--install-prefix", default=None)
    parser.add_argument("--wait-iterations", type=int, default=1_000_000)
    parser.add_argument("--exact-rounds", type=int, default=0)
    parser.add_argument("--dispatch-modes", nargs="+", choices=DISPATCH_MODES,
        default=DISPATCH_MODES)
    args = parser.parse_args(argv)
    if args.exact_rounds < 0:
        parser.error("--exact-rounds must be non-negative")
    if os.environ.get("TILEXR_UDMA_QP_ROUTE_SPEC") != "port_count:6,port_count:2":
        raise RuntimeError(
            "TILEXR_UDMA_QP_ROUTE_SPEC=port_count:6,port_count:2 is required before init")
    import torch
    torch.npu.set_device(int(os.environ.get("LOCAL_RANK", "0")))
    for case in select_cases(load_cases(args.cases), args.case_ids):
        run_case(torch, apply_overrides(case, args), args)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
