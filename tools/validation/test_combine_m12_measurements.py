"""Real M1/M2 event roles and byte accounting over standard capture readers."""
import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest

import measurements as measure
import operator_measurements as operators
from test_profiling_fixture import CAPTURE_ID, write_capture


class M12MeasurementTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="tilexr-m12-profile-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.config = dict(run_mode="breakdown", operator_mode="combine", rank_size=16, loops=5, warmup=2,
                           implemented_through="M2", scope="partial", combine_send_core_num=32,
                           bs=96, h=516, topk=6, moe_expert_num=96, dtype="bf16", block_dim="auto")
        self.row = dict(rank=0, combine_type3_rows=3, combine_algorithm_bytes=3096,
                        combine_stage_copy_bytes=6192, combine_control_publish_bytes=24576)
        self.sha = "c" * 64
        # Retained P2.1 records must use their historical mapping, never the evolving production M3 mapping.
        self.mapping = self.root / "m12.mapping.json"
        stages = (
            ("kernel_total", "total"), ("m1_init", "setup"),
            ("m1_select", "compute"), ("m1_stage_copy", "copy"),
            ("m1_prepare_publish", "issue"), ("m2_prepare_wait", "wait"),
            ("m2_notify", "issue"), ("m2_ready_wait", "wait"),
            ("m2_ready_clear", "copy"), ("m6_drain", "finalize"),
            ("p21_delay_50us", "wait"),
        )
        self.mapping.write_text(json.dumps(dict(
            schema="tilexr_profiling_mapping.v1", operator="tilexr_ep_combine_memory_server_dedup",
            cycles_per_us=1000, stages=[dict(name=name, category=category, critical=index == 0,
                begin=0x3001 + 2 * index, end=0x3002 + 2 * index)
                for index, (name, category) in enumerate(stages)])))

    def events(self):
        events = {}
        for core in range(64):
            pairs = list(range(0x3003, 0x300b, 2))
            if core >= 56:
                pairs += [0x300b, 0x300d]
            if core >= 32:
                pairs += [0x300f, 0x3011]
            pairs += [0x3013]
            rows = [(0, 0x3001)]
            for index, event in enumerate(pairs, 1):
                rows.extend(((index * 1000, event), (index * 1000 + 500, event + 1)))
            rows += [(20000, 0x3015), (70000, 0x3016), (71000, 0x3002)]
            events[core] = rows
        return events

    def capture(self, events=None, suffix="base"):
        path = write_capture(self.root / (suffix + ".records.v1"), launches=7, warmup=2,
                             events_by_core=events or self.events())
        return operators.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / suffix,
                                          operator="combine", run_id="run-test", kernel_sha=self.sha, record=self.row, mapping_path=self.mapping)

    def validate(self, evidence):
        row = dict(self.row, operator_measurements=dict(combine=dict(breakdown=evidence)))
        operators.validate_operator_measurements(row, self.config, run_id="run-test", kernel_shas=dict(combine=self.sha))

    def test_capture_has_real_modules_exact_core_roles_and_separate_wait(self):
        evidence = self.capture()
        self.validate(evidence)
        expected = operators.m12_stage_counts(self.config)
        self.assertEqual({name: item["sample_count"] for name, item in evidence["stages_us"].items()}, expected)
        self.assertEqual(evidence["sample_us"], [71.0])
        self.assertEqual(evidence["stages_us"]["p21_delay_50us"]["median"], 50.0)
        self.assertNotIn("empty_wait", evidence["stages_us"])
        self.assertEqual(evidence["native_status"], "combine-m12")
        self.assertEqual(evidence["algorithm_bytes"], 3096)
        self.assertAlmostEqual(evidence["algorithm_bandwidth_gbps"], 3096 / 71000)
        summary = json.loads(Path(evidence["summary_path"]).read_text())
        self.assertEqual(summary["implemented_through"], "M2")
        trace = json.loads(Path(evidence["artifacts"]["perfetto_trace.json"]).read_text())
        self.assertEqual(trace["metadata"]["attributes"]["scope"], "partial")
        self.assertEqual(trace["metadata"]["attributes"]["captured_launch"], 6)

    def test_missing_role_pair_wrong_core_duplicate_and_legacy_capture_fail(self):
        for fault in ("missing-pair", "wrong-core", "duplicate", "legacy", "missing-end"):
            events = self.events()
            if fault == "missing-pair":
                events[56] = [row for row in events[56] if row[1] not in (0x300b, 0x300c)]
            elif fault == "wrong-core":
                events[0], events[56] = events[56], events[0]
            elif fault == "duplicate":
                events[56].insert(2, (1100, 0x3003))
            elif fault == "missing-end":
                events[63].pop()
            else:
                events = {core: [(0, 0x3001), (50000, 0x3002)] for core in range(64)}
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                self.validate(self.capture(events, fault))

    def test_measurement_rejects_traffic_identity_or_stage_count_forgery(self):
        evidence = self.capture()
        for key, value in (("algorithm_bytes", 0), ("stage_copy_bytes", 0), ("control_publish_bytes", 0),
                           ("implemented_through", "M5"), ("scope", "full"), ("wait_us", 0)):
            broken = copy.deepcopy(evidence)
            broken[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(broken)
        broken = copy.deepcopy(evidence)
        broken["stages_us"]["m2_notify"]["sample_count"] = 64
        with self.assertRaises(ValueError):
            self.validate(broken)

    def test_msprof_excludes_warmup_and_reports_copy_rate_not_control_bandwidth(self):
        directory = self.root / "PROF_rank0"
        directory.mkdir()
        path = directory / "mindstudio_profiler_output/op_summary.csv"
        path.parent.mkdir()
        with path.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("Op Name", "Task Start Time(us)", "Task Duration(us)"))
            for index, duration in enumerate((300, 400, 51, 52, 53, 54, 55)):
                writer.writerow((measure.kernel_name("combine"), index * 1000, duration))
        config = dict(self.config, run_mode="performance")
        rows = []
        for rank in range(16):
            row = dict(self.row, rank=rank)
            evidence = operators.performance_record(directory, 5, 2, operator="combine", run_id="run-test",
                                                    rank=rank, kernel_sha=self.sha, config=config, record=row)
            self.assertEqual(evidence["sample_us"], [51, 52, 53, 54, 55])
            rows.append(dict(row, operator_measurements=dict(combine=dict(performance=evidence))))
        summary = operators.summarize_operator_measurements(rows, config, "run-test", dict(combine=self.sha))["combine"]
        self.assertEqual(summary["algorithm_bytes"], 3096 * 16)
        self.assertEqual(summary["control_publish_bytes"], 24576 * 16)
        self.assertEqual(summary["algorithm_communication_bytes"], 0)
        self.assertEqual(summary["native_status"], "combine-m12")
        self.assertEqual(summary["latency_us"]["median"], 53)
        self.assertIn("50us", operators.render_operator_measurements(dict(combine=summary)))
        empty = []
        for rank in range(16):
            row = dict(self.row, rank=rank, combine_type3_rows=0, combine_algorithm_bytes=0, combine_stage_copy_bytes=0)
            evidence = operators.performance_record(directory, 5, 2, operator="combine", run_id="run-test",
                                                    rank=rank, kernel_sha=self.sha, config=config, record=row)
            self.assertEqual(evidence["algorithm_bandwidth"], "N/A")
            empty.append(dict(row, operator_measurements=dict(combine=dict(performance=evidence))))
        summary = operators.summarize_operator_measurements(empty, config, "run-test", dict(combine=self.sha))["combine"]
        self.assertIsNone(summary["algorithm_bandwidth_gbps"])
        self.assertEqual(summary["algorithm_bandwidth"], "N/A")
        self.assertEqual(summary["rank_algorithm_bandwidth_gbps"], [None] * 16)
        self.assertIn("local_payload_GB/s=N/A", operators.render_operator_measurements(dict(combine=summary)))


if __name__ == "__main__":
    unittest.main()
