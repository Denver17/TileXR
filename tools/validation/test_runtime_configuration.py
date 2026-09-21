"""Focused script tests for immutable deployment config and per-case arguments."""
import json
import os
from pathlib import Path
import shutil
import shlex
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
        for name in ("set_env.sh", "run_commit_validation.sh", "build_all.sh", "sync.sh", "run_case.sh", "run.sh", "run_host_validation.sh", "tools/validation/measurements.py", "tools/validation/operator_measurements.py",
                     "tools/ep/msprof_kernel_stats.py", "tools/validation/run_summary.py"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((repo / name).read_text(), encoding="utf-8")
        shutil.copytree(repo / "tools/profiling", self.root / "tools/profiling",
                        ignore=shutil.ignore_patterns("__pycache__"))
        (self.root / "scripts").mkdir()
        (self.root / "scripts/common_env.sh").write_text("# environment fixture\n")
        self.local = self.root / "validation.local.sh"
        self.local.write_text('TILEXR_COMM_ID=192.0.2.1:10177\n'
                              'TILEXR_DEMO_BARRIER_ADDR=192.0.2.1:10274\n'
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
            self.assertNotIn("TILEXR_COMM_ID", values)
            self.assertNotIn("TILEXR_DEMO_BARRIER_ADDR", values)
            self.assertNotIn("TILEXR_VALIDATION_RUN_MODE", values)
            for key in ("TILEXR_VALIDATION_LOOPS", "TILEXR_VALIDATION_CHECKPOINT"):
                self.assertEqual(values.get(key), overrides.get(key))

    def test_run_discards_stale_endpoints_but_preserves_cli_overrides(self):
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json\nfrom pathlib import Path\nfrom run_summary import write_result\n"
            "root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])\n"
            "(root/'endpoints.json').write_text(json.dumps([os.environ.get('TILEXR_COMM_ID'), "
            "os.environ.get('TILEXR_DEMO_BARRIER_ADDR')]))\n"
            "write_result(root, dict(status='PASS'), root/'summary.json')\n")
        for index, arguments in enumerate(([], ["comm_id=192.0.2.3:12300", "barrier_addr=192.0.2.3:12400"])):
            with self.subTest(arguments=arguments):
                output = self.root / "run" / str(index)
                result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96", *arguments],
                    cwd=self.root, env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(output),
                                           TILEXR_COMM_ID="192.0.2.99:11111",
                                           TILEXR_DEMO_BARRIER_ADDR="192.0.2.99:11208"),
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads((output / "endpoints.json").read_text()),
                                 ["192.0.2.3:12300", "192.0.2.3:12400"] if arguments else [None, None])

    def test_single_case_alias_changes_parameters_without_writing_environment_file(self):
        stub = self.root / "tools/validation/workflow.py"
        stub.write_text('''import json, os, sys
from pathlib import Path
from measurements import render_case_description
from run_summary import write_result
config = dict(run_mode=os.environ.get('TILEXR_VALIDATION_RUN_MODE', 'functional'),
              loops=int(os.environ.get('TILEXR_VALIDATION_LOOPS', '1')), warmup=0,
              bs=int(os.environ.get('TILEXR_VALIDATION_BS', '256')), h=7168, topk=6,
              moe_expert_num=384, dtype='bf16', server_dedup_enabled=True)
root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])/'fixture'
root.mkdir(parents=True)
report = dict(status='PASS', rank_size=16, chain_configuration=config)
(root/'summary.json').write_text(json.dumps(report))
write_result(root.parent, report, root/'summary.json')
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
            summary = json.loads((output / "fixture/summary.json").read_text())
            self.assertTrue(summary["chain_configuration"]["server_dedup_enabled"])
            self.assertEqual(self.local.read_bytes(), before)
            self.assertFalse(list(output.glob("*.bak")))

        stub.write_text(stub.read_text().replace("status='PASS'", "status='FAIL'"))
        result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96"], cwd=self.root,
                                env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(self.root / "run/failed")),
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 15)
        self.assertIn("VALIDATION_FAIL 执行阶段失败", result.stdout)
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
if action == 'run':
    from run_summary import write_result
    root = Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])
    write_result(root, dict(status='FAIL' if int(os.environ['INJECT_EXIT']) else 'PASS'), root/'summary.json')
sys.exit(int(os.environ['INJECT_EXIT']))
''')
        for entry, code in (("build_all", 11), ("sync", 12), ("run_host_validation", 0), ("run_host_validation", 15), ("run", 10),
                            ("run", 13), ("run", 14), ("run", 15), ("run", 0)):
            with self.subTest(entry=entry, code=code):
                output = self.root / f"run/{entry}-{code}"
                calls_path = self.root / 'calls.jsonl'
                calls_path.unlink(missing_ok=True)
                result = subprocess.run([str(self.bash), entry + ".sh", *(["-n", "2", "-e", "96"] if entry == "run" else [])], cwd=self.root,
                    env=dict(self.env, INJECT_EXIT=str(code), TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                    text=True, capture_output=True, timeout=10)
                action = {"build_all": "build", "run_host_validation": "host-validate"}.get(entry, entry)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                calls = [json.loads(line) for line in calls_path.read_text().splitlines()]
                self.assertEqual([call['action'] for call in calls], [action])
                self.assertIsNone(calls[0]['comm_id'])
                if entry == 'run':
                    self.assertEqual(len(result.stdout.splitlines()), 1)
                    self.assertEqual(result.stderr, '')
                    self.assertNotIn('detail', result.stdout)
                    self.assertIn(str(output / 'summary.json'), result.stdout)
                    log = (output / 'workflow.log').read_text()
                    self.assertIn('run stdout detail', log)
                    self.assertIn('run stderr detail', log)
                else:
                    self.assertIn(action + ' stdout detail', result.stdout)
                    self.assertIn(action + ' stderr detail', result.stdout + result.stderr)

    def test_run_host_validation_is_optional_and_failure_stops_workflow(self):
        (self.root / 'tools/validation/workflow.py').write_text('''import json, os, sys
from pathlib import Path
action = sys.argv[1]
with Path('calls.jsonl').open('a') as log:
    print(json.dumps(dict(action=action, environment_action=os.environ['TILEXR_VALIDATION_ACTION'])), file=log)
print(action + ' stdout detail')
print(action + ' stderr detail', file=sys.stderr)
if action == 'run':
    from run_summary import write_result
    root = Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])
    write_result(root, dict(status='PASS'), root/'summary.json')
sys.exit(int(os.environ['INJECT_HOST_EXIT']) if action == 'host-validate' else 0)
''')
        for mode in ('0', '1', '2'):
            for option in (None, '1'):
                self.check_host_validation_invocation(mode, option, 0)
        self.check_host_validation_invocation('0', '0', 15)
        self.check_host_validation_invocation('0', '1', 15)

    def check_host_validation_invocation(self, mode, option, host_exit):
        with self.subTest(mode=mode, option=option, host_exit=host_exit):
            output = self.root / f'run/host-option-{mode}-{option}-{host_exit}'
            calls_path = self.root / 'calls.jsonl'
            calls_path.unlink(missing_ok=True)
            result = subprocess.run([str(self.bash), 'run.sh', '-n', '2', '-e', '96', '-m', mode,
                                     *([] if option is None else ['host_validation=' + option])], cwd=self.root,
                env=dict(self.env, INJECT_HOST_EXIT=str(host_exit), TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                text=True, capture_output=True, timeout=10)
            enabled = option == '1'
            expected_code = host_exit if enabled else 0
            self.assertEqual(result.returncode, expected_code, result.stdout + result.stderr)
            calls = [json.loads(line) for line in calls_path.read_text().splitlines()]
            expected = (['host-validate'] if enabled else []) + ([] if expected_code else ['run'])
            self.assertEqual([call['action'] for call in calls], expected)
            self.assertEqual([call['environment_action'] for call in calls], expected)
            self.assertEqual((output / 'host-validation.log').exists(), enabled)
            self.assertEqual((output / 'workflow.log').exists(), not expected_code)
            if enabled:
                log = (output / 'host-validation.log').read_text()
                self.assertIn('host-validate stdout detail', log)
                self.assertIn('host-validate stderr detail', log)
            if expected_code:
                self.assertIn('VALIDATION_FAIL 执行阶段失败 | exit=15', result.stdout)
                self.assertEqual(len(result.stdout.splitlines()), 1)

    def test_commit_entry_selects_p24_production_before_workflow_preflight(self):
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json,sys; print(json.dumps(dict(action=sys.argv[1], env=dict(os.environ))))\n")
        cann = self.root / "cann-env.sh"
        cann.write_text("# environment fixture\n")
        with self.local.open("a") as local:
            local.write("TILEXR_CANN_ENV_SCRIPT=" + shlex.quote(str(cann)) + "\n"
                        "TILEXR_EP_SERVER_DEDUP_TEST_DEVICE=5\n")
        before = self.local.read_bytes()
        result = subprocess.run([str(self.bash), "run_commit_validation.sh"], cwd=self.root,
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = json.loads(result.stdout.splitlines()[-1])
        self.assertEqual(values["action"], "commit-validate")
        self.assertEqual(values["env"]["TILEXR_VALIDATION_CHECKPOINT"], workflow.COMBINE_M5_CHECKPOINT)
        self.assertEqual(values["env"]["TILEXR_VALIDATION_BUILD_MODE"], "production")
        self.assertEqual(values["env"]["TILEXR_VALIDATION_ACTION"], "commit-validate")
        self.assertNotIn("TILEXR_EP_SERVER_DEDUP_TEST_DEVICE", values["env"])
        self.assertFalse((self.root / "hostfile").exists())
        self.assertEqual(self.local.read_bytes(), before)

    def test_commit_rejects_resume_and_clears_stale_deployment_values(self):
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json; print(json.dumps(dict(os.environ)))\n")
        cann = self.root / "cann-env.sh"
        cann.write_text("# environment fixture\n")
        with self.local.open("a") as local:
            local.write("TILEXR_CANN_ENV_SCRIPT=" + shlex.quote(str(cann)) + "\n"
                        "export TILEXR_VALIDATION_RESUME_SUMMARY=deployment-stale.json\n")
        path = str(self.root / "old run/summary.json")
        result = subprocess.run([str(self.bash), "run_commit_validation.sh"], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_RESUME_SUMMARY="shell-stale.json"),
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = json.loads(result.stdout.splitlines()[-1])
        self.assertNotIn("TILEXR_VALIDATION_RESUME_SUMMARY", values)
        for args in (["--resume"], ["--resume", ""], ["--resume", path], ["--resume", path, "extra"]):
            result = subprocess.run([str(self.bash), "run_commit_validation.sh", *args], cwd=self.root,
                                    env=self.env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)

    def test_combine_run_selects_p24_and_preserves_prior_gates(self):
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json\nfrom pathlib import Path\nfrom run_summary import write_result\n"
            "root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])\n"
            "(root/'config.json').write_text(json.dumps(dict(os.environ)))\n"
            "write_result(root, dict(status='PASS'), root/'summary.json')\n")
        cann = self.root / "cann-env.sh"
        cann.write_text("# environment fixture\n")
        with self.local.open("a") as local:
            local.write("TILEXR_CANN_ENV_SCRIPT=" + shlex.quote(str(cann)) + "\n")
        for operator, checkpoint, loops, warmup, mode, accepted in (
                ("combine", None, "1", "0", "0", True),
                ("both", None, "1", "0", "0", True),
                ("dispatch", workflow.COMBINE_MULTI_CHECKPOINT, "5", "2", "0", True),
                ("both", workflow.COMBINE_CHAIN_CHECKPOINT, "1", "0", "0", True),
                ("both", workflow.COMBINE_CHAIN_CHECKPOINT, "5", "2", "0", False),
                ("both", workflow.COMBINE_MULTI_CHECKPOINT, "5", "2", "1", False),
                ("both", workflow.COMBINE_MULTI_CHECKPOINT, "5", "2", "0", True),
                ("both", None, "1", "0", "1", True),
                ("combine", None, "1", "0", "2", True),
                ("both", workflow.COMBINE_PROFILE_CHECKPOINT, "5", "2", "0", True),
                ("both", workflow.COMBINE_BOUNDARY_CHECKPOINT, "5", "2", "2", False)):
            with self.subTest(operator=operator, checkpoint=checkpoint, mode=mode):
                output = self.root / "run" / (operator + str(checkpoint) + loops + mode)
                env = dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(output), TILEXR_VALIDATION_BOUNDARY_PROBE="1")
                if checkpoint:
                    env["TILEXR_VALIDATION_CHECKPOINT"] = checkpoint
                result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96", "-m", mode,
                                         "-l", loops, warmup, "operator_mode=" + operator], cwd=self.root,
                                        env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode == 0, accepted, result.stdout + result.stderr)
                if accepted:
                    values = json.loads((output / "config.json").read_text())
                    self.assertEqual(values["TILEXR_VALIDATION_BOUNDARY_PROBE"], "0")
                    self.assertEqual(values["TILEXR_VALIDATION_CHECKPOINT"], checkpoint or workflow.COMBINE_M5_CHECKPOINT)
                    self.assertEqual((values["TILEXR_VALIDATION_LOOPS"], values["TILEXR_VALIDATION_WARMUP"]), (loops, warmup))

    def test_case_dry_run_resolves_new_modes_before_loading_runtime(self):
        self.local.write_text("TILEXR_CANN_ENV_SCRIPT=/missing/cann\n"
                              "TILEXR_VALIDATION_OPERATOR_MODE=obsolete\n"
                              "TILEXR_VALIDATION_DISPATCH_QUANT_MODE=bad\n"
                              "TILEXR_VALIDATION_COMBINE_QUANT_MODE=4\n"
                              "TILEXR_VALIDATION_BOUNDARY_PROBE=1\n")
        (self.root / "scripts/common_env.sh").write_text("echo unexpected-cann-load >&2\nreturn 9\n")
        (self.root / "tools/validation/workflow.py").write_text(
            "import json,os,sys\nfrom pathlib import Path\nfrom run_summary import write_result\n"
            "root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])\n"
            "(root/'configuration.json').write_text(json.dumps(dict(action=sys.argv[1], env=dict(os.environ))))\n"
            "write_result(root, dict(status='PASS'), root/'summary.json')\n")
        for operator in ("dispatch", "combine", "both"):
            output = self.root / "run" / operator
            result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96",
                                     "operator_mode=" + operator, "dry_run=1", "dispatch_quant_mode=4"],
                cwd=self.root, env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config = json.loads((output / "configuration.json").read_text())
            self.assertEqual(config["action"], "case-dry-run")
            self.assertEqual(config["env"]["TILEXR_VALIDATION_OPERATOR_MODE"], operator)
            self.assertEqual(config["env"]["TILEXR_VALIDATION_QUANT_MODE"], "4")
            self.assertEqual(config["env"]["TILEXR_VALIDATION_DISPATCH_QUANT_MODE"], "4")
            self.assertEqual(config["env"]["TILEXR_VALIDATION_COMBINE_QUANT_MODE"], "0")
            self.assertEqual(config["env"]["TILEXR_VALIDATION_CHECKPOINT"], workflow.COMBINE_DRY_RUN_CHECKPOINT)
            self.assertEqual(config["env"]["TILEXR_VALIDATION_BOUNDARY_PROBE"], "0")
            self.assertNotIn("unexpected-cann-load", (output / "environment.log").read_text())

    def test_combine_cli_rejects_conflicts_and_later_stages_without_loading_environment(self):
        self.local.unlink()
        for arguments in (["operator_mode=both", "-l", "2"],
                          ["dry_run=1", "quant_mode=4", "dispatch_quant_mode=0"],
                          ["dry_run=1", "combine_quant_mode=4"], ["dry_run=2"],
                          ["operator_mode=other"], ["dispatch_quant_mode=1"]):
            with self.subTest(arguments=arguments):
                result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96", *arguments],
                    cwd=self.root, env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("validation.local.sh", result.stderr)
        self.assertFalse((self.root / "run").exists())

    def test_commit_environment_failure_never_runs_workflow(self):
        (self.root / "tools/validation/workflow.py").write_text("raise AssertionError('must not run')\n")
        result = subprocess.run([str(self.bash), "run_commit_validation.sh"], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_PYTHON=str(self.root / "missing-python")),
            capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("COMMIT_VALIDATION_FAIL", result.stdout + result.stderr)
        self.assertIn("type=CONFIG_FAILED", result.stdout + result.stderr)
        self.assertNotIn("AssertionError", result.stdout + result.stderr)

    def test_build_default_selects_dispatch_only_for_production(self):
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json; print(json.dumps(dict(os.environ)))\n")
        # A prior installed boundary checkpoint must not select an obsolete build.
        installed = self.root / 'install/validation/manifest.json'
        installed.parent.mkdir(parents=True)
        installed.write_text(json.dumps(dict(checkpoint=workflow.MEMORY_BOUNDARY_CHECKPOINT)))
        cann = self.root / 'cann-env.sh'
        cann.write_text('# CANN environment fixture\n')
        for mode, override, arguments, expected, run_mode in (
                ('production', None, [], workflow.DISPATCH_CHECKPOINT, 'functional'),
                ('production', None, ['breakdown=0'], workflow.DISPATCH_CHECKPOINT, 'functional'),
                ('production', None, ['breakdown=1'], workflow.DISPATCH_CHECKPOINT, 'breakdown'),
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

    def test_run_prints_selected_operator_performance_on_separate_lines(self):
        cann = self.root / "cann-env.sh"
        cann.write_text("# CANN environment fixture\n")
        (self.root / "tools/validation/workflow.py").write_text("""import os
from pathlib import Path
from run_summary import write_result
root = Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])
operator = os.environ['TILEXR_VALIDATION_OPERATOR_MODE']
config = dict(operator_mode=operator, run_mode=os.environ['TILEXR_VALIDATION_RUN_MODE'],
              bs=int(os.environ['TILEXR_VALIDATION_BS']), loops=int(os.environ['TILEXR_VALIDATION_LOOPS']),
              warmup=int(os.environ['TILEXR_VALIDATION_WARMUP']))
active = ('dispatch', 'combine') if operator == 'both' else (operator,)
summaries = {op: dict(backend='memory', native_status=op, kernel_sha256='a'*64,
    algorithm_communication_bytes=0, performance=dict(samples_per_rank=config['loops'],
    maximum_us=10 if op == 'dispatch' else 60, minimum_us=10 if op == 'dispatch' else 60,
    mean_us=10 if op == 'dispatch' else 60)) for op in active}
report = dict(status='PASS', rank_size=16, completed_ranks=16, chain_configuration=config)
if operator == 'dispatch':
    report['measurements'] = summaries['dispatch']
else:
    report['operator_measurements'] = summaries
print('rank details must stay in workflow.log')
write_result(root, report, root/'summary.json')
""")
        for operator, expected in (("dispatch", ["dispatch"]), ("combine", ["combine"]),
                                   ("both", ["dispatch", "combine"])):
            with self.subTest(operator=operator):
                output = self.root / "run" / operator
                result = subprocess.run([str(self.bash), "run.sh", "-n", "2", "-e", "96", "-m", "1",
                                         "-l", "20", "5", "operator_mode=" + operator, "dtype=bf16"],
                    cwd=self.root, env=dict(self.env, TILEXR_CANN_ENV_SCRIPT=str(cann),
                        TILEXR_VALIDATION_RESULT_ROOT=str(output)), capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, (output / "result.txt").read_text())
                lines = result.stdout.splitlines()
                self.assertEqual(len(lines), len(expected) + 1)
                self.assertTrue(lines[0].startswith("VALIDATION_PASS"))
                self.assertNotIn("latency", lines[0])
                for line, op in zip(lines[1:], expected):
                    self.assertTrue(line.startswith(op + ":"), line)
                    value = "10.000" if op == "dispatch" else "60.000"
                    self.assertIn("server_latency(us) min/mean/max=" + "/".join([value] * 3), line)
                self.assertNotIn("rank details", result.stdout)

    def test_environment_failure_shows_reason_and_log_without_running_workflow(self):
        (self.root / 'tools/validation/workflow.py').write_text("raise AssertionError('must not run')\n")
        output = self.root / 'run/environment-failure'
        missing = self.root / 'missing-cann-env.sh'
        result = subprocess.run([str(self.bash), 'run.sh', '-n', '2', '-e', '96'], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_BUILD_MODE='production',
                     TILEXR_CANN_ENV_SCRIPT=str(missing), TILEXR_VALIDATION_RESULT_ROOT=str(output)),
            text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('VALIDATION_FAIL 执行阶段失败', result.stdout)
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertEqual(result.stderr, '')
        self.assertIn(str(output), result.stdout)
        self.assertIn(str(missing), (output / 'environment.log').read_text())
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
        self.assertNotIn('workflow finished', result.stdout)
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertIn('VALIDATION_FAIL 执行阶段失败', result.stdout)
        self.assertIn('workflow.log', (output / 'launcher.log').read_text())

    def test_missing_workflow_result_never_reuses_previous_pass(self):
        (self.root / 'tools/validation/workflow.py').write_text("print('unfinished workflow')\n")
        output = self.root / 'run/reused'
        output.mkdir(parents=True)
        (output / 'result.txt').write_text('VALIDATION_PASS stale\n')
        result = subprocess.run([str(self.bash), 'run.sh', '-n', '2', '-e', '96'], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_RESULT_ROOT=str(output)),
            text=True, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertIn('VALIDATION_FAIL 执行阶段失败', result.stdout)
        self.assertNotIn('PASS', result.stdout)
        self.assertIn('unfinished workflow', (output / 'workflow.log').read_text())

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
            "import os,json\nfrom pathlib import Path\nfrom run_summary import write_result\n"
            "print(json.dumps(dict(os.environ)))\n"
            "root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])\n"
            "write_result(root, dict(status='PASS'), root/'summary.json')\n")
        stale = dict(self.env, TILEXR_VALIDATION_MOE_EXPERT_NUM="1024", TILEXR_VALIDATION_SERVERS="16",
                     TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="999",
                     TILEXR_VALIDATION_WARMUP="99", TILEXR_VALIDATION_BS="512",
                     TILEXR_DIRECT_HOSTS_JSON='[]')
        for mode, name in ((None, "functional"), ("0", "functional"), ("1", "performance"), ("2", "breakdown")):
            args = ['-n', '6', '-e', '288'] + ([] if mode is None else ['-m', mode, '-l', '7', '2'])
            output = self.root / f'run/options-{mode}'
            command = subprocess.run([str(self.bash), 'run.sh', *args], cwd=self.root,
                env=dict(stale, TILEXR_VALIDATION_RESULT_ROOT=str(output)), capture_output=True, text=True, timeout=10)
            self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
            values = json.loads((output / 'workflow.log').read_text().splitlines()[-1])
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
                     ['-n', '2', '-e', '96', 'host_validation='],
                     ['-n', '2', '-e', '96', 'host_validation=2'],
                     ['-n', '2', '-e', '96', 'host_validation=01'],
                     ['-n', '2', '-e', '96', 'host_validation=0', 'host_validation=1'],
                     ['-n', '2', '-e', '96', 'case_analysis='],
                     ['-n', '2', '-e', '96', 'case_analysis=2'],
                     ['-n', '2', '-e', '96', 'case_analysis=01'],
                     ['-n', '2', '-e', '96', 'case_analysis=0', 'case_analysis=1'],
                     ['-n', '2', '-e', '96', 'route_mode='],
                     ['-n', '2', '-e', '96', 'route_mode=uniform'],
                     ['-n', '2', '-e', '96', 'route_mode=balance shift'],
                     ['-n', '2', '-e', '96', 'route_mode=random', 'route_mode=balance'],
                     ['-n', '2', '-e', '96', 'seed='],
                     ['-n', '2', '-e', '96', 'seed=-1'],
                     ['-n', '2', '-e', '96', 'seed=4294967296'],
                     ['-n', '2', '-e', '96', 'seed=99999999999999999999999'],
                     ['-n', '2', '-e', '96', 'seed=01'],
                     ['-n', '2', '-e', '96', 'seed=+1'],
                     ['-n', '2', '-e', '96', 'seed=1.5'],
                     ['-n', '2', '-e', '96', 'seed=0', 'seed=7'],
                     ['-n', '2', '-e', '96', 'bs=96', 'bs=128']):
            with self.subTest(args=args):
                command = subprocess.run([str(self.bash), 'run.sh', *args], cwd=self.root,
                    env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(command.returncode, 2)
                self.assertIn('VALIDATION_FAIL 执行阶段失败', command.stderr)
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

    def test_host_validate_help_and_bad_arguments_do_not_load_environment(self):
        (self.root / 'set_env.sh').write_text('echo unexpected-environment >&2\nreturn 9\n')
        for args, code in ((['--help'], 0), (['-h'], 0), (['-n', '2'], 2), (['--help', 'extra'], 2)):
            with self.subTest(args=args):
                result = subprocess.run([str(self.bash), 'run_host_validation.sh', *args], cwd=self.root,
                    env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                self.assertNotIn('unexpected-environment', result.stdout + result.stderr)
        self.assertFalse((self.root / 'run').exists())

    def test_host_validate_does_not_load_cann_or_require_runtime_environment(self):
        self.local.write_text('TILEXR_CANN_ENV_SCRIPT=/missing/cann/set_env.sh\nTILEXR_COMM_ID=invalid\n')
        (self.root / 'scripts/common_env.sh').write_text('echo unexpected-toolchain-load >&2\nreturn 9\n')
        (self.root / 'tools/validation/workflow.py').write_text("import sys; print(sys.argv[1])\n")
        result = subprocess.run([str(self.bash), 'run_host_validation.sh'], cwd=self.root,
            env=dict(self.env, TILEXR_VALIDATION_BUILD_MODE='production'), capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('unexpected-toolchain-load', result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines()[-1], 'host-validate')

    def test_case_analysis_cli_overrides_stale_environment_and_local_config(self):
        with self.local.open('a') as config:
            config.write('TILEXR_VALIDATION_CASE_ANALYSIS=1\n')
        (self.root / 'tools/validation/workflow.py').write_text(
            "import os\nfrom pathlib import Path\nfrom run_summary import write_result\n"
            "root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])\n"
            "(root/'analysis.txt').write_text(os.environ['TILEXR_VALIDATION_CASE_ANALYSIS'])\n"
            "write_result(root, dict(status='PASS'), root/'summary.json')\n")
        for value in (None, '0', '1'):
            output = self.root / 'run' / str(value)
            result = subprocess.run([str(self.bash), 'run.sh', '-n', '2', '-e', '96',
                                     *([] if value is None else ['case_analysis=' + value])], cwd=self.root,
                env=dict(self.env, TILEXR_VALIDATION_CASE_ANALYSIS='1', TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((output / 'analysis.txt').read_text(), value or '0')

    def test_routing_cli_defaults_and_overrides_ignore_stale_configuration(self):
        with self.local.open('a') as config:
            config.write('TILEXR_VALIDATION_ROUTE_MODE=obsolete\nTILEXR_VALIDATION_SEED=invalid\n')
        (self.root / 'tools/validation/workflow.py').write_text(
            "import json, os\nfrom pathlib import Path\nfrom run_summary import write_result\n"
            "root=Path(os.environ['TILEXR_VALIDATION_RESULT_ROOT'])\n"
            "(root/'routing.json').write_text(json.dumps([os.environ['TILEXR_VALIDATION_ROUTE_MODE'], "
            "os.environ['TILEXR_VALIDATION_SEED']]))\n"
            "write_result(root, dict(status='PASS'), root/'summary.json')\n")
        cases = [(None, None, 'balance_shift', '2026'), ('random', '0', 'random', '0'),
                 ('balance', '4294967295', 'balance', '4294967295'),
                 ('balance_shift', '17', 'balance_shift', '17'), (None, '7', 'balance_shift', '7'),
                 ('random', None, 'random', '2026')]
        for index, (mode, seed, expected_mode, expected_seed) in enumerate(cases):
            with self.subTest(mode=mode, seed=seed):
                output = self.root / 'run' / str(index)
                arguments = ([] if mode is None else ['route_mode=' + mode]) + ([] if seed is None else ['seed=' + seed])
                result = subprocess.run([str(self.bash), 'run.sh', '-n', '16', '-e', '384', *arguments],
                    cwd=self.root, env=dict(self.env, TILEXR_VALIDATION_ROUTE_MODE='random',
                        TILEXR_VALIDATION_SEED='999', TILEXR_VALIDATION_RESULT_ROOT=str(output)),
                    text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads((output / 'routing.json').read_text()), [expected_mode, expected_seed])

    def test_remote_verify_receives_commit_mode_without_editing_remote_config(self):
        (self.root / "hostfile").write_text("127.0.0.1\n192.0.2.2\n")
        (self.root / "tools/validation/workflow.py").write_text(
            "import os,json; print(json.dumps(dict(os.environ)))\n")
        before = self.local.read_bytes()
        with mock.patch.dict(os.environ, dict(self.env, TILEXR_VALIDATION_ROOT=str(self.root),
                             TILEXR_VALIDATION_BUILD_MODE="production",
                             TILEXR_VALIDATION_CHECKPOINT=workflow.DISPATCH_CHECKPOINT,
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
        self.assertEqual(values["TILEXR_VALIDATION_CHECKPOINT"], workflow.DISPATCH_CHECKPOINT)
        self.assertEqual(self.local.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
