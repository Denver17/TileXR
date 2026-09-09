#!/usr/bin/env python3
"""无 NPU/真实 SSH 的七步验证行为回归：python3 tools/validation/test_workflow.py。"""

import errno
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
        self.hostfile.write_text("localhost devices=0,2\n", encoding="utf-8")
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

    def test_comments_bom_explicit_devices_and_default(self):
        self.assertEqual(self.parse("\ufeff# group\nnode-a devices=2,7 # physical IDs\n\nnode_b\n"),
                         [{"host": "node-a", "devices": [2, 7]},
                          {"host": "node_b", "devices": list(range(8))}])

    def test_rejects_bad_inventory(self):
        for text in ("", "# only comments\n", "node-a\nnode-a devices=1\n",
                     "node-a devices=1,1\n", "node-a devices=256\n",
                     "node-a devices=-1\n", "node-a devices=\n",
                     "node-a devices=0 extra\n", "user@node-a\n",
                     "-oProxyCommand=x\n", "node-a;command\n",
                     "\n".join("node-" + str(index) for index in range(65))):
            with self.subTest(text=text[:80]), self.assertRaises(ValueError):
                self.parse(text)

    def test_accepts_64_nodes_and_device_boundary(self):
        text = "\n".join(f"node-{index} devices=0,255" for index in range(64))
        self.assertEqual(len(self.parse(text)), 64)


class ConfigTests(TemporaryRoot):
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

    def test_signature_tracks_distribution_and_runtime_configuration(self):
        baseline = self.config().signature()
        self.assertEqual(baseline, self.config().signature())
        changes = [{"TILEXR_VALIDATION_TIMEOUT": "60"},
                   {"TILEXR_VALIDATION_SSH_USER": "test-user"},
                   {"TILEXR_VALIDATION_BUILD_MODE": "production"}]
        for change in changes:
            with self.subTest(change=change), mock.patch.dict(os.environ, change):
                self.assertNotEqual(baseline, self.config().signature())
        local = self.root / "validation.local.sh"
        local.write_text("export CANN_TEST_PATH=/one\n", encoding="utf-8")
        configured = self.config().signature()
        self.assertNotEqual(baseline, configured)
        local.write_text("export CANN_TEST_PATH=/two\n", encoding="utf-8")
        self.assertNotEqual(configured, self.config().signature())
        local.unlink()
        self.hostfile.write_text("localhost devices=1\n", encoding="utf-8")
        self.assertNotEqual(baseline, self.config().signature())

    def test_rejects_unsupported_or_unbounded_configuration(self):
        for key, value in (("BUILD_MODE", "npu"), ("CHECKPOINT", "device-dedup"),
                           ("JOBS", "0"), ("JOBS", "257"), ("TIMEOUT", "29"),
                           ("TIMEOUT", "181"), ("SSH_USER", "user;command")):
            with self.subTest(key=key, value=value), mock.patch.dict(
                    os.environ, {"TILEXR_VALIDATION_" + key: value}), self.assertRaises(ValueError):
                self.config()

    def test_hostfile_must_be_inside_synchronized_root(self):
        outside = Path(self.temporary.name) / "hostfile"
        outside.write_text("localhost\n", encoding="utf-8")
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
            with self.subTest(change=change), self.assertRaises(ValueError):
                workflow.validate_result(dict(self.result, **change), "run-current", self.manifest)

    def test_rejects_absent_protocol_fields(self):
        for field in self.result:
            result = dict(self.result)
            del result[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                workflow.validate_result(result, "run-current", self.manifest)


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


@unittest.skipUnless(os.name == "posix", "生产进程组契约需要 POSIX Python")
class ProcessTests(TemporaryRoot):
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
        with self.assertRaises(subprocess.TimeoutExpired):
            workflow.execute([sys.executable, "-c", parent], self.root / "timeout.log", timeout=1.5)
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


FAKE_TRANSPORT = r'''import os, pathlib, shlex, shutil, subprocess, sys

kind = pathlib.Path(sys.argv[0]).name
root = pathlib.Path(os.environ["FAKE_ROOT"])
remote = pathlib.Path(os.environ["FAKE_REMOTE"])
scenario = os.environ.get("FAKE_SCENARIO", "")
args = sys.argv[1:]

def mapped(value):
    return remote / pathlib.Path(value).relative_to(root)

if kind == "ssh":
    command = shlex.split(args[-1])
    payload = sys.stdin.read()
    if command == ["hostname"]:
        print("node-b")
        sys.exit(0)
    command[0] = os.environ["FAKE_BASH"]
    command[3] = str(mapped(command[3]))
    action = command[4] if len(command) > 4 else "prepare"
    if scenario == "verify-fail" and action == "verify":
        print("injected remote verification failure", file=sys.stderr)
        sys.exit(23)
    environment = dict(os.environ, TILEXR_VALIDATION_ROOT=str(remote),
                       TILEXR_VALIDATION_HOSTFILE=str(remote / "hostfile"))
    if action == "node" and scenario == "node-fail":
        environment["TEST_PROGRAM_BEHAVIOR"] = "missing-marker"
    if action == "node" and scenario == "stale-result":
        command[5] = "old-run"
    sys.exit(subprocess.run(command, input=payload, text=True, env=environment).returncode)
elif kind == "rsync":
    src, dest = args[-2:]
    if ":" in src:
        if scenario == "collect-fail":
            print("injected log collection failure", file=sys.stderr)
            sys.exit(24)
        src = mapped(src.split(":", 1)[1])
        dest = pathlib.Path(dest)
        if scenario == "stale-result":
            src = src.parent / "old-run"
        shutil.copytree(src, dest, dirs_exist_ok=True)
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
        self.manifest["source"] = workflow.snapshot(self.root)
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
        self.assertEqual(summary["source_sha256"], self.manifest["source"]["content_sha256"])
        self.assertEqual(len(summary["nodes"]), 2)

    def setUp(self):
        super().setUp()
        self.remote = Path(self.temporary.name) / "node-b"
        self.hostfile.write_text("localhost devices=0\nnode-b devices=2,3\n", encoding="utf-8")
        tool_root = self.root / "tools/validation"
        tool_root.mkdir(parents=True)
        for name in ("workflow.py", "source.py"):
            shutil.copy2(Path(workflow.__file__).with_name(name), tool_root / name)
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
            "if behavior != 'missing-marker':\n"
            "    print('EP_SERVER_DEDUP_ABI_HOST_PASS test=' + sys.argv[1] +\n"
            "          (' unexpected-suffix' if behavior == 'decorated-marker' else ''))\n", encoding="utf-8")
        source = {"commit": "a" * 40, "files": {str(path.relative_to(self.root)): workflow.digest(path)
                  for path in (self.script, tool_root / "source.py", self.root / "set_env.sh", helper)}}
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
            self.manifest["source"]["files"][program.name] = workflow.digest(program)
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
        self.assertEqual({node["host"] for node in summary["nodes"]}, {"localhost", "node-b"})
        self.assertTrue(all(node["tests"] == len(self.manifest["tests"]) for node in summary["nodes"]))
        self.assertEqual(summary["errors"], [])
        self.assertEqual(summary["commit"], self.manifest["source"]["commit"])
        self.assertEqual(list(self.root.rglob("pid.json")), [])
        self.assertEqual(list(self.remote.rglob("pid.json")), [])

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

    def test_run_requires_sync_receipt(self):
        result = self.invoke("run")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("VALIDATION_PASS", result.stdout)

    def test_configuration_change_requires_resync(self):
        self.prepare_sync()
        (self.root / "validation.local.sh").write_text("# changed local config\n", encoding="utf-8")
        result = self.invoke("run")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sync.sh", result.stderr)

    def test_modified_installed_artifact_blocks_launch(self):
        self.prepare_sync()
        (self.config_instance.install / "artifact").write_bytes(b"different build\n")
        result = self.invoke("run")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("VALIDATION_PASS", result.stdout)

    def test_modified_source_blocks_launch(self):
        self.prepare_sync()
        (self.root / "test_protocol.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
        result = self.invoke("run")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source mismatch", result.stderr)

    def test_remote_zero_exit_without_marker_fails_whole_run(self):
        self.prepare_sync()
        result = self.invoke("run", environment={"FAKE_SCENARIO": "node-fail"})
        self.assertNotEqual(result.returncode, 0)
        summary = self.summary()
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(any(error.get("host") == "node-b" for error in summary["errors"]))
        self.assertIn("PASS marker", result.stdout + result.stderr)

    def test_log_collection_failure_fails_whole_run(self):
        self.prepare_sync()
        result = self.invoke("run", environment={"FAKE_SCENARIO": "collect-fail"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.summary()["status"], "FAIL")
        self.assertTrue(any(error["stage"] == "collect/result" for error in self.summary()["errors"]))

    def test_stale_remote_success_is_rejected(self):
        self.prepare_sync()
        result = self.invoke("run", environment={"FAKE_SCENARIO": "stale-result"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.summary()["status"], "FAIL")

    def test_node_failure_still_records_result_and_cleanup(self):
        for behavior in ("nonzero", "missing-marker", "decorated-marker"):
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
                self.assertEqual(recorded["tests"], [])

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

    def test_node_preflight_obeys_passed_execution_budget(self):
        self.manifest["mode"] = "production"
        workflow.write_json(self.config_instance.install / "manifest.json", self.manifest)
        started_file = self.root / "preflight-started"
        for name in ("nm", "readelf", "ldd"):
            tool = self.fake_bin / name
            tool.write_text("#!" + sys.executable + "\nimport pathlib, time\n"
                            f"pathlib.Path({str(started_file)!r}).touch()\ntime.sleep(60)\n", encoding="utf-8")
            tool.chmod(0o755)
        started = time.monotonic()
        result = self.invoke("node", "preflight-deadline", self.manifest["build_id"], "1",
                             environment={"TILEXR_VALIDATION_BUILD_MODE": "production"})
        self.assertLess(time.monotonic() - started, 6)
        self.assertTrue(started_file.exists(), "须实际启动模拟生产检查工具")
        self.assertNotEqual(result.returncode, 0)
        recorded = workflow.read_json(self.config_instance.logs / "preflight-deadline/result.json")
        self.assertEqual(recorded["status"], "FAIL")
        self.assertEqual(recorded["tests"], [])
        self.assertIn("timed out", recorded["error"])

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
                     "tools/validation/workflow.py", "tools/validation/source.py"):
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

    def test_missing_default_hostfile_is_reported_without_creating_it(self):
        self.hostfile.unlink()
        result = self.source_real_entry()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.hostfile.exists())
        self.assertIn("hostfile 不存在", result.stderr)
        self.assertIn("hostfile.example", result.stderr)

    def test_source_preserves_configured_cluster_and_reports_size(self):
        contents = "localhost devices=0,2\nworker02 devices=1,3\n"
        self.hostfile.write_text(contents)
        result = self.source_real_entry()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.hostfile.read_text(), contents)
        self.assertNotIn("HOSTFILE_CREATED", result.stdout)
        self.assertIn("hosts=2", result.stdout)

    def test_missing_custom_hostfile_does_not_fall_back_to_local(self):
        self.hostfile.unlink()
        result = self.source_real_entry("custom-hosts")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("custom-hosts", result.stderr)
        self.assertIn("TILEXR_VALIDATION_HOSTFILE", result.stderr)
        self.assertFalse(self.hostfile.exists())
        self.assertFalse((self.root / "custom-hosts").exists())

    def test_empty_existing_hostfile_is_not_overwritten(self):
        self.hostfile.write_text("")
        result = self.source_real_entry()
        self.assertNotEqual(result.returncode, 0)
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
        (root / "hostfile").write_text("localhost\n", encoding="utf-8")
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
        self.assertIn("ENV_STUB_PASS", result.stdout.splitlines())
        reported = dict(line.split("=", 1) for line in result.stdout.splitlines()
                        if line.startswith(("ROOT=", "HOME=")))
        self.assertTrue(root.samefile(reported["ROOT"]))
        self.assertEqual(reported["ROOT"], reported["HOME"])
        self.assertEqual(Path(reported["ROOT"]).name, "workspace with spaces")
        self.assertTrue((root / "env/temp").is_dir())
        self.assertTrue((root / "run/plog").is_dir())


if __name__ == "__main__":
    unittest.main(verbosity=2)
