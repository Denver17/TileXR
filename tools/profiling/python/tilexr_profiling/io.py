# Purpose: Read validated input and atomically publish bounded report output.
# 用途与关系：供解析器读取输入，并以原子方式发布有容量限制的报表。
"""Atomic output helpers for profiling artifacts."""

import json
import os
import tempfile
from pathlib import Path
from typing import Any

from .model import ProfilingInputError


def json_chunks(value, *, ensure_ascii=False):
    return json.JSONEncoder(ensure_ascii=ensure_ascii, allow_nan=False,
                            sort_keys=True).iterencode(value)


def atomic_write_text(path: Path, content: str) -> None:
    atomic_write_chunks(path, (content,))


def atomic_write_chunks(path: Path, chunks, *, max_bytes=None) -> None:
    """Write bounded UTF-8 chunks without retaining a second encoded artifact."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=str(path.parent),
            delete=False,
        ) as handle:
            temporary = Path(handle.name)
            total = 0
            for chunk in chunks:
                total += len(chunk.encode("utf-8"))
                if max_bytes is not None and total > max_bytes:
                    raise ProfilingInputError("profiling output byte budget exceeded")
                handle.write(chunk)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def atomic_write_json(path: Path, value: Any) -> None:
    encoded = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False, allow_nan=False) + "\n"
    atomic_write_text(path, encoded)
