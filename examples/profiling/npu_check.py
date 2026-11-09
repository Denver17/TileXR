#!/usr/bin/env python3
# 显式运行 Add4 并核对 lane、共享事件边界和报告；依赖 manifest.py，不验证 Add4 数值输出。
# Explicitly run Add4 and check lanes, shared event boundaries and reports via manifest.py; this does not verify numerical Add4 output.
"""Explicit Ascend950 capture, shared-boundary and report qualification runner."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
from uuid import uuid4

from manifest import prepare, complete
from tilexr_profiling import generate_reports
from tilexr_profiling.analysis import analyze_run
from tilexr_profiling.records import load_record_files
from tilexr_profiling.pairing import load_profile_run


def qualify(binary, mapping_path, output, device, cycles, clock_basis):
    if not math.isfinite(cycles) or cycles <= 0 or not clock_basis.strip():
        raise ValueError("positive verified cycles_per_us and clock basis required")
    output.mkdir(parents=True, exist_ok=False)
    mapping = json.loads(mapping_path.read_text())
    mapping["cycles_per_us"] = cycles
    (output / "mapping.json").write_text(json.dumps(mapping, indent=2) + "\n")
    manifest = prepare(mapping, clock_basis=clock_basis, device=device)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    records = output / "capture.records.bin"
    run = subprocess.run([str(binary), str(records)],
        env=dict(os.environ, TILEXR_ADD4_DEVICE_ID=str(device)), text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    (output / "operator.log").write_text(run.stdout)
    if run.returncode:
        raise RuntimeError("Add4 failed; see " + str(output / "operator.log"))
    captured = load_record_files([records])[0]
    if {s.id for s in captured.streams} != set(range(4)):
        raise AssertionError("missing/unexpected logical lane")
    expected_ids = (1,10,11,10,11,10,11,10,11,20,201,202,203,21,30,31,2)
    for stream in captured.streams:
        if not stream.complete or stream.dropped_count or stream.overflow:
            raise AssertionError("incomplete/overflowed lane")
        if tuple(r.event_id for r in stream.records) != expected_ids:
            raise AssertionError("incorrect event sequence")
    manifest = complete(manifest, mapping, records)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    profile = load_profile_run(mapping, [records], manifest=manifest)
    for launch in profile.launches:
        for lane in launch.lanes:
            parent = next(s for s in lane.samples if s.stage_id == 2)
            children = [s for s in lane.samples if s.stage_id == 4]
            if len(children) != 3 or {s.end_event_id for s in children} != {201,202,203}:
                raise AssertionError("missing shared-boundary intervals")
            if any((s.raw_start, s.source, s.source_index) !=
                   (parent.raw_start, parent.source, parent.source_index) for s in children):
                raise AssertionError("shared boundary lost its original source identity")
    for stage in analyze_run(profile).stages.values():
        if (stage.bytes is None or not stage.byte_semantics or
                stage.bandwidth_gbps is None or stage.bandwidth_gbps <= 0):
            raise AssertionError("missing Add4 byte metric, semantics or bandwidth")
    result = generate_reports(mapping,[records],output / "report", manifest=manifest)
    if result.capture_status != "complete" or len(result.artifacts) != 3:
        raise AssertionError("unexpected report status/artifact set")
    results = [{"capture_id":str(captured.capture_id), "status":result.capture_status,
                "lanes":4, "records_per_lane":17, "dropped_per_lane":0}]
    evidence={"schema":"tilexr_profiling_npu_validation.v1","device":device,
              "binary":str(binary),"binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest(),
              "cycles_per_us":cycles,"clock_basis":clock_basis,"cases":results}
    (output / "validation.json").write_text(json.dumps(evidence,indent=2)+"\n")
    print("NPU lane/shared-boundary/report qualification passed: " + str(output))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,required=True)
    parser.add_argument("--mapping",type=Path,required=True)
    parser.add_argument("--output-root",type=Path,required=True)
    parser.add_argument("--device",type=int,required=True)
    parser.add_argument("--cycles-per-us",type=float,required=True)
    parser.add_argument("--clock-basis",required=True)
    args=parser.parse_args()
    if args.device < 0: parser.error("device must be nonnegative")
    qualify(args.binary.resolve(),args.mapping.resolve(),args.output_root.resolve()/str(uuid4()),
            args.device,args.cycles_per_us,args.clock_basis)


if __name__ == "__main__":
    main()
