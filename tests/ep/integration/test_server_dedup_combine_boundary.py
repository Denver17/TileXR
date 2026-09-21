"""C7 consumer lifecycle and fault checks with a sparse Host memory substitute."""

import argparse
import os
from pathlib import Path
import tempfile
import unittest

import test_server_dedup_combine_chain as single_tests

CHECKPOINT = "ep-sdedup-combine-p1-memory-boundary"
MARKER = "EP_SERVER_DEDUP_COMBINE_BOUNDARY_PASS"
CONSUMER = None


class CombineBoundaryTests(single_tests.CombineChainTests):
    @classmethod
    def setUpClass(cls):
        if CONSUMER is None:
            super().setUpClass()
            return
        cls.executable = Path(CONSUMER).resolve()
        if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
            raise RuntimeError(f"C7 consumer is not executable: {cls.executable}")
        cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-combine-boundary-")
        cls.folder = Path(cls.temp.name)

    def setUp(self):
        super().setUp()
        self.environment.update(TILEXR_VALIDATION_CHECKPOINT=CHECKPOINT,
            TILEXR_VALIDATION_BOUNDARY_PROBE="1", TILEXR_VALIDATION_OPERATOR_MODE="combine",
            TILEXR_VALIDATION_RUN_MODE="functional", TILEXR_VALIDATION_LOOPS="1", TILEXR_VALIDATION_WARMUP="0")

    def check_probe(self, rank, process):
        output, errors = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 0, output + errors)
        record, = self.records(output, MARKER)
        chain, = self.records(output, "EP_SERVER_DEDUP_CHAIN_PASS")
        for item in (record, chain):
            self.assertEqual(item["checkpoint"], CHECKPOINT)
            self.assertIs(item["boundary_probe"], True)
            self.assertEqual(item["rank"], rank)
            self.assertEqual(item["peer_mem_bytes"], 8 * 1024 ** 3)
        self.assertEqual(record["buffer_stride"], 4 * 1024 ** 3)
        self.assertEqual(record["runtime_reserved_bytes"], 4 * 1024 ** 2)
        self.assertEqual(record["legal_window_bytes"], 4092 * 1024 ** 2)
        self.assertEqual(record["probe_bytes"], 32)
        for name in ("submitted", "completed", "probe_submitted", "probe_completed"):
            self.assertEqual(record[name], 8)
        for name in ("warmup_submitted", "warmup_completed", "dispatch_submitted", "dispatch_completed",
                     "combine_submitted", "combine_completed"):
            self.assertEqual(record[name], 0)
        self.assertEqual(record["kernel_scope"], "memory-boundary")
        self.assertEqual(record["native_status"], "boundary-probe")
        self.assertEqual(record["cleanup"], "complete")
        self.assertEqual(len(record["points"]), 8)
        offsets = (764 * 1024, 816 * 1024 - 32, 1024 * 1024, 4092 * 1024 ** 2 - 32)
        patterns = set()
        for point, (window, index) in zip(record["points"], ((w, i) for w in range(2) for i in range(4))):
            self.assertEqual((point["window"], point["index"]), (window, index))
            self.assertEqual(point["offset"], window * 4 * 1024 ** 3 + offsets[index])
            self.assertEqual(point["pattern"], 0xC7000101 + window * 0x100 + index * 0x10)
            patterns.add(point["pattern"])
            for key in ("initial_zero", "device_initial_zero", "device_readback", "host_readback",
                        "guard_before_zero", "final_readback"):
                self.assertIs(point[key], True)
            self.assertIs(point["guard_after_checked"], index != 3)
            self.assertIs(point["guard_after_zero"], True if index != 3 else None)
        self.assertEqual(len(patterns), 8)
        calls = self.trace_lines(rank)
        self.assertEqual(calls.count("combine-boundary-launch"), 8)
        self.assertEqual(calls.count("sync"), 8)
        self.assertEqual(calls.count("combine-boundary-host-read"), 66)
        self.assertEqual(calls.count("comm-init"), 1)
        self.assertEqual(calls.count("create-stream"), 1)
        self.assertEqual(calls.count("malloc"), 1)
        self.assertEqual(calls.count("free"), 1)
        self.assertNotIn("launch", calls)
        self.assertNotIn("combine-launch", calls)
        self.assertNotIn("packet-read", calls)
        last_sync = max(i for i, call in enumerate(calls) if call == "sync")
        # The final verification is after every probe; the last probe itself
        # contributes two reads before the complete 22-location final sweep.
        self.assertEqual(calls[last_sync:].count("combine-boundary-host-read"), 24)
        self.assertGreater(calls.index("free"), last_sync)
        self.assertGreater(calls.index("comm-destroy"), calls.index("free"))
        self.assertEqual(calls[-1], "finalize")
        self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
        self.assertNotIn("EP_SERVER_DEDUP_COMBINE_CHAIN_PASS", output)

    def test_both_windows_points_guards_and_final_sweep_on_same_consumer(self):
        for dtype in ("bf16", "fp16"):
            with self.subTest(dtype=dtype):
                for rank, process in enumerate(self.run_ranks({"TILEXR_VALIDATION_DTYPE": dtype})):
                    self.check_probe(rank, process)
                self.reset_case()

    def test_probe_never_launches_business_operators(self):
        for mode in ("dispatch", "both"):
            for rank, process in enumerate(self.run_ranks({"TILEXR_VALIDATION_OPERATOR_MODE": mode,
                    "TILEXR_VALIDATION_BS": "1"})):
                self.check_probe(rank, process)
            self.reset_case()

    def test_default_selector_keeps_c5_batches_in_all_three_modes(self):
        self.environment.pop("TILEXR_VALIDATION_BOUNDARY_PROBE")
        for mode in ("dispatch", "combine", "both"):
            for dtype in ("bf16", "fp16"):
                env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode,
                           TILEXR_VALIDATION_LOOPS="5", TILEXR_VALIDATION_WARMUP="2", TILEXR_VALIDATION_DTYPE=dtype)
                for rank, process in enumerate(self.run_ranks(env)):
                    output, errors = process.communicate(timeout=20)
                    self.assertEqual(process.returncode, 0, output + errors)
                    marker = "EP_SERVER_DEDUP_DISPATCH_PASS" if mode == "dispatch" else "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS"
                    record, = self.records(output, marker)
                    self.assertEqual(record["checkpoint"], CHECKPOINT)
                    self.assertIs(record["boundary_probe"], False)
                    self.assertEqual((record["submitted"], record["completed"]), (5, 5))
                    self.assertEqual((record["warmup_submitted"], record["warmup_completed"]), (2, 2))
                    self.assertEqual(record["stream_synchronizations"], 1)
                    calls = self.trace_lines(rank)
                    self.assertNotIn("combine-boundary-launch", calls)
                    self.assertEqual(calls.count("launch"), 7 if mode != "combine" else 0)
                    self.assertEqual(calls.count("combine-launch"), 7 if mode != "dispatch" else 0)
                    self.assertEqual(calls.count("sync"), 1)
                    self.assertEqual(calls[-1], "finalize")
                    self.assertNotIn(MARKER, output)
                self.reset_case()

    def test_invalid_configuration_fails_before_acl(self):
        cases = [dict(TILEXR_VALIDATION_BOUNDARY_PROBE="2"), dict(TILEXR_VALIDATION_LOOPS="2"),
                 dict(TILEXR_VALIDATION_WARMUP="1"), dict(TILEXR_VALIDATION_RUN_MODE="performance"),
                 dict(TILEXR_VALIDATION_RUN_MODE="breakdown"),
                 dict(TILEXR_VALIDATION_CHECKPOINT="ep-sdedup-combine-p1-chain-multi")]
        for invalid in cases:
            with self.subTest(invalid=invalid):
                process = self.start(0, environment=invalid)
                output, errors = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0, output + errors)
                self.assertIn("stage=config", errors)
                self.assertNotIn("_PASS ", output)
                self.assertEqual(self.trace_lines(0), [])

    def test_omissions_corruption_window_alias_and_late_guard_write_fail(self):
        faults = ("initial", "host-no-read", "no-launch", "no-initial-read", "no-write", "no-readback",
                  "guard-before", "guard-after", "wrong-window", "alias", "old-guard")
        for fault in faults:
            with self.subTest(fault=fault):
                for rank, process in enumerate(self.run_ranks({"TEST_COMBINE_BOUNDARY_FAULT": fault})):
                    output, errors = process.communicate(timeout=20)
                    self.assertNotEqual(process.returncode, 0, output + errors)
                    self.assertNotIn(MARKER, output)
                    self.assertIn("cleanup=deferred-to-process-exit", errors)
                    self.assertNotIn("free", self.trace_lines(rank))
                    if fault in ("alias", "old-guard"):
                        self.assertIn("stage=boundary-final-reread", errors)
                        self.assertEqual(self.trace_lines(rank).count("combine-boundary-launch"), 8)
                self.reset_case()

    def test_capacity_and_host_device_mapping_mismatch_prevent_launch(self):
        for fault in ("TEST_SMALL_PEER", "TEST_LARGE_PEER", "TEST_MISSING_PEER", "TEST_DEVICE_MAPPING_MISMATCH"):
            for rank, process in enumerate(self.run_ranks({fault: "1"})):
                output, errors = process.communicate(timeout=15)
                self.assertNotEqual(process.returncode, 0, output + errors)
                self.assertNotIn(MARKER, output)
                self.assertNotIn("combine-boundary-launch", self.trace_lines(rank))
            self.reset_case()

    def test_launch_sync_and_cleanup_failures_never_publish_success(self):
        for stage, failed_call in (("combine-boundary-launch", 5), ("sync", 5), ("comm-destroy", 1)):
            with self.subTest(stage=stage):
                processes = [self.start(rank, environment=({"TEST_FAIL_STAGE": stage,
                    "TEST_FAIL_CALL": str(failed_call)} if rank == 1 else {})) for rank in range(16)]
                output, errors = processes[1].communicate(timeout=20)
                self.assertNotEqual(processes[1].returncode, 0, output + errors)
                self.assertNotIn(MARKER, output)
                self.assertIn("ret=91", errors)
                calls = self.trace_lines(1)
                if stage != "comm-destroy":
                    self.assertIn("cleanup=deferred-to-process-exit", errors)
                    self.assertNotIn("free", calls)
                    self.assertNotIn("comm-destroy", calls)
                else:
                    self.assertEqual(calls[-1], "finalize")
                self.reset_case()


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineBoundaryTests(name) for name in sorted(CombineBoundaryTests.__dict__)
                              if name.startswith("test_"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer")
    options, unittest_args = parser.parse_known_args()
    CONSUMER = options.consumer
    program = unittest.main(argv=[__file__] + unittest_args, exit=False)
    if program.result.wasSuccessful() and not program.result.skipped:
        print("EP_SERVER_DEDUP_COMBINE_BOUNDARY_HOST_PASS test=lifecycle", flush=True)
    else:
        raise SystemExit(1)
