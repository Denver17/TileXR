"""Build-time AllGather-GEMM generator for the canonical Relay ABI-v1.

One persistent group owns a row tile. The first column worker publishes and
pulls data; other column workers wait for its generation-tagged readiness.
Both AIVs participate in mode-2 handoff. No Host per-tile launch is involved.
Requires the separately pinned compiler.
"""

from math import gcd

import tilelang
import tilelang.language as T
from tilexr_relay.device import peer as P
from tilexr_relay.device import gemm as G
from tilexr_relay.device.abi import canonical_main
from .plans import artifact_shape


def generic_kernel(artifact):
    return _kernel(*artifact_shape(artifact, "allgather_gemm"), artifact.peer_layout)


@tilelang.jit(out_idx=[], target="ascendc", platform="A2")
def _kernel(rows, n, k, blocks, layout):
    # Batch K work in L1 without introducing tails for supported K multiples.
    block_k = gcd(k, 256)
    multiply = G.compute(k)
    cols = n // 256
    width = min(cols, blocks)
    gangs = blocks // width
    tiles = layout.slot_count
    # Fixed IPC planes: changing a later shape cannot overwrite an ACK that a
    # slower peer has not yet consumed from the preceding generation.
    ack_slot = layout.ack_slot
    waves = (tiles + gangs - 1) // gangs
    comm_rows = 1
    while comm_rows * 2 <= min(64, 32768 // k):
        comm_rows *= 2

    @T.macro
    def body(source, weight, gathered, output, own, peer, own_flags, peer_flags,
             ready, epoch, rank):
        with T.Kernel(blocks, is_npu=True) as (cid, vid):
            gang = cid // width
            column = cid % width
            # Workers have either all waves or one fewer; avoid signed division
            # in the compiler-lowered tail count.
            worker_waves = T.if_then_else((waves - 1) * gangs + gang < tiles,
                                         waves, waves - 1)
            copied = T.alloc_ub((comm_rows, k), "float16")
            token = T.alloc_ub((4,), "int64")
            a = T.alloc_L1((2, 128, block_k), "float16")
            b = T.alloc_L1((2, block_k, 256), "float16")
            a0 = T.alloc_L0A((2, 128, 64), "float16")
            b0 = T.alloc_L0B((2, 64, 256), "float16")
            c = T.alloc_L0C((128, 256), "float")
            with T.Scope("C"):
                G.initialize()
            for wave in T.serial(waves):
                tile = wave * gangs + gang
                if tile < tiles:
                    row = tile * 128
                    with T.Scope("V"):
                        if wave >= 2:
                            P.vector_wait_done(wave % 2)
                        if column == 0:
                            for chunk in T.serial(64 // comm_rows):
                                copy_row = row + vid * 64 + chunk * comm_rows
                                P.copy_to_two(source[copy_row, 0], own[copy_row, 0],
                                              gathered[rank * rows + copy_row, 0], copied)
                            P.publication_token(token, epoch)
                            P.publish(own_flags[tile, vid * 4], token)
                            P.wait_epoch(peer_flags[tile, vid * 4], token, epoch)
                            for chunk in T.serial(64 // comm_rows):
                                copy_row = row + vid * 64 + chunk * comm_rows
                                P.copy_peer(peer[copy_row, 0],
                                            gathered[(1 - rank) * rows + copy_row, 0], copied)
                            P.publication_token(token, epoch)
                            P.publish_ack_and_ready(own_flags[ack_slot + tile, vid * 4],
                                                    ready[tile, vid * 4], token)
                        else:
                            P.wait_epoch(ready[tile, vid * 4], token, epoch)
                        P.vector_ready(wave % 2)
                        if column == 0:
                            P.wait_epoch(peer_flags[ack_slot + tile, vid * 4], token, epoch)
                    with T.Scope("C"):
                        # Local work can start before the peer arrives.
                        for col_wave in T.serial((cols + width - 1) // width):
                            col = column + col_wave * width
                            if col < cols:
                                multiply(source, weight, row, col * 256, a, b, a0, b0, c)
                                G.store(c, output[rank * rows + row, col * 256])
                        P.cube_wait_ready(wave % 2)
                        for col_wave in T.serial((cols + width - 1) // width):
                            col = column + col_wave * width
                            if col < cols:
                                multiply(gathered, weight, (1 - rank) * rows + row,
                                         col * 256, a, b, a0, b0, c)
                                G.store(c, output[(1 - rank) * rows + row, col * 256])
                        P.cube_done(wave % 2)
            with T.Scope("C"):
                G.finish()
            with T.Scope("V"):
                # Drain only this worker's valid final waves, including tails.
                for pending in T.serial(2):
                    if worker_waves + pending >= 2:
                        P.vector_wait_done((worker_waves + pending) % 2)

    specs = [((rows, k), "float16"), ((k, n), "float16"),
             ((2 * rows, k), "float16"), ((2 * rows, n), "float16"),
             ((rows, k), "float16"), ((rows, k), "float16"),
             (layout.flag_shape, "int64"), (layout.flag_shape, "int64"),
             ((tiles, 8), "int64")]
    return canonical_main(body, specs, scalars=("epoch", "rank"))
