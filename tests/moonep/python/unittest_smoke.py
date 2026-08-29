from __future__ import annotations

import json
import os
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
INTEGRATION = ROOT / "integrations" / "moonep_torch"
for path in (ROOT, INTEGRATION, Path(__file__).resolve().parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from fakes import FakeRuntime, FakeStream, FakeTensor, FakeTorch
from tilexr_moonep import ProjectionBuffers, TileXRMoonEPBuffer, TileXRMoonEPContext
from tools.moonep.config import BenchmarkCase, load_cases
from tools.moonep.benchmark import (
    _multi_node_stage_barrier,
    _oversubscribed_planning_barrier,
    topology_metadata,
    validate_plan,
)
from tools.moonep.launcher import rank_to_device, resolve_topology
from tools.moonep.planner_reference import build_reference_plan, deterministic_all_topk
from tools.moonep.rendezvous import completion_barrier, offset_host_port
from tools.moonep.report import aggregate_rank_artifacts, write_json, write_jsonl


def tensor(shape, dtype, **kwargs):
    return FakeTensor(shape, dtype, "npu:0", **kwargs)


def make_buffer(*, write_status_markers=False, runtime=None, node_count=1):
    torch = FakeTorch()
    runtime = runtime or FakeRuntime(write_status_markers=write_status_markers)
    context = TileXRMoonEPContext(
        runtime=runtime,
        global_rank=0,
        global_world_size=2,
        node_rank=0,
        node_count=node_count,
        local_rank=0,
        local_world_size=2,
        planner_group_rank=0,
        planner_group_size=2,
        lane_group_rank=0,
        lane_group_size=1,
        device_index=0,
        tokens_per_rank=4,
        hidden_size=8,
        topk=2,
        expert_count=4,
        dtype=torch.bfloat16,
    )
    return torch, runtime, TileXRMoonEPBuffer(context, torch_module=torch)


class MoonEPSmokeTests(unittest.TestCase):
    def test_native_hidden_dtype_supports_float16_and_bfloat16(self):
        torch = FakeTorch()
        context = TileXRMoonEPContext(
            runtime=FakeRuntime(),
            global_rank=0,
            global_world_size=2,
            node_rank=0,
            node_count=1,
            local_rank=0,
            local_world_size=2,
            planner_group_rank=0,
            planner_group_size=2,
            lane_group_rank=0,
            lane_group_size=1,
            device_index=0,
            tokens_per_rank=4,
            hidden_size=8,
            topk=2,
            expert_count=4,
            dtype=torch.float16,
        )
        buffer = TileXRMoonEPBuffer(context, torch_module=torch)
        buffer.close()

    def test_oversubscribed_planning_barrier_is_outside_device_work(self):
        buffer = SimpleNamespace(synchronize_calls=0)
        buffer.synchronize = lambda: setattr(
            buffer, "synchronize_calls", buffer.synchronize_calls + 1
        )
        decision = SimpleNamespace(release=True, abort=False)
        environment = {
            "TILEXR_OVERSUBSCRIBED": "1",
            "RANK": "1",
            "WORLD_SIZE": "2",
        }
        with patch.dict(os.environ, environment, clear=False), patch(
            "tools.moonep.benchmark.completion_barrier_from_env",
            return_value=decision,
        ) as barrier:
            _oversubscribed_planning_barrier(buffer, "smoke", 3)
        self.assertEqual(buffer.synchronize_calls, 1)
        barrier.assert_called_once_with(
            1,
            2,
            case_id="smoke.planning.3",
            quiesced=True,
            passed=True,
        )

        with patch.dict(os.environ, {"TILEXR_OVERSUBSCRIBED": "0"}, clear=False):
            _oversubscribed_planning_barrier(buffer, "smoke", 4)
        self.assertEqual(buffer.synchronize_calls, 1)

    def test_multi_node_stage_barrier_aligns_ranks_after_device_sync(self):
        buffer = SimpleNamespace(synchronize_calls=0)
        buffer.synchronize = lambda: setattr(
            buffer, "synchronize_calls", buffer.synchronize_calls + 1
        )
        decision = SimpleNamespace(release=True, abort=False)
        environment = {"NODE_COUNT": "8", "RANK": "44", "WORLD_SIZE": "64"}
        with patch.dict(os.environ, environment, clear=False), patch(
            "tools.moonep.benchmark.completion_barrier_from_env",
            return_value=decision,
        ) as barrier:
            _multi_node_stage_barrier(buffer, "case-12", 2, "expert_forward")
        self.assertEqual(buffer.synchronize_calls, 1)
        barrier.assert_called_once_with(
            44,
            64,
            case_id="case-12.expert_forward.2",
            quiesced=True,
            passed=True,
        )

        with patch.dict(os.environ, {"NODE_COUNT": "1"}, clear=False):
            _multi_node_stage_barrier(buffer, "case-12", 3, "expert_forward")
        self.assertEqual(buffer.synchronize_calls, 1)

    def test_forward_backward_order_and_plan_reuse(self):
        torch, runtime, buffer = make_buffer(write_status_markers=True)
        projection = ProjectionBuffers.from_local_weights(
            buffer.context,
            tensor((2, 4, 8), torch.bfloat16),
            tensor((2, 4, 8), torch.bfloat16),
            tensor((2, 8, 4), torch.bfloat16),
            torch_module=torch,
        )
        buffer.register_projection_buffers(projection)

        def expert_forward(dispatched, plan, projections):
            runtime.calls.append(("expert_forward", plan))
            return dispatched, "cache"

        hidden = tensor((4, 8), torch.bfloat16)
        forward = buffer.forward(
            hidden=hidden,
            route_weights=tensor((4, 2), torch.float32),
            topk_experts=tensor((4, 2), torch.int32),
            tokens_per_expert=tensor((4,), torch.int32),
            projections=projection,
            expert_forward=expert_forward,
            apply_route_weights=False,
        )
        gradients = ProjectionBuffers(
            tensor((6, 2, 4), torch.float32),
            tensor((6, 4, 3), torch.float32),
            tensor((6, 3, 2), torch.float32),
            tensor((2, 2, 2, 4), torch.float32),
            tensor((2, 2, 4, 3), torch.float32),
            tensor((2, 2, 3, 2), torch.float32),
        )

        def expert_backward(dispatched, state):
            runtime.calls.append(("expert_backward", state.plan))
            self.assertEqual(state.expert_cache, "cache")
            return dispatched

        backward = buffer.backward(
            grad_output=hidden,
            state=forward.state,
            expert_backward=expert_backward,
            gradients=gradients,
        )
        buffer.close()
        names = [
            item[0] for item in runtime.calls
            if item[0] not in ("planning_workspace_size", "planning_dst_local_offset")
        ]
        self.assertEqual(
            names,
            [
                "udma_register",
                "planning",
                "dispatch",
                "udma_register",
                "prefetch_weight",
                "expert_forward",
                "combine",
                "dispatch",
                "expert_backward",
                "combine",
                "prepare_reduce_grad",
                "reduce_grad",
                "destroy_reduce_grad",
                "udma_unregister",
                "close",
            ],
        )
        self.assertIs(backward.plan, forward.state.plan)
        self.assertEqual(forward.state.plan.dispatched_capacity, 8)
        planning_call = next(call for call in runtime.calls if call[0] == "planning")
        self.assertEqual(planning_call[-1], 1_000_000)
        with self.assertRaises((AttributeError, TypeError)):
            forward.state.plan.epoch = 99

    def test_reduce_grad_profile_is_prepared_once_and_released(self):
        torch, runtime, buffer = make_buffer()
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        threshold_elements = (1 << 20) // 4
        gradients = ProjectionBuffers(
            tensor((6, threshold_elements), torch.float32),
            tensor((6, threshold_elements + 1), torch.float32),
            tensor((6, 8), torch.float32),
        )

        info = buffer.prepare_reduce_grad(plan, gradients)
        self.assertEqual(info.qp_count, 3)
        self.assertEqual(info.projection_qp_counts, (1, 1, 1))
        self.assertIsNotNone(runtime.registered_workspace)
        self.assertEqual(runtime.registered_workspace.numel(), info.workspace_bytes)
        self.assertEqual(
            runtime.registered_workspace.data_ptr() % info.workspace_alignment, 0
        )
        self.assertEqual(
            [call[0] for call in runtime.calls].count(
                "prepare_reduce_grad"
            ),
            1,
        )

        buffer.reduce_grad(
            plan,
            full_gate_grad=gradients.gate,
            full_up_grad=gradients.up,
            full_down_grad=gradients.down,
        )
        self.assertEqual(runtime.reduce_grad_query_calls, 1)
        self.assertEqual(
            [call[0] for call in runtime.calls].count(
                "prepare_reduce_grad"
            ),
            1,
        )
        buffer.close()
        self.assertIsNone(runtime.registered_workspace)
        names = [call[0] for call in runtime.calls]
        self.assertLess(
            names.index("destroy_reduce_grad"), names.index("close")
        )

    def test_reduce_grad_rejects_second_inflight_call_and_reuses_after_sync(self):
        torch, runtime, buffer = make_buffer()
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        gradients = ProjectionBuffers(
            tensor((6, 8), torch.float32),
            tensor((6, 8), torch.float32),
            tensor((6, 8), torch.float32),
        )

        def launch():
            buffer.reduce_grad(
                plan,
                full_gate_grad=gradients.gate,
                full_up_grad=gradients.up,
                full_down_grad=gradients.down,
            )

        launch()
        with self.assertRaisesRegex(RuntimeError, "already in flight"):
            launch()
        self.assertEqual([call[0] for call in runtime.calls].count("reduce_grad"), 1)
        buffer.synchronize()
        launch()
        self.assertEqual([call[0] for call in runtime.calls].count("reduce_grad"), 2)
        buffer.close()

    def test_reduce_grad_device_failure_releases_runtime_owner(self):
        torch, runtime, buffer = make_buffer()
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        gradients = ProjectionBuffers(
            tensor((6, 8), torch.float32),
            tensor((6, 8), torch.float32),
            tensor((6, 8), torch.float32),
        )
        buffer.reduce_grad(
            plan,
            full_gate_grad=gradients.gate,
            full_up_grad=gradients.up,
            full_down_grad=gradients.down,
        )
        plan.reduce_grad_status._item = 1002
        with self.assertRaisesRegex(RuntimeError, "1002"):
            buffer.synchronize()
        self.assertIsNone(runtime._reduce_grad_owner_token)

        plan.reduce_grad_status._item = 0
        buffer.reduce_grad(
            plan,
            full_gate_grad=gradients.gate,
            full_up_grad=gradients.up,
            full_down_grad=gradients.down,
        )
        self.assertEqual([call[0] for call in runtime.calls].count("reduce_grad"), 2)
        buffer.close()

    def test_reduce_grad_transport_failure_poisons_context(self):
        torch, runtime, buffer = make_buffer()
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        gradients = ProjectionBuffers(
            tensor((6, 8), torch.float32),
            tensor((6, 8), torch.float32),
            tensor((6, 8), torch.float32),
        )
        buffer.reduce_grad(
            plan,
            full_gate_grad=gradients.gate,
            full_up_grad=gradients.up,
            full_down_grad=gradients.down,
        )
        plan.reduce_grad_status._item = 3
        with self.assertRaisesRegex(RuntimeError, "actual 3"):
            buffer.synchronize()
        self.assertTrue(buffer.context._poisoned)
        self.assertIsNone(runtime._reduce_grad_owner_token)

    def test_strict_tensor_checks(self):
        torch, _, buffer = make_buffer()
        tpe = tensor((4,), torch.int32)
        with self.assertRaisesRegex(ValueError, "NPU"):
            buffer.planning(FakeTensor((4, 2), torch.int32, "cpu:0"), tpe)
        with self.assertRaisesRegex(ValueError, "contiguous"):
            buffer.planning(tensor((4, 2), torch.int32, contiguous=False), tpe)
        with self.assertRaisesRegex(ValueError, "storage_offset"):
            buffer.planning(tensor((4, 2), torch.int32, storage_offset=1), tpe)
        torch.npu._current_device = 1
        with self.assertRaisesRegex(ValueError, "current NPU device"):
            buffer.planning(tensor((4, 2), torch.int32), tpe)

    def test_urma_dispatch_reuse_flags_events_and_zero_copy_rejection(self):
        torch, runtime, buffer = make_buffer()
        hidden = tensor((4, 8), torch.bfloat16)
        topk = tensor((4, 2), torch.int32)
        tpe = tensor((4,), torch.int32)

        with self.assertRaisesRegex(NotImplementedError, "zero_copy=True"):
            buffer.dispatch(hidden, topk_experts_sk=topk, tokens_per_expert=tpe,
                            zero_copy=True)
        self.assertEqual(runtime.calls, [])

        hidden_nvsh, _, cu_seqlens, plan, event = buffer.dispatch(
            hidden,
            topk_experts_sk=topk,
            tokens_per_expert=tpe,
            async_finish=True,
        )
        self.assertEqual(event, ("event", 0xCAFE))
        self.assertEqual(tuple(cu_seqlens.shape), (6,))
        self.assertEqual(tuple(hidden_nvsh.shape), (8, 8))
        fresh_call = next(call for call in runtime.calls if call[0] == "dispatch")
        self.assertEqual(fresh_call[-2:], (False, True))

        planning_count = sum(call[0] == "planning" for call in runtime.calls)
        native_calls = len(runtime.calls)
        with self.assertRaisesRegex(NotImplementedError, "inter_rank_sync=False"):
            buffer.dispatch(hidden, plan=plan, inter_rank_sync=False)
        self.assertEqual(
            sum(call[0] == "planning" for call in runtime.calls), planning_count
        )
        self.assertEqual(len(runtime.calls), native_calls)

        with self.assertRaisesRegex(NotImplementedError, "inter_rank_sync=False"):
            buffer.combine(plan, hidden_nvsh, inter_rank_sync=False)
        self.assertEqual(len(runtime.calls), native_calls)

        with self.assertRaisesRegex(NotImplementedError, "zero_copy=True"):
            buffer.combine(plan, hidden_nvsh, zero_copy=True)
        self.assertEqual(len(runtime.calls), native_calls)

    def test_explicit_planning_dispatch_builds_dedup_once_after_synchronize(self):
        torch, runtime, buffer = make_buffer(write_status_markers=True)
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        buffer.synchronize()

        hidden = tensor((4, 8), torch.bfloat16)
        buffer.dispatch(hidden, plan=plan)
        first = [call for call in runtime.calls if call[0] == "dispatch"][-1]
        self.assertEqual(first[-2:], (False, True))
        buffer.synchronize()

        buffer.dispatch(hidden, plan=plan)
        second = [call for call in runtime.calls if call[0] == "dispatch"][-1]
        self.assertEqual(second[-2:], (False, True))
        buffer.synchronize()
        buffer.close()

    def test_inline_dispatch_rejects_failed_planner_before_consuming_outputs(self):
        torch, runtime, buffer = make_buffer()
        original_planning = runtime.planning

        def failed_planning(*args, **kwargs):
            original_planning(*args, **kwargs)
            args[3].status._item = 1007

        runtime.planning = failed_planning
        with self.assertRaisesRegex(RuntimeError, "Planner device status is 1007"):
            buffer.dispatch(
                tensor((4, 8), torch.bfloat16),
                topk_experts_sk=tensor((4, 2), torch.int32),
                tokens_per_expert=tensor((4,), torch.int32),
            )
        self.assertFalse(any(call[0] == "dispatch" for call in runtime.calls))
        self.assertEqual(torch.npu.synchronize_calls, [0])

        buffer._pending_plans[0].status._item = 0
        buffer.close()

    def test_failed_explicit_dispatch_retains_fresh_dedup_state(self):
        runtime = FakeRuntime(write_status_markers=True, fail_dispatch_calls=1)
        torch, runtime, buffer = make_buffer(runtime=runtime)
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        buffer.synchronize()
        hidden = tensor((4, 8), torch.bfloat16)

        with self.assertRaisesRegex(RuntimeError, "fake dispatch enqueue failed"):
            buffer.dispatch(hidden, plan=plan)
        buffer.dispatch(hidden, plan=plan)

        dispatch_calls = [call for call in runtime.calls if call[0] == "dispatch"]
        self.assertEqual([call[-2] for call in dispatch_calls], [False, False])
        buffer.synchronize()
        buffer.close()

    def test_close_clears_fresh_dedup_registry(self):
        torch, _, buffer = make_buffer()
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        self.assertIs(buffer._plans_needing_dedup[id(plan)], plan)
        buffer.close()
        self.assertEqual(buffer._plans_needing_dedup, {})

    def test_native_stage_success_markers_are_accepted(self):
        torch, _, buffer = make_buffer(write_status_markers=True)
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        buffer.synchronize()

        hidden_nvsh, _, _, _ = buffer.dispatch(
            tensor((4, 8), torch.bfloat16), plan=plan
        )
        self.assertEqual(plan.status.item(), 0)
        buffer.synchronize()

        projection = ProjectionBuffers.from_local_weights(
            buffer.context,
            tensor((2, 4, 8), torch.bfloat16),
            tensor((2, 4, 8), torch.bfloat16),
            tensor((2, 8, 4), torch.bfloat16),
            torch_module=torch,
        )
        buffer.register_projection_buffers(projection)
        buffer.prefetch_weight(plan, projection)
        self.assertEqual(plan.status.item(), 4000)
        buffer.synchronize()

        buffer.combine(plan, hidden_nvsh)
        self.assertEqual(plan.status.item(), 4000)
        buffer.synchronize()

        buffer.reduce_grad(
            plan,
            full_gate_grad=tensor((6, 2, 4), torch.float32),
            full_up_grad=tensor((6, 4, 3), torch.float32),
            full_down_grad=tensor((6, 3, 2), torch.float32),
            gate_reduce_buffer=tensor((2, 2, 2, 4), torch.float32),
            up_reduce_buffer=tensor((2, 2, 4, 3), torch.float32),
            down_reduce_buffer=tensor((2, 2, 3, 2), torch.float32),
        )
        self.assertEqual(plan.status.item(), 4000)
        self.assertEqual(plan.reduce_grad_status.item(), 0)
        buffer.synchronize()
        buffer.close()

    def test_cross_node_combine_uses_one_v2_launch(self):
        torch, runtime, buffer = make_buffer(
            write_status_markers=True, node_count=2
        )
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        buffer.synchronize()
        runtime._active_udma_owner = "projection"
        phases = []

        hidden, weights, event = buffer.combine(
            plan,
            tensor((8, 8), torch.bfloat16),
            phase_barrier=lambda phase: phases.append(phase),
        )

        combine_calls = [call for call in runtime.calls if call[0] == "combine"]
        self.assertEqual(phases, [])
        self.assertEqual([call[-1] for call in combine_calls], [0])
        self.assertEqual(runtime._active_udma_owner, "dispatch")
        self.assertEqual(len(combine_calls), 1)
        self.assertEqual(tuple(hidden.shape), (4, 8))
        self.assertIsNone(weights)
        self.assertIsNone(event)
        self.assertFalse(buffer._quiesced)
        buffer.close()

    def test_cross_node_combine_does_not_require_phase_barrier(self):
        torch, runtime, buffer = make_buffer(
            write_status_markers=True, node_count=2
        )
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        buffer.synchronize()

        buffer.combine(plan, tensor((8, 8), torch.bfloat16))
        self.assertEqual(
            len([call for call in runtime.calls if call[0] == "combine"]), 1
        )
        buffer.close()

    def test_native_stage_mismatched_success_marker_fails(self):
        torch, _, buffer = make_buffer(write_status_markers=True)
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        buffer.synchronize()
        buffer.dispatch(tensor((4, 8), torch.bfloat16), plan=plan)
        plan.status._item = 3000
        with self.assertRaisesRegex(RuntimeError, "expected 0"):
            buffer.synchronize()
        buffer.close()

    def test_close_is_idempotent(self):
        torch, runtime, buffer = make_buffer()
        buffer.close()
        buffer.close()
        self.assertTrue(runtime.closed)
        self.assertEqual([call[0] for call in runtime.calls].count("close"), 1)
        self.assertEqual(torch.npu.synchronize_calls, [0])

    def test_close_waits_for_planner_status_and_rejects_cross_stream(self):
        torch, runtime, buffer = make_buffer()
        buffer.planning(tensor((4, 2), torch.int32), tensor((4,), torch.int32))
        torch.npu._stream = FakeStream(0xBEEF)
        with self.assertRaisesRegex(RuntimeError, "bound to one NPU stream"):
            buffer.dispatch(
                tensor((4, 8), torch.bfloat16), plan=buffer._pending_plans[0]
            )
        buffer.close()
        self.assertTrue(runtime.closed)
        self.assertEqual(torch.npu.synchronize_calls, [0])

    def test_close_reports_planner_failure_after_destroying_comm(self):
        torch, runtime, buffer = make_buffer()
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        plan.status._item = 1001
        with self.assertRaisesRegex(RuntimeError, "1001"):
            buffer.close()
        self.assertTrue(runtime.closed)
        self.assertTrue(buffer._closed)
        self.assertEqual(torch.npu.synchronize_calls, [0])

    def test_close_preserves_communicator_when_quiesce_is_unproven(self):
        torch, runtime, buffer = make_buffer()

        def fail_synchronize(*_args, **_kwargs):
            raise RuntimeError("device synchronization failed")

        torch.npu.synchronize = fail_synchronize
        with self.assertRaisesRegex(RuntimeError, "device synchronization failed"):
            buffer.close()
        self.assertFalse(runtime.closed)
        self.assertFalse(buffer._closed)

    def test_projection_rank_must_be_in_supported_range(self):
        torch, _, buffer = make_buffer()
        plan, _ = buffer.planning(
            tensor((4, 2), torch.int32), tensor((4,), torch.int32)
        )
        for bad_shape in ((4,), (4, 1, 1, 1, 32)):
            bad = ProjectionBuffers(
                tensor((4, 4, 8), torch.bfloat16),
                tensor(bad_shape, torch.bfloat16),
                tensor((4, 8, 4), torch.bfloat16),
            )
            with self.subTest(shape=bad_shape), self.assertRaisesRegex(
                ValueError, "rank must be in \\[2, 4\\]"
            ):
                buffer.prefetch_weight(plan, bad)

    def test_json_case_and_oversubscribed_topology(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cases.json"
            path.write_text(
                json.dumps([{"id": "smoke", "S": 32, "K": 2, "E": 8, "H": 64}]),
                encoding="utf-8",
            )
            self.assertEqual(load_cases(path)[0], BenchmarkCase("smoke", 32, 2, 8, 64))
            self.assertEqual(
                load_cases(path)[0].route_distribution,
                "moonep_combine_balanced",
            )
            self.assertEqual(
                BenchmarkCase("fp16", 1, 1, 1, 1, dtype="float16").dtype,
                "float16",
            )
        with self.assertRaisesRegex(ValueError, "unknown benchmark case fields"):
            BenchmarkCase.from_mapping(
                {"id": "bad", "S": 1, "K": 1, "E": 1, "H": 1, "mystery": 1}
            )
        for unsafe in (".", "..", "../escape", "bad name"):
            with self.assertRaisesRegex(ValueError, "case_id"):
                BenchmarkCase(unsafe, 1, 1, 1, 1)
        topology = resolve_topology(
            physical_device_count=8,
            ranks_per_device=2,
            world_size=None,
            planner_block_dim=None,
            environment={},
        )
        self.assertEqual(topology["logical_world_size"], 16)
        self.assertTrue(topology["oversubscribed"])
        self.assertEqual(topology["planner_block_dim"], 32)
        self.assertEqual(topology["dispatch_aiv_core_count"], 64)
        self.assertEqual(topology["dispatch_aiv_core_count_source"], "default")
        device_map = [rank_to_device(rank, 8) for rank in range(16)]
        self.assertEqual(device_map, list(range(8)) * 2)
        self.assertEqual({device: device_map.count(device) for device in range(8)},
                         {device: 2 for device in range(8)})
        native = resolve_topology(
            physical_device_count=8,
            ranks_per_device=1,
            world_size=8,
            planner_block_dim=None,
            environment={},
        )
        self.assertEqual(native["planner_block_dim"], 64)
        self.assertEqual(native["planner_block_dim_source"], "default_native")
        configured = resolve_topology(
            physical_device_count=8,
            ranks_per_device=1,
            world_size=8,
            planner_block_dim=None,
            environment={"TILEXR_MOONEP_DISPATCH_AIV_CORE_COUNT": "32"},
        )
        self.assertEqual(configured["dispatch_aiv_core_count"], 32)
        self.assertEqual(configured["dispatch_aiv_core_count_source"], "environment")
        with self.assertRaisesRegex(ValueError, "world_size=16 <= blockDim <= 64"):
            resolve_topology(
                physical_device_count=8,
                ranks_per_device=2,
                world_size=16,
                planner_block_dim=8,
                environment={},
            )
        with self.assertRaisesRegex(ValueError, "blockDim <= 64"):
            resolve_topology(
                physical_device_count=8,
                ranks_per_device=1,
                world_size=8,
                planner_block_dim=65,
                environment={},
            )
        with self.assertRaisesRegex(ValueError, "1 <= coreCount <= 64"):
            resolve_topology(
                physical_device_count=8,
                ranks_per_device=1,
                world_size=8,
                planner_block_dim=None,
                environment={},
                dispatch_aiv_core_count=65,
            )

    def test_planner_wait_budget_defaults_to_bounded_native_value(self):
        from tools.moonep.benchmark import build_parser as build_benchmark_parser
        from tools.moonep.launcher import build_parser as build_launcher_parser

        benchmark_args = build_benchmark_parser().parse_args(
            ["--cases", "cases.json", "--output-dir", "output"]
        )
        launcher_args = build_launcher_parser().parse_args(
            ["--cases", "cases.json", "--output-dir", "output"]
        )
        self.assertEqual(benchmark_args.wait_iterations, 1_000_000)
        self.assertEqual(launcher_args.wait_iterations, 1_000_000)

    def test_distributed_launcher_builds_dispatch_hot_loop_command(self):
        from tools.moonep.distributed_node import build_parser as build_node_parser
        from tools.moonep.launcher import _process_command

        args = build_node_parser().parse_args([
            "--cases", "cases.json",
            "--output-dir", "output",
            "--install-prefix", "install",
            "--node-count", "4",
            "--node-rank", "0",
            "--benchmark-kind", "dispatch_hot_loop",
            "--dispatch-modes", "hidden",
            "--route-distribution", "moonep_combine_balanced",
        ])
        command = _process_command(args)
        self.assertIn("tools.moonep.dispatch_hot_loop", command)
        self.assertIn("--dispatch-modes", command)
        self.assertNotIn("--mode", command)

    def test_completion_rendezvous_preserves_peer_window_teardown_order(self):
        def run_pair(*, client_quiesced=True, client_passed=True, stale=False):
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                listener.bind(("127.0.0.1", 0))
                endpoint = f"127.0.0.1:{listener.getsockname()[1]}"

            launch_id = "launch-test"
            secret = "a" * 64
            server_results = []
            errors = []

            def run_server():
                try:
                    server_results.append(
                        completion_barrier(
                            0,
                            2,
                            endpoint,
                            launch_id=launch_id,
                            secret=secret,
                            case_id="smoke",
                            quiesced=True,
                            passed=True,
                            timeout_sec=2.0,
                        )
                    )
                except Exception as exc:
                    errors.append(exc)

            server = threading.Thread(target=run_server)
            server.start()
            stale_thread = None
            if stale:
                def run_stale():
                    try:
                        completion_barrier(
                            1,
                            2,
                            endpoint,
                            launch_id=launch_id,
                            secret="b" * 64,
                            case_id="old-case",
                            quiesced=True,
                            passed=True,
                            timeout_sec=0.2,
                        )
                    except Exception:
                        return
                    errors.append(RuntimeError("stale arrival was released"))

                stale_thread = threading.Thread(target=run_stale)
                stale_thread.start()
            client_result = completion_barrier(
                1,
                2,
                endpoint,
                launch_id=launch_id,
                secret=secret,
                case_id="smoke",
                quiesced=client_quiesced,
                passed=client_passed,
                timeout_sec=2.0,
            )
            server.join(timeout=3.0)
            if stale_thread is not None:
                stale_thread.join(timeout=1.0)
                self.assertFalse(stale_thread.is_alive())
            self.assertFalse(server.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(server_results, [client_result])
            return client_result

        success = run_pair(stale=True)
        self.assertTrue(success.release)
        self.assertFalse(success.abort)
        failed = run_pair(client_passed=False)
        self.assertTrue(failed.release)
        self.assertTrue(failed.abort)
        unsafe = run_pair(client_quiesced=False)
        self.assertFalse(unsafe.release)
        self.assertTrue(unsafe.abort)
        self.assertEqual(offset_host_port("127.0.0.1:65500"), "127.0.0.1:65387")

        source = (ROOT / "tools" / "moonep" / "benchmark.py").read_text(
            encoding="utf-8"
        )
        sync_at = source.index("buffer.quiesce()")
        barrier_at = source.index("completion_barrier_from_env(", sync_at)
        error_hold_at = source.index("_hold_unsafe_teardown(", barrier_at)
        unsafe_at = source.index("if not decision.release:", barrier_at)
        hold_at = source.index("_hold_unsafe_teardown(", unsafe_at)
        close_at = source.index("owner.close()", barrier_at)
        self.assertLess(sync_at, barrier_at)
        self.assertLess(barrier_at, error_hold_at)
        self.assertLess(error_hold_at, unsafe_at)
        self.assertLess(barrier_at, unsafe_at)
        self.assertLess(unsafe_at, hold_at)
        self.assertLess(hold_at, close_at)

    def test_launcher_detects_nonzero_rank_without_waiting_for_rank_zero(self):
        from tools.moonep.launcher import main as launcher_main

        class Process:
            def __init__(self, return_code=None):
                self.return_code = return_code
                self.terminated = False

            def poll(self):
                return self.return_code

            def terminate(self):
                self.terminated = True
                self.return_code = -15

            def wait(self, timeout=None):
                return self.return_code

            def kill(self):
                self.return_code = -9

        rank_zero = Process()
        rank_one = Process(7)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cases = root / "cases.json"
            output = root / "output"
            cases.write_text(
                json.dumps([{"id": "smoke", "S": 4, "K": 2, "E": 4, "H": 8}]),
                encoding="utf-8",
            )
            with patch(
                "tools.moonep.launcher.subprocess.Popen",
                side_effect=[rank_zero, rank_one],
            ), patch(
                "tools.moonep.launcher.secrets.token_hex", return_value="c" * 64
            ):
                with self.assertRaisesRegex(RuntimeError, "ranks failed"):
                    launcher_main(
                        [
                            "--cases",
                            str(cases),
                            "--output-dir",
                            str(output),
                            "--physical-device-count",
                            "1",
                            "--ranks-per-device",
                            "2",
                            "--world-size",
                            "2",
                            "--comm-id",
                            "127.0.0.1:19001",
                            "--timeout-sec",
                            "2",
                        ]
                    )
            self.assertTrue(rank_zero.terminated)
            metadata = json.loads((output / "launcher_metadata.json").read_text())
            self.assertIn("launch_id", metadata)
            self.assertNotIn("launch_secret", metadata)

    def test_unsafe_teardown_holds_when_artifact_writes_fail(self):
        from tools.moonep.benchmark import _hold_unsafe_teardown

        class ReachedHold(Exception):
            pass

        with tempfile.TemporaryDirectory() as directory, patch(
            "tools.moonep.benchmark.write_json", side_effect=OSError("disk full")
        ), patch(
            "tools.moonep.benchmark.signal_managed_abort",
            side_effect=OSError("sentinel write failed"),
        ), patch(
            "tools.moonep.benchmark.hold_for_managed_abort",
            side_effect=ReachedHold,
        ):
            root = Path(directory)
            with self.assertRaises(ReachedHold):
                _hold_unsafe_teardown(
                    output_root=root,
                    rank_dir=root / "rank_0",
                    result={"status": "failed"},
                    rank=0,
                    reason="unquiesced",
                    world_size=2,
                )

    def test_unmanaged_multi_rank_worker_is_rejected_before_setup(self):
        from tools.moonep.benchmark import main as benchmark_main

        with patch.dict(
            "os.environ",
            {"WORLD_SIZE": "2", "TILEXR_MOONEP_MANAGED_LAUNCH": "0"},
            clear=False,
        ):
            with self.assertRaisesRegex(RuntimeError, "managed launcher"):
                benchmark_main(
                    [
                        "--cases",
                        "not-read.json",
                        "--output-dir",
                        "not-created",
                    ]
                )

    def test_scale_metadata_can_express_256_512_1024_with_bounded_lane_groups(self):
        torch = FakeTorch()
        for global_world_size, lane_size in ((256, 32), (512, 64), (1024, 128)):
            planner_size = min(lane_size, 64)
            runtime = FakeRuntime(rank=0, world_size=planner_size)
            context = TileXRMoonEPContext(
                runtime=runtime,
                global_rank=0,
                global_world_size=global_world_size,
                node_rank=0,
                node_count=global_world_size // 8,
                local_rank=0,
                local_world_size=8,
                planner_group_rank=0,
                planner_group_size=planner_size,
                lane_group_rank=0,
                lane_group_size=lane_size,
                device_index=0,
                tokens_per_rank=1,
                hidden_size=1,
                topk=1,
                expert_count=planner_size,
                dtype=torch.bfloat16,
            )
            self.assertEqual(context.global_world_size, global_world_size)
            self.assertEqual(context.local_world_size, 8)
            self.assertEqual(context.node_count, lane_size)
            self.assertEqual(context.lane_group_size, lane_size)
            self.assertLessEqual(context.planner_group_size, 64)
            with patch.dict(
                "os.environ",
                {
                    "TILEXR_PHYSICAL_DEVICE_COUNT": "8",
                    "TILEXR_RANKS_PER_DEVICE": "1",
                    "TILEXR_MOONEP_PLANNER_BLOCK_DIM": str(planner_size),
                },
                clear=False,
            ):
                metadata = topology_metadata(context)
            self.assertEqual(metadata["planner_block_dim"], planner_size)

    def test_planner_cpu_oracle_rejects_corrupted_output(self):
        class Values:
            def __init__(self, values, scalar=None):
                self.values = values
                self.scalar = scalar

            def cpu(self):
                return self

            def tolist(self):
                return self.values

            def item(self):
                return self.scalar

        _, _, buffer = make_buffer()
        context = buffer.context
        reference = build_reference_plan(
            rank=context.planner_group_rank,
            rank_size=context.planner_group_size,
            tokens_per_rank=context.tokens_per_rank,
            topk=context.topk,
            expert_count=context.expert_count,
            all_topk=deterministic_all_topk(
                context.planner_group_size,
                context.tokens_per_rank,
                context.topk,
                context.expert_count,
            ),
        )
        plan = SimpleNamespace(
            status=Values(None, 0),
            dst=Values(list(reference.dst)),
            cu_seqlens=Values(list(reference.cu_seqlens)),
            zero_fill_ranges=Values(list(reference.zero_fill_ranges)),
            experts_to_copy=Values(list(reference.experts_to_copy)),
            remote_stats=Values(list(reference.remote_stats)),
        )
        self.assertEqual(validate_plan(plan, context)["mode"], "planner_cpu_oracle")
        plan.status.scalar = 5000
        self.assertEqual(
            validate_plan(plan, context, expected_status=5000)["mode"],
            "planner_cpu_oracle",
        )
        with self.assertRaisesRegex(RuntimeError, "expected 0"):
            validate_plan(plan, context)
        plan.status.scalar = 0
        plan.dst.values[0] += 1
        with self.assertRaisesRegex(RuntimeError, "dst mismatch"):
            validate_plan(plan, context)

    def test_planner_cpu_oracle_supports_v3_padding_and_prefetch_slots(self):
        reference = build_reference_plan(
            rank=0,
            rank_size=1,
            tokens_per_rank=8,
            topk=2,
            expert_count=8,
            prefetch_slots=2,
            token_padding=4,
            all_topk=deterministic_all_topk(1, 8, 2, 8),
        )
        self.assertEqual(reference.dispatched_capacity, 64)
        self.assertEqual(reference.dst[1], 4)
        self.assertEqual(len(reference.cu_seqlens), 10)
        self.assertEqual(len(reference.experts_to_copy), 2)

    def test_cross_rank_max_report_marks_stub_scope(self):
        capabilities = {
            "abi_version": 1,
            "stage_mask": 1,
            "stub_mask": 30,
            "implementations": {
                "planning": "native",
                "dispatch": "stub",
                "prefetch_weight": "stub",
                "combine": "stub",
                "reduce_grad": "stub",
            },
            "transport_performance_valid": False,
        }
        with tempfile.TemporaryDirectory() as directory:
            case_dir = Path(directory) / "smoke"
            for rank, values in enumerate(((10.0, 30.0), (20.0, 25.0))):
                rank_dir = case_dir / f"rank_{rank}"
                write_json(
                    rank_dir / "result.json",
                    {
                        "status": "passed",
                        "case": {"case_id": "smoke", "tokens_per_rank": 4},
                        "capabilities": capabilities,
                        "topology": {
                            "physical_device_count": 1,
                            "ranks_per_device": 2,
                            "oversubscribed": True,
                            "planner_block_dim": 32,
                            "planner_block_dim_source": "default_oversubscribed",
                        },
                        "validation": {"passed": True},
                    },
                )
                write_jsonl(
                    rank_dir / "samples.jsonl",
                    [
                        {"iteration": 0, "timings_us": {"end_to_end": values[0]}},
                        {"iteration": 1, "timings_us": {"end_to_end": values[1]}},
                    ],
                )
            summary = aggregate_rank_artifacts(case_dir, world_size=2)
            native_capabilities = dict(capabilities)
            native_capabilities["stage_mask"] = 31
            native_capabilities["stub_mask"] = 0
            native_capabilities["transport_performance_valid"] = True
            native_capabilities["implementations"] = {
                name: "native" for name in capabilities["implementations"]
            }
            for rank in range(2):
                result_path = case_dir / f"rank_{rank}" / "result.json"
                result = json.loads(result_path.read_text(encoding="utf-8"))
                result["capabilities"] = native_capabilities
                write_json(result_path, result)
            native_summary = aggregate_rank_artifacts(case_dir, world_size=2)
            self.assertFalse(native_summary["transport_performance_valid"])
            self.assertEqual(
                native_summary["performance_scope"], "oversubscribed_functional_only"
            )
            rank_one_result = case_dir / "rank_1" / "result.json"
            mixed = json.loads(rank_one_result.read_text(encoding="utf-8"))
            mixed["capabilities"]["stub_mask"] = 1
            write_json(rank_one_result, mixed)
            with self.assertRaisesRegex(ValueError, "capability metadata differs"):
                aggregate_rank_artifacts(case_dir, world_size=2)
        maxima = [
            item["timings_us"]["end_to_end"]
            for item in summary["cross_rank_max_samples"]
        ]
        self.assertEqual(maxima, [20.0, 30.0])
        self.assertEqual(summary["metrics_us"]["end_to_end"]["p99"], 30.0)
        self.assertFalse(summary["transport_performance_valid"])
        self.assertEqual(summary["performance_scope"], "oversubscribed_functional_only")
        self.assertIn("p50", summary["tokens_per_second"])

    def test_dispatch_hot_loop_report_uses_hidden_kernel_throughput(self):
        with tempfile.TemporaryDirectory() as directory:
            case_dir = Path(directory) / "dispatch"
            rank_dir = case_dir / "rank_0"
            write_json(rank_dir / "result.json", {
                "benchmark_kind": "dispatch_hot_loop",
                "dispatch_modes": ["hidden"],
                "status": "passed",
                "rank": 0,
                "case": {"case_id": "dispatch", "tokens_per_rank": 4},
                "capabilities": {
                    "stage_mask": 3,
                    "stub_mask": 28,
                    "transport_performance_valid": False,
                },
                "topology": {
                    "physical_device_count": 1,
                    "ranks_per_device": 1,
                    "oversubscribed": False,
                    "peer_memory_cross_node": False,
                    "dispatch_aiv_core_count": 64,
                    "dispatch_aiv_core_count_source": "distributed_cli",
                },
                "validation": {"passed": True},
            })
            write_jsonl(rank_dir / "samples.jsonl", [{
                "iteration": 0,
                "timings_us": {"hidden_host": 2.0, "hidden_kernel": 20.0},
            }])
            summary = aggregate_rank_artifacts(case_dir, world_size=1)
        self.assertEqual(summary["performance_scope"], "dispatch_native")
        self.assertEqual(summary["dispatch_modes"], ["hidden"])
        self.assertEqual(summary["metrics_us"]["hidden_kernel"]["p95"], 20.0)
        self.assertEqual(summary["tokens_per_second"]["mean"], 200000.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
