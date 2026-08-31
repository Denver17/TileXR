import json
import struct
from pathlib import Path
from uuid import UUID


CAPTURE_ID = "018f6f5e-41a0-7d31-9ad1-82d61fc0a001"
FILE_HEADER = struct.Struct("<8sHHHHHBBI16sQ")
STREAM_HEADER = struct.Struct("<IIIIQ")
RECORD = struct.Struct("<QII")


def stream_id(rank, core=0):
    return (rank << 16) | core


def mapping_document(cycles_per_us=1000.0):
    return {
        "schema": "tilexr_profiling_mapping.v1",
        "operator": "sample_operation",
        "cycles_per_us": cycles_per_us,
        "stages": [
            {
                "name": "kernel_total",
                "category": "total",
                "critical": True,
                "begin": 1,
                "end": 6,
            },
            {
                "name": "peer_wait",
                "category": "wait",
                "parent": "kernel_total",
                "begin": 2,
                "end": 3,
            },
            {
                "name": "payload_transfer",
                "category": "copy",
                "parent": "kernel_total",
                "begin": 4,
                "end": 5,
            },
            {
                "name": "checkpoint",
                "category": "diagnostic",
                "instant": 7,
            },
        ],
    }


def stream_records(rank=0):
    if rank == 0:
        return [
            (1000, 1),
            (1100, 2),
            (1500, 3),
            (1600, 4),
            (2200, 5),
            (3000, 6),
            (3050, 7),
        ]
    return [
        (1500, 1),
        (2000, 2),
        (4000, 3),
        (4100, 4),
        (5000, 5),
        (6500, 6),
        (6550, 7),
    ]


def overlapping_child_capture():
    mapping = mapping_document()
    mapping["stages"][2] = {
        "name": "payload_transfer",
        "category": "copy",
        "parent": "kernel_total",
        "instances": [
            {"begin": 4, "end": 5},
            {"begin": 8, "end": 9},
        ],
    }
    records = [
        (1000, 1),
        (1100, 4),
        (1200, 8),
        (1800, 5),
        (1900, 9),
        (2000, 6),
    ]
    return mapping, records


def record_block(
    stream_id_value,
    rank=0,
    records=None,
    dropped_count=0,
    complete=True,
    overflow=False,
):
    flags = (1 if complete else 0) | (2 if overflow else 0)
    return {
        "stream_id": stream_id_value,
        "records": list(stream_records(rank) if records is None else records),
        "dropped_count": dropped_count,
        "flags": flags,
    }


def records_bytes(blocks, capture_id=CAPTURE_ID):
    encoded_blocks = []
    for block in blocks:
        encoded_records = b"".join(
            RECORD.pack(timestamp, event_id, 0)
            for timestamp, event_id in block["records"]
        )
        block_size = STREAM_HEADER.size + len(encoded_records)
        encoded_blocks.append(
            STREAM_HEADER.pack(
                block["stream_id"],
                len(block["records"]),
                block.get("dropped_count", 0),
                block.get("flags", 1),
                block_size,
            )
            + encoded_records
        )
    payload = b"".join(encoded_blocks)
    file_size = FILE_HEADER.size + len(payload)
    header = FILE_HEADER.pack(
        b"TXRPRF1\0",
        1,
        0,
        48,
        16,
        24,
        1,
        0,
        len(blocks),
        UUID(capture_id).bytes,
        file_size,
    )
    return header + payload


def write_capture(root: Path, mapping=None, blocks=None, capture_id=CAPTURE_ID):
    if mapping is None:
        mapping = mapping_document()
    if blocks is None:
        blocks = [
            record_block(stream_id(0), 0),
            record_block(stream_id(1), 1),
        ]
    mapping_path = root / "capture.mapping.json"
    records_path = root / "capture.records.bin"
    mapping_path.write_text(json.dumps(mapping), encoding="utf-8")
    records_path.write_bytes(records_bytes(blocks, capture_id))
    return mapping_path, records_path
