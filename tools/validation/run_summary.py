"""One-line terminal result; detailed evidence stays in the case directory."""

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


def render_result(report, summary_path):
    status = report["status"]
    parts = ["VALIDATION_PASS" if status == "PASS" else "VALIDATION_FAIL " + failure_label(report)]
    config = report.get("chain_configuration", {})
    if config:
        parts.append(f"mode={config['run_mode']} BS={config['bs']} loops={config['loops']} warmup={config['warmup']}")
    if "rank_size" in report:
        parts.append(f"ranks={report.get('completed_ranks', 0)}/{report['rank_size']}")
    measured = report.get("measurements", {})
    if measured:
        parts.append(f"backend={measured['backend']} native={measured['native_status']} "
                     f"kernel={measured['kernel_sha256'][:12]}")
        if "performance" in measured:
            stats, suffix = measured["performance"], "_us"
            fields, label = ("maximum", "minimum", "mean"), "server_latency(us) max/min/mean="
            parts.append(f"samples/card={stats['samples_per_rank']}")
        else:
            stats, suffix = measured["breakdown"]["kernel_us"], ""
            fields, label = ("median", "minimum", "maximum"), "latency(us) median/min/max="
        parts.append(label + "/".join(f"{stats[key + suffix]:.3f}" for key in fields))
        traffic = measured.get("algorithm_communication_bytes")
        parts.append("bytes=" + (str(traffic) if traffic is not None else "N/A"))
        if "bandwidth_gbps" in measured:
            parts.append("server_bandwidth(GB/s) max/min/mean=" + "/".join(
                f"{measured['bandwidth_gbps'][key]:.3f}" for key in ("maximum", "minimum", "mean")))
        else:
            parts.append("bandwidth=N/A")
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
    return " | ".join(parts).replace("\n", " ").replace("\r", " ")


def write_result(directory, report, summary_path):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    temporary = directory / "result.txt.tmp"
    temporary.write_text(render_result(report, summary_path) + "\n", encoding="utf-8")
    temporary.replace(directory / "result.txt")
