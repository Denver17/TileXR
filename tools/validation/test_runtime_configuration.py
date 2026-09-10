"""Focused script tests for immutable deployment config and per-case arguments."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import workflow


class RuntimeConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tilexr case config ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        repo = Path(__file__).resolve().parents[2]
        for name in ("set_env.sh", "run_case.sh", "run.sh", "tools/validation/measurements.py",
                     "tools/ep/msprof_kernel_stats.py"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((repo / name).read_text(), encoding="utf-8")
        (self.root / "scripts").mkdir()
        (self.root / "scripts/common_env.sh").write_text("# environment fixture\n")
        self.local = self.root / "validation.local.sh"
        self.local.write_text('TILEXR_COMM_ID=192.0.2.1:10177\n'
                              'TILEXR_VALIDATION_RUN_MODE=obsolete\n'
                              'TILEXR_VALIDATION_LOOPS=999\n'
                              'TILEXR_VALIDATION_CHECKPOINT=obsolete\n')
        self.bash = Path(sys.executable).parent / "bash"
        if not self.bash.exists():
            self.bash = Path(shutil.which("bash") or "/missing")
        if os.name != "posix" or not self.bash.exists():
            self.skipTest("POSIX Bash required")
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("TILEXR_", "ASCEND_"))}
        self.env.update(TILEXR_VALIDATION_BUILD_MODE="host", TILEXR_VALIDATION_PYTHON=sys.executable,
                        PATH=str(self.bash.parent) + os.pathsep + os.environ["PATH"])

    def test_environment_preserves_script_values_and_ignores_legacy_case_fields(self):
        command = 'source ./set_env.sh >/dev/null; "$TILEXR_VALIDATION_PYTHON" -c '
        command += "'import os,json; print(json.dumps(dict(os.environ)))'"
        for overrides in ({}, {"TILEXR_VALIDATION_LOOPS": "7", "TILEXR_VALIDATION_CHECKPOINT": "selected"}):
            result = subprocess.run([str(self.bash), "-c", command], cwd=self.root,
                                    env=dict(self.env, **overrides), capture_output=True, text=True, check=True)
            values = json.loads(result.stdout)
            self.assertEqual(values["TILEXR_COMM_ID"], "192.0.2.1:10177")
            self.assertNotIn("TILEXR_VALIDATION_RUN_MODE", values)
            for key in ("TILEXR_VALIDATION_LOOPS", "TILEXR_VALIDATION_CHECKPOINT"):
                self.assertEqual(values.get(key), overrides.get(key))

    def test_single_case_alias_changes_parameters_without_writing_environment_file(self):
        stub = self.root / "tools/validation/workflow.py"
        stub.write_text('''import json, os
from pathlib import Path
config = dict(run_mode=os.environ.get('TILEXR_VALIDATION_RUN_MODE', 'functional'),
              loops=int(os.environ.get('TILEXR_VALIDATION_LOOPS', '1')), warmup=0,
              bs=int(os.environ.get('TILEXR_VALIDATION_BS', '256')), h=7168, topk=6,
              moe_expert_num=384, dtype='bf16')
root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])/'fixture'
root.mkdir(parents=True)
(root/'summary.json').write_text(json.dumps(dict(status='PASS', rank_size=16, chain_configuration=config)))
''')
        before = self.local.read_bytes()
        for name, loops, bs in (("first", 3, 128), ("second", 7, 512)):
            output = self.root / "run" / name
            result = subprocess.run([str(self.bash), "run.sh", "run_mode=functional",
                                     f"loops={loops}", f"bs={bs}"], cwd=self.root,
                                    env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(f"loops={loops}", result.stdout)
            self.assertIn(f"BS={bs}", result.stdout)
            self.assertEqual(self.local.read_bytes(), before)
            self.assertFalse(list(output.glob("*.bak")))

    def test_manual_checkpoint_comes_from_install_and_commit_override_wins(self):
        target = self.root / "install/validation/manifest.json"
        target.parent.mkdir(parents=True)
        target.write_text(json.dumps({"checkpoint": workflow.DRY_RUN_CHECKPOINT}))
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_ROOT": str(self.root)}, clear=True):
            self.assertEqual(workflow.Config().checkpoint, workflow.DRY_RUN_CHECKPOINT)
            os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.CHECKPOINT
            self.assertEqual(workflow.Config().checkpoint, workflow.CHECKPOINT)

    def test_remote_verify_receives_commit_mode_without_editing_remote_config(self):
        (self.root / "hostfile").write_text("localhost\nworker\n")
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json; print(json.dumps(dict(os.environ)))\n")
        before = self.local.read_bytes()
        with mock.patch.dict(os.environ, dict(self.env, TILEXR_VALIDATION_ROOT=str(self.root),
                             TILEXR_VALIDATION_BUILD_MODE="production",
                             TILEXR_VALIDATION_CHECKPOINT=workflow.CHAIN_MULTI_CHECKPOINT,
                             TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_BLOCK_DIM="4",
                             TILEXR_COMM_ID="192.0.2.1:10177"), clear=True):
            config = workflow.Config()
            command = workflow.remote_argv(config, "worker", "verify", "test-build")[-1]
        result = subprocess.run([str(self.bash), "-c", command], input=workflow.REMOTE_SCRIPT,
                                cwd=self.root, env=self.env, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = json.loads(result.stdout.splitlines()[-1])
        self.assertEqual(values["TILEXR_VALIDATION_RUN_MODE"], "breakdown")
        self.assertEqual(values["TILEXR_VALIDATION_BLOCK_DIM"], "4")
        self.assertEqual(values["TILEXR_VALIDATION_CHECKPOINT"], workflow.CHAIN_MULTI_CHECKPOINT)
        self.assertEqual(self.local.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
