#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SPECS_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.h"
SPECS_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp"
ALLOCATOR_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_resource_allocator.h"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuSpecsTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "specs_test.cpp"
            test_bin = temp_path / "specs_test"
            test_cpp.write_text(code, encoding="utf-8")
            subprocess.run(
                [
                    compiler,
                    "-std=c++14",
                    "-I",
                    str(INCLUDE_DIR),
                    "-I",
                    str(COMM_DIR),
                    str(test_cpp),
                    str(SPECS_SOURCE),
                    "-o",
                    str(test_bin),
                ],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            return subprocess.run([str(test_bin)], cwd=REPO_ROOT, check=False, text=True, capture_output=True)

    def test_decodes_hcomm_basic_info_caps_into_tilexr_spec(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_specs.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x25;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                TileXRCcuSpecInfo info;
                TileXRCcuSpecsReport report;
                if (TileXRCcuDecodeBasicInfo(basic, &info, &report) != TILEXR_SUCCESS) {
                    std::cerr << "decode failed: " << report.message << "\n";
                    return 1;
                }

                if (info.dieId != 1 || info.msId != 0x25 || info.missionKey != 0x059b0f03U ||
                    info.resourceAddr != 0x100000000ULL) {
                    std::cerr << "base fields mismatch\n";
                    return 2;
                }
                if (info.instructionNum != 170 || info.xnNum != 62 || info.gsaNum != 32 ||
                    info.msNum != 64 || info.ckeNum != 36 || info.jettyNum != 128 ||
                    info.channelNum != 4 || info.pfeNum != 16 ||
                    info.missionNum != 12 || info.loopEngineNum != 8) {
                    std::cerr << "caps decode mismatch\n";
                    return 3;
                }
                if (info.xnBaseAddr != 0x100000000ULL + TILEXR_CCU_V1_XN_RESOURCE_OFFSET) {
                    std::cerr << "xn base mismatch\n";
                    return 4;
                }

                TileXRCcuResourceSpec spec;
                if (TileXRCcuBuildResourceSpec(info, 6, 475, 1961, 332, 2, &spec, &report, 510) != TILEXR_SUCCESS) {
                    std::cerr << "build spec failed: " << report.message << "\n";
                    return 5;
                }
                if (spec.dieId != 1 || spec.missionKey != 0x059b0f03U ||
                    spec.missionStartId != 6 || spec.missionCount != 12 ||
                    spec.instructionStartId != 475 || spec.instructionCount != 170 ||
                    spec.gsaStartId != 510 || spec.gsaCount != 32 ||
                    spec.xnStartId != 1961 || spec.xnCount != 62 ||
                    spec.ckeStartId != 332 || spec.ckeCount != 36 ||
                    spec.channelStartId != 2 || spec.channelCount != 4) {
                    std::cerr << "resource spec mismatch\n";
                    return 6;
                }
                if (report.message != "ok" || report.instructionNum != 170 || report.xnNum != 62) {
                    std::cerr << "report mismatch\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_rejects_invalid_basic_info_and_overflowing_windows(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_specs.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 0;
                basic.missionKey = 0;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = 15U;
                basic.caps.cap1 = 15U;
                basic.caps.cap2 = 15U;
                basic.caps.cap3 = 15U;

                TileXRCcuSpecInfo info;
                TileXRCcuSpecsReport report;
                if (TileXRCcuDecodeBasicInfo(basic, &info, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "zero mission key was accepted\n";
                    return 1;
                }
                if (report.message.find("missing CCU mission key") == std::string::npos) {
                    std::cerr << "weak mission diagnostic: " << report.message << "\n";
                    return 2;
                }

                basic.missionKey = 0x12345678U;
                basic.caps.cap0 = (31U << 16) | 15U;
                if (TileXRCcuDecodeBasicInfo(basic, &info, &report) != TILEXR_SUCCESS) {
                    std::cerr << "decode failed after fixing key: " << report.message << "\n";
                    return 3;
                }

                TileXRCcuResourceSpec spec;
                if (TileXRCcuBuildResourceSpec(info, 65520, 10, 20, 30, 40, &spec, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "overflowing mission window was accepted\n";
                    return 4;
                }
                if (report.message.find("mission resource window overflows") == std::string::npos) {
                    std::cerr << "weak overflow diagnostic: " << report.message << "\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_specs_are_wired_and_do_not_reference_hcomm_runtime_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = SPECS_HEADER.read_text(encoding="utf-8")
        source = SPECS_SOURCE.read_text(encoding="utf-8")
        allocator_header = ALLOCATOR_HEADER.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_specs.h", cmake)
        self.assertIn("ccu/tilexr_ccu_specs.cpp", cmake)
        self.assertIn("tilexr_ccu_resource_allocator.h", header)
        self.assertIn("TileXRCcuBasicInfo", header)
        self.assertIn("TileXRCcuSpecInfo", header)
        self.assertIn("TileXRCcuDecodeBasicInfo", header)
        self.assertIn("TileXRCcuBuildResourceSpec", header)
        self.assertIn("gsaStartId", header)
        self.assertIn("gsaStartId", source)
        self.assertIn("TileXRCcuResourceSpec", allocator_header)

        combined = header + "\n" + source
        for needle in [
            "#include <hcomm/",
            "#include <hccl/",
            "libhcomm",
            "libhccl_v2",
            "libhccl_fwk",
            "libmc2_client",
            "dlopen",
            "dlsym",
            "HcomGetCcuTaskInfo",
            "HcclGetCcuTaskInfo",
            "CcuResBatchAllocator",
            "CcuDevMgrImp",
            "ra_custom_channel",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
