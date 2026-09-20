#!/usr/bin/env python3
"""Combine C1 delivery gate: build provenance, exact markers and failure reports."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import workflow


class CombineCommitValidationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tilexr-combine-c1-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("TILEXR_VALIDATION_")}
        environment.update(TILEXR_VALIDATION_ROOT=str(self.root),
                           TILEXR_VALIDATION_CHECKPOINT=workflow.COMBINE_ABI_HOST_CHECKPOINT,
                           TILEXR_VALIDATION_BUILD_MODE="host")
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        self.names = sorted(workflow.COMBINE_ABI_HOST_TESTS)
        self.commands = []
        self.behavior = "pass"
        self.real_execute = workflow.execute

    def execute(self, argv, log, **kwargs):
        args = [str(arg) for arg in argv]
        self.commands.append(args)
        work = self.root / "build/validation-combine-c1/host"
        if args[0] not in ("cmake", "ctest", "g++"):
            return self.real_execute(argv, log, **kwargs)
        if self.behavior == "configure-fails" and "-S" in args:
            return self.real_execute([sys.executable, "-c", "raise SystemExit(7)"], log, **kwargs)
        if "--build" in args:
            self.assertEqual(set(args[args.index("--target") + 1:]), workflow.COMBINE_ABI_HOST_TESTS)
            work.mkdir(parents=True, exist_ok=True)
            for name in self.names:
                marker = "EP_SERVER_DEDUP_COMBINE_ABI_HOST_PASS test=" + name.rsplit("combine_", 1)[1]
                if self.behavior == "wrong-marker":
                    marker = marker.replace("COMBINE_", "")
                program = work / name
                program.write_text("#!/usr/bin/env python3\nprint(" + repr(marker) + ")\n", encoding="utf-8")
                program.chmod(0o755)
        output = "fixture command completed\n"
        if args[0] == "ctest":
            self.assertIn("^" + workflow.COMBINE_ABI_HOST_CHECKPOINT + "$", args)
            output = json.dumps({"tests": [{"name": name, "command": [str(work / name)]}
                                           for name in self.names]})
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        Path(log).write_text(output, encoding="utf-8")
        return output

    def run_checkpoint(self):
        output = io.StringIO()
        with mock.patch.object(workflow, "require_tools"), \
                mock.patch.object(workflow, "execute", side_effect=self.execute), \
                mock.patch.object(sys, "argv", ["workflow.py", "commit-validate"]), \
                redirect_stdout(output), redirect_stderr(output):
            code = workflow.main()
        reports = list((self.root / "run/validation").glob("commit-*/summary.json"))
        self.assertEqual(len(reports), 1)
        self.assertFalse((self.root / "run/validation/.operation.lock").exists())
        return code, workflow.read_json(reports[0]), output.getvalue()

    def test_success_runs_exact_three_tests_and_preserves_dispatch_installation(self):
        dispatch = self.root / "install/validation/manifest.json"
        receipt = self.root / "run/validation/sync.json"
        for path in (dispatch, receipt):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("old Dispatch evidence", encoding="utf-8")
        code, summary, output = self.run_checkpoint()
        self.assertEqual(code, 0)
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual(summary["expected_tests"], self.names)
        self.assertEqual(summary["checkpoint"], workflow.COMBINE_ABI_HOST_CHECKPOINT)
        self.assertEqual(summary["source_provenance"], "file-copy")
        self.assertIn("COMMIT_VALIDATION_PASS", output)
        for path in (dispatch, receipt):
            self.assertEqual(path.read_text(), "old Dispatch evidence")
        config = workflow.Config("commit-validate")
        self.assertEqual(set(workflow.verify_build(config)["artifacts"]), {"bin/" + name for name in self.names})
        self.assertFalse(any("--install" in command or command[0] == "ssh" for command in self.commands))

    def test_zero_exit_with_dispatch_marker_is_not_combine_success(self):
        self.behavior = "wrong-marker"
        code, summary, output = self.run_checkpoint()
        self.assertEqual(code, workflow.FAILURE_CODES["LOG_PARSE_FAILED"])
        self.assertEqual(summary["status"], "FAIL")
        self.assertEqual(len(summary["tests"]), 1)
        self.assertIn("COMMIT_VALIDATION_FAIL", output)

    def test_failed_build_reports_failure_and_never_runs_old_installation(self):
        self.behavior = "configure-fails"
        config = workflow.Config("commit-validate")
        workflow.write_json(config.install / "manifest.json", {"stale": True})
        code, summary, output = self.run_checkpoint()
        self.assertEqual(code, workflow.FAILURE_CODES["BUILD_FAILED"])
        self.assertEqual(summary["tests"], [])
        self.assertEqual(summary["errors"][0]["returncode"], 7)
        self.assertFalse((config.install / "manifest.json").exists())
        self.assertIn("COMMIT_VALIDATION_FAIL", output)

    def test_incomplete_ctest_catalog_does_not_pass(self):
        self.names.pop()
        code, summary, _ = self.run_checkpoint()
        self.assertNotEqual(code, 0)
        self.assertEqual(summary["tests"], [])
        self.assertIn("必需测试缺失", summary["errors"][0]["message"])

    def test_build_budget_expiry_stops_before_starting_another_command(self):
        with mock.patch.object(workflow.time, "monotonic", side_effect=[0, 901]):
            code, summary, _ = self.run_checkpoint()
        self.assertEqual(code, workflow.FAILURE_CODES["BUILD_FAILED"])
        self.assertTrue(summary["errors"][0]["timeout"])
        self.assertEqual(self.commands, [])


if __name__ == "__main__":
    unittest.main()
