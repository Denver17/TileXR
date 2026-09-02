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
MEMORY_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_memory_program.h"
MEMORY_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_memory_program.cpp"
MICROCODE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


PRIVATE_CCU_PRODUCER_NEEDLES = [
    "#include <hcomm/",
    "#include <hccl/",
    "libhcomm",
    "libhccl_v2",
    "libhccl_fwk",
    "libmc2_client",
    "HcclCcuKernel",
    "HcclGetCcuTaskInfo",
    "HcomGetCcuTaskInfo",
    "HcclChannelAcquire",
    "HcclGetChannelForCcu",
    "CcuResBatchAllocator",
    "CcuResRepository",
    "CcuDeviceManager",
    "CcuDevMgrImp",
    "CcuRepContext",
    "CcuKernelMgr",
    "CtxMgrImp",
    "GeneTaskParam",
    "GetMissionKey",
    "SetMissionId",
    "SetMissionKey",
    "SetInstrId",
    "SetCcuInstrInfo",
    "LoadInstruction",
    "AllocIns",
    "AllocCke",
    "AllocXn",
    "RT_RES_TYPE_CCU_CKE",
    "RT_RES_TYPE_CCU_XN",
    "rtCCULaunch",
    "dlopen",
    "dlsym",
]


class TileXRCcuMemoryProgramTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers memory program C++ syntax")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "memory_program_test.cpp"
            test_bin = temp_path / "memory_program_test"
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
                    str(MEMORY_SOURCE),
                    str(MICROCODE_SOURCE),
                    "-o",
                    str(test_bin),
                ],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            return subprocess.run([str(test_bin)], cwd=REPO_ROOT, check=False, text=True, capture_output=True)

    def test_memory_program_builds_hcomm_style_loc_to_rmt_copy_with_completion_wait(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_memory_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                if (TileXRCcuPackMemoryToken(0x12345, 0x89abcdefU, true) !=
                    0x0011234589abcdefULL) {
                    std::cerr << "packed token mismatch\n";
                    return 1;
                }

                TileXRCcuMemoryCopySpec spec;
                spec.direction = TileXRCcuMemoryCopyDirection::LocalToRemote;
                spec.localGsa = 0x101;
                spec.localXn = 0x102;
                spec.remoteGsa = 0x201;
                spec.remoteXn = 0x202;
                spec.lengthXn = 0x301;
                spec.localAddr = 0x1122334455667788ULL;
                spec.localToken = TileXRCcuPackMemoryToken(0x12345, 0x89abcdefU, true);
                spec.remoteAddr = 0x8877665544332211ULL;
                spec.remoteToken = TileXRCcuPackMemoryToken(0x23456, 0x76543210U, true);
                spec.lengthBytes = 0x80;
                spec.channelId = 0x12;
                spec.completionCke = 0x401;
                spec.completionMask = 1;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuMemoryProgramReport report;
                if (TileXRCcuBuildMemoryCopyProgram(spec, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "memory copy program build failed: " << report.message << "\n";
                    return 2;
                }
                if (program.size() != 7 || report.loadInstructionCount != 5 ||
                    report.transferInstructionCount != 1 || report.waitInstructionCount != 1 ||
                    report.totalInstructionCount != 7 || report.message != "ok") {
                    std::cerr << "unexpected memory program report\n";
                    return 3;
                }
                if (program[0].words[0] != 0x5566778801010002ULL ||
                    program[0].words[1] != 0x0000000011223344ULL ||
                    program[1].words[0] != 0x89abcdef01020003ULL ||
                    program[1].words[1] != 0x0000000100112345ULL ||
                    program[2].words[0] != 0x4433221102010002ULL ||
                    program[2].words[1] != 0x0000000088776655ULL ||
                    program[3].words[0] != 0x7654321002020003ULL ||
                    program[3].words[1] != 0x0000000100123456ULL ||
                    program[4].words[0] != 0x0000008003010003ULL ||
                    program[4].words[1] != 0) {
                    std::cerr << "unexpected load immediate program\n";
                    return 4;
                }
                if (program[5].words[0] != 0x0101020202011009ULL ||
                    program[5].words[1] != 0x0000001203010102ULL ||
                    program[5].words[2] != 0x0003000000000000ULL ||
                    program[5].words[3] != 0x0000000000010401ULL) {
                    std::cerr << "unexpected loc->rmt transfer instruction\n";
                    return 5;
                }
                if (program[6].words[0] != 0x0000000000010804ULL ||
                    program[6].words[1] != 0x0000000000010401ULL ||
                    program[6].words[2] != 0 ||
                    program[6].words[3] != 0) {
                    std::cerr << "unexpected completion wait instruction\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_memory_program_builds_hcomm_style_rmt_to_loc_copy(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_memory_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuMemoryCopySpec spec;
                spec.direction = TileXRCcuMemoryCopyDirection::RemoteToLocal;
                spec.localGsa = 0x101;
                spec.localXn = 0x102;
                spec.remoteGsa = 0x201;
                spec.remoteXn = 0x202;
                spec.lengthXn = 0x301;
                spec.localAddr = 0x1122334455667788ULL;
                spec.localToken = TileXRCcuPackMemoryToken(0x12345, 0x89abcdefU, true);
                spec.remoteAddr = 0x8877665544332211ULL;
                spec.remoteToken = TileXRCcuPackMemoryToken(0x23456, 0x76543210U, true);
                spec.lengthBytes = 0x80;
                spec.channelId = 0x12;
                spec.completionCke = 0x401;
                spec.completionMask = 1;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuMemoryProgramReport report;
                if (TileXRCcuBuildMemoryCopyProgram(spec, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "memory copy program build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 7 ||
                    program[5].words[0] != 0x0201010201011008ULL ||
                    program[5].words[1] != 0x0000001203010202ULL ||
                    program[5].words[2] != 0x0003000000000000ULL ||
                    program[5].words[3] != 0x0000000000010401ULL) {
                    std::cerr << "unexpected rmt->loc transfer instruction\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_memory_program_rejects_missing_required_fields(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_memory_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                std::vector<TileXRCcuInstr> program(1);
                TileXRCcuMemoryProgramReport report;
                TileXRCcuMemoryCopySpec empty;
                if (TileXRCcuBuildMemoryCopyProgram(empty, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty memory copy spec accepted\n";
                    return 1;
                }
                if (!program.empty() || report.message.find("missing") == std::string::npos) {
                    std::cerr << "weak empty spec diagnostic: " << report.message << "\n";
                    return 2;
                }

                TileXRCcuMemoryCopySpec spec;
                spec.direction = TileXRCcuMemoryCopyDirection::LocalToRemote;
                spec.localGsa = 1;
                spec.localXn = 2;
                spec.remoteGsa = 3;
                spec.remoteXn = 4;
                spec.lengthXn = 5;
                spec.localAddr = 0x1000;
                spec.localToken = TileXRCcuPackMemoryToken(1, 2, true);
                spec.remoteAddr = 0x2000;
                spec.remoteToken = TileXRCcuPackMemoryToken(3, 4, true);
                spec.lengthBytes = 128;
                spec.channelId = 6;
                spec.completionCke = 7;
                spec.completionMask = 1;

                if (TileXRCcuBuildMemoryCopyProgram(spec, nullptr, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null output program accepted\n";
                    return 3;
                }

                spec.lengthBytes = 0;
                if (TileXRCcuBuildMemoryCopyProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("length") == std::string::npos) {
                    std::cerr << "zero length accepted or weak diagnostic: " << report.message << "\n";
                    return 4;
                }

                spec.lengthBytes = 128;
                spec.direction = static_cast<TileXRCcuMemoryCopyDirection>(99);
                if (TileXRCcuBuildMemoryCopyProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL ||
                    !program.empty() || report.message.find("direction") == std::string::npos) {
                    std::cerr << "invalid direction accepted or weak diagnostic: " << report.message << "\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_memory_program_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = MEMORY_HEADER.read_text(encoding="utf-8")
        source = MEMORY_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_memory_program.h", cmake)
        self.assertIn("ccu/tilexr_ccu_memory_program.cpp", cmake)
        self.assertIn("enum class TileXRCcuMemoryCopyDirection", header)
        self.assertIn("struct TileXRCcuMemoryCopySpec", header)
        self.assertIn("struct TileXRCcuMemoryProgramReport", header)
        self.assertIn("TileXRCcuPackMemoryToken", header)
        self.assertIn("TileXRCcuBuildMemoryCopyProgram", header)
        self.assertIn("TileXRCcuEncodeLoadImdToGsa", source)
        self.assertIn("TileXRCcuEncodeLoadImdToXn", source)
        self.assertIn("TileXRCcuEncodeTransRmtMemToLocMem", source)
        self.assertIn("TileXRCcuEncodeTransLocMemToRmtMem", source)
        self.assertIn("TileXRCcuEncodeClearCke", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
