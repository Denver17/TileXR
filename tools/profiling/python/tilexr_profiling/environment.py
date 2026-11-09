# Purpose: Collect and reconcile declared and observed profiling environment metadata.
# 用途与关系：收集并核对 profiling 环境的声明信息与观测信息。
"""Read-only environment discovery with explicit source and unknown boundaries."""

import os
import platform
import socket
from pathlib import Path

from .manifest import _bounded
from .model import ProfilingInputError

ENVIRONMENT_SCHEMA = "tilexr_profiling_environment.v1"
MAX_METADATA_BYTES = 65536


def _fact_value(key, value):
    if key == "device":
        if (not isinstance(value, dict) or set(value) != {"id", "namespace"} or
                not isinstance(value["namespace"], str) or not value["namespace"] or
                isinstance(value["id"], bool) or not isinstance(value["id"], (str, int)) or
                isinstance(value["id"], int) and not 0 <= value["id"] <= (1 << 32) - 1 or
                isinstance(value["id"], str) and not value["id"]):
            raise ProfilingInputError("device requires a nonempty namespace and nonnegative id")
    elif not isinstance(value, str) or not value:
        raise ProfilingInputError(f"{key} must be a nonempty string")


def _version_files(root):
    if root is None:
        return []
    base = Path(root)
    return [base / "version.cfg", base / "version.info", base / "ascend_toolkit_install.info",
            base / "compiler" / "version.info", base / "runtime" / "version.info"]


def collect_environment(toolkit_root=None, *, runtime_probe=None):
    """Read host and installed-toolkit facts, optionally calling an explicit probe.

    No device command, initialization, environment modification or cross-device
    calibration occurs. runtime_probe is supplied by a caller owning its ACL
    context, and may report soc, driver_version, firmware_version and device.
    """
    root = toolkit_root or os.environ.get("ASCEND_HOME_PATH")
    facts = {
        "host": {"value": socket.gethostname(), "source": "socket.gethostname", "status": "known"},
        "os": {"value": platform.platform(), "source": "platform.platform", "status": "known"},
        "architecture": {"value": platform.machine(), "source": "platform.machine", "status": "known"},
    }
    metadata = []
    for path in _version_files(root):
        if not path.is_file():
            continue
        try:
            with path.open("rb") as handle:
                raw = handle.read(MAX_METADATA_BYTES + 1)
            if len(raw) > MAX_METADATA_BYTES:
                raise ProfilingInputError(f"toolkit metadata exceeds budget: {path}")
            text = raw.decode("utf-8")
        except (OSError, UnicodeError) as exc:
            metadata.append({"path": str(path), "status": "unavailable", "reason": str(exc)})
            continue
        metadata.append({"path": str(path.resolve()), "status": "read", "text": text})
        for line in text.splitlines():
            key, separator, value = line.partition("=")
            if separator and key.strip().lower() in {"version", "cann_version"} and value.strip():
                claim = {"value": value.strip(), "source": str(path.resolve()), "status": "reported"}
                if "cann_version" in facts and facts["cann_version"]["value"] != claim["value"]:
                    facts["cann_version"] = {"value": None, "status": "conflict",
                                             "source": "installed metadata", "reason": "version files disagree"}
                elif facts.get("cann_version", {}).get("status") != "conflict":
                    facts["cann_version"] = claim
    for key in ("soc", "driver_version", "firmware_version", "device", "cann_version"):
        facts.setdefault(key, {"value": None, "status": "unknown", "source": "not observed"})
    errors = []
    if runtime_probe is not None:
        try:
            observed = _bounded(runtime_probe())
            if not isinstance(observed, dict) or set(observed) - {"soc", "driver_version", "firmware_version", "device"}:
                raise ProfilingInputError("runtime probe returned unsupported facts")
            for key, value in observed.items():
                if value is None:
                    continue
                _fact_value(key, value)
            for key, value in observed.items():
                if value is None:
                    continue
                facts[key] = {"value": value, "source": "caller runtime probe", "status": "reported"}
        except Exception as exc:
            errors.append({"code": "runtime_probe_unavailable", "message": str(exc)})
    return {"schema": ENVIRONMENT_SCHEMA, "facts": facts,
            "toolkit_root": str(Path(root).resolve()) if root else None,
            "metadata": metadata, "diagnostics": errors,
            "clock": {"status": "unknown", "cycles_per_us": None,
                      "reason": "Installed CANN/SoC names do not establish the device timer frequency or cross-device calibration."}}


def reconcile_environment(discovered, declared):
    """Merge manual declarations only when they do not contradict observations."""
    result = _bounded(discovered)
    declarations = _bounded(declared)
    if not isinstance(declarations, dict) or result.get("schema") != ENVIRONMENT_SCHEMA:
        raise ProfilingInputError("invalid environment document")
    for key, value in declarations.items():
        if key not in result["facts"]:
            raise ProfilingInputError(f"unknown environment field {key}")
        fact = result["facts"][key]
        if value is not None:
            _fact_value(key, value)
        if fact["status"] == "conflict" or (fact["value"] is not None and fact["value"] != value):
            raise ProfilingInputError(f"environment declaration contradicts observed {key}")
        if fact["value"] is None and value is not None:
            result["facts"][key] = {"value": value, "source": "explicit caller declaration", "status": "reported"}
    return result
