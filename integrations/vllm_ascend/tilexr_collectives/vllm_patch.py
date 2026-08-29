from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path
from typing import Callable

from .vllm_adapter import TileXRVllmCollectivesAdapter, enabled


_PATCH_MARKER = "_tilexr_collectives_patch_applied"
_ORIGINALS_ATTR = "_tilexr_collectives_originals"
_ROUTE_METHODS = ("all_reduce", "all_gather", "reduce_scatter", "all_to_all", "broadcast")


def _resolve_install_prefix(install_prefix: str | os.PathLike[str] | None) -> str:
    if install_prefix is not None:
        return str(Path(install_prefix))
    env_prefix = os.environ.get("TILEXR_INSTALL_PREFIX")
    if env_prefix:
        return env_prefix
    return str(Path.cwd() / "install")


def _trace_enabled() -> bool:
    value = os.environ.get("TILEXR_VLLM_TRACE")
    return value is not None and value.strip().lower() in {"1", "true", "yes", "on"}


def _trace(message: str) -> None:
    if _trace_enabled():
        print(f"[tilexr-vllm] {message}", file=sys.stderr)


def _initial_route_counts() -> dict[str, dict[str, int]]:
    return {name: {"tilexr": 0, "fallback": 0, "error": 0} for name in _ROUTE_METHODS}


def _ensure_adapter(self, install_prefix: str):
    adapter = getattr(self, "_tilexr_collectives_adapter", None)
    if adapter is not None:
        return adapter
    try:
        adapter = TileXRVllmCollectivesAdapter(
            rank=int(self.rank),
            world_size=int(self.world_size),
            install_prefix=install_prefix,
        )
    except Exception as exc:
        self._tilexr_collectives_last_error = exc
        _trace(f"adapter init fallback: {type(exc).__name__}: {exc}")
        return None
    self._tilexr_collectives_adapter = adapter
    return adapter


def _count_route(self, method_name: str, route: str) -> None:
    counts = getattr(self, "_tilexr_collectives_route_counts", None)
    if counts is None:
        counts = _initial_route_counts()
        self._tilexr_collectives_route_counts = counts
    counts.setdefault(method_name, {"tilexr": 0, "fallback": 0, "error": 0})
    counts[method_name][route] = counts[method_name].get(route, 0) + 1


def _call_tilexr_or_original(
    self,
    method_name: str,
    original: Callable,
    install_prefix: str,
    *args,
    **kwargs,
):
    adapter = _ensure_adapter(self, install_prefix)
    if adapter is None:
        _count_route(self, method_name, "fallback")
        return original(self, *args, **kwargs)
    adapter_method = getattr(adapter, method_name, None)
    if adapter_method is None:
        _count_route(self, method_name, "fallback")
        return original(self, *args, **kwargs)

    try:
        result = adapter_method(*args, **kwargs)
    except Exception as exc:
        self._tilexr_collectives_last_error = exc
        _count_route(self, method_name, "error")
        _trace(f"{method_name} adapter error; fallback: {type(exc).__name__}: {exc}")
        return original(self, *args, **kwargs)
    if result is None:
        self._tilexr_collectives_last_error = getattr(adapter, "last_error", None)
        _count_route(self, method_name, "fallback")
        if self._tilexr_collectives_last_error is not None:
            exc = self._tilexr_collectives_last_error
            _trace(f"{method_name} fallback: {type(exc).__name__}: {exc}")
        return original(self, *args, **kwargs)

    _count_route(self, method_name, "tilexr")
    _trace(f"{method_name} routed to TileXR")
    return result


def _patch_init(cls, install_prefix: str) -> None:
    original_init = cls.__init__

    def __init__(self, *args, **kwargs):
        original_init(self, *args, **kwargs)
        self._tilexr_collectives_install_prefix = install_prefix
        self._tilexr_collectives_adapter = None
        self._tilexr_collectives_last_error = None
        self._tilexr_collectives_route_counts = _initial_route_counts()

    cls.__init__ = __init__


def _patch_collective_methods(cls, install_prefix: str) -> None:
    originals = {}

    original_all_reduce = getattr(cls, "all_reduce", None)
    originals["all_reduce"] = original_all_reduce

    def all_reduce(self, input_):
        return _call_tilexr_or_original(self, "all_reduce", original_all_reduce, install_prefix, input_)

    original_all_gather = getattr(cls, "all_gather", None)
    originals["all_gather"] = original_all_gather

    def all_gather(self, input_, dim: int = -1):
        return _call_tilexr_or_original(self, "all_gather", original_all_gather, install_prefix, input_, dim=dim)

    original_reduce_scatter = getattr(cls, "reduce_scatter", None)
    originals["reduce_scatter"] = original_reduce_scatter

    def reduce_scatter(self, input_, dim: int = -1):
        return _call_tilexr_or_original(
            self,
            "reduce_scatter",
            original_reduce_scatter,
            install_prefix,
            input_,
            dim=dim,
        )

    original_all_to_all = getattr(cls, "all_to_all", None)
    originals["all_to_all"] = original_all_to_all

    def all_to_all(
        self,
        input_,
        scatter_dim: int = 0,
        gather_dim: int = -1,
        scatter_sizes: list[int] | None = None,
        gather_sizes: list[int] | None = None,
    ):
        return _call_tilexr_or_original(
            self,
            "all_to_all",
            original_all_to_all,
            install_prefix,
            input_,
            scatter_dim=scatter_dim,
            gather_dim=gather_dim,
            scatter_sizes=scatter_sizes,
            gather_sizes=gather_sizes,
        )

    original_broadcast = getattr(cls, "broadcast", None)
    originals["broadcast"] = original_broadcast

    def broadcast(self, tensor, src: int = 0):
        return _call_tilexr_or_original(self, "broadcast", original_broadcast, install_prefix, tensor, src=src)

    if original_all_reduce is not None:
        cls.all_reduce = all_reduce
    if original_all_gather is not None:
        cls.all_gather = all_gather
    if original_reduce_scatter is not None:
        cls.reduce_scatter = reduce_scatter
    if original_all_to_all is not None:
        cls.all_to_all = all_to_all
    if original_broadcast is not None:
        cls.broadcast = broadcast
    setattr(cls, _ORIGINALS_ATTR, originals)


def patch_npu_communicator(install_prefix: str | os.PathLike[str] | None = None) -> bool:
    if not enabled():
        return False

    module = importlib.import_module("vllm_ascend.distributed.device_communicators.npu_communicator")
    communicator_cls = module.NPUCommunicator
    if getattr(communicator_cls, _PATCH_MARKER, False):
        return True

    resolved_install_prefix = _resolve_install_prefix(install_prefix)
    _patch_init(communicator_cls, resolved_install_prefix)
    _patch_collective_methods(communicator_cls, resolved_install_prefix)
    setattr(communicator_cls, _PATCH_MARKER, True)
    setattr(communicator_cls, "_tilexr_collectives_install_prefix", resolved_install_prefix)
    _trace(f"NPUCommunicator patched with install_prefix={resolved_install_prefix}")
    return True
