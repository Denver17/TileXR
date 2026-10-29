"""Small records.v1 fixtures for validation tests; never imported by production."""
import struct
from pathlib import Path
from uuid import NAMESPACE_URL, UUID, uuid5

CAPTURE_ID = str(uuid5(NAMESPACE_URL, "tilexr-validation:run-test"))


def write_capture(path, rank=0, launches=5, warmup=2, duration=8000, fault="", capture_id=CAPTURE_ID,
                  events_by_core=None):
    blocks = []
    for core in range(64):
        records = []
        selected = range(launches) if fault == "accumulating" else (launches - 1,)
        for launch in selected:
            begin = 100000000000000000 + launch * 1000000 + core
            span = duration * 10 if launch < warmup else duration
            events = [(0, 0x53000001), (span // 8, 0x53000002),
                      (span // 4, 0x53000204), (span // 4, 0x53000206),
                      (span // 2, 0x53000207), (span // 2, 0x53000205),
                      (span * 3 // 4, 0x53000300), (span, 0x53000009)]
            if events_by_core is not None:
                events = list(events_by_core[core])
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


def combine_loop_events(bs, topk, rank_size=16, rows=10):
    events = {}
    for core in range(64):
        ids = [0x3001, 0x3003, 0x3004]
        if core < 32:
            ids += [0x301b, 0x302e, 0x302f]
            if rows:
                for server in range(rank_size // 8):
                    ids += [0x3030, 0x3031]
                    if server == 0 and core < rows:
                        ids += [0x3032, 0x3033, 0x3034, 0x3035]
                ids += [0x3036, 0x3037]
            ids += [0x3021, 0x301c]
        else:
            ids += [0x302a, 0x3038, 0x3039]
            tokens = bs // 32 + (core - 32 < bs % 32)
            for token in range(tokens):
                ids += [0x303a, 0x303b, 0x302c, 0x303c, 0x303d]
                for k in range(topk):
                    ids += [0x303e, 0x303f, 0x3040, 0x3041]
                ids += [0x3042, 0x3043, 0x302d]
            ids += [0x3044, 0x3045, 0x302b]
        ids += [0x3013, 0x3014, 0x3002]
        events[core] = [(index * 1000, event) for index, event in enumerate(ids)]
    return events
