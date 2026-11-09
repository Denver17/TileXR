# Purpose: Qualify clock conversions and ordering using declared calibration evidence.
# 用途与关系：根据声明的 calibration 证据判定时钟换算与事件顺序是否成立。
"""Evidence-bound device-cycle calibration, separate from local timer conversion."""

import datetime as dt
import math
import os
import socket
import statistics
import time
from uuid import UUID

from .manifest import _document, _uint, UINT64_MAX
from .mapping import _array, _known_keys, _required_keys, _string
from .model import ProfilingInputError

CALIBRATION_SCHEMA = "tilexr_profiling_calibration.v1"


class CalibrationSession:
    """Acquire paired observations using a caller-owned device timer probe.

    The reader must return GetSystemCycle from the declared device and complete
    inside the reference-clock bracket. It must use the same timer as records,
    not an ACL event-duration counter. No ACL context or stream is created here.
    Sample before and after the measured operation; conversion never extrapolates.
    """

    def __init__(self, read_cycles, *, capture_id, device, clock_domain, source,
                 max_error_ns, max_drift_ppm, validity_seconds=60,
                 reference_clock=None, reference_clock_id=None):
        if not callable(read_cycles):
            raise ProfilingInputError("calibration requires a callable device timer probe")
        if reference_clock is not None and (not callable(reference_clock) or not reference_clock_id):
            raise ProfilingInputError("a custom reference clock requires a qualified clock ID")
        _positive(max_error_ns, "max_error_ns")
        _positive(max_drift_ppm, "max_drift_ppm")
        _positive(validity_seconds, "validity_seconds")
        if validity_seconds > 3600:
            raise ProfilingInputError("calibration validity must be at most one hour")
        try:
            capture_id = str(UUID(capture_id))
        except (ValueError, TypeError, AttributeError) as exc:
            raise ProfilingInputError("calibration capture_id must be a UUID") from exc
        _device(device)
        _string(clock_domain, "clock_domain")
        _string(source, "source")
        self._read_cycles = read_cycles
        self._reference_clock = reference_clock or time.monotonic_ns
        self._validity = validity_seconds
        self._samples = []
        self._document = {
            "schema": CALIBRATION_SCHEMA, "capture_id": capture_id, "device": dict(device),
            "clock_domain": clock_domain, "source": source,
            "reference_clock_id": reference_clock_id or f"monotonic:{socket.gethostname()}:{os.getpid()}",
            "method": "bounded_host_device_pairs", "max_error_ns": max_error_ns,
            "max_drift_ppm": max_drift_ppm,
        }

    def sample(self):
        """Read one bracketed device cycle; a failed probe contributes no sample."""
        if len(self._samples) >= 4096:
            raise ProfilingInputError("calibration sample budget exceeded")
        before = _uint(self._reference_clock(), "before_ns", UINT64_MAX)
        cycles = _uint(self._read_cycles(), "cycles", UINT64_MAX)
        after = _uint(self._reference_clock(), "after_ns", UINT64_MAX)
        if after < before or self._samples and before < self._samples[-1]["after_ns"]:
            raise ProfilingInputError("calibration reference clock moved backwards")
        sample = {"cycles": cycles, "before_ns": before, "after_ns": after}
        self._samples.append(sample)
        return dict(sample)

    def finish(self, *, now=None):
        """Return a validated sidecar; never publish an unqualified clock fit."""
        generated = dt.datetime.now(dt.timezone.utc) if now is None else _time(now, "now")
        document = dict(self._document,
                        generated_at=generated.isoformat(),
                        valid_until=(generated + dt.timedelta(seconds=self._validity)).isoformat(),
                        samples=[dict(sample) for sample in self._samples])
        load_calibration(document, now=generated.isoformat())
        return document


def _time(value, where):
    _string(value, where)
    try:
        result = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ProfilingInputError(f"invalid {where}") from exc
    if result.tzinfo is None:
        raise ProfilingInputError(f"{where} requires a timezone")
    return result.astimezone(dt.timezone.utc)


def _positive(value, where):
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or value <= 0:
        raise ProfilingInputError(f"{where} must be positive and finite")
    return float(value)


def _device(value):
    if not isinstance(value, dict) or set(value) != {"host", "id", "namespace"}:
        raise ProfilingInputError("calibration device requires host, id and namespace")
    for key in ("host", "namespace"):
        _string(value[key], "device." + key)
    if isinstance(value["id"], bool) or not isinstance(value["id"], (int, str)):
        raise ProfilingInputError("invalid calibration device ID")
    if isinstance(value["id"], int):
        _uint(value["id"], "device.id")
    else:
        _string(value["id"], "device.id")
    return value


def _fit(samples, maximum_error):
    values = _array(samples, "calibration samples")
    if not 3 <= len(values) <= 4096:
        raise ProfilingInputError("calibration requires 3..4096 bounded samples")
    parsed = []
    for sample in values:
        if not isinstance(sample, dict) or set(sample) != {"cycles", "before_ns", "after_ns"}:
            raise ProfilingInputError("calibration sample requires cycles, before_ns and after_ns")
        cycle, before, after = (_uint(sample[key], key, UINT64_MAX)
                                for key in ("cycles", "before_ns", "after_ns"))
        if after < before:
            raise ProfilingInputError("calibration sample has reversed reference interval")
        parsed.append((cycle, before, after))
    parsed.sort()
    if len({cycle for cycle, _, _ in parsed}) != len(parsed):
        raise ProfilingInputError("duplicate calibration device cycle")
    if any(right[1] < left[2] for left, right in zip(parsed, parsed[1:])):
        raise ProfilingInputError("calibration reference brackets overlap or go backwards")
    # Subtract integer anchors before float conversion so uint64 epochs retain
    # cycle differences rather than losing precision in raw timestamp doubles.
    anchor_cycle = parsed[0][0]
    anchor_ns = parsed[0][1]
    x = [cycle - anchor_cycle for cycle, _, _ in parsed]
    y = [(before - anchor_ns) + (after - before) / 2 for _, before, after in parsed]
    x_mean, y_mean = statistics.mean(x), statistics.mean(y)
    covariance = sum((a - x_mean) * (b - y_mean) for a, b in zip(x, y))
    variance = sum((a - x_mean) ** 2 for a in x)
    slope = covariance / variance
    if not math.isfinite(slope) or slope <= 0:
        raise ProfilingInputError("calibration clock slope must be positive")
    offset = y_mean - slope * x_mean
    residual = max(abs(slope * a + offset - b) for a, b in zip(x, y))
    brackets = max((after - before) / 2 for _, before, after in parsed)
    error = residual + brackets
    if error > maximum_error:
        raise ProfilingInputError("calibration evidence exceeds maximum error")
    local_slopes = [((r[1] - l[1]) + (r[2] - l[2])) / 2 / (r[0] - l[0])
                    for l, r in zip(parsed, parsed[1:])]
    drift = max(abs(local / slope - 1) * 1e6 for local in local_slopes)
    return {"anchor_cycle": anchor_cycle, "anchor_reference_ns": anchor_ns,
            "offset_ns": offset, "ns_per_cycle": slope,
            "cycles_per_us": 1000 / slope, "error_bound_ns": error,
            "observed_drift_ppm": drift, "minimum_cycle": parsed[0][0],
            "maximum_cycle": parsed[-1][0]}


def load_calibration(value, *, now=None):
    """Validate and recompute the fit from independent bounded probe evidence.

    A sidecar does not authenticate its producer. It records evidence and rejects
    expired, imprecise, unbound or extrapolated global-coordinate claims.
    """
    result = _document(value)
    required = {"schema", "capture_id", "device", "clock_domain", "reference_clock_id",
                "method", "source", "generated_at", "valid_until", "max_error_ns",
                "max_drift_ppm", "samples"}
    _required_keys(result, required, "calibration")
    _known_keys(result, required, "calibration")
    if result["schema"] != CALIBRATION_SCHEMA or result["method"] != "bounded_host_device_pairs":
        raise ProfilingInputError("unsupported calibration schema or method")
    try:
        result["capture_id"] = str(UUID(result["capture_id"]))
    except (ValueError, TypeError, AttributeError) as exc:
        raise ProfilingInputError("calibration capture_id must be a UUID") from exc
    _device(result["device"])
    for key in ("clock_domain", "reference_clock_id", "source"):
        _string(result[key], key)
    generated = _time(result["generated_at"], "generated_at")
    expiry = _time(result["valid_until"], "valid_until")
    current = dt.datetime.now(dt.timezone.utc) if now is None else _time(now, "now")
    if expiry <= generated or expiry - generated > dt.timedelta(hours=1):
        raise ProfilingInputError("calibration validity must be positive and at most one hour")
    if not generated <= current <= expiry:
        raise ProfilingInputError("calibration is expired or not yet valid")
    maximum_error = _positive(result["max_error_ns"], "max_error_ns")
    maximum_drift = _positive(result["max_drift_ppm"], "max_drift_ppm")
    fit = _fit(result["samples"], maximum_error)
    if fit["observed_drift_ppm"] > maximum_drift:
        raise ProfilingInputError("calibration observed drift exceeds bound")
    return dict(result, fit=fit)


def convert_timestamp(calibration, raw_cycles, *, capture_id, device, clock_domain,
                      now=None, max_error_ns=None):
    """Derive a reference coordinate inside the measured range; raw data stays intact."""
    # Revalidation prevents an old previously loaded object bypassing expiry.
    document = dict(calibration)
    document.pop("fit", None)
    evidence = load_calibration(document, now=now)
    if (evidence["capture_id"] != capture_id or evidence["device"] != device or
            evidence["clock_domain"] != clock_domain):
        raise ProfilingInputError("calibration measurement/device/clock binding mismatch")
    return _coordinate(evidence, raw_cycles, max_error_ns)


def _coordinate(evidence, raw_cycles, max_error_ns):
    cycles = _uint(raw_cycles, "raw_cycles", UINT64_MAX)
    fit = evidence["fit"]
    if not fit["minimum_cycle"] <= cycles <= fit["maximum_cycle"]:
        raise ProfilingInputError("calibration does not allow extrapolation")
    if max_error_ns is not None and fit["error_bound_ns"] > _positive(max_error_ns, "max_error_ns"):
        raise ProfilingInputError("calibration exceeds requested precision")
    # Return an integer anchor and a small offset, preserving large epoch values.
    offset = fit["offset_ns"] + (cycles - fit["anchor_cycle"]) * fit["ns_per_cycle"]
    return {"reference_ns": fit["anchor_reference_ns"] + round(offset),
            "error_bound_ns": fit["error_bound_ns"],
            "reference_clock_id": evidence["reference_clock_id"],
            "raw_cycles": cycles, "source": evidence["source"]}


def compare_global_order(left, right):
    """State order only when reference domains agree and uncertainty ranges separate."""
    if left["reference_clock_id"] != right["reference_clock_id"]:
        return "incomparable_clock_domains"
    difference = right["reference_ns"] - left["reference_ns"]
    uncertainty = left["error_bound_ns"] + right["error_bound_ns"]
    if difference > uncertainty:
        return "before"
    if difference < -uncertainty:
        return "after"
    return "indeterminate_within_error"


def derive_global_coordinates(run, calibrations, *, now=None, max_error_ns=None):
    """Produce a separate derived view for a fully covered, explicitly bound run.

    All lanes must have manifest identity and one calibration for their physical
    device. Local timestamps and the original report run are never rewritten.
    """
    if not run.manifest_metadata:
        raise ProfilingInputError("global coordinates require manifest device identities")
    if not isinstance(calibrations, (list, tuple)) or not calibrations or len(calibrations) > 65536:
        raise ProfilingInputError("calibrations require a bounded nonempty sequence")
    by_device = {}
    reference = None
    for value in calibrations:
        document = dict(value) if isinstance(value, dict) else value
        if isinstance(document, dict):
            document.pop("fit", None)
        evidence = load_calibration(document, now=now)
        if evidence["capture_id"] != run.capture_id or evidence["clock_domain"] != run.clock_domain:
            raise ProfilingInputError("calibration run binding mismatch")
        device = evidence["device"]
        key = (device["host"], str(device["id"]), device["namespace"])
        if key in by_device:
            raise ProfilingInputError("duplicate calibration device")
        by_device[key] = evidence
        if reference is not None and reference != evidence["reference_clock_id"]:
            raise ProfilingInputError("calibrations do not share a qualified reference clock")
        reference = evidence["reference_clock_id"]
    participants = {item["stream_id"]: item for item in run.manifest_metadata["participants"]}
    result = []
    used = set()
    for launch in run.launches:
        for lane in launch.lanes:
            if not lane.attributes.get("present", True):
                continue
            participant = participants.get(lane.stream_id)
            if not participant or not participant.get("host") or not participant.get("device"):
                raise ProfilingInputError("lane lacks manifest host/device identity")
            declared = participant["device"]
            key = (participant["host"], str(declared["id"]), declared["namespace"])
            evidence = by_device.get(key)
            if evidence is None:
                raise ProfilingInputError("lane has no matching calibration")
            used.add(key)
            fit = evidence["fit"]
            if run.cycles_per_us is None or abs(fit["cycles_per_us"] / run.cycles_per_us - 1) * 1e6 > evidence["max_drift_ppm"]:
                raise ProfilingInputError("calibration contradicts mapping timer conversion")
            device = evidence["device"]
            records = []
            for record in lane.records:
                converted = _coordinate(evidence, record.timestamp, max_error_ns)
                records.append(dict(converted, event_id=record.event_id,
                                    source=record.source, source_index=record.source_index))
            result.append({"launch_id": launch.id, "stream_id": lane.stream_id,
                           "rank": lane.rank, "logical_lane": lane.core,
                           "device": device, "records": records})
    if used != set(by_device):
        raise ProfilingInputError("calibration contains a device unused by this capture")
    return {"schema": "tilexr_profiling_global_coordinates.v1", "capture_id": run.capture_id,
            "reference_clock_id": reference, "lanes": result,
            "boundary": "Derived coordinates only; relative order is indeterminate when uncertainty intervals overlap."}
