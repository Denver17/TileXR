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
PAYLOAD_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_payloads.h"
PAYLOAD_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_lower_layer_payloads.cpp"
ABI_CONSTANTS_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_abi_constants.h"
HCOMM_ORACLE_SOURCE = REPO_ROOT / "tests" / "ccu" / "ccu_lower_layer_payload_hcomm_oracle.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"
INCLUDE_DIR = REPO_ROOT / "src" / "include"
COMM_DIR = REPO_ROOT / "src" / "comm"


class TileXRCcuLowerLayerPayloadsTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "lower_layer_payloads_test.cpp"
            test_bin = temp_path / "lower_layer_payloads_test"
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
                    str(PAYLOAD_SOURCE),
                    "-o",
                    str(test_bin),
                ],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            return subprocess.run([str(test_bin)], cwd=REPO_ROOT, check=False, text=True, capture_output=True)

    def test_packers_match_lower_layer_pfe_jetty_and_channel_v1_layouts(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_payloads.h"

            #include <cstdint>
            #include <iostream>

            using namespace TileXR;

            uint16_t Read16(const uint8_t* raw, uint32_t offset)
            {
                return static_cast<uint16_t>(raw[offset]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(raw[offset + 1]) << 8U);
            }

            int main()
            {
                TileXRCcuLowerLayerPayloadReport report;

                TileXRCcuPfeCtx pfe;
                TileXRCcuPfeCtxSpec pfeSpec;
                pfeSpec.startJettyId = 0x1234;
                pfeSpec.jettyCount = 5;
                pfeSpec.startLocalJettyCtxId = 0x22;
                if (TileXRCcuBuildPfeCtx(pfeSpec, &pfe, &report) != TILEXR_SUCCESS) {
                    std::cerr << "pfe build failed: " << report.message << "\n";
                    return 1;
                }
                if (Read16(pfe.raw, 0) != 0x1234 ||
                    Read16(pfe.raw, 2) != static_cast<uint16_t>(4U | (0x22U << 7U)) ||
                    Read16(pfe.raw, 4) != 0 || Read16(pfe.raw, 6) != 0) {
                    std::cerr << "pfe layout mismatch\n";
                    return 2;
                }

                TileXRCcuLocalJettyCtxData jetty;
                TileXRCcuLocalJettyCtxSpec jettySpec;
                jettySpec.dieId = 1;
                jettySpec.pfeId = 3;
                jettySpec.doorbellVa = 0x1122334455667788ULL;
                jettySpec.doorbellTokenId = 0x000abcdeU;
                jettySpec.doorbellTokenValue = 0x89abcdefU;
                jettySpec.sqDepth = 16;
                jettySpec.wqeBasicBlockStartId = 0x9a;
                if (TileXRCcuBuildLocalJettyCtx(jettySpec, &jetty, &report) != TILEXR_SUCCESS) {
                    std::cerr << "jetty build failed: " << report.message << "\n";
                    return 3;
                }
                if (Read16(jetty.raw, 0) != 0x7788 || Read16(jetty.raw, 2) != 0x5566 ||
                    Read16(jetty.raw, 4) != 0x3344 || Read16(jetty.raw, 6) != 0x1122 ||
                    Read16(jetty.raw, 8) != 0xde73 || Read16(jetty.raw, 10) != 0xfabc ||
                    Read16(jetty.raw, 12) != 0xbcde || Read16(jetty.raw, 14) != 0x689a ||
                    Read16(jetty.raw, 16) != 0 || Read16(jetty.raw, 18) != 0 ||
                    Read16(jetty.raw, 20) != 0 || Read16(jetty.raw, 22) != 0xa000 ||
                    Read16(jetty.raw, 24) != 0x0009 || Read16(jetty.raw, 26) != 0 ||
                    Read16(jetty.raw, 28) != 0 || Read16(jetty.raw, 30) != 0) {
                    std::cerr << "jetty layout mismatch\n";
                    return 4;
                }

                TileXRCcuChannelCtxDataV1 channel;
                TileXRCcuChannelCtxV1Spec channelSpec;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    channelSpec.remoteEid[i] = static_cast<uint8_t>(0x10 + i);
                }
                channelSpec.tpn = 0x00ab5678U;
                channelSpec.sourcePfeId = 5;
                channelSpec.startJettyId = 0x0234;
                channelSpec.jettyCount = 7;
                channelSpec.dieId = 1;
                channelSpec.memoryTokenId = 0x000abcdeU;
                channelSpec.memoryTokenValue = 0x89abcdefU;
                channelSpec.remoteCcuVa = 0x000123456789ab00ULL;
                if (TileXRCcuBuildChannelCtxV1(channelSpec, &channel, &report) != TILEXR_SUCCESS) {
                    std::cerr << "channel build failed: " << report.message << "\n";
                    return 5;
                }
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    if (channel.raw[i] != static_cast<uint8_t>(0x10 + i)) {
                        std::cerr << "channel eid mismatch\n";
                        return 6;
                    }
                }
                const uint64_t dstVa = channelSpec.remoteCcuVa >> TILEXR_CCU_REMOTE_CCU_VA_SHIFT;
                if (Read16(channel.raw, 16) != 0x5678 ||
                    Read16(channel.raw, 18) != 0x45ab ||
                    Read16(channel.raw, 20) != 0x6023 ||
                    Read16(channel.raw, 22) != 0xcde8 ||
                    Read16(channel.raw, 24) != 0xefab ||
                    Read16(channel.raw, 26) != 0xabcd ||
                    Read16(channel.raw, 28) != static_cast<uint16_t>(0x0089U | ((dstVa & 0xffU) << 8U)) ||
                    Read16(channel.raw, 30) != static_cast<uint16_t>((dstVa >> 8U) & 0xffffU) ||
                    Read16(channel.raw, 32) != static_cast<uint16_t>((dstVa >> 24U) & 0xffffU) ||
                    Read16(channel.raw, 34) != static_cast<uint16_t>(((dstVa >> 40U) & 0x1U) | 0x2U) ||
                    Read16(channel.raw, 36) != 0 || Read16(channel.raw, 62) != 0) {
                    std::cerr << "channel layout mismatch\n";
                    return 7;
                }

                if (TileXRCcuBuildPfeCtx({0, 0, 0}, &pfe, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuBuildLocalJettyCtx({}, &jetty, &report) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuBuildChannelCtxV1({}, &channel, &report) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "invalid lower-layer payload specs accepted\n";
                    return 8;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_packers_accept_plaintext_zero_token_values_from_tilexr_udma(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_lower_layer_payloads.h"

            #include <cstdint>
            #include <iostream>

            using namespace TileXR;

            uint16_t Read16(const uint8_t* raw, uint32_t offset)
            {
                return static_cast<uint16_t>(raw[offset]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(raw[offset + 1]) << 8U);
            }

            int main()
            {
                TileXRCcuLowerLayerPayloadReport report;

                TileXRCcuLocalJettyCtxData jetty;
                TileXRCcuLocalJettyCtxSpec jettySpec;
                jettySpec.dieId = 0;
                jettySpec.pfeId = 2;
                jettySpec.doorbellVa = 0x1020304050607080ULL;
                jettySpec.doorbellTokenId = 0x12345U;
                jettySpec.doorbellTokenValue = 0;
                jettySpec.sqDepth = 8;
                if (TileXRCcuBuildLocalJettyCtx(jettySpec, &jetty, &report) != TILEXR_SUCCESS) {
                    std::cerr << "zero doorbell token value rejected: " << report.message << "\n";
                    return 1;
                }
                if (Read16(jetty.raw, 10) != 0x0123 ||
                    Read16(jetty.raw, 12) != 0 ||
                    (Read16(jetty.raw, 14) & 0x0fffU) != 0) {
                    std::cerr << "zero doorbell token value packed incorrectly\n";
                    return 2;
                }

                jettySpec.doorbellTokenId = 0;
                if (TileXRCcuBuildLocalJettyCtx(jettySpec, &jetty, &report) != TILEXR_SUCCESS) {
                    std::cerr << "zero doorbell token id rejected: " << report.message << "\n";
                    return 5;
                }
                if ((Read16(jetty.raw, 8) & 0x0040U) == 0 ||
                    (Read16(jetty.raw, 8) & 0xff00U) != 0 ||
                    (Read16(jetty.raw, 10) & 0x0fffU) != 0) {
                    std::cerr << "zero doorbell token id packed incorrectly\n";
                    return 6;
                }

                TileXRCcuChannelCtxDataV1 channel;
                TileXRCcuChannelCtxV1Spec channelSpec;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    channelSpec.remoteEid[i] = static_cast<uint8_t>(0xa0 + i);
                }
                channelSpec.tpn = 0x13579U;
                channelSpec.sourcePfeId = 2;
                channelSpec.startJettyId = 0x44;
                channelSpec.jettyCount = 1;
                channelSpec.dieId = 0;
                channelSpec.memoryTokenId = 0x12345U;
                channelSpec.memoryTokenValue = 0;
                channelSpec.remoteCcuVa = 0x0000001234000000ULL;
                if (TileXRCcuBuildChannelCtxV1(channelSpec, &channel, &report) != TILEXR_SUCCESS) {
                    std::cerr << "zero memory token value rejected: " << report.message << "\n";
                    return 7;
                }
                if ((Read16(channel.raw, 24) & 0xff00U) != 0 ||
                    Read16(channel.raw, 26) != 0 ||
                    (Read16(channel.raw, 28) & 0x00ffU) != 0 ||
                    (Read16(channel.raw, 34) & 0x2U) == 0) {
                    std::cerr << "zero memory token value packed incorrectly\n";
                    return 8;
                }

                channelSpec.memoryTokenId = 0;
                if (TileXRCcuBuildChannelCtxV1(channelSpec, &channel, &report) != TILEXR_SUCCESS) {
                    std::cerr << "zero memory token id rejected: " << report.message << "\n";
                    return 9;
                }
                if ((Read16(channel.raw, 22) & 0xfff0U) != 0 ||
                    (Read16(channel.raw, 24) & 0x00ffU) != 0 ||
                    (Read16(channel.raw, 34) & 0x2U) == 0) {
                    std::cerr << "zero memory token id packed incorrectly\n";
                    return 10;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_lower_layer_payload_packers_are_wired_without_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = PAYLOAD_HEADER.read_text(encoding="utf-8")
        source = PAYLOAD_SOURCE.read_text(encoding="utf-8")
        oracle = HCOMM_ORACLE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_lower_layer_payloads.h", cmake)
        self.assertIn("ccu/tilexr_ccu_lower_layer_payloads.cpp", cmake)
        self.assertIn("TileXRCcuPfeCtxSpec", header)
        self.assertIn("TileXRCcuLocalJettyCtxSpec", header)
        self.assertIn("TileXRCcuChannelCtxV1Spec", header)
        self.assertIn("TileXRCcuBuildPfeCtx", header)
        self.assertIn("TileXRCcuBuildLocalJettyCtx", header)
        self.assertIn("TileXRCcuBuildChannelCtxV1", header)
        self.assertIn("tilexr_ccu_abi_constants.h", header)
        self.assertIn("TILEXR_CCU_REMOTE_CCU_VA_SHIFT", ABI_CONSTANTS_HEADER.read_text(encoding="utf-8"))
        self.assertIn("BuildHcommPfeCtx", oracle)
        self.assertIn("BuildHcommLocalJettyCtx", oracle)
        self.assertIn("BuildHcommChannelCtxV1", oracle)

        combined = header + "\n" + source + "\n" + oracle
        for needle in [
            "#include <hcomm/",
            "#include <hccl/",
            "libhcomm",
            "libhccl_v2",
            "libhccl_fwk",
            "Hccl",
            "CcuDevMgrImp",
            "CcuResBatchAllocator",
            "RT_RES_TYPE_CCU_XN",
            "RT_RES_TYPE_CCU_CKE",
        ]:
            self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
