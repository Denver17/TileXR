"""Auto-layout consumer contracts with Host substitutes, not device proof."""
from pathlib import Path
import unittest

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from server_dedup_consumer_fixture import ConsumerFixture, COMBINE_MARKER, run_tests
from profiling.records import load_record_files


class AutoDedupConsumerTests(ConsumerFixture, unittest.TestCase):
    compile_definitions = ["-DTILEXR_EP_SERVER_DEDUP_ENABLE=1", "-DTILEXR_EP_SERVER_DEDUP_AUTO_ENABLE=1"]

    def setUp(self):
        super().setUp()
        self.configure_combine()
        self.environment.update(TILEXR_VALIDATION_H="17", TILEXR_VALIDATION_TOPK="6",
                                TEST_FORBID_UNUSED_METADATA_READ="1")

    def test_original_bs_controls_routes_and_final_aggregation(self):
        for bs in (1, 31, 32, 33, 256):
            for mode in ("combine", "both"):
                with self.subTest(bs=bs, mode=mode):
                    env = dict(TILEXR_VALIDATION_BS=str(bs), TILEXR_VALIDATION_OPERATOR_MODE=mode)
                    for rank, process in enumerate(self.run_ranks(env)):
                        output, errors = process.communicate(timeout=60)
                        self.assertEqual(process.returncode, 0, output + errors)
                        record, = self.records(output, COMBINE_MARKER)
                        self.assertIs(record["server_dedup_enabled"], bs > 32)
                        self.assertEqual(record["combine_receive_module"], "M5" if bs > 32 else "M6")
                        self.assertTrue(record["combine_y_out_validated"])
                        self.assertTrue(record["combine_result_flags_cleared"])
                        self.assertTrue(record["combine_control_reserved_unchanged"])
                        self.assertTrue(record["combine_stage_validated"])
                        if bs <= 32:
                            self.assertEqual(record["combine_type1_rows"], record["combine_token_num"])
                            self.assertEqual(record["combine_direct_received_rows"], bs * 6)
                            for key in ("combine_type2_rows", "combine_type3_rows",
                                        "combine_partial_received_rows", "combine_control_publish_bytes"):
                                self.assertEqual(record[key], 0, key)
                        self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                    self.reset_case()

    def test_direct_topk32_consumes_every_route(self):
        for mode in ("combine", "both"):
            with self.subTest(mode=mode):
                env = dict(TILEXR_VALIDATION_BS="1", TILEXR_VALIDATION_TOPK="32",
                           TILEXR_VALIDATION_MOE_EXPERT_NUM="32", TILEXR_VALIDATION_OPERATOR_MODE=mode)
                for process in self.run_ranks(env):
                    output, errors = process.communicate(timeout=60)
                    self.assertEqual(process.returncode, 0, output + errors)
                    record, = self.records(output, COMBINE_MARKER)
                    self.assertFalse(record["server_dedup_enabled"])
                    self.assertEqual(record["combine_receive_module"], "M6")
                    self.assertEqual(record["combine_direct_received_rows"], 32)
                    self.assertTrue(record["combine_y_out_validated"])
                    self.assertTrue(record["combine_result_flags_cleared"])
                self.reset_case()

    def test_direct_quantized_dispatch_and_two_dtypes(self):
        for dtype in ("bf16", "fp16"):
            with self.subTest(dtype=dtype):
                env = dict(TILEXR_VALIDATION_BS="32", TILEXR_VALIDATION_OPERATOR_MODE="both",
                           TILEXR_VALIDATION_DTYPE=dtype, TILEXR_VALIDATION_DISPATCH_QUANT_MODE="4",
                           TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="1",
                           TILEXR_VALIDATION_RUN_MODE="breakdown")
                processes = [self.start(rank, environment=dict(env,
                    TILEXR_PROFILING_COMBINE_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.combine.records.v1")))
                    for rank in range(16)]
                for process in processes:
                    output, errors = process.communicate(timeout=60)
                    self.assertEqual(process.returncode, 0, output + errors)
                    record, = self.records(output, COMBINE_MARKER)
                    self.assertFalse(record["server_dedup_enabled"])
                    self.assertEqual(record["combine_receive_module"], "M6")
                    self.assertTrue(record["combine_y_out_validated"])
                    self.assertEqual(record["completed"], 3)
                    self.assertEqual(record["warmup_completed"], 1)
                    captures = self.records(output, "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS")
                    capture, = [item for item in captures if item["operator"] == "combine"]
                    raw, = load_record_files([Path(capture["records_path"])])
                    events = {item.event_id for stream in raw.streams for item in stream.records}
                    self.assertIn(0x302a, events)  # M6 begin
                    self.assertIn(0x302b, events)  # M6 end
                    self.assertNotIn(0x301f, events)  # M5 begin
                self.reset_case()


if __name__ == "__main__":
    run_tests(AutoDedupConsumerTests)
