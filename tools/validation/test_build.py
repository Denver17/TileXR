#!/usr/bin/env python3
"""构建生命周期回归：真实文件系统与命令失败，CANN 编译使用安装产物夹具。"""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import workflow


class ProductionChecksTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tilexr-symbol-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install = self.root / "install"
        self.log = self.root / "logs"
        self.names = ("TileXRMoeEpDispatchMemoryServerDedup", "TileXRMoeEpDispatchMemory")
        self.symbols = "\n".join(f"{name} T 1000 10" for name in self.names)
        patch = mock.patch.dict(os.environ, {"ASCEND_DRIVER_PATH": str(self.root / "driver")})
        patch.start()
        self.addCleanup(patch.stop)

    def inspect_fixture(self, argv, log, **kwargs):
        if argv[0] == "nm":
            return self.symbols
        if argv[0] == "readelf":
            return "0x000000000000001d (RUNPATH) Library runpath: [$ORIGIN]\n"
        if argv[0] == "ldd":
            return f"libascend_hal.so => {self.root / 'driver/lib64/driver/libascend_hal.so'} (0x1000)\n"
        self.fail(f"unexpected command: {argv}")

    def check(self, checkpoint=workflow.CHECKPOINT):
        with mock.patch.object(workflow, "require_tools"), \
                mock.patch.object(workflow, "execute", side_effect=self.inspect_fixture) as execute:
            workflow.runtime_check(SimpleNamespace(mode="production", checkpoint=checkpoint), self.install, self.log)
        return execute

    def test_empty_kernel_inspects_installed_elf_once(self):
        execute = self.check(workflow.EMPTY_KERNEL_CHECKPOINT)
        self.assertEqual(execute.call_args_list[0].args[0],
                         ["readelf", "-h", self.install / "lib/tilexr/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.o"])
        self.assertEqual(len(execute.call_args_list), 4)

    def test_c_abi_functions_pass_and_remaining_library_checks_run(self):
        execute = self.check()
        self.assertEqual([call.args[0][0] for call in execute.call_args_list], ["nm", "readelf", "ldd"])

    def test_each_required_function_must_be_defined_with_its_exact_name(self):
        for missing in self.names:
            other = next(name for name in self.names if name != missing)
            for replacement in ("", f"{missing}_helper T 1000 10", f"prefix_{missing} T 1000 10",
                                f"_Z{len(missing)}{missing}v T 1000 10", f"{missing} U",
                                f"{missing} t 1000 10", f"{missing} D 1000 8"):
                with self.subTest(missing=missing, replacement=replacement):
                    self.symbols = f"{other} T 2000 10\n{replacement}"
                    with self.assertRaisesRegex(ValueError, "生产库缺少导出符号：" + missing) as error:
                        self.check()
                    self.assertIn(str(self.install / "lib/libtilexr-ep.so"), str(error.exception))
                    self.assertIn(str(self.log / "symbols.log"), str(error.exception))


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tilexr-build-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        (self.root / "hostfile").write_text("localhost\n", encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text("# copied source without Git\n", encoding="utf-8")
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("TILEXR_VALIDATION_")}
        environment.update(TILEXR_VALIDATION_ROOT=str(self.root),
                           TILEXR_VALIDATION_BUILD_MODE="host",
                           TILEXR_VALIDATION_TIMEOUT="30")
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        self.names = sorted(workflow.REQUIRED_TESTS)
        self.commands = []
        self.generation = 0

    def config(self, mode="host"):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BUILD_MODE": mode}):
            return workflow.Config()

    def compiled_command(self, argv, log, **kwargs):
        """代替昂贵的 CMake；实际创建本次安装文件，并保留所有调用参数。"""
        args = [str(value) for value in argv]
        self.commands.append((args, Path(log), kwargs))
        work = self.root / "build/validation"
        stage = work / "install"
        if "--install" in args:
            if args[-1] == str(work / "production"):
                files = ["lib/libtilexr-ep.so", "lib/libtile-comm.so"]
                files += [f"lib/tilexr/kernels/tilexr_ep_{name}_kernel.o"
                          for name in ("dispatch", "combine", "dispatch_memory", "combine_memory", "dispatch_memory_server_dedup")]
                if any("-DTILEXR_EP_BUILD_MEMORY_BOUNDARY_TEST=ON" in command for command, _, _ in self.commands):
                    files += ["lib/libtilexr-ep-memory-boundary-test.so",
                              "lib/tilexr/kernels/tilexr_ep_memory_boundary_test_kernel.o"]
            else:
                files = ["bin/" + name for name in self.names]
                if any("-DBUILD_TILEXR_EP_SERVER_DEDUP_CHAIN_TEST=ON" in command
                       for command, _, _ in self.commands) and getattr(self, "install_chain", True):
                    files.append("bin/" + workflow.CHAIN_PROGRAM)
            for name in files:
                target = stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(f"fixture generation {self.generation}\n", encoding="utf-8")
        if args[0] == "ctest":
            output = json.dumps({"tests": [{"name": name,
                                           "command": [str(work / "host/bin" / name)]}
                                          for name in self.names]})
        else:
            output = "fixture command completed\n"
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        Path(log).write_text(output, encoding="utf-8")
        return output

    def build_fixture(self, config=None):
        config = config or self.config()
        self.generation += 1
        with mock.patch.object(workflow, "require_tools"), \
                mock.patch.object(workflow, "execute", side_effect=self.compiled_command), \
                mock.patch.object(workflow, "runtime_check") as runtime, redirect_stdout(io.StringIO()):
            workflow.build(config)
        return workflow.verify_build(config), runtime

    def test_rebuild_reuses_fixed_path_and_discards_old_files(self):
        config = self.config()
        first, _ = self.build_fixture(config)
        work = self.root / "build/validation"
        (work / "old-object.o").write_text("old object")
        (work / "host/cache").mkdir(parents=True)
        (work / "host/cache/stale-config").write_text("old options")
        old_binary = config.install / "bin/removed-test"
        old_binary.write_text("obsolete executable")
        second, _ = self.build_fixture(config)
        self.assertNotEqual(first["build_id"], second["build_id"])
        self.assertEqual(set(path.name for path in (self.root / "build").iterdir()), {"validation"})
        self.assertFalse((work / "old-object.o").exists())
        self.assertFalse((work / "host/cache").exists())
        self.assertFalse(old_binary.exists())
        self.assertEqual((config.install / "bin" / self.names[0]).read_text(), "fixture generation 2\n")
        configurations = [args[args.index("-B") + 1] for args, _, _ in self.commands if "-B" in args]
        self.assertEqual(configurations, [str(work / "host"), str(work / "host")])
        self.assertEqual(len(list(config.logs.glob("build-*"))), 2)

    @unittest.skipUnless(os.name == "posix", "真实命令失败和进程组清理需要 POSIX")
    def test_failed_configuration_invalidates_previous_package_and_sync_receipt(self):
        real_execute = workflow.execute
        config = self.config()
        for boundary in ("start", "exit"):
            with self.subTest(boundary=boundary):
                self.build_fixture(config)
                with redirect_stdout(io.StringIO()):
                    workflow.sync(config)
                self.assertTrue((config.logs / "sync.json").is_file())

                def fail_configuration(argv, log, **kwargs):
                    if "-S" not in argv:
                        return self.compiled_command(argv, log, **kwargs)
                    self.assertEqual(kwargs["start_failure"], "BUILD_FAILED")
                    self.assertEqual(kwargs["exit_failure"], "BUILD_FAILED")
                    command = ([str(self.root / "missing-cmake")] if boundary == "start" else
                               [sys.executable, "-c", "print('configure failed'); raise SystemExit(7)"])
                    return real_execute(command, log, **kwargs)

                stderr = io.StringIO()
                with mock.patch.object(workflow, "require_tools"), \
                        mock.patch.object(workflow, "execute", side_effect=fail_configuration), \
                        mock.patch.object(sys, "argv", ["workflow.py", "build"]), \
                        redirect_stdout(io.StringIO()), redirect_stderr(stderr):
                    code = workflow.main()
                self.assertEqual(code, workflow.FAILURE_CODES["BUILD_FAILED"])
                self.assertIn("VALIDATION_FAIL type=BUILD_FAILED", stderr.getvalue())
                if boundary == "exit":
                    self.assertIn('"returncode": 7', stderr.getvalue())
                    self.assertIn("configure failed", stderr.getvalue())
                self.assertFalse((config.install / "manifest.json").exists())
                self.assertFalse((config.logs / "sync.json").exists())
                self.assertFalse((config.logs / ".operation.lock").exists())
                self.assertTrue((config.install / "bin" / self.names[0]).is_file())
                with self.assertRaises(FileNotFoundError):
                    workflow.verify_build(config)
                with mock.patch.object(workflow, "execute") as launch, self.assertRaises(FileNotFoundError):
                    workflow.run(config)
                launch.assert_not_called()

    def test_required_test_set_accepts_additions_and_rejects_replacements(self):
        self.names.append("test_new_additional_host_case")
        manifest, _ = self.build_fixture()
        self.assertEqual({test["name"] for test in manifest["tests"]}, set(self.names))
        self.assertGreater(len(manifest["tests"]), len(workflow.REQUIRED_TESTS))
        for missing in ("test_tilexr_ep_server_dedup_protocol", "test_tilexr_ep_layout"):
            with self.subTest(missing=missing):
                self.names = sorted(workflow.REQUIRED_TESTS - {missing}) + ["test_replacement"]
                with mock.patch.object(workflow, "runtime_check") as runtime, \
                        mock.patch.object(workflow, "require_tools"), \
                        mock.patch.object(workflow, "execute", side_effect=self.compiled_command), \
                        redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, missing):
                    workflow.build(self.config())
                runtime.assert_not_called()
                self.assertFalse((self.config().install / "manifest.json").exists())

    def test_empty_kernel_build_requires_installed_registration_consumer(self):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_CHECKPOINT": workflow.EMPTY_KERNEL_CHECKPOINT}):
            config = self.config("production")
        with self.assertRaisesRegex(ValueError, workflow.REGISTRATION_TEST):
            self.build_fixture(config)
        self.names.append(workflow.REGISTRATION_TEST)
        manifest, _ = self.build_fixture(config)
        self.assertEqual(manifest["checkpoint"], workflow.EMPTY_KERNEL_CHECKPOINT)
        self.assertIn(workflow.REGISTRATION_TEST, [test["name"] for test in manifest["tests"]])
        configure = [args for args, _, _ in self.commands if "-S" in args][-1]
        self.assertIn("-DBUILD_TILEXR_EP_SERVER_DEDUP_REGISTRATION_TEST=ON", configure)
        self.assertIn(f"-DTILEXR_INSTALL_PREFIX={self.root / 'build/validation/install'}", configure)

    def test_chain_build_installs_native_consumer_without_adding_per_host_test(self):
        (self.root / "hostfile").write_text("localhost\nnode-b\n", encoding="utf-8")
        for checkpoint in workflow.CHAIN_CHECKPOINTS:
            with self.subTest(checkpoint=checkpoint), mock.patch.dict(os.environ, {
                    "TILEXR_VALIDATION_CHECKPOINT": checkpoint, "TILEXR_COMM_ID": "192.0.2.1:10177"}):
                config = self.config("production")
                self.install_chain = False
                with self.assertRaisesRegex(ValueError, workflow.CHAIN_PROGRAM):
                    self.build_fixture(config)
                self.install_chain = True
                manifest, _ = self.build_fixture(config)
                self.assertEqual(len(manifest["tests"]),
                                 0 if checkpoint == workflow.MEMORY_BOUNDARY_CHECKPOINT else len(workflow.REQUIRED_TESTS))
                self.assertNotIn(workflow.CHAIN_PROGRAM, [test["name"] for test in manifest["tests"]])
                self.assertIn("bin/" + workflow.CHAIN_PROGRAM, manifest["artifacts"])

    def test_duplicate_test_name_cannot_replace_independent_execution(self):
        self.names.append(self.names[0])
        with mock.patch.object(workflow, "runtime_check") as runtime, \
                mock.patch.object(workflow, "require_tools"), \
                mock.patch.object(workflow, "execute", side_effect=self.compiled_command), \
                redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, "必需测试缺失或重复"):
            workflow.build(self.config())
        runtime.assert_not_called()

    def test_baseline_build_disables_breakdown_instrumentation(self):
        (self.root / "hostfile").write_text("localhost\nnode-b\n", encoding="utf-8")
        for mode in ("functional", "performance"):
            with self.subTest(mode=mode), mock.patch.dict(os.environ, {
                    "TILEXR_VALIDATION_CHECKPOINT": workflow.SOURCE_SEND_CHECKPOINT,
                    "TILEXR_VALIDATION_RUN_MODE": mode, "TILEXR_COMM_ID": "192.0.2.1:10177"}):
                self.commands.clear()
                self.build_fixture(self.config("production"))
                configure = next(args for args, _, _ in self.commands
                                 if "-S" in args and str(self.root) in args)
                self.assertIn("-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=OFF", configure)
                os.environ["TILEXR_VALIDATION_RUN_MODE"] = "breakdown"
                with self.assertRaisesRegex(ValueError, "设备打点开关"):
                    workflow.verify_build(self.config("production"))
                self.assertFalse(workflow.verify_build(self.config("production"), check_run_mode=False)["server_dedup_profiling"])

    def test_breakdown_build_enables_count_profile_and_rejects_baseline_or_legacy_package(self):
        (self.root / "hostfile").write_text("localhost\nnode-b\n", encoding="utf-8")
        with mock.patch.dict(os.environ, {
                "TILEXR_VALIDATION_CHECKPOINT": workflow.SOURCE_SEND_CHECKPOINT,
                "TILEXR_VALIDATION_RUN_MODE": "breakdown", "TILEXR_COMM_ID": "192.0.2.1:10177"}):
            config = self.config("production")
            manifest, _ = self.build_fixture(config)
            self.assertTrue(manifest["server_dedup_profiling"])
            self.assertEqual(manifest["server_dedup_profile_version"], 4)
            configure = next(args for args, _, _ in self.commands if "-S" in args and str(self.root) in args)
            self.assertIn("-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=ON", configure)
            with mock.patch.dict(os.environ, TILEXR_VALIDATION_RUN_MODE="functional"):
                self.assertEqual(workflow.verify_build(self.config("production"), check_run_mode=False), manifest)
            manifest.pop("server_dedup_profile_version")
            workflow.write_json(config.install / "manifest.json", manifest)
            with self.assertRaisesRegex(ValueError, "breakdown v4"):
                workflow.verify_build(config)
            with self.assertRaisesRegex(ValueError, "breakdown v4"):
                workflow.verify_build(config, check_run_mode=False)
            with mock.patch.dict(os.environ, TILEXR_VALIDATION_RUN_MODE="performance"):
                with self.assertRaisesRegex(ValueError, "设备打点开关"):
                    workflow.verify_build(self.config("production"))
            manifest["server_dedup_profiling"] = "true"
            workflow.write_json(config.install / "manifest.json", manifest)
            with self.assertRaisesRegex(ValueError, "设备打点开关"):
                workflow.verify_build(config, check_run_mode=False)

    def test_runtime_check_occurs_once_after_production_install_and_never_during_verify_sync(self):
        config = self.config("production")

        def inspect_runtime(actual, stage, log):
            self.assertIs(actual, config)
            self.assertEqual(stage, self.root / "build/validation/install")
            self.assertEqual(self.commands[-1][0][0], "ctest")
            self.assertTrue((stage / "lib/libtilexr-ep.so").is_file())
            self.assertTrue((stage / "lib/tilexr/kernels/tilexr_ep_dispatch_memory_kernel.o").is_file())
            self.assertTrue((stage / "bin" / self.names[0]).is_file())
            self.assertFalse((stage / "manifest.json").exists())

        with mock.patch.object(workflow, "require_tools"), \
                mock.patch.object(workflow, "execute", side_effect=self.compiled_command), \
                mock.patch.object(workflow, "runtime_check", side_effect=inspect_runtime) as runtime, \
                redirect_stdout(io.StringIO()):
            workflow.build(config)
            manifest = workflow.verify_build(config)
            workflow.sync(config)
            workflow.verify_build(config)
            workflow.sync(config)
        runtime.assert_called_once()
        self.assertEqual(workflow.read_json(config.logs / "sync.json")["build_id"], manifest["build_id"])
        production = self.root / "build/validation/production"
        operations = [args[1] for args, _, _ in self.commands if str(production) in args]
        self.assertEqual(operations, ["-S", "--build", "--install"])

    def test_file_at_build_directory_is_preserved_and_rejected(self):
        config = self.config()
        work = self.root / "build/validation"
        work.parent.mkdir()
        work.write_text("user-owned file\n")
        with self.assertRaisesRegex(ValueError, "构建路径不是目录"):
            workflow.clean_build_directory(config)
        self.assertEqual(work.read_text(), "user-owned file\n")

    def test_build_and_install_symlinks_never_delete_external_content(self):
        target = Path(self.temporary.name) / "outside"
        target.mkdir()
        sentinel = target / "keep.txt"
        sentinel.write_text("outside content\n")
        probe = self.root / "symlink-probe"
        try:
            probe.symlink_to(target, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"当前平台不能创建真实 symlink: {error}")
        if not probe.is_symlink():
            self.skipTest("MSYS 以复制模拟 symlink，无法验证真实链接保护")
        probe.unlink()
        for relative in ("build", "build/validation", "install", "install/validation"):
            with self.subTest(path=relative):
                link = self.root / relative
                link.parent.mkdir(parents=True, exist_ok=True)
                link.symlink_to(target, target_is_directory=True)
                with self.assertRaisesRegex(ValueError, "符号链接"):
                    workflow.clean_build_directory(self.config())
                self.assertEqual(sentinel.read_text(), "outside content\n")
                link.unlink()


if __name__ == "__main__":
    unittest.main(verbosity=2)
