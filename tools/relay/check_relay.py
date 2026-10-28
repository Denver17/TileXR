"""Validate benchmark baseline/TileRelay rounds and actual main launch counts."""

from decimal import Decimal

MODES = ("serial", "serial_tiled", "overlap", "generic_relay")


def check_launches(events, rows, warmup, iterations, modes=MODES):
    marks = [e for e in events if e.get("ph") == "X" and "/round" in e.get("name", "")
             and e["name"].split("/")[0] in modes]
    expected_marks = {f"{mode}/round{i}" for mode in modes for i in range(warmup + iterations)}
    if len(marks) != len(expected_marks) or {e["name"] for e in marks} != expected_marks:
        raise ValueError("missing or duplicated round trace")
    # Framework preparation (e.g. fill_) can land a few microseconds inside a
    # CPU marker after profiler clock calibration. It is not a pipeline launch;
    # retain unexpected non-framework kernels so type/count checks reject them.
    hardware = [(e, Decimal(str(e["ts"]))) for e in events if e.get("ph") == "X" and
                not e.get("name", "").startswith("aclnn") and
                e.get("args", {}).get("Task Type") in ("KERNEL_MIX_AIC", "KERNEL_AICORE", "KERNEL_AIVEC")]
    counts = {}
    for mark in marks:
        start = Decimal(str(mark["ts"])); end = start + Decimal(str(mark["dur"]))
        launches = [e for e, ts in hardware if start <= ts < end]
        mode = mark["name"].split("/")[0]
        is_relay = mode in ("generic_relay", "generic_execution")
        expected_count = 1 if is_relay else (2 if mode == "serial" else 2 * rows // 128)
        if len(launches) != expected_count:
            raise ValueError(f"{mark['name']}: expected {expected_count} launches, got {len(launches)}")
        names = [e.get("name") for e in launches]
        if is_relay and names != ["main_kernel"]:
            raise ValueError("unexpected relay kernel")
        if not is_relay:
            tiles = expected_count // 2
            if names.count("TileXRAllGather_float16_t") != tiles or names.count("main_kernel") != tiles:
                raise ValueError("wrong baseline kernel types")
        counts[mode] = expected_count
    return counts
