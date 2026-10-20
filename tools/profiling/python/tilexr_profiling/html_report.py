"""Markdown and self-contained HTML reporting."""

import html
import json
import string
from typing import Iterable, Optional

from .analysis import AnalysisResult, Finding, StageSummary
from .model import Diagnostic, ProfileRun
from .provenance import build_provenance, provenance_items

_MARKDOWN_PUNCTUATION = frozenset(string.punctuation) - {"&", ";", "`"}


def _markdown_cell(value: object) -> str:
    escaped = html.escape(str(value), quote=False).replace("`", "&#96;")
    return "".join(
        f"\\{character}" if character in _MARKDOWN_PUNCTUATION else character
        for character in escaped.replace("\n", " ").replace("\r", " ")
    )


def _attribute_value(value: object) -> str:
    if isinstance(value, str):
        return value
    return json.dumps(value, allow_nan=False, ensure_ascii=True, sort_keys=True)


def _fmt_us(value: Optional[float]) -> str:
    return "unavailable" if value is None else f"{value:.3f}"


def _coverage_us(value: Optional[float]) -> str:
    return "unknown" if value is None else f"{value:.3f}"


def _share(value: Optional[float]) -> str:
    return "unknown" if value is None else f"{value:.1%}"


def render_analysis_markdown(run: ProfileRun, analysis: AnalysisResult) -> str:
    provenance = build_provenance(run)
    lines = [
        "# TileXR Profiling Analysis",
        "",
        "## Provenance",
        "",
        f"- Status: `{run.status}`",
        f"- Capture: {_markdown_cell(run.capture_id)}",
        f"- Run: {_markdown_cell(run.run_id)}",
        f"- Producer: {_markdown_cell(run.producer)}",
        f"- Operator: {_markdown_cell(run.operator)}",
        f"- Timestamp source: {_markdown_cell(run.timestamp_source)}",
        f"- Timing: `{run.timing_unit}` / `{run.clock_domain}`",
        f"- Cycles per us: `{run.cycles_per_us if run.cycles_per_us is not None else 'not applicable'}`",
        f"- Marker overhead (us): `{_fmt_us(run.marker_overhead_us)}`",
        f"- Streams: expected `{run.expected_stream_count}`, present `{run.present_stream_count}`, usable `{run.usable_stream_count}`",
        f"- Stream counts: missing `{run.missing_stream_count}`, dropped `{run.dropped_stream_count}`, "
        f"overflowed `{run.overflowed_stream_count}`, excluded `{run.excluded_stream_count}`",
        f"- Dropped records: `{run.dropped_record_count}`",
        f"- Input files: `{len(run.sources)}`",
        "",
        "### Run Attributes",
        "",
    ]
    if run.attributes:
        lines.extend(["| Attribute | Value |", "|---|---|"])
        for key in sorted(run.attributes):
            lines.append(
                f"| {_markdown_cell(key)} | {_markdown_cell(_attribute_value(run.attributes[key]))} |"
            )
    else:
        lines.append("No producer attributes were reported.")
    lines.extend(["", "### Common Provenance", "", "| Field | Value |", "|---|---|"])
    for key, value in provenance_items(provenance):
        lines.append(f"| {_markdown_cell(key)} | {_markdown_cell(_attribute_value(value))} |")

    critical = "unavailable"
    if analysis.critical_lane is not None:
        critical = (
            f"launch {analysis.critical_lane.launch_id}, stream {analysis.critical_lane.stream_id}, "
            f"rank {analysis.critical_lane.rank}, host {analysis.critical_lane.host or 'unknown'}, "
            f"device {analysis.critical_lane.device or 'unknown'}, core {analysis.critical_lane.core}"
        )
    lines.extend(
        [
            "",
            "## Latency",
            "",
            "| Minimum (us) | Median (us) | Maximum (us) | Critical lane |",
            "|---:|---:|---:|---|",
            f"| {_fmt_us(analysis.latency.minimum_us)} | {_fmt_us(analysis.latency.median_us)} | "
            f"{_fmt_us(analysis.latency.maximum_us)} | {critical} |",
            "",
            f"- Rank imbalance: `{analysis.rank_imbalance_ratio:.2f}x`",
            f"- Core imbalance: `{analysis.core_imbalance_ratio:.2f}x`",
            f"- Wait/sync union pressure: `{_share(analysis.wait_sync_pressure)}`",
            "",
            "## Bottlenecks",
            "",
        ]
    )
    if analysis.findings:
        for finding in analysis.findings:
            lines.extend(
                [
                    f"### {_markdown_cell(finding.title)}",
                    "",
                    f"- Code: `{finding.code}`",
                    f"- Severity: `{finding.severity}`",
                    f"- Evidence: {_markdown_cell(finding.evidence)}",
                    f"- Recommendation: {_markdown_cell(finding.recommendation)}",
                    "",
                ]
            )
    else:
        lines.extend(["No material bottleneck finding was produced.", ""])

    lines.extend(
        [
            "## Stage Breakdown",
            "",
            "| ID | Parent | Stage | Category | Samples | Instances | Inclusive sum (us) | Median (us) | Min (us) | Max (us) | Critical inclusive (us) | Inclusive share | Union (us) | Exclusive (us) | Critical union (us) | Critical exclusive (us) | Union share | Exclusive share | Bytes | Byte semantics | Bandwidth (GB/s) |",
            "|---:|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|",
        ]
    )
    for stage in analysis.stages.values():
        bandwidth = "unknown" if stage.bandwidth_gbps is None else f"{stage.bandwidth_gbps:.3f}"
        byte_count = "unknown" if stage.bytes is None else str(stage.bytes)
        parent = "root"
        if stage.parent_stage_id is not None:
            parent_stage = analysis.stages.get(stage.parent_stage_id)
            parent = (
                str(stage.parent_stage_id)
                if parent_stage is None
                else f"{stage.parent_stage_id}: {parent_stage.name}"
            )
        instances = "-" if stage.instance_count == 0 else str(stage.instance_count)
        lines.append(
            f"| {stage.stage_id} | {_markdown_cell(parent)} | {_markdown_cell(stage.name)} | "
            f"{stage.category} | {stage.sample_count} | {instances} | "
            f"{stage.total_us:.3f} | {stage.median_us:.3f} | "
            f"{stage.minimum_us:.3f} | {stage.maximum_us:.3f} | {stage.critical_lane_us:.3f} | "
            f"{stage.share_of_critical:.1%} | {_coverage_us(stage.union_us)} | {_coverage_us(stage.exclusive_us)} | "
            f"{_coverage_us(stage.critical_lane_union_us)} | {_coverage_us(stage.critical_lane_exclusive_us)} | "
            f"{_share(stage.union_share_of_critical)} | {_share(stage.exclusive_share_of_critical)} | "
            f"{byte_count} | {stage.byte_semantics or 'unknown'} | "
            f"{bandwidth} |"
        )

    lines.extend(["", "## Diagnostics", ""])
    if run.diagnostics:
        lines.extend(
            [
                "| Severity | Code | Message | Source | Details |",
                "|---|---|---|---|---|",
            ]
        )
        for diagnostic in run.diagnostics:
            lines.append(
                f"| {diagnostic.severity} | {_markdown_cell(diagnostic.code)} | "
                f"{_markdown_cell(diagnostic.message)} | {_markdown_cell(diagnostic.source or '')} | "
                f"{_markdown_cell(_attribute_value(diagnostic.details))} |"
            )
    else:
        lines.append("No capture diagnostics were reported.")
    lines.append("")
    return "\n".join(lines)


def _e(value: object) -> str:
    return html.escape(str(value), quote=True)


def _finding_rows(findings: Iterable[Finding]) -> str:
    rows = []
    for finding in findings:
        rows.append(
            "<tr>"
            f'<td><span class="severity severity-{_e(finding.severity)}">{_e(finding.severity)}</span></td>'
            f"<td><strong>{_e(finding.title)}</strong><small>{_e(finding.code)}</small></td>"
            f"<td>{_e(finding.evidence)}</td><td>{_e(finding.recommendation)}</td>"
            "</tr>"
        )
    return "".join(rows) or '<tr><td colspan="4">No material bottleneck finding was produced.</td></tr>'


def _stage_rows(stages: Iterable[StageSummary]) -> str:
    stage_list = list(stages)
    stages_by_id = {stage.stage_id: stage for stage in stage_list}
    rows = []
    for stage in stage_list:
        bandwidth = "unknown" if stage.bandwidth_gbps is None else f"{stage.bandwidth_gbps:.3f}"
        byte_count = "unknown" if stage.bytes is None else str(stage.bytes)
        parent = "root"
        if stage.parent_stage_id is not None:
            parent_stage = stages_by_id.get(stage.parent_stage_id)
            parent = (
                str(stage.parent_stage_id)
                if parent_stage is None
                else f"{stage.parent_stage_id}: {parent_stage.name}"
            )
        instances = "-" if stage.instance_count == 0 else str(stage.instance_count)
        rows.append(
            "<tr>"
            f"<td>{stage.stage_id}</td><td>{_e(parent)}</td>"
            f"<td><strong>{_e(stage.name)}</strong></td>"
            f"<td>{_e(stage.category)}</td><td>{stage.sample_count}</td><td>{instances}</td>"
            f"<td>{stage.total_us:.3f}</td><td>{stage.median_us:.3f}</td>"
            f"<td>{stage.minimum_us:.3f}</td><td>{stage.maximum_us:.3f}</td>"
            f"<td>{stage.critical_lane_us:.3f}</td><td>{stage.share_of_critical:.1%}</td>"
            f"<td>{_coverage_us(stage.union_us)}</td><td>{_coverage_us(stage.exclusive_us)}</td>"
            f"<td>{_coverage_us(stage.critical_lane_union_us)}</td><td>{_coverage_us(stage.critical_lane_exclusive_us)}</td>"
            f"<td>{_share(stage.union_share_of_critical)}</td><td>{_share(stage.exclusive_share_of_critical)}</td>"
            f"<td>{byte_count}</td><td>{_e(stage.byte_semantics or 'unknown')}</td><td>{bandwidth}</td>"
            "</tr>"
        )
    return "".join(rows) or '<tr><td colspan="21">No complete stage interval is available.</td></tr>'


def _diagnostic_rows(diagnostics: Iterable[Diagnostic]) -> str:
    rows = []
    for diagnostic in diagnostics:
        rows.append(
            "<tr>"
            f"<td>{_e(diagnostic.severity)}</td><td>{_e(diagnostic.code)}</td>"
            f"<td>{_e(diagnostic.message)}</td><td>{_e(diagnostic.source or '')}</td>"
            f"<td>{_e(_attribute_value(diagnostic.details))}</td>"
            "</tr>"
        )
    return "".join(rows) or '<tr><td colspan="5">No capture diagnostics were reported.</td></tr>'


def _metadata_rows(run: ProfileRun) -> str:
    return "".join(
        "<tr>" f"<td>{_e(key)}</td><td>{_e(_attribute_value(value))}</td>" "</tr>"
        for key, value in provenance_items(build_provenance(run))
    )


def render_report_html(run: ProfileRun, analysis: AnalysisResult) -> str:
    lane_rows = []
    for launch in run.launches:
        for lane in launch.lanes:
            critical = [sample for sample in lane.samples if sample.critical]
            usable = len(critical) == 1
            duration = f"{critical[0].duration_us:.3f}" if usable else "unavailable"
            lane_rows.append(
                "<tr>"
                f"<td>{launch.id}</td><td>{lane.stream_id}</td><td>{lane.rank}</td>"
                f"<td>{_e(lane.host or 'unknown')}</td><td>{_e(lane.device or 'unknown')}</td>"
                f"<td>{lane.core}</td><td>{'yes' if usable else 'no'}</td><td>{duration}</td>"
                "</tr>"
            )

    critical_label = "unavailable"
    if analysis.critical_lane is not None:
        critical_label = (
            f"R{analysis.critical_lane.rank} / D{analysis.critical_lane.device or 'unknown'} / "
            f"C{analysis.critical_lane.core}"
        )

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TileXR Profiling Report</title>
<style>
:root {{ color-scheme:light; --ink:#18212b; --muted:#607080; --line:#d8dee5; --panel:#f6f8fa; --accent:#087e8b; --warn:#9a6700; --bad:#b42318; }}
* {{ box-sizing:border-box; }}
body {{ margin:0; color:var(--ink); background:#fff; font:14px/1.45 Arial,sans-serif; }}
header {{ border-bottom:1px solid var(--line); padding:22px 28px 18px; }}
header h1 {{ margin:0 0 4px; font-size:24px; letter-spacing:0; }}
header p {{ margin:0; color:var(--muted); }}
main {{ max-width:1440px; margin:0 auto; padding:20px 28px 40px; }}
section {{ margin:0 0 28px; overflow-x:auto; }}
h2 {{ margin:0 0 10px; font-size:17px; letter-spacing:0; }}
.summary {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(170px,1fr)); border:1px solid var(--line); border-radius:6px; }}
.metric {{ min-height:86px; padding:14px; border-right:1px solid var(--line); }}
.metric:last-child {{ border-right:0; }}
.metric small,td small {{ display:block; color:var(--muted); margin-top:3px; }}
.metric strong {{ display:block; margin-top:7px; font-size:18px; overflow-wrap:anywhere; }}
table {{ width:100%; border-collapse:collapse; table-layout:auto; }}
th,td {{ border-bottom:1px solid var(--line); padding:9px 10px; text-align:left; vertical-align:top; }}
th {{ background:var(--panel); color:#394958; font-size:12px; }}
.severity {{ display:inline-block; padding:2px 6px; border-radius:4px; background:var(--panel); }}
.severity-warning {{ color:var(--warn); }} .severity-incomplete,.severity-error {{ color:var(--bad); }}
.toolbar {{ display:flex; justify-content:space-between; align-items:center; gap:12px; margin-bottom:10px; }}
a {{ color:var(--accent); }}
@media (max-width:720px) {{ main,header {{ padding-left:14px; padding-right:14px; }} .summary {{ display:block; }} .metric {{ border-right:0; border-bottom:1px solid var(--line); }} }}
</style>
</head>
<body>
<header><h1>TileXR Profiling Report</h1><p>{_e(run.producer)} / {_e(run.operator)} / run {_e(run.run_id)}</p></header>
<main>
<section><div class="toolbar"><h2>Profiling Summary</h2><a href="perfetto_trace.json">Open Perfetto trace</a></div>
<div class="summary">
<div class="metric"><small>Status</small><strong>{_e(run.status)}</strong></div>
<div class="metric"><small>Usable streams</small><strong>{run.usable_stream_count} / {run.expected_stream_count}</strong></div>
<div class="metric"><small>Minimum latency</small><strong>{_fmt_us(analysis.latency.minimum_us)} us</strong></div>
<div class="metric"><small>Median latency</small><strong>{_fmt_us(analysis.latency.median_us)} us</strong></div>
<div class="metric"><small>Maximum latency</small><strong>{_fmt_us(analysis.latency.maximum_us)} us</strong></div>
<div class="metric"><small>Critical lane</small><strong>{_e(critical_label)}</strong></div>
<div class="metric"><small>Rank imbalance</small><strong>{analysis.rank_imbalance_ratio:.2f}x</strong></div>
<div class="metric"><small>Core imbalance</small><strong>{analysis.core_imbalance_ratio:.2f}x</strong></div>
<div class="metric"><small>Wait/sync union pressure</small><strong>{_share(analysis.wait_sync_pressure)}</strong></div>
</div></section>
<section><h2>Run Metadata</h2><table><thead><tr><th>Attribute</th><th>Value</th></tr></thead><tbody>{_metadata_rows(run)}</tbody></table></section>
<section><h2>Bottlenecks</h2><table><thead><tr><th>Severity</th><th>Finding</th><th>Evidence</th><th>Recommendation</th></tr></thead><tbody>{_finding_rows(analysis.findings)}</tbody></table></section>
<section><h2>Rank / Core Comparison</h2><table><thead><tr><th>Launch</th><th>Stream</th><th>Rank</th><th>Host</th><th>Device</th><th>Core</th><th>Usable</th><th>Critical latency (us)</th></tr></thead><tbody>{''.join(lane_rows) or '<tr><td colspan="8">No stream data is available.</td></tr>'}</tbody></table></section>
<section><h2>Stage Breakdown</h2><table><thead><tr><th>ID</th><th>Parent</th><th>Stage</th><th>Category</th><th>Samples</th><th>Instances</th><th>Inclusive sum (us)</th><th>Median (us)</th><th>Min (us)</th><th>Max (us)</th><th>Critical inclusive (us)</th><th>Inclusive share</th><th>Union (us)</th><th>Exclusive (us)</th><th>Critical union (us)</th><th>Critical exclusive (us)</th><th>Union share</th><th>Exclusive share</th><th>Bytes</th><th>Byte semantics</th><th>GB/s</th></tr></thead><tbody>{_stage_rows(analysis.stages.values())}</tbody></table></section>
<section><h2>Diagnostics</h2><table><thead><tr><th>Severity</th><th>Code</th><th>Message</th><th>Source</th><th>Details</th></tr></thead><tbody>{_diagnostic_rows(run.diagnostics)}</tbody></table></section>
</main>
</body>
</html>
"""
