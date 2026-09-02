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
MICROCODE_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_microcode.h"
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
    "CcuResBatchAllocator",
    "CcuResRepository",
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


class TileXRCcuMicrocodeTest(unittest.TestCase):
    def compile_and_run(self, code: str):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found; remote CANN compile covers microcode C++ syntax")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "microcode_test.cpp"
            test_bin = temp_path / "microcode_test"
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

    def test_microcode_encodes_trace_observed_load_and_sync_words(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuInstr load;
                if (TileXRCcuEncodeLoadSqeArgsToX(1961, 0, &load) != TILEXR_SUCCESS) {
                    std::cerr << "load encode failed\n";
                    return 1;
                }
                if (load.words[0] != 0x0000000007a90001ULL || load.words[1] != 0 ||
                    load.words[2] != 0 || load.words[3] != 0) {
                    std::cerr << "unexpected load word\n";
                    return 2;
                }

                TileXRCcuInstr sync;
                TileXRCcuSyncXnSpec spec;
                spec.remoteXn = 2361;
                spec.localXn = 1961;
                spec.channelId = 2;
                spec.notifyCke = 364;
                spec.notifyMask = 1;
                spec.setCkeId = 0;
                spec.setCkeMask = 0;
                if (TileXRCcuEncodeSyncXn(spec, &sync) != TILEXR_SUCCESS) {
                    std::cerr << "sync encode failed\n";
                    return 3;
                }
                if (sync.words[0] != 0x000007a90939100dULL ||
                    sync.words[1] != 0x00000001016c0002ULL ||
                    sync.words[2] != 0x0001000000000000ULL ||
                    sync.words[3] != 0x0000000000000000ULL) {
                    std::cerr << "unexpected sync words\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_microcode_encodes_hcomm_v1_load_immediate_to_xn_words(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuInstr load;
                if (TileXRCcuEncodeLoadImdToXn(1961, 0x1122334455667788ULL, 0, &load) != TILEXR_SUCCESS) {
                    std::cerr << "load immediate encode failed\n";
                    return 1;
                }
                if (load.words[0] != 0x5566778807a90003ULL ||
                    load.words[1] != 0x0000000011223344ULL ||
                    load.words[2] != 0 ||
                    load.words[3] != 0) {
                    std::cerr << "unexpected load immediate words\n";
                    return 2;
                }

                if (TileXRCcuEncodeLoadImdToXn(0, 1, 0, &load) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuEncodeLoadImdToXn(1961, 1, 0, nullptr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "invalid load immediate arguments accepted\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_microcode_encodes_hcomm_v1_load_immediate_to_gsa_words(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuInstr load;
                if (TileXRCcuEncodeLoadImdToGsa(510, 0x1122334455667788ULL, &load) != TILEXR_SUCCESS) {
                    std::cerr << "load immediate to GSA encode failed\n";
                    return 1;
                }
                if (load.words[0] != 0x5566778801fe0002ULL ||
                    load.words[1] != 0x0000000011223344ULL ||
                    load.words[2] != 0 ||
                    load.words[3] != 0) {
                    std::cerr << "unexpected load immediate to GSA words\n";
                    return 2;
                }

                if (TileXRCcuEncodeLoadImdToGsa(0, 1, &load) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuEncodeLoadImdToGsa(510, 1, nullptr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "invalid load immediate to GSA arguments accepted\n";
                    return 3;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_microcode_rejects_invalid_arguments(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                if (TileXRCcuEncodeLoadSqeArgsToX(1961, 13, nullptr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null output accepted\n";
                    return 1;
                }

                TileXRCcuInstr instr;
                if (TileXRCcuEncodeLoadSqeArgsToX(1961, TILEXR_CCU_SQE_ARGS_LEN, &instr) !=
                    TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "out-of-range sqe arg accepted\n";
                    return 2;
                }

                TileXRCcuSyncXnSpec missingRemote;
                missingRemote.localXn = 1961;
                missingRemote.channelId = 2;
                missingRemote.notifyCke = 364;
                missingRemote.notifyMask = 1;
                if (TileXRCcuEncodeSyncXn(missingRemote, &instr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing remote xn accepted\n";
                    return 3;
                }

                TileXRCcuSyncXnSpec missingNotify;
                missingNotify.remoteXn = 2361;
                missingNotify.localXn = 1961;
                missingNotify.channelId = 2;
                if (TileXRCcuEncodeSyncXn(missingNotify, &instr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "missing notify accepted\n";
                    return 4;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_syncxn_setcke_fields_match_hcomm_v1_layout(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuInstr sync;
                TileXRCcuSyncXnSpec spec;
                spec.remoteXn = 2361;
                spec.localXn = 1961;
                spec.channelId = 2;
                spec.notifyCke = 364;
                spec.notifyMask = 1;
                spec.setCkeId = 400;
                spec.setCkeMask = 5;
                spec.waitCkeId = 401;
                spec.waitCkeMask = 6;
                if (TileXRCcuEncodeSyncXn(spec, &sync) != TILEXR_SUCCESS) {
                    std::cerr << "sync encode failed\n";
                    return 1;
                }
                if (sync.words[0] != 0x000007a90939100dULL ||
                    sync.words[1] != 0x00000001016c0002ULL ||
                    sync.words[2] != 0x0001000000000000ULL ||
                    sync.words[3] != 0x0006019100050190ULL) {
                    std::cerr << "sync local set/wait CKE fields are not hcomm v1 packed layout\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_synccke_fields_match_hcomm_v1_layout(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuInstr sync;
                TileXRCcuSyncCkeSpec spec;
                spec.remoteCke = 0x330;
                spec.localCke = 0x221;
                spec.localCkeMask = 1;
                spec.channelId = 2;
                spec.setCkeId = 0x401;
                spec.setCkeMask = 2;
                spec.waitCkeId = 0x402;
                spec.waitCkeMask = 3;
                if (TileXRCcuEncodeSyncCke(spec, &sync) != TILEXR_SUCCESS) {
                    std::cerr << "synccke encode failed\n";
                    return 1;
                }
                if (sync.words[0] != 0x000102210330100bULL ||
                    sync.words[1] != 0x0000000000000002ULL ||
                    sync.words[2] != 0x0001000000000000ULL ||
                    sync.words[3] != 0x0003040200020401ULL) {
                    std::cerr << "synccke fields are not hcomm v1 packed layout\n";
                    return 2;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_set_and_clear_cke_microcode_encoders_match_hcomm_v1_layout(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuCkeSpec setSpec;
                setSpec.ckeId = 332;
                setSpec.mask = 3;
                setSpec.waitCkeId = 364;
                setSpec.waitMask = 1;
                setSpec.clearWait = true;

                TileXRCcuInstr setInstr;
                if (TileXRCcuEncodeSetCke(setSpec, &setInstr) != TILEXR_SUCCESS) {
                    std::cerr << "set cke encode failed\n";
                    return 1;
                }
                if (setInstr.words[0] != 0x0003014c00010802ULL ||
                    setInstr.words[1] != 0x000000000001016cULL ||
                    setInstr.words[2] != 0 ||
                    setInstr.words[3] != 0) {
                    std::cerr << "unexpected set cke words\n";
                    return 2;
                }

                TileXRCcuCkeSpec clearSpec;
                clearSpec.ckeId = 0;
                clearSpec.mask = 0;
                clearSpec.waitCkeId = 364;
                clearSpec.waitMask = 1;
                clearSpec.clearWait = true;

                TileXRCcuInstr clearInstr;
                if (TileXRCcuEncodeClearCke(clearSpec, &clearInstr) != TILEXR_SUCCESS) {
                    std::cerr << "clear cke encode failed\n";
                    return 3;
                }
                if (clearInstr.words[0] != 0x0000000000010804ULL ||
                    clearInstr.words[1] != 0x000000000001016cULL ||
                    clearInstr.words[2] != 0 ||
                    clearInstr.words[3] != 0) {
                    std::cerr << "unexpected clear cke words\n";
                    return 4;
                }

                TileXRCcuCkeSpec invalid;
                if (TileXRCcuEncodeSetCke(invalid, &setInstr) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuEncodeClearCke(invalid, &clearInstr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty cke operation accepted\n";
                    return 5;
                }

                TileXRCcuCkeSpec partialSet;
                partialSet.ckeId = 332;
                partialSet.waitCkeId = 364;
                partialSet.waitMask = 1;
                if (TileXRCcuEncodeSetCke(partialSet, &setInstr) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuEncodeClearCke(partialSet, &clearInstr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "partial set CKE pair accepted\n";
                    return 6;
                }

                TileXRCcuCkeSpec partialWait;
                partialWait.ckeId = 332;
                partialWait.mask = 1;
                partialWait.waitCkeId = 364;
                if (TileXRCcuEncodeSetCke(partialWait, &setInstr) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuEncodeClearCke(partialWait, &clearInstr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "partial wait CKE pair accepted\n";
                    return 7;
                }
                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_memory_transfer_microcode_encoders_match_hcomm_v1_layout(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuMemTransferSpec spec;
                spec.localGsa = 0x101;
                spec.localXn = 0x102;
                spec.remoteGsa = 0x201;
                spec.remoteXn = 0x202;
                spec.lengthXn = 0x301;
                spec.channelId = 0x12;
                spec.reduceDataType = 0x0a;
                spec.reduceOpCode = 0x05;
                spec.setCkeId = 0x401;
                spec.setCkeMask = 0x2;
                spec.waitCkeId = 0x402;
                spec.waitCkeMask = 0x3;
                spec.clearWait = true;
                spec.lengthFromXn = true;
                spec.reduceEnabled = true;

                TileXRCcuInstr read;
                if (TileXRCcuEncodeTransRmtMemToLocMem(spec, &read) != TILEXR_SUCCESS) {
                    std::cerr << "trans rmt->loc encode failed\n";
                    return 1;
                }
                if (read.words[0] != 0x0201010201011008ULL ||
                    read.words[1] != 0x5a00001203010202ULL ||
                    read.words[2] != 0x0007000000000000ULL ||
                    read.words[3] != 0x0003040200020401ULL) {
                    std::cerr << "unexpected trans rmt->loc words\n";
                    return 2;
                }

                TileXRCcuInstr write;
                if (TileXRCcuEncodeTransLocMemToRmtMem(spec, &write) != TILEXR_SUCCESS) {
                    std::cerr << "trans loc->rmt encode failed\n";
                    return 3;
                }
                if (write.words[0] != 0x0101020202011009ULL ||
                    write.words[1] != 0x5a00001203010102ULL ||
                    write.words[2] != 0x0007000000000000ULL ||
                    write.words[3] != 0x0003040200020401ULL) {
                    std::cerr << "unexpected trans loc->rmt words\n";
                    return 4;
                }

                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_memory_transfer_microcode_rejects_missing_required_fields(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_microcode.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuInstr instr;
                TileXRCcuMemTransferSpec empty;
                if (TileXRCcuEncodeTransRmtMemToLocMem(empty, &instr) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuEncodeTransLocMemToRmtMem(empty, &instr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "empty transfer accepted\n";
                    return 1;
                }

                TileXRCcuMemTransferSpec spec;
                spec.localGsa = 1;
                spec.localXn = 2;
                spec.remoteGsa = 3;
                spec.remoteXn = 4;
                spec.lengthXn = 5;
                spec.channelId = 6;
                spec.setCkeId = 7;
                spec.setCkeMask = 8;

                if (TileXRCcuEncodeTransRmtMemToLocMem(spec, nullptr) != TILEXR_ERROR_PARA_CHECK_FAIL ||
                    TileXRCcuEncodeTransLocMemToRmtMem(spec, nullptr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "null output accepted\n";
                    return 2;
                }

                spec.reduceDataType = 0x10;
                if (TileXRCcuEncodeTransRmtMemToLocMem(spec, &instr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "out-of-range reduce data type accepted\n";
                    return 3;
                }

                spec.reduceDataType = 0;
                spec.reduceOpCode = 0x10;
                if (TileXRCcuEncodeTransLocMemToRmtMem(spec, &instr) != TILEXR_ERROR_PARA_CHECK_FAIL) {
                    std::cerr << "out-of-range reduce op code accepted\n";
                    return 4;
                }

                return 0;
            }
            '''
        )

        result = self.compile_and_run(code)

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_microcode_builder_is_wired_and_has_no_private_hcomm_surface(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        header = MICROCODE_HEADER.read_text(encoding="utf-8")
        source = MICROCODE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ccu/tilexr_ccu_microcode.h", cmake)
        self.assertIn("ccu/tilexr_ccu_microcode.cpp", cmake)
        self.assertIn("struct TileXRCcuInstr", header)
        self.assertIn("struct TileXRCcuSyncXnSpec", header)
        self.assertIn("TileXRCcuEncodeLoadSqeArgsToX", header)
        self.assertIn("TileXRCcuEncodeLoadImdToXn", header)
        self.assertIn("TileXRCcuEncodeLoadImdToGsa", header)
        self.assertIn("TileXRCcuEncodeSyncXn", header)
        self.assertIn("TileXRCcuEncodeSyncCke", header)
        self.assertIn("TileXRCcuEncodeSetCke", header)
        self.assertIn("TileXRCcuEncodeClearCke", header)
        self.assertIn("struct TileXRCcuMemTransferSpec", header)
        self.assertIn("TileXRCcuEncodeTransRmtMemToLocMem", header)
        self.assertIn("TileXRCcuEncodeTransLocMemToRmtMem", header)
        self.assertIn("0x0001U", source)
        self.assertIn("0x0002U", source)
        self.assertIn("0x0003U", source)
        self.assertIn("0x0802U", source)
        self.assertIn("0x0804U", source)
        self.assertIn("0x1008U", source)
        self.assertIn("0x1009U", source)
        self.assertIn("0x100bU", source)
        self.assertIn("0x100dU", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)


if __name__ == "__main__":
    unittest.main()
