"""Compiler/framework-free descriptor for the canonical ABI device probe."""

from tilexr_relay.contracts import BindingSpec, KernelArtifact, Slot, Source
from tilexr_relay.planning import PlanBuilder


def echo_plan(metadata):
    artifact = KernelArtifact("abi_echo", {}, "Ascend910B", "peer_memory", metadata,
                              metadata["sha256"])
    builder = PlanBuilder()
    for slot in range(16):
        if slot != 7:
            size = 256 if slot == 0 else 32
            index = builder.add_binding(BindingSpec(size, 32))
            builder.bind_slot(slot, Slot(Source.USER, index, bytes=size, alignment=32))
    return builder.build(artifact, 1)
