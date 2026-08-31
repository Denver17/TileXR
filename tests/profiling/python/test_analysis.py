import copy
import tempfile
import unittest
from pathlib import Path

from tools.profiling.analysis import analyze_run
from tools.profiling.pairing import load_profile_run

from _fixtures import (
    overlapping_child_capture,
    record_block,
    stream_id,
    stream_records,
    write_capture,
)


class ProfilingAnalysisTests(unittest.TestCase):
    def load(self, mapping=None, blocks=None):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        mapping_path, records_path = write_capture(root, mapping=mapping, blocks=blocks)
        return load_profile_run(mapping_path, [records_path])

    def test_reports_rank_latency_and_critical_lane(self):
        result = analyze_run(self.load())

        self.assertEqual(result.latency.minimum_us, 2.0)
        self.assertEqual(result.latency.median_us, 3.5)
        self.assertEqual(result.latency.maximum_us, 5.0)
        self.assertIsNotNone(result.critical_lane)
        self.assertEqual(result.critical_lane.rank, 1)
        self.assertEqual(result.critical_lane.stream_id, stream_id(1))
        self.assertEqual(result.critical_lane.duration_us, 5.0)

    def test_rank_latency_uses_slowest_core_per_rank_invocation(self):
        slow_core = copy.deepcopy(stream_records(0))
        slow_core[-2] = (99000, 6)
        slow_core[-1] = (99050, 7)
        blocks = [
            record_block(stream_id(0, 0), 0),
            record_block(stream_id(0, 1), records=slow_core),
            record_block(stream_id(1, 0), 1),
        ]

        result = analyze_run(self.load(blocks=blocks))

        self.assertEqual(result.latency.minimum_us, 5.0)
        self.assertEqual(result.latency.median_us, 51.5)
        self.assertEqual(result.latency.maximum_us, 98.0)
        self.assertEqual(result.critical_lane.rank, 0)
        self.assertEqual(result.critical_lane.core, 1)

    def test_reports_stage_share_without_optional_metrics(self):
        result = analyze_run(self.load())

        wait = result.stages[1]
        transfer = result.stages[2]
        self.assertEqual(wait.critical_lane_us, 2.0)
        self.assertEqual(wait.share_of_critical, 0.4)
        self.assertIsNone(transfer.bytes)
        self.assertIsNone(transfer.bandwidth_gbps)

    def test_per_interval_metric_multiplies_repeated_intervals(self):
        records = [
            (1000, 1),
            (1200, 4),
            (1400, 5),
            (1600, 4),
            (2000, 5),
            (3000, 6),
        ]
        result = analyze_run(
            self.load(blocks=[record_block(stream_id(0), records=records)])
        )

        self.assertEqual(result.stages[2].sample_count, 2)
        self.assertIsNone(result.stages[2].bytes)

    def test_reports_parent_and_instance_count_for_overlapping_children(self):
        mapping, records = overlapping_child_capture()
        result = analyze_run(
            self.load(mapping, [record_block(stream_id(0), records=records)])
        )

        child = result.stages[2]
        self.assertEqual(child.parent_stage_id, 0)
        self.assertEqual(child.instance_count, 2)
        self.assertEqual(child.sample_count, 2)

    def test_emits_only_producer_neutral_findings(self):
        result = analyze_run(self.load())
        codes = {finding.code for finding in result.findings}

        self.assertIn("largest_critical_stage", codes)
        self.assertIn("wait_sync_pressure", codes)
        self.assertIn("rank_imbalance", codes)

    def test_no_usable_critical_interval_reports_unavailable_latency(self):
        records = [(1000, 1), (1200, 2), (1400, 3)]
        result = analyze_run(
            self.load(blocks=[record_block(stream_id(0), records=records)])
        )

        self.assertIsNone(result.latency.minimum_us)
        self.assertIsNone(result.latency.median_us)
        self.assertIsNone(result.latency.maximum_us)
        self.assertIsNone(result.critical_lane)
        self.assertIn(
            "no_usable_critical_interval",
            {finding.code for finding in result.findings},
        )


if __name__ == "__main__":
    unittest.main()
