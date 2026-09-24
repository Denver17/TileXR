#!/usr/bin/env python3
"""C7 independent boundary probe evidence and retained production Dispatch gate."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
import threading
import time
import unittest
from unittest import mock

import workflow
from test_combine_multi import CombineMultiTests


class CombineBoundaryTests(CombineMultiTests):
    def setUp(self):
        super().setUp()
        os.environ["TILEXR_INTERNAL_ACCEPTANCE_STAGE"] = workflow.COMBINE_BOUNDARY_CHECKPOINT
        self.manifest["checkpoint"] = workflow.COMBINE_BOUNDARY_CHECKPOINT
        self.manifest["source"]["content_sha256"] = "b" * 64

    def probe_records(self):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BOUNDARY_PROBE": "1",
                "TILEXR_VALIDATION_OPERATOR_MODE": "combine", "TILEXR_VALIDATION_LOOPS": "1",
                "TILEXR_VALIDATION_WARMUP": "0"}):
            config, mappings, records = self.records(False)
        offsets = (1273856, 1372128, 1572864, 4290772960)
        for record in records:
            record.update(native_status="boundary-probe", peer_mem_bytes=8 * 1024 ** 3,
                          buffer_stride=4 * 1024 ** 3, runtime_reserved_bytes=4 * 1024 ** 2,
                          legal_window_bytes=4290772992, probe_bytes=32, probe_submitted=8,
                          probe_completed=8, submitted=8, completed=8, combine_submitted=0,
                          combine_completed=0, points=[])
            for window in range(2):
                for index, offset in enumerate(offsets):
                    record["points"].append(dict(window=window, index=index,
                        offset=window * 4294967296 + offset,
                        pattern=0xC7000101 + window * 0x100 + index * 0x10,
                        initial_zero=True, device_initial_zero=True, device_readback=True,
                        host_readback=True, guard_before_zero=True, guard_after_checked=index != 3,
                        guard_after_zero=True if index != 3 else None, final_readback=True))
        return config, mappings, records

    def test_selector_is_internal_functional_and_isolated(self):
        config = self.config()
        self.assertIs(config.chain["boundary_probe"], False)
        self.assertEqual(config.install.name, "validation-combine-c7")
        self.assertEqual(len(workflow.COMBINE_TESTS[config.checkpoint]), 14)
        config, _, _ = self.probe_records()
        self.assertIs(config.chain["boundary_probe"], True)
        self.assertEqual(config.chain["kernel_scope"], "memory-boundary")
        for changes in ({"BOUNDARY_PROBE": "true"}, {"BOUNDARY_PROBE": "2"},
                        {"BOUNDARY_PROBE": "1"}, {"RUN_MODE": "performance"},
                        {"RUN_MODE": "breakdown"},
                        {"CHECKPOINT": workflow.COMBINE_MULTI_CHECKPOINT, "BOUNDARY_PROBE": "1"}):
            with self.subTest(changes=changes), mock.patch.dict(os.environ, {
                    "TILEXR_VALIDATION_" + key: value for key, value in changes.items()}), self.assertRaises(ValueError):
                self.config()
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BREAKDOWN": "1"}), self.assertRaises(ValueError):
            workflow.Config("build")

    def test_boundary_requires_all_eight_exact_independent_points(self):
        config, mappings, records = self.probe_records()
        workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
        for path, value in ((["submitted"], 1), (["probe_completed"], 7), (["combine_submitted"], 1),
                            (["dispatch_submitted"], True), (["peer_mem_bytes"], 4 * 1024 ** 3),
                            (["runtime_reserved_bytes"], 0), (["boundary_probe"], 1),
                            (["chain", "boundary_probe"], False), (["cleanup"], "failed"),
                            (["points"], records[0]["points"][:-1])):
            changed = copy.deepcopy(records)
            target = changed[0]
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(path=path), self.assertRaises(workflow.Failure):
                workflow.validate_rank_records(changed, mappings, "run", self.manifest, config.chain)
        for index in range(8):
            for key, value in (("offset", 0), ("pattern", 0), ("window", True),
                               ("initial_zero", False), ("device_initial_zero", False),
                               ("device_readback", False), ("host_readback", False),
                               ("guard_before_zero", False), ("final_readback", False),
                               ("guard_after_checked", index % 4 == 3),
                               ("guard_after_zero", True if index % 4 == 3 else None)):
                changed = copy.deepcopy(records)
                changed[0]["points"][index][key] = value
                with self.subTest(index=index, key=key), self.assertRaises(workflow.Failure):
                    workflow.validate_rank_records(changed, mappings, "run", self.manifest, config.chain)
        del records[0]["points"][3]["guard_after_zero"]
        with self.assertRaises(workflow.Failure):
            workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)

    def test_probe_parser_rejects_business_launch_missing_and_duplicate_markers(self):
        config, mappings, records = self.probe_records()
        for fault in (None, "missing", "duplicate", "dispatch", "combine"):
            def execute(argv, log, **kwargs):
                self.assertEqual(kwargs["env"]["TILEXR_VALIDATION_BOUNDARY_PROBE"], "1")
                record = dict(records[int(argv[-2])])
                chain = record.pop("chain")
                output = workflow.CHAIN_MARKER + json.dumps(chain) + "\n"
                if fault != "missing":
                    output += workflow.COMBINE_BOUNDARY_MARKER + json.dumps(record) + "\n"
                if fault == "duplicate":
                    output += workflow.COMBINE_BOUNDARY_MARKER + json.dumps(record) + "\n"
                if fault in ("dispatch", "combine"):
                    output += (workflow.DISPATCH_MARKER if fault == "dispatch" else workflow.COMBINE_CHAIN_MARKER) + "{}\n"
                return output
            result = dict(errors=[])
            with mock.patch.object(workflow, "execute", side_effect=execute):
                workflow.run_ranks(config, "run", self.manifest, mappings, time.monotonic() + 60,
                                   threading.Event(), result)
            self.assertEqual(bool(result["errors"]), fault is not None, (fault, result))
        remote = workflow.remote_argv(config, config.hosts()[0]["host"], "node")
        self.assertIn("TILEXR_VALIDATION_BOUNDARY_PROBE=1", " ".join(remote))

    def test_default_selector_retains_all_continuous_modes_and_dispatch_oracle(self):
        for operator in ("dispatch", "combine", "both"):
            config, mappings, records = self.multi_records(operator)
            workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
            if operator != "combine":
                for field, value in (("five_tables_validated", False), ("last_magic", 99),
                                     ("boundary_probe", True)):
                    changed = copy.deepcopy(records)
                    target = changed[0] if operator == "dispatch" else changed[0]["dispatch"]
                    target[field] = value
                    with self.subTest(operator=operator, field=field), self.assertRaises(workflow.Failure):
                        workflow.validate_rank_records(changed, mappings, "run", self.manifest, config.chain)

    def test_three_case_gate_single_package_progress_and_first_failure(self):
        for failing_case, wrong_build in ((None, False), (1, False), (2, False), (3, False), (2, True)):
            config = workflow.Config("commit-validate")
            seen = []
            def run_case(case):
                seen.append(dict(case.chain))
                case.last_summary_path = case.logs / "case-run/summary.json"
                passed = len(seen) != failing_case or wrong_build
                workflow.write_json(case.last_summary_path, dict(status="PASS" if passed else "FAIL",
                    rank_size=16, completed_ranks=16 if passed else 15,
                    build_id="wrong" if wrong_build and len(seen) == failing_case else "build"))
                return 0 if passed else 1
            output = io.StringIO()
            before = dict(os.environ)
            identifier = f"boundary-{failing_case}-{wrong_build}"
            with mock.patch.object(workflow, "verify_build", return_value=self.manifest), \
                    mock.patch.object(workflow, "host_validate", return_value=0) as host, \
                    mock.patch.object(workflow, "sync") as sync, \
                    mock.patch.object(workflow, "run", side_effect=run_case), \
                    redirect_stdout(output), redirect_stderr(io.StringIO()):
                code = workflow.combine_chain_commit_validate(config, identifier)
            self.assertEqual(os.environ, before)
            self.assertEqual(code == 0, failing_case is None)
            self.assertEqual(len(seen), failing_case or 3)
            host.assert_called_once()
            sync.assert_called_once()
            self.assertEqual(len(sync.call_args.args[0].hosts()), 2)
            report = workflow.read_json(config.logs / identifier / "summary.json")
            self.assertEqual(report["expected_cases"], 3)
            self.assertEqual(report["source_content_sha256"], "b" * 64)
            self.assertEqual(workflow.read_json(config.logs / identifier / "build-manifest.json"), self.manifest)
            self.assertEqual(len({case["summary"] for case in report["cases"]}), len(seen))
            for index, case in enumerate(seen, 1):
                self.assertEqual((case["loops"], case["warmup"]), (1, 0) if index == 1 else (5, 2))
                self.assertIs(case["boundary_probe"], index == 1)
                self.assertEqual(case["operator_mode"], "combine" if index == 1 else "dispatch")
                self.assertEqual(case["dispatch_quant_mode"], 0)
                self.assertEqual(case["combine_quant_mode"], 0)
                self.assertIn(f"[{index}/3] START", output.getvalue())
                self.assertIn(f"[{index}/3] {'FAIL' if index == failing_case else 'PASS'}", output.getvalue())

    def test_boundary_host_markers_are_unique_and_current(self):
        config = workflow.Config("host-validate")
        for name, marker in ((workflow.COMBINE_BOUNDARY_TEST, "EP_SERVER_DEDUP_COMBINE_BOUNDARY_HOST_PASS test=lifecycle"),
                             (workflow.COMBINE_BOUNDARY_LAUNCH_TEST, "EP_SERVER_DEDUP_COMBINE_BOUNDARY_LAUNCH_PASS"),
                             (workflow.COMBINE_BOUNDARY_KERNEL_TEST, "EP_SERVER_DEDUP_COMBINE_BOUNDARY_KERNEL_PASS")):
            test = dict(name=name, argv=["test-program"])
            for output in (marker + "\n", "", (marker + "\n") * 2):
                with mock.patch.object(workflow, "execute", return_value=output):
                    if output == marker + "\n":
                        workflow.run_validation_test(config, test, config.logs, 120)
                    else:
                        with self.assertRaises(workflow.Failure):
                            workflow.run_validation_test(config, test, config.logs, 120)


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineBoundaryTests(name) for name in CombineBoundaryTests.__dict__ if name.startswith("test_"))


if __name__ == "__main__":
    unittest.main()
