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
ALLOCATOR_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_resource_allocator.h"
ALLOCATOR_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_resource_allocator.cpp"
PRODUCER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_producer_plan.cpp"
BARRIER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.cpp"
MICROCODE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuResourceAllocatorTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "allocator_test.cpp"
            test_bin = temp_path / "allocator_test"
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
                    str(ALLOCATOR_SOURCE),
                    str(PRODUCER_SOURCE),
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

    def test_allocator_declares_and_uses_separate_mission_instruction_start(self):
        header = ALLOCATOR_HEADER.read_text(encoding="utf-8")
        source = ALLOCATOR_SOURCE.read_text(encoding="utf-8")

        self.assertIn("uint16_t missionInstructionStartId = 0;", header)
        self.assertIn("missionInstructionStartId", source)
        self.assertIn("missionInstructionStart", source)
        self.assertIn("repositoryPrefixCount", source)
        self.assertIn("result.repository.num", source)
        self.assertIn("result.repository.startId", source)
        self.assertIn("generated.instructionWindow = {", source)

    def test_allocator_builds_complete_tilexr_owned_producer_plan(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.instructionCount = 170;
                spec.xnStartId = 1961;
                spec.xnCount = 62;
                spec.gsaStartId = 510;
                spec.gsaCount = 20;
                spec.ckeStartId = 332;
                spec.ckeCount = 32;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 3;
                request.syncInstructionCount = 143;
                request.bindingsPerSyncResource = 3;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }

                if (!plan.mission.installed || plan.mission.dieId != 1 || plan.mission.missionId != 6 ||
                    plan.mission.key != 0x059b0f03U) {
                    std::cerr << "mission/key mismatch\n";
                    return 3;
                }
                if (plan.kernelLocalMission.startId != 6 || plan.kernelLocalMission.num != 1 ||
                    plan.kernelLocalXn.startId != 1961 || plan.kernelLocalXn.num != 14 ||
                    plan.kernelLocalGsa.startId != 510 || plan.kernelLocalGsa.num != 1 ||
                    plan.kernelLocalCke.startId != 332 || plan.kernelLocalCke.num != 3) {
                    std::cerr << "kernel local ranges mismatch\n";
                    return 4;
                }
                if (plan.instructionWindow.repositoryStartId != 475 ||
                    plan.instructionWindow.repositoryCount != 156 ||
                    plan.instructionWindow.missionStartId != 475 ||
                    plan.instructionWindow.missionCount != 156) {
                    std::cerr << "instruction window mismatch\n";
                    return 5;
                }
                if (plan.syncResources.size() != 3 || plan.taskWindows.size() != 2) {
                    std::cerr << "resource/task count mismatch\n";
                    return 6;
                }
                if (plan.syncResources[0].localXn != 1961 || plan.syncResources[0].remoteXn != 1975 ||
                    plan.syncResources[0].notifyCke != 332 || plan.syncResources[0].channelId != 2 ||
                    plan.syncResources[0].bindingCount != 3) {
                    std::cerr << "first sync resource mismatch\n";
                    return 7;
                }
                if (plan.syncResources[2].localXn != 1963 || plan.syncResources[2].remoteXn != 1977 ||
                    plan.syncResources[2].notifyCke != 334 || plan.syncResources[2].channelId != 4) {
                    std::cerr << "last sync resource mismatch\n";
                    return 8;
                }
                if (plan.taskWindows[0].instStartId != 475 || plan.taskWindows[0].instCnt != 13 ||
                    plan.taskWindows[0].argSize != 13 ||
                    plan.taskWindows[1].instStartId != 488 || plan.taskWindows[1].instCnt != 143 ||
                    plan.taskWindows[1].argSize != 13) {
                    std::cerr << "task window mismatch\n";
                    return 9;
                }

                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "generated plan invalid: " << planReport.message << "\n";
                    return 10;
                }
                if (allocation.receiptId == 0 || allocation.packageProvider != "tilexr-hcomm-derived-resource-allocator" ||
                    allocation.localXn.startId != 1961 || allocation.remoteXn.startId != 1975 ||
                    allocation.localGsa.startId != 510 || allocation.localGsa.num != 1 ||
                    allocation.notifyCke.startId != 332 || allocation.channels.startId != 2 ||
                    allocation.channels.num != 3) {
                    std::cerr << "allocation receipt mismatch\n";
                    return 11;
                }
                if (report.missionAllocated != 1 || report.localXnAllocated != 14 ||
                    report.localGsaAllocated != 1 ||
                    report.remoteXnAllocated != 3 || report.notifyCkeAllocated != 3 ||
                    report.channelBindingsAllocated != 9 || report.repositoryAllocated != 156 ||
                    report.message != "ok") {
                    std::cerr << "report mismatch\n";
                    return 12;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_can_assign_distinct_local_wait_and_remote_notify_cke_ranges(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.instructionCount = 170;
                spec.xnStartId = 1961;
                spec.xnCount = 62;
                spec.ckeStartId = 332;
                spec.ckeCount = 32;
                spec.localWaitCkeStartId = 332;
                spec.localWaitCkeCount = 8;
                spec.remoteNotifyCkeStartId = 364;
                spec.remoteNotifyCkeCount = 8;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 3;
                request.syncInstructionCount = 143;
                request.bindingsPerSyncResource = 3;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }

                if (allocation.localWaitCke.startId != 332 || allocation.localWaitCke.num != 3 ||
                    allocation.remoteNotifyCke.startId != 364 || allocation.remoteNotifyCke.num != 3 ||
                    allocation.notifyCke.startId != 364 || allocation.notifyCke.num != 3) {
                    std::cerr << "split CKE allocation ranges mismatch\n";
                    return 3;
                }
                if (plan.kernelLocalCke.startId != 332 || plan.kernelLocalCke.num != 3) {
                    std::cerr << "kernel-local CKE should describe the local wait CKE range\n";
                    return 4;
                }
                if (plan.syncResources.size() != 3 ||
                    plan.syncResources[0].localWaitCke != 332 ||
                    plan.syncResources[0].notifyCke != 364 ||
                    plan.syncResources[1].localWaitCke != 333 ||
                    plan.syncResources[1].notifyCke != 365 ||
                    plan.syncResources[2].localWaitCke != 334 ||
                    plan.syncResources[2].notifyCke != 366) {
                    std::cerr << "split CKE sync resources mismatch\n";
                    return 5;
                }
                if (report.localWaitCkeAllocated != 3 ||
                    report.remoteNotifyCkeAllocated != 3 ||
                    report.notifyCkeAllocated != 3) {
                    std::cerr << "split CKE report mismatch\n";
                    return 6;
                }

                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "split CKE generated plan invalid: " << planReport.message << "\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_can_reserve_repository_prefix_before_mission_instruction_window(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.missionInstructionStartId = 489;
                spec.instructionCount = 170;
                spec.xnStartId = 1961;
                spec.xnCount = 62;
                spec.ckeStartId = 332;
                spec.ckeCount = 32;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 3;
                request.syncInstructionCount = 143;
                request.bindingsPerSyncResource = 3;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }

                if (plan.instructionWindow.repositoryStartId != 475 ||
                    plan.instructionWindow.repositoryCount != 170 ||
                    plan.instructionWindow.missionStartId != 489 ||
                    plan.instructionWindow.missionCount != 156) {
                    std::cerr << "repository/mission instruction window mismatch\n";
                    return 3;
                }
                if (plan.taskWindows.size() != 2 ||
                    plan.taskWindows[0].instStartId != 489 ||
                    plan.taskWindows[0].instCnt != 13 ||
                    plan.taskWindows[1].instStartId != 502 ||
                    plan.taskWindows[1].instCnt != 143) {
                    std::cerr << "mission task windows did not start at mission instruction window\n";
                    return 4;
                }
                if (allocation.repository.startId != 475 ||
                    allocation.repository.num != 170 ||
                    report.repositoryAllocated != 170) {
                    std::cerr << "repository allocation/report did not include prefix\n";
                    return 5;
                }

                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "prefixed repository plan invalid: " << planReport.message << "\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_builds_pure_barrier_plan_without_sqe_load_task(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 0;
                spec.missionKey = 0x12345678U;
                spec.missionStartId = 1;
                spec.missionCount = 1;
                spec.instructionStartId = 1;
                spec.instructionCount = 16;
                spec.xnStartId = 1;
                spec.xnCount = 8;
                spec.ckeStartId = 1;
                spec.ckeCount = 8;
                spec.channelStartId = 1;
                spec.channelCount = 2;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = 0;
                request.syncResourceCount = 1;
                request.syncInstructionCount = 2;
                request.bindingsPerSyncResource = 1;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }

                if (plan.taskWindows.size() != 1) {
                    std::cerr << "pure barrier should have one sync task\n";
                    return 3;
                }
                if (plan.taskWindows[0].instStartId != 1 ||
                    plan.taskWindows[0].instCnt != 2 ||
                    plan.taskWindows[0].argSize != TILEXR_CCU_SQE_ARGS_LEN) {
                    std::cerr << "pure barrier sync task mismatch\n";
                    return 4;
                }
                if (plan.kernelLocalXn.startId != 1 || plan.kernelLocalXn.num != 1 ||
                    allocation.localXn.startId != 1 || allocation.localXn.num != 1 ||
                    allocation.remoteXn.startId != 2 || allocation.remoteXn.num != 1 ||
                    allocation.repository.startId != 1 || allocation.repository.num != 2) {
                    std::cerr << "pure barrier allocation mismatch\n";
                    return 5;
                }
                if (report.localXnAllocated != 1 ||
                    report.remoteXnAllocated != 1 ||
                    report.repositoryAllocated != 2) {
                    std::cerr << "pure barrier report mismatch\n";
                    return 6;
                }
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "pure barrier plan invalid: " << planReport.message << "\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_assigns_independent_source_cke_for_sync_cke_barrier_mode(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.instructionCount = 170;
                spec.xnStartId = 1961;
                spec.xnCount = 62;
                spec.ckeStartId = 0x220;
                spec.ckeCount = 16;
                spec.localWaitCkeStartId = 0x220;
                spec.localWaitCkeCount = 8;
                spec.remoteNotifyCkeStartId = 0x330;
                spec.remoteNotifyCkeCount = 8;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 1;
                request.syncInstructionCount = 3;
                request.bindingsPerSyncResource = 1;
                request.barrierMode = TileXRCcuBarrierMode::SyncCke;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }

                if (plan.barrierMode != TileXRCcuBarrierMode::SyncCke ||
                    allocation.localWaitCke.startId != 0x220 || allocation.localWaitCke.num != 1 ||
                    allocation.sourceCke.startId != 0x221 || allocation.sourceCke.num != 1 ||
                    plan.kernelLocalCke.startId != 0x220 || plan.kernelLocalCke.num != 2) {
                    std::cerr << "sync_cke CKE allocation ranges mismatch\n";
                    return 3;
                }
                if (plan.syncResources.size() != 1 ||
                    plan.syncResources[0].notifyCke != 0x330 ||
                    plan.syncResources[0].localWaitCke != 0x220 ||
                    plan.syncResources[0].sourceCke != 0x221 ||
                    plan.syncResources[0].sourceCkeMask != 0xffff) {
                    std::cerr << "sync_cke sync resource CKE fields mismatch\n";
                    return 4;
                }
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "sync_cke generated plan invalid: " << planReport.message << "\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_accepts_synccke_post_only_with_source_cke_and_two_instructions(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.instructionCount = 170;
                spec.xnStartId = 1961;
                spec.xnCount = 62;
                spec.ckeStartId = 0x220;
                spec.ckeCount = 16;
                spec.localWaitCkeStartId = 0x220;
                spec.localWaitCkeCount = 8;
                spec.remoteNotifyCkeStartId = 0x330;
                spec.remoteNotifyCkeCount = 8;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 1;
                request.syncInstructionCount = 2;
                request.bindingsPerSyncResource = 1;
                request.barrierMode = TileXRCcuBarrierMode::SyncCkePostOnly;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }

                if (plan.barrierMode != TileXRCcuBarrierMode::SyncCkePostOnly ||
                    allocation.sourceCke.startId != 0x221 || allocation.sourceCke.num != 1 ||
                    plan.instructionWindow.repositoryCount != TILEXR_CCU_SQE_ARGS_LEN + 2) {
                    std::cerr << "sync_cke_post_only allocation mismatch\n";
                    return 3;
                }
                if (plan.syncResources.size() != 1 ||
                    plan.syncResources[0].sourceCke != 0x221 ||
                    plan.syncResources[0].notifyCke != 0x330 ||
                    plan.syncResources[0].localWaitCke != 0x220) {
                    std::cerr << "sync_cke_post_only resource fields mismatch\n";
                    return 4;
                }
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "sync_cke_post_only generated plan invalid: " << planReport.message << "\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_builds_all_cke_only_modes_without_xn_resources(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 1;
                spec.instructionStartId = 475;
                spec.instructionCount = 16;
                spec.xnStartId = 0;
                spec.xnCount = 0;
                spec.ckeStartId = 0x220;
                spec.ckeCount = 16;
                spec.localWaitCkeStartId = 0x220;
                spec.localWaitCkeCount = 8;
                spec.remoteNotifyCkeStartId = 0x330;
                spec.remoteNotifyCkeCount = 8;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                struct ModeCase {
                    TileXRCcuBarrierMode mode;
                    uint32_t instructionCount;
                };
                const ModeCase cases[] = {
                    {TileXRCcuBarrierMode::SyncCke, 3},
                    {TileXRCcuBarrierMode::SyncCkeSetWait, 3},
                    {TileXRCcuBarrierMode::SyncCkePostOnly, 2},
                    {TileXRCcuBarrierMode::LocalCke, 2},
                    {TileXRCcuBarrierMode::LocalCkePostOnly, 1},
                };

                for (const auto& modeCase : cases) {
                    TileXRCcuResourceRequest request;
                    request.sqeArgCount = 0;
                    request.syncResourceCount = 1;
                    request.syncInstructionCount = modeCase.instructionCount;
                    request.bindingsPerSyncResource = 1;
                    request.barrierMode = modeCase.mode;

                    TileXRCcuResourceAllocator allocator;
                    if (allocator.Init(spec) != TILEXR_SUCCESS) {
                        std::cerr << "allocator init failed\n";
                        return 1;
                    }

                    TileXRCcuProducerPlan plan;
                    TileXRCcuResourceAllocation allocation;
                    TileXRCcuResourceAllocatorReport report;
                    if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                        std::cerr << "CKE-only allocate failed: " << report.message << "\n";
                        return 2;
                    }
                    if (allocation.localXn.num != 0 || allocation.remoteXn.num != 0 ||
                        plan.kernelLocalXn.num != 0) {
                        std::cerr << "CKE-only mode allocated XN resources\n";
                        return 3;
                    }

                    TileXRCcuProducerPlanReport planReport;
                    TileXRCcuProgram program;
                    if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS ||
                        TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
                        std::cerr << "CKE-only plan failed: " << planReport.message << "\n";
                        return 4;
                    }
                    if (!program.sqeLoad.empty() || program.sync.size() != modeCase.instructionCount) {
                        std::cerr << "CKE-only microcode size mismatch\n";
                        return 5;
                    }
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_accepts_local_cke_post_only_with_one_instruction(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.instructionCount = 170;
                spec.xnStartId = 0;
                spec.xnCount = 0;
                spec.ckeStartId = 0x220;
                spec.ckeCount = 16;
                spec.localWaitCkeStartId = 0x220;
                spec.localWaitCkeCount = 8;
                spec.remoteNotifyCkeStartId = 0x330;
                spec.remoteNotifyCkeCount = 8;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = 0;
                request.syncResourceCount = 1;
                request.syncInstructionCount = 1;
                request.bindingsPerSyncResource = 1;
                request.barrierMode = TileXRCcuBarrierMode::LocalCkePostOnly;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }

                if (plan.barrierMode != TileXRCcuBarrierMode::LocalCkePostOnly ||
                    plan.taskWindows.size() != 1 ||
                    plan.taskWindows[0].instCnt != 1 ||
                    allocation.localXn.num != 0 || allocation.remoteXn.num != 0 ||
                    plan.instructionWindow.repositoryCount != 1) {
                    std::cerr << "local_cke_post_only allocation mismatch\n";
                    return 3;
                }
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "local_cke_post_only generated plan invalid: " << planReport.message << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_rejects_resource_exhaustion_and_double_release(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 0;
                spec.missionKey = 0x12345678U;
                spec.missionStartId = 1;
                spec.missionCount = 1;
                spec.instructionStartId = 100;
                spec.instructionCount = 32;
                spec.xnStartId = 200;
                spec.xnCount = 17;
                spec.ckeStartId = 300;
                spec.ckeCount = 2;
                spec.channelStartId = 4;
                spec.channelCount = 1;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 3;
                request.syncInstructionCount = 11;
                request.bindingsPerSyncResource = 1;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                request.sqeArgCount = 1;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("SQE argument count") == std::string::npos) {
                    std::cerr << "partial SQE load request accepted: " << report.message << "\n";
                    return 8;
                }
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.bindingsPerSyncResource = 65536;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("binding count") == std::string::npos) {
                    std::cerr << "oversized binding count accepted: " << report.message << "\n";
                    return 9;
                }
                request.bindingsPerSyncResource = 1;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "resource exhaustion was accepted\n";
                    return 2;
                }
                if (report.message.find("insufficient CKE resources") == std::string::npos) {
                    std::cerr << "weak exhaustion diagnostic: " << report.message << "\n";
                    return 3;
                }

                spec.ckeCount = 8;
                spec.channelCount = 3;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "reinit failed\n";
                    return 4;
                }
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS) {
                    std::cerr << "second allocate failed: " << report.message << "\n";
                    return 5;
                }
                const TileXRCcuResourceAllocation released = allocation;
                if (allocator.Release(allocation.receiptId) != TILEXR_SUCCESS) {
                    std::cerr << "release failed\n";
                    return 6;
                }
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_SUCCESS ||
                    allocation.mission.startId != released.mission.startId ||
                    allocation.repository.startId != released.repository.startId ||
                    allocation.localXn.startId != released.localXn.startId ||
                    allocation.remoteXn.startId != released.remoteXn.startId ||
                    allocation.localWaitCke.startId != released.localWaitCke.startId ||
                    allocation.remoteNotifyCke.startId != released.remoteNotifyCke.startId ||
                    allocation.channels.startId != released.channels.startId) {
                    std::cerr << "released resources were not reused: " << report.message << "\n";
                    return 7;
                }
                if (allocator.Release(released.receiptId) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "double release was accepted\n";
                    return 10;
                }
                if (allocator.Release(allocation.receiptId) != TILEXR_SUCCESS) {
                    std::cerr << "replacement release failed\n";
                    return 11;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_rejects_non_lifo_release(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 0;
                spec.missionKey = 0x12345678U;
                spec.missionStartId = 10;
                spec.missionCount = 2;
                spec.instructionStartId = 100;
                spec.instructionCount = 8;
                spec.xnStartId = 200;
                spec.xnCount = 4;
                spec.ckeStartId = 300;
                spec.ckeCount = 4;
                spec.channelStartId = 20;
                spec.channelCount = 2;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = 0;
                request.syncResourceCount = 1;
                request.syncInstructionCount = 2;
                request.bindingsPerSyncResource = 1;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation first;
                TileXRCcuResourceAllocation second;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &first, &report) != TILEXR_SUCCESS ||
                    allocator.Allocate(request, &plan, &second, &report) != TILEXR_SUCCESS) {
                    std::cerr << "allocate failed: " << report.message << "\n";
                    return 2;
                }
                if (allocator.Release(first.receiptId) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "non-LIFO release was accepted\n";
                    return 3;
                }
                if (allocator.Release(second.receiptId) != TILEXR_SUCCESS ||
                    allocator.Release(first.receiptId) != TILEXR_SUCCESS) {
                    std::cerr << "tail releases failed\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_rejects_sync_instruction_window_too_small_for_barrier_program(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.instructionCount = 170;
                spec.xnStartId = 1961;
                spec.xnCount = 62;
                spec.ckeStartId = 332;
                spec.ckeCount = 32;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 2;
                request.syncInstructionCount = 3;
                request.bindingsPerSyncResource = 1;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "undersized barrier sync instruction window was accepted\n";
                    return 2;
                }
                if (report.message.find("barrier") == std::string::npos ||
                    report.message.find("sync instruction") == std::string::npos) {
                    std::cerr << "weak barrier instruction diagnostic: " << report.message << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_counts_hcomm_style_task1_prelude_for_two_task_sync_xn_programs(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_resource_allocator.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuResourceSpec spec;
                spec.dieId = 1;
                spec.missionKey = 0x059b0f03U;
                spec.missionStartId = 6;
                spec.missionCount = 2;
                spec.instructionStartId = 475;
                spec.instructionCount = 170;
                spec.xnStartId = 1961;
                spec.xnCount = 62;
                spec.ckeStartId = 332;
                spec.ckeCount = 32;
                spec.channelStartId = 2;
                spec.channelCount = 4;

                TileXRCcuResourceRequest request;
                request.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                request.syncResourceCount = 2;
                request.syncInstructionCount = 2;
                request.bindingsPerSyncResource = 1;
                request.barrierMode = TileXRCcuBarrierMode::SyncXnPostOnly;

                TileXRCcuResourceAllocator allocator;
                if (allocator.Init(spec) != TILEXR_SUCCESS) {
                    std::cerr << "allocator init failed\n";
                    return 1;
                }

                TileXRCcuProducerPlan plan;
                TileXRCcuResourceAllocation allocation;
                TileXRCcuResourceAllocatorReport report;
                if (allocator.Allocate(request, &plan, &allocation, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "undersized hcomm-style task1 prelude window was accepted\n";
                    return 2;
                }
                if (report.message.find("prelude") == std::string::npos ||
                    report.message.find("sync instruction") == std::string::npos) {
                    std::cerr << "weak prelude instruction diagnostic: " << report.message << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_allocator_is_wired_and_does_not_reference_hcomm_runtime_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = ALLOCATOR_HEADER.read_text(encoding="utf-8")
        source = ALLOCATOR_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_resource_allocator.h", cmake)
        self.assertIn("ccu/tilexr_ccu_resource_allocator.cpp", cmake)
        self.assertIn("TileXRCcuResourceAllocator", header)
        self.assertIn("TileXRCcuResourceSpec", header)
        self.assertIn("TileXRCcuResourceRequest", header)
        self.assertIn("TileXRCcuResourceAllocation", header)
        self.assertIn("TileXRCcuProducerPlan", header)
        self.assertIn("tilexr-hcomm-derived-resource-allocator", source)
        self.assertIn("requiredBarrierInstructionCount", source)
        self.assertIn("syncResourceCount * 2U", source)
        self.assertIn("TileXRCcuBarrierMode::LocalCkePostOnly", source)
        self.assertIn("request.barrierMode == TileXRCcuBarrierMode::LocalCkePostOnly", source)
        self.assertIn("barrier sync instruction window is too small", source)
        self.assertIn("const uint32_t channelCount = request.syncResourceCount", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_SYNC_RESOURCE_MAP", source)
        self.assertNotIn("UseHcommTraceSyncResourceMap", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_SQE_LOAD_XN_MAP", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_TASK1_PRELUDE_CKE", source)

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
            "HcclCcuKernel",
            "HcomGetCcuTaskInfo",
            "HcclGetCcuTaskInfo",
            "rtCCULaunch",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
