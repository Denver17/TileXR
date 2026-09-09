"""Compile the actual empty kernel with CPU API fakes; this does not validate CCE or NPU execution."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).resolve().parent


class KernelProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if compiler is None or os.name != "posix":
            raise unittest.SkipTest("POSIX C++14 compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-kernel-profile-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executables = {}
        for mode, definitions in (
            ("default", []),
            ("disabled", ["-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=0"]),
            ("enabled", ["-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=1"]),
        ):
            executable = Path(cls.temp.name) / mode
            result = subprocess.run([
                compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", *definitions,
                "-I", str(SOURCE / "server_dedup_kernel_profile_fake"),
                "-I", str(ROOT / "src/include"), "-I", str(ROOT / "src/ep/common"),
                "-I", str(ROOT / "src/ep/kernels"),
                str(SOURCE / "server_dedup_kernel_profile_stub.cpp"),
                "-o", str(executable),
            ], capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise RuntimeError(f"{mode} compile failed:\n{result.stdout}{result.stderr}")
            cls.executables[mode] = executable

    def run_mode(self, mode):
        result = subprocess.run([str(self.executables[mode])], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "PROFILE_BEHAVIOR_PASS")

    def test_default_does_not_read_comm_or_write_records(self):
        self.run_mode("default")

    def test_explicitly_disabled_matches_default(self):
        self.run_mode("disabled")

    def test_enabled_final_generation_and_guarded_record_writes(self):
        self.run_mode("enabled")


if __name__ == "__main__":
    unittest.main()
