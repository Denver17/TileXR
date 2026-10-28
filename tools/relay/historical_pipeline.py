"""Benchmark-only GEMM and FFI adapter for the frozen external Host pipeline baseline."""

import ctypes
import time

import torch
import tilelang
import tilelang.language as T

@tilelang.jit(out_idx=[], target="ascendc", platform="A2")
def gemm(m, n, k):
    @T.prim_func
    def main(A: T.Tensor((m, k), "float16"),
             B: T.Tensor((k, n), "float16"),
             C: T.Tensor((m, n), "float16")):
        with T.Kernel((m // 128) * (n // 256), is_npu=True) as (cid, _):
            row = cid // (n // 256)
            col = cid % (n // 256)
            a = T.alloc_L1((128, 64), "float16")
            b = T.alloc_L1((64, 256), "float16")
            c = T.alloc_L0C((128, 256), "float")
            with T.Scope("C"):
                for step in T.serial(k // 64):
                    T.copy(A[row * 128, step * 64], a)
                    T.copy(B[step * 64, col * 256], b)
                    T.barrier_all()
                    T.gemm_v0(a, b, c, init=(step == 0))
                    T.barrier_all()
                T.copy(c, C[row * 128, col * 256])
    return main


def execute(mode, runtime, shards, weight, kernel, comm_stream, compute_stream, native):
    """One generation; retain every tile until both streams have completed."""
    a = shards[0]
    received = torch.empty((len(shards), 2 * a.shape[0], a.shape[1]), device=a.device, dtype=a.dtype)
    output = torch.empty((len(shards), 2 * a.shape[0], weight.shape[1]), device=a.device, dtype=a.dtype)
    if len(kernel.params) != 3 or kernel.workspace_idx or kernel.auto_gm_idx:
        raise ValueError("native bridge requires the three-buffer GEMM ABI without workspace")
    begin = time.perf_counter_ns()
    ret = native.TileXRAllGatherGemmPipeline(
        runtime.comm_ptr, ctypes.cast(kernel.adapter.lib.call, ctypes.c_void_p),
        a.data_ptr(), weight.data_ptr(), received.data_ptr(), output.data_ptr(),
        len(shards), a.shape[0], weight.shape[1], a.shape[1], int(mode == "overlap"),
        comm_stream.npu_stream, compute_stream.npu_stream)
    total_us = (time.perf_counter_ns() - begin) / 1000
    runtime._check("TileXRAllGatherGemmPipeline", ret, mode)
    pairs = list(zip(received.unbind(), output.unbind()))
    # Tiles are rank-major within each received buffer; restore global row order.
    def assemble(index):
        return torch.cat([torch.cat([pair[index].chunk(2)[rank].cpu() for pair in pairs])
                          for rank in range(2)])
    return assemble(1), assemble(0), {"total_us": total_us}
