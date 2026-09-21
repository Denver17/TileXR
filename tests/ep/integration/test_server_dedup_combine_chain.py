"""C4: same consumer with Host substitutes; no NPU or device communication proof."""
import argparse
import os
from pathlib import Path
import shutil
import tempfile
import unittest

import test_server_dedup_chain as dispatch_tests


CHECKPOINT = "ep-sdedup-combine-p1-chain-single"
MARKER = "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS"
CONSUMER = None


class CombineChainTests(dispatch_tests.ChainConsumerTests):
    @classmethod
    def setUpClass(cls):
        if CONSUMER is None:
            if shutil.which("g++") is None or os.name != "posix":
                raise RuntimeError("POSIX C++14 compiler required; C4 lifecycle cannot be skipped")
            super().setUpClass()
            return
        cls.executable = Path(CONSUMER).resolve()
        if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
            raise RuntimeError(f"C4 consumer is not executable: {cls.executable}")
        cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-combine-chain-")
        cls.folder = Path(cls.temp.name)

    def setUp(self):
        super().setUp()
        self.environment.update(TILEXR_INTERNAL_ACCEPTANCE_STAGE=CHECKPOINT,
            TILEXR_VALIDATION_OPERATOR_MODE="both", TILEXR_VALIDATION_RUN_MODE="functional",
            TILEXR_VALIDATION_LOOPS="1", TILEXR_VALIDATION_WARMUP="0",
            TILEXR_VALIDATION_COMBINE_QUANT_MODE="0")
        for key in tuple(self.environment):
            if key.startswith("TEST_") and key != "TEST_TRACE_DIR":
                self.environment.pop(key)

    def reset_case(self):
        self.stop()
        self.processes.clear()
        shutil.rmtree(self.trace.name)
        Path(self.trace.name).mkdir()

    def run_ranks(self, environment):
        return [self.start(rank, environment=environment) for rank in range(16)]

    def check_success(self, rank, process, mode, quant):
        output, errors = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 0, output + errors)
        record, = self.records(output, MARKER)
        self.assertEqual(record["checkpoint"], CHECKPOINT)
        self.assertEqual(record["operator_mode"], mode)
        self.assertEqual(record["dispatch_quant_mode"], quant)
        self.assertEqual(record["combine_quant_mode"], 0)
        self.assertEqual(record["cleanup"], "complete")
        self.assertEqual((record["submitted"], record["completed"]), (1, 1))
        self.assertEqual((record["warmup_submitted"], record["warmup_completed"]), (0, 0))
        for key in ("combine_submitted", "combine_completed"):
            self.assertEqual(record[key], 1)
        for key in ("dispatch_submitted", "dispatch_completed"):
            self.assertEqual(record[key], int(mode == "both"))
        for key in ("combine_inputs_unchanged", "y_out_unchanged", "combine_empty"):
            self.assertIs(record[key], True)
        for key in ("five_tables_linked", "send_counts_linked"):
            self.assertIs(record[key], mode == "both")
        expected_source = ("standalone_expert_out" if mode == "combine" else
                           "separate_expert_out" if quant else "dispatch_expand_x")
        self.assertEqual(record["expert_out_source"], expected_source)
        self.assertEqual(record["combine_delay_us"], 50)
        self.assertEqual(record["dispatch_last_magic"], int(mode == "both"))
        self.assertEqual(record["combine_last_magic"], 2 if mode == "both" else 1)
        self.assertEqual(record["dispatch_window_offset"], 4 * 1024 ** 3 if mode == "both" else 0)
        self.assertEqual(record["combine_window_offset"], 0 if mode == "both" else 4 * 1024 ** 3)
        dispatch = self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")
        self.assertEqual(len(dispatch), int(mode == "both"))
        if dispatch:
            self.assertTrue(dispatch[0]["five_tables_validated"])
            self.assertTrue(dispatch[0]["member_addresses_validated"])
        calls = self.trace_lines(rank)
        self.assertEqual(calls.count("launch"), int(mode == "both"))
        self.assertEqual(calls.count("combine-launch"), 1)
        self.assertEqual(calls.count("sync"), 2 if mode == "both" else 1)
        self.assertEqual(calls.count("comm-init"), 1)
        self.assertEqual(calls.count("create-stream"), 1)
        self.assertEqual(calls.count("malloc"), 8 if mode == "combine" else 14 if quant else 12)
        self.assertEqual(calls.count("free"), calls.count("malloc"))
        combine = calls.index("combine-launch")
        syncs = [index for index, call in enumerate(calls) if call == "sync"]
        self.assertLess(combine, syncs[-1])
        self.assertGreater(calls.index("free"), syncs[-1])
        self.assertGreater(calls.index("comm-destroy"), max(index for index, call in enumerate(calls) if call == "free"))
        self.assertEqual(calls[-1], "finalize")
        if mode == "both":
            self.assertLess(calls.index("launch"), syncs[0])
            self.assertLess(syncs[0], combine)
            self.assertIn("packet-read", calls[syncs[0]:combine])
            self.assertNotIn("packet-read", calls[combine:])

    def test_same_consumer_modes_dtypes_and_quantized_handoff(self):
        cases = [(mode, dtype, 0, "e4m3") for mode in ("combine", "both") for dtype in ("bf16", "fp16")]
        cases += [("both", dtype, 4, fmt) for dtype in ("bf16", "fp16") for fmt in ("e4m3", "e5m2")]
        for mode, dtype, quant, fmt in cases:
            with self.subTest(mode=mode, dtype=dtype, quant=quant, fp8=fmt):
                env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_DTYPE=dtype,
                           TILEXR_VALIDATION_DISPATCH_QUANT_MODE=str(quant), TILEXR_VALIDATION_FP8_FORMAT=fmt)
                for rank, process in enumerate(self.run_ranks(env)):
                    self.check_success(rank, process, mode, quant)
                self.reset_case()

    def test_dispatch_only_remains_on_existing_path(self):
        for rank, process in enumerate(self.run_ranks({"TILEXR_VALIDATION_OPERATOR_MODE": "dispatch"})):
            output, errors = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 0, output + errors)
            self.assertEqual(len(self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")), 1)
            self.assertNotIn(MARKER, output)
            calls = self.trace_lines(rank)
            self.assertNotIn("combine-launch", calls)
            self.assertEqual(calls.count("malloc"), 11)
            self.assertEqual(calls.count("sync"), 1)

    def test_combine_ignores_inactive_dispatch_quantization(self):
        env = {"TILEXR_VALIDATION_OPERATOR_MODE": "combine", "TILEXR_VALIDATION_DISPATCH_QUANT_MODE": "4"}
        for rank, process in enumerate(self.run_ranks(env)):
            self.check_success(rank, process, "combine", 4)

    def test_standalone_combine_accepts_incomplete_dispatch_epoch(self):
        env = dict(TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_BS="1",
                   TILEXR_VALIDATION_MOE_EXPERT_NUM="32", TILEXR_VALIDATION_TOPK="3",
                   TILEXR_VALIDATION_ROUTE_MODE="balance_shift")
        for rank, process in enumerate(self.run_ranks(env)):
            self.check_success(rank, process, "combine", 0)

    def test_invalid_c4_batch_rejected_before_acl(self):
        cases = [{"TILEXR_VALIDATION_LOOPS": "2"}, {"TILEXR_VALIDATION_WARMUP": "1"},
                 {"TILEXR_VALIDATION_RUN_MODE": "performance"}, {"TILEXR_VALIDATION_RUN_MODE": "breakdown"},
                 {"TILEXR_VALIDATION_COMBINE_QUANT_MODE": "4"}]
        for mode in ("combine", "both"):
            for invalid in cases:
                with self.subTest(mode=mode, invalid=invalid):
                    process = self.start(0, environment=dict(invalid, TILEXR_VALIDATION_OPERATOR_MODE=mode))
                    output, errors = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0, output + errors)
                    self.assertNotIn("_PASS ", output)
                    self.assertIn("stage=config", errors)
                    self.assertEqual(self.trace_lines(0), [])

    def test_combine_launch_and_sync_failure_preserve_live_allocations(self):
        for mode in ("combine", "both"):
            for stage in ("combine-launch", "sync"):
                with self.subTest(mode=mode, stage=stage):
                    processes = [self.start(rank, environment=dict(TILEXR_VALIDATION_OPERATOR_MODE=mode,
                        **({"TEST_FAIL_STAGE": stage, "TEST_FAIL_CALL": "2" if mode == "both" and stage == "sync" else "1"}
                           if rank == 1 else {}))) for rank in range(16)]
                    output, errors = processes[1].communicate(timeout=15)
                    self.assertNotEqual(processes[1].returncode, 0, output + errors)
                    self.assertNotIn(MARKER, output)
                    self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                    self.assertIn("ret=91", errors)
                    self.assertIn("cleanup=deferred-to-process-exit", errors)
                    calls = self.trace_lines(1)
                    self.assertIn("combine-launch", calls)
                    self.assertNotIn("free", calls)
                    self.assertNotIn("comm-destroy", calls)
                    self.reset_case()

    def test_every_combine_input_and_output_is_checked_for_mutation(self):
        for mode in ("combine", "both"):
            faults = ("y_out", "expert_out", "send_counts", "assist", "token_type", "relay", "source_mask", "weights")
            for fault in faults + tuple(name + "_tail" for name in faults):
                with self.subTest(mode=mode, fault=fault):
                    processes = [self.start(rank, environment=dict(TILEXR_VALIDATION_OPERATOR_MODE=mode,
                        **({"TEST_COMBINE_FAULT": fault} if rank == 1 else {}))) for rank in range(16)]
                    for rank, process in enumerate(processes):
                        if rank == 1:
                            output, errors = process.communicate(timeout=20)
                            self.assertNotEqual(process.returncode, 0, output + errors)
                            self.assertNotIn(MARKER, output)
                            self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                            self.assertIn("combine", errors.lower())
                            self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                        else:
                            self.check_success(rank, process, mode, 0)
                    self.reset_case()

    def test_invalid_dispatch_output_prevents_combine_launch(self):
        for fault in ({"TEST_COUNT_FAULT": "prefix"}, {"TEST_PACKET_FAULT": "hidden"}):
            for rank, process in enumerate(self.run_ranks(fault)):
                output, errors = process.communicate(timeout=20)
                self.assertNotEqual(process.returncode, 0, output + errors)
                self.assertNotIn(MARKER, output)
                self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                self.assertNotIn("combine-launch", self.trace_lines(rank))
                self.assertEqual(self.trace_lines(rank)[-1], "finalize")
            self.reset_case()

    def test_cleanup_failure_cannot_publish_combined_success(self):
        for mode in ("combine", "both"):
            processes = [self.start(rank, environment=dict(TILEXR_VALIDATION_OPERATOR_MODE=mode,
                **({"TEST_FAIL_STAGE": "comm-destroy"} if rank == 1 else {}))) for rank in range(16)]
            for rank, process in enumerate(processes):
                if rank == 1:
                    output, errors = process.communicate(timeout=20)
                    self.assertNotEqual(process.returncode, 0, output + errors)
                    self.assertNotIn(MARKER, output)
                    self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                    self.assertIn("stage=cleanup", errors)
                    self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                else:
                    self.check_success(rank, process, mode, 0)
            self.reset_case()


def load_tests(loader, tests, pattern):
    # Reuse the established fixture, but leave its Dispatch regressions in their
    # own suite instead of executing all inherited tests a second time.
    return unittest.TestSuite(CombineChainTests(name) for name in sorted(CombineChainTests.__dict__)
                              if name.startswith("test_"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer")
    options, unittest_args = parser.parse_known_args()
    CONSUMER = options.consumer
    program = unittest.main(argv=[__file__] + unittest_args, exit=False)
    if program.result.wasSuccessful() and not program.result.skipped:
        print(MARKER + " test=lifecycle", flush=True)
    else:
        raise SystemExit(1)
