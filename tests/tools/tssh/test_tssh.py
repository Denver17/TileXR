from __future__ import annotations

import io
import json
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.tssh import tssh


class ConfigTests(unittest.TestCase):
    def write_config(self, document: object) -> Path:
        temporary = tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", suffix=".json", delete=False
        )
        with temporary:
            json.dump(document, temporary)
        self.addCleanup(Path(temporary.name).unlink, missing_ok=True)
        return Path(temporary.name)

    def test_checked_in_inventory_preserves_all_five_clusters(self):
        clusters = tssh.load_clusters(tssh.DEFAULT_CONFIG)

        self.assertEqual(
            [cluster.name for cluster in clusters],
            ["c02-a04", "c02-a07", "c02-a09", "c01-b03", "c01-b08"],
        )
        self.assertEqual([len(cluster.hosts) for cluster in clusters], [8, 8, 8, 8, 8])
        self.assertEqual(clusters[1].hosts[0], tssh.Host("cpu1", "141.61.52.35"))
        self.assertEqual(clusters[1].hosts[-1], tssh.Host("cpu8", "141.61.52.87"))

    def test_lookup_is_case_insensitive(self):
        clusters = tssh.load_clusters(tssh.DEFAULT_CONFIG)

        self.assertEqual(tssh.find_cluster(clusters, "C02-A07").name, "c02-a07")

    def test_rejects_wrong_version_and_case_insensitive_duplicates(self):
        wrong_version = self.write_config({"version": 2, "clusters": {}})
        duplicate = self.write_config(
            {
                "version": 1,
                "clusters": {
                    "group": {"user": "root", "hosts": [{"name": "cpu1", "address": "one"}]},
                    "GROUP": {"user": "root", "hosts": [{"name": "cpu2", "address": "two"}]},
                },
            }
        )

        with self.assertRaisesRegex(tssh.ConfigError, "version must be 1"):
            tssh.load_clusters(wrong_version)
        with self.assertRaisesRegex(tssh.ConfigError, "duplicate cluster name"):
            tssh.load_clusters(duplicate)

    def test_rejects_duplicate_host_names(self):
        path = self.write_config(
            {
                "version": 1,
                "clusters": {
                    "group": {
                        "user": "root",
                        "hosts": [
                            {"name": "CPU1", "address": "one"},
                            {"name": "cpu1", "address": "two"},
                        ],
                    }
                },
            }
        )

        with self.assertRaisesRegex(tssh.ConfigError, "duplicate host name"):
            tssh.load_clusters(path)


class CommandTests(unittest.TestCase):
    def test_single_command_argument_is_kept_as_shell_source(self):
        self.assertEqual(tssh.remote_command(["--", "printf '%s\\n' hello"]), "printf '%s\\n' hello")

    def test_multiple_command_arguments_are_posix_shell_quoted(self):
        self.assertEqual(
            tssh.remote_command(["--", "bash", "-s", "--", "a b", "x'y"]),
            "bash -s -- 'a b' 'x'\"'\"'y'",
        )

    def test_jobs_accepts_one_through_sixty_four(self):
        self.assertEqual(tssh.DEFAULT_JOBS, 64)
        self.assertEqual(tssh.jobs_value("1"), 1)
        self.assertEqual(tssh.jobs_value("64"), 64)
        with self.assertRaises(Exception):
            tssh.jobs_value("0")
        with self.assertRaises(Exception):
            tssh.jobs_value("65")

    def test_ssh_command_contains_fixed_safety_options(self):
        cluster = tssh.Cluster("group", "root", (tssh.Host("cpu1", "host"),))

        command = tssh.ssh_command("ssh.exe", cluster, cluster.hosts[0], "hostname", 7)

        self.assertEqual(command[0:2], ["ssh.exe", "-T"])
        self.assertIn("-n", command)
        self.assertIn("LogLevel=ERROR", command)
        self.assertIn("BatchMode=yes", command)
        self.assertIn("StrictHostKeyChecking=accept-new", command)
        self.assertIn("UpdateHostKeys=no", command)
        self.assertIn("ConnectTimeout=7", command)
        self.assertEqual(command[-2:], ["root@host", "hostname"])


class KeySetupTests(unittest.TestCase):
    def setUp(self):
        self.cluster = tssh.Cluster(
            "group",
            "root",
            (
                tssh.Host("cpu1", "host1"),
                tssh.Host("cpu2", "host2"),
                tssh.Host("cpu3", "host3"),
            ),
        )

    def test_public_key_loader_accepts_supported_key_types(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "key.pub"
            key_types = (
                "ssh-ed25519",
                "ecdsa-sha2-nistp256",
                "sk-ssh-ed25519@openssh.com",
                "sk-ecdsa-sha2-nistp256@openssh.com",
            )
            for key_type in key_types:
                with self.subTest(key_type=key_type):
                    key = f"{key_type} AAAA comment"
                    path.write_text(f"{key}\n", encoding="utf-8")
                    self.assertEqual(tssh.load_public_key(path), key)

    def test_public_key_loader_rejects_unknown_type_and_multiple_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "key.pub"
            for key_type in ("unsupported-key", "sk-ssh-rsa@openssh.com"):
                with self.subTest(key_type=key_type):
                    path.write_text(f"{key_type} AAAA comment\n", encoding="utf-8")
                    with self.assertRaisesRegex(
                        tssh.ConfigError, "unsupported key type"
                    ):
                        tssh.load_public_key(path)

            path.write_text("ssh-ed25519 AAAA one\nssh-ed25519 BBBB two\n", encoding="utf-8")
            with self.assertRaisesRegex(tssh.ConfigError, "one OpenSSH public key"):
                tssh.load_public_key(path)

    def test_install_command_quotes_public_key_and_is_idempotent(self):
        key = "ssh-ed25519 AAAA user's-key"

        command = tssh.public_key_install_command(key)

        self.assertIn("grep -qxF", command)
        self.assertIn("already-present", command)
        self.assertIn("installed", command)
        self.assertIn("chmod 700", command)
        self.assertIn("chmod 600", command)
        self.assertIn("user'\"'\"'s-key", command)

    def test_known_host_preflight_accepts_key_auth_or_password_rejection(self):
        def fake_run(command, **kwargs):
            target = command[-2]
            if target == "root@host1":
                return subprocess.CompletedProcess(command, 0, b"", b"")
            if target == "root@host2":
                return subprocess.CompletedProcess(command, 255, b"", b"Permission denied (publickey,password).")
            return subprocess.CompletedProcess(command, 255, b"", b"REMOTE HOST IDENTIFICATION HAS CHANGED")

        failures = tssh.prepare_known_hosts(
            "ssh.exe", self.cluster, 5, 10, run_process=fake_run
        )

        self.assertEqual(list(failures), ["host3"])
        self.assertIn(b"HOST IDENTIFICATION", failures["host3"].stderr)

    def test_install_key_host_uses_password_only_for_authentication(self):
        captured = {}

        class Stream:
            def __init__(self, data, status=None):
                self.data = data
                if status is not None:
                    self.channel = mock.Mock()
                    self.channel.recv_exit_status.return_value = status

            def read(self):
                return self.data

        class Client:
            def load_host_keys(self, path):
                captured["known_hosts"] = path

            def set_missing_host_key_policy(self, policy):
                captured["policy"] = policy

            def connect(self, **kwargs):
                captured["connect"] = kwargs

            def exec_command(self, command, timeout):
                captured["remote_command"] = command
                captured["timeout"] = timeout
                return Stream(b""), Stream(b"installed\n", 0), Stream(b"")

            def close(self):
                captured["closed"] = True

        class Paramiko:
            SSHClient = Client

            class RejectPolicy:
                pass

        result = tssh.install_key_host(
            Paramiko,
            Path("known_hosts"),
            self.cluster,
            self.cluster.hosts[0],
            "ssh-ed25519 AAAA test",
            "secret-value",
            6,
            9,
        )

        self.assertTrue(result.succeeded)
        self.assertEqual(result.stdout, b"installed\n")
        self.assertEqual(captured["connect"]["password"], "secret-value")
        self.assertFalse(captured["connect"]["look_for_keys"])
        self.assertFalse(captured["connect"]["allow_agent"])
        self.assertNotIn("secret-value", captured["remote_command"])
        self.assertTrue(captured["closed"])


class ExecutionTests(unittest.TestCase):
    def setUp(self):
        self.cluster = tssh.Cluster(
            "group",
            "root",
            (
                tssh.Host("cpu1", "host1"),
                tssh.Host("cpu2", "host2"),
                tssh.Host("cpu3", "host3"),
            ),
        )

    def test_run_host_broadcasts_exact_input_and_captures_both_streams(self):
        seen = {}

        def fake_run(command, **kwargs):
            seen["command"] = command
            seen.update(kwargs)
            return subprocess.CompletedProcess(command, 3, b"out\x00", b"err\n")

        result = tssh.run_host(
            "ssh.exe",
            self.cluster,
            self.cluster.hosts[0],
            "bash -s",
            b"set -e\r\n\x00",
            10,
            5,
            run_process=fake_run,
        )

        self.assertEqual(seen["input"], b"set -e\r\n\x00")
        self.assertNotIn("-n", seen["command"])
        self.assertEqual(seen["timeout"], 5)
        self.assertFalse(seen["check"])
        self.assertEqual((result.returncode, result.stdout, result.stderr), (3, b"out\x00", b"err\n"))
        self.assertFalse(result.succeeded)

    def test_run_host_closes_stdin_without_send_input(self):
        seen = {}

        def fake_run(command, **kwargs):
            seen["command"] = command
            seen.update(kwargs)
            return subprocess.CompletedProcess(command, 0, b"", b"")

        result = tssh.run_host(
            "ssh.exe", self.cluster, self.cluster.hosts[0], "true", None, 10, None, run_process=fake_run
        )

        self.assertNotIn("input", seen)
        self.assertEqual(seen["stdin"], subprocess.DEVNULL)
        self.assertIn("-n", seen["command"])
        self.assertTrue(result.succeeded)

    def test_timeout_and_launch_error_are_retained(self):
        def timeout_run(*args, **kwargs):
            raise subprocess.TimeoutExpired("ssh", 1, output=b"partial", stderr=b"late")

        def error_run(*args, **kwargs):
            raise FileNotFoundError("missing ssh")

        timed_out = tssh.run_host(
            "ssh.exe", self.cluster, self.cluster.hosts[0], "sleep 5", None, 10, 1, run_process=timeout_run
        )
        launch_error = tssh.run_host(
            "missing.exe", self.cluster, self.cluster.hosts[0], "true", None, 10, None, run_process=error_run
        )

        self.assertTrue(timed_out.timed_out)
        self.assertEqual((timed_out.stdout, timed_out.stderr), (b"partial", b"late"))
        self.assertIn("missing ssh", launch_error.launch_error or "")

    def test_cluster_limits_concurrency_and_reports_in_config_order(self):
        lock = threading.Lock()
        active = 0
        peak = 0

        def fake_run(command, **kwargs):
            nonlocal active, peak
            with lock:
                active += 1
                peak = max(peak, active)
            try:
                time.sleep({"root@host1": 0.04, "root@host2": 0.02, "root@host3": 0.01}[command[-2]])
                return subprocess.CompletedProcess(command, 0, command[-2].encode(), b"")
            finally:
                with lock:
                    active -= 1

        results = tssh.run_cluster(
            "ssh.exe", self.cluster, "hostname", b"same", 2, 10, None, run_process=fake_run
        )

        self.assertEqual(peak, 2)
        self.assertEqual([result.host.name for result in results], ["cpu1", "cpu2", "cpu3"])
        self.assertEqual([result.stdout for result in results], [b"root@host1", b"root@host2", b"root@host3"])

    def test_report_is_labeled_and_aggregate_status_is_complete(self):
        output = io.StringIO()
        results = [
            tssh.HostResult(self.cluster.hosts[0], 0, b"ok\n", b"", 0.1),
            tssh.HostResult(self.cluster.hosts[1], 4, b"", b"bad\n", 0.2),
            tssh.HostResult(self.cluster.hosts[2], None, b"", b"", 0.3, launch_error="no ssh"),
        ]

        tssh.print_report(results, output)

        report = output.getvalue()
        self.assertIn("[cpu1 host1 exit=0 0.10s]\nok", report)
        self.assertIn("[cpu2 host2 exit=4 0.20s]\n[stderr]\nbad", report)
        self.assertIn("[cpu3 host3 launch-error 0.30s]\n[error] no ssh", report)
        self.assertTrue(report.endswith("[summary] ok=1 failed=2\n"))


class MainTests(unittest.TestCase):
    def test_main_returns_nonzero_when_any_host_fails(self):
        cluster = tssh.Cluster("group", "root", (tssh.Host("cpu1", "host1"),))
        result = tssh.HostResult(cluster.hosts[0], 9, b"", b"failed", 0.1)

        with (
            mock.patch.object(tssh, "load_clusters", return_value=(cluster,)),
            mock.patch.object(tssh, "run_cluster", return_value=[result]) as run_cluster,
            mock.patch.object(tssh, "print_report"),
        ):
            status = tssh.main(["-C", "GROUP", "--", "bash", "-s", "--", "a b"])

        self.assertEqual(status, 1)
        self.assertEqual(run_cluster.call_args.args[2], "bash -s -- 'a b'")

    def test_normal_command_does_not_import_paramiko(self):
        cluster = tssh.Cluster("group", "root", (tssh.Host("cpu1", "host1"),))
        result = tssh.HostResult(cluster.hosts[0], 0, b"", b"", 0.1)

        with (
            mock.patch.object(tssh, "load_clusters", return_value=(cluster,)),
            mock.patch.object(tssh, "run_cluster", return_value=[result]),
            mock.patch.object(tssh, "print_report"),
            mock.patch.object(tssh.importlib, "import_module", side_effect=AssertionError("must not import")),
        ):
            status = tssh.main(["-C", "group", "--", "true"])

        self.assertEqual(status, 0)

    def test_install_key_mode_prompts_and_uses_optional_paramiko(self):
        cluster = tssh.Cluster("group", "root", (tssh.Host("cpu1", "host1"),))
        result = tssh.HostResult(cluster.hosts[0], 0, b"installed\n", b"", 0.1)
        paramiko = object()

        with (
            mock.patch.object(tssh, "load_clusters", return_value=(cluster,)),
            mock.patch.object(tssh, "load_public_key", return_value="ssh-ed25519 AAAA test"),
            mock.patch.object(tssh.importlib, "import_module", return_value=paramiko) as importer,
            mock.patch.object(tssh.getpass, "getpass", return_value="test-password") as prompt,
            mock.patch.object(tssh, "install_key_cluster", return_value=[result]) as install,
            mock.patch.object(tssh, "print_report"),
        ):
            status = tssh.main(["-C", "group", "--install-key"])

        self.assertEqual(status, 0)
        importer.assert_called_once_with("paramiko")
        prompt.assert_called_once_with("Password for root@group: ")
        self.assertIs(install.call_args.args[0], paramiko)
        self.assertEqual(install.call_args.args[5], "test-password")


class LauncherTests(unittest.TestCase):
    def test_windows_launcher_uses_default_python(self):
        launcher = (ROOT / "tools" / "tssh" / "tssh.cmd").read_text(
            encoding="utf-8"
        )

        self.assertIn('python "%~dp0tssh.py" %*', launcher)
        self.assertNotIn("py -", launcher)


if __name__ == "__main__":
    unittest.main(verbosity=2)
