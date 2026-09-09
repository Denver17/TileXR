"""用入口替身验证当前批次脚本的配置传递、失败退出及恢复；不运行 NPU。"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "需要 POSIX Python 和 Bash")
class BatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tilexr batch ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        repo = Path(__file__).resolve().parents[2]
        shutil.copy2(repo / "run_batch.sh", self.root / "run_batch.sh")
        self.config = self.root / "validation.local.sh"
        self.original = (b"# caller configuration\nTILEXR_COMM_ID=192.0.2.1:10177\n"
                         b"TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-p1-abi-host\n"
                         b"TILEXR_VALIDATION_RUN_MODE=breakdown\nTILEXR_VALIDATION_LOOPS=1\n"
                         b"TILEXR_VALIDATION_WARMUP=7")
        self.config.write_bytes(self.original)
        self.events = self.root / "events"
        for stage in ("build_all", "sync", "run"):
            (self.root / (stage + ".sh")).write_text(
                "set -e\nsource validation.local.sh\n"
                f"printf '%s\\n' '{stage}'\" $TILEXR_VALIDATION_BUILD_MODE "
                "$TILEXR_VALIDATION_CHECKPOINT $TILEXR_VALIDATION_RUN_MODE "
                "$TILEXR_VALIDATION_LOOPS $TILEXR_VALIDATION_WARMUP $TILEXR_COMM_ID\" >> events\n"
                f'if [[ "${{TEST_FAIL_STAGE:-}}" == {stage} && '
                '"${TEST_FAIL_MODE:-}" == "$TILEXR_VALIDATION_RUN_MODE" ]]; then\n'
                "    echo injected-failure; exit 19\nfi\necho fixture-pass\n", encoding="utf-8")
        bundled_bash = Path(sys.executable).parent / "bash"
        self.bash = str(bundled_bash) if bundled_bash.exists() else shutil.which("bash")

    def run_batch(self, **environment):
        env = dict(os.environ, **environment)
        env["PATH"] = str(Path(self.bash).parent) + os.pathsep + env["PATH"]
        return subprocess.run([self.bash, str(self.root / "run_batch.sh")], cwd=self.root.parent,
                              env=env, text=True, capture_output=True, timeout=15)

    def event(self, stage, mode):
        return (f"{stage} production ep-sdedup-p1-chain-multi {mode} 50 "
                f"{0 if mode == 'functional' else 50} 192.0.2.1:10177")

    def test_runs_both_modes_with_one_build_and_restores_configuration(self):
        result = self.run_batch()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.events.read_text().splitlines(), [
            self.event("build_all", "functional"), self.event("sync", "functional"),
            self.event("run", "functional"), self.event("sync", "performance"),
            self.event("run", "performance")])
        self.assertIn("BATCH_PASS cases=2", result.stdout)
        self.assertEqual(self.config.read_bytes(), self.original)
        batch = next((self.root / "run/validation").glob("batch-*"))
        self.assertTrue((batch / "functional.log").is_file())
        self.assertTrue((batch / "performance.log").is_file())
        self.assertFalse((batch / "validation.local.sh").exists())

    def test_failed_step_stops_batch_and_preserves_failure_status(self):
        for stage, mode, count in (("build_all", "functional", 1), ("sync", "functional", 2),
                                    ("run", "functional", 3), ("run", "performance", 5)):
            with self.subTest(stage=stage, mode=mode):
                self.events.unlink(missing_ok=True)
                result = self.run_batch(TEST_FAIL_STAGE=stage, TEST_FAIL_MODE=mode)
                self.assertEqual(result.returncode, 19, result.stdout + result.stderr)
                self.assertEqual(len(self.events.read_text().splitlines()), count)
                self.assertNotIn("BATCH_PASS", result.stdout)
                self.assertIn("injected-failure", result.stdout)
                self.assertEqual(self.config.read_bytes(), self.original)

    def test_missing_configuration_is_not_created(self):
        self.config.unlink()
        result = self.run_batch()
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.config.exists())
        self.assertFalse(self.events.exists())


if __name__ == "__main__":
    unittest.main()
