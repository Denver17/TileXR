"""C6 shared-consumer capture lifecycle; Host substitutes are not NPU evidence."""
import argparse
import os
from pathlib import Path
import tempfile
import unittest
from uuid import NAMESPACE_URL, uuid5

import test_server_dedup_combine_chain as single
import operator_measurements

CHECKPOINT = "ep-sdedup-combine-p1-profile"
PROFILE_MARKER = "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS"
RUN_ID = "run-identity"
CAPTURE = str(uuid5(NAMESPACE_URL, "tilexr-validation:" + RUN_ID))
CONSUMER = None


class CombineProfileTests(single.CombineChainTests):
    @classmethod
    def setUpClass(cls):
        if CONSUMER is None:
            super().setUpClass()
            return
        cls.executable = Path(CONSUMER).resolve()
        if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
            raise RuntimeError("C6 consumer is not executable")
        cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-combine-profile-")
        cls.folder = Path(cls.temp.name)

    def setUp(self):
        super().setUp()
        self.environment.update(TILEXR_INTERNAL_ACCEPTANCE_STAGE=CHECKPOINT,
            TILEXR_VALIDATION_LOOPS="2", TILEXR_VALIDATION_WARMUP="1")

    def start_profile(self, rank, mode, run_mode, **extra):
        env = dict(TILEXR_VALIDATION_OPERATOR_MODE=mode, TILEXR_VALIDATION_RUN_MODE=run_mode,
            TILEXR_PROFILING_CAPTURE_ID=CAPTURE,
            TILEXR_PROFILING_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.dispatch.records.v1"),
            TILEXR_PROFILING_COMBINE_RECORDS_PATH=str(Path(self.trace.name) / f"rank-{rank}.combine.records.v1"))
        env.update(extra)
        return self.start(rank, environment=env)

    def parse_evidence(self, rank, output, mode):
        config = dict(operator_mode=mode, run_mode="breakdown", loops=2, warmup=1)
        markers = self.records(output, PROFILE_MARKER)
        active = operator_measurements.active_operators(config)
        self.assertEqual({m["operator"] for m in markers}, set(active))
        self.assertEqual(len(markers), len(active))
        result = dict(rank=rank, run_id=RUN_ID, operator_measurements={})
        for marker in markers:
            operator = marker["operator"]
            self.assertEqual(marker["checkpoint"], CHECKPOINT)
            self.assertEqual(marker["capture_id"], CAPTURE)
            self.assertEqual((marker["launches"], marker["captured_launch"]), (3, 2))
            self.assertEqual(marker["captured_launches"], 1)
            self.assertEqual(marker["capture_mode"], "latest_launch")
            evidence = operator_measurements.breakdown_record(marker["records_path"], config, rank, CAPTURE,
                Path(self.trace.name) / f"profile-rank-{rank}" / operator,
                operator=operator, run_id=RUN_ID, kernel_sha="a" * 64)
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
                        success = "EP_SERVER_DEDUP_DISPATCH_PASS" if mode == "dispatch" else single.MARKER
                        record, = self.records(output, success)
                        self.assertEqual(record["checkpoint"], CHECKPOINT)
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
                                self.assertEqual(c["sample_us"], [50.0])
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
                        if fault in ("missing", "capture"):
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
            self.assertNotIn(single.MARKER, output)
            self.assertIn("stage=cleanup", errors)
            self.assertEqual(self.trace_lines(rank)[-1], "finalize")


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineProfileTests(name) for name in sorted(CombineProfileTests.__dict__)
                              if name.startswith("test_"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer")
    args, rest = parser.parse_known_args()
    CONSUMER = args.consumer
    program = unittest.main(argv=[__file__] + rest, exit=False)
    if program.result.wasSuccessful() and not program.result.skipped:
        print("EP_SERVER_DEDUP_COMBINE_PROFILE_PASS test=lifecycle", flush=True)
    else:
        raise SystemExit(1)
