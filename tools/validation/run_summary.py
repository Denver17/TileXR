"""Terminal verdict, per-operator measurements/traces and workflow wall times."""

from pathlib import Path
import math


def failure_label(report):
    error = report.get("primary_error") or {}
    if error.get("timeout") and error.get("type") in ("EXIT_FAILED", "LAUNCH_FAILED"):
        return "执行卡死（超时）"
    errors = report.get("errors") or [error]
    if errors and all(item.get("validation_status") == "FAIL" and
                      item.get("communication_completed") is True for item in errors):
        return "结果校验失败"
    return "执行阶段失败"


def _terminal_line(parts):
    # Sanitize field values without removing the intentional operator line breaks.
    return " | ".join(parts).replace("\n", " ").replace("\r", " ")


def _measurement_line(operator, measured):
    parts = [f"{operator}: backend={measured['backend']} native={measured['native_status']} "
             f"kernel={measured['kernel_sha256'][:12]}"]
    if "performance" in measured:
        stats, suffix = measured["performance"], "_us"
        fields, label = ("minimum", "mean", "maximum"), "server_latency(us) min/mean/max="
        parts.append(f"samples/card={stats['samples_per_rank']}")
    else:
        stats, suffix = measured["breakdown"]["kernel_us"], ""
        fields, label = ("minimum", "median", "maximum"), "latency(us) min/median/max="
    parts.append(label + "/".join(f"{stats[key + suffix]:.3f}" for key in fields))
    traffic = measured.get("algorithm_communication_bytes")
    parts.append("bytes=" + (str(traffic) if traffic is not None else "N/A"))
    if "bandwidth_gbps" in measured:
        parts.append("server_bandwidth(GB/s) min/mean/max=" + "/".join(
            f"{measured['bandwidth_gbps'][key]:.3f}" for key in ("minimum", "mean", "maximum")))
    else:
        parts.append("bandwidth=N/A")
    if operator == "combine":
        for key in ("scope", "implemented_through", "wait_us"):
            if key in measured:
                parts.append(f"{key}={measured[key]}")
        if "stage_copy_bytes" in measured:
            parts.append(f"algorithm_payload={measured['algorithm_bytes']}B "
                         f"stage_read_write={measured['stage_copy_bytes']}B "
                         f"control_publish={measured['control_publish_bytes']}B")
            rates = measured["algorithm_bandwidth_gbps"]
            parts.append("algorithm_payload_GB/s(nonzero-work ranks) min/median/max=" + "/".join(
                f"{rates[key]:.3f}" for key in ("minimum", "median", "maximum"))
                if rates else "algorithm_payload_GB/s=N/A")
    return _terminal_line(parts)


def _wall_time_lines(report):
    timing = report.get("wall_time")
    if not timing:
        return []
    lines = ["WORKFLOW_WALL(s) " + " ".join(
        f"{name}={seconds:.3f}" for name, seconds in timing["stages"].items()) +
        f" total={timing['total']:.3f}"]
    ranks = [row for node in report.get("nodes", [])
             for row in (node.get("reported_result") or {}).get("rank_wall_times", [])]
    maxima = []
    for phase in ("process", "profile_wait", "profile_decode", "validate"):
        samples = [(row.get("stages", {}).get(phase), row.get("rank")) for row in ranks]
        samples = [(seconds, rank) for seconds, rank in samples
                   if type(seconds) in (int, float) and math.isfinite(seconds) and seconds >= 0
                   and type(rank) is int]
        if samples:
            seconds, rank = max(samples)
            maxima.append(f"{phase}={seconds:.3f}(rank={rank})")
    if maxima:
        lines.append(f"RANK_WALL_MAX(s,observed={len(ranks)}) " + " ".join(maxima))
    return lines


def render_result(report, summary_path):
    status = report["status"]
    parts = ["VALIDATION_PASS" if status == "PASS" else "VALIDATION_FAIL " + failure_label(report)]
    config = report.get("chain_configuration", {})
    if config:
        parts.append(f"mode={config['run_mode']} BS={config['bs']} loops={config['loops']} warmup={config['warmup']}")
    if "rank_size" in report:
        parts.append(f"ranks={report.get('completed_ranks', 0)}/{report['rank_size']}")
    analysis = report.get("case_analysis")
    if analysis and analysis.get("status") == "complete":
        parts.append("dedup(%) min/max/avg=" + "/".join(f"{analysis[key]:.2f}" for key in ("min", "max", "avg")))
    elif analysis:
        parts.append("case_analysis=failed")
    parts.append(f"result={summary_path}")
    lines = [_terminal_line(parts)]
    summaries = dict(report.get("operator_measurements", {}))
    if report.get("measurements"):
        summaries.setdefault("dispatch", report["measurements"])
    operator_mode = config.get("operator_mode", "both")
    if config.get("run_mode") != "functional":
        for operator in ("dispatch", "combine"):
            measured = summaries.get(operator)
            if measured and operator_mode in (operator, "both"):
                lines.append(_measurement_line(operator, measured))
    traces_by_operator = dict(report.get("operator_traces", {}))
    if report.get("traces"):
        traces_by_operator.setdefault("dispatch", report["traces"])
    if config.get("run_mode") in (None, "breakdown"):
        for operator in ("dispatch", "combine"):
            traces = traces_by_operator.get(operator)
            if not traces or operator_mode not in (operator, "both"):
                continue
            parts = [f"TRACE {operator}", f"trace={traces['trace_dir']}"]
            if "status" in traces:
                parts.append(f"status={traces['status']}")
            for role in ("fastest", "slowest"):
                if role in traces.get("selected_ranks", {}):
                    parts.append(f"{role}_rank={traces['selected_ranks'][role]}")
            lines.append(_terminal_line(parts))
    lines.extend(_wall_time_lines(report))
    return "\n".join(lines)


def write_result(directory, report, summary_path):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    temporary = directory / "result.txt.tmp"
    temporary.write_text(render_result(report, summary_path) + "\n", encoding="utf-8")
    temporary.replace(directory / "result.txt")
