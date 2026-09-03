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
PACKAGE_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_launch_package.h"
PACKAGE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_launch_package.cpp"
REPOSITORY_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_repository.cpp"
DRIVER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp"
PRODUCER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_producer_plan.cpp"
BARRIER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.cpp"
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
    "dlopen",
    "dlsym",
]


class TileXRCcuLaunchPackageTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers launch package C++ syntax")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "launch_package_test.cpp"
            test_bin = temp_path / "launch_package_test"
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
                    str(PACKAGE_SOURCE),
                    str(REPOSITORY_SOURCE),
                    str(DRIVER_SOURCE),
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

    def test_launch_package_combines_tasks_microcode_and_repository_image(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_launch_package.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.syncResources.push_back({1, 1962, 2362, 364, 3, 3});
                plan.syncResources.push_back({1, 1963, 2364, 364, 4, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {0x100051152e00ULL, 0x100051152e00ULL}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport report;
                if (TileXRCcuBuildLaunchPackage(plan, &package, &report) != TILEXR_SUCCESS) {
                    std::cerr << "launch package build failed: " << report.message << "\n";
                    return 1;
                }

                if (package.tasks.size() != 2 || package.program.sqeLoad.size() != 13 ||
                    package.program.sync.size() != 11 || package.repository.instructions.size() != 170) {
                    std::cerr << "unexpected package sizes\n";
                    return 2;
                }
                if (package.tasks[0].instStartId != 489 || package.tasks[0].instCnt != 13 ||
                    package.tasks[0].argSize != 13 || package.tasks[0].key != 0x059b0f03U ||
                    package.tasks[0].args[0] != 0x100051152e00ULL) {
                    std::cerr << "unexpected sqe-load mission task\n";
                    return 3;
                }
                if (package.tasks[1].instStartId != 502 || package.tasks[1].instCnt != 143 ||
                    package.tasks[1].argSize != 13 || package.tasks[1].key != 0x059b0f03U) {
                    std::cerr << "unexpected sync mission task\n";
                    return 4;
                }
                if (package.repository.sqeLoadOffset != 14 || package.repository.syncOffset != 27 ||
                    package.repository.instructions[14].words[0] != 0x0000000007a90001ULL ||
                    package.repository.instructions[27].words[0] != 0x0000000007a90001ULL ||
                    package.repository.instructions[31].words[0] != 0x0001016c00000802ULL ||
                    package.repository.instructions[32].words[0] != 0x000007a90939100dULL ||
                    package.repository.instructions[35].words[0] != 0x0000000000010802ULL) {
                    std::cerr << "unexpected repository image\n";
                    return 5;
                }
                if (!package.requiresHardwareInstall) {
                    std::cerr << "package should still require hardware install\n";
                    return 6;
                }
                if (report.taskCount != 2 || report.repositoryCount != 170 ||
                    report.installedInstructionCount != 24 || report.message != "ok") {
                    std::cerr << "unexpected package report\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_launch_package_rejects_invalid_inputs_without_partial_outputs(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_launch_package.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuLaunchPackageReport report;
                if (TileXRCcuBuildLaunchPackage(plan, nullptr, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null package output accepted\n";
                    return 1;
                }
                if (report.message.find("package") == std::string::npos) {
                    std::cerr << "null output diagnostic was weak: " << report.message << "\n";
                    return 2;
                }

                TileXRCcuLaunchPackage package;
                plan.syncResources[0].bindingCount = 0;
                if (TileXRCcuBuildLaunchPackage(plan, &package, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "invalid producer state accepted\n";
                    return 3;
                }
                if (!package.tasks.empty() || !package.repository.instructions.empty()) {
                    std::cerr << "failed build left partial package state\n";
                    return 4;
                }
                if (report.message.find("binding") == std::string::npos) {
                    std::cerr << "invalid producer diagnostic was weak: " << report.message << "\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_launch_package_builds_pure_barrier_without_sqe_load_task(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_launch_package.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {0, 1, 0x12345678U, true};
                plan.kernelLocalXn = {0, 1, 1};
                plan.kernelLocalCke = {0, 1, 1};
                plan.kernelLocalMission = {0, 1, 1};
                plan.instructionWindow = {0, 1, 2, 1, 2};
                plan.syncResources.push_back({0, 1, 2, 1, 1, 1, 1, 1, 1});
                plan.taskWindows.push_back({0, 1, 2, 13, {}});

                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport report;
                if (TileXRCcuBuildLaunchPackage(plan, &package, &report) != TILEXR_SUCCESS) {
                    std::cerr << "pure barrier package build failed: " << report.message << "\n";
                    return 1;
                }
                if (package.tasks.size() != 1 || !package.program.sqeLoad.empty() ||
                    package.program.sync.size() != 2 || package.repository.instructions.size() != 2) {
                    std::cerr << "pure barrier package size mismatch\n";
                    return 2;
                }
                if (package.tasks[0].instStartId != 1 || package.tasks[0].instCnt != 2 ||
                    package.tasks[0].argSize != 13 || package.tasks[0].args[0] != 0) {
                    std::cerr << "pure barrier task mismatch\n";
                    return 3;
                }
                if (package.repository.sqeLoadCount != 0 || package.repository.syncOffset != 0 ||
                    package.repository.syncCount != 2 ||
                    package.repository.instructions[0].words[0] != package.program.sync[0].words[0] ||
                    package.repository.instructions[0].words[1] != package.program.sync[0].words[1] ||
                    package.repository.instructions[1].words[0] != package.program.sync[1].words[0] ||
                    package.repository.instructions[1].words[1] != package.program.sync[1].words[1]) {
                    std::cerr << "pure barrier repository mismatch\n";
                    return 4;
                }
                if (report.taskCount != 1 || report.installedInstructionCount != 2 ||
                    report.message != "ok") {
                    std::cerr << "pure barrier report mismatch\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_launch_package_install_scope_records_current_package_fingerprint(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_launch_package.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuProducerPlan MakePlan()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.syncResources.push_back({1, 1962, 2362, 364, 3, 3});
                plan.syncResources.push_back({1, 1963, 2364, 364, 4, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {0x100051152e00ULL}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            int main()
            {
                if (TileXRCcuBindLaunchPackageInstallScope(
                        nullptr, 3, 1, "unit-test-public-install-provider") != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null package scope bind was accepted\n";
                    return 1;
                }

                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport report;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &report) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed: " << report.message << "\n";
                    return 2;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(&package, 3, 1, "") != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty provider scope bind was accepted\n";
                    return 3;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 4;
                }

                const uint64_t fingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                if (package.installScope.deviceId != 3 ||
                    package.installScope.rank != 1 ||
                    package.installScope.provider != "unit-test-public-install-provider" ||
                    package.installScope.packageFingerprint != fingerprint) {
                    std::cerr << "unexpected install scope binding\n";
                    return 5;
                }

                package.tasks[0].key ^= 0x1U;
                if (TileXRCcuComputeLaunchPackageFingerprint(package) == fingerprint) {
                    std::cerr << "mutated package kept old fingerprint\n";
                    return 6;
                }
                if (package.installScope.packageFingerprint == TileXRCcuComputeLaunchPackageFingerprint(package)) {
                    std::cerr << "install scope fingerprint silently tracked mutation\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_launch_package_fingerprint_changes_for_local_wait_cke(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_launch_package.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuProducerPlan MakePlan(uint16_t localWaitCke)
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
                resource.localWaitCke = localWaitCke;
                resource.localWaitMask = 1;
                resource.remoteNotifyMask = 1;
                plan.syncResources.push_back(resource);

                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            int main()
            {
                TileXRCcuLaunchPackage packageA;
                TileXRCcuLaunchPackage packageB;
                TileXRCcuLaunchPackageReport report;
                if (TileXRCcuBuildLaunchPackage(MakePlan(332), &packageA, &report) != TILEXR_SUCCESS ||
                    TileXRCcuBuildLaunchPackage(MakePlan(333), &packageB, &report) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed: " << report.message << "\n";
                    return 1;
                }
                const uint64_t fingerprintA = TileXRCcuComputeLaunchPackageFingerprint(packageA);
                const uint64_t fingerprintB = TileXRCcuComputeLaunchPackageFingerprint(packageB);
                if (fingerprintA == 0 || fingerprintB == 0 || fingerprintA == fingerprintB) {
                    std::cerr << "local wait CKE did not affect launch package fingerprint\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_launch_package_fingerprint_changes_for_kernel_local_gsa(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_launch_package.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuProducerPlan MakePlan(uint16_t gsaStart)
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalGsa = {1, gsaStart, 1};
                plan.kernelLocalCke = {1, 332, 4};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3, 332, 1, 1});
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            int main()
            {
                TileXRCcuLaunchPackage packageA;
                TileXRCcuLaunchPackage packageB;
                TileXRCcuLaunchPackageReport report;
                if (TileXRCcuBuildLaunchPackage(MakePlan(510), &packageA, &report) != TILEXR_SUCCESS ||
                    TileXRCcuBuildLaunchPackage(MakePlan(511), &packageB, &report) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed: " << report.message << "\n";
                    return 1;
                }
                const uint64_t fingerprintA = TileXRCcuComputeLaunchPackageFingerprint(packageA);
                const uint64_t fingerprintB = TileXRCcuComputeLaunchPackageFingerprint(packageB);
                if (fingerprintA == 0 || fingerprintB == 0 || fingerprintA == fingerprintB) {
                    std::cerr << "kernel-local GSA did not affect launch package fingerprint\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_launch_package_layer_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = PACKAGE_HEADER.read_text(encoding="utf-8")
        source = PACKAGE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_launch_package.h", cmake)
        self.assertIn("ccu/tilexr_ccu_launch_package.cpp", cmake)
        self.assertIn("struct TileXRCcuLaunchPackage", header)
        self.assertIn("struct TileXRCcuLaunchPackageReport", header)
        self.assertIn("TileXRCcuBuildLaunchPackage", header)
        self.assertIn("TileXRCcuLaunchInstallScope", header)
        self.assertIn("installScope", header)
        self.assertIn("TileXRCcuBindLaunchPackageInstallScope", header)
        self.assertIn("requiresHardwareInstall", header)
        self.assertIn("tilexr_ccu_repository.h", header)
        self.assertIn("resource.localWaitCke", source)
        self.assertIn("resource.localWaitMask", source)
        self.assertIn("resource.remoteNotifyMask", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
