#!/usr/bin/env python3
"""Combine delivery gate: build provenance, exact markers and failure reports."""

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
    checkpoint = workflow.COMBINE_ABI_HOST_CHECKPOINT
    mode = "host"

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tilexr-combine-c1-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("TILEXR_VALIDATION_")}
        environment.update(TILEXR_VALIDATION_ROOT=str(self.root),
                           TILEXR_VALIDATION_CHECKPOINT=self.checkpoint,
                           TILEXR_VALIDATION_BUILD_MODE=self.mode)
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        self.required = workflow.COMBINE_TESTS[self.checkpoint]
        self.names = sorted(self.required)
        self.commands = []
        self.behavior = "pass"
        self.real_execute = workflow.execute

    def execute(self, argv, log, **kwargs):
        args = [str(arg) for arg in argv]
        self.commands.append(args)
        build = workflow.validation_build_directory(workflow.Config("build"))
        work = build / "host"
        if args[0] not in ("cmake", "ctest", "g++"):
            return self.real_execute(argv, log, **kwargs)
        if self.behavior == "configure-fails" and "-S" in args:
            return self.real_execute([sys.executable, "-c", "raise SystemExit(7)"], log, **kwargs)
        if "--build" in args and str(work) in args:
            expected_targets = (self.required - {workflow.COMBINE_DRY_RUN_TEST, workflow.COMBINE_CHAIN_TEST, workflow.COMBINE_MULTI_TEST, workflow.COMBINE_PROFILE_TEST, workflow.COMBINE_BOUNDARY_TEST, workflow.COMBINE_M3_CONSUMER_TEST, workflow.COMBINE_CONFIG_TEST}) | {workflow.COMBINE_DRY_RUN_PROGRAM} if self.checkpoint in (workflow.COMBINE_DRY_RUN_CHECKPOINT, *workflow.COMBINE_DEVICE_CHECKPOINTS) else self.required
            if self.checkpoint in workflow.COMBINE_DEVICE_CHECKPOINTS:
                expected_targets |= {workflow.CHAIN_PROGRAM}
            self.assertEqual(set(args[args.index("--target") + 1:]), expected_targets)
            work.mkdir(parents=True, exist_ok=True)
            if self.checkpoint in workflow.COMBINE_DEVICE_CHECKPOINTS:
                (work / workflow.CHAIN_PROGRAM).write_text("real consumer fixture\n")
            for name in self.names:
                marker = ("EP_SERVER_DEDUP_COMBINE_ABI_HOST_PASS test="
                          if name in workflow.COMBINE_ABI_HOST_TESTS else
                          "EP_SERVER_DEDUP_COMBINE_EMPTY_KERNEL_PASS test=") + name.rsplit("combine_", 1)[1]
                if name == workflow.COMBINE_DRY_RUN_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_DRYRUN_PASS test=configuration"
                if name in (workflow.COMBINE_CHAIN_TEST, workflow.COMBINE_DELAY_TEST):
                    marker = "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS test=" + ("delay" if name == workflow.COMBINE_DELAY_TEST else "lifecycle")
                if name == workflow.COMBINE_MULTI_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_MULTI_PASS test=lifecycle"
                if name == workflow.COMBINE_PROFILE_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_PROFILE_PASS test=lifecycle"
                if name == workflow.COMBINE_PROFILE_REGIONS_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_PROFILE_REGIONS_PASS"
                if name == workflow.COMBINE_PROFILE_KERNEL_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_PROFILE_KERNEL_PASS"
                if name == workflow.COMBINE_PROFILE_LAUNCH_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_EMPTY_KERNEL_PASS test=launch"
                if name == workflow.COMBINE_BOUNDARY_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_BOUNDARY_HOST_PASS test=lifecycle"
                if name == workflow.COMBINE_BOUNDARY_KERNEL_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_BOUNDARY_KERNEL_PASS"
                if name == workflow.COMBINE_BOUNDARY_LAUNCH_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_BOUNDARY_LAUNCH_PASS"
                if name in (workflow.COMBINE_M3_TEST, workflow.COMBINE_M3_CONSUMER_TEST):
                    marker = "EP_SERVER_DEDUP_COMBINE_M3_PASS" + (" test=lifecycle" if name == workflow.COMBINE_M3_CONSUMER_TEST else "")
                if name == workflow.COMBINE_CONFIG_TEST:
                    marker = "EP_SERVER_DEDUP_COMBINE_CONFIG_PASS"
                if self.behavior == "wrong-marker":
                    marker = marker.replace("COMBINE_", "")
                program = work / name
                content = "#!/usr/bin/env python3\nimport os\n"
                if name == workflow.COMBINE_REGISTRATION_TEST:
                    content += ("assert os.environ['TILEXR_EP_SERVER_DEDUP_TEST_DEVICE'] == '3'\n"
                                + "assert os.environ['LD_LIBRARY_PATH'].split(':')[0] == "
                                + repr(str(self.root / "install/validation-combine-c2/lib")) + "\n")
                    if self.behavior == "registration-skip":
                        content += "raise SystemExit(77)\n"
                    if self.behavior == "registration-no-marker":
                        marker = "registration skipped: no A5"
                content += "print(" + repr(marker) + ")\n"
                program.write_text(content, encoding="utf-8")
                program.chmod(0o755)
                if name == workflow.COMBINE_CONFIG_TEST:
                    driver = self.root / "tests/ep/unit/test_server_dedup_combine_config.py"
                    driver.parent.mkdir(parents=True, exist_ok=True)
                    driver.write_text(content)
                if name in (workflow.COMBINE_DRY_RUN_TEST, workflow.COMBINE_CHAIN_TEST, workflow.COMBINE_MULTI_TEST, workflow.COMBINE_PROFILE_TEST, workflow.COMBINE_BOUNDARY_TEST, workflow.COMBINE_M3_CONSUMER_TEST):
                    script = "test_server_dedup_combine_m3.py" if name == workflow.COMBINE_M3_CONSUMER_TEST else "test_server_dedup_combine_dry_run.py" if name == workflow.COMBINE_DRY_RUN_TEST else "test_server_dedup_combine_multi.py" if name == workflow.COMBINE_MULTI_TEST else "test_server_dedup_combine_profile.py" if name == workflow.COMBINE_PROFILE_TEST else "test_server_dedup_combine_boundary.py" if name == workflow.COMBINE_BOUNDARY_TEST else "test_server_dedup_combine_chain.py"
                    driver = self.root / "tests/ep/integration" / script
                    driver.parent.mkdir(parents=True, exist_ok=True)
                    driver.write_text("import pathlib,sys\nassert sys.argv[1] == '--consumer'\nassert pathlib.Path(sys.argv[2]).is_file()\nprint(" + repr(marker) + ")\n")
                    (work / workflow.COMBINE_DRY_RUN_PROGRAM).write_text("consumer fixture\n")
        if "--install" in args:
            self.assertEqual(args[-1], str(build / "production"))
            artifacts = ["lib/libtilexr-ep.so", "lib/libtile-comm.so"]
            artifacts += [f"lib/tilexr/kernels/tilexr_ep_{name}_kernel.o" for name in (
                "dispatch", "combine", "dispatch_memory", "combine_memory", "dispatch_memory_server_dedup",
                "combine_memory_server_dedup")]
            if self.checkpoint in workflow.COMBINE_MEASUREMENT_CHECKPOINTS:
                artifacts += ["share/tilexr/profiling/" + name for name in ("server_dedup.mapping.json", "server_dedup_combine.mapping.json")]
            if self.checkpoint == workflow.COMBINE_BOUNDARY_CHECKPOINT:
                artifacts += [workflow.COMBINE_BOUNDARY_LIBRARY, workflow.COMBINE_BOUNDARY_KERNEL]
            if self.behavior == "missing-artifact":
                artifacts.pop()
            for artifact in artifacts:
                target = build / "install" / artifact
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("production fixture\n")
        output = "fixture command completed\n"
        if args[0] == "ctest":
            self.assertIn("^(" + "|".join(sorted(self.required)) + ")$", args)
            output = json.dumps({"tests": [{"name": name, "command":
                [sys.executable, str(self.root / "tests/ep/integration" / ("test_server_dedup_combine_m3.py" if name == workflow.COMBINE_M3_CONSUMER_TEST else "test_server_dedup_combine_dry_run.py" if name == workflow.COMBINE_DRY_RUN_TEST else "test_server_dedup_combine_multi.py" if name == workflow.COMBINE_MULTI_TEST else "test_server_dedup_combine_profile.py" if name == workflow.COMBINE_PROFILE_TEST else "test_server_dedup_combine_boundary.py" if name == workflow.COMBINE_BOUNDARY_TEST else "test_server_dedup_combine_chain.py")),
                 "--consumer", str(work / workflow.COMBINE_DRY_RUN_PROGRAM)]
                if name in (workflow.COMBINE_DRY_RUN_TEST, workflow.COMBINE_CHAIN_TEST, workflow.COMBINE_MULTI_TEST, workflow.COMBINE_PROFILE_TEST, workflow.COMBINE_BOUNDARY_TEST, workflow.COMBINE_M3_CONSUMER_TEST) else [sys.executable, str(self.root / "tests/ep/unit/test_server_dedup_combine_config.py")] if name == workflow.COMBINE_CONFIG_TEST else [str(work / name)]} for name in self.names]})
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        Path(log).write_text(output, encoding="utf-8")
        return output

    def run_checkpoint(self):
        output = io.StringIO()
        with mock.patch.object(workflow, "require_tools"), \
                mock.patch.object(workflow, "runtime_check"), \
                mock.patch.object(workflow, "execute", side_effect=self.execute), \
                mock.patch.object(sys, "argv", ["workflow.py", "commit-validate"]), \
                redirect_stdout(output), redirect_stderr(output):
            code = workflow.main()
        reports = list((self.root / "run/validation").glob("commit-*/summary.json"))
        self.assertEqual(len(reports), 1)
        self.assertFalse((self.root / "run/validation/.operation.lock").exists())
        return code, workflow.read_json(reports[0]), output.getvalue()

    def test_success_runs_checkpoint_tests_and_preserves_prior_installation(self):
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


class CombineDryRunCommitValidationTests(CombineCommitValidationTests):
    checkpoint = workflow.COMBINE_DRY_RUN_CHECKPOINT

    def test_success_runs_checkpoint_tests_and_preserves_prior_installation(self):
        retained = [self.root / "install" / name / "manifest.json" for name in
                    ("validation", "validation-combine-c1", "validation-combine-c2")]
        for path in retained:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("old evidence")
        code, summary, output = self.run_checkpoint()
        self.assertEqual(code, 0, output)
        self.assertEqual(summary["expected_tests"], self.names)
        self.assertEqual(summary["checkpoint"], self.checkpoint)
        config = workflow.Config("commit-validate")
        manifest = workflow.verify_build(config)
        self.assertEqual(set(manifest["artifacts"]),
                         {"bin/" + name for name in workflow.COMBINE_ABI_HOST_TESTS | {workflow.COMBINE_DRY_RUN_PROGRAM}})
        self.assertFalse(any("--install" in command or command[0] == "ssh" for command in self.commands))
        self.assertTrue(all(path.read_text() == "old evidence" for path in retained))
        self.assertIn("COMMIT_VALIDATION_PASS", output)


class CombineEmptyKernelCommitValidationTests(CombineCommitValidationTests):
    checkpoint = workflow.COMBINE_EMPTY_KERNEL_CHECKPOINT
    mode = "production"

    def setUp(self):
        super().setUp()
        os.environ["TILEXR_EP_SERVER_DEDUP_TEST_DEVICE"] = "3"

    def test_success_runs_checkpoint_tests_and_preserves_prior_installation(self):
        # Override C1's package expectations: C2 must include installed production
        # artifacts and execute registration, while preserving earlier packages.
        retained = [self.root / name for name in (
            "install/validation/manifest.json", "install/validation-combine-c1/manifest.json",
            "run/validation/sync.json")]
        for path in retained:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("retained evidence")
        code, summary, output = self.run_checkpoint()
        self.assertEqual(code, 0, output)
        self.assertEqual(summary["expected_tests"], sorted(workflow.COMBINE_EMPTY_KERNEL_TESTS))
        self.assertEqual(len(summary["tests"]), 5)
        self.assertEqual(summary["device"], "3")
        self.assertEqual(summary["checkpoint"], self.checkpoint)
        self.assertEqual(summary["validation_timeout_seconds"], 180)
        self.assertEqual(summary["per_test_timeout_seconds"], 120)
        manifest = workflow.verify_build(workflow.Config("commit-validate"))
        self.assertEqual(manifest["server_dedup_combine_contract"], workflow.COMBINE_ABI)
        self.assertIn("lib/tilexr/kernels/tilexr_ep_combine_memory_server_dedup_kernel.o", manifest["artifacts"])
        self.assertTrue(any("-DBUILD_TILEXR_EP_SERVER_DEDUP_COMBINE_REGISTRATION_TEST=ON" in argv
                            for argv in self.commands))
        self.assertTrue(any(argv[-2:] == ["--target", "tilexr-ep"] for argv in self.commands))
        self.assertFalse(any(argv[0] == "ssh" for argv in self.commands))
        for path in retained:
            self.assertEqual(path.read_text(), "retained evidence")

    def test_missing_production_combine_artifact_fails_build(self):
        self.behavior = "missing-artifact"
        code, summary, _ = self.run_checkpoint()
        self.assertEqual(code, workflow.FAILURE_CODES["BUILD_FAILED"])
        self.assertEqual(summary["tests"], [])
        self.assertIn("combine_memory_server_dedup", summary["errors"][0]["message"])

    def test_registration_cannot_be_skipped(self):
        self.behavior = "registration-skip"
        code, summary, _ = self.run_checkpoint()
        self.assertEqual(code, workflow.FAILURE_CODES["EXIT_FAILED"])
        self.assertEqual(summary["errors"][0]["returncode"], 77)
        self.assertEqual(summary["errors"][0]["test"], workflow.COMBINE_REGISTRATION_TEST)

    def test_zero_exit_without_registration_marker_fails(self):
        self.behavior = "registration-no-marker"
        code, summary, _ = self.run_checkpoint()
        self.assertEqual(code, workflow.FAILURE_CODES["LOG_PARSE_FAILED"])
        self.assertEqual(summary["errors"][0]["test"], workflow.COMBINE_REGISTRATION_TEST)

    def test_host_mode_cannot_substitute_for_c2(self):
        with mock.patch.dict(os.environ, TILEXR_VALIDATION_BUILD_MODE="host"):
            with self.assertRaisesRegex(ValueError, "production"):
                workflow.Config("commit-validate")

    def test_device_id_is_validated_before_registration_launch(self):
        os.environ["TILEXR_EP_SERVER_DEDUP_TEST_DEVICE"] = "-1"
        code, summary, _ = self.run_checkpoint()
        self.assertEqual(code, workflow.FAILURE_CODES["CONFIG_FAILED"])
        self.assertEqual(summary["errors"][0]["test"], workflow.COMBINE_REGISTRATION_TEST)
        self.assertIn("device ID", summary["errors"][0]["message"])

    def test_stale_contract_is_rejected(self):
        code, _, output = self.run_checkpoint()
        self.assertEqual(code, 0, output)
        config = workflow.Config("commit-validate")
        path = config.install / "manifest.json"
        manifest = workflow.read_json(path)
        manifest["server_dedup_combine_contract"]["launch_bytes"] = 80
        workflow.write_json(path, manifest)
        with self.assertRaisesRegex(ValueError, "88B"):
            workflow.verify_build(config)


class CombineChainBuildTests(unittest.TestCase):
    def test_c4_package_exports_six_host_checks_and_both_real_kernel_contracts(self):
        self.check_package(workflow.COMBINE_CHAIN_CHECKPOINT, 6)

    def test_c5_package_exports_seven_host_checks_and_both_real_kernel_contracts(self):
        self.check_package(workflow.COMBINE_MULTI_CHECKPOINT, 7)

    def test_c6_package_exports_eleven_host_checks_and_independent_profile_contracts(self):
        self.check_package(workflow.COMBINE_PROFILE_CHECKPOINT, 11)

    def test_c7_package_exports_fourteen_host_checks_and_probe_artifacts(self):
        self.check_package(workflow.COMBINE_BOUNDARY_CHECKPOINT, 14)

    def test_m3_build_exports_module_contract_and_ten_host_checks(self):
        self.check_package(workflow.COMBINE_M3_CHECKPOINT, 10)

    def check_package(self, checkpoint, expected_count):
        fixture = CombineCommitValidationTests()
        fixture.checkpoint = checkpoint
        fixture.mode = "production"
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        config = workflow.Config("commit-validate")
        if checkpoint in (*workflow.COMBINE_MEASUREMENT_CHECKPOINTS, workflow.COMBINE_BOUNDARY_CHECKPOINT):
            # These Python drivers are source inputs; seed the fixture before
            # C6 fingerprints source, as a real checkout already contains them.
            targets = (fixture.required - {workflow.COMBINE_DRY_RUN_TEST, workflow.COMBINE_CHAIN_TEST,
                       workflow.COMBINE_MULTI_TEST, workflow.COMBINE_PROFILE_TEST, workflow.COMBINE_BOUNDARY_TEST, workflow.COMBINE_M3_CONSUMER_TEST, workflow.COMBINE_CONFIG_TEST}) | {
                       workflow.COMBINE_DRY_RUN_PROGRAM, workflow.CHAIN_PROGRAM}
            fixture.execute(["cmake", "--build", workflow.validation_build_directory(config) / "host",
                             "--target", *sorted(targets)], fixture.root / "fixture-build.log")
            fixture.commands.clear()
        with mock.patch.object(workflow, "require_tools"), mock.patch.object(workflow, "runtime_check"), \
                mock.patch.object(workflow, "execute", side_effect=fixture.execute), redirect_stdout(io.StringIO()):
            workflow.build(config)
            self.assertEqual(workflow.host_validate(config, "host-fixture"), 0)
        manifest = workflow.verify_build(config)
        self.assertEqual({test["name"] for test in manifest["tests"]}, workflow.COMBINE_TESTS[fixture.checkpoint])
        self.assertEqual(len(manifest["tests"]), expected_count)
        self.assertEqual(manifest["server_dedup_combine_contract"], workflow.combine_contract(checkpoint))
        self.assertEqual(manifest["server_dedup_contract"], workflow.DISPATCH_ABI)
        self.assertFalse(manifest["server_dedup_profiling"])
        self.assertIn("bin/" + workflow.CHAIN_PROGRAM, manifest["artifacts"])
        self.assertIn("bin/" + workflow.COMBINE_DRY_RUN_PROGRAM, manifest["artifacts"])
        self.assertEqual(len([test for test in manifest["tests"] if test["argv"][0] == "@python"]),
                         3 if checkpoint == workflow.COMBINE_M3_CHECKPOINT else
                         5 if checkpoint == workflow.COMBINE_BOUNDARY_CHECKPOINT else
                         4 if checkpoint == workflow.COMBINE_PROFILE_CHECKPOINT else expected_count - 4)
        self.assertEqual(workflow.node_tests(manifest), [])
        if checkpoint == workflow.COMBINE_BOUNDARY_CHECKPOINT:
            self.assertIn(workflow.COMBINE_BOUNDARY_LIBRARY, manifest["artifacts"])
            self.assertIn(workflow.COMBINE_BOUNDARY_KERNEL, manifest["artifacts"])
            self.assertIn("content_sha256", manifest["source"])
            production = next(command for command in fixture.commands if "--build" in command and any("production" in arg for arg in command))
            self.assertIn("tilexr-ep-combine-memory-boundary-test", production)


if __name__ == "__main__":
    unittest.main()
