"""Independent Dispatch/Combine measurement evidence over the shared readers."""

from pathlib import Path
from uuid import NAMESPACE_URL, uuid5

import measurements as measure


def active_operators(config):
    mode = config.get("operator_mode", "dispatch")
    if mode == "both":
        return ("dispatch", "combine")
    if mode in ("dispatch", "combine"):
        return (mode,)
    raise ValueError("operator_mode must be dispatch/combine/both")


def _identity(operator, run_id, rank, kernel_sha, loops, warmup, mode):
    if not isinstance(run_id, str) or not run_id:
        raise ValueError("operator measurement requires run_id")
    measure._integer(rank, "rank")
    measure._integer(loops, "loops", 1)
    measure._integer(warmup, "warmup")
    if (not isinstance(kernel_sha, str) or len(kernel_sha) != 64 or
            any(char not in "0123456789abcdef" for char in kernel_sha)):
        raise ValueError("operator measurement requires kernel SHA256")
    return dict(operator=operator, kernel=measure.kernel_name(operator), run_id=run_id, rank=rank,
                kernel_sha256=kernel_sha, run_mode=mode, loops=loops, warmup=warmup,
                last_launch=loops + warmup - 1)


def _empty_scope(evidence, operator):
    if operator == "combine":
        evidence.update(scope="empty/framework", native_status="combine-empty", algorithm_bytes=0,
                        algorithm_bandwidth_gbps=None, algorithm_bandwidth="N/A")
    return evidence


def performance_record(profile_dir, loops, warmup, *, operator, run_id, rank, kernel_sha):
    identity = _identity(operator, run_id, rank, kernel_sha, loops, warmup, "performance")
    evidence = measure.performance_record(profile_dir, loops, warmup, operator=operator)
    return _empty_scope(dict(evidence, **identity), operator)


def breakdown_record(records_path, config, rank, capture_id, output_dir, *,
                     operator, run_id, kernel_sha, mapping_path=None):
    identity = _identity(operator, run_id, rank, kernel_sha, config["loops"], config["warmup"], "breakdown")
    if capture_id != str(uuid5(NAMESPACE_URL, "tilexr-validation:" + run_id)):
        raise ValueError("capture UUID does not belong to this run_id")
    evidence = measure.breakdown_record(records_path, config, rank, capture_id, output_dir,
        mapping_path, operator=operator, run_id=run_id, kernel_sha=kernel_sha)
    evidence.update(identity)
    _empty_scope(evidence, operator)
    # Keep the producer's small, hashed summary identical to its scalar evidence.
    evidence.pop("summary_sha256", None)
    measure.atomic_write_json(Path(evidence["summary_path"]), evidence)
    evidence["summary_sha256"] = measure._file_digest(evidence["summary_path"])
    return evidence


def _validate_evidence(evidence, config, operator, rank, run_id, kernel_sha):
    if not isinstance(evidence, dict):
        raise ValueError("missing " + operator + " measurement evidence")
    identity = _identity(operator, run_id, rank, kernel_sha, config["loops"], config["warmup"], config["run_mode"])
    for key, value in identity.items():
        if type(evidence.get(key)) is not type(value) or evidence[key] != value:
            raise ValueError(operator + " measurement identity mismatch: " + key)
    measure.validate_rank_measurements({config["run_mode"]: evidence}, config, operator=operator)
    if config["run_mode"] == "breakdown":
        if evidence.get("capture_id") != str(uuid5(NAMESPACE_URL, "tilexr-validation:" + run_id)):
            raise ValueError(operator + " capture UUID does not belong to this run")
        if type(evidence.get("dropped_records")) is not int or evidence["dropped_records"] != 0:
            raise ValueError(operator + " capture dropped records")
        if operator == "combine":
            stages = evidence.get("stages_us", {})
            if set(stages) != {"kernel_total", "empty_wait"} or any(
                    stage.get("sample_count") != 64 for stage in stages.values()):
                raise ValueError("Combine breakdown requires both intervals on all 64 cores")
    if operator == "combine":
        expected = _empty_scope({}, operator)
        if any(type(evidence.get(key)) is not type(value) or evidence[key] != value
               for key, value in expected.items()):
            raise ValueError("empty Combine must report zero algorithm bytes and N/A bandwidth")


def validate_operator_measurements(record, config, *, run_id=None, kernel_shas=None):
    evidence = record.get("operator_measurements", {})
    if config["run_mode"] == "functional":
        if evidence:
            raise ValueError("functional mode must not contain operator measurements")
        return
    operators = active_operators(config)
    if not isinstance(evidence, dict) or set(evidence) != set(operators):
        raise ValueError("measurement evidence must contain exactly the active operators")
    paths = set()
    for operator in operators:
        entry = evidence[operator]
        mode = config["run_mode"]
        if not isinstance(entry, dict) or set(entry) != {mode}:
            raise ValueError("operator measurement mode mismatch")
        item = entry[mode]
        if not isinstance(item, dict):
            raise ValueError("operator measurement evidence must be an object")
        expected_run = run_id if run_id is not None else record.get("run_id")
        expected_sha = kernel_shas[operator] if kernel_shas is not None else item.get("kernel_sha256")
        _validate_evidence(item, config, operator, record["rank"], expected_run, expected_sha)
        if mode == "breakdown":
            for key in ("records_path", "summary_path"):
                path = str(Path(item[key]).resolve())
                if path in paths:
                    raise ValueError("operators must use separate capture and report paths")
                paths.add(path)


def _operator_rows(rank_records, operator, mode):
    rows = []
    for record in rank_records:
        # Dispatch retains the original traffic oracle for bandwidth accounting.
        source = record.get("dispatch", record) if operator == "dispatch" else record
        row = dict(source)
        row[mode] = record["operator_measurements"][operator][mode]
        rows.append(row)
    return rows


def summarize_operator_measurements(rank_records, config, run_id, kernel_shas):
    if config["run_mode"] == "functional":
        for row in rank_records:
            validate_operator_measurements(row, config)
        return {}
    for row in rank_records:
        validate_operator_measurements(row, config, run_id=run_id, kernel_shas=kernel_shas)
    result = {}
    for operator in active_operators(config):
        rows = _operator_rows(rank_records, operator, config["run_mode"])
        op_config = dict(config, kernel_scope="dispatch" if operator == "dispatch" else "combine-empty")
        summary = measure.summarize_measurements(rows, op_config, kernel_shas[operator], operator=operator)
        durations = [row[config["run_mode"]]["mean_us"] if config["run_mode"] == "performance"
                     else row["breakdown"]["sample_us"][0] for row in sorted(rows, key=lambda row: row["rank"])]
        summary.update(operator=operator, run_id=run_id, last_launch=config["loops"] + config["warmup"] - 1,
            latency_us=measure._statistics(durations), rank_latency_us=durations,
            latency_reduction="statistics across ranks of measured-launch mean" if config["run_mode"] == "performance"
            else "statistics across ranks of last-launch slowest core")
        _empty_scope(summary, operator)
        result[operator] = summary
    return result


def render_operator_measurements(summaries):
    parts = []
    for operator, summary in summaries.items():
        stats = summary["latency_us"]
        text = (f"{operator}: backend={summary['backend']} native={summary['native_status']} "
                f"median={stats['median']:.3f} us min={stats['minimum']:.3f} us max={stats['maximum']:.3f} us\n"
                f"kernel={summary['kernel']} sha256={summary['kernel_sha256']}")
        if operator == "combine":
            text += "\nscope=empty/framework algorithm_bytes=0 algorithm_bandwidth=N/A（含空算子50us等待）"
        else:
            text += "\n" + measure.render_measurements(summary)
        parts.append(text)
    return "\n\n".join(parts)


def relocate_operator_breakdown_report(evidence, node_log, rank, host, *, operator, local_records=False):
    measure.kernel_name(operator)
    directory = Path(node_log) / f"profile-rank-{rank}" / operator
    result = dict(evidence)
    result.update(source_records_path=evidence.get("source_records_path", evidence.get("records_path")),
        source_host=host, records_collected=local_records,
        local_records_path=str(Path(node_log) / f"rank-{rank}.{operator}.records.v1") if local_records else None,
        summary_path=str(directory / "summary.json"),
        artifacts={name: str(directory / name) for name in measure.PROFILE_ARTIFACTS})
    return result


def collect_operator_breakdown_reports(rank_records, config, output_dir, run_id, kernel_shas):
    """Keep completed operator reports when a different region/rank is unavailable."""
    if config["run_mode"] != "breakdown":
        raise ValueError("operator trace collection requires breakdown mode")
    result = {}
    for operator in active_operators(config):
        rows, identity_errors = [], {}
        for record in rank_records:
            row = dict(rank=record["rank"], status=record.get("status", "UNKNOWN"))
            evidence = record.get("operator_measurements", {}).get(operator, {}).get("breakdown")
            try:
                # Incomplete captures still produce useful traces; identity is
                # checked here, completeness by the shared collector below.
                expected = _identity(operator, run_id, row["rank"], kernel_shas[operator],
                                     config["loops"], config["warmup"], "breakdown")
                if not isinstance(evidence, dict) or any(
                        type(evidence.get(key)) is not type(value) or evidence[key] != value
                        for key, value in expected.items()):
                    raise ValueError("missing or mismatched " + operator + " report identity")
                row["breakdown"] = evidence
            except ValueError as error:
                identity_errors[row["rank"]] = str(error)
            rows.append(row)
        index = measure.collect_breakdown_reports(rows, config, Path(output_dir) / operator,
            run_id, kernel_shas[operator], operator=operator)
        index.update(operator=operator, kernel=measure.kernel_name(operator),
                     last_launch=config["warmup"] + config["loops"] - 1)
        for rank, message in identity_errors.items():
            index["reports"][str(rank)]["error"] = message
        measure.atomic_write_json(Path(index["index"]), index)
        result[operator] = index
    return result
