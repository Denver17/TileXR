#!/usr/bin/env python3
# 为固定 Add4 工作负载预先声明 lane 和字节量，采集后绑定 UUID；供 run_demo.sh 和 npu_check.py 使用。
# Declare fixed Add4 lanes and byte counts before capture, then bind its UUID for run_demo.sh and npu_check.py.
"""Declare Add4 workload bytes before launch, then bind the exported capture."""

import argparse
import json
import os
from pathlib import Path
import socket
from uuid import uuid4

from tilexr_profiling import MAPPING_SCHEMA_V2, load_manifest, mapping_fingerprint
from tilexr_profiling.records import load_record_files
from tilexr_profiling.mapping import load_mapping


def validate_mapping(mapping):
    """The shipped kernel records event 20 once for all four compute intervals."""
    parsed = load_mapping(mapping)
    names = [stage.name for stage in parsed.stages]
    expected_names = ["kernel_total", "copy_input", "accumulate_window", "copy_out", "partial_sum"]
    expected_intervals = [(0, None, 1, 2), (1, None, 10, 11), (2, None, 20, 21), (3, None, 30, 31),
                          (4, 0, 20, 201), (4, 1, 20, 202), (4, 2, 20, 203)]
    actual = [(event.stage_id, event.instance_id, event.edge, event.id, event.occurrence)
              for event in parsed.events]
    expected = [(stage_id, instance_id, edge, event_id, None)
                for stage_id, instance_id, begin, end in expected_intervals
                for edge, event_id in (("begin", begin), ("end", end))]
    if (parsed.schema != MAPPING_SCHEMA_V2 or parsed.operator != "Add4" or
            names != expected_names or actual != expected):
        raise ValueError("Add4-v2 requires its shared event 20 mapping")


def prepare(mapping, *, clock_basis=None, device=None):
    """The workload is fixed by this example, independently of returned records."""
    validate_mapping(mapping)
    if clock_basis is None:
        clock_basis = os.environ.get("TILEXR_PROFILING_CLOCK_BASIS")
    if device is None:
        device = int(os.environ.get("TILEXR_ADD4_DEVICE_ID", "0"))
    if isinstance(device, bool) or not isinstance(device, int) or device < 0:
        raise ValueError("device must be a nonnegative host-local ACL ordinal")
    tile_bytes = 2048 * 4
    # Bytes follow each stage's inclusive intervals, including cumulative overlap.
    workload = {
        "kernel_total": (5 * tile_bytes, "algorithm GM read+write bytes"),
        "copy_input": (4 * tile_bytes, "algorithm GM read bytes"),
        "accumulate_window": (9 * tile_bytes, "algorithm UB read+write bytes"),
        "copy_out": (tile_bytes, "algorithm GM write bytes"),
        "partial_sum": (18 * tile_bytes, "algorithm UB read+write bytes across cumulative windows"),
    }
    return {
        "schema": "tilexr_profiling_manifest.v1",
        "attempt_id": str(uuid4()),
        "capture_id": None,
        "mapping_sha256": mapping_fingerprint(mapping),
        "participants": [
            {"stream_id": lane, "rank": 0, "logical_lane": lane, "sampled": True,
             "host": socket.gethostname(), "device": {"id": device, "namespace": "host-local ACL ordinal"}}
            for lane in range(4)
        ],
        "versions": {"sdk_version": "1.0.0", "kernel_version": "Add4-v2"},
        "environment": {
            key: value for key, value in (
                ("cann_version", os.environ.get("TILEXR_PROFILING_CANN_VERSION")),
                ("driver_version", os.environ.get("TILEXR_PROFILING_DRIVER_VERSION")),
            ) if value
        },
        "attributes": {
            "clock_basis": clock_basis,
            "clock_conversion_basis": clock_basis,
            "start_boundary": "event:20",
            "shape": {"input_count": 4, "logical_lanes": 4,
                      "elements_per_lane": 2048, "dtype": "float32"},
        },
        "execution": {"kind": "compute"},
        "stage_metrics": [
            {"stream_id": lane, "stage_id": stage_id, "stage": stage["name"],
             "bytes": workload[stage["name"]][0], "byte_semantics": workload[stage["name"]][1],
             "scope": "per_lane_total", "source": "Add4-v2: fixed 2048 fp32 elements/lane"}
            for lane in range(4) for stage_id, stage in enumerate(mapping["stages"])
        ],
    }


def complete(manifest, mapping, records):
    """Bind the actual UUID without deriving expected lanes or bytes from records."""
    validate_mapping(mapping)
    captured = load_record_files([records])[0]
    capture_id = str(captured.capture_id)
    if manifest["capture_id"] not in (None, capture_id):
        raise ValueError("Add4 manifest is already bound to a different capture")
    return load_manifest(dict(manifest, capture_id=capture_id), mapping)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mapping", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--records", type=Path)
    parser.add_argument("--device", type=int)
    args = parser.parse_args()
    mapping = json.loads(args.mapping.read_text())
    if args.records:
        manifest = complete(json.loads(args.manifest.read_text()), mapping, args.records)
        args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    else:
        manifest = prepare(mapping, device=args.device)
        with args.manifest.open("x") as output:
            json.dump(manifest, output, indent=2)
            output.write("\n")
        print(manifest["attempt_id"])


if __name__ == "__main__":
    main()
