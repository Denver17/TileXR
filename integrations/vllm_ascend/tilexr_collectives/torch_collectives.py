from __future__ import annotations

from functools import lru_cache

from .runtime import (
    TILEXR_DATA_TYPE_BFP16,
    TILEXR_DATA_TYPE_FP16,
    TILEXR_DATA_TYPE_FP32,
    TILEXR_DATA_TYPE_INT8,
    TILEXR_DATA_TYPE_INT32,
    TILEXR_DATA_TYPE_INT64,
    TileXRCollectivesRuntime,
)


def _torch():
    import torch

    return torch


def _torch_dtype_to_tilexr(dtype) -> int:
    torch = _torch()
    mapping = {
        torch.int8: TILEXR_DATA_TYPE_INT8,
        torch.int32: TILEXR_DATA_TYPE_INT32,
        torch.int64: TILEXR_DATA_TYPE_INT64,
        torch.float16: TILEXR_DATA_TYPE_FP16,
        torch.float32: TILEXR_DATA_TYPE_FP32,
        torch.bfloat16: TILEXR_DATA_TYPE_BFP16,
    }
    if dtype not in mapping:
        raise TypeError(f"unsupported TileXR collective dtype: {dtype}")
    return mapping[dtype]


def _npu_device_index(tensor) -> int:
    if tensor.device.index is None:
        return int(_torch().npu.current_device())
    return int(tensor.device.index)


def _bind_npu_device(device_index: int) -> None:
    torch = _torch()
    torch.npu.set_device(device_index)


def _current_stream_ptr(device_index: int) -> int:
    torch = _torch()
    torch.npu.set_device(device_index)
    stream = torch.npu.current_stream()
    stream_value = getattr(stream, "npu_stream", None)
    if stream_value is None:
        stream_value = getattr(stream, "stream", None)
    if stream_value is None:
        raise RuntimeError("torch.npu.current_stream() does not expose npu_stream or stream")
    return int(stream_value)


def _record_stream(*tensors) -> None:
    stream = _torch().npu.current_stream()
    for tensor in tensors:
        tensor.record_stream(stream)


def _validate_npu_contiguous(tensor, name: str) -> None:
    if tensor.device.type != "npu":
        raise ValueError(f"{name} must be on an NPU device, got {tensor.device}")
    if not tensor.is_contiguous():
        raise ValueError(f"{name} must be contiguous")


def _normalize_dim(dim: int, ndim: int) -> int:
    if dim < 0:
        dim += ndim
    if dim < 0 or dim >= ndim:
        raise ValueError(f"invalid dim={dim} for tensor with ndim={ndim}")
    return dim


def _move_dim_to_front(tensor, dim: int):
    dim = _normalize_dim(dim, tensor.dim())
    if dim == 0:
        return tensor.contiguous(), dim
    return tensor.movedim(dim, 0).contiguous(), dim


def _restore_dim_from_front(tensor, dim: int):
    if dim == 0:
        return tensor.contiguous()
    return tensor.movedim(0, dim).contiguous()


@lru_cache(maxsize=32)
def _cached_runtime(rank: int, world_size: int, install_prefix: str, device_index: int) -> TileXRCollectivesRuntime:
    _bind_npu_device(device_index)
    runtime = TileXRCollectivesRuntime(rank=rank, world_size=world_size, install_prefix=install_prefix)
    runtime.device_index = int(device_index)
    return runtime


def get_runtime(rank: int, world_size: int, install_prefix: str, device_index: int | None = None) -> TileXRCollectivesRuntime:
    if device_index is None:
        device_index = int(_torch().npu.current_device())
    return _cached_runtime(int(rank), int(world_size), str(install_prefix), int(device_index))


def _select_runtime(runtime, rank: int, world_size: int, install_prefix: str, device_index: int):
    if runtime is None:
        return get_runtime(rank, world_size, install_prefix, device_index=device_index)
    runtime_device_index = getattr(runtime, "device_index", None)
    if runtime_device_index is None:
        raise ValueError("runtime must expose device_index when passed explicitly")
    if int(runtime_device_index) != int(device_index):
        raise ValueError(f"runtime device_index={runtime_device_index} does not match tensor device_index={device_index}")
    return runtime


def all_gather(tensor, rank: int, world_size: int, install_prefix: str, dim: int = -1, runtime=None):
    torch = _torch()
    _validate_npu_contiguous(tensor, "tensor")
    if tensor.numel() <= 0:
        raise ValueError("tensor must contain at least one element")
    moved, normalized_dim = _move_dim_to_front(tensor, dim)
    device_index = _npu_device_index(moved)
    _bind_npu_device(device_index)
    rt = _select_runtime(runtime, rank, world_size, install_prefix, device_index)
    output = torch.empty((world_size * moved.numel(),), dtype=moved.dtype, device=moved.device)
    _record_stream(moved, output)
    rt.all_gather(
        send_ptr=moved.data_ptr(),
        recv_ptr=output.data_ptr(),
        send_count=moved.numel(),
        tilexr_dtype=_torch_dtype_to_tilexr(moved.dtype),
        stream_ptr=_current_stream_ptr(device_index),
    )
    gathered = output.view(world_size, *moved.shape).reshape(world_size * moved.shape[0], *moved.shape[1:])
    return _restore_dim_from_front(gathered, normalized_dim)


def all_reduce(tensor, rank: int, world_size: int, install_prefix: str, runtime=None):
    torch = _torch()
    _validate_npu_contiguous(tensor, "tensor")
    if tensor.numel() <= 0:
        raise ValueError("tensor must contain at least one element")
    device_index = _npu_device_index(tensor)
    _bind_npu_device(device_index)
    rt = _select_runtime(runtime, rank, world_size, install_prefix, device_index)
    output = torch.empty_like(tensor)
    _record_stream(tensor, output)
    rt.all_reduce(
        send_ptr=tensor.data_ptr(),
        recv_ptr=output.data_ptr(),
        count=tensor.numel(),
        tilexr_dtype=_torch_dtype_to_tilexr(tensor.dtype),
        stream_ptr=_current_stream_ptr(device_index),
    )
    return output


def reduce_scatter(tensor, rank: int, world_size: int, install_prefix: str, dim: int = -1, runtime=None):
    torch = _torch()
    _validate_npu_contiguous(tensor, "tensor")
    if tensor.numel() <= 0:
        raise ValueError("tensor must contain at least one element")
    moved, normalized_dim = _move_dim_to_front(tensor, dim)
    if moved.shape[0] % world_size != 0:
        raise ValueError(
            f"tensor.shape[{normalized_dim}]={tensor.shape[normalized_dim]} "
            f"must be divisible by world_size={world_size}"
        )
    device_index = _npu_device_index(moved)
    _bind_npu_device(device_index)
    rt = _select_runtime(runtime, rank, world_size, install_prefix, device_index)
    output_shape = (moved.shape[0] // world_size, *moved.shape[1:])
    output = torch.empty(output_shape, dtype=moved.dtype, device=moved.device)
    _record_stream(moved, output)
    rt.reduce_scatter(
        send_ptr=moved.data_ptr(),
        recv_ptr=output.data_ptr(),
        recv_count=output.numel(),
        tilexr_dtype=_torch_dtype_to_tilexr(moved.dtype),
        stream_ptr=_current_stream_ptr(device_index),
    )
    return _restore_dim_from_front(output, normalized_dim)


def broadcast(tensor, rank: int, world_size: int, install_prefix: str, root: int = 0, runtime=None):
    _validate_npu_contiguous(tensor, "tensor")
    if tensor.numel() <= 0:
        raise ValueError("tensor must contain at least one element")
    if root < 0 or root >= world_size:
        raise ValueError(f"root must be in [0, {world_size}), got {root}")
    device_index = _npu_device_index(tensor)
    _bind_npu_device(device_index)
    rt = _select_runtime(runtime, rank, world_size, install_prefix, device_index)
    output = tensor.contiguous()
    _record_stream(output)
    rt.broadcast(
        buf_ptr=output.data_ptr(),
        count=output.numel(),
        tilexr_dtype=_torch_dtype_to_tilexr(output.dtype),
        root=root,
        stream_ptr=_current_stream_ptr(device_index),
    )
    return output


def all_to_all(
    tensor,
    rank: int,
    world_size: int,
    install_prefix: str,
    scatter_dim: int = 0,
    gather_dim: int = -1,
    runtime=None,
):
    torch = _torch()
    _validate_npu_contiguous(tensor, "tensor")
    if tensor.numel() <= 0:
        raise ValueError("tensor must contain at least one element")
    scatter_dim = _normalize_dim(scatter_dim, tensor.dim())
    gather_dim = _normalize_dim(gather_dim, tensor.dim())
    if scatter_dim != gather_dim:
        raise ValueError("TileXR all_to_all requires scatter_dim and gather_dim to match")
    moved, normalized_dim = _move_dim_to_front(tensor, scatter_dim)
    if moved.shape[0] % world_size != 0:
        raise ValueError(
            f"tensor.shape[{normalized_dim}]={tensor.shape[normalized_dim]} "
            f"must be divisible by world_size={world_size}"
        )
    device_index = _npu_device_index(moved)
    _bind_npu_device(device_index)
    rt = _select_runtime(runtime, rank, world_size, install_prefix, device_index)
    output = torch.empty_like(moved)
    _record_stream(moved, output)
    rt.all_to_all(
        send_ptr=moved.data_ptr(),
        recv_ptr=output.data_ptr(),
        send_count_per_peer=moved.numel() // world_size,
        tilexr_dtype=_torch_dtype_to_tilexr(moved.dtype),
        stream_ptr=_current_stream_ptr(device_index),
    )
    return _restore_dim_from_front(output, normalized_dim)
