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


def m12_stage_counts(config):
    counts = {name: 64 for name in ("kernel_total", "m1_init", "m1_select", "m1_stage_copy",
                                    "m1_prepare_publish", "m6_drain", "p21_delay_50us")}
    counts.update(m2_prepare_wait=8, m2_notify=8,
                  m2_ready_wait=64 - config.get("combine_send_core_num", 32),
                  m2_ready_clear=64 - config.get("combine_send_core_num", 32))
    return counts


def return_stage_counts(config):
    send = config.get("combine_send_core_num", 32)
    stages = dict(kernel_total=64, m1_init=64, m1_stage=64, m2_control=64 - send,
                  m3_direct=send, m6_drain=64)
    if config.get("implemented_through") in ("M4", "M5"):
        stages["m4_partial"] = 64 - send
    if config.get("implemented_through") == "M5":
        stages["m5_final"] = 64 - send
    return stages


def return_traffic(record, config):
    if not isinstance(record, dict):
        raise ValueError("Combine measurement requires rank traffic")
    stage = measure._integer(record.get("combine_type3_rows"), "combine_type3_rows")
    direct = measure._integer(record.get("combine_type1_rows"), "combine_type1_rows")
    tokens = measure._integer(record.get("combine_token_num"), "combine_token_num")
    partial = measure._integer(record.get("combine_type2_rows"), "combine_type2_rows") if config.get("implemented_through") in ("M4", "M5") else 0
    if stage + direct + partial > tokens or tokens > 10000:
        raise ValueError("Combine disjoint rows exceed supported TokenNum")
    payload = direct * config["h"] * 2
    wire = direct * ((config["h"] * 2 + 479) // 480) * 512
    expected = dict(algorithm_bytes=(stage + direct + partial) * config["h"] * 2,
                    stage_copy_bytes=stage * config["h"] * 4,
                    direct_payload_bytes=payload, direct_wire_bytes=wire,
                    control_publish_bytes=(64 * 8 + 8 * (64 - config.get("combine_send_core_num", 32))) * 32)
    if config.get("implemented_through") in ("M4", "M5"):
        members = measure._integer(record.get("combine_partial_member_rows"), "combine_partial_member_rows")
        local_members = measure._integer(record.get("combine_partial_local_members"), "combine_partial_local_members")
        member_bytes = measure._integer(record.get("combine_partial_stage_read_bytes"), "combine_partial_stage_read_bytes")
        row_bytes = config["h"] * 2
        if (not partial <= members <= partial * (config["topk"] - 1) or local_members > members or
                member_bytes != members * row_bytes):
            raise ValueError("M4 stage member reads exceed row member bounds")
        expected.update(partial_payload_bytes=partial * row_bytes,
                        partial_wire_bytes=partial * ((row_bytes + 479) // 480) * 512,
                        partial_self_read_bytes=partial * row_bytes,
                        partial_stage_read_bytes=member_bytes)
    if config.get("implemented_through") == "M5":
        direct_received = measure._integer(record.get("combine_direct_received_rows"), "combine_direct_received_rows")
        partial_received = measure._integer(record.get("combine_partial_received_rows"), "combine_partial_received_rows")
        if not config["bs"] <= direct_received + partial_received <= config["bs"] * config["topk"]:
            raise ValueError("M5 requires a nonempty mask for every original token")
        expected.update(final_read_bytes=(direct_received + partial_received) * config["h"] * 2,
                        y_out_bytes=config["bs"] * config["h"] * 2,
                        flag_clear_bytes=config["bs"] * config["topk"] * ((config["h"] * 2 + 479) // 480) * 32)
        expected["algorithm_bytes"] += expected["y_out_bytes"]
    for key, value in expected.items():
        if type(record.get("combine_" + key)) is not int or record["combine_" + key] != value:
            raise ValueError("Combine traffic mismatch: " + key)
    return dict(rank=record["rank"], type3_rows=stage, type1_rows=direct, **({"type2_rows": partial} if config.get("implemented_through") in ("M4", "M5") else {}),
                stage_payload_bytes=stage * config["h"] * 2, **expected)


def m12_traffic(record, config):
    if not isinstance(record, dict):
        raise ValueError("M1/M2 measurement requires validated rank traffic")
    rows = record.get("combine_type3_rows")
    measure._integer(rows, "combine_type3_rows")
    if rows > 10000:
        raise ValueError("M1/M2 type3 rows exceed supported TokenNum")
    expected = dict(algorithm_bytes=rows * config["h"] * 2,
                    stage_copy_bytes=rows * config["h"] * 4,
                    control_publish_bytes=(64 * 8 + 8 * (64 - config.get("combine_send_core_num", 32))) * 32)
    for key, value in expected.items():
        if type(record.get("combine_" + key)) is not int or record["combine_" + key] != value:
            raise ValueError("M1/M2 traffic mismatch: " + key)
    return dict(rank=record["rank"], type3_rows=rows, **expected)


def _operator_scope(evidence, operator, config=None, record=None, *, duration=None):
    if operator != "combine" or not config or config.get("implemented_through") not in ("M2", "M3", "M4", "M5"):
        return _empty_scope(evidence, operator)
    returns = config.get("implemented_through") in ("M3", "M4", "M5")
    traffic = (return_traffic if returns else m12_traffic)(record, config)
    measured = duration if duration is not None else evidence
    us = measured["mean_us"] if config["run_mode"] == "performance" else measured["sample_us"][0]
    if returns:
        evidence.update(scope="full" if config["implemented_through"] == "M5" else "partial", implemented_through=config["implemented_through"], native_status="combine-" + config["implemented_through"].lower(),
                        algorithm_bytes=traffic["algorithm_bytes"], stage_payload_bytes=traffic["stage_payload_bytes"],
                        stage_copy_bytes=traffic["stage_copy_bytes"], direct_payload_bytes=traffic["direct_payload_bytes"],
                        direct_wire_bytes=traffic["direct_wire_bytes"], control_publish_bytes=traffic["control_publish_bytes"],
                        wait_us=0, performance_scope="single-launch acquisition; no baseline or performance threshold",
                        algorithm_bandwidth_gbps=traffic["algorithm_bytes"] / (us * 1000) if traffic["algorithm_bytes"] else None,
                        algorithm_bandwidth="effective M1 stage + M3 DIRECT logical payload GB/s / full kernel" if traffic["algorithm_bytes"] else "N/A")
        if config["implemented_through"] in ("M4", "M5"):
            evidence.update({key: traffic[key] for key in ("partial_payload_bytes", "partial_wire_bytes",
                            "partial_self_read_bytes", "partial_stage_read_bytes")})
            evidence["algorithm_bandwidth"] = "effective stage + DIRECT + partial logical payload GB/s / full kernel" if traffic["algorithm_bytes"] else "N/A"
        if config["implemented_through"] == "M5":
            evidence.update({key: traffic[key] for key in ("final_read_bytes", "y_out_bytes", "flag_clear_bytes")})
            evidence.update(performance_scope="acquisition and module parsing; no performance threshold",
                            traffic_scope="per launch; rank-specific logical payload, wire and local byte counts",
                            algorithm_bandwidth="effective stage + DIRECT + partial + yOut logical GB/s / full kernel")
        return evidence
    evidence.update(scope="partial", implemented_through="M2", native_status="combine-m12",
                    algorithm_bytes=traffic["algorithm_bytes"], stage_copy_bytes=traffic["stage_copy_bytes"],
                    control_publish_bytes=traffic["control_publish_bytes"], wait_us=50,
                    algorithm_bandwidth_gbps=traffic["algorithm_bytes"] / (us * 1000) if traffic["algorithm_bytes"] else None,
                    algorithm_bandwidth="effective local stage payload GB/s / full kernel including 50us" if traffic["algorithm_bytes"] else "N/A")
    return evidence


def performance_record(profile_dir, loops, warmup, *, operator, run_id, rank, kernel_sha, config=None, record=None):
    identity = _identity(operator, run_id, rank, kernel_sha, loops, warmup, "performance")
    evidence = measure.performance_record(profile_dir, loops, warmup, operator=operator)
    return _operator_scope(dict(evidence, **identity), operator, config, record)


def breakdown_record(records_path, config, rank, capture_id, output_dir, *,
                     operator, run_id, kernel_sha, mapping_path=None, record=None):
    identity = _identity(operator, run_id, rank, kernel_sha, config["loops"], config["warmup"], "breakdown")
    if capture_id != str(uuid5(NAMESPACE_URL, "tilexr-validation:" + run_id)):
        raise ValueError("capture UUID does not belong to this run_id")
    evidence = measure.breakdown_record(records_path, config, rank, capture_id, output_dir,
        mapping_path, operator=operator, run_id=run_id, kernel_sha=kernel_sha)
    evidence.update(identity)
    _operator_scope(evidence, operator, config, record)
    # Keep the producer's small, hashed summary identical to its scalar evidence.
    evidence.pop("summary_sha256", None)
    measure.atomic_write_json(Path(evidence["summary_path"]), evidence)
    evidence["summary_sha256"] = measure._file_digest(evidence["summary_path"])
    return evidence


def _validate_evidence(evidence, config, operator, rank, run_id, kernel_sha, record=None):
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
            expected = (return_stage_counts(config) if config.get("implemented_through") in ("M3", "M4", "M5") else
                        m12_stage_counts(config) if config.get("implemented_through") == "M2"
                        else dict(kernel_total=64, empty_wait=64))
            if config.get("implemented_through") in ("M3", "M4", "M5") and (
                    "m2_prepare_notify" in stages or "m2_server_wait" in stages):
                expected.update(m2_prepare_notify=8,
                                m2_server_wait=64 - config.get("combine_send_core_num", 32))
            # Token stages are optional for retained module-only captures and
            # empty M4 task sets. When present, every real task must appear once.
            if config.get("implemented_through") in ("M4", "M5") and "m4_token_issue" in stages:
                expected["m4_token_issue"] = record["combine_type2_rows"]
            if config.get("implemented_through") == "M5" and "m5_token_issue" in stages:
                expected["m5_token_issue"] = config["bs"]
            if set(stages) != set(expected) or any(stages[name].get("sample_count") != count
                                                  for name, count in expected.items()):
                raise ValueError("Combine breakdown is missing required module/core-role intervals")
    if operator == "combine":
        expected = _operator_scope({}, operator, config, record, duration=evidence)
        if any(type(evidence.get(key)) is not type(value) or evidence[key] != value
               for key, value in expected.items()):
            raise ValueError("Combine module/data-volume/bandwidth scope mismatch")


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
        _validate_evidence(item, config, operator, record["rank"], expected_run, expected_sha, record)
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
        op_config = dict(config, kernel_scope="dispatch" if operator == "dispatch" else
                         "combine-" + config["implemented_through"].lower() if config.get("implemented_through") in ("M3", "M4", "M5") else
                         "combine-m12" if config.get("implemented_through") == "M2" else "combine-empty")
        summary = measure.summarize_measurements(rows, op_config, kernel_shas[operator], operator=operator)
        durations = [row[config["run_mode"]]["mean_us"] if config["run_mode"] == "performance"
                     else row["breakdown"]["sample_us"][0] for row in sorted(rows, key=lambda row: row["rank"])]
        summary.update(operator=operator, run_id=run_id, last_launch=config["loops"] + config["warmup"] - 1,
            latency_us=measure._statistics(durations), rank_latency_us=durations,
            latency_reduction="statistics across ranks of measured-launch mean" if config["run_mode"] == "performance"
            else "statistics across ranks of last-launch slowest core")
        if operator == "combine" and config.get("implemented_through") in ("M3", "M4", "M5"):
            traffic = [return_traffic(row, config) for row in sorted(rows, key=lambda row: row["rank"])]
            rates = [row["algorithm_bytes"] / (us * 1000) if row["algorithm_bytes"] else None
                     for row, us in zip(traffic, durations)]
            nonzero = [rate for rate in rates if rate is not None]
            summary.update(scope="full" if config["implemented_through"] == "M5" else "partial", implemented_through=config["implemented_through"], native_status="combine-" + config["implemented_through"].lower(), wait_us=0,
                rank_traffic=traffic, rank_algorithm_bandwidth_gbps=rates,
                algorithm_bandwidth_gbps=measure._statistics(nonzero) if nonzero else None,
                algorithm_bandwidth="effective stage + DIRECT logical payload rate; not link bandwidth" if nonzero else "N/A",
                communication_scope="M1 local stage and M3 DIRECT to all targets (including self); M2 control separate",
                performance_scope="single-launch acquisition; no baseline or performance threshold")
            for key in ("algorithm_bytes", "stage_payload_bytes", "stage_copy_bytes", "direct_payload_bytes",
                        "direct_wire_bytes", "control_publish_bytes"):
                summary[key] = sum(row[key] for row in traffic)
            if config["implemented_through"] in ("M4", "M5"):
                for key in ("partial_payload_bytes", "partial_wire_bytes", "partial_self_read_bytes", "partial_stage_read_bytes"):
                    summary[key] = sum(row[key] for row in traffic)
                summary["algorithm_bandwidth"] = "effective stage + DIRECT + partial logical payload rate; not link bandwidth" if nonzero else "N/A"
                summary["communication_scope"] = "M1 local stage; M3 DIRECT and M4 partial to all targets (including self); M4 member reads and M2 control separate"
            if config["implemented_through"] == "M5":
                for key in ("final_read_bytes", "y_out_bytes", "flag_clear_bytes"):
                    summary[key] = sum(row[key] for row in traffic)
                summary.update(performance_scope="acquisition and module parsing; no performance threshold",
                    algorithm_bandwidth="effective stage + DIRECT + partial + yOut logical rate; not link bandwidth",
                    communication_scope="M1 stage; M3/M4 return wire; M4 member reads, M5 local reads/output/Flag clear and M2 control separate",
                    traffic_scope="all-rank bytes per launch; totals below multiply unchanged inputs by explicit iteration counts",
                    measured_launches=config["loops"], submitted_launches=config["loops"] + config["warmup"])
                byte_keys = [key for key in traffic[0] if key.endswith("_bytes")]
                summary["measured_traffic_bytes"] = {key: summary[key] * config["loops"] for key in byte_keys}
                summary["submitted_traffic_bytes"] = {key: summary[key] * (config["loops"] + config["warmup"]) for key in byte_keys}
            summary["algorithm_communication_bytes"] = summary["direct_wire_bytes"] + summary.get("partial_wire_bytes", 0)
        elif operator == "combine" and config.get("implemented_through") == "M2":
            traffic = [m12_traffic(row, config) for row in sorted(rows, key=lambda row: row["rank"])]
            bandwidth = [row["algorithm_bytes"] / (us * 1000) if row["algorithm_bytes"] else None
                         for row, us in zip(traffic, durations)]
            active_bandwidth = [value for value in bandwidth if value is not None]
            summary.update(scope="partial", implemented_through="M2", native_status="combine-m12",
                algorithm_bytes=sum(row["algorithm_bytes"] for row in traffic),
                algorithm_communication_bytes=0, communication_scope="M1 local stage copies; M2 control separately reported",
                rank_traffic=traffic, control_publish_bytes=sum(row["control_publish_bytes"] for row in traffic),
                stage_copy_bytes=sum(row["stage_copy_bytes"] for row in traffic),
                rank_algorithm_bandwidth_gbps=bandwidth,
                algorithm_bandwidth_gbps=measure._statistics(active_bandwidth) if active_bandwidth else None,
                algorithm_bandwidth=("effective local stage payload GB/s over nonzero-work ranks / full kernel including 50us"
                                     if active_bandwidth else "N/A"),
                bandwidth="local copy payload rate; not inter-server communication bandwidth",
                wait_us=50)
        else:
            _empty_scope(summary, operator)
        result[operator] = summary
    return result


def render_operator_measurements(summaries):
    parts = []
    for operator, summary in summaries.items():
        stats = summary["latency_us"]
        text = (f"{operator}: backend={summary['backend']} native={summary['native_status']} "
                f"min={stats['minimum']:.3f} us median={stats['median']:.3f} us max={stats['maximum']:.3f} us\n"
                f"kernel={summary['kernel']} sha256={summary['kernel_sha256']}")
        if operator == "combine" and summary.get("implemented_through") in ("M3", "M4", "M5"):
            text += (f"\nscope={summary['scope']} implemented_through={summary['implemented_through']} stage_payload={summary['stage_payload_bytes']}B "
                     f"stage_read_write={summary['stage_copy_bytes']}B direct_payload={summary['direct_payload_bytes']}B "
                     f"direct_wire={summary['direct_wire_bytes']}B control_publish={summary['control_publish_bytes']}B")
            if summary["implemented_through"] in ("M4", "M5"):
                text += (f" partial_payload={summary['partial_payload_bytes']}B partial_wire={summary['partial_wire_bytes']}B"
                         f" partial_self_read={summary['partial_self_read_bytes']}B partial_stage_read={summary['partial_stage_read_bytes']}B")
            rates = summary["algorithm_bandwidth_gbps"]
            text += (f"\nlogical_payload_GB/s min={rates['minimum']:.3f} median={rates['median']:.3f} max={rates['maximum']:.3f}"
                     if rates else "\nlogical_payload_GB/s=N/A")
            if summary["implemented_through"] == "M5":
                text += (f"\nfinal_read={summary['final_read_bytes']}B yOut={summary['y_out_bytes']}B "
                         f"flag_clear={summary['flag_clear_bytes']}B per_launch; "
                         f"measured={summary['measured_launches']} submitted={summary['submitted_launches']}")
                text += "（按单次字节和测量时延统计；非链路带宽；无性能阈值）"
            else:
                text += "（单轮采集；无固定等待；不建立性能基线，非链路带宽）"
        elif operator == "combine" and summary.get("implemented_through") == "M2":
            rates = summary["algorithm_bandwidth_gbps"]
            text += (f"\nscope=partial implemented_through=M2 stage_payload={summary['algorithm_bytes']}B "
                     f"stage_read_write={summary['stage_copy_bytes']}B control_publish={summary['control_publish_bytes']}B")
            text += (f"\nlocal_payload_GB/s(nonzero-work ranks) min={rates['minimum']:.3f} "
                     f"median={rates['median']:.3f} max={rates['maximum']:.3f}" if rates else "\nlocal_payload_GB/s=N/A")
            text += "（完整kernel时长含50us等待；控制流量不计业务带宽）"
        elif operator == "combine":
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
