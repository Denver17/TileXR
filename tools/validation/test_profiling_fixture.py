"""Small records.v1 fixtures for validation tests; never imported by production."""
import struct
from pathlib import Path
from uuid import UUID

CAPTURE_ID = "d63acd67-e125-46c6-bd57-69ca989951dd"


def write_capture(path, rank=0, launches=5, warmup=2, duration=8000, fault="", capture_id=CAPTURE_ID):
    blocks = []
    for core in range(64):
        records = []
        for launch in range(launches - (fault == "stale")):
            begin = 100000000000000000 + launch * 1000000 + core
            span = duration * 10 if launch < warmup else duration
            events = [(0, 0x53000001), (span // 8, 0x53000002),
                      (span // 4, 0x53000204), (span // 4, 0x53000206),
                      (span // 2, 0x53000207), (span // 2, 0x53000205),
                      (span * 3 // 4, 0x53000300), (span, 0x53000009)]
            if fault == "overflow" and core == 63 and launch == launches - 1:
                events.pop()
            records.extend(struct.pack("<QII", begin + tick, event, 0) for tick, event in events)
        dropped = int(fault == "overflow" and core == 63)
        stream = (rank << 16) | core
        if fault == "rank": stream += 1 << 16
        blocks.append(struct.pack("<IIIIQ", stream, len(records), dropped, 2 if dropped else 1,
                                  24 + 16 * len(records)) + b"".join(records))
    if fault == "capture": capture_id = "563acd67-e125-46c6-bd57-69ca989951dd"
    body = b"".join(blocks)
    header = struct.pack("<8sHHHHHBBI16sQ", b"TXRPRF1\0", 1, 0, 48, 16, 24, 1, 0,
                         64, UUID(capture_id).bytes, 48 + len(body))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(header + body)
    return Path(path)
