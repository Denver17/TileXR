from __future__ import annotations

import base64
from collections import Counter
import hashlib
import struct
from types import SimpleNamespace
import zlib

import json

import torch

from tools.moonep.mindspeed.build_route_replay import build_parser, build_route_replay
from tools.moonep.model_flow import (
    MODEL_FLOW_CASE_SPECS,
    MODEL_LOGICAL_STAGE_COUNTS,
    execute_model_iteration,
    load_route_replay,
    model_flow_case_spec,
    model_route_replay_path,
    model_operator_order,
    parse_tilexr_kernel_rows,
    routes_from_expert_counts,
)


def test_model_flow_specs_cover_single_and_two_node_scales() -> None:
    assert {
        case_id: (spec.world_size, spec.replay_filename)
        for case_id, spec in MODEL_FLOW_CASE_SPECS.items()
    } == {
        "model-flow-8rank-4k-ep8-mindspeed": (
            8,
            "mindspeed_4k_ep8_route_counts.json",
        ),
        "model-flow-16rank-4k-ep16-mindspeed": (
            16,
            "mindspeed_4k_ep16_route_counts.json",
        ),
        "model-flow-8rank-8k-k16-ep8-mindspeed": (
            8,
            "mindspeed_8k_k16_ep8_route_counts.json",
        ),
        "model-flow-16rank-8k-k16-ep16-mindspeed": (
            16,
            "mindspeed_8k_k16_ep16_route_counts.json",
        ),
    }


def test_every_model_flow_spec_uses_the_cache_route_override(
    tmp_path, monkeypatch
) -> None:
    replay = tmp_path / "route_replay.json"
    monkeypatch.setenv("TILEXR_MOONEP_MODEL_ROUTE_REPLAY", str(replay))
    for spec in MODEL_FLOW_CASE_SPECS.values():
        assert model_route_replay_path(spec) == replay.resolve()


def test_dynamic_model_replay_case_uses_runtime_world_size() -> None:
    spec = model_flow_case_spec(
        "model-replay-s8192-k16-h3584-ep16-r16", dynamic_world_size=16
    )

    assert spec.world_size == 16
    assert spec.replay_filename == ""


def test_model_operator_order_matches_one_mindspeed_iteration() -> None:
    order = model_operator_order()

    assert len(order) == 55
    assert Counter(item["stage"] for item in order) == MODEL_LOGICAL_STAGE_COUNTS
    assert [(item["phase"], item["layer"], item["stage"]) for item in order[:4]] == [
        ("initial_forward", 0, "planning"),
        ("initial_forward", 0, "dispatch_forward"),
        ("initial_forward", 0, "prefetch_weight"),
        ("initial_forward", 0, "combine_forward"),
    ]
    assert [(item["phase"], item["layer"], item["stage"]) for item in order[20:27]] == [
        ("recompute_backward", 4, "planning"),
        ("recompute_backward", 4, "dispatch_forward"),
        ("recompute_backward", 4, "prefetch_weight"),
        ("recompute_backward", 4, "combine_forward"),
        ("recompute_backward", 4, "dispatch_backward"),
        ("recompute_backward", 4, "combine_backward"),
        ("recompute_backward", 4, "reduce_grad"),
    ]
    assert (order[-1]["phase"], order[-1]["layer"], order[-1]["stage"]) == (
        "recompute_backward",
        0,
        "reduce_grad",
    )


def test_route_replay_preserves_histogram_and_unique_topk_per_token() -> None:
    counts = [2, 2, 2, 2]

    routes = routes_from_expert_counts(
        torch,
        counts,
        tokens_per_rank=2,
        topk=4,
        device="cpu",
    )

    assert tuple(routes.shape) == (2, 4)
    assert routes.dtype == torch.int32
    assert torch.equal(
        torch.bincount(routes.flatten().to(torch.int64), minlength=4),
        torch.tensor(counts),
    )
    assert all(len(set(row.tolist())) == 4 for row in routes)


def test_route_replay_builder_accepts_model_flow_dimensions() -> None:
    args = build_parser().parse_args(
        [
            "--capture-dir",
            "capture",
            "--output",
            "replay.json",
            "--capture-id",
            "model",
            "--source-git-sha",
            "abc123",
            "--world-size",
            "16",
            "--tokens-per-rank",
            "8192",
            "--topk",
            "16",
            "--expert-count",
            "32",
            "--hidden-size",
            "3584",
            "--ep-size",
            "16",
        ]
    )

    assert (
        args.world_size,
        args.tokens_per_rank,
        args.topk,
        args.expert_count,
        args.hidden_size,
        args.ep_size,
    ) == (
        16,
        8192,
        16,
        32,
        3584,
        16,
    )


def test_route_replay_loader_requires_all_ten_rank_local_histograms(tmp_path) -> None:
    payload = {
        "schema_version": 1,
        "dimensions": {
            "world_size": 2,
            "tokens_per_rank": 2,
            "topk": 2,
            "expert_count": 4,
            "forward_calls": 10,
        },
        "provenance": {"run_id": "captured-model"},
        "ranks": {
            str(rank): [
                {"call": call, "tokens_per_expert": [1, 1, 1, 1]}
                for call in range(10)
            ]
            for rank in range(2)
        },
    }
    path = tmp_path / "routes.json"
    path.write_text(json.dumps(payload), encoding="utf-8")

    counts, provenance = load_route_replay(
        path,
        rank=1,
        world_size=2,
        tokens_per_rank=2,
        topk=2,
        expert_count=4,
    )

    assert counts == [[1, 1, 1, 1]] * 10
    assert provenance == {"run_id": "captured-model"}


def test_route_replay_loader_returns_captured_plan_contract(tmp_path) -> None:
    topk_raw = struct.pack("<4i", 0, 1, 2, 3)
    encoded = base64.b64encode(zlib.compress(topk_raw)).decode("ascii")
    digest = hashlib.sha256(topk_raw).hexdigest()
    records = []
    for call in range(10):
        records.append(
            {
                "call": call,
                "tokens_per_expert": [1, 1, 1, 1],
                "topk_encoding": "int32-le-zlib-base64",
                "topk_zlib_base64": encoded,
                "topk_sha256": digest,
                "remote_stats": [1, 2],
                "experts_to_copy": [[2, -1], [0, -1]],
            }
        )
    payload = {
        "schema_version": 1,
        "dimensions": {
            "world_size": 2,
            "tokens_per_rank": 2,
            "topk": 2,
            "expert_count": 4,
            "forward_calls": 10,
        },
        "provenance": {"run_id": "captured-model"},
        "ranks": {"0": records, "1": records},
    }
    path = tmp_path / "routes.json"
    path.write_text(json.dumps(payload), encoding="utf-8")

    _, _, plans, _ = load_route_replay(
        path,
        rank=0,
        world_size=2,
        tokens_per_rank=2,
        topk=2,
        expert_count=4,
        include_topk=True,
        include_plan=True,
    )

    assert plans == [
        {
            "remote_stats": [1, 2],
            "experts_to_copy": [[2, -1], [0, -1]],
        }
    ] * 10


def test_capture_builder_emits_validated_replay_and_provenance(tmp_path) -> None:
    for rank in range(2):
        for call in range(2):
            topk_raw = struct.pack("<4i", 0, 1, 2, 3)
            record = {
                "schema_version": 1,
                "complete": True,
                "rank": rank,
                "call": call,
                "topk_shape": [2, 2],
                "topk_dtype": "torch.int32",
                "topk_encoding": "int32-le-zlib-base64",
                "topk_zlib_base64": base64.b64encode(
                    zlib.compress(topk_raw)
                ).decode("ascii"),
                "topk_sha256": hashlib.sha256(topk_raw).hexdigest(),
                "tokens_per_expert": [1, 1, 1, 1],
                "remote_stats": [rank, call],
                "experts_to_copy": [[0, 1], [2, 3]],
                "capture_id": "capture",
                "payload_seed": rank * 100 + call,
                "tensor_descriptors": {
                    "hidden": {
                        "shape": [2, 128],
                        "dtype": "torch.bfloat16",
                    }
                },
            }
            (tmp_path / f"rank{rank}_call{call:02d}.json").write_text(
                json.dumps(record), encoding="utf-8"
            )

    replay = build_route_replay(
        tmp_path,
        capture_id="capture",
        source_git_sha="abc123",
        world_size=2,
        forward_calls=2,
        tokens_per_rank=2,
        topk=2,
        expert_count=4,
        hidden_size=128,
        ep_size=2,
    )

    assert replay["dimensions"]["forward_calls"] == 2
    assert replay["ranks"]["1"][1]["source_call"] == 1
    assert replay["ranks"]["1"][1]["remote_stats"] == [1, 1]
    assert replay["provenance"]["source_git_sha"] == "abc123"
    expected_hash = hashlib.sha256(struct.pack("<4i", 0, 1, 2, 3)).hexdigest()
    assert replay["provenance"]["topk_sha256_by_rank"]["0"] == [
        expected_hash,
        expected_hash,
    ]
    assert replay["ranks"]["0"][0]["topk_encoding"] == "int32-le-zlib-base64"


def test_model_iteration_calls_five_initial_and_five_recompute_backward_flows() -> None:
    events = []

    def expert_compute(
        _plan, hidden, _cu_seqlens, _projections, _route_weights
    ):
        events.append(("expert_compute", None))
        return hidden

    class Buffer:
        context = SimpleNamespace(route_count=8)

        def __init__(self):
            self.next_plan = 0
            self.reduce_inflight = False

        def dispatch(self, hidden, route_weights=None, topk=None, tpe=None, plan=None):
            del topk, tpe
            if plan is None:
                plan = SimpleNamespace(
                    index=self.next_plan,
                    remote_stats=torch.tensor([1, 1], dtype=torch.int32),
                )
                self.next_plan += 1
                events.append(("dispatch_forward", plan.index))
            else:
                plan.backward_dispatched = True
                events.append(("dispatch_backward", plan.index))
            return hidden, route_weights, None, plan

        def prefetch_weight(self, plan, _projections):
            events.append(("prefetch_weight", plan.index))

        def combine(self, plan, hidden, route_weights=None):
            events.append(
                (
                    "combine_backward"
                    if getattr(plan, "backward_dispatched", False)
                    else "combine_forward",
                    plan.index,
                )
            )
            if getattr(plan, "backward_dispatched", False):
                assert route_weights is not None
            else:
                assert route_weights is None
            return hidden, route_weights, plan

        def reduce_grad(self, plan, **_kwargs):
            if self.reduce_inflight:
                raise RuntimeError("ReduceGrad is already in flight")
            self.reduce_inflight = True
            events.append(("reduce_grad", plan.index))

        def synchronize(self):
            self.reduce_inflight = False
            events.append(("synchronize", None))

    projection = SimpleNamespace(
        gate=torch.zeros((4, 2)),
        up=torch.zeros((4, 2)),
        down=torch.zeros((4, 2)),
    )
    gradients = SimpleNamespace(
        gate=torch.zeros((4, 2)),
        up=torch.zeros((4, 2)),
        down=torch.zeros((4, 2)),
        gate_reduce=torch.zeros((2, 2)),
        up_reduce=torch.zeros((2, 2)),
        down_reduce=torch.zeros((2, 2)),
    )
    inputs = {
        "hidden": torch.ones((2, 4), dtype=torch.bfloat16),
        "route_weights": torch.ones((2, 4), dtype=torch.float32),
        "topk_experts": [torch.zeros((2, 4), dtype=torch.int32)] * 10,
        "tokens_per_expert": [torch.tensor([2, 2, 2, 2], dtype=torch.int32)] * 10,
        "grad_output": torch.ones((2, 4), dtype=torch.bfloat16),
        "projections": projection,
        "gradients": gradients,
    }

    sample = execute_model_iteration(
        Buffer(),
        inputs,
        torch_module=torch,
        expert_compute=expert_compute,
        stage_barrier=lambda stage: events.append(("barrier", stage)),
    )

    assert Counter(name for name, _ in events) == {
        "dispatch_forward": 10,
        "prefetch_weight": 10,
        "combine_forward": 10,
        "dispatch_backward": 5,
        "combine_backward": 5,
        "reduce_grad": 5,
        "expert_compute": 15,
        "synchronize": 6,
        "barrier": 35,
    }
    assert events[0] == ("barrier", "initial_forward_0_dispatch_forward")
    assert events[1] == ("dispatch_forward", 0)
    reduce_positions = [
        index for index, event in enumerate(events) if event[0] == "reduce_grad"
    ]
    assert all(events[index + 1] == ("synchronize", None) for index in reduce_positions)
    assert len(sample["operator_bytes"]) == 55
    order = model_operator_order()
    combine_forward = next(
        index for index, item in enumerate(order) if item["stage"] == "combine_forward"
    )
    combine_backward = next(
        index for index, item in enumerate(order) if item["stage"] == "combine_backward"
    )
    assert sample["operator_bytes"][combine_forward] == 64
    assert sample["operator_bytes"][combine_backward] == 96
    assert sample["checksum"] == 8.0


def _iteration_rows(offset: float, *, combine_v2_count: int = 20) -> list[dict[str, str]]:
    rows = []

    def add(name: str, count: int, base: float) -> None:
        for index in range(count):
            rows.append({"Name": name, "Duration(us)": str(offset + base + index)})

    add("tilexr_ep_plan_kernel", 10, 100.0)
    add("tilexr_moonep_dispatch_urma_kernel", 15, 200.0)
    add("tilexr_moonep_prefetch_weight_kernel", 10, 300.0)
    add("tilexr_moonep_combine_v2_kernel", combine_v2_count, 400.0)
    add("tilexr_moonep_reduce_grad_kernel", 5, 500.0)
    return rows


def test_profiler_rows_group_backward_combine_pairs_and_forward_single_launches() -> None:
    profile = parse_tilexr_kernel_rows(
        _iteration_rows(0.0) + _iteration_rows(1000.0), iterations=2
    )

    assert len(profile) == 55
    assert profile[0]["values_us"] == [100.0, 1100.0]
    first_backward = next(
        item
        for item in profile
        if item["layer"] == 4 and item["stage"] == "dispatch_backward"
    )
    assert first_backward["values_us"] == [206.0, 1206.0]
    first_backward_combine = next(
        item
        for item in profile
        if item["layer"] == 4 and item["stage"] == "combine_backward"
    )
    assert first_backward_combine["values_us"] == [813.0, 2813.0]
    assert first_backward_combine["kernel_launches_per_call"] == 2
    first_forward_combine = next(
        item
        for item in profile
        if item["phase"] == "initial_forward"
        and item["layer"] == 0
        and item["stage"] == "combine_forward"
    )
    assert first_forward_combine["values_us"] == [400.0, 1400.0]
    assert first_forward_combine["kernel_launches_per_call"] == 1


def test_profiler_rows_accept_single_launch_combine_v2_backward() -> None:
    profile = parse_tilexr_kernel_rows(
        _iteration_rows(0.0, combine_v2_count=15), iterations=1
    )

    assert len(profile) == 55
    first_backward_combine = next(
        item
        for item in profile
        if item["layer"] == 4 and item["stage"] == "combine_backward"
    )
    assert first_backward_combine["values_us"] == [406.0]
    assert first_backward_combine["kernel_launches_per_call"] == 1
    first_recompute_forward_combine = next(
        item
        for item in profile
        if item["phase"] == "recompute_backward"
        and item["layer"] == 4
        and item["stage"] == "combine_forward"
    )
    assert first_recompute_forward_combine["values_us"] == [405.0]
    assert first_recompute_forward_combine["kernel_launches_per_call"] == 1
