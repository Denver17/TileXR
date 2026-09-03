#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import argparse
import re
import sys
from pathlib import Path


def parse_devices(text):
    devices = set()
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        devices.add(int(item, 0))
    return devices


def first_int(text):
    match = re.search(r"\d+", text)
    return int(match.group(0), 10) if match else None


def parse_busy_processes(log_text, selected_devices):
    busy = []
    in_process_table = False
    for line in log_text.splitlines():
        if "|" not in line:
            continue
        lower_line = line.lower()
        if "process id" in lower_line or "process name" in lower_line:
            in_process_table = True
            continue
        if not in_process_table:
            continue
        fields = [field.strip() for field in line.strip().strip("|").split("|")]
        if len(fields) < 3:
            continue
        device = first_int(fields[0])
        if device is None or device not in selected_devices:
            continue

        pid = first_int(fields[1])
        process_index = 2
        if (pid is None or pid == 0) and len(fields) >= 4:
            pid = first_int(fields[2])
            process_index = 3
        if pid is None or pid == 0:
            continue
        process = fields[process_index].split()[0] if fields[process_index].split() else "unknown"
        if process in {"-", "N/A", "NA"}:
            continue
        busy.append((device, pid, process))
    return busy


def parse_unhealthy_devices(log_text, selected_devices):
    unhealthy = []
    observed_selected = set()
    in_status_table = False
    for line in log_text.splitlines():
        if "|" not in line:
            continue
        lower_line = line.lower()
        if "process id" in lower_line or "process name" in lower_line:
            in_status_table = False
            continue
        if "health" in lower_line:
            in_status_table = True
            continue
        if not in_status_table:
            continue

        fields = [field.strip() for field in line.strip().strip("|").split("|")]
        if len(fields) < 3:
            continue
        device = first_int(fields[0])
        if device is None or device not in selected_devices:
            continue
        health = fields[2].split()[0] if fields[2].split() else ""
        if not health:
            continue
        observed_selected.add(device)
        if health.upper() != "OK":
            unhealthy.append((device, health))
    return unhealthy, observed_selected


def main():
    parser = argparse.ArgumentParser(description="Reject TileXR CCU smoke runs on busy or unhealthy selected NPUs.")
    parser.add_argument("--log", required=True, help="npu-smi info log path")
    parser.add_argument("--devices", required=True, help="comma-separated NPU device ids selected for the smoke")
    parser.add_argument(
        "--allow-unhealthy",
        action="store_true",
        help="Allow selected devices with non-OK health while still rejecting busy processes.",
    )
    args = parser.parse_args()

    try:
        selected_devices = parse_devices(args.devices)
    except ValueError as exc:
        print(f"invalid device list {args.devices!r}: {exc}", file=sys.stderr)
        return 2
    if not selected_devices:
        print("no selected NPU devices to check", file=sys.stderr)
        return 2

    log_path = Path(args.log)
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    unhealthy, observed_health = parse_unhealthy_devices(log_text, selected_devices)
    if unhealthy:
        if not args.allow_unhealthy:
            for device, health in unhealthy:
                print(f"unhealthy selected NPU device={device} health={health}")
            return 1
        for device, health in unhealthy:
            print(f"unhealthy selected NPU devices allowed device={device} health={health}")
    if observed_health and not unhealthy:
        print(f"selected NPU devices healthy devices={args.devices}")

    busy = parse_busy_processes(log_text, selected_devices)
    if busy:
        for device, pid, process in busy:
            print(f"busy selected NPU process device={device} pid={pid} process={process}")
        return 1

    print(f"no selected NPU processes devices={args.devices}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
