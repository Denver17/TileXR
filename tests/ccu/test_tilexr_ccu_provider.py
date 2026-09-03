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
PROVIDER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_provider.h"
PROVIDER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_provider.cpp"
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


class TileXRCcuProviderTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers provider C++ syntax")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "provider_test.cpp"
            test_bin = temp_path / "provider_test"
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
                    str(PROVIDER_SOURCE),
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

    def test_local_cke_post_only_submit_does_not_require_remote_xn_or_channel_binding(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuProducerPlan MakePlan()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 1};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 1};
                plan.barrierMode = TileXRCcuBarrierMode::LocalCkePostOnly;

                TileXRCcuSyncResource resource;
                resource.dieId = 1;
                resource.localXn = 1961;
                resource.remoteXn = 2361;
                resource.notifyCke = 364;
                resource.channelId = 2;
                resource.bindingCount = 1;
                resource.localWaitCke = 332;
                plan.syncResources.push_back(resource);

                plan.taskWindows.push_back({1, 489, 1, 13, {}});
                return plan;
            }

            TileXRCcuEvidenceSource Source(
                const TileXRCcuLaunchPackage& package,
                const char* label,
                bool endpointRouteVerified)
            {
                TileXRCcuEvidenceSource source;
                source.kind = TileXRCcuEvidenceKind::PublicVerified;
                source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                source.packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                source.deviceId = package.installScope.deviceId;
                source.rank = package.installScope.rank;
                source.provider = package.installScope.provider;
                source.installAttemptReceiptId = 0xabc001ULL;
                source.endpointRouteVerified = endpointRouteVerified;
                source.source = std::string("unit-test-public-provider:") + label;
                source.detail = std::string("audited ") + label + " evidence";
                return source;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 2;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.notifyCkeInstalled = true;
                evidence.missionSource = Source(package, "mission", false);
                evidence.repositorySource = Source(package, "repository", false);
                evidence.localXnSource = Source(package, "local-xn", false);
                evidence.notifyCkeSource = Source(package, "notify-cke", false);

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport report;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "local CKE post-only submit was rejected: " << report.message << "\n";
                    return 3;
                }
                if (!report.submitReady || submitTasks.size() != package.tasks.size()) {
                    std::cerr << "local CKE submit readiness mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_provider_gate_requires_complete_hardware_install_evidence(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }

                TileXRCcuProviderReport report;
                if (TileXRCcuValidateHardwareInstall(package, {}, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty install evidence was accepted\n";
                    return 2;
                }
                if (report.message.find("mission") == std::string::npos) {
                    std::cerr << "empty install diagnostic was weak: " << report.message << "\n";
                    return 3;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                if (TileXRCcuValidateHardwareInstall(package, evidence, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing channel binding was accepted\n";
                    return 4;
                }
                if (report.message.find("channel") == std::string::npos) {
                    std::cerr << "channel diagnostic was weak: " << report.message << "\n";
                    return 5;
                }

                evidence.channelBindingsInstalled = true;
                if (TileXRCcuValidateHardwareInstall(package, evidence, &report) != TILEXR_SUCCESS) {
                    std::cerr << "complete install evidence was rejected: " << report.message << "\n";
                    return 6;
                }
                if (report.taskCount != 2 || report.installedInstructionCount != 24 ||
                    report.repositoryCount != 170 || report.submitReady ||
                    report.message.find("validate-compatible") == std::string::npos) {
                    std::cerr << "unexpected provider report\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_provider_gate_marks_package_submit_ready_only_after_install_evidence(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(plan, &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 8;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport report;
                if (TileXRCcuPrepareSubmitTasks(package, {}, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "uninstalled package became submit-ready\n";
                    return 2;
                }
                if (!submitTasks.empty()) {
                    std::cerr << "failed prepare left submit tasks\n";
                    return 3;
                }

                const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;
                evidence.missionSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.missionSource.source = "unit-test-public-provider:mission";
                evidence.missionSource.detail = "audited mission/key evidence";
                evidence.missionSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.repositorySource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.repositorySource.source = "unit-test-public-provider:repository";
                evidence.repositorySource.detail = "audited repository evidence";
                evidence.repositorySource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.localXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.localXnSource.source = "unit-test-public-provider:local-xn";
                evidence.localXnSource.detail = "audited local XN evidence";
                evidence.localXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.remoteXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.remoteXnSource.source = "ValidateRemoteXnExchangeBindingProof";
                evidence.remoteXnSource.detail =
                    "remote XN peer exchange proof matches syncXn operands and verified endpoint route channel contexts";
                evidence.remoteXnSource.endpointRouteVerified = true;
                evidence.remoteXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.notifyCkeSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.notifyCkeSource.source = "unit-test-public-provider:notify-cke";
                evidence.notifyCkeSource.detail = "audited notify CKE evidence";
                evidence.notifyCkeSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.channelBindingSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.channelBindingSource.source = "unit-test-public-provider:channel-binding";
                evidence.channelBindingSource.detail =
                    "channel binding contexts installed via SET_PFE, SET_JETTY_CTX, SET_CHANNEL with verified endpoint routes";
                evidence.channelBindingSource.endpointRouteVerified = true;
                evidence.channelBindingSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.missionSource.packageFingerprint = packageFingerprint;
                evidence.repositorySource.packageFingerprint = packageFingerprint;
                evidence.localXnSource.packageFingerprint = packageFingerprint;
                evidence.remoteXnSource.packageFingerprint = packageFingerprint;
                evidence.notifyCkeSource.packageFingerprint = packageFingerprint;
                evidence.channelBindingSource.packageFingerprint = packageFingerprint;
                evidence.missionSource.deviceId = 3;
                evidence.repositorySource.deviceId = 3;
                evidence.localXnSource.deviceId = 3;
                evidence.remoteXnSource.deviceId = 3;
                evidence.notifyCkeSource.deviceId = 3;
                evidence.channelBindingSource.deviceId = 3;
                evidence.missionSource.rank = 1;
                evidence.repositorySource.rank = 1;
                evidence.localXnSource.rank = 1;
                evidence.remoteXnSource.rank = 1;
                evidence.notifyCkeSource.rank = 1;
                evidence.channelBindingSource.rank = 1;
                evidence.missionSource.provider = "unit-test-public-install-provider";
                evidence.repositorySource.provider = "unit-test-public-install-provider";
                evidence.localXnSource.provider = "unit-test-public-install-provider";
                evidence.remoteXnSource.provider = "unit-test-public-install-provider";
                evidence.notifyCkeSource.provider = "unit-test-public-install-provider";
                evidence.channelBindingSource.provider = "unit-test-public-install-provider";
                evidence.missionSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.repositorySource.installAttemptReceiptId = 0xabc001ULL;
                evidence.localXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.remoteXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.notifyCkeSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.channelBindingSource.installAttemptReceiptId = 0xabc001ULL;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "complete install evidence was rejected: " << report.message << "\n";
                    return 4;
                }
                if (submitTasks.size() != 2 || submitTasks[0].instStartId != 489 ||
                    submitTasks[0].instCnt != 13 || submitTasks[0].argSize != 13 ||
                    submitTasks[1].instStartId != 502 || submitTasks[1].instCnt != 143 ||
                    submitTasks[1].argSize != 13) {
                    std::cerr << "unexpected submit tasks\n";
                    return 5;
                }
                if (!report.submitReady || report.message != "ok") {
                    std::cerr << "unexpected submit-ready report\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_prepare_rejects_legacy_bool_only_evidence_for_submit(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(plan, &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;

                TileXRCcuProviderReport report;
                if (TileXRCcuValidateHardwareInstall(package, evidence, &report) != TILEXR_SUCCESS) {
                    std::cerr << "legacy bool install evidence should remain validate-compatible: "
                              << report.message << "\n";
                    return 2;
                }
                if (report.submitReady || report.legacyEvidenceCount != 6) {
                    std::cerr << "legacy validate report incorrectly claimed submit-ready\n";
                    return 3;
                }

                std::vector<TileXRCcuTask> submitTasks;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "legacy bool-only evidence became submit-ready\n";
                    return 4;
                }
                if (!submitTasks.empty()) {
                    std::cerr << "failed legacy prepare left submit tasks\n";
                    return 5;
                }
                if (report.submitReady || report.legacyEvidenceCount != 6) {
                    std::cerr << "unexpected failed prepare report\n";
                    return 6;
                }
                if (report.message.find("submit requires public verified evidence") == std::string::npos) {
                    std::cerr << "legacy submit diagnostic was weak: " << report.message << "\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_prepare_requires_auditable_public_verified_evidence_sources(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            TileXRCcuHardwareInstallEvidence VerifiedEvidence(const TileXRCcuLaunchPackage& package)
            {
                const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;
                evidence.missionSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.repositorySource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.localXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.remoteXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.notifyCkeSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.channelBindingSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                return evidence;
            }

            void FillAuditSources(TileXRCcuHardwareInstallEvidence& evidence, const TileXRCcuLaunchPackage& package)
            {
                const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                evidence.missionSource.source = "unit-test-public-provider:mission";
                evidence.missionSource.detail = "audited mission/key evidence";
                evidence.missionSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.missionSource.packageFingerprint = packageFingerprint;
                evidence.repositorySource.source = "unit-test-public-provider:repository";
                evidence.repositorySource.detail = "audited repository evidence";
                evidence.repositorySource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.repositorySource.packageFingerprint = packageFingerprint;
                evidence.localXnSource.source = "unit-test-public-provider:local-xn";
                evidence.localXnSource.detail = "audited local XN evidence";
                evidence.localXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.localXnSource.packageFingerprint = packageFingerprint;
                evidence.remoteXnSource.source = "unit-test-public-provider:remote-xn";
                evidence.remoteXnSource.detail = "audited remote XN evidence";
                evidence.remoteXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.remoteXnSource.endpointRouteVerified = true;
                evidence.remoteXnSource.packageFingerprint = packageFingerprint;
                evidence.notifyCkeSource.source = "unit-test-public-provider:notify-cke";
                evidence.notifyCkeSource.detail = "audited notify CKE evidence";
                evidence.notifyCkeSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.notifyCkeSource.packageFingerprint = packageFingerprint;
                evidence.channelBindingSource.source = "unit-test-public-provider:channel-binding";
                evidence.channelBindingSource.detail = "audited channel binding evidence";
                evidence.channelBindingSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.channelBindingSource.endpointRouteVerified = true;
                evidence.channelBindingSource.packageFingerprint = packageFingerprint;
                evidence.missionSource.deviceId = package.installScope.deviceId;
                evidence.repositorySource.deviceId = package.installScope.deviceId;
                evidence.localXnSource.deviceId = package.installScope.deviceId;
                evidence.remoteXnSource.deviceId = package.installScope.deviceId;
                evidence.notifyCkeSource.deviceId = package.installScope.deviceId;
                evidence.channelBindingSource.deviceId = package.installScope.deviceId;
                evidence.missionSource.rank = package.installScope.rank;
                evidence.repositorySource.rank = package.installScope.rank;
                evidence.localXnSource.rank = package.installScope.rank;
                evidence.remoteXnSource.rank = package.installScope.rank;
                evidence.notifyCkeSource.rank = package.installScope.rank;
                evidence.channelBindingSource.rank = package.installScope.rank;
                evidence.missionSource.provider = package.installScope.provider;
                evidence.repositorySource.provider = package.installScope.provider;
                evidence.localXnSource.provider = package.installScope.provider;
                evidence.remoteXnSource.provider = package.installScope.provider;
                evidence.notifyCkeSource.provider = package.installScope.provider;
                evidence.channelBindingSource.provider = package.installScope.provider;
                evidence.missionSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.repositorySource.installAttemptReceiptId = 0xabc001ULL;
                evidence.localXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.remoteXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.notifyCkeSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.channelBindingSource.installAttemptReceiptId = 0xabc001ULL;
            }

            void MarkEndpointRoutesVerified(TileXRCcuHardwareInstallEvidence& evidence)
            {
                evidence.remoteXnSource.endpointRouteVerified = true;
                evidence.channelBindingSource.endpointRouteVerified = true;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 10;
                }

                TileXRCcuHardwareInstallEvidence evidence = VerifiedEvidence(package);
                TileXRCcuProviderReport report;
                if (TileXRCcuValidateHardwareInstall(package, evidence, &report) != TILEXR_SUCCESS) {
                    std::cerr << "validate compatibility rejected verified kind without audit strings: "
                              << report.message << "\n";
                    return 2;
                }

                std::vector<TileXRCcuTask> submitTasks;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "public verified evidence without source/detail became submit-ready\n";
                    return 3;
                }
                if (!submitTasks.empty()) {
                    std::cerr << "failed unaudited prepare left submit tasks\n";
                    return 4;
                }
                if (report.message.find("public verified evidence source/detail required") == std::string::npos) {
                    std::cerr << "unaudited diagnostic was weak: " << report.message << "\n";
                    return 5;
                }

                FillAuditSources(evidence, package);
                evidence.missionSource.surface = TileXRCcuEvidenceSurface::Unspecified;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "public verified evidence without provider surface became submit-ready\n";
                    return 6;
                }
                if (report.message.find("public install provider evidence") == std::string::npos) {
                    std::cerr << "unclassified surface diagnostic was weak: " << report.message << "\n";
                    return 7;
                }

                evidence.missionSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.remoteXnSource.endpointRouteVerified = false;
                evidence.channelBindingSource.endpointRouteVerified = false;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "generic endpoint evidence became submit-ready\n";
                    return 8;
                }
                if (report.message.find("verified endpoint route") == std::string::npos) {
                    std::cerr << "endpoint provenance diagnostic was weak: " << report.message << "\n";
                    return 9;
                }

                MarkEndpointRoutesVerified(evidence);
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "audited public verified evidence was rejected: " << report.message << "\n";
                    return 10;
                }
                if (submitTasks.size() != 2 || submitTasks[0].instStartId != 489 ||
                    submitTasks[0].instCnt != 13 || submitTasks[1].instStartId != 502 ||
                    submitTasks[1].instCnt != 143 || !report.submitReady ||
                    report.publicVerifiedEvidenceCount != 6) {
                    std::cerr << "unexpected audited submit report\n";
                    return 11;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_prepare_requires_install_evidence_to_match_launch_package_fingerprint(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuProducerPlan MakePlan(uint32_t key, uint64_t firstArg)
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, key, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.syncResources.push_back({1, 1962, 2362, 364, 3, 3});
                plan.syncResources.push_back({1, 1963, 2364, 364, 4, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {firstArg}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            TileXRCcuEvidenceSource Source(const char* label, const TileXRCcuLaunchPackage& package)
            {
                TileXRCcuEvidenceSource source;
                source.kind = TileXRCcuEvidenceKind::PublicVerified;
                source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                source.source = label;
                const uint64_t fingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                source.detail = std::string("installed package fingerprint=") + std::to_string(fingerprint);
                const std::string labelText(label);
                if (labelText.find("remote-xn") != std::string::npos ||
                    labelText.find("channel-binding") != std::string::npos) {
                    source.endpointRouteVerified = true;
                }
                source.packageFingerprint = fingerprint;
                source.deviceId = package.installScope.deviceId;
                source.rank = package.installScope.rank;
                source.provider = package.installScope.provider;
                source.installAttemptReceiptId = 0xabc001ULL;
                return source;
            }

            TileXRCcuHardwareInstallEvidence EvidenceFor(const TileXRCcuLaunchPackage& package)
            {
                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;
                evidence.missionSource = Source("unit-test-public-provider:mission", package);
                evidence.repositorySource = Source("unit-test-public-provider:repository", package);
                evidence.localXnSource = Source("unit-test-public-provider:local-xn", package);
                evidence.remoteXnSource = Source("unit-test-public-provider:remote-xn", package);
                evidence.notifyCkeSource = Source("unit-test-public-provider:notify-cke", package);
                evidence.channelBindingSource = Source("unit-test-public-provider:channel-binding", package);
                return evidence;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(0x059b0f03U, 0x100051152e00ULL), &package, &packageReport) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 11;
                }
                const uint64_t fingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                if (fingerprint == 0) {
                    std::cerr << "package fingerprint was zero\n";
                    return 2;
                }

                TileXRCcuHardwareInstallEvidence evidence = EvidenceFor(package);
                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport report;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "matching evidence rejected: " << report.message << "\n";
                    return 3;
                }

                TileXRCcuLaunchPackage changedKeyPackage;
                if (TileXRCcuBuildLaunchPackage(
                        MakePlan(0xe0ac084cU, 0x100051152e00ULL), &changedKeyPackage, &packageReport) !=
                        TILEXR_SUCCESS) {
                    std::cerr << "changed-key package build failed\n";
                    return 4;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &changedKeyPackage, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "changed-key scope bind failed\n";
                    return 12;
                }
                if (TileXRCcuComputeLaunchPackageFingerprint(changedKeyPackage) == fingerprint) {
                    std::cerr << "changed key did not affect package fingerprint\n";
                    return 5;
                }
                if (TileXRCcuPrepareSubmitTasks(changedKeyPackage, evidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "evidence for old key was accepted for changed package\n";
                    return 6;
                }
                if (report.message.find("fingerprint") == std::string::npos) {
                    std::cerr << "fingerprint diagnostic was weak: " << report.message << "\n";
                    return 7;
                }

                TileXRCcuLaunchPackage changedArgsPackage;
                if (TileXRCcuBuildLaunchPackage(
                        MakePlan(0x059b0f03U, 0x200051152e00ULL), &changedArgsPackage, &packageReport) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "changed-args package build failed\n";
                    return 8;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &changedArgsPackage, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "changed-args scope bind failed\n";
                    return 13;
                }
                if (TileXRCcuComputeLaunchPackageFingerprint(changedArgsPackage) == fingerprint) {
                    std::cerr << "changed task args did not affect package fingerprint\n";
                    return 9;
                }
                if (TileXRCcuPrepareSubmitTasks(changedArgsPackage, evidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "evidence for old task args was accepted for changed package\n";
                    return 10;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_prepare_requires_install_evidence_to_match_launch_scope(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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

            TileXRCcuEvidenceSource Source(
                const char* label,
                const TileXRCcuLaunchPackage& package,
                uint32_t deviceId,
                uint32_t rank,
                const char* provider)
            {
                TileXRCcuEvidenceSource source;
                source.kind = TileXRCcuEvidenceKind::PublicVerified;
                source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                source.source = label;
                source.detail = "installed for explicit launch scope";
                const std::string labelText(label);
                if (labelText.find("remote-xn") != std::string::npos ||
                    labelText.find("channel-binding") != std::string::npos) {
                    source.endpointRouteVerified = true;
                }
                source.packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                source.deviceId = deviceId;
                source.rank = rank;
                source.provider = provider;
                source.installAttemptReceiptId = 0xabc001ULL;
                return source;
            }

            TileXRCcuHardwareInstallEvidence EvidenceFor(
                const TileXRCcuLaunchPackage& package,
                uint32_t deviceId,
                uint32_t rank,
                const char* provider)
            {
                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;
                evidence.missionSource = Source("unit-test-public-provider:mission", package, deviceId, rank, provider);
                evidence.repositorySource =
                    Source("unit-test-public-provider:repository", package, deviceId, rank, provider);
                evidence.localXnSource =
                    Source("unit-test-public-provider:local-xn", package, deviceId, rank, provider);
                evidence.remoteXnSource =
                    Source("unit-test-public-provider:remote-xn", package, deviceId, rank, provider);
                evidence.notifyCkeSource =
                    Source("unit-test-public-provider:notify-cke", package, deviceId, rank, provider);
                evidence.channelBindingSource =
                    Source("unit-test-public-provider:channel-binding", package, deviceId, rank, provider);
                return evidence;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "failed to bind launch install scope\n";
                    return 2;
                }

                TileXRCcuProviderReport report;
                std::vector<TileXRCcuTask> submitTasks;

                TileXRCcuHardwareInstallEvidence wrongDevice =
                    EvidenceFor(package, 4, 1, "unit-test-public-install-provider");
                if (TileXRCcuPrepareSubmitTasks(package, wrongDevice, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "evidence from another device became submit-ready\n";
                    return 3;
                }
                if (!submitTasks.empty() || report.submitReady ||
                    report.message.find("device") == std::string::npos) {
                    std::cerr << "device-scope diagnostic was weak: " << report.message << "\n";
                    return 4;
                }

                TileXRCcuHardwareInstallEvidence wrongRank =
                    EvidenceFor(package, 3, 0, "unit-test-public-install-provider");
                if (TileXRCcuPrepareSubmitTasks(package, wrongRank, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "evidence from another rank became submit-ready\n";
                    return 5;
                }
                if (!submitTasks.empty() || report.submitReady ||
                    report.message.find("rank") == std::string::npos) {
                    std::cerr << "rank-scope diagnostic was weak: " << report.message << "\n";
                    return 6;
                }

                TileXRCcuHardwareInstallEvidence wrongProvider =
                    EvidenceFor(package, 3, 1, "other-public-install-provider");
                if (TileXRCcuPrepareSubmitTasks(package, wrongProvider, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "evidence from another provider became submit-ready\n";
                    return 7;
                }
                if (!submitTasks.empty() || report.submitReady ||
                    report.message.find("provider") == std::string::npos) {
                    std::cerr << "provider-scope diagnostic was weak: " << report.message << "\n";
                    return 8;
                }

                TileXRCcuHardwareInstallEvidence matching =
                    EvidenceFor(package, 3, 1, "unit-test-public-install-provider");
                if (TileXRCcuPrepareSubmitTasks(package, matching, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "matching scope evidence was rejected: " << report.message << "\n";
                    return 9;
                }
                if (submitTasks.size() != package.tasks.size() || !report.submitReady) {
                    std::cerr << "unexpected matching scope submit state\n";
                    return 10;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_prepare_requires_all_public_verified_evidence_from_same_install_receipt(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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

            TileXRCcuEvidenceSource Source(
                const char* label,
                const TileXRCcuLaunchPackage& package,
                uint64_t receipt)
            {
                TileXRCcuEvidenceSource source;
                source.kind = TileXRCcuEvidenceKind::PublicVerified;
                source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                source.source = label;
                source.detail = "installed for a single public install attempt";
                const std::string labelText(label);
                if (labelText.find("remote-xn") != std::string::npos ||
                    labelText.find("channel-binding") != std::string::npos) {
                    source.endpointRouteVerified = true;
                }
                source.packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                source.deviceId = package.installScope.deviceId;
                source.rank = package.installScope.rank;
                source.provider = package.installScope.provider;
                source.installAttemptReceiptId = receipt;
                return source;
            }

            TileXRCcuHardwareInstallEvidence EvidenceFor(
                const TileXRCcuLaunchPackage& package,
                uint64_t receipt)
            {
                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;
                evidence.missionSource = Source("unit-test-public-provider:mission", package, receipt);
                evidence.repositorySource = Source("unit-test-public-provider:repository", package, receipt);
                evidence.localXnSource = Source("unit-test-public-provider:local-xn", package, receipt);
                evidence.remoteXnSource = Source("unit-test-public-provider:remote-xn", package, receipt);
                evidence.notifyCkeSource = Source("unit-test-public-provider:notify-cke", package, receipt);
                evidence.channelBindingSource = Source("unit-test-public-provider:channel-binding", package, receipt);
                return evidence;
            }

            int ExpectReceiptFailure(
                const TileXRCcuLaunchPackage& package,
                const TileXRCcuHardwareInstallEvidence& evidence,
                const char* expectedDiagnostic)
            {
                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport report;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "receipt mismatch became submit-ready\n";
                    return 1;
                }
                if (!submitTasks.empty() || report.submitReady ||
                    report.message.find(expectedDiagnostic) == std::string::npos) {
                    std::cerr << "weak receipt diagnostic: " << report.message << "\n";
                    return 2;
                }
                return 0;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "failed to bind launch install scope\n";
                    return 2;
                }

                TileXRCcuHardwareInstallEvidence missingReceipt = EvidenceFor(package, 0);
                if (ExpectReceiptFailure(package, missingReceipt, "receipt") != 0) {
                    return 3;
                }

                TileXRCcuHardwareInstallEvidence mixedReceipt = EvidenceFor(package, 0xabc001ULL);
                mixedReceipt.notifyCkeSource.installAttemptReceiptId = 0xabc002ULL;
                if (ExpectReceiptFailure(package, mixedReceipt, "receipt") != 0) {
                    return 4;
                }

                TileXRCcuHardwareInstallEvidence matchingReceipt = EvidenceFor(package, 0xabc001ULL);
                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport report;
                if (TileXRCcuPrepareSubmitTasks(package, matchingReceipt, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "matching receipt evidence was rejected: " << report.message << "\n";
                    return 5;
                }
                if (submitTasks.size() != package.tasks.size() || !report.submitReady) {
                    std::cerr << "unexpected matching receipt submit state\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_prepare_rejects_public_verified_evidence_from_lower_layer_or_private_sources(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            TileXRCcuHardwareInstallEvidence AuditedEvidence(const TileXRCcuLaunchPackage& package)
            {
                const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;
                evidence.missionSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.missionSource.source = "unit-test-public-provider:mission";
                evidence.missionSource.detail = "audited mission/key evidence";
                evidence.missionSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.missionSource.packageFingerprint = packageFingerprint;
                evidence.repositorySource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.repositorySource.source = "unit-test-public-provider:repository";
                evidence.repositorySource.detail = "audited repository evidence";
                evidence.repositorySource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.repositorySource.packageFingerprint = packageFingerprint;
                evidence.localXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.localXnSource.source = "unit-test-public-provider:local-xn";
                evidence.localXnSource.detail = "audited local XN evidence";
                evidence.localXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.localXnSource.packageFingerprint = packageFingerprint;
                evidence.remoteXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.remoteXnSource.source = "unit-test-public-provider:remote-xn";
                evidence.remoteXnSource.detail = "audited remote XN evidence";
                evidence.remoteXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.remoteXnSource.endpointRouteVerified = true;
                evidence.remoteXnSource.packageFingerprint = packageFingerprint;
                evidence.notifyCkeSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.notifyCkeSource.source = "unit-test-public-provider:notify-cke";
                evidence.notifyCkeSource.detail = "audited notify CKE evidence";
                evidence.notifyCkeSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.notifyCkeSource.packageFingerprint = packageFingerprint;
                evidence.channelBindingSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.channelBindingSource.source = "unit-test-public-provider:channel-binding";
                evidence.channelBindingSource.detail = "audited channel binding evidence";
                evidence.channelBindingSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.channelBindingSource.endpointRouteVerified = true;
                evidence.channelBindingSource.packageFingerprint = packageFingerprint;
                evidence.missionSource.deviceId = package.installScope.deviceId;
                evidence.repositorySource.deviceId = package.installScope.deviceId;
                evidence.localXnSource.deviceId = package.installScope.deviceId;
                evidence.remoteXnSource.deviceId = package.installScope.deviceId;
                evidence.notifyCkeSource.deviceId = package.installScope.deviceId;
                evidence.channelBindingSource.deviceId = package.installScope.deviceId;
                evidence.missionSource.rank = package.installScope.rank;
                evidence.repositorySource.rank = package.installScope.rank;
                evidence.localXnSource.rank = package.installScope.rank;
                evidence.remoteXnSource.rank = package.installScope.rank;
                evidence.notifyCkeSource.rank = package.installScope.rank;
                evidence.channelBindingSource.rank = package.installScope.rank;
                evidence.missionSource.provider = package.installScope.provider;
                evidence.repositorySource.provider = package.installScope.provider;
                evidence.localXnSource.provider = package.installScope.provider;
                evidence.remoteXnSource.provider = package.installScope.provider;
                evidence.notifyCkeSource.provider = package.installScope.provider;
                evidence.channelBindingSource.provider = package.installScope.provider;
                evidence.missionSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.repositorySource.installAttemptReceiptId = 0xabc001ULL;
                evidence.localXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.remoteXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.notifyCkeSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.channelBindingSource.installAttemptReceiptId = 0xabc001ULL;
                return evidence;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 8;
                }

                TileXRCcuProviderReport report;
                std::vector<TileXRCcuTask> submitTasks;

                TileXRCcuHardwareInstallEvidence mappingEvidence = AuditedEvidence(package);
                mappingEvidence.localXnSource.source = "unit-test-public-mapper:local-xn";
                mappingEvidence.localXnSource.detail = "maps existing XN resources but does not install them";
                mappingEvidence.localXnSource.surface = TileXRCcuEvidenceSurface::LowerLayerResourceHelper;
                if (TileXRCcuValidateHardwareInstall(package, mappingEvidence, &report) != TILEXR_SUCCESS) {
                    std::cerr << "validate compatibility rejected mapping evidence: " << report.message << "\n";
                    return 2;
                }
                if (TileXRCcuPrepareSubmitTasks(package, mappingEvidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "lower-layer mapping helper became submit-ready\n";
                    return 3;
                }
                if (report.message.find("public install provider evidence") == std::string::npos ||
                    report.message.find("lower-layer resource helper") == std::string::npos) {
                    std::cerr << "mapping diagnostic was weak: " << report.message << "\n";
                    return 4;
                }

                TileXRCcuHardwareInstallEvidence notifyEvidence = AuditedEvidence(package);
                notifyEvidence.notifyCkeSource.source = "unit-test-public-notify-reader:notify-cke";
                notifyEvidence.notifyCkeSource.detail = "reads an existing notify address but does not install CKE";
                notifyEvidence.notifyCkeSource.surface = TileXRCcuEvidenceSurface::LowerLayerResourceHelper;
                if (TileXRCcuPrepareSubmitTasks(package, notifyEvidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "lower-layer notify helper became submit-ready\n";
                    return 5;
                }

                TileXRCcuHardwareInstallEvidence privateEvidence = AuditedEvidence(package);
                privateEvidence.repositorySource.source = "unit-test-private-observation:repository";
                privateEvidence.repositorySource.detail = "private repository install observation";
                privateEvidence.repositorySource.surface = TileXRCcuEvidenceSurface::PrivateProducerObservation;
                if (TileXRCcuPrepareSubmitTasks(package, privateEvidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "private repository installer became submit-ready\n";
                    return 6;
                }

                TileXRCcuHardwareInstallEvidence audited = AuditedEvidence(package);
                if (TileXRCcuPrepareSubmitTasks(package, audited, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "neutral audited evidence was rejected: " << report.message << "\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_provider_gate_rejects_private_and_unvalidated_candidate_evidence(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            TileXRCcuHardwareInstallEvidence VerifiedEvidence(const TileXRCcuLaunchPackage& package)
            {
                const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = true;
                evidence.missionSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.missionSource.source = "unit-test-public-provider:mission";
                evidence.missionSource.detail = "audited mission/key evidence";
                evidence.missionSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.missionSource.packageFingerprint = packageFingerprint;
                evidence.repositorySource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.repositorySource.source = "unit-test-public-provider:repository";
                evidence.repositorySource.detail = "audited repository evidence";
                evidence.repositorySource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.repositorySource.packageFingerprint = packageFingerprint;
                evidence.localXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.localXnSource.source = "unit-test-public-provider:local-xn";
                evidence.localXnSource.detail = "audited local XN evidence";
                evidence.localXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.localXnSource.packageFingerprint = packageFingerprint;
                evidence.remoteXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.remoteXnSource.source = "unit-test-public-provider:remote-xn";
                evidence.remoteXnSource.detail = "audited remote XN evidence";
                evidence.remoteXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.remoteXnSource.endpointRouteVerified = true;
                evidence.remoteXnSource.packageFingerprint = packageFingerprint;
                evidence.notifyCkeSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.notifyCkeSource.source = "unit-test-public-provider:notify-cke";
                evidence.notifyCkeSource.detail = "audited notify CKE evidence";
                evidence.notifyCkeSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.notifyCkeSource.packageFingerprint = packageFingerprint;
                evidence.channelBindingSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.channelBindingSource.source = "unit-test-public-provider:channel-binding";
                evidence.channelBindingSource.detail = "audited channel binding evidence";
                evidence.channelBindingSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.channelBindingSource.endpointRouteVerified = true;
                evidence.channelBindingSource.packageFingerprint = packageFingerprint;
                evidence.missionSource.deviceId = package.installScope.deviceId;
                evidence.repositorySource.deviceId = package.installScope.deviceId;
                evidence.localXnSource.deviceId = package.installScope.deviceId;
                evidence.remoteXnSource.deviceId = package.installScope.deviceId;
                evidence.notifyCkeSource.deviceId = package.installScope.deviceId;
                evidence.channelBindingSource.deviceId = package.installScope.deviceId;
                evidence.missionSource.rank = package.installScope.rank;
                evidence.repositorySource.rank = package.installScope.rank;
                evidence.localXnSource.rank = package.installScope.rank;
                evidence.remoteXnSource.rank = package.installScope.rank;
                evidence.notifyCkeSource.rank = package.installScope.rank;
                evidence.channelBindingSource.rank = package.installScope.rank;
                evidence.missionSource.provider = package.installScope.provider;
                evidence.repositorySource.provider = package.installScope.provider;
                evidence.localXnSource.provider = package.installScope.provider;
                evidence.remoteXnSource.provider = package.installScope.provider;
                evidence.notifyCkeSource.provider = package.installScope.provider;
                evidence.channelBindingSource.provider = package.installScope.provider;
                evidence.missionSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.repositorySource.installAttemptReceiptId = 0xabc001ULL;
                evidence.localXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.remoteXnSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.notifyCkeSource.installAttemptReceiptId = 0xabc001ULL;
                evidence.channelBindingSource.installAttemptReceiptId = 0xabc001ULL;
                return evidence;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 9;
                }

                TileXRCcuProviderReport report;
                TileXRCcuHardwareInstallEvidence privateEvidence = VerifiedEvidence(package);
                privateEvidence.remoteXnSource.kind = TileXRCcuEvidenceKind::PrivateObserved;
                privateEvidence.remoteXnSource.source = "hcomm::CcuKernel::CreateVariable";
                if (TileXRCcuValidateHardwareInstall(package, privateEvidence, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "private remote XN evidence was accepted\n";
                    return 2;
                }
                if (report.message.find("remote XN") == std::string::npos ||
                    report.message.find("private") == std::string::npos) {
                    std::cerr << "private evidence diagnostic was weak: " << report.message << "\n";
                    return 3;
                }

                TileXRCcuHardwareInstallEvidence candidateEvidence = VerifiedEvidence(package);
                candidateEvidence.channelBindingSource.kind = TileXRCcuEvidenceKind::PublicCandidate;
                candidateEvidence.channelBindingSource.source = "rtCcuBindChannel";
                if (TileXRCcuPrepareSubmitTasks(package, candidateEvidence, nullptr, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null submit vector was not rejected first\n";
                    return 4;
                }

                std::vector<TileXRCcuTask> submitTasks;
                if (TileXRCcuPrepareSubmitTasks(package, candidateEvidence, &submitTasks, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "public candidate channel binding was accepted\n";
                    return 5;
                }
                if (!submitTasks.empty()) {
                    std::cerr << "candidate prepare left submit tasks\n";
                    return 6;
                }
                if (report.message.find("channel") == std::string::npos ||
                    report.message.find("candidate") == std::string::npos) {
                    std::cerr << "candidate diagnostic was weak: " << report.message << "\n";
                    return 7;
                }

                TileXRCcuHardwareInstallEvidence verified = VerifiedEvidence(package);
                if (TileXRCcuPrepareSubmitTasks(package, verified, &submitTasks, &report) != TILEXR_SUCCESS) {
                    std::cerr << "public verified evidence was rejected: " << report.message << "\n";
                    return 8;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_provider_report_counts_evidence_kinds_for_auditing(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_provider.h"

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
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed\n";
                    return 1;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                evidence.missionInstalled = true;
                evidence.repositoryInstalled = true;
                evidence.localXnInstalled = true;
                evidence.remoteXnBound = true;
                evidence.notifyCkeInstalled = true;
                evidence.channelBindingsInstalled = false;
                evidence.missionSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.repositorySource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.localXnSource.kind = TileXRCcuEvidenceKind::LegacyBoolean;
                evidence.remoteXnSource.kind = TileXRCcuEvidenceKind::PrivateObserved;
                evidence.notifyCkeSource.kind = TileXRCcuEvidenceKind::PublicCandidate;
                evidence.channelBindingSource.kind = TileXRCcuEvidenceKind::Missing;

                TileXRCcuProviderReport report;
                if (TileXRCcuValidateHardwareInstall(package, evidence, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "mixed bad evidence was accepted\n";
                    return 2;
                }
                if (report.evidenceBitCount != 6 ||
                    report.publicVerifiedEvidenceCount != 2 ||
                    report.legacyEvidenceCount != 1 ||
                    report.privateObservedEvidenceCount != 1 ||
                    report.publicCandidateEvidenceCount != 1 ||
                    report.missingEvidenceCount != 1) {
                    std::cerr << "unexpected evidence counters: bits=" << report.evidenceBitCount
                              << " public=" << report.publicVerifiedEvidenceCount
                              << " legacy=" << report.legacyEvidenceCount
                              << " private=" << report.privateObservedEvidenceCount
                              << " candidate=" << report.publicCandidateEvidenceCount
                              << " missing=" << report.missingEvidenceCount << "\n";
                    return 3;
                }
                if (report.message.find("remote XN") == std::string::npos ||
                    report.message.find("private") == std::string::npos) {
                    std::cerr << "first rejection diagnostic was weak: " << report.message << "\n";
                    return 4;
                }

                evidence.remoteXnSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.remoteXnSource.source = "unit-test-public-provider:remote-xn";
                evidence.remoteXnSource.detail = "audited remote XN evidence";
                evidence.remoteXnSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.remoteXnSource.endpointRouteVerified = true;
                evidence.notifyCkeSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.notifyCkeSource.source = "unit-test-public-provider:notify-cke";
                evidence.notifyCkeSource.detail = "audited notify CKE evidence";
                evidence.notifyCkeSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.channelBindingsInstalled = true;
                evidence.channelBindingSource.kind = TileXRCcuEvidenceKind::PublicVerified;
                evidence.channelBindingSource.source = "unit-test-public-provider:channel-binding";
                evidence.channelBindingSource.detail = "audited channel binding evidence";
                evidence.channelBindingSource.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                evidence.channelBindingSource.endpointRouteVerified = true;
                if (TileXRCcuValidateHardwareInstall(package, evidence, &report) != TILEXR_SUCCESS) {
                    std::cerr << "fixed evidence was rejected: " << report.message << "\n";
                    return 5;
                }
                if (report.evidenceBitCount != 6 ||
                    report.publicVerifiedEvidenceCount != 5 ||
                    report.legacyEvidenceCount != 1 ||
                    report.privateObservedEvidenceCount != 0 ||
                    report.publicCandidateEvidenceCount != 0 ||
                    report.missingEvidenceCount != 0 ||
                    report.submitReady) {
                    std::cerr << "unexpected ready evidence counters\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_provider_layer_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = PROVIDER_HEADER.read_text(encoding="utf-8")
        source = PROVIDER_SOURCE.read_text(encoding="utf-8")
        package_header = PACKAGE_HEADER.read_text(encoding="utf-8")
        package_source = PACKAGE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_provider.h", cmake)
        self.assertIn("ccu/tilexr_ccu_provider.cpp", cmake)
        self.assertIn("struct TileXRCcuHardwareInstallEvidence", header)
        self.assertIn("enum class TileXRCcuEvidenceKind", header)
        self.assertIn("enum class TileXRCcuEvidenceSurface", header)
        self.assertIn("struct TileXRCcuEvidenceSource", header)
        self.assertIn("TileXRCcuValidateHardwareInstall", header)
        self.assertIn("TileXRCcuPrepareSubmitTasks", header)
        self.assertIn("tilexr_ccu_launch_package.h", header)
        self.assertIn("submitReady", header)
        self.assertIn("evidenceBitCount", header)
        self.assertIn("publicVerifiedEvidenceCount", header)
        self.assertIn("publicCandidateEvidenceCount", header)
        self.assertIn("privateObservedEvidenceCount", header)
        self.assertIn("missingEvidenceCount", header)
        self.assertIn("uint64_t packageFingerprint", header)
        self.assertIn("uint32_t deviceId", header)
        self.assertIn("uint32_t rank", header)
        self.assertIn("std::string provider", header)
        self.assertIn("uint64_t installAttemptReceiptId", header)
        self.assertIn("TileXRCcuComputeLaunchPackageFingerprint", package_header)
        self.assertIn("TileXRCcuComputeLaunchPackageFingerprint", package_source)
        self.assertIn("package fingerprint mismatch", source)
        self.assertIn("TileXRCcuLaunchInstallScope", package_header)
        self.assertIn("installScope", package_header)
        self.assertIn("TileXRCcuBindLaunchPackageInstallScope", package_header)
        self.assertIn("device scope mismatch", source)
        self.assertIn("rank scope mismatch", source)
        self.assertIn("provider scope mismatch", source)
        self.assertIn("install attempt receipt", source)
        self.assertIn("endpointRouteVerified", header)
        self.assertIn("submit requires verified endpoint route evidence", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_prepare_path_requires_public_verified_evidence_source(self):
        source = PROVIDER_SOURCE.read_text(encoding="utf-8")
        prepare_start = source.index("int TileXRCcuPrepareSubmitTasks(")
        prepare_body = source[prepare_start:]

        validate_pos = prepare_body.index("TileXRCcuValidateHardwareInstall(package, evidence, report)")
        submit_gate_pos = prepare_body.index("ValidateSubmitEvidence(package, evidence, report)")
        copy_pos = prepare_body.index("*submitTasks = package.tasks")

        self.assertIn("submit requires public verified evidence", source)
        self.assertLess(validate_pos, submit_gate_pos)
        self.assertLess(submit_gate_pos, copy_pos)


if __name__ == "__main__":
    unittest.main()
