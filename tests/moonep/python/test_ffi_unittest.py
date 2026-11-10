"""用 fake native functions 验证 ctypes 参数布局、工作区查询及 stage 调用，不启动 NPU。"""
from __future__ import annotations

import ctypes
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[3]
INTEGRATION = ROOT / "integrations" / "moonep_torch"
for path in (INTEGRATION, Path(__file__).resolve().parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from fakes import FakeTensor, FakeTorch
from tilexr_moonep import ProjectionBuffers, TileXRMoonEPBuffer, TileXRMoonEPContext
from tilexr_moonep.abi import (
    TileXRMoonEPDispatchArgsV1,
    TileXRMoonEPDispatchArgsV2,
    TileXRMoonEPPlanV1,
    TileXRMoonEPPlanningArgsV1,
    TileXRMoonEPPrefetchWeightArgsV1,
    TileXRMoonEPReduceGradArgsV1,
    TileXRMoonEPReduceGradArgsV2,
    TileXRMoonEPReduceGradPrepareArgsV2,
    TileXRMoonEPReduceGradSourceSliceV2,
    TileXRMoonEPReduceGradWorkspaceInfoV2,
    TileXRMoonEPReduceGradWorkspaceQueryV2,
    TileXRMoonEPTensorV1,
)
from tilexr_moonep.runtime import TileXRMoonEPRuntime, _resolve_library
from tilexr_moonep.torch_api import _format_dispatch_completion_flags


class FakeFunction:
    def __init__(self, callback):
        self.callback = callback
        self.argtypes = None
        self.restype = None

    def __call__(self, *args):
        return self.callback(*args)


class FakeLibrary:
    pass


class FakeCDLLLoader:
    def __init__(self):
        self.loads = []
        self.destroy_calls = 0
        self.lifecycle_calls = []
        self.next_udma_handle = 7
        self.planning_records = []
        self.stage_records = []
        self.combine_v2_workspace_records = []
        self.reduce_grad_records = []
        self.reduce_grad_prepare_records = []
        self.reduce_grad_destroy_records = []
        self.register_calls = []
        self.unregister_calls = []
        self.comm = self._comm_library()
        self.planner = self._planner_library()
        self.combine_v2 = self._combine_v2_library()
        self.moonep = self._moonep_library()

    def _comm_library(self):
        library = FakeLibrary()

        def init_rank_shared(domain, world, rank, output):
            self.assert_scalar(domain, 0)
            self.assert_scalar(world, 2)
            self.assert_scalar(rank, 0)
            ctypes.cast(output, ctypes.POINTER(ctypes.c_void_p)).contents.value = 0x1234
            return 0

        def init_rank_memory(domain, world, rank, output):
            self.assert_scalar(domain, 1)
            self.assert_scalar(world, 2)
            self.assert_scalar(rank, 0)
            ctypes.cast(output, ctypes.POINTER(ctypes.c_void_p)).contents.value = 0x2345
            return 0

        def destroy(comm):
            value = comm.value if hasattr(comm, "value") else comm
            if int(value) not in (0x1234, 0x2345):
                raise AssertionError(f"unexpected communicator {int(value):#x}")
            self.destroy_calls += 1
            self.lifecycle_calls.append(("destroy", int(value)))
            return 0

        def register(comm, address, byte_count, output):
            self.assert_scalar(comm, 0x1234)
            if not address.value:
                raise AssertionError("invalid UDMA registration")
            size = int(byte_count.value if hasattr(byte_count, "value") else byte_count)
            if size not in (2 * 1024 * 1024, 4 * 1024 * 1024):
                raise AssertionError(f"unexpected UDMA registration size {size}")
            handle = self.next_udma_handle
            self.next_udma_handle += 1
            ctypes.cast(output, ctypes.POINTER(ctypes.c_uint32)).contents.value = handle
            self.lifecycle_calls.append(("register", size, handle))
            self.register_calls.append((address.value, size))
            return 0

        def unregister(comm, handle):
            self.assert_scalar(comm, 0x1234)
            value = handle.value if hasattr(handle, "value") else handle
            value = int(value)
            self.lifecycle_calls.append(("unregister", value))
            self.unregister_calls.append(value)
            return 0

        def get_qp_count(comm, qp_count):
            self.assert_scalar(comm, 0x1234)
            ctypes.cast(qp_count, ctypes.POINTER(ctypes.c_uint32)).contents.value = 3
            return 0

        library.TileXRCommInitRankWithSharedQpDomain = FakeFunction(init_rank_shared)
        library.TileXRCommInitRankMemoryDomain = FakeFunction(init_rank_memory)
        library.TileXRCommDestroy = FakeFunction(destroy)
        library.TileXRUDMARegister = FakeFunction(register)
        library.TileXRUDMAUnregister = FakeFunction(unregister)
        library.TileXRUDMAGetQpCount = FakeFunction(get_qp_count)
        return library

    @staticmethod
    def assert_scalar(value, expected):
        actual = value.value if hasattr(value, "value") else value
        if int(actual) != expected:
            raise AssertionError(f"expected {expected}, got {actual}")

    def _planner_library(self):
        library = FakeLibrary()

        def dst_local_offset(comm, s, k, e, b, token_padding, output):
            self.assert_scalar(comm, 0x1234)
            for value, expected in (
                (s, 4), (k, 2), (e, 4), (b, 2), (token_padding, 2)
            ):
                self.assert_scalar(value, expected)
            ctypes.cast(output, ctypes.POINTER(ctypes.c_uint64)).contents.value = 128
            return 0

        library.TileXRMoonEpPlannerGetDstLocalOffsetV3 = FakeFunction(
            dst_local_offset
        )
        return library

    def _combine_v2_library(self):
        library = FakeLibrary()

        def workspace(bs, h, topk, nv_s, dtype, workspace_bytes,
                      output_epoch0_offset, output_epoch1_offset):
            record = tuple(
                int(value.value if hasattr(value, "value") else value)
                for value in (bs, h, topk, nv_s, dtype)
            )
            self.combine_v2_workspace_records.append(record)
            ctypes.cast(workspace_bytes, ctypes.POINTER(ctypes.c_uint64)).contents.value = (
                2 * 1024 * 1024
            )
            ctypes.cast(
                output_epoch0_offset, ctypes.POINTER(ctypes.c_uint64)
            ).contents.value = 0x2000
            ctypes.cast(
                output_epoch1_offset, ctypes.POINTER(ctypes.c_uint64)
            ).contents.value = 0x3000
            return 0

        def stage(registered_workspace, registered_workspace_bytes, dst_local,
                  comm, weight_memory_comm, bs, h, topk, nv_s, aiv_core_num, hidden_nvsh,
                  hidden_sh, route_weights_nvs, route_weights_sk, dtype, stream):
            def scalar(value):
                raw = value.value if hasattr(value, "value") else value
                return 0 if raw is None else int(raw)

            record = {
                "name": "combine",
                "stream": scalar(stream),
                "flags": 0,
                "plan_capacity": scalar(nv_s),
                "workspace": scalar(registered_workspace),
                "workspace_bytes": scalar(registered_workspace_bytes),
                "dst_local": scalar(dst_local),
                "weight_memory_comm": scalar(weight_memory_comm),
                "aiv_core_num": scalar(aiv_core_num),
                "dtype": scalar(dtype),
                "shapes": {
                    "hiddenNvsh": (scalar(nv_s), scalar(h)),
                    "routeWeightsNvs": (scalar(nv_s),)
                    if scalar(route_weights_nvs) else None,
                    "hiddenSh": (scalar(bs), scalar(h)),
                    "routeWeightsSk": (scalar(bs), scalar(topk))
                    if scalar(route_weights_sk) else None,
                },
                "pointers": {
                    "hiddenNvsh": scalar(hidden_nvsh),
                    "hiddenSh": scalar(hidden_sh),
                    "routeWeightsNvs": scalar(route_weights_nvs),
                    "routeWeightsSk": scalar(route_weights_sk),
                },
            }
            self.stage_records.append(record)
            return 0

        library.TileXRMoonEpCombineGetWorkspaceSizeV2 = FakeFunction(workspace)
        library.TileXRMoonEpCombineStageV2 = FakeFunction(lambda *_args: 0)
        library.TileXRMoonEpCombineStageV2Fused = FakeFunction(stage)
        return library

    def _moonep_library(self):
        library = FakeLibrary()
        library.TileXRMoonEpGetAbiVersion = FakeFunction(lambda: 2)

        def capabilities(native, stub):
            ctypes.cast(native, ctypes.POINTER(ctypes.c_uint64)).contents.value = 31
            ctypes.cast(stub, ctypes.POINTER(ctypes.c_uint64)).contents.value = 0
            return 0

        def workspace(comm, s, k, e, b, token_padding, workspace_bytes, capacity):
            self.assert_scalar(comm, 0x1234)
            self.assert_scalar(s, 4)
            self.assert_scalar(k, 2)
            self.assert_scalar(e, 4)
            self.assert_scalar(b, 2)
            self.assert_scalar(token_padding, 2)
            ctypes.cast(workspace_bytes, ctypes.POINTER(ctypes.c_uint64)).contents.value = 256
            ctypes.cast(capacity, ctypes.POINTER(ctypes.c_int64)).contents.value = 12
            return 0

        def planning(args_ptr, stream):
            args = ctypes.cast(
                args_ptr, ctypes.POINTER(TileXRMoonEPPlanningArgsV1)
            ).contents
            topk = args.topkExperts.contents
            tpe = args.tokensPerExpert.contents
            cu = args.cuSeqlens.contents
            plan = args.plan.contents
            self.planning_records.append(
                {
                    "stream": stream.value,
                    "workspace_bytes": args.workspaceBytes,
                    "wait_iterations": args.waitIterations,
                    "flags": args.flags,
                    "topk_shape": tuple(topk.shape[: topk.rank]),
                    "tpe_shape": tuple(tpe.shape[: tpe.rank]),
                    "cu_shape": tuple(cu.shape[: cu.rank]),
                    "plan": (plan.n, plan.r, plan.e, plan.b, plan.nvS, plan.k),
                }
            )
            return 0

        def dispatch_workspace(comm, s, k, h, dtype, workspace_bytes, alignment):
            self.assert_scalar(comm, 0x1234)
            self.assert_scalar(s, 6)
            self.assert_scalar(k, 2)
            self.assert_scalar(h, 8)
            self.assert_scalar(dtype, 11)
            ctypes.cast(workspace_bytes, ctypes.POINTER(ctypes.c_uint64)).contents.value = (
                2 * 1024 * 1024
            )
            ctypes.cast(alignment, ctypes.POINTER(ctypes.c_uint64)).contents.value = (
                2 * 1024 * 1024
            )
            return 0

        library.TileXRMoonEpGetCapabilitiesV1 = FakeFunction(capabilities)
        library.TileXRMoonEpGetCapabilitiesV2 = FakeFunction(capabilities)
        library.TileXRMoonEpPlanningGetWorkspaceSizeV1 = FakeFunction(workspace)
        library.TileXRMoonEpDispatchGetWorkspaceSizeV2 = FakeFunction(dispatch_workspace)
        library.TileXRMoonEpPlanningV1 = FakeFunction(planning)
        for name, args_type in (
            ("dispatch", TileXRMoonEPDispatchArgsV2),
            ("prefetch_weight", TileXRMoonEPPrefetchWeightArgsV1),
        ):
            setattr(
                library,
                {
                    "dispatch": "TileXRMoonEpDispatchV2",
                    "prefetch_weight": "TileXRMoonEpPrefetchWeightV1",
                }[name],
                FakeFunction(self._stage_callback(name, args_type)),
            )

        def reduce_grad_query(query_ptr, info_ptr):
            query = ctypes.cast(
                query_ptr, ctypes.POINTER(TileXRMoonEPReduceGradWorkspaceQueryV2)
            ).contents
            info = ctypes.cast(
                info_ptr, ctypes.POINTER(TileXRMoonEPReduceGradWorkspaceInfoV2)
            ).contents
            descriptors = (query.gate.contents, query.up.contents, query.down.contents)
            info.workspaceBytes = 2 << 20
            info.workspaceAlignment = 2 << 20
            info.udmaChunkBytes = 4 << 20
            info.laneStateBytes = 3 * 4096
            info.laneStateStrideBytes = 4096
            info.bankStrideBytes = 8 << 20
            info.laneStrideBytes = 16 << 20
            info.qpCount = 3
            info.blockDim = 64
            for index, tensor_value in enumerate(descriptors):
                info.rowBytes[index] = int(
                    tensor_value.elementCount // tensor_value.shape[0]
                ) * 4
                info.chunkCounts[index] = 1
                info.projectionQpCounts[index] = 1
            return 0

        def reduce_grad_prepare(args_ptr, prepared_ptr):
            args = ctypes.cast(
                args_ptr, ctypes.POINTER(TileXRMoonEPReduceGradPrepareArgsV2)
            ).contents
            self.reduce_grad_prepare_records.append({
                "workspace_bytes": args.workspaceBytes,
                "requested_chunk_bytes": args.requestedUdmaChunkBytes,
                "source_bytes": tuple(int(value.bytes) for value in args.sources),
                "source_ptrs": tuple(int(value.data) for value in args.sources),
                "registration_bytes": tuple(
                    int(value.registrationBytes) for value in args.sources
                ),
                "registration_ptrs": tuple(
                    int(value.registrationBase) for value in args.sources
                ),
            })
            ctypes.cast(prepared_ptr, ctypes.POINTER(ctypes.c_void_p)).contents.value = 0x5678
            return 0

        def reduce_grad_destroy(prepared):
            self.reduce_grad_destroy_records.append(int(prepared.value))
            return 0

        def reduce_grad(args_ptr, stream):
            args = ctypes.cast(
                args_ptr, ctypes.POINTER(TileXRMoonEPReduceGradArgsV2)
            ).contents
            self.reduce_grad_records.append({
                "stream": stream.value,
                "flags": args.flags,
                "wait_iterations": args.waitIterations,
                "prepared": int(args.prepared),
                "source_bytes": tuple(int(value.bytes) for value in args.sources),
                "registration_bytes": tuple(
                    int(value.registrationBytes) for value in args.sources
                ),
                "shapes": tuple(
                    tuple(pointer.contents.shape[: pointer.contents.rank])
                    for pointer in (args.gate, args.up, args.down)
                ),
                "status_shape": tuple(
                    args.status.contents.shape[: args.status.contents.rank]
                ),
            })
            return 0

        library.TileXRMoonEpReduceGradGetWorkspaceSizeV2 = FakeFunction(reduce_grad_query)
        library.TileXRMoonEpReduceGradPrepareV2 = FakeFunction(reduce_grad_prepare)
        library.TileXRMoonEpReduceGradDestroyPreparedV2 = FakeFunction(reduce_grad_destroy)
        library.TileXRMoonEpReduceGradV2 = FakeFunction(reduce_grad)
        return library

    def _stage_callback(self, name, args_type):
        def callback(args_ptr, stream):
            args = ctypes.cast(args_ptr, ctypes.POINTER(args_type)).contents
            plan = args.plan.contents
            fields = {
                "dispatch": ("hiddenSh", "routeWeightsSk", "hiddenNvsh", "routeWeightsNvs"),
                "prefetch_weight": ("gate", "up", "down"),
            }[name]
            shapes = {}
            for field in fields:
                pointer = getattr(args, field)
                shapes[field] = None if not pointer else tuple(
                    pointer.contents.shape[: pointer.contents.rank]
                )
            self.stage_records.append({
                "name": name,
                "stream": stream.value,
                "flags": args.flags,
                "abi_version": args.abiVersion,
                "plan_capacity": plan.nvS,
                "shapes": shapes,
            })
            return 0

        return callback

    def __call__(self, path, mode):
        self.loads.append((str(path), mode))
        name = Path(path).name
        if name == "libtile-comm.so":
            return self.comm
        if "planner" in name:
            return self.planner
        if "combine-v2" in name:
            return self.combine_v2
        if "moonep" in name:
            return self.moonep
        raise AssertionError(f"unexpected library path {path}")


def tensor(shape, dtype):
    return FakeTensor(shape, dtype, "npu:0")


class FfiAbiTests(unittest.TestCase):
    def test_udma_arena_subregion_reuses_active_registration(self):
        loader = FakeCDLLLoader()
        runtime = TileXRMoonEPRuntime(
            rank=0,
            world_size=2,
            library_paths={
                "comm": "libtile-comm.so",
                "planner": "libtilexr-moonep-planner.so",
                "combine_v2": "libtilexr-moonep-combine-v2.so.2",
                "moonep": "libtilexr-moonep.so.1",
            },
            cdll_loader=loader,
        )

        handle = runtime._activate_udma_region(
            0x200000, 2 * 1024 * 1024, "dispatch", "arena"
        )
        reused = runtime._activate_udma_region(
            0x300000, 1024 * 1024, "projection", "subregion"
        )

        self.assertEqual(reused, handle)
        self.assertEqual(loader.register_calls, [(0x200000, 2 * 1024 * 1024)])
        runtime.close()

    def test_reduce_grad_registration_uses_storage_without_expanding_source(self):
        backing = FakeTensor((2 * 1024 * 1024 // 4,), "float32")
        backing._ptr = 0x200000
        registration = backing.narrow(0, 1024, 512)
        registration._tilexr_registration_backing = backing

        source = FakeTensor((4, 16), "float32")
        slices = TileXRMoonEPRuntime._reduce_grad_source_slices(
            (source, source, source),
            (registration, registration, registration),
        )
        self.assertEqual(tuple(int(value.bytes) for value in slices), (256,) * 3)
        self.assertEqual(
            tuple(int(value.registrationBase) for value in slices),
            (0x200000,) * 3,
        )
        self.assertEqual(
            tuple(int(value.registrationBytes) for value in slices),
            (2 * 1024 * 1024,) * 3,
        )

    def test_dispatch_completion_flag_matrix_format(self):
        flags = bytearray(512 * 2 * 8)
        struct.pack_into("<QQ", flags, 0 * 16, 0, 0)
        struct.pack_into("<QQ", flags, 1 * 16, 23, 25)
        struct.pack_into("<QQ", flags, 2 * 16, 25, 25)
        struct.pack_into("<QQ", flags, 9 * 16, 21, 21)

        self.assertEqual(
            _format_dispatch_completion_flags(bytes(flags), rank_size=3),
            "host_flags=r0:0/0,r1:23/25,r2:25/25;inactive_nonzero=1",
        )

    def test_ctypes_layout_matches_tilexr_moonep_header(self):
        self.assertEqual(ctypes.sizeof(TileXRMoonEPTensorV1), 64)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPPlanV1), 120)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPPlanningArgsV1), 80)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPDispatchArgsV1), 80)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPDispatchArgsV2), 80)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPPrefetchWeightArgsV1), 56)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPReduceGradArgsV1), 48)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPReduceGradWorkspaceQueryV2), 64)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPReduceGradWorkspaceInfoV2), 136)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPReduceGradSourceSliceV2), 32)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPReduceGradPrepareArgsV2), 176)
        self.assertEqual(ctypes.sizeof(TileXRMoonEPReduceGradArgsV2), 168)
        self.assertEqual(TileXRMoonEPTensorV1.shape.offset, 32)
        self.assertEqual(TileXRMoonEPPlanV1.dst.offset, 56)
        self.assertEqual(TileXRMoonEPPlanV1.status.offset, 112)
        self.assertEqual(TileXRMoonEPPlanningArgsV1.cuSeqlens.offset, 48)
        self.assertEqual(TileXRMoonEPPlanningArgsV1.flags.offset, 72)
        self.assertEqual(TileXRMoonEPDispatchArgsV1.flags.offset, 56)
        self.assertEqual(TileXRMoonEPDispatchArgsV1.registeredWorkspace.offset, 64)
        self.assertEqual(TileXRMoonEPDispatchArgsV1.registeredWorkspaceBytes.offset, 72)
        self.assertEqual(TileXRMoonEPDispatchArgsV2.registeredWorkspace.offset, 64)
        self.assertEqual(TileXRMoonEPReduceGradWorkspaceInfoV2.rowBytes.offset, 64)
        self.assertEqual(TileXRMoonEPReduceGradWorkspaceInfoV2.chunkCounts.offset, 88)
        self.assertEqual(TileXRMoonEPReduceGradPrepareArgsV2.sources.offset, 48)
        self.assertEqual(TileXRMoonEPReduceGradArgsV2.sources.offset, 48)

    def test_context_enforces_native_planner_expert_count_limit(self):
        common = dict(
            runtime=SimpleNamespace(rank=0, world_size=2),
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
            dtype=FakeTorch().bfloat16,
        )
        context = TileXRMoonEPContext(expert_count=4096, **common)
        self.assertEqual(context.expert_count, 4096)
        with self.assertRaisesRegex(ValueError, r"expert_count must be in \[1, 4096\]"):
            TileXRMoonEPContext(expert_count=4098, **common)

    def test_fake_cdll_receives_stage_descriptors_and_combine_v2_pointers(self):
        loader = FakeCDLLLoader()
        runtime = TileXRMoonEPRuntime(
            rank=0,
            world_size=2,
            library_paths={
                "comm": "libtile-comm.so",
                "planner": "libtilexr-moonep-planner.so",
                "combine_v2": "libtilexr-moonep-combine-v2.so.2",
                "moonep": "libtilexr-moonep.so.1",
            },
            cdll_loader=loader,
        )
        torch = FakeTorch()
        context = TileXRMoonEPContext(
            runtime=runtime,
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
            dtype=torch.bfloat16,
            token_padding=2,
            prefetch_slots=2,
        )
        buffer = TileXRMoonEPBuffer(context, wait_iterations=1234, torch_module=torch)
        hidden_nvsh, route_weights_nvs, cu_seqlens, plan = buffer.dispatch(
            tensor((4, 8), torch.bfloat16),
            tensor((4, 2), torch.float32),
            tensor((4, 2), torch.int32),
            tensor((4,), torch.int32),
        )
        gate = tensor((3, 4, 8), torch.bfloat16).narrow(0, 1, 2)
        up = tensor((3, 4, 8), torch.bfloat16).narrow(0, 1, 2)
        down = tensor((3, 8, 4), torch.bfloat16).narrow(0, 1, 2)
        self.assertGreater(gate.storage_offset(), 0)
        self.assertGreater(up.storage_offset(), 0)
        self.assertGreater(down.storage_offset(), 0)
        projections = ProjectionBuffers.from_local_weights(
            context, gate, up, down, torch_module=torch
        )
        self.assertEqual(projections.backing.data_ptr() % (2 * 1024 * 1024), 0)
        self.assertEqual(
            projections.backing.numel() * projections.backing.element_size(),
            2 * 1024 * 1024,
        )
        buffer.register_projection_buffers(projections)
        plan.status._item = 2004
        with self.assertRaisesRegex(
            RuntimeError, r"actual 2004, expected 0"
        ):
            buffer.dispatch(tensor((4, 8), torch.bfloat16), plan=plan)
        self.assertEqual(plan.status.item(), 2004)
        buffer._pending_statuses[id(plan)] = 3000
        plan.status._item = 3000
        buffer.dispatch(tensor((4, 8), torch.bfloat16), plan=plan)
        self.assertEqual(plan.status.item(), 3000)
        buffer.prefetch_weight(plan, projections)
        hidden_sh, route_weights_sk, _ = buffer.combine(
            plan, hidden_nvsh, route_weights_nvs
        )
        plan.status._item = 4000
        gradients = ProjectionBuffers(
            tensor((6, 2, 4), torch.float32),
            tensor((6, 4, 3), torch.float32),
            tensor((6, 3, 2), torch.float32),
        )
        buffer.reduce_grad(
            plan,
            full_gate_grad=gradients.gate,
            full_up_grad=gradients.up,
            full_down_grad=gradients.down,
        )
        self.assertEqual(tuple(cu_seqlens.shape), (6,))
        plan.status._item = 4000
        plan.reduce_grad_status._item = 0
        buffer.close()

        self.assertEqual(
            [Path(path).name for path, _ in loader.loads],
            [
                "libtile-comm.so",
                "libtilexr-moonep-planner.so",
                "libtilexr-moonep-combine-v2.so.2",
                "libtilexr-moonep.so.1",
            ],
        )
        self.assertEqual(runtime.capabilities.stage_mask, 31)
        self.assertEqual(runtime.capabilities.stub_mask, 0)
        self.assertTrue(runtime.capabilities.transport_correctness_valid)
        self.assertFalse(runtime.capabilities.transport_performance_valid)
        self.assertEqual(loader.destroy_calls, 2)
        self.assertEqual(
            loader.lifecycle_calls,
            [
                ("register", 4 * 1024 * 1024, 7),
                ("register", 2 * 1024 * 1024, 8),
                ("register", 4 * 1024 * 1024, 9),
                ("register", 2 * 1024 * 1024, 10),
                ("register", 4 * 1024 * 1024, 11),
                ("unregister", 11),
                ("destroy", 0x2345),
                ("destroy", 0x1234),
            ],
        )
        self.assertEqual(
            loader.planning_records,
            [{
                "stream": 0xCAFE,
                "workspace_bytes": 256,
                "wait_iterations": 1234,
                "flags": 0,
                "topk_shape": (4, 2),
                "tpe_shape": (4,),
                "cu_shape": (6,),
                "plan": (8, 2, 4, 2, 12, 2),
            }],
        )
        self.assertEqual(plan.dst_local_offset, 128)
        self.assertEqual(
            loader.combine_v2_workspace_records,
            [(4, 8, 2, 12, 11)],
        )
        self.assertEqual(
            [record["name"] for record in loader.stage_records],
            ["dispatch", "dispatch", "prefetch_weight", "combine"],
        )
        self.assertTrue(all(record["stream"] == 0xCAFE for record in loader.stage_records))
        self.assertEqual(loader.stage_records[0]["flags"], 1 << 5)
        self.assertEqual(loader.stage_records[0]["abi_version"], 2)
        self.assertEqual(loader.stage_records[1]["abi_version"], 2)
        self.assertEqual(loader.stage_records[1]["flags"], 1 << 5)
        self.assertTrue(all(record["flags"] == 0 for record in loader.stage_records[2:]))
        self.assertEqual(loader.stage_records[0]["shapes"]["hiddenNvsh"], (12, 8))
        self.assertEqual(loader.stage_records[0]["shapes"]["routeWeightsNvs"], (12,))
        self.assertEqual(loader.stage_records[2]["shapes"]["up"], (4, 4, 8))
        self.assertEqual(loader.stage_records[3]["shapes"]["routeWeightsSk"], (4, 2))
        self.assertEqual(loader.stage_records[3]["aiv_core_num"], 16)
        self.assertEqual(loader.stage_records[3]["dtype"], 11)
        self.assertEqual(loader.stage_records[3]["weight_memory_comm"], 0x2345)
        self.assertEqual(
            loader.stage_records[3]["dst_local"],
            int(plan.workspace.data_ptr()) + 128,
        )
        self.assertEqual(
            loader.stage_records[3]["pointers"],
            {
                "hiddenNvsh": int(hidden_nvsh.data_ptr()),
                "hiddenSh": int(hidden_sh.data_ptr()),
                "routeWeightsNvs": int(route_weights_nvs.data_ptr()),
                "routeWeightsSk": int(route_weights_sk.data_ptr()),
            },
        )
        self.assertEqual(loader.reduce_grad_records, [{
            "stream": 0xCAFE,
            "flags": 0,
            "wait_iterations": 1234,
            "prepared": 0x5678,
            "source_bytes": (64, 96, 48),
            "registration_bytes": (192, 288, 144),
            "shapes": ((6, 2, 4), (6, 4, 3), (6, 3, 2)),
            "status_shape": (1,),
        }])
        self.assertEqual(loader.reduce_grad_prepare_records, [{
            "workspace_bytes": 2 << 20,
            "requested_chunk_bytes": 0,
            "source_bytes": (64, 96, 48),
            "source_ptrs": tuple(
                tensor.data_ptr() + 4 * row_bytes
                for tensor, row_bytes in zip(
                    (gradients.gate, gradients.up, gradients.down),
                    (32, 48, 24),
                )
            ),
            "registration_bytes": (192, 288, 144),
            "registration_ptrs": tuple(
                tensor.data_ptr()
                for tensor in (gradients.gate, gradients.up, gradients.down)
            ),
        }])
        self.assertEqual(loader.reduce_grad_destroy_records, [0x5678])
        self.assertEqual(
            [byte_count for _, byte_count in loader.register_calls],
            [
                4 * 1024 * 1024,
                2 * 1024 * 1024,
                4 * 1024 * 1024,
                2 * 1024 * 1024,
                4 * 1024 * 1024,
            ],
        )
        self.assertEqual(loader.register_calls[0], loader.register_calls[2])
        self.assertEqual(loader.register_calls[1], loader.register_calls[3])
        self.assertEqual(loader.register_calls[0], loader.register_calls[4])
        self.assertEqual(loader.unregister_calls, [11])

    def test_versioned_planner_library_is_preferred(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            library_dir = prefix / "lib"
            library_dir.mkdir()
            (library_dir / "libtilexr-moonep-planner.so").touch()
            versioned_dir = prefix / "lib64"
            versioned_dir.mkdir()
            versioned = versioned_dir / "libtilexr-moonep-planner.so.2"
            versioned.touch()
            resolved = _resolve_library(
                ("libtilexr-moonep-planner.so.2", "libtilexr-moonep-planner.so"),
                "TILEXR_TEST_UNUSED_LIBRARY",
                prefix,
            )
            self.assertEqual(Path(resolved), versioned)


if __name__ == "__main__":
    unittest.main(verbosity=2)
