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

    def test_source_send_v3_stages_traffic_and_trace_export(self):
        config = dict(self.config, run_mode="breakdown", rank_size=16, loops=1, warmup=0,
                      bs=96, h=7168, moe_expert_num=96, quant_mode=0, fp8_format="e4m3",
                      route_mode="balance_shift", seed=2026, kernel_scope="source_send")
        ranks = []
        for rank in range(16):
            cores = []
            for core in range(64):
                time = [100, 110, 120, 160, 160, 160, 160, 180] if core < 48 else [100, 110, 120, 120, 140, 150, 160, 190]
                cores.append(dict(core=core, **dict(zip(measure.SOURCE_SEND_POINTS, time))))
            ranks.append(dict(rank=rank, first_hop_packets=2 + rank, first_hop_received=2 + rank,
                              first_hop_bytes=(2 + rank) * 15872, token_stride=15872,
                              direct_packets=3, direct_received=3, direct_bytes=3 * 15872,
                              local_data_bytes=15872, same_server_data_bytes=15872, cross_server_data_bytes=(3 + rank) * 15872,
                              breakdown=dict(schema_version=3, kernel_scope="source_send", generation=1,
                                             run_mode="breakdown", loops=1, warmup=0, block_dim=64, records=cores)))
        summary = measure.summarize_measurements(ranks, config, "a" * 64)
        self.assertEqual(summary["native_status"], "source_send")
        self.assertEqual(summary["measurement"], "single_cold_submission")
        self.assertEqual(summary["algorithm_communication_bytes"], (sum(range(2, 18)) + 16 * 3) * 15872 + 16 * 90 * 32)
        self.assertEqual(summary["rank_traffic"][0]["direct_packets"], 3)
        self.assertEqual(summary["rank_traffic"][0]["local_data_bytes"], 15872)
        self.assertEqual(summary["rank_traffic"][0]["same_server_data_bytes"], 15872)
        self.assertEqual(summary["rank_traffic"][0]["cross_server_data_bytes"], 3 * 15872)
        self.assertEqual(summary["breakdown"]["schema_version"], 3)
        self.assertEqual(summary["breakdown"]["stages"]["send_data"]["median"], 40)
        self.assertEqual(summary["breakdown"]["stages"]["source_send_total"]["median"], 90)
        traces = measure.export_breakdown_traces(ranks, config, self.root / "traces", "run", "a" * 64)
        data = json.loads(Path(traces["all_ranks"]).read_text())
        self.assertEqual(data["otherData"]["schema_version"], 3)
        self.assertIn("send_data", {item["name"] for item in data["traceEvents"]})
        for key, value in (("direct_received", 2), ("direct_bytes", 0), ("same_server_data_bytes", -1),
                           ("local_data_bytes", 1), ("cross_server_data_bytes", 0)):
            bad = copy.deepcopy(ranks); bad[0][key] = value
            with self.subTest(traffic=key), self.assertRaises(ValueError):
                measure.summarize_measurements(bad, config, "a" * 64)
        for field, value in (("schema_version", 2), ("kernel_scope", "cnt_cumsum"), ("generation", 2)):
            bad = copy.deepcopy(ranks[0]); bad["breakdown"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                measure.validate_rank_measurements(bad, config)

    def detailed_fixture(self):
        config = dict(self.config, run_mode="breakdown", rank_size=16, loops=1, warmup=0,
                      bs=4, topk=6, h=7168, moe_expert_num=96, quant_mode=0, fp8_format="e4m3",
                      route_mode="balance_shift", seed=2026, kernel_scope="source_send")
        cores = []
        for core in range(64):
            time = [100, 110, 120, 160, 160, 160, 160, 180] if core < 48 else [100, 110, 120, 120, 140, 150, 160, 190]
            row = dict(core=core, **dict(zip(measure.SOURCE_SEND_POINTS, time)))
            if core < 48:
                issue = ([125, 135, 150], [130], [125, 130], [140])[core] if core < 4 else []
                row.update(first_hop_count=(2, 1, 0, 0)[core] if core < 4 else 0,
                           send_packets=[dict(issue=tick, token=core, k=index*2) for index, tick in enumerate(issue)])
            cores.append(row)
        ranks = [dict(rank=rank, first_hop_packets=3, first_hop_received=3, first_hop_bytes=3*15872,
                      direct_packets=4, direct_received=4, direct_bytes=4*15872, token_stride=15872,
                      local_data_bytes=15872, same_server_data_bytes=15872, cross_server_data_bytes=5*15872,
                      breakdown=dict(schema_version=4, kernel_scope="source_send", generation=1,
                                     run_mode="breakdown", loops=1, warmup=0, block_dim=64, records=copy.deepcopy(cores)))
                 for rank in range(16)]
        return config, ranks

    def test_send_detail_phases_partition_send_data_and_preserve_issue_intervals(self):
        config, ranks = self.detailed_fixture()
        result = measure.summarize_measurements(ranks, config, "a" * 64)
        self.assertEqual(result["breakdown"]["schema_version"], 4)
        self.assertEqual(result["breakdown"]["stages_us"]["send_prepare"]["median"], 0.040)
        self.assertEqual(result["breakdown"]["stages"]["send_first_hop"]["maximum"], 30)
        self.assertEqual(result["breakdown"]["stages"]["send_direct"]["minimum"], 35)
        for core in ranks[0]["breakdown"]["records"][:48]:
            self.assertEqual(sum(end - begin for _, begin, end in measure._send_detail_spans(core)), 40)
        traces = measure.export_breakdown_traces(ranks, config, self.root / "detail", "detail-run", "a" * 64)
        trace = json.loads(Path(traces["ranks"]["0"]).read_text())
        self.assertEqual(trace["otherData"]["schema_version"], 4)
        events = trace["traceEvents"]
        phases = {e["name"]: e["dur"] for e in events if e.get("cat") == "send_phase" and e["tid"] == 0}
        self.assertEqual(phases, dict(send_prepare=0.005, send_first_hop=0.025, send_direct=0.010))
        # Floating ts are only for the viewer; exact ticks are retained in args.
        intervals = [e for e in events if e.get("cat") == "send_issue_interval" and e["tid"] == 0]
        self.assertEqual([(e["args"]["start_ticks"], e["args"]["end_ticks"]) for e in intervals], [(125, 135), (135, 150)])
        self.assertEqual([e["args"]["kind"] for e in intervals], ["first_hop", "first_hop"])
        self.assertEqual(intervals[-1]["args"]["next_k"], 4)
        points = [e for e in events if e.get("cat") == "mte3_issue" and e["tid"] == 0]
        self.assertEqual([e["args"]["issue_ticks"] for e in points], [125, 135, 150])
        tail = next(e for e in events if e.get("cat") == "send_cleanup" and e["tid"] == 0)
        self.assertEqual(tail["name"], "send_drain_and_outputs")
        self.assertEqual(tail["args"]["start_ticks"], 150)
        self.assertEqual(tail["dur"], 0.010)
        for core, missing in ((1, "send_direct"), (2, "send_first_hop"), (4, "send_first_hop")):
            self.assertEqual(next(e["dur"] for e in events if e.get("cat") == "send_phase" and e["tid"] == core and e["name"] == missing), 0)
        self.assertFalse(any(e.get("cat") in ("mte3_issue", "send_issue_interval", "send_cleanup") and e["tid"] == 4 for e in events))

    def dispatch_fixture(self):
        config, ranks = self.detailed_fixture()
        config["kernel_scope"] = "dispatch"
        for row in ranks:
            row.update(relay_packets=2, relay_received=2, relay_bytes=2*15872, data_bytes=9*15872)
            row["same_server_data_bytes"] += row["relay_bytes"]
            row["breakdown"].update(schema_version=5, kernel_scope="dispatch")
            for core in row["breakdown"]["records"]:
                core["recv_data_end"] = core["prefix_end"] if core["core"] < 48 else core["end"] - 5
        return config, ranks

    def test_dispatch_v5_separates_receive_and_finish_and_retains_source_issues(self):
        config, ranks = self.dispatch_fixture()
        result = measure.summarize_measurements(ranks, config, "d" * 64)
        self.assertEqual(result["native_status"], "dispatch")
        self.assertEqual(result["algorithm_communication_bytes"], 16 * (9 * 15872 + 90 * 32))
        self.assertEqual(result["rank_traffic"][0]["relay_bytes"], 2 * 15872)
        self.assertEqual(result["rank_traffic"][0]["data_bytes"], 9 * 15872)
        self.assertEqual(result["breakdown"]["schema_version"], 5)
        stages = result["breakdown"]["stages"]
        self.assertEqual(stages["recv_data"]["median"], 25)
        self.assertEqual(stages["dispatch_total"]["maximum"], 90)
        self.assertEqual(stages["finish"]["median"], 20)
        self.assertNotIn("source_send_total", stages)
        index = measure.export_breakdown_traces(ranks, config, self.root / "dispatch-traces", "run", "d" * 64)
        trace = json.loads(Path(index["ranks"]["0"]).read_text())
        self.assertEqual(trace["otherData"]["schema_version"], 5)
        events = trace["traceEvents"]
        phases = [event for event in events if event.get("cat") == "kernel"]
        self.assertEqual(len(phases), 64 * 8)
        receive = next(event for event in phases if event["name"] == "recv_data" and event["tid"] == 63)
        self.assertEqual((receive["args"]["start_ticks"], receive["args"]["end_ticks"]), (160, 185))
        self.assertEqual(len([event for event in events if event.get("cat") == "mte3_issue"]), 7)
        self.assertEqual([event["tid"] for event in events if event["name"] == "thread_name"], list(range(64)))

    def test_dispatch_rejects_old_profile_missing_receive_or_incomplete_relay_traffic(self):
        config, ranks = self.dispatch_fixture()
        for fault in ("v4", "scope", "missing-receive", "early-receive", "late-receive", "detail", "relay-bytes", "relay-count", "total", "classification"):
            changed = copy.deepcopy(ranks)
            row = changed[0]
            profile = row["breakdown"]
            if fault == "v4": profile["schema_version"] = 4
            if fault == "scope": profile["kernel_scope"] = "source_send"
            if fault == "missing-receive": profile["records"][63].pop("recv_data_end")
            if fault == "early-receive": profile["records"][63]["recv_data_end"] = 159
            if fault == "late-receive": profile["records"][63]["recv_data_end"] = 191
            if fault == "detail": profile["records"][0].pop("send_packets")
            if fault == "relay-bytes": row["relay_bytes"] -= 1
            if fault == "relay-count": row["relay_received"] += 1
            if fault == "total": row["data_bytes"] -= row["relay_bytes"]
            if fault == "classification": row["same_server_data_bytes"] -= row["relay_bytes"]
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                measure.summarize_measurements(changed, config, "d" * 64)
        with self.assertRaisesRegex(ValueError, "scope"):
            measure.validate_rank_measurements(ranks[0], dict(config, kernel_scope="source_send"))

    def test_dispatch_performance_bandwidth_counts_relay_over_full_kernel(self):
        config, ranks = self.dispatch_fixture()
        config["run_mode"] = "performance"
        for rank, row in enumerate(ranks):
            row.pop("breakdown")
            path = self.csv_profile([rank + 1], name=f"PROF_dispatch{rank}")
            row["performance"] = measure.performance_record(path.parents[1], 1, 0)
        result = measure.summarize_measurements(ranks, config, "d" * 64)
        bytes_per_rank = 9 * 15872 + 90 * 32
        self.assertEqual(result["algorithm_communication_bytes"], bytes_per_rank * 16)
        self.assertAlmostEqual(result["bandwidth_gbps"]["minimum"], bytes_per_rank / 16000)
        self.assertAlmostEqual(result["bandwidth_gbps"]["maximum"], bytes_per_rank / 1000)
        self.assertIn("Dispatch with relay", measure.render_measurements(result))

    def test_send_detail_rejects_missing_overflow_stale_or_misidentified_packets(self):
        config, ranks = self.detailed_fixture()
        for fault in ("missing", "not-list", "count", "count-type", "capacity", "empty", "unordered", "before", "after",
                      "token", "k", "duplicate", "receiver", "traffic", "missing-core"):
            row = copy.deepcopy(ranks[0])
            core = row["breakdown"]["records"][0]
            packet = core["send_packets"][0]
            if fault == "missing": core.pop("send_packets")
            if fault == "not-list": core["send_packets"] = "bad"
            if fault == "count": core["first_hop_count"] = 4
            if fault == "count-type": core["first_hop_count"] = True
            if fault == "capacity": core["send_packets"] *= 3
            if fault == "empty": core["send_packets"] = []; core["first_hop_count"] = 0
            if fault == "unordered": core["send_packets"][1]["issue"] = 124
            if fault == "before": packet["issue"] = 119
            if fault == "after": packet["issue"] = 161
            if fault == "token": packet["token"] = 1
            if fault == "k": packet["k"] = 6
            if fault == "duplicate": core["send_packets"][1]["k"] = 0
            if fault == "receiver": row["breakdown"]["records"][48]["send_packets"] = [packet]
            if fault == "traffic": row["first_hop_packets"] += 1
            if fault == "missing-core": row["breakdown"]["records"].pop()
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                measure.validate_rank_measurements(row, config)
        ranks[1]["breakdown"]["schema_version"] = 3
        with self.assertRaisesRegex(ValueError, "版本不一致"):
            measure.summarize_measurements(ranks, config, "a" * 64)

    def test_send_detail_accepts_equal_ticks_without_losing_issue_points(self):
        config, ranks = self.detailed_fixture()
        ranks[0]["breakdown"]["records"][0]["send_packets"][1]["issue"] = 125
        measure.validate_rank_measurements(ranks[0], config)
        events = measure._send_detail_events(ranks[0]["breakdown"]["records"][0], 0, 100, 1)
        self.assertEqual(len([e for e in events if e["ph"] == "i"]), 3)
        self.assertEqual(next(e["dur"] for e in events if e["cat"] == "send_issue_interval"), 0)

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
        self.assertAlmostEqual(summary["bandwidth_gbps"]["minimum"], 82.24 / 16)
        self.assertAlmostEqual(summary["bandwidth_gbps"]["maximum"], 82.24)
        self.assertAlmostEqual(summary["bandwidth_gbps"]["median"], (82.24 / 8 + 82.24 / 9) / 2)
        self.assertIn("including local copies", measure.render_measurements(summary))
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

    def test_count_scope_reports_remote_record_volume_and_rank_bandwidth(self):
        config = dict(self.config, rank_size=16, bs=96, moe_expert_num=96,
                      kernel_scope="cnt_cumsum", route_mode="balance_shift", seed=2026)
        ranks = [self.performance_rank(rank, (999, 9, 10, 11)) for rank in range(16)]
        summary = measure.summarize_measurements(ranks, config, "b" * 64)
        self.assertEqual(summary["native_status"], "cnt_cumsum")
        self.assertEqual(summary["communication_bytes_per_rank"], 2880)
        self.assertEqual(summary["algorithm_communication_bytes"], 46080)
        self.assertAlmostEqual(summary["bandwidth_gbps"]["median"], 0.288)
        report = measure.render_measurements(summary)
        self.assertIn("native=cnt_cumsum", report)
        self.assertIn("46080B", report)
        self.assertIn("median=10.000 us min=10.000 us max=10.000 us", report)

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

    def test_count_breakdown_exports_real_stages_and_rejects_legacy_or_partial_evidence(self):
        config = dict(self.config, run_mode="breakdown", rank_size=16, moe_expert_num=96, kernel_scope="cnt_cumsum")
        ranks = []
        for rank in range(16):
            rows = [dict(core=core, **dict(zip(measure.COUNT_POINTS,
                    (100, 110, 140, 180, 230, 290, 300 + rank)))) for core in range(64)]
            ranks.append(dict(rank=rank, breakdown=dict(run_mode="breakdown", loops=3, warmup=1,
                schema_version=2, kernel_scope="cnt_cumsum", generation=4, block_dim=64, records=rows)))
        summary = measure.summarize_measurements(ranks, config, "c" * 64)
        self.assertEqual(summary["breakdown"]["stages"]["send_cnt"]["median"], 30)
        self.assertEqual(summary["breakdown"]["stages"]["count_total"]["maximum"], 215)
        self.assertEqual(summary["algorithm_communication_bytes"], 46080)
        self.assertNotIn("empty_total", summary["breakdown"]["stages"])
        index = measure.export_breakdown_traces(ranks, config, self.root / "traces", "run", "c" * 64)
        trace = json.loads(Path(index["all_ranks"]).read_text())
        self.assertEqual(trace["otherData"]["native_status"], "cnt_cumsum")
        events = [e for e in trace["traceEvents"] if e["ph"] == "X"]
        self.assertEqual(len(events), 16 * 64 * 6)
        self.assertEqual({e["name"] for e in events}, set(measure.COUNT_STAGES[:-1]))
        for fault in ("legacy", "missing", "unordered", "foreign"):
            broken = copy.deepcopy(ranks[0])
            p = broken["breakdown"]
            if fault == "legacy": p.pop("schema_version")
            if fault == "missing": p["records"].pop()
            if fault == "unordered": p["records"][0]["recv_cnt_end"] = 1
            if fault == "foreign": p["records"][0]["core"] = 64
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                measure.validate_rank_measurements(broken, config)

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
