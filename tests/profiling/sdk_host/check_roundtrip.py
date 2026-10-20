"""Verify actual C++ SDK output through the canonical reader and report API."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from tilexr_profiling import generate_reports
from tilexr_profiling.records import parse_record_bytes


def check(executable):
    with tempfile.TemporaryDirectory(prefix="profiling-sdk-roundtrip-") as temporary:
        root = Path(temporary)
        records = root / "sdk.records.bin"
        subprocess.run([executable, str(records)], check=True)
        capture = parse_record_bytes(records.read_bytes())
        assert capture.capture_id.bytes == bytes([1]) + bytes(15)
        assert [stream.id for stream in capture.streams] == [(3 << 16) | lane for lane in range(5)]
        sparse, backfill, overflow, empty, unsubmitted = capture.streams
        events = lambda stream: [(record.timestamp, record.event_id) for record in stream.records]
        assert events(sparse) == [(0, 1), (10, 2)]
        assert events(backfill) == [(0, 1), (0, 3), (10, 2)]
        assert sparse.complete and backfill.complete
        assert overflow.complete and overflow.overflow and overflow.dropped_count == 1
        assert events(overflow) == [(0, 1)] + [(i, 3) for i in range(1, 8)]
        assert empty.complete and not empty.records
        assert not unsubmitted.complete and not unsubmitted.records
        mapping = {
            "schema": "tilexr_profiling_mapping.v1", "operator": "sdk-roundtrip",
            "cycles_per_us": 1,
            "stages": [
                {"name": "total", "category": "total", "critical": True, "begin": 1, "end": 2},
                {"name": "checkpoint", "category": "diagnostic", "instant": 3},
            ],
        }
        result = generate_reports(mapping, [records], root / "report")
        assert result.status == "incomplete" and len(result.artifacts) == 3
        trace = json.loads(result.artifacts[0].read_text(encoding="utf-8"))
        completeness = trace["metadata"]["provenance"]["completeness"]
        assert completeness["present_stream_count"] == 5
        assert completeness["usable_stream_count"] == 2
        assert completeness["dropped_record_count"] == 1
        print("PASS: SDK writers/ACL collection/v1 encoding/canonical reports round-trip (CPU substitutes).")


if __name__ == "__main__":
    check(sys.argv[1])
