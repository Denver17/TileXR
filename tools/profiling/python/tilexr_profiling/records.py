# Purpose: Decode bounded binary capture records for the pairing pipeline.
# 用途与关系：在预算内解析二进制 capture 记录，供配对流程使用。
"""Bounded parser for tilexr_profiling_records.v1 binary files."""

import io
import struct
from pathlib import Path
from typing import Sequence, Tuple
from uuid import UUID

from .model import (
    ProfilingInputError,
    RawRecord,
    RecordFile,
    RECORDS_MAGIC,
    RecordStream,
)


MAX_RECORD_FILE_BYTES = 256 * 1024 * 1024
MAX_RECORD_FILES = 4096

FILE_HEADER = struct.Struct("<8sHHHHHBBI16sQ")
STREAM_HEADER = struct.Struct("<IIIIQ")
RECORD = struct.Struct("<QII")

FILE_HEADER_SIZE = 48
STREAM_HEADER_SIZE = 24
RECORD_SIZE = 16
FORMAT_MAJOR = 1
FORMAT_MINOR = 0
LITTLE_ENDIAN = 1
STREAM_COMPLETE = 1 << 0
STREAM_OVERFLOW = 1 << 1
KNOWN_STREAM_FLAGS = STREAM_COMPLETE | STREAM_OVERFLOW


def parse_record_bytes(data: bytes, source: str = "<memory>", *, budget=None) -> RecordFile:
    """Parse the same bounded stream contract used for disk input."""
    return _parse_file(io.BytesIO(data), len(data), source, budget)


def _parse_file(handle, size, source, budget=None, on_stream=None):
    from .budgets import InputBudget
    budget = budget or InputBudget()
    if size > MAX_RECORD_FILE_BYTES:
        raise ProfilingInputError(f"profiling records {source} exceeds maximum size")
    budget.charge(files=1, bytes=size)
    data = handle.read(FILE_HEADER_SIZE)
    if len(data) < FILE_HEADER_SIZE:
        raise ProfilingInputError(f"records file {source} is truncated before the file header")
    (
        magic,
        major,
        minor,
        header_size,
        record_size,
        stream_header_size,
        endianness,
        reserved,
        stream_count,
        capture_id_bytes,
        file_size,
    ) = FILE_HEADER.unpack_from(data, 0)

    if magic != RECORDS_MAGIC:
        raise ProfilingInputError(f"invalid records magic in {source}")
    if (major, minor) != (FORMAT_MAJOR, FORMAT_MINOR):
        raise ProfilingInputError(f"unsupported records version {major}.{minor} in {source}")
    if header_size != FILE_HEADER_SIZE:
        raise ProfilingInputError(f"invalid file header size {header_size} in {source}")
    if record_size != RECORD_SIZE:
        raise ProfilingInputError(f"invalid record size {record_size} in {source}")
    if stream_header_size != STREAM_HEADER_SIZE:
        raise ProfilingInputError(f"invalid stream header size {stream_header_size} in {source}")
    if endianness != LITTLE_ENDIAN:
        raise ProfilingInputError(f"unsupported records endianness {endianness} in {source}")
    if reserved != 0:
        raise ProfilingInputError(f"nonzero file-header reserved field in {source}")
    if file_size != size:
        raise ProfilingInputError(
            f"records file size mismatch in {source}: header={file_size}, actual={size}"
        )
    if stream_count > (size - FILE_HEADER_SIZE) // STREAM_HEADER_SIZE:
        raise ProfilingInputError(f"stream count cannot fit in records file {source}")

    offset = FILE_HEADER_SIZE
    streams = []
    stream_ids = set()
    for block_index in range(stream_count):
        if size - offset < STREAM_HEADER_SIZE:
            raise ProfilingInputError(
                f"records file {source} is truncated before stream block {block_index}"
            )
        stream_id, record_count, dropped_count, flags, block_size = STREAM_HEADER.unpack(
            _read_exact(handle, STREAM_HEADER_SIZE, source)
        )
        if stream_id in stream_ids:
            raise ProfilingInputError(f"duplicate stream ID {stream_id} in records file {source}")
        stream_ids.add(stream_id)
        if flags & ~KNOWN_STREAM_FLAGS:
            raise ProfilingInputError(f"unknown stream flags 0x{flags:x} in {source}")
        expected_block_size = STREAM_HEADER_SIZE + record_count * RECORD_SIZE
        if block_size != expected_block_size:
            raise ProfilingInputError(
                f"invalid block size for stream {stream_id} in {source}: "
                f"header={block_size}, expected={expected_block_size}"
            )
        block_end = offset + block_size
        if block_end > size:
            raise ProfilingInputError(f"stream {stream_id} is truncated in {source}")

        budget.charge(streams=1, records=record_count,
                      memory_bytes=2048 + record_count * 768)
        records = []
        previous_timestamp = None
        for record_index in range(record_count):
            timestamp, event_id, record_reserved = RECORD.unpack(_read_exact(handle, RECORD_SIZE, source))
            if event_id == 0:
                raise ProfilingInputError(
                    f"stream {stream_id} record {record_index} has reserved event ID 0 in {source}"
                )
            if record_reserved != 0:
                raise ProfilingInputError(
                    f"stream {stream_id} record {record_index} has nonzero reserved data in {source}"
                )
            if previous_timestamp is not None and timestamp < previous_timestamp:
                raise ProfilingInputError(
                    f"stream {stream_id} timestamps are not non-decreasing in {source}"
                )
            previous_timestamp = timestamp
            records.append(RawRecord(timestamp, event_id, source, record_index))

        parsed_stream = RecordStream(
            id=stream_id,
            records=tuple(records),
            dropped_count=dropped_count,
            complete=bool(flags & STREAM_COMPLETE),
            overflow=bool(flags & STREAM_OVERFLOW),
            source=source,
        )
        if on_stream is None:
            streams.append(parsed_stream)
        else:
            on_stream(UUID(bytes=capture_id_bytes), parsed_stream)
        offset = block_end

    if offset != size or handle.read(1):
        raise ProfilingInputError(f"trailing bytes after stream blocks in {source}")
    return RecordFile(UUID(bytes=capture_id_bytes), tuple(streams), source)


def _read_exact(handle, count, source):
    data = handle.read(count)
    if len(data) != count:
        raise ProfilingInputError(f"records file {source} is truncated")
    return data


def load_record_files(paths: Sequence[Path], *, budget=None) -> Tuple[RecordFile, ...]:
    """Read incrementally, without retaining encoded file copies."""
    return _load_paths(paths, budget=budget)


def visit_record_files(paths, on_stream, *, budget=None):
    """Validate and consume each stream before decoding the next one.

    Returned file headers retain provenance only. The callback uses the same
    strict decoder as load_record_files; exceptions abort the whole input set.
    """
    return _load_paths(paths, budget=budget, on_stream=on_stream)


def _load_paths(paths, *, budget=None, on_stream=None):
    from .budgets import InputBudget
    budget = budget or InputBudget()
    if not paths:
        raise ProfilingInputError("at least one profiling records file is required")
    if len(paths) > min(MAX_RECORD_FILES, budget.limits.max_files):
        raise ProfilingInputError(f"too many profiling record files: {len(paths)}")
    result = []
    for value in paths:
        path = Path(value)
        try:
            with path.open("rb") as handle:
                import os
                size = os.fstat(handle.fileno()).st_size
                result.append(_parse_file(handle, size, path.as_posix(), budget, on_stream))
        except OSError as exc:
            raise ProfilingInputError(f"cannot read records {path}: {exc}") from exc
    return tuple(result)
