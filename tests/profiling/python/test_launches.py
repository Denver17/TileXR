import tempfile
import unittest
from pathlib import Path

from tools.profiling.analysis import analyze_run
from tools.profiling.model import ProfilingInputError
from tools.profiling.pairing import load_profile_run

from _fixtures import mapping_document, record_block, stream_id, write_capture


class ProfilingLaunchTests(unittest.TestCase):
    def load(self, blocks, mapping=None, **selection):
        with tempfile.TemporaryDirectory() as tmp:
            mapping_path, records_path = write_capture(Path(tmp), mapping=mapping, blocks=blocks)
            return load_profile_run(mapping_path, [records_path], **selection)

    def test_repeated_launches_preserve_each_rank_and_core(self):
        blocks = [record_block(stream_id(rank, core), records=[
            (offset, 1), (offset + 10, 6),
            (offset + 100, 1), (offset + 120, 6),
        ]) for rank, core, offset in [(0, 0, 0), (0, 1, 5), (1, 0, 500)]]

        run = self.load(blocks, expected_launch_count=2)

        self.assertEqual(run.status, "complete")
        self.assertEqual([launch.id for launch in run.launches], [0, 1])
        for launch in run.launches:
            self.assertEqual([(lane.rank, lane.core) for lane in launch.lanes], [(0, 0), (0, 1), (1, 0)])
        result = analyze_run(run)
        self.assertEqual(result.latency.minimum_us, 0.01)
        self.assertEqual(result.latency.maximum_us, 0.02)
        # Between-launch timing changes are not cross-rank imbalance.
        self.assertEqual(result.rank_imbalance_ratio, 1.0)

    def test_selection_excludes_warmup_and_preserves_original_launch_ids(self):
        records = [(0, 1), (10000, 6), (11000, 1), (11020, 6), (12000, 1), (12030, 6)]
        run = self.load([record_block(0, records=records)], skip_launches=1,
                        launch_count=2, expected_launch_count=3)

        self.assertEqual(run.status, "complete")
        self.assertEqual([launch.id for launch in run.launches], [1, 2])
        self.assertEqual(analyze_run(run).latency.maximum_us, 0.03)
        self.assertEqual(run.attributes["selected_launch_ids"], [1, 2])

    def test_warmup_selection_does_not_hide_capture_loss(self):
        run = self.load([record_block(0, records=[(0, 1), (100, 1), (120, 6)])],
                        skip_launches=1, launch_count=1, expected_launch_count=2)

        self.assertEqual(run.status, "incomplete")
        self.assertEqual(analyze_run(run).latency.maximum_us, 0.02)
        self.assertTrue(any(item.code == "unmatched_begin" and item.details["launch_id"] == 0
                            for item in run.diagnostics))

    def test_truncated_interval_never_pairs_across_new_launch(self):
        run = self.load([record_block(0, records=[
            (0, 1), (10, 2), (100, 1), (110, 3), (120, 6),
        ])])

        self.assertEqual(run.status, "incomplete")
        self.assertFalse(any(sample.stage_id == 1 for launch in run.launches
                             for lane in launch.lanes for sample in lane.samples))
        self.assertEqual(analyze_run(run).latency.maximum_us, 0.02)
        self.assertEqual(len(run.launches[0].lanes[0].records), 2)

    def test_repeated_nested_operations_and_skipped_branch(self):
        mapping = mapping_document()
        mapping["stages"][2]["parent"] = "peer_wait"
        run = self.load([record_block(0, records=[
            (0, 1), (10, 2), (12, 4), (15, 5), (20, 3),
            (30, 2), (40, 3),
            (50, 2), (52, 4), (56, 5), (60, 3), (100, 6),
        ])], mapping)

        self.assertEqual(run.status, "complete")
        self.assertEqual(analyze_run(run).stages[1].sample_count, 3)
        self.assertEqual(analyze_run(run).stages[2].sample_count, 2)

    def test_truncated_child_never_pairs_with_next_parent_invocation(self):
        mapping = mapping_document()
        mapping["stages"][2]["parent"] = "peer_wait"
        run = self.load([record_block(0, records=[
            (0, 1), (10, 2), (12, 4), (20, 3),
            (30, 2), (35, 5), (40, 4), (45, 5), (50, 3), (100, 6),
        ])], mapping)

        self.assertEqual(run.status, "incomplete")
        children = [sample for sample in run.launches[0].lanes[0].samples if sample.stage_id == 2]
        self.assertEqual([(sample.raw_start, sample.raw_end) for sample in children], [(40, 45)])
        self.assertIn("unmatched_end", {item.code for item in run.diagnostics})

    def test_ambiguous_repeated_begin_recovers_at_next_clean_interval(self):
        run = self.load([record_block(0, records=[
            (0, 1), (10, 2), (11, 2), (15, 3), (20, 2), (25, 3), (30, 6),
        ])])
        self.assertEqual(run.status, "incomplete")
        waits = [sample for sample in run.launches[0].lanes[0].samples if sample.stage_id == 1]
        self.assertEqual([(sample.raw_start, sample.raw_end) for sample in waits], [(20, 25)])

    def test_preamble_without_begin_is_retained_without_pairing_into_launch(self):
        run = self.load([record_block(0, records=[(0, 2), (10, 1), (15, 3), (20, 6)])])
        self.assertEqual(run.status, "incomplete")
        self.assertEqual([launch.id for launch in run.launches], [-1, 0])
        self.assertEqual(run.launches[0].attributes, {"fragment": True})
        self.assertFalse(any(sample.stage_id == 1 for launch in run.launches
                             for lane in launch.lanes for sample in lane.samples))

    def test_missing_launch_counts_are_detected_without_guessing_positions(self):
        run = self.load([
            record_block(0, records=[(0, 1), (10, 6), (20, 1), (30, 6)]),
            record_block(1, records=[(20, 1), (30, 6)]),
        ])

        self.assertEqual(run.status, "incomplete")
        diagnostic = next(item for item in run.diagnostics if item.code == "launch_count_mismatch")
        self.assertEqual(diagnostic.details["stream_id"], 1)
        self.assertNotIn("missing_launch_id", diagnostic.details)
        self.assertIn("cannot be located", diagnostic.message)
        self.assertEqual(run.attributes["missing_launch_lane_count"], 1)

    def test_expected_count_detects_wholly_missing_launch_in_all_streams(self):
        run = self.load([record_block(0, records=[(0, 1), (10, 6)])], expected_launch_count=2)
        self.assertEqual(run.status, "incomplete")
        self.assertIn("launch_count_mismatch", {item.code for item in run.diagnostics})

    def test_absent_requested_launch_has_no_fabricated_latency(self):
        run = self.load([record_block(0, records=[(0, 1), (10, 6)])],
                        skip_launches=1, launch_count=1)
        self.assertEqual(run.launches, ())
        self.assertEqual(run.status, "incomplete")
        self.assertIsNone(analyze_run(run).latency.maximum_us)

    def test_invalid_selections_are_rejected(self):
        for selection in ({"skip_launches": -1}, {"skip_launches": None},
                          {"launch_count": 0}, {"expected_launch_count": True},
                          {"launch_count": 1.5},
                          {"skip_launches": 1, "launch_count": 2, "expected_launch_count": 2}):
            with self.subTest(selection=selection):
                with self.assertRaises(ProfilingInputError):
                    self.load([record_block(0)], **selection)


if __name__ == "__main__":
    unittest.main()
