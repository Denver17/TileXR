#!/usr/bin/env python3
"""不启动 NPU 的 profile 读取及跨 rank 测量汇总测试。"""

import copy
from contextlib import closing
import csv
from pathlib import Path
import sqlite3
import tempfile
import unittest

import measurements as measure


class MeasurementTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tilexr-measurements-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.config = dict(run_mode="performance", rank_size=2, loops=3, warmup=1,
                           bs=256, h=7168, topk=6, moe_expert_num=384, dtype="bf16", block_dim="auto")

    def csv_profile(self, durations, kernel=measure.KERNEL_NAME, name="PROF_rank", reverse=False):
        path = self.root / name / "mindstudio_profiler_output/op_summary.csv"
        path.parent.mkdir(parents=True, exist_ok=True)
        rows = [(kernel, index, value) for index, value in enumerate(durations)]
        if reverse:
            rows.reverse()
        with path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.writer(output)
            writer.writerow(("Op Name", "Task Start Time(us)", "Task Duration(us)"))
            writer.writerows(rows)
        return path

    def sqlite_profile(self, durations):
        path = self.root / "PROF_rank/device_3/sqlite/ai_core_op_summary.db"
        path.parent.mkdir(parents=True)
        with closing(sqlite3.connect(path)) as db:
            db.execute("CREATE TABLE ge_summary (task_id INTEGER, stream_id INTEGER, index_id INTEGER, "
                       "model_id INTEGER, batch_id INTEGER, op_name TEXT)")
            db.execute("CREATE TABLE task_time (task_id INTEGER, stream_id INTEGER, index_id INTEGER, "
                       "model_id INTEGER, batch_id INTEGER, start_time REAL, duration_time REAL)")
            for index, value in enumerate(durations):
                key = (index, 1, -1, 4294967295, 0)
                db.execute("INSERT INTO ge_summary VALUES (?, ?, ?, ?, ?, ?)", (*key, measure.KERNEL_NAME))
                db.execute("INSERT INTO task_time VALUES (?, ?, ?, ?, ?, ?, ?)", (*key, index, value))
            db.commit()
        return path

    def performance_rank(self, rank, durations=(1000, 2, 4, 9)):
        path = self.csv_profile(durations, name=f"PROF_rank{rank}")
        return {"rank": rank, "performance": measure.performance_record(path.parents[1], 3, 1)}

    def breakdown_rank(self, rank):
        return {"rank": rank,
                "breakdown": {"run_mode": "breakdown", "loops": 3, "warmup": 1,
                              "generation": 4, "block_dim": 2, "records": [
                    {"core": 1, "start": 1000, "checked": 1004 + rank * 2, "end": 1010 + rank * 10},
                    {"core": 0, "start": 500, "checked": 507 + rank * 2, "end": 509 + rank * 2}]}}

    def test_performance_sorts_by_start_and_excludes_warmup(self):
        path = self.csv_profile([1000, 2, 4, 9], reverse=True)
        result = measure.performance_record(self.root, 3, 1)
        self.assertEqual(result["sample_us"], [2, 4, 9])
        self.assertEqual(result["matched_samples"], 4)
        self.assertEqual(result["median_us"], 4)
        self.assertEqual(result["minimum_us"], 2)
        self.assertEqual(result["maximum_us"], 9)
        self.assertEqual(result["mean_us"], 5)
        self.assertEqual(result["source"]["path"], str(path))

    def test_performance_reuses_static_kernel_name_matching(self):
        static = measure.KERNEL_NAME.removesuffix("_kernel") + "_static_0123456789abcdef"
        self.csv_profile([1], kernel=static)
        self.assertEqual(measure.performance_record(self.root, 1, 0)["source"]["matched_kernel_names"], [static])

    def test_sqlite_profile_converts_ns_and_supports_small_loop_count(self):
        path = self.sqlite_profile([100000, 2500, 4500])
        result = measure.performance_record(self.root, 2, 1)
        self.assertEqual(result["sample_us"], [2.5, 4.5])
        self.assertEqual(result["median_us"], 3.5)
        self.assertEqual(result["source"]["source_type"], "ai_core_op_summary_db")
        self.assertEqual(result["source"]["path"], str(path))

    def test_rejects_wrong_kernel_or_missing_and_extra_samples(self):
        for durations, kernel in (([1, 2], measure.KERNEL_NAME), ([1, 2, 3], measure.KERNEL_NAME),
                                  ([1, 2, 3, 4, 5], measure.KERNEL_NAME),
                                  ([1, 2, 3, 4], "tilexr_ep_dispatch_memory_kernel")):
            with self.subTest(durations=durations, kernel=kernel):
                self.csv_profile(durations, kernel=kernel)
                with self.assertRaises(ValueError):
                    measure.performance_record(self.root, 3, 1)

    def test_rejects_nonpositive_and_nonfinite_csv_including_warmup(self):
        for invalid in (0, -1, float("inf"), float("nan")):
            for index in (0, 2):
                durations = [100, 1, 2, 3]
                durations[index] = invalid
                self.csv_profile(durations)
                with self.subTest(invalid=invalid, index=index), self.assertRaises(ValueError):
                    measure.performance_record(self.root, 3, 1)

    def test_negative_sqlite_row_cannot_be_discarded_to_make_this_run_pass(self):
        self.sqlite_profile([-1, 1000, 2000, 3000, 4000])
        with self.assertRaisesRegex(ValueError, "invalid kernel"):
            measure.performance_record(self.root, 3, 1)

    def test_multiple_profiler_sessions_are_rejected(self):
        self.csv_profile([1, 2, 3, 4], name="PROF_first")
        self.csv_profile([1, 2, 3, 4], name="PROF_second")
        with self.assertRaisesRegex(ValueError, "唯一"):
            measure.performance_record(self.root, 3, 1)

    def test_duplicate_csv_sample_cannot_replace_a_missing_iteration(self):
        path = self.csv_profile([100, 1, 1, 5])
        with path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.writer(output)
            writer.writerow(("Op Name", "Task Start Time(us)", "Task Duration(us)"))
            writer.writerows((measure.KERNEL_NAME, start, duration)
                             for start, duration in ((0, 100), (100, 1), (100, 1), (110, 5)))
        with self.assertRaisesRegex(ValueError, "duplicate kernel"):
            measure.performance_record(self.root, 3, 1)
        # 旧调用默认仍保留原解析语义；严格限制仅服务本单 stream 检查点。
        self.assertEqual(measure.profiler.series_from_input(path, measure.KERNEL_NAME, 3)[0], [1, 1, 5])

    def test_duplicate_sqlite_sample_cannot_replace_a_missing_iteration(self):
        path = self.sqlite_profile([100000, 1000, 1000, 5000])
        with closing(sqlite3.connect(path)) as db:
            db.execute("UPDATE task_time SET start_time=1 WHERE task_id=2")
            db.commit()
        with self.assertRaisesRegex(ValueError, "duplicate kernel"):
            measure.performance_record(self.root, 3, 1)

    def test_same_session_csv_and_sqlite_are_one_profile_input(self):
        self.sqlite_profile([100000, 2500, 4500])
        path = self.csv_profile([100, 2.5, 4.5])
        result = measure.performance_record(self.root, 2, 1)
        self.assertEqual(result["source"]["path"], str(path))
        self.assertEqual(result["source"]["source_type"], "op_summary_csv")
        self.assertEqual(result["sample_us"], [2.5, 4.5])

    def test_invalid_loop_and_warmup_counts_are_rejected(self):
        for loops, warmup in ((0, 0), (-1, 1), (1, -1), (True, 0), (1, 0.5)):
            with self.subTest(loops=loops, warmup=warmup), self.assertRaises(ValueError):
                measure.performance_record(self.root, loops, warmup)

    def test_summary_uses_rank_medians_not_pooled_samples_or_means(self):
        ranks = [self.performance_rank(0, (999, 1, 10, 11)), self.performance_rank(1, (888, 4, 5, 100))]
        summary = measure.summarize_measurements(list(reversed(ranks)), self.config, "a" * 64)
        self.assertEqual(summary["performance"]["rank_median_us"], [10, 5])
        self.assertEqual(summary["performance"]["median_us"], 7.5)
        self.assertEqual(summary["performance"]["minimum_us"], 5)
        self.assertEqual(summary["performance"]["maximum_us"], 10)
        self.assertNotIn("host_batch_us", summary)
        self.assertEqual(summary["algorithm_communication_bytes"], 0)
        self.assertEqual(summary["native_status"], "empty")
        report = measure.render_measurements(summary)
        self.assertIn("median=7.500 us min=5.000 us max=10.000 us", report)
        self.assertNotIn("Host 测量批", report)
        self.assertIn("backend=memory", report)
        self.assertIn("带宽=N/A", report)
        self.assertIn("sha256=" + "a" * 64, report)

    def test_missing_duplicate_or_foreign_rank_is_rejected(self):
        ranks = [self.performance_rank(0), self.performance_rank(1)]
        for invalid in (ranks[:1], [ranks[0], ranks[0]], [ranks[0], dict(ranks[1], rank=2)]):
            with self.assertRaisesRegex(ValueError, "rank"):
                measure.summarize_measurements(invalid, self.config, "a" * 64)

    def test_corrupted_performance_evidence_is_rejected(self):
        original = self.performance_rank(0)
        invalid = [dict(original, performance={})]
        for key, value in (("samples", 2), ("matched_samples", 9), ("sample_us", [1, 2]),
                           ("sample_us", [0, 4, 9]), ("sample_us", [float("nan"), 4, 9]),
                           ("median_us", 123), ("kernel", "wrong"), ("source", {})):
            invalid.append(dict(original, performance=dict(original["performance"], **{key: value})))
        for record in invalid:
            with self.subTest(record=record), self.assertRaises(ValueError):
                measure.validate_rank_measurements(record, self.config)

    def test_breakdown_uses_independent_core_maxima_and_raw_ticks(self):
        config = dict(self.config, run_mode="breakdown")
        summary = measure.summarize_measurements([self.breakdown_rank(0), self.breakdown_rank(1)], config, "b" * 64)
        stages = summary["breakdown"]["stages"]
        self.assertEqual(stages["entry_checks"], {"median": 8, "minimum": 7, "maximum": 9, "mean": 8})
        self.assertEqual(stages["record_prepare"]["median"], 10)
        self.assertEqual(stages["empty_total"]["median"], 15)
        self.assertEqual(summary["breakdown"]["stages_us"]["empty_total"]["median"], 0.015)
        self.assertEqual(summary["breakdown"]["selection"], "final_measured_iteration")
        self.assertIn("ticks", measure.render_measurements(summary))
        self.assertIn("未计量最终记录写出", measure.render_measurements(summary))

    def test_breakdown_uses_documented_a5_1ghz_clock_and_preserves_provenance(self):
        config = dict(self.config, run_mode="breakdown")
        ranks = [self.breakdown_rank(0), self.breakdown_rank(1)]
        summary = measure.summarize_measurements(ranks, config, "b" * 64)
        breakdown = summary["breakdown"]
        self.assertEqual(breakdown["ticks_per_us"], 1000)
        self.assertEqual(breakdown["clock"]["frequency_hz"], 1000000000)
        self.assertEqual(breakdown["clock"]["version"], "9.1.0")
        self.assertEqual(breakdown["clock"]["commit"], "91c03327210113585f52739a6c85c35ece492970")
        self.assertIn("GetSystemCycle_ISASI.md", breakdown["clock"]["url"])
        self.assertEqual(breakdown["stages_us"]["empty_total"],
                         {"median": 0.015, "minimum": 0.01, "maximum": 0.02, "mean": 0.015})
        self.assertEqual(breakdown["rank_stages"][0]["empty_total"], 10)
        self.assertIn("empty_total：median=0.015 us min=0.010 us max=0.020 us", measure.render_measurements(summary))

    def test_breakdown_invalid_core_identity_and_timestamps_are_rejected(self):
        original = self.breakdown_rank(0)
        config = dict(self.config, run_mode="breakdown")
        bad_values = (("core", 0), ("core", 2), ("start", 0), ("start", 1005),
                      ("checked", 1011), ("checked", 999), ("end", 1000),
                      ("end", 1 << 64), ("start", 1000.0))
        for key, value in bad_values:
            record = copy.deepcopy(original)
            record["breakdown"]["records"][0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                measure.validate_rank_measurements(record, config)
        for key, value in (("records", []), ("generation", 0), ("block_dim", 3)):
            record = dict(original, breakdown=dict(original["breakdown"], **{key: value}))
            with self.assertRaises(ValueError):
                measure.validate_rank_measurements(record, config)

    def test_breakdown_marker_must_match_current_batch_and_explicit_block_dim(self):
        original = self.breakdown_rank(0)
        config = dict(self.config, run_mode="breakdown", block_dim="2")
        measure.validate_rank_measurements(original, config)
        for key, value in (("generation", 3), ("run_mode", "performance"), ("loops", 2), ("warmup", 2)):
            record = dict(original, breakdown=dict(original["breakdown"], **{key: value}))
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "本次配置"):
                measure.validate_rank_measurements(record, config)
        with self.assertRaisesRegex(ValueError, "block_dim.*本次配置"):
            measure.validate_rank_measurements(original, dict(config, block_dim="1"))

    def test_functional_has_no_measurements_or_report(self):
        config = dict(self.config, run_mode="functional", loops=1, warmup=0)
        summary = measure.summarize_measurements([{"rank": 0}, {"rank": 1}], config, "a" * 64)
        self.assertNotIn("performance", summary)
        self.assertNotIn("breakdown", summary)
        self.assertNotIn("host_batch_us", summary)
        self.assertEqual(measure.render_measurements(summary), "")


if __name__ == "__main__":
    unittest.main()
