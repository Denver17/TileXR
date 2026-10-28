"""Build and serialize immutable plans from concrete Relay resource contracts.

Validate live-region/channel separation, capacities and finite ABI slot
bindings without operator formulas, compiler imports or device allocations.
Mutable generations and invocation addresses are execution-time concerns.
"""

import hashlib
import json
from dataclasses import asdict, dataclass, field

from .contracts import (VERSION, SLOT_COUNT, FLAG_PLANE_BYTES,
                        BindingSpec, Channel, KernelArtifact, PeerMemoryLayout, Region, RegionSpace,
                        Slot, Source, canonical_json, checked_uint)


def _disjoint(intervals, label):
    intervals = sorted(intervals)
    for previous, current in zip(intervals, intervals[1:]):
        if current[0] < previous[1]:
            raise ValueError(f"overlapping {label}")


@dataclass(frozen=True)
class RelayPlan:
    artifact: KernelArtifact
    blocks: int
    binding_specs: tuple = ()
    regions: tuple = ()
    channels: tuple = ()
    slots: tuple = field(default_factory=lambda: (Slot(),) * SLOT_COUNT)
    workspace_bytes: int = 0
    peer_payload_bytes: int = 0
    rank_count: int = 2

    def __post_init__(self):
        if not isinstance(self.artifact, KernelArtifact):
            raise ValueError("plan requires a KernelArtifact")
        if not checked_uint(self.blocks, 32, "blocks") or self.rank_count != 2:
            raise ValueError("Relay v1 requires positive blocks and two ranks")
        checked_uint(self.workspace_bytes, name="workspace bytes")
        checked_uint(self.peer_payload_bytes, name="peer payload bytes")
        checked_uint(self.peer_payload_bytes + 2 * FLAG_PLANE_BYTES, name="peer mapping bytes")
        checked_uint(self.rank_count, 32, "rank count")
        for name, item_type in (("binding_specs", BindingSpec), ("regions", Region),
                                ("channels", Channel), ("slots", Slot)):
            values = tuple(getattr(self, name))
            if any(not isinstance(item, item_type) for item in values):
                raise ValueError(f"invalid {name} type")
            checked_uint(len(values), 32, name)
            object.__setattr__(self, name, values)
        if len(self.slots) != SLOT_COUNT:
            raise ValueError("canonical ABI requires exactly sixteen slots")
        if len(self.binding_specs) > 16 or len(self.regions) > 64 or len(self.channels) > 16384:
            raise ValueError("descriptor resource limit exceeded")
        for space, capacity in ((RegionSpace.PRIVATE, self.workspace_bytes),
                                (RegionSpace.PAYLOAD, self.peer_payload_bytes)):
            regions = [r for r in self.regions if r.space == space]
            if any(r.offset + r.bytes > capacity for r in regions):
                raise ValueError("region exceeds declared capacity")
            _disjoint([(r.offset, r.offset + r.bytes) for r in regions], "live regions")
        layout = self.artifact.peer_layout
        flags = {i: slot for i, slot in enumerate(self.slots)
                 if slot.source in (Source.OWNER_FLAGS, Source.PEER_FLAGS)}
        if layout is None:
            if self.channels or flags:
                raise ValueError("communication artifact has no peer layout; regenerate the manifest")
        elif self.channels != (layout.channel,) or flags != layout.flag_bindings:
            raise ValueError("channels/flag bindings do not match artifact peer layout")
        for slot in self.slots:
            self._validate_slot(slot)

    def _validate_slot(self, slot):
        if slot.source == Source.UNUSED:
            return
        if slot.source == Source.USER:
            if slot.index >= len(self.binding_specs):
                raise ValueError("slot references missing binding")
            bound = self.binding_specs[slot.index]
        elif slot.source in (Source.WORKSPACE, Source.OWNER_PAYLOAD, Source.PEER_PAYLOAD):
            if slot.index >= len(self.regions):
                raise ValueError("slot references missing region")
            bound = self.regions[slot.index]
            wanted = RegionSpace.PRIVATE if slot.source == Source.WORKSPACE else RegionSpace.PAYLOAD
            if bound.space != wanted:
                raise ValueError("slot references incorrect region space")
        else:
            # Flag bindings were checked exactly against the artifact layout.
            return
        if slot.offset + slot.bytes > bound.bytes or slot.alignment > bound.alignment:
            raise ValueError("slot exceeds source capacity/alignment")

    def to_dict(self):
        return {"version": VERSION, "artifact": self.artifact.to_dict(),
                "blocks": self.blocks, "rank_count": self.rank_count,
                "workspace_bytes": self.workspace_bytes,
                "peer_payload_bytes": self.peer_payload_bytes,
                **{name: [asdict(item) for item in getattr(self, name)]
                   for name in ("binding_specs", "regions", "channels", "slots")}}

    def to_json(self):
        return canonical_json(self.to_dict())

    @classmethod
    def from_json(cls, text):
        data = json.loads(text)
        if not isinstance(data, dict) or type(data.get("version")) is not int or data.pop("version", None) != VERSION:
            raise ValueError("unsupported manifest version")
        try:
            if not isinstance(data["artifact"], dict):
                raise ValueError("artifact must be a manifest object")
            layout = data["artifact"].get("peer_layout")
            if layout is not None:
                data["artifact"]["peer_layout"] = PeerMemoryLayout(**layout)
            data["artifact"] = KernelArtifact(**data["artifact"])
            for name, item_type in (("binding_specs", BindingSpec), ("regions", Region),
                                    ("channels", Channel), ("slots", Slot)):
                data[name] = tuple(item_type(**item) for item in data[name])
            return cls(**data)
        except (KeyError, TypeError) as error:
            raise ValueError("malformed Relay plan manifest") from error

    @property
    def identity(self):
        return hashlib.sha256(self.to_json().encode()).hexdigest()


class PlanBuilder:
    """A small mutable builder; build produces an independently immutable plan."""

    def __init__(self, *, workspace_bytes=0, peer_payload_bytes=0):
        self.workspace_bytes = workspace_bytes
        self.peer_payload_bytes = peer_payload_bytes
        self.binding_specs = []
        self.regions = []
        self.channels = []
        self.slots = [Slot() for _ in range(SLOT_COUNT)]

    def add_binding(self, binding):
        self.binding_specs.append(binding)
        return len(self.binding_specs) - 1

    def add_region(self, region):
        self.regions.append(region)
        return len(self.regions) - 1

    def add_channel(self, channel):
        self.channels.append(channel)
        return len(self.channels) - 1

    def bind_slot(self, index, slot):
        if type(index) is not int or not 0 <= index < SLOT_COUNT:
            raise ValueError("slot index is outside canonical ABI")
        if self.slots[index].source != Source.UNUSED:
            raise ValueError("slot already assigned")
        self.slots[index] = slot
        return self

    def build(self, artifact, blocks):
        return RelayPlan(artifact, blocks, self.binding_specs, self.regions,
                         self.channels, self.slots, self.workspace_bytes,
                         self.peer_payload_bytes)
