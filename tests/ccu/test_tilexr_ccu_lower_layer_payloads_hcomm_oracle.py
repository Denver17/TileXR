#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ORACLE_SOURCE = REPO_ROOT / "tests" / "ccu" / "ccu_lower_layer_payload_hcomm_oracle.cpp"
PAYLOAD_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_payloads.cpp"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuLowerLayerPayloadsHcommOracleTest(unittest.TestCase):
    def compile_and_run_oracle(self):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_bin = temp_path / "ccu_lower_layer_payload_hcomm_oracle"
            subprocess.run(
                [
                    compiler,
                    "-std=c++14",
                    "-I",
                    str(INCLUDE_DIR),
                    "-I",
                    str(COMM_DIR),
                    str(ORACLE_SOURCE),
                    str(PAYLOAD_SOURCE),
                    "-o",
                    str(test_bin),
                ],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            return subprocess.run([str(test_bin)], cwd=REPO_ROOT, check=False, text=True, capture_output=True)

    def test_hcomm_oracle_source_documents_reference_files(self):
        source = ORACLE_SOURCE.read_text(encoding="utf-8")

        for reference in [
            "ccu_pfe/ccu_pfe_mgr.h",
            "ccu_pfe/ccu_pfe_mgr.cc",
            "ccu_jetty_ctx_mgr.h",
            "ccu_jetty_ctx_mgr.cc",
            "ccu_channel_ctx_v1/ccu_channel_ctx_mgr_v1.h",
            "ccu_channel_ctx_v1/ccu_channel_ctx_mgr_v1.cc",
        ]:
            with self.subTest(reference=reference):
                self.assertIn(reference, source)
        self.assertNotIn("#include <hcomm/", source)
        self.assertNotIn("#include <hccl/", source)

    def test_tilexr_payloads_match_hcomm_packed_struct_oracle(self):
        result = self.compile_and_run_oracle()

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("hcomm lower-layer payload oracle matched", result.stdout)


if __name__ == "__main__":
    unittest.main()
