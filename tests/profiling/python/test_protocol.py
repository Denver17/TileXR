"""Critical capture identity, completeness and timestamp contracts."""

import tempfile
import unittest
from pathlib import Path

from tilexr_profiling.model import ProfilingInputError
from tilexr_profiling.pairing import load_profile_run

from _fixtures import mapping_document, record_block, records_bytes, stream_id, write_capture


class ProfilingPairingTests(unittest.TestCase):
    def test_empty_record_capture_is_incomplete(self):
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(Path(tmp), blocks=[])
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.status, "incomplete")
        self.assertEqual(run.present_stream_count, 0)
        self.assertIn("no_record_streams", {item.code for item in run.diagnostics})

    def test_rejects_capture_id_mismatch_across_record_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mapping_path, first = write_capture(
                root, blocks=[record_block(stream_id(0), 0)]
            )
            second = root / "second.records.bin"
            second.write_bytes(
                records_bytes(
                    [record_block(stream_id(1), 1)],
                    "118f6f5e-41a0-7d31-9ad1-82d61fc0a001",
                )
            )

            with self.assertRaisesRegex(ProfilingInputError, "capture ID mismatch"):
                load_profile_run(mapping_path, [first, second])

    def test_unmatched_event_produces_incomplete_lane_without_fabrication(self):
        records = [(1000, 1), (1100, 2), (1500, 3)]
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp), blocks=[record_block(stream_id(0), records=records)]
            )
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.status, "incomplete")
        self.assertEqual(run.usable_stream_count, 0)
        self.assertEqual(run.excluded_stream_count, 1)
        codes = {item.code for item in run.diagnostics}
        self.assertIn("unmatched_begin", codes)
        self.assertIn("invalid_critical_interval_count", codes)

    def test_overlapping_critical_begin_does_not_fabricate_latency(self):
        records = [(1000, 1), (1500, 1), (3000, 6)]
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp), blocks=[record_block(stream_id(0), records=records)]
            )
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.status, "incomplete")
        self.assertEqual(run.usable_stream_count, 0)
        self.assertFalse(any(sample.critical for sample in run.launches[0].lanes[0].samples))

    def test_rejects_non_finite_timestamp_conversion(self):
        mapping = mapping_document(cycles_per_us=5e-324)
        records = [((1 << 64) - 2, 1), ((1 << 64) - 1, 6)]
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp),
                mapping=mapping,
                blocks=[record_block(stream_id(0), records=records)],
            )
            with self.assertRaisesRegex(ProfilingInputError, "finite microseconds"):
                load_profile_run(mapping_path, [records_path])

    def test_preserves_short_intervals_at_large_absolute_timestamps(self):
        base = (1 << 64) - 4
        records = [
            (base, 1),
            (base + 1, 4),
            (base + 2, 5),
            (base + 3, 6),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp), blocks=[record_block(stream_id(0), records=records)]
            )
            run = load_profile_run(mapping_path, [records_path])

        samples = {sample.stage_id: sample for sample in run.launches[0].lanes[0].samples}
        self.assertAlmostEqual(samples[0].duration_us, 0.003)
        self.assertAlmostEqual(samples[2].start_us, 0.001)
        self.assertAlmostEqual(samples[2].end_us, 0.002)

    def test_undeclared_event_id_is_fatal(self):
        records = [(1000, 1), (2000, 99), (3000, 6)]
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp), blocks=[record_block(stream_id(0), records=records)]
            )
            with self.assertRaisesRegex(ProfilingInputError, "undeclared event ID 99"):
                load_profile_run(mapping_path, [records_path])


if __name__ == "__main__":
    unittest.main()
