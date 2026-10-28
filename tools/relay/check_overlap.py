"""Validate benchmark same-device, cross-stream overlap in framework traces."""

import argparse
from decimal import Decimal
import json
from pathlib import Path

MODES = ("serial", "serial_tiled", "overlap")


def interval(task):
    start = Decimal(str(task["ts"]))
    return start, start + Decimal(str(task["dur"]))


def intersection(left, right):
    a, b = interval(left)
    c, d = interval(right)
    return max(Decimal(0), min(b, d) - max(a, c))


def analyze(events):
    rounds = [e for e in events if e.get("ph") == "X" and
              e.get("name", "").split("/")[0] in MODES
              and "/round" in e.get("name", "")]
    hardware = sorted(
        [(e, Decimal(str(e["ts"]))) for e in events if e.get("ph") == "X" and
         e.get("args", {}).get("Task Type", "").startswith("KERNEL_")],
        key=lambda task: task[1])
    rows = []
    for mark in rounds:
        start, end = interval(mark)
        tasks = [e for e, task_start in hardware if start <= task_start < end]
        communication = [e for e in tasks if e["name"] == "TileXRAllGather_float16_t"]
        compute = [e for e in tasks if e["name"] == "main_kernel" and
                   e["args"]["Task Type"] in ("KERNEL_MIX_AIC", "KERNEL_AICORE")]
        if not communication or len(communication) != len(compute):
            raise ValueError(f"{mark['name']}: missing or unequal AllGather/GEMM task counts")
        pairs = []
        for i, a in enumerate(communication):
            for j, b in enumerate(compute):
                if a["pid"] != b["pid"] or a["tid"] == b["tid"]:
                    continue
                overlap = intersection(a, b)
                if overlap:
                    if i <= j:
                        raise ValueError(f"{mark['name']}: GEMM overlaps its own or later input tile")
                    pairs.append({"communication_tile": i, "compute_tile": j,
                                  "overlap_us": float(overlap),
                                  "hardware_pid": a["pid"],
                                  "communication_stream": a["tid"], "compute_stream": b["tid"]})
        rows.append({"round": mark["name"], "allgather_tasks": len(communication),
                     "gemm_tasks": len(compute),
                     "allgather_kernel_us": float(sum(Decimal(str(e["dur"])) for e in communication)),
                     "gemm_kernel_us": float(sum(Decimal(str(e["dur"])) for e in compute)),
                     "overlap_us": sum(p["overlap_us"] for p in pairs), "pairs": pairs})
    if not rows:
        raise ValueError("no tagged rounds in trace")
    for mode in MODES:
        selected = [r for r in rows if r["round"].startswith(mode + "/")]
        if not selected:
            raise ValueError(f"missing {mode} control")
        overlap = sum(r["overlap_us"] for r in selected)
        if mode == "overlap" and overlap <= 0:
            raise ValueError("no hardware overlap in pipelined mode")
        if mode != "overlap" and overlap != 0:
            raise ValueError(f"unexpected overlap in serial control {mode}")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    args = parser.parse_args()
    results = {}
    for rank in range(2):
        paths = list((args.results / f"rank{rank}").rglob("trace_view.json"))
        if len(paths) != 1:
            raise ValueError(f"rank{rank}: expected exactly one trace, got {len(paths)}")
        raw = json.loads(paths[0].read_text())
        events = raw.get("traceEvents", []) if isinstance(raw, dict) else raw
        rows = analyze(events)
        numerical = json.loads((args.results / f"result-rank{rank}.json").read_text())
        if numerical.get("correct") is not True:
            raise ValueError(f"rank{rank}: numerical validation did not finish")
        count = numerical["iterations"] + numerical["warmup"]
        expected_rounds = {f"{mode}/round{i}" for mode in MODES
                           for i in range(count)}
        if len(rows) != len(expected_rounds) or {r["round"] for r in rows} != expected_rounds:
            raise ValueError(f"rank{rank}: incomplete or duplicate round trace")
        shape = numerical["shape"]
        for row in rows:
            tiles = 1 if row["round"].startswith("serial/") else shape["rows"] // 128
            if row["allgather_tasks"] != tiles:
                raise ValueError(f"rank{rank} {row['round']}: wrong hardware tile count")
        results[f"rank{rank}"] = {"trace": str(paths[0]), "rounds": rows}
    output = args.results / "overlap.json"
    output.write_text(json.dumps(results, indent=2))
    for rank, result in results.items():
        rows = [r for r in result["rounds"] if r["round"].startswith("overlap/")]
        print(f"PASS {rank}: {sum(len(r['pairs']) for r in rows)} overlapping task pairs, "
              f"{sum(r['overlap_us'] for r in rows):.3f} us intersection")


if __name__ == "__main__":
    main()
