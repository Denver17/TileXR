import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from tilexr_profiling.analysis import analyze_run
from tilexr_profiling import generate_reports
from tilexr_profiling.model import ProfilingInputError
from tilexr_profiling.pairing import load_profile_run

from _fixtures import mapping_document, record_block, stream_id, write_capture


class CoverageStatisticsTests(unittest.TestCase):
    def test_finite_individual_intervals_cannot_publish_infinite_statistics(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mapping, records = write_capture(
                root, mapping=mapping_document(1e-300),
                blocks=[record_block(stream_id(rank), records=[(0, 1), (100000000, 6)])
                        for rank in range(2)],
            )
            run = load_profile_run(mapping, [records])
            self.assertEqual(run.launches[0].lanes[0].samples[0].duration_us, 1e308)
            with self.assertRaisesRegex(ProfilingInputError, "finite statistical range"):
                analyze_run(run)
            output = root / "report"
            with self.assertRaises(ProfilingInputError):
                generate_reports(mapping, [records], output)
            self.assertFalse(output.exists())

    def test_derived_ratio_cannot_overflow_even_when_sums_remain_finite(self):
        run = self.run_with([(1, 0, 1)])
        lane = run.launches[0].lanes[0]
        first = replace(lane, samples=tuple(replace(sample, duration_us=1e-308)
                                            if sample.critical else sample for sample in lane.samples))
        second = replace(lane, stream_id=65536, rank=1,
                         samples=tuple(replace(sample, duration_us=1e308)
                                       if sample.critical else sample for sample in lane.samples))
        run = replace(run, launches=(replace(run.launches[0], lanes=(first, second)),))
        with self.assertRaisesRegex(ProfilingInputError, "imbalance_ratio"):
            analyze_run(run)

    def run_with(self, intervals, critical=(0, 2000), origin=0):
        with tempfile.TemporaryDirectory() as tmp:
            mapping, records = write_capture(Path(tmp), blocks=[record_block(stream_id(0))])
            run = load_profile_run(mapping, [records])
        lane = run.launches[0].lanes[0]
        prototype = lane.samples[0]

        def sample(stage_id, start, end, parent=None, category="compute", is_critical=False, kind="interval"):
            return replace(prototype, stage_id=stage_id, name="stage" + str(stage_id),
                           parent_stage_id=parent, category=category, critical=is_critical,
                           kind=kind, raw_start=start + origin, raw_end=end + origin,
                           start_us=start / 1000, end_us=end / 1000,
                           duration_us=(end - start) / 1000)

        samples = [sample(0, *critical, category="total", is_critical=True)] if critical else []
        samples += [sample(*interval) for interval in intervals]
        lane = replace(lane, samples=tuple(samples))
        return replace(run, launches=(replace(run.launches[0], lanes=(lane,)),))

    def test_nested_wait_sync_pressure_uses_union(self):
        run = self.run_with([(1, 100, 1900, 0, "wait"), (2, 200, 1800, 1, "sync")])
        result = analyze_run(run)
        self.assertAlmostEqual(result.wait_sync_pressure, 0.9)
        self.assertAlmostEqual(sum(result.stages[key].share_of_critical for key in (1, 2)), 1.7)
        self.assertAlmostEqual(result.stages[1].exclusive_us, 0.2)
        pressure = next(f for f in result.findings if f.code == "wait_sync_pressure")
        self.assertIn("90.0%", pressure.evidence)

    def test_disjoint_adjacent_overlapping_duplicate_and_zero_intervals(self):
        cases = [
            ([(0, 200), (400, 700)], 0.5),
            ([(0, 200), (200, 700)], 0.7),
            ([(0, 500), (300, 700)], 0.7),
            ([(0, 700), (0, 700)], 0.7),
            ([(100, 100)], 0.0),
        ]
        for spans, expected in cases:
            with self.subTest(spans=spans):
                result = analyze_run(self.run_with([(1, start, end, 0) for start, end in spans]))
                self.assertAlmostEqual(result.stages[1].union_us, expected)
                self.assertAlmostEqual(result.stages[1].exclusive_us, expected)

    def test_clip_to_critical_before_merging_and_preserve_inclusive(self):
        result = analyze_run(self.run_with([(1, -1000, 1000, 0), (1, 1500, 3000, 0)]))
        stage = result.stages[1]
        self.assertEqual(stage.total_us, 3.5)
        self.assertEqual(stage.union_us, 1.5)
        self.assertEqual(stage.union_share_of_critical, 0.75)

    def test_multiple_parent_instances_subtract_child_union_once(self):
        result = analyze_run(self.run_with([
            (1, 100, 1100, 0), (1, 500, 1500, 0),
            (2, 200, 1000, 1), (3, 700, 1200, 1),
            (4, 300, 500, 2),
        ]))
        self.assertAlmostEqual(result.stages[1].union_us, 1.4)
        self.assertAlmostEqual(result.stages[1].exclusive_us, 0.4)
        self.assertAlmostEqual(result.stages[2].exclusive_us, 0.6)

    def test_huge_absolute_timer_keeps_short_coverage_exact(self):
        result = analyze_run(self.run_with([(1, 1, 3, 0)], critical=(0, 4), origin=2**63))
        self.assertEqual(result.stages[1].union_us, 0.002)
        self.assertEqual(result.stages[1].union_share_of_critical, 0.5)
        self.assertEqual(result.stages[0].exclusive_us, 0.002)

    def test_missing_or_multiple_critical_makes_new_statistics_unknown(self):
        for critical_count in (0, 2):
            run = self.run_with([(1, 1, 3)], critical=None)
            if critical_count:
                lane = run.launches[0].lanes[0]
                critical = replace(lane.samples[0], stage_id=0, name="total", critical=True)
                lane = replace(lane, samples=lane.samples + (critical, critical))
                run = replace(run, launches=(replace(run.launches[0], lanes=(lane,)),))
            result = analyze_run(run)
            self.assertIsNone(result.stages[1].union_us)
            self.assertIsNone(result.stages[1].critical_lane_union_us)
            self.assertIsNone(result.stages[1].union_share_of_critical)
            self.assertIsNone(result.wait_sync_pressure)

    def test_zero_critical_has_zero_coverage_and_unknown_share(self):
        result = analyze_run(self.run_with([(1, 0, 100, 0)], critical=(0, 0)))
        self.assertEqual(result.stages[1].union_us, 0.0)
        self.assertIsNone(result.stages[1].union_share_of_critical)
        self.assertIsNone(result.wait_sync_pressure)

    def test_instant_does_not_cover_time_and_lanes_are_not_merged(self):
        run = self.run_with([(1, 0, 1000, 0), (2, 500, 500, 0, "diagnostic", False, "instant")])
        lane = run.launches[0].lanes[0]
        second = replace(lane, stream_id=65536, rank=1)
        run = replace(run, launches=(replace(run.launches[0], lanes=(lane, second)),))
        result = analyze_run(run)
        self.assertEqual(result.stages[1].union_us, 2.0)
        self.assertEqual(result.stages[1].critical_lane_union_us, 1.0)
        self.assertEqual(result.stages[2].union_us, 0.0)


if __name__ == "__main__":
    unittest.main()
