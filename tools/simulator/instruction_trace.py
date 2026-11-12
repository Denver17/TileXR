#!/usr/bin/env python3
"""将 simulator 指令 dump 配对为 Trace，并统计驻留、重叠和核结束分布。

只读取调用者选定的 dump，保留 PC、InstrID 和原始 cycle；生成的驻留区间包含
依赖等待，不能用作执行单元利用率。调用者负责证明 dump 的公共时钟与换算频率。
"""

import argparse
import collections
import hashlib
import json
import math
import pathlib
import re
import sys


CORE = re.compile(r"(?:David_V100|Ascend910BC)_(\d+)_aicore(\d+)_(aiv[01]|aic)(?:_|\.)")
RECORD = re.compile(
    r"\[time:\s*(\d+)\]\s*-\s*\[(Popped|Retire) Instruction Pipe:\s*([^\]]+)\]"
    r"\s*-\s*\(\[PC:\s*(0x[0-9a-fA-F]+|\d+)\],\s*\[InstrID:\s*(\d+)\],"
    r"\s*\[InstrName:\s*([^\]]+)\]\)")


def write_json(path, value):
    pathlib.Path(path).write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")


def interval_union(intervals):
    merged = []
    for start, end in sorted(intervals):
        if not merged or start > merged[-1][1]:
            merged.append([start, end])
        else:
            merged[-1][1] = max(merged[-1][1], end)
    return merged


def interval_length(intervals):
    return sum(end - start for start, end in intervals)


def intersection_length(left, right):
    i = j = 0
    result = 0
    while i < len(left) and j < len(right):
        result += max(0, min(left[i][1], right[j][1]) - max(left[i][0], right[j][0]))
        if left[i][1] <= right[j][1]:
            i += 1
        else:
            j += 1
    return result


def distribution(values):
    values = sorted(values)
    def percentile(fraction):
        position = (len(values) - 1) * fraction
        low = int(position)
        return values[low] + (values[math.ceil(position)] - values[low]) * (position - low)
    return {"count": len(values), "min": values[0], "median": percentile(.5),
            "p90": percentile(.9), "p99": percentile(.99), "max": values[-1],
            "mean": sum(values) / len(values)}


def parse_dumps(paths):
    events = []
    diagnostics = collections.Counter()
    inputs = []
    seen = set()
    for path in paths:
        core_match = CORE.search(path.name)
        if not core_match:
            raise ValueError("Unrecognized dump core: %s" % path.name)
        chip, aicore = map(int, core_match.group(1, 2))
        core = "AICORE%d_%s" % (aicore, core_match.group(3).upper())
        pending = {}
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for line_number, raw in enumerate(stream, 1):
                digest.update(raw)
                line = raw.decode("utf-8", errors="strict")
                if "Popped Instruction" not in line and "Retire Instruction" not in line:
                    continue
                match = RECORD.search(line)
                if not match:
                    diagnostics["malformed_records"] += 1
                    continue
                time, action, pipe, pc, instr_id, name = match.groups()
                time, instr_id = int(time), int(instr_id)
                pc = int(pc, 16 if pc.startswith("0x") else 10)
                pipe, name = pipe.strip(), name.strip()
                key = (pipe, instr_id)
                if action == "Popped":
                    if key in pending:
                        diagnostics["duplicate_popped"] += 1
                        continue
                    pending[key] = (time, pc, name, line_number)
                    continue
                if key not in pending:
                    diagnostics["unmatched_retire"] += 1
                    continue
                start, start_pc, start_name, start_line = pending.pop(key)
                if pc != start_pc or name != start_name:
                    diagnostics["identity_mismatch"] += 1
                    continue
                if time < start:
                    diagnostics["negative_duration"] += 1
                    continue
                identity = (chip, core, pipe, instr_id, start, time, pc)
                if identity in seen:
                    diagnostics["duplicate_events"] += 1
                    continue
                seen.add(identity)
                events.append({"chip": chip, "core": core, "pipe": pipe, "instr_id": instr_id,
                               "pc": hex(pc), "name": name, "start_cycle": start,
                               "end_cycle": time, "duration_cycle": time - start,
                               "file": path.name, "popped_line": start_line,
                               "retire_line": line_number})
        diagnostics["unmatched_popped"] += len(pending)
        inputs.append({"file": path.name, "bytes": path.stat().st_size, "sha256": digest.hexdigest()})
    if not events:
        raise ValueError("No paired instruction events")
    return events, dict(diagnostics), inputs


def summarize(events, clock_ghz):
    scale = clock_ghz * 1000  # cycle / (GHz * 1000) = microseconds
    first = min(event["start_cycle"] for event in events)
    last = max(event["end_cycle"] for event in events)
    span = last - first
    if span <= 0:
        raise ValueError("Instruction trace has no positive time span")
    threads = collections.defaultdict(list)
    groups = collections.defaultdict(list)
    core_ends = {}
    for event in events:
        threads[event["chip"], event["core"], event["pipe"]].append(
            (event["start_cycle"], event["end_cycle"]))
        groups[event["pipe"], event["name"], event["pc"]].append(event["duration_cycle"] / scale)
        key = (event["chip"], event["core"])
        core_ends[key] = max(core_ends.get(key, 0), event["end_cycle"])
    unions = {key: interval_union(intervals) for key, intervals in threads.items()}
    pipelines = collections.defaultdict(list)
    for (_, _, pipe), intervals in unions.items():
        pipelines[pipe].append(interval_length(intervals))
    cores = []
    for (chip, core), end in sorted(core_ends.items()):
        vec = unions.get((chip, core, "VEC"), [])
        mte2 = unions.get((chip, core, "MTE2"), [])
        mte3 = unions.get((chip, core, "MTE3"), [])
        cores.append({"chip": chip, "core": core, "end_from_first_us": (end - first) / scale,
                      "vec_mte2_overlap_us": intersection_length(vec, mte2) / scale,
                      "vec_mte3_overlap_us": intersection_length(vec, mte3) / scale,
                      "mte2_mte3_overlap_us": intersection_length(mte2, mte3) / scale})
    instructions = [{"pipe": pipe, "name": name, "pc": pc,
                     "residence_us": distribution(durations), "sum_residence_us": sum(durations)}
                    for (pipe, name, pc), durations in groups.items()]
    instructions.sort(key=lambda group: group["sum_residence_us"], reverse=True)
    long_events = sorted(events, key=lambda event: event["duration_cycle"], reverse=True)[:40]
    return {"metric": "Popped-to-Retire residence, including dependency waiting",
            "clock_ghz": clock_ghz, "start_cycle": first, "end_cycle": last,
            "span_us": span / scale, "instruction_events": len(events),
            "chips_observed": sorted({event["chip"] for event in events}),
            "thread_count": len(threads), "core_count": len(cores),
            "pipelines": {pipe: {"thread_count": len(values),
                                  "per_thread_residence_us": distribution([v / scale for v in values]),
                                  "mean_union_fraction_of_global_span": sum(values) / len(values) / span}
                          for pipe, values in sorted(pipelines.items())},
            "core_end_from_first_us": distribution([core["end_from_first_us"] for core in cores]),
            "cores": cores, "instructions_by_pc": instructions,
            "long_events": [dict(event, duration_us=event["duration_cycle"] / scale,
                                 start_from_first_us=(event["start_cycle"] - first) / scale)
                            for event in long_events]}


def chrome_trace(events, clock_ghz):
    thread_keys = sorted({(event["chip"], event["core"], event["pipe"]) for event in events})
    tids = {key: index for index, key in enumerate(thread_keys)}
    records = [{"ph": "M", "name": "thread_name", "pid": chip, "tid": tid,
                "args": {"name": "CHIP%d_%s_%s" % (chip, core, pipe)}}
               for (chip, core, pipe), tid in tids.items()]
    for event in sorted(events, key=lambda value: (value["start_cycle"], value["chip"], value["core"])):
        records.append({"ph": "X", "pid": event["chip"],
                        "tid": tids[event["chip"], event["core"], event["pipe"]],
                        "name": event["name"], "ts": event["start_cycle"] / (clock_ghz * 1000),
                        "dur": event["duration_cycle"] / (clock_ghz * 1000),
                        "args": {key: event[key] for key in ("pc", "instr_id", "start_cycle",
                                                             "end_cycle", "file", "popped_line", "retire_line")}})
    return {"traceEvents": records, "displayTimeUnit": "ns",
            "otherData": {"source_time_unit": "cycle", "clock_ghz": clock_ghz,
                          "duration_semantics": "Popped-to-Retire residence"}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump_dir", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--clock-ghz", type=float, required=True,
                        help="仅在已证明所有所选 dump 使用此公共 cycle 时钟时设置")
    args = parser.parse_args()
    if not math.isfinite(args.clock_ghz) or args.clock_ghz <= 0:
        parser.error("clock frequency must be finite and positive")
    args.output.mkdir(parents=True, exist_ok=False)
    paths = sorted(path for path in args.dump_dir.glob("*.dump") if "sudetail" not in path.name)
    events, diagnostics, inputs = parse_dumps(paths)
    report = summarize(events, args.clock_ghz)
    report.update(status="PARTIAL" if any(diagnostics.values()) else "PASS",
                  pairing_diagnostics=diagnostics, inputs=inputs)
    write_json(args.output / "instruction-statistics.json", report)
    write_json(args.output / "trace-us.json", chrome_trace(events, args.clock_ghz))
    print(json.dumps({key: report[key] for key in ("status", "instruction_events", "chips_observed",
                                                 "thread_count", "span_us", "pairing_diagnostics")}))
    return 1 if report["status"] == "PARTIAL" else 0


if __name__ == "__main__":
    sys.exit(main())
