"""Load bounded case JSON and validate the supported workload before launch.

Resolve rank count from selected devices and reject unsupported shapes or modes
before any Worker or NPU initialization.
"""
import json
from pathlib import Path


def load_case(path, devices):
    path = Path(path)
    if path.stat().st_size > 65536:
        raise ValueError("case exceeds 64 KiB")
    value = json.loads(path.read_text(encoding="utf-8"))
    required = {"schema", "id", "backend", "dtype", "quant_mode",
                "bs", "h", "topk", "moe_expert_num", "seed"}
    if not isinstance(value, dict) or not required <= value.keys():
        raise ValueError("case must be an object containing " + ", ".join(sorted(required)))
    unknown = value.keys() - required - {"world_size", "operator", "steps", "validate"}
    if unknown:
        raise ValueError("unknown case fields: " + ", ".join(sorted(unknown)))
    for name, choices in {
        "schema": ("tilexr.unitest.case.v1",),
        "backend": ("memory",), "dtype": ("bf16",),
    }.items():
        if value[name] not in choices:
            raise ValueError("unsupported {}: {}".format(name, value[name]))
    if not isinstance(value["id"], str) or not 1 <= len(value["id"]) <= 128:
        raise ValueError("case id must contain 1..128 characters")
    for name, low, high in (("bs", 1, 256), ("h", 16, 4096), ("topk", 1, 8),
                            ("moe_expert_num", 1, 128), ("seed", 0, 2**32-1),
                            ("quant_mode", 0, 0)):
        if type(value[name]) is not int or not low <= value[name] <= high:
            raise ValueError("{} must be an integer in {}..{}".format(name, low, high))
    steps = value.get("steps")
    legacy = value.get("operator")
    if steps is None:
        if legacy not in ("ep_dispatch", "ep_combine"):
            raise ValueError("provide registered steps or a standalone operator")
        steps = [{"id": "dispatch" if legacy == "ep_dispatch" else "combine", "operator": legacy}]
    if not isinstance(steps, list) or not 1 <= len(steps) <= 2:
        raise ValueError("steps must contain one or two registered operations")
    ids, operators = set(), []
    for step in steps:
        if not isinstance(step, dict) or set(step) - {"id", "operator", "inputs"}:
            raise ValueError("invalid step fields")
        ident = step.get("id")
        if (not isinstance(ident, str) or not ident or len(ident) > 64 or
                not all(c.isascii() and (c.isalnum() or c == "_") for c in ident) or ident in ids):
            raise ValueError("step id must be a unique ASCII identifier")
        ids.add(ident)
        if not isinstance(step.get("operator"), str):
            raise ValueError("step operator must be a registered name")
        operators.append(step["operator"])
    plans = {("ep_dispatch",): "ep_dispatch", ("ep_combine",): "ep_combine",
             ("ep_dispatch", "ep_combine"): "ep_dispatch_combine",
             ("ep_dispatch", "barrier"): "ep_dispatch_barrier"}
    if tuple(operators) not in plans:
        raise ValueError("unsupported registered step sequence")
    if "inputs" in steps[0] and steps[0]["inputs"] != {}:
        raise ValueError("first step inputs come from Host stimulus")
    if len(steps) == 2:
        expected = ({name: steps[0]["id"] + "." + name for name in ("payload", "assist", "counts")}
                    if operators[1] == "ep_combine" else {})
        if steps[1].get("inputs", {}) != expected:
            raise ValueError("step inputs must bind preceding Dispatch device payload/assist/counts")
    derived = plans[tuple(operators)]
    if legacy is not None and legacy != derived:
        raise ValueError("operator disagrees with steps")
    output = steps[-1 if operators[-1] != "barrier" else 0]["id"] + ".output"
    if value.get("validate", output) != output:
        raise ValueError("validate must select the final data-producing step.output")
    value.update(operator=derived, steps=steps, validate=output)
    world = len(devices)
    if not 2 <= world <= 128:
        raise ValueError("EP communicator supports 2..128 global ranks")
    if "world_size" in value and (type(value["world_size"]) is not int or value["world_size"] != world):
        raise ValueError("case world_size must match selected devices")
    value["world_size"] = world
    experts = value["moe_expert_num"]
    if value["h"] % 16 or experts % world or experts // world > 32 or value["topk"] > experts:
        raise ValueError("H must divide by 16; experts must divide across ranks (<=32 each); TopK <= experts")
    if value["operator"] in ("ep_combine", "ep_dispatch_combine") and value["bs"] * value["topk"] < experts:
        raise ValueError("standalone Combine requires BS*TopK >= experts for full peer coverage")
    return value
