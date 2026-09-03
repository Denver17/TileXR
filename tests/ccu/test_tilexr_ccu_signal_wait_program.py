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
SIGNAL_WAIT_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_signal_wait_program.h"
SIGNAL_WAIT_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_signal_wait_program.cpp"
BARRIER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.cpp"
MICROCODE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuSignalWaitProgramTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers signal/wait C++ syntax")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "signal_wait_program_test.cpp"
            test_bin = temp_path / "signal_wait_program_test"
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
                    str(SIGNAL_WAIT_SOURCE),
                    str(BARRIER_SOURCE),
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

    def test_signal_wait_program_initializes_source_cke_before_signal_only_post(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_signal_wait_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuSignalWaitProgramSpec spec;
                spec.role = TileXRCcuSignalWaitProgramRole::Signal;
                spec.channelId = 2;
                spec.remoteXn = 2361;
                spec.localXn = 1961;
                spec.remoteNotifyCke = 364;
                spec.remoteNotifyMask = 1;
                spec.sourceCke = 0x101;
                spec.sourceCkeMask = 0xffff;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildSignalWaitProgram(spec, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "signal program build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 5 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 0 || report.totalInstructionCount != 5) {
                    std::cerr << "unexpected signal report\n";
                    return 2;
                }
                if (program[2].words[0] != 0xffff010100010802ULL ||
                    program[3].words[0] != 0xffff0101016c100bULL ||
                    program[3].words[1] != 0x0000000000000002ULL ||
                    program[3].words[2] != 0x0001000000000000ULL) {
                    std::cerr << "unexpected signal instructions\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_signal_wait_program_builds_wait_only_clear_cke_wait(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_signal_wait_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuSignalWaitProgramSpec spec;
                spec.role = TileXRCcuSignalWaitProgramRole::Wait;
                spec.localXn = 0x7a9;
                spec.localWaitCke = 0x220;
                spec.localWaitMask = 1;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildSignalWaitProgram(spec, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "wait program build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 1 || report.postInstructionCount != 0 ||
                    report.waitInstructionCount != 1 || report.totalInstructionCount != 1) {
                    std::cerr << "unexpected wait report\n";
                    return 2;
                }
                if (program[0].words[0] != 0x0000000000010804ULL ||
                    program[0].words[1] != 0x0000000000010220ULL) {
                    std::cerr << "unexpected wait instruction\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_signal_wait_program_builds_signal_and_wait_for_barrier(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_signal_wait_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuSignalWaitProgramSpec spec;
                spec.role = TileXRCcuSignalWaitProgramRole::SignalAndWait;
                spec.channelId = 2;
                spec.remoteXn = 2361;
                spec.localXn = 1961;
                spec.remoteNotifyCke = 364;
                spec.remoteNotifyMask = 1;
                spec.localWaitCke = 0x220;
                spec.localWaitMask = 1;
                spec.sourceCke = 0x101;
                spec.sourceCkeMask = 0xffff;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildSignalWaitProgram(spec, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "barrier program build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 6 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 1 || report.totalInstructionCount != 6) {
                    std::cerr << "unexpected signal_and_wait report\n";
                    return 2;
                }
                if (program[2].words[0] != 0xffff010100010802ULL ||
                    program[3].words[0] != 0xffff0101016c100bULL ||
                    program[4].words[0] != 0x0000000000010804ULL ||
                    program[4].words[1] != 0x0000000000010220ULL) {
                    std::cerr << "unexpected signal_and_wait instructions\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_signal_wait_program_rejects_missing_role_resources(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_signal_wait_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                std::vector<TileXRCcuInstr> program(1);
                TileXRCcuBarrierProgramReport report;
                TileXRCcuSignalWaitProgramSpec spec;
                spec.role = TileXRCcuSignalWaitProgramRole::Signal;
                if (TileXRCcuBuildSignalWaitProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing signal resources accepted\n";
                    return 1;
                }
                if (!program.empty() || report.message.find("signal") == std::string::npos) {
                    std::cerr << "weak signal diagnostic: " << report.message << "\n";
                    return 2;
                }
                spec.role = TileXRCcuSignalWaitProgramRole::Wait;
                if (TileXRCcuBuildSignalWaitProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing wait resources accepted\n";
                    return 3;
                }
                if (!program.empty() || report.message.find("wait") == std::string::npos) {
                    std::cerr << "weak wait diagnostic: " << report.message << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_signal_wait_program_is_wired(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = SIGNAL_WAIT_HEADER.read_text(encoding="utf-8")
        source = SIGNAL_WAIT_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_signal_wait_program.h", cmake)
        self.assertIn("ccu/tilexr_ccu_signal_wait_program.cpp", cmake)
        self.assertIn("enum class TileXRCcuSignalWaitProgramRole", header)
        self.assertIn("struct TileXRCcuSignalWaitProgramSpec", header)
        self.assertIn("TileXRCcuBuildSignalWaitProgram", header)
        self.assertIn("TileXRCcuEncodeLoadImdToXn", source)
        self.assertIn("TileXRCcuEncodeSetCke", source)
        self.assertIn("TileXRCcuEncodeSyncCke", source)
        self.assertIn("TileXRCcuEncodeClearCke", source)


if __name__ == "__main__":
    unittest.main()
