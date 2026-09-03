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
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DRIVER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.h"
DRIVER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp"
SPECS_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.h"
SPECS_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuDriverAdapterTest(unittest.TestCase):
    def compile_and_run(self, code: str, extra_env=None):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "driver_adapter_test.cpp"
            test_bin = temp_path / "driver_adapter_test"
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
                    str(DRIVER_SOURCE),
                    str(SPECS_SOURCE),
                    "-o",
                    str(test_bin),
                ],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            env = None
            if extra_env:
                env = {**os.environ, **extra_env}
            return subprocess.run(
                [str(test_bin)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env)

    def test_adapter_wraps_get_basic_info_and_reuses_tilexr_specs_decode(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <iostream>

            using namespace TileXR;

            struct FakeState {
                int calls = 0;
                uint32_t observedDevice = 0;
                uint32_t observedOp = 0;
                uint32_t observedDie = 0;
            };

            int FakeCustomChannel(
                uint32_t devicePhyId,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->calls++;
                state->observedDevice = devicePhyId;
                state->observedOp = in.op;
                state->observedDie = in.data.dataInfo.udieIdx;
                if (out == nullptr) {
                    return -1;
                }
                out->opRet = 0;
                out->data.dataInfo.dataArray[0].baseinfo.msId = 0x45;
                out->data.dataInfo.dataArray[0].baseinfo.tokenId = 0x1234;
                out->data.dataInfo.dataArray[0].baseinfo.tokenValue = 0;
                out->data.dataInfo.dataArray[0].baseinfo.tokenValid = 1;
                out->data.dataInfo.dataArray[0].baseinfo.missionKey = 0x059b0f03U;
                out->data.dataInfo.dataArray[0].baseinfo.resourceAddr = 0x200000000ULL;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap0 = (3U << 24) | (5U << 16) | 255U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap1 = (127U << 16) | 63U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap2 = (31U << 16) | 15U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap3 = (7U << 16) | 1U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap4 = 9U;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(4, FakeCustomChannel, &state, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }

                TileXRCcuBasicInfo basic;
                if (adapter.GetBasicInfo(1, &basic, &report) != TILEXR_SUCCESS) {
                    std::cerr << "get basic info failed: " << report.message << "\n";
                    return 2;
                }
                if (state.calls != 1 || state.observedDevice != 4 ||
                    state.observedOp != TILEXR_CCU_U_OP_GET_BASIC_INFO || state.observedDie != 1) {
                    std::cerr << "custom channel request mismatch\n";
                    return 3;
                }
                if (basic.dieId != 1 || basic.msId != 0x45 || basic.missionKey != 0x059b0f03U ||
                    basic.resourceAddr != 0x200000000ULL || basic.caps.cap0 == 0 ||
                    basic.msidToken.tokenId != 0x1234 || basic.msidToken.tokenValue != 0 ||
                    !basic.msidToken.valid) {
                    std::cerr << "basic info mismatch\n";
                    return 4;
                }

                TileXRCcuSpecInfo info;
                TileXRCcuSpecsReport specsReport;
                if (TileXRCcuDecodeBasicInfo(basic, &info, &specsReport) != TILEXR_SUCCESS) {
                    std::cerr << "decode failed: " << specsReport.message << "\n";
                    return 5;
                }
                if (info.instructionNum != 256 || info.xnNum != 128 || info.channelNum != 2 ||
                    info.missionNum != 6 || info.loopEngineNum != 4) {
                    std::cerr << "decoded info mismatch\n";
                    return 6;
                }
                if (report.message != "ok" || report.opcode != TILEXR_CCU_U_OP_GET_BASIC_INFO ||
                    report.dieId != 1 || report.devicePhyId != 4) {
                    std::cerr << "report mismatch\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_adapter_wraps_die_enable_and_reports_driver_errors(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <iostream>

            using namespace TileXR;

            struct FakeState {
                bool fail = false;
                uint32_t observedOp = 0;
            };

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->observedOp = in.op;
                if (state->fail) {
                    return -22;
                }
                out->opRet = 0;
                out->data.dataInfo.dataArray[0].dieinfo.enableFlag = TILEXR_CCU_ENABLE_FLAG;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(7, FakeCustomChannel, &state, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed\n";
                    return 1;
                }

                bool enabled = false;
                if (adapter.GetDieEnabled(0, &enabled, &report) != TILEXR_SUCCESS || !enabled) {
                    std::cerr << "die enable query failed: " << report.message << "\n";
                    return 2;
                }
                if (state.observedOp != TILEXR_CCU_U_OP_GET_DIE_WORKING) {
                    std::cerr << "die opcode mismatch\n";
                    return 3;
                }

                state.fail = true;
                if (adapter.GetDieEnabled(0, &enabled, &report) != TILEXR_ERROR_MKIRT) {
                    std::cerr << "driver failure was accepted\n";
                    return 4;
                }
                if (report.message.find("CCU custom channel call failed") == std::string::npos ||
                    report.message.find("driverRet=-22") == std::string::npos ||
                    report.message.find("opRet=0") == std::string::npos ||
                    report.message.find("op=15") == std::string::npos ||
                    report.driverRet != -22) {
                    std::cerr << "weak driver diagnostic: " << report.message << "\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_adapter_installs_instruction_repository_with_set_instruction_opcode(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <iostream>

            using namespace TileXR;

            struct FakeState {
                int calls = 0;
                uint32_t observedDevice = 0;
                uint32_t observedOp = 0;
                uint32_t observedDie = 0;
                uint32_t observedOffset = 0;
                uint32_t observedDataLen = 0;
                uint32_t observedArraySize = 0;
                uint64_t observedResourceAddr = 0;
            };

            int FakeCustomChannel(
                uint32_t devicePhyId,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->calls++;
                state->observedDevice = devicePhyId;
                state->observedOp = in.op;
                state->observedDie = in.data.dataInfo.udieIdx;
                state->observedOffset = in.offsetStartIdx;
                state->observedDataLen = in.data.dataInfo.dataLen;
                state->observedArraySize = in.data.dataInfo.dataArraySize;
                state->observedResourceAddr = in.data.dataInfo.dataArray[0].insinfo.resourceAddr;
                out->opRet = 0;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(5, FakeCustomChannel, &state, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }

                if (adapter.InstallInstructions(1, 489, 13, 0x100051152e00ULL, 13 * 32, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "install failed: " << report.message << "\n";
                    return 2;
                }
                if (state.calls != 1 || state.observedDevice != 5 ||
                    state.observedOp != TILEXR_CCU_U_OP_SET_INSTRUCTION ||
                    state.observedDie != 1 || state.observedOffset != 489 ||
                    state.observedDataLen != 13 * 32 || state.observedArraySize != 1 ||
                    state.observedResourceAddr != 0x100051152e00ULL) {
                    std::cerr << "SET_INSTRUCTION request mismatch\n";
                    return 3;
                }
                if (report.message != "ok" || report.opcode != TILEXR_CCU_U_OP_SET_INSTRUCTION ||
                    report.dieId != 1 || report.devicePhyId != 5) {
                    std::cerr << "report mismatch\n";
                    return 4;
                }

                if (adapter.InstallInstructions(1, 489, 13, 0, 13 * 32, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "zero device instruction address accepted\n";
                    return 5;
                }
                if (adapter.InstallInstructions(1, 489, 0, 0x100051152e00ULL, 0, &report) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty instruction image accepted\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_adapter_rejects_instruction_byte_mismatch_before_custom_channel(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <iostream>

            using namespace TileXR;

            struct FakeState {
                int calls = 0;
            };

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn&,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->calls++;
                out->opRet = 0;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(5, FakeCustomChannel, &state, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }

                const int ret = adapter.InstallInstructions(
                    1,
                    489,
                    13,
                    0x100051152e00ULL,
                    12 * TILEXR_CCU_INSTRUCTION_BYTES,
                    &report);
                if (ret != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "byte mismatch accepted ret=" << ret << "\n";
                    return 2;
                }
                if (state.calls != 0 ||
                    report.message.find("byte size mismatch") == std::string::npos) {
                    std::cerr << "byte mismatch should fail before custom channel: "
                              << report.message << " calls=" << state.calls << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_adapter_reads_each_instruction_from_its_own_data_array_slot(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <cstring>
            #include <iostream>

            using namespace TileXR;

            struct InstructionWords {
                uint64_t words[4];
            };

            struct FakeState {
                int calls = 0;
                uint32_t observedDevice = 0;
                uint32_t observedOp = 0;
                uint32_t observedDie = 0;
                uint32_t observedOffset = 0;
                uint32_t observedDataLen = 0;
                uint32_t observedArraySize = 0;
            };

            int FakeCustomChannel(
                uint32_t devicePhyId,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                state->calls++;
                state->observedDevice = devicePhyId;
                state->observedOp = in.op;
                state->observedDie = in.data.dataInfo.udieIdx;
                state->observedOffset = in.offsetStartIdx;
                state->observedDataLen = in.data.dataInfo.dataLen;
                state->observedArraySize = in.data.dataInfo.dataArraySize;
                out->opRet = 0;

                InstructionWords first {{0x1014c00010802ULL, 0, 0, 0}};
                InstructionWords second {{0x10804ULL, 0x1014cULL, 0, 0}};
                std::memcpy(out->data.dataInfo.dataArray[0].byte32.raw, &first, sizeof(first));
                std::memcpy(out->data.dataInfo.dataArray[1].byte32.raw, &second, sizeof(second));
                out->data.dataInfo.dataArraySize = 2;
                out->data.dataInfo.dataLen = 2 * TILEXR_CCU_INSTRUCTION_BYTES;
                out->offsetNextIdx = 491;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(5, FakeCustomChannel, &state, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }

                InstructionWords instructions[2] {};
                if (adapter.ReadInstructions(
                        1,
                        489,
                        instructions,
                        2,
                        2 * TILEXR_CCU_INSTRUCTION_BYTES,
                        &report) != TILEXR_SUCCESS) {
                    std::cerr << "read failed: " << report.message << "\n";
                    return 2;
                }
                if (state.calls != 1 || state.observedDevice != 5 ||
                    state.observedOp != TILEXR_CCU_U_OP_GET_INSTRUCTION ||
                    state.observedDie != 1 || state.observedOffset != 489 ||
                    state.observedDataLen != 2 * TILEXR_CCU_INSTRUCTION_BYTES ||
                    state.observedArraySize != 2) {
                    std::cerr << "GET_INSTRUCTION request mismatch\n";
                    return 3;
                }
                if (instructions[0].words[0] != 0x1014c00010802ULL ||
                    instructions[0].words[1] != 0 ||
                    instructions[1].words[0] != 0x10804ULL ||
                    instructions[1].words[1] != 0x1014cULL) {
                    std::cerr << "readback slot copy mismatch first=0x" << std::hex
                              << instructions[0].words[0] << " second0=0x"
                              << instructions[1].words[0] << " second1=0x"
                              << instructions[1].words[1] << "\n";
                    return 4;
                }
                if (report.message != "ok" || report.opcode != TILEXR_CCU_U_OP_GET_INSTRUCTION ||
                    report.dieId != 1 || report.devicePhyId != 5) {
                    std::cerr << "report mismatch\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_adapter_set_instruction_trailer_wire_word_is_offset_then_opcode(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <cstring>
            #include <iostream>

            using namespace TileXR;

            struct FakeState {
                uint64_t trailer = 0;
            };

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                std::memcpy(&state->trailer, &in.offsetStartIdx, sizeof(state->trailer));
                out->opRet = 0;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(5, FakeCustomChannel, &state, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }
                if (adapter.InstallInstructions(
                        1, 489, 13, 0x100051152e00ULL, 13 * TILEXR_CCU_INSTRUCTION_BYTES, &report) !=
                    TILEXR_SUCCESS) {
                    std::cerr << "install failed: " << report.message << "\n";
                    return 2;
                }
                const uint64_t expected =
                    (static_cast<uint64_t>(TILEXR_CCU_U_OP_SET_INSTRUCTION) << 32U) | 489ULL;
                if (state.trailer != expected) {
                    std::cerr << "trailer mismatch observed=0x" << std::hex << state.trailer
                              << " expected=0x" << expected << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_adapter_wraps_lower_layer_set_payloads_without_hcomm_runtime(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <cstring>
            #include <iostream>

            using namespace TileXR;

            struct ObservedCall {
                uint32_t op = 0;
                uint32_t die = 0;
                uint32_t offset = 0;
                uint32_t dataLen = 0;
                uint32_t arraySize = 0;
                uint8_t raw[256] = {0};
                uint32_t msId = 0;
                uint32_t tokenId = 0;
                uint32_t tokenValue = 0;
            };

            struct FakeState {
                int calls = 0;
                ObservedCall observed[8];
            };

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn& in,
                TileXRCcuCustomChannelOut* out,
                void* userData)
            {
                auto* state = static_cast<FakeState*>(userData);
                if (state->calls >= 8) {
                    return -1;
                }
                auto& observed = state->observed[state->calls++];
                observed.op = in.op;
                observed.die = in.data.dataInfo.udieIdx;
                observed.offset = in.offsetStartIdx;
                observed.dataLen = in.data.dataInfo.dataLen;
                observed.arraySize = in.data.dataInfo.dataArraySize;
                observed.msId = in.data.dataInfo.dataArray[0].baseinfo.msId;
                observed.tokenId = in.data.dataInfo.dataArray[0].baseinfo.tokenId;
                observed.tokenValue = in.data.dataInfo.dataArray[0].baseinfo.tokenValue;
                std::memcpy(observed.raw, in.data.dataInfo.dataArray, sizeof(observed.raw));
                out->opRet = 0;
                return 0;
            }

            int main()
            {
                FakeState state;
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(5, FakeCustomChannel, &state, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }

                if (adapter.InstallMsidToken(1, 0x45, 0x1234, 0x5678, &report) != TILEXR_SUCCESS) {
                    std::cerr << "msid install failed: " << report.message << "\n";
                    return 2;
                }
                if (state.observed[0].op != TILEXR_CCU_U_OP_SET_MSID_TOKEN ||
                    state.observed[0].die != 1 || state.observed[0].offset != 0 ||
                    state.observed[0].dataLen != 0 || state.observed[0].arraySize != 0 ||
                    state.observed[0].msId != 0x45 || state.observed[0].tokenId != 0x1234 ||
                    state.observed[0].tokenValue != 0x5678) {
                    std::cerr << "SET_MSID_TOKEN request mismatch\n";
                    return 3;
                }

                TileXRCcuPfeCtx pfe{};
                for (uint32_t i = 0; i < sizeof(pfe.raw); ++i) {
                    pfe.raw[i] = static_cast<uint8_t>(0xa0 + i);
                }
                if (adapter.InstallPfeCtx(1, 7, pfe, &report) != TILEXR_SUCCESS) {
                    std::cerr << "pfe install failed: " << report.message << "\n";
                    return 4;
                }
                if (state.observed[1].op != TILEXR_CCU_U_OP_SET_PFE ||
                    state.observed[1].die != 1 || state.observed[1].offset != 7 ||
                    state.observed[1].dataLen != TILEXR_CCU_PFE_CTX_BYTES ||
                    state.observed[1].arraySize != 1 ||
                    std::memcmp(state.observed[1].raw, pfe.raw, TILEXR_CCU_PFE_CTX_BYTES) != 0) {
                    std::cerr << "SET_PFE request mismatch\n";
                    return 5;
                }

                TileXRCcuLocalJettyCtxData jettys[2]{};
                for (uint32_t i = 0; i < sizeof(jettys[0].raw); ++i) {
                    jettys[0].raw[i] = static_cast<uint8_t>(0x10 + i);
                    jettys[1].raw[i] = static_cast<uint8_t>(0x50 + i);
                }
                if (adapter.InstallJettyCtx(1, 9, jettys, 2, &report) != TILEXR_SUCCESS) {
                    std::cerr << "jetty install failed: " << report.message << "\n";
                    return 6;
                }
                if (state.observed[2].op != TILEXR_CCU_U_OP_SET_JETTY_CTX ||
                    state.observed[2].die != 1 || state.observed[2].offset != 9 ||
                    state.observed[2].dataLen != 2 * TILEXR_CCU_LOCAL_JETTY_CTX_BYTES ||
                    state.observed[2].arraySize != 2 ||
                    std::memcmp(state.observed[2].raw, jettys[0].raw, TILEXR_CCU_LOCAL_JETTY_CTX_BYTES) != 0 ||
                    std::memcmp(
                        state.observed[2].raw + TILEXR_CCU_DATA_ARRAY_SLOT_BYTES,
                        jettys[1].raw,
                        TILEXR_CCU_LOCAL_JETTY_CTX_BYTES) != 0) {
                    std::cerr << "SET_JETTY_CTX request mismatch\n";
                    return 7;
                }

                TileXRCcuChannelCtxDataV1 channel{};
                for (uint32_t i = 0; i < sizeof(channel.raw); ++i) {
                    channel.raw[i] = static_cast<uint8_t>(0xc0 + i);
                }
                if (adapter.InstallChannelCtxV1(1, 11, channel, &report) != TILEXR_SUCCESS) {
                    std::cerr << "channel install failed: " << report.message << "\n";
                    return 8;
                }
                if (state.observed[3].op != TILEXR_CCU_U_OP_SET_CHANNEL ||
                    state.observed[3].die != 1 || state.observed[3].offset != 11 ||
                    state.observed[3].dataLen != TILEXR_CCU_CHANNEL_CTX_V1_BYTES ||
                    state.observed[3].arraySize != 1 ||
                    std::memcmp(state.observed[3].raw, channel.raw, TILEXR_CCU_CHANNEL_CTX_V1_BYTES) != 0) {
                    std::cerr << "SET_CHANNEL request mismatch\n";
                    return 9;
                }

                if (adapter.ClearCkeRange(1, 16, 10, &report) != TILEXR_SUCCESS) {
                    std::cerr << "cke clear failed: " << report.message << "\n";
                    return 10;
                }
                if (state.observed[4].op != TILEXR_CCU_U_OP_SET_CKE ||
                    state.observed[4].offset != 16 || state.observed[4].arraySize != 8 ||
                    state.observed[4].dataLen != 8 * TILEXR_CCU_CKE_SLOT_BYTES ||
                    state.observed[5].op != TILEXR_CCU_U_OP_SET_CKE ||
                    state.observed[5].offset != 24 || state.observed[5].arraySize != 2 ||
                    state.observed[5].dataLen != 2 * TILEXR_CCU_CKE_SLOT_BYTES) {
                    std::cerr << "SET_CKE batching mismatch\n";
                    return 11;
                }

                if (adapter.InstallXnRange(1, 32, 10, &report) != TILEXR_SUCCESS) {
                    std::cerr << "xn install failed: " << report.message << "\n";
                    return 12;
                }
                if (state.observed[6].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.observed[6].offset != 32 || state.observed[6].arraySize != 8 ||
                    state.observed[6].dataLen != 8 * TILEXR_CCU_XN_SLOT_BYTES ||
                    state.observed[7].op != TILEXR_CCU_U_OP_SET_XN ||
                    state.observed[7].offset != 40 || state.observed[7].arraySize != 2 ||
                    state.observed[7].dataLen != 2 * TILEXR_CCU_XN_SLOT_BYTES) {
                    std::cerr << "SET_XN batching mismatch\n";
                    return 13;
                }

                if (adapter.InstallJettyCtx(1, 9, jettys, 0, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    adapter.InstallJettyCtx(1, 9, nullptr, 2, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    adapter.ClearCkeRange(1, 0, 0, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    adapter.InstallXnRange(1, 0, 0, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "invalid lower-layer payload accepted\n";
                    return 14;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_msid_token_envelope_intentionally_matches_hcomm_zero_length_reference(self):
        hcomm_path = (
            REPO_ROOT
            / "3rdparty"
            / "hcomm"
            / "src"
            / "framework"
            / "next"
            / "comms"
            / "ccu"
            / "ccu_device"
            / "ccu_comp"
            / "ccu_comp.cc"
        )
        if not hcomm_path.exists():
            self.skipTest("comparison-only hcomm checkout is unavailable")
        hcomm_source = hcomm_path.read_text(encoding="utf-8")
        hcomm_body = hcomm_source[
            hcomm_source.index("HcclResult CcuComponent::ConfigMsIdToken()") :
            hcomm_source.index("HcclResult CcuComponent::GetCcuResourceSpaceBufInfo")
        ]
        source = DRIVER_SOURCE.read_text(encoding="utf-8")
        tilexr_body = source[
            source.index("int TileXRCcuDriverAdapter::InstallMsidToken(") :
            source.index("int TileXRCcuDriverAdapter::InstallPfeCtx(")
        ]

        for needle in [
            "CCU_U_OP_SET_MSID_TOKEN",
            "baseinfo.msId",
            "baseinfo.tokenId",
            "baseinfo.tokenValue",
        ]:
            with self.subTest(reference=needle):
                self.assertIn(needle, hcomm_body)
                self.assertIn(needle, tilexr_body)

        self.assertNotIn("dataArraySize", hcomm_body)
        self.assertNotIn("dataLen", hcomm_body)
        self.assertNotIn("dataArraySize", tilexr_body)
        self.assertNotIn("dataLen", tilexr_body)

    def test_direct_trace_dumps_custom_channel_envelope_and_payload_words(self):
        source = DRIVER_SOURCE.read_text(encoding="utf-8")

        for needle in [
            "TILEXR_CCU_DIRECT_TRACE",
            "TraceCustomChannelRequest",
            "TileXRDirectCcuTrace customChannel",
            "devicePhyId=",
            "op=",
            "dieId=",
            "offset=",
            "dataLen=",
            "arraySize=",
            "payloadWords=",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, source)

    def test_direct_trace_dumps_custom_channel_return_and_trailer_fields(self):
        source = DRIVER_SOURCE.read_text(encoding="utf-8")

        for needle in [
            "TraceCustomChannelReturn",
            "TileXRDirectCcuTrace customChannel.return",
            "driverRet=",
            "opRet=",
            "offsetNext=",
            "customChannel.requestTrailer",
            "customChannel.response",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, source)

    def test_direct_trace_runtime_emits_custom_channel_request_payload(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <iostream>

            using namespace TileXR;

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn&,
                TileXRCcuCustomChannelOut* out,
                void*)
            {
                out->opRet = 0;
                return 0;
            }

            int main()
            {
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(5, FakeCustomChannel, nullptr, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }

                TileXRCcuChannelCtxDataV1 channel{};
                for (uint32_t i = 0; i < TILEXR_CCU_CHANNEL_CTX_V1_BYTES; ++i) {
                    channel.raw[i] = static_cast<uint8_t>(0x10 + i);
                }
                if (adapter.InstallChannelCtxV1(1, 11, channel, &report) != TILEXR_SUCCESS) {
                    std::cerr << "channel install failed: " << report.message << "\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code, {"TILEXR_CCU_DIRECT_TRACE": "1"})

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("TileXRDirectCcuTrace customChannel", result.stderr)
        self.assertIn("devicePhyId=5", result.stderr)
        self.assertIn("op=256", result.stderr)
        self.assertIn("dieId=1", result.stderr)
        self.assertIn("offset=11", result.stderr)
        self.assertIn("dataLen=64", result.stderr)
        self.assertIn("arraySize=1", result.stderr)
        self.assertIn("payloadWords=8", result.stderr)
        self.assertIn("customChannel.payloadWords=8", result.stderr)
        self.assertIn("w0=0x1716151413121110", result.stderr)

    def test_direct_trace_runtime_emits_custom_channel_return_and_trailer_fields(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_driver_adapter.h"

            #include <iostream>

            using namespace TileXR;

            int FakeCustomChannel(
                uint32_t,
                const TileXRCcuCustomChannelIn&,
                TileXRCcuCustomChannelOut* out,
                void*)
            {
                out->offsetNextIdx = 99;
                out->opRet = 7;
                out->data.dataInfo.dataArray[0].dieinfo.enableFlag = 0;
                return -22;
            }

            int main()
            {
                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport report;
                if (adapter.Init(5, FakeCustomChannel, nullptr, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }

                bool enabled = true;
                const int ret = adapter.GetDieEnabled(1, &enabled, &report);
                if (ret != TILEXR_ERROR_MKIRT) {
                    std::cerr << "driver failure was accepted\n";
                    return 2;
                }
                if (report.driverRet != -22 || report.opRet != 7 ||
                    report.opcode != TILEXR_CCU_U_OP_GET_DIE_WORKING) {
                    std::cerr << "report did not retain driver diagnostics\n";
                    return 3;
                }
                if (report.message.find("driverRet=-22") == std::string::npos ||
                    report.message.find("opRet=7") == std::string::npos ||
                    report.message.find("op=15") == std::string::npos) {
                    std::cerr << "message did not retain driver diagnostics: " << report.message << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code, {"TILEXR_CCU_DIRECT_TRACE": "1"})

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("TileXRDirectCcuTrace customChannel.return", result.stderr)
        self.assertIn("driverRet=-22", result.stderr)
        self.assertIn("opRet=7", result.stderr)
        self.assertIn("offsetNext=99", result.stderr)
        self.assertIn("op=15", result.stderr)
        self.assertIn("customChannel.requestTrailerWords=1", result.stderr)
        self.assertIn("customChannel.responseWords=", result.stderr)

    def test_driver_adapter_failure_message_includes_opcode_and_driver_status(self):
        source = DRIVER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("CcuCustomChannelFailureMessage", source)
        self.assertIn('"CCU custom channel call failed"', source)
        self.assertIn('"CCU custom channel operation failed"', source)
        self.assertIn('" op="', source)
        self.assertIn('" driverRet="', source)
        self.assertIn('" opRet="', source)
        self.assertIn(
            "CcuCustomChannelFailureMessage(\"CCU custom channel call failed\", opcode, driverRet, out->opRet)",
            source,
        )
        self.assertIn(
            "CcuCustomChannelFailureMessage(\"CCU custom channel operation failed\", opcode, driverRet, out->opRet)",
            source,
        )

    def test_driver_adapter_is_wired_and_does_not_reference_hcomm_runtime_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = DRIVER_HEADER.read_text(encoding="utf-8")
        source = DRIVER_SOURCE.read_text(encoding="utf-8")
        specs_header = SPECS_HEADER.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_driver_adapter.h", cmake)
        self.assertIn("ccu/tilexr_ccu_driver_adapter.cpp", cmake)
        self.assertIn("TileXRCcuDriverAdapter", header)
        self.assertIn("TileXRCcuMsidTokenInfo", specs_header)
        self.assertIn("TileXRCcuMsidTokenInfo msidToken", specs_header)
        self.assertIn("basicInfo->msidToken.tokenId = raw.tokenId", source)
        self.assertIn("basicInfo->msidToken.tokenValue = raw.tokenValue", source)
        self.assertIn("basicInfo->msidToken.valid = raw.tokenValid != 0", source)
        self.assertIn("TileXRCcuCustomChannelIn", header)
        self.assertIn("TILEXR_CCU_U_OP_GET_BASIC_INFO", header)
        self.assertIn("TILEXR_CCU_U_OP_GET_DIE_WORKING", header)
        self.assertIn("TILEXR_CCU_U_OP_SET_MSID_TOKEN", header)
        self.assertIn("TILEXR_CCU_U_OP_SET_INSTRUCTION", header)
        self.assertIn("TILEXR_CCU_U_OP_SET_XN", header)
        self.assertIn("TILEXR_CCU_U_OP_SET_CKE", header)
        self.assertIn("TILEXR_CCU_U_OP_SET_PFE", header)
        self.assertIn("TILEXR_CCU_U_OP_SET_CHANNEL", header)
        self.assertIn("TILEXR_CCU_U_OP_SET_JETTY_CTX", header)
        self.assertIn("TILEXR_CCU_XN_SLOT_BYTES", header)
        self.assertIn("TileXRCcuPfeCtx", header)
        self.assertIn("TileXRCcuLocalJettyCtxData", header)
        self.assertIn("TileXRCcuChannelCtxDataV1", header)
        self.assertIn("TileXRCcuCustomChannelFn", header)
        self.assertIn("GetBasicInfo", header)
        self.assertIn("GetDieEnabled", header)
        self.assertIn("InstallInstructions", header)
        self.assertIn("InstallMsidToken", header)
        self.assertIn("InstallPfeCtx", header)
        self.assertIn("InstallJettyCtx", header)
        self.assertIn("InstallChannelCtxV1", header)
        self.assertIn("ClearCkeRange", header)
        self.assertIn("InstallXnRange", header)

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
            "HcomGetCcuTaskInfo",
            "HcclGetCcuTaskInfo",
            "CcuResBatchAllocator",
            "CcuDevMgrImp",
            "rtCCULaunch",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
