#!/usr/bin/env python3
# 用普通 Add4 binary 的独立采集提供 timer 观测，交给 CalibrationSession 拟合；不改变原始 records。
# Provide timer observations from independent captures of the ordinary Add4 binary to CalibrationSession without modifying raw records.
"""Bracket a measurement with explicit Ascend950 Add4 timer observations.

This is an opt-in device runner. It uses the ordinary Add4 binary and preserves
its raw captures; it does not introduce another recorder.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import re
import subprocess
import time
from uuid import uuid4

from tilexr_profiling import CalibrationSession
from tilexr_profiling.io import atomic_write_json
from tilexr_profiling.records import load_record_files


class Add4TimerProbe:
    """Read GetSystemCycle through a fresh Add4 capture."""

    def __init__(self, binary, output, device):
        if isinstance(device, bool) or not isinstance(device, int) or device < 0:
            raise ValueError("device must be a nonnegative host-local ACL ordinal")
        self.binary = Path(binary).resolve(strict=True)
        self.output = Path(output).resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.device = {"host": socket.gethostname(), "id": device,
                       "namespace": "host-local ACL ordinal"}
        self.binary_sha256 = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        self.capture_ids = set()

    def __call__(self):
        directory = self.output / str(uuid4())
        directory.mkdir()
        records = directory / "capture.records.bin"
        # Each observation uses the ordinary capture path.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("TILEXR_ADD4_") and key not in {
                   "TILEXR_PROFILING_MAPPING_SHA256", "TILEXR_PROFILING_ATTEMPT_ID"}}
        env.update(TILEXR_ADD4_DEVICE_ID=str(self.device["id"]))
        completed = subprocess.run([str(self.binary), str(records)], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        (directory / "operator.log").write_text(completed.stdout, encoding="utf-8")
        if completed.returncode:
            raise RuntimeError("timer probe runtime failure: " + str(directory))
        identity = re.search(r"^actual SoC=(\S+), device=(\d+)$", completed.stdout, re.MULTILINE)
        if (identity is None or int(identity[2]) != self.device["id"] or
                not identity[1].startswith("Ascend950")):
            raise ValueError("timer probe runtime device identity mismatch")
        capture = load_record_files([records])[0]
        if capture.capture_id in self.capture_ids:
            raise ValueError("timer probe reused a capture UUID")
        expected_ids = (1, 10, 11, 10, 11, 10, 11, 10, 11, 20, 201, 202, 203, 21, 30, 31, 2)
        if {stream.id for stream in capture.streams} != set(range(4)):
            raise ValueError("timer probe requires the unfiltered four-lane Add4 binary")
        for stream in capture.streams:
            if (not stream.complete or stream.overflow or stream.dropped_count or
                    tuple(record.event_id for record in stream.records) != expected_ids):
                raise ValueError("timer probe requires a complete unfiltered Add4 capture")
        self.capture_ids.add(capture.capture_id)
        # This marker executed within the enclosing host bracket. All raw lane
        # cycles remain in the retained capture, including the other observations.
        return capture.streams[0].records[0].timestamp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", type=int, required=True)
    parser.add_argument("--capture-id", required=True, help="UUID of the enclosed measurement")
    parser.add_argument("--max-error-ns", type=float, required=True)
    parser.add_argument("--max-drift-ppm", type=float, required=True)
    parser.add_argument("--samples-per-side", type=int, default=3)
    parser.add_argument("--interval-seconds", type=float, default=0.1)
    parser.add_argument("measurement", nargs=argparse.REMAINDER,
                        help="-- COMMAND ARGS; must produce the declared measurement UUID on this device")
    args = parser.parse_args()
    command = args.measurement[1:] if args.measurement[:1] == ["--"] else args.measurement
    if not command or not 2 <= args.samples_per_side <= 32 or not 0 <= args.interval_seconds <= 10:
        parser.error("requires a measurement command, 2..32 samples per side and interval 0..10 seconds")
    args.output.mkdir(parents=True, exist_ok=False)
    probe = Add4TimerProbe(args.binary, args.output / "probes", args.device)
    session = CalibrationSession(probe, capture_id=args.capture_id, device=probe.device,
        clock_domain="uncalibrated_device_system_timer",
        source="Ascend950 Add4-v2 GetSystemCycle; binary SHA256 " + probe.binary_sha256,
        max_error_ns=args.max_error_ns, max_drift_ppm=args.max_drift_ppm)
    observations = []
    for side in ("before", "after"):
        for _ in range(args.samples_per_side):
            observations.append(session.sample())
            # Preserve evidence even when a later probe/measurement/fit fails.
            atomic_write_json(args.output / "paired-observations.json", observations)
            time.sleep(args.interval_seconds)
        if side == "before":
            with (args.output / "measurement.log").open("w", encoding="utf-8") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=3600)
    atomic_write_json(args.output / "calibration.json", session.finish())
    atomic_write_json(args.output / "probe-provenance.json", {
        "binary": str(probe.binary), "binary_sha256": probe.binary_sha256,
        "measurement_command": command, "capture_id": args.capture_id,
        "probe_capture_ids": sorted(str(value) for value in probe.capture_ids),
        "reference_scope": "host/process monotonic; no cross-host synchronization is asserted",
        "boundary": "Fit qualification does not establish the enclosed command's capture/device identity; strict derived-view binding and sample-range checks remain mandatory."})


if __name__ == "__main__":
    main()
