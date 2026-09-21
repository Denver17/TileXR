"""Terminal verdict followed by one measurement line per active operator."""

from pathlib import Path


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
    traces = report.get("traces")
    if traces:
        parts.append(f"trace={traces['trace_dir']}")
        selected = traces.get("selected_ranks", {})
        for role in ("fastest", "slowest"):
            if role in selected:
                parts.append(f"{role}_rank={selected[role]}")
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
    return "\n".join(lines)


def write_result(directory, report, summary_path):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    temporary = directory / "result.txt.tmp"
    temporary.write_text(render_result(report, summary_path) + "\n", encoding="utf-8")
    temporary.replace(directory / "result.txt")
