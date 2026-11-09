#!/usr/bin/env python3
"""读取两 rank 的采样和首轮 offset，输出后续轮次相对逻辑 rank 0 的残差与 causal interval。

由 test_clock_sync_e2e 生成输入；四时间戳估计依赖路径对称假设，不是外部时钟测量。
Read test_clock_sync_e2e samples/initial offsets and report later residuals and causal intervals
relative to logical rank 0. Four-timestamp estimates assume path symmetry, not an external clock reference.
"""
import argparse
import csv
import json
from pathlib import Path


def trunc_half(value):
    return value // 2 if value >= 0 else -((-value) // 2)


def evaluate(row, fixed_offset):
    """Return twice the residual and causal endpoints in clock cycles.

    t0/t3 are local, t1/t2 are root. Correction is local + fixed_offset.
    Under equal effective forward/reverse delays, residual is corrected local
    minus root. Keep half cycles instead of rounding each held-out sample.
    """
    t0, t1, t2, t3 = (int(row[key]) for key in ("t0", "t1", "t2", "t3"))
    if int(row["status"]) != 0 or min(t0, t1, t2, t3) <= 0:
        raise ValueError("failed/zero timestamp sample")
    delay = (t3 - t0) - (t2 - t1)
    if t3 < t0 or t2 < t1 or delay < 0 or delay != int(row["rtt_cycles"]):
        raise ValueError("invalid timestamp ordering or RTT")
    offset2 = (t1 - t0) + (t2 - t3)
    return {
        "residual_times_two_cycles": 2 * fixed_offset - offset2,
        "causal_lower_cycles": fixed_offset - (t1 - t0),
        "causal_upper_cycles": fixed_offset - (t2 - t3),
        "offset_times_two_cycles": offset2,
        "delay_cycles": delay,
    }


def nearest_rank(values, percent):
    values = sorted(values)
    return values[max(0, (len(values) * percent + 99) // 100 - 1)]


def analyze(directory, output):
    directory, output = Path(directory), Path(output)
    clocks = [json.loads((directory / ("frozen_calibration_rank%d.json" % rank)).read_text())
              for rank in (0, 1)]
    if clocks[0] != clocks[1]:
        raise ValueError("ranks disagree on frozen calibration")
    clock = clocks[0]
    if (clock["calibration_round"] != 0 or clock["validation_first_round"] != 1 or
            len(clock["offset_cycles"]) != 2 or clock["offset_cycles"][0] != 0):
        raise ValueError("requires two ranks and first-round-only calibration")
    frequency = int(clock["cycles_per_second"])
    if frequency <= 0:
        raise ValueError("invalid counter frequency")
    fixed = int(clock["offset_cycles"][1])
    with (directory / "samples_rank1.csv").open() as stream:
        raw = list(csv.DictReader(stream))
    if not raw:
        raise ValueError("empty raw sample file")
    groups = {}
    for row in raw:
        if int(row["rank"]) != 1 or int(row["applied_offset_cycles"]) != fixed:
            raise ValueError("wrong rank or changing applied correction")
        round_id = int(row["round"])
        group = groups.setdefault(round_id, [])
        if int(row["sequence"]) != len(group):
            raise ValueError("missing/repeated/out-of-order sample")
        group.append((row, evaluate(row, fixed)))
    if sorted(groups) != list(range(len(groups))) or len(groups) < 2:
        raise ValueError("requires calibration followed by independent validation rounds")
    if any(len(group) != len(groups[0]) for group in groups.values()):
        raise ValueError("incomplete round")
    # Independently reproduce the production minimum-delay/first-sequence rule.
    selected, selected_value = min(groups[0], key=lambda item:
        (item[1]["delay_cycles"], int(item[0]["sequence"])))
    if trunc_half(selected_value["offset_times_two_cycles"]) != fixed:
        raise ValueError("frozen offset does not match calibration samples")
    ns_per_cycle = 1e9 / frequency
    rows = []
    for round_id in sorted(groups):
        if round_id == 0:
            continue
        for row, value in groups[round_id]:
            signed = value["residual_times_two_cycles"] * ns_per_cycle / 2
            rows.append({
                "round": round_id, "sequence": int(row["sequence"]),
                "local_t0_cycles": int(row["t0"]),
                "elapsed_since_calibration_ns": (int(row["t0"]) - int(selected["t0"])) * ns_per_cycle,
                "applied_offset_cycles": fixed,
                "estimated_signed_residual_ns": signed,
                "estimated_pairwise_skew_ns": abs(signed),
                "causal_lower_ns": value["causal_lower_cycles"] * ns_per_cycle,
                "causal_upper_ns": value["causal_upper_cycles"] * ns_per_cycle,
                "effective_rtt_ns": value["delay_cycles"] * ns_per_cycle,
            })
    maximum = max(rows, key=lambda row: row["estimated_pairwise_skew_ns"])
    absolute = [row["estimated_pairwise_skew_ns"] for row in rows]
    output.mkdir(parents=True, exist_ok=True)
    with (output / "residual_samples.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    result = {
        "schema": "tilexr_heldout_clock_residual.v1",
        "source": str(directory.resolve()), "rank_size": 2,
        "correction": "root_cycles = local_cycles + frozen_offset_cycles",
        "model_assumptions": ["equal effective forward and reverse one-way delays",
                              "clock offset constant during each individual exchange"],
        "path_symmetry_verified": False,
        "independently_measured_max_clock_skew_ns": None,
        "cycles_per_second": frequency, "frozen_offset_cycles": fixed,
        "calibration_samples": len(groups[0]), "calibration_selected_sequence": int(selected["sequence"]),
        "validation_rounds": len(groups) - 1, "validation_samples": len(rows),
        "rejected_samples": 0, "calibration_samples_reused_for_validation": 0,
        "validation_span_ns": (max(row["local_t0_cycles"] for row in rows) -
                               min(row["local_t0_cycles"] for row in rows)) * ns_per_cycle,
        "estimated_max_pairwise_skew_ns": max(absolute),
        "estimated_p50_pairwise_skew_ns": nearest_rank(absolute, 50),
        "estimated_p95_pairwise_skew_ns": nearest_rank(absolute, 95),
        "estimated_p99_pairwise_skew_ns": nearest_rank(absolute, 99),
        "worst_sample": maximum,
        "max_causal_absolute_envelope_ns": max(max(abs(row["causal_lower_ns"]),
            abs(row["causal_upper_ns"])) for row in rows),
        "notes": "Full maximum without trimming. Causal intervals need nonnegative effective delays; "
                 "they do not establish symmetry or an independent clock residual. "
                 "New per-round candidate calibrations are ignored. Only first-round correction is applied.",
    }
    (output / "residual_summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    arguments = parser.parse_args()
    print(json.dumps(analyze(arguments.input, arguments.output), indent=2))
