#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import os
import platform
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DIRECT_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_direct_orchestrator.h"
DIRECT_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_direct_orchestrator.cpp"
INSTALL_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_install_provider.cpp"
PROVIDER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_provider.cpp"
PACKAGE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_launch_package.cpp"
REPOSITORY_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_repository.cpp"
DRIVER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp"
SPECS_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp"
ALLOCATOR_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_resource_allocator.cpp"
PRODUCER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_producer_plan.cpp"
BARRIER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_barrier_program.cpp"
MICROCODE_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.cpp"
MEMORY_PROGRAM_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_memory_program.cpp"
RUNTIME_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime.cpp"
LOWER_LAYER_PLAN_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_plan_builder.cpp"
LOWER_LAYER_PAYLOAD_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_payloads.cpp"
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
    if not (lib_dir / "libascendcl.so").exists():
        return None

    flags = []
    for include_dir in include_dirs:
        if include_dir.exists():
            flags.extend(["-I", str(include_dir)])
    flags.extend(["-L", str(lib_dir), f"-Wl,-rpath-link,{lib_dir}", "-lascendcl", "-lruntime"])
    return flags, str(lib_dir)


class TileXRCcuDirectOrchestratorTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers direct orchestrator syntax")
        cann_config = cann_compile_flags()
        if cann_config is None:
            self.skipTest("CANN ACL headers/libs are not configured for direct orchestrator C++ syntax check")
        cann_flags, cann_lib_dir = cann_config
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "direct_orchestrator_test.cpp"
            test_bin = temp_path / "direct_orchestrator_test"
            test_cpp.write_text(code, encoding="utf-8")
            compile_cmd = [
                compiler,
                "-std=c++14",
                "-I",
                str(INCLUDE_DIR),
                "-I",
                str(COMM_DIR),
                str(test_cpp),
                str(DIRECT_SOURCE),
                str(INSTALL_SOURCE),
                str(PROVIDER_SOURCE),
                str(PACKAGE_SOURCE),
                str(REPOSITORY_SOURCE),
                str(DRIVER_SOURCE),
                str(SPECS_SOURCE),
                str(ALLOCATOR_SOURCE),
                str(PRODUCER_SOURCE),
                str(BARRIER_SOURCE),
                str(MICROCODE_SOURCE),
                str(MEMORY_PROGRAM_SOURCE),
                str(RUNTIME_SOURCE),
                str(LOWER_LAYER_PLAN_SOURCE),
                str(LOWER_LAYER_PAYLOAD_SOURCE),
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

    def test_direct_install_attempt_builds_and_installs_package_until_known_missing_surfaces(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"
            #include "ccu/tilexr_ccu_lower_layer_payloads.h"

            #include <cstdlib>
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
                state->calls.push_back(call);
                out->opRet = 0;
                return 0;
            }

            TileXRCcuLowerLayerInstallPlan MakeLowerLayer()
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 7;
                pfe.ctx.raw[0] = 0xa1;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb2;
                lowerLayer.jettys.push_back(jetty);

                TileXRCcuChannelInstall channel;
                channel.dieId = 1;
                channel.channelId = 11;
                channel.ctx.raw[0] = 0xc3;
                lowerLayer.channels.push_back(channel);
                lowerLayer.xnClears.push_back({1, 32, 1});
                lowerLayer.ckeClears.push_back({1, 16, 1});
                return lowerLayer;
            }

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.valid = true;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0x5678;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                TileXRCcuDirectInstallOptions options;
                options.basicInfo = &basic;
                options.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                options.syncResourceCount = 3;
                options.syncInstructionCount = 143;
                options.bindingsPerSyncResource = 3;
                options.deviceId = 3;
                options.rank = 1;
                options.provider = "unit-test-direct-install-provider";
                options.missionStartId = 6;
                options.instructionStartId = 475;
                options.xnStartId = 1961;
                options.ckeStartId = 332;
                options.channelStartId = 2;

                TileXRCcuLowerLayerInstallPlan lowerLayer = MakeLowerLayer();
                options.lowerLayerPlan = &lowerLayer;

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 1;
                }
                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;
                options.driverAdapter = &adapter;
                options.repositoryMemoryOps = memoryOps;
                options.repositoryMemoryUserData = &state;

                TileXRCcuDirectInstallAttempt attempt;
                TileXRCcuDirectInstallReport report;
                const int ret = TileXRCcuRunDirectInstallAttempt(options, &attempt, &report);
                if (ret != TILEXR_ERROR_NOT_FOUND) {
                    std::cerr << "direct install attempt should stop at known missing surfaces: "
                              << report.message << "\n";
                    return 2;
                }
                if (!report.pipelineBuilt || !report.installAttempted || report.submitReady ||
                    report.submitTaskCount != 0 || report.missingInstallSurfaceCount != 2 ||
                    report.publicVerifiedInstallSurfaceCount != 4) {
                    std::cerr << "direct install report mismatch\n";
                    return 3;
                }
                if (report.message.find("remote XN install provider is missing") ==
                    std::string::npos) {
                    std::cerr << "direct install diagnostic is weak: " << report.message << "\n";
                    return 4;
                }
                if (attempt.package.tasks.size() != 2 || attempt.manifest.requirements.size() != 6 ||
                    attempt.allocation.receiptId == 0 ||
                    attempt.repositoryReceipt.instructionStartId != attempt.package.repository.missionStartId ||
                    attempt.repositoryReceipt.instructionCount != attempt.package.repository.missionCount ||
                    attempt.package.tasks[0].instStartId != attempt.package.repository.missionStartId ||
                    attempt.package.tasks[0].instCnt != 13 ||
                    attempt.package.tasks[1].instStartId !=
                        attempt.package.repository.missionStartId + attempt.package.repository.sqeLoadCount ||
                    attempt.package.tasks[1].instCnt !=
                        attempt.package.repository.missionCount - attempt.package.repository.sqeLoadCount) {
                    std::cerr << "attempt artifacts mismatch"
                              << " taskSize=" << attempt.package.tasks.size()
                              << " reqSize=" << attempt.manifest.requirements.size()
                              << " receipt=" << attempt.allocation.receiptId
                              << " receiptStart=" << attempt.repositoryReceipt.instructionStartId
                              << " missionStart=" << attempt.package.repository.missionStartId
                              << " receiptCount=" << attempt.repositoryReceipt.instructionCount
                              << " missionCount=" << attempt.package.repository.missionCount
                              << " task0Start=" << attempt.package.tasks[0].instStartId
                              << " task0Cnt=" << attempt.package.tasks[0].instCnt
                              << " task1Start=" << attempt.package.tasks[1].instStartId
                              << " expectedTask1Start="
                              << attempt.package.repository.missionStartId + attempt.package.repository.sqeLoadCount
                              << " task1Cnt=" << attempt.package.tasks[1].instCnt
                              << " expectedTask1Cnt="
                              << attempt.package.repository.missionCount - attempt.package.repository.sqeLoadCount
                              << "\n";
                    return 5;
                }
                if (!attempt.evidence.repositoryInstalled || !attempt.evidence.missionInstalled ||
                    !attempt.evidence.localXnInstalled ||
                    !attempt.evidence.notifyCkeInstalled ||
                    attempt.evidence.channelBindingsInstalled ||
                    attempt.evidence.remoteXnBound) {
                    std::cerr << "attempt evidence mismatch\n";
                    return 6;
                }
                if (state.calls.size() != 7 ||
                    state.calls[0].op != TILEXR_CCU_U_OP_SET_MSID_TOKEN ||
                    state.calls[1].op != TILEXR_CCU_U_OP_SET_PFE ||
                    state.calls[2].op != TILEXR_CCU_U_OP_SET_JETTY_CTX ||
                    state.calls[3].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[4].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.calls[5].op != TILEXR_CCU_U_OP_SET_CKE ||
                    state.calls[6].op != TILEXR_CCU_U_OP_SET_INSTRUCTION) {
                    std::cerr << "unexpected direct install call sequence\n";
                    return 7;
                }
                if (state.calls[6].offset != attempt.package.repository.missionStartId ||
                    state.calls[6].dataLen !=
                        attempt.package.repository.missionCount * sizeof(TileXRCcuInstr)) {
                    std::cerr << "repository install range mismatch\n";
                    return 8;
                }
                if (state.calls[0].offset != 0 || state.calls[0].dataLen != 0 ||
                    state.calls[0].arraySize != 0) {
                    std::cerr << "MSID token install envelope mismatch\n";
                    return 10;
                }
                if (TileXRCcuReleaseRepositoryInstallReceipt(
                        attempt.repositoryReceipt, memoryOps, &state, &attempt.repositoryReleaseReport) !=
                    TILEXR_SUCCESS || !state.freed) {
                    std::cerr << "repository release failed: " << attempt.repositoryReleaseReport.message << "\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_install_attempt_becomes_submit_ready_with_remote_xn_peer_exchange_proof(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct ObservedCall {
                uint32_t op = 0;
                uint32_t offset = 0;
                uint32_t arraySize = 0;
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
                state->calls.push_back({in.op, in.offsetStartIdx, in.data.dataInfo.dataArraySize});
                out->opRet = 0;
                return 0;
            }

            TileXRCcuLowerLayerInstallPlan MakeCompleteLowerLayer()
            {
                TileXRCcuLowerLayerInstallPlan lowerLayer;
                lowerLayer.msidTokens.push_back({1, 0x45, 0x1234, 0x5678});

                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = 2;
                pfe.ctx.raw[0] = 0xa1;
                lowerLayer.pfes.push_back(pfe);

                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb2;
                lowerLayer.jettys.push_back(jetty);

                for (uint32_t i = 0; i < 3; ++i) {
                    TileXRCcuChannelInstall channel;
                    channel.dieId = 1;
                    channel.channelId = 2 + i;
                    channel.ctx.raw[0] = 0xc3 + i;
                    lowerLayer.channels.push_back(channel);
                }

                lowerLayer.xnClears.push_back({1, 1961, 14});
                lowerLayer.ckeClears.push_back({1, 332, 3});
                lowerLayer.remoteXnBindings.push_back({1, 2, 1961, 1975, 332, 0, true, 0, true, true, true});
                lowerLayer.remoteXnBindings.push_back({1, 3, 1962, 1976, 333, 0, true, 0, true, true, true});
                lowerLayer.remoteXnBindings.push_back({1, 4, 1963, 1977, 334, 0, true, 0, true, true, true});
                return lowerLayer;
            }

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.valid = true;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0x5678;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                TileXRCcuDirectInstallOptions options;
                options.basicInfo = &basic;
                options.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                options.syncResourceCount = 3;
                options.syncInstructionCount = 143;
                options.bindingsPerSyncResource = 3;
                options.deviceId = 3;
                options.rank = 1;
                options.provider = "unit-test-direct-install-provider";
                options.missionStartId = 6;
                options.instructionStartId = 475;
                options.xnStartId = 1961;
                options.ckeStartId = 332;
                options.channelStartId = 2;
                TileXRCcuLowerLayerInstallPlan lowerLayer = MakeCompleteLowerLayer();
                options.lowerLayerPlan = &lowerLayer;

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 1;
                }
                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;
                options.driverAdapter = &adapter;
                options.repositoryMemoryOps = memoryOps;
                options.repositoryMemoryUserData = &state;

                TileXRCcuDirectInstallAttempt attempt;
                TileXRCcuDirectInstallReport report;
                const int ret = TileXRCcuRunDirectInstallAttempt(options, &attempt, &report);
                if (ret != TILEXR_SUCCESS) {
                    std::cerr << "direct install attempt should be submit-ready: "
                              << report.message << "\n";
                    return 2;
                }
                if (!report.pipelineBuilt || !report.installAttempted || !report.installSucceeded ||
                    !report.submitReady || report.missingInstallSurfaceCount != 0 ||
                    report.publicVerifiedInstallSurfaceCount != 6 ||
                    report.submitTaskCount != attempt.package.tasks.size()) {
                    std::cerr << "submit-ready direct install report mismatch\n";
                    return 3;
                }
                if (!attempt.evidence.repositoryInstalled || !attempt.evidence.missionInstalled ||
                    !attempt.evidence.localXnInstalled || !attempt.evidence.remoteXnBound ||
                    !attempt.evidence.notifyCkeInstalled || !attempt.evidence.channelBindingsInstalled) {
                    std::cerr << "submit-ready evidence mismatch\n";
                    return 4;
                }
                if (attempt.evidence.remoteXnSource.source.find("ValidateRemoteXnExchangeBindingProof") ==
                        std::string::npos ||
                    attempt.evidence.remoteXnSource.detail.find("syncXn") == std::string::npos) {
                    std::cerr << "remote XN evidence detail is weak\n";
                    return 5;
                }
                if (state.calls.size() != 10 ||
                    state.calls[0].op != TILEXR_CCU_U_OP_SET_MSID_TOKEN ||
                    state.calls[1].op != TILEXR_CCU_U_OP_SET_PFE ||
                    state.calls[2].op != TILEXR_CCU_U_OP_SET_JETTY_CTX ||
                    state.calls[3].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[4].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[5].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.calls[6].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.calls[6].arraySize != 8 ||
                    state.calls[7].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.calls[7].arraySize != 6 ||
                    state.calls[8].op != TILEXR_CCU_U_OP_SET_CKE ||
                    state.calls[8].arraySize != 3 ||
                    state.calls[9].op != TILEXR_CCU_U_OP_SET_INSTRUCTION) {
                    std::cerr << "submit-ready call sequence mismatch\n";
                    return 6;
                }
                if (TileXRCcuReleaseRepositoryInstallReceipt(
                        attempt.repositoryReceipt, memoryOps, &state, &attempt.repositoryReleaseReport) !=
                    TILEXR_SUCCESS || !state.freed) {
                    std::cerr << "repository release failed: " << attempt.repositoryReleaseReport.message << "\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_submit_tasks_runs_submitter_in_order_and_stops_on_failure(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct SubmitState {
                std::vector<uint16_t> starts;
                int failOnCall = 0;
            };

            int FakeSubmit(const TileXRCcuTask& task, void* stream, void* userData)
            {
                if (stream != reinterpret_cast<void*>(0x1234)) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                auto* state = static_cast<SubmitState*>(userData);
                state->starts.push_back(task.instStartId);
                if (state->failOnCall != 0 &&
                    state->starts.size() == static_cast<size_t>(state->failOnCall)) {
                    return TILEXR_ERROR_MKIRT;
                }
                return TILEXR_SUCCESS;
            }

            TileXRCcuTask Task(uint16_t start)
            {
                TileXRCcuTask task;
                task.dieId = 1;
                task.missionId = 6;
                task.instStartId = start;
                task.instCnt = 13;
                task.key = 0x059b0f03U;
                task.argSize = TILEXR_CCU_SQE_ARGS_LEN;
                return task;
            }

            int main()
            {
                std::vector<TileXRCcuTask> tasks;
                tasks.push_back(Task(489));
                tasks.push_back(Task(502));

                SubmitState okState;
                TileXRCcuDirectSubmitReport report;
                if (TileXRCcuSubmitPreparedTasks(
                        tasks, reinterpret_cast<void*>(0x1234), FakeSubmit, &okState, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "submit batch failed: " << report.message << "\n";
                    return 1;
                }
                if (!report.submitted || report.taskCount != 2 || report.submittedTaskCount != 2 ||
                    okState.starts.size() != 2 || okState.starts[0] != 489 || okState.starts[1] != 502 ||
                    report.message.find("submitted") == std::string::npos) {
                    std::cerr << "submit success report mismatch\n";
                    return 2;
                }

                SubmitState failState;
                failState.failOnCall = 2;
                if (TileXRCcuSubmitPreparedTasks(
                        tasks, reinterpret_cast<void*>(0x1234), FakeSubmit, &failState, &report) !=
                    TILEXR_ERROR_MKIRT) {
                    std::cerr << "submit failure was not propagated\n";
                    return 3;
                }
                if (report.submitted || report.taskCount != 2 || report.submittedTaskCount != 1 ||
                    failState.starts.size() != 2 ||
                    report.message.find("task=1") == std::string::npos ||
                    report.message.find("ret=-2") == std::string::npos ||
                    report.message.find("missionId=6") == std::string::npos ||
                    report.message.find("instStartId=502") == std::string::npos ||
                    report.message.find("key=0x59b0f03") == std::string::npos ||
                    report.message.find("argSize=13") == std::string::npos) {
                    std::cerr << "submit failure report mismatch: " << report.message << "\n";
                    return 4;
                }

                if (TileXRCcuSubmitPreparedTasks(
                        tasks, nullptr, FakeSubmit, &okState, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null stream accepted\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_submit_default_runtime_trace_indexes_final_tasks(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

            #include <cstdlib>
            #include <cstring>
            #include <iostream>
            #include <vector>

            #include <runtime/kernel.h>

            using namespace TileXR;

            namespace {
            int g_launchCount = 0;
            }

            extern "C" rtError_t rtCCULaunch(rtCcuTaskInfo_t*, rtStream_t)
            {
                ++g_launchCount;
                return RT_ERROR_NONE;
            }

            TileXRCcuTask Task(uint16_t start)
            {
                TileXRCcuTask task;
                task.dieId = 1;
                task.missionId = 6;
                task.timeout = 68;
                task.instStartId = start;
                task.instCnt = 13;
                task.key = 0x059b0f03U;
                task.argSize = TILEXR_CCU_SQE_ARGS_LEN;
                task.args[0] = 0x1000ULL + start;
                return task;
            }

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_TRACE", "1", 1);

                std::vector<TileXRCcuTask> tasks;
                tasks.push_back(Task(489));
                tasks.push_back(Task(502));

                TileXRCcuDirectSubmitReport report;
                const int ret = TileXRCcuSubmitPreparedTasks(
                    tasks, reinterpret_cast<void*>(0x1234), nullptr, nullptr, &report);
                if (ret != TILEXR_SUCCESS || !report.submitted || g_launchCount != 2) {
                    std::cerr << "submit mismatch ret=" << ret
                              << " submitted=" << report.submitted
                              << " launchCount=" << g_launchCount
                              << " message=" << report.message << "\n";
                    return 1;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("TileXRDirectCcuTrace finalRuntimeTask[0] dieId=1 missionId=6 timeout=68 instStartId=489", result.stderr)
        self.assertIn("TileXRDirectCcuTrace finalRuntimeTask[1] dieId=1 missionId=6 timeout=68 instStartId=502", result.stderr)
        self.assertNotIn("TileXRDirectCcuTrace finalRuntimeTask task=0", result.stderr)

    def test_direct_install_attempt_can_prepare_lower_layer_plan_after_allocation(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                std::vector<uint32_t> ops;
            };

            struct CallbackState {
                uint32_t callCount = 0;
                uint32_t syncResourceCount = 0;
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
                state->ops.push_back(in.op);
                out->opRet = 0;
                return 0;
            }

            int PrepareLowerLayerPlan(
                const TileXRCcuResourceAllocation& allocation,
                TileXRCcuLowerLayerInstallPlan* plan,
                TileXRCcuLowerLayerPlanBuilderReport* report,
                void* userData)
            {
                auto* state = static_cast<CallbackState*>(userData);
                ++state->callCount;
                state->syncResourceCount = allocation.remoteXn.num;
                if (plan == nullptr || report == nullptr) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                *plan = TileXRCcuLowerLayerInstallPlan{};
                plan->msidTokens.push_back({1, 0x45, 0x1234, 0x5678});
                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = allocation.channels.startId;
                pfe.ctx.raw[0] = 0xa1;
                plan->pfes.push_back(pfe);
                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb2;
                plan->jettys.push_back(jetty);
                TileXRCcuLowerLayerPayloadReport payloadReport;
                for (uint32_t i = 0; i < allocation.channels.num; ++i) {
                    TileXRCcuChannelInstall channel;
                    channel.dieId = 1;
                    channel.channelId = allocation.channels.startId + i;
                    TileXRCcuChannelCtxV1Spec channelSpec;
                    channelSpec.remoteEid[0] = static_cast<uint8_t>(0x10 + i);
                    channelSpec.tpn = 1;
                    channelSpec.sourcePfeId = allocation.channels.startId;
                    channelSpec.startJettyId = 1024;
                    channelSpec.jettyCount = 1;
                    channelSpec.dieId = 1;
                    channelSpec.memoryTokenId = 0x2345;
                    channelSpec.memoryTokenValue = 0x67890000U;
                    channelSpec.remoteCcuVa = 0x0000100054800000ULL;
                    if (TileXRCcuBuildChannelCtxV1(channelSpec, &channel.ctx, &payloadReport) != TILEXR_SUCCESS) {
                        report->message = payloadReport.message;
                        return TILEXR_ERROR_PARA_CHECK_FAIL;
                    }
                    plan->channels.push_back(channel);
                }
                plan->xnClears.push_back({1, allocation.localXn.startId, allocation.localXn.num});
                plan->ckeClears.push_back({1, allocation.notifyCke.startId, allocation.notifyCke.num});
                for (uint32_t i = 0; i < allocation.remoteXn.num; ++i) {
                    plan->remoteXnBindings.push_back({
                        1,
                        static_cast<uint16_t>(allocation.channels.startId + i),
                        static_cast<uint16_t>(allocation.localXn.startId + i),
                        static_cast<uint16_t>(allocation.remoteXn.startId + i),
                        static_cast<uint16_t>(allocation.notifyCke.startId + i),
                        i,
                        true,
                        static_cast<uint16_t>(allocation.localWaitCke.startId + i),
                        true,
                        true,
                        true});
                }
                report->msidTokenCount = 1;
                report->pfeCount = 1;
                report->jettyCount = 1;
                report->localJettyCtxCount = 1;
                report->channelCount = allocation.channels.num;
                report->ckeClearCount = 1;
                report->message = "callback lower-layer plan prepared";
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.valid = true;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0x5678;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                FakeState fake;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &fake, &driverReport) != TILEXR_SUCCESS) {
                    return 1;
                }

                TileXRCcuDirectInstallOptions options;
                options.basicInfo = &basic;
                options.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                options.syncResourceCount = 3;
                options.syncInstructionCount = 143;
                options.bindingsPerSyncResource = 3;
                options.deviceId = 3;
                options.rank = 1;
                options.provider = "unit-test-callback-provider";
                options.missionStartId = 6;
                options.instructionStartId = 475;
                options.xnStartId = 1961;
                options.gsaStartId = 510;
                options.remoteXnStartId = 2361;
                options.remoteXnCount = 8;
                options.ckeStartId = 332;
                options.remoteNotifyCkeStartId = 364;
                options.remoteNotifyCkeCount = 8;
                options.channelStartId = 2;
                options.driverAdapter = &adapter;
                options.repositoryMemoryOps = {FakeAlloc, FakeCopy, FakeFree};
                options.repositoryMemoryUserData = &fake;

                CallbackState callback;
                options.prepareLowerLayerPlan = PrepareLowerLayerPlan;
                options.lowerLayerPlanUserData = &callback;

                TileXRCcuDirectInstallAttempt attempt;
                TileXRCcuDirectInstallReport report;
                const int ret = TileXRCcuRunDirectInstallAttempt(options, &attempt, &report);
                if (ret != TILEXR_SUCCESS || !report.submitReady || report.submitTaskCount != 2) {
                    std::cerr << "callback install should be submit-ready: " << report.message << "\n";
                    return 2;
                }
                if (callback.callCount != 1 || callback.syncResourceCount != 3) {
                    std::cerr << "lower-layer callback was not called with allocation\n";
                    return 3;
                }
                if (attempt.plan.kernelLocalGsa.startId != 510 || attempt.plan.kernelLocalGsa.num != 1 ||
                    attempt.allocation.localGsa.startId != 510 || attempt.allocation.localGsa.num != 1) {
                    std::cerr << "GSA resource did not flow into direct CCU producer plan\n";
                    return 13;
                }
                if (attempt.preparedLowerLayerPlan.remoteXnBindings.size() != 3 ||
                    attempt.lowerLayerPlanReport.message.find("callback") == std::string::npos) {
                    std::cerr << "prepared lower-layer artifacts were not retained\n";
                    return 4;
                }
                if (!attempt.evidence.remoteXnBound || !attempt.evidence.channelBindingsInstalled ||
                    attempt.evidence.remoteXnSource.source.find("ValidateRemoteXnExchangeBindingProof") ==
                        std::string::npos) {
                    std::cerr << "prepared lower-layer evidence mismatch\n";
                    return 5;
                }
                const uint64_t expectedPackedToken = (1ULL << 52U) | (0x1234ULL << 32U) | 0x5678ULL;
                if (attempt.package.plan.taskWindows[0].args.size() != TILEXR_CCU_SQE_ARGS_LEN ||
                    attempt.package.plan.taskWindows[0].args[0] != basic.resourceAddr ||
                    attempt.package.plan.taskWindows[0].args[1] != basic.resourceAddr ||
                    attempt.package.plan.taskWindows[0].args[2] != expectedPackedToken ||
                    attempt.package.plan.taskWindows[0].args[3] != 0x0000100054800000ULL ||
                    attempt.submitTasks[0].args[0] != basic.resourceAddr ||
                    attempt.submitTasks[0].args[1] != basic.resourceAddr ||
                    attempt.submitTasks[0].args[2] != expectedPackedToken ||
                    attempt.submitTasks[0].args[3] != 0x0000100054800000ULL) {
                    std::cerr << "hcomm-style SQE task args were not generated from lower-layer token state\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_install_attempt_passes_split_cke_ranges_to_lower_layer_callback(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
            };

            struct CallbackState {
                bool sawSplitCke = false;
                uint16_t localWaitStart = 0;
                uint16_t remoteNotifyStart = 0;
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

            int PrepareSplitLowerLayerPlan(
                const TileXRCcuResourceAllocation& allocation,
                TileXRCcuLowerLayerInstallPlan* plan,
                TileXRCcuLowerLayerPlanBuilderReport* report,
                void* userData)
            {
                auto* state = static_cast<CallbackState*>(userData);
                state->localWaitStart = allocation.localWaitCke.startId;
                state->remoteNotifyStart = allocation.remoteNotifyCke.startId;
                state->sawSplitCke =
                    allocation.localWaitCke.startId == 332 &&
                    allocation.localWaitCke.num == 3 &&
                    allocation.remoteNotifyCke.startId == 364 &&
                    allocation.remoteNotifyCke.num == 3 &&
                    allocation.notifyCke.startId == 364 &&
                    allocation.notifyCke.num == 3;
                if (plan == nullptr || report == nullptr || !state->sawSplitCke) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }

                *plan = TileXRCcuLowerLayerInstallPlan{};
                plan->msidTokens.push_back({1, 0x45, 0x1234, 0x5678});
                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = allocation.channels.startId;
                pfe.ctx.raw[0] = 0xa1;
                plan->pfes.push_back(pfe);
                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb2;
                plan->jettys.push_back(jetty);
                for (uint32_t i = 0; i < allocation.channels.num; ++i) {
                    TileXRCcuChannelInstall channel;
                    channel.dieId = 1;
                    channel.channelId = allocation.channels.startId + i;
                    channel.ctx.raw[0] = 0xc3 + i;
                    plan->channels.push_back(channel);
                }
                plan->xnClears.push_back({1, allocation.localXn.startId, allocation.localXn.num});
                plan->ckeClears.push_back({1, allocation.localWaitCke.startId, allocation.localWaitCke.num});
                for (uint32_t i = 0; i < allocation.remoteXn.num; ++i) {
                    plan->remoteXnBindings.push_back({
                        1,
                        allocation.channels.startId + i,
                        static_cast<uint16_t>(allocation.localXn.startId + i),
                        static_cast<uint16_t>(allocation.remoteXn.startId + i),
                        static_cast<uint16_t>(allocation.remoteNotifyCke.startId + i),
                        i,
                        true,
                        static_cast<uint16_t>(allocation.localWaitCke.startId + i),
                        true,
                        true,
                        true});
                }
                report->message = "split CKE lower-layer plan prepared";
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.valid = true;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0x5678;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                FakeState fake;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &fake, &driverReport) != TILEXR_SUCCESS) {
                    return 1;
                }

                TileXRCcuDirectInstallOptions options;
                options.basicInfo = &basic;
                options.syncResourceCount = 3;
                options.syncInstructionCount = 143;
                options.bindingsPerSyncResource = 3;
                options.deviceId = 3;
                options.rank = 1;
                options.provider = "unit-test-split-cke-provider";
                options.missionStartId = 6;
                options.instructionStartId = 475;
                options.xnStartId = 1961;
                options.ckeStartId = 332;
                options.localWaitCkeStartId = 332;
                options.remoteNotifyCkeStartId = 364;
                options.channelStartId = 2;
                options.driverAdapter = &adapter;
                options.repositoryMemoryOps = {FakeAlloc, FakeCopy, FakeFree};
                options.repositoryMemoryUserData = &fake;

                CallbackState callback;
                options.prepareLowerLayerPlan = PrepareSplitLowerLayerPlan;
                options.lowerLayerPlanUserData = &callback;

                TileXRCcuDirectInstallAttempt attempt;
                TileXRCcuDirectInstallReport report;
                const int ret = TileXRCcuRunDirectInstallAttempt(options, &attempt, &report);
                if (ret != TILEXR_SUCCESS || !report.submitReady) {
                    std::cerr << "split CKE direct install should be submit-ready: "
                              << report.message << "\n";
                    return 2;
                }
                if (!callback.sawSplitCke || callback.localWaitStart != 332 ||
                    callback.remoteNotifyStart != 364) {
                    std::cerr << "split CKE allocation was not passed to callback\n";
                    return 3;
                }
                if (attempt.plan.kernelLocalCke.startId != 332 ||
                    attempt.plan.syncResources[0].localWaitCke != 332 ||
                    attempt.plan.syncResources[0].notifyCke != 364 ||
                    attempt.evidence.remoteXnSource.detail.find("syncXn") == std::string::npos) {
                    std::cerr << "split CKE plan or evidence mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_install_attempt_rewrites_sync_resources_from_peer_lower_layer_proof(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

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

            int PreparePeerLowerLayerPlan(
                const TileXRCcuResourceAllocation& allocation,
                TileXRCcuLowerLayerInstallPlan* plan,
                TileXRCcuLowerLayerPlanBuilderReport* report,
                void*)
            {
                if (plan == nullptr || report == nullptr) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                *plan = TileXRCcuLowerLayerInstallPlan{};
                plan->msidTokens.push_back({1, 0x45, 0x1234, 0x5678});
                TileXRCcuPfeInstall pfe;
                pfe.dieId = 1;
                pfe.pfeOffset = allocation.channels.startId;
                pfe.ctx.raw[0] = 0xa1;
                plan->pfes.push_back(pfe);
                TileXRCcuJettyInstall jetty;
                jetty.dieId = 1;
                jetty.startJettyCtxId = 9;
                jetty.ctxs.resize(1);
                jetty.ctxs[0].raw[0] = 0xb2;
                plan->jettys.push_back(jetty);
                for (uint32_t i = 0; i < allocation.channels.num; ++i) {
                    TileXRCcuChannelInstall channel;
                    channel.dieId = 1;
                    channel.channelId = allocation.channels.startId + i;
                    channel.ctx.raw[0] = 0xc3 + i;
                    plan->channels.push_back(channel);
                }
                plan->xnClears.push_back({1, allocation.localXn.startId, allocation.localXn.num});
                plan->ckeClears.push_back({1, allocation.localWaitCke.startId, allocation.localWaitCke.num});

                for (uint32_t i = 0; i < allocation.remoteXn.num; ++i) {
                    const uint16_t peerLocalXn = static_cast<uint16_t>(0x1a0 + i);
                    const uint16_t peerNotifyCke = static_cast<uint16_t>(0x360 + i);
                    plan->remoteXnBindings.push_back({
                        1,
                        allocation.channels.startId + i,
                        static_cast<uint16_t>(allocation.localXn.startId + i),
                        peerLocalXn,
                        peerNotifyCke,
                        i,
                        true,
                        static_cast<uint16_t>(allocation.localWaitCke.startId + i),
                        true,
                        true,
                        true});
                }
                report->message = "peer lower-layer plan prepared";
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.valid = true;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0x5678;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                FakeState fake;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &fake, &driverReport) != TILEXR_SUCCESS) {
                    return 1;
                }

                TileXRCcuDirectInstallOptions options;
                options.basicInfo = &basic;
                options.sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
                options.syncResourceCount = 2;
                options.syncInstructionCount = 9;
                options.bindingsPerSyncResource = 1;
                options.deviceId = 3;
                options.rank = 1;
                options.provider = "unit-test-peer-xn-rewrite-provider";
                options.missionStartId = 6;
                options.instructionStartId = 475;
                options.xnStartId = 0x2a0;
                options.ckeStartId = 0x220;
                options.localWaitCkeStartId = 0x220;
                options.remoteNotifyCkeStartId = 0x360;
                options.channelStartId = 2;
                options.driverAdapter = &adapter;
                options.repositoryMemoryOps = {FakeAlloc, FakeCopy, FakeFree};
                options.repositoryMemoryUserData = &fake;
                options.prepareLowerLayerPlan = PreparePeerLowerLayerPlan;

                TileXRCcuDirectInstallAttempt attempt;
                TileXRCcuDirectInstallReport report;
                const int ret = TileXRCcuRunDirectInstallAttempt(options, &attempt, &report);
                if (ret != TILEXR_SUCCESS || !report.submitReady) {
                    std::cerr << "peer XN rewrite should make direct install submit-ready: "
                              << report.message << "\n";
                    return 2;
                }
                if (attempt.plan.syncResources.size() != 2 ||
                    attempt.plan.syncResources[0].remoteXn != 0x1a0 ||
                    attempt.plan.syncResources[1].remoteXn != 0x1a1 ||
                    attempt.plan.syncResources[0].notifyCke != 0x360 ||
                    attempt.plan.syncResources[1].notifyCke != 0x361) {
                    std::cerr << "producer plan was not rewritten from peer lower-layer proof\n";
                    return 3;
                }
                if (attempt.package.plan.syncResources[0].remoteXn != 0x1a0 ||
                    attempt.package.plan.syncResources[1].remoteXn != 0x1a1 ||
                    attempt.package.plan.syncResources[0].notifyCke != 0x360 ||
                    attempt.package.plan.syncResources[1].notifyCke != 0x361 ||
                    attempt.submitTasks.size() != 2 ||
                    attempt.submitTasks[0].instStartId != attempt.package.repository.missionStartId ||
                    attempt.submitTasks[0].instCnt != 13 ||
                    attempt.submitTasks[1].instStartId !=
                        attempt.package.repository.missionStartId + attempt.package.repository.sqeLoadCount ||
                    attempt.submitTasks[1].instCnt !=
                        attempt.package.repository.missionCount - attempt.package.repository.sqeLoadCount) {
                    std::cerr << "launch package did not use peer lower-layer proof resources\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_install_attempt_release_helper_frees_repository_receipt_once(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

            #include <iostream>

            using namespace TileXR;

            struct FakeState {
                int freeCalls = 0;
                int payload = 0;
            };

            int FakeFree(void* ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (ptr != &state->payload) {
                    return -1;
                }
                ++state->freeCalls;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDirectInstallAttempt attempt;
                attempt.repositoryMemoryOps.free = FakeFree;
                attempt.repositoryMemoryUserData = &state;
                attempt.repositoryReceipt.deviceInstructionPtr = &state.payload;
                attempt.repositoryReceipt.deviceInstructionAddr =
                    reinterpret_cast<uint64_t>(&state.payload);
                attempt.repositoryReceipt.uploaded = true;
                attempt.repositoryReceipt.installed = true;

                if (TileXRCcuReleaseDirectInstallAttemptResources(attempt) != TILEXR_SUCCESS) {
                    std::cerr << "first release failed: " << attempt.repositoryReleaseReport.message << "\n";
                    return 1;
                }
                if (state.freeCalls != 1 || attempt.repositoryReceipt.deviceInstructionPtr != nullptr ||
                    attempt.repositoryReleaseReport.message != "ok") {
                    std::cerr << "release did not clear repository receipt\n";
                    return 2;
                }
                if (TileXRCcuReleaseDirectInstallAttemptResources(attempt) != TILEXR_SUCCESS ||
                    state.freeCalls != 1) {
                    std::cerr << "release helper is not idempotent\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_install_attempt_trace_dumps_final_peer_resources_and_tasks_when_enabled(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"

            #include <cstdlib>
            #include <iostream>

            using namespace TileXR;

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_TRACE", "1", 1);

                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.valid = true;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0x5678;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                TileXRCcuDirectInstallOptions options;
                options.basicInfo = &basic;
                options.syncResourceCount = 1;
                options.syncInstructionCount = 2;
                options.bindingsPerSyncResource = 1;
                options.deviceId = 3;
                options.rank = 1;
                options.provider = "unit-test-direct-trace-provider";
                options.missionStartId = 6;
                options.instructionStartId = 475;
                options.xnStartId = 1961;
                options.ckeStartId = 332;
                options.channelStartId = 2;
                options.offlineOnly = true;

                TileXRCcuDirectInstallAttempt attempt;
                TileXRCcuDirectInstallReport report;
                const int ret = TileXRCcuRunDirectInstallAttempt(options, &attempt, &report);
                (void)ret;
                if (!report.pipelineBuilt || attempt.package.tasks.empty()) {
                    std::cerr << "trace setup failed before package build: " << report.message << "\n";
                    return 1;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("TileXRDirectCcuTrace", result.stderr)
        self.assertIn("syncResource[0]", result.stderr)
        self.assertIn("remoteXn=", result.stderr)
        self.assertIn("taskWindow[0]", result.stderr)
        self.assertIn("task[0]", result.stderr)
        self.assertNotIn("program.sqeLoad[0]", result.stderr)
        self.assertIn("program.sync[0]", result.stderr)

    def test_direct_install_attempt_trace_decodes_lower_layer_contexts_when_enabled(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_orchestrator.h"
            #include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

            #include <cstdlib>
            #include <iostream>

            using namespace TileXR;

            int PrepareLowerLayer(
                const TileXRCcuResourceAllocation& allocation,
                TileXRCcuLowerLayerInstallPlan* plan,
                TileXRCcuLowerLayerPlanBuilderReport* report,
                void* userData)
            {
                auto* basic = static_cast<TileXRCcuBasicInfo*>(userData);

                std::array<uint8_t, TILEXR_CCU_EID_BYTES> eid;
                for (uint32_t i = 0; i < eid.size(); ++i) {
                    eid[i] = static_cast<uint8_t>(0x20 + i);
                }

                TileXRCcuLowerLayerPlanSpec spec;
                spec.msidToken.dieId = basic->dieId;
                spec.msidToken.msId = basic->msId;
                spec.msidToken.tokenId = basic->msidToken.tokenId;
                spec.msidToken.tokenValue = basic->msidToken.tokenValue;
                spec.msidToken.valid = basic->msidToken.valid;
                spec.pfe.dieId = basic->dieId;
                spec.pfe.pfeOffset = allocation.channels.startId;
                spec.pfe.startJettyId = 0x400;
                spec.pfe.startLocalJettyCtxId = 0x02;

                TileXRCcuLowerLayerJettySpec jetty;
                jetty.dieId = basic->dieId;
                jetty.pfeId = allocation.channels.startId;
                jetty.startJettyCtxId = spec.pfe.startLocalJettyCtxId;
                jetty.doorbellVa = 0x1122334455667788ULL;
                jetty.doorbellTokenId = 0xabcdeU;
                jetty.doorbellTokenValue = 0x12345678U;
                jetty.sqDepth = 8;
                jetty.wqeBasicBlockStartId = 0x34;
                spec.jettys.push_back(jetty);

                TileXRCcuLowerLayerChannelSpec channel;
                channel.dieId = basic->dieId;
                channel.channelId = allocation.channels.startId;
                channel.remoteEid = eid;
                channel.tpn = 0x00876543U;
                channel.sourcePfeId = allocation.channels.startId;
                channel.startJettyId = 0x400;
                channel.jettyCount = 1;
                channel.memoryTokenId = 0x23456U;
                channel.memoryTokenValue = 0x3456789aU;
                channel.remoteCcuVa =
                    basic->resourceAddr + TILEXR_CCU_V1_XN_RESOURCE_OFFSET + allocation.remoteXn.startId * 8ULL;
                spec.channels.push_back(channel);

                spec.xnClear.dieId = basic->dieId;
                spec.xnClear.startXnId = allocation.localXn.startId;
                spec.xnClear.count = allocation.localXn.num;
                spec.xnClear.valid = true;
                spec.ckeClear.dieId = basic->dieId;
                spec.ckeClear.startCkeId = allocation.notifyCke.startId;
                spec.ckeClear.count = allocation.notifyCke.num;
                spec.ckeClear.valid = true;

                TileXRCcuRemoteXnBindingProof proof;
                proof.dieId = basic->dieId;
                proof.channelId = allocation.channels.startId;
                proof.localXn = allocation.localXn.startId;
                proof.remoteXn = allocation.remoteXn.startId;
                proof.notifyCke = allocation.notifyCke.startId;
                proof.localWaitCke = allocation.notifyCke.startId;
                proof.peerRank = 0;
                proof.peerExchangeObserved = true;
                proof.endpointRouteVerified = true;
                spec.remoteXnBindings.push_back(proof);

                return TileXRCcuBuildLowerLayerInstallPlan(spec, plan, report);
            }

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_TRACE", "1", 1);

                TileXRCcuBasicInfo basic;
                basic.dieId = 1;
                basic.msId = 0x45;
                basic.msidToken.valid = true;
                basic.msidToken.tokenId = 0x1234;
                basic.msidToken.tokenValue = 0x5678;
                basic.missionKey = 0x059b0f03U;
                basic.resourceAddr = 0x100000000ULL;
                basic.caps.cap0 = (7U << 24) | (11U << 16) | 169U;
                basic.caps.cap1 = (61U << 16) | 31U;
                basic.caps.cap2 = (63U << 16) | 35U;
                basic.caps.cap3 = (127U << 16) | 3U;
                basic.caps.cap4 = 15U;

                TileXRCcuDirectInstallOptions options;
                options.basicInfo = &basic;
                options.syncResourceCount = 1;
                options.syncInstructionCount = 2;
                options.bindingsPerSyncResource = 1;
                options.deviceId = 3;
                options.rank = 1;
                options.provider = "unit-test-direct-trace-lower-layer-provider";
                options.missionStartId = 6;
                options.instructionStartId = 475;
                options.xnStartId = 1961;
                options.ckeStartId = 332;
                options.channelStartId = 2;
                options.offlineOnly = true;
                options.prepareLowerLayerPlan = PrepareLowerLayer;
                options.lowerLayerPlanUserData = &basic;

                TileXRCcuDirectInstallAttempt attempt;
                TileXRCcuDirectInstallReport report;
                const int ret = TileXRCcuRunDirectInstallAttempt(options, &attempt, &report);
                (void)ret;
                if (!report.pipelineBuilt || attempt.preparedLowerLayerPlan.channels.empty()) {
                    std::cerr << "lower-layer trace setup failed: " << report.message << "\n";
                    return 1;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("decoded=PfeCtx", result.stderr)
        self.assertIn("decoded=LocalJettyCtx", result.stderr)
        self.assertIn("decoded=ChannelCtxV1", result.stderr)
        self.assertIn("startTaJettyId=1024", result.stderr)
        self.assertIn("startLocalJettyCtxId=2", result.stderr)
        self.assertIn("doorbellVa=0x1122334455667788", result.stderr)
        self.assertIn("wqeBasicBlockStartId=52", result.stderr)
        self.assertIn("sourcePfeId=2", result.stderr)
        self.assertIn("remoteCcuVa=", result.stderr)

    def test_direct_orchestrator_is_wired_and_has_no_private_runtime_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = DIRECT_HEADER.read_text(encoding="utf-8")
        source = DIRECT_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_direct_orchestrator.h", cmake)
        self.assertIn("ccu/tilexr_ccu_direct_orchestrator.cpp", cmake)
        self.assertIn("struct TileXRCcuDirectInstallOptions", header)
        self.assertIn("struct TileXRCcuDirectMemoryCopySpec", header)
        self.assertIn("struct TileXRCcuDirectInstallAttempt", header)
        self.assertIn("struct TileXRCcuDirectInstallReport", header)
        self.assertIn("struct TileXRCcuDirectSubmitReport", header)
        self.assertIn("TileXRCcuLowerLayerPlanPrepareFn", header)
        self.assertIn("localWaitCkeStartId", header)
        self.assertIn("gsaStartId", header)
        self.assertIn("remoteNotifyCkeStartId", header)
        self.assertIn("prepareLowerLayerPlan", header)
        self.assertIn("preparedLowerLayerPlan", header)
        self.assertIn("lowerLayerPlanReport", header)
        self.assertIn("TileXRCcuSubmitPreparedTasks", header)
        self.assertIn("TileXRCcuRunDirectInstallAttempt", header)
        self.assertIn("TileXRCcuRunDirectMemoryCopyInstallAttempt", header)
        self.assertIn("TileXRCcuDecodeBasicInfo", source)
        self.assertIn("TileXRCcuBuildResourceSpec", source)
        self.assertIn("TileXRCcuResourceAllocator", source)
        self.assertIn("TileXRCcuBuildLaunchPackage", source)
        self.assertIn("TileXRCcuBuildMemoryCopyProgram", source)
        self.assertIn("BuildDirectMemoryCopyLaunchPackage", source)
        self.assertIn("TileXRCcuBindLaunchPackageInstallScope", source)
        self.assertIn("TileXRCcuBuildInstallManifest", source)
        self.assertIn("options.prepareLowerLayerPlan", source)
        self.assertIn("TileXRCcuInstallHardware", source)
        self.assertIn("TileXRCcuPrepareSubmitTasks", source)
        self.assertIn("TileXRCcuSubmitTask", source)
        self.assertIn("remote XN install provider is missing", source)
        self.assertIn("TraceDecodedInstr", source)
        self.assertIn("TraceDecodedPfeCtx", source)
        self.assertIn("TraceDecodedLocalJettyCtx", source)
        self.assertIn("TraceDecodedChannelCtxV1", source)
        self.assertIn("decoded=SyncXn", source)
        self.assertIn("decoded=SyncCke", source)
        self.assertIn("decoded=LoadImdToXn", source)
        self.assertIn("decoded=LoadImdToGSA", source)
        self.assertIn("decoded=TransRmtMemToLocMem", source)
        self.assertIn("decoded=TransLocMemToRmtMem", source)
        self.assertIn("TILEXR_CCU_TRACE_TRANS_RMT_MEM_TO_LOC_MEM_HEADER", source)
        self.assertIn("TILEXR_CCU_TRACE_TRANS_LOC_MEM_TO_RMT_MEM_HEADER", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_direct_install_options_default_to_lower_layer_first(self):
        header = DIRECT_HEADER.read_text(encoding="utf-8")
        source = DIRECT_SOURCE.read_text(encoding="utf-8")

        self.assertIn(
            "TileXRCcuInstallOrder installOrder = "
            "TileXRCcuInstallOrder::InstallLowerLayerFirst",
            header,
        )
        self.assertIn("installRequest.installOrder = options.installOrder", source)


if __name__ == "__main__":
    unittest.main()
