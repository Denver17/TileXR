#!/usr/bin/env python3
"""无 NPU/真实 SSH 的七步验证行为回归：python3 tools/validation/test_workflow.py。"""

import contextlib
import errno
import io
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import workflow


class TemporaryRoot(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tilexr-validation-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "coordinator"
        self.root.mkdir()
        self.hostfile = self.root / "hostfile"
        self.hostfile.write_text("127.0.0.1\n", encoding="utf-8")
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("TILEXR_VALIDATION_")}
        environment.update(TILEXR_VALIDATION_ROOT=str(self.root),
                           TILEXR_VALIDATION_HOSTFILE=str(self.hostfile),
                           TILEXR_VALIDATION_BUILD_MODE="host",
                           TILEXR_VALIDATION_TIMEOUT="30")
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)

    def config(self):
        return workflow.Config()


class HostfileTests(TemporaryRoot):
    def parse(self, text):
        self.hostfile.write_text(text, encoding="utf-8")
        return workflow.hosts_from_file(self.hostfile)

    def test_comments_bom_and_whole_servers(self):
        self.assertEqual(self.parse("\ufeff# group\n192.0.2.1 # physical IDs\n\n192.0.2.3\n"),
                         [{"host": "192.0.2.1", "devices": list(range(8))},
                          {"host": "192.0.2.3", "devices": list(range(8))}])

    def test_rejects_bad_inventory(self):
        for text in ("", "# only comments\n", "192.0.2.1\n192.0.2.1\n",
                     "192.0.2.1 devices=0,1,2,3,4,5,6,7\n", "192.0.2.256\n", "worker01\n",
                     "192.0.2.1 devices=-1\n", "192.0.2.1 devices=\n",
                     "192.0.2.1 extra\n", "user@192.0.2.1\n",
                     "-oProxyCommand=x\n", "192.0.2.1;command\n",
                     "\n".join("192.0.2." + str(index + 1) for index in range(65))):
            with self.subTest(text=text[:80]), self.assertRaises(ValueError):
                self.parse(text)

    def test_accepts_64_server_inventory(self):
        text = "\n".join(f"192.0.2.{index + 1}" for index in range(64))
        self.assertEqual(len(self.parse(text)), 64)


class ConfigTests(TemporaryRoot):
    def test_empty_kernel_requires_production(self):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_CHECKPOINT": workflow.EMPTY_KERNEL_CHECKPOINT}):
            with self.assertRaisesRegex(ValueError, "production"):
                self.config()
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BUILD_MODE": "production"}):
                self.assertEqual(self.config().checkpoint, workflow.EMPTY_KERNEL_CHECKPOINT)

    def test_production_without_spdlog_reaches_cmake(self):
        (self.root / "CMakeLists.txt").write_text("# source fixture\n")
        (self.root / "3rdparty/spdlog").mkdir(parents=True)

        def configure_boundary(argv, *args, **kwargs):
            if "-S" in argv:
                self.assertEqual(argv[:3], ["cmake", "-S", self.root])
                self.assertIn("-DTILEXR_BUILD_EP=ON", argv)
                raise RuntimeError("production CMake reached")
            return "tool version\n"

        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BUILD_MODE": "production"}), \
                mock.patch.object(workflow, "require_tools"), \
                mock.patch.object(workflow, "execute", side_effect=configure_boundary):
            with self.assertRaisesRegex(RuntimeError, "production CMake reached"):
                workflow.build(self.config())

    def test_sync_ignores_case_configuration_and_selects_entire_inventory(self):
        self.hostfile.write_text("\n".join(f"192.0.2.{i + 1}" for i in range(8)))
        with mock.patch.dict(os.environ, {
                "TILEXR_VALIDATION_CHECKPOINT": workflow.DISPATCH_CHECKPOINT,
                "TILEXR_VALIDATION_BUILD_MODE": "production", "TILEXR_VALIDATION_SERVERS": "bad",
                "TILEXR_VALIDATION_BS": "bad", "TILEXR_VALIDATION_MOE_EXPERT_NUM": "96",
                "TILEXR_VALIDATION_RUN_MODE": "bad", "TILEXR_VALIDATION_TIMEOUT": "bad",
                "TILEXR_COMM_ID": "bad"}):
            config = workflow.Config("sync")
            self.assertIsNone(config.chain)
            self.assertEqual(len(config.hosts()), 8)
            self.assertIsNone(workflow.Config("verify").chain)
            self.assertIsNone(workflow.Config("build").chain)

    def test_rejects_unsupported_or_unbounded_configuration(self):
        for key, value in (("BUILD_MODE", "npu"), ("CHECKPOINT", "device-dedup"),
                           ("JOBS", "0"), ("JOBS", "257"), ("TIMEOUT", "29"),
                           ("TIMEOUT", "181"), ("SSH_USER", "user;command")):
            with self.subTest(key=key, value=value), mock.patch.dict(
                    os.environ, {"TILEXR_VALIDATION_" + key: value}), self.assertRaises(ValueError):
                workflow.Config("build" if key == "JOBS" else "run")

    def test_hostfile_must_be_inside_synchronized_root(self):
        outside = Path(self.temporary.name) / "hostfile"
        outside.write_text("127.0.0.1\n", encoding="utf-8")
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_HOSTFILE": str(outside)}):
            with self.assertRaises(ValueError):
                self.config()


class ResultTests(unittest.TestCase):
    def setUp(self):
        self.manifest = {"build_id": "build-current", "checkpoint": workflow.CHECKPOINT,
                         "mode": "host", "tests": [{"name": "a"}, {"name": "b"}]}
        self.result = {"run_id": "run-current", "build_id": "build-current", "status": "PASS",
                       "cleanup": "complete", "checkpoint": workflow.CHECKPOINT, "mode": "host",
                       "tests": [{"name": "a", "status": "PASS"}, {"name": "b", "status": "PASS"}]}

    def test_accepts_complete_current_result_independent_of_test_order(self):
        self.result["tests"].reverse()
        workflow.validate_result(self.result, "run-current", self.manifest)

    def test_rejects_stale_failed_missing_duplicate_and_wrong_scope_results(self):
        changes = [{"run_id": "old-run"}, {"build_id": "old-build"}, {"status": "FAIL"},
                   {"cleanup": "pending"}, {"checkpoint": "another-checkpoint"}, {"mode": "production"},
                   {"tests": []}, {"tests": [{"name": "a", "status": "PASS"}]},
                   {"tests": [{"name": "a", "status": "PASS"}, {"name": "a", "status": "PASS"}]},
                   {"tests": [{"name": "a", "status": "PASS"}, {"name": "b", "status": "FAIL"}]},
                   {"tests": [{"name": "a", "status": "PASS"}, {"name": "unknown", "status": "PASS"}]}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(workflow.Failure):
                workflow.validate_result(dict(self.result, **change), "run-current", self.manifest)

    def test_rejects_absent_protocol_fields(self):
        for field in self.result:
            result = dict(self.result)
            del result[field]
            with self.subTest(field=field), self.assertRaises(workflow.Failure) as error:
                workflow.validate_result(result, "run-current", self.manifest)
            self.assertEqual(error.exception.kind, "LOG_PARSE_FAILED")


class CancellationTests(TemporaryRoot):
    def test_absent_pid_record_does_not_signal(self):
        with mock.patch.object(workflow.os, "kill") as kill:
            workflow.cancel(self.config(), "finished-run")
        kill.assert_not_called()

    def test_reused_or_unverifiable_pid_is_never_signalled(self):
        record = self.config().logs / "old-run" / "pid.json"
        for start, actual in (("old-start", "new-start"), (None, None), ("old-start", None)):
            with self.subTest(start=start, actual=actual):
                workflow.write_json(record, {"pid": os.getpid(), "start": start})
                with mock.patch.object(workflow, "pid_identity", return_value=actual), \
                        mock.patch.object(workflow.os, "kill") as kill, self.assertRaises(ValueError):
                    workflow.cancel(self.config(), "old-run")
                kill.assert_not_called()
                self.assertTrue(record.exists())


class FailureEvidenceTests(TemporaryRoot):
    def test_parse_failure_preserves_exit_and_cleanup_evidence(self):
        manifest = {"tests": []}
        for output in (b"NODE_STARTED run_id=run\n\xff", b"NODE_STARTED run_id=run\nNODE_RESULT {broken\n",
                       b"NODE_STARTED run_id=run\nNODE_RESULT null\n"):
            with self.subTest(output=output):
                log = self.root / "launch.log"
                log.write_bytes(output)
                execution = workflow.Failure("EXIT_FAILED", "exit=9", returncode=9,
                    additional_failures=[workflow.Failure("CLEANUP_FAILED", "not stopped").record()])
                result, errors = workflow.launch_outcome(log, execution, "run", manifest)
                self.assertIsNone(result)
                self.assertEqual([error["type"] for error in errors],
                                 ["EXIT_FAILED", "CLEANUP_FAILED", "LOG_PARSE_FAILED"])
                self.assertEqual(errors[0]["returncode"], 9)

    def test_build_and_sync_keep_lock_when_process_cleanup_failed(self):
        for kind in ("BUILD_FAILED", "SYNC_FAILED"):
            with self.subTest(kind=kind):
                config = self.config()
                with self.assertRaises(workflow.Failure):
                    with workflow.operation_lock(config, ".operation.lock"):
                        raise workflow.Failure(kind, "command failed", additional_failures=[
                            workflow.Failure("CLEANUP_FAILED", "not stopped").record()])
                lock = config.logs / ".operation.lock"
                self.assertTrue(lock.exists())
                with self.assertRaises(ValueError):
                    with workflow.operation_lock(self.config(), ".operation.lock"):
                        self.fail("must not begin another operation")
                lock.rmdir()  # 夹具未启动真实进程。

    def test_normal_build_failure_releases_operation_lock(self):
        config = self.config()
        with self.assertRaises(workflow.Failure):
            with workflow.operation_lock(config, ".operation.lock"):
                raise workflow.Failure("BUILD_FAILED", "compile failed and stopped")
        self.assertFalse((config.logs / ".operation.lock").exists())


@unittest.skipUnless(os.name == "posix", "生产进程组契约需要 POSIX Python")
class ProcessTests(TemporaryRoot):
    def test_missing_executable_is_launch_failure(self):
        with self.assertRaises(workflow.Failure) as error:
            workflow.execute([self.root / "absent-program"], self.root / "missing.log")
        self.assertEqual(error.exception.kind, "LAUNCH_FAILED")

    def test_signal_is_exit_failure_and_preserves_return_code(self):
        with self.assertRaises(workflow.Failure) as error:
            workflow.execute([sys.executable, "-c", "import os, signal; os.kill(os.getpid(), signal.SIGKILL)"],
                             self.root / "signal.log")
        self.assertEqual(error.exception.kind, "EXIT_FAILED")
        self.assertLess(error.exception.details["returncode"], 0)

    def test_success_with_invalid_utf8_is_log_parse_failure(self):
        with self.assertRaises(workflow.Failure) as error:
            workflow.execute([sys.executable, "-c", "import sys; sys.stdout.buffer.write(bytes([255]))"],
                             self.root / "invalid.log")
        self.assertEqual(error.exception.kind, "LOG_PARSE_FAILED")

    def test_cleanup_failure_preserves_original_exit_failure(self):
        with mock.patch.object(workflow, "stop_process", side_effect=OSError("cleanup denied")), \
                self.assertRaises(workflow.Failure) as error:
            workflow.execute([sys.executable, "-c", "raise SystemExit(7)"], self.root / "cleanup.log")
        self.assertEqual(error.exception.kind, "EXIT_FAILED")
        self.assertEqual(error.exception.details["returncode"], 7)
        self.assertEqual(error.exception.details["additional_failures"][0]["type"], "CLEANUP_FAILED")

    def test_success_captures_stdout_stderr_and_stdin(self):
        log = self.root / "logs/output.log"
        output = workflow.execute([sys.executable, "-c", "import sys; print(sys.stdin.read()); "
                                   "print('stderr-text', file=sys.stderr)"], log, stdin="payload")
        self.assertIn("payload", output)
        self.assertIn("stderr-text", output)
        self.assertEqual(output, log.read_text(encoding="utf-8"))

    def test_nonzero_reports_exit_and_log_tail(self):
        with self.assertRaisesRegex(RuntimeError, "exit=7") as failure:
            workflow.execute([sys.executable, "-c", "print('first boundary'); raise SystemExit(7)"],
                             self.root / "failure.log")
        self.assertIn("first boundary", str(failure.exception))
        self.assertIn("failure.log", str(failure.exception))

    def test_timeout_kills_child_that_ignores_sigterm(self):
        pid_file = self.root / "child.pid"
        child = ("import os, pathlib, signal, time; "
                 "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                 f"pathlib.Path({str(pid_file)!r}).write_text(str(os.getpid())); time.sleep(60)")
        parent = ("import subprocess, sys, time; "
                  f"subprocess.Popen([sys.executable, '-c', {child!r}]); time.sleep(60)")
        started = time.monotonic()
        with self.assertRaises(workflow.Failure) as error:
            workflow.execute([sys.executable, "-c", parent], self.root / "timeout.log", timeout=1.5)
        self.assertEqual(error.exception.kind, "EXIT_FAILED")
        self.assertTrue(error.exception.details["timeout"])
        self.assertLess(time.monotonic() - started, 9)
        self.assertTrue(pid_file.exists(), "超时前应实际启动并记录子进程")
        self.assert_process_stopped(int(pid_file.read_text()))

    def test_success_also_reaps_background_children(self):
        pid_file = self.root / "background.pid"
        child = ("import os, pathlib, signal, time; "
                 "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                 f"pathlib.Path({str(pid_file)!r}).write_text(str(os.getpid())); time.sleep(60)")
        parent = ("import pathlib, subprocess, sys, time; "
                  f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
                  f"p=pathlib.Path({str(pid_file)!r}); "
                  "exec('while not p.exists(): time.sleep(0.01)')")
        workflow.execute([sys.executable, "-c", parent], self.root / "background.log", timeout=5)
        self.assert_process_stopped(int(pid_file.read_text()))

    def assert_process_stopped(self, pid):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
                stat = Path(f"/proc/{pid}/stat")
                try:
                    if stat.read_text().rsplit(")", 1)[1].split()[0] == "Z":
                        return
                except OSError as error:
                    if error.errno not in (errno.ENOENT, errno.ESRCH, errno.EINVAL):
                        raise
                    # MSYS 的进程消失窗口可能返回 EINVAL；仍须确认 PID 已消失，
                    # 不能把 /proc 读取失败直接当成清理成功。
                    os.kill(pid, 0)
            except ProcessLookupError:
                return
            time.sleep(0.05)
        # 限定到本测试直接创建的 PID，失败时也不留下测试子进程。
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        self.fail(f"本次命令的子进程 {pid} 在清理后仍运行")


FAKE_TRANSPORT = r'''import os, pathlib, shlex, shutil, subprocess, sys, time

kind = pathlib.Path(sys.argv[0]).name
root = pathlib.Path(os.environ["FAKE_ROOT"])
remote = pathlib.Path(os.environ["FAKE_REMOTE"])
scenario = os.environ.get("FAKE_SCENARIO", "")
args = sys.argv[1:]
if os.environ.get("FAKE_REMOTE_MAP"):
    import json
    target = args[-2] if kind == "ssh" else (args[-2] if ":" in args[-2] else args[-1]).split(":", 1)[0]
    remote = pathlib.Path(json.loads(os.environ["FAKE_REMOTE_MAP"])[target.split("@")[-1]])

def mapped(value):
    return remote / pathlib.Path(value).relative_to(root)

if kind == "ssh":
    command = shlex.split(args[-1])
    direct = None
    if command[0].startswith("TILEXR_DIRECT_CONFIG="):
        direct = command.pop(0).split("=", 1)[1].replace(str(root), str(remote))
    payload = sys.stdin.read()
    if command == ["hostname"]:
        print("192.0.2.2")
        sys.exit(0)
    command[0] = os.environ["FAKE_BASH"]
    command[3] = str(mapped(command[3]))
    action = command[4] if len(command) > 4 else "prepare"
    if action == "cancel" and scenario == "node-fail-cleanup-fail":
        print("injected cancellation transport failure", file=sys.stderr)
        sys.exit(255)
    if action == "node" and scenario == "ssh-fail":
        print("injected SSH connection failure", file=sys.stderr)
        sys.exit(255)
    if action == "node" and scenario == "delayed-node":
        time.sleep(5)
    if action == "node" and scenario in ("exit-after-start", "no-completion", "bad-completion"):
        print("NODE_STARTED run_id=" + command[5])
        if scenario == "bad-completion":
            print("NODE_RESULT {invalid JSON}")
        sys.exit(9 if scenario == "exit-after-start" else 0)
    if scenario == "verify-fail" and action == "verify":
        print("injected remote verification failure", file=sys.stderr)
        sys.exit(23)
    environment = dict(os.environ, TILEXR_VALIDATION_ROOT=str(remote),
                       TILEXR_VALIDATION_HOSTFILE=str(remote / "hostfile"))
    if direct is not None:
        environment["TILEXR_DIRECT_CONFIG"] = direct
    if action == "node" and scenario in ("node-fail", "node-fail-cleanup-fail", "node-fail-collect-fail"):
        environment["TEST_PROGRAM_BEHAVIOR"] = "missing-marker"
    if action == "node" and scenario == "result-fail":
        environment["TEST_PROGRAM_BEHAVIOR"] = "result-fail"
    if action == "node" and scenario == "stale-result":
        command[5] = "old-run"
    sys.exit(subprocess.run(command, input=payload, text=True, env=environment).returncode)
elif kind == "rsync":
    src, dest = args[-2:]
    if ":" in src:
        if scenario in ("collect-fail", "node-fail-collect-fail"):
            print("injected log collection failure", file=sys.stderr)
            sys.exit(24)
        src = mapped(src.split(":", 1)[1])
        dest = pathlib.Path(dest)
        if scenario == "stale-result":
            src = src.parent / "old-run"
        shutil.copytree(src, dest, dirs_exist_ok=True)
        if scenario == "corrupt-result":
            (dest / "result.json").write_text("{invalid JSON}")
    else:
        if scenario == "transfer-fail":
            print("injected transfer failure", file=sys.stderr)
            sys.exit(25)
        dest = mapped(dest.split(":", 1)[1])
        listing = next(arg.split("=", 1)[1] for arg in args if arg.startswith("--files-from="))
        for name in pathlib.Path(listing).read_text().splitlines():
            source = pathlib.Path(src) / name
            target = dest / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target, dirs_exist_ok=True)
            else:
                shutil.copy2(source, target)
'''


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "集成回归需要 POSIX Python 和 Bash")
class WorkflowIntegrationTests(TemporaryRoot):
    def test_file_copy_manifest_syncs_and_runs_with_unknown_commit(self):
        (self.root / ".tilexr-source.json").unlink()
        self.manifest["source"] = workflow.inventory(self.root)
        self.assertIsNone(self.manifest["source"]["commit"])
        workflow.write_json(self.root / ".tilexr-source.json", self.manifest["source"])
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        self.assert_success(self.invoke("sync"))
        result = self.invoke("run")
        self.assert_success(result)
        self.assertIn("commit=unknown", result.stdout)
        summary_paths = list(self.config_instance.logs.glob("*/summary.json"))
        self.assertEqual(len(summary_paths), 1)
        summary = workflow.read_json(summary_paths[0])
        self.assertIsNone(summary["commit"])
        self.assertEqual(summary["source_provenance"], "file-copy")
        self.assertNotIn("source_sha256", summary)
        self.assertEqual(len(summary["nodes"]), 2)

    def setUp(self):
        super().setUp()
        self.remote = Path(self.temporary.name) / "192.0.2.2"
        self.hostfile.write_text("127.0.0.1\n192.0.2.2\n", encoding="utf-8")
        tool_root = self.root / "tools/validation"
        tool_root.mkdir(parents=True)
        for name in ("workflow.py", "source.py", "measurements.py"):
            shutil.copy2(Path(workflow.__file__).with_name(name), tool_root / name)
        profiler_source = Path(workflow.__file__).resolve().parents[1] / "ep/msprof_kernel_stats.py"
        profiler_target = self.root / "tools/ep/msprof_kernel_stats.py"
        profiler_target.parent.mkdir()
        shutil.copy2(profiler_source, profiler_target)
        profiling_source = Path(workflow.__file__).resolve().parents[1] / "profiling"
        profiling_target = self.root / "tools/profiling"
        shutil.copytree(profiling_source, profiling_target, ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copy2(Path(workflow.__file__).with_name("test_profiling_fixture.py"), tool_root)
        self.script = tool_root / "workflow.py"
        (self.root / "set_env.sh").write_text(
            'export TILEXR_VALIDATION_ROOT="$PWD"\n'
            'export TILEXR_VALIDATION_HOSTFILE="$PWD/hostfile"\n'
            'export TILEXR_VALIDATION_BUILD_MODE=host\n'
            f'export TILEXR_VALIDATION_PYTHON={shlex.quote(sys.executable)}\n', encoding="utf-8")
        (self.root / ".tilexr-validation-root").write_text("TileXR validation deployment\n", encoding="utf-8")
        helper = self.root / "test_program.py"
        helper.write_text(
            "import os, sys, time\n"
            "behavior = os.environ.get('TEST_PROGRAM_BEHAVIOR', 'pass')\n"
            "if behavior == 'nonzero':\n    print('operator failed'); sys.exit(9)\n"
            "if behavior == 'wait':\n    time.sleep(60)\n"
            "if behavior == 'result-fail':\n"
            "    print('EP_SERVER_DEDUP_ABI_HOST_FAIL test=' + sys.argv[1]); sys.exit(0)\n"
            "if behavior != 'missing-marker':\n"
            "    print('EP_SERVER_DEDUP_ABI_HOST_PASS test=' + sys.argv[1] +\n"
            "          (' unexpected-suffix' if behavior == 'decorated-marker' else ''))\n", encoding="utf-8")
        source = {"commit": "a" * 40, "provenance": "manifest-unverified",
                  "files": [str(path.relative_to(self.root))
                            for path in (self.script, tool_root / "source.py", tool_root / "measurements.py",
                                         profiler_target, self.root / "set_env.sh", helper)]}
        source["files"].extend(str(path.relative_to(self.root)) for path in profiling_target.rglob("*") if path.is_file())
        source["files"].append("tools/validation/test_profiling_fixture.py")
        workflow.write_json(self.root / ".tilexr-source.json", source)
        self.config_instance = self.config()
        self.config_instance.install.mkdir(parents=True)
        (self.config_instance.install / "artifact").write_bytes(b"test installed artifact\n")
        self.manifest = {"build_id": "test-build", "mode": "host", "checkpoint": workflow.CHECKPOINT,
                         "architecture": workflow.platform.machine(), "source": source,
                         "artifacts": workflow.artifact_files(self.config_instance.install),
                         "tests": [{"name": "test_tilexr_ep_server_dedup_" + name}
                                   for name in workflow.CHECKS]}
        # 每项使用独立无参数程序，覆盖安装后实际执行的测试协议。
        for test, name in zip(self.manifest["tests"], workflow.CHECKS):
            program = self.root / ("test_" + name + ".py")
            program.write_text(helper.read_text().replace("sys.argv[1]", repr(name)), encoding="utf-8")
            self.manifest["source"]["files"].append(program.name)
            test["argv"] = ["@python", program.name]
        workflow.write_json(self.root / ".tilexr-source.json", self.manifest["source"])
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        fake_bin = Path(self.temporary.name) / "bin"
        self.fake_bin = fake_bin
        fake_bin.mkdir()
        for name in ("ssh", "rsync"):
            program = fake_bin / name
            program.write_text("#!" + sys.executable + "\n" + FAKE_TRANSPORT, encoding="utf-8")
            program.chmod(0o755)
        bundled_bash = Path(sys.executable).parent / "bash"
        bash = str(bundled_bash) if bundled_bash.exists() else shutil.which("bash")
        self.bash = bash
        self.transport_env = {"FAKE_ROOT": str(self.root), "FAKE_REMOTE": str(self.remote), "FAKE_BASH": bash,
                              "PATH": os.pathsep.join((str(fake_bin), str(Path(bash).parent), os.environ["PATH"]))}
        environment = mock.patch.dict(os.environ, self.transport_env)
        environment.start()
        self.addCleanup(environment.stop)

    def invoke(self, action, *arguments, environment=None):
        return subprocess.run([sys.executable, str(self.script), action, *arguments],
                              env=dict(os.environ, **(environment or {})), cwd=self.root,
                              capture_output=True, text=True, timeout=25)

    def assert_success(self, command):
        self.assertEqual(command.returncode, 0, command.stdout + "\n" + command.stderr)

    def prepare_sync(self):
        self.assert_success(self.invoke("sync"))

    def summary(self):
        summaries = list(self.config_instance.logs.glob("*/summary.json"))
        self.assertEqual(len(summaries), 1)
        return workflow.read_json(summaries[0])

    def test_sync_and_run_complete_on_two_separate_nodes(self):
        self.prepare_sync()
        self.assertTrue((self.remote / "install/validation/manifest.json").exists())
        result = self.invoke("run")
        self.assert_success(result)
        summary = self.summary()
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual({node["host"] for node in summary["nodes"]}, {"127.0.0.1", "192.0.2.2"})
        self.assertTrue(all(node["tests"] == len(self.manifest["tests"]) for node in summary["nodes"]))
        self.assertEqual(summary["errors"], [])
        self.assertEqual(summary["commit"], self.manifest["source"]["commit"])
        self.assertEqual(summary["scope"], "ABI/Host；未执行 NPU launch 或去重通信。")
        self.assertIn("范围：" + summary["scope"], result.stdout)
        self.assertEqual(list(self.root.rglob("pid.json")), [])
        self.assertEqual(list(self.remote.rglob("pid.json")), [])

    def test_empty_kernel_uses_first_device_per_host_with_existing_entry_points(self):
        self.hostfile.write_text("127.0.0.1\n192.0.2.2\n", encoding="utf-8")
        entry = self.root / "set_env.sh"
        entry.write_text(entry.read_text().replace("BUILD_MODE=host", "BUILD_MODE=production"), encoding="utf-8")
        self.manifest.update(mode="production", checkpoint=workflow.EMPTY_KERNEL_CHECKPOINT)
        program = self.root / "registration.py"
        program.write_text("import os\n"
                           "print('device=' + os.environ['TILEXR_EP_SERVER_DEDUP_TEST_DEVICE'])\n"
                           "print('EP_SERVER_DEDUP_EMPTY_KERNEL_PASS test=registration')\n", encoding="utf-8")
        self.manifest["source"]["files"].append(program.name)
        self.manifest["tests"].append({"name": workflow.REGISTRATION_TEST, "argv": ["@python", program.name]})
        workflow.write_json(self.root / ".tilexr-source.json", self.manifest["source"])
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BUILD_MODE": "production",
                                        "TILEXR_VALIDATION_CHECKPOINT": workflow.EMPTY_KERNEL_CHECKPOINT}):
            self.prepare_sync()
            result = self.invoke("run")
            self.assert_success(result)
        self.assertEqual(self.summary()["status"], "PASS")
        self.assertEqual(self.summary()["scope"],
                         "ABI/Host、A5 空 kernel 安装与设备注册；未执行 NPU launch 或去重通信。")
        self.assertIn("范围：" + self.summary()["scope"], result.stdout)
        for root, device in ((self.root, 0), (self.remote, 0)):
            logs = list((root / "run/validation").glob("*/" + workflow.REGISTRATION_TEST + ".log"))
            self.assertEqual(len(logs), 1)
            self.assertIn(f"device={device}\n", logs[0].read_text())

    def test_empty_kernel_missing_registration_marker_fails(self):
        self.manifest.update(mode="production", checkpoint=workflow.EMPTY_KERNEL_CHECKPOINT)
        self.manifest["tests"] = [{"name": workflow.REGISTRATION_TEST, "argv": ["@python", "test_protocol.py"]}]
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        result = self.invoke("node", "registration-marker", self.manifest["build_id"], "10", "2",
                             environment={"TILEXR_VALIDATION_BUILD_MODE": "production",
                                          "TILEXR_VALIDATION_CHECKPOINT": workflow.EMPTY_KERNEL_CHECKPOINT})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LOG_PARSE_FAILED", result.stdout)

    def enable_dry_run(self, injected_rank_code=""):
        self.hostfile.write_text("127.0.0.1\n192.0.2.2\n", encoding="utf-8")
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.DRY_RUN_CHECKPOINT
        self.manifest["checkpoint"] = workflow.DRY_RUN_CHECKPOINT
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        if injected_rank_code:
            # 只改临时部署夹具；生产入口不增加故障开关。
            original = self.script.read_text(encoding="utf-8")
            wrapper = ("\n_original_rank = dry_run_rank\n"
                       "def dry_run_rank(config, run_id, build_id, rank):\n" +
                       injected_rank_code +
                       "\n    return _original_rank(config, run_id, build_id, rank)\n")
            self.script.write_text(original.replace('if __name__ == "__main__":',
                                                   wrapper + '\nif __name__ == "__main__":'), encoding="utf-8")

    def test_dry_run_sync_run_and_collect_all_16_rank_records(self):
        self.enable_dry_run()
        self.prepare_sync()
        command = self.invoke("run")
        self.assert_success(command)
        summary = self.summary()
        self.assertEqual((summary["rank_size"], summary["completed_ranks"]), (16, 16))
        self.assertIn("ranks=16/16", command.stdout)
        self.assertIn("不创建设备上下文或 communicator", summary["scope"])
        self.assertEqual([item["device"] for item in summary["rank_mapping"][:8]], list(range(8)))
        records = [rank for node in summary["nodes"] for rank in node["reported_result"]["ranks"]]
        workflow.validate_rank_records(records, summary["rank_mapping"], summary["run_id"], self.manifest)
        run_log = self.config_instance.logs / summary["run_id"]
        self.assertEqual(len(list(run_log.glob("rank-*.log"))), 8)
        self.assertEqual(len(list((run_log / "nodes/1").glob("rank-*.log"))), 8)
        self.assertEqual(list(self.root.rglob("pid.json")) + list(self.remote.rglob("pid.json")), [])

    def test_dry_run_rank_failure_preserves_first_rank_and_cancels_siblings(self):
        self.enable_dry_run(
            "    if int(rank) == 8:\n"
            "        print('injected rank failure', flush=True)\n"
            "        return 9\n"
            "    time.sleep(60)\n")
        self.prepare_sync()
        command = self.invoke("run")
        self.assertNotEqual(command.returncode, 0)
        summary = self.summary()
        self.assertEqual(summary["primary_error"]["rank"], 8)
        self.assertEqual(summary["primary_error"]["returncode"], 9)
        self.assertEqual(summary["primary_error"]["host"], "192.0.2.2")
        self.assertTrue(all(node["reported_result"]["cleanup"] == "complete" for node in summary["nodes"]))
        self.assertEqual(list(self.root.rglob("pid.json")) + list(self.remote.rglob("pid.json")), [])

    def test_dry_run_delayed_ssh_cannot_extend_cluster_execution_deadline(self):
        # TIMEOUT=30 给执行阶段 10 秒。远端延迟 5 秒，再运行 7 秒；必须取消，不能 PASS。
        self.enable_dry_run("    time.sleep(7)\n")
        self.prepare_sync()
        command = self.invoke("run", environment={"FAKE_SCENARIO": "delayed-node"})
        self.assertNotEqual(command.returncode, 0)
        summary = self.summary()
        self.assertTrue(summary["primary_error"]["timeout"])
        self.assertEqual(summary["primary_error"]["stage"], "execution")
        self.assertEqual(summary["primary_error"]["rank"], "unknown")
        self.assertTrue(all(node["reported_result"]["cleanup"] == "complete" for node in summary["nodes"]))
        self.assertEqual(list(self.root.rglob("pid.json")) + list(self.remote.rglob("pid.json")), [])

    def test_dry_run_rejects_missing_duplicate_malformed_and_stale_markers(self):
        self.enable_dry_run(
            "    if int(rank) == 0:\n"
            "        behavior = os.environ['RANK_TEST_BEHAVIOR']\n"
            "        if behavior == 'duplicate':\n"
            "            _original_rank(config, run_id, build_id, rank)\n"
            "            return _original_rank(config, run_id, build_id, rank)\n"
            "        if behavior == 'stale':\n"
            "            return _original_rank(config, 'old-run', build_id, rank)\n"
            "        if behavior == 'malformed':\n"
            "            print(DRY_RUN_MARKER + '{bad JSON}')\n"
            "        return 0\n"
            "    time.sleep(60)\n")
        for behavior in ("missing", "duplicate", "malformed", "stale"):
            with self.subTest(behavior=behavior):
                command = self.invoke("node", behavior, self.manifest["build_id"], "10", "", "0",
                                      environment={"RANK_TEST_BEHAVIOR": behavior})
                self.assertNotEqual(command.returncode, 0)
                result = workflow.read_json(self.config_instance.logs / behavior / "result.json")
                self.assertEqual(result["errors"][0]["rank"], 0)
                self.assertEqual(result["errors"][0]["type"],
                                 "RESULT_FAILED" if behavior == "stale" else "LOG_PARSE_FAILED")
                self.assertEqual(result["cleanup"], "complete")

    def start_hanging_dry_run(self, run_id, budget):
        self.enable_dry_run(
            "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "    child = subprocess.Popen([sys.executable, '-c',\n"
            "        'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(60)'])\n"
            "    (config.logs / run_id / ('worker-' + rank + '.pid')).write_text(\n"
            "        str(os.getpid()) + '\\n' + str(child.pid))\n"
            "    time.sleep(60)\n")
        process = subprocess.Popen([self.bash, "-s", "--", str(self.root), "node", run_id,
                                    self.manifest["build_id"], str(budget), "", "0"], cwd=self.root,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        self.addCleanup(lambda: workflow.stop_process(process))
        process.stdin.write(workflow.REMOTE_SCRIPT)
        process.stdin.close()
        process.stdin = None
        return process

    def assert_hanging_ranks_cleaned(self, run_id):
        log = self.config_instance.logs / run_id
        pids = list(log.glob("worker-*.pid"))
        self.assertTrue(pids, "必须实际启动会卡住的 rank")
        for path in pids:
            for pid in path.read_text().splitlines():
                ProcessTests.assert_process_stopped(self, int(pid))
        result = workflow.read_json(log / "result.json")
        self.assertEqual((result["status"], result["cleanup"]), ("FAIL", "complete"))
        self.assertFalse((log / "pid.json").exists())
        self.assertFalse((self.config_instance.logs / "node.lock").exists())

    def test_dry_run_watchdog_reaps_term_resistant_ranks_and_children(self):
        process = self.start_hanging_dry_run("rank-timeout", 3)
        stdout, stderr = process.communicate(timeout=15)
        self.assertNotEqual(process.returncode, 0, stdout + stderr)
        self.assert_hanging_ranks_cleaned("rank-timeout")

    def test_dry_run_hangup_reaps_term_resistant_ranks_and_children(self):
        process = self.start_hanging_dry_run("rank-hangup", 15)
        log = self.config_instance.logs / "rank-hangup"
        deadline = time.monotonic() + 8
        while len(list(log.glob("worker-*.pid"))) < 8 and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(len(list(log.glob("worker-*.pid"))), 8)
        node_pid = workflow.read_json(log / "pid.json")["pid"]
        os.kill(node_pid, signal.SIGHUP)  # 模拟控制连接断开向节点送达的 HUP。
        stdout, stderr = process.communicate(timeout=10)
        self.assertNotEqual(process.returncode, 0, stdout + stderr)
        self.assert_hanging_ranks_cleaned("rank-hangup")

    def enable_chain(self, checkpoint=workflow.DISPATCH_CHECKPOINT, run_mode="functional"):
        self.hostfile.write_text("127.0.0.1\n192.0.2.2\n", encoding="utf-8")
        os.environ.update(TILEXR_VALIDATION_CHECKPOINT=checkpoint,
                          TILEXR_VALIDATION_BUILD_MODE="production", TILEXR_COMM_ID="192.0.2.1:10177",
                          TILEXR_VALIDATION_RUN_MODE=run_mode)
        os.environ.pop("TILEXR_DEMO_BARRIER_ADDR", None)
        entry = self.root / "set_env.sh"
        entry.write_text(entry.read_text().replace("BUILD_MODE=host", "BUILD_MODE=production"), encoding="utf-8")
        self.manifest.update(checkpoint=checkpoint, mode="production", server_dedup_profiling=run_mode == "breakdown")
        if run_mode == "breakdown":
            self.manifest["server_dedup_profile_schema"] = workflow.measurements.RECORDS_SCHEMA
        self.manifest["server_dedup_contract"] = dict(workflow.DISPATCH_ABI)
        program = self.config_instance.install / "bin" / workflow.CHAIN_PROGRAM
        program.parent.mkdir()
        mapping = self.config_instance.install / "share/tilexr/profiling/server_dedup.mapping.json"
        mapping.parent.mkdir(parents=True)
        shutil.copy2(workflow.measurements.profiling_mapping(), mapping)
        # 传输/编排夹具使用普通进程产出原生协议，不把它作为 CANN launch 证据。
        program.write_text("#!" + sys.executable + "\n" + r'''
import json, os, sys
run, build, commit, host = sys.argv[1:5]
host_index, local_rank, device, rank, rank_size = map(int, sys.argv[5:10])
assert os.environ['TILEXR_COMM_ID'] == '192.0.2.1:10177'
assert os.environ['TILEXR_DEMO_BARRIER_ADDR'] == '192.0.2.1:10274'
assert os.environ['TILEXR_VALIDATION_BS'] == '96'
checkpoint = os.environ['TILEXR_VALIDATION_CHECKPOINT']
boundary = checkpoint == 'ep-sdedup-p1-memory-boundary'
loops = int(os.environ['TILEXR_VALIDATION_LOOPS'])
warmup = int(os.environ['TILEXR_VALIDATION_WARMUP'])
run_mode = os.environ['TILEXR_VALIDATION_RUN_MODE']
record = dict(run_id=run, build_id=build, commit=None if commit == 'unknown' else commit,
              host=host, host_index=host_index, local_rank=local_rank, device=device,
              rank=rank, rank_size=rank_size, pid=os.getpid(),
              checkpoint=checkpoint, status='PASS', loops=loops, warmup=warmup, run_mode=run_mode)
record.update({key: int(os.environ['TILEXR_VALIDATION_' + key.upper()])
               for key in ('bs', 'h', 'topk', 'moe_expert_num')})
record['dtype'] = os.environ['TILEXR_VALIDATION_DTYPE']
record.update(route_mode='balance_shift', seed=2026, quant_mode=int(os.environ['TILEXR_VALIDATION_QUANT_MODE']),
              fp8_format=os.environ['TILEXR_VALIDATION_FP8_FORMAT'], kernel_scope='dispatch', native_status='dispatch',
              first_hop_packets=48, first_hop_received=48, first_hop_bytes=48*15872, token_stride=15872,
              direct_packets=288, direct_received=288, direct_bytes=288*15872,
              local_data_bytes=24*15872, same_server_data_bytes=504*15872, cross_server_data_bytes=48*15872,
              relay_packets=240, relay_received=240, relay_bytes=240*15872, data_bytes=576*15872,
              relay_validated=True, dispatch_outputs_validated=True, group_validated=True, flags_cleared=True, counter_cleared=True,
              first_hop_validated=True, direct_validated=True, route_coverage_validated=True, source_outputs_validated=True, weights_unchanged=True)
previous_relay = rank // 8 * 8 + (rank + 7) % 8
record['relay_links'] = [[(rank + 8) % rank_size, token * 2, 0, rank, token, 0, 5] for token in range(48)] + [
    [(previous_relay + 8) % rank_size, token * 2, 0, previous_relay, token, k, -1] for token in range(48) for k in range(1, 6)]
behavior = os.environ.get('CHAIN_TEST_BEHAVIOR', '') if rank == 8 else ''
if behavior == 'wrong-relay-index':
    record['relay_links'][-1][4] += 1
if behavior == 'missing-relay-link':
    record['relay_links'].pop()
if behavior == 'wrong-shape':
    record['bs'] += 1
if behavior == 'launch-failure':
    print('stage=launch code=507000', flush=True)
    sys.exit(9)
if behavior != 'missing-chain':
    print('EP_SERVER_DEDUP_CHAIN_PASS ' + json.dumps(dict(record, peer_count=rank_size,
                                                       peer_mem_bytes=4 * 1024 ** 3)))
if behavior == 'stale-round':
    record['run_id'] = 'old-run'
if run_mode != 'functional':
    record['timing_us'] = dict(submit=4, sync=3, barrier=2, batch_total=10)
if run_mode == 'breakdown':
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools/validation'))
    from test_profiling_fixture import write_capture
    path = os.environ['TILEXR_PROFILING_RECORDS_PATH']
    capture_id = os.environ['TILEXR_PROFILING_CAPTURE_ID']
    assert int(os.environ['TILEXR_PROFILING_RECORDS_PER_CORE']) == (loops + warmup) * 65536
    if behavior != 'missing-breakdown':
        write_capture(path, rank=rank, launches=warmup+loops, warmup=warmup,
                      fault={'stale-breakdown': 'stale', 'overflow-breakdown': 'overflow'}.get(behavior, ''), capture_id=capture_id)
    print('EP_SERVER_DEDUP_BREAKDOWN_PASS ' + json.dumps(dict(record,
          schema='tilexr_profiling_records.v1', records_path=path, capture_id=capture_id, launches=warmup+loops)))
if behavior == 'wrong-loops':
    record['loops'] = loops - 1
prefix = 'EP_SERVER_DEDUP_DISPATCH_PASS '
if boundary:
    prefix = 'EP_SERVER_DEDUP_MEMORY_BOUNDARY_PASS '
    record.update(peer_mem_bytes=4 * 1024 ** 3, boundary_offset=4 * 1024 ** 3 - 32, probe_bytes=32,
                  initial_zero=True, device_initial_zero=True, device_readback_ff=True,
                  host_boundary_ff=behavior != 'boundary-mismatch', guard_zero=True)
if behavior == 'single-marker':
    prefix = 'EP_SERVER_DEDUP_SINGLE_ROUND_PASS '
print(prefix + json.dumps(dict(record, submitted=loops,
      completed=loops - 1 if behavior == 'incomplete-round' else loops,
      warmup_submitted=warmup, warmup_completed=warmup, cleanup='complete')))
''', encoding="utf-8")
        program.chmod(0o755)
        kernel = self.config_instance.install / "lib/tilexr/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.o"
        kernel.parent.mkdir(parents=True)
        kernel.write_bytes(b"fake kernel artifact for workflow only")
        self.manifest["artifacts"] = workflow.artifact_files(self.config_instance.install)
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)

    def test_chain_single_sync_run_and_collect_native_rank_protocol(self):
        self.enable_chain()
        self.prepare_sync()
        command = self.invoke("run")
        self.assert_success(command)
        summary = self.summary()
        self.assertEqual((summary["rank_size"], summary["completed_ranks"]), (16, 16))
        self.assertEqual(summary["chain_configuration"]["loops"], 1)
        self.assertEqual(summary["chain_configuration"]["dtype"], "bf16")
        self.assertTrue(summary["relay_links_validated"])
        self.assertIn("最后一轮 cnt/cumsum", summary["scope"])
        self.assertNotIn("未执行 NPU launch", summary["scope"])
        for node in summary["nodes"]:
            self.assertEqual(node["tests"], len(self.manifest["tests"]))
            for rank in node["reported_result"]["ranks"]:
                self.assertEqual(rank["chain"]["peer_count"], 16)
                self.assertEqual(rank["completed"], 1)

    def test_memory_boundary_sync_run_collects_16_ranks_without_old_tests(self):
        self.enable_chain(workflow.MEMORY_BOUNDARY_CHECKPOINT)
        self.manifest["tests"] = []
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        self.prepare_sync()
        self.assert_success(self.invoke("run"))
        summary = self.summary()
        self.assertEqual((summary["rank_size"], summary["completed_ranks"]), (16, 16))
        self.assertIn("4 GiB", summary["scope"])
        self.assertNotIn("空 Dispatch launch", summary["scope"])
        for node in summary["nodes"]:
            self.assertEqual(node["reported_result"]["tests"], [])
            for rank in node["reported_result"]["ranks"]:
                self.assertIs(rank["host_boundary_ff"], True)

    def test_memory_boundary_one_rank_mismatch_fails_node(self):
        self.enable_chain(workflow.MEMORY_BOUNDARY_CHECKPOINT)
        self.manifest["tests"] = []
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        command = self.invoke("node", "boundary-failure", self.manifest["build_id"], "10", "", "1",
                              environment={"CHAIN_TEST_BEHAVIOR": "boundary-mismatch"})
        self.assertNotEqual(command.returncode, 0)
        result = workflow.read_json(self.config_instance.logs / "boundary-failure/result.json")
        self.assertEqual(result["errors"][0]["type"], "RESULT_FAILED")
        self.assertEqual(result["errors"][0]["rank"], 8)

    def test_chain_single_missing_stale_or_incomplete_evidence_fails(self):
        self.enable_chain()
        for behavior, kind in (("missing-chain", "LOG_PARSE_FAILED"), ("stale-round", "RESULT_FAILED"),
                               ("incomplete-round", "RESULT_FAILED"), ("launch-failure", "EXIT_FAILED"),
                               ("wrong-shape", "RESULT_FAILED")):
            with self.subTest(behavior=behavior):
                command = self.invoke("node", behavior, self.manifest["build_id"], "10", "", "1",
                                      environment={"CHAIN_TEST_BEHAVIOR": behavior})
                self.assertNotEqual(command.returncode, 0)
                result = workflow.read_json(self.config_instance.logs / behavior / "result.json")
                self.assertEqual(result["errors"][0]["rank"], 8)
                self.assertEqual(result["errors"][0]["type"], kind)
                self.assertEqual(result["cleanup"], "complete")

    def test_dispatch_runs_multiple_submissions(self):
        self.enable_chain()
        self.prepare_sync()
        for key, value in (("TILEXR_VALIDATION_LOOPS", "7"), ("TILEXR_VALIDATION_WARMUP", "2")):
            command = self.invoke("run", environment={key: value, "TILEXR_DIRECT_RUN": "1"})
            self.assertEqual(command.returncode, 0, command.stdout + command.stderr)

    def test_one_sync_to_eight_servers_runs_16_32_48_64p_and_different_experts(self):
        self.enable_chain()
        ips = ["127.0.0.1"] + [f"192.0.2.{i}" for i in range(2, 9)]
        self.hostfile.write_text("\n".join(ips) + "\n")
        remotes = {ip: str(Path(self.temporary.name) / ip) for ip in ips[1:]}
        with mock.patch.dict(os.environ, {"FAKE_REMOTE_MAP": workflow.json.dumps(remotes)}):
            # Distribution does not need a valid endpoint, expert count or run mode.
            self.assert_success(self.invoke("sync", environment={
                "TILEXR_VALIDATION_MOE_EXPERT_NUM": "invalid", "TILEXR_VALIDATION_BS": "invalid",
                "TILEXR_VALIDATION_SERVERS": "invalid", "TILEXR_VALIDATION_RUN_MODE": "invalid",
                "TILEXR_COMM_ID": "invalid"}))
            receipt = (self.config_instance.logs / "sync.json").read_bytes()
            for servers, experts in ((2, 96), (4, 192), (6, 288), (8, 384), (8, 512)):
                with self.subTest(servers=servers, experts=experts):
                    command = self.invoke("run", environment={"TILEXR_VALIDATION_SERVERS": str(servers),
                        "TILEXR_VALIDATION_MOE_EXPERT_NUM": str(experts), "TILEXR_VALIDATION_LOOPS": "3",
                        "TILEXR_VALIDATION_WARMUP": "1"})
                    self.assert_success(command)
                    summaries = list(self.config_instance.logs.glob("*/summary.json"))
                    summary = workflow.read_json(max(summaries, key=lambda p: p.stat().st_mtime_ns))
                    self.assertEqual(summary["build_id"], self.manifest["build_id"])
                    self.assertEqual(summary["completed_ranks"], servers * 8)
                    self.assertEqual([h["host"] for h in summary["expected_hosts"]], ips[:servers])
                    self.assertEqual(summary["chain_configuration"]["moe_expert_num"], experts)
                    for ip in ips[servers:]:
                        self.assertFalse((Path(remotes[ip]) / "run/validation" / summary["run_id"]).exists())
            self.assertEqual((self.config_instance.logs / "sync.json").read_bytes(), receipt)

    def test_selected_node_artifact_failure_prevents_all_rank_launches(self):
        self.enable_chain()
        self.prepare_sync()
        (self.remote / "install/validation/artifact").write_text("stale binary")
        command = self.invoke("run")
        self.assertNotEqual(command.returncode, 0)
        self.assertIn("192.0.2.2", command.stderr)
        self.assertFalse(list(self.config_instance.logs.glob("*/launch-*.log")))
        self.assertFalse(list(self.config_instance.logs.glob("*/pid.json")))

    def test_dispatch_cross_rank_relay_mismatch_fails_functional_summary(self):
        self.enable_chain()
        self.prepare_sync()
        command = self.invoke("run", environment={"CHAIN_TEST_BEHAVIOR": "wrong-relay-index"})
        self.assertNotEqual(command.returncode, 0)
        summary = self.summary()
        self.assertEqual(summary["primary_error"]["type"], "RESULT_FAILED")
        self.assertIn("终端 t", summary["primary_error"]["message"])
        self.assertNotIn("relay_links_validated", summary)

    def test_chain_multi_rejects_missing_rounds_or_single_marker(self):
        self.enable_chain(workflow.DISPATCH_CHECKPOINT)
        for behavior, kind in (("incomplete-round", "RESULT_FAILED"), ("single-marker", "LOG_PARSE_FAILED"),
                               ("wrong-loops", "RESULT_FAILED"), ("stale-round", "RESULT_FAILED")):
            with self.subTest(behavior=behavior):
                command = self.invoke("node", behavior, self.manifest["build_id"], "10", "", "1",
                                      environment={"CHAIN_TEST_BEHAVIOR": behavior})
                self.assertNotEqual(command.returncode, 0)
                result = workflow.read_json(self.config_instance.logs / behavior / "result.json")
                self.assertEqual(result["errors"][0]["rank"], 8)
                self.assertEqual(result["errors"][0]["type"], kind)
                self.assertEqual(result["cleanup"], "complete")

    def enable_msprof(self):
        profiler = self.fake_bin / "msprof"
        profiler.write_text("#!" + sys.executable + "\n" + r'''
import csv, os, pathlib, shlex, subprocess, sys
args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:])
command = shlex.split(args['application'])
subprocess.run(command, check=True)
output = pathlib.Path(args['output']) / 'PROF_test/mindstudio_profiler_output/op_summary_test.csv'
output.parent.mkdir(parents=True)
count = int(os.environ['TILEXR_VALIDATION_LOOPS']) + int(os.environ['TILEXR_VALIDATION_WARMUP'])
if os.environ.get('PROFILE_TEST_BEHAVIOR') == 'missing-sample' and command[-2] == '8':
    count -= 1
with output.open('w', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(['Op Name', 'Task Start Time(us)', 'Task Duration(us)'])
    for index in range(count):
        writer.writerow(['tilexr_ep_dispatch_memory_server_dedup_kernel', index * 10, index + 1])
''', encoding="utf-8")
        profiler.chmod(0o755)

    def test_dispatch_performance_collects_multiple_samples_after_warmup(self):
        self.enable_chain(workflow.DISPATCH_CHECKPOINT, "performance")
        self.enable_msprof()
        os.environ.update(TILEXR_VALIDATION_RUN_MODE="performance", TILEXR_VALIDATION_LOOPS="3",
                          TILEXR_VALIDATION_WARMUP="2")
        self.prepare_sync()
        command = self.invoke("run")
        self.assert_success(command)
        summary = self.summary()
        self.assertEqual(summary["measurements"]["run_mode"], "performance")
        for node in summary["nodes"]:
            for rank in node["reported_result"]["ranks"]:
                self.assertEqual(rank["performance"]["sample_us"], [3, 4, 5])
                self.assertEqual(rank["performance"]["median_us"], 4)
        self.assertEqual(summary["measurements"]["measurement"], "continuous_batch")
        self.assertEqual(summary["measurements"]["submission"], "continuous_batch_then_sync_and_host_barrier")

    def test_dispatch_performance_missing_profiler_samples_fails(self):
        self.enable_chain(workflow.DISPATCH_CHECKPOINT, "performance")
        self.enable_msprof()
        os.environ.update(TILEXR_VALIDATION_RUN_MODE="performance", TILEXR_VALIDATION_LOOPS="1",
                          TILEXR_VALIDATION_WARMUP="0")
        command = self.invoke("node", "missing-sample", self.manifest["build_id"], "10", "", "1",
                              environment={"PROFILE_TEST_BEHAVIOR": "missing-sample"})
        self.assertNotEqual(command.returncode, 0)
        result = workflow.read_json(self.config_instance.logs / "missing-sample/result.json")
        self.assertEqual(result["errors"][0]["type"], "RESULT_FAILED")
        self.assertEqual(result["errors"][0]["rank"], 8)

    def test_dispatch_breakdown_collects_all_rank_records(self):
        self.enable_chain(workflow.DISPATCH_CHECKPOINT, "breakdown")
        os.environ.update(TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="3",
                          TILEXR_VALIDATION_WARMUP="2")
        # A plain sync has functional defaults, even when the installed kernel is profiled.
        ordinary_mode = dict(TILEXR_VALIDATION_RUN_MODE="functional", TILEXR_VALIDATION_LOOPS="1",
                             TILEXR_VALIDATION_WARMUP="0")
        self.assert_success(self.invoke("sync", environment=ordinary_mode))
        mismatch = self.invoke("run", environment=ordinary_mode)
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertIn("设备打点开关", mismatch.stderr)
        # run.sh forwards case arguments independently of the configuration used to sync.
        self.assert_success(self.invoke("run", environment={"TILEXR_DIRECT_RUN": "1"}))
        summary = self.summary()
        self.assertEqual(summary["measurements"]["run_mode"], "breakdown")
        self.assertEqual(summary["measurements"]["breakdown"]["schema"], workflow.measurements.RECORDS_SCHEMA)
        self.assertIn("metadata", summary["measurements"]["breakdown"]["stages_us"])
        self.assertIn("atomic", summary["measurements"]["breakdown"]["stages_us"])
        self.assertEqual(len(summary["traces"]["ranks"]), 16)
        trace = workflow.read_json(Path(summary["traces"]["all_ranks"]))
        self.assertEqual({event["args"]["rank"] for event in trace["traceEvents"] if event["ph"] == "X"}, set(range(16)))
        for pid in {event["pid"] for event in trace["traceEvents"] if event["ph"] == "X"}:
            ordered = [event["args"]["sort_index"] for event in trace["traceEvents"]
                       if event["name"] == "thread_sort_index" and event["pid"] == pid]
            self.assertEqual(ordered, list(range(64)))
        for node in summary["nodes"]:
            for rank in node["reported_result"]["ranks"]:
                self.assertEqual(rank["breakdown"]["launches"], 5)
                for artifact in rank["breakdown"]["artifacts"].values():
                    self.assertTrue(Path(artifact).is_file(), artifact)
                    if rank["rank"] >= 8:
                        self.assertIn("nodes/1", Path(artifact).as_posix())

    def test_dispatch_breakdown_missing_or_stale_record_fails(self):
        self.enable_chain(workflow.DISPATCH_CHECKPOINT, "breakdown")
        os.environ["TILEXR_VALIDATION_RUN_MODE"] = "breakdown"
        for behavior in ("missing-breakdown", "stale-breakdown"):
            with self.subTest(behavior=behavior):
                command = self.invoke("node", behavior, self.manifest["build_id"], "10", "", "1",
                                      environment={"CHAIN_TEST_BEHAVIOR": behavior})
                self.assertNotEqual(command.returncode, 0)

    def test_capture_overflow_preserves_communication_and_report_evidence(self):
        self.enable_chain(workflow.DISPATCH_CHECKPOINT, "breakdown")
        os.environ.update(TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="1",
                          TILEXR_VALIDATION_WARMUP="0")
        command = self.invoke("node", "capture-overflow", self.manifest["build_id"], "20", "", "1",
                              environment={"CHAIN_TEST_BEHAVIOR": "overflow-breakdown"})
        self.assertNotEqual(command.returncode, 0, command.stdout + command.stderr)
        result = workflow.read_json(self.config_instance.logs / "capture-overflow/result.json")
        self.assertEqual(result["status"], "FAIL")
        failure = next(item for item in result["errors"] if item.get("rank") == 8)
        self.assertTrue(failure["communication_completed"])
        self.assertEqual(failure["profile_status"], "incomplete")
        self.assertTrue(Path(failure["records_path"]).is_file())
        self.assertTrue((Path(failure["report_dir"]) / "report.html").is_file())
        evidence = next(item for item in result["profiling_failures"] if item["rank"] == 8)
        self.assertEqual(evidence["communication"]["cleanup"], "complete")
        self.assertEqual(evidence["communication"]["completed"], 1)
        self.assertTrue(evidence["communication"]["dispatch_outputs_validated"])
        self.prepare_sync()
        command = self.invoke("run", environment={"CHAIN_TEST_BEHAVIOR": "overflow-breakdown"})
        self.assertNotEqual(command.returncode, 0, command.stdout + command.stderr)
        summary = self.summary()
        failure = next(item for item in summary["errors"] if item.get("rank") == 8
                       and item.get("profile_status") == "incomplete")
        remote = next(node for node in summary["nodes"] if node["host"] == "192.0.2.2")
        evidence = next(item for item in remote["reported_result"]["profiling_failures"] if item["rank"] == 8)
        self.assertTrue(remote["reported_result"]["ranks"], "mixed node must retain successful ranks")
        for successful in remote["reported_result"]["ranks"]:
            for artifact in successful["breakdown"]["artifacts"].values():
                self.assertIn("nodes/1", Path(artifact).as_posix())
                self.assertTrue(Path(artifact).is_file(), artifact)
        for artifact in evidence["communication"]["breakdown"]["artifacts"].values():
            self.assertIn("nodes/1", Path(artifact).as_posix())
            self.assertTrue(Path(artifact).is_file(), artifact)
        self.assertIn("nodes/1", Path(failure["records_path"]).as_posix())
        self.assertTrue(Path(failure["records_path"]).is_file())
        self.assertTrue((Path(failure["report_dir"]) / "report.html").is_file())

    def test_measurement_aggregation_failure_preserves_summary_and_exit_code(self):
        self.enable_chain(workflow.DISPATCH_CHECKPOINT, "breakdown")
        os.environ.update(TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="1",
                          TILEXR_VALIDATION_WARMUP="0")
        self.prepare_sync()
        with mock.patch.object(workflow.measurements, "summarize_measurements",
                               side_effect=ValueError("invalid measurement aggregate")), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = workflow.run(workflow.Config())
        self.assertEqual(code, workflow.FAILURE_CODES["RESULT_FAILED"])
        summary = self.summary()
        self.assertEqual(summary["status"], "FAIL")
        self.assertEqual(summary["primary_error"]["type"], "RESULT_FAILED")
        self.assertNotIn("measurements", summary)

    def test_sync_failure_invalidates_previous_receipt(self):
        self.prepare_sync()
        for scenario in ("transfer-fail", "verify-fail"):
            with self.subTest(scenario=scenario):
                workflow.write_json(self.config_instance.logs / "sync.json", {"stale": True})
                result = self.invoke("sync", environment={"FAKE_SCENARIO": scenario})
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.config_instance.logs / "sync.json").exists())

    def test_resync_removes_retired_artifacts_and_local_configuration(self):
        self.prepare_sync()
        stale_artifact = self.remote / "install/validation/old-artifact"
        stale_artifact.write_bytes(b"previous build\n")
        stale_config = self.remote / "validation.local.sh"
        stale_config.write_text("export OLD_CONFIGURATION=1\n", encoding="utf-8")
        self.prepare_sync()
        self.assertFalse(stale_artifact.exists())
        self.assertFalse(stale_config.exists())

    def test_unmanaged_destination_is_preserved_and_rejected(self):
        self.remote.mkdir()
        sentinel = self.remote / "unrelated-work"
        sentinel.write_bytes(b"must remain\n")
        result = self.invoke("sync")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"must remain\n")
        self.assertFalse((self.config_instance.logs / "sync.json").exists())

    def test_active_node_lock_blocks_new_controller_operations(self):
        lock = self.config_instance.logs / "node.lock"
        lock.mkdir(parents=True)
        for action in ("build", "sync", "run"):
            with self.subTest(action=action):
                result = self.invoke(action)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(lock.is_dir())
                self.assertFalse((self.config_instance.logs / ".operation.lock").exists())

    def test_run_rejects_missing_remote_installation(self):
        result = self.invoke("run")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("VALIDATION_PASS", result.stdout)

    def test_case_and_timeout_changes_reuse_installed_artifacts(self):
        self.prepare_sync()
        (self.root / "validation.local.sh").write_text("# changed local config\n", encoding="utf-8")
        with self.hostfile.open("a") as output:
            output.write("# comment only\n\n")
        self.assert_success(self.invoke("run"))
        result = self.invoke("run", environment={"TILEXR_VALIDATION_TIMEOUT": "60"})
        self.assert_success(result)

    def test_modified_installed_artifact_blocks_launch(self):
        self.prepare_sync()
        (self.config_instance.install / "artifact").write_bytes(b"different build\n")
        result = self.invoke("run")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("VALIDATION_PASS", result.stdout)

    def test_modified_source_is_not_verified(self):
        self.prepare_sync()
        with (self.root / "test_protocol.py").open("a") as output:
            output.write("\n# harmless source change after sync\n")
        (self.root / "new-document.md").write_text("changed source inventory\n", encoding="utf-8")
        result = self.invoke("run")
        self.assert_success(result)

    def test_remote_zero_exit_without_marker_fails_whole_run(self):
        self.prepare_sync()
        result = self.invoke("run", environment={"FAKE_SCENARIO": "node-fail"})
        self.assertNotEqual(result.returncode, 0)
        summary = self.summary()
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(any(error.get("host") == "192.0.2.2" for error in summary["errors"]))
        self.assertIn("PASS/FAIL marker", result.stdout + result.stderr)
        remote_errors = [error["type"] for error in summary["errors"] if error.get("host") == "192.0.2.2"]
        self.assertEqual(remote_errors, ["LOG_PARSE_FAILED"])

    def assert_remote_failure(self, scenario, kind):
        self.prepare_sync()
        command = self.invoke("run", environment={"FAKE_SCENARIO": scenario})
        self.assertNotEqual(command.returncode, 0)
        summary = self.summary()
        self.assertEqual(summary["primary_error"]["type"], kind, summary)
        return summary

    def test_ssh_failure_is_launch_failure(self):
        summary = self.assert_remote_failure("ssh-fail", "LAUNCH_FAILED")
        self.assertFalse(any(error["type"] in ("EXIT_FAILED", "RESULT_FAILED", "LOG_PARSE_FAILED")
                             for error in summary["errors"] if error.get("host") == "192.0.2.2"))

    def test_abnormal_exit_after_start_is_exit_failure(self):
        self.assert_remote_failure("exit-after-start", "EXIT_FAILED")

    def test_success_without_completion_is_log_parse_failure(self):
        self.assert_remote_failure("no-completion", "LOG_PARSE_FAILED")

    def test_invalid_completion_is_log_parse_failure(self):
        self.assert_remote_failure("bad-completion", "LOG_PARSE_FAILED")

    def test_completed_fail_marker_is_result_failure(self):
        summary = self.assert_remote_failure("result-fail", "RESULT_FAILED")
        self.assertEqual([error["type"] for error in summary["errors"] if error.get("host") == "192.0.2.2"],
                         ["RESULT_FAILED"])

    def test_corrupt_collected_json_is_log_parse_failure(self):
        self.assert_remote_failure("corrupt-result", "LOG_PARSE_FAILED")

    def test_collection_failure_preserves_test_failure_from_launch_log(self):
        summary = self.assert_remote_failure("node-fail-collect-fail", "LOG_PARSE_FAILED")
        self.assertEqual({error["type"] for error in summary["errors"] if error.get("host") == "192.0.2.2"},
                         {"LOG_PARSE_FAILED", "LOG_COLLECT_FAILED"})

    def test_cleanup_failure_keeps_primary_error_and_controller_lock(self):
        summary = self.assert_remote_failure("node-fail-cleanup-fail", "LOG_PARSE_FAILED")
        self.assertIn("CLEANUP_FAILED", {error["type"] for error in summary["errors"]})
        self.assertTrue((self.config_instance.logs / ".operation.lock").exists())

    def test_cleanup_failure_never_records_complete_or_removes_node_lock(self):
        config = self.config_instance
        handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGHUP)}
        try:
            with mock.patch.object(workflow, "execute", side_effect=workflow.Failure("CLEANUP_FAILED", "denied")):
                with workflow.operation_lock(config, "node.lock"):
                    status = workflow.node(config, "cleanup-failure", self.manifest["build_id"])
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
        self.assertNotEqual(status, 0)
        recorded = workflow.read_json(config.logs / "cleanup-failure/result.json")
        self.assertEqual(recorded["cleanup"], "failed")
        self.assertTrue((config.logs / "node.lock").exists())
        self.assertTrue((config.logs / "cleanup-failure/pid.json").exists())

    def test_remote_cancel_does_not_source_broken_environment_or_parse_config(self):
        (self.root / "set_env.sh").write_text("exit 88\n", encoding="utf-8")
        environment = dict(os.environ, TILEXR_VALIDATION_TIMEOUT="invalid", TILEXR_VALIDATION_BUILD_MODE="invalid")
        with mock.patch.dict(os.environ, environment):
            workflow.execute([self.bash, "-s", "--", self.root, "cancel", "minimal-cancel"],
                             self.root / "cancel-minimal.log", stdin=workflow.REMOTE_SCRIPT)
        self.assertTrue((self.config_instance.logs / "minimal-cancel/cancelled").exists())

    def test_log_collection_failure_fails_whole_run(self):
        self.prepare_sync()
        result = self.invoke("run", environment={"FAKE_SCENARIO": "collect-fail"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.summary()["status"], "FAIL")
        self.assertTrue(any(error["type"] == "LOG_COLLECT_FAILED" for error in self.summary()["errors"]))
        remote = next(node for node in self.summary()["nodes"] if node["host"] == "192.0.2.2")
        self.assertEqual(remote["status"], "FAIL")
        self.assertEqual(remote["reported_result"]["status"], "PASS")
        self.assertEqual(remote["tests"], len(self.manifest["tests"]))

    def test_stale_remote_success_is_rejected(self):
        self.prepare_sync()
        result = self.invoke("run", environment={"FAKE_SCENARIO": "stale-result"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.summary()["status"], "FAIL")

    def test_node_failure_still_records_result_and_cleanup(self):
        for behavior, kind in (("nonzero", "EXIT_FAILED"), ("missing-marker", "LOG_PARSE_FAILED"),
                               ("decorated-marker", "LOG_PARSE_FAILED"), ("result-fail", "RESULT_FAILED")):
            with self.subTest(behavior=behavior):
                run_id = behavior
                result = self.invoke("node", run_id, self.manifest["build_id"],
                                     environment={"TEST_PROGRAM_BEHAVIOR": behavior})
                self.assertNotEqual(result.returncode, 0)
                node_log = self.config_instance.logs / run_id
                recorded = workflow.read_json(node_log / "result.json")
                self.assertEqual(recorded["status"], "FAIL")
                self.assertEqual(recorded["cleanup"], "complete")
                self.assertFalse((node_log / "pid.json").exists())
                self.assertEqual(recorded["tests"][0]["status"], "FAIL")
                self.assertEqual(recorded["errors"][0]["type"], kind)

    def test_node_rejects_request_for_different_build(self):
        result = self.invoke("node", "wrong-build-run", "another-build")
        self.assertNotEqual(result.returncode, 0)
        recorded = workflow.read_json(self.config_instance.logs / "wrong-build-run/result.json")
        self.assertEqual(recorded["status"], "FAIL")
        self.assertEqual(recorded["tests"], [])

    def test_cancel_before_node_starts_prevents_any_test_execution(self):
        run_id = "cancel-before-start"
        self.assert_success(self.invoke("cancel", run_id))
        result = self.invoke("node", run_id, self.manifest["build_id"])
        self.assertNotEqual(result.returncode, 0)
        node_log = self.config_instance.logs / run_id
        recorded = workflow.read_json(node_log / "result.json")
        self.assertEqual(recorded["status"], "FAIL")
        self.assertEqual(recorded["tests"], [])
        self.assertEqual(recorded["cleanup"], "complete")
        self.assertFalse((node_log / "pid.json").exists())
        self.assertEqual(list(node_log.glob("test_*.log")), [])

    def test_node_never_repeats_production_library_checks(self):
        self.manifest["mode"] = "production"
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        started_file = self.root / "preflight-started"
        for name in ("nm", "readelf", "ldd"):
            tool = self.fake_bin / name
            tool.write_text("#!" + sys.executable + "\nimport pathlib, time\n"
                            f"pathlib.Path({str(started_file)!r}).touch()\nraise SystemExit(99)\n", encoding="utf-8")
            tool.chmod(0o755)
        started = time.monotonic()
        result = self.invoke("node", "no-preflight", self.manifest["build_id"], "10",
                             environment={"TILEXR_VALIDATION_BUILD_MODE": "production"})
        self.assertLess(time.monotonic() - started, 6)
        self.assertFalse(started_file.exists(), "节点不能再次执行生产检查工具")
        self.assert_success(result)

    def test_remote_watchdog_includes_slow_environment_preamble(self):
        (self.root / "set_env.sh").write_text(
            "echo preamble-started\nsleep 60\necho preamble-finished\n", encoding="utf-8")
        log = self.root / "watchdog.log"
        started = time.monotonic()
        with self.assertRaisesRegex(RuntimeError, "exit=124"):
            workflow.execute([self.bash, "-s", "--", self.root, "node", "slow-env", "build", "0.5"],
                             log, timeout=5, stdin=workflow.REMOTE_SCRIPT)
        self.assertLess(time.monotonic() - started, 4)
        output = log.read_text(encoding="utf-8")
        self.assertIn("preamble-started", output)
        self.assertNotIn("preamble-finished", output)
        self.assertEqual(list(self.config_instance.logs.glob("*/result.json")), [])

    def test_cancel_running_node_waits_for_cleanup_and_failed_result(self):
        run_id = "cancel-running"
        process = subprocess.Popen([sys.executable, str(self.script), "node", run_id,
                                    self.manifest["build_id"]], cwd=self.root,
                                   env=dict(os.environ, TEST_PROGRAM_BEHAVIOR="wait"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        self.addCleanup(lambda: workflow.stop_process(process))
        node_log = self.config_instance.logs / run_id
        started = node_log / "test_tilexr_ep_server_dedup_protocol.log"
        deadline = time.monotonic() + 5
        while not started.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(started.exists(), "取消前必须实际启动节点测试")
        self.assert_success(self.invoke("cancel", run_id))
        stdout, stderr = process.communicate(timeout=10)
        self.assertNotEqual(process.returncode, 0, stdout + stderr)
        recorded = workflow.read_json(node_log / "result.json")
        self.assertEqual(recorded["status"], "FAIL")
        self.assertEqual(recorded["cleanup"], "complete")
        self.assertFalse((node_log / "pid.json").exists())


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "环境入口回归需要 POSIX Python 和 Bash")
class EnvironmentEntryTests(TemporaryRoot):
    def source_real_entry(self, hostfile=None):
        repo = Path(workflow.__file__).resolve().parents[2]
        for name in ("set_env.sh", "scripts/common_env.sh", "scripts/common_util.sh",
                     "tools/validation/workflow.py", "tools/validation/source.py", "tools/validation/measurements.py",
                     "tools/ep/msprof_kernel_stats.py"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((repo / name).read_text(encoding="utf-8"), encoding="utf-8")
        bundled_bash = Path(sys.executable).parent / "bash"
        bash = str(bundled_bash) if bundled_bash.exists() else shutil.which("bash")
        environment = dict(os.environ, TILEXR_VALIDATION_BUILD_MODE="host",
                           TILEXR_VALIDATION_PYTHON=sys.executable,
                           PATH=str(Path(bash).parent) + os.pathsep + os.environ["PATH"])
        if hostfile is None:
            environment.pop("TILEXR_VALIDATION_HOSTFILE", None)
        else:
            environment["TILEXR_VALIDATION_HOSTFILE"] = hostfile
        return subprocess.run([bash, "-c", 'source "$1/set_env.sh"', "bash", str(self.root)],
                              env=environment, cwd=self.root, capture_output=True, text=True, timeout=10)

    def test_environment_load_does_not_create_missing_hostfile(self):
        self.hostfile.unlink()
        result = self.source_real_entry()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.hostfile.exists())

    def test_source_preserves_configured_cluster_and_reports_size(self):
        contents = "127.0.0.1\nworker02\n"
        self.hostfile.write_text(contents)
        result = self.source_real_entry()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.hostfile.read_text(), contents)
        self.assertNotIn("HOSTFILE_CREATED", result.stdout)
        self.assertIn("ENV_READY", result.stdout)

    def test_source_accepts_dry_run_with_eight_devices_per_host(self):
        self.hostfile.write_text("127.0.0.1\n192.0.2.2\n", encoding="utf-8")
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_CHECKPOINT": workflow.DRY_RUN_CHECKPOINT}):
            result = self.source_real_entry()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("checkpoint=", result.stdout)
        self.assertIn("ENV_READY", result.stdout)

    def test_source_ignores_legacy_case_settings_in_environment_file(self):
        (self.root / "validation.local.sh").write_text(
            "TILEXR_VALIDATION_CHECKPOINT=obsolete\nTILEXR_VALIDATION_LOOPS=0\n")
        result = self.source_real_entry()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ENV_READY", result.stdout)

    def test_missing_custom_hostfile_does_not_fall_back_to_local(self):
        self.hostfile.unlink()
        result = self.source_real_entry("custom-hosts")
        self.assertEqual(result.returncode, 0)
        self.assertFalse(self.hostfile.exists())
        self.assertFalse((self.root / "custom-hosts").exists())

    def test_empty_existing_hostfile_is_not_overwritten(self):
        self.hostfile.write_text("")
        result = self.source_real_entry()
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.hostfile.read_text(), "")

    def test_real_environment_entry_preserves_root_with_spaces(self):
        repo = Path(workflow.__file__).resolve().parents[2]
        root = Path(self.temporary.name) / "workspace with spaces"
        for name in ("set_env.sh", "scripts/common_env.sh", "scripts/common_util.sh"):
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            # 与 PC 导出一致使用 LF；直接 source 真实环境入口及公共脚本。
            target.write_text((repo / name).read_text(encoding="utf-8"), encoding="utf-8")
        stub = root / "tools/validation/workflow.py"
        stub.parent.mkdir(parents=True)
        stub.write_text("import os, pathlib, sys\n"
                        "assert sys.argv[1:] == ['env']\n"
                        "root = pathlib.Path(os.environ['TILEXR_VALIDATION_ROOT'])\n"
                        "assert root == pathlib.Path(__file__).resolve().parents[2]\n"
                        "assert pathlib.Path(os.environ['TILEXR_VALIDATION_HOSTFILE']) == root / 'hostfile'\n"
                        "print('ENV_STUB_PASS')\n", encoding="utf-8")
        (root / "hostfile").write_text("127.0.0.1\n", encoding="utf-8")
        bundled_bash = Path(sys.executable).parent / "bash"
        bash = str(bundled_bash) if bundled_bash.exists() else shutil.which("bash")
        environment = dict(os.environ, TILEXR_VALIDATION_ROOT=str(root),
                           TILEXR_VALIDATION_HOSTFILE="hostfile", TILEXR_VALIDATION_BUILD_MODE="host",
                           TILEXR_VALIDATION_PYTHON=sys.executable,
                           PATH=str(Path(bash).parent) + os.pathsep + os.environ["PATH"])
        result = subprocess.run([bash, "-c", 'source "$1/set_env.sh" && '
                                 'printf "ROOT=%s\\nHOME=%s\\n" "$TILEXR_VALIDATION_ROOT" "$TILEXR_HOME"',
                                 "bash", str(root)], env=environment, cwd=self.root,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("ENV_STUB_PASS", result.stdout)
        reported = dict(line.split("=", 1) for line in result.stdout.splitlines()
                        if line.startswith(("ROOT=", "HOME=")))
        self.assertTrue(root.samefile(reported["ROOT"]))
        self.assertEqual(reported["ROOT"], reported["HOME"])
        self.assertEqual(Path(reported["ROOT"]).name, "workspace with spaces")
        self.assertTrue((root / "env/temp").is_dir())
        self.assertTrue((root / "run/plog").is_dir())


if __name__ == "__main__":
    unittest.main(verbosity=2)
