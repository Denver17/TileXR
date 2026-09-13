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
        for name in ("set_env.sh", "build_all.sh", "sync.sh", "run_case.sh", "run.sh", "tools/validation/measurements.py",
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
        # Bash expands MSYS short temp-directory names when reporting absolute paths.
        self.root = Path(subprocess.check_output([str(self.bash), '-c', 'pwd -P'],
                         cwd=self.root, env=self.env, text=True).strip())
        self.local = self.root / 'validation.local.sh'

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
        stub.write_text('''import json, os, sys
from pathlib import Path
from measurements import render_case_description
config = dict(run_mode=os.environ.get('TILEXR_VALIDATION_RUN_MODE', 'functional'),
              loops=int(os.environ.get('TILEXR_VALIDATION_LOOPS', '1')), warmup=0,
              bs=int(os.environ.get('TILEXR_VALIDATION_BS', '256')), h=7168, topk=6,
              moe_expert_num=384, dtype='bf16')
root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])/'fixture'
root.mkdir(parents=True)
report = dict(status='PASS', rank_size=16, chain_configuration=config)
(root/'summary.json').write_text(json.dumps(report))
print(render_case_description(report))
print('VALIDATION_' + report['status'])
print('Summary: ' + str(root/'summary.json'))
sys.exit(0 if report['status'] == 'PASS' else 15)
''')
        before = self.local.read_bytes()
        for name, loops, bs in (("first", 3, 128), ("second", 7, 512)):
            output = self.root / "run" / name
            result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96", "-m", "0",
                                     "-l", str(loops), "0", f"bs={bs}"], cwd=self.root,
                                    env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(f"loops={loops}", result.stdout)
            self.assertIn(f"BS={bs}", result.stdout)
            self.assertEqual(self.local.read_bytes(), before)
            self.assertFalse(list(output.glob("*.bak")))

        stub.write_text(stub.read_text().replace("status='PASS'", "status='FAIL'"))
        result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96"], cwd=self.root,
                                env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(self.root / "run/failed")),
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 15)
        self.assertIn("Failed", result.stderr)
        self.assertNotIn("PASS", result.stdout)

    def test_manual_entries_invoke_only_requested_action_and_preserve_failures(self):
        stub = self.root / "tools/validation/workflow.py"
        stub.write_text('''import json, os, sys
from pathlib import Path
action = sys.argv[1]
with Path('calls.jsonl').open('a') as log:
    print(json.dumps(dict(action=action,
        checkpoint=os.environ.get('TILEXR_VALIDATION_CHECKPOINT'),
        comm_id=os.environ.get('TILEXR_COMM_ID'))), file=log)
print(action + ' stdout detail')
print(action + ' stderr detail', file=sys.stderr)
sys.exit(int(os.environ['INJECT_EXIT']))
''')
        for entry, code in (("build_all", 11), ("sync", 12), ("run", 10),
                            ("run", 13), ("run", 14), ("run", 15), ("run", 0)):
            with self.subTest(entry=entry, code=code):
                output = self.root / f"run/{entry}-{code}"
                calls_path = self.root / 'calls.jsonl'
                calls_path.unlink(missing_ok=True)
                result = subprocess.run([str(self.bash), entry + ".sh", *(["-n", "2", "-e", "96"] if entry == "run" else [])], cwd=self.root,
                    env=dict(self.env, INJECT_EXIT=str(code), TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                    text=True, capture_output=True, timeout=10)
                action = "build" if entry == "build_all" else entry
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                calls = [json.loads(line) for line in calls_path.read_text().splitlines()]
                self.assertEqual([call['action'] for call in calls], [action])
                self.assertEqual(calls[0]['comm_id'], None if entry == 'sync' else '192.0.2.1:10177')
                self.assertIn(action + ' stdout detail', result.stdout)
                self.assertIn(action + ' stderr detail', result.stdout + result.stderr)
                if entry == 'run':
                    self.assertIn(str(output / 'environment.log'), result.stdout)
                    self.assertIn(str(output / 'workflow.log'), result.stdout)
                    log = (output / 'workflow.log').read_text()
                    self.assertIn('run stdout detail', log)
                    self.assertIn('run stderr detail', log)
                    if code:
                        self.assertIn(f'workflow exit={code}', result.stderr)

    def test_build_default_selects_counts_only_for_production(self):
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json; print(json.dumps(dict(os.environ)))\n")
        # A prior installed boundary checkpoint must not select an obsolete build.
        installed = self.root / 'install/validation/manifest.json'
        installed.parent.mkdir(parents=True)
        installed.write_text(json.dumps(dict(checkpoint=workflow.MEMORY_BOUNDARY_CHECKPOINT)))
        cann = self.root / 'cann-env.sh'
        cann.write_text('# CANN environment fixture\n')
        for mode, override, arguments, expected, run_mode in (
                ('production', None, [], workflow.SOURCE_SEND_CHECKPOINT, 'functional'),
                ('production', None, ['breakdown=0'], workflow.SOURCE_SEND_CHECKPOINT, 'functional'),
                ('production', None, ['breakdown=1'], workflow.SOURCE_SEND_CHECKPOINT, 'breakdown'),
                ('production', workflow.CHECKPOINT, [], workflow.CHECKPOINT, 'functional'),
                ('host', None, [], None, 'functional')):
            with self.subTest(mode=mode, override=override, arguments=arguments):
                environment = dict(self.env, TILEXR_VALIDATION_BUILD_MODE=mode,
                                   TILEXR_CANN_ENV_SCRIPT=str(cann), TILEXR_VALIDATION_RUN_MODE='breakdown',
                                   TILEXR_VALIDATION_LOOPS='3', TILEXR_VALIDATION_WARMUP='2')
                if override:
                    environment['TILEXR_VALIDATION_CHECKPOINT'] = override
                result = subprocess.run([str(self.bash), 'build_all.sh', *arguments], cwd=self.root,
                    env=environment, text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                values = json.loads(result.stdout.splitlines()[-1])
                self.assertEqual(values.get('TILEXR_VALIDATION_CHECKPOINT'), expected)
                self.assertEqual(values['TILEXR_VALIDATION_BREAKDOWN'], '1' if run_mode == 'breakdown' else '0')
                self.assertEqual(values['TILEXR_VALIDATION_RUN_MODE'], 'breakdown')
                self.assertEqual((values['TILEXR_VALIDATION_LOOPS'], values['TILEXR_VALIDATION_WARMUP']), ('3', '2'))

    def test_environment_failure_shows_reason_and_log_without_running_workflow(self):
        (self.root / 'tools/validation/workflow.py').write_text("raise AssertionError('must not run')\n")
        output = self.root / 'run/environment-failure'
        missing = self.root / 'missing-cann-env.sh'
        result = subprocess.run([str(self.bash), 'run.sh', '-n', '2', '-e', '96'], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_BUILD_MODE='production',
                     TILEXR_CANN_ENV_SCRIPT=str(missing), TILEXR_VALIDATION_RESULT_ROOT=str(output)),
            text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('type=CONFIG_FAILED', result.stderr)
        self.assertIn(str(missing), result.stderr)
        self.assertIn(str(output / 'environment.log'), result.stderr)
        self.assertIn('type=CONFIG_FAILED', (output / 'environment.log').read_text())
        self.assertFalse((output / 'workflow.log').exists())

    def test_log_capture_failure_is_nonzero(self):
        (self.root / 'tools/validation/workflow.py').write_text("print('workflow finished')\n")
        output = self.root / 'run/log-failure'
        (output / 'workflow.log').mkdir(parents=True)
        result = subprocess.run([str(self.bash), 'run.sh', '-n', '2', '-e', '96'], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(output)),
            text=True, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('workflow finished', result.stdout)
        self.assertIn('log capture exit=', result.stderr)
        self.assertIn(str(output / 'workflow.log'), result.stderr)

    def test_bad_arguments_and_missing_configuration_name_the_problem(self):
        for arguments in (['breakdown=2'], ['run_mode=breakdown'], ['breakdown=1']):
            # The last case rejects profiling in the fixture's Host build mode.
            result = subprocess.run([str(self.bash), 'build_all.sh', *arguments], cwd=self.root,
                                    env=self.env, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn('[build] Failed:', result.stderr)
        result = subprocess.run([str(self.bash), 'run.sh', 'typo=3'], cwd=self.root,
                                env=self.env, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn('unknown argument: typo=3', result.stderr)
        self.local.unlink()
        result = subprocess.run([str(self.bash), 'run.sh', '-n', '2', '-e', '96'], cwd=self.root,
                                env=self.env, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn(str(self.local), result.stderr)

    def test_manual_checkpoint_comes_from_install_and_explicit_override_wins(self):
        target = self.root / "install/validation/manifest.json"
        target.parent.mkdir(parents=True)
        target.write_text(json.dumps({"checkpoint": workflow.DRY_RUN_CHECKPOINT}))
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_ROOT": str(self.root)}, clear=True):
            self.assertEqual(workflow.Config().checkpoint, workflow.DRY_RUN_CHECKPOINT)
            os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.CHECKPOINT
            self.assertEqual(workflow.Config().checkpoint, workflow.CHECKPOINT)

    def test_short_options_defaults_modes_and_stale_environment(self):
        (self.root / 'tools/validation/workflow.py').write_text(
            "import os,json; print(json.dumps(dict(os.environ)))\n")
        stale = dict(self.env, TILEXR_VALIDATION_MOE_EXPERT_NUM="1024", TILEXR_VALIDATION_SERVERS="16",
                     TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="999",
                     TILEXR_VALIDATION_WARMUP="99", TILEXR_VALIDATION_BS="512",
                     TILEXR_DIRECT_HOSTS_JSON='[]')
        for mode, name in ((None, "functional"), ("0", "functional"), ("1", "performance"), ("2", "breakdown")):
            args = ['-n', '6', '-e', '288'] + ([] if mode is None else ['-m', mode, '-l', '7', '2'])
            command = subprocess.run([str(self.bash), 'run.sh', *args], cwd=self.root,
                env=stale, capture_output=True, text=True, timeout=10)
            self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
            values = json.loads(command.stdout.splitlines()[-1])
            self.assertEqual(values['TILEXR_VALIDATION_SERVERS'], '6')
            self.assertEqual(values['TILEXR_VALIDATION_MOE_EXPERT_NUM'], '288')
            self.assertEqual(values['TILEXR_VALIDATION_RUN_MODE'], name)
            self.assertEqual((values['TILEXR_VALIDATION_LOOPS'], values['TILEXR_VALIDATION_WARMUP']),
                             ('1', '0') if mode is None else ('7', '2'))
            self.assertEqual(values['TILEXR_VALIDATION_BS'], '96')
            self.assertNotIn('TILEXR_DIRECT_HOSTS_JSON', values)

    def test_cli_errors_and_help_precede_environment_loading(self):
        self.local.unlink()
        for args in ([], [''], ['-n'], ['-n', '1', '-e', '96'], ['-n', '17', '-e', '96'],
                     ['-n', '8', '-e', '96'], ['-n', '2', '-e', '1025'],
                     ['-n', '2', '-e', '96', '-m', '3'], ['-n', '2', '-e', '96', '-l', '3'],
                     ['-n', '2', '-e', '96', '-l', '3', '-1'],
                     ['-n', '2', '-e', '96', '-l', '4294967296', '0'],
                     ['-n', '2', '-e', '96', '-l', '1', '-m', '0'],
                     ['-n', '2', '-e', '96', '-e', '192'],
                     ['-n', '2', '-e', '96', 'bs=96', 'bs=128']):
            with self.subTest(args=args):
                command = subprocess.run([str(self.bash), 'run.sh', *args], cwd=self.root,
                    env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(command.returncode, 2)
                self.assertIn('[run] Failed:', command.stderr)
                self.assertNotIn('validation.local.sh', command.stderr)
        help_result = subprocess.run([str(self.bash), 'run.sh', '--help'], cwd=self.root,
            env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn('-l', help_result.stdout)
        self.assertFalse((self.root / 'run').exists())

    def test_sync_does_not_load_cann_or_require_a_runtime_endpoint(self):
        self.local.write_text('TILEXR_CANN_ENV_SCRIPT=/missing/cann/set_env.sh\nTILEXR_COMM_ID=invalid\n')
        (self.root / 'scripts/common_env.sh').write_text('echo unexpected-toolchain-load >&2\nreturn 9\n')
        (self.root / 'tools/validation/workflow.py').write_text("import sys; print(sys.argv[1])\n")
        command = subprocess.run([str(self.bash), 'sync.sh'], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_BUILD_MODE='production'), capture_output=True, text=True, timeout=10)
        self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
        self.assertNotIn('unexpected-toolchain-load', command.stdout + command.stderr)
        self.assertEqual(command.stdout.splitlines()[-1], 'sync')

    def test_remote_verify_receives_commit_mode_without_editing_remote_config(self):
        (self.root / "hostfile").write_text("127.0.0.1\n192.0.2.2\n")
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json; print(json.dumps(dict(os.environ)))\n")
        before = self.local.read_bytes()
        with mock.patch.dict(os.environ, dict(self.env, TILEXR_VALIDATION_ROOT=str(self.root),
                             TILEXR_VALIDATION_BUILD_MODE="production",
                             TILEXR_VALIDATION_CHECKPOINT=workflow.SOURCE_SEND_CHECKPOINT,
                             TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_BLOCK_DIM="64",
                             TILEXR_COMM_ID="192.0.2.1:10177"), clear=True):
            config = workflow.Config()
            command = workflow.remote_argv(config, "192.0.2.2", "verify", "test-build")[-1]
        result = subprocess.run([str(self.bash), "-c", command], input=workflow.REMOTE_SCRIPT,
                                cwd=self.root, env=self.env, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = json.loads(result.stdout.splitlines()[-1])
        self.assertNotIn("TILEXR_VALIDATION_RUN_MODE", values)
        self.assertNotIn("TILEXR_VALIDATION_BLOCK_DIM", values)
        self.assertEqual(values["TILEXR_VALIDATION_CHECKPOINT"], workflow.SOURCE_SEND_CHECKPOINT)
        self.assertEqual(self.local.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
