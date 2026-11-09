# Purpose: Define capture, lane and stage data shared by parsers and reports.
# 用途与关系：定义解析器和报表共享的 capture、lane 与 stage 数据。
"""Immutable, producer-neutral profiling data structures."""

from dataclasses import dataclass, field
from typing import Any, Dict, Optional, Tuple
from uuid import UUID


MAPPING_SCHEMA = "tilexr_profiling_mapping.v1"
MAPPING_SCHEMA_V2 = "tilexr_profiling_mapping.v2"
SUPPORTED_MAPPING_SCHEMAS = frozenset({MAPPING_SCHEMA, MAPPING_SCHEMA_V2})
RECORDS_SCHEMA = "tilexr_profiling_records.v1"
RECORDS_MAGIC = b"TXRPRF1\0"

STAGE_CATEGORIES = frozenset(
    {
        "total",
        "setup",
        "copy",
        "issue",
        "wait",
        "sync",
        "compute",
        "reduce",
        "finalize",
        "diagnostic",
        "other",
    }
)


class ProfilingInputError(ValueError):
    """Raised when profiling input is unsafe, corrupt, or contradictory."""


@dataclass(frozen=True)
class Diagnostic:
    severity: str
    code: str
    message: str
    source: Optional[str] = None
    details: Dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class StageDefinition:
    id: int
    name: str
    category: str
    kind: str
    critical: bool
    attributes: Dict[str, Any] = field(default_factory=dict)
    parent_stage_id: Optional[int] = None


@dataclass(frozen=True)
class EventDefinition:
    id: int
    stage_id: int
    edge: str
    instance_id: Optional[int] = None
    occurrence: Optional[int] = None

    def selector(self):
        if self.occurrence is None:
            return self.id
        return {"event": self.id, "occurrence": self.occurrence}


@dataclass(frozen=True)
class StageMetric:
    stage_id: int
    bytes: Optional[int]
    byte_semantics: str
    scope: str
    attributes: Dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class StreamDefinition:
    id: int
    launch_id: int
    rank: int
    host: Optional[str]
    device: Optional[str]
    core: int
    attributes: Dict[str, Any]
    stage_metrics: Tuple[StageMetric, ...]


@dataclass(frozen=True)
class ProfilingMapping:
    operator: str
    cycles_per_us: float
    stages: Tuple[StageDefinition, ...]
    events: Tuple[EventDefinition, ...]
    source: str
    schema: str = MAPPING_SCHEMA


@dataclass(frozen=True)
class RawRecord:
    timestamp: int
    event_id: int
    source: str
    source_index: int


@dataclass(frozen=True)
class RecordStream:
    id: int
    records: Tuple[RawRecord, ...]
    dropped_count: int
    complete: bool
    overflow: bool
    source: str


@dataclass(frozen=True)
class RecordFile:
    capture_id: UUID
    streams: Tuple[RecordStream, ...]
    source: str


@dataclass(frozen=True)
class StageSample:
    name: str
    stage_id: int
    parent_stage_id: Optional[int]
    instance_id: Optional[int]
    category: str
    kind: str
    raw_start: int
    raw_end: int
    start_us: float
    end_us: float
    duration_us: float
    critical: bool
    begin_event_id: int
    end_event_id: Optional[int]
    attributes: Dict[str, Any] = field(default_factory=dict)
    source: Optional[str] = None
    source_index: Optional[int] = None
    end_source_index: Optional[int] = None


@dataclass(frozen=True)
class Lane:
    stream_id: int
    rank: int
    host: Optional[str]
    device: Optional[str]
    core: int
    samples: Tuple[StageSample, ...]
    records: Tuple[RawRecord, ...]
    stage_metrics: Tuple[StageMetric, ...]
    attributes: Dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class Launch:
    id: int
    lanes: Tuple[Lane, ...]
    attributes: Dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class ProfileRun:
    capture_id: str
    run_id: str
    producer: str
    operator: str
    timing_unit: str
    cycles_per_us: Optional[float]
    clock_domain: str
    timestamp_source: str
    marker_overhead_us: Optional[float]
    status: str
    launches: Tuple[Launch, ...]
    diagnostics: Tuple[Diagnostic, ...]
    attributes: Dict[str, Any]
    sources: Tuple[str, ...]
    expected_stream_count: int
    present_stream_count: int
    usable_stream_count: int
    missing_stream_count: int
    dropped_stream_count: int
    overflowed_stream_count: int
    excluded_stream_count: int
    dropped_record_count: int
    display_origin: str = "per_launch_rank_device_minimum"
    completeness_scope: str = "provided_inputs_only"
    stages: Tuple[StageDefinition, ...] = ()
    health_status: str = "unverified"
    metadata_readiness: str = "not_provided"
    coverage: Dict[str, Any] = field(default_factory=dict)
    capture_events: Tuple[Dict[str, Any], ...] = ()
    manifest_metadata: Dict[str, Any] = field(default_factory=dict)
    mapping_schema: str = MAPPING_SCHEMA
    pairing_sha256: Optional[str] = None
