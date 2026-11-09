# Purpose: Validate stage mappings consumed by record pairing.
# 用途与关系：校验 stage mapping，为记录配对提供事件与阶段定义。
"""Strict parser for the human-authored TileXR profiling event mapping."""

import json
import hashlib
import math
from dataclasses import asdict
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Tuple, Union

from .model import (
    EventDefinition,
    MAPPING_SCHEMA,
    MAPPING_SCHEMA_V2,
    SUPPORTED_MAPPING_SCHEMAS,
    ProfilingInputError,
    ProfilingMapping,
    STAGE_CATEGORIES,
    StageDefinition,
)


MAX_MAPPING_BYTES = 1024 * 1024
MAX_STAGES = 4096
MAX_EVENTS = 65536
UINT32_MAX = (1 << 32) - 1

MappingInput = Union[str, Path, Mapping[str, Any]]

_TOP_LEVEL_KEYS = frozenset({"schema", "operator", "cycles_per_us", "stages"})
_STAGE_KEYS = frozenset(
    {
        "name",
        "category",
        "parent",
        "track",
        "critical",
        "begin",
        "end",
        "instant",
        "instances",
        "instance_range",
    }
)
_INSTANCE_KEYS = frozenset({"begin", "end"})
_INSTANCE_RANGE_KEYS = frozenset({"count", "begin", "end", "stride"})


def _object(value: Any, where: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ProfilingInputError(f"{where} must be an object")
    return value


def _array(value: Any, where: str) -> List[Any]:
    if not isinstance(value, list):
        raise ProfilingInputError(f"{where} must be an array")
    return value


def _known_keys(value: Mapping[str, Any], allowed: Iterable[str], where: str) -> None:
    if any(not isinstance(key, str) for key in value):
        raise ProfilingInputError(f"{where} keys must be strings")
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise ProfilingInputError(f"unknown field(s) in {where}: {', '.join(unknown)}")


def _required_keys(value: Mapping[str, Any], required: Iterable[str], where: str) -> None:
    missing = sorted(set(required) - set(value))
    if missing:
        raise ProfilingInputError(f"missing required field(s) in {where}: {', '.join(missing)}")


def _string(value: Any, where: str) -> str:
    if not isinstance(value, str) or not value:
        raise ProfilingInputError(f"{where} must be a non-empty string")
    return value


def _uint32(value: Any, where: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value <= 0
        or value > UINT32_MAX
    ):
        raise ProfilingInputError(f"{where} must be a nonzero uint32")
    return value


def _positive_number(value: Any, where: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ProfilingInputError(f"{where} must be a positive number")
    try:
        result = float(value)
    except OverflowError as exc:
        raise ProfilingInputError(f"{where} must be a finite positive number") from exc
    if not math.isfinite(result) or result <= 0.0:
        raise ProfilingInputError(f"{where} must be a finite positive number")
    return result


def _enum(value: Any, allowed: Iterable[str], where: str) -> str:
    result = _string(value, where)
    if result not in allowed:
        raise ProfilingInputError(f"{where} is unsupported: {result}")
    return result


def _read_mapping_bytes(path: Path) -> bytes:
    source = path.as_posix()
    try:
        size = path.stat().st_size
        if size > MAX_MAPPING_BYTES:
            raise ProfilingInputError(
                f"profiling mapping {source} exceeds maximum size of {MAX_MAPPING_BYTES} bytes"
            )
        with path.open("rb") as stream:
            data = stream.read(MAX_MAPPING_BYTES + 1)
    except OSError as exc:
        raise ProfilingInputError(f"cannot read mapping {source}: {exc}") from exc
    if len(data) > MAX_MAPPING_BYTES:
        raise ProfilingInputError(
            f"profiling mapping {source} exceeds maximum size of {MAX_MAPPING_BYTES} bytes"
        )
    return data


def _reject_duplicate_keys(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProfilingInputError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _reject_nonfinite_constant(token: str) -> None:
    raise ProfilingInputError(f"non-finite JSON number: {token}")


def _load_json(path: Path) -> Mapping[str, Any]:
    source = path.as_posix()
    try:
        value = json.loads(
            _read_mapping_bytes(path).decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_nonfinite_constant,
        )
    except RecursionError as exc:
        raise ProfilingInputError(f"cannot parse mapping {source}: nesting is too deep") from exc
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ProfilingInputError(f"cannot parse mapping {source}: {exc}") from exc
    return _object(value, source)


def _append_event(
    events: List[EventDefinition],
    event_ids: set,
    event_id: int,
    stage_id: int,
    edge: str,
    instance_id: Any,
    where: str,
    schema: str = MAPPING_SCHEMA,
    occurrence: Any = None,
) -> None:
    if schema == MAPPING_SCHEMA and event_id in event_ids:
        raise ProfilingInputError(f"duplicate event ID {event_id} in {where}")
    if len(events) >= MAX_EVENTS:
        raise ProfilingInputError(f"profiling mapping exceeds {MAX_EVENTS} events")
    event_ids.add(event_id)
    events.append(EventDefinition(event_id, stage_id, edge, instance_id, occurrence))


def _endpoint(value, where, schema):
    if schema == MAPPING_SCHEMA_V2 and isinstance(value, dict):
        _known_keys(value, {"event", "occurrence"}, where)
        _required_keys(value, {"event", "occurrence"}, where)
        return (_uint32(value["event"], f"{where}.event"),
                _uint32(value["occurrence"], f"{where}.occurrence"))
    return _uint32(value, where), None


def parse_mapping_document(raw: Mapping[str, Any], source: str = "<memory>") -> ProfilingMapping:
    obj = _object(dict(raw), source)
    _known_keys(obj, _TOP_LEVEL_KEYS, source)
    _required_keys(obj, _TOP_LEVEL_KEYS, source)
    if not isinstance(obj["schema"], str) or obj["schema"] not in SUPPORTED_MAPPING_SCHEMAS:
        raise ProfilingInputError(f"unsupported mapping schema in {source}: {obj['schema']!r}")

    raw_stages = _array(obj["stages"], f"{source}.stages")
    if not raw_stages:
        raise ProfilingInputError(f"{source}.stages must not be empty")
    if len(raw_stages) > MAX_STAGES:
        raise ProfilingInputError(f"{source}.stages exceeds maximum count {MAX_STAGES}")

    stage_rows = []
    names = set()
    for index, raw_stage in enumerate(raw_stages):
        where = f"{source}.stages[{index}]"
        stage_obj = _object(raw_stage, where)
        _known_keys(stage_obj, _STAGE_KEYS, where)
        _required_keys(stage_obj, {"name", "category"}, where)
        name = _string(stage_obj["name"], f"{where}.name")
        if name in names:
            raise ProfilingInputError(f"duplicate stage name {name!r} in {source}")
        names.add(name)
        critical = stage_obj.get("critical", False)
        if not isinstance(critical, bool):
            raise ProfilingInputError(f"{where}.critical must be a boolean")
        parent = stage_obj.get("parent")
        if parent is not None:
            parent = _string(parent, f"{where}.parent")
        track = stage_obj.get("track")
        if track is not None:
            track = _string(track, f"{where}.track")

        has_interval = "begin" in stage_obj or "end" in stage_obj
        has_instant = "instant" in stage_obj
        has_instances = "instances" in stage_obj
        has_instance_range = "instance_range" in stage_obj
        if sum((has_interval, has_instant, has_instances, has_instance_range)) != 1:
            raise ProfilingInputError(
                f"{where} must define exactly one of begin/end, instant, instances, "
                "or instance_range"
            )
        if has_interval:
            _required_keys(stage_obj, {"begin", "end"}, where)
            kind = "interval"
        elif has_instances or has_instance_range:
            kind = "interval"
        else:
            kind = "instant"
        if critical and kind != "interval":
            raise ProfilingInputError(f"instant stage {name!r} cannot be critical")
        if critical and parent is not None:
            raise ProfilingInputError(f"critical stage {name!r} cannot have a parent")
        stage_rows.append((stage_obj, name, parent, critical, kind, where))

    if sum(1 for _, _, _, critical, _, _ in stage_rows if critical) != 1:
        raise ProfilingInputError(f"{source}.stages must contain exactly one critical stage")

    name_to_id = {row[1]: index for index, row in enumerate(stage_rows)}
    stages = []
    for stage_id, (_, name, parent_name, critical, kind, where) in enumerate(stage_rows):
        parent_id = None
        if parent_name is not None:
            parent_id = name_to_id.get(parent_name)
            if parent_id is None:
                raise ProfilingInputError(
                    f"{where}.parent refers to unknown stage {parent_name!r}"
                )
            if parent_id == stage_id:
                raise ProfilingInputError(f"stage {name!r} cannot be its own parent")
            if stage_rows[parent_id][4] != "interval":
                raise ProfilingInputError(
                    f"stage {name!r} parent {parent_name!r} must be an interval"
                )
        stages.append(
            StageDefinition(
                id=stage_id,
                name=name,
                category=_enum(stage_rows[stage_id][0]["category"], STAGE_CATEGORIES,
                    f"{where}.category"),
                kind=kind,
                critical=critical,
                attributes=(
                    {}
                    if stage_rows[stage_id][0].get("track") is None
                    else {"track": stage_rows[stage_id][0]["track"]}
                ),
                parent_stage_id=parent_id,
            )
        )

    for stage in stages:
        ancestors = set()
        current = stage
        while current.parent_stage_id is not None:
            if current.parent_stage_id in ancestors:
                raise ProfilingInputError(
                    f"stage hierarchy contains a cycle at stage {stage.name!r}"
                )
            ancestors.add(current.parent_stage_id)
            current = stages[current.parent_stage_id]

    events: List[EventDefinition] = []
    event_ids = set()

    def append_pair(begin_value, end_value, stage_id, instance_id, where):
        begin, begin_occurrence = _endpoint(begin_value, f"{where}.begin", obj["schema"])
        end, end_occurrence = _endpoint(end_value, f"{where}.end", obj["schema"])
        if obj["schema"] == MAPPING_SCHEMA_V2 and begin == end:
            raise ProfilingInputError(f"{where} begin/end must use distinct event IDs")
        _append_event(events, event_ids, begin, stage_id, "begin", instance_id, source,
                      obj["schema"], begin_occurrence)
        _append_event(events, event_ids, end, stage_id, "end", instance_id, source,
                      obj["schema"], end_occurrence)

    for stage_id, (stage_obj, _, _, _, _, where) in enumerate(stage_rows):
        if "instant" in stage_obj:
            event_id = _uint32(stage_obj["instant"], f"{where}.instant")
            _append_event(events, event_ids, event_id, stage_id, "instant", None, source, obj["schema"])
            continue
        if "instances" in stage_obj:
            instances = _array(stage_obj["instances"], f"{where}.instances")
            if not instances:
                raise ProfilingInputError(f"{where}.instances must not be empty")
            for instance_id, raw_instance in enumerate(instances):
                instance_where = f"{where}.instances[{instance_id}]"
                instance = _object(raw_instance, instance_where)
                _known_keys(instance, _INSTANCE_KEYS, instance_where)
                _required_keys(instance, _INSTANCE_KEYS, instance_where)
                append_pair(instance["begin"], instance["end"], stage_id, instance_id, instance_where)
            continue
        if "instance_range" in stage_obj:
            range_where = f"{where}.instance_range"
            instance_range = _object(stage_obj["instance_range"], range_where)
            _known_keys(instance_range, _INSTANCE_RANGE_KEYS, range_where)
            _required_keys(instance_range, _INSTANCE_RANGE_KEYS, range_where)
            count = _uint32(instance_range["count"], f"{range_where}.count")
            begin_base = _uint32(instance_range["begin"], f"{range_where}.begin")
            end_base = _uint32(instance_range["end"], f"{range_where}.end")
            stride = _uint32(instance_range["stride"], f"{range_where}.stride")
            if count > MAX_EVENTS // 2:
                raise ProfilingInputError(f"{range_where}.count exceeds event capacity")
            for instance_id in range(count):
                begin = begin_base + instance_id * stride
                end = end_base + instance_id * stride
                if begin > UINT32_MAX or end > UINT32_MAX:
                    raise ProfilingInputError(f"{range_where} event ID exceeds uint32")
                append_pair(begin, end, stage_id, instance_id, range_where)
            continue
        append_pair(stage_obj["begin"], stage_obj["end"], stage_id, None, where)

    return ProfilingMapping(
        operator=_string(obj["operator"], f"{source}.operator"),
        cycles_per_us=_positive_number(obj["cycles_per_us"], f"{source}.cycles_per_us"),
        stages=tuple(stages),
        events=tuple(events),
        source=source,
        schema=obj["schema"],
    )


def load_mapping(value: MappingInput) -> ProfilingMapping:
    """Load one event mapping from a path or in-memory mapping."""
    if isinstance(value, Mapping):
        return parse_mapping_document(value)
    path = Path(value)
    return parse_mapping_document(_load_json(path), path.as_posix())


def pairing_fingerprint(mapping: ProfilingMapping) -> str:
    """Identify boundaries independently of capture paths and clock conversion."""
    contract = {"schema": mapping.schema, "stages": [asdict(stage) for stage in mapping.stages],
                "events": [asdict(event) for event in mapping.events]}
    encoded = json.dumps(contract, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()
