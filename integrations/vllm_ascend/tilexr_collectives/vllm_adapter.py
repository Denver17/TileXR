from __future__ import annotations

import os
from dataclasses import dataclass, field
from threading import Lock

from .torch_collectives import (
    _torch_dtype_to_tilexr, all_gather, all_reduce, broadcast, reduce_scatter,
)

_ENABLED_VALUES = {"1", "true", "yes", "on"}


def enabled() -> bool:
    value = os.environ.get("VLLM_ASCEND_TILEXR_COLLECTIVES")
    return value is not None and value.strip().lower() in _ENABLED_VALUES


@dataclass
class TileXRVllmCollectivesAdapter:
    rank: int
    world_size: int
    install_prefix: str
    runtime: object
    domain: int
    members: tuple[int, ...]
    last_error: BaseException | None = field(default=None, init=False, repr=False)
    _lock: object = field(default_factory=Lock, init=False, repr=False)
    _completion: object = field(default=None, init=False, repr=False)
    _closed: bool = field(default=False, init=False)

    def _check_active(self):
        if self._closed:
            raise RuntimeError("TileXR group is closed")
        if self.last_error is not None:
            raise RuntimeError("TileXR group previously failed") from self.last_error

    def should_fallback(self, tensor) -> bool:
        self._check_active()
        if getattr(getattr(tensor, "device", None), "type", None) != "npu":
            return True
        if tensor.numel() == 0:
            return True
        try:
            _torch_dtype_to_tilexr(tensor.dtype)
        except TypeError:
            return True
        return False

    def _execute(self, callback):
        import torch

        with self._lock:
            self._check_active()
            torch.npu.set_device(self.runtime.device_index)
            stream = torch.npu.current_stream()
            if self._completion is not None:
                stream.wait_event(self._completion)
            try:
                result = callback()
                event = torch.npu.Event()
                event.record(stream)
                self._completion = event
                return result
            except Exception as exc:
                self.last_error = exc
                raise

    def _kwargs(self):
        return dict(rank=self.rank, world_size=self.world_size,
                    install_prefix=self.install_prefix, runtime=self.runtime)

    def all_reduce(self, input_):
        if self.should_fallback(input_):
            return None

        def run():
            output = all_reduce(input_.contiguous(), **self._kwargs())
            input_.copy_(output)
            return input_

        return self._execute(run)

    def all_gather(self, input_, dim: int = -1):
        if self.should_fallback(input_):
            return None
        return self._execute(lambda: all_gather(input_.contiguous(), dim=dim, **self._kwargs()))

    def reduce_scatter(self, input_, dim: int = -1):
        if self.should_fallback(input_):
            return None

        def run():
            # Match DeviceCommunicatorBase's axis movement, including 3-D input.
            moved = input_.movedim(0, dim).contiguous()
            output = reduce_scatter(moved, dim=0, **self._kwargs())
            return output.movedim(0, dim).contiguous()

        return self._execute(run)

    def broadcast(self, tensor, src: int = 0):
        if self.should_fallback(tensor):
            return None

        def run():
            output = broadcast(tensor.contiguous(), root=src, **self._kwargs())
            if output is not tensor:
                tensor.copy_(output)
            return tensor

        return self._execute(run)

    def all_to_all(self, input_, scatter_dim=0, gather_dim=-1,
                   scatter_sizes=None, gather_sizes=None):
        # Native AllToAll requires TOPO_910_93. This integration is qualified on
        # 910B TP8; keep the framework implementation for this operation.
        self._check_active()
        return None

    def close(self):
        import torch

        with self._lock:
            if self._closed:
                return
            torch.npu.synchronize(self.runtime.device_index)
            self.runtime.close()
            self._closed = True
