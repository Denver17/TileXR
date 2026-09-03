import tempfile
import unittest
from pathlib import Path

from tools.profiling.mapping import load_mapping, parse_mapping_document
from tools.profiling.model import ProfilingInputError
from tools.profiling.pairing import load_profile_run

from _fixtures import (
    CAPTURE_ID,
    mapping_document,
    overlapping_child_capture,
    record_block,
    records_bytes,
    stream_id,
    write_capture,
)


class ProfilingMappingTests(unittest.TestCase):
    def test_loads_compact_mapping_and_generates_internal_ids(self):
        mapping = load_mapping(mapping_document())

        self.assertEqual(mapping.operator, "sample_operation")
        self.assertEqual(mapping.cycles_per_us, 1000.0)
        self.assertEqual([stage.id for stage in mapping.stages], [0, 1, 2, 3])
        self.assertEqual(mapping.stages[1].parent_stage_id, 0)
        self.assertEqual(len(mapping.events), 7)

    def test_instances_generate_stable_instance_ids(self):
        document, _ = overlapping_child_capture()
        mapping = parse_mapping_document(document)
        events = [event for event in mapping.events if event.stage_id == 2]

        self.assertEqual([event.instance_id for event in events], [0, 0, 1, 1])

    def test_instance_range_generates_stable_instance_ids(self):
        document = mapping_document()
        document["stages"][2] = {
            "name": "payload_transfer",
            "category": "copy",
            "parent": "kernel_total",
            "instance_range": {"count": 3, "begin": 20, "end": 21, "stride": 2},
        }

        mapping = parse_mapping_document(document)
        events = [event for event in mapping.events if event.stage_id == 2]

        self.assertEqual(
            [(event.id, event.instance_id, event.edge) for event in events],
            [
                (20, 0, "begin"), (21, 0, "end"),
                (22, 1, "begin"), (23, 1, "end"),
                (24, 2, "begin"), (25, 2, "end"),
            ],
        )

    def test_stage_track_is_preserved_as_renderer_metadata(self):
        document = mapping_document()
        document["stages"][1]["track"] = "send"

        mapping = parse_mapping_document(document)

        self.assertEqual(mapping.stages[1].attributes, {"track": "send"})

    def test_rejects_instance_range_uint32_overflow(self):
        document = mapping_document()
        document["stages"][2] = {
            "name": "payload_transfer",
            "category": "copy",
            "parent": "kernel_total",
            "instance_range": {
                "count": 2,
                "begin": (1 << 32) - 2,
                "end": (1 << 32) - 1,
                "stride": 2,
            },
        }

        with self.assertRaisesRegex(ProfilingInputError, "exceeds uint32"):
            parse_mapping_document(document)

    def test_rejects_unknown_structural_field(self):
        document = mapping_document()
        document["prodcer"] = "typo"

        with self.assertRaisesRegex(ProfilingInputError, "unknown.*prodcer"):
            parse_mapping_document(document)

    def test_rejects_non_string_mapping_keys(self):
        document = mapping_document()
        document[1] = "invalid"

        with self.assertRaisesRegex(ProfilingInputError, "keys must be strings"):
            parse_mapping_document(document)

    def test_rejects_numbers_too_large_for_finite_conversion(self):
        document = mapping_document(cycles_per_us=10**10000)

        with self.assertRaisesRegex(ProfilingInputError, "finite"):
            parse_mapping_document(document)

    def test_requires_exactly_one_critical_interval_stage(self):
        document = mapping_document()
        document["stages"][0]["critical"] = False

        with self.assertRaisesRegex(ProfilingInputError, "exactly one critical"):
            parse_mapping_document(document)

    def test_rejects_mixed_event_modes(self):
        document = mapping_document()
        document["stages"][1]["instant"] = 8

        with self.assertRaisesRegex(ProfilingInputError, "exactly one"):
            parse_mapping_document(document)

    def test_rejects_incomplete_interval_mode(self):
        document = mapping_document()
        del document["stages"][1]["end"]

        with self.assertRaisesRegex(ProfilingInputError, "missing required.*end"):
            parse_mapping_document(document)

    def test_rejects_duplicate_event_ids(self):
        document = mapping_document()
        document["stages"][1]["begin"] = 1

        with self.assertRaisesRegex(ProfilingInputError, "duplicate event ID 1"):
            parse_mapping_document(document)

    def test_rejects_unknown_parent(self):
        document = mapping_document()
        document["stages"][1]["parent"] = "missing"

        with self.assertRaisesRegex(ProfilingInputError, "unknown stage"):
            parse_mapping_document(document)

    def test_rejects_instant_parent_stage(self):
        document = mapping_document()
        document["stages"][1]["parent"] = "checkpoint"

        with self.assertRaisesRegex(ProfilingInputError, "must be an interval"):
            parse_mapping_document(document)

    def test_rejects_stage_hierarchy_cycle(self):
        document = mapping_document()
        document["stages"][1]["parent"] = "payload_transfer"
        document["stages"][2]["parent"] = "peer_wait"

        with self.assertRaisesRegex(ProfilingInputError, "hierarchy contains a cycle"):
            parse_mapping_document(document)

    def test_rejects_duplicate_json_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "duplicate.json"
            path.write_text('{"schema":"a","schema":"b"}', encoding="utf-8")

            with self.assertRaisesRegex(ProfilingInputError, "duplicate JSON field"):
                load_mapping(path)


class ProfilingPairingTests(unittest.TestCase):
    def test_pairs_records_and_derives_rank_and_core_from_stream_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(Path(tmp))
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.capture_id, CAPTURE_ID)
        self.assertEqual(run.run_id, CAPTURE_ID)
        self.assertEqual(run.producer, "tilexr")
        self.assertEqual(run.status, "complete")
        self.assertEqual(run.expected_stream_count, 2)
        self.assertEqual(run.present_stream_count, 2)
        self.assertEqual(run.usable_stream_count, 2)
        self.assertEqual(run.missing_stream_count, 0)
        lanes = run.launches[0].lanes
        self.assertEqual([(lane.rank, lane.core) for lane in lanes], [(0, 0), (1, 0)])
        total = next(sample for sample in lanes[0].samples if sample.stage_id == 0)
        self.assertEqual(total.start_us, 0.0)
        self.assertEqual(total.duration_us, 2.0)

    def test_normalizes_each_stream_independently(self):
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(Path(tmp))
            run = load_profile_run(mapping_path, [records_path])

        totals = [
            next(sample for sample in lane.samples if sample.stage_id == 0)
            for lane in run.launches[0].lanes
        ]
        self.assertEqual([sample.start_us for sample in totals], [0.0, 0.0])

    def test_accepts_every_stream_present_in_records(self):
        blocks = [
            record_block(stream_id(0, 3), 0),
            record_block(stream_id(7, 11), 1),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(Path(tmp), blocks=blocks)
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.status, "complete")
        self.assertEqual(
            [(lane.rank, lane.core) for lane in run.launches[0].lanes],
            [(0, 3), (7, 11)],
        )

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

    def test_pairs_overlapping_child_instances_within_each_core(self):
        mapping, records = overlapping_child_capture()
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp),
                mapping=mapping,
                blocks=[record_block(stream_id(0), records=records)],
            )
            run = load_profile_run(mapping_path, [records_path])

        children = [
            sample for sample in run.launches[0].lanes[0].samples if sample.stage_id == 2
        ]
        self.assertEqual([sample.instance_id for sample in children], [0, 1])
        self.assertTrue(all(sample.parent_stage_id == 0 for sample in children))
        self.assertLess(children[1].raw_start, children[0].raw_end)

    def test_reports_child_interval_outside_parent(self):
        mapping, _ = overlapping_child_capture()
        records = [(1000, 1), (1500, 6), (1600, 4), (1700, 5)]
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(
                Path(tmp),
                mapping=mapping,
                blocks=[record_block(stream_id(0), records=records)],
            )
            run = load_profile_run(mapping_path, [records_path])

        self.assertEqual(run.status, "incomplete")
        self.assertIn("child_outside_parent", {item.code for item in run.diagnostics})

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
