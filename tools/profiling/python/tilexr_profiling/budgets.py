# Purpose: Bound cumulative input and retained models before parser allocations.
# 用途与关系：在解析器分配内存前检查累计输入和保留模型预算。
"""Cumulative input and retained-model budgets, shared by all input paths."""

from dataclasses import dataclass, fields
import json
from pathlib import Path
from collections.abc import Mapping

from .model import ProfilingInputError


@dataclass(frozen=True)
class InputLimits:
    max_files: int = 4096
    max_bytes: int = 512 * 1024 * 1024
    max_records: int = 1000000
    max_streams: int = 65536
    max_metrics: int = 65536
    max_events: int = 65536
    max_memory_bytes: int = 1024 * 1024 * 1024
    max_pairing_actions: int = 1000000
    max_samples: int = 1000000

    def __post_init__(self):
        for item in fields(self):
            value = getattr(self, item.name)
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                raise ProfilingInputError(item.name + " must be a positive integer")


class InputBudget:
    """Charge before allocation; estimates deliberately include paired samples.

    This bounds the retained model, not the Python allocator or process RSS.
    Renderers additionally have their own encoded-output limit.
    """

    def __init__(self, limits=None):
        self.limits = limits or InputLimits()
        if not isinstance(self.limits, InputLimits):
            raise ProfilingInputError("limits must be InputLimits")
        self.usage = dict(files=0, bytes=0, records=0, streams=0, metrics=0,
                          events=0, memory_bytes=0, pairing_actions=0, samples=0)

    def charge(self, **counts):
        updated = dict(self.usage)
        for key, count in counts.items():
            if key not in updated or count < 0:
                raise ProfilingInputError("invalid budget charge")
            updated[key] += count
            if updated[key] > getattr(self.limits, "max_" + key):
                raise ProfilingInputError("cumulative profiling " + key + " budget exceeded")
        self.usage = updated

    def manifest(self, document):
        metrics = len(document.get("stage_metrics", []))
        events = len(document.get("events", []))
        participants = len(document.get("participants", []))
        self.charge(metrics=metrics, events=events,
                    memory_bytes=(metrics + events + participants) * 2048)

    def document(self, value):
        if isinstance(value, Mapping):
            size = len(json.dumps(dict(value), ensure_ascii=False, allow_nan=False).encode("utf-8"))
        else:
            size = Path(value).stat().st_size
        self.charge(files=1, bytes=size, memory_bytes=size * 4)
