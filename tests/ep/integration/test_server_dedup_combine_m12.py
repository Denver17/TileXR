"""P2.1 consumer oracles using a Host substitute, never device-protocol proof."""
import argparse
import os
from pathlib import Path
import tempfile
import unittest

import test_server_dedup_combine_chain as chain

CHECKPOINT = "ep-sdedup-combine-p2-m12"
CONSUMER = None


class CombineM12Tests(chain.CombineChainTests):
    @classmethod
    def setUpClass(cls):
        if CONSUMER is None:
            super().setUpClass()
        else:
            cls.executable = Path(CONSUMER).resolve()
            if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
                raise RuntimeError(f"P2.1 consumer is not executable: {cls.executable}")
            cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-combine-m12-")
            cls.folder = Path(cls.temp.name)

    def setUp(self):
        super().setUp()
        self.environment.update(TILEXR_INTERNAL_ACCEPTANCE_STAGE=CHECKPOINT,
                                TILEXR_VALIDATION_BS="16", TILEXR_VALIDATION_H="13",
                                TILEXR_VALIDATION_TOPK="2",
                                TILEXR_VALIDATION_COMBINE_FIXTURE="balanced",
                                TILEXR_VALIDATION_COMBINE_OBSERVATION="batch")

    def check_m12(self, rank, process, mode, *, loops=1, warmup=0, diagnostic=False, empty=False):
        output, errors = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 0, output + errors)
        record, = self.records(output, chain.MARKER)
        self.assertEqual(record["checkpoint"], CHECKPOINT)
        self.assertEqual(record["native_status"], "combine-m12")
        self.assertEqual(record["implemented_through"], "M2")
        self.assertEqual(record["scope"], "partial")
        self.assertFalse(record["combine_empty"])
        self.assertEqual(record["combine_delay_us"], 50)
        self.assertEqual(record["submitted"], loops)
        self.assertEqual(record["completed"], loops)
        self.assertEqual(record["warmup_completed"], warmup)
        for key in ("combine_stage_validated", "combine_control_validated",
                    "combine_control_reserved_unchanged", "combine_result_unchanged", "y_out_unchanged"):
            self.assertTrue(record[key], key)
        count = loops + warmup
        mask = 1 if mode == "both" else 3 if count > 1 else 2
        self.assertEqual(record["combine_checked_window_mask"], mask)
        self.assertEqual(record["combine_observed_rounds"], count if diagnostic else 1)
        self.assertEqual(record["stream_synchronizations"], count if diagnostic else 1)
        self.assertEqual(record["combine_control_publish_bytes"], (64 * 8 + 8 * 32) * 32)
        if empty:
            self.assertEqual(record["combine_type3_rows"], 0)
            self.assertEqual(record["combine_algorithm_bytes"], 0)
        self.assertEqual(record["combine_stage_copy_bytes"], 2 * record["combine_algorithm_bytes"])
        calls = self.trace_lines(rank)
        self.assertEqual(calls.count("combine-launch"), count)
        self.assertEqual(calls.count("sync"), count if diagnostic else 1)
        self.assertIn("m12-window-seed", calls)
        self.assertIn("m12-window-read", calls)
        self.assertEqual(calls[-1], "finalize")
        return record

    def test_single_and_continuous_dtype_handoff_matrix(self):
        for mode in ("combine", "both"):
            for dtype in ("bf16", "fp16"):
                for loops, warmup in ((1, 0), (5, 2)):
                    for quant in ((0, 4) if mode == "both" else (0,)):
                        with self.subTest(mode=mode, dtype=dtype, loops=loops, quant=quant):
                            env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_DTYPE=dtype,
                                       TILEXR_VALIDATION_DISPATCH_QUANT_MODE=str(quant),
                                       TILEXR_VALIDATION_LOOPS=str(loops), TILEXR_VALIDATION_WARMUP=str(warmup))
                            for rank, process in enumerate(self.run_ranks(env)):
                                self.check_m12(rank, process, mode, loops=loops, warmup=warmup)
                            self.reset_case()

    def test_empty_and_sparse_diagnostic_same_window_reuse(self):
        for fixture, bs in (("empty", 16), ("sparse", 16), ("sparse", 320)):
            with self.subTest(fixture=fixture, bs=bs):
                env = dict(TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_BS=str(bs),
                           TILEXR_VALIDATION_H="257", TILEXR_VALIDATION_COMBINE_FIXTURE=fixture,
                           TILEXR_VALIDATION_COMBINE_OBSERVATION="per-round",
                           TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="2")
                for rank, process in enumerate(self.run_ranks(env)):
                    record = self.check_m12(rank, process, "combine", loops=3, warmup=2,
                                           diagnostic=True, empty=fixture == "empty")
                    if fixture == "sparse":
                        self.assertGreater(record["combine_type3_rows"], 0)
                self.reset_case()

    def test_payload_padding_control_and_result_faults_are_rejected(self):
        faults = ("missing-copy", "packed-index", "stage-tail", "padding", "prepare", "ready",
                  "neighbor", "reserved", "result", "inactive-window")
        for fault in faults:
            with self.subTest(fault=fault):
                env = dict(TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_COMBINE_FIXTURE="sparse",
                           TEST_M12_FAULT=fault)
                for rank, process in enumerate(self.run_ranks(env)):
                    output, errors = process.communicate(timeout=30)
                    self.assertNotEqual(process.returncode, 0, output + errors)
                    self.assertNotIn(chain.MARKER, output)
                    self.assertIn("EP_SERVER_DEDUP_COMBINE_CHAIN_INVALID", output)
                    self.assertIn("mismatch", errors)
                    self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                self.reset_case()

    def test_performance_and_breakdown_use_same_batch_and_separate_capture(self):
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
                        output, errors = process.communicate(timeout=30)
                        self.assertEqual(process.returncode, 0, output + errors)
                        record, = self.records(output, chain.MARKER)
                        self.assertEqual(record["native_status"], "combine-m12")
                        self.assertEqual(record["submitted"], 5)
                        self.assertEqual(record["warmup_completed"], 2)
                        self.assertEqual(record["stream_synchronizations"], 1)
                        self.assertTrue(record["combine_stage_validated"])
                        captures = self.records(output, "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS")
                        expected = ({"combine", "dispatch"} if mode == "both" else {"combine"}) if run_mode == "breakdown" else set()
                        self.assertEqual({capture["operator"] for capture in captures}, expected)
                        for capture in captures:
                            self.assertEqual(capture["captured_launches"], 1)
                            self.assertEqual(capture["captured_launch"], 6)
                            self.assertGreater(Path(capture["records_path"]).stat().st_size, 0)
                        self.assertEqual(len({capture["records_path"] for capture in captures}), len(expected))
                        for operator in ("dispatch", "combine"):
                            path = Path(self.trace.name) / f"rank-{rank}.{operator}.records.v1"
                            self.assertEqual(path.exists(), operator in expected)
                    self.reset_case()

    def test_invalid_fixture_and_diagnostic_modes_fail_before_acl(self):
        cases = ({"TILEXR_VALIDATION_COMBINE_FIXTURE": "invalid"},
                 {"TILEXR_VALIDATION_COMBINE_OBSERVATION": "invalid"},
                 {"TILEXR_VALIDATION_COMBINE_FIXTURE": "sparse"},
                 {"TILEXR_VALIDATION_COMBINE_OBSERVATION": "per-round",
                  "TILEXR_VALIDATION_RUN_MODE": "performance"})
        for env in cases:
            with self.subTest(env=env):
                process = self.start(0, environment=env)
                output, errors = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0, output + errors)
                self.assertIn("stage=config", errors)
                self.assertEqual(self.trace_lines(0), [])


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineM12Tests(name) for name in sorted(CombineM12Tests.__dict__)
                              if name.startswith("test_"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer")
    options, unittest_args = parser.parse_known_args()
    CONSUMER = options.consumer
    program = unittest.main(argv=[__file__] + unittest_args, exit=False)
    if program.result.wasSuccessful() and not program.result.skipped:
        print("EP_SERVER_DEDUP_COMBINE_M12_PASS test=lifecycle", flush=True)
    else:
        raise SystemExit(1)
