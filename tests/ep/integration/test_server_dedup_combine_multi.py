"""C5: continuous batches through the shared consumer using Host substitutes."""
import argparse
import os
from pathlib import Path
import tempfile
import unittest

import test_server_dedup_combine_chain as single_tests


CHECKPOINT = "ep-sdedup-combine-p1-chain-multi"
MARKER = single_tests.MARKER
CONSUMER = None


class CombineMultiTests(single_tests.CombineChainTests):
    @classmethod
    def setUpClass(cls):
        if CONSUMER is None:
            super().setUpClass()
            return
        cls.executable = Path(CONSUMER).resolve()
        if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
            raise RuntimeError(f"C5 consumer is not executable: {cls.executable}")
        cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-combine-multi-")
        cls.folder = Path(cls.temp.name)

    def setUp(self):
        super().setUp()
        self.environment.update(TILEXR_INTERNAL_ACCEPTANCE_STAGE=CHECKPOINT,
                                TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="2")

    def check_success(self, rank, process, mode, quant=0, loops=3, warmup=2):
        output, errors = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 0, output + errors)
        total = loops + warmup
        dispatch = mode != "combine"
        combine = mode != "dispatch"
        records = self.records(output, MARKER if combine else "EP_SERVER_DEDUP_DISPATCH_PASS")
        record, = records
        self.assertEqual(record["checkpoint"], CHECKPOINT)
        self.assertEqual(record["operator_mode"], mode)
        self.assertEqual(record["dispatch_quant_mode"], quant)
        self.assertEqual(record["combine_quant_mode"], 0)
        self.assertEqual(record["cleanup"], "complete")
        self.assertEqual((record["submitted"], record["completed"]), (loops, loops))
        self.assertEqual((record["warmup_submitted"], record["warmup_completed"]), (warmup, warmup))
        self.assertEqual(record["stream_synchronizations"], 1)
        dispatch_last = (2 * total - 1 if combine else total) if dispatch else 0
        combine_last = (2 * total if dispatch else total) if combine else 0
        dispatch_mask = (2 if combine else 3 if total > 1 else 2) if dispatch else 0
        combine_mask = (1 if dispatch else 3 if total > 1 else 2) if combine else 0
        if combine:
            for prefix, active in (("dispatch", dispatch), ("combine", combine)):
                for suffix in ("submitted", "completed"):
                    self.assertEqual(record[f"{prefix}_{suffix}"], loops if active else 0)
                    self.assertEqual(record[f"{prefix}_warmup_{suffix}"], warmup if active else 0)
            self.assertEqual(record["dispatch_first_magic"], 1 if dispatch else 0)
            self.assertEqual(record["combine_first_magic"], 2 if dispatch else 1)
            self.assertEqual(record["dispatch_last_magic"], dispatch_last)
            self.assertEqual(record["combine_last_magic"], combine_last)
            self.assertEqual(record["dispatch_window_mask"], dispatch_mask)
            self.assertEqual(record["combine_window_mask"], combine_mask)
            self.assertEqual(record["dispatch_window_offset"], (dispatch_last % 2) * 4 * 1024 ** 3)
            self.assertEqual(record["combine_window_offset"], (combine_last % 2) * 4 * 1024 ** 3)
            self.assertEqual(record["combine_input_check"], "dispatch-final-reference" if dispatch else "full-snapshot")
            self.assertIs(record["combine_inputs_unchanged"], not dispatch)
            self.assertIs(record["independent_expert_out_unchanged"], not dispatch or quant == 4)
            for key in ("combine_empty", "y_out_unchanged"):
                self.assertIs(record[key], True)
            for key in ("five_tables_linked", "send_counts_linked"):
                self.assertIs(record[key], dispatch)
            self.assertEqual(record["combine_delay_us"], 50)
            self.assertEqual(record["expert_out_source"], "standalone_expert_out" if not dispatch else
                             "separate_expert_out" if quant else "dispatch_expand_x")
        else:
            self.assertEqual(record["dispatch_first_magic"], 1)
            self.assertEqual(record["dispatch_window_mask"], dispatch_mask)
            self.assertNotIn(MARKER, output)
        d_records = self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")
        self.assertEqual(len(d_records), int(dispatch))
        if dispatch:
            self.assertEqual(d_records[0]["last_magic"], dispatch_last)
            self.assertEqual(d_records[0]["window_offset"], dispatch_last % 2 * 4 * 1024 ** 3)
            for key in ("five_tables_validated", "member_addresses_validated"):
                self.assertIs(d_records[0][key], True)
        calls = self.trace_lines(rank)
        launches = [call for call in calls if call in ("launch", "combine-launch")]
        one_round = (["launch"] if dispatch else []) + (["combine-launch"] if combine else [])
        self.assertEqual(launches, one_round * total)
        self.assertEqual(calls.count("sync"), 1)
        self.assertEqual(calls.count("comm-init"), 1)
        self.assertEqual(calls.count("create-stream"), 1)
        self.assertEqual(calls.count("malloc"), 8 if not dispatch else 14 if combine and quant else
                         12 if combine or quant else 11)
        self.assertEqual(calls.count("free"), calls.count("malloc"))
        begin, end = calls.index(one_round[0]), calls.index("sync")
        self.assertEqual(calls[begin:end], one_round * total)
        self.assertLess(max(i for i, call in enumerate(calls) if call == "malloc"), begin)
        self.assertGreater(calls.index("free"), end)
        self.assertGreater(calls.index("comm-destroy"), max(i for i, call in enumerate(calls) if call == "free"))
        self.assertEqual(calls[-1], "finalize")
        self.assertEqual("packet-read" in calls[end:], dispatch)

    def test_continuous_batch_matrix_and_each_operator_window(self):
        cases = [(mode, dtype, 0, "e4m3") for mode in ("dispatch", "combine", "both")
                 for dtype in ("bf16", "fp16")]
        cases += [("both", "bf16", 4, "e4m3"), ("both", "fp16", 4, "e5m2")]
        for mode, dtype, quant, fmt in cases:
            # Odd final generation with warmup, then even without warmup.
            for loops, warmup in ((3, 2), (4, 0)):
                with self.subTest(mode=mode, dtype=dtype, quant=quant, loops=loops, warmup=warmup):
                    env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_DTYPE=dtype,
                               TILEXR_VALIDATION_DISPATCH_QUANT_MODE=str(quant), TILEXR_VALIDATION_FP8_FORMAT=fmt,
                               TILEXR_VALIDATION_LOOPS=str(loops), TILEXR_VALIDATION_WARMUP=str(warmup))
                    for rank, process in enumerate(self.run_ranks(env)):
                        self.check_success(rank, process, mode, quant, loops, warmup)
                    self.reset_case()

    def test_combine_ignores_dispatch_quantization_and_partial_epoch(self):
        env = dict(TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_DISPATCH_QUANT_MODE="4",
                   TILEXR_VALIDATION_BS="1", TILEXR_VALIDATION_TOPK="3", TILEXR_VALIDATION_ROUTE_MODE="balance_shift")
        for rank, process in enumerate(self.run_ranks(env)):
            self.check_success(rank, process, "combine", 4)

    def test_invalid_c5_configuration_fails_before_acl(self):
        cases = [{"TILEXR_VALIDATION_LOOPS": "0"}, {"TILEXR_VALIDATION_RUN_MODE": "performance"},
                 {"TILEXR_VALIDATION_RUN_MODE": "breakdown"}, {"TILEXR_VALIDATION_COMBINE_QUANT_MODE": "4"}]
        for mode in ("dispatch", "combine", "both"):
            for invalid in cases:
                with self.subTest(mode=mode, invalid=invalid):
                    process = self.start(0, environment=dict(invalid, TILEXR_VALIDATION_OPERATOR_MODE=mode))
                    output, errors = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0, output + errors)
                    self.assertNotIn("_PASS ", output)
                    self.assertIn("stage=config", errors)
                    self.assertEqual(self.trace_lines(0), [])

    def test_mid_batch_launch_or_sync_failure_keeps_live_resources(self):
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

    def assert_completed_failure(self, process, rank):
        output, errors = process.communicate(timeout=20)
        self.assertNotEqual(process.returncode, 0, output + errors)
        self.assertNotIn(MARKER, output)
        self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
        self.assertEqual(self.trace_lines(rank)[-1], "finalize")
        return output, errors

    def test_full_snapshots_and_independent_tensors_detect_head_tail_changes(self):
        standalone = ("y_out", "expert_out", "send_counts", "assist", "token_type", "relay", "source_mask", "weights")
        cases = [("combine", 0, fault) for fault in standalone]
        cases += [("both", 0, "y_out"), ("both", 4, "expert_out"), ("both", 4, "y_out")]
        for mode, quant, name in cases:
            for fault in (name, name + "_tail"):
                with self.subTest(mode=mode, quant=quant, fault=fault):
                    env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_DISPATCH_QUANT_MODE=str(quant),
                               TEST_COMBINE_FAULT=fault, TEST_COMBINE_FAULT_CALL="5")
                    for rank, process in enumerate(self.run_ranks(env)):
                        self.assert_completed_failure(process, rank)
                    self.reset_case()

    def test_early_independent_mutation_survives_to_batch_end(self):
        for mode, quant, fault in (("combine", 0, "y_out"), ("both", 0, "y_out_tail"),
                                  ("both", 4, "expert_out_tail")):
            with self.subTest(mode=mode, fault=fault):
                env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_DISPATCH_QUANT_MODE=str(quant),
                           TEST_COMBINE_FAULT=fault, TEST_COMBINE_FAULT_CALL="2")
                for rank, process in enumerate(self.run_ranks(env)):
                    self.assert_completed_failure(process, rank)
                    self.assertEqual(self.trace_lines(rank).count("combine-launch"), 5)
                self.reset_case()

    def test_final_dispatch_outputs_and_actual_dispatch_window_are_checked(self):
        for fault in ({"TEST_PACKET_FAULT": "hidden"}, {"TEST_PACKET_FAULT": "destination-source"},
                      {"TEST_WINDOW_FAULT": "1"}):
            with self.subTest(fault=fault):
                for rank, process in enumerate(self.run_ranks(fault)):
                    self.assert_completed_failure(process, rank)
                    self.assertEqual(self.trace_lines(rank).count("combine-launch"), 5)
                    if "TEST_WINDOW_FAULT" in fault:
                        self.assertIn("wrong-bank-read", self.trace_lines(rank))
                self.reset_case()

    def test_cleanup_failure_suppresses_success_after_complete_batch(self):
        for mode in ("combine", "both"):
            env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TEST_FAIL_STAGE="comm-destroy")
            for rank, process in enumerate(self.run_ranks(env)):
                _, errors = self.assert_completed_failure(process, rank)
                self.assertIn("stage=cleanup", errors)
                self.assertEqual(self.trace_lines(rank).count("sync"), 1)
            self.reset_case()


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineMultiTests(name) for name in sorted(CombineMultiTests.__dict__)
                              if name.startswith("test_"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer")
    options, unittest_args = parser.parse_known_args()
    CONSUMER = options.consumer
    program = unittest.main(argv=[__file__] + unittest_args, exit=False)
    if program.result.wasSuccessful() and not program.result.skipped:
        print("EP_SERVER_DEDUP_COMBINE_MULTI_PASS test=lifecycle", flush=True)
    else:
        raise SystemExit(1)
