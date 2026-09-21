"""C6 operator identity, independent captures and empty-kernel measurement scope."""

import copy
import csv
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

import measurements as measure
import operator_measurements as operators
from test_profiling_fixture import CAPTURE_ID, write_capture


class OperatorMeasurementTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tilexr-operator-measurements-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.config = dict(run_mode="breakdown", operator_mode="both", rank_size=2, loops=3, warmup=2,
            bs=96, h=7168, topk=6, moe_expert_num=96, dtype="bf16", block_dim="auto",
            route_mode="balance_shift", seed=2026, quant_mode=0, dispatch_quant_mode=0,
            combine_quant_mode=0, fp8_format="e4m3")
        self.hashes = dict(dispatch="d" * 64, combine="c" * 64)

    def capture(self, rank, operator, *, fault="", events=None):
        if operator == "combine" and events is None:
            events = {core: [(0, 0x3001), (50000 + rank * 1000, 0x3002)] for core in range(64)}
        path = write_capture(self.root / f"rank-{rank}.{operator}.records.v1", rank=rank,
            events_by_core=events, fault=fault)
        directory = self.root / f"profile-rank-{rank}" / operator
        if directory.exists():
            shutil.rmtree(directory)
        return operators.breakdown_record(path, self.config, rank, CAPTURE_ID,
            directory, operator=operator,
            run_id="run-test", kernel_sha=self.hashes[operator])

    def records(self, mode="both"):
        return [dict(rank=rank, run_id="run-test", status="PASS", operator_measurements={
            operator: dict(breakdown=self.capture(rank, operator))
            for operator in operators.active_operators(dict(self.config, operator_mode=mode))})
            for rank in range(self.config["rank_size"])]

    def profile(self, rank=0, dispatch=(100, 2, 4, 6, 8), combine=(200, 51, 52, 53, 54)):
        directory = self.root / f"PROF_rank{rank}"
        path = directory / "mindstudio_profiler_output/op_summary.csv"
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("Op Name", "Task Start Time(us)", "Task Duration(us)"))
            for operator, samples in (("dispatch", dispatch), ("combine", combine)):
                for index, duration in enumerate(samples):
                    writer.writerow((measure.kernel_name(operator), index * 1000 + (operator == "combine") * 100,
                                     duration))
        return directory

    def performance(self, directory, rank, operator):
        return operators.performance_record(directory, 3, 2, operator=operator, run_id="run-test",
            rank=rank, kernel_sha=self.hashes[operator])

    def validate(self, row, config=None):
        operators.validate_operator_measurements(row, config or self.config,
            run_id="run-test", kernel_shas=self.hashes)

    def test_shared_msprof_input_filters_each_operator_and_excludes_own_warmup(self):
        directory = self.profile()
        dispatch = self.performance(directory, 0, "dispatch")
        combine = self.performance(directory, 0, "combine")
        self.assertEqual(dispatch["sample_us"], [4, 6, 8])
        self.assertEqual(combine["sample_us"], [52, 53, 54])
        self.assertEqual(dispatch["source"]["path"], combine["source"]["path"])
        row = dict(rank=0, operator_measurements=dict(dispatch=dict(performance=dispatch), combine=dict(performance=combine)))
        self.validate(row, dict(self.config, run_mode="performance"))
        for count in (0, 4, 6):
            directory = self.profile(combine=[50] * count)
            with self.subTest(count=count), self.assertRaises(ValueError):
                self.performance(directory, 0, "combine")
            # The unrelated operator remains independently readable.
            self.assertEqual(self.performance(directory, 0, "dispatch")["samples"], 3)

    def test_each_measurement_is_bound_to_operator_rank_run_kernel_and_final_launch(self):
        directory = self.profile()
        row = dict(rank=0, operator_measurements={operator: dict(performance=self.performance(directory, 0, operator))
                                                for operator in ("dispatch", "combine")})
        config = dict(self.config, run_mode="performance")
        mutations = dict(operator="dispatch", rank=1, run_id="other-run", kernel=measure.KERNEL_NAME,
                         kernel_sha256="d" * 64, last_launch=3, algorithm_bytes=1,
                         algorithm_bandwidth_gbps=0, algorithm_bandwidth="0 GB/s", scope="algorithm")
        for key, value in mutations.items():
            changed = copy.deepcopy(row)
            changed["operator_measurements"]["combine"]["performance"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(changed, config)
        for missing in ("dispatch", "combine"):
            changed = copy.deepcopy(row)
            del changed["operator_measurements"][missing]
            with self.assertRaises(ValueError):
                self.validate(changed, config)

    def test_breakdown_has_distinct_files_trace_identity_and_two_combine_intervals(self):
        row = self.records()[0]
        self.validate(row)
        paths = []
        for operator, entry in row["operator_measurements"].items():
            evidence = entry["breakdown"]
            paths.append(evidence["artifacts"]["perfetto_trace.json"])
            trace = json.loads(Path(paths[-1]).read_text())
            metadata = trace["metadata"]
            self.assertEqual(metadata["operator"], measure.kernel_name(operator).removesuffix("_kernel"))
            attributes = metadata["attributes"]
            self.assertEqual(attributes["run_id"], "run-test")
            self.assertEqual(attributes["kernel_sha256"], self.hashes[operator])
            self.assertEqual(attributes["operator"], operator)
            self.assertEqual(attributes["captured_launch"], 4)
            if operator == "combine":
                self.assertEqual(set(evidence["stages_us"]), {"kernel_total", "empty_wait"})
                self.assertEqual(evidence["sample_us"], [50])
                events = [event for event in trace["traceEvents"] if event["ph"] == "X"]
                self.assertEqual(len(events), 128)
                self.assertEqual({event["args"]["rank"] for event in events}, {0})
        self.assertNotEqual(*paths)
        changed = copy.deepcopy(row)
        changed["operator_measurements"]["combine"]["breakdown"]["records_path"] = (
            changed["operator_measurements"]["dispatch"]["breakdown"]["records_path"])
        with self.assertRaisesRegex(ValueError, "separate"):
            self.validate(changed)

    def test_missing_duplicate_and_overflow_events_cannot_be_complete(self):
        for fault in ("missing-end", "duplicate-start", "overflow", "accumulating"):
            events = {core: [(0, 0x3001), (50000, 0x3002)] for core in range(64)}
            if fault == "missing-end":
                events[63].pop()
            if fault == "duplicate-start":
                events[63].insert(1, (1, 0x3001))
            evidence = self.capture(0, "combine", fault=fault, events=events)
            row = dict(rank=0, operator_measurements=dict(combine=dict(breakdown=evidence)))
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                self.validate(row, dict(self.config, operator_mode="combine"))
        path = write_capture(self.root / "wrong.records.v1")
        with self.assertRaisesRegex(ValueError, "mapping operator"):
            operators.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "wrong",
                operator="combine", run_id="run-test", kernel_sha=self.hashes["combine"],
                mapping_path=measure.profiling_mapping())

    def test_collection_preserves_other_operator_when_capture_is_missing(self):
        records = self.records()
        del records[1]["operator_measurements"]["combine"]
        # Collection only verifies and copies finished artifacts, never raw data.
        with mock.patch.object(measure, "load_profile_run", side_effect=AssertionError("raw parse")):
            result = operators.collect_operator_breakdown_reports(records, self.config, self.root / "traces",
                "run-test", self.hashes)
        self.assertEqual(result["dispatch"]["status"], "complete")
        self.assertEqual(result["combine"]["status"], "incomplete")
        for operator, index in result.items():
            self.assertEqual(index["operator"], operator)
            self.assertEqual(index["kernel_sha256"], self.hashes[operator])
            self.assertEqual(Path(index["index"]).parent.name, operator)
            self.assertEqual(json.loads(Path(index["index"]).read_text()), index)
        self.assertNotEqual(result["dispatch"]["ranks"]["0"], result["combine"]["ranks"]["0"])

    def test_single_operator_does_not_parse_or_collect_inactive_region(self):
        records = self.records("combine")
        for row in records:
            self.validate(row, dict(self.config, operator_mode="combine"))
            row["operator_measurements"]["dispatch"] = object()
        with mock.patch.object(measure, "load_profile_run", side_effect=AssertionError("raw parse")):
            result = operators.collect_operator_breakdown_reports(records, dict(self.config, operator_mode="combine"),
                self.root / "traces", "run-test", self.hashes)
        self.assertEqual(set(result), {"combine"})
        self.assertEqual(result["combine"]["status"], "complete")
        self.assertFalse((self.root / "traces/dispatch").exists())

    def test_relocation_and_artifact_identity_prevent_cross_operator_reuse(self):
        records = self.records()
        original = records[0]["operator_measurements"]["combine"]["breakdown"]
        destination = self.root / "collected-node"
        shutil.copytree(self.root / "profile-rank-0", destination / "profile-rank-0")
        relocated = operators.relocate_operator_breakdown_report(original, destination, 0, "producer",
            operator="combine", local_records=True)
        self.assertEqual(Path(relocated["local_records_path"]).name, "rank-0.combine.records.v1")
        measure.validate_report_artifacts(relocated, 0, self.config)
        relocated["operator"] = "dispatch"
        with self.assertRaisesRegex(ValueError, "disagrees"):
            measure.validate_report_artifacts(relocated, 0, self.config)
        records[0]["operator_measurements"]["combine"]["breakdown"] = records[0]["operator_measurements"]["dispatch"]["breakdown"]
        result = operators.collect_operator_breakdown_reports(records, self.config, self.root / "traces", "run-test", self.hashes)
        self.assertEqual(result["dispatch"]["status"], "complete")
        self.assertEqual(result["combine"]["status"], "incomplete")

    def test_combine_summary_has_zero_bytes_no_bandwidth_and_across_rank_latency(self):
        config = dict(self.config, operator_mode="combine", rank_size=16, run_mode="performance")
        records = []
        for rank in range(16):
            directory = self.profile(rank, combine=[500, 500, 50 + rank, 52 + rank, 54 + rank])
            records.append(dict(rank=rank, run_id="run-test", operator_measurements=dict(
                combine=dict(performance=self.performance(directory, rank, "combine")))))
        summary = operators.summarize_operator_measurements(records, config, "run-test", self.hashes)
        combine = summary["combine"]
        self.assertEqual(combine["latency_us"], dict(median=59.5, minimum=52, maximum=67, mean=59.5))
        self.assertEqual(combine["algorithm_bytes"], 0)
        self.assertEqual(combine["algorithm_communication_bytes"], 0)
        self.assertIsNone(combine["algorithm_bandwidth_gbps"])
        self.assertNotIn("bandwidth_gbps", combine)
        report = operators.render_operator_measurements(summary)
        for text in ("empty/framework", "N/A", "min=52.000 us median=59.500 us max=67.000 us", "c" * 64):
            self.assertIn(text, report)
        for invalid in (records[:-1], records[:-1] + [records[0]]):
            with self.assertRaises(ValueError):
                operators.summarize_operator_measurements(invalid, config, "run-test", self.hashes)

    def test_joint_summary_uses_dispatch_traffic_and_distinct_operator_latency(self):
        config = dict(self.config, rank_size=16, run_mode="performance")
        records = []
        for rank in range(16):
            directory = self.profile(rank)
            dispatch = dict(rank=rank, first_hop_packets=3, first_hop_received=3, first_hop_bytes=3 * 15872,
                direct_packets=4, direct_received=4, direct_bytes=4 * 15872,
                relay_packets=2, relay_received=2, relay_bytes=2 * 15872, data_bytes=9 * 15872,
                token_stride=15872, local_data_bytes=15872, same_server_data_bytes=15872,
                cross_server_data_bytes=7 * 15872)
            records.append(dict(rank=rank, run_id="run-test", dispatch=dispatch, operator_measurements={
                operator: dict(performance=self.performance(directory, rank, operator))
                for operator in ("dispatch", "combine")}))
        summary = operators.summarize_operator_measurements(records, config, "run-test", self.hashes)
        self.assertEqual(summary["dispatch"]["algorithm_communication_bytes"], (9 * 15872 + 90 * 32) * 16)
        self.assertEqual(summary["combine"]["algorithm_communication_bytes"], 0)
        self.assertEqual(summary["dispatch"]["latency_us"]["median"], 6)
        self.assertEqual(summary["combine"]["latency_us"]["median"], 53)
        self.assertIn("bandwidth_gbps", summary["dispatch"])
        self.assertNotIn("bandwidth_gbps", summary["combine"])
        records[0]["dispatch"]["relay_received"] = 0
        with self.assertRaises(ValueError):
            operators.summarize_operator_measurements(records, config, "run-test", self.hashes)

    def test_combine_breakdown_summary_keeps_last_launch_and_clock_units(self):
        records = self.records("combine")
        config = dict(self.config, operator_mode="combine")
        summary = operators.summarize_operator_measurements(records, config, "run-test", self.hashes)["combine"]
        self.assertEqual(summary["last_launch"], 4)
        self.assertEqual(summary["latency_us"]["median"], 50.5)
        self.assertEqual(summary["breakdown"]["ticks_per_us"], 1000)
        self.assertEqual(set(summary["breakdown"]["stages_us"]), {"kernel_total", "empty_wait"})
        self.assertEqual(summary["algorithm_bytes"], 0)

    def test_functional_has_no_measurements(self):
        config = dict(self.config, run_mode="functional")
        self.assertEqual(operators.summarize_operator_measurements([dict(rank=0)], config, "run-test", self.hashes), {})
        self.assertEqual(operators.render_operator_measurements({}), "")
        with self.assertRaises(ValueError):
            self.validate(dict(rank=0, operator_measurements={"combine": {}}), config)


if __name__ == "__main__":
    unittest.main()
