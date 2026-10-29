"""Complete Combine consumer contracts with Host substitutes, not NPU proof."""
from pathlib import Path
from uuid import NAMESPACE_URL, uuid5
import unittest

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/validation"))
from server_dedup_consumer_fixture import ConsumerFixture, COMBINE_MARKER, run_tests
import operator_measurements

MARKER = COMBINE_MARKER
PROFILE_MARKER = "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS"
RUN_ID = "run-identity"
CAPTURE = str(uuid5(NAMESPACE_URL, "tilexr-validation:" + RUN_ID))


class CombineTests(ConsumerFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.configure_combine()

    def check_success(self, rank, process, mode, loops=1, warmup=0):
        output, errors = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 0, output + errors)
        timing, = self.records(errors, "EP_SERVER_DEDUP_WALL")
        self.assertEqual(timing["rank"], rank)
        self.assertEqual(timing["unit"], "seconds")
        self.assertAlmostEqual(timing["total"], sum(timing["stages"].values()), delta=1e-5)
        for phase in ("communicator", "allocation", "reference-build", "chain-barrier", "submission",
                      "stream-sync", "combine-read", "cleanup-communicator", "cleanup-acl"):
            self.assertGreaterEqual(timing["stages"][phase], 0)
        record, = self.records(output, MARKER)
        self.assertIsNone(record["checkpoint"])
        self.assertEqual(record["native_status"], "combine-m5")
        self.assertEqual(record["implemented_through"], "M5")
        self.assertEqual(record["scope"], "full")
        self.assertEqual(record["combine_delay_us"], 0)
        self.assertEqual(record["submitted"], loops)
        self.assertEqual(record["completed"], loops)
        self.assertEqual(record["warmup_completed"], warmup)
        for key in ("combine_stage_validated", "combine_control_validated",
                    "combine_control_reserved_unchanged", "combine_result_validated",
                    "combine_result_flags_validated", "combine_unwritten_results_unchanged",
                    "combine_y_out_validated", "combine_result_flags_cleared"):
            self.assertTrue(record[key], key)
        self.assertEqual(record["combine_checked_window_mask"], 1 if mode == "both" else 3 if loops + warmup > 1 else 2)
        self.assertEqual(record["combine_observed_rounds"], 1)
        self.assertEqual(record["stream_synchronizations"], 1)
        self.assertGreater(record["combine_direct_received_rows"], 0)
        self.assertEqual(record["combine_direct_payload_bytes"], record["combine_type1_rows"] * 514)
        self.assertEqual(record["combine_direct_wire_bytes"], record["combine_type1_rows"] * 1024)
        self.assertEqual(record["combine_algorithm_bytes"],
                         (record["combine_type1_rows"] + record["combine_type2_rows"] + record["combine_type3_rows"] + record["bs"]) * 514)
        self.assertTrue(record["combine_partial_validated"])
        self.assertGreater(record["combine_partial_received_rows"], 0)
        self.assertEqual(record["combine_partial_payload_bytes"], record["combine_type2_rows"] * 514)
        self.assertEqual(record["combine_partial_wire_bytes"], record["combine_type2_rows"] * 1024)
        self.assertEqual(record["combine_partial_stage_read_bytes"], record["combine_partial_member_rows"] * 514)
        self.assertEqual(record["combine_numeric_policy"], "fp32-sequential-rne-bit-exact")
        self.assertEqual((record["combine_numeric_atol"], record["combine_numeric_rtol"]), (0, 0))
        calls = self.trace_lines(rank)
        self.assertEqual(calls.count("combine-launch"), loops + warmup)
        self.assertEqual(calls.count("m5-result-clear"), loops + warmup)
        self.assertEqual(calls.count("sync"), 1)
        # All guard/initial Flag writes precede launch. There is no test helper
        # clearing results after kernel completion or before a second call.
        self.assertNotIn("peer-window-seed", calls[calls.index("combine-launch") + 1:])
        self.assertEqual(calls[-1], "finalize")
        return output, record

    def test_dtype_modes_and_quantized_dispatch_connection(self):
        self.check_matrix(1, 0)
        self.check_matrix(2, 0)
        self.check_matrix(5, 2)

    def check_matrix(self, loops, warmup):
        for mode in ("combine", "both"):
            for dtype in ("bf16", "fp16"):
                for quant in ((0, 4) if mode == "both" else (0,)):
                    with self.subTest(mode=mode, dtype=dtype, quant=quant):
                        env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode,
                                   TILEXR_VALIDATION_DTYPE=dtype,
                                   TILEXR_VALIDATION_DISPATCH_QUANT_MODE=str(quant),
                                   TILEXR_VALIDATION_LOOPS=str(loops), TILEXR_VALIDATION_WARMUP=str(warmup))
                        local_members, minimum, maximum = 0, 6, 0
                        for rank, process in enumerate(self.run_ranks(env)):
                            _, record = self.check_success(rank, process, mode, loops, warmup)
                            local_members += record["combine_partial_local_members"]
                            if record["combine_type2_rows"]:
                                minimum = min(minimum, record["combine_partial_min_members"])
                                maximum = max(maximum, record["combine_partial_max_members"])
                        self.assertGreater(local_members, 0, "fixture exercises same-card stage members")
                        self.assertEqual((minimum, maximum), (1, 5), "fixture exercises N=1 and K-1")
                        self.reset_case()

    def test_performance_and_module_breakdown_multiple_calls(self):
        for mode in ("combine", "both"):
            for run_mode in ("performance", "breakdown"):
                with self.subTest(mode=mode, run_mode=run_mode):
                    processes = [self.start(rank, environment=dict(
                        TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_RUN_MODE=run_mode,
                        TILEXR_VALIDATION_LOOPS="5", TILEXR_VALIDATION_WARMUP="2",
                        TILEXR_PROFILING_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.dispatch.records.v1"),
                        TILEXR_PROFILING_COMBINE_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.combine.records.v1")))
                        for rank in range(16)]
                    for rank, process in enumerate(processes):
                        output, _ = self.check_success(rank, process, mode, 5, 2)
                        captures = self.records(output, "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS")
                        expected = ({"combine", "dispatch"} if mode == "both" else {"combine"}) if run_mode == "breakdown" else set()
                        self.assertEqual({capture["operator"] for capture in captures}, expected)
                        for capture in captures:
                            self.assertEqual(capture["captured_launch"], 6)
                            self.assertEqual(capture["captured_launches"], 1)
                            self.assertGreater(Path(capture["records_path"]).stat().st_size, 0)
                    self.reset_case()

    def test_payload_flag_and_unwritten_slot_mismatches_keep_capture(self):
        for fault in ("yout", "yout-tail", "flag-first", "flag-tail", "inactive-flag", "payload-clobber", "unrounded-partial"):
            with self.subTest(fault=fault):
                processes = [self.start(rank, environment=dict(
                    TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_RUN_MODE="breakdown",
                    TEST_M5_FAULT=fault,
                    TILEXR_PROFILING_COMBINE_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.combine.records.v1")))
                    for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=30)
                    self.assertEqual(process.returncode, 0, output + errors)
                    record, = self.records(output, "EP_SERVER_DEDUP_COMBINE_CHAIN_INVALID")
                    self.assertEqual(record["status"], "FAIL")
                    self.assertEqual(record["validation_status"], "FAIL")
                    self.assertTrue(record["execution_completed"])
                    self.assertEqual(record["completed"], 1)
                    self.assertFalse(record["combine_result_validated"])
                    self.assertGreater(record["combine_direct_payload_bytes"], 0)
                    capture, = self.records(output, "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS")
                    self.assertEqual(capture["operator"], "combine")
                    self.assertGreater(Path(capture["records_path"]).stat().st_size, 0)
                    self.assertIn("mismatch", errors)
                    self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                self.reset_case()


    def test_stage_control_direct_and_partial_corruption(self):
        cases = [("TEST_M12_FAULT", fault) for fault in
                 ("stage-tail", "padding", "prepare", "reserved", "inactive-window")]
        cases += [("TEST_M3_FAULT", "payload"), ("TEST_M3_FAULT", "unwritten"),
                  ("TEST_M4_FAULT", "payload"), ("TEST_M4_FAULT", "tail")]
        for key, fault in cases:
            with self.subTest(key=key, fault=fault):
                for process in self.run_ranks({key: fault, "TILEXR_VALIDATION_OPERATOR_MODE": "combine"}):
                    output, errors = process.communicate(timeout=30)
                    self.assertEqual(process.returncode, 0, output + errors)
                    record, = self.records(output, "EP_SERVER_DEDUP_COMBINE_CHAIN_INVALID")
                    self.assertFalse(record["combine_result_validated"])
                    self.assertTrue(record["execution_completed"])
                self.reset_case()

    def test_invalid_round_count_rejected_before_acl(self):
        for mode in ("functional", "performance", "breakdown"):
            for invalid in ({"TILEXR_VALIDATION_LOOPS": "0"},
                            {"TILEXR_VALIDATION_WARMUP": "-1"}):
                env = dict(TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_RUN_MODE=mode, **invalid)
                with self.subTest(env=env):
                    process = self.start(0, environment=env)
                    output, errors = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0, output + errors)
                    self.assertIn("stage=config", errors)
                    self.assertEqual(self.trace_lines(0), [])


    def start_profile(self, rank, mode, run_mode, **extra):
        env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_RUN_MODE=run_mode,
            TILEXR_PROFILING_CAPTURE_ID=CAPTURE, TILEXR_VALIDATION_LOOPS="2", TILEXR_VALIDATION_WARMUP="1",
            TILEXR_PROFILING_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.dispatch.records.v1"),
            TILEXR_PROFILING_COMBINE_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.combine.records.v1"))
        env.update(extra)
        return self.start(rank, environment=env)


    def parse_evidence(self, rank, output, mode):
        config = dict(operator_mode=mode, run_mode="breakdown", loops=2, warmup=1, implemented_through="M5", bs=16, h=257, topk=6, rank_size=16)
        markers = self.records(output, PROFILE_MARKER)
        active = operator_measurements.active_operators(config)
        self.assertEqual({m["operator"] for m in markers}, set(active))
        self.assertEqual(len(markers), len(active))
        result = dict(self.records(output, MARKER)[0] if mode != "dispatch" else {},
                      rank=rank, run_id=RUN_ID, operator_measurements={})
        for marker in markers:
            operator = marker["operator"]
            self.assertIsNone(marker["checkpoint"])
            self.assertEqual(marker["capture_id"], CAPTURE)
            self.assertEqual((marker["launches"], marker["captured_launch"]), (3, 2))
            self.assertEqual(marker["captured_launches"], 1)
            self.assertEqual(marker["capture_mode"], "latest_launch")
            evidence = operator_measurements.breakdown_record(marker["records_path"], config, rank, CAPTURE,
                Path(self.trace.name) / f"profile-rank-{rank}" / operator,
                operator=operator, run_id=RUN_ID, kernel_sha="a" * 64, record=result)
            result["operator_measurements"][operator] = dict(breakdown=evidence)
        return config, result


    def test_all_modes_share_continuous_batches_and_only_active_capture(self):
        for mode in ("dispatch", "combine", "both"):
            for run_mode in ("functional", "performance", "breakdown"):
                with self.subTest(mode=mode, run_mode=run_mode):
                    self.reset_case()
                    processes = [self.start_profile(rank, mode, run_mode) for rank in range(16)]
                    for rank, process in enumerate(processes):
                        output, errors = process.communicate(timeout=25)
                        self.assertEqual(process.returncode, 0, output + errors)
                        success = "EP_SERVER_DEDUP_DISPATCH_PASS" if mode == "dispatch" else MARKER
                        record, = self.records(output, success)
                        self.assertIsNone(record["checkpoint"])
                        self.assertEqual((record["submitted"], record["completed"]), (2, 2))
                        self.assertEqual((record["warmup_submitted"], record["warmup_completed"]), (1, 1))
                        calls = self.trace_lines(rank)
                        self.assertEqual(calls.count("sync"), 1)
                        self.assertEqual(calls.count("launch"), 3 if mode != "combine" else 0)
                        self.assertEqual(calls.count("combine-launch"), 3 if mode != "dispatch" else 0)
                        for operator in ("dispatch", "combine"):
                            active = run_mode == "breakdown" and mode in (operator, "both")
                            self.assertEqual((Path(self.trace.name) / f"rank-{rank}.{operator}.records.v1").exists(), active)
                            export = "profiling-export" if operator == "dispatch" else "combine-profiling-export"
                            self.assertEqual(calls.count(export), int(active))
                            if active:
                                self.assertGreater(calls.index(export), calls.index("sync"))
                        if run_mode == "breakdown" and rank == 0:
                            config, result = self.parse_evidence(rank, output, mode)
                            operator_measurements.validate_operator_measurements(result, config)
                            if mode == "both":
                                d = result["operator_measurements"]["dispatch"]["breakdown"]
                                c = result["operator_measurements"]["combine"]["breakdown"]
                                self.assertNotEqual(d["artifacts"]["perfetto_trace.json"], c["artifacts"]["perfetto_trace.json"])
                                self.assertGreater(c["sample_us"][0], 0)
                        elif run_mode != "breakdown":
                            self.assertNotIn(PROFILE_MARKER, output)


    def test_combine_capture_fault_does_not_hide_dispatch_evidence(self):
        for fault in ("missing", "capture", "overflow", "end"):
            with self.subTest(fault=fault):
                self.reset_case()
                processes = [self.start_profile(rank, "both", "breakdown", TEST_COMBINE_PROFILE_FAULT=fault)
                             for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=25)
                    self.assertEqual(process.returncode, 0, output + errors)
                    if rank == 0:
                        config = dict(operator_mode="both", run_mode="breakdown", loops=2, warmup=1)
                        markers = {m["operator"]: m for m in self.records(output, PROFILE_MARKER)}
                        dispatch = operator_measurements.breakdown_record(markers["dispatch"]["records_path"],
                            config, rank, CAPTURE, Path(self.trace.name) / "dispatch-report",
                            operator="dispatch", run_id=RUN_ID, kernel_sha="a" * 64)
                        self.assertEqual(dispatch["status"], "complete")
                        if fault in ("missing", "capture", "end"):
                            with self.assertRaises(ValueError):
                                self.parse_evidence(rank, output, "both")
                        else:
                            config, result = self.parse_evidence(rank, output, "both")
                            with self.assertRaises(ValueError):
                                operator_measurements.validate_operator_measurements(result, config)


    def test_export_failure_prevents_pass_after_safe_cleanup(self):
        processes = [self.start_profile(rank, "both", "breakdown", TEST_FAIL_STAGE="combine-profiling-export")
                     for rank in range(16)]
        for rank, process in enumerate(processes):
            output, errors = process.communicate(timeout=25)
            self.assertNotEqual(process.returncode, 0, output + errors)
            self.assertNotIn(PROFILE_MARKER, output)
            self.assertNotIn(MARKER, output)
            self.assertIn("stage=cleanup", errors)
            self.assertEqual(self.trace_lines(rank)[-1], "finalize")


    def test_mid_batch_launch_or_sync_failure_keeps_live_resources(self):
        self.environment.update(TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="2")
        for mode in ("dispatch", "combine", "both"):
            stages = (["launch"] if mode != "combine" else []) + (["combine-launch"] if mode != "dispatch" else []) + ["sync"]
            for stage in stages:
                for failed_call in ((1,) if stage == "sync" else (2, 4)):
                    with self.subTest(mode=mode, stage=stage, failed_call=failed_call):
                        processes = [self.start(rank, environment=dict(TILEXR_VALIDATION_OPERATOR_MODE=mode,
                            **({"TEST_FAIL_STAGE": stage, "TEST_FAIL_CALL": str(failed_call)} if rank == 1 else {})))
                            for rank in range(16)]
                        output, errors = processes[1].communicate(timeout=15)
                        self.assertNotEqual(processes[1].returncode, 0, output + errors)
                        self.assertNotIn(MARKER, output)
                        self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                        self.assertIn("ret=91", errors)
                        self.assertIn("cleanup=deferred-to-process-exit", errors)
                        calls = self.trace_lines(1)
                        self.assertEqual(calls.count(stage), failed_call)
                        self.assertNotIn("free", calls)
                        self.assertNotIn("comm-destroy", calls)
                        self.reset_case()


    def test_input_mutation_and_cleanup_failure(self):
        cases = [{"TEST_COMBINE_FAULT": name} for name in
                 ("expert_out", "expert_out_tail", "send_counts", "assist", "token_type", "relay", "source_mask", "weights")]
        cases += [{"TEST_FAIL_STAGE": "comm-destroy"}]
        for fault in cases:
            with self.subTest(fault=fault):
                for rank, process in enumerate(self.run_ranks(dict(fault, TILEXR_VALIDATION_OPERATOR_MODE="combine"))):
                    output, errors = process.communicate(timeout=30)
                    self.assertNotIn(MARKER, output)
                    self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                    if "TEST_FAIL_STAGE" in fault:
                        self.assertNotEqual(process.returncode, 0, output + errors)
                        self.assertIn("stage=cleanup", errors)
                    else:
                        self.assertEqual(process.returncode, 0, output + errors)
                        record, = self.records(output, "EP_SERVER_DEDUP_COMBINE_CHAIN_INVALID")
                        self.assertTrue(record["execution_completed"])
                        self.assertEqual(record["validation_status"], "FAIL")
                self.reset_case()


if __name__ == "__main__":
    run_tests(CombineTests)
    print("EP_SERVER_DEDUP_COMBINE_CONSUMER_PASS test=lifecycle", flush=True)
