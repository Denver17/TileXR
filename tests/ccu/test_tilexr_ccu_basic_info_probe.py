#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PROBE_SOURCE = REPO_ROOT / "tests" / "ccu" / "ccu_tilexr_basic_info_probe.cpp"
COMM_DIR = REPO_ROOT / "src" / "comm"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
CCU_DRIVER_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_driver_adapter.cpp"
CCU_DIRECT_RUNTIME_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_direct_runtime.cpp"
CCU_HCCP_LOADER_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_hccp_loader.cpp"
CCU_RA_PROVIDER_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp"
CCU_SPECS_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_specs.cpp"


class TileXRCcuBasicInfoProbeTest(unittest.TestCase):
    def compile_probe(self):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        temp_dir = tempfile.TemporaryDirectory()
        temp_path = Path(temp_dir.name)
        probe_bin = temp_path / "ccu_tilexr_basic_info_probe"
        subprocess.run(
            [
                compiler,
                "-std=c++14",
                "-I",
                str(INCLUDE_DIR),
                "-I",
                str(COMM_DIR),
                str(PROBE_SOURCE),
                str(CCU_DIRECT_RUNTIME_SOURCE),
                str(CCU_DRIVER_SOURCE),
                str(CCU_HCCP_LOADER_SOURCE),
                str(CCU_RA_PROVIDER_SOURCE),
                str(CCU_SPECS_SOURCE),
                "-ldl",
                "-pthread",
                "-o",
                str(probe_bin),
            ],
            cwd=REPO_ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        return temp_dir, probe_bin

    def test_probe_compiles_against_tilexr_owned_ccu_ra_chain(self):
        temp_dir, _ = self.compile_probe()
        temp_dir.cleanup()

    def test_probe_uses_tilexr_ra_provider_not_hcomm_runtime(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")

        for needle in [
            "TileXRCcuDirectRuntime",
            "TileXRCcuDirectRuntimeOptions",
            "TileXRCcuDriverAdapter",
            "TileXRCcuDecodeBasicInfo",
            "deviceLogicId",
            "runtime.QueryBasicInfo",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, source)

        for needle in [
            "#include <hcomm/",
            "#include <hccl/",
            "libhcomm",
            "libhccl_v2",
            "HcomGetCcuTaskInfo",
            "HcclGetCcuTaskInfo",
            "CcuResBatchAllocator",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, source)

    def test_probe_prints_direct_runtime_basic_info_result(self):
        source = PROBE_SOURCE.read_text(encoding="utf-8")

        for needle in [
            "tilexr_ccu_basic_info runtime",
            "tilexr_ccu_basic_info result",
            "hdcType=",
            "raInitialized=",
            "driverRet=",
            "opRet=",
            "msId=",
            "tokenId=",
            "tokenValue=",
            "missionKey",
            "resourceAddr",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, source)

    def test_tilexr_ccu_hccp_network_mode_values_match_cann_hccp_common(self):
        defs = (COMM_DIR / "ccu" / "tilexr_ccu_hccp_types.h").read_text(encoding="utf-8")

        for needle in [
            "TILEXR_CCU_NETWORK_OFFLINE = 1",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, defs)

    def test_optional_probe_runtime_when_enabled(self):
        if os.environ.get("TILEXR_RUN_CCU_BASIC_INFO_PROBE") != "1":
            self.skipTest("set TILEXR_RUN_CCU_BASIC_INFO_PROBE=1 to query real CCU basic info")
        temp_dir, probe_bin = self.compile_probe()
        try:
            result = subprocess.run(
                [str(probe_bin), os.environ.get("TILEXR_CCU_PROBE_DEVICE", "0"),
                 os.environ.get("TILEXR_CCU_PROBE_DIE", "0")],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )
        finally:
            temp_dir.cleanup()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("tilexr_ccu_basic_info", result.stdout)
        self.assertIn("missionKey=0x", result.stdout)


if __name__ == "__main__":
    unittest.main()
