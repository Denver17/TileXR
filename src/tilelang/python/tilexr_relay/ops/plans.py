"""Build concrete AllGather-GEMM and GEMM-AllReduce binding/resource plans.

Operator shape rules, byte formulas, slot assignments and peer/workspace
layouts live here, outside the generic runtime. No compiler or live device
allocations are needed to construct these immutable plans.
"""

from ..contracts import (BindingSpec, Initialization, PeerMemoryLayout, Region, RegionSpace,
                         Slot, Source)
from ..planning import PlanBuilder

FP16, FP32, INT64 = 1, 2, 3
ROW_MAJOR = 1
PEER_PAYLOAD_LIMIT = 1022 * 1024 * 1024


def validate_shape(rows, n, k, blocks):
    for value, multiple, maximum in ((rows, 128, 1048576), (n, 256, 1048576),
                                     (k, 64, 32768), (blocks, 1, 64)):
        if type(value) is not int or value <= 0 or value > maximum or value % multiple:
            raise ValueError("unsupported full-tile shape or worker count")


def peer_layout(operator, rows, n, k, blocks):
    """Derive the fixed communication layout before compilation."""
    validate_shape(rows, n, k, blocks)
    if operator == "allgather_gemm":
        return PeerMemoryLayout(rows // 128, 6, 7)
    if operator == "gemm_allreduce":
        return PeerMemoryLayout((rows // 128) * (n // 256), 5, 6)
    raise ValueError(f"unsupported operator {operator}")


def artifact_shape(artifact, operator):
    if artifact.operator_id != operator:
        raise ValueError(f"expected {operator} artifact, got operator {artifact.operator_id}")
    shape = tuple(artifact.specialization.get(key) for key in ("rows", "n", "k", "blocks"))
    expected = peer_layout(operator, *shape)
    if artifact.peer_layout is None:
        raise ValueError("communication artifact has no peer layout; regenerate the manifest")
    if artifact.peer_layout != expected:
        raise ValueError("artifact peer layout does not match operator specialization/argument slots")
    return shape


def _user(builder, slot_index, size, dtype=FP16):
    index = builder.add_binding(BindingSpec(size, 32, dtype, ROW_MAJOR))
    builder.bind_slot(slot_index, Slot(Source.USER, index, bytes=size, alignment=32))


def _peer(builder, own_slot, peer_slot, payload_bytes, layout):
    if payload_bytes > PEER_PAYLOAD_LIMIT:
        raise ValueError("operator payload exceeds peer window")
    region = builder.add_region(Region(0, payload_bytes, 32, RegionSpace.PAYLOAD))
    for slot_index, source in ((own_slot, Source.OWNER_PAYLOAD), (peer_slot, Source.PEER_PAYLOAD)):
        builder.bind_slot(slot_index, Slot(source, region, bytes=payload_bytes, alignment=32))
    builder.add_channel(layout.channel)
    for slot_index, slot in layout.flag_bindings.items():
        builder.bind_slot(slot_index, slot)


def allgather_gemm_plan(artifact):
    rows, n, k, blocks = artifact_shape(artifact, "allgather_gemm")
    if blocks % min(n // 256, blocks):
        raise ValueError("worker groups must contain complete column gangs")
    payload = rows * k * 2
    ready_bytes = artifact.peer_layout.slot_count * 64
    builder = PlanBuilder(workspace_bytes=ready_bytes, peer_payload_bytes=payload)
    for slot, size in enumerate((payload, k * n * 2, 2 * payload, 2 * rows * n * 2)):
        _user(builder, slot, size)
    _peer(builder, 4, 5, payload, artifact.peer_layout)
    ready = builder.add_region(Region(0, ready_bytes, 64, RegionSpace.PRIVATE,
                                      Initialization.ZERO_ONCE))
    builder.bind_slot(8, Slot(Source.WORKSPACE, ready, bytes=ready_bytes, alignment=64))
    return builder.build(artifact, blocks)


def gemm_allreduce_plan(artifact):
    rows, n, k, blocks = artifact_shape(artifact, "gemm_allreduce")
    payload = rows * n * 4
    builder = PlanBuilder(peer_payload_bytes=payload)
    for slot, size in enumerate((rows * k * 2, k * n * 2, rows * n * 2)):
        _user(builder, slot, size)
    _peer(builder, 3, 4, payload, artifact.peer_layout)
    return builder.build(artifact, blocks)
