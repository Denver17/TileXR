"""K-sharded GEMM -> two-rank FP32 AllReduce SUM -> FP16 output.

Each mixed worker owns one 128x256 output tile per wave. Cube publishes a
FP32 partial to owner peer memory; both paired Vectors exchange and reduce
their 64-row halves. Reuse waits for peer ACK and local completion. No Host
operator dispatch, FP16 partial rounding, or separate reduction kernel.
"""

from math import gcd

import tilelang
import tilelang.language as T
from tilexr_relay.device import peer as P
from tilexr_relay.device import gemm as G
from tilexr_relay.device.abi import canonical_main
from .plans import artifact_shape


def generic_kernel(artifact):
    return _kernel(*artifact_shape(artifact, "gemm_allreduce"), artifact.peer_layout)


@tilelang.jit(out_idx=[], target="ascendc", platform="A2")
def _kernel(rows, n, k, blocks, layout):
    # Batch K work in L1 without introducing tails for supported K multiples.
    block_k = gcd(k, 256)
    multiply = G.compute(k)
    cols = n // 256
    tiles = layout.slot_count
    waves = (tiles + blocks - 1) // blocks
    ack_slot = layout.ack_slot

    @T.macro
    def body(source, weight, output, own, peer, own_flags, peer_flags, epoch):
        with T.Kernel(blocks, is_npu=True) as (cid, vid):
            # Workers have either all waves or one fewer; avoid signed division
            # in the compiler-lowered tail count.
            worker_waves = T.if_then_else((waves - 1) * blocks + cid < tiles,
                                         waves, waves - 1)
            a = T.alloc_L1((2, 128, block_k), "float16")
            b = T.alloc_L1((2, block_k, 256), "float16")
            a0 = T.alloc_L0A((2, 128, 64), "float16")
            b0 = T.alloc_L0B((2, 64, 256), "float16")
            c = T.alloc_L0C((128, 256), "float32")
            local = T.alloc_ub((8, 256), "float32")
            remote = T.alloc_ub((8, 256), "float32")
            summed = T.alloc_ub((8, 256), "float32")
            converted = T.alloc_ub((8, 256), "float16")
            token = T.alloc_ub((4,), "int64")
            with T.Scope("C"):
                G.initialize()
            for wave in T.serial(waves):
                tile = wave * blocks + cid
                if tile < tiles:
                    row = (tile // cols) * 128
                    col = (tile % cols) * 256
                    with T.Scope("C"):
                        if wave >= 2:
                            P.cube_wait_ready(wave % 2)
                        multiply(source, weight, row, col, a, b, a0, b0, c)
                        G.store(c, own[row, col])
                        P.cube_done(wave % 2)
                    with T.Scope("V"):
                        P.vector_wait_done(wave % 2)
                        P.publication_token(token, epoch)
                        P.publish(own_flags[tile, vid * 4], token)
                        P.wait_epoch(peer_flags[tile, vid * 4], token, epoch)
                        for chunk in T.serial(8):
                            copy_row = row + vid * 64 + chunk * 8
                            T.copy(own[copy_row, col], local)
                            T.copy(peer[copy_row, col], remote)
                            T.barrier_all()
                            T.tile.add(summed, local, remote)
                            T.barrier_all()
                            T.copy(summed, converted)
                            T.barrier_all()
                            T.copy(converted, output[copy_row, col])
                            T.set_flag("MTE3", "MTE2", 0)
                            T.wait_flag("MTE3", "MTE2", 0)
                        # Remote reads and local output stores have completed;
                        # peer publication for this epoch was already observed.
                        P.publication_token(token, epoch)
                        P.publish(own_flags[ack_slot + tile, vid * 4], token)
                        P.wait_epoch(peer_flags[ack_slot + tile, vid * 4], token, epoch)
                        P.vector_ready(wave % 2)
            with T.Scope("C"):
                G.finish()
                # Peer ACKs and output stores must finish before kernel exit.
                for pending in T.serial(2):
                    if worker_waves + pending >= 2:
                        P.cube_wait_ready((worker_waves + pending) % 2)
    specs = [((rows, k), "float16"), ((k, n), "float16"), ((rows, n), "float16"),
             ((rows, n), "float32"), ((rows, n), "float32"),
             (layout.flag_shape, "int64"), (layout.flag_shape, "int64")]
    return canonical_main(body, specs, scalars=("epoch",))
