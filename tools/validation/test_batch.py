"""批测入口只负责调用核心脚本，不隐式编译或同步。"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "需要 POSIX Python 和 Bash")
class BatchTests(unittest.TestCase):
    def test_batch_failure_stops_without_success_conclusion(self):
        repo = Path(__file__).resolve().parents[2]
        bash = Path(sys.executable).parent / "bash"
        if not bash.exists():
            bash = Path(shutil.which("bash"))
        for entry, failure, expected in (
            ("run_batch.sh", "run", 1),
            ("run_batch.sh", "none", 3),
        ):
            with self.subTest(entry=entry, failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / entry).write_text((repo / entry).read_text())
                for step in ("build_all", "sync", "run"):
                    (root / (step + ".sh")).write_text(
                        f"echo {step} >> calls\n"
                        + ("echo ERROR injected >&2\nexit 19\n" if step == failure else "exit 0\n"))
                result = subprocess.run([str(bash), entry], cwd=root, text=True, capture_output=True,
                    env=dict(os.environ, PATH=str(bash.parent) + os.pathsep + os.environ["PATH"]))
                self.assertEqual(len((root / "calls").read_text().splitlines()), expected)
                if failure == "none":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("Batch PASS cases=3", result.stdout)
                else:
                    self.assertEqual(result.returncode, 19, result.stderr)
                    self.assertNotIn("PASS", result.stdout)
                    self.assertIn("Failed", result.stdout)


    def test_batch_invokes_core_cases_without_build_or_sync(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = Path(__file__).resolve().parents[2]
            shutil.copy2(repo / "run_batch.sh", root / "run_batch.sh")
            (root / "run.sh").write_text(
                "#!/usr/bin/env bash\nset -e\nprintf '%s\n' \"$*\" >> calls\n",
                encoding="utf-8")
            (root / "run.sh").chmod(0o755)
            bash = Path(sys.executable).parent / "bash"
            if not bash.exists():
                bash = Path(shutil.which("bash"))
            result = subprocess.run([str(bash), "run_batch.sh"], cwd=root,
                                    env=dict(os.environ, PATH=str(bash.parent) + os.pathsep + os.environ["PATH"]),
                                    capture_output=True, check=False)
            output = result.stdout.decode("utf-8", errors="replace")
            self.assertEqual(result.returncode, 0, output + result.stderr.decode("utf-8", errors="replace"))
            calls = (root / "calls").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(calls), 3)
            self.assertTrue(all("-n 2 -e 96 -m 1 -l 50 50" in call for call in calls))
            self.assertNotIn("build_all", "\n".join(calls))
            self.assertNotIn("sync", "\n".join(calls))


if __name__ == "__main__":
    unittest.main()
