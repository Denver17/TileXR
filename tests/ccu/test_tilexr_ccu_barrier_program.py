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
BARRIER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.h"
BARRIER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.cpp"
MICROCODE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.cpp"
PRODUCER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_producer_plan.cpp"
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


class TileXRCcuBarrierProgramTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers barrier C++ syntax")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "barrier_program_test.cpp"
            test_bin = temp_path / "barrier_program_test"
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
                    str(BARRIER_SOURCE),
                    str(MICROCODE_SOURCE),
                    str(PRODUCER_SOURCE),
                    "-o",
                    str(test_bin),
                ],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            return subprocess.run([str(test_bin)], cwd=REPO_ROOT, check=False, text=True, capture_output=True)

    def test_barrier_program_posts_all_remote_xn_then_waits_and_clears_local_cke(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                std::vector<TileXRCcuBarrierSyncSpec> specs;
                specs.push_back({2361, 1961, 2, 364, 1, 332, 1});
                specs.push_back({2362, 1962, 3, 365, 1, 333, 1});

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram(specs, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "barrier build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 4 || report.postInstructionCount != 2 ||
                    report.waitInstructionCount != 2 || report.totalInstructionCount != 4 ||
                    report.message != "ok") {
                    std::cerr << "unexpected barrier report\n";
                    return 2;
                }
                if (program[0].words[0] != 0x000007a90939100dULL ||
                    program[0].words[1] != 0x00000001016c0002ULL ||
                    program[0].words[2] != 0x0001000000000000ULL ||
                    program[0].words[3] != 0) {
                    std::cerr << "unexpected first post instruction\n";
                    return 3;
                }
                if (program[1].words[0] != 0x000007aa093a100dULL ||
                    program[1].words[1] != 0x00000001016d0003ULL ||
                    program[1].words[2] != 0x0001000000000000ULL ||
                    program[1].words[3] != 0) {
                    std::cerr << "unexpected second post instruction\n";
                    return 4;
                }
                if (program[2].words[0] != 0x0000000000010802ULL ||
                    program[2].words[1] != 0x000000000001014cULL ||
                    program[2].words[2] != 0 ||
                    program[2].words[3] != 0) {
                    std::cerr << "unexpected first wait/clear instruction\n";
                    return 5;
                }
                if (program[3].words[0] != 0x0000000000010802ULL ||
                    program[3].words[1] != 0x000000000001014dULL ||
                    program[3].words[2] != 0 ||
                    program[3].words[3] != 0) {
                    std::cerr << "unexpected second wait/clear instruction\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_rejects_incomplete_resource_bindings(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                std::vector<TileXRCcuInstr> program(1);
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram({}, &program, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty barrier specs accepted\n";
                    return 1;
                }
                if (!program.empty() || report.message.find("missing") == std::string::npos) {
                    std::cerr << "empty barrier diagnostic/report mismatch: " << report.message << "\n";
                    return 2;
                }

                std::vector<TileXRCcuBarrierSyncSpec> specs;
                specs.push_back({2361, 1961, 2, 364, 1, 0, 1});
                if (TileXRCcuBuildBarrierProgram(specs, &program, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing local wait CKE accepted\n";
                    return 3;
                }
                if (!program.empty() || report.message.find("local wait CKE") == std::string::npos) {
                    std::cerr << "weak missing local wait diagnostic: " << report.message << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_can_emit_hcomm_like_synccke_post_and_clear_wait(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBarrierSyncSpec spec;
                spec.channelId = 2;
                spec.remoteNotifyCke = 0x330;
                spec.remoteNotifyMask = 1;
                spec.localWaitCke = 0x220;
                spec.localWaitMask = 1;
                spec.sourceCke = 0x221;
                spec.sourceCkeMask = 0xffff;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram(
                        std::vector<TileXRCcuBarrierSyncSpec>{spec},
                        &program,
                        &report,
                        TileXRCcuBarrierMode::SyncCke) != TILEXR_SUCCESS) {
                    std::cerr << "sync_cke barrier build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 3 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 1 || report.totalInstructionCount != 3) {
                    std::cerr << "unexpected sync_cke barrier report\n";
                    return 2;
                }
                if (program[0].words[0] != 0xffff022100010802ULL ||
                    program[0].words[1] != 0 ||
                    program[0].words[2] != 0 ||
                    program[0].words[3] != 0) {
                    std::cerr << "source CKE init mismatch\n";
                    return 3;
                }
                if (program[1].words[0] != 0xffff02210330100bULL ||
                    program[1].words[1] != 0x0000000000000002ULL ||
                    program[1].words[2] != 0x0001000000000000ULL ||
                    program[1].words[3] != 0) {
                    std::cerr << "SyncCKE post mismatch\n";
                    return 4;
                }
                if (program[2].words[0] != 0x0000000000010804ULL ||
                    program[2].words[1] != 0x0000000000010220ULL ||
                    program[2].words[2] != 0 ||
                    program[2].words[3] != 0) {
                    std::cerr << "ClearCKE wait mismatch\n";
                    return 5;
                }

                TileXRCcuBarrierSyncSpec conflicting = spec;
                conflicting.channelId = 3;
                conflicting.remoteNotifyCke = 0x331;
                conflicting.localWaitCke = 0x222;
                conflicting.sourceCkeMask = 1;
                if (TileXRCcuBuildBarrierProgram(
                        std::vector<TileXRCcuBarrierSyncSpec>{spec, conflicting},
                        &program,
                        &report,
                        TileXRCcuBarrierMode::SyncCke) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    !program.empty() || report.message.find("conflicting source CKE masks") == std::string::npos) {
                    std::cerr << "conflicting source CKE masks accepted: " << report.message << "\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_can_emit_synccke_post_and_set_wait(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBarrierSyncSpec spec;
                spec.channelId = 2;
                spec.remoteNotifyCke = 0x330;
                spec.remoteNotifyMask = 1;
                spec.localWaitCke = 0x220;
                spec.localWaitMask = 1;
                spec.sourceCke = 0x221;
                spec.sourceCkeMask = 0xffff;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram(
                        std::vector<TileXRCcuBarrierSyncSpec>{spec},
                        &program,
                        &report,
                        TileXRCcuBarrierMode::SyncCkeSetWait) != TILEXR_SUCCESS) {
                    std::cerr << "sync_cke_set_wait barrier build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 3 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 1 || report.totalInstructionCount != 3) {
                    std::cerr << "unexpected sync_cke_set_wait barrier report\n";
                    return 2;
                }
                if (program[0].words[0] != 0xffff022100010802ULL ||
                    program[0].words[1] != 0 ||
                    program[0].words[2] != 0 ||
                    program[0].words[3] != 0) {
                    std::cerr << "source CKE init mismatch\n";
                    return 3;
                }
                if (program[1].words[0] != 0xffff02210330100bULL ||
                    program[1].words[1] != 0x0000000000000002ULL ||
                    program[1].words[2] != 0x0001000000000000ULL ||
                    program[1].words[3] != 0) {
                    std::cerr << "SyncCKE post mismatch\n";
                    return 4;
                }
                if (program[2].words[0] != 0x0000000000010802ULL ||
                    program[2].words[1] != 0x0000000000010220ULL ||
                    program[2].words[2] != 0 ||
                    program[2].words[3] != 0) {
                    std::cerr << "SetCKE wait mismatch\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_can_emit_synccke_post_only_diagnostic(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBarrierSyncSpec spec;
                spec.channelId = 2;
                spec.remoteNotifyCke = 0x330;
                spec.remoteNotifyMask = 1;
                spec.sourceCke = 0x221;
                spec.sourceCkeMask = 0xffff;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram(
                        std::vector<TileXRCcuBarrierSyncSpec>{spec},
                        &program,
                        &report,
                        TileXRCcuBarrierMode::SyncCkePostOnly) != TILEXR_SUCCESS) {
                    std::cerr << "sync_cke_post_only barrier build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 2 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 0 || report.totalInstructionCount != 2) {
                    std::cerr << "unexpected sync_cke_post_only barrier report\n";
                    return 2;
                }
                if (program[0].words[0] != 0xffff022100010802ULL ||
                    program[0].words[1] != 0 ||
                    program[0].words[2] != 0 ||
                    program[0].words[3] != 0) {
                    std::cerr << "source CKE init mismatch\n";
                    return 3;
                }
                if (program[1].words[0] != 0xffff02210330100bULL ||
                    program[1].words[1] != 0x0000000000000002ULL ||
                    program[1].words[2] != 0x0001000000000000ULL ||
                    program[1].words[3] != 0) {
                    std::cerr << "SyncCKE post-only mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_can_emit_local_cke_completion_diagnostic(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBarrierSyncSpec spec;
                spec.localWaitCke = 0x220;
                spec.localWaitMask = 1;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram(
                        std::vector<TileXRCcuBarrierSyncSpec>{spec},
                        &program,
                        &report,
                        TileXRCcuBarrierMode::LocalCke) != TILEXR_SUCCESS) {
                    std::cerr << "local CKE diagnostic build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 2 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 1 || report.totalInstructionCount != 2) {
                    std::cerr << "unexpected local CKE diagnostic report\n";
                    return 2;
                }
                if (program[0].words[0] != 0x0001022000000802ULL ||
                    program[0].words[1] != 0 ||
                    program[0].words[2] != 0 ||
                    program[0].words[3] != 0) {
                    std::cerr << "local CKE set mismatch\n";
                    return 3;
                }
                if (program[1].words[0] != 0x0000000000010804ULL ||
                    program[1].words[1] != 0x0000000000010220ULL ||
                    program[1].words[2] != 0 ||
                    program[1].words[3] != 0) {
                    std::cerr << "local CKE clear/wait mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_can_emit_local_cke_post_only_diagnostic(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBarrierSyncSpec spec;
                spec.localWaitCke = 0x220;
                spec.localWaitMask = 1;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram(
                        std::vector<TileXRCcuBarrierSyncSpec>{spec},
                        &program,
                        &report,
                        TileXRCcuBarrierMode::LocalCkePostOnly) != TILEXR_SUCCESS) {
                    std::cerr << "local CKE post-only diagnostic build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 1 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 0 || report.totalInstructionCount != 1) {
                    std::cerr << "unexpected local CKE post-only diagnostic report\n";
                    return 2;
                }
                if (program[0].words[0] != 0x0001022000000802ULL ||
                    program[0].words[1] != 0 ||
                    program[0].words[2] != 0 ||
                    program[0].words[3] != 0) {
                    std::cerr << "local CKE post-only set mismatch\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_can_load_local_xn_before_sync_xn_post_only_diagnostic(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_barrier_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuBarrierSyncSpec spec;
                spec.remoteXn = 0x240;
                spec.localXn = 0x120;
                spec.channelId = 2;
                spec.remoteNotifyCke = 0x330;
                spec.remoteNotifyMask = 1;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuBarrierProgramReport report;
                if (TileXRCcuBuildBarrierProgram(
                        std::vector<TileXRCcuBarrierSyncSpec>{spec},
                        &program,
                        &report,
                        TileXRCcuBarrierMode::SyncXnLoadPostOnly) != TILEXR_SUCCESS) {
                    std::cerr << "sync_xn_load_post_only diagnostic build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.size() != 2 || report.postInstructionCount != 1 ||
                    report.waitInstructionCount != 0 || report.totalInstructionCount != 2) {
                    std::cerr << "unexpected sync_xn_load_post_only report\n";
                    return 2;
                }
                if (program[0].words[0] != 0x0000000101200003ULL ||
                    program[0].words[1] != 0 ||
                    program[0].words[2] != 0 ||
                    program[0].words[3] != 0) {
                    std::cerr << "local XN load immediate mismatch\n";
                    return 3;
                }
                if (program[1].words[0] != 0x000001200240100dULL ||
                    program[1].words[1] != 0x0000000103300002ULL ||
                    program[1].words[2] != 0x0001000000000000ULL ||
                    program[1].words[3] != 0) {
                    std::cerr << "SyncXn post mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_producer_microcode_uses_barrier_post_and_wait_program(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_producer_plan.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 3};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 332, 2, 3});
                plan.syncResources.push_back({1, 1962, 2362, 333, 3, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport report;
                if (TileXRCcuBuildMicrocode(plan, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.sqeLoad.size() != 13 || program.sync.size() != 9) {
                    std::cerr << "unexpected producer barrier microcode size\n";
                    return 2;
                }
                if (program.sync[5].words[0] != 0x000007a90939100dULL ||
                    program.sync[5].words[1] != 0x00000001014c0002ULL ||
                    program.sync[7].words[0] != 0x0000000000010802ULL ||
                    program.sync[7].words[1] != 0x000000000001014cULL) {
                    std::cerr << "unexpected first producer barrier pair\n";
                    return 3;
                }
                if (program.sync[6].words[0] != 0x000007aa093a100dULL ||
                    program.sync[6].words[1] != 0x00000001014d0003ULL ||
                    program.sync[8].words[0] != 0x0000000000010802ULL ||
                    program.sync[8].words[1] != 0x000000000001014dULL) {
                    std::cerr << "unexpected second producer barrier pair\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_barrier_program_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = BARRIER_HEADER.read_text(encoding="utf-8")
        source = BARRIER_SOURCE.read_text(encoding="utf-8")
        producer = PRODUCER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_barrier_program.h", cmake)
        self.assertIn("ccu/tilexr_ccu_barrier_program.cpp", cmake)
        self.assertIn("struct TileXRCcuBarrierSyncSpec", header)
        self.assertIn("enum class TileXRCcuBarrierMode", header)
        self.assertIn("LocalCke", header)
        self.assertIn("LocalCkePostOnly", header)
        self.assertIn("SyncXnPostOnly", header)
        self.assertIn("SyncXnLoadPostOnly", header)
        self.assertIn("SyncCkePostOnly", header)
        self.assertIn("SyncCkeSetWait", header)
        self.assertIn("struct TileXRCcuBarrierProgramReport", header)
        self.assertIn("TileXRCcuBuildBarrierProgram", header)
        self.assertIn("TileXRCcuBarrierMode::LocalCke", source)
        self.assertIn("TileXRCcuBarrierMode::LocalCkePostOnly", source)
        self.assertIn("TileXRCcuBarrierMode::SyncXnPostOnly", source)
        self.assertIn("TileXRCcuBarrierMode::SyncXnLoadPostOnly", source)
        self.assertIn("TileXRCcuBarrierMode::SyncCkePostOnly", source)
        self.assertIn("TileXRCcuBarrierMode::SyncCkeSetWait", source)
        self.assertIn("TileXRCcuEncodeLoadImdToXn", source)
        self.assertIn("TileXRCcuEncodeSyncXn", source)
        self.assertIn("TileXRCcuEncodeSyncCke", source)
        self.assertIn("TileXRCcuEncodeSetCke", source)
        self.assertIn("TileXRCcuEncodeClearCke", source)
        self.assertIn("TileXRCcuBuildBarrierProgram", producer)

        combined = header + "\n" + source + "\n" + producer
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
