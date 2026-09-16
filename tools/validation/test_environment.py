#!/usr/bin/env python3
"""真实 Bash 环境入口回归；不依赖 CANN 安装、SSH 或 NPU。"""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "需要 POSIX Python 和 Bash")
class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tilexr environment ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source with spaces"
        repo = Path(__file__).resolve().parents[2]
        for name in ("set_env.sh", "scripts/common_env.sh", "scripts/common_util.sh",
                     "tools/validation/workflow.py", "tools/validation/source.py", "tools/validation/measurements.py", "tools/validation/run_summary.py",
                     "tools/ep/msprof_kernel_stats.py"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((repo / name).read_text(encoding="utf-8"), encoding="utf-8")
        self.hostfile = self.root / "hostfile"
        self.hostfile.write_text("localhost devices=0,2\n# persistent deployment config\n")
        self.probes = self.root / "probe-calls"
        self.bin = self.root / "fake-bin"
        self.bin.mkdir()
        for name in ("npu-smi", "lspci"):
            self.executable(self.bin / name,
                            'printf "%s\\n" "$0" >> "$TEST_PROBE_CALLS"\n'
                            'printf "| 0 | Ascend950 |\\n"\n')
        self.bash = str(Path(sys.executable).parent / "bash")
        if not Path(self.bash).exists():
            self.bash = shutil.which("bash")
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith(("TILEXR_", "ASCEND_", "_TILEXR_"))
                            and key not in ("MPI_HOME", "LD_LIBRARY_PATH")}
        self.environment.update(TILEXR_VALIDATION_BUILD_MODE="host",
                                TILEXR_VALIDATION_PYTHON=sys.executable,
                                TEST_PROBE_CALLS=str(self.probes),
                                PATH=os.pathsep.join((str(self.bin), str(Path(self.bash).parent),
                                                      os.environ["PATH"])))
        self.snapshot = self.root / "snapshot.py"
        self.snapshot.write_text("import json, os, sys\n"
                                 "print('SNAPSHOT=' + json.dumps(dict(os.environ)))\n")

    def executable(self, path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/usr/bin/env bash\n" + content, encoding="utf-8")
        path.chmod(0o755)

    def run_shell(self, script, environment=None):
        return subprocess.run([self.bash, "-c", "set -euo pipefail\n" + script,
                               "bash", str(self.root)], cwd=self.root,
                              env=environment or self.environment, capture_output=True,
                              text=True, timeout=15)

    def snapshots(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return [json.loads(line.removeprefix("SNAPSHOT="))
                for line in result.stdout.splitlines() if line.startswith("SNAPSHOT=")]

    def fake_cann(self, name="toolkit", real_driver_headers=True, configure_driver=True):
        toolkit = self.root / name
        driver = self.root / "driver"
        (driver / "lib64/driver").mkdir(parents=True, exist_ok=True)
        if real_driver_headers:
            (driver / "kernel/inc").mkdir(parents=True, exist_ok=True)
        script = toolkit / "set_env.sh"
        driver_config = f'export ASCEND_DRIVER_PATH={shlex.quote(str(driver))}\n' if configure_driver else ""
        self.executable(script,
                        f'export ASCEND_HOME_PATH={shlex.quote(str(toolkit))}\n'
                        + driver_config +
                        'export PATH="$ASCEND_HOME_PATH/bin:$PATH"\n'
                        'export LD_LIBRARY_PATH="$ASCEND_HOME_PATH/lib64:${LD_LIBRARY_PATH:-}"\n'
                        'export CANN_LOAD_COUNT=$((${CANN_LOAD_COUNT:-0} + 1))\n')
        return script, toolkit, driver

    def test_host_repeated_source_skips_hardware_cann_and_extra_tool_paths(self):
        fallback = self.root / "env/cann/cann/set_env.sh"
        self.executable(fallback, 'echo unexpected-cann >> "$TEST_PROBE_CALLS"\nreturn 9\n')
        before = self.hostfile.read_bytes()
        result = self.run_shell('for attempt in 1 2 3; do\n'
                                '  source "$1/set_env.sh"\n'
                                '  "$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n'
                                'done\n')
        snapshots = self.snapshots(result)
        self.assertEqual(len(snapshots), 3)
        self.assertEqual(len({entry["PATH"] for entry in snapshots}), 1)
        self.assertEqual(len({entry["LD_LIBRARY_PATH"] for entry in snapshots}), 1)
        for entry in snapshots:
            self.assertNotIn("MPI_HOME", entry)
            self.assertNotIn("TILEXR_HCCL_TEST_HOME", entry)
            self.assertNotIn("ASCEND_HOME_PATH", entry)
            self.assertNotIn("ASCEND_DRIVER_PATH", entry)
            utilities = Path(entry["TILEXR_HOME"]) / "env/util"
            for name in ("mpich", "ccache", "ripgrep", "sshpass", "time", "patch", "pigz"):
                self.assertNotIn(str(utilities / name), entry["PATH"])
            self.assertEqual(entry["TILEXR_CANN_ENV_SCRIPT"], "")
        self.assertFalse(self.probes.exists())
        self.assertEqual(self.hostfile.read_bytes(), before)

    def test_installed_cmake_and_ctest_remain_available_without_path_growth(self):
        cmake_bin = self.root / "env/util/cmake/bin"
        for name in ("cmake", "ctest"):
            self.executable(cmake_bin / name, f"printf 'INSTALLED_{name.upper()}_PASS\\n'\n")
        result = self.run_shell('for attempt in 1 2 3; do\n'
                                '  source "$1/set_env.sh"\n'
                                '  printf "CMAKE=%s\\n" "$(command -v cmake)"\n'
                                '  printf "CTEST=%s\\n" "$(command -v ctest)"\n'
                                '  cmake --version\n'
                                '  ctest --version\n'
                                '  "$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n'
                                'done\n')
        entries = self.snapshots(result)
        self.assertEqual(len({entry["PATH"] for entry in entries}), 1)
        for name in ("cmake", "ctest"):
            resolved = [line.split("=", 1)[1] for line in result.stdout.splitlines()
                        if line.startswith(name.upper() + "=")]
            self.assertEqual(len(resolved), 3)
            self.assertTrue(all((cmake_bin / name).samefile(path) for path in resolved))
            self.assertEqual(result.stdout.splitlines().count(f"INSTALLED_{name.upper()}_PASS"), 3)
        cmake_path = str(Path(entries[0]["TILEXR_HOME"]) / "env/util/cmake/bin")
        self.assertEqual(entries[0]["PATH"].split(":").count(cmake_path), 1)

    def test_production_reloads_configuration_and_preserves_unique_paths(self):
        self.environment["TILEXR_VALIDATION_BUILD_MODE"] = "production"
        cann, toolkit, driver = self.fake_cann()
        local = self.root / "validation.local.sh"
        local.write_text(f'TILEXR_VALIDATION_BUILD_MODE=production\n'
                         f'TILEXR_CANN_ENV_SCRIPT={shlex.quote(str(cann))}\n'
                         'TILEXR_VALIDATION_TIMEOUT=30\n')
        result = self.run_shell('source "$1/set_env.sh"\n'
                                '"$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n'
                                'printf "# comment only\\n" >> "$1/validation.local.sh"\n'
                                'source "$1/set_env.sh"\n'
                                '"$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n'
                                'printf "TILEXR_VALIDATION_TIMEOUT=60\\n" >> "$1/validation.local.sh"\n'
                                'source "$1/set_env.sh"\n'
                                '"$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n'
                                'bash -c \'source "$1/set_env.sh"; '
                                '"$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\' bash "$1"\n')
        entries = self.snapshots(result)
        self.assertEqual([entry["TILEXR_VALIDATION_TIMEOUT"] for entry in entries], ["180", "180", "180", "180"])
        self.assertEqual([entry["CANN_LOAD_COUNT"] for entry in entries], ["1", "2", "3", "4"])
        for name in ("PATH", "LD_LIBRARY_PATH", "ASCEND_HOME_PATH", "ASCEND_DRIVER_PATH"):
            self.assertEqual(len({entry[name] for entry in entries}), 1, name)
        self.assertEqual(entries[0]["ASCEND_HOME_PATH"], str(toolkit))
        self.assertEqual(entries[0]["ASCEND_DRIVER_PATH"], str(driver))
        self.assertIn(str(toolkit / "bin"), entries[0]["PATH"].split(":"))
        self.assertIn(str(driver / "lib64/driver"), entries[0]["LD_LIBRARY_PATH"])
        self.assertNotIn("MPI_HOME", entries[0])
        self.assertFalse(self.probes.exists())

    def test_new_cann_script_takes_effect_on_next_source(self):
        first, _, _ = self.fake_cann("first-toolkit")
        second, toolkit, _ = self.fake_cann("second-toolkit")
        self.environment.update(TILEXR_VALIDATION_BUILD_MODE="production",
                                TILEXR_CANN_ENV_SCRIPT=str(first), TEST_NEXT_CANN=str(second))
        result = self.run_shell('source "$1/set_env.sh"\n'
                                'TILEXR_CANN_ENV_SCRIPT="$TEST_NEXT_CANN"\n'
                                'source "$1/set_env.sh"\n'
                                '"$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n')
        entry = self.snapshots(result)[0]
        self.assertEqual(entry["ASCEND_HOME_PATH"], str(toolkit))
        self.assertLess(entry["PATH"].split(":").index(str(toolkit / "bin")),
                        entry["PATH"].split(":").index(str(self.root / "first-toolkit/bin")))

    def test_environment_load_does_not_require_or_create_hostfile(self):
        self.hostfile.unlink()
        result = self.run_shell('source "$1/set_env.sh"\n')
        self.assertEqual(result.returncode, 0)
        self.assertFalse(self.hostfile.exists())

    def test_shell_configuration_failures_have_explicit_category(self):
        failing_cann = self.root / "failing-cann.sh"
        self.executable(failing_cann, "return 7\n")
        cases = ({"TILEXR_VALIDATION_BUILD_MODE": "wrong"},
                 {"TILEXR_VALIDATION_BUILD_MODE": "production", "TILEXR_CANN_ENV_SCRIPT": str(self.root / "missing")},
                 {"TILEXR_VALIDATION_BUILD_MODE": "production", "TILEXR_CANN_ENV_SCRIPT": str(failing_cann)},
                 {"TILEXR_VALIDATION_PYTHON": "missing-tilexr-test-python"})
        for overrides in cases:
            with self.subTest(overrides=overrides):
                environment = dict(self.environment, **overrides)
                result = self.run_shell('source "$1/set_env.sh"\n', environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("type=CONFIG_FAILED", result.stderr)

    def test_direct_common_environment_retains_hardware_and_tool_setup(self):
        cann, toolkit, _ = self.fake_cann()
        environment = dict(self.environment, TILEXR_CANN_ENV_SCRIPT=str(cann))
        result = self.run_shell('source "$1/scripts/common_env.sh"\n'
                                '"$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n', environment)
        entry = self.snapshots(result)[0]
        self.assertEqual(len(self.probes.read_text().splitlines()), 3)
        self.assertEqual(entry["TILEXR_SOC_NAME"], "ascend950")
        self.assertEqual(entry["TILEXR_ASCEND_DEV_NUM"], "1")
        runtime_root = Path(entry["TILEXR_HOME"])
        self.assertTrue(self.root.samefile(runtime_root))
        self.assertIn(str(runtime_root / "env/util/mpich/bin"), entry["PATH"])
        self.assertIn(str(runtime_root / "env/util/mpich/lib"), entry["LD_LIBRARY_PATH"])
        self.assertEqual(entry["ASCEND_HOME_PATH"], str(toolkit))

    def test_driver_shim_keeps_real_driver_after_repeated_source(self):
        probe_target = self.root / "symlink-target"
        probe_target.mkdir()
        probe_link = self.root / "symlink-probe"
        try:
            probe_link.symlink_to(probe_target, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"当前平台不能创建 symlink: {error}")
        if not probe_link.is_symlink():
            self.skipTest("当前平台以复制模拟 symlink，不能验证真实 driver shim 链接")
        cann, toolkit, driver = self.fake_cann(real_driver_headers=False, configure_driver=False)
        arch = subprocess.check_output(["uname", "-m"], text=True).strip()
        if arch == "arm64":
            arch = "aarch64"
        (toolkit / f"{arch}-linux/include/driver").mkdir(parents=True)
        self.environment.update(TILEXR_VALIDATION_BUILD_MODE="production", TILEXR_CANN_ENV_SCRIPT=str(cann),
                                ASCEND_DRIVER_PATH=str(driver))
        result = self.run_shell('source "$1/set_env.sh"\n'
                                'source "$1/set_env.sh"\n'
                                '"$TILEXR_VALIDATION_PYTHON" "$1/snapshot.py"\n')
        entry = self.snapshots(result)[0]
        shim = self.root / "env/driver-shim"
        self.assertEqual(entry["ASCEND_DRIVER_PATH"], str(shim))
        self.assertEqual((shim / "lib64/driver").resolve(), (driver / "lib64/driver").resolve())


if __name__ == "__main__":
    unittest.main(verbosity=2)
