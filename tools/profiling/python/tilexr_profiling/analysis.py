# Purpose: Compute coverage-aware stage metrics from paired captures for reports.
# 用途与关系：从配对后的 capture 计算带覆盖范围的阶段指标，供报表使用。
"""Producer-neutral analysis for normalized profiling runs."""

import math
import statistics
from dataclasses import dataclass, field, fields, is_dataclass
from typing import Dict, List, Optional, Tuple

from .model import Lane, ProfileRun, ProfilingInputError, StageMetric, StageSample


@dataclass(frozen=True)
class LatencySummary:
    minimum_us: Optional[float]
    median_us: Optional[float]
    maximum_us: Optional[float]


@dataclass(frozen=True)
class CriticalLane:
    launch_id: int
    stream_id: int
    rank: int
    host: Optional[str]
    device: Optional[str]
    core: int
    duration_us: float


@dataclass(frozen=True)
class StageSummary:
    stage_id: int
    parent_stage_id: Optional[int]
    name: str
    category: str
    sample_count: int
    instance_count: Optional[int]
    total_us: Optional[float]
    median_us: Optional[float]
    minimum_us: Optional[float]
    maximum_us: Optional[float]
    critical_lane_us: Optional[float]
    share_of_critical: Optional[float]
    bytes: Optional[int]
    byte_semantics: Optional[str]
    bandwidth_gbps: Optional[float]
    union_us: Optional[float]
    exclusive_us: Optional[float]
    critical_lane_union_us: Optional[float]
    critical_lane_exclusive_us: Optional[float]
    union_share_of_critical: Optional[float]
    exclusive_share_of_critical: Optional[float]
    states: Dict[str, dict] = field(default_factory=dict)
    rank_latency: dict = field(default_factory=dict)
    rank_bandwidth: dict = field(default_factory=dict)
    instance_coverage: dict = field(default_factory=dict)
    bandwidth_basis: str = "lane-time effective bandwidth"


@dataclass(frozen=True)
class Finding:
    code: str
    severity: str
    title: str
    evidence: str
    recommendation: str


@dataclass(frozen=True)
class AnalysisResult:
    status: str
    latency: LatencySummary
    critical_lane: Optional[CriticalLane]
    stages: Dict[int, StageSummary]
    findings: Tuple[Finding, ...]
    rank_imbalance_ratio: float
    core_imbalance_ratio: float
    wait_sync_pressure: Optional[float]
    rank_coverage: dict = field(default_factory=dict)


def _union(intervals: List[Tuple[int, int]]) -> List[Tuple[int, int]]:
    """Merge half-open integer timer intervals, including adjacent intervals."""
    merged: List[Tuple[int, int]] = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((start, end))
    return merged


def _length(intervals: List[Tuple[int, int]]) -> int:
    return sum(end - start for start, end in intervals)


def _intersection_length(left: List[Tuple[int, int]], right: List[Tuple[int, int]]) -> int:
    total = 0
    i = j = 0
    while i < len(left) and j < len(right):
        total += max(0, min(left[i][1], right[j][1]) - max(left[i][0], right[j][0]))
        if left[i][1] <= right[j][1]:
            i += 1
        else:
            j += 1
    return total


def _coverage(lane: Lane) -> Optional[Dict[int, List[Tuple[int, int]]]]:
    critical = _critical_sample(lane)
    if critical is None:
        return None
    by_stage: Dict[int, List[Tuple[int, int]]] = {}
    for sample in lane.samples:
        intervals = by_stage.setdefault(sample.stage_id, [])
        if sample.kind != "instant":
            intervals.append((max(sample.raw_start, critical.raw_start),
                              min(sample.raw_end, critical.raw_end)))
    return {stage_id: _union(intervals) for stage_id, intervals in by_stage.items()}


def _critical_sample(lane: Lane) -> Optional[StageSample]:
    samples = [sample for sample in lane.samples if sample.critical]
    return samples[0] if len(samples) == 1 else None


def _ratio(values: List[float]) -> float:
    positive = [value for value in values if value > 0.0]
    if len(positive) < 2:
        return 1.0
    return max(positive) / min(positive)


def _metric_for_stage(lane: Lane, stage_id: int) -> Optional[StageMetric]:
    return next((metric for metric in lane.stage_metrics if metric.stage_id == stage_id), None)


def _rank_distribution(values, expected_ranks, partial_ranks=(), *, bandwidth=False):
    """Summarize local rank measurements without inventing a common clock."""
    partial = set(partial_ranks)
    valid = {rank: value for rank, value in values.items()
             if rank not in partial and rank in expected_ranks}
    numbers = list(valid.values())
    covered = sorted(valid)
    slowest_rank = None
    if valid:
        select = min if bandwidth else max
        slowest_rank = select(valid, key=lambda rank: (valid[rank], rank))
    return {
        "minimum": min(numbers) if numbers else None,
        "median": float(statistics.median(numbers)) if numbers else None,
        "maximum": max(numbers) if numbers else None,
        "covered_ranks": len(covered),
        "expected_ranks": len(expected_ranks),
        "status": "known" if set(covered) == set(expected_ranks) and covered else "incomplete",
        "slowest_rank": slowest_rank,
        "per_rank": {str(rank): {"value": values.get(rank),
                                 "state": "incomplete" if rank in partial else
                                 "known" if rank in valid else "not_observed"}
                     for rank in sorted(set(expected_ranks) | set(values))},
    }


def _lane_complete(lane):
    return lane.attributes.get("capture_status", "complete") == "complete"


def _sampled(lane):
    return lane.attributes.get("sampled", True) and lane.attributes.get("capture_status") != "not_sampled"


def _stage_bytes(stage_id, lanes_with_samples, all_lanes, run, expected_ranks, partial_ranks):
    """Keep byte and inclusive-time populations identical, including repeats."""
    applicable = []
    missing = False
    not_applicable = []
    rank_totals = {}
    rank_invalid = set(partial_ranks)
    for lane, interval_count, duration in lanes_with_samples.values():
        metric = _metric_for_stage(lane, stage_id)
        if metric is None:
            missing = True
            rank_invalid.add(lane.rank)
            continue
        if metric.attributes.get("status") == "not_applicable":
            not_applicable.append(metric.attributes.get("reason", "Explicitly not applicable."))
            continue
        count = metric.bytes * interval_count if metric.scope == "per_interval" else metric.bytes
        if count > (1 << 64) - 1:
            raise ProfilingInputError("stage byte aggregation exceeds uint64 range")
        applicable.append((metric, count, duration))
        totals = rank_totals.setdefault(lane.rank, [0, 0.0, set()])
        totals[0] += count
        totals[1] += duration
        totals[2].add(metric.byte_semantics)

    # An absent interval is not proof that this lane was inapplicable. Require
    # an explicit N/A declaration before excluding a sampled lane's workload.
    for launch_id, lane in all_lanes:
        metric = _metric_for_stage(lane, stage_id)
        if (_sampled(lane) and (launch_id, lane.stream_id) not in lanes_with_samples
                and (metric is None or metric.attributes.get("status") != "not_applicable")):
            missing = True
            rank_invalid.add(lane.rank)
    semantics = {metric.byte_semantics for metric, _, _ in applicable}
    conflict = len(semantics) > 1
    rank_rates = {}
    for rank, (count, duration, meanings) in rank_totals.items():
        if len(meanings) == 1 and duration > 0 and not conflict:
            rank_rates[rank] = count / duration / 1000.0
        else:
            rank_invalid.add(rank)
    rank_declarations = {}
    for _, lane in all_lanes:
        if _sampled(lane):
            rank_declarations.setdefault(lane.rank, []).append(_metric_for_stage(lane, stage_id))
    inapplicable_ranks = {rank for rank, declarations in rank_declarations.items()
                          if declarations and all(metric is not None and
                              metric.attributes.get("status") == "not_applicable" for metric in declarations)}
    rank_summary = _rank_distribution(rank_rates, set(expected_ranks) - inapplicable_ranks,
                                      rank_invalid - inapplicable_ranks, bandwidth=True)
    rank_summary["not_applicable_ranks"] = sorted(inapplicable_ranks)
    rank_summary["planned_ranks"] = len(expected_ranks)
    for rank in inapplicable_ranks:
        rank_summary["per_rank"][str(rank)] = {"value": None, "state": "not_applicable"}
    count = meaning = bandwidth = None
    if not lanes_with_samples:
        declarations = [_metric_for_stage(lane, stage_id) for _, lane in all_lanes if _sampled(lane)]
        if declarations and all(metric is not None and metric.attributes.get("status") == "not_applicable"
                                for metric in declarations):
            state, reason = "not_applicable", "; ".join(sorted({metric.attributes["reason"] for metric in declarations}))
        else:
            state, reason = "not_observed", "No paired stage samples; declared bytes cannot imply measured traffic."
    elif conflict:
        state, reason = "incomplete", "Byte semantics differ across applicable lanes."
    elif not_applicable and not applicable and not missing:
        state, reason = "not_applicable", "; ".join(sorted(set(not_applicable)))
    elif run.status != "complete":
        state, reason = "incomplete", "Capture loss or missing lanes prevents pairing a full byte total with a full duration."
    elif missing:
        state, reason = "not_provided", "A metric or its matching intervals are missing for a participating lane."
    elif applicable:
        count = sum(value for _, value, _ in applicable)
        if count > (1 << 64) - 1:
            raise ProfilingInputError("stage byte aggregation exceeds uint64 range")
        meaning = next(iter(semantics))
        duration = sum(value for _, _, value in applicable)
        state, reason = "known", "Declared bytes and paired inclusive durations cover the same lane intervals."
        if duration > 0:
            bandwidth = count / duration / 1000.0
    else:
        state, reason = "not_provided", "No stage byte metric was provided."
    byte_state = {name: {"state": state, "reason": reason} for name in ("bytes", "byte_semantics", "bandwidth_gbps")}
    if count is not None and bandwidth is None:
        byte_state["bandwidth_gbps"] = {"state": "undefined_denominator", "reason": "Inclusive duration is zero."}
    return count, meaning, bandwidth, byte_state, rank_summary


def _require_finite_statistics(value, path="statistics") -> None:
    """Finite samples may still overflow sums, averages, or ratios."""
    if isinstance(value, float) and not math.isfinite(value):
        raise ProfilingInputError(f"{path} exceeds finite statistical range")
    if is_dataclass(value):
        for field in fields(value):
            _require_finite_statistics(getattr(value, field.name), f"{path}.{field.name}")
    elif isinstance(value, dict):
        for key, item in value.items():
            _require_finite_statistics(item, f"{path}.{key}")
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            _require_finite_statistics(item, f"{path}[{index}]")


def analyze_run(run: ProfileRun) -> AnalysisResult:
    all_lanes = [
        (launch.id, lane)
        for launch in run.launches
        for lane in launch.lanes
    ]
    coverage = {(launch_id, lane.stream_id): _coverage(lane) for launch_id, lane in all_lanes}
    # No absolute timestamp is converted to float while computing coverage.
    cycles_per_us = run.cycles_per_us
    if cycles_per_us is None or not math.isfinite(cycles_per_us) or cycles_per_us <= 0:
        raise ProfilingInputError("cycle analysis requires finite positive cycles_per_us")
    lane_entries = [
        (launch_id, lane, critical)
        for launch_id, lane in all_lanes
        for critical in [_critical_sample(lane)]
        if critical is not None
    ]

    invocation_durations: Dict[Tuple[int, int], float] = {}
    for launch_id, lane, sample in lane_entries:
        identity = (launch_id, lane.rank)
        invocation_durations[identity] = max(
            invocation_durations.get(identity, 0.0), sample.duration_us
        )
    critical_durations = list(invocation_durations.values())
    expected_ranks = (set(run.coverage["expected_ranks"]) if run.coverage.get("verified")
                      else {lane.rank for _, lane in all_lanes if _sampled(lane)})
    partial_ranks = {lane.rank for _, lane in all_lanes
                     if _sampled(lane) and not _lane_complete(lane)}
    partial_ranks.update(sid >> 16 for sid in run.coverage.get("unexpected_lanes", ()))

    critical_lane: Optional[CriticalLane] = None
    critical_lane_model: Optional[Lane] = None
    if lane_entries:
        critical_launch_id, critical_lane_model, critical_sample = max(
            lane_entries,
            key=lambda item: (
                item[2].duration_us,
                item[1].rank,
                item[1].core,
                item[0],
                item[1].stream_id,
            ),
        )
        critical_lane = CriticalLane(
            launch_id=critical_launch_id,
            stream_id=critical_lane_model.stream_id,
            rank=critical_lane_model.rank,
            host=critical_lane_model.host,
            device=critical_lane_model.device,
            core=critical_lane_model.core,
            duration_us=critical_sample.duration_us,
        )

    by_stage: Dict[int, List[Tuple[int, Lane, StageSample]]] = {}
    for launch_id, lane in all_lanes:
        for sample in lane.samples:
            by_stage.setdefault(sample.stage_id, []).append((launch_id, lane, sample))
    definitions = {stage.id: stage for stage in run.stages}
    for stage_id in definitions:
        by_stage.setdefault(stage_id, [])

    stage_summaries: Dict[int, StageSummary] = {}
    for stage_id in sorted(by_stage):
        entries = by_stage[stage_id]
        names = {sample.name for _, _, sample in entries}
        categories = {sample.category for _, _, sample in entries}
        parent_stage_ids = {sample.parent_stage_id for _, _, sample in entries}
        if not entries:
            definition = definitions[stage_id]
            names = {definition.name}
            categories = {definition.category}
            parent_stage_ids = {definition.parent_stage_id}
        if len(names) != 1 or len(categories) != 1 or len(parent_stage_ids) != 1:
            raise ValueError(f"stage {stage_id} has conflicting metadata")
        durations = [sample.duration_us for _, _, sample in entries]
        critical_lane_us = None
        if critical_lane is not None:
            critical_samples = [
                sample.duration_us
                for launch_id, lane, sample in entries
                if launch_id == critical_lane.launch_id and lane.stream_id == critical_lane.stream_id
            ]
            critical_lane_us = sum(critical_samples) if critical_samples else None

        lane_keys = {(launch_id, lane.stream_id) for launch_id, lane, _ in entries}
        children = {sample.stage_id for _, lane in all_lanes for sample in lane.samples
                    if sample.parent_stage_id == stage_id}
        lane_coverage = {}
        for key in lane_keys:
            stage_coverage = coverage[key]
            if stage_coverage is None:
                lane_coverage[key] = None
                continue
            intervals = stage_coverage.get(stage_id, [])
            child_intervals = _union([interval for child in children
                                      for interval in stage_coverage.get(child, [])])
            union_cycles = _length(intervals)
            exclusive_cycles = union_cycles - _intersection_length(intervals, child_intervals)
            lane_coverage[key] = (union_cycles / cycles_per_us, exclusive_cycles / cycles_per_us)
        known_coverage = bool(lane_coverage) and all(value is not None for value in lane_coverage.values())
        union_us = sum(value[0] for value in lane_coverage.values()) if known_coverage else None
        exclusive_us = sum(value[1] for value in lane_coverage.values()) if known_coverage else None
        critical_coverage = None
        if critical_lane is not None:
            critical_coverage = lane_coverage.get((critical_lane.launch_id, critical_lane.stream_id))
        critical_union = critical_coverage[0] if critical_coverage is not None else None
        critical_exclusive = critical_coverage[1] if critical_coverage is not None else None
        denominator = critical_lane.duration_us if critical_lane is not None else 0.0

        lanes_with_samples = {}
        for launch_id, lane, sample in entries:
            key = (launch_id, lane.stream_id)
            previous = lanes_with_samples.get(key, (lane, 0, 0.0))
            lanes_with_samples[key] = (
                lane,
                previous[1] + 1,
                previous[2] + sample.duration_us,
            )
        byte_count, byte_semantics, bandwidth, byte_state, rank_bandwidth = _stage_bytes(
            stage_id, lanes_with_samples, all_lanes, run, expected_ranks, partial_ranks)
        rank_unions = {}
        stage_partial_ranks = set(partial_ranks)
        for key, (lane, _, _) in lanes_with_samples.items():
            value = lane_coverage.get(key)
            if value is None:
                stage_partial_ranks.add(lane.rank)
            else:
                rank_unions[lane.rank] = max(rank_unions.get(lane.rank, 0.0), value[0])
        instances = {sample.instance_id for _, _, sample in entries if sample.instance_id is not None}
        definition = definitions.get(stage_id)
        configured_instances = definition.attributes.get("configured_instance_count", 0) if definition else 0
        instance_count = len(instances) if instances or configured_instances else None
        # The three output formats consume these reasons and never reconstruct them.
        states = {}
        timing_fields = {
            "total_us": sum(durations) if durations else None,
            "median_us": float(statistics.median(durations)) if durations else None,
            "minimum_us": min(durations) if durations else None,
            "maximum_us": max(durations) if durations else None,
            "critical_lane_us": critical_lane_us,
            "union_us": union_us, "exclusive_us": exclusive_us,
            "critical_lane_union_us": critical_union,
            "critical_lane_exclusive_us": critical_exclusive,
        }
        for name, value in timing_fields.items():
            if value is None:
                states[name] = {"state": "not_observed", "reason":
                                "No paired stage samples or no unique critical interval in the required lane."}
            elif run.status != "complete":
                states[name] = {"state": "incomplete", "reason": "Observed samples only; capture coverage is incomplete."}
            else:
                states[name] = {"state": "known", "reason": "Computed from paired samples."}
        for name, value in (("share_of_critical", critical_lane_us),
                            ("union_share_of_critical", critical_union),
                            ("exclusive_share_of_critical", critical_exclusive)):
            states[name] = ({"state": "undefined_denominator", "reason": "A positive unique critical duration is required."}
                            if denominator <= 0 else
                            {"state": "not_observed", "reason": "No stage sample on the critical lane."}
                            if value is None else
                            {"state": "incomplete" if run.status != "complete" else "known",
                             "reason": "Ratio uses the critical lane duration."})
        states["sample_count"] = {"state": "known", "reason": "Number of observed paired samples."}
        states["instance_count"] = {"state": "known" if instance_count is not None else "not_applicable",
                                    "reason": "Observed explicit instance IDs only; ordinary repeats are not instances."}
        states.update(byte_state)

        stage_summaries[stage_id] = StageSummary(
            stage_id=stage_id,
            parent_stage_id=next(iter(parent_stage_ids)),
            name=next(iter(names)),
            category=next(iter(categories)),
            sample_count=len(entries),
            instance_count=instance_count,
            total_us=timing_fields["total_us"],
            median_us=timing_fields["median_us"],
            minimum_us=timing_fields["minimum_us"],
            maximum_us=timing_fields["maximum_us"],
            critical_lane_us=critical_lane_us,
            share_of_critical=(
                critical_lane_us / critical_lane.duration_us
                if critical_lane_us is not None and denominator > 0.0
                else None
            ),
            bytes=byte_count,
            byte_semantics=byte_semantics,
            bandwidth_gbps=bandwidth,
            union_us=union_us,
            exclusive_us=exclusive_us,
            critical_lane_union_us=critical_union,
            critical_lane_exclusive_us=critical_exclusive,
            union_share_of_critical=critical_union / denominator if denominator > 0 and critical_union is not None else None,
            exclusive_share_of_critical=critical_exclusive / denominator if denominator > 0 and critical_exclusive is not None else None,
            states=states,
            rank_latency=_rank_distribution(rank_unions, expected_ranks, stage_partial_ranks),
            rank_bandwidth=rank_bandwidth,
            instance_coverage={"configured": configured_instances or None, "observed": instance_count},
        )

    core_latencies = [sample.duration_us for _, _, sample in lane_entries]
    rank_imbalance = _ratio(list(invocation_durations.values()))
    core_imbalance = _ratio(core_latencies)

    findings: List[Finding] = []
    non_total = [
        summary
        for summary in stage_summaries.values()
        if summary.category not in {"total", "diagnostic"} and (summary.critical_lane_union_us or 0.0) > 0.0
    ]
    if non_total:
        largest = max(non_total, key=lambda summary: (summary.critical_lane_union_us, summary.stage_id))
        findings.append(
            Finding(
                code="largest_critical_stage",
                severity="info",
                title="Largest stage on the critical lane",
                evidence=f"{largest.name} covers {largest.critical_lane_union_us:.3f} us "
                f"({largest.union_share_of_critical:.1%} of critical latency; interval union)",
                recommendation="Inspect this stage first and compare its rank/core distribution.",
            )
        )

    wait_sync_share = None
    if critical_lane is not None and critical_lane.duration_us > 0.0:
        critical = _critical_sample(critical_lane_model)
        wait_intervals = [(max(sample.raw_start, critical.raw_start), min(sample.raw_end, critical.raw_end))
                          for sample in critical_lane_model.samples
                          if sample.kind != "instant" and sample.category in {"wait", "sync"}]
        wait_sync_share = _length(_union(wait_intervals)) / (critical.raw_end - critical.raw_start)
    if wait_sync_share is not None and wait_sync_share >= 0.20:
        findings.append(
            Finding(
                code="wait_sync_pressure",
                severity="warning",
                title="Wait and synchronization pressure",
                evidence=f"wait/sync interval union covers {wait_sync_share:.1%} of critical latency",
                recommendation="Compare peer progress and synchronization balance before tuning compute work.",
            )
        )
    if rank_imbalance >= 1.20:
        findings.append(
            Finding(
                code="rank_imbalance",
                severity="warning",
                title="Rank latency imbalance",
                evidence=f"slowest/fastest rank latency ratio is {rank_imbalance:.2f}x",
                recommendation="Inspect route, byte, and wait distributions across ranks.",
            )
        )
    if core_imbalance >= 1.50:
        findings.append(
            Finding(
                code="core_imbalance",
                severity="warning",
                title="Core latency imbalance",
                evidence=f"slowest/fastest core latency ratio is {core_imbalance:.2f}x",
                recommendation="Inspect stage ownership and work partitioning across active cores.",
            )
        )
    if run.status == "incomplete":
        codes = ", ".join(sorted({diagnostic.code for diagnostic in run.diagnostics})) or "unknown"
        findings.append(
            Finding(
                code="incomplete_profile",
                severity="incomplete",
                title="Profiling data is incomplete",
                evidence=f"capture diagnostics: {codes}",
                recommendation="Resolve missing, dropped, overflowed, or unmatched records before final conclusions.",
            )
        )
    if not lane_entries:
        findings.append(
            Finding(
                code="no_usable_critical_interval",
                severity="incomplete",
                title="No usable critical interval",
                evidence="no stream contains exactly one complete critical interval",
                recommendation="Check critical event IDs, capture capacity, and stream completeness.",
            )
        )

    latency = LatencySummary(None, None, None)
    if critical_durations:
        latency = LatencySummary(
            minimum_us=min(critical_durations),
            median_us=float(statistics.median(critical_durations)),
            maximum_us=max(critical_durations),
        )
    result = AnalysisResult(
        status=run.status,
        latency=latency,
        critical_lane=critical_lane,
        stages=stage_summaries,
        findings=tuple(findings),
        rank_imbalance_ratio=rank_imbalance,
        core_imbalance_ratio=core_imbalance,
        wait_sync_pressure=wait_sync_share,
        rank_coverage=_rank_distribution(
            {rank: duration for (_, rank), duration in invocation_durations.items()},
            expected_ranks, partial_ranks),
    )
    _require_finite_statistics(result)
    return result
