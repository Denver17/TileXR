"""Inlined peer-memory DMA, epoch flags and Cube/Vector handoff primitives.

Flag buffers use 64-byte slots split between the paired AIVs. Publication and
ACK planes remain fixed across specializations. These macros allocate no UB
and perform no scalar GM polling. Callers own storage and participant counts.
"""

import tilelang.language as T


@T.macro
def copy_to_two(source, first, second, scratch):
    T.copy(source, scratch)
    T.set_flag("MTE2", "MTE3", 0)
    T.wait_flag("MTE2", "MTE3", 0)
    T.copy(scratch, first)
    T.copy(scratch, second)
    T.set_flag("MTE3", "MTE2", 0)
    T.wait_flag("MTE3", "MTE2", 0)


@T.macro
def copy_peer(source, destination, scratch):
    T.copy(source, scratch)
    T.set_flag("MTE2", "MTE3", 0)
    T.wait_flag("MTE2", "MTE3", 0)
    T.copy(scratch, destination)
    T.set_flag("MTE3", "MTE2", 0)
    T.wait_flag("MTE3", "MTE2", 0)


@T.macro
def publication_token(token, epoch):
    # Payload MTE3 writes precede the flag publication.
    T.set_flag("MTE3", "S", 1)
    T.wait_flag("MTE3", "S", 1)
    for lane in T.serial(4):
        token[lane] = epoch
    T.set_flag("S", "MTE3", 1)
    T.wait_flag("S", "MTE3", 1)


@T.macro
def publish(destination, token):
    T.copy(token, destination)
    T.set_flag("MTE3", "MTE2", 0)
    T.wait_flag("MTE3", "MTE2", 0)


@T.macro
def publish_ack_and_ready(ack, ready, token):
    T.copy(token, ack)
    T.copy(token, ready)
    T.set_flag("MTE3", "MTE2", 0)
    T.wait_flag("MTE3", "MTE2", 0)


@T.macro
def wait_epoch(source, token, epoch):
    T.copy(source, token)
    T.set_flag("MTE2", "S", 1)
    T.wait_flag("MTE2", "S", 1)
    while token[0] != epoch:
        T.set_flag("S", "MTE2", 1)
        T.wait_flag("S", "MTE2", 1)
        T.copy(source, token)
        T.set_flag("MTE2", "S", 1)
        T.wait_flag("MTE2", "S", 1)


@T.macro
def vector_ready(slot):
    # Mode 2 includes both paired AIV participants, never just one.
    T.set_cross_flag("MTE3", slot, 2)


@T.macro
def cube_wait_ready(slot):
    T.wait_cross_flag(slot)


@T.macro
def cube_done(slot):
    T.set_cross_flag("FIX", 2 + slot, 2)


@T.macro
def vector_wait_done(slot):
    T.wait_cross_flag(2 + slot)
