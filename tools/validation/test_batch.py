"""批测入口只负责调用核心脚本，不隐式编译或同步。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "需要 POSIX Python 和 Bash")
class BatchTests(unittest.TestCase):
    def test_batch_invokes_core_cases_without_build_or_sync(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = Path(__file__).resolve().parents[2]
            shutil.copy2(repo / "run_batch.sh", root / "run_batch.sh")
            (root / "run_case.sh").write_text(
                "#!/usr/bin/env bash\nset -e\nprintf '%s\n' \"$*\" >> calls\n",
                encoding="utf-8")
            (root / "run_case.sh").chmod(0o755)
            result = subprocess.run(["bash", "run_batch.sh"], cwd=root,
                                    capture_output=True, check=False)
            output = result.stdout.decode("utf-8", errors="replace")
            self.assertEqual(result.returncode, 0, output + result.stderr.decode("utf-8", errors="replace"))
            calls = (root / "calls").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(calls), 3)
            self.assertTrue(all("run_mode=performance" in call for call in calls))
            self.assertNotIn("build_all", "\n".join(calls))
            self.assertNotIn("sync", "\n".join(calls))


if __name__ == "__main__":
    unittest.main()
