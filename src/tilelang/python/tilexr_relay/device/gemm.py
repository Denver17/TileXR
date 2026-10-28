"""Shared AIC GEMM pipeline: two L1/L0 stages and one FP32 accumulator.

Operators own buffers, tile traversal and communication readiness. These
inlined macros own only intra-AIC credits; initialize/drain once per worker,
not once per K batch. FIX completion protects reuse of the accumulator.
"""

from math import gcd

import tilelang.language as T


@T.macro
def initialize():
    T.set_flag("MTE1", "MTE2", 0)
    T.set_flag("MTE1", "MTE2", 1)
    T.set_flag("M", "MTE1", 0)
    T.set_flag("M", "MTE1", 1)
    T.set_flag("FIX", "M", 0)


@T.macro
def finish():
    T.wait_flag("MTE1", "MTE2", 0)
    T.wait_flag("MTE1", "MTE2", 1)
    T.wait_flag("M", "MTE1", 0)
    T.wait_flag("M", "MTE1", 1)
    T.wait_flag("FIX", "M", 0)


@T.macro
def store(accumulator, destination):
    T.set_flag("M", "FIX", 0)
    T.wait_flag("M", "FIX", 0)
    T.copy(accumulator, destination)
    T.set_flag("FIX", "M", 0)


def compute(k):
    """Specialize a full 128x256 output tile without compiler/runtime dispatch."""
    block_k = gcd(k, 256)
    microtiles = block_k // 64

    @T.macro
    def multiply(source, weight, row, col, a, b, a0, b0, accumulator):
        T.wait_flag("MTE1", "MTE2", 0)
        T.copy(source[row, 0], a[0, :, :])
        T.copy(weight[0, col], b[0, :, :])
        T.set_flag("MTE2", "MTE1", 0)
        T.wait_flag("FIX", "M", 0)
        for step in T.serial(k // block_k):
            if step + 1 < k // block_k:
                T.wait_flag("MTE1", "MTE2", (step + 1) % 2)
                T.copy(source[row, (step + 1) * block_k], a[(step + 1) % 2, :, :])
                T.copy(weight[(step + 1) * block_k, col], b[(step + 1) % 2, :, :])
                T.set_flag("MTE2", "MTE1", (step + 1) % 2)
            for chunk in T.serial(microtiles):
                stage = (step * microtiles + chunk) % 2
                if chunk == 0:
                    T.wait_flag("MTE2", "MTE1", step % 2)
                T.wait_flag("M", "MTE1", stage)
                T.copy(a[step % 2, 0, chunk * 64], a0[stage, :, :])
                T.copy(b[step % 2, chunk * 64, 0], b0[stage, :, :])
                if chunk == microtiles - 1:
                    T.set_flag("MTE1", "MTE2", step % 2)
                T.set_flag("MTE1", "M", stage)
                T.wait_flag("MTE1", "M", stage)
                T.mma(a0[stage, :, :], b0[stage, :, :], accumulator,
                      init=T.And(step == 0, chunk == 0))
                T.set_flag("M", "MTE1", stage)

    return multiply
