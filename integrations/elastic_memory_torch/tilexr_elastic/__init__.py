"""通过 _C 扩展封装 ElasticMemory pool 与 compact EP，返回推理用的普通 NPU Tensor。

Pending.materialize 等待元数据并在消费 stream 排入 producer 完成依赖，缓存结果；DeepEPBuffer 每次持有一个可消费的路由
handle，下一轮 Dispatch 前须 Combine 或 discard。Storage 的 allocation lease 与
路由 handle 分开持有，view 可跨 pool 关闭存活。额外消费 stream 须显式登记，调用者
保持设备及 stream 存活至退休；pool 统计不包括 communicator 的 staging。
"""
from contextlib import contextmanager

import torch
import torch_npu  # noqa: F401 -- registers PrivateUse1 and NPU stream/device support

from . import _C


class ElasticMemory:
    """An initially empty data-HBM pool bound to one ordered communicator.

    Call close() before device teardown. Existing tensors keep their segments
    alive after close; keep their NPU device and consumer streams alive as well.
    """

    def __init__(self, device=None, *, initial_bytes=0, max_committed_bytes=1 << 30,
                 max_cached_free_bytes=32 << 20, segment_bytes=2 << 20, max_pending=64):
        if initial_bytes != 0:
            raise ValueError("ElasticMemory requires initial_bytes=0")
        if device is None:
            index = torch.npu.current_device()
        else:
            value = torch.device(device)
            if value.type != "npu":
                raise ValueError("ElasticMemory requires an NPU device")
            index = value.index if value.index is not None else torch.npu.current_device()
        self.device = torch.device("npu", index)
        with torch.npu.device(self.device):
            torch.npu.current_stream()  # Initialize the framework-owned ACL context.
            self._native = _C.Pool(index, max_committed_bytes, max_cached_free_bytes,
                                   segment_bytes, max_pending)

    def snapshot(self):
        return self._native.snapshot()

    def collect(self):
        """Request nonblocking collection and return the current snapshot."""
        return self._native.collect()

    def trim(self, wait=True):
        """Release empty segments, optionally waiting for retired consumers."""
        return self._native.trim(wait)

    def close(self):
        """Drain admitted work and close admission; live tensors stay valid."""
        return self._native.close()


class DispatchResult:
    """Stable compact tensors and their single-use Combine routing handle."""

    def __init__(self, native):
        self._native = native
        self.hidden = native.hidden
        self.assist = native.assist
        self.expert_counts = native.expert_counts
        self.send_counts = native.send_counts
        self.handle = self

    @property
    def allocation_address(self):
        return self._native.allocation_address

    def synchronize(self):
        """Wait for producer completion and raise any late execution error."""
        self._native.synchronize()
        return self

    @contextmanager
    def use_on_stream(self, stream=None):
        """Keep the result alive and record the selected consumer stream's tail.

        Use this scope for every additional consumer stream. Ordinary operations
        on the materialization stream are registered automatically. Tensor views
        retain Storage; raw-pointer consumers must obey this same scope contract.
        """
        if stream is None:
            stream = torch.npu.current_stream(self.hidden.device)
        if stream.device != self.hidden.device:
            raise ValueError("consumer stream must belong to the output NPU")
        with torch.npu.stream(stream):
            self._native.record_current(False)
            try:
                yield self
            finally:
                self._native.record_current(True)

    def discard(self):
        """Wait/check the producer, then release its turn without Combine."""
        self._native.discard()


class Pending:
    """Native progression continues without materialize() or Python callbacks."""

    def __init__(self, native, device):
        self._native = native
        self.device = device
        self._result = None

    def materialize(self, stream=None):
        """Wait for metadata, then enqueue data readiness on the consumer stream.

        This may return before Dispatch completes. Call synchronize() to check
        late runtime/protocol failures; failed output contents are invalid.
        """
        if stream is not None and stream.device != self.device:
            raise ValueError("materialization stream must belong to the pool NPU")
        if stream is None:
            native = self._native.materialize()
        else:
            with torch.npu.stream(stream):
                native = self._native.materialize()
        if self._result is None:
            self._result = DispatchResult(native)
        return self._result

    def synchronize(self):
        return self.materialize().synchronize()


class DeepEPBuffer:
    """A5 ordinary Memory compact Dispatch/Combine, without a public Planner.

    Rank calls must use identical order and shape. One routing handle is active
    at a time; combine or discard it before the next Dispatch. The current EP
    transport retains a fixed 2-GiB peer staging allocation per rank, reported
    separately from ElasticMemory's data pool. No autograd is implemented.
    """

    def __init__(self, *, output_pool, rank, world_size, comm_domain, staging_mib=2048):
        self.output_pool = output_pool
        self._native = _C.Buffer(output_pool._native, rank, world_size, comm_domain, staging_mib)

    @property
    def staging_bytes(self):
        """Configured data staging; excludes communicator flags and offset guard."""
        return self._native.staging_bytes

    @property
    def peer_window_bytes(self):
        """Runtime logical peer window, including its flag area.

        The current memory-domain allocator additionally reserves a 1-MiB guard
        outside this window; these allocations are separate from pool statistics.
        """
        return self._native.peer_window_bytes

    def dispatch_compact_async(self, hidden, *, topk_experts, num_experts):
        return Pending(self._native.dispatch(hidden, topk_experts, num_experts), self.output_pool.device)

    def dispatch_compact(self, hidden, *, topk_experts, num_experts):
        return self.dispatch_compact_async(hidden, topk_experts=topk_experts,
                                           num_experts=num_experts).materialize()

    def combine_compact(self, handle, expert_output, *, topk_weights):
        """Return data-complete output; native waiting releases the GIL."""
        return self._native.combine(handle._native, expert_output, topk_weights)
