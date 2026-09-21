"""M5 consumer/oracle contracts with a Host transport substitute, not NPU proof."""
import argparse
import os
from pathlib import Path
import tempfile
import unittest

import test_server_dedup_combine_chain as chain

CHECKPOINT = "ep-sdedup-combine-p2-m5"
CONSUMER = None


class CombineM5Tests(chain.CombineChainTests):
    @classmethod
    def setUpClass(cls):
        if CONSUMER is None:
            super().setUpClass()
        else:
            cls.executable = Path(CONSUMER).resolve()
            if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
                raise RuntimeError(f"M5 consumer is not executable: {cls.executable}")
            cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-combine-m5-")
            cls.folder = Path(cls.temp.name)

    def setUp(self):
        super().setUp()
        self.environment.update(TILEXR_VALIDATION_CHECKPOINT=CHECKPOINT,
                                TILEXR_VALIDATION_BS="16", TILEXR_VALIDATION_H="257",
                                TILEXR_VALIDATION_TOPK="6",
                                TILEXR_VALIDATION_COMBINE_FIXTURE="balanced",
                                TILEXR_VALIDATION_COMBINE_OBSERVATION="batch")

    def check_successful_m5(self, rank, process, mode, loops=1, warmup=0):
        output, errors = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 0, output + errors)
        record, = self.records(output, chain.MARKER)
        self.assertEqual(record["checkpoint"], CHECKPOINT)
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
        self.assertNotIn("m12-window-seed", calls[calls.index("combine-launch") + 1:])
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
                            _, record = self.check_successful_m5(rank, process, mode, loops, warmup)
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
                        output, _ = self.check_successful_m5(rank, process, mode, 5, 2)
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

    def test_invalid_round_count_and_diagnostic_rejected_before_acl(self):
        for mode in ("functional", "performance", "breakdown"):
            for invalid in ({"TILEXR_VALIDATION_LOOPS": "0"},
                            {"TILEXR_VALIDATION_WARMUP": "-1"},
                            {"TILEXR_VALIDATION_COMBINE_OBSERVATION": "per-round"},
                            {"TILEXR_VALIDATION_COMBINE_FIXTURE": "sparse"}):
                env = dict(TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_RUN_MODE=mode, **invalid)
                with self.subTest(env=env):
                    process = self.start(0, environment=env)
                    output, errors = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0, output + errors)
                    self.assertIn("stage=config", errors)
                    self.assertEqual(self.trace_lines(0), [])


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineM5Tests(name) for name in sorted(CombineM5Tests.__dict__)
                              if name.startswith("test_"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer")
    options, unittest_args = parser.parse_known_args()
    CONSUMER = options.consumer
    program = unittest.main(argv=[__file__] + unittest_args, exit=False)
    if program.result.wasSuccessful() and not program.result.skipped:
        print("EP_SERVER_DEDUP_COMBINE_M5_PASS test=lifecycle", flush=True)
    else:
        raise SystemExit(1)
