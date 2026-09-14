"""Bounded parser for tilexr_profiling_records.v1 binary files."""

import struct
from io import BytesIO
from pathlib import Path
from typing import BinaryIO, Sequence, Tuple
from uuid import UUID

from .model import (
    ProfilingInputError,
    RawRecord,
    RecordFile,
    RECORDS_MAGIC,
    RecordStream,
)


# Match the common runtime's checked capacity: 64 cores with up to
# 67,108,856 records/core, including records.v1 stream and file headers.
MAX_RECORD_FILE_BYTES = 64 * 1024 * 1024 * 1024
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


def _parse_record_stream(handle: BinaryIO, size: int, source: str) -> RecordFile:
    if size > MAX_RECORD_FILE_BYTES:
        raise ProfilingInputError(
            f"profiling records {source} exceeds maximum size of {MAX_RECORD_FILE_BYTES} bytes"
        )
    header = handle.read(FILE_HEADER_SIZE)
    if len(header) < FILE_HEADER_SIZE:
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
    ) = FILE_HEADER.unpack(header)

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
        stream_header = handle.read(STREAM_HEADER_SIZE)
        if len(stream_header) < STREAM_HEADER_SIZE:
            raise ProfilingInputError(
                f"records file {source} is truncated before stream block {block_index}"
            )
        stream_id, record_count, dropped_count, flags, block_size = STREAM_HEADER.unpack(stream_header)
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

        records = []
        previous_timestamp = None
        # Large captures must not be duplicated as one giant bytes object in
        # addition to the normalized records. Bound transient IO to 64 KiB.
        for chunk_start in range(0, record_count, 4096):
            chunk_count = min(4096, record_count - chunk_start)
            chunk = handle.read(chunk_count * RECORD_SIZE)
            if len(chunk) != chunk_count * RECORD_SIZE:
                raise ProfilingInputError(f"stream {stream_id} is truncated in {source}")
            for chunk_index, (timestamp, event_id, record_reserved) in enumerate(RECORD.iter_unpack(chunk)):
                record_index = chunk_start + chunk_index
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

    if offset != size or handle.read(1):
        raise ProfilingInputError(f"trailing bytes after stream blocks in {source}")
    return RecordFile(UUID(bytes=capture_id_bytes), tuple(streams), source)


def parse_record_bytes(data: bytes, source: str = "<memory>") -> RecordFile:
    """Parse one complete records.v1 byte sequence."""
    return _parse_record_stream(BytesIO(data), len(data), source)


def load_record_files(paths: Sequence[Path]) -> Tuple[RecordFile, ...]:
    """Read and parse one or more bounded records.v1 files."""
    if not paths:
        raise ProfilingInputError("at least one profiling records file is required")
    if len(paths) > MAX_RECORD_FILES:
        raise ProfilingInputError(f"too many profiling record files: {len(paths)}")
    result = []
    for value in paths:
        path = Path(value)
        source = path.as_posix()
        try:
            size = path.stat().st_size
            with path.open("rb") as handle:
                result.append(_parse_record_stream(handle, size, source))
        except OSError as exc:
            raise ProfilingInputError(f"cannot read records {source}: {exc}") from exc
    return tuple(result)
