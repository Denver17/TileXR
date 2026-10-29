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
from current_run_fixture import combine_traffic, combine_events
from test_profiling_fixture import CAPTURE_ID, combine_loop_events, write_capture


class OperatorMeasurementTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tilexr-operator-measurements-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.config = dict(run_mode="breakdown", operator_mode="both", rank_size=2, loops=3, warmup=2,
            bs=96, h=7168, topk=6, moe_expert_num=96, dtype="bf16", block_dim="auto",
            route_mode="balance_shift", seed=2026, quant_mode=0, dispatch_quant_mode=0,
            combine_quant_mode=0, fp8_format="e4m3", implemented_through="M5", scope="full")
        self.hashes = dict(dispatch="d" * 64, combine="c" * 64)

    def capture(self, rank, operator, *, fault="", events=None):
        if operator == "combine" and events is None:
            events = combine_events()
        path = write_capture(self.root / f"rank-{rank}.{operator}.records.v1", rank=rank,
            events_by_core=events, fault=fault)
        directory = self.root / f"profile-rank-{rank}" / operator
        if directory.exists():
            shutil.rmtree(directory)
        return operators.breakdown_record(path, self.config, rank, CAPTURE_ID,
            directory, operator=operator,
            run_id="run-test", kernel_sha=self.hashes[operator], record=combine_traffic(self.config, rank))

    def test_m6_capture_and_traffic_require_only_direct_returns(self):
        self.config.update(implemented_through="M5", scope="full", server_dedup_enabled=False)
        events = {}
        for core in range(64):
            values = [(0, 0x3001), (1, 0x3003), (2, 0x3004)]
            if core < 32:
                values += [(3, 0x301b), (10, 0x3021), (11, 0x301c)]
            else:
                values += [(3, 0x302a)]
                for token in range(3):
                    values += [(4 + 2 * token, 0x302c), (5 + 2 * token, 0x302d)]
                values += [(11, 0x302b)]
            events[core] = values + [(12, 0x3013), (13, 0x3014), (14, 0x3002)]
        record = dict(rank=0, combine_type3_rows=0, combine_type1_rows=10, combine_type2_rows=0,
            combine_token_num=10, combine_direct_received_rows=576, combine_partial_received_rows=0,
            combine_partial_member_rows=0, combine_partial_local_members=0, combine_partial_stage_read_bytes=0,
            combine_partial_self_read_bytes=0, combine_partial_payload_bytes=0, combine_partial_wire_bytes=0,
            combine_algorithm_bytes=106*14336, combine_stage_copy_bytes=0, combine_direct_payload_bytes=10*14336,
            combine_direct_wire_bytes=10*15360, combine_control_publish_bytes=0,
            combine_final_read_bytes=576*14336, combine_y_out_bytes=96*14336, combine_flag_clear_bytes=576*30*32)
        path = write_capture(self.root / "m6.records.v1", events_by_core=events)
        evidence = operators.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "m6",
            operator="combine", run_id="run-test", kernel_sha=self.hashes["combine"], record=record)
        self.assertEqual(evidence["receive_module"], "M6")
        self.assertIn("m6_final", evidence["stages_us"])
        self.assertNotIn("m4_partial", evidence["stages_us"])
        self.assertNotIn("m5_final", evidence["stages_us"])
        self.assertEqual(operators.return_stage_counts(self.config)["m6_final"], 32)
        self.config["rank_size"] = 16
        path = write_capture(self.root / "m6-loops.records.v1", events_by_core=self.loop_events())
        detailed = operators.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "m6-loops",
            operator="combine", run_id="run-test", kernel_sha=self.hashes["combine"], record=record)
        operators._validate_evidence(detailed, self.config, "combine", 0, "run-test", self.hashes["combine"], record)
        bad_count = copy.deepcopy(detailed)
        bad_count["stages_us"]["m3_loop_read"]["sample_count"] -= 1
        with self.assertRaisesRegex(ValueError, "required module/core-role"):
            operators._validate_evidence(bad_count, self.config, "combine", 0, "run-test", self.hashes["combine"], record)
        bad = dict(record, combine_direct_received_rows=575)
        with self.assertRaisesRegex(ValueError, "M6 requires"):
            operators.return_traffic(bad, self.config)
        bad = dict(record, combine_control_publish_bytes=32)
        with self.assertRaisesRegex(ValueError, "control_publish_bytes"):
            operators.return_traffic(bad, self.config)

    def loop_events(self, rows=10):
        return combine_loop_events(self.config["bs"], self.config["topk"], rows=rows)

    def test_nondedup_loop_detail_and_per_core_totals(self):
        self.config.update(implemented_through="M5", scope="full", server_dedup_enabled=False, rank_size=16)
        path = write_capture(self.root / "loops.records.v1", events_by_core=self.loop_events())
        evidence = measure.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "loops",
                                           operator="combine")
        self.assertEqual(evidence["status"], "complete")
        self.assertEqual(evidence["stages_us"]["m6_loop_input_wait"]["sample_count"], 576)
        self.assertEqual(evidence["stages_us"]["m6_loop_input_wait"]["median"], 1)
        self.assertEqual(evidence["core_totals_us"]["m6_loop_input_wait"]["maximum"], 18)
        self.assertEqual(len(evidence["core_totals_us"]["m6_loop_input_wait"]["cores"]), 32)
        summary = measure.summarize_measurements([
            dict(rank=rank, server_dedup_enabled=False, breakdown=evidence) for rank in range(16)],
            self.config, self.hashes["combine"], operator="combine")
        self.assertEqual(summary["breakdown"]["core_totals_us"]["m6_loop_input_wait"]["median"], 18)
        self.assertIn("每核累计最大值（跨 rank） m6_loop_input_wait", measure.render_measurements(summary))
        run = measure.combine_breakdown_timeline(measure.load_profile_run(
            measure.profiling_mapping(operator="combine"), [path]), self.config)
        lane = next(lane for lane in run.launches[0].lanes if lane.core == 32)
        loops = [s for s in lane.samples if s.name == "m6_loop_compute_prefetch"]
        self.assertEqual([s.instance_id for s in loops], list(range(18)))
        self.assertEqual(loops[7].attributes["token_sequence"], 1)
        self.assertEqual(loops[7].attributes["topk_iteration"], 1)
        # Empty send tasks and receive cores must retain preparation/retirement
        # but must not invent any loop samples or readiness waits.
        self.config["bs"] = 3
        path = write_capture(self.root / "empty.records.v1", events_by_core=self.loop_events(rows=0))
        empty = measure.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "empty", operator="combine")
        self.assertEqual(empty["stages_us"]["m6_loop_input_wait"]["sample_count"], 18)
        self.assertNotIn("m3_loop_read", empty["stages_us"])
        # Losing an entire pair is syntactically valid; role/count checks must catch it.
        events = self.loop_events()
        events[32] = [item for item in events[32] if item[1] not in (0x303e, 0x303f)]
        path = write_capture(self.root / "missing.records.v1", events_by_core=events)
        with self.assertRaisesRegex(ValueError, "loop/core role coverage"):
            measure.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "missing", operator="combine")

    def records(self, mode="both"):
        return [dict(combine_traffic(self.config, rank), run_id="run-test", status="PASS", operator_measurements={
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
            rank=rank, kernel_sha=self.hashes[operator], config=dict(self.config, run_mode="performance"), record=combine_traffic(self.config, rank))

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
        row = dict(combine_traffic(self.config), operator_measurements=dict(dispatch=dict(performance=dispatch), combine=dict(performance=combine)))
        self.validate(row, dict(self.config, run_mode="performance"))
        for count in (0, 4, 6):
            directory = self.profile(combine=[50] * count)
            with self.subTest(count=count), self.assertRaises(ValueError):
                self.performance(directory, 0, "combine")
            # The unrelated operator remains independently readable.
            self.assertEqual(self.performance(directory, 0, "dispatch")["samples"], 3)

    def test_each_measurement_is_bound_to_operator_rank_run_kernel_and_final_launch(self):
        directory = self.profile()
        row = dict(combine_traffic(self.config), operator_measurements={operator: dict(performance=self.performance(directory, 0, operator))
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
            self.assertEqual(metadata["operator"], f"tilexr_ep_{operator}_memory_server_dedup")
            attributes = metadata["attributes"]
            self.assertEqual(attributes["run_id"], "run-test")
            self.assertEqual(attributes["kernel_sha256"], self.hashes[operator])
            self.assertEqual(attributes["operator"], operator)
            self.assertEqual(attributes["captured_launch"], 4)
            if operator == "combine":
                self.assertEqual(set(evidence["stages_us"]), set(operators.return_stage_counts(self.config)))
                self.assertEqual(evidence["sample_us"], [7])
                events = [event for event in trace["traceEvents"] if event["ph"] == "X"]
                self.assertEqual(len(events), 384)
                self.assertEqual({event["args"]["rank"] for event in events}, {0})
        self.assertNotEqual(*paths)
        changed = copy.deepcopy(row)
        changed["operator_measurements"]["combine"]["breakdown"]["records_path"] = (
            changed["operator_measurements"]["dispatch"]["breakdown"]["records_path"])
        with self.assertRaisesRegex(ValueError, "separate"):
            self.validate(changed)

    def test_missing_duplicate_and_overflow_events_cannot_be_complete(self):
        for fault in ("missing-end", "duplicate-start", "overflow", "accumulating"):
            events = combine_events()
            if fault == "missing-end":
                events[63].pop()
            if fault == "duplicate-start":
                events[63].insert(1, (1, 0x3001))
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                evidence = self.capture(0, "combine", fault=fault, events=events)
                row = dict(combine_traffic(self.config), operator_measurements=dict(combine=dict(breakdown=evidence)))
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

    def test_combine_summary_reports_traffic_and_across_rank_latency(self):
        config = dict(self.config, operator_mode="combine", rank_size=16, run_mode="performance")
        records = []
        for rank in range(16):
            directory = self.profile(rank, combine=[500, 500, 50 + rank, 52 + rank, 54 + rank])
            records.append(dict(combine_traffic(self.config, rank), run_id="run-test", operator_measurements=dict(
                combine=dict(performance=self.performance(directory, rank, "combine")))))
        summary = operators.summarize_operator_measurements(records, config, "run-test", self.hashes)
        combine = summary["combine"]
        self.assertEqual(combine["latency_us"], dict(median=59.5, minimum=52, maximum=67, mean=59.5))
        self.assertEqual(combine["algorithm_bytes"], 16 * combine_traffic(config)["combine_algorithm_bytes"])
        self.assertEqual(combine["algorithm_communication_bytes"], 16 * 4 * 15360)
        self.assertGreater(combine["algorithm_bandwidth_gbps"]["minimum"], 0)
        self.assertNotIn("bandwidth_gbps", combine)
        report = operators.render_operator_measurements(summary)
        for text in ("scope=full", "logical_payload_GB/s", "min=52.000 us median=59.500 us max=67.000 us", "c" * 64):
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
            records.append(dict(combine_traffic(self.config, rank), run_id="run-test", dispatch=dispatch, operator_measurements={
                operator: dict(performance=self.performance(directory, rank, operator))
                for operator in ("dispatch", "combine")}))
        summary = operators.summarize_operator_measurements(records, config, "run-test", self.hashes)
        self.assertEqual(summary["dispatch"]["algorithm_communication_bytes"], (9 * 15872 + 90 * 32) * 16)
        self.assertEqual(summary["combine"]["algorithm_communication_bytes"], 16 * 4 * 15360)
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
        self.assertEqual(summary["latency_us"]["median"], 7)
        self.assertEqual(summary["breakdown"]["ticks_per_us"], 1000)
        self.assertEqual(set(summary["breakdown"]["stages_us"]), set(operators.return_stage_counts(self.config)))
        self.assertEqual(summary["algorithm_bytes"], 2 * combine_traffic(config)["combine_algorithm_bytes"])

    def test_functional_has_no_measurements(self):
        config = dict(self.config, run_mode="functional")
        self.assertEqual(operators.summarize_operator_measurements([dict(rank=0)], config, "run-test", self.hashes), {})
        self.assertEqual(operators.render_operator_measurements({}), "")
        with self.assertRaises(ValueError):
            self.validate(dict(combine_traffic(self.config), operator_measurements={"combine": {}}), config)


if __name__ == "__main__":
    unittest.main()
