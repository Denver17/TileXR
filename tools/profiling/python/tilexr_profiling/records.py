"""Bounded parser for tilexr_profiling_records.v1 binary files."""

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


def _read_record_bytes(path: Path) -> bytes:
    source = path.as_posix()
    try:
        size = path.stat().st_size
        if size > MAX_RECORD_FILE_BYTES:
            raise ProfilingInputError(
                f"profiling records {source} exceeds maximum size of {MAX_RECORD_FILE_BYTES} bytes"
            )
        with path.open("rb") as stream:
            data = stream.read(MAX_RECORD_FILE_BYTES + 1)
    except OSError as exc:
        raise ProfilingInputError(f"cannot read records {source}: {exc}") from exc
    if len(data) > MAX_RECORD_FILE_BYTES:
        raise ProfilingInputError(
            f"profiling records {source} exceeds maximum size of {MAX_RECORD_FILE_BYTES} bytes"
        )
    return data


def parse_record_bytes(data: bytes, source: str = "<memory>") -> RecordFile:
    """Parse one complete records.v1 byte sequence."""
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
    if file_size != len(data):
        raise ProfilingInputError(
            f"records file size mismatch in {source}: header={file_size}, actual={len(data)}"
        )
    if stream_count > (len(data) - FILE_HEADER_SIZE) // STREAM_HEADER_SIZE:
        raise ProfilingInputError(f"stream count cannot fit in records file {source}")

    offset = FILE_HEADER_SIZE
    streams = []
    stream_ids = set()
    for block_index in range(stream_count):
        if len(data) - offset < STREAM_HEADER_SIZE:
            raise ProfilingInputError(
                f"records file {source} is truncated before stream block {block_index}"
            )
        stream_id, record_count, dropped_count, flags, block_size = STREAM_HEADER.unpack_from(
            data, offset
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
        if block_end > len(data):
            raise ProfilingInputError(f"stream {stream_id} is truncated in {source}")

        records = []
        record_offset = offset + STREAM_HEADER_SIZE
        previous_timestamp = None
        for record_index in range(record_count):
            timestamp, event_id, record_reserved = RECORD.unpack_from(data, record_offset)
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
            record_offset += RECORD_SIZE

        streams.append(
            RecordStream(
                id=stream_id,
                records=tuple(records),
                dropped_count=dropped_count,
                complete=bool(flags & STREAM_COMPLETE),
                overflow=bool(flags & STREAM_OVERFLOW),
                source=source,
            )
        )
        offset = block_end

    if offset != len(data):
        raise ProfilingInputError(f"trailing bytes after stream blocks in {source}")
    return RecordFile(UUID(bytes=capture_id_bytes), tuple(streams), source)


def load_record_files(paths: Sequence[Path]) -> Tuple[RecordFile, ...]:
    """Read and parse one or more bounded records.v1 files."""
    if not paths:
        raise ProfilingInputError("at least one profiling records file is required")
    if len(paths) > MAX_RECORD_FILES:
        raise ProfilingInputError(f"too many profiling record files: {len(paths)}")
    result = []
    for value in paths:
        path = Path(value)
        result.append(parse_record_bytes(_read_record_bytes(path), path.as_posix()))
    return tuple(result)
