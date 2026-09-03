#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import shutil
import subprocess
import tempfile
import textwrap
import unittest
import os
import platform
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALL_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_install_provider.h"
INSTALL_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_install_provider.cpp"
PROVIDER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_provider.cpp"
PACKAGE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_launch_package.cpp"
REPOSITORY_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_repository.cpp"
DRIVER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp"
SPECS_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp"
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


def cann_compile_flags():
    ascend_home = os.environ.get("ASCEND_HOME_PATH") or os.environ.get("ASCEND_HOME")
    if not ascend_home:
        return None

    arch = os.environ.get("ARCH")
    if not arch:
        arch = "aarch64" if platform.machine() in ("aarch64", "arm64") else "x86_64"

    cann_root = Path(ascend_home) / f"{arch}-linux"
    include_dirs = [
        cann_root / "pkg_inc",
        cann_root / "pkg_inc" / "runtime",
        cann_root / "include",
    ]
    header_exists = any((include_dir / "acl" / "acl_rt.h").exists() for include_dir in include_dirs)
    if not header_exists:
        return None

    lib_dir = cann_root / "lib64"
    if not (lib_dir / "libascendcl.so").exists() or not (lib_dir / "libruntime.so").exists():
        return None

    flags = []
    for include_dir in include_dirs:
        if include_dir.exists():
            flags.extend(["-I", str(include_dir)])
    flags.extend(["-L", str(lib_dir), "-lascendcl", "-lruntime"])
    return flags, str(lib_dir)


class TileXRCcuInstallProviderTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers install provider C++ syntax")
        cann_config = cann_compile_flags()
        if cann_config is None:
            self.skipTest("CANN ACL headers/libs are not configured for install provider C++ syntax check")
        cann_flags, cann_lib_dir = cann_config
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "install_provider_test.cpp"
            test_bin = temp_path / "install_provider_test"
            test_cpp.write_text(code, encoding="utf-8")
            compile_cmd = [
                compiler,
                "-std=c++14",
                "-I",
                str(INCLUDE_DIR),
                "-I",
                str(COMM_DIR),
                str(test_cpp),
                str(INSTALL_SOURCE),
                str(PROVIDER_SOURCE),
                str(PACKAGE_SOURCE),
                str(REPOSITORY_SOURCE),
                str(DRIVER_SOURCE),
                str(SPECS_SOURCE),
                str(PRODUCER_SOURCE),
                str(BARRIER_SOURCE),
                str(MICROCODE_SOURCE),
            ]
            compile_cmd.extend(cann_flags)
            compile_cmd.extend(["-o", str(test_bin)])
            subprocess.run(
                compile_cmd,
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            env = os.environ.copy()
            env["LD_LIBRARY_PATH"] = cann_lib_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")
            return subprocess.run(
                [str(test_bin)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env)

    def test_default_install_provider_reports_unsupported_without_evidence(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 10;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = true;
                if (TileXRCcuInstallHardware(request, &evidence, &installReport) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "default install provider unexpectedly succeeded\n";
                    return 2;
                }
                if (installReport.installAttempted || installReport.installSucceeded) {
                    std::cerr << "default install provider reported an attempted/succeeded install\n";
                    return 3;
                }
                if (installReport.message.find("no public no-hcomm CCU install provider") == std::string::npos) {
                    std::cerr << "weak install provider diagnostic: " << installReport.message << "\n";
                    return 4;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "default install provider filled install evidence\n";
                    return 5;
                }
                if (evidence.missionSource.kind != TileXRCcuEvidenceKind::Missing ||
                    evidence.repositorySource.kind != TileXRCcuEvidenceKind::Missing ||
                    evidence.localXnSource.kind != TileXRCcuEvidenceKind::Missing ||
                    evidence.remoteXnSource.kind != TileXRCcuEvidenceKind::Missing ||
                    evidence.notifyCkeSource.kind != TileXRCcuEvidenceKind::Missing ||
                    evidence.channelBindingSource.kind != TileXRCcuEvidenceKind::Missing) {
                    std::cerr << "default install provider did not mark sources missing\n";
                    return 6;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "unsupported install evidence became submit-ready\n";
                    return 7;
                }
                if (!submitTasks.empty() || providerReport.submitReady) {
                    std::cerr << "failed prepare leaked submit-ready state\n";
                    return 8;
                }
                if (providerReport.message.find("mission") == std::string::npos ||
                    providerReport.message.find("missing") == std::string::npos) {
                    std::cerr << "weak provider diagnostic: " << providerReport.message << "\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_request_must_match_bound_launch_scope(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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

            int ExpectScopeFailure(
                const TileXRCcuInstallRequest& request,
                const char* diagnostic)
            {
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport report;
                const int ret = TileXRCcuInstallHardware(request, &evidence, &report);
                if (ret != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "scope mismatch was not rejected: " << diagnostic << "\n";
                    return 1;
                }
                if (report.message.find(diagnostic) == std::string::npos) {
                    std::cerr << "weak scope mismatch diagnostic: " << report.message << "\n";
                    return 2;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed scope mismatch left install evidence\n";
                    return 3;
                }
                return 0;
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

                TileXRCcuInstallRequest request;
                request.package = &package;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;

                TileXRCcuInstallRequest wrongDevice = request;
                wrongDevice.deviceId = 4;
                if (ExpectScopeFailure(wrongDevice, "device scope mismatch") != 0) {
                    return 3;
                }

                TileXRCcuInstallRequest wrongRank = request;
                wrongRank.rank = 0;
                if (ExpectScopeFailure(wrongRank, "rank scope mismatch") != 0) {
                    return 4;
                }

                TileXRCcuInstallRequest wrongProvider = request;
                wrongProvider.provider = "other-public-install-provider";
                if (ExpectScopeFailure(wrongProvider, "provider scope mismatch") != 0) {
                    return 5;
                }

                TileXRCcuInstallRequest missingProvider = request;
                missingProvider.provider.clear();
                if (ExpectScopeFailure(missingProvider, "provider scope mismatch") != 0) {
                    return 6;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport report;
                if (TileXRCcuInstallHardware(request, &evidence, &report) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "matching scope should reach unsupported provider: " << report.message << "\n";
                    return 7;
                }
                if (report.message.find("no public no-hcomm CCU install provider") == std::string::npos) {
                    std::cerr << "matching scope diagnostic was weak: " << report.message << "\n";
                    return 8;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_online_install_provider_installs_repository_and_keeps_remaining_bits_missing(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                uint32_t observedOp = 0;
                uint32_t observedOffset = 0;
                uint32_t observedDataLen = 0;
                uint64_t observedResourceAddr = 0;
                bool freed = false;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void* ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (ptr != state->deviceBytes.data()) {
                    return -1;
                }
                state->freed = true;
                state->deviceBytes.clear();
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->observedOp = in.op;
                state->observedOffset = in.offsetStartIdx;
                state->observedDataLen = in.data.dataInfo.dataLen;
                state->observedResourceAddr = in.data.dataInfo.dataArray[0].insinfo.resourceAddr;
                out->opRet = 0;
                return 0;
            }

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
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 2;
                }

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 4;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;

                if (TileXRCcuInstallHardware(request, &evidence, &installReport) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "partial hardware install should stop at missing lower-layer resources: "
                              << installReport.message << "\n";
                    return 5;
                }
                if (!installReport.installAttempted || !installReport.installSucceeded ||
                    installReport.installAttemptReceiptId == 0 ||
                    installReport.publicVerifiedInstallSurfaceCount != 2 ||
                    installReport.missingInstallSurfaceCount != 4) {
                    std::cerr << "repository install report mismatch\n";
                    return 6;
                }
                if (!installReport.repository.satisfied || !installReport.mission.satisfied ||
                    installReport.localXn.satisfied || installReport.remoteXn.satisfied ||
                    installReport.notifyCke.satisfied || installReport.channelBinding.satisfied) {
                    std::cerr << "unexpected install step evidence bits\n";
                    return 7;
                }
                if (!evidence.repositoryInstalled || !evidence.missionInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "unexpected hardware evidence bits\n";
                    return 8;
                }
                if (evidence.repositorySource.kind != TileXRCcuEvidenceKind::PublicVerified ||
                    evidence.repositorySource.surface != TileXRCcuEvidenceSurface::PublicInstallProvider ||
                    evidence.repositorySource.packageFingerprint != TileXRCcuComputeLaunchPackageFingerprint(package) ||
                    evidence.repositorySource.deviceId != 3 || evidence.repositorySource.rank != 1 ||
                    evidence.repositorySource.provider != "unit-test-public-install-provider" ||
                    evidence.repositorySource.installAttemptReceiptId != installReport.installAttemptReceiptId) {
                    std::cerr << "repository evidence scope mismatch\n";
                    return 9;
                }
                if (evidence.missionSource.kind != TileXRCcuEvidenceKind::PublicVerified ||
                    evidence.missionSource.surface != TileXRCcuEvidenceSurface::PublicInstallProvider ||
                    evidence.missionSource.installAttemptReceiptId != installReport.installAttemptReceiptId ||
                    evidence.missionSource.detail.find("launch task descriptor") == std::string::npos ||
                    evidence.missionSource.detail.find("mission key") == std::string::npos) {
                    std::cerr << "mission descriptor evidence mismatch\n";
                    return 12;
                }
                if (state.observedOp != TILEXR_CCU_U_OP_SET_INSTRUCTION ||
                    state.observedOffset != 489 ||
                    state.observedDataLen != 156U * sizeof(TileXRCcuInstr) ||
                    state.observedResourceAddr != receipt.deviceInstructionAddr ||
                    receipt.instructionStartId != 489 || receipt.instructionCount != 156 ||
                    !receipt.uploaded || !receipt.installed) {
                    std::cerr << "SET_INSTRUCTION request or receipt mismatch\n";
                    return 10;
                }

                TileXRCcuRepositoryReport releaseReport;
                if (TileXRCcuReleaseRepositoryInstallReceipt(receipt, memoryOps, &state, &releaseReport) !=
                    TILEXR_SUCCESS || !state.freed) {
                    std::cerr << "release failed: " << releaseReport.message << "\n";
                    return 11;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_online_install_provider_installs_lower_layer_payloads_without_unlocking_submit_gate(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct ObservedCall {
                uint32_t op = 0;
                uint32_t die = 0;
                uint32_t offset = 0;
                uint32_t dataLen = 0;
                uint32_t arraySize = 0;
                uint8_t raw[160] = {0};
                uint32_t msId = 0;
                uint32_t tokenId = 0;
                uint32_t tokenValue = 0;
            };

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                std::vector<ObservedCall> calls;
                bool freed = false;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void* ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (ptr != state->deviceBytes.data()) {
                    return -1;
                }
                state->freed = true;
                state->deviceBytes.clear();
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                ObservedCall call;
                call.op = in.op;
                call.die = in.data.dataInfo.udieIdx;
                call.offset = in.offsetStartIdx;
                call.dataLen = in.data.dataInfo.dataLen;
                call.arraySize = in.data.dataInfo.dataArraySize;
                call.msId = in.data.dataInfo.dataArray[0].baseinfo.msId;
                call.tokenId = in.data.dataInfo.dataArray[0].baseinfo.tokenId;
                call.tokenValue = in.data.dataInfo.dataArray[0].baseinfo.tokenValue;
                std::memcpy(call.raw, in.data.dataInfo.dataArray, sizeof(call.raw));
                state->calls.push_back(call);
                out->opRet = 0;
                return 0;
            }

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
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 2;
                }

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 4;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 7;
                for (uint32_t i = 0; i < TILEXR_CCU_PFE_CTX_BYTES; ++i) {
                    pfe.ctx.raw[i] = static_cast<uint8_t>(0xa0 + i);
                }
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(2);
                for (uint32_t i = 0; i < TILEXR_CCU_LOCAL_JETTY_CTX_BYTES; ++i) {
                    jetty.ctxs[0].raw[i] = static_cast<uint8_t>(0x10 + i);
                    jetty.ctxs[1].raw[i] = static_cast<uint8_t>(0x50 + i);
                }
                lowerLayer.jettys.push_back(jetty);

                TileXRCcuChannelInstall channel;
                channel.dieId = 1;
                channel.channelId = 11;
                for (uint32_t i = 0; i < TILEXR_CCU_CHANNEL_CTX_V1_BYTES; ++i) {
                    channel.ctx.raw[i] = static_cast<uint8_t>(0xc0 + i);
                }
                lowerLayer.channels.push_back(channel);
                lowerLayer.xnClears.push_back({1, 32, 10});
                lowerLayer.ckeClears.push_back({1, 16, 10});

                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;
                request.lowerLayerPlan = &lowerLayer;

                if (TileXRCcuInstallHardware(request, &evidence, &installReport) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "partial lower-layer install should not unlock submit gate: "
                              << installReport.message << "\n";
                    return 5;
                }
                if (!installReport.installAttempted || !installReport.installSucceeded ||
                    installReport.publicVerifiedInstallSurfaceCount != 4 ||
                    installReport.missingInstallSurfaceCount != 2) {
                    std::cerr << "lower-layer install report mismatch\n";
                    return 6;
                }
                if (!evidence.repositoryInstalled || !evidence.missionInstalled || !evidence.localXnInstalled ||
                    !evidence.notifyCkeInstalled || evidence.channelBindingsInstalled ||
                    evidence.remoteXnBound) {
                    std::cerr << "unexpected evidence bits after lower-layer install\n";
                    return 7;
                }
                if (evidence.repositorySource.installAttemptReceiptId != installReport.installAttemptReceiptId ||
                    evidence.missionSource.installAttemptReceiptId != installReport.installAttemptReceiptId ||
                    evidence.localXnSource.installAttemptReceiptId != installReport.installAttemptReceiptId ||
                    evidence.notifyCkeSource.installAttemptReceiptId != installReport.installAttemptReceiptId) {
                    std::cerr << "lower-layer evidence receipts mismatch\n";
                    return 8;
                }
                if (evidence.missionSource.detail.find("launch task descriptor") == std::string::npos ||
                    evidence.missionSource.detail.find("mission key") == std::string::npos ||
                    evidence.localXnSource.detail.find("SET_XN") == std::string::npos ||
                    evidence.notifyCkeSource.detail.find("SET_CKE") == std::string::npos ||
                    evidence.channelBindingSource.kind != TileXRCcuEvidenceKind::Missing ||
                    evidence.channelBindingSource.detail.find("channel binding install evidence is missing") ==
                        std::string::npos ||
                    installReport.channelBinding.satisfied ||
                    installReport.channelBinding.message.find("channel binding endpoint route provenance was not verified") ==
                        std::string::npos) {
                    std::cerr << "lower-layer evidence detail is weak\n";
                    return 9;
                }

                if (state.calls.size() != 9 ||
                    state.calls[0].op != TILEXR_CCU_U_OP_SET_INSTRUCTION ||
                    state.calls[1].op != TILEXR_CCU_U_OP_SET_MSID_TOKEN ||
                    state.calls[2].op != TILEXR_CCU_U_OP_SET_PFE ||
                    state.calls[3].op != TILEXR_CCU_U_OP_SET_JETTY_CTX ||
                    state.calls[4].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[5].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.calls[6].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.calls[7].op != TILEXR_CCU_U_OP_SET_CKE ||
                    state.calls[8].op != TILEXR_CCU_U_OP_SET_CKE) {
                    std::cerr << "unexpected lower-layer call sequence\n";
                    return 10;
                }
                if (state.calls[1].msId != 0x45 || state.calls[1].tokenId != 0x1234 ||
                    state.calls[1].tokenValue != 0x5678) {
                    std::cerr << "MSID token request mismatch\n";
                    return 11;
                }
                if (state.calls[2].offset != 7 || state.calls[2].dataLen != TILEXR_CCU_PFE_CTX_BYTES ||
                    std::memcmp(state.calls[2].raw, pfe.ctx.raw, TILEXR_CCU_PFE_CTX_BYTES) != 0) {
                    std::cerr << "PFE request mismatch\n";
                    return 12;
                }
                if (state.calls[3].offset != 9 ||
                    state.calls[3].dataLen != 2 * TILEXR_CCU_LOCAL_JETTY_CTX_BYTES ||
                    std::memcmp(state.calls[3].raw, jetty.ctxs[0].raw, TILEXR_CCU_LOCAL_JETTY_CTX_BYTES) != 0 ||
                    std::memcmp(
                        state.calls[3].raw + TILEXR_CCU_DATA_ARRAY_SLOT_BYTES,
                        jetty.ctxs[1].raw,
                        TILEXR_CCU_LOCAL_JETTY_CTX_BYTES) != 0) {
                    std::cerr << "Jetty request mismatch\n";
                    return 13;
                }
                if (state.calls[4].offset != 11 ||
                    state.calls[4].dataLen != TILEXR_CCU_CHANNEL_CTX_V1_BYTES ||
                    std::memcmp(state.calls[4].raw, channel.ctx.raw, TILEXR_CCU_CHANNEL_CTX_V1_BYTES) != 0) {
                    std::cerr << "Channel request mismatch\n";
                    return 14;
                }
                if (state.calls[5].offset != 32 || state.calls[5].arraySize != 8 ||
                    state.calls[6].offset != 40 || state.calls[6].arraySize != 2) {
                    std::cerr << "XN batching mismatch\n";
                    return 15;
                }
                if (state.calls[7].offset != 16 || state.calls[7].arraySize != 8 ||
                    state.calls[8].offset != 24 || state.calls[8].arraySize != 2) {
                    std::cerr << "CKE batching mismatch\n";
                    return 16;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "partial lower-layer evidence became submit-ready\n";
                    return 17;
                }
                if (providerReport.message.find("remote XN") == std::string::npos ||
                    providerReport.message.find("missing") == std::string::npos) {
                    std::cerr << "submit rejection diagnostic is weak: " << providerReport.message << "\n";
                    return 18;
                }

                TileXRCcuRepositoryReport releaseReport;
                if (TileXRCcuReleaseRepositoryInstallReceipt(receipt, memoryOps, &state, &releaseReport) !=
                    TILEXR_SUCCESS || !state.freed) {
                    std::cerr << "release failed: " << releaseReport.message << "\n";
                    return 19;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_online_install_provider_promotes_remote_xn_when_peer_exchange_matches_sync_resources(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct ObservedCall {
                uint32_t op = 0;
                uint32_t offset = 0;
            };

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                std::vector<ObservedCall> calls;
                bool freed = false;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void*, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->freed = true;
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->calls.push_back({in.op, in.offsetStartIdx});
                out->opRet = 0;
                return 0;
            }

            TileXRCcuProducerPlan MakePlan()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 3};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.syncResources.push_back({1, 1962, 2362, 365, 3, 3});
                plan.syncResources.push_back({1, 1963, 2364, 366, 4, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            TileXRCcuLowerLayerInstallPlan MakeLowerLayer(bool staleRemoteXn)
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 2;
                pfe.ctx.raw[0] = 0xa0;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb0;
                lowerLayer.jettys.push_back(jetty);

                for (uint32_t i = 0; i < 3; ++i) {
                    TileXRCcuChannelInstall channel;
                    channel.dieId = 1;
                    channel.channelId = 2 + i;
                    channel.ctx.raw[0] = 0xc0 + i;
                    lowerLayer.channels.push_back(channel);
                }

                lowerLayer.xnClears.push_back({1, 1961, 3});
                lowerLayer.ckeClears.push_back({1, 332, 3});

                lowerLayer.remoteXnBindings.push_back(
                    {1, 2, 1961, staleRemoteXn ? 2369 : 2361, 364, 0, true, 0, true, true, true});
                lowerLayer.remoteXnBindings.push_back({1, 3, 1962, 2362, 365, 0, true, 0, true, true, true});
                lowerLayer.remoteXnBindings.push_back({1, 4, 1963, 2364, 366, 0, true, 0, true, true, true});
                return lowerLayer;
            }

            int RunInstall(const TileXRCcuLaunchPackage& package,
                const TileXRCcuInstallManifest& manifest,
                const TileXRCcuLowerLayerInstallPlan& lowerLayer,
                FakeState* state,
                TileXRCcuHardwareInstallEvidence* evidence,
                TileXRCcuInstallProviderReport* installReport)
            {
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 100;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = state;
                request.repositoryReceipt = &receipt;
                request.lowerLayerPlan = &lowerLayer;

                const int ret = TileXRCcuInstallHardware(request, evidence, installReport);
                TileXRCcuRepositoryReport releaseReport;
                if (receipt.deviceInstructionAddr != 0 &&
                    TileXRCcuReleaseRepositoryInstallReceipt(receipt, memoryOps, state, &releaseReport) !=
                        TILEXR_SUCCESS) {
                    std::cerr << "release failed: " << releaseReport.message << "\n";
                    return 101;
                }
                return ret;
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState staleState;
                TileXRCcuHardwareInstallEvidence staleEvidence;
                TileXRCcuInstallProviderReport staleReport;
                TileXRCcuLowerLayerInstallPlan staleLowerLayer = MakeLowerLayer(true);
                if (RunInstall(package, manifest, staleLowerLayer, &staleState, &staleEvidence, &staleReport) !=
                    TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "stale remote XN proof should not complete install\n";
                    return 4;
                }
                if (staleEvidence.remoteXnBound || staleReport.remoteXn.satisfied ||
                    staleReport.publicVerifiedInstallSurfaceCount != 5 ||
                    staleReport.missingInstallSurfaceCount != 1 ||
                    staleReport.remoteXn.message.find("remote XN peer exchange proof") == std::string::npos) {
                    std::cerr << "stale remote XN proof was accepted or weakly diagnosed: "
                              << staleReport.remoteXn.message << "\n";
                    return 5;
                }

                FakeState state;
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuLowerLayerInstallPlan lowerLayer = MakeLowerLayer(false);
                if (RunInstall(package, manifest, lowerLayer, &state, &evidence, &installReport) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "complete remote XN proof should complete install: "
                              << installReport.message << "\n";
                    return 6;
                }
                if (!installReport.installAttempted || !installReport.installSucceeded ||
                    installReport.publicVerifiedInstallSurfaceCount != 6 ||
                    installReport.missingInstallSurfaceCount != 0) {
                    std::cerr << "complete install report mismatch\n";
                    return 7;
                }
                if (!evidence.repositoryInstalled || !evidence.missionInstalled ||
                    !evidence.localXnInstalled || !evidence.remoteXnBound ||
                    !evidence.notifyCkeInstalled || !evidence.channelBindingsInstalled) {
                    std::cerr << "complete install evidence mismatch\n";
                    return 8;
                }
                if (evidence.remoteXnSource.source.find("ValidateRemoteXnExchangeBindingProof") ==
                        std::string::npos ||
                    evidence.remoteXnSource.detail.find("peer exchange") == std::string::npos ||
                    evidence.remoteXnSource.detail.find("verified endpoint route channel contexts") ==
                        std::string::npos ||
                    evidence.remoteXnSource.detail.find("channel resource owner") == std::string::npos ||
                    evidence.remoteXnSource.detail.find("transport resource exchange") == std::string::npos ||
                    !evidence.remoteXnSource.endpointRouteVerified) {
                    std::cerr << "remote XN evidence detail is weak\n";
                    return 9;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "submit preparation failed: " << providerReport.message << "\n";
                    return 10;
                }
                if (!providerReport.submitReady || submitTasks.size() != package.tasks.size()) {
                    std::cerr << "submit readiness mismatch\n";
                    return 11;
                }
                if (state.calls.size() != 9 ||
                    state.calls[0].op != TILEXR_CCU_U_OP_SET_INSTRUCTION ||
                    state.calls[4].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[5].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[6].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[7].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.calls[8].op != TILEXR_CCU_U_OP_SET_CKE) {
                    std::cerr << "unexpected complete install call sequence\n";
                    return 12;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_lower_layer_first_repository_failure_reports_installed_preconditions(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct ObservedCall {
                uint32_t op = 0;
            };

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                std::vector<ObservedCall> calls;
                bool freed = false;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void*, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->freed = true;
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->calls.push_back({in.op});
                out->opRet = in.op == TILEXR_CCU_U_OP_SET_INSTRUCTION ? 0x51 : 0;
                return 0;
            }

            TileXRCcuProducerPlan MakePlan()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 1};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 3});
                plan.syncResources.push_back({1, 1962, 2362, 365, 3, 3});
                plan.syncResources.push_back({1, 1963, 2364, 366, 4, 3});
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            TileXRCcuLowerLayerInstallPlan MakeLowerLayer()
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 2;
                pfe.ctx.raw[0] = 0xa0;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb0;
                lowerLayer.jettys.push_back(jetty);

                for (uint32_t i = 0; i < 3; ++i) {
                    TileXRCcuChannelInstall channel;
                    channel.dieId = 1;
                    channel.channelId = 2 + i;
                    channel.ctx.raw[0] = static_cast<uint8_t>(0xc0 + i);
                    lowerLayer.channels.push_back(channel);
                }

                lowerLayer.xnClears.push_back({1, 1961, 3});
                lowerLayer.ckeClears.push_back({1, 332, 3});
                lowerLayer.remoteXnBindings.push_back({1, 2, 1961, 2361, 364, 0, true, 0, true});
                lowerLayer.remoteXnBindings.push_back({1, 3, 1962, 2362, 365, 0, true, 0, true});
                lowerLayer.remoteXnBindings.push_back({1, 4, 1963, 2364, 366, 0, true, 0, true});
                return lowerLayer;
            }

            bool Contains(const std::string& text, const char* needle)
            {
                return text.find(needle) != std::string::npos;
            }

            bool Contains(const std::string& text, const std::string& needle)
            {
                return text.find(needle) != std::string::npos;
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 4;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuLowerLayerInstallPlan lowerLayer = MakeLowerLayer();
                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;
                request.installOrder = TileXRCcuInstallOrder::InstallLowerLayerFirst;
                request.lowerLayerPlan = &lowerLayer;

                if (TileXRCcuInstallHardware(request, &evidence, &installReport) != TILEXR_ERROR_MKIRT) {
                    std::cerr << "repository failure should propagate MKIRT: " << installReport.message << "\n";
                    return 5;
                }
                if (!installReport.installAttempted || installReport.installSucceeded ||
                    installReport.publicVerifiedInstallSurfaceCount != 0 ||
                    installReport.missingInstallSurfaceCount != 6) {
                    std::cerr << "repository failure report counts mismatch\n";
                    return 6;
                }
                if (evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled ||
                    evidence.channelBindingsInstalled || evidence.missionInstalled) {
                    std::cerr << "failed repository install leaked hardware evidence\n";
                    return 7;
                }
                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "failed repository install released submit gate\n";
                    return 8;
                }
                if (providerReport.submitReady || !submitTasks.empty()) {
                    std::cerr << "failed repository install leaked submit-ready state\n";
                    return 9;
                }
                if (state.calls.size() < 9 ||
                    state.calls[0].op != TILEXR_CCU_U_OP_SET_MSID_TOKEN ||
                    state.calls[1].op != TILEXR_CCU_U_OP_SET_PFE ||
                    state.calls[2].op != TILEXR_CCU_U_OP_SET_JETTY_CTX ||
                    state.calls[3].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[4].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[5].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[6].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.calls[7].op != TILEXR_CCU_U_OP_SET_CKE ||
                    state.calls[8].op != TILEXR_CCU_U_OP_SET_INSTRUCTION) {
                    std::cerr << "lower-layer-first call sequence mismatch\n";
                    return 10;
                }
                for (size_t i = 9; i < state.calls.size(); ++i) {
                    if (state.calls[i].op != TILEXR_CCU_U_OP_GET_INSTRUCTION) {
                        std::cerr << "repository failure diagnostic readback sequence mismatch\n";
                        return 10;
                    }
                }
                if (!state.freed || receipt.deviceInstructionPtr != nullptr || receipt.installed) {
                    std::cerr << "failed repository install did not release uploaded image\n";
                    return 11;
                }
                const std::string summaryNeedles[] = {
                    "lowerLayerPreconditions{",
                    "msidTokenCount=1",
                    "pfeCount=1",
                    "jettyCount=1",
                    "channelCount=3",
                    "xnClearCount=1",
                    "ckeClearCount=1",
                    "localXnInstalled=1",
                    "notifyCkeInstalled=1",
                    "channelBindingInstalled=1",
                    "msidToken0{dieId=1 msId=69 tokenId=0x1234 tokenValue=0x5678}",
                    "pfe0{dieId=1 offset=2}",
                    "jetty0{dieId=1 startJettyCtxId=9 ctxCount=1}",
                    "channel0{dieId=1 channelId=2}",
                    "xnClear0{dieId=1 startXnId=1961 count=3}",
                    "ckeClear0{dieId=1 startCkeId=332 count=3}",
                    "remoteXn0{dieId=1 channelId=2 localXn=1961 remoteXn=2361 notifyCke=364",
                };
                for (const auto& needle : summaryNeedles) {
                    if (!Contains(installReport.message, needle)) {
                        std::cerr << "missing lower-layer precondition summary: " << needle
                                  << " in " << installReport.message << "\n";
                        return 12;
                    }
                }
                if (!Contains(installReport.message, "CCU custom channel operation failed op=") ||
                    !Contains(installReport.message, "opRet=81")) {
                    std::cerr << "repository failure context is missing: " << installReport.message << "\n";
                    return 13;
                }
                if (!Contains(installReport.localXn.message, "lowerLayerPreconditions{") ||
                    !Contains(installReport.remoteXn.message, "lowerLayerPreconditions{") ||
                    !Contains(installReport.notifyCke.message, "lowerLayerPreconditions{") ||
                    !Contains(installReport.channelBinding.message, "lowerLayerPreconditions{")) {
                    std::cerr << "lower-layer step messages lost precondition summary\n";
                    return 14;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_remote_xn_proof_checks_remote_notify_and_local_wait_cke(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void*, void*)
            {
                return 0;
            }

            int FakeCustomChannel(uint32_t, const TileXRCcuCustomChannelIn&, TileXRCcuCustomChannelOut* out, void*)
            {
                out->opRet = 0;
                return 0;
            }

            TileXRCcuProducerPlan MakePlan()
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
                return plan;
            }

            TileXRCcuLowerLayerInstallPlan MakeLowerLayer(uint16_t remoteNotifyCke, uint16_t localWaitCke)
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 2;
                pfe.ctx.raw[0] = 0xa0;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb0;
                lowerLayer.jettys.push_back(jetty);

                TileXRCcuChannelInstall channel;
                channel.dieId = 1;
                channel.channelId = 2;
                channel.ctx.raw[0] = 0xc0;
                lowerLayer.channels.push_back(channel);

                lowerLayer.xnClears.push_back({1, 1961, 1});
                lowerLayer.ckeClears.push_back({1, 332, 4});

                TileXRCcuRemoteXnBindingProof proof;
                proof.dieId = 1;
                proof.channelId = 2;
                proof.localXn = 1961;
                proof.remoteXn = 2361;
                proof.notifyCke = remoteNotifyCke;
                proof.localWaitCke = localWaitCke;
                proof.peerRank = 0;
                proof.peerExchangeObserved = true;
                proof.endpointRouteVerified = true;
                proof.channelResourceOwnerVerified = true;
                proof.transportResourceExchangeVerified = true;
                lowerLayer.remoteXnBindings.push_back(proof);
                return lowerLayer;
            }

            int RunInstall(
                const TileXRCcuLaunchPackage& package,
                const TileXRCcuInstallManifest& manifest,
                const TileXRCcuLowerLayerInstallPlan& lowerLayer,
                TileXRCcuInstallProviderReport* report)
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    return TILEXR_ERROR_INTERNAL;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;
                request.lowerLayerPlan = &lowerLayer;

                TileXRCcuHardwareInstallEvidence evidence;
                return TileXRCcuInstallHardware(request, &evidence, report);
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                TileXRCcuInstallProviderReport staleLocalWait;
                if (RunInstall(package, manifest, MakeLowerLayer(364, 333), &staleLocalWait) !=
                    TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "stale local wait CKE proof was accepted\n";
                    return 4;
                }
                if (staleLocalWait.remoteXn.satisfied ||
                    staleLocalWait.remoteXn.message.find("local wait CKE") == std::string::npos) {
                    std::cerr << "local wait CKE diagnostic was weak: "
                              << staleLocalWait.remoteXn.message << "\n";
                    return 5;
                }

                TileXRCcuInstallProviderReport staleRemoteNotify;
                if (RunInstall(package, manifest, MakeLowerLayer(365, 332), &staleRemoteNotify) !=
                    TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "stale remote notify CKE proof was accepted\n";
                    return 6;
                }
                if (staleRemoteNotify.remoteXn.satisfied ||
                    staleRemoteNotify.remoteXn.message.find("remote notify CKE") == std::string::npos) {
                    std::cerr << "remote notify CKE diagnostic was weak: "
                              << staleRemoteNotify.remoteXn.message << "\n";
                    return 7;
                }

                TileXRCcuInstallProviderReport ok;
                if (RunInstall(package, manifest, MakeLowerLayer(364, 332), &ok) != TILEXR_SUCCESS ||
                    !ok.remoteXn.satisfied) {
                    std::cerr << "complete dual CKE proof was rejected: " << ok.message << "\n";
                    return 8;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_online_install_provider_rejects_stale_mission_launch_descriptor_proof(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                uint32_t observedOp = 0;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void*, void*)
            {
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->observedOp = in.op;
                out->opRet = 0;
                return 0;
            }

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
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }

                package.tasks[0].key ^= 0x1U;
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 2;
                }

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 4;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;

                if (TileXRCcuInstallHardware(request, &evidence, &installReport) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "stale mission descriptor should leave install incomplete: "
                              << installReport.message << "\n";
                    return 5;
                }
                if (!installReport.repository.satisfied || installReport.mission.satisfied ||
                    !evidence.repositoryInstalled || evidence.missionInstalled) {
                    std::cerr << "stale mission descriptor was promoted\n";
                    return 6;
                }
                if (installReport.publicVerifiedInstallSurfaceCount != 1 ||
                    installReport.missingInstallSurfaceCount != 5) {
                    std::cerr << "stale descriptor install counts mismatch\n";
                    return 7;
                }
                if (evidence.missionSource.kind != TileXRCcuEvidenceKind::Missing ||
                    installReport.mission.message.find("launch task descriptor") == std::string::npos ||
                    installReport.mission.message.find("mission key") == std::string::npos) {
                    std::cerr << "stale descriptor diagnostic is weak: "
                              << installReport.mission.message << "\n";
                    return 8;
                }
                if (state.observedOp != TILEXR_CCU_U_OP_SET_INSTRUCTION) {
                    std::cerr << "repository install did not run before descriptor proof\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_evidence_adapter_keeps_offline_candidates_not_submit_ready(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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

            TileXRCcuInstallStepEvidence Candidate(const char* source, const char* detail)
            {
                TileXRCcuInstallStepEvidence step;
                step.satisfied = true;
                step.source.kind = TileXRCcuEvidenceKind::PublicCandidate;
                step.source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                step.source.source = source;
                step.source.detail = detail;
                step.message = detail;
                return step;
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
                    return 8;
                }

                TileXRCcuInstallProviderReport installReport;
                installReport.offlineOnly = true;
                installReport.mission = Candidate("scan:mission", "public-looking mission candidate");
                installReport.repository = Candidate("scan:repository", "public-looking repository candidate");
                installReport.localXn = Candidate("scan:local-xn", "public-looking local XN candidate");
                installReport.remoteXn = Candidate("scan:remote-xn", "public-looking remote XN candidate");
                installReport.notifyCke = Candidate("scan:notify-cke", "public-looking notify CKE candidate");
                installReport.channelBinding = Candidate("scan:channel", "public-looking channel candidate");

                TileXRCcuHardwareInstallEvidence evidence;
                if (TileXRCcuBuildInstallEvidence(installReport, &evidence) != TILEXR_SUCCESS) {
                    std::cerr << "adapter rejected candidate report\n";
                    return 2;
                }
                if (!evidence.missionInstalled || !evidence.repositoryInstalled || !evidence.localXnInstalled ||
                    !evidence.remoteXnBound || !evidence.notifyCkeInstalled || !evidence.channelBindingsInstalled) {
                    std::cerr << "adapter dropped candidate evidence unexpectedly\n";
                    return 3;
                }
                if (evidence.missionSource.kind != TileXRCcuEvidenceKind::PublicCandidate ||
                    evidence.channelBindingSource.kind != TileXRCcuEvidenceKind::PublicCandidate) {
                    std::cerr << "adapter upgraded/downgraded candidate evidence unexpectedly\n";
                    return 4;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "offline public candidates became submit-ready\n";
                    return 5;
                }
                if (!submitTasks.empty() || providerReport.submitReady) {
                    std::cerr << "failed candidate prepare leaked submit-ready state\n";
                    return 6;
                }
                if (providerReport.message.find("candidate") == std::string::npos ||
                    providerReport.message.find("public install provider") == std::string::npos) {
                    std::cerr << "weak candidate diagnostic: " << providerReport.message << "\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_evidence_adapter_does_not_accept_offline_public_verified_claims(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuInstallStepEvidence ForgedVerified(const char* source, const char* detail)
            {
                TileXRCcuInstallStepEvidence step;
                step.satisfied = true;
                step.source.kind = TileXRCcuEvidenceKind::PublicVerified;
                step.source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                step.source.source = source;
                step.source.detail = detail;
                step.message = detail;
                return step;
            }

            int main()
            {
                TileXRCcuInstallProviderReport installReport;
                installReport.offlineOnly = true;
                installReport.mission = ForgedVerified("forged:mission", "forged mission/key evidence");
                installReport.repository = ForgedVerified("forged:repository", "forged repository evidence");
                installReport.localXn = ForgedVerified("forged:local-xn", "forged local XN evidence");
                installReport.remoteXn = ForgedVerified("forged:remote-xn", "forged remote XN evidence");
                installReport.notifyCke = ForgedVerified("forged:notify-cke", "forged notify CKE evidence");
                installReport.channelBinding = ForgedVerified("forged:channel", "forged channel binding evidence");

                TileXRCcuHardwareInstallEvidence evidence;
                if (TileXRCcuBuildInstallEvidence(installReport, &evidence) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "offline public verified install claims were accepted\n";
                    return 1;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed offline verified adapter left install evidence\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_evidence_adapter_requires_consistent_online_receipt(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuInstallStepEvidence VerifiedStep(
                const char* source,
                const char* detail,
                uint64_t receipt)
            {
                TileXRCcuInstallStepEvidence step;
                step.satisfied = true;
                step.source.kind = TileXRCcuEvidenceKind::PublicVerified;
                step.source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                step.source.packageFingerprint = 0xfeed001ULL;
                step.source.deviceId = 3;
                step.source.rank = 1;
                step.source.provider = "unit-test-public-install-provider";
                step.source.source = source;
                step.source.detail = detail;
                step.source.installAttemptReceiptId = receipt;
                step.message = detail;
                return step;
            }

            TileXRCcuInstallProviderReport VerifiedReport(uint64_t receipt)
            {
                TileXRCcuInstallProviderReport report;
                report.offlineOnly = false;
                report.installAttempted = true;
                report.installSucceeded = true;
                report.installAttemptReceiptId = receipt;
                report.mission = VerifiedStep("provider:mission", "mission/key installed", receipt);
                report.repository = VerifiedStep("provider:repository", "repository installed", receipt);
                report.localXn = VerifiedStep("provider:local-xn", "local XN installed", receipt);
                report.remoteXn = VerifiedStep("provider:remote-xn", "remote XN bound", receipt);
                report.notifyCke = VerifiedStep("provider:notify-cke", "notify CKE installed", receipt);
                report.channelBinding = VerifiedStep("provider:channel", "channel binding installed", receipt);
                return report;
            }

            int main()
            {
                TileXRCcuHardwareInstallEvidence evidence;

                TileXRCcuInstallProviderReport missingReceipt = VerifiedReport(0);
                if (TileXRCcuBuildInstallEvidence(missingReceipt, &evidence) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "online public verified report with missing receipt was accepted\n";
                    return 1;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed missing receipt adapter left install evidence\n";
                    return 2;
                }

                TileXRCcuInstallProviderReport mixedReceipt = VerifiedReport(0xabc001ULL);
                mixedReceipt.remoteXn.source.installAttemptReceiptId = 0xabc002ULL;
                if (TileXRCcuBuildInstallEvidence(mixedReceipt, &evidence) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "online public verified report with mixed receipts was accepted\n";
                    return 3;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed mixed receipt adapter left install evidence\n";
                    return 4;
                }

                TileXRCcuInstallProviderReport notAttempted = VerifiedReport(0xabc001ULL);
                notAttempted.installAttempted = false;
                if (TileXRCcuBuildInstallEvidence(notAttempted, &evidence) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "online public verified report without attempted install was accepted\n";
                    return 5;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed not-attempted adapter left install evidence\n";
                    return 6;
                }

                TileXRCcuInstallProviderReport notSucceeded = VerifiedReport(0xabc001ULL);
                notSucceeded.installSucceeded = false;
                if (TileXRCcuBuildInstallEvidence(notSucceeded, &evidence) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "online public verified report without successful install was accepted\n";
                    return 7;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed not-succeeded adapter left install evidence\n";
                    return 8;
                }

                TileXRCcuInstallProviderReport matchingReceipt = VerifiedReport(0xabc001ULL);
                if (TileXRCcuBuildInstallEvidence(matchingReceipt, &evidence) != TILEXR_SUCCESS) {
                    std::cerr << "matching receipt report was rejected\n";
                    return 9;
                }
                if (evidence.missionSource.installAttemptReceiptId != 0xabc001ULL ||
                    evidence.repositorySource.installAttemptReceiptId != 0xabc001ULL ||
                    evidence.localXnSource.installAttemptReceiptId != 0xabc001ULL ||
                    evidence.remoteXnSource.installAttemptReceiptId != 0xabc001ULL ||
                    evidence.notifyCkeSource.installAttemptReceiptId != 0xabc001ULL ||
                    evidence.channelBindingSource.installAttemptReceiptId != 0xabc001ULL) {
                    std::cerr << "adapter did not preserve matching receipt ids\n";
                    return 10;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_manifest_from_launch_package_lists_all_required_surfaces(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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

            const TileXRCcuInstallRequirement* FindRequirement(
                const TileXRCcuInstallManifest& manifest,
                TileXRCcuInstallRequirementKind kind)
            {
                for (const auto& requirement : manifest.requirements) {
                    if (requirement.kind == kind) {
                        return &requirement;
                    }
                }
                return nullptr;
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                const uint64_t fingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
                if (manifest.deviceId != 3 || manifest.rank != 1 ||
                    manifest.provider != "unit-test-public-install-provider" ||
                    manifest.packageFingerprint != fingerprint ||
                    !manifest.requiresHardwareInstall ||
                    !manifest.installAttemptReceiptRequired ||
                    manifest.requiredEvidenceKind != TileXRCcuEvidenceKind::PublicVerified ||
                    manifest.requiredEvidenceSurface != TileXRCcuEvidenceSurface::PublicInstallProvider) {
                    std::cerr << "manifest did not preserve scope/fingerprint/evidence contract\n";
                    return 4;
                }
                if (manifest.requirements.size() != 6 || manifestReport.requirementCount != 6 ||
                    manifestReport.message != "ok") {
                    std::cerr << "manifest did not list the six required install surfaces\n";
                    return 5;
                }

                const TileXRCcuInstallRequirement* mission =
                    FindRequirement(manifest, TileXRCcuInstallRequirementKind::MissionKey);
                const TileXRCcuInstallRequirement* repository =
                    FindRequirement(manifest, TileXRCcuInstallRequirementKind::RepositoryImage);
                const TileXRCcuInstallRequirement* localXn =
                    FindRequirement(manifest, TileXRCcuInstallRequirementKind::LocalXn);
                const TileXRCcuInstallRequirement* remoteXn =
                    FindRequirement(manifest, TileXRCcuInstallRequirementKind::RemoteXnBinding);
                const TileXRCcuInstallRequirement* notifyCke =
                    FindRequirement(manifest, TileXRCcuInstallRequirementKind::NotifyCke);
                const TileXRCcuInstallRequirement* channel =
                    FindRequirement(manifest, TileXRCcuInstallRequirementKind::ChannelBinding);
                if (mission == nullptr || repository == nullptr || localXn == nullptr ||
                    remoteXn == nullptr || notifyCke == nullptr || channel == nullptr) {
                    std::cerr << "manifest missed one or more required install kinds\n";
                    return 6;
                }
                if (mission->missionId != 6 || mission->missionKey != 0x059b0f03U ||
                    mission->resourceStartId != 6 || mission->resourceCount != 1) {
                    std::cerr << "manifest mission/key requirement is incomplete\n";
                    return 7;
                }
                if (repository->repositoryStartId != 475 || repository->repositoryCount != 170 ||
                    repository->missionStartId != 489 || repository->missionCount != 156 ||
                    repository->instructionCount != 170) {
                    std::cerr << "manifest repository requirement is incomplete\n";
                    return 8;
                }
                if (localXn->resourceStartId != 1961 || localXn->resourceCount != 62) {
                    std::cerr << "manifest local XN requirement is incomplete\n";
                    return 9;
                }
                if (remoteXn->syncResourceCount != 3 || notifyCke->syncResourceCount != 3 ||
                    channel->syncResourceCount != 3 || channel->bindingCount != 9) {
                    std::cerr << "manifest sync resource requirements are incomplete\n";
                    return 10;
                }
                for (const auto& requirement : manifest.requirements) {
                    if (requirement.packageFingerprint != fingerprint ||
                        requirement.label.empty() ||
                        requirement.detail.empty()) {
                        std::cerr << "manifest requirement missed audit metadata\n";
                        return 11;
                    }
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_manifest_rejects_unbound_or_stale_scope_without_partial_output(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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

            int ExpectManifestFailure(
                const TileXRCcuLaunchPackage& package,
                const char* diagnostic)
            {
                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport report;
                const int ret = TileXRCcuBuildInstallManifest(package, &manifest, &report);
                if (ret != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "manifest build unexpectedly succeeded for " << diagnostic << "\n";
                    return 1;
                }
                if (report.message.find(diagnostic) == std::string::npos) {
                    std::cerr << "weak manifest diagnostic: " << report.message << "\n";
                    return 2;
                }
                if (!manifest.requirements.empty() || report.requirementCount != 0) {
                    std::cerr << "failed manifest build left partial requirements\n";
                    return 3;
                }
                return 0;
            }

            int main()
            {
                TileXRCcuLaunchPackage package;
                TileXRCcuLaunchPackageReport packageReport;
                if (TileXRCcuBuildLaunchPackage(MakePlan(), &package, &packageReport) != TILEXR_SUCCESS) {
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }
                if (ExpectManifestFailure(package, "launch install scope is not bound") != 0) {
                    return 2;
                }

                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 3;
                }
                package.tasks[0].key ^= 0x1U;
                if (ExpectManifestFailure(package, "launch install scope is stale") != 0) {
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_manifest_contract_does_not_make_default_provider_submit_ready(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 2;
                }
                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                if (TileXRCcuInstallHardware(request, &evidence, &installReport) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "default install provider accepted manifest as installed evidence\n";
                    return 4;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "manifest leaked into install evidence\n";
                    return 5;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "manifest-only default provider became submit-ready\n";
                    return 6;
                }
                if (!submitTasks.empty() || providerReport.submitReady ||
                    providerReport.missingEvidenceCount == 0) {
                    std::cerr << "failed manifest-only prepare leaked submit-ready state\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_manifest_default_provider_reports_missing_required_surfaces(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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
                    std::cerr << "package build failed: " << packageReport.message << "\n";
                    return 1;
                }
                if (TileXRCcuBindLaunchPackageInstallScope(
                        &package, 3, 1, "unit-test-public-install-provider") != TILEXR_SUCCESS) {
                    std::cerr << "scope bind failed\n";
                    return 2;
                }

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;

                if (TileXRCcuInstallHardware(request, &evidence, &installReport) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "default provider did not stop at unsupported install path\n";
                    return 4;
                }
                if (installReport.requiredInstallSurfaceCount != 6 ||
                    installReport.publicVerifiedInstallSurfaceCount != 0 ||
                    installReport.missingInstallSurfaceCount != 6) {
                    std::cerr << "manifest requirement counts were not reported: required="
                              << installReport.requiredInstallSurfaceCount
                              << " verified=" << installReport.publicVerifiedInstallSurfaceCount
                              << " missing=" << installReport.missingInstallSurfaceCount << "\n";
                    return 5;
                }
                if (installReport.message.find("no public no-hcomm CCU install provider") == std::string::npos) {
                    std::cerr << "weak unsupported provider diagnostic: " << installReport.message << "\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_request_rejects_mismatched_manifest_scope(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

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

            int ExpectManifestFailure(
                const TileXRCcuInstallRequest& request,
                const char* diagnostic)
            {
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport report;
                const int ret = TileXRCcuInstallHardware(request, &evidence, &report);
                if (ret != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "manifest mismatch was not rejected: " << diagnostic << "\n";
                    return 1;
                }
                if (report.message.find(diagnostic) == std::string::npos) {
                    std::cerr << "weak manifest mismatch diagnostic: " << report.message << "\n";
                    return 2;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed manifest mismatch left install evidence\n";
                    return 3;
                }
                return 0;
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;

                TileXRCcuInstallManifest wrongDevice = manifest;
                wrongDevice.deviceId = 4;
                request.manifest = &wrongDevice;
                if (ExpectManifestFailure(request, "install manifest device mismatch") != 0) {
                    return 4;
                }

                TileXRCcuInstallManifest wrongFingerprint = manifest;
                wrongFingerprint.packageFingerprint ^= 0x1ULL;
                request.manifest = &wrongFingerprint;
                if (ExpectManifestFailure(request, "install manifest fingerprint mismatch") != 0) {
                    return 5;
                }

                TileXRCcuInstallManifest wrongRequirementKind = manifest;
                wrongRequirementKind.requirements[5].kind = wrongRequirementKind.requirements[0].kind;
                request.manifest = &wrongRequirementKind;
                if (ExpectManifestFailure(request, "install manifest requirement kind mismatch") != 0) {
                    return 6;
                }

                TileXRCcuInstallManifest wrongHardwareRequirement = manifest;
                wrongHardwareRequirement.requiresHardwareInstall = false;
                request.manifest = &wrongHardwareRequirement;
                if (ExpectManifestFailure(request, "install manifest hardware requirement mismatch") != 0) {
                    return 7;
                }

                TileXRCcuInstallManifest wrongMission = manifest;
                wrongMission.requirements[0].missionKey ^= 0x1U;
                request.manifest = &wrongMission;
                if (ExpectManifestFailure(request, "install manifest mission requirement mismatch") != 0) {
                    return 8;
                }

                TileXRCcuInstallManifest wrongChannel = manifest;
                wrongChannel.requirements[5].bindingCount = 1;
                request.manifest = &wrongChannel;
                if (ExpectManifestFailure(request, "install manifest channel requirement mismatch") != 0) {
                    return 9;
                }

                request.manifest = &manifest;
                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport report;
                if (TileXRCcuInstallHardware(request, &evidence, &report) != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "matching manifest should reach unsupported provider: " << report.message << "\n";
                    return 10;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_evidence_adapter_rejects_public_verified_with_bad_surface_or_scope(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <iostream>

            using namespace TileXR;

            TileXRCcuInstallStepEvidence VerifiedStep(
                const char* source,
                const char* detail,
                uint64_t receipt)
            {
                TileXRCcuInstallStepEvidence step;
                step.satisfied = true;
                step.source.kind = TileXRCcuEvidenceKind::PublicVerified;
                step.source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
                step.source.packageFingerprint = 0xfeed001ULL;
                step.source.deviceId = 3;
                step.source.rank = 1;
                step.source.provider = "unit-test-public-install-provider";
                step.source.installAttemptReceiptId = receipt;
                step.source.source = source;
                step.source.detail = detail;
                step.message = detail;
                return step;
            }

            TileXRCcuInstallProviderReport VerifiedReport(uint64_t receipt)
            {
                TileXRCcuInstallProviderReport report;
                report.offlineOnly = false;
                report.installAttempted = true;
                report.installSucceeded = true;
                report.installAttemptReceiptId = receipt;
                report.mission = VerifiedStep("provider:mission", "mission/key installed", receipt);
                report.repository = VerifiedStep("provider:repository", "repository installed", receipt);
                report.localXn = VerifiedStep("provider:local-xn", "local XN installed", receipt);
                report.remoteXn = VerifiedStep("provider:remote-xn", "remote XN bound", receipt);
                report.notifyCke = VerifiedStep("provider:notify-cke", "notify CKE installed", receipt);
                report.channelBinding = VerifiedStep("provider:channel", "channel binding installed", receipt);
                return report;
            }

            int ExpectAdapterFailure(const TileXRCcuInstallProviderReport& report, const char* diagnostic)
            {
                TileXRCcuHardwareInstallEvidence evidence;
                const int ret = TileXRCcuBuildInstallEvidence(report, &evidence);
                if (ret != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "invalid public verified evidence was accepted: " << diagnostic << "\n";
                    return 1;
                }
                if (evidence.missionInstalled || evidence.repositoryInstalled || evidence.localXnInstalled ||
                    evidence.remoteXnBound || evidence.notifyCkeInstalled || evidence.channelBindingsInstalled) {
                    std::cerr << "failed adapter left invalid public verified evidence\n";
                    return 2;
                }
                return 0;
            }

            int main()
            {
                TileXRCcuInstallProviderReport wrongSurface = VerifiedReport(0xabc001ULL);
                wrongSurface.mission.source.surface = TileXRCcuEvidenceSurface::LowerLayerResourceHelper;
                if (ExpectAdapterFailure(wrongSurface, "surface mismatch") != 0) {
                    return 1;
                }

                TileXRCcuInstallProviderReport missingScope = VerifiedReport(0xabc001ULL);
                missingScope.repository.source.packageFingerprint = 0;
                if (ExpectAdapterFailure(missingScope, "missing fingerprint") != 0) {
                    return 2;
                }

                TileXRCcuInstallProviderReport missingProvider = VerifiedReport(0xabc001ULL);
                missingProvider.localXn.source.provider.clear();
                if (ExpectAdapterFailure(missingProvider, "missing provider") != 0) {
                    return 3;
                }

                TileXRCcuInstallProviderReport missingDetail = VerifiedReport(0xabc001ULL);
                missingDetail.remoteXn.source.detail.clear();
                if (ExpectAdapterFailure(missingDetail, "missing source/detail") != 0) {
                    return 4;
                }

                TileXRCcuHardwareInstallEvidence evidence;
                if (TileXRCcuBuildInstallEvidence(VerifiedReport(0xabc001ULL), &evidence) != TILEXR_SUCCESS) {
                    std::cerr << "valid public verified evidence was rejected\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_peer_exchange_without_endpoint_route_provenance_does_not_submit(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void*, void*)
            {
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn&,
                TileXRCcuCustomChannelOut* out,
                void*)
            {
                out->opRet = 0;
                return 0;
            }

            TileXRCcuProducerPlan MakePlan()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 3};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                plan.syncResources.push_back({1, 1961, 2361, 364, 2, 1});
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            TileXRCcuLowerLayerInstallPlan MakeSyntheticLowerLayer()
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 2;
                pfe.ctx.raw[0] = 0xa0;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb0;
                lowerLayer.jettys.push_back(jetty);

                TileXRCcuChannelInstall channel;
                channel.dieId = 1;
                channel.channelId = 2;
                channel.ctx.raw[0] = 0xc0;
                lowerLayer.channels.push_back(channel);

                lowerLayer.xnClears.push_back({1, 1961, 1});
                lowerLayer.ckeClears.push_back({1, 332, 1});

                TileXRCcuRemoteXnBindingProof proof;
                proof.dieId = 1;
                proof.channelId = 2;
                proof.localXn = 1961;
                proof.remoteXn = 2361;
                proof.notifyCke = 364;
                proof.peerRank = 0;
                proof.peerExchangeObserved = true;
                lowerLayer.remoteXnBindings.push_back(proof);
                return lowerLayer;
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 4;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuLowerLayerInstallPlan lowerLayer = MakeSyntheticLowerLayer();
                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;
                request.lowerLayerPlan = &lowerLayer;

                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                const int ret = TileXRCcuInstallHardware(request, &evidence, &installReport);
                if (ret != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "synthetic endpoint route unexpectedly completed install: "
                              << installReport.message << "\n";
                    return 5;
                }
                if (evidence.remoteXnBound || evidence.channelBindingsInstalled ||
                    installReport.remoteXn.satisfied || installReport.channelBinding.satisfied ||
                    installReport.publicVerifiedInstallSurfaceCount != 4 ||
                    installReport.missingInstallSurfaceCount != 2) {
                    std::cerr << "synthetic endpoint route produced submit evidence\n";
                    return 6;
                }
                if (installReport.remoteXn.message.find("endpoint route") == std::string::npos ||
                    installReport.channelBinding.message.find("endpoint route") == std::string::npos) {
                    std::cerr << "weak synthetic endpoint diagnostic remote=\""
                              << installReport.remoteXn.message << "\" channel=\""
                              << installReport.channelBinding.message << "\"\n";
                    return 7;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "synthetic endpoint route became submit-ready\n";
                    return 8;
                }
                if (!submitTasks.empty() || providerReport.submitReady) {
                    std::cerr << "submit-ready state leaked from synthetic endpoint route\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_peer_exchange_with_endpoint_route_but_without_channel_resource_binding_does_not_submit(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void*, void*)
            {
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn&,
                TileXRCcuCustomChannelOut* out,
                void*)
            {
                out->opRet = 0;
                return 0;
            }

            TileXRCcuProducerPlan MakePlan()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 3};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                TileXRCcuSyncResource resource;
                resource.dieId = 1;
                resource.localXn = 1961;
                resource.remoteXn = 2361;
                resource.notifyCke = 364;
                resource.channelId = 2;
                resource.bindingCount = 1;
                resource.localWaitCke = 332;
                plan.syncResources.push_back(resource);
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                return plan;
            }

            TileXRCcuLowerLayerInstallPlan MakeEndpointOnlyLowerLayer()
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 2;
                pfe.ctx.raw[0] = 0xa0;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb0;
                lowerLayer.jettys.push_back(jetty);

                TileXRCcuChannelInstall channel;
                channel.dieId = 1;
                channel.channelId = 2;
                channel.ctx.raw[0] = 0xc0;
                lowerLayer.channels.push_back(channel);

                lowerLayer.xnClears.push_back({1, 1961, 1});
                lowerLayer.ckeClears.push_back({1, 332, 3});

                TileXRCcuRemoteXnBindingProof proof;
                proof.dieId = 1;
                proof.channelId = 2;
                proof.localXn = 1961;
                proof.remoteXn = 2361;
                proof.notifyCke = 364;
                proof.peerRank = 0;
                proof.peerExchangeObserved = true;
                proof.localWaitCke = 332;
                proof.endpointRouteVerified = true;
                lowerLayer.remoteXnBindings.push_back(proof);
                return lowerLayer;
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 4;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuLowerLayerInstallPlan lowerLayer = MakeEndpointOnlyLowerLayer();
                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;
                request.lowerLayerPlan = &lowerLayer;

                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                const int ret = TileXRCcuInstallHardware(request, &evidence, &installReport);
                if (ret != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "endpoint-only remote XN proof unexpectedly completed install: "
                              << installReport.message << "\n";
                    return 5;
                }
                if (evidence.remoteXnBound || evidence.channelBindingsInstalled ||
                    installReport.remoteXn.satisfied || installReport.channelBinding.satisfied) {
                    std::cerr << "endpoint-only remote XN proof produced submit evidence\n";
                    return 6;
                }
                if (installReport.remoteXn.message.find("channel resource owner") == std::string::npos ||
                    installReport.channelBinding.message.find("channel resource owner") == std::string::npos) {
                    std::cerr << "weak endpoint-only diagnostic remote=\""
                              << installReport.remoteXn.message << "\" channel=\""
                              << installReport.channelBinding.message << "\"\n";
                    return 7;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "endpoint-only remote XN proof became submit-ready\n";
                    return 8;
                }
                if (!submitTasks.empty() || providerReport.submitReady) {
                    std::cerr << "submit-ready state leaked from endpoint-only remote XN proof\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_peer_exchange_with_channel_owner_but_without_transport_exchange_does_not_submit(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_install_provider.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size() ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeFree(void*, void*)
            {
                return 0;
            }

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn&,
                TileXRCcuCustomChannelOut* out,
                void*)
            {
                out->opRet = 0;
                return 0;
            }

            TileXRCcuProducerPlan MakePlan()
            {
                TileXRCcuProducerPlan plan;
                plan.mission = {1, 6, 0x059b0f03U, true};
                plan.kernelLocalXn = {1, 1961, 62};
                plan.kernelLocalCke = {1, 332, 3};
                plan.kernelLocalMission = {1, 6, 1};
                plan.instructionWindow = {1, 475, 170, 489, 156};
                TileXRCcuSyncResource resource;
                resource.dieId = 1;
                resource.localXn = 1961;
                resource.remoteXn = 2361;
                resource.notifyCke = 364;
                resource.channelId = 2;
                resource.bindingCount = 1;
                resource.localWaitCke = 332;
                plan.syncResources.push_back(resource);
                plan.taskWindows.push_back({1, 489, 13, 13, {}});
                return plan;
            }

            TileXRCcuLowerLayerInstallPlan MakeLowerLayer()
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 2;
                pfe.ctx.raw[0] = 0xa0;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb0;
                lowerLayer.jettys.push_back(jetty);

                TileXRCcuChannelInstall channel;
                channel.dieId = 1;
                channel.channelId = 2;
                channel.ctx.raw[0] = 0xc0;
                lowerLayer.channels.push_back(channel);

                lowerLayer.xnClears.push_back({1, 1961, 1});
                lowerLayer.ckeClears.push_back({1, 332, 3});

                TileXRCcuRemoteXnBindingProof proof;
                proof.dieId = 1;
                proof.channelId = 2;
                proof.localXn = 1961;
                proof.remoteXn = 2361;
                proof.notifyCke = 364;
                proof.peerRank = 0;
                proof.peerExchangeObserved = true;
                proof.localWaitCke = 332;
                proof.endpointRouteVerified = true;
                proof.channelResourceOwnerVerified = true;
                proof.transportResourceExchangeVerified = false;
                lowerLayer.remoteXnBindings.push_back(proof);
                return lowerLayer;
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

                TileXRCcuInstallManifest manifest;
                TileXRCcuInstallManifestReport manifestReport;
                if (TileXRCcuBuildInstallManifest(package, &manifest, &manifestReport) != TILEXR_SUCCESS) {
                    std::cerr << "manifest build failed: " << manifestReport.message << "\n";
                    return 3;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 4;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuLowerLayerInstallPlan lowerLayer = MakeLowerLayer();
                TileXRCcuRepositoryInstallReceipt receipt;
                TileXRCcuInstallRequest request;
                request.package = &package;
                request.manifest = &manifest;
                request.deviceId = 3;
                request.rank = 1;
                request.provider = "unit-test-public-install-provider";
                request.offlineOnly = false;
                request.driverAdapter = &adapter;
                request.repositoryMemoryOps = memoryOps;
                request.repositoryMemoryUserData = &state;
                request.repositoryReceipt = &receipt;
                request.lowerLayerPlan = &lowerLayer;

                TileXRCcuHardwareInstallEvidence evidence;
                TileXRCcuInstallProviderReport installReport;
                const int ret = TileXRCcuInstallHardware(request, &evidence, &installReport);
                if (ret != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "missing transport exchange proof unexpectedly completed install: "
                              << installReport.message << "\n";
                    return 5;
                }
                if (evidence.remoteXnBound || evidence.channelBindingsInstalled ||
                    installReport.remoteXn.satisfied || installReport.channelBinding.satisfied) {
                    std::cerr << "missing transport exchange proof produced submit evidence\n";
                    return 6;
                }
                if (installReport.remoteXn.message.find("transport resource exchange") == std::string::npos ||
                    installReport.channelBinding.message.find("transport resource exchange") == std::string::npos) {
                    std::cerr << "weak transport-exchange diagnostic remote=\""
                              << installReport.remoteXn.message << "\" channel=\""
                              << installReport.channelBinding.message << "\"\n";
                    return 7;
                }

                std::vector<TileXRCcuTask> submitTasks;
                TileXRCcuProviderReport providerReport;
                if (TileXRCcuPrepareSubmitTasks(package, evidence, &submitTasks, &providerReport) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing transport exchange proof became submit-ready\n";
                    return 8;
                }
                if (!submitTasks.empty() || providerReport.submitReady) {
                    std::cerr << "submit-ready state leaked from missing transport exchange proof\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_install_provider_layer_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = INSTALL_HEADER.read_text(encoding="utf-8")
        source = INSTALL_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_install_provider.h", cmake)
        self.assertIn("ccu/tilexr_ccu_install_provider.cpp", cmake)
        self.assertIn("struct TileXRCcuInstallRequest", header)
        self.assertIn("struct TileXRCcuInstallStepEvidence", header)
        self.assertIn("struct TileXRCcuInstallProviderReport", header)
        self.assertIn("TileXRCcuBuildInstallEvidence", header)
        self.assertIn("TileXRCcuInstallHardware", header)
        self.assertIn("TileXRCcuHardwareInstallEvidence", header)
        self.assertIn("deviceId", header)
        self.assertIn("rank", header)
        self.assertIn("provider", header)
        self.assertIn("offlineOnly", header)
        self.assertIn("driverAdapter", header)
        self.assertIn("repositoryMemoryOps", header)
        self.assertIn("repositoryMemoryUserData", header)
        self.assertIn("repositoryInstallOptions", header)
        self.assertIn("repositoryReceipt", header)
        self.assertIn("TileXRCcuInstallOrder", header)
        self.assertIn("installOrder", header)
        self.assertIn("TileXRCcuLowerLayerInstallPlan", header)
        self.assertIn("TileXRCcuMsidTokenInstall", header)
        self.assertIn("TileXRCcuPfeInstall", header)
        self.assertIn("TileXRCcuJettyInstall", header)
        self.assertIn("TileXRCcuChannelInstall", header)
        self.assertIn("TileXRCcuCkeClearInstall", header)
        self.assertIn("TileXRCcuXnClearInstall", header)
        self.assertIn("TileXRCcuRemoteXnBindingProof", header)
        self.assertIn("localWaitCke", header)
        self.assertIn("lowerLayerPlan", header)
        self.assertIn("xnClears", header)
        self.assertIn("remoteXnBindings", header)
        self.assertIn("installAttempted", header)
        self.assertIn("installSucceeded", header)
        self.assertIn("installAttemptReceiptId", header)
        self.assertIn("TileXRCcuInstallRepositoryImage", source)
        self.assertIn("TileXRCcuInstallRepositoryImageWithOptions", source)
        self.assertIn("InstallLowerLayerResources", source)
        self.assertIn("InstallLowerLayerFirst", source)
        self.assertIn("ValidateRemoteXnExchangeBindingProof", source)
        self.assertIn("local wait CKE", source)
        self.assertIn("remote notify CKE", source)
        self.assertIn("InstallMsidToken", source)
        self.assertIn("InstallPfeCtx", source)
        self.assertIn("InstallJettyCtx", source)
        self.assertIn("InstallChannelCtxV1", source)
        self.assertIn("const uint32_t expectedChannelCount", source)
        self.assertIn("CountInstalledRemoteBindingChannels", source)
        self.assertIn("installedChannelCount >= expectedChannelCount", source)
        self.assertIn("InstallXnRange", source)
        self.assertIn("ClearCkeRange", source)
        self.assertIn("BuildRepositoryInstallReceiptId", source)
        self.assertIn("repository instruction image installed via SET_INSTRUCTION", source)
        self.assertIn("ValidateInstallRequestScope", source)
        self.assertIn("ValidateInstallReceipt", source)
        self.assertIn("install attempt did not succeed", source)
        self.assertIn("device scope mismatch", source)
        self.assertIn("rank scope mismatch", source)
        self.assertIn("provider scope mismatch", source)
        self.assertIn("no public no-hcomm CCU install provider", source)
        self.assertIn("offline install evidence cannot be public verified", source)
        self.assertIn("install attempt receipt", source)
        self.assertIn("RejectOfflinePublicVerified", source)
        self.assertIn("TileXRCcuInstallManifest", header)
        self.assertIn("TileXRCcuInstallRequirement", header)
        self.assertIn("TileXRCcuBuildInstallManifest", header)
        self.assertIn("ValidateInstallManifestScope", source)
        self.assertIn("ValidateInstallRequestManifest", source)
        self.assertIn("install manifest fingerprint mismatch", source)
        self.assertIn("install manifest requirement kind mismatch", source)
        self.assertIn("install manifest hardware requirement mismatch", source)
        self.assertIn("install manifest mission requirement mismatch", source)
        self.assertIn("ValidateMissionLaunchDescriptorProof", source)
        self.assertIn("launch task descriptor", source)
        self.assertIn("install manifest channel requirement mismatch", source)
        self.assertIn("ValidatePublicVerifiedStepScope", source)
        self.assertIn("public verified evidence scope is incomplete", source)
        self.assertIn("launch install scope is stale", source)

        combined = header + "\n" + source
        self.assertNotIn("SET_MISSION_CTX", combined)
        self.assertNotIn("TILEXR_CCU_U_OP_SET_MISSION_CTX", combined)
        self.assertNotIn("258", combined)
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
