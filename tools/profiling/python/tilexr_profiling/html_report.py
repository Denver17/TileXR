# Purpose: Render self-contained HTML from the shared capture analysis.
# 用途与关系：将共享 capture 分析结果渲染为独立 HTML 报表。
"""Markdown and self-contained HTML reporting."""

import html
import json
import string
from typing import Iterable, Optional

from .io import json_chunks
from .analysis import AnalysisResult, Finding, StageSummary
from .model import Diagnostic, ProfileRun
from .provenance import build_provenance, provenance_items, metadata_summary, report_payload

_MARKDOWN_PUNCTUATION = frozenset(string.punctuation) - {"&", ";", "`"}

# The browser only rearranges already rendered cells. All statistics and global
# health remain the Python analysis, including when every row is filtered out.
_INTERACTION_SCRIPT = r"""
<script>
"use strict";
document.querySelectorAll("table[data-report-table]").forEach(function (table) {
  const controls = document.querySelector('[data-controls="' + table.id + '"]');
  const body = table.tBodies[0];
  const rows = Array.from(body.rows).filter(row => row.dataset.entry === "true");
  const originalIndex = new Map(rows.map((row, index) => [row, index]));
  const search = controls.querySelector('input[type="search"]');
  const filter = controls.querySelector("select");
  const count = controls.querySelector('[role="status"]');
  const collapsed = new Set();
  const byStage = new Map(rows.map(row => [row.dataset.stageId, row]));
  let sortIndex = null;
  let ascending = true;
  function update() {
    const query = search.value.trim().toLocaleLowerCase();
    let visible = 0;
    rows.forEach(function (row) {
      let hidden = !row.textContent.toLocaleLowerCase().includes(query);
      hidden = hidden || (filter.value !== "" && row.dataset[filter.dataset.field] !== filter.value);
      if (!query) {
        let parent = row.dataset.parentId;
        const visited = new Set();
        while (parent && byStage.has(parent) && !visited.has(parent)) {
          visited.add(parent);
          hidden = hidden || collapsed.has(parent);
          parent = byStage.get(parent).dataset.parentId;
        }
      }
      row.hidden = hidden;
      if (!hidden) visible += 1;
    });
    count.textContent = visible + " of " + rows.length + " rows visible. Global coverage and health are unchanged.";
  }
  search.addEventListener("input", update);
  filter.addEventListener("change", update);
  controls.querySelector('[data-action="reset"]').addEventListener("click", function () {
    search.value = "";
    filter.value = "";
    collapsed.clear();
    body.querySelectorAll("button[data-collapse]").forEach(button => {
      button.setAttribute("aria-expanded", "true");
      button.textContent = "−";
    });
    rows.forEach(row => body.appendChild(row));
    table.querySelectorAll("th").forEach(cell => cell.removeAttribute("aria-sort"));
    sortIndex = null;
    update();
  });
  body.querySelectorAll("button[data-collapse]").forEach(function (button) {
    button.addEventListener("click", function () {
      const id = button.dataset.collapse;
      if (collapsed.has(id)) collapsed.delete(id); else collapsed.add(id);
      button.setAttribute("aria-expanded", String(!collapsed.has(id)));
      button.textContent = collapsed.has(id) ? "+" : "−";
      update();
    });
  });
  table.querySelectorAll("thead button[data-sort]").forEach(function (button) {
    button.addEventListener("click", function () {
      const index = Number(button.dataset.sort);
      ascending = sortIndex === index ? !ascending : true;
      sortIndex = index;
      table.querySelectorAll("th").forEach(cell => cell.removeAttribute("aria-sort"));
      button.parentElement.setAttribute("aria-sort", ascending ? "ascending" : "descending");
      function value(row) {
        const text = row.cells[index].textContent.trim();
        const numeric = /^-?\d+(?:\.\d+)?(?:%| \(partial\))?$/.test(text);
        return numeric ? parseFloat(text) : text.toLocaleLowerCase();
      }
      rows.slice().sort(function (left, right) {
        const a = value(left), b = value(right);
        const order = typeof a === "number" && typeof b === "number" ? a - b : String(a).localeCompare(String(b));
        return (ascending ? order : -order) || originalIndex.get(left) - originalIndex.get(right);
      }).forEach(row => body.appendChild(row));
    });
  });
  update();
});
</script>
"""


def _table_controls(table_id, field, values):
    options = ''.join(f'<option value="{_e(value)}">{_e(value)}</option>' for value in sorted(set(values), key=str))
    return (f'<div class="table-controls" data-controls="{table_id}">'
            f'<label>Search <input type="search" aria-controls="{table_id}" maxlength="256"></label>'
            f'<label>{_e(field.title())} <select data-field="{field}" aria-controls="{table_id}">'
            f'<option value="">All</option>{options}</select></label>'
            '<button type="button" data-action="reset">Reset view</button>'
            '<span role="status" aria-live="polite">All rows visible. Global coverage and health are unchanged.</span></div>')


def _sortable_headers(names):
    return ''.join(f'<th scope="col"><button type="button" data-sort="{index}" title="Sort by {_e(name)}">{_e(name)}</button></th>'
                   for index, name in enumerate(names))


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


def _markdown_lines(run: ProfileRun, analysis: AnalysisResult):
    provenance = build_provenance(run)
    yield from ["# TileXR Profiling Analysis", "", "## Run Metadata", "",
             "| Attribute | Value |", "|---|---|"]
    yield from (f"| {_markdown_cell(k)} | {_markdown_cell(_attribute_value(v))} |"
                 for k, v in metadata_summary(run))
    yield from ["", f"Status: `{run.status}`; health: `{run.health_status}`; metadata: `{run.metadata_readiness}`.",
              "", f"Streams: expected `{run.expected_stream_count if run.manifest_metadata else 'unverified'}`, "
              f"present `{run.present_stream_count}`, usable `{run.usable_stream_count}`; "
              f"overflowed `{run.overflowed_stream_count}`.", ""]

    critical = "unavailable"
    if analysis.critical_lane is not None:
        critical = (
            f"launch {analysis.critical_lane.launch_id}, stream {analysis.critical_lane.stream_id}, "
            f"rank {analysis.critical_lane.rank}, host {analysis.critical_lane.host or 'unknown'}, "
            f"device {analysis.critical_lane.device or 'unknown'}, core {analysis.critical_lane.core}"
        )
    yield from (
        [
            "",
            "## Latency",
            "",
            "| Minimum (us) | Median (us) | Maximum (us) | Critical lane |",
            "|---:|---:|---:|---|",
            f"| {_fmt_us(analysis.latency.minimum_us)} | {_fmt_us(analysis.latency.median_us)} | "
            f"{_fmt_us(analysis.latency.maximum_us)} | {_markdown_cell(critical)} |",
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
            yield from (
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
        yield from (["No material bottleneck finding was produced.", ""])

    yield from (
        [
            "## Stage Breakdown",
            "",
            "| ID | Parent | Stage | Category | Samples | Instances | Inclusive sum (us) | Median (us) | Min (us) | Max (us) | Critical inclusive (us) | Inclusive share | Union (us) | Exclusive (us) | Critical union (us) | Critical exclusive (us) | Union share | Exclusive share | Bytes | Byte semantics | Bandwidth (GB/s) |",
            "|---:|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|",
        ]
    )
    for stage in analysis.stages.values():
        bandwidth = _stage_value(stage, "bandwidth_gbps", compact=True)
        byte_count = _stage_value(stage, "bytes", compact=True)
        parent = "root"
        if stage.parent_stage_id is not None:
            parent_stage = analysis.stages.get(stage.parent_stage_id)
            parent = (
                str(stage.parent_stage_id)
                if parent_stage is None
                else f"{stage.parent_stage_id}: {parent_stage.name}"
            )
        instances = "not_provided: no explicit instance IDs" if stage.instance_count is None else str(stage.instance_count)
        yield (
            f"| {stage.stage_id} | {_markdown_cell(parent)} | {_markdown_cell(stage.name)} | "
            f"{stage.category} | {stage.sample_count} | {instances} | "
            f"{_markdown_cell(_stage_value(stage, 'total_us'))} | {_markdown_cell(_stage_value(stage, 'median_us'))} | "
            f"{_markdown_cell(_stage_value(stage, 'minimum_us'))} | {_markdown_cell(_stage_value(stage, 'maximum_us'))} | {_markdown_cell(_stage_value(stage, 'critical_lane_us'))} | "
            f"{_markdown_cell(_stage_value(stage, 'share_of_critical'))} | {_markdown_cell(_stage_value(stage, 'union_us'))} | {_markdown_cell(_stage_value(stage, 'exclusive_us'))} | "
            f"{_markdown_cell(_stage_value(stage, 'critical_lane_union_us'))} | {_markdown_cell(_stage_value(stage, 'critical_lane_exclusive_us'))} | "
            f"{_markdown_cell(_stage_value(stage, 'union_share_of_critical'))} | {_markdown_cell(_stage_value(stage, 'exclusive_share_of_critical'))} | "
            f"{_markdown_cell(byte_count)} | {'yes' if stage.byte_semantics else 'no'} | "
            f"{_markdown_cell(bandwidth)} |"
        )

    yield from (["", "## Diagnostics", ""])
    if run.diagnostics:
        yield from (
            [
                "| Severity | Code | Message | Source | Details |",
                "|---|---|---|---|---|",
            ]
        )
        for diagnostic in run.diagnostics:
            yield (
                f"| {diagnostic.severity} | {_markdown_cell(diagnostic.code)} | "
                f"{_markdown_cell(diagnostic.message)} | {_markdown_cell(diagnostic.source or '')} | "
                f"{_markdown_cell(_attribute_value(diagnostic.details))} |"
            )
    else:
        yield ("No capture diagnostics were reported.")
    yield from ["", "## Rank / Core Comparison", "",
              "| Launch | Profiling stream ID | Rank | Host | Device | Logical lane | Timing usable / capture health | Critical latency (us) |",
              "|---|---|---|---|---|---|---|---|"]
    for launch in run.launches:
        for lane in launch.lanes:
            critical = [s for s in lane.samples if s.critical]
            row = (launch.id, lane.stream_id, lane.rank, lane.host if lane.host is not None else "not_provided",
                   lane.device if lane.device is not None else "not_provided", lane.core,
                   ("yes; " if len(critical) == 1 else "no; ") + _lane_health(lane),
                   _fmt_us(critical[0].duration_us) if len(critical) == 1 else "unavailable: requires unique critical")
            yield ("| " + " | ".join(_markdown_cell(v) for v in row) + " |")
    yield from ["", "Launch 0 denotes this capture; logical lanes are not physical cores or ACL streams.",
              "Bandwidth is lane-time effective bandwidth, not parallel wall-clock bandwidth.",
              "", "Rank coverage: " + _markdown_cell(_attribute_value(analysis.rank_coverage)),
              "", "### Metric states and rank statistics", ""]
    for stage in analysis.stages.values():
        yield from [_markdown_cell(stage.name) + ": " + _markdown_cell(_attribute_value(_metric_details(stage))), ""]
    yield from ["## Run Health", "", "## Provenance", "", "<details><summary>Provenance / Statistical definitions</summary>", "",
              "| Field | Value |", "|---|---|"]
    yield from (f"| {_markdown_cell(k)} | {_markdown_cell(_attribute_value(v))} |" for k, v in provenance_items(provenance))
    yield from ["", "Complete shared report data:", ""]


def iter_analysis_markdown(run, analysis):
    first = True
    for line in _markdown_lines(run, analysis):
        if not first:
            yield "\n"
        first = False
        yield line
    yield "\n<pre>"
    for chunk in json_chunks(report_payload(run, analysis), ensure_ascii=True):
        yield _e(chunk).replace("[", "&#91;").replace("]", "&#93;")
    yield "</pre>\n\n</details>\n"


def render_analysis_markdown(run: ProfileRun, analysis: AnalysisResult) -> str:
    return "".join(iter_analysis_markdown(run, analysis))


def _e(value: object) -> str:
    return html.escape(str(value), quote=True)


def _finding_rows(findings: Iterable[Finding]):
    found = False
    for finding in findings:
        found = True
        yield (
            "<tr>"
            f'<td><span class="severity severity-{_e(finding.severity)}">{_e(finding.severity)}</span></td>'
            f"<td><strong>{_e(finding.title)}</strong><small>{_e(finding.code)}</small></td>"
            f"<td>{_e(finding.evidence)}</td><td>{_e(finding.recommendation)}</td>"
            "</tr>"
        )
    if not found:
        yield '<tr><td colspan="4">No material bottleneck finding was produced.</td></tr>'


def _stage_rows(stages: Iterable[StageSummary]):
    stage_list = list(stages)
    stages_by_id = {stage.stage_id: stage for stage in stage_list}
    found = False
    parents = {stage.parent_stage_id for stage in stage_list}
    for stage in stage_list:
        bandwidth = _stage_value(stage, "bandwidth_gbps", compact=True)
        byte_count = _stage_value(stage, "bytes", compact=True)
        parent = "root"
        if stage.parent_stage_id is not None:
            parent_stage = stages_by_id.get(stage.parent_stage_id)
            parent = (
                str(stage.parent_stage_id)
                if parent_stage is None
                else f"{stage.parent_stage_id}: {parent_stage.name}"
            )
        instances = "not_provided: no explicit instance IDs" if stage.instance_count is None else str(stage.instance_count)
        disclosure = (f'<button type="button" class="disclosure" data-collapse="{stage.stage_id}" '
                      f'aria-expanded="true" aria-label="Toggle children of {_e(stage.name)}">−</button> '
                      if stage.stage_id in parents else "")
        found = True
        yield (
            f'<tr data-entry="true" data-stage-id="{stage.stage_id}" '
            f'data-parent-id="{stage.parent_stage_id if stage.parent_stage_id is not None else ""}" '
            f'data-category="{_e(stage.category)}">'
            f"<td>{stage.stage_id}</td><td>{_e(parent)}</td>"
            f"<td>{disclosure}<strong>{_e(stage.name)}</strong></td>"
            f"<td>{_e(stage.category)}</td><td>{stage.sample_count}</td><td>{instances}</td>"
            f"<td>{_e(_stage_value(stage, 'total_us'))}</td><td>{_e(_stage_value(stage, 'median_us'))}</td>"
            f"<td>{_e(_stage_value(stage, 'minimum_us'))}</td><td>{_e(_stage_value(stage, 'maximum_us'))}</td>"
            f"<td>{_e(_stage_value(stage, 'critical_lane_us'))}</td><td>{_e(_stage_value(stage, 'share_of_critical'))}</td>"
            f"<td>{_e(_stage_value(stage, 'union_us'))}</td><td>{_e(_stage_value(stage, 'exclusive_us'))}</td>"
            f"<td>{_e(_stage_value(stage, 'critical_lane_union_us'))}</td><td>{_e(_stage_value(stage, 'critical_lane_exclusive_us'))}</td>"
            f"<td>{_e(_stage_value(stage, 'union_share_of_critical'))}</td><td>{_e(_stage_value(stage, 'exclusive_share_of_critical'))}</td>"
            f"<td>{_e(byte_count)}</td><td>{'yes' if stage.byte_semantics else 'no'}</td><td>{_e(bandwidth)}</td>"
            "</tr>"
        )
    if not found:
        yield '<tr><td colspan="21">No complete stage interval is available.</td></tr>'


def _diagnostic_rows(diagnostics: Iterable[Diagnostic]):
    found = False
    for diagnostic in diagnostics:
        found = True
        yield (
            "<tr>"
            f"<td>{_e(diagnostic.severity)}</td><td>{_e(diagnostic.code)}</td>"
            f"<td>{_e(diagnostic.message)}</td><td>{_e(diagnostic.source or '')}</td>"
            f"<td>{_e(_attribute_value(diagnostic.details))}</td>"
            "</tr>"
        )
    if not found:
        yield '<tr><td colspan="5">No capture diagnostics were reported.</td></tr>'


def _metadata_rows(run: ProfileRun) -> str:
    return "".join(
        "<tr>" f"<td>{_e(key)}</td><td>{_e(_attribute_value(value))}</td>" "</tr>"
        for key, value in metadata_summary(run)
    )


def _lane_rows(run):
    found = False
    for launch in run.launches:
        for lane in launch.lanes:
            critical = [sample for sample in lane.samples if sample.critical]
            usable = len(critical) == 1
            duration = f"{critical[0].duration_us:.3f}" if usable else "unavailable"
            found = True
            yield (
                f'<tr data-entry="true" data-rank="{lane.rank}">'
                f"<td>{launch.id}</td><td>{lane.stream_id}</td><td>{lane.rank}</td>"
                f"<td>{_e(lane.host or 'unknown')}</td><td>{_e(lane.device if lane.device is not None else 'not_provided')}</td>"
                f"<td>{lane.core}</td><td>{'yes' if usable else 'no'}<small>{_e(_lane_health(lane))}</small></td><td>{duration}</td>"
                "</tr>"
            )

    if not found:
        yield '<tr><td colspan="8">No stream data is available.</td></tr>'


def iter_report_html(run, analysis, *, trace_available=True):
    critical_label = "unavailable"
    if analysis.critical_lane is not None:
        critical_label = (
            f"R{analysis.critical_lane.rank} / D{analysis.critical_lane.device or 'unknown'} / "
            f"H{analysis.critical_lane.host or 'unknown'} / logical lane {analysis.critical_lane.core}"
        )

    yield f"""<!doctype html>
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
pre {{ white-space:pre-wrap; overflow-wrap:anywhere; }} details {{ margin:14px 0; }} summary {{ cursor:pointer; font-weight:bold; }}
table {{ width:100%; border-collapse:collapse; table-layout:auto; }}
th,td {{ border-bottom:1px solid var(--line); padding:9px 10px; text-align:left; vertical-align:top; }}
th {{ background:var(--panel); color:#394958; font-size:12px; }}
#stage-table th:nth-last-child(-n+3),#stage-table td:nth-last-child(-n+3) {{ white-space:nowrap; }}
.severity {{ display:inline-block; padding:2px 6px; border-radius:4px; background:var(--panel); }}
.severity-warning {{ color:var(--warn); }} .severity-incomplete,.severity-error {{ color:var(--bad); }}
.toolbar {{ display:flex; justify-content:space-between; align-items:center; gap:12px; margin-bottom:10px; }}
.table-controls {{ display:flex; flex-wrap:wrap; align-items:center; gap:12px; margin:10px 0; }}
input,select,button {{ font:inherit; }}
input,select,.table-controls button {{ padding:5px 8px; border:1px solid var(--line); border-radius:4px; background:#fff; color:var(--ink); }}
button {{ cursor:pointer; }} th button {{ border:0; padding:0; background:transparent; color:inherit; text-align:left; font-weight:bold; }}
button:focus-visible,input:focus-visible,select:focus-visible {{ outline:2px solid var(--accent); outline-offset:3px; }}
th[aria-sort="ascending"] button::after {{ content:" ↑"; }} th[aria-sort="descending"] button::after {{ content:" ↓"; }}
.disclosure {{ min-width:24px; border:1px solid var(--line); background:var(--panel); border-radius:3px; }}
[hidden] {{ display:none !important; }}
a {{ color:var(--accent); }}
@media (max-width:720px) {{ main,header {{ padding-left:14px; padding-right:14px; }} .summary {{ display:block; }} .metric {{ border-right:0; border-bottom:1px solid var(--line); }} }}
</style>
</head>
<body>
<header><h1>TileXR Profiling Report</h1><p>{_e(run.producer)} / {_e(run.operator)} / run {_e(run.run_id)}</p></header>
<main>
<section><div class="toolbar"><h2>Profiling Summary</h2>{'<a href="perfetto_trace.json">Open Perfetto trace</a>' if trace_available else ''}</div>
<div class="summary">
<div class="metric"><small>Status</small><strong>{_e(run.status)}</strong></div>
<div class="metric"><small>Usable streams (timing)</small><strong>{run.usable_stream_count} / {run.expected_stream_count if run.manifest_metadata else "unverified"}</strong></div>
<div class="metric"><small>Minimum latency</small><strong>{_fmt_us(analysis.latency.minimum_us)} us</strong></div>
<div class="metric"><small>Median latency</small><strong>{_fmt_us(analysis.latency.median_us)} us</strong></div>
<div class="metric"><small>Maximum latency</small><strong>{_fmt_us(analysis.latency.maximum_us)} us</strong></div>
<div class="metric"><small>Critical lane</small><strong>{_e(critical_label)}</strong></div>
<div class="metric"><small>Rank imbalance</small><strong>{analysis.rank_imbalance_ratio:.2f}x</strong></div>
<div class="metric"><small>Core imbalance</small><strong>{analysis.core_imbalance_ratio:.2f}x</strong></div>
<div class="metric"><small>Wait/sync union pressure</small><strong>{_share(analysis.wait_sync_pressure)}</strong></div>
</div></section>
<section><h2>Run Metadata</h2><table><thead><tr><th>Attribute</th><th>Value</th></tr></thead><tbody>{_metadata_rows(run)}</tbody></table></section>
<section><details><summary>Provenance / Statistical definitions</summary>"""
    yield from _full_provenance_html(run)
    yield f"""</details></section>
<section><h2>Bottlenecks</h2><table><thead><tr><th>Severity</th><th>Finding</th><th>Evidence</th><th>Recommendation</th></tr></thead><tbody>"""
    yield from _finding_rows(analysis.findings)
    yield f"""</tbody></table></section>
<section><h2>Rank / Core Comparison</h2>{_table_controls('lane-table', 'rank', (lane.rank for launch in run.launches for lane in launch.lanes))}<table id="lane-table" data-report-table><thead><tr>{_sortable_headers(('Launch', 'Profiling stream ID', 'Rank', 'Host', 'Device', 'Logical lane', 'Timing usable / capture health', 'Critical latency (us)'))}</tr></thead><tbody>"""
    yield from _lane_rows(run)
    yield f"""</tbody></table></section>
<section><h2>Stage Breakdown</h2>{_table_controls('stage-table', 'category', (stage.category for stage in analysis.stages.values()))}<table id="stage-table" data-report-table><thead><tr>{_sortable_headers(('ID', 'Parent', 'Stage', 'Category', 'Samples', 'Instances', 'Inclusive sum (us)', 'Median (us)', 'Min (us)', 'Max (us)', 'Critical inclusive (us)', 'Inclusive share', 'Union (us)', 'Exclusive (us)', 'Critical union (us)', 'Critical exclusive (us)', 'Union share', 'Exclusive share', 'Bytes', 'Byte semantics', 'GB/s'))}</tr></thead><tbody>"""
    yield from _stage_rows(analysis.stages.values())
    yield f"""</tbody></table></section>
<section><h2>Run Health</h2><p>Capture: {_e(run.status)}; health: {_e(run.health_status)}; metadata: {_e(run.metadata_readiness)}. Missing participants: {_e(run.missing_stream_count if run.manifest_metadata else "unverified")}.</p><h3>Diagnostics</h3><table><thead><tr><th>Severity</th><th>Code</th><th>Message</th><th>Source</th><th>Details</th></tr></thead><tbody>"""
    yield from _diagnostic_rows(run.diagnostics)
    yield f"""</tbody></table></section>
<section><p>Launch 0 denotes this capture. Logical lanes are not inferred physical cores or ACL execution streams. GB/s means lane-time effective bandwidth, not parallel wall-clock bandwidth.</p><p>Rank coverage: {_e(_attribute_value(analysis.rank_coverage))}</p><details><summary>Metric states and rank statistics</summary>"""
    yield from _metric_details_html(analysis)
    yield f"""</details><details><summary>Complete shared report data</summary><pre id="report-data">"""
    yield from _iter_attribute_html(report_payload(run, analysis))
    yield f"""</pre></details></section>
</main>
{_INTERACTION_SCRIPT}
</body>
</html>
"""


def render_report_html(run: ProfileRun, analysis: AnalysisResult, *, trace_available=True) -> str:
    return "".join(iter_report_html(run, analysis, trace_available=trace_available))


def _lane_health(lane):
    attributes = lane.attributes
    dropped = attributes.get("dropped_count", 0)
    count = f"at least {dropped}" if dropped == 0xffffffff else str(dropped)
    return (f"{attributes.get('capture_status', 'unverified')}; dropped records {count}; "
            f"overflow {attributes.get('overflow', False)}; "
            f"device namespace {attributes.get('device_namespace', 'not_provided')}")


def _iter_attribute_html(value):
    if isinstance(value, str):
        yield _e(value)
    else:
        for chunk in json_chunks(value, ensure_ascii=True):
            yield _e(chunk)


def _full_provenance_html(run):
    yield "<table>"
    for key, value in provenance_items(build_provenance(run)):
        yield f"<tr><td>{_e(key)}</td><td>"
        yield from _iter_attribute_html(value)
        yield "</td></tr>"
    yield "</table>"


def _metric_details(stage):
    return dict(states=stage.states, rank_latency=stage.rank_latency,
                rank_bandwidth=stage.rank_bandwidth, bandwidth_basis=stage.bandwidth_basis,
                instance_coverage=stage.instance_coverage)


def _metric_details_html(analysis):
    yield "<table>"
    for stage in analysis.stages.values():
        yield f"<tr><td>{_e(stage.name)}</td><td>"
        yield from _iter_attribute_html(_metric_details(stage))
        yield "</td></tr>"
    yield "</table>"


def _stage_value(stage, key, *, compact=False):
    value = getattr(stage, key)
    state = stage.states.get(key, {})
    if value is None:
        if compact:
            return "missing"
        # Reasons may include caller declarations; each renderer escapes the returned plain text.
        return state.get("state", "not_provided") + ": " + state.get("reason", "not provided")
    text = f"{value:.1%}" if "share" in key else f"{value:.3f}" if isinstance(value, float) else str(value)
    return text + " (partial)" if state.get("state") == "incomplete" else text
