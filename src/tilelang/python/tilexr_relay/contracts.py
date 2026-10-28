"""Immutable ABI-v1 artifact, binding, memory-region and channel contracts.

These compiler-free values carry checked concrete metadata and build identity;
they neither allocate device memory nor own live execution state. ELF naming
uses the build-time digest under the trusted immutable installation contract.
"""

import hashlib
import json
from dataclasses import asdict, dataclass
from enum import IntEnum
from types import MappingProxyType
from collections.abc import Mapping

VERSION = 1
SLOT_COUNT = 16
FLAG_PLANE_BYTES = 1 << 20
FLAG_SLOT_BYTES = 64
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1


def checked_uint(value, bits=64, name="value"):
    if type(value) is not int or not 0 <= value < (1 << bits):
        raise ValueError(f"{name} must be an unsigned {bits}-bit integer")
    return value


def checked_end(offset, size):
    checked_uint(offset, name="offset")
    checked_uint(size, name="bytes")
    return checked_uint(offset + size, name="region end")


def alignment(value):
    checked_uint(value, 32, "alignment")
    if not 32 <= value <= 4096 or value & (value - 1):
        raise ValueError("alignment must be a power of two in 32..4096")
    return value


def _enum(enum_type, value):
    if type(value) is not int and not isinstance(value, enum_type):
        raise ValueError(f"{enum_type.__name__} requires an integer enum value")
    return enum_type(value)


def canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _freeze(value):
    if isinstance(value, Mapping):
        if any(type(key) is not str for key in value):
            raise ValueError("manifest mapping keys must be strings")
        return MappingProxyType({key: _freeze(item) for key, item in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_freeze(item) for item in value)
    if value is None or type(value) in (str, int, float, bool):
        canonical_json(value)
        return value
    raise ValueError("manifest values must be JSON-compatible")


def _thaw(value):
    if isinstance(value, Mapping):
        return {key: _thaw(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw(item) for item in value]
    return value


class Source(IntEnum):
    UNUSED = 0
    USER = 1
    WORKSPACE = 2
    OWNER_PAYLOAD = 3
    PEER_PAYLOAD = 4
    OWNER_FLAGS = 5
    PEER_FLAGS = 6


class RegionSpace(IntEnum):
    PRIVATE = 0
    PAYLOAD = 1


class Initialization(IntEnum):
    UNINITIALIZED = 0
    ZERO_ONCE = 1


@dataclass(frozen=True)
class BindingSpec:
    bytes: int
    alignment: int = 32
    dtype_tag: int = 0
    layout_tag: int = 0

    def __post_init__(self):
        if checked_uint(self.bytes, name="binding bytes") == 0:
            raise ValueError("binding bytes must be nonzero")
        alignment(self.alignment)
        checked_uint(self.dtype_tag, 32, "dtype tag")
        checked_uint(self.layout_tag, 32, "layout tag")


@dataclass(frozen=True)
class Region:
    offset: int
    bytes: int
    alignment: int = 32
    space: RegionSpace = RegionSpace.PRIVATE
    initialization: Initialization = Initialization.UNINITIALIZED

    def __post_init__(self):
        checked_end(self.offset, self.bytes)
        if self.bytes == 0:
            raise ValueError("region bytes must be nonzero")
        alignment(self.alignment)
        if self.offset % self.alignment:
            raise ValueError("region offset is misaligned")
        object.__setattr__(self, "space", _enum(RegionSpace, self.space))
        object.__setattr__(self, "initialization", _enum(Initialization, self.initialization))
        if self.space == RegionSpace.PAYLOAD and self.initialization != Initialization.UNINITIALIZED:
            raise ValueError("peer payload initialization belongs to the communicator")


@dataclass(frozen=True)
class Channel:
    first_slot: int
    slot_count: int
    participants: int = 2
    max_credits: int = 1

    def __post_init__(self):
        checked_uint(self.first_slot, 32, "first slot")
        checked_uint(self.slot_count, 32, "slot count")
        checked_uint(self.participants, 32, "participants")
        checked_uint(self.max_credits, 32, "max credits")
        if not self.slot_count or self.first_slot + self.slot_count > FLAG_PLANE_BYTES // FLAG_SLOT_BYTES:
            raise ValueError("channel exceeds the fixed flag plane")
        if self.participants != 2 or self.max_credits != 1:
            raise ValueError("Relay v1 requires two participants and one credit")


@dataclass(frozen=True)
class Slot:
    source: Source = Source.UNUSED
    index: int = 0
    offset: int = 0
    bytes: int = 0
    alignment: int = 0

    def __post_init__(self):
        object.__setattr__(self, "source", _enum(Source, self.source))
        checked_uint(self.index, 32, "slot index")
        checked_end(self.offset, self.bytes)
        if self.source == Source.UNUSED:
            if self.index or self.offset or self.bytes or self.alignment:
                raise ValueError("unused slot fields must all be zero")
        else:
            alignment(self.alignment)
            if not self.bytes or self.offset % self.alignment:
                raise ValueError("slot range must be nonempty and aligned")


@dataclass(frozen=True)
class PeerMemoryLayout:
    """Fixed single-channel protocol, shared by planning and device generation."""
    slot_count: int
    owner_slot: int
    peer_slot: int

    def __post_init__(self):
        Channel(0, self.slot_count)
        for slot in (self.owner_slot, self.peer_slot):
            if checked_uint(slot, 32, "flag argument slot") >= SLOT_COUNT:
                raise ValueError("flag argument slot is outside canonical ABI")
        if self.owner_slot == self.peer_slot:
            raise ValueError("owner and peer flags require distinct argument slots")

    @property
    def channel(self):
        return Channel(0, self.slot_count)

    @property
    def ack_slot(self):
        return FLAG_PLANE_BYTES // FLAG_SLOT_BYTES

    @property
    def flag_shape(self):
        return (2 * self.ack_slot, FLAG_SLOT_BYTES // 8)

    @property
    def flag_bindings(self):
        return {index: Slot(source, bytes=2 * FLAG_PLANE_BYTES, alignment=FLAG_SLOT_BYTES)
                for index, source in ((self.owner_slot, Source.OWNER_FLAGS),
                                      (self.peer_slot, Source.PEER_FLAGS))}


@dataclass(frozen=True)
class KernelArtifact:
    operator_id: str
    specialization: Mapping
    target: str
    backend: str
    compiler: Mapping
    sha256: str
    abi_version: int = VERSION
    protocol_version: int = VERSION
    measurement_mode: str = "baseline"
    entry: str = "main_kernel"
    peer_layout: PeerMemoryLayout = None

    def __post_init__(self):
        checked_uint(self.abi_version, 32, "ABI version")
        checked_uint(self.protocol_version, 32, "protocol version")
        if self.abi_version != VERSION or self.protocol_version != VERSION:
            raise ValueError("unsupported artifact ABI/protocol version")
        if self.target != "Ascend910B" or self.backend != "peer_memory":
            raise ValueError("unsupported Relay v1 target/backend")
        if self.entry != "main_kernel":
            raise ValueError("canonical entry must be main_kernel")
        for name in ("operator_id", "measurement_mode", "entry"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value or "\x00" in value:
                raise ValueError(f"invalid {name}")
        if not isinstance(self.sha256, str) or len(self.sha256) != 64 or any(c not in "0123456789abcdef" for c in self.sha256):
            raise ValueError("artifact sha256 must be a lowercase ELF digest")
        if not isinstance(self.specialization, Mapping) or not isinstance(self.compiler, Mapping):
            raise ValueError("specialization/compiler must be mappings")
        object.__setattr__(self, "specialization", _freeze(self.specialization))
        object.__setattr__(self, "compiler", _freeze(self.compiler))
        if self.peer_layout is not None and not isinstance(self.peer_layout, PeerMemoryLayout):
            raise ValueError("artifact requires a PeerMemoryLayout")

    @property
    def file_name(self):
        return f"tilexr_relay_{self.sha256}.o"

    def to_dict(self):
        values = {name: _thaw(getattr(self, name)) for name in self.__dataclass_fields__}
        values["peer_layout"] = asdict(self.peer_layout) if self.peer_layout else None
        return values

    @property
    def identity(self):
        return hashlib.sha256(canonical_json(self.to_dict()).encode()).hexdigest()


@dataclass(frozen=True)
class Binding:
    """A borrowed device address; this value does not own a tensor/allocation."""
    data: int
    bytes: int
    dtype_tag: int = 0
    layout_tag: int = 0

    def __post_init__(self):
        checked_uint(self.data, name="device address")
        checked_uint(self.bytes, name="binding capacity")
        checked_uint(self.dtype_tag, 32, "dtype tag")
        checked_uint(self.layout_tag, 32, "layout tag")
        if not self.data:
            raise ValueError("binding address must be nonnull")
