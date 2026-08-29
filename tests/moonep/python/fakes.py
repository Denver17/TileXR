from __future__ import annotations

import math
import sys
import threading
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
INTEGRATION = ROOT / 'integrations' / 'moonep_torch'
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

from tilexr_moonep.runtime import NativeCapabilities


@dataclass(frozen=True)
class FakeDevice:
    type: str = "npu"
    index: int | None = 0

    def __str__(self) -> str:
        return self.type if self.index is None else f"{self.type}:{self.index}"


class FakeTensor:
    _next_ptr = 0x1000

    def __init__(
        self,
        shape,
        dtype,
        device="npu:0",
        *,
        contiguous=True,
        storage_offset=0,
    ):
        self.shape = tuple(int(value) for value in shape)
        self.dtype = dtype
        if isinstance(device, FakeDevice):
            self.device = device
        else:
            kind, _, index = str(device).partition(":")
            self.device = FakeDevice(kind, int(index) if index else None)
        self._contiguous = bool(contiguous)
        self._storage_offset = int(storage_offset)
        self._item = 0
        self.masked_fill_calls = []
        self.copy_calls = []
        self.zero_calls = []
        allocation_bytes = max(512, self.numel() * self.element_size())
        self._ptr = (FakeTensor._next_ptr + 511) // 512 * 512
        FakeTensor._next_ptr = self._ptr + (allocation_bytes + 511) // 512 * 512

    def is_contiguous(self):
        return self._contiguous

    def storage_offset(self):
        return self._storage_offset

    def numel(self):
        return math.prod(self.shape)

    def element_size(self):
        return {
            "uint8": 1,
            "float16": 2,
            "bfloat16": 2,
            "int32": 4,
            "float32": 4,
            "int64": 8,
        }[self.dtype]

    def data_ptr(self):
        return self._ptr

    def reshape(self, *shape):
        if len(shape) == 1 and isinstance(shape[0], tuple):
            shape = shape[0]
        result = FakeTensor(shape, self.dtype, self.device,
            storage_offset=self._storage_offset)
        result._ptr = self._ptr
        result._base = getattr(self, "_base", self)
        result.masked_fill_calls = self.masked_fill_calls
        result.copy_calls = self.copy_calls
        result.zero_calls = self.zero_calls
        return result

    def view(self, *args):
        if len(args) == 1 and isinstance(args[0], str):
            dtype = args[0]
            source_bytes = self.numel() * self.element_size()
            target_bytes = {
                "uint8": 1,
                "float16": 2,
                "bfloat16": 2,
                "int32": 4,
                "float32": 4,
                "int64": 8,
            }[dtype]
            if source_bytes % target_bytes:
                raise ValueError("FakeTensor.view dtype has incompatible byte size")
            result = FakeTensor(
                (source_bytes // target_bytes,), dtype, self.device,
                storage_offset=self._storage_offset,
            )
            result._ptr = self._ptr
            result._base = getattr(self, "_base", self)
            return result
        return self.reshape(*args)

    def narrow(self, dim, start, length):
        dim = int(dim)
        start = int(start)
        length = int(length)
        if dim < 0 or dim >= len(self.shape):
            raise ValueError("FakeTensor.narrow dimension is out of bounds")
        if start < 0 or length < 0 or start + length > self.shape[dim]:
            raise ValueError("FakeTensor.narrow range is out of bounds")
        shape = list(self.shape)
        shape[dim] = length
        stride = math.prod(self.shape[dim + 1:])
        result = FakeTensor(
            shape,
            self.dtype,
            self.device,
            storage_offset=self._storage_offset + start * stride,
        )
        result._ptr = self._ptr + start * stride * self.element_size()
        result._base = getattr(self, "_base", self)
        result.masked_fill_calls = self.masked_fill_calls
        result.copy_calls = self.copy_calls
        result.zero_calls = self.zero_calls
        return result

    def __getitem__(self, item):
        if isinstance(item, int):
            index = item + self.shape[0] if item < 0 else item
            if index < 0 or index >= self.shape[0]:
                raise IndexError("FakeTensor index is out of bounds")
            result = self.narrow(0, index, 1)
            result.shape = self.shape[1:]
            return result
        if not isinstance(item, slice):
            raise TypeError("FakeTensor only supports integer or slice indexing on dim 0")
        start, stop, step = item.indices(self.shape[0])
        if step != 1:
            raise TypeError("FakeTensor slice step must be one")
        return self.narrow(0, start, stop - start)

    def clone(self):
        return FakeTensor(self.shape, self.dtype, self.device)

    def to(self, *, dtype):
        return FakeTensor(self.shape, dtype, self.device)

    def clamp_min(self, value):
        return FakeTensor(self.shape, self.dtype, self.device)

    def index_select(self, dim, index):
        if int(dim) != 0:
            raise ValueError("FakeTensor.index_select supports only dim 0")
        return FakeTensor((index.shape[0], *self.shape[1:]), self.dtype, self.device)

    def item(self):
        return self._item

    def __mul__(self, other):
        return FakeTensor(self.shape, self.dtype, self.device)

    __rmul__ = __mul__

    def __ge__(self, other):
        return FakeTensor(self.shape, "uint8", self.device)

    def masked_fill_(self, mask, value):
        self.masked_fill_calls.append((mask, value))
        return self

    def contiguous(self):
        return self

    def copy_(self, other):
        if not isinstance(other, FakeTensor):
            raise TypeError("FakeTensor.copy_ requires another FakeTensor")
        if self.shape != other.shape or self.dtype != other.dtype or self.device != other.device:
            raise ValueError("FakeTensor.copy_ source and destination must match")
        self.copy_calls.append((self.data_ptr(), other.data_ptr(), self.shape))
        self._item = other._item
        return self

    def zero_(self):
        self._item = 0
        self.zero_calls.append((self.data_ptr(), self.shape))
        return self

    def fill_(self, value):
        self._item = value
        return self


class FakeEvent:
    def __init__(self, stream):
        self.stream = int(stream)
        self.wait_calls = []

    def wait(self, stream):
        self.wait_calls.append(stream)

    def synchronize(self):
        self.wait_calls.append("synchronize")

    def __eq__(self, other):
        return other == ("event", self.stream)


class FakeStream:
    def __init__(self, value=0xCAFE):
        self.npu_stream = int(value)

    def record_event(self):
        return FakeEvent(self.npu_stream)


class FakeNpu:
    def __init__(self, current_device=0):
        self._current_device = int(current_device)
        self._stream = FakeStream()
        self.synchronize_calls = []

    def current_device(self):
        return self._current_device

    def current_stream(self):
        return self._stream

    def synchronize(self, device=None):
        self.synchronize_calls.append(device)


class FakeTorch:
    uint8 = "uint8"
    int32 = "int32"
    int64 = "int64"
    float16 = "float16"
    bfloat16 = "bfloat16"
    float32 = "float32"

    def __init__(self, current_device=0):
        self.npu = FakeNpu(current_device)

    @staticmethod
    def empty(shape, *, dtype, device):
        return FakeTensor(shape, dtype, device)

    @staticmethod
    def zeros(shape, *, dtype, device):
        value = FakeTensor(shape, dtype, device)
        value._item = 0
        return value

    @staticmethod
    def where(condition, input, other):
        if input.shape != other.shape:
            raise ValueError("FakeTorch.where input shapes must match")
        return FakeTensor(input.shape, input.dtype, input.device)


class FakeRuntime:
    def __init__(
        self,
        rank=0,
        world_size=2,
        *,
        write_status_markers=False,
        fail_dispatch_calls=0,
        udma_qp_count=1,
    ):
        self.rank = rank
        self.world_size = world_size
        self.write_status_markers = bool(write_status_markers)
        self.fail_dispatch_calls = int(fail_dispatch_calls)
        self._udma_qp_count = int(udma_qp_count)
        self.calls = []
        self.closed = False
        self.udma_handles = []
        self._projection_handles = {}
        self._active_udma_owner = None
        self._active_projection = None
        self.capabilities = NativeCapabilities(
            abi_version=2,
            stage_mask=31,
            stub_mask=0,
        )
        self.registered_workspace = None
        self.reduce_grad_prepared = None
        self._next_reduce_grad_prepared = 1
        self.reduce_grad_query_calls = 0
        self._reduce_grad_lock = threading.RLock()
        self._reduce_grad_owner_token = None

    @property
    def udma_qp_count(self):
        return self._udma_qp_count

    def _acquire_reduce_grad(self, owner_token):
        with self._reduce_grad_lock:
            if self.closed:
                raise RuntimeError("cannot launch ReduceGrad on a closed TileXR MoonEP runtime")
            if self._reduce_grad_owner_token is not None:
                raise RuntimeError(
                    "ReduceGrad is already in flight on this TileXR MoonEP runtime; "
                    "synchronize the owning buffer before launching another ReduceGrad"
                )
            self._reduce_grad_owner_token = owner_token

    def _release_reduce_grad(self, owner_token):
        with self._reduce_grad_lock:
            if self._reduce_grad_owner_token is not owner_token:
                raise RuntimeError("ReduceGrad owner token mismatch")
            self._reduce_grad_owner_token = None

    def planning_workspace_size(self, context):
        self.calls.append(("planning_workspace_size", None))
        return 64 + int(context.nv_s) * 4

    def planning_dst_local_offset(self, context):
        self.calls.append(("planning_dst_local_offset", None))
        return 64

    def dispatch_workspace_size(self, context):
        return 2 * 1024 * 1024, 2 * 1024 * 1024

    def register_dispatch_workspace(self, pointer, size):
        self._active_udma_owner = "dispatch"
        self._active_projection = None
        return 7 if self.world_size > 1 else None

    def unregister_dispatch_workspace(self, handle):
        if handle is not None and self._active_udma_owner == "dispatch":
            self.calls.append(("udma_unregister", int(handle)))
            self._active_udma_owner = None
        return None

    def planning(self, context, topk, tpe, plan, cu_seqlens, stream, wait_iterations):
        self.calls.append(("planning", plan, stream, wait_iterations))
        if self.write_status_markers:
            plan.status._item = 0

    def dispatch(
        self,
        context,
        plan,
        hidden,
        output,
        stream,
        route_weights=None,
        output_route_weights=None,
        *,
        build_dedup,
        inter_rank_sync,
        registered_workspace=None,
        registered_workspace_bytes=0,
    ):
        self.calls.append(
            (
                "dispatch",
                plan,
                hidden,
                output,
                route_weights,
                output_route_weights,
                stream,
                build_dedup,
                inter_rank_sync,
            )
        )
        if self.fail_dispatch_calls > 0:
            self.fail_dispatch_calls -= 1
            raise RuntimeError("fake dispatch enqueue failed")
        if self.write_status_markers:
            plan.status._item = 0

    def prefetch_weight(self, context, plan, projections, stream):
        self.calls.append(("prefetch_weight", plan, projections, stream))
        if self.write_status_markers:
            plan.status._item = 4000

    def udma_register(self, tensor):
        key = id(tensor)
        if self._active_udma_owner == "projection" and self._active_projection == key:
            return self._projection_handles[key]
        self.calls.append(("udma_register", tensor))
        handle = self._projection_handles.get(key)
        if handle is None:
            handle = len(self._projection_handles) + 1
            self._projection_handles[key] = handle
            self.udma_handles.append(handle)
        self._active_udma_owner = "projection"
        self._active_projection = key
        return handle

    def udma_unregister(self, handle):
        if self._active_udma_owner == "projection":
            self.calls.append(("udma_unregister", int(handle)))
            self._active_udma_owner = None
            self._active_projection = None

    def combine(
        self,
        context,
        plan,
        hidden,
        output,
        stream,
        route_weights=None,
        output_route_weights=None,
        *,
        inter_rank_sync,
        flags=0,
        registered_workspace=None,
        registered_workspace_bytes=0,
    ):
        self.calls.append(
            (
                "combine",
                plan,
                hidden,
                output,
                route_weights,
                output_route_weights,
                stream,
                inter_rank_sync,
                registered_workspace,
                registered_workspace_bytes,
                flags,
            )
        )

    def reduce_grad_workspace_info(
        self, context, plan, gradients, *, requested_udma_chunk_bytes=0
    ):
        from tilexr_moonep.runtime import ReduceGradWorkspaceInfo

        row_bytes = tuple(
            int(getattr(gradients, name).numel() // getattr(gradients, name).shape[0]) * 4
            for name in ("gate", "up", "down")
        )
        chunk_bytes = int(requested_udma_chunk_bytes) or (8 << 20)
        qp_count = max(3, self._udma_qp_count)
        qp_counts = (1, 1, qp_count - 2)
        lane_state_bytes = qp_count * 4096
        bank_stride_bytes = self.world_size * chunk_bytes
        lane_stride_bytes = 2 * bank_stride_bytes
        payload_bytes = qp_count * lane_stride_bytes
        workspace_bytes = (
            (lane_state_bytes + payload_bytes + (2 << 20) - 1) // (2 << 20)
        ) * (2 << 20)
        self.reduce_grad_query_calls += 1
        return ReduceGradWorkspaceInfo(
            workspace_bytes=workspace_bytes,
            workspace_alignment=2 << 20,
            udma_chunk_bytes=chunk_bytes,
            lane_state_bytes=lane_state_bytes,
            lane_state_stride_bytes=4096,
            bank_stride_bytes=bank_stride_bytes,
            lane_stride_bytes=lane_stride_bytes,
            row_bytes=row_bytes,
            chunk_counts=tuple(
                (value + chunk_bytes - 1) // chunk_bytes for value in row_bytes
            ),
            projection_qp_counts=qp_counts,
            qp_count=qp_count,
            block_dim=64,
        )

    def prepare_reduce_grad(
        self,
        context,
        plan,
        gradients,
        sources,
        source_registrations,
        workspace,
        *,
        requested_udma_chunk_bytes=0,
    ):
        prepared = self._next_reduce_grad_prepared
        self._next_reduce_grad_prepared += 1
        self.registered_workspace = workspace
        self.reduce_grad_prepared = prepared
        self.calls.append(
            (
                "prepare_reduce_grad",
                prepared,
                plan,
                gradients,
                sources,
                source_registrations,
                workspace,
                requested_udma_chunk_bytes,
            )
        )
        return prepared

    def destroy_reduce_grad(self, prepared):
        if prepared is None:
            return
        self.calls.append(("destroy_reduce_grad", prepared))
        if self.reduce_grad_prepared == prepared:
            self.reduce_grad_prepared = None
            self.registered_workspace = None

    def reduce_grad(
        self,
        context,
        plan,
        gradients,
        sources,
        source_registrations,
        prepared,
        stream,
        wait_iterations,
    ):
        self.calls.append(
            (
                "reduce_grad",
                plan,
                gradients,
                sources,
                source_registrations,
                prepared,
                stream,
                wait_iterations,
            )
        )
        plan.reduce_grad_status._item = 0

    def close(self):
        with self._reduce_grad_lock:
            if self._reduce_grad_owner_token is not None:
                raise RuntimeError(
                    "cannot close TileXR MoonEP runtime while ReduceGrad is in flight; "
                    "synchronize the owning buffer first"
                )
            if self.closed:
                return
            self.closed = True
            self.calls.append(("close", None))
