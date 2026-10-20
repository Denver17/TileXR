import tempfile
import unittest
from pathlib import Path

from tilexr_profiling.model import ProfilingInputError
from tilexr_profiling.pairing import load_profile_run
from tilexr_profiling.records import parse_record_bytes

from _fixtures import CAPTURE_ID, mapping_document, record_block, records_bytes, write_capture


class ProfilingRecordTests(unittest.TestCase):
    def test_parses_exact_file_stream_and_record_layout(self):
        data = records_bytes([record_block(100, 0)])
        parsed = parse_record_bytes(data)

        self.assertEqual(str(parsed.capture_id), CAPTURE_ID)
        self.assertEqual(len(parsed.streams), 1)
        stream = parsed.streams[0]
        self.assertEqual(stream.id, 100)
        self.assertTrue(stream.complete)
        self.assertFalse(stream.overflow)
        self.assertEqual(stream.records[0].timestamp, 1000)
        self.assertEqual(stream.records[0].event_id, 1)

    def test_rejects_truncated_file(self):
        data = records_bytes([record_block(100, 0)])
        with self.assertRaisesRegex(ProfilingInputError, "size mismatch|truncated"):
            parse_record_bytes(data[:-1])

    def test_rejects_bad_magic(self):
        data = bytearray(records_bytes([record_block(100, 0)]))
        data[0] = ord("X")
        with self.assertRaisesRegex(ProfilingInputError, "invalid records magic"):
            parse_record_bytes(bytes(data))

    def test_rejects_unknown_stream_flags(self):
        block = record_block(100, 0)
        block["flags"] = 4
        with self.assertRaisesRegex(ProfilingInputError, "unknown stream flags"):
            parse_record_bytes(records_bytes([block]))

    def test_rejects_nonzero_record_reserved_field(self):
        data = bytearray(records_bytes([record_block(100, 0)]))
        first_record_reserved = 48 + 24 + 12
        data[first_record_reserved] = 1
        with self.assertRaisesRegex(ProfilingInputError, "nonzero reserved"):
            parse_record_bytes(bytes(data))

    def test_rejects_non_monotonic_stream_timestamps(self):
        block = record_block(100, records=[(2000, 1), (1000, 6)])
        with self.assertRaisesRegex(ProfilingInputError, "not non-decreasing"):
            parse_record_bytes(records_bytes([block]))

    def test_capture_id_is_derived_from_records(self):
        other_capture_id = "118f6f5e-41a0-7d31-9ad1-82d61fc0a001"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mapping_path, _ = write_capture(root, mapping=mapping_document())
            records_path = root / "mismatch.records.bin"
            records_path.write_bytes(records_bytes([record_block(100, 0)], other_capture_id))
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.capture_id, other_capture_id)

    def test_overflow_and_dropped_records_are_incomplete_not_fatal(self):
        block = record_block(100, 0, dropped_count=3, overflow=True)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mapping_path, records_path = write_capture(root, blocks=[block])
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.status, "incomplete")
        self.assertEqual(run.dropped_record_count, 3)
        self.assertEqual(
            {item.code for item in run.diagnostics},
            {"records_dropped", "stream_overflow"},
        )


if __name__ == "__main__":
    unittest.main()
