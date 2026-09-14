"""Producer-neutral analysis for normalized profiling runs."""

import statistics
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from .model import Lane, ProfileRun, StageMetric, StageSample


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
    instance_count: int
    total_us: float
    median_us: float
    minimum_us: float
    maximum_us: float
    critical_lane_us: float
    share_of_critical: float
    bytes: Optional[int]
    byte_semantics: Optional[str]
    bandwidth_gbps: Optional[float]


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


def analyze_run(run: ProfileRun) -> AnalysisResult:
    all_lanes = [
        (launch.id, lane)
        for launch in run.launches
        for lane in launch.lanes
    ]
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

    stage_summaries: Dict[int, StageSummary] = {}
    for stage_id in sorted(by_stage):
        entries = by_stage[stage_id]
        names = {sample.name for _, _, sample in entries}
        categories = {sample.category for _, _, sample in entries}
        parent_stage_ids = {sample.parent_stage_id for _, _, sample in entries}
        if len(names) != 1 or len(categories) != 1 or len(parent_stage_ids) != 1:
            raise ValueError(f"stage {stage_id} has conflicting metadata")
        durations = [sample.duration_us for _, _, sample in entries]
        critical_lane_us = 0.0
        if critical_lane is not None:
            critical_lane_us = sum(
                sample.duration_us
                for launch_id, lane, sample in entries
                if launch_id == critical_lane.launch_id and lane.stream_id == critical_lane.stream_id
            )

        lanes_with_samples: Dict[Tuple[int, int], Tuple[Lane, int, float]] = {}
        for launch_id, lane, sample in entries:
            identity = (launch_id, lane.stream_id)
            previous = lanes_with_samples.get(identity, (lane, 0, 0.0))
            lanes_with_samples[identity] = (
                lane,
                previous[1] + 1,
                previous[2] + sample.duration_us,
            )
        covered_metrics = []
        for lane, interval_count, covered_duration in lanes_with_samples.values():
            metric = _metric_for_stage(lane, stage_id)
            if metric is not None:
                byte_count = metric.bytes * interval_count if metric.scope == "per_interval" else metric.bytes
                covered_metrics.append((metric, byte_count, covered_duration))

        byte_count = None
        byte_semantics = None
        bandwidth = None
        if len(covered_metrics) == len(lanes_with_samples):
            semantics = {metric.byte_semantics for metric, _, _ in covered_metrics}
            if len(semantics) == 1:
                byte_count = sum(item[1] for item in covered_metrics)
                byte_semantics = next(iter(semantics))
                metric_duration = sum(item[2] for item in covered_metrics)
                if metric_duration > 0.0:
                    bandwidth = byte_count / metric_duration / 1000.0

        stage_summaries[stage_id] = StageSummary(
            stage_id=stage_id,
            parent_stage_id=next(iter(parent_stage_ids)),
            name=next(iter(names)),
            category=next(iter(categories)),
            sample_count=len(entries),
            instance_count=len(
                {
                    sample.instance_id
                    for _, _, sample in entries
                    if sample.instance_id is not None
                }
            ),
            total_us=sum(durations),
            median_us=float(statistics.median(durations)),
            minimum_us=min(durations),
            maximum_us=max(durations),
            critical_lane_us=critical_lane_us,
            share_of_critical=(
                critical_lane_us / critical_lane.duration_us
                if critical_lane is not None and critical_lane.duration_us > 0.0
                else 0.0
            ),
            bytes=byte_count,
            byte_semantics=byte_semantics,
            bandwidth_gbps=bandwidth,
        )

    rank_latencies: Dict[int, List[float]] = {}
    core_latencies: Dict[int, List[float]] = {}
    for (launch_id, _), duration in invocation_durations.items():
        rank_latencies.setdefault(launch_id, []).append(duration)
    for launch_id, _, sample in lane_entries:
        core_latencies.setdefault(launch_id, []).append(sample.duration_us)
    rank_imbalance = max((_ratio(values) for values in rank_latencies.values()), default=1.0)
    core_imbalance = max((_ratio(values) for values in core_latencies.values()), default=1.0)

    findings: List[Finding] = []
    non_total = [
        summary
        for summary in stage_summaries.values()
        if summary.category not in {"total", "diagnostic"} and summary.critical_lane_us > 0.0
    ]
    if non_total:
        largest = max(non_total, key=lambda summary: (summary.critical_lane_us, summary.stage_id))
        findings.append(
            Finding(
                code="largest_critical_stage",
                severity="info",
                title="Largest stage on the critical lane",
                evidence=f"{largest.name} consumes {largest.critical_lane_us:.3f} us "
                f"({largest.share_of_critical:.1%} of critical latency)",
                recommendation="Inspect this stage first and compare its rank/core distribution.",
            )
        )

    # Nested waits (or overlapping named instances) cover the same cycles.
    # Count their union on the critical lane instead of adding inclusive times.
    wait_sync_intervals = sorted(
        (sample.raw_start, sample.raw_end)
        for sample in (critical_lane_model.samples if critical_lane_model else ())
        if sample.kind == "interval" and sample.category in {"wait", "sync"}
    )
    covered_cycles = 0
    covered_end = None
    for begin, end in wait_sync_intervals:
        covered_cycles += max(0, end - max(begin, covered_end if covered_end is not None else begin))
        covered_end = max(covered_end if covered_end is not None else end, end)
    wait_sync_share = (
        covered_cycles / run.cycles_per_us / critical_lane.duration_us
        if critical_lane is not None and critical_lane.duration_us > 0.0 and run.cycles_per_us
        else 0.0
    )
    if wait_sync_share >= 0.20:
        findings.append(
            Finding(
                code="wait_sync_pressure",
                severity="warning",
                title="Wait and synchronization pressure",
                evidence=f"wait/sync stages consume {wait_sync_share:.1%} of critical latency",
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
    return AnalysisResult(
        status=run.status,
        latency=latency,
        critical_lane=critical_lane,
        stages=stage_summaries,
        findings=tuple(findings),
        rank_imbalance_ratio=rank_imbalance,
        core_imbalance_ratio=core_imbalance,
    )
