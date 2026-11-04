"""Bound the installed NodeAgent JSON-line protocol and bind each reply to a run.

A broken channel fails the complete run; it never silently retries an operation.
"""
import json
import math
import re

VERSION = "tilexr.unitest.rpc.v1"
MAX_MESSAGE = 32 * 1024 * 1024
RUN_ID = re.compile(r"^[a-f0-9]{32}$")
ACTIONS = {"start", "collect", "phase", "wait", "tools", "close"}


def duration(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 < value <= 86400:
        raise ValueError("RPC timeout must be finite and in (0,86400]")
    return value


def encode(message):
    data = json.dumps(message, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"
    if len(data) > MAX_MESSAGE:
        raise ValueError("NodeAgent message exceeds 32 MiB")
    return data


def decode(line):
    if len(line) > MAX_MESSAGE or not line.endswith(b"\n"):
        raise ValueError("oversize or incomplete NodeAgent message")
    message = json.loads(line)
    if not isinstance(message, dict) or message.get("schema") != VERSION:
        raise ValueError("invalid NodeAgent protocol version")
    if not isinstance(message.get("run_id"), str) or not RUN_ID.fullmatch(message["run_id"]):
        raise ValueError("invalid NodeAgent run ID")
    if type(message.get("sequence")) is not int or message["sequence"] < 1 or message.get("action") not in ACTIONS:
        raise ValueError("invalid NodeAgent sequence or action")
    return message


def envelope(run_id, sequence, action, **fields):
    return dict(schema=VERSION, run_id=run_id, sequence=sequence, action=action, **fields)
