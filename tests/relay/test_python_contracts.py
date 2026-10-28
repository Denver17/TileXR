"""Host-only contract tests; no TileLang, TVM, framework or NPU needed."""

import ctypes as C
import dataclasses
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src/tilelang/python"))

from tilexr_relay.contracts import (Binding, BindingSpec, Channel, Initialization,
                                    KernelArtifact, PeerMemoryLayout, Region, RegionSpace,
                                    Slot, Source)
from tilexr_relay.planning import PlanBuilder, RelayPlan
from tilexr_relay.runtime import RelayError, RelayRuntime, _PlanDesc
from tilexr_relay.ops.plans import allgather_gemm_plan, gemm_allreduce_plan, peer_layout


def artifact(**changes):
    values = dict(operator_id="test.echo", specialization={"rows": 128},
                  target="Ascend910B", backend="peer_memory",
                  compiler={"revision": "test", "options": ["O2", "auto_sync"]},
                  sha256="a" * 64)
    values.update(changes)
    return KernelArtifact(**values)


def plan(**changes):
    builder = PlanBuilder(workspace_bytes=128, peer_payload_bytes=128)
    builder.add_binding(BindingSpec(64, 64, 2, 3))
    builder.add_region(Region(0, 128, 64, initialization=Initialization.ZERO_ONCE))
    builder.add_region(Region(0, 128, 64, RegionSpace.PAYLOAD))
    builder.add_channel(Channel(0, 2))
    builder.bind_slot(0, Slot(Source.USER, 0, 0, 64, 64))
    builder.bind_slot(1, Slot(Source.WORKSPACE, 0, 0, 128, 64))
    builder.bind_slot(2, Slot(Source.OWNER_PAYLOAD, 1, 0, 128, 64))
    builder.bind_slot(3, Slot(Source.PEER_PAYLOAD, 1, 0, 128, 64))
    builder.bind_slot(4, Slot(Source.OWNER_FLAGS, 0, 0, 2 << 20, 64))
    builder.bind_slot(5, Slot(Source.PEER_FLAGS, 0, 0, 2 << 20, 64))
    return dataclasses.replace(builder.build(artifact(peer_layout=PeerMemoryLayout(2, 4, 5)), 1),
                               **changes)


class Function:
    def __init__(self, callback):
        self.callback = callback

    def __call__(self, *args):
        return self.callback(*args)


class Library:
    def __init__(self):
        self.calls = 0
        self.launch_error = 0
        self.destroy_calls = 0
        self.TileXRRelayPlanCreate = Function(self.create)
        self.TileXRRelayPlanGetWorkspaceSize = Function(self.size)
        self.TileXRRelayLaunch = Function(self.launch)
        self.TileXRRelayPlanDestroy = Function(self.destroy)

    def create(self, description, handle):
        self.description = C.cast(description, C.POINTER(_PlanDesc)).contents
        C.cast(handle, C.POINTER(C.c_void_p))[0] = C.c_void_p(1)
        return 0

    def size(self, _, output):
        C.cast(output, C.POINTER(C.c_uint64))[0] = 128
        return 0

    def launch(self, *args):
        self.calls += 1
        self.last_args = args
        return self.launch_error

    def destroy(self, _):
        self.destroy_calls += 1
        return 0


class ExecutionLibrary(Library):
    def __init__(self):
        super().__init__()
        self.execution_create_calls = 0
        self.execution_launch_calls = 0
        self.execution_destroy_calls = 0
        self.execution_create_error = 0
        self.execution_launch_error = 0
        self.execution_destroy_error = 0
        self.TileXRRelayExecutionCreate = Function(self.execution_create)
        self.TileXRRelayExecutionLaunch = Function(self.execution_launch)
        self.TileXRRelayExecutionDestroy = Function(self.execution_destroy)

    def execution_create(self, plan, comm, bindings, count, workspace, capacity, output):
        self.execution_create_calls += 1
        if self.execution_create_error:
            return self.execution_create_error
        C.cast(output, C.POINTER(C.c_void_p))[0] = 100 + self.execution_create_calls
        return 0

    def execution_launch(self, handle, stream):
        self.execution_launch_calls += 1
        return self.execution_launch_error

    def execution_destroy(self, handle):
        self.execution_destroy_calls += 1
        return self.execution_destroy_error


class ContractTests(unittest.TestCase):
    def runtime_plan(self, library=None):
        if library is None:
            library = Library()
        with patch("tilexr_relay.runtime.C.CDLL", return_value=library):
            handle = RelayRuntime("mock").create_plan(plan())
        return library, handle

    def test_immutable_provenance_and_canonical_roundtrip(self):
        compiler = {"revision": "old", "options": ["O2"]}
        value = artifact(compiler=compiler, peer_layout=PeerMemoryLayout(2, 4, 5))
        compiler["options"].append("changed")
        self.assertEqual(value.compiler["options"], ("O2",))
        with self.assertRaises(TypeError):
            value.specialization["rows"] = 1
        original = plan(artifact=value)
        self.assertEqual(original.to_json(), RelayPlan.from_json(original.to_json()).to_json())
        with self.assertRaises(dataclasses.FrozenInstanceError):
            original.blocks = 2
        with self.assertRaises(dataclasses.FrozenInstanceError):
            value.peer_layout.slot_count = 1

    def test_identity_includes_provenance(self):
        original = artifact()
        for change in (dict(operator_id="test.other"), dict(specialization={"rows": 256}),
                       dict(compiler={"revision": "other"}), dict(measurement_mode="diagnostic"),
                       dict(sha256="b" * 64), dict(peer_layout=PeerMemoryLayout(2, 4, 5))):
            self.assertNotEqual(original.identity, artifact(**change).identity)
        self.assertNotEqual(artifact(peer_layout=PeerMemoryLayout(2, 4, 5)).identity,
                            artifact(peer_layout=PeerMemoryLayout(1, 4, 5)).identity)
        self.assertNotEqual(plan().identity, plan(blocks=2).identity)

    def test_versions_and_malformed_manifest(self):
        for fields in (dict(abi_version=0), dict(protocol_version=2), dict(abi_version=True),
                       dict(target="Ascend950"), dict(backend="udma"), dict(entry="other"),
                       dict(sha256="../"), dict(sha256="A" * 64)):
            with self.assertRaises(ValueError):
                artifact(**fields)
        for change in (dict(version=2), dict(version=True), dict(unexpected=1), dict(slots=[]),
                       dict(artifact=[])):
            data = plan().to_dict()
            data.update(change)
            with self.assertRaises(ValueError):
                RelayPlan.from_json(json.dumps(data))

    def test_checked_sizes_alignment_and_slots(self):
        for size in (-1, 0, 1 << 64, True):
            with self.assertRaises(ValueError):
                BindingSpec(size)
        for align in (0, 16, 48, 8192):
            with self.assertRaises(ValueError):
                BindingSpec(64, align)
        with self.assertRaises(ValueError):
            Region((1 << 64) - 64, 64, 64)
        with self.assertRaises(ValueError):
            Slot(Source.UNUSED, bytes=64)
        with self.assertRaises(ValueError):
            Slot(7, bytes=64, alignment=64)
        with self.assertRaises(ValueError):
            plan(peer_payload_bytes=(1 << 64) - 1)
        with self.assertRaises(ValueError):
            plan(slots=(Slot(),) * 15)
        with self.assertRaises(ValueError):
            plan(binding_specs=(BindingSpec(64),) * 17)

    def test_regions_and_channel_capacity(self):
        for regions in ((Region(0, 128), Region(64, 64)), (Region(0, 256),)):
            with self.assertRaises(ValueError):
                plan(regions=regions)
        for arguments in ((16384, 1), (0, 0), (0, 1, 3), (0, 1, 2, 2)):
            with self.assertRaises(ValueError):
                Channel(*arguments)
        Channel(16383, 1)
        with self.assertRaises(ValueError):
            Region(0, 64, 64, RegionSpace.PAYLOAD, Initialization.ZERO_ONCE)

    def test_plan_channels_must_match_compiled_layout(self):
        original = plan()
        for channels in ((Channel(100, 1),), (Channel(0, 1),),
                         (Channel(0, 2), Channel(100, 1)), ()):
            with self.subTest(channels=channels):
                with self.assertRaises(ValueError):
                    dataclasses.replace(original, channels=channels)
                data = original.to_dict()
                data["channels"] = [dataclasses.asdict(c) for c in channels]
                with self.assertRaises(ValueError):
                    RelayPlan.from_json(json.dumps(data))

    def test_plan_flag_bindings_must_match_compiled_layout(self):
        original = plan()
        own, peer = original.slots[4:6]
        for replacements in ({4: dataclasses.replace(own, offset=64, bytes=own.bytes - 64)},
                             {4: peer, 5: own}, {4: Slot()}, {9: own}):
            with self.subTest(replacements=replacements):
                slots = list(original.slots)
                for index, slot in replacements.items():
                    slots[index] = slot
                with self.assertRaises(ValueError):
                    dataclasses.replace(original, slots=tuple(slots))
                data = original.to_dict()
                data["slots"] = [dataclasses.asdict(s) for s in slots]
                with self.assertRaises(ValueError):
                    RelayPlan.from_json(json.dumps(data))

    def test_missing_layout_requires_regeneration_only_for_communication(self):
        data = plan().to_dict()
        del data["artifact"]["peer_layout"]
        with self.assertRaisesRegex(ValueError, "regenerate"):
            RelayPlan.from_json(json.dumps(data))
        original = PlanBuilder().build(artifact(), 1)
        data = original.to_dict()
        del data["artifact"]["peer_layout"]
        self.assertEqual(RelayPlan.from_json(json.dumps(data)), original)
        for changes in (dict(channels=(Channel(0, 1),)),
                        dict(slots=(Slot(Source.OWNER_FLAGS, bytes=64, alignment=64),) +
                             (Slot(),) * 15)):
            with self.assertRaises(ValueError):
                dataclasses.replace(original, **changes)

    def test_source_capacity_and_space(self):
        for slot in (Slot(Source.USER, 1, 0, 64, 64),
                     Slot(Source.USER, 0, 0, 128, 64),
                     Slot(Source.WORKSPACE, 1, 0, 64, 64)):
            original = plan()
            with self.assertRaises(ValueError):
                dataclasses.replace(original, slots=(slot,) + original.slots[1:])

    def test_operator_rejects_unsupported_resources_before_compile(self):
        for planner, operator in ((allgather_gemm_plan, "allgather_gemm"),
                                  (gemm_allreduce_plan, "gemm_allreduce")):
            for shape in ((0, 256, 64, 1), (129, 256, 64, 1), (128, 257, 64, 1),
                          (128, 256, 65, 1), (128, 256, 64, 0), (True, 256, 64, 1)):
                with self.assertRaises(ValueError):
                    planner(artifact(operator_id=operator,
                                     specialization=dict(zip(("rows", "n", "k", "blocks"), shape))))
        with self.assertRaises(ValueError):
            allgather_gemm_plan(artifact(operator_id="allgather_gemm",
                                        specialization=dict(rows=128, n=768, k=64, blocks=4),
                                        peer_layout=peer_layout("allgather_gemm", 128, 768, 64, 4)))
        with self.assertRaises(ValueError):
            gemm_allreduce_plan(artifact(operator_id="gemm_allreduce",
                                        specialization=dict(rows=1048576, n=4096, k=64, blocks=1)))

    def test_operator_artifact_is_the_only_shape_source(self):
        shape = dict(rows=256, n=512, k=128, blocks=2)
        for planner, operator, sizes, workspace, payload in (
                (allgather_gemm_plan, "allgather_gemm", (65536, 131072, 131072, 524288), 128, 65536),
                (gemm_allreduce_plan, "gemm_allreduce", (65536, 131072, 262144), 0, 524288)):
            with self.subTest(operator=operator):
                layout = peer_layout(operator, **shape)
                self.assertEqual(layout, PeerMemoryLayout(2, 6, 7) if operator == "allgather_gemm"
                                 else PeerMemoryLayout(4, 5, 6))
                value = artifact(operator_id=operator, specialization=shape, peer_layout=layout)
                result = planner(value)
                self.assertEqual(tuple(b.bytes for b in result.binding_specs), sizes)
                self.assertEqual((result.blocks, result.workspace_bytes, result.peer_payload_bytes),
                                 (shape["blocks"], workspace, payload))
                self.assertIs(result.artifact, value)
                self.assertEqual(result.channels, (layout.channel,))
                self.assertEqual({i: s for i, s in enumerate(result.slots)
                                  if s.source in (Source.OWNER_FLAGS, Source.PEER_FLAGS)},
                                 layout.flag_bindings)
                self.assertEqual(RelayPlan.from_json(result.to_json()), result)
                with self.assertRaises(TypeError):
                    planner(value, 128, 512, 128, 2)

    def test_operator_rejects_wrong_artifact_and_missing_specialization(self):
        shape = dict(rows=256, n=512, k=128, blocks=2)
        for planner, operator, other in (
                (allgather_gemm_plan, "allgather_gemm", "gemm_allreduce"),
                (gemm_allreduce_plan, "gemm_allreduce", "allgather_gemm")):
            with self.subTest(operator=operator):
                with self.assertRaisesRegex(ValueError, "operator"):
                    planner(artifact(operator_id=other, specialization=shape))
                for missing in shape:
                    with self.subTest(missing=missing), self.assertRaises(ValueError):
                        planner(artifact(operator_id=operator,
                                         specialization={k: v for k, v in shape.items() if k != missing}))

    def test_operator_rejects_layout_inconsistent_with_specialization_or_slots(self):
        shape = dict(rows=256, n=512, k=128, blocks=2)
        for planner, operator in ((allgather_gemm_plan, "allgather_gemm"),
                                  (gemm_allreduce_plan, "gemm_allreduce")):
            expected = peer_layout(operator, **shape)
            for layout in (None, dataclasses.replace(expected, slot_count=1),
                           dataclasses.replace(expected, owner_slot=expected.peer_slot,
                                               peer_slot=expected.owner_slot)):
                with self.subTest(operator=operator, layout=layout), self.assertRaises(ValueError):
                    planner(artifact(operator_id=operator, specialization=shape, peer_layout=layout))

    def test_builder_snapshot_and_slot_reassignment(self):
        builder = PlanBuilder()
        index = builder.add_binding(BindingSpec(64))
        builder.bind_slot(0, Slot(Source.USER, index, 0, 64, 32))
        original = builder.build(artifact(), 1)
        builder.add_binding(BindingSpec(128))
        self.assertEqual(len(original.binding_specs), 1)
        with self.assertRaises(ValueError):
            builder.bind_slot(0, Slot())

    def test_runtime_signatures_bindings_and_poison(self):
        library = Library()
        with patch("tilexr_relay.runtime.C.CDLL", return_value=library):
            runtime = RelayRuntime("mock")
        self.assertEqual(len(library.TileXRRelayLaunch.argtypes), 7)
        handle = runtime.create_plan(plan())
        self.assertEqual(handle.workspace_bytes, 128)
        for binding in (Binding(0x1000, 32, 2, 3), Binding(0x1000, 64, 1, 3),
                        Binding(0x1001, 64, 2, 3), Binding(0x2000, 64, 2, 3)):
            with self.assertRaises(ValueError):
                handle.launch(1, [binding], 0x2000, 128, 2)
        self.assertEqual(library.calls, 0)
        self.assertEqual(handle.state, "ready")
        handle.launch(1, [Binding(0x1000, 64, 2, 3)], 0x2000, 128, 2)
        self.assertEqual(library.calls, 1)
        library.launch_error = 507035
        with self.assertRaises(RelayError) as caught:
            handle.launch(1, [Binding(0x1000, 64, 2, 3)], 0x2000, 128, 2)
        self.assertEqual(caught.exception.code, 507035)
        self.assertTrue(caught.exception.poisoned)
        with self.assertRaises(RuntimeError):
            handle.launch(1, [], stream=2)
        self.assertEqual(library.calls, 2)
        handle.destroy()
        handle.destroy()
        self.assertEqual(library.destroy_calls, 1)
        with self.assertRaises(RuntimeError):
            handle.workspace_bytes

    def test_runtime_rejects_races_and_null_stream(self):
        library = Library()
        with patch("tilexr_relay.runtime.C.CDLL", return_value=library):
            handle = RelayRuntime("mock").create_plan(plan())
        with self.assertRaises(ValueError):
            handle.launch(1, [Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        handle._lock.acquire()
        try:
            with self.assertRaises(RuntimeError):
                handle.destroy()
        finally:
            handle._lock.release()
        handle.destroy()

    def test_native_first_error_propagation(self):
        library = Library()
        with patch("tilexr_relay.runtime.C.CDLL", return_value=library):
            runtime = RelayRuntime("mock")
        library.TileXRRelayPlanCreate.callback = lambda *_: 17
        with self.assertRaises(RelayError) as caught:
            runtime.create_plan(plan())
        self.assertEqual(caught.exception.code, 17)
        self.assertEqual(library.calls, 0)
        library.TileXRRelayPlanCreate.callback = library.create
        handle = runtime.create_plan(plan())
        library.TileXRRelayPlanGetWorkspaceSize.callback = lambda *_: 18
        with self.assertRaises(RelayError) as caught:
            handle.workspace_bytes
        self.assertEqual(caught.exception.code, 18)
        self.assertEqual(handle.state, "ready")
        library.TileXRRelayPlanDestroy.callback = lambda *_: 19
        with self.assertRaises(RelayError) as caught:
            handle.destroy()
        self.assertEqual(caught.exception.code, 19)
        self.assertEqual(handle.state, "ready")
        library.TileXRRelayPlanDestroy.callback = library.destroy
        handle.destroy()

    def test_context_cleanup_preserves_first_error(self):
        library = Library()
        with patch("tilexr_relay.runtime.C.CDLL", return_value=library):
            handle = RelayRuntime("mock").create_plan(plan())
        library.launch_error = 507035
        library.TileXRRelayPlanDestroy.callback = lambda *_: 19
        with self.assertRaises(RelayError) as caught:
            with handle:
                handle.launch(1, [Binding(0x1000, 64, 2, 3)], 0x2000, 128, 2)
        self.assertEqual(caught.exception.code, 507035)
        self.assertEqual(caught.exception.relay_cleanup_error.code, 19)

    def test_prepare_validates_without_launching(self):
        library, handle = self.runtime_plan()
        valid = Binding(0x1000, 64, 2, 3)
        for bindings, workspace, capacity in (
                ([], 0x2000, 128), ([Binding(0x1000, 32, 2, 3)], 0x2000, 128),
                ([Binding(0x1000, 64, 1, 3)], 0x2000, 128),
                ([Binding(0x1001, 64, 2, 3)], 0x2000, 128),
                ([valid], 0x1000, 128), ([valid], 0x2000, 64),
                ([valid], 0, 128), ([valid], 0x2001, 128),
                ([valid], True, 128), ([valid], 0x2000, True)):
            with self.assertRaises(ValueError):
                handle.prepare_bindings(bindings, workspace, capacity)
        self.assertEqual(library.calls, 0)
        self.assertEqual(handle.state, "ready")
        handle.destroy()

    def test_prepared_owner_lifetime_and_dynamic_validation(self):
        library, handle = self.runtime_plan()
        _, other = self.runtime_plan()
        prepared = handle.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        for comm, value, stream in ((0, prepared, 2), (1, prepared, 0),
                                    (True, prepared, 2), (1, prepared, True),
                                    (1, object(), 2)):
            with self.assertRaises(ValueError):
                handle.launch_prepared(comm, value, stream)
        with self.assertRaises(ValueError):
            other.launch_prepared(1, prepared, 2)
        self.assertEqual(library.calls, 0)
        self.assertEqual(handle.state, "ready")
        handle.destroy()
        for action in (lambda: handle.launch_prepared(1, prepared, 2),
                       lambda: handle.prepare_bindings([], 0, 0)):
            with self.assertRaises(RuntimeError):
                action()
        other.destroy()

    def test_prepared_poison_error_and_race_semantics(self):
        library, handle = self.runtime_plan()
        prepared = handle.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        handle._lock.acquire()
        try:
            for action in (lambda: handle.launch_prepared(1, prepared, 2),
                           lambda: handle.prepare_bindings([], 0, 0)):
                with self.assertRaises(RuntimeError):
                    action()
        finally:
            handle._lock.release()
        self.assertEqual(library.calls, 0)
        library.launch_error = 507035
        with self.assertRaises(RelayError) as caught:
            handle.launch_prepared(1, prepared, 2)
        self.assertEqual(caught.exception.code, 507035)
        self.assertTrue(caught.exception.poisoned)
        self.assertEqual(handle.state, "poisoned")
        for action in (lambda: handle.launch_prepared(1, prepared, 2),
                       lambda: handle.launch(1, prepared.bindings, 0x2000, 128, 2),
                       lambda: handle.prepare_bindings([], 0, 0)):
            with self.assertRaises(RuntimeError):
                action()
        self.assertEqual(library.calls, 1)
        handle.destroy()

    def test_prepared_native_exception_poisoning(self):
        library, handle = self.runtime_plan()
        prepared = handle.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        failure = RuntimeError("native adapter failure")
        def fail(*_):
            raise failure
        library.TileXRRelayLaunch.callback = fail
        with self.assertRaises(RuntimeError) as caught:
            handle.launch_prepared(1, prepared, 2)
        self.assertIs(caught.exception, failure)
        self.assertEqual(handle.state, "poisoned")
        handle.destroy()

    def test_execution_symbols_lazy_and_older_dso_compatible(self):
        library, parent = self.runtime_plan()
        self.assertIsNone(parent.runtime._execution_api)
        prepared = parent.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        parent.launch_prepared(1, prepared, 2)
        with self.assertRaisesRegex(RuntimeError, "does not support prepared execution"):
            parent.prepare_execution(1, prepared)
        self.assertIsNone(parent.runtime._execution_api)
        self.assertEqual(parent.state, "ready")
        parent.launch(1, prepared.bindings, 0x2000, 128, 2)
        self.assertEqual(library.calls, 2)
        parent.destroy()

    def test_execution_create_validation_and_failure_do_not_poison(self):
        library, parent = self.runtime_plan(ExecutionLibrary())
        _, other = self.runtime_plan()
        prepared = parent.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        other_prepared = other.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        for comm, values in ((0, prepared), (True, prepared), (1, object()), (1, other_prepared)):
            with self.assertRaises(ValueError):
                parent.prepare_execution(comm, values)
        self.assertEqual(library.execution_create_calls, 0)
        library.execution_create_error = 17
        with self.assertRaises(RelayError) as caught:
            parent.prepare_execution(1, prepared)
        self.assertEqual((caught.exception.operation, caught.exception.code),
                         ("TileXRRelayExecutionCreate", 17))
        self.assertFalse(caught.exception.poisoned)
        self.assertEqual(parent.state, "ready")
        self.assertEqual((library.calls, library.execution_launch_calls), (0, 0))
        parent.destroy()
        other.destroy()

    def test_execution_locks_and_invalid_stream_prevent_native_calls(self):
        library, parent = self.runtime_plan(ExecutionLibrary())
        prepared = parent.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        execution = parent.prepare_execution(1, prepared)
        for stream in (0, True, -1, 1 << 64):
            with self.assertRaises(ValueError):
                execution.launch(stream)
        parent._lock.acquire()
        try:
            for action in (lambda: parent.prepare_execution(1, prepared),
                           lambda: execution.launch(2), lambda: execution.destroy(),
                           lambda: execution.__enter__()):
                with self.assertRaisesRegex(RuntimeError, "concurrent"):
                    action()
        finally:
            parent._lock.release()
        self.assertEqual((library.execution_create_calls, library.execution_launch_calls,
                          library.execution_destroy_calls), (1, 0, 0))
        self.assertEqual(parent.state, "ready")
        with execution:
            execution.launch(2)
        self.assertEqual(execution.state, "destroyed")
        parent.destroy()

    def test_execution_error_poison_shared_parent_siblings_and_old_paths(self):
        library, parent = self.runtime_plan(ExecutionLibrary())
        prepared = parent.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        first = parent.prepare_execution(1, prepared)
        sibling = parent.prepare_execution(1, prepared)
        library.execution_launch_error = 507035
        with self.assertRaises(RelayError) as caught:
            first.launch(2)
        self.assertEqual((caught.exception.operation, caught.exception.code),
                         ("TileXRRelayExecutionLaunch", 507035))
        self.assertTrue(caught.exception.poisoned)
        self.assertEqual((parent.state, first.state, sibling.state), ("poisoned",) * 3)
        for action in (lambda: sibling.launch(2), lambda: parent.launch_prepared(1, prepared, 2),
                       lambda: parent.prepare_execution(1, prepared), lambda: first.__enter__()):
            with self.assertRaisesRegex(RuntimeError, "poisoned"):
                action()
        self.assertEqual(library.execution_launch_calls, 1)
        first.destroy()
        sibling.destroy()
        parent.destroy()

    def test_old_path_error_poison_execution(self):
        library, parent = self.runtime_plan(ExecutionLibrary())
        prepared = parent.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        execution = parent.prepare_execution(1, prepared)
        library.launch_error = 507035
        with self.assertRaises(RelayError):
            parent.launch_prepared(1, prepared, 2)
        with self.assertRaisesRegex(RuntimeError, "poisoned"):
            execution.launch(2)
        self.assertEqual(library.execution_launch_calls, 0)
        execution.destroy()
        parent.destroy()

    def test_execution_cleanup_preserves_first_error_and_retries_host_destroy(self):
        library, parent = self.runtime_plan(ExecutionLibrary())
        prepared = parent.prepare_bindings([Binding(0x1000, 64, 2, 3)], 0x2000, 128)
        execution = parent.prepare_execution(1, prepared)
        library.execution_launch_error = 507035
        library.execution_destroy_error = 19
        with self.assertRaises(RelayError) as caught:
            with execution:
                execution.launch(2)
        self.assertEqual(caught.exception.code, 507035)
        self.assertEqual(caught.exception.relay_cleanup_error.code, 19)
        self.assertEqual(execution.state, "poisoned")
        library.execution_destroy_error = 0
        execution.destroy()
        self.assertEqual(execution.state, "destroyed")
        parent.destroy()

    def test_public_runtime_exports_are_compiler_free(self):
        source = "import sys; sys.modules.update({k: None for k in ('tilelang','tvm','torch')}); "
        source += "from tilexr_relay import RelayRuntime, RuntimePlan, RuntimeExecution, PreparedBindings"
        environment = dict(os.environ, PYTHONPATH=str(ROOT / "src/tilelang/python"))
        subprocess.run([sys.executable, "-c", source], env=environment, check=True,
                       capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
