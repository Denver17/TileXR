#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import subprocess
import tempfile
import textwrap
import unittest
import shutil
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
CCU_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_producer_plan.h"
CCU_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_producer_plan.cpp"
BARRIER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.cpp"
MICROCODE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.cpp"
RUNTIME_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime.h"
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
    "dlopen",
    "dlsym",
]


class TileXRCcuProducerPlanTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers producer plan C++ syntax")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "producer_plan_test.cpp"
            test_bin = temp_path / "producer_plan_test"
            test_cpp.write_text(code, encoding="utf-8")
            compile_cmd = [
                compiler,
                "-std=c++14",
                "-I",
                str(INCLUDE_DIR),
                "-I",
                str(COMM_DIR),
                str(test_cpp),
                str(CCU_SOURCE),
                str(BARRIER_SOURCE),
                str(MICROCODE_SOURCE),
                "-o",
                str(test_bin),
            ]
            subprocess.run(compile_cmd, cwd=REPO_ROOT, check=True, text=True, capture_output=True)
            return subprocess.run([str(test_bin)], cwd=REPO_ROOT, check=False, text=True, capture_output=True)

    def test_plan_validates_evidence_and_generates_launch_tasks(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_producer_plan.h"

            #include <cstdint>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalGsa = {1, 510, 20};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.syncResources.push_back({1, 1962, 2362, 364, 3, 3});
                plan.syncResources.push_back({1, 1963, 2364, 364, 4, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {0x100051152e00ULL, 0x100051152e00ULL, 0x0010017f86b7d29aULL}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuProducerPlanReport report;
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_SUCCESS) {
                    std::cerr << "valid plan rejected: " << report.message << "\n";
                    return 1;
                }
                if (report.syncResourceCount != 3 || report.taskCount != 2 || report.instructionCount != 170) {
                    std::cerr << "unexpected report counts\n";
                    return 2;
                }

                std::vector<TileXRCcuTask> tasks;
                if (TileXRCcuBuildTasks(plan, &tasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "task build failed: " << report.message << "\n";
                    return 3;
                }
                if (tasks.size() != 2) {
                    std::cerr << "unexpected task size\n";
                    return 4;
                }
                if (tasks[0].dieId != 1 || tasks[0].missionId != 6 || tasks[0].key != 0x059b0f03U ||
                    tasks[0].instStartId != 489 || tasks[0].instCnt != 13 || tasks[0].argSize != 13 ||
                    tasks[0].args[0] != 0x100051152e00ULL || tasks[0].args[2] != 0x0010017f86b7d29aULL) {
                    std::cerr << "unexpected sqe-load mission task\n";
                    return 5;
                }
                if (tasks[1].dieId != 1 || tasks[1].missionId != 6 || tasks[1].key != 0x059b0f03U ||
                    tasks[1].instStartId != 502 || tasks[1].instCnt != 143 || tasks[1].argSize != 13 ||
                    tasks[1].args[0] != 0 || tasks[1].args[12] != 0) {
                    std::cerr << "unexpected sync mission task\n";
                    return 6;
                }
                if (tasks[0].timeout != TILEXR_CCU_DEFAULT_TASK_TIMEOUT_SEC ||
                    tasks[1].timeout != TILEXR_CCU_DEFAULT_TASK_TIMEOUT_SEC) {
                    std::cerr << "unexpected task timeout " << tasks[0].timeout
                              << " " << tasks[1].timeout << "\n";
                    return 12;
                }
                TileXRCcuProgram program;
                if (TileXRCcuBuildMicrocode(plan, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << report.message << "\n";
                    return 7;
                }
                if (program.sqeLoad.size() != 13 || program.sync.size() != 11) {
                    std::cerr << "unexpected microcode sizes\n";
                    return 8;
                }
                if (program.sqeLoad[0].words[0] != 0x0000000007a90001ULL ||
                    program.sqeLoad[12].words[0] != 0x0000000c07b50001ULL) {
                    std::cerr << "unexpected sqe load microcode\n";
                    return 9;
                }
                if (program.sync[0].words[0] != 0x0000000007a90001ULL ||
                    program.sync[1].words[0] != 0x0000000107aa0001ULL ||
                    program.sync[2].words[0] != 0x0000000007b60003ULL ||
                    program.sync[3].words[0] != 0x0000000001fe0002ULL ||
                    program.sync[4].words[0] != 0x0001016c00000802ULL ||
                    program.sync[4].words[1] != 0) {
                    std::cerr << "unexpected hcomm-style task1 prelude\n";
                    return 10;
                }
                if (program.sync[5].words[0] != 0x000007a90939100dULL ||
                    program.sync[5].words[1] != 0x00000001016c0002ULL ||
                    program.sync[5].words[2] != 0x0001000000000000ULL) {
                    std::cerr << "unexpected first sync microcode after prelude\n";
                    return 10;
                }
                if (program.sync[8].words[0] != 0x0000000000010802ULL ||
                    program.sync[8].words[1] != 0x000000000001016cULL) {
                    std::cerr << "unexpected sync wait microcode\n";
                    return 11;
                }
                plan.taskWindows[0].instCnt = 1;
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("SQE load task") == std::string::npos) {
                    std::cerr << "partial SQE load task accepted: " << report.message << "\n";
                    return 13;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_plan_rejects_missing_producer_state(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_producer_plan.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            int main()
            {
                TileXRCcuProducerPlan plan;
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {}});

                TileXRCcuProducerPlanReport report;
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing mission/key was accepted\n";
                    return 1;
                }
                if (report.message.find("mission") == std::string::npos) {
                    std::cerr << "missing mission/key diagnostic was weak: " << report.message << "\n";
                    return 2;
                }

                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.syncResources[0].bindingCount = 0;
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing channel binding was accepted\n";
                    return 3;
                }
                if (report.message.find("binding") == std::string::npos) {
                    std::cerr << "missing binding diagnostic was weak: " << report.message << "\n";
                    return 4;
                }

                plan.syncResources[0].bindingCount = 1;
                plan.syncResources[0].channelId = 0;
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing channel id was accepted\n";
                    return 5;
                }
                if (report.message.find("channel id") == std::string::npos) {
                    std::cerr << "missing channel id diagnostic was weak: " << report.message << "\n";
                    return 6;
                }

                plan.syncResources[0].channelId = 2;
                plan.syncResources.push_back({1, 1962, 2362, 365, 2, 1});
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "duplicate channel id was accepted\n";
                    return 7;
                }
                if (report.message.find("duplicate channel id") == std::string::npos) {
                    std::cerr << "duplicate channel id diagnostic was weak: " << report.message << "\n";
                    return 8;
                }
                plan.syncResources.pop_back();

                plan.syncResources[0].localXn = 2040;
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "out-of-range local xn was accepted\n";
                    return 9;
                }
                if (report.message.find("local XN") == std::string::npos) {
                    std::cerr << "local xn diagnostic was weak: " << report.message << "\n";
                    return 10;
                }

                plan.syncResources[0].localXn = 1961;
                plan.taskWindows[0].instStartId = 646;
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "out-of-window task was accepted\n";
                    return 11;
                }
                if (report.message.find("instruction") == std::string::npos) {
                    std::cerr << "instruction diagnostic was weak: " << report.message << "\n";
                    return 12;
                }

                plan.taskWindows[0].instStartId = 489;
                plan.kernelLocalCke = {1, 0, 0};
                if (TileXRCcuValidateProducerPlan(plan, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing kernel-local cke repository was accepted\n";
                    return 13;
                }
                if (report.message.find("CKE") == std::string::npos) {
                    std::cerr << "kernel-local cke diagnostic was weak: " << report.message << "\n";
                    return 14;
                }

                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_producer_plan_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = CCU_HEADER.read_text(encoding="utf-8")
        source = CCU_SOURCE.read_text(encoding="utf-8")
        runtime_header = RUNTIME_HEADER.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_producer_plan.h", cmake)
        self.assertIn("ccu/tilexr_ccu_producer_plan.cpp", cmake)
        self.assertIn('#include "ccu/tilexr_ccu_runtime.h"', header)
        self.assertIn("TileXRCcuProducerPlan", header)
        self.assertIn("TileXRCcuValidateProducerPlan", header)
        self.assertIn("TileXRCcuBuildTasks", header)
        self.assertIn("TileXRCcuBuildMicrocode", header)
        self.assertIn("TileXRCcuProgram", header)
        self.assertIn("localWaitCke", header)
        self.assertIn("localWaitMask", header)
        self.assertIn("remoteNotifyMask", header)
        self.assertIn("tilexr_ccu_barrier_program.h", header)
        self.assertIn("TileXRCcuBuildBarrierProgram", source)
        self.assertIn("spec.localWaitCke", source)
        self.assertIn("spec.remoteNotifyCke", source)
        self.assertIn("TileXRCcuTask", runtime_header)
        self.assertIn("kernelLocalCke", source)
        self.assertIn("std::set<uint16_t> channelIds", source)
        self.assertIn("missing channel id for sync resource", source)
        self.assertIn("duplicate channel id for sync resource", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_microcode_can_use_distinct_remote_notify_and_local_wait_cke(self):
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
                plan.kernelLocalCke = {1, 332, 4};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};

                TileXRCcuSyncResource resource;
                resource.dieId = 1;
                resource.localXn = 1961;
                resource.remoteXn = 2361;
                resource.notifyCke = 364;
                resource.channelId = 2;
                resource.bindingCount = 3;
                resource.localWaitCke = 332;
                resource.localWaitMask = 1;
                resource.remoteNotifyMask = 1;
                plan.syncResources.push_back(resource);

                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport report;
                if (TileXRCcuBuildMicrocode(plan, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.sync.size() != 7) {
                    std::cerr << "unexpected sync size\n";
                    return 2;
                }
                if (program.sync[5].words[1] != 0x00000001016c0002ULL) {
                    std::cerr << "post did not use remote notify CKE\n";
                    return 3;
                }
                if (program.sync[6].words[0] != 0x0000000000010802ULL ||
                    program.sync[6].words[1] != 0x000000000001014cULL) {
                    std::cerr << "wait/clear did not use local wait CKE\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_microcode_can_emit_sync_cke_barrier_mode_with_source_cke(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_producer_plan.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuProducerPlan plan;
                plan.barrierMode = TileXRCcuBarrierMode::SyncCke;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 0x220, 2};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};

                TileXRCcuSyncResource resource;
                resource.dieId = 1;
                resource.localXn = 1961;
                resource.remoteXn = 2361;
                resource.notifyCke = 0x330;
                resource.channelId = 2;
                resource.bindingCount = 1;
                resource.localWaitCke = 0x220;
                resource.localWaitMask = 1;
                resource.remoteNotifyMask = 1;
                resource.sourceCke = 0x221;
                resource.sourceCkeMask = 0xffff;
                plan.syncResources.push_back(resource);

                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport report;
                if (TileXRCcuBuildMicrocode(plan, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "sync_cke microcode build failed: " << report.message << "\n";
                    return 1;
                }
                if (program.sync.size() != 3) {
                    std::cerr << "unexpected sync_cke sync size\n";
                    return 2;
                }
                if (program.sync[0].words[0] != 0xffff022100010802ULL ||
                    program.sync[1].words[0] != 0xffff02210330100bULL ||
                    program.sync[1].words[1] != 0x0000000000000002ULL ||
                    program.sync[2].words[0] != 0x0000000000010804ULL ||
                    program.sync[2].words[1] != 0x0000000000010220ULL) {
                    std::cerr << "sync_cke barrier microcode mismatch\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
