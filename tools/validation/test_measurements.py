#!/usr/bin/env python3
"""不启动 NPU 的 profile 读取及跨 rank 测量汇总测试。"""

import copy
from contextlib import closing
import csv
import json
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
        self.config = dict(run_mode="performance", rank_size=16, loops=3, warmup=1,
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

    def test_dispatch_performance_bandwidth_counts_relay_over_full_kernel(self):
        config = dict(self.config, rank_size=16, bs=96, moe_expert_num=96, loops=1, warmup=0,
                      kernel_scope="dispatch", route_mode="balance_shift", seed=2026, quant_mode=0, fp8_format="e4m3")
        ranks = []
        for rank in range(16):
            path = self.csv_profile([rank + 1], name=f"PROF_dispatch{rank}")
            ranks.append(dict(rank=rank, first_hop_packets=3, first_hop_received=3, first_hop_bytes=3*15872,
                              direct_packets=4, direct_received=4, direct_bytes=4*15872,
                              relay_packets=2, relay_received=2, relay_bytes=2*15872, data_bytes=9*15872,
                              token_stride=15872, local_data_bytes=15872, same_server_data_bytes=15872,
                              cross_server_data_bytes=7*15872, performance=measure.performance_record(path.parents[1], 1, 0)))
        result = measure.summarize_measurements(ranks, config, "d" * 64)
        bytes_per_rank = 9 * 15872 + 90 * 32
        self.assertEqual(result["algorithm_communication_bytes"], bytes_per_rank * 16)
        self.assertAlmostEqual(result["bandwidth_gbps"]["minimum"], 8 * bytes_per_rank / 12500)
        self.assertAlmostEqual(result["bandwidth_gbps"]["maximum"], 8 * bytes_per_rank / 4500)
        self.assertIn("per-server GB/s", measure.render_measurements(result))
        for field, value in (("relay_received", 1), ("relay_bytes", 1), ("data_bytes", 0)):
            changed = copy.deepcopy(ranks)
            changed[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                measure.summarize_measurements(changed, config, "d" * 64)

    def test_invalid_output_keeps_complete_timing_without_trusting_traffic(self):
        config = dict(self.config, kernel_scope="dispatch")
        ranks = [self.performance_rank(rank) for rank in range(16)]
        ranks[0]["validation_status"] = "FAIL"
        summary = measure.summarize_measurements(ranks, config, "d" * 64)
        self.assertEqual(summary["validation_status"], "FAIL")
        self.assertEqual(summary["performance"]["server_mean_us"], [5, 5])
        self.assertIsNone(summary["algorithm_communication_bytes"])
        self.assertNotIn("rank_traffic", summary)
        self.assertNotIn("bandwidth_gbps", summary)
        self.assertIn("N/A（流量未通过完整校验）", measure.render_measurements(summary))
        with self.assertRaises(ValueError):
            measure.summarize_measurements(ranks[:1], config, "d" * 64)
        ranks[1]["performance"]["sample_us"] = []
        with self.assertRaises(ValueError):
            measure.summarize_measurements(ranks, config, "d" * 64)

    def test_source_send_performance_includes_direct_and_local_copy_bytes(self):
        config = dict(self.config, rank_size=16, bs=96, moe_expert_num=96, loops=1, warmup=0,
                      kernel_scope="source_send", route_mode="balance_shift", seed=2026, quant_mode=0, fp8_format="e4m3")
        ranks = []
        for rank in range(16):
            path = self.csv_profile([rank + 1], name=f"PROF_rank{rank}")
            ranks.append(dict(rank=rank, first_hop_packets=2, first_hop_received=2, first_hop_bytes=31744,
                              direct_packets=3, direct_received=3, direct_bytes=47616, token_stride=15872,
                              local_data_bytes=15872, same_server_data_bytes=15872, cross_server_data_bytes=47616,
                              performance=measure.performance_record(path.parents[1], 1, 0)))
        summary = measure.summarize_measurements(ranks, config, "a" * 64)
        self.assertEqual(summary["algorithm_communication_bytes"], 16 * 82240)
        self.assertAlmostEqual(summary["bandwidth_gbps"]["minimum"], 8 * 82.24 / 12.5)
        self.assertAlmostEqual(summary["bandwidth_gbps"]["maximum"], 8 * 82.24 / 4.5)
        self.assertAlmostEqual(summary["bandwidth_gbps"]["median"], (8 * 82.24 / 4.5 + 8 * 82.24 / 12.5) / 2)
        self.assertIn("per-server GB/s", measure.render_measurements(summary))
        self.assertNotIn("不含本卡访问", measure.render_measurements(summary))

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

    def test_summary_uses_server_sample_means_and_ignores_rank_order(self):
        ranks = [self.performance_rank(rank, (999, 1, 10, 11) if rank < 8 else (888, 4, 5, 100))
                 for rank in range(16)]
        summary = measure.summarize_measurements(list(reversed(ranks)), self.config, "a" * 64)
        self.assertEqual(summary["performance"]["server_mean_us"], [22 / 3, 109 / 3])
        self.assertAlmostEqual(summary["performance"]["mean_us"], 131 / 6)
        self.assertEqual(summary["performance"]["minimum_us"], 22 / 3)
        self.assertEqual(summary["performance"]["maximum_us"], 109 / 3)
        self.assertEqual(summary["performance"]["samples_per_server"], 24)
        self.assertNotIn("host_batch_us", summary)
        self.assertEqual(summary["algorithm_communication_bytes"], 0)
        self.assertEqual(summary["native_status"], "empty")
        report = measure.render_measurements(summary)
        self.assertIn("max=36.333 us min=7.333 us mean=21.833 us", report)
        self.assertNotIn("Host 测量批", report)
        self.assertIn("backend=memory", report)
        self.assertIn("带宽=N/A", report)
        self.assertIn("sha256=" + "a" * 64, report)

    def test_server_window_covers_short_exact_and_long_runs_without_warmup(self):
        for loops in (1, 49, 50, 51, 100):
            with self.subTest(loops=loops):
                selected = min(loops, 50)
                config = dict(self.config, loops=loops, warmup=2)
                ranks = []
                for rank in range(16):
                    durations = [10000, 20000] + [30000] * (loops - selected) + [rank + 1] * selected
                    durations[-1] += selected * 10  # Mean differs from median for long windows.
                    path = self.csv_profile(durations, name=f"PROF_window{loops}_rank{rank}", reverse=True)
                    ranks.append(dict(rank=rank, performance=measure.performance_record(path.parents[1], loops, 2)))
                result = measure.summarize_measurements(ranks, config, "c" * 64)["performance"]
                self.assertEqual(result["samples_per_rank"], selected)
                self.assertEqual(result["samples_per_server"], 8 * selected)
                self.assertEqual(result["server_mean_us"], [14.5, 22.5])
                self.assertEqual((result["maximum_us"], result["minimum_us"], result["mean_us"]), (22.5, 14.5, 18.5))
                self.assertEqual(len(ranks[0]["performance"]["sample_us"]), loops)
                # Even a discarded early measurement remains subject to evidence validation.
                ranks[0]["performance"]["sample_us"][0] = 0
                with self.assertRaises(ValueError):
                    measure.summarize_measurements(ranks, config, "c" * 64)

    def test_partial_server_is_not_a_performance_representative(self):
        ranks = [self.performance_rank(rank) for rank in range(15)]
        with self.assertRaisesRegex(ValueError, "8 个 rank"):
            measure.summarize_measurements(ranks, dict(self.config, rank_size=15), "a" * 64)

    def test_missing_duplicate_or_foreign_rank_is_rejected(self):
        ranks = [self.performance_rank(rank) for rank in range(16)]
        for invalid in (ranks[:1], [ranks[0], *ranks[:-1]], [*ranks[:-1], dict(ranks[-1], rank=16)]):
            with self.assertRaisesRegex(ValueError, "rank"):
                measure.summarize_measurements(invalid, self.config, "a" * 64)

    def test_count_scope_reports_remote_record_volume_and_server_bandwidth(self):
        config = dict(self.config, rank_size=16, bs=96, moe_expert_num=96,
                      kernel_scope="cnt_cumsum", route_mode="balance_shift", seed=2026)
        ranks = [self.performance_rank(rank, (999, 9, 10, 11)) for rank in range(16)]
        summary = measure.summarize_measurements(ranks, config, "b" * 64)
        self.assertEqual(summary["native_status"], "cnt_cumsum")
        self.assertEqual(summary["communication_bytes_per_rank"], 2880)
        self.assertEqual(summary["algorithm_communication_bytes"], 46080)
        self.assertAlmostEqual(summary["bandwidth_gbps"]["median"], 2.304)
        report = measure.render_measurements(summary)
        self.assertIn("native=cnt_cumsum", report)
        self.assertIn("46080B", report)
        self.assertIn("max=10.000 us min=10.000 us mean=10.000 us", report)

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

    def test_functional_has_no_measurements_or_report(self):
        config = dict(self.config, run_mode="functional", loops=1, warmup=0)
        summary = measure.summarize_measurements([{"rank": rank} for rank in range(16)], config, "a" * 64)
        self.assertNotIn("performance", summary)
        self.assertNotIn("breakdown", summary)
        self.assertNotIn("host_batch_us", summary)
        self.assertEqual(measure.render_measurements(summary), "")


if __name__ == "__main__":
    unittest.main()
