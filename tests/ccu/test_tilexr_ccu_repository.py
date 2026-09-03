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
REPOSITORY_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_repository.h"
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
    if not (lib_dir / "libascendcl.so").exists():
        return None

    flags = []
    for include_dir in include_dirs:
        if include_dir.exists():
            flags.extend(["-I", str(include_dir)])
    flags.extend(["-L", str(lib_dir), f"-Wl,-rpath-link,{lib_dir}", "-lascendcl", "-lruntime"])
    return flags, str(lib_dir)


class TileXRCcuRepositoryTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers repository C++ syntax")
        cann_config = cann_compile_flags()
        if cann_config is None:
            self.skipTest("CANN ACL headers/libs are not configured for repository C++ syntax check")
        cann_flags, cann_lib_dir = cann_config
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "repository_test.cpp"
            test_bin = temp_path / "repository_test"
            test_cpp.write_text(code, encoding="utf-8")
            compile_cmd = [
                compiler,
                "-std=c++14",
                "-I",
                str(INCLUDE_DIR),
                "-I",
                str(COMM_DIR),
                str(test_cpp),
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

    def test_repository_image_places_trace_microcode_at_declared_windows(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_repository.h"

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
                plan.taskWindows.push_back({1, 489, 13, 13, {0x100051152e00ULL}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});

                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << planReport.message << "\n";
                    return 1;
                }

                TileXRCcuRepositoryImage image;
                TileXRCcuRepositoryReport report;
                if (TileXRCcuBuildRepositoryImage(plan, program, &image, &report) != TILEXR_SUCCESS) {
                    std::cerr << "repository image build failed: " << report.message << "\n";
                    return 2;
                }

                if (image.repositoryStartId != 475 || image.repositoryCount != 170 ||
                    image.missionStartId != 489 || image.missionCount != 156 ||
                    image.missionOffset != 14 || image.sqeLoadOffset != 14 ||
                    image.sqeLoadCount != 13 || image.syncOffset != 27 || image.syncCount != 11) {
                    std::cerr << "unexpected repository metadata\n";
                    return 3;
                }
                if (image.instructions.size() != 170) {
                    std::cerr << "unexpected repository image size\n";
                    return 4;
                }
                if (image.instructions[14].words[0] != 0x0000000007a90001ULL ||
                    image.instructions[26].words[0] != 0x0000000c07b50001ULL ||
                    image.instructions[27].words[0] != 0x0000000007a90001ULL ||
                    image.instructions[28].words[0] != 0x0000000107aa0001ULL ||
                    image.instructions[29].words[0] != 0x0000000007b60003ULL ||
                    image.instructions[30].words[0] != 0x0000000007b60003ULL ||
                    image.instructions[31].words[0] != 0x0001016c00000802ULL ||
                    image.instructions[31].words[1] != 0 ||
                    image.instructions[32].words[0] != 0x000007a90939100dULL ||
                    image.instructions[32].words[1] != 0x00000001016c0002ULL ||
                    image.instructions[32].words[2] != 0x0001000000000000ULL ||
                    image.instructions[35].words[0] != 0x0000000000010802ULL ||
                    image.instructions[35].words[1] != 0x000000000001016cULL) {
                    std::cerr << "unexpected installed instructions\n";
                    return 5;
                }
                TileXRCcuInstr expectedNop;
                if (TileXRCcuEncodeLoadImdToXn(plan.kernelLocalXn.startId, 0, 0, &expectedNop) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "failed to encode expected repository padding nop\n";
                    return 6;
                }
                if (image.instructions[13].words[0] != expectedNop.words[0] ||
                    image.instructions[13].words[1] != expectedNop.words[1] ||
                    image.instructions[13].words[2] != expectedNop.words[2] ||
                    image.instructions[13].words[3] != expectedNop.words[3] ||
                    image.instructions[38].words[0] != expectedNop.words[0] ||
                    image.instructions[38].words[1] != expectedNop.words[1] ||
                    image.instructions[38].words[2] != expectedNop.words[2] ||
                    image.instructions[38].words[3] != expectedNop.words[3]) {
                    std::cerr << "unused repository slots should contain valid nop padding\n";
                    return 6;
                }
                if (report.repositoryCount != 170 || report.installedInstructionCount != 24 ||
                    report.sqeLoadOffset != 14 || report.syncOffset != 27 || report.message != "ok") {
                    std::cerr << "unexpected repository report\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_repository_image_uploads_mission_window_and_installs_via_driver_adapter(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_repository.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                uint64_t allocBytes = 0;
                uint64_t copiedBytes = 0;
                bool freed = false;
                uint32_t observedOp = 0;
                uint32_t observedOffset = 0;
                uint32_t observedDataLen = 0;
                uint64_t observedResourceAddr = 0;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->allocBytes = bytes;
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
                state->copiedBytes = bytes;
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
                plan.taskWindows.push_back({1, 489, 13, 13, {0x100051152e00ULL}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            int main()
            {
                TileXRCcuProducerPlan plan = MakePlan();
                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << planReport.message << "\n";
                    return 1;
                }

                TileXRCcuRepositoryImage image;
                TileXRCcuRepositoryReport report;
                if (TileXRCcuBuildRepositoryImage(plan, program, &image, &report) != TILEXR_SUCCESS) {
                    std::cerr << "repository build failed: " << report.message << "\n";
                    return 2;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 3;
                }

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;

                TileXRCcuRepositoryInstallReceipt receipt;
                if (TileXRCcuInstallRepositoryImage(
                        image, memoryOps, &state, adapter, &receipt, &report) != TILEXR_SUCCESS) {
                    std::cerr << "repository install failed: " << report.message << "\n";
                    return 4;
                }

                const uint64_t expectedBytes = 156ULL * sizeof(TileXRCcuInstr);
                if (state.allocBytes != expectedBytes || state.copiedBytes != expectedBytes ||
                    receipt.instructionStartId != 489 || receipt.instructionCount != 156 ||
                    receipt.instructionBytes != expectedBytes || !receipt.uploaded || !receipt.installed) {
                    std::cerr << "upload receipt mismatch\n";
                    return 5;
                }
                if (state.observedOp != TILEXR_CCU_U_OP_SET_INSTRUCTION ||
                    state.observedOffset != 489 || state.observedDataLen != expectedBytes ||
                    state.observedResourceAddr != receipt.deviceInstructionAddr) {
                    std::cerr << "install adapter request mismatch\n";
                    return 6;
                }

                const auto* installed = reinterpret_cast<const TileXRCcuInstr*>(state.deviceBytes.data());
                if (installed[0].words[0] != image.instructions[image.missionOffset].words[0] ||
                    installed[12].words[0] != image.instructions[image.missionOffset + 12].words[0] ||
                    installed[13].words[0] != image.instructions[image.syncOffset].words[0]) {
                    std::cerr << "mission-window upload content mismatch\n";
                    return 7;
                }
                if (report.installedInstructionCount != 156 || !report.repositoryInstalled ||
                    report.message != "ok") {
                    std::cerr << "install report mismatch\n";
                    return 8;
                }

                if (TileXRCcuReleaseRepositoryInstallReceipt(receipt, memoryOps, &state, &report) !=
                    TILEXR_SUCCESS || !state.freed) {
                    std::cerr << "release failed: " << report.message << "\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_repository_install_options_can_upload_full_repository_with_descriptor_len(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_repository.h"

            #include <cstring>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                uint64_t allocBytes = 0;
                uint64_t copiedBytes = 0;
                std::vector<uint8_t> deviceBytes;
                uint32_t observedOffset = 0;
                uint32_t observedDataLen = 0;
                uint64_t observedResourceAddr = 0;
                bool freed = false;
            };

            int FakeAlloc(uint64_t bytes, void** ptr, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->allocBytes = bytes;
                state->deviceBytes.assign(static_cast<size_t>(bytes), 0);
                *ptr = state->deviceBytes.data();
                return 0;
            }

            int FakeCopy(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (dst != state->deviceBytes.data() || dstBytes != state->deviceBytes.size()) {
                    return -1;
                }
                state->copiedBytes = bytes;
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
                plan.taskWindows.push_back({1, 489, 13, 13, {0x100051152e00ULL}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            int main()
            {
                TileXRCcuProducerPlan plan = MakePlan();
                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << planReport.message << "\n";
                    return 1;
                }

                TileXRCcuRepositoryImage image;
                TileXRCcuRepositoryReport report;
                if (TileXRCcuBuildRepositoryImage(plan, program, &image, &report) != TILEXR_SUCCESS) {
                    std::cerr << "repository build failed: " << report.message << "\n";
                    return 2;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 3;
                }

                TileXRCcuRepositoryInstallOptions options;
                options.window = TileXRCcuRepositoryInstallWindow::FullRepository;
                options.dataLenMode = TileXRCcuRepositoryInstallDataLenMode::DescriptorBytes;

                TileXRCcuDeviceMemoryOps memoryOps = {FakeAlloc, FakeCopy, FakeFree};
                TileXRCcuRepositoryInstallReceipt receipt;
                if (TileXRCcuInstallRepositoryImageWithOptions(
                        image, options, memoryOps, &state, adapter, &receipt, &report) != TILEXR_SUCCESS) {
                    std::cerr << "repository install failed: " << report.message << "\n";
                    return 4;
                }

                const uint64_t expectedBytes = 170ULL * sizeof(TileXRCcuInstr);
                if (state.allocBytes != expectedBytes || state.copiedBytes != expectedBytes ||
                    receipt.instructionStartId != 475 || receipt.instructionCount != 170 ||
                    receipt.instructionBytes != expectedBytes || !receipt.uploaded || !receipt.installed) {
                    std::cerr << "full repository receipt mismatch\n";
                    return 5;
                }
                if (state.observedOffset != 475 ||
                    state.observedDataLen != sizeof(TileXRCcuInstrInfo) ||
                    state.observedResourceAddr != receipt.deviceInstructionAddr) {
                    std::cerr << "full repository SET_INSTRUCTION envelope mismatch\n";
                    return 6;
                }
                const auto* installed = reinterpret_cast<const TileXRCcuInstr*>(state.deviceBytes.data());
                if (installed[0].words[0] != image.instructions[0].words[0] ||
                    installed[14].words[0] != image.instructions[image.missionOffset].words[0]) {
                    std::cerr << "full repository upload content mismatch\n";
                    return 7;
                }
                if (report.installedInstructionCount != 170 ||
                    report.message.find("window=full_repository") == std::string::npos ||
                    report.message.find("dataLenMode=descriptor_bytes") == std::string::npos) {
                    std::cerr << "install report missing option detail: " << report.message << "\n";
                    return 8;
                }
                if (TileXRCcuReleaseRepositoryInstallReceipt(receipt, memoryOps, &state, &report) !=
                    TILEXR_SUCCESS || !state.freed) {
                    std::cerr << "release failed: " << report.message << "\n";
                    return 9;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_repository_install_failure_reports_full_set_instruction_context(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_repository.h"

            #include <cstring>
            #include <iostream>
            #include <string>
            #include <vector>

            using namespace TileXR;

            struct FakeState {
                std::vector<uint8_t> deviceBytes;
                uint64_t readbackBytes = 0;
                uint64_t driverReadbackBytes = 0;
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
                    bytes != dstBytes) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                return 0;
            }

            int FakeCopyDeviceToHost(void* dst, uint64_t dstBytes, const void* src, uint64_t bytes, void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (src != state->deviceBytes.data() || dstBytes < bytes ||
                    bytes != state->deviceBytes.size()) {
                    return -1;
                }
                std::memcpy(dst, src, static_cast<size_t>(bytes));
                state->readbackBytes = bytes;
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
                out->opRet = 0;
                if (in.op == TILEXR_CCU_U_OP_SET_INSTRUCTION) {
                    return 328107;
                }
                if (in.op != TILEXR_CCU_U_OP_GET_INSTRUCTION) {
                    return -1;
                }
                auto* state = static_cast<FakeState*>(userData);
                constexpr uint32_t kBaseInstruction = 475;
                const uint32_t count = in.data.dataInfo.dataArraySize;
                if (count == 0 || count > TILEXR_CCU_MAX_DATA_ARRAY_SIZE ||
                    in.offsetStartIdx < kBaseInstruction ||
                    in.data.dataInfo.dataLen != count * TILEXR_CCU_INSTRUCTION_BYTES) {
                    return -2;
                }
                const uint64_t byteOffset =
                    static_cast<uint64_t>(in.offsetStartIdx - kBaseInstruction) *
                    TILEXR_CCU_INSTRUCTION_BYTES;
                const uint64_t bytes = static_cast<uint64_t>(count) * TILEXR_CCU_INSTRUCTION_BYTES;
                if (byteOffset + bytes > state->deviceBytes.size()) {
                    return -3;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    std::memcpy(
                        out->data.dataInfo.dataArray[i].byte32.raw,
                        state->deviceBytes.data() + byteOffset +
                            static_cast<uint64_t>(i) * TILEXR_CCU_INSTRUCTION_BYTES,
                        TILEXR_CCU_INSTRUCTION_BYTES);
                }
                out->data.dataInfo.dataArraySize = count;
                out->data.dataInfo.dataLen = static_cast<uint32_t>(bytes);
                out->offsetNextIdx = in.offsetStartIdx + count;
                state->driverReadbackBytes += bytes;
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
                plan.taskWindows.push_back({1, 489, 13, 13, {0x100051152e00ULL}});
                plan.taskWindows.push_back({1, 502, 143, 13, {}});
                return plan;
            }

            bool Has(const std::string& haystack, const std::string& needle)
            {
                return haystack.find(needle) != std::string::npos;
            }

            int main()
            {
                TileXRCcuProducerPlan plan = MakePlan();
                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << planReport.message << "\n";
                    return 1;
                }

                TileXRCcuRepositoryImage image;
                TileXRCcuRepositoryReport report;
                if (TileXRCcuBuildRepositoryImage(plan, program, &image, &report) != TILEXR_SUCCESS) {
                    std::cerr << "repository build failed: " << report.message << "\n";
                    return 2;
                }

                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport driverReport;
                if (adapter.Init(5, FakeCustomChannel, &state, &driverReport) != TILEXR_SUCCESS) {
                    std::cerr << "adapter init failed\n";
                    return 3;
                }

                TileXRCcuRepositoryInstallOptions options;
                options.window = TileXRCcuRepositoryInstallWindow::FullRepository;
                options.dataLenMode = TileXRCcuRepositoryInstallDataLenMode::InstructionBytes;

                TileXRCcuDeviceMemoryOps memoryOps;
                memoryOps.alloc = FakeAlloc;
                memoryOps.copyHostToDevice = FakeCopy;
                memoryOps.free = FakeFree;
                memoryOps.copyDeviceToHost = FakeCopyDeviceToHost;
                TileXRCcuRepositoryInstallReceipt receipt;
                const int ret = TileXRCcuInstallRepositoryImageWithOptions(
                    image, options, memoryOps, &state, adapter, &receipt, &report);
                if (ret != TILEXR_ERROR_MKIRT || !state.freed || receipt.deviceInstructionPtr != nullptr ||
                    state.readbackBytes != 5440ULL || state.driverReadbackBytes != 5440ULL) {
                    std::cerr << "install failure handling mismatch ret=" << ret
                              << " freed=" << state.freed
                              << " readbackBytes=" << state.readbackBytes
                              << " driverReadbackBytes=" << state.driverReadbackBytes << "\n";
                    return 4;
                }

                const std::string msg = report.message;
                for (const char* needle : {
                         "failed to install CCU repository instruction image",
                         "CCU custom channel call failed op=251 driverRet=328107 opRet=0",
                         "dieId=1",
                         "installStartId=475",
                         "installCount=170",
                         "instructionBytes=5440",
                         "customChannelDataLen=5440",
                         "deviceInstructionAddr=0x",
                         "window=full_repository",
                         "dataLenMode=instruction_bytes",
                         "firstInstructionWords=",
                         "lastInstructionWords=",
                         "instructionFnv1a64=0x",
                         "uploadReadback=ok",
                         "uploadReadbackBytes=5440",
                         "uploadReadbackFnv1a64=0x",
                         "uploadReadbackFirstInstructionWords=",
                         "uploadReadbackLastInstructionWords=",
                         "uploadReadbackMismatchCount=0",
                         "driverReadback=ok",
                         "driverReadbackRet=0",
                         "driverReadbackBytes=5440",
                         "driverReadbackFnv1a64=0x",
                         "driverReadbackFirstInstructionWords=",
                         "driverReadbackLastInstructionWords=",
                         "driverReadbackMismatchCount=0"}) {
                    if (!Has(msg, needle)) {
                        std::cerr << "missing diagnostic field '" << needle << "' in: " << msg << "\n";
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

    def test_repository_image_places_pure_barrier_sync_microcode_without_sqe_load(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_repository.h"

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

                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed: " << planReport.message << "\n";
                    return 1;
                }
                if (!program.sqeLoad.empty() || program.sync.size() != 2) {
                    std::cerr << "pure barrier microcode size mismatch\n";
                    return 2;
                }

                TileXRCcuRepositoryImage image;
                TileXRCcuRepositoryReport report;
                if (TileXRCcuBuildRepositoryImage(plan, program, &image, &report) != TILEXR_SUCCESS) {
                    std::cerr << "repository image build failed: " << report.message << "\n";
                    return 3;
                }
                if (image.repositoryStartId != 1 || image.repositoryCount != 2 ||
                    image.missionStartId != 1 || image.missionCount != 2 ||
                    image.missionOffset != 0 || image.sqeLoadCount != 0 ||
                    image.syncOffset != 0 || image.syncCount != 2 ||
                    image.instructions.size() != 2) {
                    std::cerr << "pure barrier repository metadata mismatch\n";
                    return 4;
                }
                if (image.instructions[0].words[0] == 0 ||
                    image.instructions[1].words[0] == 0 ||
                    report.installedInstructionCount != 2 ||
                    report.sqeLoadOffset != 0 || report.syncOffset != 0) {
                    std::cerr << "pure barrier repository content/report mismatch\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_repository_image_rejects_inconsistent_windows(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_repository.h"

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
                TileXRCcuProducerPlan plan = MakePlan();
                TileXRCcuProgram program;
                TileXRCcuProducerPlanReport planReport;
                if (TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
                    std::cerr << "microcode build failed\n";
                    return 1;
                }

                TileXRCcuRepositoryImage image;
                TileXRCcuRepositoryReport report;

                TileXRCcuProducerPlan smallRepo = plan;
                smallRepo.instructionWindow.repositoryCount = 15;
                if (TileXRCcuBuildRepositoryImage(smallRepo, program, &image, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "small repository was accepted\n";
                    return 2;
                }
                if (report.message.find("repository") == std::string::npos) {
                    std::cerr << "small repository diagnostic was weak: " << report.message << "\n";
                    return 3;
                }

                TileXRCcuProducerPlan shortSqeTask = plan;
                shortSqeTask.taskWindows[0].instCnt = 12;
                if (TileXRCcuBuildRepositoryImage(shortSqeTask, program, &image, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "short SQE task window was accepted\n";
                    return 4;
                }
                if (report.message.find("SQE") == std::string::npos) {
                    std::cerr << "short SQE task diagnostic was weak: " << report.message << "\n";
                    return 5;
                }

                TileXRCcuProducerPlan shiftedSyncTask = plan;
                shiftedSyncTask.taskWindows[1].instStartId = 501;
                if (TileXRCcuBuildRepositoryImage(shiftedSyncTask, program, &image, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "misaligned sync task window was accepted\n";
                    return 6;
                }
                if (report.message.find("sync") == std::string::npos) {
                    std::cerr << "misaligned sync diagnostic was weak: " << report.message << "\n";
                    return 7;
                }

                if (TileXRCcuBuildRepositoryImage(plan, program, nullptr, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null output image was accepted\n";
                    return 8;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_repository_layer_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = REPOSITORY_HEADER.read_text(encoding="utf-8")
        source = REPOSITORY_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_repository.h", cmake)
        self.assertIn("ccu/tilexr_ccu_repository.cpp", cmake)
        self.assertIn("struct TileXRCcuRepositoryImage", header)
        self.assertIn("struct TileXRCcuRepositoryReport", header)
        self.assertIn("struct TileXRCcuDeviceMemoryOps", header)
        self.assertIn("TileXRCcuCopyDeviceToHostFn", header)
        self.assertIn("copyDeviceToHost", header)
        self.assertIn("struct TileXRCcuRepositoryInstallReceipt", header)
        self.assertIn("enum class TileXRCcuRepositoryMemoryAllocMode", header)
        self.assertIn("TileXRCcuMakeAclModule3DeviceMemoryOps", header)
        self.assertIn("RtHbm", header)
        self.assertIn("TileXRCcuMakeRtHbmDeviceMemoryOps", header)
        self.assertIn("TileXRCcuMakeRepositoryDeviceMemoryOps", header)
        self.assertIn("TileXRCcuRepositoryInstallOptions", header)
        self.assertIn("TileXRCcuBuildRepositoryImage", header)
        self.assertIn("TileXRCcuInstallRepositoryImageWithOptions", header)
        self.assertIn("TileXRCcuInstallRepositoryImage", header)
        self.assertIn("TileXRCcuReleaseRepositoryInstallReceipt", header)
        self.assertIn("TileXRCcuMakeAclDeviceMemoryOps", header)
        self.assertIn("tilexr_ccu_producer_plan.h", header)
        self.assertIn("missionOffset", header)
        self.assertIn("sqeLoadOffset", header)
        self.assertIn("syncOffset", header)
        self.assertIn("#include <acl/acl_rt.h>", source)
        self.assertIn("#include <runtime/mem.h>", source)
        self.assertIn("aclrtMalloc", source)
        self.assertIn("ACL_MEM_MALLOC_HUGE_FIRST", source)
        self.assertIn("aclrtMallocWithCfg", source)
        self.assertIn("ACL_RT_MEM_ATTR_MODULE_ID", source)
        self.assertIn("ACL_MEM_TYPE_HIGH_BAND_WIDTH", source)
        self.assertIn("TILEXR_CCU_ACL_MODULE3_ID", source)
        self.assertIn("aclrtMemcpy", source)
        self.assertIn("ACL_MEMCPY_HOST_TO_DEVICE", source)
        self.assertIn("ACL_MEMCPY_DEVICE_TO_HOST", source)
        self.assertIn("aclrtFree", source)
        self.assertIn("rtMalloc", source)
        self.assertIn("RT_MEMORY_HBM", source)
        self.assertIn("rtMemcpy", source)
        self.assertIn("RT_MEMCPY_HOST_TO_DEVICE", source)
        self.assertIn("RT_MEMCPY_DEVICE_TO_HOST", source)
        self.assertIn("rtFree", source)
        self.assertIn("TileXRCcuMakeAclDeviceMemoryOps", source)
        self.assertIn("TileXRCcuMakeAclModule3DeviceMemoryOps", source)
        self.assertIn("TileXRCcuMakeRtHbmDeviceMemoryOps", source)
        self.assertIn("TileXRCcuMakeRepositoryDeviceMemoryOps", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_repository_install_failure_diagnostic_source_contract(self):
        source = REPOSITORY_SOURCE.read_text(encoding="utf-8")

        for needle in [
            "BuildInstallFailureDiagnostic",
            "dieId=",
            "installStartId=",
            "installCount=",
            "instructionBytes=",
            "customChannelDataLen=",
            "deviceInstructionAddr=0x",
            "window=",
            "dataLenMode=",
            "firstInstructionWords=",
            "lastInstructionWords=",
            "instructionFnv1a64=0x",
            "uploadReadback=",
            "uploadReadbackBytes=",
            "uploadReadbackFnv1a64=0x",
            "uploadReadbackFirstInstructionWords=",
            "uploadReadbackLastInstructionWords=",
            "uploadReadbackMismatchCount=",
            "driverReadback=",
            "driverReadbackRet=",
            "driverReadbackBytes=",
            "driverReadbackFnv1a64=0x",
            "driverReadbackFirstInstructionWords=",
            "driverReadbackLastInstructionWords=",
            "driverReadbackMismatchCount=",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, source)


if __name__ == "__main__":
    unittest.main()
