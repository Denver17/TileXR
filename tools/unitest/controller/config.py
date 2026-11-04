"""Resolve runtime options field by field: explicit CLI > config > defaults."""
import json
import math
import uuid
from pathlib import Path
from .selection import normalize

FIELDS = {"mode", "timing", "warmup", "iterations", "batches", "check_schedule", "profiling"}


def _count(name, value):
    if type(value) is not int or not (0 if name == "warmup" else 1) <= value <= 10000:
        raise ValueError(name + " must be an integer in " + ("0" if name == "warmup" else "1") + "..10000")


def _choice(name, value, choices):
    if not isinstance(value, str) or value not in choices:
        raise ValueError("unsupported " + name)


def resolve(args):
    config = {}
    if args.config is not None:
        path = Path(args.config)
        if path.stat().st_size > 65536:
            raise ValueError("run config exceeds 64 KiB")
        config = json.loads(path.read_text(encoding="utf-8"))
        if (not isinstance(config, dict) or config.get("schema") != "tilexr.unitest.run.v1"
                or config.keys() - FIELDS - {"schema"}):
            raise ValueError("invalid run config schema or unknown fields")
        config = {key: value for key, value in config.items() if key != "schema"}
    values, sources = {}, {}
    for key in FIELDS - {"profiling"}:
        if key in config:
            values[key], sources[key] = config[key], "config"
        cli = getattr(args, key, None)
        if cli is not None:
            values[key], sources[key] = cli, "cli"
    mode = values.get("mode", "single")
    _choice("mode", mode, ("single", "correctness", "perf"))
    for key in ("warmup", "iterations", "batches"):
        if key in values:
            _count(key, values[key])
    if "timing" in values:
        _choice("timing", values["timing"], ("host_batch",))
    if "check_schedule" in values:
        _choice("check_schedule", values["check_schedule"], ("powers_of_two", "every_iteration"))
    if mode != "perf" and any(key in values for key in ("warmup", "batches", "timing")):
        raise ValueError("warmup/batches/timing require perf")
    if mode != "correctness" and "check_schedule" in values:
        raise ValueError("check_schedule requires correctness")
    if mode == "single" and values.get("iterations", 1) != 1:
        raise ValueError("single executes exactly one iteration")
    defaults = {"mode": mode, "timing": "host_batch", "warmup": 5 if mode == "perf" else 0,
                "iterations": 1 if mode == "single" else 100,
                "batches": 3 if mode == "perf" else 1, "check_schedule": "powers_of_two"}
    resolved = {key: values.get(key, value) for key, value in defaults.items()}
    sources = {key: sources.get(key, "default") for key in defaults}
    profile = config.get("profiling", {})
    if (not isinstance(profile, dict) or profile.keys() - {"enabled", "tool", "options_file", "selection"}):
        raise ValueError("invalid profiling config")
    profile = dict(profile)
    profile_sources = {}
    for key, default in (("enabled", False), ("tool", "msprof"), ("options_file", None), ("selection", {"kind": "all"})):
        cli = getattr(args, "profiling" if key == "enabled" else "profiling_" + key, None)
        profile_sources[key] = "cli" if cli is not None else "config" if key in profile else "default"
        profile[key] = cli if cli is not None else profile.get(key, default)
    if type(profile["enabled"]) is not bool:
        raise ValueError("profiling.enabled must be boolean")
    _choice("profiling.tool", profile["tool"], ("msprof", "internal"))
    profile["selection"] = normalize(profile["selection"], mode, resolved["iterations"], resolved["batches"])
    if profile["enabled"] and profile["tool"] == "msprof" and profile["selection"]["kind"] != "all":
        raise ValueError("msprof supports only all actual rounds; selected rounds are not supported in this version")
    profile["options"] = {}
    options_file = profile["options_file"]
    if options_file is not None:
        if not isinstance(options_file, str) or not options_file:
            raise ValueError("profiling.options_file must be a path or null")
        path = Path(options_file)
        if not path.is_absolute() and profile_sources["options_file"] == "config":
            path = Path(args.config).resolve().parent / path
        path = path.resolve()
        if path.stat().st_size > 65536:
            raise ValueError("profiling options exceed 64 KiB")
        options = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(options, dict):
            raise ValueError("profiling options must be an object")
        profile["options"], profile["options_file"] = options, str(path)
    if profile["enabled"] and profile["tool"] == "msprof" and profile["options"]:
        raise ValueError("custom msprof collection options are unsupported by this ACLprof integration")
    if profile["enabled"] and profile["tool"] == "internal":
        options = profile["options"]
        frequency = options.get("cycles_per_us")
        if (options.keys() - {"cycles_per_us", "clock_basis", "max_workspace_bytes"} or
                type(frequency) not in (int, float) or not math.isfinite(frequency) or frequency <= 0 or
                not isinstance(options.get("clock_basis"), str) or not options["clock_basis"].strip()):
            raise ValueError("internal profiling requires confirmed cycles_per_us and clock_basis")
        budget = options.get("max_workspace_bytes", 64*1024*1024)
        if type(budget) is not int or not 1 <= budget <= 1024*1024*1024:
            raise ValueError("max_workspace_bytes must be in 1..1GiB per rank/run")
        options["max_workspace_bytes"] = budget
        profile["capture_seed"] = uuid.uuid4().hex
    resolved["profiling"], sources["profiling"] = profile, profile_sources
    return resolved, sources



def checkpoints(iterations, schedule):
    if schedule == "every_iteration":
        return list(range(1, iterations + 1))
    result, point = [], 1
    while point < iterations:
        result.append(point)
        point *= 2
    return result + [iterations]
