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
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"
ALLTOALL_HEADER = COMM_DIR / "ccu" / "tilexr_ccu_alltoall_program.h"
ALLTOALL_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_alltoall_program.cpp"
MICROCODE_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_microcode.cpp"
MEMORY_SOURCE = COMM_DIR / "ccu" / "tilexr_ccu_memory_program.cpp"
COMM_CMAKE = COMM_DIR / "CMakeLists.txt"


PRIVATE_CCU_PRODUCER_NEEDLES = [
    "#include <hcomm/",
    "#include <hccl/",
    "libhcomm",
    "libhccl_v2",
    "libhccl_fwk",
    "HcommCcuKernelLaunch",
    "HcclGetCcuTaskInfo",
    "HcomGetCcuTaskInfo",
    "CcuLoopGroupCreate",
    "CcuResBatchAllocator",
    "CcuResRepository",
    "CcuDeviceManager",
    "CcuDevMgrImp",
    "ccu::",
]


class TileXRCcuAllToAllProgramTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "alltoall_program_test.cpp"
            test_bin = temp_path / "alltoall_program_test"
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
                    str(ALLTOALL_SOURCE),
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

    def test_two_mb_program_has_presync_64_copy_blocks_postsync_and_finish(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_alltoall_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            constexpr uint16_t kSetCkeHeader = 0x0802U;
            constexpr uint16_t kClearCkeHeader = 0x0804U;
            constexpr uint16_t kLoadSqeArgsToXnHeader = 0x0001U;
            constexpr uint16_t kLoadImdToXnHeader = 0x0003U;
            constexpr uint16_t kSyncXnHeader = 0x100dU;

            uint16_t Slot(const TileXRCcuInstr& instr, uint32_t slot)
            {
                const uint32_t word = slot / 4U;
                const uint32_t shift = (slot % 4U) * 16U;
                return static_cast<uint16_t>((instr.words[word] >> shift) & 0xffffU);
            }

            int main()
            {
                TileXRCcuAllToAll2RankProgramSpec spec;
                spec.localSendAddr = 0x10000000ULL;
                spec.localSendToken = TileXRCcuPackMemoryToken(0x12345, 0x11112222U, true);
                spec.localRecvAddr = 0x18000000ULL;
                spec.localRecvToken = TileXRCcuPackMemoryToken(0x12346, 0x22223333U, true);
                spec.remoteSendAddr = 0x20000000ULL;
                spec.remoteSendToken = TileXRCcuPackMemoryToken(0x23456, 0x33334444U, true);
                spec.remoteRecvAddr = 0x28000000ULL;
                spec.remoteRecvToken = TileXRCcuPackMemoryToken(0x23457, 0x44445555U, true);
                spec.bytes = 2ULL * 1024ULL * 1024ULL;
                spec.memorySliceBytes = TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES;
                spec.memSlicePerBlock = TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK;
                spec.localGsa = 0x101;
                spec.remoteGsa = 0x102;
                spec.localXn = 0x201;
                spec.remoteXn = 0x202;
                spec.lengthXn = 0x203;
                spec.preSyncLocalAddrXn = 0x209;
                spec.preSyncLocalTokenXn = 0x20a;
                spec.preSyncLocalMarkerXn = 0x208;
                spec.preSyncRemoteMarkerXn = 0x210;
                spec.preSyncMarkerArgIndex = 0;
                spec.preSyncMarkerEnabled = true;
                spec.channelId = 0x12;
                spec.preSyncMarkerChannelId = 0x14;
                spec.preSyncChannelId = 0x13;
                spec.preSyncTokenChannelId = 0x13;
                spec.copyCompletionCke = 0x301;
                spec.preSyncRemoteAddrXn = 0x211;
                spec.preSyncRemoteTokenXn = 0x212;
                spec.preSyncLocalWaitCke = 0x302;
                spec.preSyncRemoteNotifyCke = 0x303;
                spec.preSyncTokenLocalWaitCke = 0x302;
                spec.preSyncRemoteTokenNotifyCke = 0x303;
                spec.postSyncLocalWaitCke = 0x306;
                spec.postSyncRemoteNotifyCke = 0x307;
                spec.sourceCke = 0x308;
                spec.ckeMask = 1;

                std::vector<TileXRCcuInstr> program;
                TileXRCcuAllToAllProgramReport report;
                int ret = TileXRCcuBuildAllToAll2RankProgram(spec, &program, &report);
                if (ret != TILEXR_SUCCESS) {
                    std::cerr << "builder failed: " << report.message << "\n";
                    return 1;
                }
                const uint32_t expectedBlocks = 64;
                const uint32_t expectedInstructions = 7 + expectedBlocks * 7 + 3 + 1;
                if (report.blockCount != expectedBlocks ||
                    report.copyInstructionCount != expectedBlocks * 7 ||
                    report.preSyncInstructionCount != 7 ||
                    report.postSyncInstructionCount != 3 ||
                    report.finishInstructionCount != 1 ||
                    report.totalInstructionCount != expectedInstructions ||
                    program.size() != expectedInstructions) {
                    std::cerr << "unexpected report counts"
                              << " blocks=" << report.blockCount
                              << " copyInst=" << report.copyInstructionCount
                              << " pre=" << report.preSyncInstructionCount
                              << " post=" << report.postSyncInstructionCount
                              << " finish=" << report.finishInstructionCount
                              << " total=" << report.totalInstructionCount
                              << " size=" << program.size() << "\n";
                    return 2;
                }
                if (report.bytesPerBlock != 32768 || report.message != "ok") {
                    std::cerr << "unexpected block size or message\n";
                    return 3;
                }
                const uint16_t markerMask = 1U;
                const uint16_t outputMask = 1U << TILEXR_CCU_ALLTOALL_OUTPUT_XN_ID;
                const uint16_t tokenMask = 1U << TILEXR_CCU_ALLTOALL_TOKEN_XN_ID;
                const uint16_t waitMask = markerMask | outputMask | tokenMask;
                const uint16_t syncMask = spec.ckeMask;
                const uint32_t postSyncSetIndex = expectedInstructions - 4;
                const uint32_t postSyncWaitIndex = expectedInstructions - 2;
                if (program.size() < 7 ||
                    Slot(program[0], 0) != kLoadSqeArgsToXnHeader ||
                    Slot(program[0], 1) != spec.preSyncLocalMarkerXn ||
                    Slot(program[0], 2) != spec.preSyncMarkerArgIndex ||
                    Slot(program[1], 0) != kSyncXnHeader ||
                    Slot(program[1], 1) != spec.preSyncRemoteMarkerXn ||
                    Slot(program[1], 2) != spec.preSyncLocalMarkerXn ||
                    Slot(program[1], 4) != spec.preSyncMarkerChannelId ||
                    Slot(program[1], 5) != spec.preSyncRemoteNotifyCke ||
                    Slot(program[1], 6) != markerMask ||
                    Slot(program[2], 0) != kLoadImdToXnHeader ||
                    Slot(program[2], 1) != spec.preSyncLocalAddrXn ||
                    Slot(program[3], 0) != kSyncXnHeader ||
                    Slot(program[3], 1) != spec.preSyncRemoteAddrXn ||
                    Slot(program[3], 2) != spec.preSyncLocalAddrXn ||
                    Slot(program[3], 6) != outputMask ||
                    Slot(program[4], 0) != kLoadImdToXnHeader ||
                    Slot(program[4], 1) != spec.preSyncLocalTokenXn ||
                    Slot(program[5], 0) != kSyncXnHeader ||
                    Slot(program[5], 1) != spec.preSyncRemoteTokenXn ||
                    Slot(program[5], 2) != spec.preSyncLocalTokenXn ||
                    Slot(program[5], 4) != spec.preSyncTokenChannelId ||
                    Slot(program[5], 5) != spec.preSyncRemoteTokenNotifyCke ||
                    Slot(program[5], 6) != tokenMask ||
                    Slot(program[6], 0) != kSetCkeHeader ||
                    Slot(program[6], 4) != spec.preSyncLocalWaitCke ||
                    Slot(program[6], 5) != waitMask) {
                    std::cerr << "PreSync should publish marker, output, and token then wait for all"
                              << " loadHeader=0x" << std::hex << Slot(program[0], 0)
                              << " loadXn=0x" << Slot(program[0], 1)
                              << " notifyHeader=0x" << Slot(program[1], 0)
                              << " remoteXn=0x" << Slot(program[1], 1)
                              << " localXn=0x" << Slot(program[1], 2)
                              << " channel=0x" << Slot(program[1], 4)
                              << " remoteCke=0x" << Slot(program[1], 5)
                              << " notifyMask=0x" << Slot(program[1], 6)
                              << " tokenNotifyHeader=0x" << Slot(program[5], 0)
                              << " tokenRemoteXn=0x" << Slot(program[5], 1)
                              << " tokenLocalXn=0x" << Slot(program[5], 2)
                              << " tokenChannel=0x" << Slot(program[5], 4)
                              << " tokenRemoteCke=0x" << Slot(program[5], 5)
                              << " tokenNotifyMask=0x" << Slot(program[5], 6)
                              << " waitHeader=0x" << Slot(program[6], 0)
                              << " waitCke=0x" << Slot(program[6], 4)
                              << " waitMask=0x" << Slot(program[6], 5)
                              << std::dec << "\n";
                    return 4;
                }
                if (Slot(program[postSyncSetIndex], 0) != kSetCkeHeader ||
                    Slot(program[postSyncSetIndex], 3) != syncMask ||
                    Slot(program[postSyncWaitIndex], 0) != kClearCkeHeader ||
                    Slot(program[postSyncWaitIndex], 4) != spec.postSyncLocalWaitCke ||
                    Slot(program[postSyncWaitIndex], 5) != syncMask) {
                    std::cerr << "PostSync should use the allocated post CKE resource with the resource mask"
                              << " postSetMask=0x" << std::hex << Slot(program[postSyncSetIndex], 3)
                              << " postWaitCke=0x" << Slot(program[postSyncWaitIndex], 4)
                              << " postWaitMask=0x" << Slot(program[postSyncWaitIndex], 5)
                              << " syncMask=0x" << spec.ckeMask
                              << std::dec << "\n";
                    return 5;
                }

                spec.preSyncNotify = false;
                spec.postSyncNotify = false;
                spec.emitFinish = false;
                ret = TileXRCcuBuildAllToAll2RankProgram(spec, &program, &report);
                if (ret != TILEXR_SUCCESS ||
                    report.preSyncInstructionCount != 0 ||
                    report.postSyncInstructionCount != 0 ||
                    report.finishInstructionCount != 0 ||
                    report.totalInstructionCount != expectedBlocks * 7 ||
                    program.size() != expectedBlocks * 7) {
                    std::cerr << "copy-only diagnostic program has unexpected counts\n";
                    return 6;
                }

                spec.preSyncNotify = true;
                spec.preSyncWait = false;
                spec.sourceCke = 0;
                ret = TileXRCcuBuildAllToAll2RankProgram(spec, &program, &report);
                if (ret != TILEXR_SUCCESS ||
                    report.preSyncInstructionCount != 6 ||
                    report.totalInstructionCount != expectedBlocks * 7 + 6 ||
                    program.size() != expectedBlocks * 7 + 6) {
                    std::cerr << "notify-only PreSync program has unexpected counts\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_two_rank_program_uses_same_hccl_style_copy_region_for_both_ranks(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_alltoall_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            constexpr uint16_t kTransLocMemToRmtMemHeader = 0x1009U;

            uint16_t Header(const TileXRCcuInstr& instr)
            {
                return static_cast<uint16_t>(instr.words[0] & 0xffffU);
            }

            TileXRCcuAllToAll2RankProgramSpec ValidSpec(uint32_t localRank)
            {
                TileXRCcuAllToAll2RankProgramSpec spec;
                spec.localRank = localRank;
                spec.localSendAddr = 0x10000000ULL;
                spec.localSendToken = TileXRCcuPackMemoryToken(0x12345, 0x11112222U, true);
                spec.localRecvAddr = 0x18000000ULL;
                spec.localRecvToken = TileXRCcuPackMemoryToken(0x12346, 0x22223333U, true);
                spec.remoteSendAddr = 0x20000000ULL;
                spec.remoteSendToken = TileXRCcuPackMemoryToken(0x23456, 0x33334444U, true);
                spec.remoteRecvAddr = 0x28000000ULL;
                spec.remoteRecvToken = TileXRCcuPackMemoryToken(0x23457, 0x44445555U, true);
                spec.bytes = 2ULL * 1024ULL * 1024ULL;
                spec.memorySliceBytes = TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES;
                spec.memSlicePerBlock = TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK;
                spec.localGsa = 0x101;
                spec.remoteGsa = 0x102;
                spec.localXn = 0x201;
                spec.remoteXn = 0x202;
                spec.lengthXn = 0x203;
                spec.preSyncLocalMarkerXn = 0x201;
                spec.preSyncRemoteMarkerXn = 0x213;
                spec.preSyncMarkerArgIndex = 0;
                spec.preSyncMarkerEnabled = true;
                spec.channelId = 0x12;
                spec.preSyncMarkerChannelId = spec.channelId;
                spec.copyCompletionCke = 0x301;
                spec.preSyncRemoteAddrXn = 0x211;
                spec.preSyncRemoteTokenXn = 0x212;
                spec.preSyncLocalWaitCke = 0x302;
                spec.preSyncRemoteNotifyCke = 0x303;
                spec.preSyncTokenLocalWaitCke = 0x304;
                spec.preSyncRemoteTokenNotifyCke = 0x305;
                spec.postSyncLocalWaitCke = 0x306;
                spec.postSyncRemoteNotifyCke = 0x307;
                spec.sourceCke = 0x308;
                spec.ckeMask = 1;
                return spec;
            }

            size_t FirstCopyIndex(const std::vector<TileXRCcuInstr>& program)
            {
                for (size_t i = 0; i < program.size(); ++i) {
                    if (Header(program[i]) == kTransLocMemToRmtMemHeader) {
                        return i;
                    }
                }
                return program.size();
            }

            uint32_t CopyInstructionCount(const std::vector<TileXRCcuInstr>& program)
            {
                uint32_t count = 0;
                for (const auto& instr : program) {
                    if (Header(instr) == kTransLocMemToRmtMemHeader) {
                        ++count;
                    }
                }
                return count;
            }

            int main()
            {
                std::vector<TileXRCcuInstr> rank0;
                std::vector<TileXRCcuInstr> rank1;
                TileXRCcuAllToAllProgramReport report0;
                TileXRCcuAllToAllProgramReport report1;
                int ret0 = TileXRCcuBuildAllToAll2RankProgram(ValidSpec(0), &rank0, &report0);
                int ret1 = TileXRCcuBuildAllToAll2RankProgram(ValidSpec(1), &rank1, &report1);
                if (ret0 != TILEXR_SUCCESS || ret1 != TILEXR_SUCCESS) {
                    std::cerr << "builder failed rank0=" << report0.message
                              << " rank1=" << report1.message << "\n";
                    return 1;
                }
                const uint32_t expectedInstructions = 7 + 64 * 7 + 3 + 1;
                if (report0.totalInstructionCount != expectedInstructions ||
                    report1.totalInstructionCount != expectedInstructions ||
                    rank0.size() != expectedInstructions ||
                    rank1.size() != expectedInstructions) {
                    std::cerr << "unexpected instruction count"
                              << " rank0=" << rank0.size()
                              << " rank1=" << rank1.size()
                              << " report0=" << report0.totalInstructionCount
                              << " report1=" << report1.totalInstructionCount << "\n";
                    return 2;
                }
                if (CopyInstructionCount(rank0) != 64 || CopyInstructionCount(rank1) != 64) {
                    std::cerr << "each rank should issue exactly 64 local-to-remote writes\n";
                    return 3;
                }
                const size_t rank0FirstCopy = FirstCopyIndex(rank0);
                const size_t rank1FirstCopy = FirstCopyIndex(rank1);
                if (rank0FirstCopy != 12 || rank1FirstCopy != 12) {
                    std::cerr << "copy region should start at the same instruction after the single PreSync"
                              << " rank0FirstCopy=" << rank0FirstCopy
                              << " rank1FirstCopy=" << rank1FirstCopy << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_builder_rejects_invalid_slice_configuration(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_alltoall_program.h"

            #include <iostream>
            #include <vector>

            using namespace TileXR;

            TileXRCcuAllToAll2RankProgramSpec ValidSpec()
            {
                TileXRCcuAllToAll2RankProgramSpec spec;
                spec.localSendAddr = 0x10000000ULL;
                spec.localSendToken = TileXRCcuPackMemoryToken(1, 2, true);
                spec.localRecvAddr = 0x18000000ULL;
                spec.localRecvToken = TileXRCcuPackMemoryToken(2, 3, true);
                spec.remoteSendAddr = 0x20000000ULL;
                spec.remoteSendToken = TileXRCcuPackMemoryToken(3, 4, true);
                spec.remoteRecvAddr = 0x28000000ULL;
                spec.remoteRecvToken = TileXRCcuPackMemoryToken(4, 5, true);
                spec.bytes = 2ULL * 1024ULL * 1024ULL;
                spec.memorySliceBytes = TILEXR_CCU_ALLTOALL_MEMORY_SLICE_BYTES;
                spec.memSlicePerBlock = TILEXR_CCU_ALLTOALL_MEM_SLICE_PER_BLOCK;
                spec.localGsa = 1;
                spec.remoteGsa = 2;
                spec.localXn = 3;
                spec.remoteXn = 4;
                spec.lengthXn = 5;
                spec.channelId = 6;
                spec.copyCompletionCke = 7;
                spec.preSyncRemoteAddrXn = 13;
                spec.preSyncRemoteTokenXn = 14;
                spec.preSyncLocalWaitCke = 8;
                spec.preSyncRemoteNotifyCke = 9;
                spec.preSyncTokenLocalWaitCke = 10;
                spec.preSyncRemoteTokenNotifyCke = 11;
                spec.postSyncLocalWaitCke = 12;
                spec.postSyncRemoteNotifyCke = 15;
                spec.sourceCke = 16;
                spec.ckeMask = 1;
                return spec;
            }

            int main()
            {
                std::vector<TileXRCcuInstr> program;
                TileXRCcuAllToAllProgramReport report;

                auto spec = ValidSpec();
                spec.memSlicePerBlock = 9;
                if (TileXRCcuBuildAllToAll2RankProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("memSlicePerBlock") == std::string::npos) {
                    std::cerr << "memSlicePerBlock > 8 accepted: " << report.message << "\n";
                    return 1;
                }

                spec = ValidSpec();
                spec.bytes = 4097;
                if (TileXRCcuBuildAllToAll2RankProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("4KB") == std::string::npos) {
                    std::cerr << "non-4KB size accepted: " << report.message << "\n";
                    return 2;
                }

                spec = ValidSpec();
                spec.remoteRecvToken = 0;
                if (TileXRCcuBuildAllToAll2RankProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("token") == std::string::npos) {
                    std::cerr << "missing token accepted: " << report.message << "\n";
                    return 3;
                }

                spec = ValidSpec();
                spec.localRank = 2;
                if (TileXRCcuBuildAllToAll2RankProgram(spec, &program, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("localRank") == std::string::npos) {
                    std::cerr << "invalid localRank accepted: " << report.message << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_builder_is_wired_and_has_no_hccl_dependency_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = ALLTOALL_HEADER.read_text(encoding="utf-8")
        source = ALLTOALL_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_alltoall_program.h", cmake)
        self.assertIn("ccu/tilexr_ccu_alltoall_program.cpp", cmake)
        self.assertIn("TileXRCcuBuildAllToAll2RankProgram", header)
        self.assertIn("TileXRCcuBuildMemoryCopyProgram", source)
        self.assertIn("TileXRCcuMemoryCopyDirection::LocalToRemote", source)
        self.assertIn("preSyncRemoteTokenNotifyCke", header)
        self.assertIn("preSyncTokenLocalWaitCke", header)
        self.assertNotIn("tokenLocalWaitCke", source)
        self.assertIn("PreSyncSignalMask", source)
        self.assertIn("PostSyncSignalMask", source)
        self.assertIn("1U << TILEXR_CCU_ALLTOALL_OUTPUT_XN_ID", source)
        self.assertIn("1U << TILEXR_CCU_ALLTOALL_TOKEN_XN_ID", source)
        self.assertNotIn("1U << TILEXR_CCU_ALLTOALL_POST_SYNC_ID", source)
        self.assertIn("post.clearWait = true;", source)
        self.assertIn("TILEXR_CCU_ALLTOALL_SIGNAL_MASK", header)
        self.assertIn("TILEXR_CCU_ALLTOALL_RANK0_SIGNAL_MASK", header)
        self.assertIn("TILEXR_CCU_ALLTOALL_RANK1_SIGNAL_MASK", header)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_local_rank_does_not_split_the_long_mission_into_copy_phases(self):
        header = ALLTOALL_HEADER.read_text(encoding="utf-8")
        source = ALLTOALL_SOURCE.read_text(encoding="utf-8")
        orchestrator = (COMM_DIR / "ccu" / "tilexr_ccu_direct_orchestrator.cpp").read_text(encoding="utf-8")
        planner = (COMM_DIR / "ccu" / "tilexr_ccu_collective_planner.cpp").read_text(encoding="utf-8")

        self.assertIn("uint32_t localRank = 0", header)
        self.assertNotIn("append copy only for the local rank's active phase", source)
        self.assertNotIn("for (uint32_t phase = 0; phase < 2U; ++phase)", source)
        self.assertIn("alltoallSpec.localRank = alltoall.localRank", orchestrator)
        self.assertIn("alltoall.localRank = static_cast<uint32_t>(rank)", planner)

    def test_four_rank_mesh_posts_all_peers_then_copies_remote_and_self_chunks(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_alltoall_program.h"

            #include <cstdint>
            #include <iostream>
            #include <vector>

            using namespace TileXR;

            uint16_t Slot(const TileXRCcuInstr& instr, uint32_t slot)
            {
                return static_cast<uint16_t>((instr.words[slot / 4U] >> ((slot % 4U) * 16U)) & 0xffffU);
            }

            uint64_t Immediate(const TileXRCcuInstr& instr)
            {
                return (instr.words[0] >> 32U) | (instr.words[1] << 32U);
            }

            TileXRCcuAllToAllMeshPeerSpec Peer(uint32_t localRank, uint32_t peerRank, uint16_t ordinal)
            {
                TileXRCcuAllToAllMeshPeerSpec peer;
                peer.peerRank = peerRank;
                auto& route = peer.route;
                route.localRank = localRank;
                route.localSendAddr = 0x10000000ULL;
                route.localSendToken = TileXRCcuPackMemoryToken(1, 2, true);
                route.localRecvAddr = 0x20000000ULL;
                route.localRecvToken = TileXRCcuPackMemoryToken(2, 3, true);
                route.remoteRecvAddr = 0x30000000ULL + static_cast<uint64_t>(peerRank) * 0x1000000ULL;
                route.remoteRecvToken = TileXRCcuPackMemoryToken(10 + peerRank, 20 + peerRank, true);
                route.bytes = 2ULL * 1024ULL * 1024ULL;
                route.localGsa = 0x100;
                route.remoteGsa = 0x101;
                route.localXn = 0x200;
                route.remoteXn = 0x301;
                route.lengthXn = 0x202;
                route.preSyncLocalAddrXn = 0x200;
                route.preSyncLocalTokenXn = 0x201;
                route.preSyncRemoteAddrXn = 0x300;
                route.preSyncRemoteTokenXn = 0x301;
                route.preSyncMarkerEnabled = false;
                route.preSyncChannelId = static_cast<uint16_t>(0x10 + ordinal);
                route.preSyncTokenChannelId = route.preSyncChannelId;
                route.copyChannelId = route.preSyncChannelId;
                route.postSyncChannelId = route.preSyncChannelId;
                route.copyCompletionCke = 0x491;
                route.preSyncLocalWaitCke = static_cast<uint16_t>(0x401 + ordinal);
                route.preSyncTokenLocalWaitCke = route.preSyncLocalWaitCke;
                route.preSyncRemoteNotifyCke = static_cast<uint16_t>(0x500 + ordinal);
                route.preSyncRemoteTokenNotifyCke = route.preSyncRemoteNotifyCke;
                route.postSyncLocalWaitCke = route.preSyncLocalWaitCke;
                route.postSyncRemoteNotifyCke = route.preSyncRemoteNotifyCke;
                route.sourceCke = 0x490;
                route.ckeMask = 0x8;
                return peer;
            }

            int main()
            {
                TileXRCcuAllToAllMeshProgramSpec spec;
                spec.rankSize = 4;
                spec.localRank = 2;
                spec.localSendAddr = 0x10000000ULL;
                spec.localSendToken = TileXRCcuPackMemoryToken(1, 2, true);
                spec.localRecvAddr = 0x20000000ULL;
                spec.localRecvToken = TileXRCcuPackMemoryToken(2, 3, true);
                spec.chunkBytes = 2ULL * 1024ULL * 1024ULL;
                spec.selfSourceGsa = 0x180;
                spec.selfDestinationGsa = 0x181;
                spec.selfSourceXn = 0x200;
                spec.selfDestinationXn = 0x201;
                spec.selfLengthXn = 0x202;
                spec.selfChannelId = 0;
                spec.selfCompletionCke = 0x480;
                spec.remoteCompletionCkes = {0x491};
                spec.peers = {Peer(2, 3, 2), Peer(2, 0, 0), Peer(2, 1, 1)};

                std::vector<TileXRCcuInstr> program;
                TileXRCcuAllToAllProgramReport report;
                const int ret = TileXRCcuBuildAllToAllMeshProgram(spec, &program, &report);
                if (ret != TILEXR_SUCCESS) {
                    std::cerr << report.message << "\n";
                    return 1;
                }
                if (report.peerCount != 3 || report.syncResourceCount != 3 ||
                    report.remoteBlockCount != 192 || report.selfBlockCount != 64 ||
                    report.preSyncInstructionCount != 12 || report.copyInstructionCount != 1792 ||
                    report.postSyncInstructionCount != 6 || report.finishInstructionCount != 1 ||
                    report.totalInstructionCount != 1811 || program.size() != 1811) {
                    std::cerr << "unexpected mesh counts total=" << program.size() << "\n";
                    return 2;
                }
                // Match HCCL: load both values, initialize source CKE, then post output/token per channel.
                if (Slot(program[0], 1) != 0x200U || Slot(program[1], 1) != 0x201U ||
                    Slot(program[2], 0) != 0x0802U || Slot(program[2], 2) != 0x490U ||
                    Slot(program[2], 3) != 0xffffU) {
                    std::cerr << "unexpected HCCL-style presync prelude\n";
                    return 3;
                }
                for (uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
                    const uint32_t output = 3 + ordinal * 2;
                    const uint32_t token = output + 1;
                    if (Slot(program[output], 0) != 0x100dU || Slot(program[output], 6) != 0x2U ||
                        Slot(program[token], 0) != 0x100dU || Slot(program[token], 6) != 0x4U ||
                        Slot(program[output], 4) != Slot(program[token], 4)) {
                        std::cerr << "presync output/token are not paired by channel\n";
                        return 4;
                    }
                }
                for (uint32_t i = 9; i < 12; ++i) {
                    if (Slot(program[i], 0) != 0x0802U || Slot(program[i], 5) != 0x6U) {
                        std::cerr << "missing presync wait mask\n";
                        return 5;
                    }
                }
                // Sorted peer 0 copy: send[target=0] -> recv_peer0[source=2].
                if (Immediate(program[12]) != spec.localSendAddr ||
                    Immediate(program[14]) != 0x30000000ULL + 2ULL * spec.chunkBytes ||
                    Slot(program[17], 0) != 0x1009U) {
                    std::cerr << "unexpected first remote copy offsets\n";
                    return 6;
                }
                const uint32_t selfStart = 30;
                const uint64_t selfOffset = 2ULL * spec.chunkBytes;
                if (Immediate(program[selfStart]) != spec.localSendAddr + selfOffset ||
                    Immediate(program[selfStart + 2]) != spec.localRecvAddr + selfOffset ||
                    Slot(program[selfStart + 5], 0) != 0x1000U ||
                    Slot(program[selfStart + 5], 1) != 0U ||
                    Slot(program[selfStart + 5], 5) != 0U ||
                    Slot(program[selfStart + 7], 0) != 0x1002U ||
                    Slot(program[selfStart + 7], 3) != 0U ||
                    Slot(program[selfStart + 7], 5) != 0U) {
                    std::cerr << "unexpected CCU self copy offsets\n";
                    return 7;
                }
                auto corrupted = program;
                corrupted[3].words[1] ^= 1ULL;
                if (TileXRCcuValidateAllToAllMeshProgramBindings(spec, corrupted, &report) !=
                        TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("output SyncXn") == std::string::npos) {
                    std::cerr << "corrupted output channel accepted: " << report.message << "\n";
                    return 8;
                }
                auto sharedRemoteIds = spec;
                for (uint32_t ordinal = 1; ordinal < sharedRemoteIds.peers.size(); ++ordinal) {
                    sharedRemoteIds.peers[ordinal].route.remoteXn = sharedRemoteIds.peers[0].route.remoteXn;
                    sharedRemoteIds.peers[ordinal].route.preSyncRemoteAddrXn =
                        sharedRemoteIds.peers[0].route.preSyncRemoteAddrXn;
                    sharedRemoteIds.peers[ordinal].route.preSyncRemoteTokenXn =
                        sharedRemoteIds.peers[0].route.preSyncRemoteTokenXn;
                    sharedRemoteIds.peers[ordinal].route.preSyncRemoteNotifyCke =
                        sharedRemoteIds.peers[0].route.preSyncRemoteNotifyCke;
                    sharedRemoteIds.peers[ordinal].route.preSyncRemoteTokenNotifyCke =
                        sharedRemoteIds.peers[0].route.preSyncRemoteTokenNotifyCke;
                    sharedRemoteIds.peers[ordinal].route.postSyncRemoteNotifyCke =
                        sharedRemoteIds.peers[0].route.postSyncRemoteNotifyCke;
                }
                if (TileXRCcuBuildAllToAllMeshProgram(sharedRemoteIds, &program, &report) != TILEXR_SUCCESS) {
                    std::cerr << "per-peer remote resource IDs rejected: " << report.message << "\n";
                    return 7;
                }
                auto overlappingCke = spec;
                overlappingCke.remoteCompletionCkes[0] = overlappingCke.peers[0].route.sourceCke;
                for (auto& peer : overlappingCke.peers) {
                    peer.route.copyCompletionCke = overlappingCke.remoteCompletionCkes[0];
                }
                if (TileXRCcuBuildAllToAllMeshProgram(overlappingCke, &program, &report) !=
                        TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("overlaps source CKE") == std::string::npos) {
                    std::cerr << "overlapping source/completion CKE accepted: " << report.message << "\n";
                    return 8;
                }
                auto duplicate = spec;
                duplicate.peers[1].route.copyChannelId = duplicate.peers[0].route.copyChannelId;
                if (TileXRCcuBuildAllToAllMeshProgram(duplicate, &program, &report) !=
                        TILEXR_ERROR_PARA_CHECK_FAIL ||
                    report.message.find("duplicate") == std::string::npos) {
                    std::cerr << "duplicate peer resource accepted: " << report.message << "\n";
                    return 8;
                }
                for (uint32_t localRank = 0; localRank < 4; ++localRank) {
                    auto rankSpec = spec;
                    rankSpec.localRank = localRank;
                    rankSpec.peers.clear();
                    uint16_t ordinal = 0;
                    for (uint32_t peerRank = 0; peerRank < 4; ++peerRank) {
                        if (peerRank != localRank) {
                            rankSpec.peers.push_back(Peer(localRank, peerRank, ordinal++));
                        }
                    }
                    if (TileXRCcuBuildAllToAllMeshProgram(rankSpec, &program, &report) != TILEXR_SUCCESS) {
                        std::cerr << "rank " << localRank << " rejected: " << report.message << "\n";
                        return 9;
                    }
                    const uint64_t rankOffset = static_cast<uint64_t>(localRank) * rankSpec.chunkBytes;
                    if (program.size() != 1811 ||
                        Immediate(program[selfStart]) != rankSpec.localSendAddr + rankOffset ||
                        Immediate(program[selfStart + 2]) != rankSpec.localRecvAddr + rankOffset) {
                        std::cerr << "rank " << localRank << " self offset mismatch\n";
                        return 9;
                    }
                }
                auto spec8 = spec;
                spec8.rankSize = 8;
                spec8.localRank = 3;
                spec8.peers.clear();
                spec8.remoteCompletionCkes = {0x491};
                uint16_t ordinal8 = 0;
                for (uint32_t peerRank = 0; peerRank < spec8.rankSize; ++peerRank) {
                    if (peerRank != spec8.localRank) {
                        spec8.peers.push_back(Peer(spec8.localRank, peerRank, ordinal8++));
                    }
                }
                if (TileXRCcuBuildAllToAllMeshProgram(spec8, &program, &report) != TILEXR_SUCCESS ||
                    program.size() != 3367 || report.peerCount != 7 || report.syncResourceCount != 7 ||
                    report.remoteBlockCount != 448 || report.selfBlockCount != 64) {
                    std::cerr << "unexpected 8-rank 2MB mesh: " << report.message
                              << " instructions=" << program.size() << "\n";
                    return 10;
                }

                auto spec2 = spec;
                spec2.rankSize = 2;
                spec2.localRank = 0;
                spec2.peers = {Peer(0, 1, 0)};
                spec2.remoteCompletionCkes = {0x491};
                if (TileXRCcuBuildAllToAllMeshProgram(spec2, &program, &report) != TILEXR_SUCCESS ||
                    program.size() != 1033 || report.peerCount != 1 || report.syncResourceCount != 1 ||
                    report.remoteBlockCount != 64 || report.selfBlockCount != 64) {
                    std::cerr << "unexpected 2-rank 2MB full mesh: " << report.message
                              << " instructions=" << program.size() << "\n";
                    return 11;
                }

                auto spec64 = spec;
                spec64.rankSize = 64;
                spec64.localRank = 17;
                spec64.chunkBytes = 128ULL * 1024ULL;
                spec64.peers.clear();
                spec64.remoteCompletionCkes = {0x491, 0x492, 0x493, 0x494};
                uint16_t ordinal64 = 0;
                for (uint32_t peerRank = 0; peerRank < spec64.rankSize; ++peerRank) {
                    if (peerRank == spec64.localRank) {
                        continue;
                    }
                    auto peer = Peer(spec64.localRank, peerRank, ordinal64);
                    peer.route.bytes = spec64.chunkBytes;
                    peer.route.copyCompletionCke = spec64.remoteCompletionCkes[ordinal64 / 16U];
                    spec64.peers.push_back(peer);
                    ++ordinal64;
                }
                if (TileXRCcuBuildAllToAllMeshProgram(spec64, &program, &report) != TILEXR_SUCCESS ||
                    program.size() != 1883 || report.peerCount != 63 || report.syncResourceCount != 63 ||
                    report.remoteBlockCount != 252 || report.selfBlockCount != 4) {
                    std::cerr << "unexpected 64-rank 128KB mesh: " << report.message
                              << " instructions=" << program.size() << "\n";
                    return 12;
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
