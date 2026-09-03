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
PROVIDER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.h"
PROVIDER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp"
DRIVER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp"
SPECS_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuRaCustomChannelProviderTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "ra_provider_test.cpp"
            test_bin = temp_path / "ra_provider_test"
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
            return subprocess.run([str(test_bin)], cwd=REPO_ROOT, check=False, text=True, capture_output=True)

    def test_provider_adapts_ra_custom_channel_to_driver_adapter(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_ra_custom_channel_provider.h"

            #include <iostream>

            using namespace TileXR;

            void* g_tilexrCcuRaProviderTestState = nullptr;

            struct FakeRaState {
                uint32_t phyId = 0;
                uint32_t mode = 0;
                uint32_t op = 0;
                uint32_t die = 0;
            };

            int FakeRaCustomChannel(
                TileXRCcuRaInfo info,
                TileXRCcuCustomChannelIn* in,
                TileXRCcuCustomChannelOut* out)
            {
                auto* state = static_cast<FakeRaState*>(g_tilexrCcuRaProviderTestState);
                state->phyId = info.phyId;
                state->mode = info.mode;
                state->op = in->op;
                state->die = in->data.dataInfo.udieIdx;
                out->opRet = 0;
                out->data.dataInfo.dataArray[0].baseinfo.msId = 0x66;
                out->data.dataInfo.dataArray[0].baseinfo.missionKey = 0x0badcafeU;
                out->data.dataInfo.dataArray[0].baseinfo.resourceAddr = 0x300000000ULL;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap0 = (1U << 24) | (2U << 16) | 31U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap1 = (15U << 16) | 7U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap2 = (3U << 16) | 5U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap3 = (9U << 16) | 1U;
                return 0;
            }

            int main()
            {
                FakeRaState state;
                g_tilexrCcuRaProviderTestState = &state;

                TileXRCcuRaCustomChannelProvider provider;
                TileXRCcuRaCustomChannelProviderReport providerReport;
                if (provider.Init(9, FakeRaCustomChannel, &providerReport) != TILEXR_SUCCESS) {
                    std::cerr << "provider init failed: " << providerReport.message << "\n";
                    return 1;
                }

                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport adapterReport;
                if (provider.CreateAdapter(&adapter, &adapterReport) != TILEXR_SUCCESS) {
                    std::cerr << "create adapter failed: " << adapterReport.message << "\n";
                    return 2;
                }

                TileXRCcuBasicInfo basic;
                if (adapter.GetBasicInfo(1, &basic, &adapterReport) != TILEXR_SUCCESS) {
                    std::cerr << "get basic info failed: " << adapterReport.message << "\n";
                    return 3;
                }
                if (state.phyId != 9 || state.mode != TILEXR_CCU_NETWORK_OFFLINE ||
                    state.op != TILEXR_CCU_U_OP_GET_BASIC_INFO || state.die != 1) {
                    std::cerr << "RA call mismatch\n";
                    return 4;
                }
                if (basic.missionKey != 0x0badcafeU || basic.resourceAddr != 0x300000000ULL ||
                    basic.msId != 0x66) {
                    std::cerr << "basic info mismatch\n";
                    return 5;
                }
                if (providerReport.message != "ok" || providerReport.devicePhyId != 9) {
                    std::cerr << "provider report mismatch\n";
                    return 6;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_provider_accepts_opaque_ra_custom_channel_c_abi_shape(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_ra_custom_channel_provider.h"

            #include <iostream>

            using namespace TileXR;

            void* g_tilexrCcuRaProviderTestState = nullptr;

            struct LegacyRaInfo {
                int mode = 0;
                uint32_t phyId = 0;
            };

            struct FakeRaState {
                uint32_t phyId = 0;
                uint32_t mode = 0;
                uint32_t op = 0;
                uint32_t die = 0;
            };

            int FakeOpaqueRaCustomChannel(LegacyRaInfo info, void* rawIn, void* rawOut)
            {
                auto* state = static_cast<FakeRaState*>(g_tilexrCcuRaProviderTestState);
                auto* in = static_cast<TileXRCcuCustomChannelIn*>(rawIn);
                auto* out = static_cast<TileXRCcuCustomChannelOut*>(rawOut);
                state->phyId = info.phyId;
                state->mode = info.mode;
                state->op = in->op;
                state->die = in->data.dataInfo.udieIdx;
                out->opRet = 0;
                out->data.dataInfo.dataArray[0].baseinfo.msId = 0x77;
                out->data.dataInfo.dataArray[0].baseinfo.missionKey = 0x12345678U;
                out->data.dataInfo.dataArray[0].baseinfo.resourceAddr = 0x400000000ULL;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap0 = (1U << 24) | (2U << 16) | 31U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap1 = (15U << 16) | 7U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap2 = (3U << 16) | 5U;
                out->data.dataInfo.dataArray[0].baseinfo.caps.cap3 = (9U << 16) | 1U;
                return 0;
            }

            int main()
            {
                FakeRaState state;
                g_tilexrCcuRaProviderTestState = &state;

                TileXRCcuRaCustomChannelProvider provider;
                TileXRCcuRaCustomChannelProviderReport providerReport;
                if (provider.Init(13, FakeOpaqueRaCustomChannel, &providerReport) != TILEXR_SUCCESS) {
                    std::cerr << "provider init failed: " << providerReport.message << "\n";
                    return 1;
                }

                TileXRCcuDriverAdapter adapter;
                TileXRCcuDriverAdapterReport adapterReport;
                if (provider.CreateAdapter(&adapter, &adapterReport) != TILEXR_SUCCESS) {
                    std::cerr << "create adapter failed: " << adapterReport.message << "\n";
                    return 2;
                }

                TileXRCcuBasicInfo basic;
                if (adapter.GetBasicInfo(2, &basic, &adapterReport) != TILEXR_SUCCESS) {
                    std::cerr << "get basic info failed: " << adapterReport.message << "\n";
                    return 3;
                }
                if (state.phyId != 13 || state.mode != TILEXR_CCU_NETWORK_OFFLINE ||
                    state.op != TILEXR_CCU_U_OP_GET_BASIC_INFO || state.die != 2) {
                    std::cerr << "RA call mismatch\n";
                    return 4;
                }
                if (basic.missionKey != 0x12345678U || basic.resourceAddr != 0x400000000ULL ||
                    basic.msId != 0x77) {
                    std::cerr << "basic info mismatch\n";
                    return 5;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_provider_is_wired_and_keeps_hcomm_runtime_out_of_ccu_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = PROVIDER_HEADER.read_text(encoding="utf-8")
        source = PROVIDER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_ra_custom_channel_provider.h", cmake)
        self.assertIn("ccu/tilexr_ccu_ra_custom_channel_provider.cpp", cmake)
        self.assertIn("TileXRCcuRaCustomChannelProvider", header)
        self.assertIn("CreateAdapter", header)
        self.assertIn("TileXRCcuRaCustomChannelFunc", header)
        self.assertIn("std::function", header)
        self.assertIn("TILEXR_CCU_NETWORK_OFFLINE", source)
        self.assertIn("TileXRCcuDriverAdapter", header)
        self.assertNotIn("udma/", header)

        combined = header + "\n" + source
        for needle in [
            "#include <hcomm/",
            "#include <hccl/",
            "libhcomm",
            "libhccl_v2",
            "libhccl_fwk",
            "HcomGetCcuTaskInfo",
            "HcclGetCcuTaskInfo",
            "CcuResBatchAllocator",
            "dlopen",
            "dlsym",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
