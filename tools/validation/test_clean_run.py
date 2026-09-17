"""Exercise cleanup only in temporary deployments; no SSH or NPU required."""

import contextlib
import io
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import clean_run


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tilexr clean ")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.deployment("test's workspace $(unused)")

    def deployment(self, name):
        root = self.base / name
        (root / "run/case/profile").mkdir(parents=True)
        (root / ".tilexr-validation-root").write_text("TileXR validation deployment\n")
        (root / "run/case/profile/trace.json").write_text("trace")
        (root / "run/.hidden").write_text("log")
        (root / "hostfile").write_text("127.0.0.1\n")
        (root / "install").mkdir()
        (root / "install/library.so").write_text("keep")
        return root

    def test_removes_all_contents_preserving_root_and_other_files(self):
        result = clean_run.clean_node(self.root)
        self.assertIn("CLEANED", result)
        self.assertEqual(list((self.root / "run").iterdir()), [])
        self.assertEqual((self.root / "install/library.so").read_text(), "keep")
        self.assertEqual((self.root / "hostfile").read_text(), "127.0.0.1\n")
        self.assertIn("entries=0", clean_run.clean_node(self.root))

    def test_dry_run_and_absent_run_do_not_create_or_remove_files(self):
        self.assertIn("DRY_RUN", clean_run.clean_node(self.root, True))
        self.assertTrue((self.root / "run/case/profile/trace.json").exists())
        shutil.rmtree(self.root / "run")
        self.assertIn("absent", clean_run.clean_node(self.root))
        self.assertFalse((self.root / "run").exists())

    def test_child_links_are_unlinked_without_following_them(self):
        outside = self.base / "outside"
        outside.mkdir()
        (outside / "keep").write_text("keep")
        (self.root / "run/external").symlink_to(outside, target_is_directory=True)
        (self.root / "run/dangling").symlink_to(self.base / "absent")
        clean_run.clean_node(self.root)
        self.assertEqual((outside / "keep").read_text(), "keep")
        self.assertEqual(list((self.root / "run").iterdir()), [])

    def test_rejects_symlinked_run_or_deployment(self):
        original = self.root / "run"
        original.rename(self.root / "saved-run")
        original.symlink_to(self.root / "saved-run", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlinked run"):
            clean_run.clean_node(self.root)
        self.assertTrue((self.root / "saved-run/case/profile/trace.json").exists())
        alias = self.base / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            clean_run.clean_node(alias)

    def test_rejects_unmanaged_directory_and_accepts_checkout(self):
        (self.root / ".tilexr-validation-root").unlink()
        with self.assertRaisesRegex(ValueError, "managed"):
            clean_run.clean_node(self.root)
        self.assertTrue((self.root / "run/.hidden").exists())
        (self.root / ".git").mkdir()
        (self.root / "run.sh").write_text("#!/bin/bash\n")
        clean_run.clean_node(self.root)
        self.assertTrue((self.root / ".git").is_dir())

    def test_nested_task_locks_prevent_deletion(self):
        for name in ("node.lock", ".operation.lock"):
            lock = self.root / "run/case" / name
            lock.mkdir()
            with self.assertRaisesRegex(ValueError, "lock"):
                clean_run.clean_node(self.root)
            self.assertTrue((self.root / "run/.hidden").exists())
            lock.rmdir()

    def test_full_hostfile_cleanup_streams_worker_without_remote_install(self):
        remote = self.deployment("remote")
        hosts = [{"host": "127.0.0.1"}, {"host": "192.0.2.2"}]
        real_run = subprocess.run

        def ssh(argv, **kwargs):
            self.assertEqual(argv[0], "ssh")
            self.assertEqual(argv[-2], "tester@192.0.2.2")
            worker_args = shlex.split(argv[-1])
            self.assertEqual(worker_args, ["python3", "-", "--node", str(self.root)])
            worker_args[0] = sys.executable
            worker_args[3] = str(remote)
            return real_run(worker_args, **kwargs)

        with mock.patch.object(clean_run.subprocess, "run", side_effect=ssh), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(clean_run.clean_hosts(self.root, hosts, "tester"), 0)
        for root in (self.root, remote):
            self.assertEqual(list((root / "run").iterdir()), [])
            self.assertTrue((root / "install/library.so").exists())

    def test_remote_failure_is_reported_and_other_nodes_are_still_checked(self):
        hosts = [{"host": "127.0.0.1"}, {"host": "192.0.2.2"}, {"host": "192.0.2.3"}]
        error = io.StringIO()
        with mock.patch.object(clean_run.subprocess, "run", side_effect=[
                subprocess.CompletedProcess([], 255, "", "connection refused"),
                subprocess.CompletedProcess([], 0, "DRY_RUN path=test/run entries=3", "")]) as ssh, \
                contextlib.redirect_stderr(error), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(clean_run.clean_hosts(self.root, hosts, "", True), 1)
        self.assertEqual(ssh.call_count, 2)
        self.assertIn("CLEAN_FAIL", error.getvalue())
        self.assertIn("connection refused", error.getvalue())
        self.assertTrue((self.root / "run/.hidden").exists())

    def test_wrong_coordinator_prevents_all_cleanup(self):
        with mock.patch("workflow.check_coordinator", side_effect=ValueError("wrong coordinator")), \
                mock.patch.object(clean_run, "clean_node") as clean:
            with self.assertRaisesRegex(ValueError, "wrong coordinator"):
                clean_run.clean_hosts(self.root, [{"host": "192.0.2.2"}], "")
            clean.assert_not_called()
        self.assertTrue((self.root / "run/.hidden").exists())

    def test_public_script_uses_config_without_loading_cann(self):
        repo = Path(__file__).resolve().parents[2]
        for name in ("clean_run.sh", "tools/validation/clean_run.py", "tools/validation/workflow.py",
                     "tools/validation/source.py", "tools/validation/measurements.py", "tools/validation/run_summary.py",
                     "tools/ep/msprof_kernel_stats.py"):
            dest = self.root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(repo / name, dest)
        shutil.copytree(repo / "tools/profiling", self.root / "tools/profiling",
                        ignore=shutil.ignore_patterns("__pycache__"))
        (self.root / "custom.hosts").write_text("127.0.0.1\n")
        (self.root / "validation.local.sh").write_text(
            "TILEXR_VALIDATION_HOSTFILE=custom.hosts\n"
            "TILEXR_CANN_ENV_SCRIPT=/nonexistent/cann/set_env.sh\n"
            f"TILEXR_VALIDATION_PYTHON={shlex.quote(sys.executable)}\n")
        env = {key: value for key, value in os.environ.items() if not key.startswith("TILEXR_")}
        script = self.root / "clean_run.sh"
        preview = subprocess.run(["bash", str(script), "--dry-run"], env=env,
                                 capture_output=True, text=True, timeout=20, cwd=self.base)
        self.assertEqual(preview.returncode, 0, preview.stdout + preview.stderr)
        self.assertTrue((self.root / "run/.hidden").exists())
        rejected = subprocess.run(["bash", str(script), "hostfile=missing"], env=env,
                                  capture_output=True, text=True, timeout=20, cwd=self.base)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertTrue((self.root / "run/.hidden").exists())
        deleted = subprocess.run(["bash", str(script)], env=env,
                                 capture_output=True, text=True, timeout=20, cwd=self.base)
        self.assertEqual(deleted.returncode, 0, deleted.stdout + deleted.stderr)
        self.assertEqual(list((self.root / "run").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
