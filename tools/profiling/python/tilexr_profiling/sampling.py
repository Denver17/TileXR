# Purpose: Select producer event and lane subsets without erasing expected coverage.
# 用途与关系：选择 producer 的事件和 lane 子集，同时保留预期覆盖范围。
"""Execution-before-capture sampling plans; no inference from received files."""

import copy
from dataclasses import dataclass

from .manifest import _document, _uint, load_manifest, mapping_fingerprint
from .mapping import _array, _known_keys, _required_keys, _string, load_mapping
from .model import ProfilingInputError

SAMPLING_SCHEMA = "tilexr_profiling_sampling.v1"


def _unique_uints(value, where):
    values = _array(value, where)
    if len(values) > 65536:
        raise ProfilingInputError(f"{where} exceeds item budget")
    result = tuple(_uint(item, where) for item in values)
    if len(set(result)) != len(result):
        raise ProfilingInputError(f"duplicate {where}")
    return tuple(sorted(result))


@dataclass(frozen=True)
class SamplingSelection:
    """One immutable plan decision, to be consumed before instrumentation runs."""

    iteration: int
    participants: tuple
    selected_stream_ids: tuple
    stage_names: tuple
    event_ids: tuple
    mapping: dict
    policy: dict
    original_mapping: dict

    def should_record(self, event_id):
        return event_id in self.event_ids

    def cpp_header(self):
        """C++14 predicate for a caller's pre-marker guard, independent of ABI."""
        cases = "\n".join(f"    case {value}u: return true;" for value in self.event_ids)
        return ("#pragma once\n#include <cstdint>\n"
                "#if defined(__CCE__) && defined(__CCE_IS_AICORE__)\n"
                "#define TILEXR_PROFILING_POLICY_FUNCTION __aicore__\n"
                "#else\n#define TILEXR_PROFILING_POLICY_FUNCTION\n#endif\n"
                "TILEXR_PROFILING_POLICY_FUNCTION inline constexpr bool TileXRProfilingEventSelected(std::uint32_t eventId) {\n"
                "  switch (eventId) {\n" + cases +
                "\n    default: return false;\n  }\n}\n"
                "#undef TILEXR_PROFILING_POLICY_FUNCTION\n")

    def apply_manifest(self, manifest):
        """Bind a pre-execution manifest to this selection and remap stage IDs.

        This updates declarations only. The producer must still consume the
        selected sparse IDs and predicate before recording any markers.
        """
        result = load_manifest(manifest, self.original_mapping)
        expected = {item["stream_id"]: item for item in result["participants"]}
        selected = {item["stream_id"]: item for item in self.participants}
        if set(expected) != set(selected):
            raise ProfilingInputError("sampling selection differs from manifest expected participants")
        for sid, item in expected.items():
            before = {key: value for key, value in item.items() if key != "sampled"}
            after = {key: value for key, value in selected[sid].items() if key != "sampled"}
            if before != after or selected[sid]["sampled"] and not item["sampled"]:
                raise ProfilingInputError("sampling selection contradicts manifest identity or prior exclusion")
            item["sampled"] = selected[sid]["sampled"]
        stage_ids = {stage.name: stage.id for stage in load_mapping(self.mapping).stages}
        result["stage_metrics"] = [dict(metric, stage_id=stage_ids[metric["stage"]])
                                   for metric in result["stage_metrics"] if metric["stage"] in stage_ids]
        if "execution" in result and "stages" in result["execution"]:
            result["execution"]["stages"] = [dict(row, stage_id=stage_ids[row["stage"]])
                                              for row in result["execution"]["stages"] if row["stage"] in stage_ids]
        result["mapping_sha256"] = mapping_fingerprint(self.mapping)
        result.setdefault("attributes", {})["sampling"] = {"policy": copy.deepcopy(self.policy),
                                                           "iteration": self.iteration}
        return load_manifest(result, self.mapping)


@dataclass(frozen=True)
class SamplingPolicy:
    source: str
    iterations: object = None
    stream_ids: object = None
    stages: object = None

    @classmethod
    def from_document(cls, value):
        document = _document(value)
        _known_keys(document, {"schema", "source", "iterations", "stream_ids", "stages"}, "sampling policy")
        _required_keys(document, {"schema", "source"}, "sampling policy")
        if document["schema"] != SAMPLING_SCHEMA:
            raise ProfilingInputError("unsupported sampling schema")
        source = _string(document["source"], "sampling.source")
        stages = None
        if "stages" in document:
            stages = tuple(_string(item, "sampling stage") for item in _array(document["stages"], "stages"))
            if len(stages) > 4096 or len(set(stages)) != len(stages):
                raise ProfilingInputError("too many or duplicate sampling stages")
        return cls(source,
                   _unique_uints(document["iterations"], "iterations") if "iterations" in document else None,
                   _unique_uints(document["stream_ids"], "stream_ids") if "stream_ids" in document else None,
                   stages)

    def to_document(self):
        result = {"schema": SAMPLING_SCHEMA, "source": self.source}
        for name in ("iterations", "stream_ids", "stages"):
            value = getattr(self, name)
            if value is not None:
                result[name] = list(value)
        return result

    def select(self, mapping, participants, iteration):
        """Retain expected participants; excluded lanes explicitly have sampled=false.

        Producers must guard markers with the selected event IDs and allocate only
        the selected logical lanes. A filtered mapping must never be applied to
        previously recorded, unfiltered inputs.
        """
        _uint(iteration, "iteration")
        SamplingPolicy.from_document(self.to_document())
        raw = _document(mapping)
        parsed = load_mapping(raw)
        names = {stage.name for stage in parsed.stages}
        selected = set(names if self.stages is None else self.stages)
        if selected - names:
            raise ProfilingInputError("sampling policy references unknown stage")
        selected.update(stage.name for stage in parsed.stages if stage.critical)
        parents = {stage["name"]: stage.get("parent") for stage in raw["stages"]}
        for name in tuple(selected):
            parent = parents[name]
            while parent is not None:
                selected.add(parent)
                parent = parents[parent]
        filtered = dict(raw, stages=[stage for stage in raw["stages"] if stage["name"] in selected])
        # One physical marker can serve several stage/instance boundaries.
        # Keeping an ID keeps every occurrence, including predecessors of an nth selector.
        events = tuple(sorted({event.id for event in load_mapping(filtered).events}))
        values = _array(participants, "participants")
        if not 1 <= len(values) <= 65536:
            raise ProfilingInputError("sampling requires 1..65536 participants")
        result = []
        seen = set()
        enabled = self.iterations is None or iteration in self.iterations
        for participant in values:
            if not isinstance(participant, dict):
                raise ProfilingInputError("sampling participant must be an object")
            sid = _uint(participant.get("stream_id"), "participant.stream_id")
            _uint(participant.get("rank"), "participant.rank", 65535)
            _uint(participant.get("logical_lane"), "participant.logical_lane", 65535)
            if sid in seen:
                raise ProfilingInputError("duplicate sampling participant")
            seen.add(sid)
            if participant.get("rank") != sid >> 16 or participant.get("logical_lane") != sid & 65535:
                raise ProfilingInputError("sampling participant identity mismatch")
            prior = participant.get("sampled", True)
            if not isinstance(prior, bool):
                raise ProfilingInputError("participant.sampled must be a boolean")
            item = copy.deepcopy(participant)
            item["sampled"] = prior and enabled and (self.stream_ids is None or sid in self.stream_ids)
            result.append(item)
        if self.stream_ids is not None and set(self.stream_ids) - seen:
            raise ProfilingInputError("sampling policy references undeclared stream")
        return SamplingSelection(iteration, tuple(result),
                                 tuple(sorted(p["stream_id"] for p in result if p["sampled"])),
                                 tuple(stage["name"] for stage in filtered["stages"]),
                                 events, filtered, self.to_document(), raw)
